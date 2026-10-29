/* gfx2d/tilemap.c — Karo haritası depolama, ASCII yükleme, çizim ve AABB çarpışma çözümü.
 * Çarpışma: hareket yarım karodan küçük alt adımlara bölünür; her alt adımda önce x sonra y
 * ekseni ilerletilir, giren katı karoda AABB karo kenarına yaslanır. Dört kenar da aynı eps toleransıyla karoya
 * çevrilir (eps'ten küçük örtüşme yok sayılır); onlarda sol/üst kenar toleranssızdı → genişliği karo katını eps'ten
 * az aşan kutu sağa yaslanınca soldaki karoya giriyordu (bizim fuzz testi buldu).
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/gfx2d/tilemap.c), izinle uyarlandı (D37). */
#include "gfx2d/tilemap.h"
#include "core/fmt.h"

void tilemap_init(Tilemap* m, Arena* a, u32 w, u32 h, f32 tile_size) {
    m->w             = w;
    m->h             = h;
    m->tile_size     = tile_size;
    m->tiles         = ARENA_ARRAY(a, u16, (usize)w * h);
    m->defs          = NULL;
    m->def_count     = 0;
    m->border_tile   = TILE_EMPTY;
    m->atlas         = NULL;
    m->atlas_tile_px = 16;
}

u16 tilemap_get(const Tilemap* m, i32 tx, i32 ty) {
    if (tx < 0 || ty < 0 || tx >= (i32)m->w || ty >= (i32)m->h) return m->border_tile;
    return m->tiles[(u32)ty * m->w + (u32)tx];
}

void tilemap_set(Tilemap* m, i32 tx, i32 ty, u16 tile) {
    if (tx < 0 || ty < 0 || tx >= (i32)m->w || ty >= (i32)m->h) return;
    m->tiles[(u32)ty * m->w + (u32)tx] = tile;
}

u32 tilemap_flags(const Tilemap* m, i32 tx, i32 ty) {
    u16 t = tilemap_get(m, tx, ty);
    if (t == TILE_EMPTY || t >= m->def_count || m->defs == NULL) return 0;
    return m->defs[t].flags;
}

void tilemap_load_ascii(Tilemap* m, const char* text, const u16 legend[128]) {
    u32 x = 0, y = 0;
    for (const char* p = text; *p && y < m->h; p++) {
        if (*p == '\n') { x = 0; y++; continue; }
        u8 ch = (u8)*p;
        if (x < m->w) tilemap_set(m, (i32)x, (i32)y, ch < 128 ? legend[ch] : TILE_EMPTY);
        x++;
    }
}

static inline i32 floor_div(f32 v, f32 ts) {
    return (i32)m_floorf(v / ts);
}

u32 tilemap_draw(const Tilemap* m, Batch2D* b, Rect vis, Vec4 tint) {
    if (m->atlas == NULL || m->defs == NULL) return 0;
    f32 ts = m->tile_size;
    i32 x0 = floor_div(vis.x, ts) - 1, x1 = floor_div(vis.x + vis.w, ts) + 1;
    i32 y0 = floor_div(vis.y, ts) - 1, y1 = floor_div(vis.y + vis.h, ts) + 1;
    x0 = MAX(x0, 0); y0 = MAX(y0, 0);
    x1 = MIN(x1, (i32)m->w - 1); y1 = MIN(y1, (i32)m->h - 1);
    u32 cols = m->atlas->w / m->atlas_tile_px;
    f32 tp = (f32)m->atlas_tile_px;
    u32 drawn = 0;
    for (i32 ty = y0; ty <= y1; ty++) {
        for (i32 tx = x0; tx <= x1; tx++) {
            u16 t = m->tiles[(u32)ty * m->w + (u32)tx];
            if (t == TILE_EMPTY || t >= m->def_count) continue;
            u32 ai = m->defs[t].atlas_index;
            Rect src = rect((f32)(ai % cols) * tp, (f32)(ai / cols) * tp, tp, tp);
            Vec2 c = v2(((f32)tx + 0.5f) * ts, ((f32)ty + 0.5f) * ts);
            b2d_sprite(b, m->atlas, src, c, v2(ts, ts), 0.0f, tint);
            drawn++;
        }
    }
    return drawn;
}

