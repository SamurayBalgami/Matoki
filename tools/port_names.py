#!/usr/bin/env python3
"""tools/port_names.py — Xassemblianist/MatokiEngine kaynaklarını bu deponun adlarına çevirir (M13, D37).

Onların çekirdek API'si (vec3_add, m_sin, arena_push, ...) eşleme tablosuyla bizim adlara (v3_add, m_sinf,
arena_alloc, ...) yeniden adlandırılır; imzası farklı çağrılar (işaretçi argüman, tek argümanlı arena_reset)
ayraç dengeli olarak yeniden yazılır. Dize ve karakter sabitlerine dokunulmaz (Matoki kaynak dizeleri bozulmaz).
Sonda denetim: onların çekirdeğinden gelen ama bizim core/ başlıklarında olmayan ad ya da bulunmayan #include
varsa liste yazılır ve çıkış kodu 1 olur — sessiz eksik kalmaz.

Kullanım:
  python tools/port_names.py --from <onların kökü> [--to <hedef kök>] [--check] src/audio/synth.c src/audio/synth.h ...
  --to verilmezse bu deponun köküne yazar (src/... aynı yolda). --check: yazmaz, yalnız rapor.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE_REPO = "Xassemblianist/MatokiEngine"
SOURCE_REV = "0a7b782"

# ---- düz yeniden adlandırma (kelime sınırında) ----
RENAME = {
    # types.h
    "ARRAY_LEN": "ARRAY_COUNT",
    # arena.h (anlam aynı: sıfırlanmış bellek, dolunca panik)
    "arena_push": "arena_alloc",
    "arena_push_raw": "arena_alloc_nozero",
    "arena_pop_to": "arena_reset",
    "ARENA_PUSH_ARRAY": "ARENA_ARRAY",
    "ARENA_PUSH_ARRAY_RAW": "ARENA_ARRAY_NOZERO",
    "ARENA_PUSH_STRUCT": "ARENA_NEW",
    # math.h skaler
    "m_sqrt": "m_sqrtf", "m_abs": "m_absf", "m_floor": "m_floorf", "m_ceil": "m_ceilf", "m_trunc": "m_truncf",
    "m_min": "m_minf", "m_max": "m_maxf", "m_clamp": "m_clampf", "m_lerp": "m_lerpf",
    "m_sin": "m_sinf", "m_cos": "m_cosf", "m_tan": "m_tanf", "m_atan2": "m_atan2f",
    "m_exp2": "m_exp2f", "m_log2": "m_log2f", "m_pow": "m_powf",
    "M_PI": "M_PI_F", "M_TAU": "M_TAU_F", "M_HALF_PI": "M_HALF_PI_F",
    "M_DEG2RAD": "M_DEG2RAD_F", "M_RAD2DEG": "M_RAD2DEG_F",
    # math.h vektör
    "ivec2": "iv2",
    "vec2": "v2", "vec2_add": "v2_add", "vec2_sub": "v2_sub", "vec2_scale": "v2_scale",
    "vec2_dot": "v2_dot", "vec2_len": "v2_len", "vec2_norm": "v2_norm",
    "vec3": "v3", "vec3_add": "v3_add", "vec3_sub": "v3_sub", "vec3_scale": "v3_scale",
    "vec3_dot": "v3_dot", "vec3_cross": "v3_cross", "vec3_len": "v3_len", "vec3_norm": "v3_norm",
    "vec4": "v4", "vec4_add": "v4_add", "vec4_sub": "v4_sub", "vec4_scale": "v4_scale", "vec4_dot": "v4_dot",
    # rng.h: onların rng_range tamsayı [lo, hi); bizimki f32
    "rng_range": "rng_range_i32",
    # quaternion (core/quat.h; onların scene/transform.h'sindeki tanımlar kaldırılır)
    "quat_norm": "quat_normalize", "quat_rotate_vec3": "quat_rotate",
    # platform
    "plat_time_ms": "sys_now_ms",
}

# ---- imzası farklı çağrılar: (onların adı, argüman sayısı koşulu, yeniden yazıcı) ----
def _addr(x):
    x = x.strip()
    return "&" + x if re.fullmatch(r"[A-Za-z_]\w*(\.\w+|->\w+|\[[^\]]*\])*", x) else "&(" + x + ")"

CALLS = {
    # tek argüman = tümünü boşalt
    "arena_reset": lambda a: "arena_reset(%s, 0)" % a[0] if len(a) == 1 else None,
    # değerle Mat → işaretçiyle Mat
    "mat4_mul_vec4": lambda a: "mat4_mul_v4(%s, %s)" % (_addr(a[0]), a[1].strip()),
    "mat3_mul_vec3": lambda a: "mat3_mul_v3(%s, %s)" % (_addr(a[0]), a[1].strip()),
    "mat3_mul": lambda a: "mat3_mul(%s, %s)" % (_addr(a[0]), _addr(a[1])),
    "mat3_mul_point": lambda a: "mat3_mul_point(%s, %s)" % (_addr(a[0]), a[1].strip()),
    # PCG32 tohumlama → xorshift64* (dizi farklı; belirlenimcilik korunur)
    "rng_seed": lambda a: "(*(%s) = rng_make(%s))" % (a[0].strip(), a[1].strip()),
}

INCLUDE = {"core/log.h": "core/fmt.h"}

# onların çekirdeğinde olup henüz bizim çekirdekte karşılığı olmayan gruplar (denetimde ayrıca adlandırılır)
THEIR_CORE_HEADERS = ["arena.h", "fixed.h", "log.h", "math.h", "profiler.h", "rng.h", "simd.h", "types.h"]

IDENT = re.compile(r"[A-Za-z_]\w*")
C_KEYWORDS = {"if", "else", "for", "while", "do", "switch", "case", "return", "sizeof", "defined", "_Alignof",
              "_Static_assert", "_Noreturn", "struct", "union", "enum", "typedef", "static", "inline", "const"}


def rename_text(s):
    return IDENT.sub(lambda m: RENAME.get(m.group(0), m.group(0)), s)


def parse_args(s, i):
    """s[i] == '(' → (argümanlar, kapanıştan sonraki indeks) ya da None"""
    depth, j, args, cur = 0, i, [], i + 1
    while j < len(s):
        ch = s[j]
        if ch in "\"'":
            k = j + 1
            while k < len(s) and s[k] != ch:
                k += 2 if s[k] == "\\" else 1
            j = k
        elif ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
            if depth == 0:
                args.append(s[cur:j])
                return [a for a in args if a.strip() or len(args) > 1], j + 1
        elif ch == "," and depth == 1:
            args.append(s[cur:j])
            cur = j + 1
        j += 1
    return None


def rewrite_calls(s):
    pat = re.compile(r"\b(" + "|".join(CALLS) + r")\s*\(")
    out, pos = [], 0
    for m in pat.finditer(s):
        if m.start() < pos:
            continue
        # tanım / bildirim satırlarına dokunma (önünde tip adı: "void arena_reset(")
        if re.search(r"\b(void|Vec[234]|Mat[34]|Arena|u32|f32|define)\s*\*?\s*$", s[max(0, m.start() - 16):m.start()]):
            continue
        r = parse_args(s, m.end() - 1)
        if not r:
            continue
        args, end = r
        args = [rewrite_calls(a) for a in args]
        new = CALLS[m.group(1)](args)
        if new is None:
            continue
        out.append(s[pos:m.start()])
        out.append(new)
        pos = end
    out.append(s[pos:])
    return "".join(out)


def port_source(src, rel):
    # 1) imzası farklı çağrılar (yeniden adlandırmadan önce: onların adlarıyla eşleşir)
    s = rewrite_calls(src)
    # 2) dize sabitleri dışında yeniden adlandırma (yorumlar dahil: belgeler de bizim adları anlatsın)
    parts, i, out = [], 0, []
    for m in re.finditer(r"\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'", s):
        parts.append(("code", s[i:m.start()]))
        parts.append(("str", m.group(0)))
        i = m.end()
    parts.append(("code", s[i:]))
    for kind, t in parts:
        out.append(rename_text(t) if kind == "code" else t)
    s = "".join(out)
    # 3) #include eşlemesi
    s = re.sub(r'#include "([^"]+)"', lambda m: '#include "%s"' % INCLUDE.get(m.group(1), m.group(1)), s)
    # 4) kaynak notu: ilk açıklama bloğunun son satırı olarak
    note = " * Kaynak: %s@%s (%s), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.\n" % (
        SOURCE_REPO, SOURCE_REV, rel.replace("\\", "/"))
    end = s.find("*/") if s.startswith("/*") else -1
    if end >= 0:
        s = s[:end].rstrip() + "\n" + note + " */" + s[end + 2:]
    else:
        s = "/*" + note[2:] + " */\n" + s
    return s


def header_symbols(paths):
    syms = set()
    for p in paths:
        if not os.path.isfile(p):
            continue
        t = open(p, encoding="utf-8").read()
        t = re.sub(r"/\*.*?\*/|//[^\n]*", "", t, flags=re.S)
        syms.update(re.findall(r"#define\s+([A-Za-z_]\w*)", t))
        syms.update(re.findall(r"\b([A-Za-z_]\w*)\s*\(", t))
        syms.update(re.findall(r"typedef\s+(?:struct\s+\w*\s*\{[^}]*\}|[^;]*?)\s*(\w+)\s*;", t, flags=re.S))
        syms.update(re.findall(r"\}\s*(\w+)\s*;", t))
    return syms - C_KEYWORDS


def code_idents(src):
    t = re.sub(r"/\*.*?\*/|//[^\n]*", "", src, flags=re.S)
    t = re.sub(r"\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'", "", t)
    return set(IDENT.findall(t))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--from", dest="src_root", required=True)
    ap.add_argument("--to", dest="dst_root", default=ROOT)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("files", nargs="+")
    o = ap.parse_args()

    theirs = header_symbols([os.path.join(o.src_root, "src", "core", h) for h in THEIR_CORE_HEADERS])
    ours_dir = os.path.join(ROOT, "src", "core")
    ours = header_symbols([os.path.join(ours_dir, f) for f in os.listdir(ours_dir) if f.endswith(".h")])
    batch = {f.replace("\\", "/")[4:] for f in o.files if f.replace("\\", "/").startswith("src/")}

    problems = 0
    for rel in o.files:
        rel = rel.replace("\\", "/")
        src = open(os.path.join(o.src_root, rel), encoding="utf-8").read()
        out = port_source(src, rel)
        # denetim 1: onların çekirdeğinden kalan ad (yeniden adlandırılmamış ve bizde yok)
        used = code_idents(out)
        missing = sorted(x for x in used & theirs
                         if x not in ours and x not in RENAME.values() and not x.startswith("wasm_"))
        # denetim 2: çözülemeyen include (bizim src/'de yok ve bu partide taşınmıyor)
        incs = re.findall(r'#include "([^"]+)"', out)
        bad_inc = [x for x in incs
                   if not os.path.isfile(os.path.join(ROOT, "src", x)) and x not in batch]
        status = "ok"
        if missing or bad_inc:
            status = "EKSİK"
            problems += 1
        print("%-28s %s" % (rel, status))
        if missing:
            print("    bizim çekirdekte yok:", " ".join(missing))
        if bad_inc:
            print("    bulunamayan include:", " ".join(bad_inc))
        if not o.check:
            dst = os.path.join(o.dst_root, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "w", encoding="utf-8", newline="\n") as f:
                f.write(out)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
