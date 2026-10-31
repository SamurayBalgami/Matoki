/* gpu/gcmd.c — GPU komut akışı yazıcısı (gpu/gcmd.h). Başlık 6 sözcük, ardından kayıtlar; MESH kaydı bir karede mesh
 * başına bir kez (ilk çiziminden önce). Taşma: kalan çizimler atlanır ve sayılır, akış her zaman GC_END ile kapanır. */
#include "gpu/gcmd.h"
#include "core/mem.h"

#define GCMD_HEADER 6u
#define GCMD_MESH_WORDS 12u
#define GCMD_DRAW_WORDS 6u

b32 gcmd_begin(GpuCmd* c, Arena* frame, u32 max_draws, u32 w, u32 h, u32 clear_rgba, f32 clear_depth, u32 mode) {
    mem_zero(c, sizeof(*c));
    c->cap = GCMD_HEADER + 8u + max_draws * (GCMD_MESH_WORDS + GCMD_DRAW_WORDS) + 1u;
    c->words = ARENA_TRY_ARRAY(frame, u32, c->cap);
    c->uniforms = (u8*)arena_try(frame, (usize)max_draws * GCMD_SLOT, GCMD_SLOT);
    c->mesh_seen_cap = 1;
    while (c->mesh_seen_cap < max_draws * 2u) c->mesh_seen_cap <<= 1;
    c->mesh_seen = ARENA_TRY_ARRAY(frame, u32, c->mesh_seen_cap);
    if (!c->words || !c->uniforms || !c->mesh_seen) {
        c->cap = 0;
        return FALSE;
    }
    c->max_draws = max_draws;
    c->count = GCMD_HEADER;
    u32 db;
    mem_copy(&db, &clear_depth, 4);
    u32 fr[6] = { GC_FRAME, w, h, clear_rgba, db, mode };
    mem_copy(c->words + c->count, fr, sizeof(fr));
    c->count += 6;
    return TRUE;
}

/* TRUE: bu karede ilk kez görüldü */
static b32 mesh_first(GpuCmd* c, u32 id) {
    u32 mask = c->mesh_seen_cap - 1, h = (id * 2654435761u) & mask;
    while (c->mesh_seen[h]) {
        if (c->mesh_seen[h] == id) return FALSE;
        h = (h + 1) & mask;
    }
    c->mesh_seen[h] = id;
    return TRUE;
}

void gcmd_draw(GpuCmd* c, const Mesh* m, const Mat4* model, const Mat4* view_proj, const Mat3* normal_mat, CullMode cull,
               b32 depth_test, b32 depth_write) {
    if (!c->cap || !m->vertex_count || !m->index_count) return;
    if (c->draws >= c->max_draws || c->count + GCMD_MESH_WORDS + GCMD_DRAW_WORDS + 1u > c->cap) {
        c->overflow++;
        return;
    }
    u32 id = (u32)(uptr)m;
    if (mesh_first(c, id)) {
        u32 r[GCMD_MESH_WORDS] = { GC_MESH, id, m->gen, m->vertex_count, m->index_count, (u32)(uptr)m->px,
                                   (u32)(uptr)m->py, (u32)(uptr)m->pz, (u32)(uptr)m->nx, (u32)(uptr)m->ny,
                                   (u32)(uptr)m->nz, (u32)(uptr)m->indices };
        mem_copy(c->words + c->count, r, sizeof(r));
        c->count += GCMD_MESH_WORDS;
    }
    GpuDrawUniforms* u = (GpuDrawUniforms*)(c->uniforms + (usize)c->draws * GCMD_SLOT);
    mem_zero(u, GCMD_SLOT);
    u->model = *model;
    u->view_proj = *view_proj;
    for (u32 col = 0; col < 3; col++) {
        for (u32 row = 0; row < 3; row++) u->normal_mat[col * 4 + row] = normal_mat->m[col * 3 + row];
    }
    u32 d[GCMD_DRAW_WORDS] = { GC_DRAW, id, c->draws, (u32)cull, depth_test ? 1u : 0u, depth_write ? 1u : 0u };
    mem_copy(c->words + c->count, d, sizeof(d));
    c->count += GCMD_DRAW_WORDS;
    c->draws++;
}

void gcmd_end(GpuCmd* c) {
    if (!c->cap) return;
    c->words[c->count++] = GC_END;
    u32 hdr[GCMD_HEADER] = { GCMD_MAGIC, GCMD_VERSION, c->count, c->draws, (u32)(uptr)c->uniforms, c->draws * GCMD_SLOT };
    mem_copy(c->words, hdr, sizeof(hdr));
}
