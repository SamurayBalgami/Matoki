/* raster/raster_simd_wasm.c — wasm SIMD karo döngüsü (quad = 4 lane = bir v128).
 * Kaplama: kenar fonksiyonları i64x2 çiftleri (lane 0/1 ve 2/3), işaret bitleri bitmask ile.
 * Derinlik/renk: quad'ın iki satırı 64-bit yük/saklama ile tek vektörde. Öznitelikler f32x4.
 * Kayan nokta işlem sırası raster_tile_scalar ile AYNI → çıktı bit-bit aynı (FMA yok). */
#include <wasm_simd128.h>
#include "raster/raster_internal.h"

static inline v128_t plane4(v128_t a, v128_t b, v128_t c, v128_t fxv, v128_t fyv) {
    return wasm_f32x4_add(wasm_f32x4_add(c, wasm_f32x4_mul(a, fxv)), wasm_f32x4_mul(b, fyv));
}

static inline v128_t load_quad(const void* row0, const void* row1) {
    v128_t v = wasm_v128_load64_zero(row0);
    return wasm_v128_load64_lane(row1, v, 1);
}

static inline void store_quad(void* row0, void* row1, v128_t v) {
    wasm_v128_store64_lane(row0, v, 0);
    wasm_v128_store64_lane(row1, v, 1);
}

static inline u32 depth_mask4(u32 func, v128_t z, v128_t d) {
    switch ((DepthFunc)func) {
    case DEPTH_LESS:    return (u32)wasm_i32x4_bitmask(wasm_f32x4_lt(z, d));
    case DEPTH_LEQUAL:  return (u32)wasm_i32x4_bitmask(wasm_f32x4_le(z, d));
    case DEPTH_GREATER: return (u32)wasm_i32x4_bitmask(wasm_f32x4_gt(z, d));
    case DEPTH_GEQUAL:  return (u32)wasm_i32x4_bitmask(wasm_f32x4_ge(z, d));
    case DEPTH_ALWAYS:  return 0xF;
    }
    return 0xF;
}

