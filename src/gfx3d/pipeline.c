/* gfx3d/pipeline.c — 3D boru hattının uygulaması (skaler v0; SIMD vertex aşaması sonra).
 * Çatlak önleme: kırpma kesişimi, kenarın iki ucu kanonik sıraya (sözlüksel) dizilerek
 * hesaplanır → ortak kenarı farklı yönde gezen iki üçgen bit-bit aynı noktayı üretir.
 * Varying'ler klip uzayında doğrusal interpole edilir (perspektif-doğru, çünkü raster
 * var*inv_w'yi ekran uzayında interpole eder). */
#include "gfx3d/pipeline.h"
#include "core/mem.h"

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
    mem_zero(&p->stats, sizeof(p->stats));
}

void pipe_end(Pipeline* p) { raster_flush(p->r); }

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

static void emit_polygon(Pipeline* p, const ClipVert* poly, u32 n, u32 nv) {
    if (n < 3) return;
    for (u32 i = 0; i < n; i++) {
        if (!(poly[i].w > 1e-7f)) return;   /* yakın kırpmadan sonra olmamalı; NaN güvenliği */
    }
    RasterVert rv0, rv1, rv2;
    pipe_project(p, &poly[0], &rv0, nv);
    pipe_project(p, &poly[1], &rv1, nv);
    for (u32 i = 2; i < n; i++) {
        pipe_project(p, &poly[i], &rv2, nv);
        raster_triangle(p->r, &rv0, &rv1, &rv2);
        p->stats.tris_emitted++;
        rv1 = rv2;
    }
}

void pipe_draw(Pipeline* p, const DrawItem* d) {
    const Mesh* m = d->mesh;
    p->stats.objects_in++;

    Vec3 sc = mat4_mul_point(&d->model, m->sphere_center);
    f32 sr = m->sphere_radius * mat4_max_scale(&d->model);
    Vec3 bmn, bmx;
    pipe_transform_aabb(&d->model, m->aabb_min, m->aabb_max, &bmn, &bmx);
    if (!pipe_sphere_visible(p->frustum, sc, sr) || !pipe_aabb_visible(p->frustum, bmn, bmx)) {
        p->stats.objects_culled++;
        return;
    }

    const u32 nv = VAR3D_COUNT;
    raster_set_shader(p->r, d->mat->shade, d->mat->uniforms, nv);
    raster_set_cull(p->r, d->mat->cull);

    usize mark = arena_mark(p->frame);
    u32 vc = m->vertex_count;
    ClipVert* cv = (ClipVert*)arena_alloc_nozero(p->frame, sizeof(ClipVert) * vc, _Alignof(ClipVert));
    u32* oc = (u32*)arena_alloc_nozero(p->frame, sizeof(u32) * vc, _Alignof(u32));
    Mat3 nm = mat3_normal_from_mat4(&d->model);
    for (u32 i = 0; i < vc; i++) {
        Vec3 wp = mat4_mul_point(&d->model, v3(m->px[i], m->py[i], m->pz[i]));
        Vec4 c = mat4_mul_v4(&p->view_proj, v4_from3(wp, 1.0f));
        Vec3 wn = mat3_mul_v3(&nm, v3(m->nx[i], m->ny[i], m->nz[i]));
        ClipVert* o = &cv[i];
        o->x = c.x; o->y = c.y; o->z = c.z; o->w = c.w;
        o->var[VAR3D_WPOS + 0] = wp.x;
        o->var[VAR3D_WPOS + 1] = wp.y;
        o->var[VAR3D_WPOS + 2] = wp.z;
        o->var[VAR3D_NRM + 0] = wn.x;
        o->var[VAR3D_NRM + 1] = wn.y;
        o->var[VAR3D_NRM + 2] = wn.z;
        o->var[VAR3D_UV + 0] = m->u[i];
        o->var[VAR3D_UV + 1] = m->v[i];
        oc[i] = outcode(o, p->guard_x, p->guard_y);
    }
    p->stats.verts_transformed += vc;

    static const Vec4 guard_planes_unit[4] = { { 1, 0, 0, 1 }, { -1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, -1, 0, 1 } };
    static const u32 guard_bits[4] = { OC_GL, OC_GR, OC_GB, OC_GT };
    const Vec4 near_plane = v4(0, 0, 1, 0);

    ClipVert buf_a[CLIP_MAX_VERTS], buf_b[CLIP_MAX_VERTS];
    for (u32 t = 0; t + 2 < m->index_count; t += 3) {
        u32 i0 = m->indices[t], i1 = m->indices[t + 1], i2 = m->indices[t + 2];
        if (i0 >= vc || i1 >= vc || i2 >= vc) continue;
        p->stats.tris_in++;
        u32 c_and = oc[i0] & oc[i1] & oc[i2];
        u32 c_or = oc[i0] | oc[i1] | oc[i2];
        if (c_and & OC_FRUSTUM) {
            p->stats.tris_rejected++;
            continue;
        }
        if (!(c_or & (OC_NEAR | OC_GUARD))) {
            RasterVert a, b, c;
            pipe_project(p, &cv[i0], &a, nv);
            pipe_project(p, &cv[i1], &b, nv);
            pipe_project(p, &cv[i2], &c, nv);
            raster_triangle(p->r, &a, &b, &c);
            p->stats.tris_emitted++;
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
            p->stats.tris_near_clipped++;
        }
        if (c_or & OC_GUARD) {
            p->stats.tris_guard_clipped++;
            for (u32 g = 0; g < 4 && n >= 3; g++) {
                if (!(c_or & guard_bits[g])) continue;
                Vec4 gp = guard_planes_unit[g];
                f32 gk = g < 2 ? p->guard_x : p->guard_y;
                n = pipe_clip_polygon(src, n, dst, v4(gp.x, gp.y, gp.z, gk), nv);
                ClipVert* tmp = src; src = dst; dst = tmp;
            }
        }
        emit_polygon(p, src, n, nv);
    }
    arena_reset(p->frame, mark);
}
