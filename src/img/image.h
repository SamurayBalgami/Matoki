/* img/image.h — doku oluşturma ve mip zinciri (shade_abi.h §Texture yerleşimi).
 * Seviyeler px'ten itibaren bitişik; img_mip_offset seviye k'nın texel ofsetini verir.
 * Mip üretimi kutu filtresi, v0'da lineer uzayda (sRGB-doğru indirgeme sonraki adım). */
#ifndef IMG_IMAGE_H
#define IMG_IMAGE_H
#include "core/types.h"
#include "core/arena.h"
#include "raster/shade_abi.h"

u32   img_mip_level_count(u32 w, u32 h);          /* 1 + floor(log2(max(w,h))) */
static inline u32 img_mip_dim(u32 d, u32 level) { u32 r = d >> level; return r ? r : 1; }
usize img_mip_offset(u32 w, u32 h, u32 level);    /* seviye 'level'ın başlangıcı (texel) */
usize img_mip_total(u32 w, u32 h, u32 levels);    /* tüm zincirin texel sayısı */
void  img_build_mips(u32* px, u32 w, u32 h, u32 levels);

/* Önceden hesaplanmış mip seviyesi bilgisi (Texture ABI'sine dokunmadan, shader uniform'larında tutulur).
 * Örnekleme sıcak yolunda img_mip_offset döngüsünü ortadan kaldırır. */
typedef struct TexLevel {
    const u32* px;     /* seviyenin ilk texel'i */
    u32        w, h;
    f32        fw, fh; /* (f32)w, (f32)h */
} TexLevel;

/* out[TEX_MAX_LEVELS] doldurulur; dönüş = kullanılabilir seviye sayısı (>= 1). */
u32 tex_level_table(const Texture* t, TexLevel* out);

/* w, h 2'nin kuvveti olmalı (wrap için). TEX_FLAG_MIPMAP verilirse tam zincir ayrılır;
 * içerik doldurulduktan sonra img_build_mips(t->px, t->w, t->h, t->levels) çağrılmalı. */
Texture* tex_create(Arena* a, u32 w, u32 h, u32 flags);
void     tex_build_mips(Texture* t);
/* seviye tablolarını (lv_ofs, lv_w/h, lv_fw/fh) w, h, levels'tan doldurur (tex_create çağırır; elle kurulan dokular için) */
void     tex_fill_level_offsets(Texture* t);

#endif
