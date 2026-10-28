/* core/jobs.h — iş sistemi: ana thread + worker'lar paylaşılan bellekte paralel döngü çalıştırır.
 * Worker'lar aynı motor modülünün ayrı örnekleridir (JS başlatır: web/mt.mjs, web/worker.mjs);
 * her biri kendi yığınıyla jobs_worker_wait / jobs_worker_run döngüsünde bekler. Worker yoksa
 * jobs_parallel_for doğrudan ana thread'de sırayla çalışır (sonuç aynı olmalı: iş indeksleri bağımsız). */
#ifndef CORE_JOBS_H
#define CORE_JOBS_H
#include "core/types.h"

#define JOBS_MAX_THREADS 16      /* ana thread dahil */
#define JOBS_MAX_COUNT   65535   /* bir paralel döngüdeki en fazla indeks */

/* tid: 0 = ana thread, 1.. = worker */
typedef void (*JobFn)(void* ctx, u32 index, u32 tid);

/* Kullanılan thread sayısı (1 + etkin worker, sınırla kesilmiş). */
u32  jobs_threads(void);
/* Ölçüm / test: kullanılacak en fazla thread, ana dahil (0 = sınırsız, 1 = sırayla). */
void jobs_set_limit(u32 threads);
/* Yalnızca ana thread'den. fn(ctx, i, tid) her i < count için tam bir kez; dönüşte hepsi bitmiştir. */
void jobs_parallel_for(JobFn fn, void* ctx, u32 count);

#endif
