/* asset/json.c — Özyinelemeli inişli JSON ayrıştırıcısı ve küçük yazıcı.
 * Diziler/nesneler önce arena'da bağlı liste olarak toplanır, sonra bitişik diziye kopyalanır.
 * Sayı dönüşümü: <= 19 anlamlı basamak u64 mantise, ondalık üs ayrı tutulur; mantis <= 2^53 ve
 * |üs| <= 22 ise sonuç tam (Clinger), aksi halde 1e22 parçalarıyla ölçekleme (hata testte ölçülür).
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/asset/json.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "asset/json.h"

typedef struct {
    const char* s;
    u32         len;
    u32         pos;
    u32         depth;
    Arena*      a;
    JsonError*  err;
    b32         failed;
} JP;

static void jp_fail(JP* p, const char* msg) {
    if (p->failed) return;
    p->failed = TRUE;
    if (!p->err) return;
    u32 line = 1, col = 1;
    for (u32 i = 0; i < p->pos && i < p->len; i++) {
        if (p->s[i] == '\n') { line++; col = 1; } else col++;
    }
    p->err->line = line;
    p->err->col  = col;
    u32 k = 0;
    while (msg[k] && k + 1 < sizeof(p->err->msg)) { p->err->msg[k] = msg[k]; k++; }
    p->err->msg[k] = 0;
}

static inline b32 jp_eof(const JP* p) { return p->pos >= p->len; }
static inline u8  jp_peek(const JP* p) { return jp_eof(p) ? 0 : (u8)p->s[p->pos]; }

static void jp_ws(JP* p) {
    while (!jp_eof(p)) {
        u8 c = (u8)p->s[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
        else break;
    }
}

static b32 jp_lit(JP* p, const char* lit) {
    u32 i = 0;
    while (lit[i]) {
        if (p->pos + i >= p->len || p->s[p->pos + i] != lit[i]) { jp_fail(p, "gecersiz sabit"); return FALSE; }
        i++;
    }
    p->pos += i;
    return TRUE;
}

/* ---------- Sayılar ---------- */

static const f64 k_pow10[23] = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

f64 json_make_f64(u64 mant, i32 e10) {
    if (mant == 0) return 0.0;
    if (mant <= (1ull << 53) && e10 >= -22 && e10 <= 22) {
        f64 m = (f64)mant;
        return e10 >= 0 ? m * k_pow10[e10] : m / k_pow10[-e10];
    }
    if (e10 > 330) return __builtin_inf();
    if (e10 < -360) return 0.0;
    f64 r = (f64)mant;
    while (e10 > 22)  { r *= 1e22; e10 -= 22; }
    while (e10 < -22) { r /= 1e22; e10 += 22; }
    return e10 >= 0 ? r * k_pow10[e10] : r / k_pow10[-e10];
}

static b32 jp_number(JP* p, JsonValue* out) {
    b32 neg = FALSE;
    if (jp_peek(p) == '-') { neg = TRUE; p->pos++; }
    u8 c = jp_peek(p);
    if (c < '0' || c > '9') { jp_fail(p, "sayi bekleniyordu"); return FALSE; }
    u64 mant = 0;
    u32 digits = 0;
    i32 e10 = 0;
    if (c == '0') {
        p->pos++;
        c = jp_peek(p);
        if (c >= '0' && c <= '9') { jp_fail(p, "basta sifir"); return FALSE; }
    } else {
        while ((c = jp_peek(p)) >= '0' && c <= '9') {
            if (digits < 19) { mant = mant * 10u + (u64)(c - '0'); if (mant) digits++; }
            else e10++;
            p->pos++;
        }
    }
    if (jp_peek(p) == '.') {
        p->pos++;
        c = jp_peek(p);
        if (c < '0' || c > '9') { jp_fail(p, "ondalik basamak bekleniyordu"); return FALSE; }
        while ((c = jp_peek(p)) >= '0' && c <= '9') {
            if (digits < 19) { mant = mant * 10u + (u64)(c - '0'); if (mant) digits++; e10--; }
            p->pos++;
        }
    }
    c = jp_peek(p);
    if (c == 'e' || c == 'E') {
        p->pos++;
        b32 eneg = FALSE;
        c = jp_peek(p);
        if (c == '+' || c == '-') { eneg = (c == '-'); p->pos++; }
        c = jp_peek(p);
        if (c < '0' || c > '9') { jp_fail(p, "us basamagi bekleniyordu"); return FALSE; }
        i32 ev = 0;
        while ((c = jp_peek(p)) >= '0' && c <= '9') {
            if (ev < 100000) ev = ev * 10 + (i32)(c - '0');
            p->pos++;
        }
        e10 += eneg ? -ev : ev;
    }
    f64 v = json_make_f64(mant, e10);
    out->type = JSON_NUMBER;
    out->count = 0;
    out->as.num = neg ? -v : v;
    return TRUE;
}

