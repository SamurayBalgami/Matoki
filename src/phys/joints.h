/* phys/joints.h — eklem çözücüsünün dünya içi arayüzü (world.c kullanır).
 * Eklemler temaslardan önce çözülür (Box2D sırası). Soft step'te her alt adımda hazırlık (güncel kollar), yumuşak
 * (60 Hz, ζ = 2) + relax geçişi; SI'da Baumgarte. Ağaç eklemleri doğrudan çözülür (D60). Biriken itkiler warm start'tır. */
#ifndef PHYS_JOINTS_H
#define PHYS_JOINTS_H
#include "phys/phys.h"

void joints_init(PhysWorld* w, Arena* a);   /* doğrudan çözücü tamponları (phys_world_init çağırır) */
b32  joints_block_pair(const PhysWorld* w, u32 a, u32 b);
void joints_prepare(PhysWorld* w, f32 h);
void joints_warm_start(PhysWorld* w);
void joints_solve(PhysWorld* w, f32 h, b32 use_bias, b32 soft);
void joints_stabilize(PhysWorld* w);   /* konum integrasyonundan hemen sonra (D60) */

#endif
