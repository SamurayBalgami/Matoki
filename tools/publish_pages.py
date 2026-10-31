#!/usr/bin/env python3
"""tools/publish_pages.py — demoyu GitHub Pages'e yayınlar: motoru derler, web/ dosyalarını + engine.wasm'ı geçici bir
klasörde gh-pages dalına (tek commit, geçmişsiz) yazar ve origin'e gönderir. COOP/COEP web/coi.js service worker'ıyla
(Pages başlık ayarlamaz). Yalnız kendi depomuzun origin'ine gönderir. Kullanım: python tools/publish_pages.py
Sayfa: https://<kullanıcı>.github.io/<depo>/3d.html"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILES = ["3d.html", "3d.js", "mt.mjs", "worker.mjs", "audio.js", "audio_worklet.js", "coi.js", "gpu.mjs", "gpu_check.html", "engine.wasm"]


def git(*args, cwd=ROOT):
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


def main():
    subprocess.run([sys.executable, os.path.join(ROOT, "build.py")], check=True)
    rev = git("rev-parse", "--short", "HEAD")
    dirty = git("status", "--porcelain")
    origin = git("remote", "get-url", "origin")
    name = git("config", "user.name")
    email = git("config", "user.email")
    tmp = tempfile.mkdtemp(prefix="matoki_pages_")
    try:
        for f in FILES:
            shutil.copyfile(os.path.join(ROOT, "web", f), os.path.join(tmp, f))
        with open(os.path.join(tmp, "index.html"), "w", encoding="utf-8") as fh:
            fh.write('<!doctype html><meta charset="utf-8"><meta http-equiv="refresh" content="0; url=3d.html">'
                     '<title>MATOKI</title><a href="3d.html">MATOKI 3D</a>\n')
        open(os.path.join(tmp, ".nojekyll"), "w").close()
        git("init", "-q", "-b", "gh-pages", cwd=tmp)
        git("add", "-A", cwd=tmp)
        msg = "Pages: demo @ %s%s" % (rev, " (+ kaydedilmemiş değişiklikler)" if dirty else "")
        git("-c", "user.name=" + name, "-c", "user.email=" + email, "commit", "-q", "-m", msg, cwd=tmp)
        git("push", "-q", "--force", origin, "gh-pages", cwd=tmp)
        print("gönderildi:", msg)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
