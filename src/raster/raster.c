/* raster/raster.c — üçgen kurulumu, 64×64 karo bin'leme ve skaler karo döngüsü.
 * Kaplama 24.8 sabit noktada, i64 kenar fonksiyonlarıyla (top-left kuralı → ortak kenarda
 * çift boyama/boşluk yok). Varying'ler perspektif-doğru (var*inv_w ve inv_w doğrusal; piksel
 * başına bölme); yardımcı lane'ler aynı düzlemden dışdeğerlenir. Early-z: shade'den önce.
 * Karolar sırayla, karo içinde üçgenler gönderim sırasıyla işlenir → anlık çizimle aynı sonuç. */
#include "raster/raster_internal.h"
#include "core/jobs.h"
#include "core/math.h"
#include "core/mem.h"
#include "core/sys.h"

static const i32 LANE_X[4] = { 0, 1, 0, 1 };
static const i32 LANE_Y[4] = { 0, 0, 1, 1 };

void fb_clear(Framebuffer* fb, u32 color, f32 depth) {
    usize n = (usize)fb->w * (usize)fb->h;
    for (usize i = 0; i < n; i++) fb->color[i] = color;
    if (fb->depth) {
        for (usize i = 0; i < n; i++) fb->depth[i] = depth;
    }
}

void raster_init(Raster* r, Framebuffer* fb) {
    mem_zero(r, sizeof(*r));
    r->fb = fb;
    r->depth_test = TRUE;
    r->depth_write = TRUE;
    r->depth_func = DEPTH_LESS;
    r->cull = CULL_BACK;
#if defined(__wasm_simd128__)
    r->mode = RASTER_MODE_SIMD;
#else
    r->mode = RASTER_MODE_SCALAR;
#endif
}

void raster_set_shader(Raster* r, ShadeFn fn, const void* uniforms, u32 var_count) {
    r->shade = fn;
    r->prelude = NULL;
    r->uniforms = uniforms;
    r->var_count = var_count > SHADE_MAX_VARS ? SHADE_MAX_VARS : var_count;
}

void raster_set_prelude(Raster* r, PreludeFn fn) { r->prelude = fn; }

void raster_set_depth(Raster* r, b32 test, b32 write, DepthFunc func) {
    r->depth_test = test;
    r->depth_write = write;
    r->depth_func = func;
}

void raster_set_cull(Raster* r, CullMode mode) { r->cull = mode; }

void raster_set_mode(Raster* r, RasterMode mode) { r->mode = mode < RASTER_MODE_COUNT ? mode : RASTER_MODE_SCALAR; }

void raster_reset_stats(Raster* r) { mem_zero(&r->stats, sizeof(r->stats)); }

/* ---- bin'leme ---- */

static void bins_alloc(Raster* r) {
    r->tiles_x = (r->fb->w + RASTER_TILE - 1) >> RASTER_TILE_SHIFT;
    r->tiles_y = (r->fb->h + RASTER_TILE - 1) >> RASTER_TILE_SHIFT;
    r->bins = ARENA_ARRAY(r->arena, Bin, (usize)r->tiles_x * (usize)r->tiles_y);
    r->pending = 0;
}

void raster_begin(Raster* r, Arena* arena) {
    r->arena = arena;
    r->arena_mark = arena_mark(arena);
    bins_alloc(r);
}

static void bin_push(Raster* r, Bin* b, TriSetup* t, u32 full) {
    BinChunk* c = b->tail;
    if (!c || c->count == BIN_CHUNK_ITEMS) {
        BinChunk* n = (BinChunk*)arena_alloc_nozero(r->arena, sizeof(BinChunk), _Alignof(BinChunk));
        n->next = NULL;
        n->count = 0;
        if (c) c->next = n;
        else b->head = n;
        b->tail = n;
        c = n;
    }
    c->items[c->count].t = t;
    c->items[c->count].full = full;
    c->count++;
}

