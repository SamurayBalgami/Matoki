/* img/png.h — Sıfırdan PNG kodlayıcı/çözücü ve RFC 1950/1951 (zlib/DEFLATE) motoru.
 * Kodlayıcı: CRC32, Adler-32, LZ77 hash zinciri + sabit & dinamik Huffman DEFLATE,
 *            ve 5 PNG satır filtresinden (None/Sub/Up/Avg/Paeth) satır başına en iyisini seçme.
 * Çözücü:    zlib/DEFLATE inflate (stored, sabit, dinamik Huffman), 5 PNG filtresi,
 *            8-bit RGBA, RGB, Gri, Gri+Alfa ve Paletli (PLTE+tRNS) renk tipleri (interlace hariç).
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/img/png.h), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#ifndef IMG_PNG_H
#define IMG_PNG_H

#include "core/types.h"
#include "core/arena.h"

/* CRC32 (IEEE 802.3 polinomu 0xEDB88320) ve Adler-32 (RFC 1950) */
u32  png_crc32(const u8* data, usize len);
u32  png_adler32(const u8* data, usize len);

/* Ham DEFLATE (RFC 1951) sıkıştırma ve açma */
u8*  png_deflate_compress(const u8* src, usize src_len, usize* out_len, Arena* arena);
u8*  png_deflate_decompress(const u8* src, usize src_len, usize* out_len, Arena* arena);

/* Zlib sarmalayıcılı (RFC 1950: 2 bayt başlık + DEFLATE + 4 bayt Adler-32) sıkıştırma ve açma */
u8*  png_zlib_compress(const u8* src, usize src_len, usize* out_len, Arena* arena);
u8*  png_zlib_decompress(const u8* src, usize src_len, usize* out_len, Arena* arena);

/* RGBA8 (u32 little-endian 0xAABBGGRR) pikselleri PNG ikili akışına kodlar.
 * Başarıda PNG bayt dizisini döner ve *out_len'i yazar; hatada NULL döner. */
u8*  png_encode(const u32* rgba_px, u32 w, u32 h, usize* out_len, Arena* arena);

/* PNG ikili akışını çözer ve RGBA8 (u32) piksel dizisini Arena'dan ayırıp döner.
 * Desteklenen: bit_depth=8, color_type ∈ {0 (Gri), 2 (RGB), 3 (Paletli), 4 (Gri+A), 6 (RGBA)}, interlace=0.
 * Geçersiz veri veya interlace durumunda NULL döner. */
u32* png_decode(const u8* png_data, usize png_len, u32* out_w, u32* out_h, Arena* arena);

#endif
