/* gfx3d/pipeline.c — 3D boru hattı: ertelenmiş çizimler, paralel köşe aşaması (VertexFn: C kâhini ya da
 * Matoki vert, 4 köşe/çağrı) ve paralel üçgen kurulumu (kırpma → izdüşüm → raster parçası). Sıra: parça
 * indeksi = gönderim sırası → çıktı thread sayısından bağımsız. Çatlak önleme: kırpma kesişimi kenar uçları
 * kanonik sıraya dizilerek hesaplanır. Varying'ler klip uzayında doğrusal (raster var*inv_w'yi interpole eder). */
#include "gfx3d/pipeline.h"
#include "core/jobs.h"
#include "core/mem.h"
#include "core/sys.h"

#define OC_LEFT   (1u << 0)
#define OC_RIGHT  (1u << 1)
#define OC_BOTTOM (1u << 2)
#define OC_TOP    (1u << 3)
#define OC_NEAR   (1u << 4)
#define OC_FAR    (1u << 5)
#define OC_GL     (1u << 6)
#define OC_GR     (1u << 7)
#define OC_GB     (1u << 8)
#define OC_GT     (1u << 9)
#define OC_FRUSTUM (OC_LEFT | OC_RIGHT | OC_BOTTOM | OC_TOP | OC_NEAR | OC_FAR)
#define OC_GUARD   (OC_GL | OC_GR | OC_GB | OC_GT)

static inline Vec4 mat_row(const Mat4* m, u32 r) { return v4(m->m[r], m->m[4 + r], m->m[8 + r], m->m[12 + r]); }

static inline Vec4 plane_norm(Vec4 p) {
    f32 l = m_sqrtf(p.x * p.x + p.y * p.y + p.z * p.z);
    f32 s = l > 0.0f ? 1.0f / l : 0.0f;
    return v4(p.x * s, p.y * s, p.z * s, p.w * s);
}

void pipe_extract_frustum(const Mat4* vp, Vec4 out[6]) {
    Vec4 r0 = mat_row(vp, 0), r1 = mat_row(vp, 1), r2 = mat_row(vp, 2), r3 = mat_row(vp, 3);
    out[0] = plane_norm(v4(r3.x + r0.x, r3.y + r0.y, r3.z + r0.z, r3.w + r0.w));
    out[1] = plane_norm(v4(r3.x - r0.x, r3.y - r0.y, r3.z - r0.z, r3.w - r0.w));
    out[2] = plane_norm(v4(r3.x + r1.x, r3.y + r1.y, r3.z + r1.z, r3.w + r1.w));
    out[3] = plane_norm(v4(r3.x - r1.x, r3.y - r1.y, r3.z - r1.z, r3.w - r1.w));
    out[4] = plane_norm(r2);                                                       /* z >= 0 */
    out[5] = plane_norm(v4(r3.x - r2.x, r3.y - r2.y, r3.z - r2.z, r3.w - r2.w));  /* z <= w */
}

b32 pipe_sphere_visible(const Vec4 planes[6], Vec3 c, f32 r) {
    for (u32 i = 0; i < 6; i++) {
        if (planes[i].x * c.x + planes[i].y * c.y + planes[i].z * c.z + planes[i].w < -r) return FALSE;
    }
    return TRUE;
}

b32 pipe_aabb_visible(const Vec4 planes[6], Vec3 mn, Vec3 mx) {
    for (u32 i = 0; i < 6; i++) {
        Vec4 p = planes[i];
        Vec3 v = v3(p.x >= 0 ? mx.x : mn.x, p.y >= 0 ? mx.y : mn.y, p.z >= 0 ? mx.z : mn.z);
        if (p.x * v.x + p.y * v.y + p.z * v.z + p.w < 0.0f) return FALSE;
    }
    return TRUE;
}

/* Arvo: dönüştürülmüş AABB = merkez' ± |M| * yarı-boyut */
void pipe_transform_aabb(const Mat4* m, Vec3 mn, Vec3 mx, Vec3* out_mn, Vec3* out_mx) {
    Vec3 c = mat4_mul_point(m, v3_scale(v3_add(mn, mx), 0.5f));
    Vec3 e = v3_scale(v3_sub(mx, mn), 0.5f);
    const f32* a = m->m;
    Vec3 ne = v3(m_absf(a[0]) * e.x + m_absf(a[4]) * e.y + m_absf(a[8]) * e.z,
                 m_absf(a[1]) * e.x + m_absf(a[5]) * e.y + m_absf(a[9]) * e.z,
                 m_absf(a[2]) * e.x + m_absf(a[6]) * e.y + m_absf(a[10]) * e.z);
    *out_mn = v3_sub(c, ne);
    *out_mx = v3_add(c, ne);
}

