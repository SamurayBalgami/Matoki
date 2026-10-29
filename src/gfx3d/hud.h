/* gfx3d/hud.h — motor içi metin katmanı: gömülü Matoki Sans'tan iki boy atlas (14 / 20 px), yarı saydam panel
 * ve gölgeli satırlar. Çözümlenmiş (resolve sonrası) framebuffer'a CPU'da yazar; raster'dan geçmez, kare
 * hash'lerine dokunmaz (kapalıyken). Sayfadaki HTML istatistik panelinin motor içi karşılığı (M13 faz 2). */
#ifndef GFX3D_HUD_H
#define GFX3D_HUD_H
#include "core/arena.h"
#include "font/atlas.h"
#include "font/text.h"
#include "raster/raster.h"

typedef struct Hud {
    TtfFont   font;
    FontAtlas small, big;   /* 14 px, 20 px (ekran yüksekliği >= 900 ise büyük) */
    b32       ready;
} Hud;

/* perm: atlas pikselleri (kalıcı); scratch: glif rasterleme (dönüşte geri sarılır) */
b32  hud_init(Hud* h, Arena* perm, Arena* scratch);
const FontAtlas* hud_atlas(const Hud* h, const Framebuffer* fb);
/* [x, x+w) × [y, y+h) dikdörtgenini düz alfa ile karıştırır (raster BLEND_ALPHA ile aynı tamsayı formülü) */
void hud_panel(Framebuffer* fb, i32 x, i32 y, i32 w, i32 h, u32 rgba);
/* lines[n] satırları, sol üstten (x, y); panel otomatik boyutlanır. Dönüş: panelin yüksekliği */
i32  hud_draw_lines(const Hud* h, Framebuffer* fb, i32 x, i32 y, const char* const* lines, u32 n);

#endif
