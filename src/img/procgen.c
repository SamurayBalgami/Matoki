/* img/procgen.c — dama tahtası, ızgara, tuğla ve gerçekçi malzeme desenleri (D52).
 * Tuğla rengi blok başına hash ile değişir; harç sabit kalınlıkta, koyu ve içeride (kabartı parlaklıktan okunur).
 * Malzemeler periyodik değer gürültüsünden (fbm): örgü doku boyuna tam sığar, kenarlar dikişsiz.
 * Deterministik: aynı seed → aynı doku. */
#include "img/procgen.h"
#include "core/math.h"
#include "core/rng.h"

void procgen_checker(Texture* t, u32 cell, u32 c0, u32 c1) {
    for (u32 y = 0; y < t->h; y++) {
        for (u32 x = 0; x < t->w; x++) {
            t->px[y * t->w + x] = (((x / cell) ^ (y / cell)) & 1) ? c1 : c0;
        }
    }
}

void procgen_grid(Texture* t, u32 cell, u32 bg, u32 line) {
    for (u32 y = 0; y < t->h; y++) {
        for (u32 x = 0; x < t->w; x++) {
            b32 on = (x % cell) < 2 || (y % cell) < 2;
            t->px[y * t->w + x] = on ? line : bg;
        }
    }
}

/* periyodik değer gürültüsü: (x, y) ∈ [0, 1), örgü period × period; sonuç [0, 1] */
static f32 lattice(u32 x, u32 y, u32 seed) {
    return (f32)(hash_u32(seed ^ (x * 73856093u) ^ (y * 19349663u)) & 0xFFFFu) / 65535.0f;
}

static f32 value_noise(f32 x, f32 y, u32 period, u32 seed) {
    f32 fx = x * (f32)period, fy = y * (f32)period;
    u32 ix = (u32)fx, iy = (u32)fy;
    f32 ax = fx - (f32)ix, ay = fy - (f32)iy;
    ax = ax * ax * (3.0f - 2.0f * ax);
    ay = ay * ay * (3.0f - 2.0f * ay);
    u32 x0 = ix % period, x1 = (ix + 1u) % period, y0 = iy % period, y1 = (iy + 1u) % period;
    f32 a = m_lerpf(lattice(x0, y0, seed), lattice(x1, y0, seed), ax);
    f32 b = m_lerpf(lattice(x0, y1, seed), lattice(x1, y1, seed), ax);
    return m_lerpf(a, b, ay);
}

/* oktavlar: örgü ×2, genlik ×1/2; sonuç [0, 1] */
static f32 fbm(f32 x, f32 y, u32 period, u32 octaves, u32 seed) {
    f32 sum = 0.0f, amp = 0.5f, norm = 0.0f;
    for (u32 i = 0; i < octaves; i++) {
        sum += value_noise(x, y, period, seed + i * 131u) * amp;
        norm += amp;
        amp *= 0.5f;
        period *= 2u;
    }
    return sum / norm;
}

static u32 pack_f(f32 r, f32 g, f32 b) {
    return color_pack((u32)(m_clampf(r, 0.0f, 1.0f) * 255.0f + 0.5f), (u32)(m_clampf(g, 0.0f, 1.0f) * 255.0f + 0.5f),
                      (u32)(m_clampf(b, 0.0f, 1.0f) * 255.0f + 0.5f), 255);
}

/* piksel başına ince gren, [-1, 1] */
static f32 grain(u32 x, u32 y, u32 seed) {
    return lattice(x, y, seed) * 2.0f - 1.0f;
}

