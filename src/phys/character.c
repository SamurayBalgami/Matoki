/* phys/character.c — kinematik karakter kontrolcüsü.
 * Çözme: kapsülün 3 küresi her gövdeyle phys_collide ile sınanır; en çok 4 tur girişim
 * giderilir. Yürünebilir yüzey (normal·yukarı ≥ cos(eğim)) → yalnızca yukarı itilir ve
 * yere basılı sayılır; dik yüzey → normal boyunca itilir ve hızın yüzeye giren bileşeni silinir.
 * Basamak: dik bir yüzey alt küreyi durdurduysa yukarı-ileri-aşağı dene; daha çok ilerleme
 * sağlıyor ve yürünebilir zeminde bitiyorsa kabul et. */
#include "phys/character.h"

#define CHAR_SKIN 0.002f

CharacterDesc character_desc_default(Vec3 pos) {
    CharacterDesc d;
    d.pos = pos;
    d.radius = 0.35f;
    d.height = 1.8f;
    d.step_height = 0.4f;
    d.max_slope_deg = 45.0f;
    d.speed = 5.0f;
    d.jump_speed = 5.5f;
    d.mass = 80.0f;
    return d;
}

void character_init(Character* c, const CharacterDesc* d) {
    c->pos = d->pos;
    c->vel = v3(0, 0, 0);
    c->radius = d->radius;
    c->height = d->height;
    c->step_height = d->step_height;
    c->slope_cos = m_cosf(d->max_slope_deg * (M_PI_F / 180.0f));
    c->speed = d->speed;
    c->jump_speed = d->jump_speed;
    c->mass = d->mass;
    c->grounded = FALSE;
    c->ground_normal = v3(0, 1, 0);
    c->ground_body = PHYS_NO_BODY;
    c->last_contacts = 0;
    c->stepped = FALSE;
}

Vec3 character_eye(const Character* c) { return v3_add(c->pos, v3(0, c->height - 0.15f, 0)); }

typedef struct Resolve {
    b32 grounded;
    b32 blocked;      /* alt küre dik bir yüzeye takıldı */
    Vec3 ground_normal;
    u32 ground_body;
    u32 contacts;
} Resolve;

static void resolve(PhysWorld* w, Character* c, Vec3* pos, Vec3* vel, f32 dt, b32 push_bodies, Resolve* r) {
    const f32 offs[3] = { c->radius, c->height * 0.5f, c->height - c->radius };
    Body s;
    s = (Body){ 0 };
    s.shape = SHAPE_SPHERE;
    s.radius = c->radius;
    s.rot = quat_identity();
    s.rot_m = mat3_from_quat(s.rot);
    for (u32 iter = 0; iter < 4; iter++) {
        b32 any = FALSE;
        Vec3 mn = v3(pos->x - c->radius, pos->y, pos->z - c->radius);
        Vec3 mx = v3(pos->x + c->radius, pos->y + c->height, pos->z + c->radius);
        for (u32 i = 0; i < w->count; i++) {
            const Body* b = &w->bodies[i];
            if (b->shape != SHAPE_PLANE &&
                (b->aabb_max.x < mn.x || b->aabb_min.x > mx.x || b->aabb_max.y < mn.y || b->aabb_min.y > mx.y ||
                 b->aabb_max.z < mn.z || b->aabb_min.z > mx.z)) {
                continue;
            }
            for (u32 k = 0; k < 3; k++) {
                s.pos = v3_add(*pos, v3(0, offs[k], 0));
                Manifold m;
                if (!phys_collide(&s, b, &m)) continue;
                f32 depth = m.p[0].depth;
                if (depth <= 0.0f) continue;
                Vec3 out = v3_scale(m.normal, -1.0f);   /* gövdeden dışarı */
                r->contacts++;
                any = TRUE;
                if (out.y >= c->slope_cos) {
                    pos->y += depth / out.y + CHAR_SKIN;
                    if (vel->y < 0.0f) vel->y = 0.0f;
                    r->grounded = TRUE;
                    r->ground_normal = out;
                    r->ground_body = i;
                } else {
                    *pos = v3_add(*pos, v3_scale(out, depth + CHAR_SKIN));
                    f32 vn = v3_dot(*vel, out);
                    if (vn < 0.0f) {
                        *vel = v3_sub(*vel, v3_scale(out, vn));
                        if (push_bodies && b->inv_mass > 0.0f) {
                            /* karakter kütlesinin yarısıyla it (esnek olmayan, kaba) */
                            phys_apply_impulse_at(w, i, v3_scale(out, vn * c->mass * 0.5f), m.p[0].pos);
                        }
                    }
                    if (k == 0 && m_absf(out.y) < 0.7f) r->blocked = TRUE;
                }
                s.pos = v3_add(*pos, v3(0, offs[k], 0));
            }
        }
        if (!any) break;
    }
    UNUSED(dt);
}