/* ---------- Dizeler ---------- */

static i32 jp_hex4(JP* p) {
    if (p->pos + 4 > p->len) return -1;
    i32 v = 0;
    for (u32 i = 0; i < 4; i++) {
        u8 c = (u8)p->s[p->pos + i];
        i32 d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return -1;
        v = v * 16 + d;
    }
    p->pos += 4;
    return v;
}

static u32 utf8_put(char* o, u32 cp) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}

/* Geçerli bir UTF-8 dizisinin uzunluğu (1..4), geçersizse 0 */
static u32 utf8_valid(const u8* s, u32 avail) {
    u8 c = s[0];
    if (c < 0x80) return 1;
    u32 n; u32 cp;
    if ((c & 0xE0) == 0xC0) { n = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07; }
    else return 0;
    if (n > avail) return 0;
    for (u32 i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000)) return 0;  /* fazla uzun */
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return n;
}

static b32 jp_string(JP* p, const char** out_str, u32* out_len) {
    p->pos++;   /* " */
    u32 start = p->pos;
    /* Ham uzunluk üst sınırı: kaçışlar çıktıyı uzatmaz (\uXXXX 6 bayt → en çok 4 bayt) */
    u32 end = start;
    while (end < p->len && p->s[end] != '"') {
        if (p->s[end] == '\\') end++;
        end++;
    }
    if (end >= p->len) { jp_fail(p, "kapanmamis dize"); return FALSE; }
    char* buf = (char*)arena_alloc(p->a, (usize)(end - start) + 1u, 1);
    u32 o = 0;
    while (!jp_eof(p)) {
        u8 c = (u8)p->s[p->pos];
        if (c == '"') { p->pos++; buf[o] = 0; *out_str = buf; *out_len = o; return TRUE; }
        if (c < 0x20) { jp_fail(p, "dizede kontrol karakteri"); return FALSE; }
        if (c == '\\') {
            p->pos++;
            u8 e = jp_peek(p);
            p->pos++;
            switch (e) {
            case '"':  buf[o++] = '"'; break;
            case '\\': buf[o++] = '\\'; break;
            case '/':  buf[o++] = '/'; break;
            case 'b':  buf[o++] = '\b'; break;
            case 'f':  buf[o++] = '\f'; break;
            case 'n':  buf[o++] = '\n'; break;
            case 'r':  buf[o++] = '\r'; break;
            case 't':  buf[o++] = '\t'; break;
            case 'u': {
                i32 cp = jp_hex4(p);
                if (cp < 0) { jp_fail(p, "gecersiz \\u kacisi"); return FALSE; }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (p->pos + 2 > p->len || p->s[p->pos] != '\\' || p->s[p->pos + 1] != 'u') {
                        jp_fail(p, "eslesmemis yuksek vekil"); return FALSE;
                    }
                    p->pos += 2;
                    i32 lo = jp_hex4(p);
                    if (lo < 0xDC00 || lo > 0xDFFF) { jp_fail(p, "gecersiz dusuk vekil"); return FALSE; }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    jp_fail(p, "eslesmemis dusuk vekil"); return FALSE;
                }
                o += utf8_put(buf + o, (u32)cp);
            } break;
            default:
                jp_fail(p, "gecersiz kacis");
                return FALSE;
            }
            continue;
        }
        u32 n = utf8_valid((const u8*)p->s + p->pos, p->len - p->pos);
        if (n == 0) { jp_fail(p, "gecersiz UTF-8"); return FALSE; }
        for (u32 i = 0; i < n; i++) buf[o++] = p->s[p->pos + i];
        p->pos += n;
    }
    jp_fail(p, "kapanmamis dize");
    return FALSE;
}

/* ---------- Değerler ---------- */

typedef struct JNode { JsonValue v; struct JNode* next; } JNode;
typedef struct JMNode { JsonMember m; struct JMNode* next; } JMNode;

static b32 jp_value(JP* p, JsonValue* out);

