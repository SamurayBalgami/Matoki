/* matoki/lexer.c — sözcük çözümleyici.
 * Token dizisi iki geçişte değil, kapasitesi kaynak uzunluğu + 1 olan tek dizide üretilir
 * (her token en az 1 bayt tükettiği için yeterli). İlk hatada durur. */
#include "matoki/lexer.h"
#include "core/mem.h"

static b32 is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static b32 is_digit(char c) { return c >= '0' && c <= '9'; }
static b32 is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

typedef struct Kw {
    const char* s;
    TokKind     k;
} Kw;

static const Kw KEYWORDS[] = {
    { "fn", T_FN }, { "let", T_LET }, { "var", T_VAR }, { "if", T_IF }, { "else", T_ELSE },
    { "for", T_FOR }, { "in", T_IN }, { "return", T_RETURN }, { "uniform", T_UNIFORM },
    { "varying", T_VARYING }, { "true", T_TRUE }, { "false", T_FALSE }, { "discard", T_DISCARD }, { "attribute", T_ATTRIBUTE },
};

static const char* const TOK_NAMES[T_COUNT] = {
    "dosya sonu", "hata", "isim", "tamsayı", "ondalık sayı",
    "'('", "')'", "'{'", "'}'", "','", "';'", "':'", "'.'", "'..'", "'->'", "'@'",
    "'+'", "'-'", "'*'", "'/'", "'%'",
    "'<'", "'>'", "'<='", "'>='", "'=='", "'!='", "'&&'", "'||'", "'!'",
    "'='", "'+='", "'-='", "'*='", "'/='",
    "'&'", "'|'", "'^'", "'<<'", "'>>'", "'~'",
    "'fn'", "'let'", "'var'", "'if'", "'else'", "'for'", "'in'", "'return'", "'uniform'", "'varying'", "'true'", "'false'",
    "'discard'", "'attribute'",
};

const char* tok_name(TokKind k) { return k < T_COUNT ? TOK_NAMES[k] : "?"; }

static const f64 POW10[23] = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

f32 lex_parse_f32(const char* p, u32 len) {
    u64 mant = 0;
    i32 exp10 = 0, digits = 0;
    u32 i = 0;
    while (i < len && is_digit(p[i])) {
        if (digits < 19) { mant = mant * 10 + (u64)(p[i] - '0'); if (mant) digits++; }
        else exp10++;
        i++;
    }
    if (i < len && p[i] == '.') {
        i++;
        while (i < len && is_digit(p[i])) {
            if (digits < 19) { mant = mant * 10 + (u64)(p[i] - '0'); if (mant) digits++; exp10--; }
            i++;
        }
    }
    if (i < len && (p[i] == 'e' || p[i] == 'E')) {
        i++;
        i32 sign = 1, e = 0;
        if (i < len && (p[i] == '+' || p[i] == '-')) { if (p[i] == '-') sign = -1; i++; }
        while (i < len && is_digit(p[i])) { if (e < 10000) e = e * 10 + (p[i] - '0'); i++; }
        exp10 += sign * e;
    }
    f64 v = (f64)mant;
    if (mant == 0) return 0.0f;
    while (exp10 > 22) { v *= 1e22; exp10 -= 22; if (v > 1e300) break; }
    while (exp10 < -22) { v /= 1e22; exp10 += 22; if (v < 1e-300) break; }
    if (exp10 > 0) v *= POW10[exp10 > 22 ? 22 : exp10];
    else if (exp10 < 0) v /= POW10[-exp10 > 22 ? 22 : -exp10];
    return (f32)v;
}

static b32 fail(TokenList* out, i32 line, i32 col, const char* msg) {
    out->failed = TRUE;
    out->err_line = line;
    out->err_col = col;
    out->err_msg = msg;
    return FALSE;
}