static inline f32 cv_dist(const ClipVert* v, Vec4 p) { return p.x * v->x + p.y * v->y + p.z * v->z + p.w * v->w; }

static inline b32 cv_less(const ClipVert* a, const ClipVert* b) {
    if (a->x != b->x) return a->x < b->x;
    if (a->y != b->y) return a->y < b->y;
    if (a->z != b->z) return a->z < b->z;
    return a->w < b->w;
}

static void cv_intersect(const ClipVert* a, const ClipVert* b, Vec4 pl, u32 nv, ClipVert* o) {
    if (cv_less(b, a)) {
        const ClipVert* t = a; a = b; b = t;
    }
    f32 da = cv_dist(a, pl), db = cv_dist(b, pl);
    f32 t = da / (da - db);
    o->x = a->x + t * (b->x - a->x);
    o->y = a->y + t * (b->y - a->y);
    o->z = a->z + t * (b->z - a->z);
    o->w = a->w + t * (b->w - a->w);
    for (u32 i = 0; i < nv; i++) o->var[i] = a->var[i] + t * (b->var[i] - a->var[i]);
}

static inline void cv_copy(ClipVert* dst, const ClipVert* src, u32 nv) {
    dst->x = src->x; dst->y = src->y; dst->z = src->z; dst->w = src->w;
    for (u32 i = 0; i < nv; i++) dst->var[i] = src->var[i];
}

u32 pipe_clip_polygon(const ClipVert* in, u32 n, ClipVert* out, Vec4 pl, u32 nv) {
    u32 m = 0;
    for (u32 i = 0; i < n; i++) {
        const ClipVert* a = &in[i];
        const ClipVert* b = &in[i + 1 == n ? 0 : i + 1];
        f32 da = cv_dist(a, pl), db = cv_dist(b, pl);
        b32 ina = da >= 0.0f, inb = db >= 0.0f;
        if (ina) cv_copy(&out[m++], a, nv);
        if (ina != inb) cv_intersect(a, b, pl, nv, &out[m++]);
    }
    return m;
}

void pipe_project(const Pipeline* p, const ClipVert* c, RasterVert* o, u32 nv) {
    f32 iw = 1.0f / c->w;
    o->x = (c->x * iw * 0.5f + 0.5f) * p->vw;
    o->y = (0.5f - c->y * iw * 0.5f) * p->vh;
    o->z = c->z * iw;
    o->inv_w = iw;
    for (u32 i = 0; i < nv; i++) o->var[i] = c->var[i];
}

void pipe_begin(Pipeline* p, Raster* r, Arena* frame, Mat4 view, Mat4 proj, Vec3 eye) {
    p->r = r;
    p->frame = frame;
    p->view = view;
    p->proj = proj;
    p->view_proj = mat4_mul(proj, view);
    p->eye = eye;
    pipe_extract_frustum(&p->view_proj, p->frustum);
    p->vw = (f32)r->fb->w;
    p->vh = (f32)r->fb->h;
    /* ekran x_s = -GUARD  ⇔  ndc = -1 - 2*GUARD/vw */
    p->guard_x = 1.0f + 2.0f * PIPE_GUARD_PX / p->vw;
    p->guard_y = 1.0f + 2.0f * PIPE_GUARD_PX / p->vh;
    p->time = 0.0f;
    p->frame_mark = arena_mark(frame);
    p->draws = ARENA_TRY_ARRAY(frame, DrawRec, PIPE_MAX_DRAWS);   /* NULL: bu kare çizim kaydı yok (sayılır) */
    p->ndraws = 0;
    p->batches = 0;
    p->vert_ms = p->setup_ms = p->raster_ms = 0.0;
    mem_zero(&p->stats, sizeof(p->stats));
}

void pipe_set_time(Pipeline* p, f32 t) { p->time = t; }

