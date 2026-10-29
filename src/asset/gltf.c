/* asset/gltf.c — glTF/GLB ayrıştırma: JSON DOM → tamponlar → accessor okuma → skin/animasyon/node ağacı.
 * Her accessor erişimi bufferView ve buffer sınırlarına karşı u64 aritmetiğiyle denetlenir
 * (bozuk dosya → hata/uyarı, asla sınır dışı okuma veya arena PANIC).
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/asset/gltf.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "asset/gltf.h"
#include "asset/json.h"
#include "asset/base64.h"
#include "img/png.h"
#include "img/image.h"
#include "img/filter.h"
#include "core/fmt.h"
#include "core/sys.h"

#define GLTF_MAX_NODES  4096u
#define GLTF_MAX_SKINS  64u
#define GLTF_MAX_ANIMS  128u
#define GLTF_MAX_VISITS 8192u

typedef struct { const u8* data; u32 len; } GBuf;

typedef struct {
    const JsonValue* root;
    GBuf*  bufs;
    u32    buf_count;
    Arena* out;
    Arena* scratch;
    GltfModel* m;
    GltfResolveFn resolve;
    void*  user;
    u32    node_count;
    u8*    node_visiting;
    u32    visit_steps;
    /* çizim toplama (bağlı liste → dizi) */
    struct DrawNode { GltfDraw d; struct DrawNode* next; } *head, *tail;
    u32    draws;
} G;

static b32 arena_has_space(const Arena* a, usize bytes) {
    if (!a || bytes > a->cap) return FALSE;
    usize aligned = ALIGN_UP(a->used, 16u);
    if (aligned > a->cap || bytes > a->cap - aligned) return FALSE;
    return TRUE;
}

static void g_err(GltfModel* m, const char* msg) {
    if (m->err[0]) return;
    u32 i = 0;
    while (msg[i] && i + 1 < sizeof(m->err)) { m->err[i] = msg[i]; i++; }
    m->err[i] = 0;
}

static void g_warn(GltfModel* m, const char* msg) {
    m->warnings++;
    u32 n = 0;
    while (m->warn[n]) n++;
    if (n + 4 >= sizeof(m->warn)) return;
    if (n) { m->warn[n++] = ';'; m->warn[n++] = ' '; }
    for (u32 i = 0; msg[i] && n + 1 < sizeof(m->warn); i++) m->warn[n++] = msg[i];
    m->warn[n] = 0;
}

static b32 str_starts(const char* s, const char* p) {
    while (*p) { if (*s++ != *p++) return FALSE; }
    return TRUE;
}

static b32 str_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* "data:...;base64,XXXX" → baytlar (scratch) */
static b32 decode_data_uri(G* g, const char* uri, GBuf* out) {
    const char* comma = uri;
    while (*comma && *comma != ',') comma++;
    if (!*comma) return FALSE;
    const char* b64 = comma + 1;
    b32 is_b64 = FALSE;
    for (const char* p = uri; p + 7 <= comma; p++) {
        if (str_starts(p, ";base64")) is_b64 = TRUE;
    }
    if (!is_b64) return FALSE;
    u32 tl = 0;
    while (b64[tl]) tl++;
    u32 cap = base64_max_decoded(tl);
    usize alloc_sz = cap ? (usize)cap : 1u;
    if (!arena_has_space(g->scratch, alloc_sz)) return FALSE;
    u8* buf = (u8*)arena_alloc(g->scratch, alloc_sz, 16);
    u32 n = 0;
    if (!base64_decode(b64, tl, buf, cap, &n)) return FALSE;
    out->data = buf;
    out->len = n;
    return TRUE;
}

static b32 load_uri(G* g, const char* uri, GBuf* out) {
    if (str_starts(uri, "data:")) return decode_data_uri(g, uri, out);
    if (g->resolve) {
        u32 len = 0;
        const u8* d = g->resolve(g->user, uri, &len);
        if (d) { out->data = d; out->len = len; return TRUE; }
    }
    return FALSE;
}

/* bufferView → (ptr, len, stride) */
static b32 view_slice(G* g, i64 vi, const u8** ptr, u32* len, u32* stride) {
    const JsonValue* v = json_at(json_get(g->root, "bufferViews"), (u32)vi);
    if (!v || vi < 0) return FALSE;
    i64 bi = json_int(json_get(v, "buffer"), -1);
    if (bi < 0 || (u64)bi >= g->buf_count || !g->bufs[bi].data) return FALSE;
    i64 off = json_int(json_get(v, "byteOffset"), 0);
    i64 bl  = json_int(json_get(v, "byteLength"), -1);
    i64 st  = json_int(json_get(v, "byteStride"), 0);
    if (off < 0 || bl < 0 || st < 0 || st > 255 || (u64)off + (u64)bl > g->bufs[bi].len) return FALSE;
    *ptr = g->bufs[bi].data + off;
    *len = (u32)bl;
    *stride = (u32)st;
    return TRUE;
}

static u32 type_comps(const char* t) {
    if (str_eq(t, "SCALAR")) return 1;
    if (str_eq(t, "VEC2")) return 2;
    if (str_eq(t, "VEC3")) return 3;
    if (str_eq(t, "VEC4")) return 4;
    if (str_eq(t, "MAT4")) return 16;
    return 0;
}

static u32 ctype_size(i64 ct) {
    switch (ct) {
    case 5120: case 5121: return 1;
    case 5122: case 5123: return 2;
    case 5125: case 5126: return 4;
    default: return 0;
    }
}

static f32 read_comp(const u8* p, i64 ct, b32 norm) {
    switch (ct) {
    case 5126: { f32 f; __builtin_memcpy(&f, p, 4); return f; }
    case 5121: return norm ? (f32)p[0] / 255.0f : (f32)p[0];
    case 5120: { i8 v = (i8)p[0]; return norm ? m_maxf((f32)v / 127.0f, -1.0f) : (f32)v; }
    case 5123: { u16 v; __builtin_memcpy(&v, p, 2); return norm ? (f32)v / 65535.0f : (f32)v; }
    case 5122: { i16 v; __builtin_memcpy(&v, p, 2); return norm ? m_maxf((f32)v / 32767.0f, -1.0f) : (f32)v; }
    case 5125: { u32 v; __builtin_memcpy(&v, p, 4); return (f32)v; }
    default: return 0.0f;
    }
}

