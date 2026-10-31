// assets/gpu_8a.wgsl — GPU arka ucu faz 8a (D38): konum dönüşümü std_vert / vert_std_c ile aynı sırada
// (wp = model · p, klip = view_proj · wp). İki parça shader: fs_flat yazılım yolundaki shade_flat'in birebir eşi
// (2×2 dörtlünün başlangıç pikselinin x, y'sinin düşük 8 biti, mavi 128) → iki arka uç yalnız kaplama ve derinlikte
// ayrışabilir (karşılaştırma testi); fs_normal basit yönlü ışık (8b'de Matoki → WGSL gelene dek görüntü).
// Köşe akışları SoA (Mesh): px, py, pz, nx, ny, nz ayrı tamponlar. Uniform: çizim başına 256 baytlık dinamik ofset.

struct Draw {
    model: mat4x4f,
    view_proj: mat4x4f,
    normal_mat: mat3x3f,
};
@group(0) @binding(0) var<uniform> d: Draw;

struct VOut {
    @builtin(position) pos: vec4f,
    @location(0) n: vec3f,
};

@vertex
fn vs(@location(0) px: f32, @location(1) py: f32, @location(2) pz: f32,
      @location(3) nx: f32, @location(4) ny: f32, @location(5) nz: f32) -> VOut {
    var o: VOut;
    let wp = d.model * vec4f(px, py, pz, 1.0);
    o.pos = d.view_proj * wp;
    o.n = d.normal_mat * vec3f(nx, ny, nz);
    return o;
}

@fragment
fn fs_flat(v: VOut) -> @location(0) vec4f {
    let q = vec2u(v.pos.xy) & vec2u(0xFEu, 0xFEu);
    return vec4f(f32(q.x) / 255.0, f32(q.y) / 255.0, 128.0 / 255.0, 1.0);
}

@fragment
fn fs_normal(v: VOut) -> @location(0) vec4f {
    let n = normalize(v.n);
    let l = max(dot(n, normalize(vec3f(0.4, 0.8, 0.35))), 0.0);
    return vec4f(vec3f(0.35 + 0.6 * l), 1.0);
}