void raster_tile_simd(Raster* r, TriSetup* t, i32 tx0, i32 ty0, i32 tx1, i32 ty1, b32 full, RasterStats* st) {
    Framebuffer* fb = r->fb;
    i32 minx = MAX(t->minx, tx0) & ~1, miny = MAX(t->miny, ty0) & ~1;
    i32 maxx = MIN(t->maxx, tx1), maxy = MIN(t->maxy, ty1);
    if (minx > maxx || miny > maxy) return;
    const i32 fbw = fb->w, fbh = fb->h;
    const u32 nv = t->var_count;

    const v128_t lxf = wasm_f32x4_make(0.0f, 1.0f, 0.0f, 1.0f);
    const v128_t lyf = wasm_f32x4_make(0.0f, 0.0f, 1.0f, 1.0f);
    const v128_t lane_bits = wasm_i32x4_make(1, 2, 4, 8);
    const v128_t zero = wasm_i32x4_splat(0);
    const v128_t one = wasm_f32x4_splat(1.0f);

    /* kenarlar: lo = (lane0, lane1), hi = (lane2, lane3) */
    v128_t row_lo[3], row_hi[3], step_x[3], step_y[3];
    for (u32 k = 0; k < 3; k++) {
        const EdgeEq* e = &t->e[k];
        i64 base = e->c + e->sx * minx + e->sy * miny;
        row_lo[k] = wasm_i64x2_make(base, base + e->sx);
        row_hi[k] = wasm_i64x2_make(base + e->sy, base + e->sx + e->sy);
        step_x[k] = wasm_i64x2_splat(2 * e->sx);
        step_y[k] = wasm_i64x2_splat(2 * e->sy);
    }

    const v128_t za = wasm_f32x4_splat(t->z.a), zb = wasm_f32x4_splat(t->z.b), zc = wasm_f32x4_splat(t->z.c);
    const v128_t wa = wasm_f32x4_splat(t->w.a), wb = wasm_f32x4_splat(t->w.b), wc = wasm_f32x4_splat(t->w.c);
    v128_t va[SHADE_MAX_VARS], vb[SHADE_MAX_VARS], vc[SHADE_MAX_VARS];
    for (u32 i = 0; i < nv; i++) {
        va[i] = wasm_f32x4_splat(t->v[i].a);
        vb[i] = wasm_f32x4_splat(t->v[i].b);
        vc[i] = wasm_f32x4_splat(t->v[i].c);
    }

    const u32 blend = t->blend;   /* shader çağrısı opak: t->blend her quad'da yeniden okunmasın */
    ShadeQuad q;
    q.kill = 0;
    u32 out[4];
    u32 quads = 0, pixels = 0;

    for (i32 qy = miny; qy <= maxy; qy += 2) {
        v128_t lo0 = row_lo[0], lo1 = row_lo[1], lo2 = row_lo[2];
        v128_t hi0 = row_hi[0], hi1 = row_hi[1], hi2 = row_hi[2];
        const f32 fy = (f32)qy + 0.5f - t->y0;
        const v128_t fyv = wasm_f32x4_add(wasm_f32x4_splat(fy), lyf);
        const b32 last_row = qy + 1 >= fbh;
        u32* crow = fb->color + qy * fbw;
        f32* drow = fb->depth ? fb->depth + qy * fbw : NULL;

        for (i32 qx = minx; qx <= maxx; qx += 2) {
            u32 cov;
            if (full) {
                cov = 0xF;
            } else {
                v128_t olo = wasm_v128_or(wasm_v128_or(lo0, lo1), lo2);
                v128_t ohi = wasm_v128_or(wasm_v128_or(hi0, hi1), hi2);
                cov = (~(u32)wasm_i64x2_bitmask(olo) & 3u) | ((~(u32)wasm_i64x2_bitmask(ohi) & 3u) << 2);
                lo0 = wasm_i64x2_add(lo0, step_x[0]);
                lo1 = wasm_i64x2_add(lo1, step_x[1]);
                lo2 = wasm_i64x2_add(lo2, step_x[2]);
                hi0 = wasm_i64x2_add(hi0, step_x[0]);
                hi1 = wasm_i64x2_add(hi1, step_x[1]);
                hi2 = wasm_i64x2_add(hi2, step_x[2]);
            }
            const b32 inner = qx + 1 < fbw && !last_row;
            if (!inner) {
                if (qx + 1 >= fbw) cov &= ~(2u | 8u);
                if (last_row) cov &= ~(4u | 8u);
            }
            if (!cov) continue;

            const f32 fx = (f32)qx + 0.5f - t->x0;
            const v128_t fxv = wasm_f32x4_add(wasm_f32x4_splat(fx), lxf);
            const v128_t zv = plane4(za, zb, zc, fxv, fyv);
            u32 pass = cov;
            if (t->depth_test) {
                v128_t dv;
                if (inner) {
                    dv = load_quad(drow + qx, drow + fbw + qx);
                } else {
                    f32 d[4] = { 0, 0, 0, 0 };
                    for (u32 l = 0; l < 4; l++) {
                        if (cov & (1u << l)) d[l] = drow[(i32)(l >> 1) * fbw + qx + (i32)(l & 1)];
                    }
                    dv = wasm_v128_load(d);
                }
                pass &= depth_mask4(t->depth_func, zv, dv);
                if (!pass) continue;
            }

            if (!t->shade) {   /* yalnız derinlik (gölge haritası): varying ve renk yok */
                if (t->depth_write) {
                    if (inner) {
                        v128_t m = wasm_i32x4_ne(wasm_v128_and(wasm_i32x4_splat((i32)pass), lane_bits), zero);
                        f32* d0 = drow + qx;
                        f32* d1 = drow + fbw + qx;
                        store_quad(d0, d1, wasm_v128_bitselect(zv, load_quad(d0, d1), m));
                    } else {
                        f32 zl[4];
                        wasm_v128_store(zl, zv);
                        for (u32 l = 0; l < 4; l++) {
                            if (pass & (1u << l)) drow[(i32)(l >> 1) * fbw + qx + (i32)(l & 1)] = zl[l];
                        }
                    }
                }
                quads++;
                pixels += (u32)__builtin_popcount(pass);
                continue;
            }

            q.x = qx;
            q.y = qy;
            q.mask = pass;
            wasm_v128_store(q.z, zv);
            const v128_t wv = wasm_f32x4_div(one, plane4(wa, wb, wc, fxv, fyv));
            for (u32 i = 0; i < nv; i++) {
                wasm_v128_store(q.var[i], wasm_f32x4_mul(plane4(va[i], vb[i], vc[i], fxv, fyv), wv));
            }
            q.kill = 0;
            t->shade(&q, t->uniforms, out);
            pass &= ~q.kill;
            if (!pass) {   /* tümü atıldı: gölgelendi ama yazılacak piksel yok */
                quads++;
                continue;
            }

            if (inner && !blend) {
                v128_t ov = wasm_v128_load(out);
                u32* c0 = crow + qx;
                u32* c1 = crow + fbw + qx;
                if (pass == 0xF) {
                    store_quad(c0, c1, ov);
                    if (t->depth_write) store_quad(drow + qx, drow + fbw + qx, zv);
                } else {
                    v128_t m = wasm_i32x4_ne(wasm_v128_and(wasm_i32x4_splat((i32)pass), lane_bits), zero);
                    store_quad(c0, c1, wasm_v128_bitselect(ov, load_quad(c0, c1), m));
                    if (t->depth_write) {
                        f32* d0 = drow + qx;
                        f32* d1 = drow + fbw + qx;
                        store_quad(d0, d1, wasm_v128_bitselect(zv, load_quad(d0, d1), m));
                    }
                }
            } else {
                for (u32 l = 0; l < 4; l++) {
                    if (!(pass & (1u << l))) continue;
                    i32 off = (i32)(l >> 1) * fbw + qx + (i32)(l & 1);
                    crow[off] = blend ? raster_blend_px(blend, out[l], crow[off]) : out[l];
                    if (t->depth_write) drow[off] = q.z[l];
                }
            }
            quads++;
            pixels += (u32)__builtin_popcount(pass);
        }
        if (!full) {
            for (u32 k = 0; k < 3; k++) {
                row_lo[k] = wasm_i64x2_add(row_lo[k], step_y[k]);
                row_hi[k] = wasm_i64x2_add(row_hi[k], step_y[k]);
            }
        }
    }
    if (quads) {
        st->quads_shaded += quads;
        st->pixels_written += pixels;
        /* üçgen birden çok karoda (thread'de) çizilebilir: ilk çizen sayar */
        if (!__atomic_exchange_n(&t->drawn, 1, __ATOMIC_RELAXED)) st->tris_drawn++;
    }
}

