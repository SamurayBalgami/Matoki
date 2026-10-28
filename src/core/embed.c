/* core/embed.c — gömülü dosya araması (tablo build/gen/embed_files.c'de).
 * Doğrusal arama; dosya sayısı küçük. */
#include "core/embed.h"
#include "core/fmt.h"
#include "core/mem.h"

const EmbedFile* embed_find(const char* name) {
    usize n = cstr_len(name);
    for (u32 i = 0; i < g_embed_count; i++) {
        const char* e = g_embed_files[i].name;
        if (cstr_len(e) == n && mem_equal(e, name, n)) return &g_embed_files[i];
    }
    return NULL;
}
