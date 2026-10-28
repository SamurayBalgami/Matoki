/* core/embed.h — derleme anında gömülen dosyalar (shaders/ altındaki .mtk'ler).
 * Tablo build.py tarafından build/gen/embed_files.c olarak üretilir. Veriler NUL ile biter
 * (len sona eklenen NUL'u içermez). */
#ifndef CORE_EMBED_H
#define CORE_EMBED_H
#include "core/types.h"

typedef struct EmbedFile {
    const char* name;
    const u8*   data;
    u32         len;
} EmbedFile;

extern const EmbedFile g_embed_files[];
extern const u32 g_embed_count;

const EmbedFile* embed_find(const char* name);

#endif
