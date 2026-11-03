/* phys/wave.c — yüzey dalga denklemi (phys/wave.h). Leapfrog: h⁺ = h + (h − h⁻)(1 − k·dt) + r²·∇²h, r = c·dt/dx.
 * 2B kararlılık r² ≤ 1/2; adım r² = 0,25 olacak şekilde seçilir, dağılım az kalır. Dış kenar ve engeller h = 0 (sabit uç). */
#include "phys/wave.h"
#include "core/math.h"
#include "core/mem.h"

#define WAVE_R2 0.25f
#define WAVE_MAX_STEPS 8u   /* kare başına: uzun karede zaman atılır, motor geride kalmaz */

void wave_init(WaveField* w, Arena* a, u32 n, f32 size, f32 c, f32 damping) {
    mem_zero(w, sizeof(*w));
    w->n = n;
    w->size = size;
    w->dx = size / (f32)(n - 1u);
    w->c = c;
    w->damping = damping;
    w->r2 = WAVE_R2;
    w->dt = m_sqrtf(WAVE_R2) * w->dx / c;
    w->h = ARENA_ARRAY(a, f32, (usize)n * n);
    w->hp = ARENA_ARRAY(a, f32, (usize)n * n);
    w->solid = ARENA_ARRAY(a, u8, (usize)n * n);
}

void wave_step(WaveField* w, f32 dt) {
    w->accum += dt;
    u32 steps = 0;
    u32 n = w->n;
    f32 keep = 1.0f - w->damping * w->dt, r2 = w->r2;
    while (w->accum >= w->dt && steps < WAVE_MAX_STEPS) {
        w->accum -= w->dt;
        steps++;
        /* hp yerinde h⁺ olur, sonra işaretçiler yer değiştirir: ek bellek yok */
        for (u32 y = 1; y + 1 < n; y++) {
            f32* out = w->hp + (usize)y * n;
            const f32* c = w->h + (usize)y * n;
            const u8* s = w->solid + (usize)y * n;
            for (u32 x = 1; x + 1 < n; x++) {
                f32 lap = c[x - 1] + c[x + 1] + c[(i32)x - (i32)n] + c[x + n] - 4.0f * c[x];
                f32 next = c[x] + (c[x] - out[x]) * keep + r2 * lap;
                out[x] = s[x] ? 0.0f : next;
            }
        }
        f32* t = w->h;
        w->h = w->hp;
        w->hp = t;
    }
    if (w->accum >= w->dt) w->accum = 0.0f;
}

void wave_add(WaveField* w, f32 x, f32 z, f32 radius, f32 amount) {
    i32 n = (i32)w->n;
    f32 fx = (x + w->size * 0.5f) / w->dx, fz = (z + w->size * 0.5f) / w->dx;
    i32 r = (i32)(radius * 3.0f / w->dx) + 1;
    i32 x0 = (i32)fx - r, x1 = (i32)fx + r, y0 = (i32)fz - r, y1 = (i32)fz + r;
    if (x0 < 1) x0 = 1;
    if (y0 < 1) y0 = 1;
    if (x1 > n - 2) x1 = n - 2;
    if (y1 > n - 2) y1 = n - 2;
    f32 k = w->dx * w->dx / (2.0f * radius * radius);
    for (i32 j = y0; j <= y1; j++) {
        for (i32 i = x0; i <= x1; i++) {
            usize c = (usize)j * (usize)n + (usize)i;
            if (w->solid[c]) continue;
            f32 ax = (f32)i - fx, az = (f32)j - fz;
            /* hız vermeden: iki zaman düzeyine birden, tepe durağan başlar */
            f32 g = amount * m_expf(-(ax * ax + az * az) * k);
            w->h[c] += g;
            w->hp[c] += g;
        }
    }
}

void wave_block(WaveField* w, f32 x, f32 z, f32 half_x, f32 half_z) {
    i32 n = (i32)w->n;
    f32 o = w->size * 0.5f;
    i32 x0 = (i32)((x - half_x + o) / w->dx), x1 = (i32)((x + half_x + o) / w->dx);
    i32 y0 = (i32)((z - half_z + o) / w->dx), y1 = (i32)((z + half_z + o) / w->dx);
    for (i32 j = y0; j <= y1; j++) {
        for (i32 i = x0; i <= x1; i++) {
            if (i < 0 || j < 0 || i >= n || j >= n) continue;
            usize c = (usize)j * (usize)n + (usize)i;
            w->solid[c] = 1;
            w->h[c] = w->hp[c] = 0.0f;
        }
    }
}

f32 wave_height(const WaveField* w, f32 x, f32 z) {
    f32 fx = (x + w->size * 0.5f) / w->dx, fz = (z + w->size * 0.5f) / w->dx;
    i32 n = (i32)w->n;
    if (fx < 0.0f || fz < 0.0f || fx >= (f32)(n - 1) || fz >= (f32)(n - 1)) return 0.0f;
    i32 i = (i32)fx, j = (i32)fz;
    f32 ax = fx - (f32)i, az = fz - (f32)j;
    const f32* p = w->h + (usize)j * (usize)n + (usize)i;
    return (p[0] * (1.0f - ax) + p[1] * ax) * (1.0f - az) + (p[n] * (1.0f - ax) + p[n + 1] * ax) * az;
}

f32 wave_energy(const WaveField* w) {
    u32 n = w->n;
    f32 ek = 0.0f, ep = 0.0f;
    /* leapfrog'un korunan biçimi: gradyan iki zaman düzeyinin çarpımı; kenar hücrelerine giden farklar dahil */
    for (u32 y = 0; y < n; y++) {
        const f32* h = w->h + (usize)y * n;
        const f32* p = w->hp + (usize)y * n;
        for (u32 x = 0; x < n; x++) {
            f32 v = h[x] - p[x];
            ek += v * v;
            if (x + 1 < n) ep += (h[x + 1] - h[x]) * (p[x + 1] - p[x]);
            if (y + 1 < n) ep += (h[x + n] - h[x]) * (p[x + n] - p[x]);
        }
    }
    return 0.5f * (ek / (w->dt * w->dt) + ep * w->c * w->c / (w->dx * w->dx)) * w->dx * w->dx;
}

static u32 wave_byte(f32 v) {
    i32 b = (i32)(v + 128.5f);
    return b < 0 ? 0u : b > 255 ? 255u : (u32)b;
}

void wave_bake(const WaveField* w, u32* px) {
    u32 n = w->n;
    f32 k = 510.0f / (2.0f * w->dx);
    for (u32 y = 0; y < n; y++) {
        const f32* h = w->h + (usize)y * n;
        u32* out = px + (usize)y * n;
        for (u32 x = 0; x < n; x++) {
            f32 l = x ? h[x - 1] : h[x], r = x + 1 < n ? h[x + 1] : h[x];
            f32 u = y ? h[(i32)x - (i32)n] : h[x], d = y + 1 < n ? h[x + n] : h[x];
            out[x] = wave_byte((r - l) * k) | (wave_byte((d - u) * k) << 8) | (wave_byte(h[x] * 255.0f) << 16) | 0xFF000000u;
        }
    }
}
