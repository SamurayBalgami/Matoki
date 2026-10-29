/* asset/json.h — RFC 8259 JSON ayrıştırıcısı (libc'siz, sıfırdan). Arena'da DOM üretir.
 * UTF-8 doğrulama, \uXXXX + vekil çiftleri, derinlik sınırı, satır:sütun hata konumu.
 * Sayılar f64: Clinger hızlı yolu (tam sonuç) + üs/mantis yedeği (hata sınırı testte ölçülür).
 * Yinelenen anahtarlarda json_get SON değeri döner (RFC "son kazanır" alışkanlığı).
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/asset/json.h), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#ifndef ASSET_JSON_H
#define ASSET_JSON_H

#include "core/types.h"
#include "core/arena.h"

#define JSON_MAX_DEPTH 256

typedef enum {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue  JsonValue;
typedef struct JsonMember JsonMember;

struct JsonValue {
    JsonType type;
    u32      count;          /* dizi eleman / nesne üye sayısı, dize bayt uzunluğu */
    union {
        b32         b;
        f64         num;
        const char* str;     /* UTF-8, NUL ile biter (kaçışlar çözülmüş) */
        JsonValue*  items;   /* [count] */
        JsonMember* members; /* [count] */
    } as;
};

struct JsonMember {
    const char* key;
    u32         key_len;
    JsonValue   value;
};

typedef struct {
    u32  line, col;          /* 1 tabanlı */
    char msg[96];
} JsonError;

b32 json_parse(const char* text, u32 len, Arena* a, JsonValue* out, JsonError* err);

/* Erişim yardımcıları (tip uymazsa NULL / varsayılan döner) */
const JsonValue* json_get(const JsonValue* obj, const char* key);
const JsonValue* json_at(const JsonValue* arr, u32 i);
f64         json_num(const JsonValue* v, f64 def);
i64         json_int(const JsonValue* v, i64 def);
const char* json_str(const JsonValue* v, const char* def);
b32         json_bool(const JsonValue* v, b32 def);
u32         json_len(const JsonValue* v);    /* dizi/nesne eleman sayısı, aksi 0 */

/* mant * 10^e10 → f64 (JSON sayı yolu; OBJ gibi metin biçimleri de kullanır) */
f64 json_make_f64(u64 mant, i32 e10);

/* Test/araç: f64 → kısa ondalık metin (gidiş-dönüş testi için yeterli hassasiyet, 17 anlamlı basamak) */
u32 json_write(const JsonValue* v, char* buf, u32 cap);

#endif
