/* phys/joints.h — eklem çözücüsünün dünya içi arayüzü (world.c kullanır).
 * Eklemler temaslardan önce çözülür (Box2D sırası). Soft step'te her alt adımda yumuşak
 * (60 Hz, ζ = 2) + relax geçişi; SI'da Baumgarte. Biriken itkiler adımlar arası warm start'tır. */
#ifndef PHYS_JOINTS_H
#define PHYS_JOINTS_H
#include "phys/phys.h"

b32  joints_block_pair(const PhysWorld* w, u32 a, u32 b);
void joints_prepare(PhysWorld* w, f32 h);
void joints_warm_start(PhysWorld* w);
void joints_solve(PhysWorld* w, f32 h, b32 use_bias, b32 soft);

#endif