/* accessor → count × comps f32 (comps beklenen bileşen sayısı; fazlası atılır) */
static f32* read_accessor_f32(G* g, i64 ai, u32 want, u32* out_count) {
    const JsonValue* a = json_at(json_get(g->root, "accessors"), (u32)ai);
    if (!a || ai < 0) return NULL;
    if (json_get(a, "sparse")) g_warn(g->m, "sparse accessor yok sayildi");
    u32 comps = type_comps(json_str(json_get(a, "type"), ""));
    i64 ct = json_int(json_get(a, "componentType"), 0);
    u32 csz = ctype_size(ct);
    i64 count = json_int(json_get(a, "count"), -1);
    b32 norm = json_bool(json_get(a, "normalized"), FALSE);
    if (comps < want || csz == 0 || count <= 0 || count > (i64)SKIN_MAX_VERTICES) return NULL;
    i64 vi = json_int(json_get(a, "bufferView"), -1);
    const u8* base; u32 vlen, stride;
    if (!view_slice(g, vi, &base, &vlen, &stride)) return NULL;
    i64 aoff = json_int(json_get(a, "byteOffset"), 0);
    u32 esz = comps * csz;
    if (stride == 0) stride = esz;
    if (aoff < 0 || stride < esz) return NULL;
    if ((u64)aoff + (u64)(count - 1) * stride + esz > vlen) return NULL;
    usize bytes = (usize)count * (usize)want * sizeof(f32);
    if (!arena_has_space(g->scratch, bytes)) return NULL;
    f32* out = ARENA_ARRAY(g->scratch, f32, (usize)count * want);
    for (i64 i = 0; i < count; i++) {
        const u8* e = base + aoff + (u64)i * stride;
        for (u32 c = 0; c < want; c++) out[i * want + c] = read_comp(e + c * csz, ct, norm);
    }
    *out_count = (u32)count;
    return out;
}

/* JOINTS_0 accessor → count × 4 u16 (UNSIGNED_BYTE 5121 veya UNSIGNED_SHORT 5123) */
static u16* read_joints_u16(G* g, i64 ai, u32* out_count) {
    const JsonValue* a = json_at(json_get(g->root, "accessors"), (u32)ai);
    if (!a || ai < 0) return NULL;
    if (json_get(a, "sparse")) g_warn(g->m, "sparse accessor yok sayildi");
    u32 comps = type_comps(json_str(json_get(a, "type"), ""));
    i64 ct = json_int(json_get(a, "componentType"), 0);
    if (comps != 4u || (ct != 5121 && ct != 5123 && ct != 5125)) return NULL;
    u32 csz = ctype_size(ct);
    i64 count = json_int(json_get(a, "count"), -1);
    if (count <= 0 || count > (i64)SKIN_MAX_VERTICES) return NULL;
    const u8* base; u32 vlen, stride;
    if (!view_slice(g, json_int(json_get(a, "bufferView"), -1), &base, &vlen, &stride)) return NULL;
    i64 aoff = json_int(json_get(a, "byteOffset"), 0);
    u32 esz = 4u * csz;
    if (stride == 0) stride = esz;
    if (aoff < 0 || stride < esz) return NULL;
    if ((u64)aoff + (u64)(count - 1) * stride + esz > vlen) return NULL;
    usize bytes = (usize)count * 4u * sizeof(u16);
    if (!arena_has_space(g->scratch, bytes)) return NULL;
    u16* out = ARENA_ARRAY(g->scratch, u16, (usize)count * 4u);
    for (i64 i = 0; i < count; i++) {
        const u8* e = base + aoff + (u64)i * stride;
        for (u32 c = 0; c < 4u; c++) {
            const u8* p = e + c * csz;
            u32 v = (ct == 5121) ? (u32)p[0]
                  : (ct == 5123) ? (u32)(p[0] | ((u32)p[1] << 8))
                  : (u32)(p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24));
            out[i * 4 + c] = (v <= 0xFFFFu) ? (u16)v : 0xFFFFu;
        }
    }
    *out_count = (u32)count;
    return out;
}

static u32* read_indices(G* g, i64 ai, u32* out_count, u32 vertex_count) {
    const JsonValue* a = json_at(json_get(g->root, "accessors"), (u32)ai);
    if (!a || ai < 0) return NULL;
    i64 ct = json_int(json_get(a, "componentType"), 0);
    if (ct != 5121 && ct != 5123 && ct != 5125) return NULL;
    u32 csz = ctype_size(ct);
    i64 count = json_int(json_get(a, "count"), -1);
    if (count <= 0 || count > (1 << 22)) return NULL;
    const u8* base; u32 vlen, stride;
    if (!view_slice(g, json_int(json_get(a, "bufferView"), -1), &base, &vlen, &stride)) return NULL;
    i64 aoff = json_int(json_get(a, "byteOffset"), 0);
    if (stride == 0) stride = csz;
    if (aoff < 0 || stride < csz || (u64)aoff + (u64)(count - 1) * stride + csz > vlen) return NULL;
    usize bytes = (usize)count * sizeof(u32);
    if (!arena_has_space(g->scratch, bytes)) return NULL;
    u32* out = ARENA_ARRAY(g->scratch, u32, (usize)count);
    for (i64 i = 0; i < count; i++) {
        const u8* e = base + aoff + (u64)i * stride;
        u32 v = (ct == 5121) ? e[0] : (ct == 5123) ? (u32)(e[0] | (e[1] << 8)) :
                (u32)(e[0] | (e[1] << 8) | (e[2] << 16) | ((u32)e[3] << 24));
        if (v >= vertex_count) return NULL;
        out[i] = v;
    }
    *out_count = (u32)count;
    return out;
}

Mat4 mat4_from_quat(f32 x, f32 y, f32 z, f32 w) {
    Quat q = quat_normalize(quat(x, y, z, w));
    x = q.x; y = q.y; z = q.z; w = q.w;
    Mat4 r = mat4_identity();
    r.m[0]  = 1.0f - 2.0f * (y * y + z * z);
    r.m[1]  = 2.0f * (x * y + z * w);
    r.m[2]  = 2.0f * (x * z - y * w);
    r.m[4]  = 2.0f * (x * y - z * w);
    r.m[5]  = 1.0f - 2.0f * (x * x + z * z);
    r.m[6]  = 2.0f * (y * z + x * w);
    r.m[8]  = 2.0f * (x * z + y * w);
    r.m[9]  = 2.0f * (y * z - x * w);
    r.m[10] = 1.0f - 2.0f * (x * x + y * y);
    return r;
}

