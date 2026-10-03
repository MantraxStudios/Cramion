#include "CramionCore/anim/FootPlacement.h"

#include "CramionCore/ecs/MathUtil.h"

#include <algorithm>
#include <cmath>

namespace cramion::ik {

using core::Mat4;
using core::Quat;
using core::Vec3;

namespace {

constexpr Vec3 kUp{0.0f, 1.0f, 0.0f};

float smooth01(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / std::max(e1 - e0, 1e-6f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float angleOf(const Quat& q) { return 2.0f * std::acos(std::clamp(std::abs(q.w), 0.0f, 1.0f)); }

// El mismo giro con `max_degrees` como mucho.
Quat limited(const Quat& q, float max_degrees) {
    const float angle = angleOf(q);
    const float max_angle = std::max(max_degrees, 0.0f) * core::kPi / 180.0f;
    if (angle <= max_angle || angle < 1e-6f) return q;
    return core::slerp(Quat{}, q, max_angle / angle);
}

struct Probe {
    bool hit = false;
    Vec3 point{};
    Vec3 normal{0.0f, 1.0f, 0.0f};
};

// Lo que se usa de cada pata este frame.
struct Work {
    bool ok = false;
    Vec3 ankle{};      // tobillo de la animacion (mundo, antes de mover nada)
    Vec3 root{};       // cadera/hombro (mundo)
    float e = 0.0f;    // suelo bajo el pie respecto a la base
    float land = 0.0f; // suelo donde va a apoyarse si va por el aire (respecto a la base)
    float rise = 0.0f; // lo que sube su raiz al inclinar el cuerpo
    float slack = 0.0f;  // lo que aun puede estirarse la pata (la animacion la dobla)
};

}  // namespace

void groundLegs(const Pose& pose, const Mat4& world, int body, const std::vector<GroundLeg>& legs, const Vec3& forward,
                const GroundSettings& settings, const GroundRay& ground, GroundState& state, float delta_seconds,
                Support support) {
    const std::size_t count = legs.size();
    const int nodes = static_cast<int>(pose.nodes->size());
    if (count == 0 || !ground || nodes == 0) return;
    if (state.feet.size() != count) state.feet.assign(count, GroundState::Foot{});
    const float dt = std::max(delta_seconds, 0.0f);
    // Fraccion del camino hacia lo pedido este frame (`rate` por segundo).
    const auto follow = [dt](float rate) { return dt > 0.0f ? 1.0f - std::exp(-rate * std::min(dt, 0.1f)) : 1.0f; };
    const float max_step = std::max(settings.max_step, 0.01f);
    const Mat4 to_model = core::inverse(world);
    const Vec3 up_model = core::normalize(ecs::transformDirection(to_model, kUp));
    const Vec3 position{world.m[3][0], world.m[3][1], world.m[3][2]};
    const float base = position.y;

    // --- La base: teletransportes, saltos y escalones de golpe ---
    float shift = 0.0f;  // lo que subio la base este frame (m)
    if (!state.has_base || core::length(position - state.base) > std::max(2.0f, max_step * 4.0f)) {
        // Primera vez o teletransporte: se empieza de cero.
        for (GroundState::Foot& f : state.feet) f = GroundState::Foot{};
        state.base_velocity = 0.0f;
        state.step_frames = 0;
        state.body = state.body_speed = 0.0f;
        state.rising = state.falling = 0.0f;
        state.active = support == Support::Air ? 0.0f : 1.0f;
    } else {
        shift = base - state.base.y;
    }
    state.has_base = true;
    state.base = position;
    const float vertical = dt > 0.0f ? shift / dt : 0.0f;

    // En el aire no se apoya nada (si no, al despegar los pies buscaban el
    // suelo y la cadera se hundia). La fisica lo sabe (Character Controller);
    // si no, subir o caer deprisa varios frames seguidos (un escalon que el
    // controlador sube de golpe es un solo frame y no cuenta).
    state.rising = vertical > 1.0f ? state.rising + dt : 0.0f;
    state.falling = vertical < -3.0f ? state.falling + dt : 0.0f;
    bool air = support == Support::Air;
    if (support == Support::Unknown) air = state.rising > 0.04f || state.falling > 0.06f;
    state.active += ((air ? 0.0f : 1.0f) - state.active) * follow(air ? 16.0f : 10.0f);
    const float weight = std::clamp(settings.weight, 0.0f, 1.0f) * state.active;

    // Escalones de golpe: el Character Controller sube o baja un escalon en
    // un solo paso. Ese salto (lo que se aparta de como venia moviendose) no
    // lo hace la cadera: se queda en su altura del mundo y luego se acomoda
    // sin tirones (mas abajo). Una cuesta o un ascensor (varios frames
    // seguidos) se siguen sin retraso. En el aire no cuenta (al aterrizar no
    // hay rebote).
    const bool settled = dt > 0.0f && state.active >= 0.5f;
    if (!settled) {
        state.base_velocity = 0.0f;
        state.step_frames = 0;
        if (state.active < 0.5f) state.body = state.body_speed = 0.0f;
    } else {
        const float residual = shift - state.base_velocity * dt;
        if (std::abs(residual) > std::max(0.04f, 3.0f * dt)) {
            const int direction = residual > 0.0f ? 1 : -1;
            state.step_frames = state.step_frames * direction > 0 ? state.step_frames + direction : direction;
            if (std::abs(state.step_frames) >= 3) {
                state.base_velocity = vertical;  // no era un escalon: se mueve asi
                state.step_frames = 0;
            } else {
                state.body = std::clamp(state.body - residual, -max_step, max_step);
            }
        } else {
            state.step_frames = 0;
            state.base_velocity += (vertical - state.base_velocity) * follow(10.0f);
        }
    }

    // --- Cada pie: apoyado o no, y el suelo bajo el ---
    std::vector<Work> work(count);
    for (std::size_t i = 0; i < count; ++i) {
        const GroundLeg& leg = legs[i];
        GroundState::Foot& f = state.feet[i];
        Work& w = work[i];
        bool valid_leg = leg.joints.size() >= 2;
        for (const int j : leg.joints) valid_leg = valid_leg && j >= 0 && j < nodes;
        if (!valid_leg) {
            f = GroundState::Foot{};
            continue;
        }
        w.ok = true;
        w.ankle = ecs::transformPoint(world, nodePosition(pose, leg.joints.back()));
        w.root = ecs::transformPoint(world, nodePosition(pose, leg.joints.front()));
        float length = 0.0f;
        for (std::size_t k = 0; k + 1 < leg.joints.size(); ++k) {
            length += core::length(ecs::transformPoint(world, nodePosition(pose, leg.joints[k + 1])) -
                                   ecs::transformPoint(world, nodePosition(pose, leg.joints[k])));
        }
        length = std::max(length, 1e-3f);
        w.slack = std::max(length - core::length(w.ankle - w.root), 0.0f);

        // Apoyado segun la animacion: cerca de lo mas bajo que lo pone el clip
        // (ese suelo se adapta despacio si el clip cambia de altura; deprisa,
        // un pie que empieza a levantarse pareceria apoyado).
        f.lift = w.ankle.y - base;
        if (!f.has_floor || f.lift < f.floor) {
            f.floor = f.lift;
        } else {
            f.floor = std::min(f.lift, f.floor + 0.02f * length * dt);
        }
        f.has_floor = true;
        f.plant = 1.0f - smooth01(f.floor + 0.03f * length, f.floor + 0.12f * length, f.lift);

        // Suelo bajo el tobillo y bajo la punta (solo suelo de verdad: lo que
        // es pared o techo no cuenta).
        const float top = base + max_step + 0.05f;
        const float reach = max_step * 2.0f + 0.1f;
        const auto probe = [&](const Vec3& at) {
            Probe p;
            p.hit = ground(Vec3{at.x, top, at.z}, kUp * -1.0f, reach, p.point, p.normal) && p.normal.y > 0.5f;
            if (p.hit) p.normal = core::normalize(p.normal);
            return p;
        };
        // Hacia donde va el pie (la animacion y el personaje, mundo).
        Vec3 velocity{};
        if (f.has_last && dt > 0.0f) {
            velocity = (w.ankle - f.last) * (1.0f / dt);
            velocity.y = 0.0f;
            if (core::length(velocity) > 10.0f) velocity = Vec3{};  // teletransporte
        }
        f.last = w.ankle;
        f.has_last = true;
        const Probe heel = probe(w.ankle);
        Probe tip;
        Vec3 toe{};
        if (leg.toe >= 0 && leg.toe < nodes) {
            toe = ecs::transformPoint(world, nodePosition(pose, leg.toe));
            tip = probe(toe);
            // El pie que va por el aire mira por delante de la punta: un
            // escalon que viene lo sube como una rampa de 45 grados, y asi
            // llega arriba justo al borde (ni tropieza con la tabica ni la
            // atraviesa). El apoyado no se mueve: solo mira debajo.
            const float speed = core::length(velocity);
            if (speed > 0.3f) {
                const Vec3 dir = velocity * (1.0f / speed);
                for (const float k : {0.06f, 0.12f, 0.18f}) {
                    const float d = k * length;
                    const Probe ahead = probe(toe + dir * d);
                    if (!ahead.hit || (tip.hit && ahead.point.y - d <= tip.point.y)) continue;
                    if (!tip.hit) tip.normal = kUp;
                    tip.hit = true;
                    tip.point = Vec3{toe.x, ahead.point.y - d, toe.z};
                }
            }
        }
        bool hit = heel.hit || tip.hit;
        float g = base;
        Vec3 n = kUp;
        if (heel.hit && tip.hit) {
            // La punta no se mete en un escalon: el pie sube lo que haga falta.
            // En una pendiente lisa la punta ya sube al girar el pie con ella
            // (eso no cuenta como escalon).
            float rise = 0.0f;
            if (settings.align_feet) {
                const Vec3 d = toe - w.ankle;
                rise = -(heel.normal.x * d.x + heel.normal.z * d.z) / std::max(heel.normal.y, 0.5f);
            }
            g = std::max(heel.point.y, tip.point.y - rise);
            n = core::normalize(heel.normal + tip.normal);
        } else if (heel.hit) {
            g = heel.point.y;
            n = heel.normal;
        } else if (tip.hit) {
            g = tip.point.y;
            n = tip.normal;
        }
        if (hit) {
            g = std::clamp(g, base - max_step, base + max_step);
            if (!f.valid) {
                f.ground = g;
                f.normal = n;
                f.valid = true;
            } else {
                // En el mundo: si el personaje sube un escalon de golpe, el pie
                // apoyado se queda donde estaba. Apoyado, casi al momento (no
                // se mueve y asi no se queda atras en una cuesta); en el aire,
                // si el suelo sube, al momento (con la rampa de delante ya es
                // suave; asi no se mete en el escalon) y si baja, en ~70 ms
                // (cruzar un borde no da un salto).
                const float planted = f.plant * f.plant;
                if (g > f.ground && f.plant < 0.5f) {
                    f.ground = g;
                } else {
                    f.ground += (g - f.ground) * follow(g > f.ground ? 80.0f : 14.0f + 60.0f * planted);
                }
                f.normal = core::normalize(f.normal + (n - f.normal) * follow(12.0f));
            }
        } else if (f.valid) {
            // Sin suelo debajo (un agujero, demasiado lejos): a la altura de la animacion.
            f.ground += (base - f.ground) * follow(14.0f);
            f.normal = core::normalize(f.normal + (kUp - f.normal) * follow(12.0f));
        }
        w.e = f.valid ? std::clamp(f.ground - base, -max_step, max_step) : 0.0f;
        // Donde va a apoyarse el que va por el aire (un poco mas adelante en su
        // camino): si es mas abajo (bajar un escalon), la cadera empieza a
        // bajar antes de que llegue, no de golpe al apoyarlo.
        w.land = w.e;
        const float speed = core::length(velocity);
        if (f.valid && speed > 0.3f && f.plant < 1.0f) {
            const float ahead = std::min(speed * 0.12f, 0.3f * length);
            const Probe landing = probe(w.ankle + velocity * (ahead / speed));
            if (landing.hit) w.land = std::min(w.e, std::clamp(landing.point.y - base, -max_step, max_step));
        }
    }

    // --- Inclinar el cuerpo con el suelo (animales: 3 patas o mas) ---
    Quat tilt{};
    Vec3 pivot{};
    bool tilting = false;
    if (settings.align_body && settings.body_align_weight > 0.0f && body >= 0 && body < nodes) {
        std::vector<Vec3> feet;
        std::vector<float> heights;
        Vec3 center{};
        int roots = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (!work[i].ok) continue;
            center = center + work[i].root;
            ++roots;
            if (!state.feet[i].valid) continue;
            feet.push_back(work[i].ankle);
            heights.push_back(work[i].e);
        }
        if (feet.size() >= 3 && roots > 0) {
            pivot = center * (1.0f / static_cast<float>(roots));
            const Vec3 forward_world = ecs::transformDirection(world, forward);
            tilt = core::slerp(Quat{}, groundTilt(feet, heights, forward_world, settings.max_body_angle),
                               std::clamp(settings.body_align_weight, 0.0f, 1.0f));
            tilting = angleOf(tilt) > 1e-5f;
        }
    }
    if (tilting) {
        for (Work& w : work) {
            if (w.ok) w.rise = (ecs::quatRotate(tilt, w.root - pivot) + pivot - w.root).y;
        }
    }

    // --- La cadera o el cuerpo ---
    // Cada pie pone un techo: la cadera no puede quedar mas arriba de lo que
    // su pierna llega (su suelo menos lo que su raiz ya baja al inclinar el
    // cuerpo). El apoyado, justo eso (la pierna como en la animacion); el que
    // va por el aire, con la holgura que le da la rodilla doblada: al bajar
    // hacia un escalon de abajo la cadera empieza a bajar con el, no de golpe
    // al apoyarlo. Si un techo queda por debajo, la cadera baja deprisa (el
    // pie llega); si no, va con un muelle critico hacia la altura de la
    // animacion (o el techo, si es mas bajo), sin tirones: al soltarse el pie
    // de atras en una escalera sube con calma, y tras un escalon de golpe se
    // acomoda.
    float offset = 0.0f;
    if (settings.move_body && body >= 0 && body < nodes) {
        float ceiling = max_step;
        for (std::size_t i = 0; i < count; ++i) {
            if (!work[i].ok || !state.feet[i].valid) continue;
            const float air = 1.0f - state.feet[i].plant;
            const float ground_at = work[i].e + (work[i].land - work[i].e) * air;
            ceiling = std::min(ceiling, ground_at - work[i].rise + air * 0.8f * work[i].slack);
        }
        ceiling = std::max(ceiling, -max_step);
        if (!settled) {
            state.body = std::min(0.0f, ceiling);  // sin tiempo (el editor en pausa): directo
            state.body_speed = 0.0f;
        } else if (ceiling < state.body) {
            state.body = std::max(ceiling, state.body - 1.6f * dt);
            state.body_speed = 0.0f;
        } else {
            const float target = std::min(0.0f, ceiling);
            constexpr float kOmega = 10.0f;
            float remaining = std::min(dt, 0.1f);
            while (remaining > 1e-6f) {
                const float h = std::min(remaining, 1.0f / 240.0f);
                state.body_speed += (kOmega * kOmega * (target - state.body) - 2.0f * kOmega * state.body_speed) * h;
                state.body += state.body_speed * h;
                remaining -= h;
            }
            if (state.body > ceiling) {
                state.body = ceiling;
                state.body_speed = std::min(state.body_speed, 0.0f);
            }
        }
        state.body = std::clamp(state.body, -max_step, max_step);
        offset = state.body * weight;
        if (std::abs(offset) > 1e-6f) translateGlobal(pose, body, ecs::transformDirection(to_model, kUp * offset));
        if (tilting) {
            // Gira alrededor del centro de las caderas y los hombros (no de la
            // cadera: el pecho bajaria el doble). El giro del mundo, con su eje
            // pasado al modelo.
            const Vec3 axis_world{tilt.x, tilt.y, tilt.z};
            const float s = core::length(axis_world);
            if (s > 1e-6f) {
                const Vec3 axis = core::normalize(ecs::transformDirection(to_model, axis_world * (1.0f / s)));
                const float angle = 2.0f * std::acos(std::clamp(tilt.w, -1.0f, 1.0f)) * weight;
                const Vec3 a = axis * std::sin(angle * 0.5f);
                // Donde queda el centro de las raices antes y despues de girar.
                const auto center = [&] {
                    Vec3 c{};
                    int k = 0;
                    for (std::size_t i = 0; i < count; ++i) {
                        if (!work[i].ok) continue;
                        c = c + nodePosition(pose, legs[i].joints.front());
                        ++k;
                    }
                    return k > 0 ? c * (1.0f / static_cast<float>(k)) : c;
                };
                const Vec3 before = center();
                rotateGlobal(pose, body, Quat{a.x, a.y, a.z, std::cos(angle * 0.5f)});
                translateGlobal(pose, body, before - center());
            }
        }
    }
    state.body_offset = offset;

    // --- Cada pata a su sitio ---
    for (std::size_t i = 0; i < count; ++i) {
        const Work& w = work[i];
        GroundState::Foot& f = state.feet[i];
        if (!w.ok || !f.valid) {
            f.offset = 0.0f;
            continue;
        }
        const GroundLeg& leg = legs[i];
        // El apoyado, a su suelo. El que va por el aire sigue a su cadera (lo
        // que bajo o subio el cuerpo) salvo que debajo haya algo mas alto: asi
        // no se estira hacia un escalon de abajo ni atraviesa uno de arriba.
        const float with_body = (settings.move_body ? state.body : 0.0f) + w.rise;
        const float free = std::max(with_body, w.e);
        f.offset = (free + (w.e - free) * f.plant) * weight;
        const Vec3 target = ecs::transformPoint(to_model, w.ankle + kUp * f.offset);
        const float leg_weight = std::clamp(leg.weight, 0.0f, 1.0f);
        const std::vector<Vec3>* hints = leg.bend_hints.size() == leg.joints.size() ? &leg.bend_hints : nullptr;
        chain(pose, leg.joints, target, leg.pole, leg_weight, 12, hints);
        // El pie apoyado sigue la pendiente (como mucho max_foot_angle).
        if (settings.align_feet && leg_weight > 0.0f) {
            const Vec3 normal_model = core::normalize(ecs::transformDirection(to_model, f.normal));
            Quat turn = limited(rotationBetween(up_model, normal_model), settings.max_foot_angle);
            turn = core::slerp(Quat{}, turn, weight * f.plant * leg_weight);
            if (angleOf(turn) > 1e-5f) rotateGlobal(pose, leg.joints.back(), turn);
        }
    }
}

}  // namespace cramion::ik
