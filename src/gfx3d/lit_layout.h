/* gfx3d/lit_layout.h — LitUniforms'un Matoki uniform düzeni (D56): isim → ofset, tip. Derleyici frag shader'larını bu tabloyla
 * derler (MatokiOptions.layout): shader yalnız kullandığı uniform'ları istediği sırada bildirir, tabloda olmayan isim derleme hatası.
 * Düzen tek yerde, C yapısının kendisinden (offsetof): yapı değişince shader'lar kendiliğinden uyar. */
#ifndef GFX3D_LIT_LAYOUT_H
#define GFX3D_LIT_LAYOUT_H
#include "matoki/sema.h"

extern const MatokiLayout LIT_LAYOUT;

#endif