static Mat4 node_local(G* g, const JsonValue* n) {
    const JsonValue* mat = json_get(n, "matrix");
    if (json_len(mat) == 16) {
        Mat4 r;
        for (u32 i = 0; i < 16; i++) {
            f32 v = (f32)json_num(json_at(mat, i), 0.0);
            if (!anim_f32_finite(v)) { g_warn(g->m, "node matrisinde NaN"); return mat4_identity(); }
            r.m[i] = v;
        }
        return r;
    }
    const JsonValue* t = json_get(n, "translation");
    const JsonValue* q = json_get(n, "rotation");
    const JsonValue* s = json_get(n, "scale");
    Mat4 T = mat4_identity(), R = mat4_identity(), S = mat4_identity();
    if (json_len(t) == 3) {
        f32 tx = (f32)json_num(json_at(t, 0), 0), ty = (f32)json_num(json_at(t, 1), 0), tz = (f32)json_num(json_at(t, 2), 0);
        if (!anim_f32_finite(tx) || !anim_f32_finite(ty) || !anim_f32_finite(tz)) {
            g_warn(g->m, "node translation NaN");
        } else {
            T.m[12] = tx; T.m[13] = ty; T.m[14] = tz;
        }
    }
    if (json_len(q) == 4) {
        f32 qx = (f32)json_num(json_at(q, 0), 0), qy = (f32)json_num(json_at(q, 1), 0),
            qz = (f32)json_num(json_at(q, 2), 0), qw = (f32)json_num(json_at(q, 3), 1);
        if (!anim_f32_finite(qx) || !anim_f32_finite(qy) || !anim_f32_finite(qz) || !anim_f32_finite(qw)) {
            g_warn(g->m, "node rotation NaN");
        } else {
            R = mat4_from_quat(qx, qy, qz, qw);
        }
    }
    if (json_len(s) == 3) {
        f32 sx = (f32)json_num(json_at(s, 0), 1), sy = (f32)json_num(json_at(s, 1), 1), sz = (f32)json_num(json_at(s, 2), 1);
        if (!anim_f32_finite(sx) || !anim_f32_finite(sy) || !anim_f32_finite(sz)) {
            g_warn(g->m, "node scale NaN");
        } else {
            S.m[0] = sx; S.m[5] = sy; S.m[10] = sz;
        }
    }
    return mat4_mul(T, mat4_mul(R, S));
}

static JointPose node_local_pose(G* g, const JsonValue* n) {
    if (!n) return joint_pose_identity();
    const JsonValue* mat = json_get(n, "matrix");
    if (json_len(mat) == 16) {
        return joint_pose_from_mat4(node_local(g, n));
    }
    JointPose p = joint_pose_identity();
    const JsonValue* t = json_get(n, "translation");
    const JsonValue* q = json_get(n, "rotation");
    const JsonValue* s = json_get(n, "scale");
    if (json_len(t) == 3) {
        f32 tx = (f32)json_num(json_at(t, 0), 0), ty = (f32)json_num(json_at(t, 1), 0), tz = (f32)json_num(json_at(t, 2), 0);
        if (!anim_f32_finite(tx) || !anim_f32_finite(ty) || !anim_f32_finite(tz)) {
            g_warn(g->m, "eklem translation NaN");
        } else {
            p.translation = v3(tx, ty, tz);
        }
    }
    if (json_len(q) == 4) {
        f32 qx = (f32)json_num(json_at(q, 0), 0), qy = (f32)json_num(json_at(q, 1), 0),
            qz = (f32)json_num(json_at(q, 2), 0), qw = (f32)json_num(json_at(q, 3), 1);
        f32 l2 = qx * qx + qy * qy + qz * qz + qw * qw;
        if (!anim_f32_finite(l2) || l2 <= 1e-20f) {
            g_warn(g->m, "eklem rotation NaN veya sifir");
        } else {
            p.rotation = quat_normalize(quat(qx, qy, qz, qw));
        }
    }
    if (json_len(s) == 3) {
        f32 sx = (f32)json_num(json_at(s, 0), 1), sy = (f32)json_num(json_at(s, 1), 1), sz = (f32)json_num(json_at(s, 2), 1);
        if (!anim_f32_finite(sx) || !anim_f32_finite(sy) || !anim_f32_finite(sz)) {
            g_warn(g->m, "eklem scale NaN");
        } else {
            p.scale = v3(sx, sy, sz);
        }
    }
    return p;
}

