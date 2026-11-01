// web/coi.js — başlık ayarlanamayan barındırmada (GitHub Pages) SharedArrayBuffer için COOP/COEP (D27).
// Sayfada klasik <script> olarak: kendini service worker olarak kaydeder, sayfa bir kez yeniden yüklenir; worker
// aynı kökenli yanıtlara Cross-Origin-Opener/Embedder-Policy ekler. Başlıklar zaten varsa (python build.py serve)
// hiçbir şey yapmaz. Motor mantığı yok. Aynı kökenli dosyalar her açılışta sunucuya sorulur (304 ucuz): Pages max-age=600
// önbelleği eski JS'i yeni engine.wasm ile buluşturmasın (D44).
if (typeof window === 'undefined') {
  self.addEventListener('install', () => self.skipWaiting());
  self.addEventListener('activate', (e) => e.waitUntil(self.clients.claim()));
  self.addEventListener('fetch', (e) => {
    const r = e.request;
    if (r.cache === 'only-if-cached' && r.mode !== 'same-origin') return;
    const fresh = r.method === 'GET' && new URL(r.url).origin === self.location.origin;
    e.respondWith(fetch(fresh ? new Request(r.url, { cache: 'no-cache', credentials: r.credentials }) : r).then((res) => {
      if (res.status === 0) return res;
      const h = new Headers(res.headers);
      h.set('Cross-Origin-Opener-Policy', 'same-origin');
      h.set('Cross-Origin-Embedder-Policy', 'require-corp');
      h.set('Cross-Origin-Resource-Policy', 'same-origin');
      return new Response(res.body, { status: res.status, statusText: res.statusText, headers: h });
    }));
  });
} else if (!window.crossOriginIsolated && window.isSecureContext && navigator.serviceWorker) {
  const KEY = 'matoki-coi-reload';
  navigator.serviceWorker.register(document.currentScript.src).then(() => navigator.serviceWorker.ready).then(() => {
    // bir kez: worker sayfayı denetlemeye başlasın; yine yalıtılmamışsa döngüye girme (3d.js hatayı gösterir)
    if (!sessionStorage.getItem(KEY)) {
      sessionStorage.setItem(KEY, '1');
      location.reload();
    }
  });
}