/* ---- flush: karolar thread'lere dağıtılır (jobs.h) ----
 * Bir karonun üçgenleri gönderim sırasıyla tek thread'de işlenir; karolar ayrık pikseller yazar →
 * thread sayısından bağımsız aynı framebuffer. İstatistik ve son önsöz (shader önbelleği thread başına,
 * codegen.h) thread başına tutulur, sonda toplanır. */
typedef struct ThreadTile {
    RasterStats st;
    PreludeFn   last_pre;
    const void* last_uni;
    u8          pad[64];      /* komşu thread'lerin yazdığı alanlar ayrı önbellek satırında */
} ThreadTile;

typedef struct FlushCtx {
    Raster*     r;
    const u32*  tiles;        /* boş olmayan karo indeksleri */
    ThreadTile  th[JOBS_MAX_THREADS];
} FlushCtx;

static FlushCtx g_flush;

static void flush_tile(void* ctx, u32 index, u32 tid) {
    FlushCtx* fc = (FlushCtx*)ctx;
    Raster* r = fc->r;
    ThreadTile* th = &fc->th[tid];
    const Framebuffer* fb = r->fb;
    u32 tile = fc->tiles[index];
    i32 tx = (i32)(tile % (u32)r->tiles_x), ty = (i32)(tile / (u32)r->tiles_x);
    const Bin* b = &r->bins[tile];
    i32 x0 = tx << RASTER_TILE_SHIFT, y0 = ty << RASTER_TILE_SHIFT;
    i32 x1 = MIN(x0 + RASTER_TILE - 1, fb->w - 1), y1 = MIN(y0 + RASTER_TILE - 1, fb->h - 1);
    for (const BinChunk* c = b->head; c; c = c->next) {
        for (u32 i = 0; i < c->count; i++) {
            TriSetup* t = c->items[i].t;
            /* son çağrılan önsöz: bu thread'in önbelleği (önsöz, uniforms) çifti değişene kadar geçerli */
            if (t->prelude && (t->prelude != th->last_pre || t->uniforms != th->last_uni)) {
                t->prelude(t->uniforms);
                th->last_pre = t->prelude;
                th->last_uni = t->uniforms;
                th->st.preludes++;
            }
#if defined(__wasm_simd128__)
            if (r->mode == RASTER_MODE_SIMD) {
                raster_tile_simd(r, t, x0, y0, x1, y1, c->items[i].full, &th->st);
                continue;
            }
#endif
            raster_tile_scalar(r, t, x0, y0, x1, y1, c->items[i].full, &th->st);
        }
    }
}

void raster_flush(Raster* r) {
    if (!r->bins) return;
    r->stats.flushes++;
    if (r->pending) {
        u32 n_tiles = (u32)(r->tiles_x * r->tiles_y);
        u32* tiles = ARENA_ARRAY(r->arena, u32, n_tiles);
        u32 n = 0;
        for (u32 i = 0; i < n_tiles; i++) {
            if (r->bins[i].head) tiles[n++] = i;
        }
        FlushCtx* fc = &g_flush;
        mem_zero(fc, sizeof(*fc));
        fc->r = r;
        fc->tiles = tiles;
        jobs_parallel_for(flush_tile, fc, n);
        for (u32 k = 0; k < JOBS_MAX_THREADS; k++) {
            const RasterStats* s = &fc->th[k].st;
            r->stats.quads_shaded += s->quads_shaded;
            r->stats.pixels_written += s->pixels_written;
            r->stats.tris_drawn += s->tris_drawn;
            r->stats.preludes += s->preludes;
        }
    }
    arena_reset(r->arena, r->arena_mark);
    bins_alloc(r);
}

/* ---- kurulum ---- */

static inline i32 to_fixed(f32 v) { return m_f2i_sat(m_floorf(v * 256.0f + 0.5f)); }

static inline AttrPlane plane_make(f32 f0, f32 f1, f32 f2, f32 e1x, f32 e1y, f32 e2x, f32 e2y, f32 inv_d) {
    AttrPlane p;
    f32 d1 = f1 - f0, d2 = f2 - f0;
    p.a = (d1 * e2y - d2 * e1y) * inv_d;
    p.b = (d2 * e1x - d1 * e2x) * inv_d;
    p.c = f0;
    return p;
}

