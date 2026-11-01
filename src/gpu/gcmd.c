/* gpu/gcmd.c — GPU komut akışı yazıcısı (gpu/gcmd.h). Başlık 6 sözcük, ardından kayıtlar; MESH ve TEX kaydı bir karede
 * nesne başına bir kez (ilk çiziminden önce). Taşma: kalan çizimler atlanır ve sayılır, akış her zaman GC_END ile kapanır. */
#include "gpu/gcmd.h"
#include "core/mem.h"
#include "img/image.h"

#define GCMD_HEADER 6u
#define GCMD_FRAME_WORDS 6u
#define GCMD_SHADOW_WORDS 3u
#define GCMD_OVERLAY_WORDS 6u
#define GCMD_OVERLAYS_MAX 2u
#define GCMD_FIXED_WORDS (GCMD_FRAME_WORDS + GCMD_SHADOW_WORDS + 1u + GCMD_OVERLAYS_MAX * GCMD_OVERLAY_WORDS)
#define GCMD_MESH_WORDS 14u
#define GCMD_DRAW_WORDS 11u
#define GCMD_TEX_WORDS 4u
#define GCMD_DRAW_MAX (GCMD_MESH_WORDS + GCMD_DRAW_WORDS + WGSL_MAX_TEX * GCMD_TEX_WORDS)   /* çizim başına en çok */

b32 gcmd_begin(GpuCmd* c, Arena* frame, u32 max_draws, const GpuProgram* std_vert) {
    mem_zero(c, sizeof(*c));
    c->cap = GCMD_HEADER + GCMD_FIXED_WORDS + max_draws * GCMD_DRAW_MAX + 1u;
    /* akış ve yuvalar sıfırlanmaz: yuvayı slot_take, akışı kayıtlar yazar (kare başına 10 MB temizlemek ölçülür bir maliyet) */
    c->words = (u32*)arena_try_nozero(frame, (usize)c->cap * 4u, 4);
    c->max_slots = max_draws * 2u;   /* köşe + parça */
    c->uniforms = (u8*)arena_try_nozero(frame, (usize)c->max_slots * GCMD_SLOT, GCMD_SLOT);
    c->seen_cap = 1;
    while (c->seen_cap < max_draws * 4u) c->seen_cap <<= 1;   /* çizim başına mesh + 2 doku: en çok %75 dolu */
    c->seen = ARENA_TRY_ARRAY(frame, u32, c->seen_cap);
    if (!c->words || !c->uniforms || !c->seen) {
        c->cap = 0;
        return FALSE;
    }
    c->max_draws = max_draws;
    c->std_vert = std_vert;
    c->count = GCMD_HEADER;
    return TRUE;
}

/* TRUE: bu karede ilk kez görüldü */
static b32 seen_first(GpuCmd* c, u32 id) {
    u32 mask = c->seen_cap - 1, h = (id * 2654435761u) & mask;
    while (c->seen[h]) {
        if (c->seen[h] == id) return FALSE;
        h = (h + 1) & mask;
    }
    c->seen[h] = id;
    c->seen_count++;
    return TRUE;
}

/* doku px'inin sözcük sayısı: son seviyenin sonu (görüntü tamponu + mip zinciri) */
static u32 tex_px_words(const Texture* t) {
    u32 last = t->levels ? t->levels - 1u : 0u;
    return t->lv_ofs[last] + t->lv_w[last] * t->lv_h[last];
}

static u32 tex_record(GpuCmd* c, const Texture* t) {
    u32 id = (u32)(uptr)t;
    if (t && seen_first(c, id)) {
        u32 r[GCMD_TEX_WORDS] = { GC_TEX, id, (u32)(uptr)t->px, tex_px_words(t) };
        mem_copy(c->words + c->count, r, sizeof(r));
        c->count += GCMD_TEX_WORDS;
    }
    return id;
}

void gcmd_shadow_begin(GpuCmd* c, const Texture* tex, u32 size) {
    if (!c->cap) return;
    u32 id = (u32)(uptr)tex;
    seen_first(c, id);   /* içerik GPU'da: GC_TEX yazılmaz */
    u32 r[GCMD_SHADOW_WORDS] = { GC_SHADOW, id, size };
    mem_copy(c->words + c->count, r, sizeof(r));
    c->count += GCMD_SHADOW_WORDS;
}

void gcmd_shadow_end(GpuCmd* c) {
    if (c->cap) c->words[c->count++] = GC_SHADOW_END;
}

void gcmd_frame(GpuCmd* c, u32 w, u32 h, u32 clear_rgba, f32 clear_depth, u32 samples) {
    if (!c->cap) return;
    u32 db;
    mem_copy(&db, &clear_depth, 4);
    u32 fr[GCMD_FRAME_WORDS] = { GC_FRAME, w, h, clear_rgba, db, samples };
    mem_copy(c->words + c->count, fr, sizeof(fr));
    c->count += GCMD_FRAME_WORDS;
}

