/* gfx2d/tilemap.h — Izgara tabanlı karo haritası: atlas'tan çizim (yalnız görünen karolar),
 * karo bayrakları (katı, tek yönlü platform, tehlike) ve AABB ↔ harita çarpışma çözümü.
 * Çarpışma eksen ayrık ve alt adımlıdır (adım <= yarım karo) → yüksek hızda tünelleme olmaz.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/gfx2d/tilemap.h), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#ifndef GFX2D_TILEMAP_H
#define GFX2D_TILEMAP_H

#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "gfx2d/batch2d.h"

#define TILE_EMPTY   0u

#define TILE_SOLID   1u   /* her yönden katı */
#define TILE_ONEWAY  2u   /* yalnız yukarıdan inerken katı (aşağı + zıpla ile geçilir) */
#define TILE_HAZARD  4u   /* değince hasar (çarpışma çözmez, yalnız raporlanır) */
#define TILE_PICKUP  8u   /* toplanabilir (oyun mantığı karar verir) */

typedef struct {
    u16 atlas_index;      /* atlas'taki karo indeksi (satır öncelikli) */
    u16 flags;            /* TILE_* */
} TileDef;

typedef struct {
    u32  w, h;            /* karo cinsinden */
    f32  tile_size;       /* dünya birimi */
    u16* tiles;           /* [w*h], TileDef indeksi; 0 = boş */
    const TileDef* defs;
    u32  def_count;
    u16  border_tile;     /* harita dışı okunduğunda dönen karo (ör. katı duvar) */
    const Texture* atlas;
    u32  atlas_tile_px;   /* atlas'ta bir karonun piksel boyu */
} Tilemap;

typedef struct {
    b32 on_ground;        /* aşağı hareket katı/tek yönlü karoda durdu */
    b32 hit_ceiling;
    b32 hit_left;
    b32 hit_right;
    u32 touched;          /* hareket sonunda AABB'nin değdiği karoların bayrakları (OR) */
} TileHit;

void tilemap_init(Tilemap* m, Arena* a, u32 w, u32 h, f32 tile_size);
u16  tilemap_get(const Tilemap* m, i32 tx, i32 ty);
void tilemap_set(Tilemap* m, i32 tx, i32 ty, u16 tile);
u32  tilemap_flags(const Tilemap* m, i32 tx, i32 ty);
/* ASCII yükleyici: satırlar '\n' ile ayrılır; legend[ch] → karo (tanımsız karakter 0) */
void tilemap_load_ascii(Tilemap* m, const char* text, const u16 legend[128]);

/* Görünür dikdörtgendeki karoları batch'e çizer, çizilen karo sayısını döner */
u32  tilemap_draw(const Tilemap* m, Batch2D* b, Rect visible, Vec4 tint);

/* pos = AABB sol üst köşesi (dünya), size = boyut. delta kadar hareket ettirir, çarpışmaları çözer.
 * drop_through: tek yönlü platformlardan aşağı geç. */
TileHit tilemap_move(const Tilemap* m, Vec2* pos, Vec2 size, Vec2 delta, b32 drop_through);
/* AABB'nin şu an değdiği karoların bayrakları */
u32  tilemap_query(const Tilemap* m, Vec2 pos, Vec2 size);

#endif
