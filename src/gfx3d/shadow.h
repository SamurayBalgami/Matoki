/* gfx3d/shadow.h — yönlü ışık (güneş) için gölge haritası (M13 faz 7; MatokiEngine gfx3d/shadow'dan bizim boru hattına).
 * Işık uzayında ortografik izdüşüm, yalnız derinlik geçişi (Material.shade = NULL → raster varying / renk yazmaz).
 * Doku dokunmadan derinlik tamponunu gösterir (px = f32 bitleri, D-107 yöntemi): lit_hq.mtk texel_fetch + bitcast_f32.
 * Kırpma kutusu kameraya bağlı küreye oturur ve texel ızgarasına hizalanır (kamera kayarken titreme yok).
 * Derinlik kuralı D2: 0 yakın (ışığa), 1 uzak, DEPTH_LESS, temizleme 1. */
#ifndef GFX3D_SHADOW_H
#define GFX3D_SHADOW_H
#include "gfx3d/pipeline.h"

typedef struct ShadowMap {
    Framebuffer fb;          /* yalnız depth (size²) */
    Texture     tex;         /* px = (u32*)fb.depth, 1 seviye, kenara kenetli */
    Raster      raster;
    u32         size;
    Mat4        view, proj;
    Mat4        world_to_tex;   /* dünya → (u, v, derinlik); LitUniforms.shadow_mat */
    f32         texel_world;    /* bir texel'in dünya boyu (m) */
    f32         depth_range;    /* ışık yönünde derinlik aralığı (m): sapmayı metreden 0..1'e çevirir */
} ShadowMap;

void shadow_init(ShadowMap* s, Arena* perm, u32 size);
/* sun_dir: ışığa doğru birim vektör; center / radius: gölge alacak bölgeyi saran küre; caster_extra: kürenin dışında,
 * ışık tarafında kalan gölge düşürücüler için ek derinlik (m) */
void shadow_fit(ShadowMap* s, Vec3 sun_dir, Vec3 center, f32 radius, f32 caster_extra);
/* derinliği temizler, raster'ı haritaya bağlar ve boru hattını ışık görüşüyle başlatır; çizimler shadow_caster ile */
void shadow_begin(ShadowMap* s, Pipeline* p, Arena* frame, Arena* bins);
/* yalnız derinlik, ön yüzler ayıklanır (arka yüzler yazılır → yüzey kendini daha az gölgeler) */
const Material* shadow_caster(void);
void shadow_end(ShadowMap* s, Pipeline* p);
/* gölge kapalıyken LitUniforms.shadow: 1×1, derinlik 2 (hiçbir nokta gölgede değil) */
const Texture* shadow_none(void);
/* sapma: metre → harita derinliği */
static inline f32 shadow_bias_for(const ShadowMap* s, f32 meters) { return meters / s->depth_range; }

#endif