void gcmd_overlay(GpuCmd* c, u32 x, u32 y, u32 w, u32 h, const u32* px) {
    if (!c->cap || !px || !w || !h || c->overlays >= GCMD_OVERLAYS_MAX) return;
    c->overlays++;
    u32 r[GCMD_OVERLAY_WORDS] = { GC_OVERLAY, x, y, w, h, (u32)(uptr)px };
    mem_copy(c->words + c->count, r, sizeof(r));
    c->count += GCMD_OVERLAY_WORDS;
}

void gcmd_overlay_extract(const u32* on_black, const u32* on_white, u32* out, u32 n) {
    for (u32 i = 0; i < n; i++) {
        u32 b = on_black[i], w = on_white[i], d = 0;
        for (u32 ch = 0; ch < 24; ch += 8) d += ((w >> ch) & 0xFFu) - ((b >> ch) & 0xFFu);
        u32 a = 255u - (d + 1u) / 3u;   /* üç kanalın ortalaması: yuvarlama gürültüsünü azaltır */
        out[i] = (b & 0x00FFFFFFu) | (a << 24);
    }
}

static u32 slot_take(GpuCmd* c, const void* src, u32 bytes) {
    u32 slot = c->slots++;
    u8* dst = c->uniforms + (usize)slot * GCMD_SLOT;
    mem_zero(dst, GCMD_SLOT);
    if (src) mem_copy(dst, src, bytes);
    return slot;
}

void gcmd_draw(GpuCmd* c, const Mesh* m, const VertUniforms* vu, const GpuProgram* vert, const GpuProgram* frag, const void* uniforms,
               CullMode cull, b32 depth_test, b32 depth_write, b32 depth_only) {
    if (!c->cap || !m->vertex_count || !m->index_count) return;
    if (!vert) vert = c->std_vert;
    if (!vert || c->draws >= c->max_draws || c->slots + 2u > c->max_slots || c->count + GCMD_DRAW_MAX + 1u > c->cap ||
        c->seen_count + 3u > c->seen_cap - c->seen_cap / 4u) {
        c->overflow++;
        return;
    }
    u32 id = (u32)(uptr)m;
    if (seen_first(c, id)) {
        u32 r[GCMD_MESH_WORDS] = { GC_MESH, id, m->gen, m->vertex_count, m->index_count, (u32)(uptr)m->px,
                                   (u32)(uptr)m->py, (u32)(uptr)m->pz, (u32)(uptr)m->nx, (u32)(uptr)m->ny,
                                   (u32)(uptr)m->nz, (u32)(uptr)m->u, (u32)(uptr)m->v, (u32)(uptr)m->indices };
        mem_copy(c->words + c->count, r, sizeof(r));
        c->count += GCMD_MESH_WORDS;
    }
    u32 tex[WGSL_MAX_TEX] = { 0, 0 };
    if (!depth_only && frag && uniforms) {
        for (u32 k = 0; k < frag->wgsl.tex_count; k++) {
            const Texture* t;
            mem_copy(&t, (const u8*)uniforms + frag->wgsl.tex_ofs[k], sizeof(t));
            tex[k] = tex_record(c, t);
        }
    }
    u32 vslot = slot_take(c, vu, (u32)sizeof(*vu));
    u32 fslot;
    if (depth_only) {
        fslot = vslot;   /* parça aşaması yok, bağlama geçerli olsun yeter */
    } else if (frag && uniforms == c->last_uniforms && frag == c->last_frag) {
        fslot = c->last_slot;
    } else {
        fslot = slot_take(c, frag ? uniforms : NULL, frag ? frag->wgsl.uniform_size : 0u);
        c->last_uniforms = uniforms;
        c->last_frag = frag;
        c->last_slot = fslot;
    }
    u32 d[GCMD_DRAW_WORDS] = { GC_DRAW, id, vslot, fslot, vert->id, depth_only ? GPROG_NONE : frag ? frag->id : GPROG_FLAT, tex[0], tex[1], (u32)cull,
                               depth_test ? 1u : 0u, depth_write ? 1u : 0u };
    mem_copy(c->words + c->count, d, sizeof(d));
    c->count += GCMD_DRAW_WORDS;
    c->draws++;
}

void gcmd_end(GpuCmd* c) {
    if (!c->cap) return;
    c->words[c->count++] = GC_END;
    u32 hdr[GCMD_HEADER] = { GCMD_MAGIC, GCMD_VERSION, c->count, c->draws, (u32)(uptr)c->uniforms, c->slots * GCMD_SLOT };
    mem_copy(c->words, hdr, sizeof(hdr));
}
