// assets/gpu_overlay.wgsl — GPU arka ucu bindirmesi (D44; program 1): motor içi panel ve editör CPU'da çizilir, ekran boyu
// dokuya yüklenir. Üçgen tüm ekranı örter, makas dikdörtgeni sınırlar; önceden çarpılmış renk (one, 1 - alfa) ile karışır.

@group(0) @binding(0) var t: texture_2d<f32>;

@vertex
fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let p = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
    return vec4f(p[i], 0.0, 1.0);
}

@fragment
fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    return textureLoad(t, vec2i(pos.xy), 0);
}
