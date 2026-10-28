/* matoki/lexer.h — Matoki sözcük çözümleyicisi.
 * Kaynağın tamamını tek seferde Token dizisine çevirir (arena). Yorumlar: satır (//) ve blok.
 * Sayılar: 123, 0x1F (i32), 1.5, 1e-3, 2.0e+4 (f32). "0..4" → INT DOTDOT INT. */
#ifndef MATOKI_LEXER_H
#define MATOKI_LEXER_H
#include "core/arena.h"

typedef enum TokKind {
    T_EOF, T_ERROR,
    T_IDENT, T_INT, T_FLOAT,
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_COMMA, T_SEMI, T_COLON, T_DOT, T_DOTDOT, T_ARROW, T_AT,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_LT, T_GT, T_LE, T_GE, T_EQEQ, T_NE, T_ANDAND, T_OROR, T_BANG,
    T_ASSIGN, T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ,
    T_AMP, T_PIPE, T_CARET, T_SHL, T_SHR, T_TILDE,
    /* anahtar sözcükler */
    T_FN, T_LET, T_VAR, T_IF, T_ELSE, T_FOR, T_IN, T_RETURN, T_UNIFORM, T_VARYING, T_TRUE, T_FALSE,
    T_DISCARD,
    T_COUNT
} TokKind;

typedef struct Token {
    TokKind     kind;
    const char* p;
    u32         len;
    i32         line, col;
    i64         ival;
    f32         fval;
} Token;

typedef struct TokenList {
    Token* toks;
    u32    count;
    /* hata (kind == T_ERROR olan token'da da konum var) */
    b32         failed;
    i32         err_line, err_col;
    const char* err_msg;
} TokenList;

b32 lex_source(Arena* a, const char* src, u32 len, TokenList* out);
const char* tok_name(TokKind k);

/* "1.5e-3" → f32. f64 ara değerle (≤19 anlamlı basamak); doğru yuvarlama garanti değil, hata ≤ 1 ULP. */
f32 lex_parse_f32(const char* p, u32 len);

#endif
