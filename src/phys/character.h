/* phys/character.h — kinematik karakter kontrolcüsü (kapsül = 3 küre).
 * Fizik dünyasında gövde DEĞİLDİR; dünyayı sorgular, dinamik gövdeleri iter.
 * Özellikler: kaydırarak çarpışma çözme, eğim sınırı (yürünebilir yüzeyde yalnızca dikey itme →
 * eğimden kaymaz), basamak çıkma (yukarı → ileri → aşağı), zemine yapışma, zıplama, hava kontrolü. */
#ifndef PHYS_CHARACTER_H
#define PHYS_CHARACTER_H
#include "phys/phys.h"

typedef struct CharacterDesc {
    Vec3 pos;           /* ayak noktası */
    f32  radius, height;
    f32  step_height;
    f32  max_slope_deg;
    f32  speed, jump_speed;
    f32  mass;          /* dinamik gövdeleri iterken */
} CharacterDesc;

typedef struct Character {
    Vec3 pos;
    Vec3 vel;
    f32  radius, height, step_height, slope_cos, speed, jump_speed, mass;
    b32  grounded;
    Vec3 ground_normal;
    u32  ground_body;
    u32  last_contacts;   /* son güncellemede çözülen girişim sayısı */
    b32  stepped;         /* son güncellemede basamak çıktı mı */
} Character;

CharacterDesc character_desc_default(Vec3 pos);
void character_init(Character* c, const CharacterDesc* d);
/* wish: yatay istenen yön (|wish| <= 1), dünya uzayında */
void character_update(PhysWorld* w, Character* c, Vec3 wish, b32 jump, f32 dt);
Vec3 character_eye(const Character* c);

#endif