u32 tilemap_query(const Tilemap* m, Vec2 pos, Vec2 size) {
    f32 ts = m->tile_size, eps = ts * 1e-3f;
    i32 x0 = floor_div(pos.x + eps, ts), x1 = floor_div(pos.x + size.x - eps, ts);
    i32 y0 = floor_div(pos.y + eps, ts), y1 = floor_div(pos.y + size.y - eps, ts);
    u32 f = 0;
    for (i32 ty = y0; ty <= y1; ty++) {
        for (i32 tx = x0; tx <= x1; tx++) f |= tilemap_flags(m, tx, ty);
    }
    return f;
}

/* [r0, r1] satırlarında tx sütununda katı karo var mı */
static b32 column_solid(const Tilemap* m, i32 tx, i32 r0, i32 r1) {
    for (i32 ty = r0; ty <= r1; ty++) {
        if (tilemap_flags(m, tx, ty) & TILE_SOLID) return TRUE;
    }
    return FALSE;
}

TileHit tilemap_move(const Tilemap* m, Vec2* pos, Vec2 size, Vec2 delta, b32 drop_through) {
    TileHit hit = { FALSE, FALSE, FALSE, FALSE, 0 };
    f32 ts = m->tile_size, eps = ts * 1e-3f;
    f32 maxd = m_maxf(m_absf(delta.x), m_absf(delta.y));
    u32 steps = (u32)m_ceilf(maxd / (ts * 0.45f));
    if (steps == 0) steps = 1;
    f32 sx = delta.x / (f32)steps, sy = delta.y / (f32)steps;

    for (u32 s = 0; s < steps; s++) {
        /* --- x ekseni --- */
        if (sx != 0.0f) {
            f32 nx = pos->x + sx;
            i32 r0 = floor_div(pos->y + eps, ts), r1 = floor_div(pos->y + size.y - eps, ts);
            if (sx > 0.0f) {
                i32 tx = floor_div(nx + size.x - eps, ts);
                if (column_solid(m, tx, r0, r1)) {
                    nx = (f32)tx * ts - size.x;
                    hit.hit_right = TRUE;
                    sx = 0.0f;
                }
            } else {
                i32 tx = floor_div(nx + eps, ts);
                if (column_solid(m, tx, r0, r1)) {
                    nx = (f32)(tx + 1) * ts;
                    hit.hit_left = TRUE;
                    sx = 0.0f;
                }
            }
            pos->x = nx;
        }
        /* --- y ekseni --- */
        if (sy != 0.0f) {
            f32 ny = pos->y + sy;
            i32 c0 = floor_div(pos->x + eps, ts), c1 = floor_div(pos->x + size.x - eps, ts);
            if (sy > 0.0f) {
                f32 old_bottom = pos->y + size.y;
                i32 ty = floor_div(ny + size.y - eps, ts);
                f32 top = (f32)ty * ts;
                b32 blocked = FALSE;
                for (i32 tx = c0; tx <= c1 && !blocked; tx++) {
                    u32 f = tilemap_flags(m, tx, ty);
                    if (f & TILE_SOLID) blocked = TRUE;
                    else if ((f & TILE_ONEWAY) && !drop_through && old_bottom <= top + eps) blocked = TRUE;
                }
                if (blocked) {
                    ny = top - size.y;
                    hit.on_ground = TRUE;
                    sy = 0.0f;
                }
            } else {
                i32 ty = floor_div(ny + eps, ts);
                b32 blocked = FALSE;
                for (i32 tx = c0; tx <= c1 && !blocked; tx++) {
                    if (tilemap_flags(m, tx, ty) & TILE_SOLID) blocked = TRUE;
                }
                if (blocked) {
                    ny = (f32)(ty + 1) * ts;
                    hit.hit_ceiling = TRUE;
                    sy = 0.0f;
                }
            }
            pos->y = ny;
        }
    }
    hit.touched = tilemap_query(m, *pos, size);
    return hit;
}
