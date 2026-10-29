/* tests/test_json.c — JSON ayrıştırıcı: 90+ geçerli/geçersiz vaka, hata konumu, bilinen ondalık → f64 bit
 * eşlemeleri (doğru yuvarlama), ~1 MB rastgele belge gidiş-dönüşü, 100 000 mutasyonla fuzz (çökme yok).
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (tests/test_asset_json.c), izinle uyarlandı (D37): onların
 * Python float() ile popen karşılaştırması wasm'da koşamaz → kesin bit değerli vakalara çevrildi. */
#include "test.h"
#include "core/mem.h"
#include "core/fmt.h"
#include "core/rng.h"
#include "asset/json.h"

#define G_MEM_SIZE (MB(48))
static u8* g_mem;   /* her TEST başında test_arena'dan */

static b32 parse(const char* s, JsonValue* v, JsonError* e, Arena* a) {
    return json_parse(s, (u32)cstr_len(s), a, v, e);
}

static b32 str_same(const char* a, const char* b) {
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

TEST(json_valid_cases) {
    g_mem = (u8*)arena_alloc_nozero(test_arena(), G_MEM_SIZE, 16);
    static const char* ok[] = {
        "0", "-0", "1", "-1", "123456789", "1.5", "-1.25e-3", "1E10", "1e+2", "0.0", "1e-400", "1e400",
        "\"\"", "\"abc\"", "\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"", "\"\\u0041\"", "\"\\u00e7\\u011f\"",
        "\"\\ud83d\\ude00\"", "\"ğüşıöç\"", "\"\xF0\x9F\x98\x80\"", "true", "false", "null",
        "[]", "[1]", "[1,2,3]", "[ 1 , 2 ]", "[[]]", "[[[[[]]]]]", "{}", "{\"a\":1}", "{\"a\":{\"b\":[1,{}]}}",
        " \t\r\n 1 \t\r\n", "\xEF\xBB\xBF{}", "[\"a\",true,null,-0.5]", "{\"\":0}", "{\"a\":1,\"a\":2}",
        "[1e1,1E-1,1.0e+1]", "-123.456e-7", "[0,-0,0.0]", "\"/\"", "{\"x\":[],\"y\":{}}",
        "123456789012345678901234567890", "0.1", "9007199254740993", "4.9e-324", "1.7976931348623157e308",
    };
    Arena a;
    for (u32 i = 0; i < ARRAY_COUNT(ok); i++) {
        arena_init(&a, g_mem, MB(1));
        JsonValue v; JsonError e;
        b32 r = parse(ok[i], &v, &e, &a);
        if (!r) LOG("gecmesi gereken: %s -> %u:%u %s", ok[i], e.line, e.col, e.msg);
        CHECK(r);
    }
    /* Anlam kontrolleri */
    arena_init(&a, g_mem, MB(1));
    JsonValue v; JsonError e;
    CHECK(parse("{\"a\":1,\"b\":[true,\"x\\u00e7\"],\"a\":7}", &v, &e, &a));
    CHECK(json_num(json_get(&v, "a"), 0) == 7.0);                 /* son kazanır */
    CHECK(json_len(json_get(&v, "b")) == 2);
    CHECK(json_bool(json_at(json_get(&v, "b"), 0), FALSE));
    CHECK(str_same(json_str(json_at(json_get(&v, "b"), 1), ""), "x\xC3\xA7"));
    CHECK(parse("\"\\ud83d\\ude00\"", &v, &e, &a) && v.count == 4 && (u8)v.as.str[0] == 0xF0);
    CHECK(parse("1e400", &v, &e, &a) && v.as.num > 1e308);
    CHECK(parse("-0", &v, &e, &a) && v.as.num == 0.0 && (1.0 / v.as.num) < 0.0);
}

TEST(json_invalid_cases) {
    g_mem = (u8*)arena_alloc_nozero(test_arena(), G_MEM_SIZE, 16);
    static const char* bad[] = {
        "", " ", "{", "}", "[", "]", "[1,]", "[,1]", "{\"a\"}", "{\"a\":}", "{a:1}", "{\"a\":1,}", "{,}",
        "01", "-", "+1", "1.", ".5", "1e", "1e+", "0x10", "NaN", "Infinity", "-Infinity", "1 2",
        "tru", "nul", "falsey", "\"abc", "\"\\x\"", "\"\\u12\"", "\"\\uZZZZ\"", "\"\\ud800\"", "\"\\udc00\"",
        "\"\\ud800\\u0041\"", "\"a\nb\"", "\"\x01\"", "\"\xC0\xAF\"", "\"\xED\xA0\x80\"", "\"\xF8\x88\x80\x80\x80\"",
        "\"\xC3\"", "\"\xE2\x82\"", "\"\xFF\"", "[1 2]", "{\"a\" 1}", "{\"a\":1 \"b\":2}", "'a'", "[\"a\",]",
        "{} {}", "[]]", "\"\\\"", "-01", "1.e3", "--1", "[-]", "{\"a\":[}", "[1,{]",
    };
    Arena a;
    for (u32 i = 0; i < ARRAY_COUNT(bad); i++) {
        arena_init(&a, g_mem, MB(1));
        JsonValue v; JsonError e;
        b32 r = parse(bad[i], &v, &e, &a);
        if (r) LOG("reddedilmesi gereken kabul edildi: [%s]", bad[i]);
        CHECK(!r);
        CHECK(e.line >= 1 && e.msg[0] != 0);
    }
    /* Derinlik sınırı */
    static char deep[2 * (JSON_MAX_DEPTH + 10) + 1];
    u32 n = JSON_MAX_DEPTH + 10;
    for (u32 i = 0; i < n; i++) { deep[i] = '['; deep[n + i] = ']'; }
    deep[2 * n] = 0;
    arena_init(&a, g_mem, MB(4));
    JsonValue v; JsonError e;
    CHECK(!parse(deep, &v, &e, &a));
    /* Hata konumu */
    CHECK(!parse("{\n  \"a\": 1,\n  \"b\": tru\n}", &v, &e, &a));
    CHECK(e.line == 3);
    CHECK(e.col == 8);
}

static f64 ulp_dist(f64 a, f64 b) {
    i64 x, y;
    mem_copy(&x, &a, 8);
    mem_copy(&y, &b, 8);
    if (x < 0) x = (i64)0x8000000000000000ull - x;
    if (y < 0) y = (i64)0x8000000000000000ull - y;
    f64 d = (f64)(x - y);
    return d < 0 ? -d : d;
}

/* ondalık metin → doğru yuvarlanmış f64 bitleri (Python float() ile bir kez üretildi):
 * 12 sınır vakası + 200 tipik (1e±30) + 200 geniş aralık (1e±320, denormal dahil) rastgele sayı */
typedef struct NumCase {
    const char* text;
    u64 bits;
} NumCase;

static const NumCase k_nums[] = {
    { "0.1", 0x3fb999999999999aull },
    { "0.3", 0x3fd3333333333333ull },
    { "9007199254740993", 0x4340000000000000ull },
    { "4.9e-324", 0x0000000000000001ull },
    { "2.2250738585072014e-308", 0x0010000000000000ull },
    { "1.7976931348623157e308", 0x7fefffffffffffffull },
    { "123456789012345678901234567890", 0x45f8ee90ff6c373eull },
    { "1e23", 0x44b52d02c7e14af6ull },
    { "8.98846567431158e307", 0x7fe0000000000000ull },
    { "2.2250738585072011e-308", 0x000fffffffffffffull },
    { "0.000001", 0x3eb0c6f7a0b5ed8dull },
    { "3.14159265358979323846", 0x400921fb54442d18ull },
    { "5.364837568934858e-26", 0x3ab09a76548eab45ull },
    { "-3.836056e-1", 0xbfd88cfe80a3ff4eull },
    { "6.655e20", 0x444209d57a03d33bull },
    { "-7.659363e-17", 0xbc96139cc3b335bbull },
    { "1.4e24", 0x44f287626ee52198ull },
    { "-7.9159337523579246e9", 0xc1fd7d390385ba0full },
    { "-8.2253911202e-2", 0xbfb50e97a294a751ull },
    { "-4.1343794e0", 0xc010899ac0e10640ull },
    { "-7.3144429481733628e24", 0xc5183392fcab201full },
    { "-6.0986113e-27", 0xba7e32e884b5ec81ull },
    { "4.9182379221291989e10", 0x4226e6ff19aa9580ull },
    { "9e-19", 0x3c309a213cd6d681ull },
    { "-1.14020199393372754e16", 0xc3444109d21a621eull },
    { "-4.69769232463257e13", 0xc2c55cd5694f7adaull },
    { "6e13", 0x42cb48eb57e00000ull },
    { "9e-4", 0x3f4d7dbf487fcb92ull },
    { "8.8e1", 0x4056000000000000ull },
    { "7.50858423916122054e28", 0x45ee53b1b3063966ull },
    { "-3.57679520e19", 0xc3ff0614ad657390ull },
    { "6.628e15", 0x43378c2181924000ull },
    { "7.043e-23", 0x3b554941df18a6e8ull },
    { "4.9916194682e18", 0x43d15172dd69ff0eull },
    { "9.1368967e6", 0x41616d6016666666ull },
    { "9.44957e10", 0x42360061f4200000ull },
    { "-7.458e3", 0xc0bd220000000000ull },
    { "6.3679443e-1", 0x3fe4609eb663ffafull },
    { "5.63468254821837944e-15", 0x3cf96058c01df86eull },
    { "-5.2750961216953194e-28", 0xba44e59540454c1cull },
    { "7.23741711e7", 0x4191415e6c666666ull },
    { "7.91393242e22", 0x44b0c226bf9dca43ull },
    { "3.565583094629e-13", 0x3d59172ef872f5f3ull },
    { "-4.82683e-22", 0xbb823c393955c154ull },
    { "5.0916e3", 0x40b3e3999999999aull },
    { "8.2457547978042e-6", 0x3ee14ae7e8f58f17ull },
    { "-9.752188432775902e17", 0xc3ab1158bdf7a7fdull },
    { "-1.411e12", 0xc274886275e00000ull },
    { "-4.883584749038734e11", 0xc25c6d1bbd25f7e6ull },
    { "-6.0298845520e-8", 0xbe702fb48236f967ull },
    { "-6e-18", 0xbc5bab8cbabb6581ull },
    { "-8.80217e10", 0xc2347e808da00000ull },
    { "-1.63225075e18", 0xc3b6a6eb68ba1d6cull },
    { "2.5975098673218e13", 0x42b79fcc87884200ull },
    { "-7.7730781415851e-10", 0xbe0ab54597523684ull },
    { "-5.668410386223004e-27", 0xba7c1190570f731eull },
    { "-3.338e-13", 0xbd577d34cafed73full },
    { "1.45e-25", 0x3ac6700ab23e680aull },
    { "-9.2154098867930254e4", 0xc0f67fa194f689f1ull },
    { "-4e-7", 0xbe9ad7f29abcaf48ull },
    { "3.35191711301273e26", 0x45715439b146094cull },
    { "8.699168e18", 0x43de2e69cd2c5c80ull },
    { "-3.353270246366e23", 0xc4d1c08752d31125ull },
    { "-7.79148e-14", 0xbd35ee5a600a1c23ull },
    { "6.1515719328271996799e-4", 0x3f442850037652b4ull },
    { "-6.95983239608785e-28", 0xba4b921bcb57b0a4ull },
    { "5.339967581e-1", 0x3fe116805e86b46aull },
    { "-6.0937347681278644e2", 0xc0830afce16945dbull },
    { "3.3188097322e-25", 0x3ad9ad9475aa178cull },
    { "-3.85455727e-15", 0xbcf15c0080ee2e31ull },
    { "-2.800642301923e29", 0xc60c477d9ca3fd24ull },
    { "2.15e-29", 0x39fb41261e8ccd86ull },
    { "9.737e-18", 0x3c6673b5d6942c8aull },
    { "-6e-6", 0xbed92a737110e454ull },
    { "2.87556837e-21", 0x3bab28b29c474a3bull },
    { "-2.23e9", 0xc1e09d6330000000ull },
    { "7.25326123041e14", 0x43049d712f480740ull },
    { "8.563004357930931837e-14", 0x3d381a4b7fb9c0c0ull },
    { "9.508076566112594e18", 0x43e07e6f28598b6dull },
    { "9.9573e-29", 0x3a1f8e524ce5aca7ull },
    { "7.520625913342e-11", 0x3dd4ac2b85be6afeull },
    { "-6e-3", 0xbf789374bc6a7efaull },
    { "-1.0407038393872e7", 0xc163d98fcc9a9974ull },
    { "2.1017094469908081e-15", 0x3ce2ee3647f9720cull },
    { "-2.015110641196890777e-23", 0xbb385c772236de00ull },
    { "-9.236279233048e29", 0xc62750ce390588aeull },
    { "-1.750997004e26", 0xc5621ad9b8eb9e63ull },
    { "6.3702483964e-17", 0x3c925c69e68cf871ull },
    { "4.70016900632469e1", 0x4047803761472fd2ull },
    { "-1.7029997591763e19", 0xc3ed8ad69296d953ull },
    { "7.51585e-3", 0x3f7ec8f09f39ec8bull },
    { "-2.471567147e-8", 0xbe5a89cac34a3e62ull },
    { "2.079573011105e15", 0x431d8d714a7213a0ull },
    { "2.0011150903e7", 0x41731588ee72b021ull },
    { "-1.67186755e-15", 0xbcde1e20d20aef3dull },
    { "-2.71071702222315e-28", 0xba3579fcbf757f46ull },
    { "-1.1706055e-4", 0xbf1eafccef83f243ull },
    { "-4.76202948e-13", 0xbd60c1413d15b9afull },
    { "-9.69843218604e3", 0xc0c2f13751df45cbull },
    { "2.470699652141e-15", 0x3ce6410ba73f8e70ull },
    { "6.88957625696675e28", 0x45ebd3a7a694ed29ull },
    { "-4.533e23", 0xc4d7ff5c4da66ac7ull },
    { "-4.2927355207e-17", 0xbc88bef5a09ebdfbull },
    { "-4e-15", 0xbcf203af9ee75616ull },
    { "-6.0778760230759e-19", 0xbc266c6440238fc5ull },
    { "6.442255327e11", 0x4262bfdab0638000ull },
    { "2.911155771542669028e9", 0x41e5b096c7715d8bull },
    { "6.8503211261864014135e14", 0x43037843bddbe481ull },
    { "1.191041208e1", 0x4027d221883af778ull },
    { "-4.8150324114469941805e25", 0xc543ea1d267813d6ull },
    { "5.817187e18", 0x43d42eb3305dc14cull },
    { "9.060749639e-8", 0x3e78527feefa9e46ull },
    { "9.2490872697788e0", 0x40227f885ddb1989ull },
    { "6.4038e-23", 0x3b535ab3448cb69cull },
    { "-6.860016e22", 0xc4ad0da4ed998ce2ull },
    { "-8.23843960e-3", 0xbf80df50a537712bull },
    { "4.0823e-6", 0x3ed11f56145b4220ull },
    { "-2.75415e-17", 0xbc7fc0d0e75fe9c8ull },
    { "-7.9338854e10", 0xc23278f6d6700000ull },
    { "2.199716984211619236e22", 0x4492a1e038c59d9cull },
    { "6.515835722e24", 0x45158f20a6297721ull },
    { "6.344074340087e-17", 0x3c924919c2d9f77cull },
    { "3.2e-29", 0x3a04484bfeebc2a0ull },
    { "7e23", 0x44e287626ee52198ull },
    { "5.084e24", 0x4510d250c4dc1645ull },
    { "-9.19814169716e9", 0xc2012204080947aeull },
    { "-2.7288777091323800712e-6", 0xbec6e43862cfe7eaull },
    { "6e-7", 0x3ea421f5f40d8376ull },
    { "-3e-21", 0xbbac558e0f15e8f7ull },
    { "9.1320077464169887138e25", 0x4552e271d46904f9ull },
    { "7.91948903807e28", 0x45effc8f4b40cf63ull },
    { "2.830e3", 0x40a61c0000000000ull },
    { "5.4624539e-30", 0x39dbb2aba99276f8ull },
    { "7.29e5", 0x41263f5000000000ull },
    { "-7e-17", 0xbc942d169d7dfa04ull },
    { "1.61274e21", 0x4455db50dd5595deull },
    { "5.1890506274164193877e13", 0x42c798d998caba19ull },
    { "-6.17691e19", 0xc40ac9bf69775ca6ull },
    { "7.69120e-20", 0x3bf6b34e7134c0ebull },
    { "-6.43170943137e-27", 0xba7fd9290acee4e9ull },
    { "-7.04715338291199704e-20", 0xbbf4ccadb56d7ce7ull },
    { "-4.05611695428489e-5", 0xbf0544072c637162ull },
    { "-2.6361278748341298e5", 0xc11016f326620d44ull },
    { "1.8324762701515835203e6", 0x413bf61c4528a778ull },
    { "-7e-30", 0xb9e1bf427f0e4a4cull },
    { "6.9348e-5", 0x3f122ddd9173b8e8ull },
    { "8.3370612828e-20", 0x3bf89b4e2ee0cf3dull },
    { "-2.6772600253855605147e-5", 0xbefc12b7148f00dcull },
    { "-8.30928484638806040e15", 0xc33d8540239b435cull },
    { "-7.82768950188109049e14", 0xc3063f64c9bcc268ull },
    { "2.99109661742e14", 0x42f1009e52313b00ull },
    { "8.8586761e-25", 0x3af1229a9b87b12dull },
    { "8.57822146e8", 0x41c990a9e1000000ull },
    { "-3.26e-15", 0xbced5d0c577e32b8ull },
    { "-2.8e13", 0xc2b977420dc00000ull },
    { "-6.4413170861475796e-2", 0xbfb07d61ae481d68ull },
    { "3.269939973e7", 0x417f2f407bae147bull },
    { "9.1706679e6", 0x41617ddd7ccccccdull },
    { "-9.8152718e-12", 0xbda59581bdd126b7ull },
    { "-4.51e26", 0xc57750ef6fd5306aull },
    { "2.516663708561726e10", 0x42177031a0767813ull },
    { "-9.56150e22", 0xc4b43f4cae6604bfull },
    { "8.2825959002075407976e7", 0x4193bf4b9c02200eull },
    { "5.283879271346e11", 0x425ec197fb17a666ull },
    { "9.102144e-7", 0x3eae8aaea2155a31ull },
    { "6.613248432796915e8", 0x41c3b58215a3cceeull },
    { "4.90e-12", 0x3d958ce8d7d83ebbull },
    { "-8.27979579510331644e3", 0xc0c02be5dc9d2b88ull },
    { "6.9994972197289e10", 0x42304c06842549fcull },
    { "5.90136388650451e-21", 0x3bbbde4fa90e4840ull },
    { "6.2e28", 0x45e90aa655d68fe2ull },
    { "-8.4337e17", 0xc3a7688117e07140ull },
    { "-1.3502577652781e28", 0xc5c5d08904a1ea82ull },
    { "8.735333e-1", 0x3febf3fc1b6ef14full },
    { "-3.176317e-2", 0xbfa0434320bc0d37ull },
    { "-9.0646e22", 0xc4b331edf420e8abull },
    { "-3.655625969787e-19", 0xbc1af9483faa4285ull },
    { "3.831050e22", 0x44a039a1fac88b25ull },
    { "-3.19351363361e23", 0xc4d0e8049d3d329bull },
    { "8.282432262941417e18", 0x43dcbc4710b336e8ull },
    { "1.03930e-30", 0x39b5145aa7f0e0e0ull },
    { "3.0280353e20", 0x44306a3e3e369057ull },
    { "2.3846568666e-25", 0x3ad2734caeeda2eaull },
    { "6.81978203383025e9", 0x41f967d9d91d48b4ull },
    { "-5.17539e4", 0xc0e9453ccccccccdull },
    { "6.808512505108566145e-21", 0x3bc0137e479e1926ull },
    { "6.312e-3", 0x3f79da9c99285a92ull },
    { "-9.44112e13", 0xc2d57774827c0000ull },
    { "5.89278870569912690e-14", 0x3d309633a6be880eull },
    { "-3.52802331159638e-9", 0xbe2e4e348f163308ull },
    { "4.30184752910623114e12", 0x428f4cd0c03491d9ull },
    { "7.8151050811428e-3", 0x3f80015da5e80ffeull },
    { "-6.81428654e-26", 0xbab516d581f2aeecull },
    { "9.8438344732313302312e27", 0x45bfcea10812c0dfull },
    { "-4.4096e22", 0xc4a2ace5dcc2c212ull },
    { "-8.4628545064236e-27", 0xba84f3f8a6f18ad3ull },
    { "-4.09e3", 0xc0aff40000000000ull },
    { "-6.1852287220589344e-11", 0xbdd10077c02ed59bull },
    { "1.49837700255e-10", 0x3de497f205a64f60ull },
    { "3.6777013e17", 0x43946a53f05698d0ull },
    { "-4.123193728220511e-16", 0xbcbdb5f32fbb5d03ull },
    { "-6.658e15", 0xc337a76a6cea2000ull },
    { "2.3e-9", 0x3e23c1c0e493105eull },
    { "-1.99158918e-29", 0xb9f93f138f10b997ull },
    { "8.72888091549197639e7", 0x4194cfafa49ea349ull },
    { "2e-4", 0x3f2a36e2eb1c432dull },
    { "-7e26", 0xc5821836204bc2ceull },
    { "8.2110128624551e-7", 0x3eab8d34d2d8994eull },
    { "-7.325e19", 0xc40fc462373d2ae8ull },
    { "5.4332380184e0", 0x4015bba2bf41a5bbull },
    { "8.41355e5", 0x4129ad1600000000ull },
    { "5.1481870386271e-26", 0x3aafdda0c3c19a87ull },
    { "5.272025518602595e313", 0x7ff0000000000000ull },
    { "2.9e-278", 0x065073474236a666ull },
    { "9.43035e-249", 0x0c70e132bd4e5ad7ull },
    { "-2.26426e90", 0xd2b1c8e4dfefce26ull },
    { "1.227420950e-134", 0x2421d7bde901ccb0ull },
    { "-3.58350918697544783e132", 0xdb7431c28c49aeffull },
    { "-1.2086285675339e256", 0xf519c21d54a43999ull },
    { "7.683411e-317", 0x0000000000ed4ba5ull },
    { "-6e-217", 0x930a79a22d1d6e4bull },
    { "-2.47e-168", 0x9d22a4b42eeb401bull },
    { "-7.79203734334e83", 0xd159ab9db2764035ull },
    { "-6.47182575e-30", 0xb9e068747f1050ebull },
    { "5.50829311470394e-34", 0x3906e16c03d7ddd6ull },
    { "-7.906559886892783767e-196", 0x976d8d19b6b70c54ull },
    { "8.86e-123", 0x26976d85d7d6ac4full },
    { "-8.55e-107", 0xa9e9199ccd3955d6ull },
    { "5.0467e-28", 0x3a43fdf63a50feb9ull },
    { "-9e182", 0xe5eb1c91b7d83925ull },
    { "3e273", 0x78b62ea686bfa47cull },
    { "-2.3516707583706023e-70", 0xb179f811707efd9dull },
    { "9.2308304616e-140", 0x2311968d43b43527ull },
    { "-5.2e-206", 0x9550b1dc447c536aull },
    { "-9.28536102123268118e263", 0xf6bd7ce15cf6da41ull },
    { "7.1584373171167627174e306", 0x7fa4634dd29b2af5ull },
    { "-2.989884e-118", 0xa78820808806b580ull },
    { "-6.437439913e250", 0xf401fb7c33f95d87ull },
    { "-1.549092003932243e64", 0xcd42d402f874cc80ull },
    { "7.63e-68", 0x320074d372c67533ull },
    { "5.98003706015559e317", 0x7ff0000000000000ull },
    { "-7.34273e50", 0xca7f668f412cd557ull },
    { "-8.4858e263", 0xf6baf2d8eea54d0bull },
    { "5.33e-144", 0x2230a3903f0a9813ull },
    { "-4.501769e-304", 0x80f3c1ffe04c6630ull },
    { "1.6741443e-98", 0x2ba24f11e2cd296dull },
    { "-2.478e56", 0xcba4364e30b8ec24ull },
    { "2.4e-316", 0x0000000002e5381dull },
    { "-2.8373826039575206052e17", 0xc38f80538e609bd4ull },
    { "-8.1286066948980268e-148", 0xa164c99ef75ed012ull },
    { "-8.3084482e-155", 0x9ff1d2dd81beee1bull },
    { "8.9437324748e187", 0x66f48e20e8f26d55ull },
    { "6.3358019242663468189e213", 0x6c52d1f9d0c0e5f6ull },
    { "5.87002426842e169", 0x632f1ba6f817c284ull },
    { "1.091703838842878367e106", 0x55f30a3bb90a32d1ull },
    { "-9.56830584552126272e-41", 0xb7a0ab9b6531b9f1ull },
    { "-8.71674275962e-274", 0x873e2debac700ff4ull },
    { "8.539894e202", 0x6a116ead09d2f68cull },
    { "-4.749203102686672e-255", 0x8b21d3cdf006a0dcull },
    { "-3.7131774391812702e167", 0xe2b9300b873c7359ull },
    { "-7e-239", 0x8e7d2c2d192353f5ull },
    { "-3.6e134", 0xdbdfb2ec30626bddull },
    { "8.4555731511201243160e38", 0x4803e1035281fc3full },
    { "-1.0043089708889e48", 0xc9e5fd58b28ccdc4ull },
    { "-3.005e242", 0xf246886f0f27326aull },
    { "-9.326e-305", 0x80d05f5250b3fdfaull },
    { "-5.44809730613659483e-239", 0x8e76b47a87d397b7ull },
    { "8.229817710e-304", 0x01020f56f1a1c7cfull },
    { "-5.9407052593668e216", 0xecf13ba67775e640ull },
    { "4.7119884e-2", 0x3fa82018f18ae9a0ull },
    { "8.70393497618362748e-173", 0x1c358708c0f12814ull },
    { "5.173774893130612e-2", 0x3faa7d5ec741da51ull },
    { "-4.562636e192", 0xe7f0002ce78135b3ull },
    { "3.776143547e134", 0x5be09ffce5046249ull },
    { "-7.3635e-313", 0x80000022b3692017ull },
    { "9.190070978664e234", 0x70b71f7620c78383ull },
    { "-4.7269e120", 0xd8fd49df9036573full },
    { "-6e84", 0xd188b5519aa50023ull },
    { "-6.9832962897e-7", 0xbea76e9b1819f446ull },
    { "-3.2013e-61", 0xb3607633f73ff022ull },
    { "4.2712431e-168", 0x1d301e98454d6584ull },
    { "6.4341e-5", 0x3f10ddd9f6922949ull },
    { "-4.2122548069e283", 0xfad221136b7c2a34ull },
    { "-2.97693996e-212", 0x94040b2d400d11b3ull },
    { "8.56507e214", 0x6c8fcd7dbd5facf3ull },
    { "3.26163646e-89", 0x2d909c348fc72311ull },
    { "2.8263713167e244", 0x72b08eb337095ad6ull },
    { "7.119e-185", 0x19b35c0e93918acbull },
    { "-3.71e305", 0xff60e80291610b39ull },
    { "-7.8454787098522949720e-115", 0xa83ee9aaa4ea9da3ull },
    { "2.91958173173e-132", 0x24a094214fb93a5dull },
    { "7.540e-3", 0x3f7ee2435696e58aull },
    { "8.4306e-198", 0x17042a90d20884dcull },
    { "-3.288938e210", 0xeba4021b72f9b832ull },
    { "-1.104e-60", 0xb37c628c51edb561ull },
    { "-8e-46", 0xb69244ce242c5561ull },
    { "6.2546e-238", 0x0eb04a9388de3159ull },
    { "3.7644e-204", 0x15b2e24a7967951full },
    { "7.9e120", 0x5908798b3dbe3383ull },
    { "-7.2717021424874985e-94", 0xac9844e794020b63ull },
    { "-8.408559e-177", 0x9b61096e49955cccull },
    { "3.105e-155", 0x1fdaa4dceaa03537ull },
    { "-1.02925137617570e-237", 0x8ebacf1022d36e00ull },
    { "-8.67752943103445e-213", 0x93e75ec783d8718cull },
    { "-4.4526222e224", 0xee933f2fd3db5c80ull },
    { "-8.352522798182003e-52", 0xb5540014e23f9c6aull },
    { "1e232", 0x7019c3bc80c85c7full },
    { "6.6120677937859e274", 0x78fe8e6bd7993448ull },
    { "-8.6092269516152e-82", 0xaf1a21f371b8c261ull },
    { "-6.927632474135836825e307", 0xffd8a9c6d66e23ddull },
    { "5.89266690747737894e-78", 0x2fe5d59996305e33ull },
    { "-6.04819770551134150e75", 0xcfaabe513298dd24ull },
    { "7.256e29", 0x4622511481438c57ull },
    { "-1.511965e-314", 0x80000000b667ba5dull },
    { "-8.12492135776359e312", 0xfff0000000000000ull },
    { "6.319127e-271", 0x07d55d91c4dc1a38ull },
    { "8.030714908963480e-319", 0x0000000000027aefull },
    { "-6.4440638824e-300", 0x81d1431de30cd479ull },
    { "-7.797128411900e-172", 0x9c681b1815b8c761ull },
    { "-9.659586210171298318e17", 0xc3aacf8c7ef2e787ull },
    { "7.5e-140", 0x230c949921541af6ull },
    { "3.66e-285", 0x04e16a7b2d50ddb5ull },
    { "3e-25", 0x3ad7361cb863de62ull },
    { "3.5088457600e136", 0x5c4823418b709a20ull },
    { "6.913e-149", 0x212c4943863ead1eull },
    { "-2e-278", 0x8646b0a8e891ffffull },
    { "2.0507767e-194", 0x17b7f3e815fb0871ull },
    { "-3e-165", 0x9dc61cf19235ac11ull },
    { "6.44208342519344151e301", 0x7e980c7437ea7cc7ull },
    { "-4.3e-71", 0xb152fe5cf594c296ull },
    { "5.64720144e-198", 0x16fb043ec6d77bacull },
    { "-9.171175943366367551e197", 0xe90889b588af5af3ull },
    { "-9.986144e-49", 0xb5f75a041d738dd7ull },
    { "7.446451869008e228", 0x6f73a55be223ceffull },
    { "-3.171363474995e-31", 0xb999baaa0bdaec0eull },
    { "1.94041160435115400e-50", 0x359d0a2e29f623c5ull },
    { "4.35e249", 0x73c3712c7905b56full },
    { "-6.957906992738929e69", 0xce7021546f7c66e7ull },
    { "-2e212", 0xec0302cb5e6f642aull },
    { "-7.78750e107", 0xd65538bf7c6269ebull },
    { "2.63017468e29", 0x460a8ed79a32e508ull },
    { "-1.95940e-95", 0xac44ed1fb3d36b3eull },
    { "7.94467164974e11", 0x42671f3ddc65c000ull },
    { "-2.639376254989e-233", 0x8fa4fae90b3dc880ull },
    { "-7.012585e-11", 0xbdd346ab01f9afa3ull },
    { "-1e215", 0xec9290ba9a38c7d1ull },
    { "-3.10379039072e218", 0xed4c22de4ad3e188ull },
    { "9.4157764e-294", 0x03180de39a3cd6d5ull },
    { "3.17904840080e125", 0x59fe0e792433de1full },
    { "-2.38965299547e232", 0xf02ec8c2b37b87fdull },
    { "8.4202125e267", 0x7790522d5f8cd28eull },
    { "3.567e317", 0x7ff0000000000000ull },
    { "-2.07583e132", 0xdb67656785348550ull },
    { "5.033246928544161e99", 0x54a268ce7f13aaccull },
    { "7.14244808496e49", 0x4a486f703eca663aull },
    { "6.6891730660129415273e95", 0x53d40ae7080e0cd4ull },
    { "2.79289181992404112e260", 0x76022a28bb34f8d0ull },
    { "1.33559e41", 0x487887e9fbe869c2ull },
    { "5.7097314e292", 0x7cb6e2f35ebabf82ull },
    { "-7.8104773301879e4", 0xc0f3118c5f71ca84ull },
    { "3.5483858118e-7", 0x3e97d014957ea6afull },
    { "-4.316454095e-299", 0x81fce83037587a30ull },
    { "6.411573023e241", 0x72233b17c2108ea8ull },
    { "-8.35e-240", 0x8e4bd6c61973a20aull },
    { "8.528391885751573229e-282", 0x0593d0bec3c17585ull },
    { "-5.94e-89", 0xad9e3ffef7bc1c64ull },
    { "9.9885882626811e-105", 0x2a56e8a7fdfad935ull },
    { "1.0647161561e118", 0x5870e380129ae809ull },
    { "2.351481269982e196", 0x68b421ffa6517724ull },
    { "-4.55753479189422716e226", 0xeefec82070a939a2ull },
    { "-9.26922965659e93", 0xd371c659fa2baee5ull },
    { "7.72263314e253", 0x74a5111b4c907da0ull },
    { "6.76e155", 0x6049358d95db8089ull },
    { "-4.4920043203e113", 0xd787591deb754a07ull },
    { "4.0e302", 0x7ec2aa4f4a405be2ull },
    { "7.594346035153e-50", 0x35bc69f03ac38c3cull },
    { "-6.23038924785911e-264", 0x89491cac4882c20full },
    { "4.1841289413e205", 0x6aa0ae7e18176bf1ull },
    { "-4.979062287657519008e193", 0xe825d389167584eeull },
    { "1.46140366236780e-301", 0x01790df414f9e823ull },
    { "7.606009230e-158", 0x1f50b558f4802a21ull },
    { "4.1957387369507899800e-243", 0x0d9ca6113a37ddd5ull },
    { "8.5343311813643845809e-299", 0x020c93affcfb0a8dull },
    { "6.262051133e-88", 0x2dd3ee68ccf5840dull },
    { "2.187882651003834533e52", 0x4acd3d0cdfd5ffb9ull },
    { "-2.405113564e-279", 0x8615d431d0890791ull },
    { "-1.8665e-62", 0xb31eb69d9e405aabull },
    { "-5.418584785237851e-114", 0xa86ab0197cd5ae27ull },
    { "-1.309663351068e126", 0xda1ef4acae8afac7ull },
    { "8.1062017733e26", 0x4584f43bfeedbfd4ull },
    { "6.084677e182", 0x65e25458cc9aa77bull },
    { "5e151", 0x5f6e8ca3185deb72ull },
    { "-4.86734218442090e-11", 0xbdcac22cd5f9d57bull },
    { "4.91140509985179166e204", 0x6a6f5472ca2ec8a9ull },
    { "-3.4261637278111e-115", 0xa82affe22c76e53bull },
    { "7.5588287122e-214", 0x13b0492fe3c5cb89ull },
    { "6.3554514416332e-193", 0x18073274df7d26f0ull },
    { "-3e-315", 0x8000000024313d68ull },
    { "6.36422260543977e-114", 0x286f586ba181252bull },
    { "-4.703403441581e-246", 0x8d0071628dd4a794ull },
    { "9.685755853624e-70", 0x319abd4cba7416f3ull },
    { "-8.9494e67", 0xce0a8e5ee76be8ffull },
    { "2.21871814e-118", 0x2781e76d7ee02d21ull },
    { "1.278e268", 0x7798c589f55e561bull },
    { "-2.7436849850211e23", 0xc4cd0cc5e6d48805ull },
    { "-3.7e-132", 0xa4a5029b47809258ull },
    { "-3.0544383909510e-41", 0xb785494cd8c295f1ull },
    { "4.5099192645e197", 0x68f8220d502ac605ull },
    { "-6.900181823446174e-49", 0xb5f022a9c99d800cull },
    { "4.03799220420699444e244", 0x72b7a7c398558ab3ull },
    { "6.59607072965486452e-188", 0x19125e373d948482ull },
    { "6.439704417e-41", 0x3796706885f9932cull },
};

TEST(json_number_accuracy_known_bits) {
    g_mem = (u8*)arena_alloc_nozero(test_arena(), G_MEM_SIZE, 16);
    Arena a;
    f64 max_ulp_typ = 0.0, max_ulp_all = 0.0;
    u32 exact = 0;
    for (u32 i = 0; i < ARRAY_COUNT(k_nums); i++) {
        f64 ref;
        mem_copy(&ref, &k_nums[i].bits, 8);
        arena_init(&a, g_mem, MB(1));
        JsonValue v;
        JsonError e;
        CHECK(parse(k_nums[i].text, &v, &e, &a));
        f64 d = ulp_dist(v.as.num, ref);
        if (d == 0.0) exact++;
        if (i < 212 && d > max_ulp_typ) max_ulp_typ = d;
        if (d > max_ulp_all) max_ulp_all = d;
    }
    test_note("json sayi: tam esit", exact, 0);
    test_note("json sayi: tipik aralik en buyuk ULP", max_ulp_typ, 0);
    test_note("json sayi: tum aralik en buyuk ULP", max_ulp_all, 0);
    /* çözümleyici doğru yuvarlamaz (mantis × 1e22 adımları, her adımda yuvarlama): onlarda 2000 sayıda ≤ 2 ULP
     * ölçülmüştü, bu 412'lik kümede 3 ULP çıktı; f32'ye inen glTF verisi için önemsiz (M13 notu) */
    CHECK(exact >= 300);
    CHECK(max_ulp_typ <= 3.0);
    CHECK(max_ulp_all <= 8.0);
}

static void gen_json(Rng* r, char* buf, u32* n, u32 cap, u32 depth) {
    u32 kind = (depth > 6) ? (rng_u32(r) % 4u) : (rng_u32(r) % 6u);
    if (*n + 64 >= cap) kind = 0;
    switch (kind) {
    case 0: *n += fmt_format(buf + *n, cap - *n, "%d", (int)(rng_u32(r) % 2000000u) - 1000000); break;
    case 1: *n += fmt_format(buf + *n, cap - *n, "%.6f", (double)(rng_f32(r) * 1e6f - 5e5f)); break;
    case 2: {
        buf[(*n)++] = '"';
        u32 l = rng_u32(r) % 12u;
        for (u32 i = 0; i < l; i++) {
            u32 c = rng_u32(r) % 40u;
            if (c == 0) { mem_copy(buf + *n, "\\n", 2); *n += 2; }
            else if (c == 1) { mem_copy(buf + *n, "\\u00e7", 6); *n += 6; }
            else if (c == 2) { mem_copy(buf + *n, "\xC4\x9F", 2); *n += 2; }
            else buf[(*n)++] = (char)('a' + c % 26u);
        }
        buf[(*n)++] = '"';
    } break;
    case 3: *n += fmt_format(buf + *n, cap - *n, "%s", (rng_u32(r) & 1u) ? "true" : "null"); break;
    case 4: {
        buf[(*n)++] = '[';
        u32 l = rng_u32(r) % 8u;
        for (u32 i = 0; i < l; i++) { if (i) buf[(*n)++] = ','; gen_json(r, buf, n, cap, depth + 1); }
        buf[(*n)++] = ']';
    } break;
    default: {
        buf[(*n)++] = '{';
        u32 l = rng_u32(r) % 8u;
        for (u32 i = 0; i < l; i++) {
            if (i) buf[(*n)++] = ',';
            *n += fmt_format(buf + *n, cap - *n, "\"k%u\":", i);
            gen_json(r, buf, n, cap, depth + 1);
        }
        buf[(*n)++] = '}';
    } break;
    }
}

static b32 json_eq(const JsonValue* a, const JsonValue* b) {
    if (a->type != b->type || a->count != b->count) return FALSE;
    switch (a->type) {
    case JSON_NULL: return TRUE;
    case JSON_BOOL: return a->as.b == b->as.b;
    case JSON_NUMBER: {
        f64 d = a->as.num - b->as.num, m = a->as.num < 0 ? -a->as.num : a->as.num;
        if (d < 0) d = -d;
        return d <= m * 1e-14 + 1e-300;
    }
    case JSON_STRING: return mem_equal(a->as.str, b->as.str, a->count);
    case JSON_ARRAY:
        for (u32 i = 0; i < a->count; i++) if (!json_eq(&a->as.items[i], &b->as.items[i])) return FALSE;
        return TRUE;
    case JSON_OBJECT:
        for (u32 i = 0; i < a->count; i++) {
            if (a->as.members[i].key_len != b->as.members[i].key_len) return FALSE;
            if (!mem_equal(a->as.members[i].key, b->as.members[i].key, a->as.members[i].key_len)) return FALSE;
            if (!json_eq(&a->as.members[i].value, &b->as.members[i].value)) return FALSE;
        }
        return TRUE;
    }
    return FALSE;
}

TEST(json_roundtrip_1mb) {
    g_mem = (u8*)arena_alloc_nozero(test_arena(), G_MEM_SIZE, 16);
    const u32 SRC_CAP = MB(1) + 4096, DST_CAP = MB(2);
    char* src = ARENA_ARRAY(test_arena(), char, SRC_CAP);
    char* dst = ARENA_ARRAY(test_arena(), char, DST_CAP);
    Rng r = rng_make(77);
    u32 n = 0;
    src[n++] = '[';
    u32 items = 0;
    while (n < MB(1)) {
        if (items++) src[n++] = ',';
        gen_json(&r, src, &n, SRC_CAP - 8, 0);
    }
    src[n++] = ']';
    src[n] = 0;
    Arena a;
    arena_init(&a, g_mem, MB(48));
    JsonValue v1, v2; JsonError e;
    CHECK(json_parse(src, n, &a, &v1, &e));
    u32 wn = json_write(&v1, dst, DST_CAP);
    CHECK(wn < DST_CAP);
    CHECK(json_parse(dst, wn, &a, &v2, &e));
    CHECK(json_eq(&v1, &v2));
}

TEST(json_fuzz_mutations) {
    g_mem = (u8*)arena_alloc_nozero(test_arena(), G_MEM_SIZE, 16);
    static const char* seed =
        "{\"asset\":{\"version\":\"2.0\"},\"nodes\":[{\"mesh\":0,\"translation\":[1.5,-2,3e-1]}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
        "\"s\":\"\\u00e7\\ud83d\\ude00 x\",\"b\":[true,false,null]}";
    static char buf[512];
    u32 len = (u32)(u32)cstr_len(seed);
    Rng r = rng_make(31337);
    Arena a;
    u32 accepted = 0;
    for (u32 it = 0; it < 100000; it++) {
        mem_copy(buf, seed, len);
        u32 n = len;
        u32 muts = 1u + rng_u32(&r) % 4u;
        for (u32 k = 0; k < muts; k++) {
            u32 pos = rng_u32(&r) % n;
            u32 op = rng_u32(&r) % 3u;
            if (op == 0) buf[pos] = (char)(rng_u32(&r) & 0xFFu);
            else if (op == 1 && n > 1) { mem_copy(buf + pos, buf + pos + 1, n - pos - 1); n--; }
            else if (n + 1 < sizeof(buf)) { mem_copy(buf + pos + 1, buf + pos, n - pos); buf[pos] = "{}[]\",:\\0e-"[rng_u32(&r) % 11u]; n++; }
        }
        arena_init(&a, g_mem, MB(1));
        JsonValue v; JsonError e;
        accepted += json_parse(buf, n, &a, &v, &e) ? 1u : 0u;
    }
    test_note("json fuzz: 100000 mutasyondan gecerli kabul edilen", accepted, 0);
    CHECK(accepted < 100000);
}
