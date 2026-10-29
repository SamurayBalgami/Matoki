/* font/raster.c — İkinci derece Bezier düzleştirme ve işaretli alan (signed-area) AA rasterleyici.
 * Her yönlü kenar parçası tarama satırlarına [j, j+1] ve piksel sütunlarına [i, i+1] kırpılarak
 * yamuk alanı fark dizisine (diff) biriktirilir; önek toplamı tam analitik kapsamayı verir.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/font/raster.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "font/raster.h"
#include "core/math.h"

#define MAX_BEZIER_DEPTH 10u
#define MAX_RASTER_DIM   4096u

void font_glyph_bbox(const TtfGlyph* glyph, f32 scale_x, f32 scale_y,
                     f32 shift_x, f32 shift_y,
                     i32* out_x0, i32* out_y0, i32* out_x1, i32* out_y1) {
    if (!glyph || glyph->point_count == 0u || glyph->contour_count == 0u ||
        scale_x <= 0.0f || scale_y <= 0.0f) {
        if (out_x0) *out_x0 = 0;
        if (out_y0) *out_y0 = 0;
        if (out_x1) *out_x1 = 0;
        if (out_y1) *out_y1 = 0;
        return;
    }

    f32 fx0 = (f32)glyph->xmin * scale_x + shift_x;
    f32 fy0 = -(f32)glyph->ymax * scale_y + shift_y;
    f32 fx1 = (f32)glyph->xmax * scale_x + shift_x;
    f32 fy1 = -(f32)glyph->ymin * scale_y + shift_y;

    fx0 = CLAMP(fx0, -32768.0f, 32767.0f);
    fy0 = CLAMP(fy0, -32768.0f, 32767.0f);
    fx1 = CLAMP(fx1, -32768.0f, 32767.0f);
    fy1 = CLAMP(fy1, -32768.0f, 32767.0f);

    if (out_x0) *out_x0 = (i32)m_floorf(fx0);
    if (out_y0) *out_y0 = (i32)m_floorf(fy0);
    if (out_x1) *out_x1 = (i32)m_ceilf(fx1);
    if (out_y1) *out_y1 = (i32)m_ceilf(fy1);
}

static void accum_edge(f32* diff, i32 w, i32 h, f32 x0, f32 y0, f32 x1, f32 y1) {
    if (!(x0 == x0 && y0 == y0 && x1 == x1 && y1 == y1)) return;
    if (x0 < -65536.0f || x0 > 65536.0f || y0 < -65536.0f || y0 > 65536.0f ||
        x1 < -65536.0f || x1 > 65536.0f || y1 < -65536.0f || y1 > 65536.0f) {
        return;
    }
    if (y0 == y1) return;

    f32 dir = 1.0f;
    if (y0 > y1) {
        dir = -1.0f;
        f32 tx = x0; x0 = x1; x1 = tx;
        f32 ty = y0; y0 = y1; y1 = ty;
    }

    if (y1 <= 0.0f || y0 >= (f32)h) return;

    i32 j0 = (y0 > 0.0f) ? (i32)m_floorf(y0) : 0;
    i32 j1 = (y1 < (f32)h) ? (i32)m_ceilf(y1) : h;
    if (j0 < 0) j0 = 0;
    if (j1 > h) j1 = h;

    f32 dy_seg = y1 - y0;
    if (dy_seg <= 1e-8f) return;
    f32 dx_dy = (x1 - x0) / dy_seg;
    u32 row_stride = (u32)(w + 1);

    for (i32 j = j0; j < j1; j++) {
        f32 ya = (y0 > (f32)j) ? y0 : (f32)j;
        f32 yb = (y1 < (f32)(j + 1)) ? y1 : (f32)(j + 1);
        if (yb <= ya) continue;

        f32 dy_total = dir * (yb - ya);
        f32 xa = x0 + (ya - y0) * dx_dy;
        f32 xb = x0 + (yb - y0) * dx_dy;
        f32* row = diff + (usize)j * row_stride;

        if (m_absf(xb - xa) <= 1e-6f) {
            f32 xm = 0.5f * (xa + xb);
            if (xm <= 0.0f) {
                row[0] += dy_total;
            } else if (xm < (f32)w) {
                i32 i = (i32)xm;
                f32 f = xm - (f32)i;
                row[i]     += dy_total * (1.0f - f);
                row[i + 1] += dy_total * f;
            }
            continue;
        }

        f32 xL = m_minf(xa, xb);
        f32 xR = m_maxf(xa, xb);
        if (xR <= 0.0f) {
            row[0] += dy_total;
            continue;
        }
        if (xL >= (f32)w) continue;

        f32 inv_dx = dy_total / (xR - xL);
        if (xL < 0.0f) {
            row[0] += (0.0f - xL) * inv_dx;
            xL = 0.0f;
        }
        if (xR > (f32)w) {
            xR = (f32)w;
        }
        if (xR <= xL) continue;

        i32 i0 = (i32)xL;
        i32 i1 = (i32)xR;
        if (i0 >= w) i0 = w - 1;
        if (i1 >= w) i1 = w - 1;

        if (i0 == i1) {
            f32 dy = (xR - xL) * inv_dx;
            f32 f  = 0.5f * (xL + xR) - (f32)i0;
            row[i0]     += dy * (1.0f - f);
            row[i0 + 1] += dy * f;
        } else {
            f32 x_next = (f32)(i0 + 1);
            f32 dy0    = (x_next - xL) * inv_dx;
            f32 f0     = 0.5f * (xL + x_next) - (f32)i0;
            row[i0]     += dy0 * (1.0f - f0);
            row[i0 + 1] += dy0 * f0;

            f32 half_dy = 0.5f * inv_dx;
            for (i32 i = i0 + 1; i < i1; i++) {
                row[i]     += half_dy;
                row[i + 1] += half_dy;
            }

            f32 x_prev = (f32)i1;
            f32 dy1    = (xR - x_prev) * inv_dx;
            f32 f1     = 0.5f * (xR - x_prev);
            row[i1]     += dy1 * (1.0f - f1);
            row[i1 + 1] += dy1 * f1;
        }
    }
}

typedef struct {
    FontEdge* edges;
    u32       count;
    u32       cap;
    f32*      diff;
    i32       w, h;
    f32       flatness_sq16;
} FlattenCtx;

static void emit_segment(FlattenCtx* ctx, Vec2 a, Vec2 b) {
    if (ctx->diff) {
        accum_edge(ctx->diff, ctx->w, ctx->h, a.x, a.y, b.x, b.y);
    }
    if (ctx->edges && ctx->count < ctx->cap) {
        FontEdge* e = &ctx->edges[ctx->count];
        e->x0 = a.x; e->y0 = a.y;
        e->x1 = b.x; e->y1 = b.y;
    }
    ctx->count++;
}

static void flatten_qbezier(FlattenCtx* ctx, Vec2 p0, Vec2 p1, Vec2 p2, u32 depth) {
    f32 dx = p0.x - 2.0f * p1.x + p2.x;
    f32 dy = p0.y - 2.0f * p1.y + p2.y;
    if (depth >= MAX_BEZIER_DEPTH || (dx * dx + dy * dy) <= ctx->flatness_sq16) {
        emit_segment(ctx, p0, p2);
        return;
    }
    Vec2 p01  = v2(0.5f * (p0.x + p1.x), 0.5f * (p0.y + p1.y));
    Vec2 p12  = v2(0.5f * (p1.x + p2.x), 0.5f * (p1.y + p2.y));
    Vec2 p012 = v2(0.5f * (p01.x + p12.x), 0.5f * (p01.y + p12.y));
    flatten_qbezier(ctx, p0, p01, p012, depth + 1u);
    flatten_qbezier(ctx, p012, p12, p2, depth + 1u);
}

static inline Vec2 xform_pt(TtfPoint p, f32 sx, f32 sy, f32 ox, f32 oy) {
    return v2(p.x * sx + ox, -p.y * sy + oy);
}

static void walk_glyph_contours(const TtfGlyph* glyph, f32 sx, f32 sy, f32 ox, f32 oy, FlattenCtx* ctx) {
    if (!glyph || glyph->contour_count == 0u || glyph->point_count == 0u) return;

    u32 start_idx = 0u;
    for (u32 c = 0u; c < glyph->contour_count; c++) {
        u32 end_idx = (u32)glyph->contour_ends[c];
        if (end_idx >= glyph->point_count || end_idx < start_idx) {
            start_idx = end_idx + 1u;
            continue;
        }
        u32 n = end_idx - start_idx + 1u;
        const TtfPoint* pts = glyph->points + start_idx;
        start_idx = end_idx + 1u;

        if (n < 2u) continue;

        Vec2 start_pos;
        u32  first_i = 0u;
        u32  last_i  = n - 1u;

        if (pts[0].on_curve) {
            start_pos = xform_pt(pts[0], sx, sy, ox, oy);
            first_i   = 1u;
        } else if (pts[n - 1u].on_curve) {
            start_pos = xform_pt(pts[n - 1u], sx, sy, ox, oy);
            last_i    = n - 2u;
        } else {
            Vec2 p_last = xform_pt(pts[n - 1u], sx, sy, ox, oy);
            Vec2 p_zero = xform_pt(pts[0], sx, sy, ox, oy);
            start_pos   = v2(0.5f * (p_last.x + p_zero.x), 0.5f * (p_last.y + p_zero.y));
        }

        Vec2 cur = start_pos;
        u32  i   = first_i;
        while (i <= last_i) {
            Vec2 p = xform_pt(pts[i], sx, sy, ox, oy);
            if (pts[i].on_curve) {
                emit_segment(ctx, cur, p);
                cur = p;
                i++;
            } else {
                if (i < last_i) {
                    Vec2 next_p = xform_pt(pts[i + 1u], sx, sy, ox, oy);
                    if (pts[i + 1u].on_curve) {
                        flatten_qbezier(ctx, cur, p, next_p, 0u);
                        cur = next_p;
                        i += 2u;
                    } else {
                        Vec2 mid = v2(0.5f * (p.x + next_p.x), 0.5f * (p.y + next_p.y));
                        flatten_qbezier(ctx, cur, p, mid, 0u);
                        cur = mid;
                        i++;
                    }
                } else {
                    flatten_qbezier(ctx, cur, p, start_pos, 0u);
                    cur = start_pos;
                    i++;
                }
            }
        }

        if (cur.x != start_pos.x || cur.y != start_pos.y) {
            emit_segment(ctx, cur, start_pos);
        }
    }
}

static void resolve_diff_to_alpha(const f32* diff, u8* out_px, u32 w, u32 h, u32 stride) {
    u32 row_stride = w + 1u;
    for (u32 j = 0u; j < h; j++) {
        const f32* row = diff + (usize)j * row_stride;
        u8* dst = out_px + (usize)j * stride;
        f32 sum = 0.0f;
        for (u32 i = 0u; i < w; i++) {
            sum += row[i];
            f32 cov = m_absf(sum);
            if (cov > 1.0f) cov = 1.0f;
            dst[i] = (u8)(cov * 255.0f + 0.5f);
        }
    }
}

u32 font_flatten_glyph(const TtfGlyph* glyph, f32 scale_x, f32 scale_y,
                       f32 offset_x, f32 offset_y, f32 flatness,
                       Arena* arena, FontEdge** out_edges) {
    if (out_edges) *out_edges = NULL;
    if (!glyph || !arena || glyph->contour_count == 0u || glyph->point_count == 0u) return 0u;
    if (flatness <= 0.01f) flatness = FONT_DEFAULT_FLATNESS;

    FlattenCtx ctx = {0};
    ctx.flatness_sq16 = 16.0f * flatness * flatness;

    /* 1. geçiş: kenar sayısını hesapla */
    walk_glyph_contours(glyph, scale_x, scale_y, offset_x, offset_y, &ctx);
    if (ctx.count == 0u) return 0u;

    /* 2. geçiş: kenarları doldur */
    FontEdge* arr = ARENA_ARRAY(arena, FontEdge, ctx.count);
    ctx.edges = arr;
    ctx.cap   = ctx.count;
    ctx.count = 0u;
    walk_glyph_contours(glyph, scale_x, scale_y, offset_x, offset_y, &ctx);

    if (out_edges) *out_edges = arr;
    return ctx.count;
}

