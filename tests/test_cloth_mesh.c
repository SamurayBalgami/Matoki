/* tests/test_cloth_mesh.c — kumaş çizim yüzeyi (D58, gfx3d/cloth_mesh).
 * Catmull-Rom yüzeyi her parçacıktan geçer; düz ızgara düz ve doğrusal kalır (kenar yansıtması uçta sarkıtmaz); normal birim
 * ve fiziğin üçgen sarımıyla aynı yönde, arka kopya ters; kıvrımlı ızgarada komşu normaller arası açı parçacık ağınınkinden küçük. */
#include "test.h"
#include "core/math.h"
#include "core/mem.h"
#include "gfx3d/cloth_mesh.h"

#define NU 7u
#define NV 5u

static Vec3 vtx(const Mesh* m, u32 i) { return v3(m->px[i], m->py[i], m->pz[i]); }
static Vec3 nrm(const Mesh* m, u32 i) { return v3(m->nx[i], m->ny[i], m->nz[i]); }

TEST(cloth_mesh_flat_grid_interpolates) {
    Vec3 x[NU * NV];
    for (u32 j = 0; j < NV; j++) {
        for (u32 i = 0; i < NU; i++) x[j * NU + i] = v3(0.5f * (f32)i, 2.0f, -0.25f * (f32)j);
    }
    ClothMesh* c = cloth_mesh_alloc(test_arena(), NU, NV, 3, 3.0f, 1.0f);
    cloth_mesh_update(c, x);
    const Mesh* m = c->mesh;
    u32 n = c->mu * c->mv;
    CHECK(c->mu == 19 && c->mv == 13 && m->vertex_count == 2 * n);
    u32 bad = 0;
    for (u32 b = 0; b < c->mv; b++) {
        for (u32 a = 0; a < c->mu; a++) {
            u32 k = b * c->mu + a;
            Vec3 want = v3(0.5f * (f32)a / 3.0f, 2.0f, -0.25f * (f32)b / 3.0f);
            /* düz ızgarada yüzey doğrusal: alt noktalar eşit aralıklı, normal +y (u × v = x × −z = +y) */
            if (v3_len(v3_sub(vtx(m, k), want)) > 1e-5f) bad++;
            if (v3_len(v3_sub(nrm(m, k), v3(0, 1, 0))) > 1e-5f) bad++;
            if (v3_len(v3_add(nrm(m, k + n), v3(0, 1, 0))) > 1e-5f) bad++;
            if (m->u[k] != m->u[k + n] || m->v[k] != m->v[k + n]) bad++;
        }
    }
    CHECK(bad == 0);
    CHECK_NEAR(m->u[c->mu - 1], 3.0f, 1e-6f);
    CHECK_NEAR(m->v[n - 1], 1.0f, 1e-6f);
    /* ön yüz üçgenleri normal yönünde sarılı, arka yüzünkiler ters */
    u32 wrong = 0, half = m->index_count / 2;
    for (u32 t = 0; t < m->index_count; t += 3) {
        Vec3 p0 = vtx(m, m->indices[t]), p1 = vtx(m, m->indices[t + 1]), p2 = vtx(m, m->indices[t + 2]);
        f32 s = v3_dot(v3_cross(v3_sub(p1, p0), v3_sub(p2, p0)), nrm(m, m->indices[t]));
        if (s <= 0.0f) wrong++;
        if ((t < half) != (m->indices[t] < n)) wrong++;
    }
    CHECK(wrong == 0);
}

TEST(cloth_mesh_passes_particles_and_smooths_normals) {
    Vec3 x[NU * NV];
    for (u32 j = 0; j < NV; j++) {
        for (u32 i = 0; i < NU; i++) {
            /* kıvrımlı kumaş: u boyunca dalga */
            x[j * NU + i] = v3(0.5f * (f32)i, 0.3f * m_sinf(1.3f * (f32)i) + 0.05f * (f32)j, -0.25f * (f32)j);
        }
    }
    ClothMesh* c = cloth_mesh_alloc(test_arena(), NU, NV, 4, 3.0f, 1.0f);
    cloth_mesh_update(c, x);
    const Mesh* m = c->mesh;
    u32 miss = 0, unit = 0;
    for (u32 j = 0; j < NV; j++) {
        for (u32 i = 0; i < NU; i++) {
            if (v3_len(v3_sub(vtx(m, (j * 4) * c->mu + i * 4), x[j * NU + i])) > 1e-5f) miss++;
        }
    }
    for (u32 k = 0; k < m->vertex_count; k++) unit += m_absf(v3_len(nrm(m, k)) - 1.0f) > 1e-4f;
    CHECK(miss == 0);
    CHECK(unit == 0);
    /* komşu çizim köşeleri arasında normalin en büyük dönüşü, parçacık ağının yüz normalleri arasındakinin altında */
    f32 fine = 0.0f, coarse = 0.0f;
    for (u32 a = 0; a + 1 < c->mu; a++) {
        u32 k = 2 * 4 * c->mu + a;
        fine = m_maxf(fine, 1.0f - v3_dot(nrm(m, k), nrm(m, k + 1)));
    }
    for (u32 i = 0; i + 2 < NU; i++) {
        Vec3 d0 = v3_sub(x[2 * NU + i + 1], x[2 * NU + i]), d1 = v3_sub(x[2 * NU + i + 2], x[2 * NU + i + 1]);
        Vec3 dv = v3(0, 0, -1);
        Vec3 n0 = v3_norm(v3_cross(d0, dv)), n1 = v3_norm(v3_cross(d1, dv));
        coarse = m_maxf(coarse, 1.0f - v3_dot(n0, n1));
    }
    CHECK(fine < coarse * 0.25f);
    test_note("en büyük komşu normal sapması (1 − cos): çizim yüzeyi", fine, 5);
    test_note("en büyük komşu normal sapması (1 − cos): parçacık ağı", coarse, 5);
}