void procgen_bricks(Texture* t, u32 seed) {
    const u32 bw = t->w / 4, bh = t->h / 8;
    const u32 joint = t->h / 64 + 1u;
    for (u32 y = 0; y < t->h; y++) {
        u32 row = y / bh;
        u32 shift = (row & 1) ? bw / 2 : 0;
        for (u32 x = 0; x < t->w; x++) {
            u32 xs = (x + shift) % t->w;
            u32 col = xs / bw;
            f32 nx = (f32)x / (f32)t->w, ny = (f32)y / (f32)t->h;
            f32 m = fbm(nx, ny, 8, 4, seed);
            if ((y % bh) < joint || (xs % bw) < joint) {
                f32 k = 0.24f + 0.12f * m + 0.02f * grain(x, y, seed + 5u);
                t->px[y * t->w + x] = pack_f(k * 1.02f, k, k * 0.95f);
                continue;
            }
            /* blok başına ton: kiremitten koyu kahveye, arada is lekesi */
            u32 hsh = hash_u32(seed ^ (row * 73856093u) ^ (col * 19349663u));
            f32 v = (f32)(hsh & 255u) / 255.0f;
            f32 burnt = (f32)((hsh >> 8) & 255u) / 255.0f;
            f32 l = (0.5f + 0.14f * v + 0.2f * (m - 0.5f) + 0.025f * grain(x, y, seed + 9u)) * (burnt > 0.86f ? 0.78f : 1.0f);
            t->px[y * t->w + x] = pack_f(l, l * (0.56f + 0.06f * v), l * (0.43f + 0.05f * v));
        }
    }
}

void procgen_pavers(Texture* t, u32 slabs, u32 seed) {
    const u32 cell = t->w / slabs;
    const u32 joint = cell / 32u + 1u;
    for (u32 y = 0; y < t->h; y++) {
        u32 row = y / cell;
        u32 shift = (row & 1) ? cell / 2 : 0;
        for (u32 x = 0; x < t->w; x++) {
            u32 xs = (x + shift) % t->w;
            u32 col = xs / cell;
            u32 ex = xs % cell, ey = y % cell;
            f32 nx = (f32)x / (f32)t->w, ny = (f32)y / (f32)t->h;
            f32 blot = fbm(nx, ny, 6, 5, seed);
            f32 g = grain(x, y, seed + 3u);
            /* kenardan uzaklık: derz, sonra hafif aşınmış taş kenarı */
            u32 edge = MIN(MIN(ex, cell - 1u - ex), MIN(ey, cell - 1u - ey));
            if (edge < joint) {
                f32 k = 0.24f + 0.1f * blot + 0.02f * g;
                t->px[y * t->w + x] = pack_f(k * 1.03f, k, k * 0.93f);
                continue;
            }
            u32 hsh = hash_u32(seed ^ (row * 2654435761u) ^ (col * 83492791u));
            f32 tone = (f32)(hsh & 255u) / 255.0f;
            f32 warm = (f32)((hsh >> 8) & 255u) / 255.0f - 0.5f;
            f32 wear = m_minf((f32)(edge - joint) / (f32)(joint * 3u), 1.0f);
            f32 fine = fbm(nx, ny, 64, 2, seed + 40u);
            f32 l = (0.385f + 0.075f * tone + 0.2f * (blot - 0.5f) + 0.06f * (fine - 0.5f) + 0.018f * g) * (0.86f + 0.14f * wear);
            t->px[y * t->w + x] = pack_f(l * (1.03f + 0.04f * warm), l, l * (0.92f - 0.05f * warm));
        }
    }
}

void procgen_rock(Texture* t, u32 seed) {
    for (u32 y = 0; y < t->h; y++) {
        for (u32 x = 0; x < t->w; x++) {
            f32 nx = (f32)x / (f32)t->w, ny = (f32)y / (f32)t->h;
            f32 f = fbm(nx, ny, 4, 6, seed);
            /* damar: gürültünün orta değerinden geçişi ince koyu çizgi */
            f32 vein = m_clampf(m_absf(fbm(nx, ny, 6, 4, seed + 77u) - 0.5f) * 18.0f, 0.0f, 1.0f);
            f32 l = (0.34f + 0.62f * f + 0.03f * grain(x, y, seed + 1u)) * (0.72f + 0.28f * vein);
            t->px[y * t->w + x] = pack_f(l * 1.02f, l, l * 0.95f);
        }
    }
}