/* (x0,y0) → (x1,y1) kenarı; piksel merkezi = p*256 + 128 */
static inline EdgeEq edge_make(i32 x0, i32 y0, i32 x1, i32 y1) {
    EdgeEq ed;
    i64 dx = (i64)x1 - x0, dy = (i64)y1 - y0;
    b32 top_left = dy < 0 || (dy == 0 && dx > 0);
    ed.c = dx * (128 - (i64)y0) - dy * (128 - (i64)x0) + (top_left ? 0 : -1);
    ed.sx = -dy * 256;
    ed.sy = dx * 256;
    return ed;
}

static inline i64 edge_at(const EdgeEq* e, i64 px, i64 py) { return e->c + e->sx * px + e->sy * py; }

#define RASTER_ARENA_SLACK (64u * 1024u)

void raster_triangle(Raster* r, const RasterVert* va, const RasterVert* vb, const RasterVert* vc) {
    Framebuffer* fb = r->fb;
    r->stats.tris_in++;
    if (!r->shade) return;
    if (!r->bins) sys_panic("raster_triangle: önce raster_begin");

    const RasterVert* v[3] = { va, vb, vc };
    for (u32 i = 0; i < 3; i++) {
        /* NaN da bu testi geçemez */
        if (!(m_absf(v[i]->x) <= RASTER_MAX_COORD && m_absf(v[i]->y) <= RASTER_MAX_COORD)) {
            r->stats.tris_overflow++;
            return;
        }
    }

    i32 X[3], Y[3];
    for (u32 i = 0; i < 3; i++) {
        X[i] = to_fixed(v[i]->x);
        Y[i] = to_fixed(v[i]->y);
    }
    i64 area = ((i64)X[1] - X[0]) * ((i64)Y[2] - Y[0]) - ((i64)Y[1] - Y[0]) * ((i64)X[2] - X[0]);
    if (area == 0) {
        r->stats.tris_degenerate++;
        return;
    }
    /* NDC'de CCW (ön yüz) → y aşağı ekranda alan < 0. Kenar fonksiyonları alan > 0 bekler. */
    b32 back = area > 0;
    if ((r->cull == CULL_BACK && back) || (r->cull == CULL_FRONT && !back)) {
        r->stats.tris_culled++;
        return;
    }
    if (area < 0) {
        const RasterVert* tv = v[1]; v[1] = v[2]; v[2] = tv;
        i32 t = X[1]; X[1] = X[2]; X[2] = t;
        t = Y[1]; Y[1] = Y[2]; Y[2] = t;
    }

    /* Sınır kutusu: piksel i'nin merkezi i*256+128. */
    i64 min_fx = MIN(X[0], MIN(X[1], X[2])), max_fx = MAX(X[0], MAX(X[1], X[2]));
    i64 min_fy = MIN(Y[0], MIN(Y[1], Y[2])), max_fy = MAX(Y[0], MAX(Y[1], Y[2]));
    i64 minx = (min_fx - 128 + 255) >> 8, maxx = (max_fx - 128) >> 8;
    i64 miny = (min_fy - 128 + 255) >> 8, maxy = (max_fy - 128) >> 8;
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > fb->w - 1) maxx = fb->w - 1;
    if (maxy > fb->h - 1) maxy = fb->h - 1;
    if (minx > maxx || miny > maxy) {
        r->stats.tris_empty++;
        return;
    }

    /* Öznitelik düzlemleri (kayan noktalı, yakalanmamış konumlarla → daha doğru). */
    f32 x0 = v[0]->x, y0 = v[0]->y;
    f32 e1x = v[1]->x - x0, e1y = v[1]->y - y0;
    f32 e2x = v[2]->x - x0, e2y = v[2]->y - y0;
    f32 det = e1x * e2y - e1y * e2x;
    if (det == 0.0f) {
        r->stats.tris_degenerate++;
        return;
    }

    i32 tx0 = (i32)minx >> RASTER_TILE_SHIFT, tx1 = (i32)maxx >> RASTER_TILE_SHIFT;
    i32 ty0 = (i32)miny >> RASTER_TILE_SHIFT, ty1 = (i32)maxy >> RASTER_TILE_SHIFT;
    u32 nv = r->var_count;
    usize tri_bytes = sizeof(TriSetup) + sizeof(AttrPlane) * nv;
    usize need = tri_bytes + sizeof(BinChunk) * (usize)((tx1 - tx0 + 1) * (ty1 - ty0 + 1)) + RASTER_ARENA_SLACK;
    if (r->arena->cap - r->arena->used < need) {
        raster_flush(r);
        if (r->arena->cap - r->arena->used < need) sys_panic("raster arena çok küçük");
    }

    TriSetup* t = (TriSetup*)arena_alloc_nozero(r->arena, tri_bytes, _Alignof(TriSetup));
    t->shade = r->shade;
    t->prelude = r->prelude;
    t->uniforms = r->uniforms;
    t->var_count = nv;
    t->depth_test = (u8)(r->depth_test && fb->depth);
    t->depth_write = (u8)(r->depth_write && fb->depth);
    t->depth_func = (u8)r->depth_func;
    t->drawn = 0;
    t->minx = (i32)minx;
    t->miny = (i32)miny;
    t->maxx = (i32)maxx;
    t->maxy = (i32)maxy;
    t->x0 = x0;
    t->y0 = y0;
    t->e[0] = edge_make(X[0], Y[0], X[1], Y[1]);
    t->e[1] = edge_make(X[1], Y[1], X[2], Y[2]);
    t->e[2] = edge_make(X[2], Y[2], X[0], Y[0]);
    f32 inv_d = 1.0f / det;
    t->z = plane_make(v[0]->z, v[1]->z, v[2]->z, e1x, e1y, e2x, e2y, inv_d);
    t->w = plane_make(v[0]->inv_w, v[1]->inv_w, v[2]->inv_w, e1x, e1y, e2x, e2y, inv_d);
    for (u32 i = 0; i < nv; i++) {
        t->v[i] = plane_make(v[0]->var[i] * v[0]->inv_w, v[1]->var[i] * v[1]->inv_w,
                             v[2]->var[i] * v[2]->inv_w, e1x, e1y, e2x, e2y, inv_d);
    }

    /* Bin'leme: tek karoluk üçgenlerde test yok; aksi halde karo köşe piksel merkezlerinde
     * her kenarı değerlendir: hepsi < 0 → karo dışarıda; hepsi >= 0 → o kenar için tam içeride. */
    r->pending++;
    if (tx0 == tx1 && ty0 == ty1) {
        bin_push(r, &r->bins[ty0 * r->tiles_x + tx0], t, 0);
        r->stats.bin_items++;
        return;
    }
    for (i32 ty = ty0; ty <= ty1; ty++) {
        i64 ry0 = (i64)ty << RASTER_TILE_SHIFT, ry1 = MIN(ry0 + RASTER_TILE - 1, (i64)fb->h - 1);
        for (i32 tx = tx0; tx <= tx1; tx++) {
            i64 rx0 = (i64)tx << RASTER_TILE_SHIFT, rx1 = MIN(rx0 + RASTER_TILE - 1, (i64)fb->w - 1);
            b32 outside = FALSE;
            u32 full = 1;
            for (u32 k = 0; k < 3; k++) {
                const EdgeEq* e = &t->e[k];
                i64 a = edge_at(e, rx0, ry0), b = edge_at(e, rx1, ry0);
                i64 c = edge_at(e, rx0, ry1), d = edge_at(e, rx1, ry1);
                if ((a & b & c & d) < 0) {   /* dördü de negatif */
                    outside = TRUE;
                    break;
                }
                if ((a | b | c | d) < 0) full = 0;
            }
            if (outside) continue;
            bin_push(r, &r->bins[ty * r->tiles_x + tx], t, full);
            r->stats.bin_items++;
            r->stats.bin_full += full;
        }
    }
}