static b32 jp_array(JP* p, JsonValue* out) {
    p->pos++;
    JNode *head = NULL, *tail = NULL;
    u32 n = 0;
    jp_ws(p);
    if (jp_peek(p) == ']') { p->pos++; goto done; }
    for (;;) {
        JNode* nd = ARENA_NEW(p->a, JNode);
        if (!jp_value(p, &nd->v)) return FALSE;
        if (tail) tail->next = nd; else head = nd;
        tail = nd;
        n++;
        jp_ws(p);
        u8 c = jp_peek(p);
        if (c == ',') { p->pos++; jp_ws(p); continue; }
        if (c == ']') { p->pos++; break; }
        jp_fail(p, "',' veya ']' bekleniyordu");
        return FALSE;
    }
done:
    out->type = JSON_ARRAY;
    out->count = n;
    out->as.items = n ? ARENA_ARRAY(p->a, JsonValue, n) : NULL;
    u32 i = 0;
    for (JNode* nd = head; nd; nd = nd->next) out->as.items[i++] = nd->v;
    return TRUE;
}

static b32 jp_object(JP* p, JsonValue* out) {
    p->pos++;
    JMNode *head = NULL, *tail = NULL;
    u32 n = 0;
    jp_ws(p);
    if (jp_peek(p) == '}') { p->pos++; goto done; }
    for (;;) {
        if (jp_peek(p) != '"') { jp_fail(p, "anahtar (dize) bekleniyordu"); return FALSE; }
        JMNode* nd = ARENA_NEW(p->a, JMNode);
        if (!jp_string(p, &nd->m.key, &nd->m.key_len)) return FALSE;
        jp_ws(p);
        if (jp_peek(p) != ':') { jp_fail(p, "':' bekleniyordu"); return FALSE; }
        p->pos++;
        if (!jp_value(p, &nd->m.value)) return FALSE;
        if (tail) tail->next = nd; else head = nd;
        tail = nd;
        n++;
        jp_ws(p);
        u8 c = jp_peek(p);
        if (c == ',') { p->pos++; jp_ws(p); continue; }
        if (c == '}') { p->pos++; break; }
        jp_fail(p, "',' veya '}' bekleniyordu");
        return FALSE;
    }
done:
    out->type = JSON_OBJECT;
    out->count = n;
    out->as.members = n ? ARENA_ARRAY(p->a, JsonMember, n) : NULL;
    u32 i = 0;
    for (JMNode* nd = head; nd; nd = nd->next) out->as.members[i++] = nd->m;
    return TRUE;
}

static b32 jp_value(JP* p, JsonValue* out) {
    jp_ws(p);
    if (jp_eof(p)) { jp_fail(p, "beklenmeyen dosya sonu"); return FALSE; }
    u8 c = jp_peek(p);
    out->count = 0;
    switch (c) {
    case '{':
    case '[': {
        if (++p->depth > JSON_MAX_DEPTH) { jp_fail(p, "ic ice derinlik siniri asildi"); return FALSE; }
        b32 ok = (c == '{') ? jp_object(p, out) : jp_array(p, out);
        p->depth--;
        return ok;
    }
    case '"': {
        const char* s; u32 l;
        if (!jp_string(p, &s, &l)) return FALSE;
        out->type = JSON_STRING; out->as.str = s; out->count = l;
        return TRUE;
    }
    case 't': out->type = JSON_BOOL; out->as.b = TRUE;  return jp_lit(p, "true");
    case 'f': out->type = JSON_BOOL; out->as.b = FALSE; return jp_lit(p, "false");
    case 'n': out->type = JSON_NULL; return jp_lit(p, "null");
    default:
        if (c == '-' || (c >= '0' && c <= '9')) return jp_number(p, out);
        jp_fail(p, "beklenmeyen karakter");
        return FALSE;
    }
}

b32 json_parse(const char* text, u32 len, Arena* a, JsonValue* out, JsonError* err) {
    JP p = { text, len, 0, 0, a, err, FALSE };
    if (err) { err->line = 0; err->col = 0; err->msg[0] = 0; }
    /* UTF-8 BOM'u yoksay (RFC 8259 §8.1 izin verir) */
    if (len >= 3 && (u8)text[0] == 0xEF && (u8)text[1] == 0xBB && (u8)text[2] == 0xBF) p.pos = 3;
    if (!jp_value(&p, out)) return FALSE;
    jp_ws(&p);
    if (!jp_eof(&p)) { jp_fail(&p, "degerden sonra fazladan veri"); return FALSE; }
    return TRUE;
}

/* ---------- Erişim ---------- */

static b32 key_eq(const JsonMember* m, const char* key) {
    u32 i = 0;
    for (; i < m->key_len; i++) {
        if (key[i] == 0 || key[i] != m->key[i]) return FALSE;
    }
    return key[i] == 0;
}

const JsonValue* json_get(const JsonValue* obj, const char* key) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (u32 i = obj->count; i-- > 0;) {
        if (key_eq(&obj->as.members[i], key)) return &obj->as.members[i].value;
    }
    return NULL;
}

