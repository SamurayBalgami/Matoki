/* core/profiler.h — iç içe bölge zamanlayıcısı ve kare bazlı rapor (en çok 64 bölge, 32 derinlik).
 * Yalnız ana thread'den çağrılır: worker işlerini ölçmek için bölge jobs_parallel_for çağrısını sarar
 * (duvar saati süresi). Zaman kaynağı sys_now_ms; prof_report LOG ile hizalı tablo basar.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/core/profiler.h), izinle uyarlandı (D37). */
#ifndef CORE_PROFILER_H
#define CORE_PROFILER_H
#include "core/types.h"

#define PROF_MAX_ZONES 64
#define PROF_MAX_STACK 32

typedef struct ProfZone {
    const char* name;
    f64 cur_ms;       /* etkin kare içinde birikmiş süre */
    u32 cur_calls;
    f64 last_ms;      /* tamamlanan son kare */
    u32 last_calls;
    f64 total_ms;     /* tüm kareler */
    f64 max_ms;       /* tek karede en yüksek */
    u64 total_calls;
} ProfZone;

void prof_reset(void);
void prof_begin(const char* name);
void prof_end(void);
void prof_frame_end(void);
void prof_report(void);

u32             prof_zone_count(void);
const ProfZone* prof_zone_get(u32 idx);
u32             prof_frame_count(void);

#define PROF_BEGIN(name) prof_begin(name)
#define PROF_END()       prof_end()

#endif