/* ---- skaler karo döngüsü (referans) ---- */

void raster_tile_scalar(Raster* r, TriSetup* t, i32 tx0, i32 ty0, i32 tx1, i32 ty1, b32 full, RasterStats* st) {
    Framebuffer* fb = r->fb;
    i32 minx = MAX(t->minx, tx0) & ~1, miny = MAX(t->miny, ty0) & ~1;
    i32 maxx = MIN(t->maxx, tx1), maxy = MIN(t->maxy, ty1);
    if (minx > maxx || miny > maxy) return;
    const i32 fbw = fb->w, fbh = fb->h;
    const u32 nv = t->var_count;
    const EdgeEq* e0 = &t->e[0];
    const EdgeEq* e1 = &t->e[1];
    const EdgeEq* e2 = &t->e[2];

    ShadeQuad q;
    q.kill = 0;
    u32 out[4];

    i64 row0 = edge_at(e0, minx, miny), row1 = edge_at(e1, minx, miny), row2 = edge_at(e2, minx, miny);
    for (i32 qy = miny; qy <= maxy; qy += 2) {
        i64 c0 = row0, c1 = row1, c2 = row2;
        f32 fy = (f32)qy + 0.5f - t->y0;
        for (i32 qx = minx; qx <= maxx; qx += 2) {
            u32 cov = 0;
            for (u32 l = 0; l < 4; l++) {
                i32 lx = LANE_X[l], ly = LANE_Y[l];
                if (qx + lx >= fbw || qy + ly >= fbh) continue;
                if (full) {
                    cov |= 1u << l;
                    continue;
                }
                i64 a0 = c0 + lx * e0->sx + ly * e0->sy;
                i64 a1 = c1 + lx * e1->sx + ly * e1->sy;
                i64 a2 = c2 + lx * e2->sx + ly * e2->sy;
                if ((a0 | a1 | a2) >= 0) cov |= 1u << l;
            }
            c0 += 2 * e0->sx;
            c1 += 2 * e1->sx;
            c2 += 2 * e2->sx;
            if (!cov) continue;

            f32 fx = (f32)qx + 0.5f - t->x0;
            u32 pass = cov;
            for (u32 l = 0; l < 4; l++) q.z[l] = attr_eval(&t->z, fx + (f32)LANE_X[l], fy + (f32)LANE_Y[l]);
            if (t->depth_test) {
                for (u32 l = 0; l < 4; l++) {
                    if (!(pass & (1u << l))) continue;
                    i32 idx = (qy + LANE_Y[l]) * fbw + qx + LANE_X[l];
                    if (!raster_depth_pass(t->depth_func, q.z[l], fb->depth[idx])) pass &= ~(1u << l);
                }
            }
            if (!pass) continue;

            q.x = qx;
            q.y = qy;
            q.mask = pass;
            for (u32 l = 0; l < 4; l++) {
                f32 lfx = fx + (f32)LANE_X[l], lfy = fy + (f32)LANE_Y[l];
                f32 w = 1.0f / attr_eval(&t->w, lfx, lfy);
                for (u32 i = 0; i < nv; i++) q.var[i][l] = attr_eval(&t->v[i], lfx, lfy) * w;
            }
            q.kill = 0;
            t->shade(&q, t->uniforms, out);
            pass &= ~q.kill;

            for (u32 l = 0; l < 4; l++) {
                if (!(pass & (1u << l))) continue;
                i32 idx = (qy + LANE_Y[l]) * fbw + qx + LANE_X[l];
                fb->color[idx] = out[l];
                if (t->depth_write) fb->depth[idx] = q.z[l];
            }
            st->quads_shaded++;
            st->pixels_written += (u32)__builtin_popcount(pass);
            /* üçgen birden çok karoda (thread'de) çizilebilir: ilk çizen sayar */
            if (!__atomic_exchange_n(&t->drawn, 1, __ATOMIC_RELAXED)) st->tris_drawn++;
        }
        row0 += 2 * e0->sy;
        row1 += 2 * e1->sy;
        row2 += 2 * e2->sy;
    }
}
