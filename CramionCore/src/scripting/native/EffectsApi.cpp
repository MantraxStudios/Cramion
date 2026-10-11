// VFX Graph, 2D, destruccion, repeticiones, Motion Matching y vehiculos.
//
//   entity:playEffect()                       VisualEffect
//   entity:sendEffectEvent("Explode")
//   entity.velocity2D = Vec3(3, 0, 0)         Rigidbody2D
//   Physics2D.raycast(posicion, Vec3(0, -1, 0), 1.2)
//   entity:fracture(punto, 6)                 Destructible
//   Replay.start()  Replay.play(-5)  Replay.save("gol")
//   entity:setMotionVelocity(dir * 4)         Motion Matching (modo Script)
//
// Eventos para los scripts de C++ (mensajes, fase Events):
//   OnCollisionEnter2D/Stay2D/Exit2D y OnTriggerEnter2D/Stay2D/Exit2D
//     {other, point, normal, relativeVelocity}
//   OnBreak (el Destructible que se rompio)  {pieces, point}

#include "Modules.h"

#include "CramionCore/anim/MotionMatching.h"
#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/physics/Destruction.h"
#include "CramionCore/replay/Replay.h"
#include "CramionCore/twod/System2D.h"
#include "CramionCore/vfx/VisualEffect.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cramion::scripting::native {
namespace {

using core::Vec3;

constexpr float kRadToDeg = 57.2957795f;

struct EffectsState {
    // Roturas de los Destructible (OnBreak): las apunta el listener de la
    // fisica y se entregan en la fase Events.
    physics::PhysicsSystem* break_physics = nullptr;
    int break_listener = -1;
    std::vector<physics::BreakEvent> breaks;
};

core::Vec2 xy(const Vec3& v) { return core::Vec2{v.x, v.y}; }

twod::ForceMode2D forceMode2D(const api::Call& c, std::size_t i) {
    const std::string name = lowerText(c.string(i, "force"));
    if (name == "impulse") return twod::ForceMode2D::Impulse;
    if (name == "velocity" || name == "velocitychange") return twod::ForceMode2D::VelocityChange;
    return twod::ForceMode2D::Force;
}

// Mascara de capas opcional (nil = todas).
std::uint32_t maskArg(const api::Call& c, std::size_t i) {
    return c.has(i) ? static_cast<std::uint32_t>(c.integer(i)) : 0xFFFFFFFFu;
}

api::Value entityList(const Runtime& rt, const std::vector<ecs::Entity>& list) {
    api::Value::Array out;
    out.reserve(list.size());
    for (const ecs::Entity& e : list) out.push_back(rt.entityValue(e));
    return api::Value(std::move(out));
}

api::Value hitValue(const Runtime& rt, const twod::RaycastHit2D& h) {
    api::Value t = api::Value::object();
    t.set("entity", rt.entityValue(h.entity));
    t.set("point", Vec3{h.point.x, h.point.y, h.entity.valid() ? h.entity.worldPosition().z : 0.0f});
    t.set("normal", Vec3{h.normal.x, h.normal.y, 0.0f});
    t.set("distance", h.distance);
    t.set("fraction", h.fraction);
    return t;
}

template <typename T>
T* component(const ecs::Entity& e) {
    return e.valid() ? e.tryGet<T>() : nullptr;
}

// OnCollisionEnter2D... a los dos objetos de cada evento de la fisica 2D.
void dispatch2DEvents(Runtime& rt) {
    twod::System2D* s = twod::System2D::forWorld(rt.world);
    if (s == nullptr) return;
    const std::vector<twod::Physics2DEvent> events = s->takeEvents();  // se sacan siempre (si no, se acumulan)
    if (!rt.message_listener) return;
    for (const twod::Physics2DEvent& event : events) {
        const char* method = "OnCollisionEnter2D";
        switch (event.type) {
            case twod::Physics2DEventType::CollisionEnter: method = "OnCollisionEnter2D"; break;
            case twod::Physics2DEventType::CollisionStay: method = "OnCollisionStay2D"; break;
            case twod::Physics2DEventType::CollisionExit: method = "OnCollisionExit2D"; break;
            case twod::Physics2DEventType::TriggerEnter: method = "OnTriggerEnter2D"; break;
            case twod::Physics2DEventType::TriggerStay: method = "OnTriggerStay2D"; break;
            case twod::Physics2DEventType::TriggerExit: method = "OnTriggerExit2D"; break;
        }
        const twod::Physics2DEvent sides[2] = {event, event.flipped()};
        for (const twod::Physics2DEvent& side : sides) {
            if (!side.a.valid() || !side.b.valid()) continue;
            api::Value contact = api::Value::object();
            contact.set("other", rt.entityValue(side.b));
            contact.set("point", side.point);
            contact.set("normal", side.normal);
            contact.set("relativeVelocity", side.relative_velocity);
            rt.message_listener(side.a, method, rt.native.toJson(contact).dump());
        }
    }
}

// OnBreak de los Destructible que se rompieron.
void dispatchBreakEvents(Runtime& rt, EffectsState& state) {
    if (state.breaks.empty()) return;
    std::vector<physics::BreakEvent> queue;
    queue.swap(state.breaks);
    if (!rt.message_listener) return;
    for (const physics::BreakEvent& event : queue) {
        if (!event.entity.valid()) continue;
        api::Value arg = api::Value::object();
        arg.set("pieces", entityList(rt, event.pieces));
        arg.set("point", event.point);
        rt.message_listener(event.entity, "OnBreak", rt.native.toJson(arg).dump());
    }
}

void registerVfx(Runtime& rt) {
    api::NativeApi& api = rt.native;
    api.method("Entity", "playEffect", [&rt](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        if (vfx::VfxSystem* s = vfx::activeSystem()) s->play(e);
        return api::Value{};
    }, {"", "VisualEffect: empieza (o reinicia) el efecto"});
    api.method("Entity", "stopEffect", [&rt](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        if (vfx::VfxSystem* s = vfx::activeSystem()) s->stop(e, c.boolean(0, false));
        return api::Value{};
    }, {"borrar", "deja de emitir (true = borra las particulas)"});
    api.method("Entity", "pauseEffect", [&rt](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        if (vfx::VfxSystem* s = vfx::activeSystem()) s->pause(e, c.boolean(0, true));
        return api::Value{};
    }, {"true", "pausa o sigue la simulacion"});
    api.method("Entity", "isEffectPlaying", [&rt](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        const vfx::VfxSystem* s = vfx::activeSystem();
        return api::Value(s != nullptr && s->isPlaying(e));
    }, {"", "esta emitiendo?", "bool"});
    api.method("Entity", "sendEffectEvent", [&rt](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        const std::string name = c.string(0);
        if (vfx::VfxSystem* s = vfx::activeSystem()) s->sendEvent(e, name);
        return api::Value{};
    }, {"\"Explode\"", "evento del bloque Spawn 'Al recibir un evento'"});
    api.method("Entity", "setEffectFloat", [&rt](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        const std::string name = c.string(0);
        const float value = static_cast<float>(c.number(1));
        if (vfx::VfxSystem* s = vfx::activeSystem()) s->setFloat(e, name, value);
        return api::Value{};
    }, {"\"Rate\", 200", "parametro expuesto (float)"});
    api.method("Entity", "setEffectVector", [&rt](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        const std::string name = c.string(0);
        const Vec3 value = c.vec3(1);
        if (vfx::VfxSystem* s = vfx::activeSystem()) s->setVector(e, name, value);
        return api::Value{};
    }, {"\"Viento\", Vec3(1, 0, 0)", "parametro expuesto (Vector3)"});
    api.method("Entity", "setEffectColor", [&rt](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        const std::string name = c.string(0);
        const Vec3 rgb = c.vec3(1);
        const float alpha = static_cast<float>(c.number(2, 1.0));
        if (vfx::VfxSystem* s = vfx::activeSystem()) s->setColor(e, name, core::Vec4{rgb, alpha});
        return api::Value{};
    }, {"\"Color\", Vec3(1, 0.5, 0), 1", "parametro expuesto (color y alfa)"});
    api.method("Entity", "setEffectBool", [&rt](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        const std::string name = c.string(0);
        if (vfx::VfxSystem* s = vfx::activeSystem()) s->setBool(e, name, c.boolean(1, false));
        return api::Value{};
    }, {"\"Activo\", true", "parametro expuesto (bool)"});
    api.method("Entity", "getEffectFloat", [&rt](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        const std::string name = c.string(0);
        vfx::VfxSystem* s = vfx::activeSystem();
        core::Vec4 v{};
        if (s == nullptr || !s->getParam(e, name, v)) return api::Value{};
        return api::Value(v.x);
    }, {"\"Rate\"", "valor actual de un parametro (o nil)", "numero"});
    api.property("Entity", "effectParticles", [&rt](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        const vfx::VfxSystem* s = vfx::activeSystem();
        return api::Value(s != nullptr ? static_cast<int>(s->aliveCount(e)) : 0);
    }, {}, {"", "particulas vivas de su VisualEffect", "numero"}, true);
}

void register2D(Runtime& rt) {
    api::NativeApi& api = rt.native;
    const auto system2D = [&rt]() -> twod::System2D* { return twod::System2D::forWorld(rt.world); };

    // --- Rigidbody2D ---
    api.property("Entity", "velocity2D",
                 [&rt, system2D](api::Call& c) {
                     const ecs::Entity e = rt.selfEntity(c);
                     twod::System2D* s = system2D();
                     const core::Vec2 v = s != nullptr ? s->physics().velocity(e) : core::Vec2{};
                     return api::Value(Vec3{v.x, v.y, 0.0f});
                 },
                 [&rt, system2D](api::Call& c) {
                     const ecs::Entity e = rt.selfEntity(c);
                     const Vec3 v = c.vec3(0);
                     if (twod::System2D* s = system2D()) s->physics().setVelocity(e, xy(v));
                     return api::Value{};
                 },
                 {"", "Vec3 de su Rigidbody2D (z = 0)", "Vec3"}, true);
    api.property("Entity", "angularVelocity2D",
                 [&rt, system2D](api::Call& c) {
                     const ecs::Entity e = rt.selfEntity(c);
                     twod::System2D* s = system2D();
                     return api::Value(s != nullptr ? s->physics().angularVelocity(e) * kRadToDeg : 0.0f);
                 },
                 [&rt, system2D](api::Call& c) {
                     const ecs::Entity e = rt.selfEntity(c);
                     const float degrees = static_cast<float>(c.number(0));
                     if (twod::System2D* s = system2D()) s->physics().setAngularVelocity(e, degrees / kRadToDeg);
                     return api::Value{};
                 },
                 {"", "giro de su Rigidbody2D (grados/s)", "numero"}, true);
    api.method("Entity", "addForce2D", [&rt, system2D](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        const Vec3 f = c.vec3(0);
        const twod::ForceMode2D mode = forceMode2D(c, 1);
        if (twod::System2D* s = system2D()) s->physics().addForce(e, xy(f), mode);
        return api::Value{};
    }, {"Vec3(0, 5, 0), \"impulse\"", "Rigidbody2D: fuerza (force, impulse, velocity)"});
    api.method("Entity", "addForceAtPosition2D", [&rt, system2D](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        const Vec3 f = c.vec3(0);
        const Vec3 p = c.vec3(1);
        const twod::ForceMode2D mode = forceMode2D(c, 2);
        if (twod::System2D* s = system2D()) s->physics().addForceAtPosition(e, xy(f), xy(p), mode);
        return api::Value{};
    }, {"fuerza, punto, \"impulse\"", "fuerza en un punto (tambien gira)"});
    api.method("Entity", "addTorque2D", [&rt, system2D](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        const float torque = static_cast<float>(c.number(0));
        const twod::ForceMode2D mode = forceMode2D(c, 1);
        if (twod::System2D* s = system2D()) s->physics().addTorque(e, torque, mode);
        return api::Value{};
    }, {"par, \"impulse\"", "giro en el plano"});
    api.method("Entity", "movePosition2D", [&rt, system2D](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        const Vec3 p = c.vec3(0);
        if (twod::System2D* s = system2D()) s->physics().setPosition(e, xy(p));
        return api::Value{};
    }, {"Vec3(x, y, 0)", "lleva el cuerpo 2D ahi (sin chocar)"});
    api.method("Entity", "isSleeping2D", [&rt, system2D](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        twod::System2D* s = system2D();
        return api::Value(s != nullptr && s->physics().isSleeping(e));
    }, {"", "el cuerpo 2D esta dormido", "bool"});

    // --- Sprites ---
    api.method("Entity", "playSpriteAnimation", [&rt](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        const std::string clip = c.string(0);
        twod::SpriteAnimator* a = component<twod::SpriteAnimator>(e);
        if (a == nullptr || a->findClip(clip) == nullptr) return api::Value(false);
        a->play(clip, c.boolean(1, false));
        return api::Value(true);
    }, {"\"Correr\", reiniciar", "SpriteAnimator: cambia de clip (true si existe)", "bool"});
    api.property("Entity", "spriteAnimation", [&rt](api::Call& c) {
        const twod::SpriteAnimator* a = component<twod::SpriteAnimator>(rt.selfEntity(c));
        return api::Value(a != nullptr ? a->state.clip : std::string());
    }, {}, {"", "clip que suena en su SpriteAnimator", "texto"}, true);
    api.property("Entity", "spriteAnimationFinished", [&rt](api::Call& c) {
        const twod::SpriteAnimator* a = component<twod::SpriteAnimator>(rt.selfEntity(c));
        return api::Value(a != nullptr && a->state.finished);
    }, {}, {"", "el clip sin bucle termino", "bool"}, true);
    api.property("Entity", "spriteFrame",
                 [&rt](api::Call& c) {
                     const twod::SpriteRenderer* r = component<twod::SpriteRenderer>(rt.selfEntity(c));
                     return api::Value(r != nullptr ? r->frame : 0);
                 },
                 [&rt](api::Call& c) {
                     twod::SpriteRenderer* r = component<twod::SpriteRenderer>(rt.selfEntity(c));
                     const int frame = static_cast<int>(c.integer(0));
                     if (r != nullptr) r->frame = std::max(frame, 0);
                     return api::Value{};
                 },
                 {"", "corte de la hoja de su SpriteRenderer", "numero"}, true);
    api.property("Entity", "flipX",
                 [&rt](api::Call& c) {
                     const twod::SpriteRenderer* r = component<twod::SpriteRenderer>(rt.selfEntity(c));
                     return api::Value(r != nullptr && r->flip_x);
                 },
                 [&rt](api::Call& c) {
                     twod::SpriteRenderer* r = component<twod::SpriteRenderer>(rt.selfEntity(c));
                     if (r != nullptr) r->flip_x = c.boolean(0, false);
                     return api::Value{};
                 },
                 {"", "voltea su sprite", "bool"}, true);
    api.property("Entity", "spriteColor",
                 [&rt](api::Call& c) {
                     const twod::SpriteRenderer* r = component<twod::SpriteRenderer>(rt.selfEntity(c));
                     return api::Value(r != nullptr ? r->color : Vec3{1.0f, 1.0f, 1.0f});
                 },
                 [&rt](api::Call& c) {
                     twod::SpriteRenderer* r = component<twod::SpriteRenderer>(rt.selfEntity(c));
                     const Vec3 color = c.vec3(0);
                     if (r != nullptr) r->color = color;
                     return api::Value{};
                 },
                 {"", "Vec3 tinte de su sprite", "Vec3"}, true);

    // --- Tilemap: celdas en coordenadas de la rejilla (x, y); capas desde 1 ---
    api.method("Entity", "setTile", [&rt](api::Call& c) {
        twod::Tilemap* map = component<twod::Tilemap>(rt.selfEntity(c));
        const int cx = static_cast<int>(c.integer(0));
        const int cy = static_cast<int>(c.integer(1));
        const int id = static_cast<int>(c.integer(2));
        const int layer = std::max(static_cast<int>(c.integer(3, 1)) - 1, 0);
        return api::Value(map != nullptr && map->setTile(cx, cy, id, layer));
    }, {"x, y, id, capa", "Tilemap: pone una celda (id 0 = vacia, n = celda n-1 del tileset, -k = Rule Tile k)", "bool"});
    api.method("Entity", "getTile", [&rt](api::Call& c) {
        const twod::Tilemap* map = component<twod::Tilemap>(rt.selfEntity(c));
        const int cx = static_cast<int>(c.integer(0));
        const int cy = static_cast<int>(c.integer(1));
        const int layer = std::max(static_cast<int>(c.integer(2, 1)) - 1, 0);
        return api::Value(map != nullptr ? static_cast<int>(map->getTile(cx, cy, layer)) : 0);
    }, {"x, y, capa", "Tilemap: lo que hay en una celda", "numero"});
    // Dos resultados: {x, y}.
    api.method("Entity", "worldToCell", [&rt](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        const Vec3 p = c.vec3(0);
        const twod::Tilemap* map = component<twod::Tilemap>(e);
        int cx = 0, cy = 0;
        if (map != nullptr) map->cellAt(ecs::transformPoint(core::inverse(e.worldMatrix()), p), cx, cy);
        return api::Value(api::Value::Array{api::Value(cx), api::Value(cy)});
    }, {"posicion", "Tilemap: celda (x, y) de un punto del mundo", "lista: x, y"});
    api.method("Entity", "cellToWorld", [&rt](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        const int cx = static_cast<int>(c.integer(0));
        const int cy = static_cast<int>(c.integer(1));
        const twod::Tilemap* map = component<twod::Tilemap>(e);
        return api::Value(map != nullptr ? ecs::transformPoint(e.worldMatrix(), map->cellCenter(cx, cy)) : Vec3{});
    }, {"x, y", "Tilemap: centro de una celda en el mundo", "Vec3"});

    // --- Physics2D ---
    api.function("Physics2D.raycast", [&rt, system2D](api::Call& c) {
        const Vec3 origin = c.vec3(0);
        const Vec3 direction = c.vec3(1);
        const float distance = static_cast<float>(c.number(2, 1000.0));
        const std::uint32_t mask = maskArg(c, 3);
        twod::System2D* s = system2D();
        twod::RaycastHit2D hit;
        if (s == nullptr || !s->physics().raycast(xy(origin), xy(direction), distance, hit, mask)) return api::Value{};
        return hitValue(rt, hit);
    }, {"origen, direccion, distancia, mascara", "nil o {entity, point, normal, distance, fraction}", "objeto"});
    api.function("Physics2D.raycastAll", [&rt, system2D](api::Call& c) {
        const Vec3 origin = c.vec3(0);
        const Vec3 direction = c.vec3(1);
        const float distance = static_cast<float>(c.number(2, 1000.0));
        const std::uint32_t mask = maskArg(c, 3);
        api::Value::Array out;
        if (twod::System2D* s = system2D()) {
            for (const twod::RaycastHit2D& h : s->physics().raycastAll(xy(origin), xy(direction), distance, mask)) {
                out.push_back(hitValue(rt, h));
            }
        }
        return api::Value(std::move(out));
    }, {"origen, direccion, distancia, mascara", "lista de choques, del mas cercano al mas lejano", "lista de objetos"});
    api.function("Physics2D.overlapCircle", [&rt, system2D](api::Call& c) {
        const Vec3 center = c.vec3(0);
        const float radius = static_cast<float>(c.number(1));
        const std::uint32_t mask = maskArg(c, 2);
        twod::System2D* s = system2D();
        return entityList(rt, s != nullptr ? s->physics().overlapCircle(xy(center), radius, mask) : std::vector<ecs::Entity>{});
    }, {"centro, radio, mascara", "objetos con collider 2D dentro del circulo", "lista de Entity"});
    api.function("Physics2D.overlapBox", [&rt, system2D](api::Call& c) {
        const Vec3 center = c.vec3(0);
        const Vec3 size = c.vec3(1);
        const float angle = static_cast<float>(c.number(2, 0.0));
        const std::uint32_t mask = maskArg(c, 3);
        twod::System2D* s = system2D();
        return entityList(rt, s != nullptr ? s->physics().overlapBox(xy(center), xy(size), angle, mask) : std::vector<ecs::Entity>{});
    }, {"centro, tamano, angulo, mascara", "objetos dentro de la caja", "lista de Entity"});
    api.function("Physics2D.overlapPoint", [&rt, system2D](api::Call& c) {
        const Vec3 point = c.vec3(0);
        const std::uint32_t mask = maskArg(c, 1);
        twod::System2D* s = system2D();
        return entityList(rt, s != nullptr ? s->physics().overlapPoint(xy(point), mask) : std::vector<ecs::Entity>{});
    }, {"punto, mascara", "objetos que tocan el punto", "lista de Entity"});
    api.function("Physics2D.getGravity", [system2D](api::Call&) {
        twod::System2D* s = system2D();
        return api::Value(s != nullptr ? Vec3{s->physics().gravity.x, s->physics().gravity.y, 0.0f} : Vec3{0.0f, -9.81f, 0.0f});
    }, {"", "Vec3 gravedad 2D", "Vec3"});
    api.function("Physics2D.setGravity", [system2D](api::Call& c) {
        const Vec3 g = c.vec3(0);
        if (twod::System2D* s = system2D()) s->physics().gravity = xy(g);
        return api::Value{};
    }, {"Vec3(0, -9.81, 0)", "cambia la gravedad 2D"});
}

void registerDestruction(Runtime& rt) {
    api::NativeApi& api = rt.native;
    api.method("Entity", "fracture", [&rt](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        const Vec3 point = c.vec3(0, e.worldPosition());
        const float force = static_cast<float>(c.number(1, -1.0));
        return api::Value(rt.physics != nullptr && rt.physics->destruction().fracture(e, point, force));
    }, {"punto, fuerza", "Destructible: se rompe ya (fuerza en m/s hacia fuera)", "bool"});
    api.method("Entity", "damage", [&rt](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        const float amount = static_cast<float>(c.number(0));
        const Vec3 point = c.vec3(1, e.worldPosition());
        const float force = static_cast<float>(c.number(2, -1.0));
        if (rt.physics != nullptr) rt.physics->destruction().applyDamage(e, amount, point, force);
        return api::Value{};
    }, {"cantidad, punto, fuerza", "Destructible: quita vida (se rompe al llegar a 0)"});
    api.property("Entity", "health", [&rt](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        return api::Value(rt.physics != nullptr ? rt.physics->destruction().health(e) : 0.0f);
    }, {}, {"", "vida de su Destructible", "numero"}, true);
    api.property("Entity", "isBroken", [&rt](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        return api::Value(rt.physics != nullptr && rt.physics->destruction().isBroken(e));
    }, {}, {"", "su Destructible ya se rompio", "bool"}, true);
}

void registerVehicles(Runtime& rt) {
    api::NativeApi& api = rt.native;
    api.method("Entity", "setGear", [&rt](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        const int gear = static_cast<int>(c.integer(0));
        if (rt.physics != nullptr) rt.physics->setVehicleGear(e, gear);
        return api::Value{};
    }, {"marcha", "Vehicle con cambio manual: -1 atras, 0 punto muerto, 1..n"});
    api.method("Entity", "shiftGear", [&rt](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        const int delta = static_cast<int>(c.integer(0));
        if (rt.physics != nullptr) rt.physics->shiftVehicleGear(e, delta);
        return api::Value{};
    }, {"1", "Vehicle: sube (1) o baja (-1) una marcha"});
    api.method("Entity", "vehicleState", [&rt](api::Call& c) {
        const ecs::Entity e = rt.selfEntity(c);
        if (rt.physics == nullptr) return api::Value{};
        const physics::PhysicsSystem::VehicleState s = rt.physics->vehicleState(e);
        if (!s.valid) return api::Value{};
        api::Value t = api::Value::object();
        t.set("speed", s.speed_kmh);
        t.set("forwardSpeed", s.forward_speed_kmh);
        t.set("rpm", s.rpm);
        t.set("rpmFraction", s.rpm_fraction);
        t.set("gear", s.gear);
        t.set("gearCount", s.gear_count);
        t.set("automatic", s.automatic);
        return t;
    }, {"", "{speed, forwardSpeed, rpm, rpmFraction, gear, gearCount, automatic} o nil", "objeto"});
}

void registerMotionMatching(Runtime& rt) {
    api::NativeApi& api = rt.native;
    api.method("Entity", "setMotionVelocity", [&rt](api::Call& c) {
        anim::MotionMatching* mm = component<anim::MotionMatching>(rt.selfEntity(c));
        const Vec3 v = c.vec3(0);
        if (mm != nullptr) mm->runtime.desired_velocity = v;
        return api::Value{};
    }, {"Vec3", "Motion Matching (modo Script): velocidad que se pide (m/s)"});
    api.method("Entity", "setMotionFacing", [&rt](api::Call& c) {
        anim::MotionMatching* mm = component<anim::MotionMatching>(rt.selfEntity(c));
        const Vec3 v = c.vec3(0);
        if (mm != nullptr) mm->runtime.desired_facing = v;
        return api::Value{};
    }, {"Vec3", "Motion Matching: hacia donde mirar (cero = hacia donde se mueve)"});
    api.method("Entity", "setMotionTags", [&rt](api::Call& c) {
        anim::MotionMatching* mm = component<anim::MotionMatching>(rt.selfEntity(c));
        const std::string tags = c.string(0);
        if (mm != nullptr) mm->runtime.extra_tags = tags;
        return api::Value{};
    }, {"\"agachado\"", "Motion Matching: solo fotogramas con esas etiquetas"});
    api.property("Entity", "motionClip", [&rt](api::Call& c) {
        const anim::MotionMatching* mm = component<anim::MotionMatching>(rt.selfEntity(c));
        return api::Value(mm != nullptr ? mm->runtime.clip_label : std::string());
    }, {}, {"", "clip que eligio el Motion Matching", "texto"}, true);
}

void registerReplay(Runtime& rt) {
    api::NativeApi& api = rt.native;
    api.function("Replay.start", [&rt](api::Call& c) {
        const api::Value& options = c.object(0);
        replay::ReplaySystem* r = replay::activeSystem();
        if (r == nullptr || rt.world == nullptr) return api::Value(false);
        replay::ReplayOptions o;
        if (options["rate"].isNumber()) o.rate = static_cast<float>(options["rate"].asNumber());
        if (options["maxSeconds"].isNumber()) o.max_seconds = static_cast<float>(options["maxSeconds"].asNumber());
        if (options["tag"].isString()) o.tag = options["tag"].asString();
        if (options["audio"].isBool()) o.record_audio = options["audio"].truthy();
        if (options["animation"].isBool()) o.record_animation = options["animation"].truthy();
        return api::Value(r->startRecording(*rt.world, o));
    }, {"{rate = 30, maxSeconds = 10, tag = \"Coche\"}", "empieza a grabar", "bool"});
    api.function("Replay.stop", [](api::Call&) {
        if (replay::ReplaySystem* r = replay::activeSystem()) r->stopRecording();
        return api::Value{};
    }, {"", "deja de grabar"});
    api.function("Replay.play", [&rt](api::Call& c) {
        const float from = static_cast<float>(c.number(0, 0.0));
        const float speed = static_cast<float>(c.number(1, 1.0));
        replay::ReplaySystem* r = replay::activeSystem();
        return api::Value(r != nullptr && rt.world != nullptr && r->play(*rt.world, from, speed));
    }, {"desde, velocidad", "reproduce (desde < 0 = los ultimos N segundos)", "bool"});
    api.function("Replay.stopPlayback", [&rt](api::Call&) {
        replay::ReplaySystem* r = replay::activeSystem();
        if (r != nullptr && rt.world != nullptr) r->stop(*rt.world);
        return api::Value{};
    }, {"", "vuelve al juego"});
    api.function("Replay.seek", [](api::Call& c) {
        const float t = static_cast<float>(c.number(0));
        if (replay::ReplaySystem* r = replay::activeSystem()) r->seek(t);
        return api::Value{};
    }, {"segundos", "salta a ese momento"});
    api.function("Replay.pause", [](api::Call& c) {
        if (replay::ReplaySystem* r = replay::activeSystem()) r->setPaused(c.boolean(0, true));
        return api::Value{};
    }, {"true", "pausa la reproduccion"});
    api.function("Replay.setSpeed", [](api::Call& c) {
        const float speed = static_cast<float>(c.number(0));
        if (replay::ReplaySystem* r = replay::activeSystem()) r->setSpeed(speed);
        return api::Value{};
    }, {"0.25", "camara lenta / rapida"});
    api.function("Replay.setLoop", [](api::Call& c) {
        if (replay::ReplaySystem* r = replay::activeSystem()) r->setLoop(c.boolean(0, false));
        return api::Value{};
    }, {"true", "en bucle"});
    api.function("Replay.setFreeCamera", [](api::Call& c) {
        if (replay::ReplaySystem* r = replay::activeSystem()) r->setFreeCamera(c.boolean(0, false));
        return api::Value{};
    }, {"true", "camara libre (WASD + raton)"});
    api.function("Replay.mark", [](api::Call& c) {
        const std::string name = c.string(0);
        const std::string data = c.string(1, "");
        if (replay::ReplaySystem* r = replay::activeSystem()) r->mark(name, data);
        return api::Value{};
    }, {"\"Gol\", datos", "marcador en la linea de tiempo"});
    api.function("Replay.save", [](api::Call& c) {
        const std::string name = c.string(0);
        replay::ReplaySystem* r = replay::activeSystem();
        return api::Value(r != nullptr && r->save(r->fileFor(name)));
    }, {"\"gol\"", "guarda un .crreplay", "bool"});
    api.function("Replay.load", [](api::Call& c) {
        const std::string name = c.string(0);
        replay::ReplaySystem* r = replay::activeSystem();
        return api::Value(r != nullptr && r->load(r->fileFor(name)));
    }, {"\"gol\"", "carga un .crreplay", "bool"});
    api.function("Replay.list", [](api::Call&) {
        api::Value::Array out;
        if (replay::ReplaySystem* r = replay::activeSystem()) {
            for (const std::string& name : r->list()) out.emplace_back(name);
        }
        return api::Value(std::move(out));
    }, {"", "repeticiones guardadas", "lista de textos"});
    api.function("Replay.isRecording", [](api::Call&) {
        const replay::ReplaySystem* r = replay::activeSystem();
        return api::Value(r != nullptr && r->recording());
    }, {"", "esta grabando?", "bool"});
    api.function("Replay.isPlaying", [](api::Call&) {
        const replay::ReplaySystem* r = replay::activeSystem();
        return api::Value(r != nullptr && r->playing());
    }, {"", "esta reproduciendo?", "bool"});
    api.function("Replay.time", [](api::Call&) {
        const replay::ReplaySystem* r = replay::activeSystem();
        return api::Value(r != nullptr ? r->time() : 0.0f);
    }, {"", "segundo de la reproduccion", "numero"});
    api.function("Replay.duration", [](api::Call&) {
        const replay::ReplaySystem* r = replay::activeSystem();
        return api::Value(r != nullptr ? r->duration() : 0.0f);
    }, {"", "segundos grabados", "numero"});
}

}  // namespace

