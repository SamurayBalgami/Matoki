/* matoki/ir_guard.h — quad-sabit koşula dallanma planı (D43). IR düzdür: if ve select iki kolu da hesaplar. Maskesi quad'ın
 * tüm lane'lerinde aynı olan select'te yalnız bir kola yarayan komutlar gerçek bir dala taşınabilir: seçilmeyen kol
 * çalışmaz, select'in kendisi yerinde kaldığı için sonuç bit bit aynıdır. Komutlar saf olduğundan atlamak güvenlidir.
 * Plan: her canlı komutun düğümü (0 = koşulsuz). Ardışık, aynı maskeli select'ler (vec bileşenleri) tek dal paylaşır. */
#ifndef MATOKI_IR_GUARD_H
#define MATOKI_IR_GUARD_H
#include "matoki/ir.h"

#define IR_GUARD_MAX_DEPTH 16u   /* iç içe dal sınırı: uzun if zincirleri yığını ve wasm yuvalamasını şişirmesin */
#define IR_GUARD_MIN_COST  8u    /* iki kolun toplamı bundan azsa dal, atladığından pahalı: düz hesaplanır */

typedef struct IrGuardNode {
    u32 parent;
    u32 head, tail;   /* üyeler (artan komut sırası; IrGuards.next ile) */
    u32 depth;
    u32 cost;         /* üye sayısı, alt dallar dahil */
    u32 deriv;        /* alt ağaçta türev komutu sayısı */
} IrGuardNode;

typedef struct IrGuards {
    u32*         node;       /* komut → düğüm */
    u32*         next;       /* düğüm listesinde sonraki komut */
    u32*         group_at;   /* komut → bu select'ten önce açılacak dal (grup + 1; 0 = yok) */
    IrGuardNode* nodes;      /* 0 kök; grup g: 2g + 1 doğru kolu, 2g + 2 yanlış kolu */
    IrRef*       mask;       /* grup → quad-sabit tam maske */
    u8*          branch;     /* grup → gerçek dal mı (0: kolları düz hesaplanır) */
    u32          ngroups;
} IrGuards;

/* skip != NULL: skip[i] olan select dallanmaz (codegen: çizim-sabit select'ler önsözde hesaplanır).
 * no_deriv_arm: türev içeren kol dal olmaz (WGSL düzgünlük analizi türevi düzgün olmayan dalda reddeder). */
void ir_guard_build(Arena* a, const IrFunc* f, const u8* skip, b32 no_deriv_arm, IrGuards* g);

static inline u32 ir_guard_true(u32 group) { return group * 2u + 1u; }
static inline u32 ir_guard_false(u32 group) { return group * 2u + 2u; }

#endif
