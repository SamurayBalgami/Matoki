// assets/gpu_flat.wgsl — GPU arka ucu düz shader'ı (D38; program 0): yazılım yolundaki shade_flat'in birebir eşi (2×2
// dörtlünün başlangıç pikselinin x, y'sinin düşük 8 biti, mavi 128) → iki arka uç yalnız kaplama ve derinlikte ayrışabilir
// (web/gpu_check.html). Köşe aşaması Matoki std_vert'ten; girdiler matoki/wgsl.c VOut yerleşimiyle aynı.

struct VOut {
    @builtin(position) pos: vec4f,
    @location(0) v0: vec4f,
    @location(1) v1: vec4f,
};

@fragment
fn fs(v: VOut) -> @location(0) vec4f {
    let q = vec2u(v.pos.xy) & vec2u(0xFEu, 0xFEu);
    return vec4f(f32(q.x) / 255.0, f32(q.y) / 255.0, 128.0 / 255.0, 1.0);
}