void registerEffectsApi(Runtime& rt) {
    auto state = std::make_shared<EffectsState>();
    registerVfx(rt);
    register2D(rt);
    registerDestruction(rt);
    registerVehicles(rt);
    registerMotionMatching(rt);
    registerReplay(rt);

    // Roturas: el listener de la fisica vive mientras corre el juego.
    rt.onStart([&rt, state] {
        if (rt.physics == nullptr || state->break_listener >= 0) return;
        const std::weak_ptr<EffectsState> weak = state;
        state->break_physics = rt.physics;
        state->break_listener = rt.physics->destruction().addBreakListener([weak](const physics::BreakEvent& event) {
            if (const auto s = weak.lock()) s->breaks.push_back(event);
        });
    });
    rt.onStop([&rt, state](bool) {
        // Solo si la fisica sigue siendo la misma (si no, ya no es nuestra).
        if (state->break_listener >= 0 && state->break_physics != nullptr && state->break_physics == rt.physics) {
            state->break_physics->destruction().removeBreakListener(state->break_listener);
        }
        state->break_physics = nullptr;
        state->break_listener = -1;
        state->breaks.clear();
    });
    rt.onFrame(Phase::Events, [&rt, state](float) {
        dispatch2DEvents(rt);
        dispatchBreakEvents(rt, *state);
    });
}

}  // namespace cramion::scripting::native
