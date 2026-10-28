/* gfx3d/mesh.c — mesh ayırma, sınır hesabı ve ilkel üreteçler.
 * Sarım: dışarıdan bakıldığında saat yönünün tersi (CCW) = ön yüz.
 * Küre sınırı AABB merkezli (en küçük küre değil; culling için yeterince sıkı). */
#include "gfx3d/mesh.h"

Mesh* mesh_alloc(Arena* a, u32 vc, u32 ic) {
    Mesh* m = ARENA_NEW(a, Mesh);
    m->px = ARENA_ARRAY(a, f32, vc);
    m->py = ARENA_ARRAY(a, f32, vc);
    m->pz = ARENA_ARRAY(a, f32, vc);
    m->nx = ARENA_ARRAY(a, f32, vc);
    m->ny = ARENA_ARRAY(a, f32, vc);
    m->nz = ARENA_ARRAY(a, f32, vc);
    m->u = ARENA_ARRAY(a, f32, vc);
    m->v = ARENA_ARRAY(a, f32, vc);
    m->indices = ARENA_ARRAY(a, u32, ic);
    m->vertex_count = vc;
    m->index_count = ic;
    return m;
}

void mesh_compute_bounds(Mesh* m) {
    if (!m->vertex_count) return;
    Vec3 mn = v3(m->px[0], m->py[0], m->pz[0]), mx = mn;
    for (u32 i = 1; i < m->vertex_count; i++) {
        mn.x = m_minf(mn.x, m->px[i]); mx.x = m_maxf(mx.x, m->px[i]);
        mn.y = m_minf(mn.y, m->py[i]); mx.y = m_maxf(mx.y, m->py[i]);
        mn.z = m_minf(mn.z, m->pz[i]); mx.z = m_maxf(mx.z, m->pz[i]);
    }
    m->aabb_min = mn;
    m->aabb_max = mx;
    Vec3 c = v3_scale(v3_add(mn, mx), 0.5f);
    f32 r2 = 0.0f;
    for (u32 i = 0; i < m->vertex_count; i++) {
        Vec3 d = v3_sub(v3(m->px[i], m->py[i], m->pz[i]), c);
        r2 = m_maxf(r2, v3_dot(d, d));
    }
    m->sphere_center = c;
    m->sphere_radius = m_sqrtf(r2);
}

static void set_vert(Mesh* m, u32 i, Vec3 p, Vec3 n, f32 u, f32 v) {
    m->px[i] = p.x; m->py[i] = p.y; m->pz[i] = p.z;
    m->nx[i] = n.x; m->ny[i] = n.y; m->nz[i] = n.z;
    m->u[i] = u; m->v[i] = v;
}

Mesh* mesh_cube(Arena* a) {
    Mesh* m = mesh_alloc(a, 24, 36);
    /* her yüz: normal n, yüzeyde sağ (s) ve yukarı (t) eksenleri; s × t = n (CCW) */
    static const f32 faces[6][9] = {
        {  1, 0, 0,   0, 0, -1,   0, 1, 0 },
        { -1, 0, 0,   0, 0, 1,    0, 1, 0 },
        {  0, 1, 0,   1, 0, 0,    0, 0, -1 },
        {  0, -1, 0,  1, 0, 0,    0, 0, 1 },
        {  0, 0, 1,   1, 0, 0,    0, 1, 0 },
        {  0, 0, -1,  -1, 0, 0,   0, 1, 0 },
    };
    for (u32 f = 0; f < 6; f++) {
        Vec3 n = v3(faces[f][0], faces[f][1], faces[f][2]);
        Vec3 s = v3(faces[f][3], faces[f][4], faces[f][5]);
        Vec3 t = v3(faces[f][6], faces[f][7], faces[f][8]);
        Vec3 c = v3_scale(n, 0.5f);
        u32 b = f * 4;
        set_vert(m, b + 0, v3_add(c, v3_add(v3_scale(s, -0.5f), v3_scale(t, -0.5f))), n, 0, 1);
        set_vert(m, b + 1, v3_add(c, v3_add(v3_scale(s, 0.5f), v3_scale(t, -0.5f))), n, 1, 1);
        set_vert(m, b + 2, v3_add(c, v3_add(v3_scale(s, 0.5f), v3_scale(t, 0.5f))), n, 1, 0);
        set_vert(m, b + 3, v3_add(c, v3_add(v3_scale(s, -0.5f), v3_scale(t, 0.5f))), n, 0, 0);
        u32* ix = m->indices + f * 6;
        ix[0] = b; ix[1] = b + 1; ix[2] = b + 2;
        ix[3] = b; ix[4] = b + 2; ix[5] = b + 3;
    }
    mesh_compute_bounds(m);
    return m;
}

