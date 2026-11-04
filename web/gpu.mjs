// web/gpu.mjs — GPU arka ucu platform katmanı (M13 faz 8a-8c, D38, D42, D44): motorun komut akışını (src/gpu/gcmd.h) WebGPU
// çağrılarına çevirir. Motor mantığı yok: hangi mesh'in hangi programla ne zaman çizileceğine C karar verir; burada
// yalnızca kaynak önbelleği (mesh, doku, program sürümü), boru hattı önbelleği ve kayıtların sırayla yürütülmesi var.
// WGSL metni motordan gelir (Matoki → WGSL, matoki/wgsl.c; id 0 gömülü düz shader). WebGPU yoksa createGpu null döner →
// sayfa yazılım render'da kalır.

const GC_END = 0, GC_FRAME = 1, GC_MESH = 2, GC_DRAW = 3, GC_TEX = 4, GC_SHADOW = 5, GC_SHADOW_END = 6, GC_OVERLAY = 7, GC_REFLECT = 8, GC_REFLECT_END = 9, GPROG_OVERLAY = 1, GPROG_NONE = 0xFFFFFFFF, MAGIC = 0x5550474d, SLOT = 256, TEX_HEADER = 356;
const STREAM_VERSION = 8;   // gpu/gcmd.h GCMD_VERSION; doku bağlama düzeni değişince de artar (D50)
const CULL = ['none', 'back', 'front'];   // raster.h CullMode sırası; ön yüz NDC'de CCW (D3)

