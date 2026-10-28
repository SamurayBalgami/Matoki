/* matoki/ast.h — Matoki soyut sözdizim ağacı ve tipler.
 * Parser düğümleri üretir; sema tipleri (Node.type) ve isim çözümlemesini (Node.sym,
 * Node.builtin) doldurur; lower IR'a çevirir. Tüm düğümler arena'da yaşar. */
#ifndef MATOKI_AST_H
#define MATOKI_AST_H
#include "core/arena.h"
#include "matoki/lexer.h"

/* Tip: temel tür + bileşen sayısı. vec3 = {TB_FLOAT, 3}, i32 = {TB_INT, 1}.
 * Matris: {TB_MAT, 3|4} = mat3/mat4 (n = boyut), yalnızca uniform ve yalnızca 'M * v' içinde. */
enum {
    TB_ERROR = 0,
    TB_VOID,
    TB_BOOL,
    TB_INT,
    TB_FLOAT,
    TB_TEX,
    TB_MAT
};

typedef struct Type {
    u8 base;
    u8 n;
} Type;

static inline Type type_make(u8 base, u8 n) { Type t; t.base = base; t.n = n; return t; }
static inline b32 type_eq(Type a, Type b) { return a.base == b.base && a.n == b.n; }

typedef enum NodeKind {
    /* ifadeler */
    NK_INT, NK_FLOAT, NK_BOOL, NK_NAME, NK_UNARY, NK_BINARY, NK_CALL, NK_SWIZZLE,
    /* deyimler */
    NK_LET, NK_VAR, NK_ASSIGN, NK_IF, NK_FOR, NK_RETURN, NK_EXPR, NK_BLOCK, NK_DISCARD
} NodeKind;

struct Sym;

typedef struct Node {
    NodeKind     kind;
    i32          line, col;
    Type         type;          /* sema */
    TokKind      op;            /* unary/binary/assign işleci */
    const char*  name;
    u32          name_len;
    i64          ival;
    f32          fval;
    struct Node* a;             /* işlenen / koşul / ilk değer / hedef */
    struct Node* b;             /* ikinci işlenen / then bloğu / değer */
    struct Node* c;             /* else */
    struct Node* list;          /* argümanlar / blok deyimleri */
    struct Node* next;
    u8           swz[4];
    u8           swz_n;
    b32          has_decl_type;
    Type         decl_type;
    i64          lo, hi;        /* for aralığı */
    struct Sym*  sym;           /* sema: yerel / global sembol */
    struct FnDecl* fn;          /* sema: kullanıcı fonksiyonu çağrısı */
    i32          builtin;       /* sema: yerleşik çağrı kimliği (0 = yok) */
} Node;

typedef struct Param {
    const char*   name;
    u32           name_len;
    Type          type;
    i32           line, col;
    struct Sym*   sym;
    struct Param* next;
} Param;

typedef struct FnDecl {
    const char*    name;
    u32            name_len;
    i32            line, col;
    Param*         params;
    u32            param_count;
    Type           ret;
    Node*          body;        /* NK_BLOCK */
    u8             visit;       /* sema: 0 yok, 1 ziyarette, 2 bitti (özyineleme tespiti) */
    struct FnDecl* next;
} FnDecl;

enum { GD_UNIFORM, GD_VARYING };

#define ATTR_CLAMP    1u
#define ATTR_MIPMAP   2u
#define ATTR_BILINEAR 4u

typedef struct GlobalDecl {
    u32                kind;
    const char*        name;
    u32                name_len;
    i32                line, col;
    Type               type;
    u32                attrs;
    i32                slot;     /* varying: açık @slot(N) veya -1 */
    u32                offset;   /* sema: uniform bayt ofseti / varying yuvası */
    struct GlobalDecl* next;
} GlobalDecl;

typedef struct Module {
    GlobalDecl* globals;
    FnDecl*     fns;
} Module;

typedef struct MatokiError {
    b32  failed;
    i32  line, col;
    char msg[200];
} MatokiError;

void matoki_error(MatokiError* e, i32 line, i32 col, const char* msg, const char* detail, u32 detail_len);

b32 parse_module(Arena* a, const TokenList* toks, Module* out, MatokiError* err);

#endif
