/* font/ttf.h — Sıfırdan TrueType (.ttf, glyf tabanlı, big-endian) yazı tipi ayrıştırıcı.
 * head, hhea, hmtx, maxp, loca (kısa/uzun), glyf (basit ve dönüşümlü bileşik), cmap (biçim 4/12)
 * ve kern (biçim 0) tablolarını sınır denetimli olarak okur; bozuk veride sınır dışı okumaz.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/font/ttf.h), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#ifndef FONT_TTF_H
#define FONT_TTF_H

#include "core/types.h"
#include "core/arena.h"

#define TTF_MAX_COMPOSITE_DEPTH 16u
#define TTF_MAX_GLYPH_POINTS    16384u
#define TTF_MAX_GLYPH_CONTOURS  2048u
#define TTF_MAX_GLYPH_NODES     1024u   /* bir glif açılırken ziyaret edilen toplam (bileşik dahil) glif sayısı */

typedef struct {
    const u8* data;
    u32       len;

    /* Tablo ofset ve uzunlukları */
    u32 head_off, head_len;
    u32 hhea_off, hhea_len;
    u32 hmtx_off, hmtx_len;
    u32 maxp_off, maxp_len;
    u32 loca_off, loca_len;
    u32 glyf_off, glyf_len;
    u32 cmap_off, cmap_len;
    u32 kern_off, kern_len;

    /* Cmap alt tablo ofsetleri (dosya başından; 0 ise yok) */
    u32 cmap4_off,  cmap4_len;
    u32 cmap12_off, cmap12_len;

    /* Kern biçim 0 alt tablosu (dosya başından; 0 ise yok) */
    u32 kern0_pairs_off;
    u16 kern0_pair_count;
    b32 kern0_sorted;

    /* Genel yazı tipi metrikleri (FUnits) */
    u16 units_per_em;
    i16 ascent;
    i16 descent;
    i16 line_gap;
    i16 bbox_xmin, bbox_ymin, bbox_xmax, bbox_ymax;
    u16 num_glyphs;
    u16 num_hmetrics;
    i16 index_to_loc_format; /* 0: kısa (u16*2), 1: uzun (u32) */
} TtfFont;

typedef struct {
    f32 x, y;     /* FUnits cinsinden glif noktası (bileşik glif dönüşümü uygulanmış) */
    u8  on_curve; /* 1: eğri üzerinde, 0: ikinci derece Bezier kontrol noktası */
} TtfPoint;

typedef struct {
    u16       glyph_index;
    i16       xmin, ymin, xmax, ymax;
    u16       advance;
    i16       lsb;
    u32       contour_count;
    u32       point_count;
    u16*      contour_ends; /* her konturun son nokta indeksi [contour_count] */
    TtfPoint* points;       /* nokta dizisi [point_count] */
} TtfGlyph;

/* TTF belleğini ayrıştırır ve tüm zorunlu tabloların sınırlarını doğrular. */
b32 ttf_parse(const u8* data, u32 len, TtfFont* out);

/* Unicode kod noktasını glif indeksine eşler (bulunamazsa 0 döner). */
u16 ttf_glyph_index(const TtfFont* font, u32 codepoint);
u16 ttf_cmap4_lookup(const TtfFont* font, u32 codepoint);
u16 ttf_cmap12_lookup(const TtfFont* font, u32 codepoint);

/* Glifin yatay metriklerini (advance ve lsb, FUnits cinsinden) döner. */
b32 ttf_glyph_metrics(const TtfFont* font, u16 glyph_index, u16* out_advance, i16* out_lsb);

/* İki glif arasındaki kerning değerini (FUnits) döner; kern yoksa 0 döner. */
i16 ttf_kern_advance(const TtfFont* font, u16 left_glyph, u16 right_glyph);

/* Verilen glifin kontur ve noktalarını (bileşik glifleri özyinelemeli açarak) arena'ya yükler. */
b32 ttf_load_glyph(const TtfFont* font, u16 glyph_index, Arena* arena, TtfGlyph* out);

#endif
