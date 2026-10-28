/* phys/vehicle.h — raycast tekerlekli araç (arcade-gerçekçi).
 * Şasi sıradan bir kutu gövde; her tekerlek şasiye bağlı bir noktadan aşağı ışın atar.
 * Süspansiyon: yay + sönüm (sıkışma hızı). Lastik: boyuna (motor/fren) ve yanal (kayma
 * hızını sıfırlayan) itkiler, sürtünme çemberi |J| <= μ·Fn·dt ile sınırlı. phys_step'ten ÖNCE çağrılır. */
#ifndef PHYS_VEHICLE_H
#define PHYS_VEHICLE_H
#include "phys/phys.h"

#define VEHICLE_MAX_WHEELS 4

typedef struct Wheel {
    Vec3 local_pos;      /* şasi yerel uzayında bağlantı noktası */
    f32  rest_length;    /* serbest süspansiyon boyu */
    f32  radius;
    b32  steer, drive, handbrake;
    /* durum */
    b32  contact;
    u32  ground_body;
    Vec3 contact_point, contact_normal;
    f32  compression, prev_compression;
    f32  load;           /* süspansiyon kuvveti (N) */
    f32  spin;           /* çizim için dönüş açısı */
    f32  slip;           /* yanal kayma hızı (m/s), ses/iz için */
} Wheel;

typedef struct VehicleDesc {
    Vec3 pos;
    Quat rot;
    Vec3 half;           /* şasi yarı boyutları */
    f32  mass;
    f32  spring, damper; /* N/m, N·s/m (tekerlek başına) */
    f32  engine_force;   /* toplam (N) */
    f32  brake_force;    /* toplam (N) */
    f32  max_steer;      /* rad */
    f32  grip;           /* lastik sürtünme katsayısı μ */
    f32  roll_reduction; /* 0..1: yanal itkiyi kütle merkezi yüksekliğine taşıma (devrilmeyi azaltır) */
} VehicleDesc;

typedef struct Vehicle {
    u32   body;
    u32   wheel_count;
    Wheel wheels[VEHICLE_MAX_WHEELS];
    f32   spring, damper, engine_force, brake_force, max_steer, grip, roll_reduction;
    /* girişler (oyun kodu yazar) */
    f32   throttle;      /* -1..1 (negatif = geri) */
    f32   brake;         /* 0..1 */
    f32   steer;         /* -1..1 (pozitif = sola) */
    b32   handbrake;
    /* çıktı */
    f32   speed;         /* ileri hız (m/s) */
    u32   wheels_on_ground;
} Vehicle;

VehicleDesc vehicle_desc_default(Vec3 pos);
void vehicle_create(PhysWorld* w, Vehicle* v, const VehicleDesc* d);
void vehicle_update(PhysWorld* w, Vehicle* v, f32 dt);
/* Tekerleğin çizim dönüşümü (silindir ekseni yerel x) */
Mat4 vehicle_wheel_matrix(const PhysWorld* w, const Vehicle* v, u32 i);
Vec3 vehicle_forward(const PhysWorld* w, const Vehicle* v);

#endif