Mesh* mesh_sphere(Arena* a, u32 lat, u32 lon) {
    if (lat < 2) lat = 2;
    if (lon < 3) lon = 3;
    u32 cols = lon + 1;
    Mesh* m = mesh_alloc(a, (lat + 1) * cols, lat * lon * 6);
    for (u32 i = 0; i <= lat; i++) {
        f32 th = M_PI_F * (f32)i / (f32)lat;
        f32 st = m_sinf(th), ct = m_cosf(th);
        for (u32 j = 0; j <= lon; j++) {
            f32 ph = M_TAU_F * (f32)j / (f32)lon;
            Vec3 p = v3(st * m_cosf(ph), ct, st * m_sinf(ph));
            set_vert(m, i * cols + j, p, p, (f32)j / (f32)lon * 2.0f, (f32)i / (f32)lat);
        }
    }
    u32 k = 0;
    for (u32 i = 0; i < lat; i++) {
        for (u32 j = 0; j < lon; j++) {
            u32 a0 = i * cols + j, a1 = a0 + 1, b0 = a0 + cols, b1 = b0 + 1;
            /* dışarıdan bakınca CCW */
            m->indices[k++] = a0; m->indices[k++] = a1; m->indices[k++] = b1;
            m->indices[k++] = a0; m->indices[k++] = b1; m->indices[k++] = b0;
        }
    }
    mesh_compute_bounds(m);
    return m;
}

Mesh* mesh_cylinder(Arena* a, u32 seg) {
    if (seg < 3) seg = 3;
    /* yan yüz: 2*(seg+1) köşe; kapaklar: her biri merkez + seg+1 köşe */
    u32 side_v = 2 * (seg + 1), cap_v = seg + 2;
    Mesh* m = mesh_alloc(a, side_v + 2 * cap_v, seg * 6 + 2 * seg * 3);
    for (u32 j = 0; j <= seg; j++) {
        f32 ph = M_TAU_F * (f32)j / (f32)seg;
        f32 cy = m_cosf(ph), sz = m_sinf(ph);
        Vec3 n = v3(0, cy, sz);
        set_vert(m, j * 2, v3(-0.5f, cy, sz), n, (f32)j / (f32)seg * 4.0f, 0.0f);
        set_vert(m, j * 2 + 1, v3(0.5f, cy, sz), n, (f32)j / (f32)seg * 4.0f, 0.25f);
    }
    u32 k = 0;
    for (u32 j = 0; j < seg; j++) {
        u32 a0 = j * 2, a1 = a0 + 1, b0 = a0 + 2, b1 = a0 + 3;
        /* dışarıdan CCW: (x-, φ) → (x-, φ+) → (x+, φ+) */
        m->indices[k++] = a0; m->indices[k++] = b0; m->indices[k++] = b1;
        m->indices[k++] = a0; m->indices[k++] = b1; m->indices[k++] = a1;
    }
    for (u32 c = 0; c < 2; c++) {
        f32 x = c ? 0.5f : -0.5f;
        Vec3 n = v3(c ? 1.0f : -1.0f, 0, 0);
        u32 base = side_v + c * cap_v;
        set_vert(m, base, v3(x, 0, 0), n, 0.5f, 0.5f);
        for (u32 j = 0; j <= seg; j++) {
            f32 ph = M_TAU_F * (f32)j / (f32)seg;
            set_vert(m, base + 1 + j, v3(x, m_cosf(ph), m_sinf(ph)), n, 0.5f + 0.5f * m_cosf(ph), 0.5f + 0.5f * m_sinf(ph));
        }
        for (u32 j = 0; j < seg; j++) {
            m->indices[k++] = base;
            if (c) { m->indices[k++] = base + 1 + j; m->indices[k++] = base + 2 + j; }
            else { m->indices[k++] = base + 2 + j; m->indices[k++] = base + 1 + j; }
        }
    }
    mesh_compute_bounds(m);
    return m;
}

Mesh* mesh_plane(Arena* a, f32 sx, f32 sz, f32 uv_per_unit, u32 segments) {
    if (segments < 1) segments = 1;
    u32 n = segments + 1;
    Mesh* m = mesh_alloc(a, n * n, segments * segments * 6);
    for (u32 j = 0; j < n; j++) {
        for (u32 i = 0; i < n; i++) {
            f32 x = -sx * 0.5f + sx * (f32)i / (f32)segments;
            f32 z = -sz * 0.5f + sz * (f32)j / (f32)segments;
            set_vert(m, j * n + i, v3(x, 0, z), v3(0, 1, 0), x * uv_per_unit, z * uv_per_unit);
        }
    }
    u32 k = 0;
    for (u32 j = 0; j < segments; j++) {
        for (u32 i = 0; i < segments; i++) {
            u32 a0 = j * n + i, a1 = a0 + 1, b0 = a0 + n, b1 = b0 + 1;
            /* +y'den bakınca CCW: (x,z) düzleminde z ekseni ekrana doğru */
            m->indices[k++] = a0; m->indices[k++] = b0; m->indices[k++] = b1;
            m->indices[k++] = a0; m->indices[k++] = b1; m->indices[k++] = a1;
        }
    }
    mesh_compute_bounds(m);
    return m;
}