void procgen_grass(Texture* t, u32 seed) {
    for (u32 y = 0; y < t->h; y++) {
        for (u32 x = 0; x < t->w; x++) {
            f32 nx = (f32)x / (f32)t->w, ny = (f32)y / (f32)t->h;
            f32 patch = fbm(nx, ny, 4, 5, seed);
            f32 blade = fbm(nx, ny, 48, 2, seed + 50u);
            f32 dry = m_clampf((patch - 0.42f) * 2.2f, 0.0f, 1.0f);
            f32 l = 0.56f + 0.5f * (blade - 0.5f) + 0.05f * grain(x, y, seed + 2u);
            t->px[y * t->w + x] = pack_f(l * m_lerpf(0.42f, 0.56f, dry), l * m_lerpf(0.62f, 0.64f, dry), l * m_lerpf(0.26f, 0.3f, dry));
        }
    }
}

void procgen_cloth(Texture* t, u32 seed) {
    for (u32 y = 0; y < t->h; y++) {
        for (u32 x = 0; x < t->w; x++) {
            f32 nx = (f32)x / (f32)t->w, ny = (f32)y / (f32)t->h;
            /* üstten / alttan geçen iplik: iki pikselde bir sıra */
            f32 weave = (((x >> 1) ^ (y >> 1)) & 1u) ? 1.0f : 0.84f;
            f32 l = weave * (0.84f + 0.1f * (fbm(nx, ny, 5, 4, seed) - 0.5f) + 0.02f * grain(x, y, seed + 6u));
            t->px[y * t->w + x] = pack_f(l, l, l);
        }
    }
}

void procgen_paint(Texture* t, u32 seed) {
    for (u32 y = 0; y < t->h; y++) {
        for (u32 x = 0; x < t->w; x++) {
            f32 nx = (f32)x / (f32)t->w, ny = (f32)y / (f32)t->h;
            f32 blot = fbm(nx, ny, 3, 5, seed);
            /* kir kenarda toplanır: dokunun kenarı yüzün kenarı */
            f32 ex = m_minf(nx, 1.0f - nx), ey = m_minf(ny, 1.0f - ny);
            f32 edge = m_clampf(m_minf(ex, ey) * 14.0f, 0.0f, 1.0f);
            f32 scuff = m_clampf((fbm(nx, ny, 12, 3, seed + 30u) - 0.64f) * 4.0f, 0.0f, 1.0f);
            f32 l = (0.84f + 0.14f * (blot - 0.5f) + 0.012f * grain(x, y, seed + 7u)) * (0.74f + 0.26f * edge) * (1.0f - 0.07f * scuff);
            t->px[y * t->w + x] = pack_f(l, l, l * 0.98f);
        }
    }
}

void procgen_macro(Texture* t, u32 seed) {
    for (u32 y = 0; y < t->h; y++) {
        for (u32 x = 0; x < t->w; x++) {
            f32 f = fbm((f32)x / (f32)t->w, (f32)y / (f32)t->h, 5, 5, seed);
            t->px[y * t->w + x] = pack_f(0.0f, 0.0f, m_clampf((f - 0.28f) * 2.2f, 0.0f, 1.0f));
        }
    }
}

void procgen_clouds(Texture* t, u32 seed, f32 sun_x, f32 sun_y) {
    for (u32 y = 0; y < t->h; y++) {
        for (u32 x = 0; x < t->w; x++) {
            f32 nx = (f32)x / (f32)t->w, ny = (f32)y / (f32)t->h;
            f32 d = fbm(nx, ny, 4, 6, seed);
            /* güneşe doğru yoğunluk azalıyorsa bu nokta aydınlık yüzde (yönlü türev, tek adım) */
            f32 sx = nx + sun_x * 0.025f, sy = ny + sun_y * 0.025f;
            sx -= m_floorf(sx);
            sy -= m_floorf(sy);
            f32 lit = m_clampf(0.55f + (d - fbm(sx, sy, 4, 6, seed)) * 6.0f, 0.0f, 1.0f);
            t->px[y * t->w + x] = pack_f(d, lit, 0.0f);
        }
    }
}
