#!/usr/bin/env python3
"""build.py — MATOKI derleme betiği (Makefile'ın Windows/Linux taşınabilir karşılığı).

Kullanım:
  python build.py            # = engine
  python build.py engine     # build/engine.wasm (+ web/engine.wasm)
  python build.py test       # testleri wasm'a derler ve node ile koşar
  python build.py all        # engine + test
  python build.py serve      # http://localhost:8080 (COOP/COEP başlıklı) — web/ dizini
  python build.py clean

Testler de wasm'a derlenir (host C derleyicisi / libc gerekmez); tests/run.mjs ile node'da koşar.
"""
import glob
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(ROOT, "build")


def find_tool(name, extra_dirs):
    p = shutil.which(name)
    if p:
        return p
    for d in extra_dirs:
        for cand in (os.path.join(d, name), os.path.join(d, name + ".exe")):
            if os.path.isfile(cand):
                return cand
    sys.exit(f"HATA: '{name}' bulunamadı (PATH veya {extra_dirs})")


LLVM_DIRS = [r"C:\Program Files\LLVM\bin", "/usr/lib/llvm-18/bin", "/opt/homebrew/opt/llvm/bin"]
NODE_DIRS = [r"C:\Program Files\nodejs"]

CFLAGS = [
    "--target=wasm32", "-std=c11", "-O2", "-ffreestanding", "-nostdlib",
    "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
    "-msimd128", "-mbulk-memory", "-mnontrapping-fptoint", "-matomics",
    "-fno-stack-protector",
    "-I" + os.path.join(ROOT, "src"),
]
LDFLAGS = [
    "-Wl,--no-entry",
    "-Wl,-z,stack-size=1048576",
    "-Wl,--initial-memory=4194304",
    # paylaşılan bellek (worker'lar aynı belleği kullanır; D27): JS oluşturur, modül içe aktarır
    "-Wl,--import-memory",
    "-Wl,--shared-memory",
    "-Wl,--max-memory=1073741824",
    # her worker örneğinin kendi yığını: JS __stack_pointer'ı örnek başına ayarlar
    "-Wl,--export=__stack_pointer",
    # JIT: üretilen shader modüllerinin fonksiyonları bu tabloya eklenir
    "-Wl,--export-table",
    "-Wl,--growable-table",
]


def sources(pattern):
    return sorted(glob.glob(os.path.join(ROOT, pattern), recursive=True))


def clang(args):
    cc = find_tool("clang", LLVM_DIRS)
    cmd = [cc] + args
    r = subprocess.run(cmd)
    if r.returncode != 0:
        sys.exit(r.returncode)


def gen_embed(test=False):
    """shaders/*.mtk + assets/* (+ testte tests/assets/*; .py hariç, aynı ad bir kez) → build/gen/embed_*.c
    (NUL sonlu bayt dizileri)."""
    gen = os.path.join(BUILD, "gen")
    os.makedirs(gen, exist_ok=True)
    files = sources("shaders/*.mtk")
    extra = sources("assets/*") + (sources("tests/assets/*") if test else [])
    seen = {os.path.basename(f) for f in files}
    for f in extra:
        name = os.path.basename(f)
        if os.path.isfile(f) and not f.endswith(".py") and name not in seen:
            files.append(f)
            seen.add(name)
    path = os.path.join(gen, "embed_tests.c" if test else "embed_files.c")
    lines = ["/* build.py tarafından üretildi — elle düzenleme */", '#include "core/embed.h"']
    for i, f in enumerate(files):
        data = open(f, "rb").read() + b"\0"
        body = ",".join(str(b) for b in data)
        lines.append(f"static const u8 EMB_{i}[{len(data)}] = {{{body}}};")
    lines.append("const EmbedFile g_embed_files[] = {")
    for i, f in enumerate(files):
        size = os.path.getsize(f)
        lines.append(f'    {{ "{os.path.basename(f)}", EMB_{i}, {size} }},')
    lines.append("    { 0, 0, 0 }")
    lines.append("};")
    lines.append(f"const u32 g_embed_count = {len(files)};")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")
    return path


def build_engine():
    os.makedirs(BUILD, exist_ok=True)
    out = os.path.join(BUILD, "engine.wasm")
    clang(CFLAGS + LDFLAGS + sources("src/**/*.c") + [gen_embed(), "-o", out])
    shutil.copyfile(out, os.path.join(ROOT, "web", "engine.wasm"))
    print(f"engine.wasm  {os.path.getsize(out)} bayt")


def gen_test_registry(test_files):
    names = []
    rx = re.compile(r"^\s*TEST\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)", re.M)
    for f in test_files:
        with open(f, encoding="utf-8") as fh:
            names += rx.findall(fh.read())
    if len(set(names)) != len(names):
        sys.exit("HATA: yinelenen test adı")
    path = os.path.join(BUILD, "test_registry.c")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("/* build.py tarafından üretildi — elle düzenleme */\n#include \"test.h\"\n")
        for n in names:
            fh.write(f"void test_{n}(TestCtx* t_);\n")
        fh.write("const TestEntry g_tests[] = {\n")
        for n in names:
            fh.write(f"    {{ \"{n}\", test_{n} }},\n")
        fh.write("    { 0, 0 }\n};\n")
        fh.write(f"const u32 g_test_count = {len(names)};\n")
    return path


def build_tests():
    os.makedirs(BUILD, exist_ok=True)
    tests = sources("tests/*.c")
    reg = gen_test_registry([t for t in tests if not t.endswith("test_main.c")])
    out = os.path.join(BUILD, "tests.wasm")
    clang(CFLAGS + ["-I" + os.path.join(ROOT, "tests")] + LDFLAGS + sources("src/**/*.c") + tests + [reg, gen_embed(test=True), "-o", out])
    return out


def run_tests():
    wasm = build_tests()
    node = find_tool("node", NODE_DIRS)
    args = sys.argv[2:]
    r = subprocess.run([node, os.path.join(ROOT, "tests", "run.mjs"), wasm] + args)
    if r.returncode != 0:
        sys.exit(r.returncode)


def serve():
    import http.server
    import functools

    class Handler(http.server.SimpleHTTPRequestHandler):
        extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                          ".wasm": "application/wasm", ".js": "text/javascript", ".mjs": "text/javascript"}

        def end_headers(self):
            self.send_header("Cross-Origin-Opener-Policy", "same-origin")
            self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
            self.send_header("Cache-Control", "no-store")
            super().end_headers()

    port = 8080
    h = functools.partial(Handler, directory=os.path.join(ROOT, "web"))
    print(f"http://localhost:{port}/3d.html")
    http.server.ThreadingHTTPServer(("127.0.0.1", port), h).serve_forever()


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "engine"
    if cmd == "engine":
        build_engine()
    elif cmd == "test":
        run_tests()
    elif cmd == "all":
        build_engine()
        run_tests()
    elif cmd == "serve":
        serve()
    elif cmd == "clean":
        shutil.rmtree(BUILD, ignore_errors=True)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