export async function createGpu(ex, memory, colorFormat) {
  if (!navigator.gpu) return null;
  // önbellekten gelen eski JS + yeni motor (ya da tersi) sessizce bozuk çizmesin: kurulum düşer, sayfa yazılım render'da kalır
  if (ex.engine_gpu_stream_version?.() !== STREAM_VERSION) throw new Error(`GPU akışı sürümü uyuşmuyor (motor ${ex.engine_gpu_stream_version?.()}, gpu.mjs ${STREAM_VERSION}): sayfayı yenileyin`);
  const adapter = await navigator.gpu.requestAdapter();
  if (!adapter) return null;
  const hasTs = adapter.features.has('timestamp-query');
  const device = await adapter.requestDevice({ requiredFeatures: hasTs ? ['timestamp-query'] : [] });
  device.addEventListener('uncapturederror', (e) => console.error('[gpu]', e.error.message));
  const uniformEntry = (binding, visibility) => ({ binding, visibility,
    buffer: { type: 'uniform', hasDynamicOffset: true, minBindingSize: SLOT } });
  const bglU = device.createBindGroupLayout({ entries: [uniformEntry(0, GPUShaderStage.VERTEX), uniformEntry(1, GPUShaderStage.FRAGMENT)] });
  const bglT = device.createBindGroupLayout({ entries: [
    ...[0, 1, 2, 3].map((binding) => ({ binding, visibility: GPUShaderStage.FRAGMENT, buffer: { type: 'read-only-storage' } })),
    ...[4, 5, 6, 7].map((binding) => ({ binding, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } })),
    ...[8, 9].map((binding) => ({ binding, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } })),
  ] });
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
  // fp = GPROG_NONE: yalnız derinlik (gölge geçişi); samples: geçişin örnek sayısı
  const pipeline = (vp, fp, cull, dt, dw, fmt, samples) => {
    const v = program(vp), f = fp === GPROG_NONE ? null : program(fp);
    const key = `${vp}.${v.gen}|${fp}.${f ? f.gen : 0}|${cull}|${dt}|${dw}|${fmt}|${samples}`;
    let p = pipes.get(key);
    if (!p) {
      p = device.createRenderPipeline({
        layout, vertex: { module: v.module, entryPoint: 'vs', buffers: vbufs },
        fragment: f ? { module: f.module, entryPoint: 'fs', targets: [{ format: fmt }] } : undefined,
        primitive: { topology: 'triangle-list', cullMode: CULL[cull] || 'none', frontFace: 'ccw' },
        depthStencil: { format: 'depth32float', depthWriteEnabled: !!dw, depthCompare: dt ? 'less' : 'always' },
        multisample: { count: samples },
      });
      pipes.set(key, p);
    }
    return p;
  };
  const meshes = new Map();   // kimlik → { gen, vc, ic, bufs[8], ibuf, seen }
  const textures = new Map();   // kimlik → { px, words, buf }
  const texBinds = new Map();   // "doku0|doku1|doku2" → bind group
  const noTex = device.createBuffer({ size: 4, usage: GPUBufferUsage.STORAGE });   // okunmayan doku yuvası
  // bilinear okuma donanımda (D50): depolama tamponu başlık ve texel_fetch için kalır, sample_level bu dokudan okur
  const noHw = device.createTexture({ size: [1, 1], format: 'rgba8unorm', usage: GPUTextureUsage.TEXTURE_BINDING }).createView();
  const hwSampler = (addr) => device.createSampler({ addressModeU: addr, addressModeV: addr, magFilter: 'linear', minFilter: 'linear', mipmapFilter: 'nearest' });
  const samplerWrap = hwSampler('repeat'), samplerClamp = hwSampler('clamp-to-edge');
  const TW_LEVELS = 7, TW_LVOFS = 8, TW_LVW = 24, TW_LVH = 40;
  function hwCreate(id) {
    const h = new Uint32Array(memory.buffer, id, TEX_HEADER / 4);
    return device.createTexture({ size: [h[TW_LVW], h[TW_LVH]], mipLevelCount: h[TW_LEVELS], format: 'rgba8unorm',
      usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
  }
  function hwWrite(tex, id, px) {
    const h = new Uint32Array(memory.buffer, id, TEX_HEADER / 4);
    for (let k = 0; k < h[TW_LEVELS]; k++) {
      const [lw, lh] = [h[TW_LVW + k], h[TW_LVH + k]];
      device.queue.writeTexture({ texture: tex, mipLevel: k }, memory.buffer, { offset: px + h[TW_LVOFS + k] * 4, bytesPerRow: lw * 4 }, [lw, lh]);
    }
  }
  const dynOfs = new Uint32Array(2);
  let ubuf = null, bindU = null, depth = null, frameNo = 0, keepDepth = false, msaaColor = null, lastSamples = 1;
  let shadowDepth = null;   // gölge derinliği; içerik her gölge geçişinde doku tamponuna kopyalanır
  let reflColor = null, reflDepth = null;   // su yansıması; renk her karede donanım dokusuna kopyalanır (D47, D50)

  // bindirme: motorun CPU'da çizdiği panel / editör, ekran boyu doku + makas
  let ovTex = null, ovBind = null;
  const ovPipes = new Map();
  const ovPipeline = (fmt) => {
    let p = ovPipes.get(fmt);
    if (!p) {
      const m = program(GPROG_OVERLAY).module;
      const blend = { srcFactor: 'one', dstFactor: 'one-minus-src-alpha' };
      p = device.createRenderPipeline({ layout: 'auto', vertex: { module: m, entryPoint: 'vs' },
        fragment: { module: m, entryPoint: 'fs', targets: [{ format: fmt, blend: { color: blend, alpha: blend } }] },
        primitive: { topology: 'triangle-list' } });
      ovPipes.set(fmt, p);
    }
    return p;
  };

  // GPU süresi: geçiş başı / sonu zaman damgaları (0-1 gölge, 2-3 ana, 4-5 yansıma); yoksa kuyruğun bitiş gecikmesi (üst sınır)
  const querySet = hasTs ? device.createQuerySet({ type: 'timestamp', count: 6 }) : null;
  const qResolve = hasTs ? device.createBuffer({ size: 48, usage: GPUBufferUsage.QUERY_RESOLVE | GPUBufferUsage.COPY_SRC }) : null;
  const qRead = hasTs ? [0, 1, 2].map(() => ({ busy: false, buf: device.createBuffer({ size: 48, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ }) })) : [];
  const gpuTime = { lastMs: -1, shadowMs: 0, mainMs: 0, reflMs: 0, precise: hasTs };

  function uploadMesh(w, i) {
    const [id, gen, vc, ic] = [w[i + 1], w[i + 2], w[i + 3], w[i + 4]];
    let m = meshes.get(id);
    if (m && m.gen === gen && m.vc === vc && m.ic === ic) { m.seen = frameNo; return; }
    // aynı boyut (her kare değişen mesh: okyanus, kumaş, kaplama): tamponlar kalır, yalnız içerik
    if (m && m.vc === vc && m.ic === ic) {
      m.bufs.forEach((b, k) => device.queue.writeBuffer(b, 0, memory.buffer, w[i + 5 + k], vc * 4));
      device.queue.writeBuffer(m.ibuf, 0, memory.buffer, w[i + 13], ic * 4);
      m.gen = gen;
      m.seen = frameNo;
      return;
    }
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
    const [id, px, words, gen] = [w[i + 1], w[i + 2], w[i + 3], w[i + 4]];
    const t = textures.get(id);
    if (t && t.px === px && t.words === words) {
      if (t.gen !== gen) {   // fizik alanı gibi her kare değişen doku: tampon kalır, yalnız içerik
        if (t.hw) hwWrite(t.hw, id, px);
        else device.queue.writeBuffer(t.buf, TEX_HEADER, memory.buffer, px, words * 4);
        t.gen = gen;
      }
      return;
    }
    if (t) { t.buf.destroy(); t.hw?.destroy(); }
    texBinds.clear();
    const buf = device.createBuffer({ size: TEX_HEADER + words * 4, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
    device.queue.writeBuffer(buf, 0, memory.buffer, id, TEX_HEADER);
    device.queue.writeBuffer(buf, TEX_HEADER, memory.buffer, px, words * 4);
    const hw = hwCreate(id);
    if (hw) hwWrite(hw, id, px);
    textures.set(id, { px, words, buf, gen, hw });
  }

  function texBind(a, b, c, e) {
    const key = `${a}|${b}|${c}|${e}`;
    let g = texBinds.get(key);
    if (!g) {
      const res = (id) => ({ buffer: id ? textures.get(id).buf : noTex });
      const hwRes = (id) => (id && textures.get(id).hw ? textures.get(id).hw.createView() : noHw);
      g = device.createBindGroup({ layout: bglT, entries: [{ binding: 0, resource: res(a) }, { binding: 1, resource: res(b) }, { binding: 2, resource: res(c) }, { binding: 3, resource: res(e) },
        { binding: 4, resource: hwRes(a) }, { binding: 5, resource: hwRes(b) }, { binding: 6, resource: hwRes(c) }, { binding: 7, resource: hwRes(e) },
        { binding: 8, resource: samplerWrap }, { binding: 9, resource: samplerClamp }] });
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
    let sPass = null, sMesh = null, sVert = -1, sFrag = -1, sCull = -1, sTest = -1, sWrite = -1, sT0 = -1, sT1 = -1, sT2 = -1, sT3 = -1;
    let pass = null, draws = 0, samples = 1, shadowTex = 0, shadowLayer = 0, tsShadow = false, inOverlay = false, reflTex = 0, tsRefl = false, passFmt = fmt;
    const stamp = (begin, end) => (hasTs ? { timestampWrites: { querySet, beginningOfPassWriteIndex: begin, endOfPassWriteIndex: end } } : {});
    for (let i = 6; i < w.length;) {
      const op = w[i];
      if (op === GC_END) break;
      if (op === GC_SHADOW) {
        const [id, size, layer, layers] = [w[i + 1], w[i + 2], w[i + 3], w[i + 4]];
        if (!shadowDepth || shadowDepth.width !== size) {
          shadowDepth?.destroy();
          shadowDepth = device.createTexture({ size: [size, size], format: 'depth32float', usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC });
        }
        if (!textures.has(id)) {
          const buf = device.createBuffer({ size: TEX_HEADER + size * size * layers * 4, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
          device.queue.writeBuffer(buf, 0, memory.buffer, id, TEX_HEADER);
          textures.set(id, { px: 0, words: size * size * layers, buf });
          texBinds.clear();
        }
        shadowTex = id;
        shadowLayer = layer;
        samples = 1;
        tsShadow = true;
        pass = enc.beginRenderPass({ colorAttachments: [], ...stamp(layer === 0 ? 0 : undefined, layer === layers - 1 ? 1 : undefined),
          depthStencilAttachment: { view: shadowDepth.createView(), depthLoadOp: 'clear', depthClearValue: 1, depthStoreOp: 'store' } });
        i += 5;
      } else if (op === GC_SHADOW_END) {
        pass.end();
        pass = null;
        const size = shadowDepth.width;
        enc.copyTextureToBuffer({ texture: shadowDepth, aspect: 'depth-only' }, { buffer: textures.get(shadowTex).buf, offset: TEX_HEADER + shadowLayer * size * size * 4, bytesPerRow: size * 4 }, [size, size]);
        i += 1;
      } else if (op === GC_REFLECT) {
        const [id, rw, rh] = [w[i + 1], w[i + 2], w[i + 3]];
        if (!reflColor || reflColor.width !== rw || reflColor.height !== rh) {
          reflColor?.destroy();
          reflDepth?.destroy();
          reflColor = device.createTexture({ size: [rw, rh], format: 'rgba8unorm', usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC });
          reflDepth = device.createTexture({ size: [rw, rh], format: 'depth32float', usage: GPUTextureUsage.RENDER_ATTACHMENT });
        }
        if (!textures.has(id)) {
          const buf = device.createBuffer({ size: TEX_HEADER + 4, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
          device.queue.writeBuffer(buf, 0, memory.buffer, id, TEX_HEADER);
          textures.set(id, { px: 0, words: 1, buf, hw: hwCreate(id) });
          texBinds.clear();
        }
        reflTex = id;
        samples = 1;
        tsRefl = true;
        passFmt = 'rgba8unorm';
        pass = enc.beginRenderPass({ colorAttachments: [{ view: reflColor.createView(), loadOp: 'clear', storeOp: 'store', clearValue: { r: 0, g: 0, b: 0, a: 0 } }], ...stamp(4, 5),
          depthStencilAttachment: { view: reflDepth.createView(), depthLoadOp: 'clear', depthClearValue: 1, depthStoreOp: 'discard' } });
        i += 4;
      } else if (op === GC_REFLECT_END) {
        pass.end();
        pass = null;
        enc.copyTextureToTexture({ texture: reflColor }, { texture: textures.get(reflTex).hw }, [reflColor.width, reflColor.height]);
        passFmt = fmt;
        i += 1;
      } else if (op === GC_FRAME) {
        const [fw, fh, rgba, zb] = [w[i + 1], w[i + 2], w[i + 3], w[i + 4]];
        samples = w[i + 5];
        if (!depth || depth.width !== fw || depth.height !== fh || depth.sampleCount !== samples) {
          depth?.destroy();
          depth = device.createTexture({ size: [fw, fh], format: 'depth32float', sampleCount: samples,
            usage: GPUTextureUsage.RENDER_ATTACHMENT | (samples === 1 ? GPUTextureUsage.COPY_SRC : 0) });
        }
        if (samples > 1 && (!msaaColor || msaaColor.width !== fw || msaaColor.height !== fh || msaaColor.format !== fmt)) {
          msaaColor?.destroy();
          msaaColor = device.createTexture({ size: [fw, fh], format: fmt, sampleCount: samples, usage: GPUTextureUsage.RENDER_ATTACHMENT });
        }
        lastSamples = samples;
        const zf = new Float32Array(new Uint32Array([zb]).buffer)[0];
        const clearValue = { r: (rgba & 255) / 255, g: ((rgba >>> 8) & 255) / 255, b: ((rgba >>> 16) & 255) / 255, a: (rgba >>> 24) / 255 };
        const color = samples > 1
          ? { view: msaaColor.createView(), resolveTarget: view, loadOp: 'clear', storeOp: 'discard', clearValue }
          : { view, loadOp: 'clear', storeOp: 'store', clearValue };
        pass = enc.beginRenderPass({ colorAttachments: [color], ...stamp(2, 3),
          depthStencilAttachment: { view: depth.createView(), depthLoadOp: 'clear', depthClearValue: zf, depthStoreOp: keepDepth && samples === 1 ? 'store' : 'discard' } });
        i += 6;
      } else if (op === GC_MESH) {
        uploadMesh(w, i);
        i += 14;
      } else if (op === GC_TEX) {
        uploadTex(w, i);
        i += 5;
      } else if (op === GC_DRAW) {
        const m = meshes.get(w[i + 1]);
        // aynı durum yeniden kurulmaz (D53): 1000 küpte gönderim ana iş parçacığının en büyük kalemi. Durum geçiş başına sıfırlanır
        if (pass !== sPass) { sPass = pass; sVert = sFrag = sCull = sTest = sWrite = sT0 = sT1 = sT2 = sT3 = -1; sMesh = null; }
        if (w[i + 4] !== sVert || w[i + 5] !== sFrag || w[i + 10] !== sCull || w[i + 11] !== sTest || w[i + 12] !== sWrite) {
          sVert = w[i + 4]; sFrag = w[i + 5]; sCull = w[i + 10]; sTest = w[i + 11]; sWrite = w[i + 12];
          pass.setPipeline(pipeline(sVert, sFrag, sCull, sTest, sWrite, passFmt, samples));
        }
        dynOfs[0] = w[i + 2] * SLOT;
        dynOfs[1] = w[i + 3] * SLOT;
        pass.setBindGroup(0, bindU, dynOfs, 0, 2);
        if (w[i + 6] !== sT0 || w[i + 7] !== sT1 || w[i + 8] !== sT2 || w[i + 9] !== sT3) {
          sT0 = w[i + 6]; sT1 = w[i + 7]; sT2 = w[i + 8]; sT3 = w[i + 9];
          pass.setBindGroup(1, texBind(sT0, sT1, sT2, sT3));
        }
        if (m !== sMesh) {
          sMesh = m;
          m.bufs.forEach((b, k) => pass.setVertexBuffer(k, b));
          pass.setIndexBuffer(m.ibuf, 'uint32');
        }
        pass.drawIndexed(m.ic);
        draws++;
        i += 13;
      } else if (op === GC_OVERLAY) {
        const [x, y, ow, oh, px] = [w[i + 1], w[i + 2], w[i + 3], w[i + 4], w[i + 5]];
        if (!inOverlay) {
          pass.end();
          pass = enc.beginRenderPass({ colorAttachments: [{ view, loadOp: 'load', storeOp: 'store' }] });
          inOverlay = true;
        }
        const [fw, fh] = [depth.width, depth.height];
        if (!ovTex || ovTex.width !== fw || ovTex.height !== fh) {
          ovTex?.destroy();
          ovTex = device.createTexture({ size: [fw, fh], format: 'rgba8unorm', usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
          ovBind = null;
        }
        device.queue.writeTexture({ texture: ovTex, origin: [x, y] }, new Uint8Array(memory.buffer, px, ow * oh * 4).slice(), { bytesPerRow: ow * 4 }, [ow, oh]);
        const pipe = ovPipeline(fmt);
        if (!ovBind || ovBind.fmt !== fmt) ovBind = { fmt, group: device.createBindGroup({ layout: pipe.getBindGroupLayout(0), entries: [{ binding: 0, resource: ovTex.createView() }] }) };
        pass.setPipeline(pipe);
        pass.setBindGroup(0, ovBind.group);
        pass.setScissorRect(x, y, ow, oh);
        pass.draw(3);
        i += 6;
      } else {
        throw new Error('GPU akışı: bilinmeyen kayıt ' + op);
      }
    }
    pass?.end();
    const slot = hasTs ? qRead.find((q) => !q.busy) : null;
    if (slot) {
      enc.resolveQuerySet(querySet, 0, 6, qResolve, 0);
      enc.copyBufferToBuffer(qResolve, 0, slot.buf, 0, 48);
    }
    const t0 = performance.now();
    device.queue.submit([enc.finish()]);
    if (slot) {
      slot.busy = true;
      const hadShadow = tsShadow, hadRefl = tsRefl;
      slot.buf.mapAsync(GPUMapMode.READ).then(() => {
        const q = new BigInt64Array(slot.buf.getMappedRange().slice(0));
        slot.buf.unmap();
        slot.busy = false;
        const d = (a, b) => Number(q[b] - q[a]) / 1e6;
        gpuTime.shadowMs = hadShadow ? d(0, 1) : 0;
        gpuTime.mainMs = d(2, 3);
        gpuTime.reflMs = hadRefl ? d(4, 5) : 0;
        gpuTime.lastMs = gpuTime.shadowMs + gpuTime.mainMs + gpuTime.reflMs;
      }).catch(() => { slot.busy = false; });
    } else if (!hasTs) {
      device.queue.onSubmittedWorkDone().then(() => { gpuTime.lastMs = performance.now() - t0; });
    }
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
    // çok örnekli derinlik kopyalanamaz (WebGPU): MSAA'da yalnız renk
    const depthOut = lastSamples === 1 ? new Float32Array((await copyOut(depth, width, height, 'depth-only')).buffer) : null;
    tex.destroy();
    return { pixels, depth: depthOut, draws };
  }

  return { device, adapter, render, readback, gpuTime, meshCount: () => meshes.size };
}