static Mesh* build_primitive(G* g, const JsonValue* prim, i32 skin_idx, SkinnedMesh** out_skinned) {
    *out_skinned = NULL;
    const JsonValue* attrs = json_get(prim, "attributes");
    i64 mode = json_int(json_get(prim, "mode"), 4);
    if (mode != 4) { g_warn(g->m, "ucgen olmayan primitive atlandi"); return NULL; }
    if (json_get(prim, "targets")) g_warn(g->m, "morph yok sayildi");
    u32 vn = 0, nn = 0, tn = 0;
    f32* pos = read_accessor_f32(g, json_int(json_get(attrs, "POSITION"), -1), 3, &vn);
    if (!pos) { g_err(g->m, "POSITION okunamadi"); return NULL; }
    f32* nrm = NULL;
    f32* uv  = NULL;
    if (json_get(attrs, "NORMAL")) {
        nrm = read_accessor_f32(g, json_int(json_get(attrs, "NORMAL"), -1), 3, &nn);
        if (!nrm || nn != vn) { g_err(g->m, "NORMAL okunamadi"); return NULL; }
    }
    if (json_get(attrs, "TEXCOORD_0")) {
        uv = read_accessor_f32(g, json_int(json_get(attrs, "TEXCOORD_0"), -1), 2, &tn);
        if (!uv || tn != vn) { g_err(g->m, "TEXCOORD_0 okunamadi"); return NULL; }
    }
    u32 icount = 0;
    u32* idx = NULL;
    if (json_get(prim, "indices")) {
        idx = read_indices(g, json_int(json_get(prim, "indices"), -1), &icount, vn);
        if (!idx) { g_err(g->m, "indices okunamadi veya aralik disi"); return NULL; }
    } else {
        icount = vn;
    }
    icount -= icount % 3u;
    if (icount == 0) return NULL;

    usize mesh_bytes = sizeof(Mesh) + (usize)vn * 8u * sizeof(f32) + (usize)icount * sizeof(u32) + 256u;
    if (!arena_has_space(g->out, mesh_bytes)) { g_err(g->m, "mesh icin arena yetersiz"); return NULL; }

    Mesh* mesh = mesh_alloc(g->out, vn, icount);
    b32 pos_nan = FALSE, nrm_nan = FALSE;
    for (u32 i = 0; i < vn; i++) {
        f32 px = pos[i * 3], py = pos[i * 3 + 1], pz = pos[i * 3 + 2];
        if (!anim_f32_finite(px) || !anim_f32_finite(py) || !anim_f32_finite(pz)) {
            pos_nan = TRUE; px = 0.0f; py = 0.0f; pz = 0.0f;
        }
        mesh->px[i] = px; mesh->py[i] = py; mesh->pz[i] = pz;
        f32 u0 = uv ? uv[i * 2] : 0.0f;
        f32 v0 = uv ? uv[i * 2 + 1] : 0.0f;
        mesh->u[i] = anim_f32_finite(u0) ? u0 : 0.0f;
        mesh->v[i] = anim_f32_finite(v0) ? v0 : 0.0f;
        if (nrm) {
            f32 nx = nrm[i * 3], ny = nrm[i * 3 + 1], nz = nrm[i * 3 + 2];
            if (!anim_f32_finite(nx) || !anim_f32_finite(ny) || !anim_f32_finite(nz)) {
                nrm_nan = TRUE; nx = 0.0f; ny = 1.0f; nz = 0.0f;
            }
            mesh->nx[i] = nx; mesh->ny[i] = ny; mesh->nz[i] = nz;
        }
    }
    if (pos_nan) g_warn(g->m, "POSITION icinde NaN");
    if (nrm_nan) g_warn(g->m, "NORMAL icinde NaN");

    for (u32 i = 0; i < icount; i++) mesh->indices[i] = idx ? idx[i] : i;
    if (!nrm) mesh_compute_normals(mesh);
    mesh_compute_bounds(mesh);

    if (json_get(attrs, "JOINTS_0") || json_get(attrs, "WEIGHTS_0")) {
        u32 jn = 0, wn = 0;
        u16* jts = read_joints_u16(g, json_int(json_get(attrs, "JOINTS_0"), -1), &jn);
        f32* wts = read_accessor_f32(g, json_int(json_get(attrs, "WEIGHTS_0"), -1), 4, &wn);
        if (!jts || !wts || jn != vn || wn != vn) {
            g_err(g->m, "JOINTS_0 veya WEIGHTS_0 okunamadi");
            return NULL;
        }
        u32 jc = 1u;
        if (skin_idx >= 0 && (u32)skin_idx < g->m->skin_count && g->m->skins[skin_idx].joint_count > 0u) {
            jc = g->m->skins[skin_idx].joint_count;
        } else {
            g_warn(g->m, "JOINTS_0 var ama gecerli skin yok");
        }

        b32 bad_joint = FALSE, bad_nan = FALSE, bad_zero = FALSE;
        for (u32 i = 0; i < vn; i++) {
            f32 sum = 0.0f;
            for (u32 c = 0; c < 4u; c++) {
                u16 jv = jts[i * 4 + c];
                f32 wv = wts[i * 4 + c];
                if (jv >= jc) bad_joint = TRUE;
                if (!anim_f32_finite(wv) || wv < 0.0f) bad_nan = TRUE;
                else sum += wv;
            }
            if (!anim_f32_finite(sum) || sum <= 1e-8f) bad_zero = TRUE;
        }
        if (bad_joint) g_warn(g->m, "eklem indeksi >= eklem sayisi");
        if (bad_nan)   g_warn(g->m, "agirlik NaN veya negatif");
        if (bad_zero)  g_warn(g->m, "agirlik toplami 0");

        usize skin_bytes = mesh_bytes + sizeof(SkinnedMesh) + (usize)vn * 4u * (sizeof(u16) + sizeof(f32)) + 128u;
        if (!arena_has_space(g->out, skin_bytes)) {
            g_err(g->m, "skin verisi icin arena yetersiz");
            return NULL;
        }
        Mesh* bind_mesh = skin_clone_mesh(g->out, mesh);
        SkinnedMesh* sm = ARENA_NEW(g->out, SkinnedMesh);
        sm->bind_mesh    = bind_mesh;
        sm->vertex_count = vn;
        sm->joints       = ARENA_ARRAY(g->out, u16, (usize)vn * 4u);
        sm->weights      = ARENA_ARRAY(g->out, f32, (usize)vn * 4u);
        __builtin_memcpy(sm->joints, jts, (usize)vn * 4u * sizeof(u16));
        __builtin_memcpy(sm->weights, wts, (usize)vn * 4u * sizeof(f32));
        skin_sanitize_weights(sm->joints, sm->weights, vn, jc);
        *out_skinned = sm;
    }

    return mesh;
}

