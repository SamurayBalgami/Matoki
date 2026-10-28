/* core/jobs_wasm.c — jobs.h'nin wasm uygulaması (atomikler + memory.atomic.wait/notify).
 * İş dağıtımı tek bir "bilet" sözcüğüyle: üst 16 bit iş sırası, alt 16 bit sıradaki indeks. Worker indeksi
 * CAS ile alır; sıra değiştiyse almaz → geç uyanan worker bir sonraki işten indeks tüketemez. Ana thread
 * tarayıcıda bekleyemez (wait yasak): kendisi de iş alır, sonra done == count olana dek döner.
 * Export'lar JS platform katmanı içindir (web/mt.mjs, web/worker.mjs). */
#include "core/jobs.h"
#include "core/arena.h"
#include "core/sys.h"

#define JOBS_STACK_BYTES (1u << 20)

typedef struct JobState {
    u32         gen;            /* her yeni işte artar; worker'lar bunda bekler */
    u32         ticket;         /* (sıra << 16) | sıradaki indeks */
    u32         done;           /* biten indeks sayısı */
    u32         seq;
    JobFn       fn;
    void*       ctx;
    u32         count;
    u32         workers;        /* çevrimiçi worker */
    u32         limit;
    u32         seen[JOBS_MAX_THREADS];
} JobState;

static JobState g_jobs;
static Arena g_stacks;
static b32 g_stacks_ok;

static inline u32 aload(const u32* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static inline void astore(u32* p, u32 v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }

u32 jobs_threads(void) {
    u32 n = 1 + aload(&g_jobs.workers);
    u32 lim = g_jobs.limit;
    if (lim && n > lim) n = lim;
    return n;
}

void jobs_set_limit(u32 threads) { g_jobs.limit = threads; }

/* Bu işin indekslerini bitene dek al ve çalıştır. seq/fn/ctx/count anlık görüntüsüyle. */
static void run_indices(u32 tid) {
    u32 t = aload(&g_jobs.ticket);
    u32 seq = t >> 16;
    JobFn fn = g_jobs.fn;
    void* ctx = g_jobs.ctx;
    u32 count = g_jobs.count;
    for (;;) {
        if ((t >> 16) != seq || (t & 0xFFFFu) >= count) return;
        if (__atomic_compare_exchange_n(&g_jobs.ticket, &t, t + 1, FALSE, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            fn(ctx, t & 0xFFFFu, tid);
            __atomic_fetch_add(&g_jobs.done, 1u, __ATOMIC_SEQ_CST);
            t = aload(&g_jobs.ticket);
        }
        /* CAS başarısızsa t güncel değerle dolduruldu; yeniden dene */
    }
}

void jobs_parallel_for(JobFn fn, void* ctx, u32 count) {
    if (count == 0) return;
    if (count > JOBS_MAX_COUNT) sys_panic("jobs_parallel_for: indeks sayısı çok büyük");
    if (jobs_threads() <= 1 || count == 1) {
        for (u32 i = 0; i < count; i++) fn(ctx, i, 0);
        return;
    }
    /* önce iş tanımı, sonra bilet (yeni sıra), sonra uyandırma */
    g_jobs.fn = fn;
    g_jobs.ctx = ctx;
    g_jobs.count = count;
    astore(&g_jobs.done, 0);
    g_jobs.seq = (g_jobs.seq + 1) & 0xFFFFu;
    astore(&g_jobs.ticket, g_jobs.seq << 16);
    __atomic_fetch_add(&g_jobs.gen, 1u, __ATOMIC_SEQ_CST);
    __builtin_wasm_memory_atomic_notify((i32*)&g_jobs.gen, (u32)-1);
    run_indices(0);
    while (aload(&g_jobs.done) < count) {
        /* diğer thread'lerin son indeksleri bitiyor */
    }
}

/* ---- JS platform katmanı ---- */

/* worker tid'nin yığın tepesi (JS örneğin __stack_pointer'ına yazar); ana thread çağırır */
EXPORT(jobs_stack_top) u32 jobs_stack_top(u32 tid) {
    if (tid == 0 || tid >= JOBS_MAX_THREADS) return 0;
    if (!g_stacks_ok) {
        if (!arena_init_sys(&g_stacks, (usize)JOBS_STACK_BYTES * (JOBS_MAX_THREADS - 1))) return 0;
        g_stacks_ok = TRUE;
    }
    return (u32)(uptr)(g_stacks.base + (usize)JOBS_STACK_BYTES * tid);   /* yığın aşağı büyür */
}

/* tüm worker'lar hazır olunca ana thread bildirir */
EXPORT(jobs_set_workers) void jobs_set_workers(u32 n) {
    if (n > JOBS_MAX_THREADS - 1) n = JOBS_MAX_THREADS - 1;
    astore(&g_jobs.workers, n);
}

/* worker: yeni iş yayımlanana dek uyu */
EXPORT(jobs_worker_wait) void jobs_worker_wait(u32 tid) {
    u32 seen = g_jobs.seen[tid];
    while (aload(&g_jobs.gen) == seen) {
        __builtin_wasm_memory_atomic_wait32((i32*)&g_jobs.gen, (i32)seen, -1);
    }
    g_jobs.seen[tid] = aload(&g_jobs.gen);
}

/* worker: yayımlanmış işin kalan indekslerini çalıştır (sınırın dışındaki worker katılmaz) */
EXPORT(jobs_worker_run) void jobs_worker_run(u32 tid) {
    if (g_jobs.limit && tid >= g_jobs.limit) return;
    run_indices(tid);
}