/* ---- MSAA 4x (raster_tile_msaa ile BİT-BİT aynı) ----
 * Bir pikselin 4 örneği bellekte bitişik → derinlik testi ve yazma tek v128. Kapsama: piksel merkezindeki
 * kenar değeri + örnek ofsetlerinin en küçüğü ≥ 0 ise 4 örnek de içeride, en büyüğü < 0 ise hiçbiri;
 * yalnızca kenar pikselleri örnek örnek test edilir (sonuç skalerle aynı). */
static const i32 MSV_OX[RASTER_MSAA] = { -32, 96, -96, 32 };
static const i32 MSV_OY[RASTER_MSAA] = { -96, -32, 32, 96 };

void raster_tile_msaa_simd(Raster* r, TriSetup* t, i32 tx0, i32 ty0, i32 tx1, i32 ty1, b32 full, RasterStats* st) {
    Framebuffer* fb = r->fb;
    i32 minx = MAX(t->minx, tx0) & ~1, miny = MAX(t->miny, ty0) & ~1;
    i32 maxx = MIN(t->maxx, tx1), maxy = MIN(t->maxy, ty1);
    if (minx > maxx || miny > maxy) return;
    const i32 fbw = fb->w, fbh = fb->h;
    const u32 nv = t->var_count;
    i64 off[3][RASTER_MSAA], mn[3], mx[3];
    for (u32 k = 0; k < 3; k++) {
        mn[k] = mx[k] = 0;
        for (u32 s = 0; s < RASTER_MSAA; s++) {
            off[k][s] = (t->e[k].sx / 256) * MSV_OX[s] + (t->e[k].sy / 256) * MSV_OY[s];
            mn[k] = s ? MIN(mn[k], off[k][s]) : off[k][s];
            mx[k] = s ? MAX(mx[k], off[k][s]) : off[k][s];
        }
    }
    const v128_t oxv = wasm_f32x4_make((f32)MSV_OX[0] * (1.0f / 256.0f), (f32)MSV_OX[1] * (1.0f / 256.0f),
                                       (f32)MSV_OX[2] * (1.0f / 256.0f), (f32)MSV_OX[3] * (1.0f / 256.0f));
    const v128_t oyv = wasm_f32x4_make((f32)MSV_OY[0] * (1.0f / 256.0f), (f32)MSV_OY[1] * (1.0f / 256.0f),
                                       (f32)MSV_OY[2] * (1.0f / 256.0f), (f32)MSV_OY[3] * (1.0f / 256.0f));
    const v128_t za = wasm_f32x4_splat(t->z.a), zb = wasm_f32x4_splat(t->z.b), zc = wasm_f32x4_splat(t->z.c);
    const v128_t sbits = wasm_i32x4_make(1, 2, 4, 8);
    const v128_t zero = wasm_i32x4_splat(0);
    static const i32 LX[4] = { 0, 1, 0, 1 }, LY[4] = { 0, 0, 1, 1 };

    const u32 blend = t->blend;
    ShadeQuad q;
    q.kill = 0;
    u32 out[4];
    for (i32 qy = miny; qy <= maxy; qy += 2) {
        f32 fy = (f32)qy + 0.5f - t->y0;
        for (i32 qx = minx; qx <= maxx; qx += 2) {
            f32 fx = (f32)qx + 0.5f - t->x0;
            u32 smask[4] = { 0, 0, 0, 0 };
            v128_t zs[4];
            u32 pass = 0;
            for (u32 l = 0; l < 4; l++) {
                i32 px = qx + LX[l], py = qy + LY[l];
                if (px >= fbw || py >= fbh) continue;
                u32 cov = 0xF;
                if (!full) {
                    i64 c[3];
                    b32 all_in = TRUE, none = FALSE;
                    for (u32 k = 0; k < 3; k++) {
                        c[k] = t->e[k].c + t->e[k].sx * px + t->e[k].sy * py;
                        if (c[k] + mn[k] < 0) all_in = FALSE;
                        if (c[k] + mx[k] < 0) none = TRUE;
                    }
                    if (none) continue;
                    if (!all_in) {
                        cov = 0;
                        for (u32 s = 0; s < RASTER_MSAA; s++) {
                            if (((c[0] + off[0][s]) | (c[1] + off[1][s]) | (c[2] + off[2][s])) >= 0) cov |= 1u << s;
                        }
                        if (!cov) continue;
                    }
                }
                usize base = ((usize)py * (usize)fbw + (usize)px) * RASTER_MSAA;
                const v128_t xs = wasm_f32x4_add(wasm_f32x4_splat(fx + (f32)LX[l]), oxv);
                const v128_t ys = wasm_f32x4_add(wasm_f32x4_splat(fy + (f32)LY[l]), oyv);
                const v128_t z = plane4(za, zb, zc, xs, ys);
                if (t->depth_test) cov &= depth_mask4(t->depth_func, z, wasm_v128_load(&fb->depth_ms[base]));
                if (!cov) continue;
                zs[l] = z;
                smask[l] = cov;
                pass |= 1u << l;
            }
            if (!pass) continue;

            /* merkezde gölgeleme girdileri (raster_tile_simd ile aynı ifadeler) */
            const v128_t fxv = wasm_f32x4_add(wasm_f32x4_splat(fx), wasm_f32x4_make(0, 1, 0, 1));
            const v128_t fyv = wasm_f32x4_add(wasm_f32x4_splat(fy), wasm_f32x4_make(0, 0, 1, 1));
            q.x = qx;
            q.y = qy;
            q.mask = pass;
            wasm_v128_store(q.z, plane4(za, zb, zc, fxv, fyv));
            const v128_t wv = wasm_f32x4_div(wasm_f32x4_splat(1.0f),
                                             plane4(wasm_f32x4_splat(t->w.a), wasm_f32x4_splat(t->w.b), wasm_f32x4_splat(t->w.c), fxv, fyv));
            for (u32 i = 0; i < nv; i++) {
                const AttrPlane* p = &t->v[i];
                wasm_v128_store(q.var[i], wasm_f32x4_mul(plane4(wasm_f32x4_splat(p->a), wasm_f32x4_splat(p->b), wasm_f32x4_splat(p->c), fxv, fyv), wv));
            }
            q.kill = 0;
            t->shade(&q, t->uniforms, out);
            pass &= ~q.kill;

            u32 written = 0;
            for (u32 l = 0; l < 4; l++) {
                if (!(pass & (1u << l))) continue;
                usize base = ((usize)(qy + LY[l]) * (usize)fbw + (usize)(qx + LX[l])) * RASTER_MSAA;
                const v128_t m = wasm_i32x4_ne(wasm_v128_and(wasm_i32x4_splat((i32)smask[l]), sbits), zero);
                u32* cp = &fb->color_ms[base];
                if (blend) {
                    for (u32 s = 0; s < RASTER_MSAA; s++) {
                        if (smask[l] & (1u << s)) cp[s] = raster_blend_px(blend, out[l], cp[s]);
                    }
                } else {
                    wasm_v128_store(cp, wasm_v128_bitselect(wasm_i32x4_splat((i32)out[l]), wasm_v128_load(cp), m));
                }
                if (t->depth_write) {
                    f32* dp = &fb->depth_ms[base];
                    wasm_v128_store(dp, wasm_v128_bitselect(zs[l], wasm_v128_load(dp), m));
                }
                written++;
            }
            st->quads_shaded++;
            st->pixels_written += written;
            if (written && !__atomic_exchange_n(&t->drawn, 1, __ATOMIC_RELAXED)) st->tris_drawn++;
        }
    }
}