b32 font_rasterize_edges(const FontEdge* edges, u32 edge_count,
                         u8* out_px, u32 w, u32 h, u32 stride, Arena* scratch) {
    if (!out_px || !scratch) return FALSE;
    if (w == 0u || h == 0u) return TRUE;
    if (w > MAX_RASTER_DIM || h > MAX_RASTER_DIM) return FALSE;

    usize mark = arena_mark(scratch);
    f32* diff = ARENA_ARRAY(scratch, f32, (usize)(w + 1u) * (usize)h);

    for (u32 i = 0u; i < edge_count; i++) {
        accum_edge(diff, (i32)w, (i32)h, edges[i].x0, edges[i].y0, edges[i].x1, edges[i].y1);
    }

    resolve_diff_to_alpha(diff, out_px, w, h, stride);
    arena_reset(scratch, mark);
    return TRUE;
}

b32 font_rasterize_glyph_ex(const TtfGlyph* glyph, f32 scale_x, f32 scale_y,
                            f32 offset_x, f32 offset_y,
                            u8* out_px, u32 w, u32 h, u32 stride, Arena* scratch) {
    if (!glyph || !out_px || !scratch) return FALSE;
    if (w == 0u || h == 0u) return TRUE;
    if (w > MAX_RASTER_DIM || h > MAX_RASTER_DIM) return FALSE;

    usize mark = arena_mark(scratch);
    f32* diff = ARENA_ARRAY(scratch, f32, (usize)(w + 1u) * (usize)h);

    FlattenCtx ctx = {0};
    ctx.diff          = diff;
    ctx.w             = (i32)w;
    ctx.h             = (i32)h;
    ctx.flatness_sq16 = 16.0f * FONT_DEFAULT_FLATNESS * FONT_DEFAULT_FLATNESS;

    walk_glyph_contours(glyph, scale_x, scale_y, offset_x, offset_y, &ctx);
    resolve_diff_to_alpha(diff, out_px, w, h, stride);

    arena_reset(scratch, mark);
    return TRUE;
}

b32 font_rasterize_glyph(const TtfGlyph* glyph, f32 scale_x, f32 scale_y,
                         f32 shift_x, f32 shift_y,
                         u8* out_px, u32 w, u32 h, u32 stride, Arena* scratch) {
    if (!glyph || !out_px || !scratch) return FALSE;
    if (w == 0u || h == 0u) return TRUE;
    i32 ix0 = 0, iy0 = 0, ix1 = 0, iy1 = 0;
    font_glyph_bbox(glyph, scale_x, scale_y, shift_x, shift_y, &ix0, &iy0, &ix1, &iy1);
    return font_rasterize_glyph_ex(glyph, scale_x, scale_y,
                                   shift_x - (f32)ix0, shift_y - (f32)iy0,
                                   out_px, w, h, stride, scratch);
}
