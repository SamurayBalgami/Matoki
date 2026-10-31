// web/gpu.mjs — GPU arka ucu platform katmanı (M13 faz 8a, D38): motorun komut akışını (src/gpu/gcmd.h) WebGPU
// çağrılarına çevirir. Motor mantığı yok: hangi mesh'in ne zaman çizileceğine C karar verir; burada yalnızca
// kaynak önbelleği (mesh kimliği + sürüm), boru hattı önbelleği ve kayıtların sırayla yürütülmesi var.
// WGSL metni motorda gömülü (assets/gpu_8a.wgsl). WebGPU yoksa createGpu null döner → sayfa yazılım render'da kalır.

const GC_END = 0, GC_FRAME = 1, GC_MESH = 2, GC_DRAW = 3, MAGIC = 0x5550474d, SLOT = 256;
const CULL = ['none', 'back', 'front'];   // raster.h CullMode sırası; ön yüz NDC'de CCW (D3)

export async function createGpu(ex, memory, colorFormat) {
  if (!navigator.gpu) return null;
  const adapter = await navigator.gpu.requestAdapter();
  if (!adapter) return null;
  const device = await adapter.requestDevice();
  const text = new TextDecoder().decode(new Uint8Array(memory.buffer, ex.engine_gpu_shader(), ex.engine_gpu_shader_len()).slice());
  const module = device.createShaderModule({ code: text });
  const info = await module.getCompilationInfo();
  const errs = info.messages.filter((m) => m.type === 'error');
  if (errs.length) throw new Error('gpu_8a.wgsl: ' + errs.map((m) => `${m.lineNum}:${m.linePos} ${m.message}`).join('; '));
  const bgl = device.createBindGroupLayout({ entries: [{ binding: 0, visibility: GPUShaderStage.VERTEX,
    buffer: { type: 'uniform', hasDynamicOffset: true, minBindingSize: 176 } }] });
  const layout = device.createPipelineLayout({ bindGroupLayouts: [bgl] });
  const vbufs = [0, 1, 2, 3, 4, 5].map((i) => ({ arrayStride: 4, attributes: [{ shaderLocation: i, offset: 0, format: 'float32' }] }));
  const pipes = new Map();
  const pipeline = (mode, cull, dt, dw, fmt) => {
    const key = `${mode}|${cull}|${dt}|${dw}|${fmt}`;
    let p = pipes.get(key);
    if (!p) {
      p = device.createRenderPipeline({
        layout, vertex: { module, entryPoint: 'vs', buffers: vbufs },
        fragment: { module, entryPoint: mode === 0 ? 'fs_flat' : 'fs_normal', targets: [{ format: fmt }] },
        primitive: { topology: 'triangle-list', cullMode: CULL[cull] || 'none', frontFace: 'ccw' },
        depthStencil: { format: 'depth32float', depthWriteEnabled: !!dw, depthCompare: dt ? 'less' : 'always' },
      });
      pipes.set(key, p);
    }
    return p;
  };
  const meshes = new Map();   // kimlik → { gen, vc, ic, bufs[6], ibuf, seen }
  let ubuf = null, bind = null, depth = null, frameNo = 0, keepDepth = false;

  function upload(w, i) {
    const [id, gen, vc, ic] = [w[i + 1], w[i + 2], w[i + 3], w[i + 4]];
    let m = meshes.get(id);
    if (m && m.gen === gen && m.vc === vc && m.ic === ic) { m.seen = frameNo; return; }
    if (m) { m.bufs.forEach((b) => b.destroy()); m.ibuf.destroy(); }
    const bufs = [5, 6, 7, 8, 9, 10].map((k) => {
      const b = device.createBuffer({ size: Math.max(4, vc * 4), usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
      device.queue.writeBuffer(b, 0, memory.buffer, w[i + k], vc * 4);
      return b;
    });
    const ibuf = device.createBuffer({ size: Math.max(4, ic * 4), usage: GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST });
    device.queue.writeBuffer(ibuf, 0, memory.buffer, w[i + 11], ic * 4);
    meshes.set(id, { gen, vc, ic, bufs, ibuf, seen: frameNo });
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
      bind = device.createBindGroup({ layout: bgl, entries: [{ binding: 0, resource: { buffer: ubuf, size: 176 } }] });
    }
    if (ubytes) device.queue.writeBuffer(ubuf, 0, memory.buffer, hdr[4], ubytes);
    const enc = device.createCommandEncoder();
    let pass = null, mode = 0, draws = 0;
    for (let i = 6; i < w.length;) {
      const op = w[i];
      if (op === GC_END) break;
      if (op === GC_FRAME) {
        const [fw, fh, rgba, zb] = [w[i + 1], w[i + 2], w[i + 3], w[i + 4]];
        mode = w[i + 5];
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
        i += 6;
      } else if (op === GC_MESH) {
        upload(w, i);
        i += 12;
      } else if (op === GC_DRAW) {
        const m = meshes.get(w[i + 1]);
        pass.setPipeline(pipeline(mode, w[i + 3], w[i + 4], w[i + 5], fmt));
        pass.setBindGroup(0, bind, [w[i + 2] * SLOT]);
        m.bufs.forEach((b, k) => pass.setVertexBuffer(k, b));
        pass.setIndexBuffer(m.ibuf, 'uint32');
        pass.drawIndexed(m.ic);
        draws++;
        i += 6;
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
