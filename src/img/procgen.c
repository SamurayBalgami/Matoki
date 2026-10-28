/* img/procgen.c — dama tahtası, ızgara ve tuğla desenleri.
 * Tuğla rengi blok başına hash ile değişir; harç çizgileri sabit kalınlıkta.
 * Deterministik: aynı seed → aynı doku. */
#include "img/procgen.h"
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

static u32 scale_rgb(u32 c, u32 k256) {
    u32 r = ((c & 0xFF) * k256) >> 8;
    u32 g = (((c >> 8) & 0xFF) * k256) >> 8;
    u32 b = (((c >> 16) & 0xFF) * k256) >> 8;
    return color_pack(MIN(r, 255u), MIN(g, 255u), MIN(b, 255u), 255);
}

void procgen_bricks(Texture* t, u32 seed) {
    const u32 bw = t->w / 4, bh = t->h / 8;
    const u32 brick = color_pack(168, 74, 52, 255);
    const u32 mortar = color_pack(200, 196, 186, 255);
    for (u32 y = 0; y < t->h; y++) {
        u32 row = y / bh;
        u32 shift = (row & 1) ? bw / 2 : 0;
        for (u32 x = 0; x < t->w; x++) {
            u32 xs = (x + shift) % t->w;
            u32 col = xs / bw;
            b32 is_mortar = (y % bh) < 2 || (xs % bw) < 2;
            u32 c;
            if (is_mortar) {
                c = mortar;
            } else {
                u32 hsh = hash_u32(seed ^ (row * 73856093u) ^ (col * 19349663u));
                u32 noise = hash_u32(hsh ^ (x * 83492791u) ^ (y * 2654435761u)) & 31;
                c = scale_rgb(brick, 200 + (hsh & 63) + noise);
            }
            t->px[y * t->w + x] = c;
        }
    }
}
