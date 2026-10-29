/* core/profiler.c — profiler.h uygulaması: sabit diziler, ayırma yok, tek thread (ana).
 * Bölge eşlemesi önce işaretçi, sonra içerik eşitliği; dolu tabloda yeni bölge sessizce yok sayılır.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/core/profiler.c), izinle uyarlandı (D37):
 * onların thread başına tabloları çıkarıldı (bizim worker'lar jobs_parallel_for içinde ölçülmez). */
#include "core/profiler.h"
#include "core/fmt.h"
#include "core/sys.h"

typedef struct ProfOpen {
    u32 zone;
    f64 start_ms;
} ProfOpen;

static ProfZone g_zones[PROF_MAX_ZONES];
static u32 g_zone_count;
static u32 g_frame_count;
static ProfOpen g_stack[PROF_MAX_STACK];
static u32 g_top;
static u32 g_overflow;   /* derinlik aşımında açılmayan bölgeler: eşleşen prof_end yok sayılır */

static b32 name_eq(const char* a, const char* b) {
    if (a == b) return TRUE;
    if (!a || !b) return FALSE;
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

static u32 zone_find(const char* name) {
    for (u32 i = 0; i < g_zone_count; i++) {
        if (name_eq(g_zones[i].name, name)) return i;
    }
    if (g_zone_count >= PROF_MAX_ZONES) return U32_MAX;
    ProfZone* z = &g_zones[g_zone_count];
    *z = (ProfZone){ 0 };
    z->name = name;
    return g_zone_count++;
}

void prof_reset(void) {
    g_zone_count = 0;
    g_frame_count = 0;
    g_top = 0;
    g_overflow = 0;
}

void prof_begin(const char* name) {
    if (g_top >= PROF_MAX_STACK) {
        g_overflow++;
        return;
    }
    g_stack[g_top].zone = zone_find(name);
    g_stack[g_top].start_ms = sys_now_ms();
    g_top++;
}

void prof_end(void) {
    f64 end_ms = sys_now_ms();
    if (g_overflow) {
        g_overflow--;
        return;
    }
    if (g_top == 0) return;
    ProfOpen* o = &g_stack[--g_top];
    if (o->zone == U32_MAX) return;
    f64 dt = end_ms - o->start_ms;
    ProfZone* z = &g_zones[o->zone];
    z->cur_ms += dt > 0.0 ? dt : 0.0;
    z->cur_calls++;
}

void prof_frame_end(void) {
    while (g_top > 0 || g_overflow) prof_end();
    g_frame_count++;
    for (u32 i = 0; i < g_zone_count; i++) {
        ProfZone* z = &g_zones[i];
        z->last_ms = z->cur_ms;
        z->last_calls = z->cur_calls;
        z->total_ms += z->cur_ms;
        z->total_calls += z->cur_calls;
        if (z->cur_ms > z->max_ms) z->max_ms = z->cur_ms;
        z->cur_ms = 0.0;
        z->cur_calls = 0;
    }
}

void prof_report(void) {
    f64 frames = g_frame_count ? (f64)g_frame_count : 1.0;
    LOG("%-24s %10s %10s %10s %8s", "Bolge", "Son(ms)", "Ort(ms)", "Max(ms)", "Cagri");
    for (u32 i = 0; i < g_zone_count; i++) {
        const ProfZone* z = &g_zones[i];
        b32 any = g_frame_count > 0;
        LOG("%-24s %10.3f %10.3f %10.3f %8u", z->name, any ? z->last_ms : z->cur_ms,
            any ? z->total_ms / frames : z->cur_ms, any ? z->max_ms : z->cur_ms, any ? z->last_calls : z->cur_calls);
    }
}

u32 prof_zone_count(void) { return g_zone_count; }

const ProfZone* prof_zone_get(u32 idx) { return idx < g_zone_count ? &g_zones[idx] : NULL; }

u32 prof_frame_count(void) { return g_frame_count; }
