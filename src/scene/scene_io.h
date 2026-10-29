/* scene/scene_io.h — ECS dünyasını JSON metnine serileştirme (kaydet) ve JSON'dan geri yükleme.
 * Bileşen alan tanımlarına (EcsFieldDesc) göre tüm dünyayı deterministik olarak yazar ve okur.
 * Yükleme sırasında entity referansları yeni tutamaklara eşlenir; bilinmeyen bileşen/alanlar uyarı üretir.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/scene/scene_io.h), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#ifndef SCENE_IO_H
#define SCENE_IO_H

#include "core/types.h"
#include "core/arena.h"
#include "scene/ecs.h"

typedef struct {
    u32  entities_loaded;
    u32  warnings;
    char warn[256];
    char err[128];
} SceneIoResult;

/* Tüm dünyayı JSON metnine dönüştürür (src/asset/json.h yazıcısını kullanır).
 * Dönüş: yazılan bayt sayısı (NUL hariç). */
u32 scene_save(const EcsWorld* w, char* buf, u32 cap, Arena* scratch);

/* JSON metninden dünyayı yükler; entity referanslarını yeni tutamaklara yeniden eşler.
 * Bilinmeyen bileşen ve alanlar çökmeden uyarı olarak atlanır. */
b32 scene_load(EcsWorld* w, const char* json, u32 len, Arena* scratch, SceneIoResult* res);

#endif