const JsonValue* json_at(const JsonValue* arr, u32 i) {
    if (!arr || arr->type != JSON_ARRAY || i >= arr->count) return NULL;
    return &arr->as.items[i];
}

f64 json_num(const JsonValue* v, f64 def) {
    return (v && v->type == JSON_NUMBER) ? v->as.num : def;
}

i64 json_int(const JsonValue* v, i64 def) {
    if (!v || v->type != JSON_NUMBER) return def;
    f64 x = v->as.num;
    if (!(x >= -9.0e18 && x <= 9.0e18)) return def;
    return (i64)x;
}

const char* json_str(const JsonValue* v, const char* def) {
    return (v && v->type == JSON_STRING) ? v->as.str : def;
}

b32 json_bool(const JsonValue* v, b32 def) {
    return (v && v->type == JSON_BOOL) ? v->as.b : def;
}

u32 json_len(const JsonValue* v) {
    return (v && (v->type == JSON_ARRAY || v->type == JSON_OBJECT)) ? v->count : 0;
}

/* ---------- Yazıcı ---------- */

typedef struct { char* b; u32 cap, n; } JW;

static void jw_c(JW* w, char c) { if (w->n + 1 < w->cap) w->b[w->n] = c; w->n++; }
static void jw_s(JW* w, const char* s) { while (*s) jw_c(w, *s++); }

static void jw_num(JW* w, f64 x) {
    if (x != x || x - x != 0.0) { jw_s(w, "null"); return; }   /* NaN/inf JSON'da yok */
    if (x < 0.0) { jw_c(w, '-'); x = -x; }
    if (x == 0.0) { jw_c(w, '0'); return; }
    if (x < 1e15 && x == (f64)(u64)x) {
        char t[24]; u32 k = 0; u64 v = (u64)x;
        while (v) { t[k++] = (char)('0' + v % 10u); v /= 10u; }
        while (k) jw_c(w, t[--k]);
        return;
    }
    /* 17 anlamlı basamak, bilimsel gösterim */
    i32 e = 0;
    while (x >= 10.0) { x /= 10.0; e++; }
    while (x < 1.0)   { x *= 10.0; e--; }
    for (u32 i = 0; i < 17; i++) {
        u32 d = (u32)x;
        if (d > 9u) d = 9u;
        jw_c(w, (char)('0' + d));
        if (i == 0) jw_c(w, '.');
        x = (x - (f64)d) * 10.0;
    }
    jw_c(w, 'e');
    if (e < 0) { jw_c(w, '-'); e = -e; }
    char t[8]; u32 k = 0;
    do { t[k++] = (char)('0' + e % 10); e /= 10; } while (e);
    while (k) jw_c(w, t[--k]);
}

static void jw_str(JW* w, const char* s, u32 len) {
    static const char hex[] = "0123456789abcdef";
    jw_c(w, '"');
    for (u32 i = 0; i < len; i++) {
        u8 c = (u8)s[i];
        if (c == '"' || c == '\\') { jw_c(w, '\\'); jw_c(w, (char)c); }
        else if (c < 0x20) { jw_s(w, "\\u00"); jw_c(w, hex[c >> 4]); jw_c(w, hex[c & 15]); }
        else jw_c(w, (char)c);
    }
    jw_c(w, '"');
}

static void jw_value(JW* w, const JsonValue* v) {
    switch (v->type) {
    case JSON_NULL:   jw_s(w, "null"); break;
    case JSON_BOOL:   jw_s(w, v->as.b ? "true" : "false"); break;
    case JSON_NUMBER: jw_num(w, v->as.num); break;
    case JSON_STRING: jw_str(w, v->as.str, v->count); break;
    case JSON_ARRAY:
        jw_c(w, '[');
        for (u32 i = 0; i < v->count; i++) { if (i) jw_c(w, ','); jw_value(w, &v->as.items[i]); }
        jw_c(w, ']');
        break;
    case JSON_OBJECT:
        jw_c(w, '{');
        for (u32 i = 0; i < v->count; i++) {
            if (i) jw_c(w, ',');
            jw_str(w, v->as.members[i].key, v->as.members[i].key_len);
            jw_c(w, ':');
            jw_value(w, &v->as.members[i].value);
        }
        jw_c(w, '}');
        break;
    }
}

u32 json_write(const JsonValue* v, char* buf, u32 cap) {
    JW w = { buf, cap, 0 };
    jw_value(&w, v);
    if (cap) buf[w.n < cap ? w.n : cap - 1] = 0;
    return w.n;
}
