/* matoki/sema.h — anlamsal çözümleme: isimler, tipler, yerleşikler, uniform/varying yerleşimi.
 * Uniform'lar bildirim sırasıyla 4 bayt hizalı, aralıksız paketlenir (f32/i32 4, vecN 4N,
 * texture2d 4 = wasm32 işaretçisi). Varying'ler ardışık yuvalara (veya @slot(N)) yerleşir.
 * Kurallar docs/MATOKI.md'de. */
#ifndef MATOKI_SEMA_H
#define MATOKI_SEMA_H
#include "matoki/ast.h"
#include "matoki/ir.h"

enum { SYM_LOCAL, SYM_UNIFORM, SYM_VARYING };

typedef struct Sym {
    const char*  name;
    u32          name_len;
    u32          kind;
    Type         type;
    b32          mutable_;
    GlobalDecl*  g;
    IrRef        vals[4];      /* lower: yerel değişkenin güncel bileşenleri */
    struct Sym*  next;
} Sym;

typedef enum Builtin {
    BI_NONE,
    BI_VEC2, BI_VEC3, BI_VEC4, BI_F32, BI_I32,
    BI_ABS, BI_MIN, BI_MAX, BI_CLAMP,
    BI_FLOOR, BI_CEIL, BI_FRACT, BI_TRUNC, BI_ROUND, BI_SQRT, BI_SIN, BI_COS,
    BI_EXP2, BI_LOG2, BI_EXP, BI_LOG, BI_POW,
    BI_MIX, BI_STEP, BI_SMOOTHSTEP, BI_MOD,
    BI_DOT, BI_CROSS, BI_LENGTH, BI_DISTANCE, BI_NORMALIZE, BI_REFLECT,
    BI_SELECT,
    BI_SAMPLE, BI_TEXEL_FETCH, BI_TEX_SIZE, BI_SAMPLE_LEVEL, BI_MIP_LEVEL,
    BI_BITCAST_F32, BI_BITCAST_I32,
    BI_DFDX, BI_DFDY, BI_FWIDTH,
    BI_FRAG_COORD,
    BI_COUNT
} Builtin;

typedef struct SemaResult {
    FnDecl* frag;
    u32     uniform_size;
    u32     varying_slots;
} SemaResult;

b32 sema_module(Arena* a, Module* m, SemaResult* out, MatokiError* err);

/* lower: tip denetimi geçmiş modülü IR'a çevirir (frag satır içi, çıktı paketlenmiş renk) */
b32 lower_module(Arena* a, Module* m, const SemaResult* s, IrFunc* f, MatokiError* err);

#endif