b32 lex_source(Arena* a, const char* src, u32 len, TokenList* out) {
    mem_zero(out, sizeof(*out));
    out->toks = ARENA_ARRAY(a, Token, len + 1);
    u32 i = 0, n = 0;
    i32 line = 1;
    u32 line_start = 0;
    while (TRUE) {
        /* boşluk ve yorumlar */
        while (i < len) {
            char c = src[i];
            if (c == '\n') { line++; i++; line_start = i; }
            else if (c == ' ' || c == '\t' || c == '\r') i++;
            else if (c == '/' && i + 1 < len && src[i + 1] == '/') { while (i < len && src[i] != '\n') i++; }
            else if (c == '/' && i + 1 < len && src[i + 1] == '*') {
                i32 sl = line, sc = (i32)(i - line_start) + 1;
                i += 2;
                while (i + 1 < len && !(src[i] == '*' && src[i + 1] == '/')) {
                    if (src[i] == '\n') { line++; line_start = i + 1; }
                    i++;
                }
                if (i + 1 >= len) return fail(out, sl, sc, "kapanmamış blok yorum");
                i += 2;
            } else break;
        }
        Token* t = &out->toks[n];
        mem_zero(t, sizeof(*t));
        t->p = src + i;
        t->line = line;
        t->col = (i32)(i - line_start) + 1;
        if (i >= len) {
            t->kind = T_EOF;
            n++;
            break;
        }
        char c = src[i];
        u32 start = i;
        if (is_alpha(c)) {
            while (i < len && (is_alpha(src[i]) || is_digit(src[i]))) i++;
            t->kind = T_IDENT;
            t->len = i - start;
            for (u32 k = 0; k < ARRAY_COUNT(KEYWORDS); k++) {
                u32 kl = 0;
                while (KEYWORDS[k].s[kl]) kl++;
                if (kl == t->len && mem_equal(KEYWORDS[k].s, t->p, kl)) { t->kind = KEYWORDS[k].k; break; }
            }
        } else if (is_digit(c)) {
            if (c == '0' && i + 1 < len && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
                i += 2;
                u64 v = 0;
                u32 nd = 0;
                while (i < len && is_hex(src[i])) {
                    char h = src[i];
                    u32 d = is_digit(h) ? (u32)(h - '0') : (u32)((h | 32) - 'a' + 10);
                    v = v * 16 + d;
                    nd++;
                    i++;
                    if (v > 0xFFFFFFFFull) return fail(out, t->line, t->col, "onaltılık sayı 32 biti aşıyor");
                }
                if (!nd) return fail(out, t->line, t->col, "0x'ten sonra onaltılık basamak bekleniyor");
                t->kind = T_INT;
                t->ival = (i64)(i32)(u32)v;
            } else {
                b32 is_float = FALSE;
                while (i < len && is_digit(src[i])) i++;
                if (i + 1 < len && src[i] == '.' && src[i + 1] != '.') {
                    is_float = TRUE;
                    i++;
                    while (i < len && is_digit(src[i])) i++;
                } else if (i + 1 == len && src[i] == '.') {
                    is_float = TRUE;
                    i++;
                }
                if (i < len && (src[i] == 'e' || src[i] == 'E')) {
                    u32 j = i + 1;
                    if (j < len && (src[j] == '+' || src[j] == '-')) j++;
                    if (j < len && is_digit(src[j])) {
                        is_float = TRUE;
                        i = j;
                        while (i < len && is_digit(src[i])) i++;
                    }
                }
                if (i < len && is_alpha(src[i])) return fail(out, t->line, t->col, "sayının ardından harf gelemez");
                t->len = i - start;
                if (is_float) {
                    t->kind = T_FLOAT;
                    t->fval = lex_parse_f32(t->p, t->len);
                } else {
                    u64 v = 0;
                    for (u32 k = start; k < i; k++) {
                        v = v * 10 + (u64)(src[k] - '0');
                        if (v > 2147483648ull) return fail(out, t->line, t->col, "tamsayı i32 aralığının dışında");
                    }
                    t->kind = T_INT;
                    t->ival = (i64)v;
                }
            }
            t->len = i - start;
        } else {
            char d = i + 1 < len ? src[i + 1] : 0;
            TokKind k = T_ERROR;
            u32 w = 1;
            switch (c) {
            case '(': k = T_LPAREN; break;
            case ')': k = T_RPAREN; break;
            case '{': k = T_LBRACE; break;
            case '}': k = T_RBRACE; break;
            case ',': k = T_COMMA; break;
            case ';': k = T_SEMI; break;
            case ':': k = T_COLON; break;
            case '@': k = T_AT; break;
            case '~': k = T_TILDE; break;
            case '^': k = T_CARET; break;
            case '%': k = T_PERCENT; break;
            case '.': if (d == '.') { k = T_DOTDOT; w = 2; } else k = T_DOT; break;
            case '+': if (d == '=') { k = T_PLUSEQ; w = 2; } else k = T_PLUS; break;
            case '-':
                if (d == '>') { k = T_ARROW; w = 2; }
                else if (d == '=') { k = T_MINUSEQ; w = 2; }
                else k = T_MINUS;
                break;
            case '*': if (d == '=') { k = T_STAREQ; w = 2; } else k = T_STAR; break;
            case '/': if (d == '=') { k = T_SLASHEQ; w = 2; } else k = T_SLASH; break;
            case '<':
                if (d == '=') { k = T_LE; w = 2; }
                else if (d == '<') { k = T_SHL; w = 2; }
                else k = T_LT;
                break;
            case '>':
                if (d == '=') { k = T_GE; w = 2; }
                else if (d == '>') { k = T_SHR; w = 2; }
                else k = T_GT;
                break;
            case '=': if (d == '=') { k = T_EQEQ; w = 2; } else k = T_ASSIGN; break;
            case '!': if (d == '=') { k = T_NE; w = 2; } else k = T_BANG; break;
            case '&': if (d == '&') { k = T_ANDAND; w = 2; } else k = T_AMP; break;
            case '|': if (d == '|') { k = T_OROR; w = 2; } else k = T_PIPE; break;
            default: break;
            }
            if (k == T_ERROR) return fail(out, t->line, t->col, "beklenmeyen karakter");
            t->kind = k;
            t->len = w;
            i += w;
        }
        n++;
    }
    out->count = n;
    return TRUE;
}