static inline u32 outcode(const ClipVert* v, f32 gx, f32 gy) {
    u32 c = 0;
    if (v->x < -v->w) c |= OC_LEFT;
    if (v->x > v->w) c |= OC_RIGHT;
    if (v->y < -v->w) c |= OC_BOTTOM;
    if (v->y > v->w) c |= OC_TOP;
    if (v->z < 0.0f) c |= OC_NEAR;
    if (v->z > v->w) c |= OC_FAR;
    if (v->x < -gx * v->w) c |= OC_GL;
    if (v->x > gx * v->w) c |= OC_GR;
    if (v->y < -gy * v->w) c |= OC_GB;
    if (v->y > gy * v->w) c |= OC_GT;
    return c;
}

/* ---- C kâhini vertex shader (eski sabit kodlu yol ile aynı aritmetik) ---- */
void vert_std_c(const VertexIn* in, const void* uniforms, VertexOut* out) {
    const VertUniforms* u = (const VertUniforms*)uniforms;
    for (u32 l = 0; l < 4; l++) {
        Vec3 pos = v3(in->attr[ATTR3D_POS][l], in->attr[ATTR3D_POS + 1][l], in->attr[ATTR3D_POS + 2][l]);
        Vec3 nrm = v3(in->attr[ATTR3D_NRM][l], in->attr[ATTR3D_NRM + 1][l], in->attr[ATTR3D_NRM + 2][l]);
        Vec3 wp = mat4_mul_point(&u->model, pos);
        Vec4 c = mat4_mul_v4(&u->view_proj, v4_from3(wp, 1.0f));
        Vec3 wn = mat3_mul_v3(&u->normal_mat, nrm);
        out->pos[0][l] = c.x;
        out->pos[1][l] = c.y;
        out->pos[2][l] = c.z;
        out->pos[3][l] = c.w;
        out->var[VAR3D_WPOS][l] = wp.x;
        out->var[VAR3D_WPOS + 1][l] = wp.y;
        out->var[VAR3D_WPOS + 2][l] = wp.z;
        out->var[VAR3D_NRM][l] = wn.x;
        out->var[VAR3D_NRM + 1][l] = wn.y;
        out->var[VAR3D_NRM + 2][l] = wn.z;
        out->var[VAR3D_UV][l] = in->attr[ATTR3D_UV][l];
        out->var[VAR3D_UV + 1][l] = in->attr[ATTR3D_UV + 1][l];
    }
}

void pipe_draw(Pipeline* p, const DrawItem* d) {
    const Mesh* m = d->mesh;
    const Material* mat = d->mat;
    p->stats.objects_in++;

    /* frustum eleme: vertex shader yer değiştirmesi için cull_margin kadar genişletilmiş sınırlar */
    f32 margin = mat->cull_margin;
    Vec3 sc = mat4_mul_point(&d->model, m->sphere_center);
    f32 sr = m->sphere_radius * mat4_max_scale(&d->model) + margin;
    Vec3 bmn, bmx;
    pipe_transform_aabb(&d->model, m->aabb_min, m->aabb_max, &bmn, &bmx);
    bmn = v3_sub(bmn, v3(margin, margin, margin));
    bmx = v3_add(bmx, v3(margin, margin, margin));
    if (!pipe_sphere_visible(p->frustum, sc, sr) || !pipe_aabb_visible(p->frustum, bmn, bmx)) {
        p->stats.objects_culled++;
        return;
    }
    VertUniforms* vu = p->draws && p->ndraws < PIPE_MAX_DRAWS ? ARENA_TRY_ARRAY(p->frame, VertUniforms, 1) : NULL;
    if (!vu) {   /* çizim sınırı ya da kare belleği doldu: motor durmaz, çizim atlanır */
        p->stats.draws_dropped++;
        return;
    }
    DrawRec* r = &p->draws[p->ndraws++];
    mem_zero(r, sizeof(*r));
    r->mesh = m;
    r->mat = mat;
    vu->model = d->model;
    vu->view_proj = p->view_proj;
    vu->normal_mat = mat3_normal_from_mat4(&d->model);
    vu->time = p->time;
    for (u32 k = 0; k < VERT_PARAMS; k++) vu->params[k] = mat->vert_params[k];
    r->vu = vu;
    /* çizim durumu: materyal + raster'ın o anki derinlik ayarı */
    const Raster* rs = p->r;
    r->rs.shade = mat->shade;
    r->rs.prelude = mat->prelude;
    r->rs.uniforms = mat->uniforms;
    r->rs.var_count = VAR3D_COUNT;
    r->rs.cull = mat->cull;
    r->rs.depth_test = rs->depth_test;
    r->rs.depth_write = rs->depth_write;
    r->rs.depth_func = rs->depth_func;
    r->rs.blend = rs->blend;
}