static void load_skins_and_anims(G* g) {
    const JsonValue* nodes = json_get(g->root, "nodes");
    u32 nc = json_len(nodes);
    if (nc > GLTF_MAX_NODES) {
        g_warn(g->m, "cok fazla node");
        nc = GLTF_MAX_NODES;
    }
    g->node_count = nc;

    /* Node ebeveyn tablosu ve döngü denetimi */
    i32* node_parent = ARENA_ARRAY(g->scratch, i32, nc + 1u);
    for (u32 i = 0; i <= nc; i++) node_parent[i] = -1;

    for (u32 i = 0; i < nc; i++) {
        const JsonValue* ch = json_get(json_at(nodes, i), "children");
        u32 clen = json_len(ch);
        for (u32 k = 0; k < clen; k++) {
            i64 c = json_int(json_at(ch, k), -1);
            if (c < 0 || (u64)c >= nc) {
                g_warn(g->m, "gecersiz child node indeksi");
                continue;
            }
            if ((u32)c == i) {
                g_warn(g->m, "dongusel eklem hiyerarsisi");
                continue;
            }
            /* Ata zincirinde c var mı? */
            i32 cur = (i32)i;
            u32 steps = 0;
            b32 cyc = FALSE;
            while (cur >= 0 && (u32)cur < nc && steps <= nc) {
                if (cur == (i32)c) { cyc = TRUE; break; }
                cur = node_parent[cur];
                steps++;
            }
            if (cyc) {
                g_warn(g->m, "dongusel eklem hiyerarsisi");
                continue;
            }
            node_parent[c] = (i32)i;
        }
    }

    /* Skins */
    const JsonValue* skins_json = json_get(g->root, "skins");
    u32 skin_cnt = json_len(skins_json);
    if (skin_cnt > GLTF_MAX_SKINS) {
        g_warn(g->m, "cok fazla skin");
        skin_cnt = GLTF_MAX_SKINS;
    }
    if (skin_cnt > 0u) {
        if (!arena_has_space(g->out, (usize)(skin_cnt + 1u) * sizeof(Skeleton))) {
            g_err(g->m, "skin icin arena yetersiz");
            return;
        }
        g->m->skins = ARENA_ARRAY(g->out, Skeleton, skin_cnt + 1u);
        g->m->skin_count = skin_cnt;

        i32* node_to_joint = ARENA_ARRAY(g->scratch, i32, nc + 1u);
        for (u32 si = 0; si < skin_cnt; si++) {
            const JsonValue* sj = json_at(skins_json, si);
            const JsonValue* j_arr = json_get(sj, "joints");
            u32 jc = json_len(j_arr);
            if (jc == 0u || jc > SKEL_MAX_JOINTS) {
                g_warn(g->m, "gecersiz skin eklem sayisi");
                continue;
            }
            usize sk_bytes = (usize)jc * (sizeof(i32) * 2u + sizeof(Mat4) + sizeof(JointPose)) + 128u;
            if (!arena_has_space(g->out, sk_bytes)) {
                g_err(g->m, "iskelet icin arena yetersiz");
                return;
            }
            Skeleton* skel = &g->m->skins[si];
            skeleton_init(skel, g->out, jc);
            skel->skeleton_node = (i32)json_int(json_get(sj, "skeleton"), -1);

            for (u32 i = 0; i <= nc; i++) node_to_joint[i] = -1;
            for (u32 k = 0; k < jc; k++) {
                i64 ni = json_int(json_at(j_arr, k), -1);
                if (ni < 0 || (u64)ni >= nc) {
                    g_warn(g->m, "eklem node indeksi aralik disi");
                    continue;
                }
                if (node_to_joint[ni] != -1) {
                    g_warn(g->m, "dongusel veya yinelenen eklem dugumu");
                }
                node_to_joint[ni] = (i32)k;
                skel->node_indices[k] = (i32)ni;
                skel->bind_pose[k] = node_local_pose(g, json_at(nodes, (u32)ni));
            }

            for (u32 k = 0; k < jc; k++) {
                i32 ni = skel->node_indices[k];
                if (ni < 0 || (u32)ni >= nc) { skel->parents[k] = -1; continue; }
                i32 p = node_parent[ni];
                u32 steps = 0;
                while (p >= 0 && (u32)p < nc && steps <= nc) {
                    if (node_to_joint[p] >= 0) {
                        skel->parents[k] = node_to_joint[p];
                        break;
                    }
                    p = node_parent[p];
                    steps++;
                }
            }

            /* Kök eklemin üstünde eklem olmayan düğümler varsa root_transform'a biriktir */
            if (skel->node_indices[0] >= 0 && (u32)skel->node_indices[0] < nc && skel->parents[0] == -1) {
                Mat4 rt = mat4_identity();
                i32 p = node_parent[skel->node_indices[0]];
                u32 steps = 0;
                while (p >= 0 && (u32)p < nc && steps <= nc) {
                    rt = mat4_mul(node_local(g, json_at(nodes, (u32)p)), rt);
                    p = node_parent[p];
                    steps++;
                }
                skel->root_transform = rt;
            }

            i64 ibm_ai = json_int(json_get(sj, "inverseBindMatrices"), -1);
            if (ibm_ai >= 0) {
                u32 ibm_cnt = 0;
                f32* ibm = read_accessor_f32(g, ibm_ai, 16, &ibm_cnt);
                if (!ibm || ibm_cnt < jc) {
                    g_warn(g->m, "inverseBindMatrices okunamadi");
                } else {
                    b32 ibm_nan = FALSE;
                    for (u32 k = 0; k < jc; k++) {
                        Mat4 m;
                        for (u32 c = 0; c < 16; c++) {
                            f32 v = ibm[k * 16 + c];
                            if (!anim_f32_finite(v)) ibm_nan = TRUE;
                            m.m[c] = v;
                        }
                        skel->inv_bind[k] = m;
                    }
                    if (ibm_nan) g_warn(g->m, "inverseBindMatrices icinde NaN");
                }
            }

            if (!skeleton_validate(skel)) {
                g_warn(g->m, "bozuk veya dongusel iskelet temizlendi");
                skeleton_sanitize(skel);
            }
        }
    }

    /* Animations */
    const JsonValue* anims_json = json_get(g->root, "animations");
    u32 anim_cnt = json_len(anims_json);
    if (anim_cnt > GLTF_MAX_ANIMS) {
        g_warn(g->m, "cok fazla animasyon");
        anim_cnt = GLTF_MAX_ANIMS;
    }
    if (anim_cnt > 0u) {
        if (!arena_has_space(g->out, (usize)(anim_cnt + 1u) * sizeof(AnimClip))) {
            g_err(g->m, "animasyon icin arena yetersiz");
            return;
        }
        g->m->animations = ARENA_ARRAY(g->out, AnimClip, anim_cnt + 1u);
        g->m->anim_count = anim_cnt;

        for (u32 ai = 0; ai < anim_cnt; ai++) {
            const JsonValue* aj = json_at(anims_json, ai);
            AnimClip* clip = &g->m->animations[ai];
            const char* nm = json_str(json_get(aj, "name"), "");
            for (u32 c = 0; nm[c] && c + 1u < sizeof(clip->name); c++) clip->name[c] = nm[c];

            const JsonValue* smps_j = json_get(aj, "samplers");
            const JsonValue* chs_j  = json_get(aj, "channels");
            u32 sc = json_len(smps_j);
            u32 cc = json_len(chs_j);
            if (sc == 0u || cc == 0u || sc > ANIM_MAX_SAMPLERS || cc > ANIM_MAX_CHANNELS) {
                g_warn(g->m, "0 anahtar kare veya bos animasyon");
                continue;
            }
            if (!arena_has_space(g->out, (usize)sc * sizeof(AnimSampler) + (usize)cc * sizeof(AnimChannel) + 128u)) {
                g_err(g->m, "animasyon kanal/sampler icin arena yetersiz");
                return;
            }
            clip->samplers = ARENA_ARRAY(g->out, AnimSampler, sc);
            clip->sampler_count = sc;
            clip->channels = ARENA_ARRAY(g->out, AnimChannel, cc);

            for (u32 si = 0; si < sc; si++) {
                const JsonValue* sj = json_at(smps_j, si);
                AnimSampler* dst = &clip->samplers[si];
                const char* interp_s = json_str(json_get(sj, "interpolation"), "LINEAR");
                if (str_eq(interp_s, "LINEAR")) dst->interp = ANIM_INTERP_LINEAR;
                else if (str_eq(interp_s, "STEP")) dst->interp = ANIM_INTERP_STEP;
                else if (str_eq(interp_s, "CUBICSPLINE")) dst->interp = ANIM_INTERP_CUBICSPLINE;
                else {
                    g_warn(g->m, "bilinmeyen interpolasyon modu");
                    dst->interp = ANIM_INTERP_LINEAR;
                }

                i64 in_ai  = json_int(json_get(sj, "input"), -1);
                i64 out_ai = json_int(json_get(sj, "output"), -1);
                const JsonValue* in_acc  = json_at(json_get(g->root, "accessors"), (u32)in_ai);
                const JsonValue* out_acc = json_at(json_get(g->root, "accessors"), (u32)out_ai);
                if (!in_acc || !out_acc || json_int(json_get(in_acc, "count"), 0) <= 0) {
                    g_warn(g->m, "0 anahtar kare veya gecersiz sampler");
                    continue;
                }
                u32 comps = type_comps(json_str(json_get(out_acc, "type"), ""));
                if (comps != 3u && comps != 4u) {
                    g_warn(g->m, "desteklenmeyen animasyon output tipi");
                    continue;
                }
                u32 in_cnt = 0, out_cnt = 0;
                f32* t_tmp = read_accessor_f32(g, in_ai, 1, &in_cnt);
                f32* v_tmp = read_accessor_f32(g, out_ai, comps, &out_cnt);
                u32 want_out = (dst->interp == ANIM_INTERP_CUBICSPLINE) ? (in_cnt * 3u) : in_cnt;
                if (!t_tmp || !v_tmp || in_cnt == 0u || out_cnt < want_out) {
                    g_warn(g->m, "sampler input/output okunamadi");
                    continue;
                }

                b32 has_nan = FALSE, unsorted = FALSE;
                for (u32 k = 0; k < in_cnt; k++) {
                    if (!anim_f32_finite(t_tmp[k])) { has_nan = TRUE; t_tmp[k] = 0.0f; }
                    if (k > 0u && t_tmp[k] < t_tmp[k - 1u]) {
                        unsorted = TRUE;
                        t_tmp[k] = t_tmp[k - 1u];
                    }
                    if (t_tmp[k] > clip->duration) clip->duration = t_tmp[k];
                }
                usize total_v = (usize)want_out * (usize)comps;
                for (usize k = 0; k < total_v; k++) {
                    if (!anim_f32_finite(v_tmp[k])) { has_nan = TRUE; v_tmp[k] = 0.0f; }
                }
                if (has_nan) g_warn(g->m, "animasyon anahtar karesinde NaN");
                if (unsorted) g_warn(g->m, "sirali olmayan anahtar kare zamanlari");

                usize copy_bytes = ((usize)in_cnt + total_v) * sizeof(f32) + 64u;
                if (!arena_has_space(g->out, copy_bytes)) {
                    g_err(g->m, "animasyon verisi icin arena yetersiz");
                    return;
                }
                f32* t_out = ARENA_ARRAY(g->out, f32, in_cnt);
                f32* v_out = ARENA_ARRAY(g->out, f32, total_v);
                __builtin_memcpy(t_out, t_tmp, (usize)in_cnt * sizeof(f32));
                __builtin_memcpy(v_out, v_tmp, total_v * sizeof(f32));
                dst->count  = in_cnt;
                dst->comps  = comps;
                dst->times  = t_out;
                dst->values = v_out;
            }

            u32 valid_ch = 0;
            for (u32 ci = 0; ci < cc; ci++) {
                const JsonValue* cj = json_at(chs_j, ci);
                i64 si = json_int(json_get(cj, "sampler"), -1);
                const JsonValue* tgt = json_get(cj, "target");
                if (!json_get(tgt, "node")) continue;
                i64 tnode = json_int(json_get(tgt, "node"), -1);
                const char* path_s = json_str(json_get(tgt, "path"), "");
                if (str_eq(path_s, "weights")) {
                    g_warn(g->m, "morph animasyonu yok sayildi");
                    continue;
                }
                AnimPath path;
                if (str_eq(path_s, "translation")) path = ANIM_PATH_TRANSLATION;
                else if (str_eq(path_s, "rotation")) path = ANIM_PATH_ROTATION;
                else if (str_eq(path_s, "scale")) path = ANIM_PATH_SCALE;
                else {
                    g_warn(g->m, "bilinmeyen animasyon path");
                    continue;
                }
                if (si < 0 || (u64)si >= sc || clip->samplers[si].count == 0u) {
                    g_warn(g->m, "gecersiz kanal sampler");
                    continue;
                }
                u32 want_comps = (path == ANIM_PATH_ROTATION) ? 4u : 3u;
                if (clip->samplers[si].comps != want_comps) {
                    g_warn(g->m, "kanal/sampler bilesen uyusmazligi");
                    continue;
                }
                i32 found_joint = -1;
                for (u32 sk_i = 0; sk_i < g->m->skin_count && found_joint < 0; sk_i++) {
                    const Skeleton* sk = &g->m->skins[sk_i];
                    for (u32 jk = 0; jk < sk->joint_count; jk++) {
                        if (sk->node_indices[jk] == (i32)tnode) {
                            found_joint = (i32)jk;
                            break;
                        }
                    }
                }
                if (found_joint < 0) {
                    g_warn(g->m, "kanal hedef dugumu iskelette yok");
                    continue;
                }
                AnimChannel* ch_dst = &clip->channels[valid_ch++];
                ch_dst->sampler = (u32)si;
                ch_dst->joint   = (u32)found_joint;
                ch_dst->node    = (i32)tnode;
                ch_dst->path    = path;
            }
            clip->channel_count = valid_ch;
        }
    }
}

