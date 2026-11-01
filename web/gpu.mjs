// web/gpu.mjs — GPU arka ucu platform katmanı (M13 faz 8a-8b, D38, D42): motorun komut akışını (src/gpu/gcmd.h) WebGPU
// çağrılarına çevirir. Motor mantığı yok: hangi mesh'in hangi programla ne zaman çizileceğine C karar verir; burada
// yalnızca kaynak önbelleği (mesh, doku, program sürümü), boru hattı önbelleği ve kayıtların sırayla yürütülmesi var.
// WGSL metni motordan gelir (Matoki → WGSL, matoki/wgsl.c; id 0 gömülü düz shader). WebGPU yoksa createGpu null döner →
// sayfa yazılım render'da kalır.

const GC_END = 0, GC_FRAME = 1, GC_MESH = 2, GC_DRAW = 3, GC_TEX = 4, MAGIC = 0x5550474d, SLOT = 256, TEX_HEADER = 352;
const CULL = ['none', 'back', 'front'];   // raster.h CullMode sırası; ön yüz NDC'de CCW (D3)

export async function createGpu(ex, memory, colorFormat) {
  if (!navigator.gpu) return null;
  const adapter = await navigator.gpu.requestAdapter();
  if (!adapter) return null;
  const device = await adapter.requestDevice();
  const uniformEntry = (binding, visibility) => ({ binding, visibility,
    buffer: { type: 'uniform', hasDynamicOffset: true, minBindingSize: SLOT } });
  const bglU = device.createBindGroupLayout({ entries: [uniformEntry(0, GPUShaderStage.VERTEX), uniformEntry(1, GPUShaderStage.FRAGMENT)] });
  const bglT = device.createBindGroupLayout({ entries: [0, 1].map((binding) => ({ binding, visibility: GPUShaderStage.FRAGMENT,
    buffer: { type: 'read-only-storage' } })) });
  const layout = device.createPipelineLayout({ bindGroupLayouts: [bglU, bglT] });
  const vbufs = [0, 1, 2, 3, 4, 5, 6, 7].map((i) => ({ arrayStride: 4, attributes: [{ shaderLocation: i, offset: 0, format: 'float32' }] }));

  // programlar: id → { gen, module }; canlı derleme gen'i artırır, boru hattı anahtarı gen'i içerir
  const progs = new Map();
  const progText = (id) => new TextDecoder().decode(new Uint8Array(memory.buffer, ex.engine_gpu_prog_text(id), ex.engine_gpu_prog_len(id)).slice());
  function program(id) {
    const gen = ex.engine_gpu_prog_gen(id);
    let p = progs.get(id);
    if (!p || p.gen !== gen) {
      p = { gen, module: device.createShaderModule({ code: progText(id), label: `gpu prog ${id}` }) };
      progs.set(id, p);
    }
    return p;
  }
  // açılışta tüm programlar derlenir: üretilen WGSL'deki hata burada görünür, kurulum yazılım render'a düşer
  const nprogs = ex.engine_gpu_prog_count();
  for (let id = 0; id < nprogs; id++) {
    if (!ex.engine_gpu_prog_len(id)) continue;
    const info = await program(id).module.getCompilationInfo();
    const errs = info.messages.filter((m) => m.type === 'error');
    if (errs.length) throw new Error(`gpu prog ${id}: ` + errs.map((m) => `${m.lineNum}:${m.linePos} ${m.message}`).join('; '));
  }

  const pipes = new Map();
  const pipeline = (vp, fp, cull, dt, dw, fmt) => {
    const v = program(vp), f = program(fp);
    const key = `${vp}.${v.gen}|${fp}.${f.gen}|${cull}|${dt}|${dw}|${fmt}`;
    let p = pipes.get(key);
    if (!p) {
      p = device.createRenderPipeline({
        layout, vertex: { module: v.module, entryPoint: 'vs', buffers: vbufs },
        fragment: { module: f.module, entryPoint: 'fs', targets: [{ format: fmt }] },
        primitive: { topology: 'triangle-list', cullMode: CULL[cull] || 'none', frontFace: 'ccw' },
        depthStencil: { format: 'depth32float', depthWriteEnabled: !!dw, depthCompare: dt ? 'less' : 'always' },
      });
      pipes.set(key, p);
    }
    return p;
  };
  const meshes = new Map();   // kimlik → { gen, vc, ic, bufs[8], ibuf, seen }
  const textures = new Map();   // kimlik → { px, words, buf }
  const texBinds = new Map();   // "doku0|doku1" → bind group
  const noTex = device.createBuffer({ size: 4, usage: GPUBufferUsage.STORAGE });   // okunmayan doku yuvası
  let ubuf = null, bindU = null, depth = null, frameNo = 0, keepDepth = false;

  function uploadMesh(w, i) {
    const [id, gen, vc, ic] = [w[i + 1], w[i + 2], w[i + 3], w[i + 4]];
    let m = meshes.get(id);
    if (m && m.gen === gen && m.vc === vc && m.ic === ic) { m.seen = frameNo; return; }
    if (m) { m.bufs.forEach((b) => b.destroy()); m.ibuf.destroy(); }
    const bufs = [5, 6, 7, 8, 9, 10, 11, 12].map((k) => {
      const b = device.createBuffer({ size: Math.max(4, vc * 4), usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
      device.queue.writeBuffer(b, 0, memory.buffer, w[i + k], vc * 4);
      return b;
    });
    const ibuf = device.createBuffer({ size: Math.max(4, ic * 4), usage: GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST });
    device.queue.writeBuffer(ibuf, 0, memory.buffer, w[i + 13], ic * 4);
    meshes.set(id, { gen, vc, ic, bufs, ibuf, seen: frameNo });
  }

  // doku tamponu = Texture başlığı + px (matoki/wgsl.h); WGSL motorun kendi aritmetiğiyle süzer
  function uploadTex(w, i) {
    const [id, px, words] = [w[i + 1], w[i + 2], w[i + 3]];
    const t = textures.get(id);
    if (t && t.px === px && t.words === words) return;
    if (t) t.buf.destroy();
    texBinds.clear();
    const buf = device.createBuffer({ size: TEX_HEADER + words * 4, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
    device.queue.writeBuffer(buf, 0, memory.buffer, id, TEX_HEADER);
    device.queue.writeBuffer(buf, TEX_HEADER, memory.buffer, px, words * 4);
    textures.set(id, { px, words, buf });
  }

  function texBind(a, b) {
    const key = `${a}|${b}`;
    let g = texBinds.get(key);
    if (!g) {
      const res = (id) => ({ buffer: id ? textures.get(id).buf : noTex });
      g = device.createBindGroup({ layout: bglT, entries: [{ binding: 0, resource: res(a) }, { binding: 1, resource: res(b) }] });
      texBinds.set(key, g);
    }
    return g;
  }

  // son karenin akışını view'a çizer; çizim sayısını döndürür (akış yoksa -1)
  function render(view, fmt) {
    const ptr = ex.engine_gpu_cmds();
    if (!ptr) return -1;
    const hdr = new Uint32Array(memory.buffer, ptr, 6);
    if (hdr[0] !== MAGIC) throw new Error('GPU akışı: başlık bozuk');
    const w = new Uint32Array(memory.buffer, ptr, hdr[2]);
    frameNo++;
    const ubytes = hdr[5];
    if (ubytes && (!ubuf || ubuf.size < ubytes)) {
      ubuf?.destroy();
      ubuf = device.createBuffer({ size: Math.max(ubytes, 64 * SLOT), usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
      const res = { buffer: ubuf, size: SLOT };
      bindU = device.createBindGroup({ layout: bglU, entries: [{ binding: 0, resource: res }, { binding: 1, resource: res }] });
    }
    if (ubytes) device.queue.writeBuffer(ubuf, 0, memory.buffer, hdr[4], ubytes);
    const enc = device.createCommandEncoder();
    let pass = null, draws = 0;
    for (let i = 6; i < w.length;) {
      const op = w[i];
      if (op === GC_END) break;
      if (op === GC_FRAME) {
        const [fw, fh, rgba, zb] = [w[i + 1], w[i + 2], w[i + 3], w[i + 4]];
        if (!depth || depth.width !== fw || depth.height !== fh) {
          depth?.destroy();
          depth = device.createTexture({ size: [fw, fh], format: 'depth32float', usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC });
        }
        const zf = new Float32Array(new Uint32Array([zb]).buffer)[0];
        pass = enc.beginRenderPass({
          colorAttachments: [{ view, loadOp: 'clear', storeOp: 'store',
            clearValue: { r: (rgba & 255) / 255, g: ((rgba >>> 8) & 255) / 255, b: ((rgba >>> 16) & 255) / 255, a: (rgba >>> 24) / 255 } }],
          depthStencilAttachment: { view: depth.createView(), depthLoadOp: 'clear', depthClearValue: zf, depthStoreOp: keepDepth ? 'store' : 'discard' },
        });
        i += 5;
      } else if (op === GC_MESH) {
        uploadMesh(w, i);
        i += 14;
      } else if (op === GC_TEX) {
        uploadTex(w, i);
        i += 4;
      } else if (op === GC_DRAW) {
        const m = meshes.get(w[i + 1]);
        pass.setPipeline(pipeline(w[i + 4], w[i + 5], w[i + 8], w[i + 9], w[i + 10], fmt));
        pass.setBindGroup(0, bindU, [w[i + 2] * SLOT, w[i + 3] * SLOT]);
        pass.setBindGroup(1, texBind(w[i + 6], w[i + 7]));
        m.bufs.forEach((b, k) => pass.setVertexBuffer(k, b));
        pass.setIndexBuffer(m.ibuf, 'uint32');
        pass.drawIndexed(m.ic);
        draws++;
        i += 11;
      } else {
        throw new Error('GPU akışı: bilinmeyen kayıt ' + op);
      }
    }
    pass?.end();
    device.queue.submit([enc.finish()]);
    // uzun süre görülmeyen mesh'ler (yeniden kurulan sahneler) bırakılır
    for (const [id, m] of meshes) if (frameNo - m.seen > 120) { m.bufs.forEach((b) => b.destroy()); m.ibuf.destroy(); meshes.delete(id); }
    return draws;
  }

  // dokuyu (renk ya da derinlik) satır başı sıkışık diziye oku
  async function copyOut(texture, width, height, aspect) {
    const bpr = Math.ceil(width * 4 / 256) * 256;
    const buf = device.createBuffer({ size: bpr * height, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    const enc = device.createCommandEncoder();
    enc.copyTextureToBuffer({ texture, aspect }, { buffer: buf, bytesPerRow: bpr }, [width, height]);
    device.queue.submit([enc.finish()]);
    await buf.mapAsync(GPUMapMode.READ);
    const src = new Uint8Array(buf.getMappedRange());
    const out = new Uint8Array(width * height * 4);
    for (let y = 0; y < height; y++) out.set(src.subarray(y * bpr, y * bpr + width * 4), y * width * 4);
    buf.unmap();
    buf.destroy();
    return out;
  }

  // test / araç: ekran dışı rgba8unorm dokuya çiz; renk (w·h·4 bayt) ve derinlik (w·h f32) geri okunur
  async function readback(width, height) {
    const tex = device.createTexture({ size: [width, height], format: 'rgba8unorm',
      usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC });
    keepDepth = true;
    const draws = render(tex.createView(), 'rgba8unorm');
    keepDepth = false;
    const pixels = await copyOut(tex, width, height, 'all');
    const depthOut = new Float32Array((await copyOut(depth, width, height, 'depth-only')).buffer);
    tex.destroy();
    return { pixels, depth: depthOut, draws };
  }

  return { device, adapter, render, readback, meshCount: () => meshes.size };
}
