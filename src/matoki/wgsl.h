/* matoki/wgsl.h — Matoki → WGSL arka ucu (M13 faz 8b, D42). JIT ile aynı IR'dan üretir: düz SSA, her canlı komut bir
 * skaler `let` (dört lane'i donanım quad'ı karşılar), sin / pow / doku süzme zaten IR aritmetiği → iki arka uç aynı
 * işlemleri aynı sırayla yapar. Uniform bloğu C yapısının ham baytları, doku = Texture başlığı + px tek storage tampon.
 * Bağlama: köşe uniform'u @group(0) @binding(0), parça uniform'u @binding(1), dokular @group(1) @binding(k).
 * Giriş noktaları: `vs` (8 f32 köşe girdisi, 2 vec4 varying) ve `fs`; web/gpu.mjs bunlara bağlanır. */
#ifndef MATOKI_WGSL_H
#define MATOKI_WGSL_H
#include "core/fmt.h"
#include "matoki/ast.h"

struct MatokiProgram;

#define WGSL_MAX_TEX      3u     /* parça başına doku (lit_hq: tex + shadow + field) */
#define WGSL_MAX_UNIFORM  256u   /* uniform yuvası (gpu/gcmd.h GCMD_SLOT) */
#define WGSL_MAX_VARYINGS 8u     /* 3D boru hattı yerleşimi (VAR3D_COUNT) */
#define WGSL_MAX_ATTRS    8u     /* ATTR3D_COUNT; WebGPU varsayılan köşe tamponu sınırı da 8 */
#define WGSL_TEX_HEADER   89u    /* sizeof(Texture) / 4: px bu sözcükten başlar */

typedef struct MatokiWgsl {
    const char* text;
    u32 len;
    u32 uniform_size;            /* C bloğundan kopyalanacak bayt */
    u32 tex_count;               /* canlı IR'ın okuduğu dokular */
    u32 tex_ofs[WGSL_MAX_TEX];   /* uniform bloğunda Texture* ofseti; binding k ↔ tex_ofs[k] */
} MatokiWgsl;

/* metin a'dan ayrılır. Hata: GPU yolunun sınırını aşan program (derleme hatası değil; yazılım yolu çalışmaya devam eder) */
b32 matoki_wgsl(Arena* a, const struct MatokiProgram* p, MatokiWgsl* out, MatokiError* err);

/* f32 bitleri → birebir WGSL onaltılık sabiti (0x1.8p+1f); inf / NaN'ın sabiti yoktur: FALSE */
b32 wgsl_f32_literal(StrBuf* s, u32 bits);

#endif