static void visit_node(G* g, i64 ni, Mat4 parent, u32 depth) {
    if (ni < 0 || (u64)ni >= g->node_count) return;
    const JsonValue* n = json_at(json_get(g->root, "nodes"), (u32)ni);
    if (!n) return;
    if (g->node_visiting[ni] || depth > 64u || ++g->visit_steps > GLTF_MAX_VISITS) {
        g_warn(g->m, "node derinligi/dongusu");
        return;
    }
    g->node_visiting[ni] = 1u;

    Mat4 world = mat4_mul(parent, node_local(g, n));
    i64 skin_i64 = json_int(json_get(n, "skin"), -1);
    i32 skin_idx = (skin_i64 >= 0 && (u64)skin_i64 < g->m->skin_count &&
                    g->m->skins[skin_i64].joint_count > 0u) ? (i32)skin_i64 : -1;
    if (json_get(n, "skin") && skin_idx < 0) {
        g_warn(g->m, "gecersiz node skin indeksi");
    }

    const JsonValue* mesh = json_at(json_get(g->root, "meshes"), (u32)json_int(json_get(n, "mesh"), -1));
    if (json_get(n, "mesh") && mesh) {
        const JsonValue* prims = json_get(mesh, "primitives");
        u32 plen = json_len(prims);
        for (u32 p = 0; p < plen; p++) {
            const JsonValue* prim = json_at(prims, p);
            SkinnedMesh* skinned = NULL;
            Mesh* m = build_primitive(g, prim, skin_idx, &skinned);
            if (!m) { if (g->m->err[0]) { g->node_visiting[ni] = 0u; return; } continue; }
            if (!arena_has_space(g->scratch, sizeof(struct DrawNode) + 32u)) {
                g_err(g->m, "cizim dugumu icin arena yetersiz");
                g->node_visiting[ni] = 0u;
                return;
            }
            struct DrawNode* d = ARENA_NEW(g->scratch, struct DrawNode);
            d->d.mesh     = m;
            /* glTF 2.0 §3.7.3.3: skin bağlı düğümün kendi yerel dönüşümü skinning'de uygulanmaz */
            d->d.world    = (skin_idx >= 0 && skinned) ? mat4_identity() : world;
            d->d.material = (i32)json_int(json_get(prim, "material"), -1);
            if (d->d.material >= (i32)g->m->material_count) d->d.material = -1;
            d->d.skin     = skin_idx;
            d->d.skinned  = skinned;
            if (g->tail) g->tail->next = d; else g->head = d;
            g->tail = d;
            g->draws++;
            g->m->triangles += m->index_count / 3u;
        }
    }
    const JsonValue* ch = json_get(n, "children");
    u32 clen = json_len(ch);
    for (u32 i = 0; i < clen && !g->m->err[0]; i++) {
        visit_node(g, json_int(json_at(ch, i), -1), world, depth + 1u);
    }
    g->node_visiting[ni] = 0u;
}