void character_update(PhysWorld* w, Character* c, Vec3 wish, b32 jump, f32 dt) {
    Vec3 target = v3(wish.x * c->speed, 0.0f, wish.z * c->speed);
    b32 was_grounded = c->grounded;
    if (was_grounded) {
        c->vel.x = target.x;
        c->vel.z = target.z;
        if (jump) {
            c->vel.y = c->jump_speed;
            was_grounded = FALSE;
        }
    } else {
        f32 k = m_minf(1.0f, 2.0f * dt);
        c->vel.x += (target.x - c->vel.x) * k;
        c->vel.z += (target.z - c->vel.z) * k;
    }
    c->vel = v3_add(c->vel, v3_scale(w->gravity, dt));

    Vec3 start = c->pos;
    Vec3 pos = v3_add(start, v3_scale(c->vel, dt));
    Vec3 vel = c->vel;
    Resolve r = { 0 };
    resolve(w, c, &pos, &vel, dt, TRUE, &r);
    c->stepped = FALSE;

    /* basamak: önü kapandıysa, hareket yönünde (yarıçap + 0,1) ileride yukarıdan aşağı ışınla
     * basamağın üst yüzünü yokla; yürünebilir ve en çok step_height yüksekse oraya çık ve
     * yatay hareketi o yükseklikten yeniden çöz. (Çarpışma çözmeyle "yukarı-ileri-aşağı" denemesi,
     * yarıçap basamak derinliğinden büyükken kenardan geri itildiği için güvenilir değil.) */
    Vec3 hmove = v3(c->vel.x * dt, 0.0f, c->vel.z * dt);
    f32 hlen = m_sqrtf(hmove.x * hmove.x + hmove.z * hmove.z);
    if (r.blocked && was_grounded && hlen > 1e-6f) {
        Vec3 dir = v3_scale(hmove, 1.0f / hlen);
        Vec3 probe = v3_add(v3_add(start, v3_scale(dir, c->radius + 0.1f)), v3(0, c->step_height + 0.05f, 0));
        RayHit hit;
        if (phys_raycast(w, probe, v3(0, -1, 0), c->step_height + 0.05f, PHYS_NO_BODY, &hit) &&
            hit.normal.y >= c->slope_cos) {
            f32 rise = hit.point.y - start.y;
            if (rise > 0.01f && rise <= c->step_height + 0.001f) {
                Vec3 p2 = v3_add(start, v3(0, rise + CHAR_SKIN, 0));
                Vec3 v2 = v3(c->vel.x, 0.0f, c->vel.z);
                p2 = v3_add(p2, hmove);
                Resolve r2 = { 0 };
                resolve(w, c, &p2, &v2, dt, TRUE, &r2);
                Vec3 d1 = v3_sub(pos, start), d2 = v3_sub(p2, start);
                if (d2.x * d2.x + d2.z * d2.z > d1.x * d1.x + d1.z * d1.z + 1e-8f) {
                    pos = p2;
                    vel = v2;
                    r = r2;
                    r.grounded = TRUE;   /* yürünebilir basamak üstüne çıktı */
                    r.ground_normal = hit.normal;
                    r.ground_body = hit.body;
                    c->stepped = TRUE;
                }
            }
        }
    }

    /* zemine yapış / zemin denetimi: zıplarken değil, ve çözme zaten yürünebilir bir destek
     * bulduysa (ör. basamak kenarına basıyor) hiç değil — merkez ışını kenarın aşağısındaki
     * zemini görüp karakteri basamaktan geri çekerdi */
    if (vel.y <= 0.0f && !r.grounded) {
        f32 snap = was_grounded ? c->step_height : 0.05f;
        RayHit hit;
        Vec3 o = v3_add(pos, v3(0, c->radius, 0));
        if (phys_raycast(w, o, v3(0, -1, 0), c->radius + snap, PHYS_NO_BODY, &hit) && hit.normal.y >= c->slope_cos) {
            f32 gap = pos.y - hit.point.y;
            if (gap <= snap) {
                pos.y = hit.point.y + CHAR_SKIN;
                vel.y = 0.0f;
                r.grounded = TRUE;
                r.ground_normal = hit.normal;
                r.ground_body = hit.body;
            }
        }
    }

    c->pos = pos;
    c->vel = vel;
    c->grounded = r.grounded;
    c->ground_normal = r.grounded ? r.ground_normal : v3(0, 1, 0);
    c->ground_body = r.grounded ? r.ground_body : PHYS_NO_BODY;
    c->last_contacts = r.contacts;
}