/* ---- paralel aşamalar ---- */

#define VJOB_VERTS  128    /* köşe işi başına köşe (4'ün katı; 256 → göl 66 iş, dengesiz) */
#define TCHUNK_TRIS 256    /* üçgen parçası başına üçgen */
#define TRI_BYTES_EST 1024 /* bütçe: üçgen başına en kötü kurulum + bin belleği (kırpma fanı dahil ortalama) */

typedef struct ThreadGeo {
    PipeStats   ps;
    RasterStats rs;
    PreludeFn   last_pre;
    const void* last_uni;
    u8          pad[64];
} ThreadGeo;

typedef struct GeoCtx {
    Pipeline*  p;
    u32        d0, d1;         /* parti: çizim aralığı */
    u32        total_verts, total_tris;
    u32        first_chunk;
    ThreadGeo  th[JOBS_MAX_THREADS];
} GeoCtx;

static GeoCtx g_geo;

/* önek toplamında global indeksin çizimini bul (ikili arama) */
static u32 find_draw(const Pipeline* p, u32 d0, u32 d1, u32 global, b32 tris) {
    u32 lo = d0, hi = d1 - 1;
    while (lo < hi) {
        u32 mid = (lo + hi + 1) >> 1;
        u32 base = tris ? p->draws[mid].tri_base : p->draws[mid].vert_base;
        if (base <= global) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

static void vertex_range(GeoCtx* gc, DrawRec* d, u32 a, u32 b, u32 tid) {
    const Mesh* m = d->mesh;
    VertexFn vf = d->mat->vert ? d->mat->vert : vert_std_c;
    PreludeFn pre = d->mat->vert ? d->mat->vert_prelude : NULL;
    ThreadGeo* th = &gc->th[tid];
    if (pre && (pre != th->last_pre || d->vu != th->last_uni)) {
        pre(d->vu);
        th->last_pre = pre;
        th->last_uni = d->vu;
    }
    const Pipeline* p = gc->p;
    VertexIn in;
    VertexOut out;
    in._pad[0] = in._pad[1] = in._pad[2] = 0;
    for (u32 i = a; i < b; i += 4) {
        u32 n = MIN(4u, b - i);
        in.count = n;
        for (u32 l = 0; l < 4; l++) {
            u32 s = i + MIN(l, n - 1);   /* geçersiz lane = son geçerli köşe */
            in.attr[ATTR3D_POS][l] = m->px[s];
            in.attr[ATTR3D_POS + 1][l] = m->py[s];
            in.attr[ATTR3D_POS + 2][l] = m->pz[s];
            in.attr[ATTR3D_NRM][l] = m->nx[s];
            in.attr[ATTR3D_NRM + 1][l] = m->ny[s];
            in.attr[ATTR3D_NRM + 2][l] = m->nz[s];
            in.attr[ATTR3D_UV][l] = m->u[s];
            in.attr[ATTR3D_UV + 1][l] = m->v[s];
        }
        vf(&in, d->vu, &out);
        for (u32 l = 0; l < n; l++) {
            ClipVert* o = &d->cv[i + l];
            o->x = out.pos[0][l];
            o->y = out.pos[1][l];
            o->z = out.pos[2][l];
            o->w = out.pos[3][l];
            for (u32 k = 0; k < VAR3D_COUNT; k++) o->var[k] = out.var[k][l];
            d->oc[i + l] = outcode(o, p->guard_x, p->guard_y);
        }
    }
    th->ps.verts_transformed += b - a;
}

static void vertex_job(void* ctx, u32 index, u32 tid) {
    GeoCtx* gc = (GeoCtx*)ctx;
    Pipeline* p = gc->p;
    u32 g0 = index * VJOB_VERTS, g1 = MIN(g0 + VJOB_VERTS, gc->total_verts);
    u32 di = find_draw(p, gc->d0, gc->d1, g0, FALSE);
    while (g0 < g1 && di < gc->d1) {
        DrawRec* d = &p->draws[di];
        u32 vc = d->nv;
        u32 a = g0 - d->vert_base, b = MIN(vc, g1 - d->vert_base);
        if (a < b) vertex_range(gc, d, a, b, tid);
        g0 = d->vert_base + b;
        di++;
    }
}

static void emit_polygon(GeoCtx* gc, u32 chunk, const DrawRec* d, const ClipVert* poly, u32 n, ThreadGeo* th, RasterAlloc* ra) {
    const Pipeline* p = gc->p;
    const u32 nv = VAR3D_COUNT;
    if (n < 3) return;
    for (u32 i = 0; i < n; i++) {
        if (!(poly[i].w > 1e-7f)) return;   /* yakın kırpmadan sonra olmamalı; NaN güvenliği */
    }
    RasterVert rv0, rv1, rv2;
    pipe_project(p, &poly[0], &rv0, nv);
    pipe_project(p, &poly[1], &rv1, nv);
    for (u32 i = 2; i < n; i++) {
        pipe_project(p, &poly[i], &rv2, nv);
        raster_triangle_to(p->r, chunk, &d->rs, &rv0, &rv1, &rv2, &th->rs, ra);
        th->ps.tris_emitted++;
        rv1 = rv2;
    }
}

static void triangle_range(GeoCtx* gc, u32 chunk, const DrawRec* d, u32 t0, u32 t1, ThreadGeo* th, RasterAlloc* ra) {
    const Pipeline* p = gc->p;
    const Mesh* m = d->mesh;
    const u32 nv = VAR3D_COUNT;
    const u32 vc = m->vertex_count;
    const ClipVert* cv = d->cv;
    const u32* oc = d->oc;
    static const Vec4 guard_planes_unit[4] = { { 1, 0, 0, 1 }, { -1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, -1, 0, 1 } };
    static const u32 guard_bits[4] = { OC_GL, OC_GR, OC_GB, OC_GT };
    const Vec4 near_plane = v4(0, 0, 1, 0);
    ClipVert buf_a[CLIP_MAX_VERTS], buf_b[CLIP_MAX_VERTS];
    for (u32 tri = t0; tri < t1; tri++) {
        u32 t = tri * 3;
        u32 i0 = m->indices[t], i1 = m->indices[t + 1], i2 = m->indices[t + 2];
        if (i0 >= vc || i1 >= vc || i2 >= vc) continue;
        th->ps.tris_in++;
        u32 c_and = oc[i0] & oc[i1] & oc[i2];
        u32 c_or = oc[i0] | oc[i1] | oc[i2];
        if (c_and & OC_FRUSTUM) {
            th->ps.tris_rejected++;
            continue;
        }
        if (!(c_or & (OC_NEAR | OC_GUARD))) {
            RasterVert a, b, c;
            pipe_project(p, &cv[i0], &a, nv);
            pipe_project(p, &cv[i1], &b, nv);
            pipe_project(p, &cv[i2], &c, nv);
            raster_triangle_to(p->r, chunk, &d->rs, &a, &b, &c, &th->rs, ra);
            th->ps.tris_emitted++;
            continue;
        }
        ClipVert* src = buf_a;
        ClipVert* dst = buf_b;
        cv_copy(&src[0], &cv[i0], nv);
        cv_copy(&src[1], &cv[i1], nv);
        cv_copy(&src[2], &cv[i2], nv);
        u32 n = 3;
        if (c_or & OC_NEAR) {
            n = pipe_clip_polygon(src, n, dst, near_plane, nv);
            ClipVert* tmp = src; src = dst; dst = tmp;
            th->ps.tris_near_clipped++;
        }
        if (c_or & OC_GUARD) {
            th->ps.tris_guard_clipped++;
            for (u32 g = 0; g < 4 && n >= 3; g++) {
                if (!(c_or & guard_bits[g])) continue;
                Vec4 gp = guard_planes_unit[g];
                f32 gk = g < 2 ? p->guard_x : p->guard_y;
                n = pipe_clip_polygon(src, n, dst, v4(gp.x, gp.y, gp.z, gk), nv);
                ClipVert* tmp = src; src = dst; dst = tmp;
            }
        }
        emit_polygon(gc, chunk, d, src, n, th, ra);
    }
}

static void triangle_job(void* ctx, u32 index, u32 tid) {
    GeoCtx* gc = (GeoCtx*)ctx;
    Pipeline* p = gc->p;
    ThreadGeo* th = &gc->th[tid];
    u32 chunk = gc->first_chunk + index;
    u32 g0 = index * TCHUNK_TRIS, g1 = MIN(g0 + TCHUNK_TRIS, gc->total_tris);
    u32 di = find_draw(p, gc->d0, gc->d1, g0, TRUE);
    RasterAlloc ra = { NULL, NULL, 0 };
    while (g0 < g1 && di < gc->d1) {
        const DrawRec* d = &p->draws[di];
        u32 tc = d->nt;
        u32 a = g0 - d->tri_base, b = MIN(tc, g1 - d->tri_base);
        if (a < b) triangle_range(gc, chunk, d, a, b, th, &ra);
        g0 = d->tri_base + b;
        di++;
    }
    raster_alloc_done(p->r, &ra);
}

/* [d0, d1) çizimlerini işle: köşe aşaması → üçgen kurulumu (raster parçalarına) */
static void run_batch(Pipeline* p, u32 d0, u32 d1) {
    GeoCtx* gc = &g_geo;
    mem_zero(gc, sizeof(*gc));
    gc->p = p;
    gc->d0 = d0;
    gc->d1 = d1;
    usize mark = arena_mark(p->frame);
    u32 tv = 0, tt = 0;
    for (u32 i = d0; i < d1; i++) {
        DrawRec* d = &p->draws[i];
        u32 vc = d->mesh->vertex_count;
        d->vert_base = tv;
        d->tri_base = tt;
        d->cv = (ClipVert*)arena_try_array(p->frame, sizeof(ClipVert), vc ? vc : 1, _Alignof(ClipVert));
        d->oc = d->cv ? (u32*)arena_try_array(p->frame, sizeof(u32), vc ? vc : 1, _Alignof(u32)) : NULL;
        if (!d->oc) {   /* köşe belleği yok: çizim bu partide atlanır */
            d->nv = d->nt = 0;
            p->stats.draws_dropped++;
            continue;
        }
        d->nv = vc;
        d->nt = d->mesh->index_count / 3;
        tv += d->nv;
        tt += d->nt;
    }
    gc->total_verts = tv;
    gc->total_tris = tt;
    f64 t0 = sys_now_ms();
    jobs_parallel_for(vertex_job, gc, (tv + VJOB_VERTS - 1) / VJOB_VERTS);
    f64 t1 = sys_now_ms();
    u32 nchunks = (tt + TCHUNK_TRIS - 1) / TCHUNK_TRIS;
    if (nchunks) {
        gc->first_chunk = raster_chunks_alloc(p->r, nchunks);
        jobs_parallel_for(triangle_job, gc, nchunks);
    }
    p->vert_ms += t1 - t0;
    p->setup_ms += sys_now_ms() - t1;
    /* thread istatistiklerini topla */
    for (u32 k = 0; k < JOBS_MAX_THREADS; k++) {
        const PipeStats* s = &gc->th[k].ps;
        p->stats.tris_in += s->tris_in;
        p->stats.tris_rejected += s->tris_rejected;
        p->stats.tris_near_clipped += s->tris_near_clipped;
        p->stats.tris_guard_clipped += s->tris_guard_clipped;
        p->stats.tris_emitted += s->tris_emitted;
        p->stats.verts_transformed += s->verts_transformed;
        const RasterStats* rs = &gc->th[k].rs;
        RasterStats* R = &p->r->stats;
        R->tris_in += rs->tris_in;
        R->tris_culled += rs->tris_culled;
        R->tris_degenerate += rs->tris_degenerate;
        R->tris_empty += rs->tris_empty;
        R->tris_overflow += rs->tris_overflow;
        R->tris_dropped += rs->tris_dropped;
        R->bin_items += rs->bin_items;
        R->bin_full += rs->bin_full;
    }
    arena_reset(p->frame, mark);
    p->batches++;
}

void pipe_end(Pipeline* p) {
    /* partiler: tahmini kurulum belleği raster arenasının yarısını aşmasın (paralel kurulumda flush yok) */
    u32 d0 = 0;
    while (d0 < p->ndraws) {
        usize budget = raster_room(p->r) / 2;
        usize est = 64u * 1024u;
        u32 d1 = d0;
        while (d1 < p->ndraws) {
            usize add = (usize)(p->draws[d1].mesh->index_count / 3) * TRI_BYTES_EST + 16u * 1024u;
            if (d1 > d0 && est + add > budget) break;
            est += add;
            d1++;
        }
        run_batch(p, d0, d1);
        d0 = d1;
        if (d0 < p->ndraws) {
            f64 t = sys_now_ms();
            raster_flush(p->r);
            p->raster_ms += sys_now_ms() - t;
        }
    }
    f64 t = sys_now_ms();
    raster_flush(p->r);
    p->raster_ms += sys_now_ms() - t;
    /* çizim kayıtları ve VertUniforms: pipe_begin'deki işarete dön */
    arena_reset(p->frame, p->frame_mark);
    p->ndraws = 0;
}