static u32 next_pow2_le(u32 v, u32 cap) {
    u32 p = 1;
    while (p < v && p < cap) p <<= 1;
    return p;
}

static void load_texture(G* g, u32 ti, Texture* out) {
    __builtin_memset(out, 0, sizeof(*out));
    const JsonValue* t = json_at(json_get(g->root, "textures"), ti);
    const JsonValue* img = json_at(json_get(g->root, "images"), (u32)json_int(json_get(t, "source"), -1));
    if (!img) { g_warn(g->m, "doku kaynagi yok"); return; }
    GBuf b = { NULL, 0 };
    const char* mime = json_str(json_get(img, "mimeType"), "");
    const char* uri = json_str(json_get(img, "uri"), NULL);
    if (uri) {
        if (str_starts(uri, "data:image/jpeg")) { g_warn(g->m, "JPEG desteklenmiyor"); return; }
        if (!load_uri(g, uri, &b)) { g_warn(g->m, "goruntu uri cozulemedi"); return; }
    } else {
        u32 stride;
        if (!view_slice(g, json_int(json_get(img, "bufferView"), -1), &b.data, &b.len, &stride)) {
            g_warn(g->m, "goruntu bufferView gecersiz"); return;
        }
        if (str_eq(mime, "image/jpeg")) { g_warn(g->m, "JPEG desteklenmiyor"); return; }
    }
    if (b.len < 8 || b.data[0] != 0x89 || b.data[1] != 'P') { g_warn(g->m, "PNG olmayan goruntu"); return; }
    u32 w = 0, h = 0;
    u32* px = png_decode(b.data, b.len, &w, &h, g->scratch);
    if (!px || w == 0 || h == 0) { g_warn(g->m, "PNG cozulemedi"); return; }
    u32 nw = next_pow2_le(w, 1024), nh = next_pow2_le(h, 1024);
    if (!arena_has_space(g->out, (usize)nw * (usize)nh * 8u + 256u)) {
        g_warn(g->m, "doku icin arena yetersiz");
        return;
    }
    u32 flags = TEX_FLAG_BILINEAR;
    const JsonValue* smp = json_at(json_get(g->root, "samplers"), (u32)json_int(json_get(t, "sampler"), -1));
    if (json_int(json_get(smp, "wrapS"), 10497) == 33071 || json_int(json_get(smp, "wrapT"), 10497) == 33071) {
        flags |= TEX_FLAG_CLAMP;
    }
    *out = *tex_create(g->out, nw, nh, flags | TEX_FLAG_MIPMAP);
    if (nw == w && nh == h) {
        for (u32 i = 0; i < w * h; i++) out->px[i] = px[i];
    } else {
        img_resize_bilinear(px, w, h, out->px, nw, nh);
    }
    tex_build_mips(out);
}

b32 gltf_load(const u8* data, u32 len, Arena* out, Arena* scratch,
              GltfResolveFn resolve, void* user, GltfModel* model) {
    if (!model) return FALSE;
    __builtin_memset(model, 0, sizeof(*model));
    if (!data || len == 0 || !out || !scratch) {
        g_err(model, "gecersiz girdi veya arena");
        return FALSE;
    }
    const char* json_text = (const char*)data;
    u32 json_len_b = len;
    GBuf bin = { NULL, 0 };

    if (len >= 12 && data[0] == 'g' && data[1] == 'l' && data[2] == 'T' && data[3] == 'F') {
        u32 ver, total;
        __builtin_memcpy(&ver, data + 4, 4);
        __builtin_memcpy(&total, data + 8, 4);
        if (ver != 2) { g_err(model, "GLB surumu 2 degil"); return FALSE; }
        if (total > len) { g_err(model, "GLB kesik"); return FALSE; }
        u32 off = 12;
        json_text = NULL;
        while (off + 8 <= total) {
            u32 clen, ctype;
            __builtin_memcpy(&clen, data + off, 4);
            __builtin_memcpy(&ctype, data + off + 4, 4);
            if ((u64)off + 8u + clen > total) { g_err(model, "GLB parcasi tasiyor"); return FALSE; }
            if (ctype == 0x4E4F534Au && !json_text) { json_text = (const char*)data + off + 8; json_len_b = clen; }
            else if (ctype == 0x004E4942u && !bin.data) { bin.data = data + off + 8; bin.len = clen; }
            off += 8u + ((clen + 3u) & ~3u);
        }
        if (!json_text) { g_err(model, "GLB JSON parcasi yok"); return FALSE; }
    }

    JsonValue root;
    JsonError je;
    f64 t0 = sys_now_ms();
    b32 jok = json_parse(json_text, json_len_b, scratch, &root, &je);
    model->t_json_ms = sys_now_ms() - t0;
    if (!jok) {
        g_err(model, "JSON hatasi");
        return FALSE;
    }
    const JsonValue* asset = json_get(&root, "asset");
    const char* ver = json_str(json_get(asset, "version"), "");
    if (!(ver[0] == '2' && ver[1] == '.')) { g_err(model, "asset.version 2.x degil"); return FALSE; }
    if (json_len(json_get(&root, "extensionsRequired"))) { g_err(model, "zorunlu uzanti desteklenmiyor"); return FALSE; }
    if (json_len(json_get(&root, "extensionsUsed"))) g_warn(model, "uzantilar yok sayildi");

    G g;
    __builtin_memset(&g, 0, sizeof(g));
    g.root = &root;
    g.out = out;
    g.scratch = scratch;
    g.m = model;
    g.resolve = resolve;
    g.user = user;

    const JsonValue* bufs = json_get(&root, "buffers");
    g.buf_count = json_len(bufs);
    if (g.buf_count > 256u || !arena_has_space(scratch, (usize)(g.buf_count + 1u) * sizeof(GBuf))) {
        g_err(model, "buffer sayisi cok buyuk");
        return FALSE;
    }
    g.bufs = ARENA_ARRAY(scratch, GBuf, g.buf_count + 1u);
    for (u32 i = 0; i < g.buf_count; i++) {
        const JsonValue* b = json_at(bufs, i);
        const char* uri = json_str(json_get(b, "uri"), NULL);
        i64 bl = json_int(json_get(b, "byteLength"), -1);
        if (!uri) {
            if (i == 0 && bin.data) g.bufs[i] = bin;
        } else if (!load_uri(&g, uri, &g.bufs[i])) {
            g_err(model, "buffer uri cozulemedi");
            return FALSE;
        }
        if (g.bufs[i].data && bl >= 0 && (u64)bl <= g.bufs[i].len) g.bufs[i].len = (u32)bl;
        else if (g.bufs[i].data && bl >= 0) { g_err(model, "buffer byteLength veriden buyuk"); return FALSE; }
    }

    /* Materyaller ve dokular */
    const JsonValue* texs = json_get(&root, "textures");
    model->texture_count = json_len(texs);
    if (model->texture_count > 256u || !arena_has_space(out, (usize)(model->texture_count + 1u) * sizeof(Texture))) {
        g_err(model, "doku sayisi cok buyuk");
        return FALSE;
    }
    model->textures = ARENA_ARRAY(out, Texture, model->texture_count + 1u);
    t0 = sys_now_ms();
    for (u32 i = 0; i < model->texture_count; i++) load_texture(&g, i, &model->textures[i]);
    model->t_tex_ms = sys_now_ms() - t0;
    t0 = sys_now_ms();

    const JsonValue* mats = json_get(&root, "materials");
    model->material_count = json_len(mats);
    if (model->material_count > 512u || !arena_has_space(out, (usize)(model->material_count + 1u) * sizeof(GltfMaterial))) {
        g_err(model, "materyal sayisi cok buyuk");
        return FALSE;
    }
    model->materials = ARENA_ARRAY(out, GltfMaterial, model->material_count + 1u);
    for (u32 i = 0; i < model->material_count; i++) {
        const JsonValue* mt = json_at(mats, i);
        GltfMaterial* gm = &model->materials[i];
        const JsonValue* pbr = json_get(mt, "pbrMetallicRoughness");
        const JsonValue* bc = json_get(pbr, "baseColorFactor");
        gm->base_color = v4(1, 1, 1, 1);
        if (json_len(bc) == 4) {
            gm->base_color = v4((f32)json_num(json_at(bc, 0), 1), (f32)json_num(json_at(bc, 1), 1),
                                  (f32)json_num(json_at(bc, 2), 1), (f32)json_num(json_at(bc, 3), 1));
        }
        i64 ti = json_int(json_get(json_get(pbr, "baseColorTexture"), "index"), -1);
        gm->base_texture = (ti >= 0 && (u64)ti < model->texture_count && model->textures[ti].px) ? (i32)ti : -1;
        gm->double_sided = json_bool(json_get(mt, "doubleSided"), FALSE);
        const char* am = json_str(json_get(mt, "alphaMode"), "OPAQUE");
        gm->alpha_mode = str_eq(am, "MASK") ? GLTF_ALPHA_MASK : str_eq(am, "BLEND") ? GLTF_ALPHA_BLEND : GLTF_ALPHA_OPAQUE;
        gm->alpha_cutoff = (f32)json_num(json_get(mt, "alphaCutoff"), 0.5);
    }

    /* İskeletler ve animasyonlar */
    load_skins_and_anims(&g);
    if (model->err[0]) return FALSE;
    g.node_visiting = ARENA_ARRAY(scratch, u8, g.node_count + 1u);

    /* Sahne */
    const JsonValue* scenes = json_get(&root, "scenes");
    i64 si = json_int(json_get(&root, "scene"), 0);
    const JsonValue* scene = json_at(scenes, (u32)si);
    if (scene) {
        const JsonValue* roots = json_get(scene, "nodes");
        for (u32 i = 0; i < json_len(roots) && !model->err[0]; i++) {
            visit_node(&g, json_int(json_at(roots, i), -1), mat4_identity(), 0);
        }
    } else {
        /* sahnesiz dosya: ebeveyni olmayan tüm node'lar kök */
        const JsonValue* nodes = json_get(&root, "nodes");
        u32 nc = g.node_count;
        u8* is_child = ARENA_ARRAY(scratch, u8, nc + 1u);
        for (u32 i = 0; i < nc; i++) {
            const JsonValue* ch = json_get(json_at(nodes, i), "children");
            for (u32 k = 0; k < json_len(ch); k++) {
                i64 c = json_int(json_at(ch, k), -1);
                if (c >= 0 && (u64)c < nc) is_child[c] = 1;
            }
        }
        for (u32 i = 0; i < nc && !model->err[0]; i++) if (!is_child[i]) visit_node(&g, i, mat4_identity(), 0);
    }
    model->t_mesh_ms = sys_now_ms() - t0;
    if (model->err[0]) return FALSE;
    if (g.draws == 0) { g_err(model, "cizilecek ucgen yok"); return FALSE; }
    if (!arena_has_space(out, (usize)g.draws * sizeof(GltfDraw))) {
        g_err(model, "draws icin arena yetersiz");
        return FALSE;
    }

    model->draw_count = g.draws;
    model->draws = ARENA_ARRAY(out, GltfDraw, g.draws);
    u32 k = 0;
    Vec3 lo = v3(1e30f, 1e30f, 1e30f), hi = v3(-1e30f, -1e30f, -1e30f);
    for (struct DrawNode* d = g.head; d; d = d->next) {
        model->draws[k++] = d->d;
        const Mesh* m = d->d.mesh;
        for (u32 c = 0; c < 8; c++) {
            Vec4 p = v4((c & 1) ? m->aabb_max.x : m->aabb_min.x, (c & 2) ? m->aabb_max.y : m->aabb_min.y,
                          (c & 4) ? m->aabb_max.z : m->aabb_min.z, 1.0f);
            Vec4 w = mat4_mul_v4(&d->d.world, p);
            lo = v3(m_minf(lo.x, w.x), m_minf(lo.y, w.y), m_minf(lo.z, w.z));
            hi = v3(m_maxf(hi.x, w.x), m_maxf(hi.y, w.y), m_maxf(hi.z, w.z));
        }
    }
    model->bounds_min = lo;
    model->bounds_max = hi;
    return TRUE;
}
