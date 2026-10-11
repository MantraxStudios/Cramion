// Entity: transformacion, jerarquia, componentes, fisica, sonido, animacion e interfaz.
// (Los huesos, el IK, el ragdoll y la navegacion estan en RigApi.cpp; lo de
// red, en NetworkApi.cpp.)

#include "Modules.h"

#include "../ComponentFields.h"

#include "CramionCore/asset/AssetTypes.h"
#include "CramionCore/asset/MaterialAsset.h"
#include "CramionCore/audio/Audio.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/RuntimeMesh.h"
#include "CramionCore/physics/Cloth.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/physics/SoftBody.h"
#include "CramionCore/ui/UI.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace cramion::scripting::native {

namespace {

using core::Vec3;

void property(Runtime& rt, const char* name, api::Function get, api::Function set, api::Doc doc) {
    rt.native.property("Entity", name, std::move(get), std::move(set), std::move(doc), true);
}

void method(Runtime& rt, const char* name, api::Function fn, api::Doc doc) {
    rt.native.method("Entity", name, std::move(fn), std::move(doc));
}

// El tipo de componente por su nombre ("Rigidbody", "Light"...) si la entidad
// lo tiene (nullptr si no).
const ecs::ComponentType* componentOf(const ecs::Entity& x, const std::string& name) {
    const ecs::ComponentType* type = ecs::ComponentRegistry::instance().find(name);
    if (type == nullptr || x.world() == nullptr || !type->has(*x.world(), x.handle())) return nullptr;
    return type;
}

// La simulacion de su tela o de su cuerpo blando (se crea si aun no hay).
template <typename T>
physics::ClothRuntime* clothRuntime(const ecs::Entity& x) {
    T* component = x.tryGet<T>();
    if (component == nullptr) return nullptr;
    if (!component->runtime.ptr) component->runtime.ptr = std::make_shared<physics::ClothRuntime>();
    return component->runtime.ptr.get();
}

// --- Campos de componentes (getField / setField / getFields) ---
api::Value fieldValue(const PostValue& v) {
    switch (v.type) {
        case PostValue::Type::Bool: return v.flag;
        case PostValue::Type::Number: return v.number;
        case PostValue::Type::Text: return v.text;
        case PostValue::Type::Vector: return v.vector;
        case PostValue::Type::None: break;
    }
    return {};
}

// Del script al campo: bool, numero, texto, Vec3, una entidad (su UUID) o nil
// (texto vacio: quita la referencia).
bool fieldFromValue(const Runtime& rt, const api::Value& value, PostValue& out) {
    switch (value.type()) {
        case api::Value::Type::Bool:
            out.type = PostValue::Type::Bool;
            out.flag = value.truthy();
            return true;
        case api::Value::Type::Number:
            out.type = PostValue::Type::Number;
            out.number = value.asNumber();
            return true;
        case api::Value::Type::String:
            out.type = PostValue::Type::Text;
            out.text = value.asString();
            return true;
        case api::Value::Type::Vec3:
            out.type = PostValue::Type::Vector;
            out.vector = value.asVec3();
            return true;
        case api::Value::Type::Entity: {
            const ecs::Entity x = rt.entity(value.asEntity());
            out.type = PostValue::Type::Text;
            out.text = x.valid() ? x.uuid().toString() : std::string();
            return true;
        }
        case api::Value::Type::Nil:
            out.type = PostValue::Type::Text;
            out.text.clear();
            return true;
        default: return false;
    }
}

// --- Transformacion y jerarquia ---
void registerTransform(Runtime& rt) {
    method(rt, "valid", [&rt](api::Call& c) -> api::Value { return rt.entity(c.selfEntity()).valid(); },
           {"", "sigue existiendo?", "bool"});
    // Lo que en Lua eran == y tostring(e).
    method(rt, "equals", [](api::Call& c) -> api::Value { return c.selfEntity() == c.entity(0); },
           {"otra", "es la misma entidad?", "bool"});
    method(rt, "toString",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity x = rt.entity(c.selfEntity());
               return "Entity(" + (x.valid() ? x.name() : std::string("destruida")) + ")";
           },
           {"", "texto para mostrar: Entity(nombre)", "texto"});

    property(rt, "name", [&rt](api::Call& c) -> api::Value { return rt.selfEntity(c).name(); },
             [&rt](api::Call& c) -> api::Value {
                 rt.selfEntity(c).setName(c.string(0));
                 return {};
             },
             {"", "nombre", "texto"});
    property(rt, "tag", [&rt](api::Call& c) -> api::Value { return rt.selfEntity(c).tag(); },
             [&rt](api::Call& c) -> api::Value {
                 rt.selfEntity(c).setTag(c.string(0));
                 return {};
             },
             {"", "tag", "texto"});
    property(rt, "active", [&rt](api::Call& c) -> api::Value { return rt.selfEntity(c).activeSelf(); },
             [&rt](api::Call& c) -> api::Value {
                 rt.selfEntity(c).setActive(c.boolean(0));
                 return {};
             },
             {"", "activo (true/false)", "bool"});
    property(rt, "position", [&rt](api::Call& c) -> api::Value { return rt.selfEntity(c).worldPosition(); },
             [&rt](api::Call& c) -> api::Value {
                 rt.selfEntity(c).setWorldPosition(c.vec3(0));
                 return {};
             },
             {"", "Vec3 en el mundo", "Vec3"});
    property(rt, "localPosition", [&rt](api::Call& c) -> api::Value { return rt.selfEntity(c).localPosition(); },
             [&rt](api::Call& c) -> api::Value {
                 rt.selfEntity(c).setLocalPosition(c.vec3(0));
                 return {};
             },
             {"", "Vec3 respecto al padre", "Vec3"});
    property(rt, "rotation", [&rt](api::Call& c) -> api::Value { return rt.selfEntity(c).localEulerDegrees(); },
             [&rt](api::Call& c) -> api::Value {
                 rt.selfEntity(c).setLocalEulerDegrees(c.vec3(0));
                 return {};
             },
             {"", "Vec3 en grados", "Vec3"});
    property(rt, "quaternion", [&rt](api::Call& c) -> api::Value { return rt.selfEntity(c).localRotation(); },
             [&rt](api::Call& c) -> api::Value {
                 rt.selfEntity(c).setLocalRotation(c.quat(0));
                 return {};
             },
             {"", "Quat de su giro", "Quat"});
    property(rt, "scale", [&rt](api::Call& c) -> api::Value { return rt.selfEntity(c).localScale(); },
             [&rt](api::Call& c) -> api::Value {
                 rt.selfEntity(c).setLocalScale(c.vec3(0));
                 return {};
             },
             {"", "Vec3 (escala local)", "Vec3"});
    property(rt, "forward", [&rt](api::Call& c) -> api::Value { return rt.selfEntity(c).forward(); }, {},
             {"", "Vec3 hacia delante", "Vec3"});
    property(rt, "right", [&rt](api::Call& c) -> api::Value { return rt.selfEntity(c).right(); }, {},
             {"", "Vec3 a la derecha", "Vec3"});
    property(rt, "up", [&rt](api::Call& c) -> api::Value { return rt.selfEntity(c).up(); }, {},
             {"", "Vec3 hacia arriba", "Vec3"});
    property(rt, "parent", [&rt](api::Call& c) -> api::Value { return rt.entityValue(rt.selfEntity(c).parent()); }, {},
             {"", "el padre (no valida si no tiene)", "Entity"});

    method(rt, "translate",
           [&rt](api::Call& c) -> api::Value {
               ecs::Entity x = rt.selfEntity(c);
               x.setWorldPosition(x.worldPosition() + c.vec3(0));
               return {};
           },
           {"Vec3", "mueve en el mundo"});
    method(rt, "translateLocal",
           [&rt](api::Call& c) -> api::Value {
               ecs::Entity x = rt.selfEntity(c);
               const Vec3 d = c.vec3(0);
               x.setWorldPosition(x.worldPosition() + x.right() * d.x + x.up() * d.y - x.forward() * d.z);
               return {};
           },
           {"Vec3", "mueve en sus ejes"});
    method(rt, "rotate",
           [&rt](api::Call& c) -> api::Value {
               ecs::Entity x = rt.selfEntity(c);
               x.setLocalEulerDegrees(x.localEulerDegrees() + c.vec3(0));
               return {};
           },
           {"Vec3 grados", "gira"});
    method(rt, "lookAt",
           [&rt](api::Call& c) -> api::Value {
               ecs::Entity x = rt.selfEntity(c);
               const Vec3 d = c.vec3(0) - x.worldPosition();
               const float len = core::length(d);
               if (len < 1e-5f) return {};
               const Vec3 dir = d * (1.0f / len);
               constexpr float kDeg = 57.2957795f;
               x.setLocalEulerDegrees(
                   Vec3{std::asin(std::clamp(dir.y, -1.0f, 1.0f)) * kDeg, std::atan2(-dir.x, -dir.z) * kDeg, 0.0f});
               return {};
           },
           {"Vec3", "mira hacia un punto"});
    method(rt, "distanceTo",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity a = rt.selfEntity(c);
               const ecs::Entity b = rt.entityArg(c, 0);
               return b.valid() ? core::length(a.worldPosition() - b.worldPosition()) : 0.0f;
           },
           {"otra", "distancia a otro objeto", "numero"});
    // Al final del frame, con sus hijos (los modulos sueltan lo suyo antes).
    // Como antes, destruir una entidad que ya no existe no hace nada.
    method(rt, "destroy",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity x = rt.entity(c.selfEntity());
               if (x.valid()) rt.destroyLater(x.handle());
               return {};
           },
           {"", "lo destruye (al final del frame, con sus hijos)"});
    method(rt, "find",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity x = rt.selfEntity(c);
               const std::string name = c.string(0);
               std::vector<ecs::Entity> stack{x};
               while (!stack.empty()) {
                   const ecs::Entity current = stack.back();
                   stack.pop_back();
                   for (const entt::entity child : current.children()) {
                       const ecs::Entity found = x.world()->wrap(child);
                       if (found.name() == name) return rt.entityValue(found);
                       stack.push_back(found);
                   }
               }
               return rt.entityValue(ecs::Entity{});
           },
           {"\"hijo\"", "un hijo por nombre", "Entity"});
}

// --- Fisica y vehiculos ---
void registerPhysics(Runtime& rt) {
    property(rt, "velocity",
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity x = rt.selfEntity(c);
                 return rt.physics != nullptr ? rt.physics->linearVelocity(x) : Vec3{};
             },
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity x = rt.selfEntity(c);
                 const Vec3 v = c.vec3(0);
                 if (rt.physics != nullptr) rt.physics->setLinearVelocity(x, v);
                 return {};
             },
             {"", "Vec3 de su Rigidbody", "Vec3"});
    property(rt, "angularVelocity",
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity x = rt.selfEntity(c);
                 return rt.physics != nullptr ? rt.physics->angularVelocity(x) : Vec3{};
             },
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity x = rt.selfEntity(c);
                 const Vec3 v = c.vec3(0);
                 if (rt.physics != nullptr) rt.physics->setAngularVelocity(x, v);
                 return {};
             },
             {"", "Vec3 de giro (rad/s)", "Vec3"});
    method(rt, "addForce",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity x = rt.selfEntity(c);
               const Vec3 f = c.vec3(0);
               const std::string name = lowerText(c.string(1, "force"));
               if (rt.physics == nullptr) return {};
               physics::ForceMode m = physics::ForceMode::Force;
               if (name == "impulse") m = physics::ForceMode::Impulse;
               if (name == "acceleration") m = physics::ForceMode::Acceleration;
               if (name == "velocity" || name == "velocitychange") m = physics::ForceMode::VelocityChange;
               rt.physics->addForce(x, f, m);
               return {};
           },
           {"Vec3, \"impulse\"", "fuerza (force, impulse, acceleration, velocity)"});
    method(rt, "addImpulse",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity x = rt.selfEntity(c);
               const Vec3 f = c.vec3(0);
               if (rt.physics != nullptr) rt.physics->addImpulse(x, f);
               return {};
           },
           {"Vec3", "impulso"});
    method(rt, "addTorque",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity x = rt.selfEntity(c);
               const Vec3 t = c.vec3(0);
               if (rt.physics != nullptr) rt.physics->addTorque(x, t, physics::ForceMode::Force);
               return {};
           },
           {"Vec3", "par de giro"});
    // Vehiculos (Vehicle + WheelCollider): acelerador -1..1, direccion -1..1, frenos 0..1.
    method(rt, "setVehicleInput",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity x = rt.selfEntity(c);
               const auto throttle = static_cast<float>(c.number(0));
               const auto steering = static_cast<float>(c.number(1));
               const auto brake = static_cast<float>(c.number(2, 0.0));
               const auto handbrake = static_cast<float>(c.number(3, 0.0));
               if (rt.physics != nullptr) rt.physics->setVehicleInput(x, throttle, steering, brake, handbrake);
               return {};
           },
           {"acelerador, direccion, freno, freno de mano", "conduce su Vehicle"});
    property(rt, "speed",
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity x = rt.selfEntity(c);
                 return rt.physics != nullptr ? rt.physics->vehicleState(x).speed_kmh : 0.0f;
             },
             {}, {"", "km/h de su Vehicle", "numero"});
    property(rt, "rpm",
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity x = rt.selfEntity(c);
                 return rt.physics != nullptr ? rt.physics->vehicleState(x).rpm : 0.0f;
             },
             {}, {"", "rpm del motor del Vehicle", "numero"});
    property(rt, "gear",
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity x = rt.selfEntity(c);
                 return rt.physics != nullptr ? rt.physics->vehicleState(x).gear : 0;
             },
             {}, {"", "marcha del Vehicle", "numero"});
}

// --- Sonido y animacion ---
void registerSoundAndAnimation(Runtime& rt) {
    method(rt, "playSound",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity x = rt.selfEntity(c);
               if (rt.audio != nullptr) rt.audio->play(x);
               return {};
           },
           {"", "suena su Audio Source"});
    method(rt, "stopSound",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity x = rt.selfEntity(c);
               if (rt.audio != nullptr) rt.audio->stop(x);
               return {};
           },
           {"", "para su sonido"});
    method(rt, "isPlayingSound",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity x = rt.selfEntity(c);
               return rt.audio != nullptr && rt.audio->isPlaying(x);
           },
           {"", "suena?", "bool"});
    // Efectos del AudioSource: "lowpass"|"highpass"|"echo"|"reverb"|"occlusion", activo, valor.
    method(rt, "setSoundEffect",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity x = rt.selfEntity(c);
               const std::string n = lowerText(c.string(0));
               const bool enabled = c.boolean(1);
               const bool has_value = c.has(2);
               const auto value = static_cast<float>(c.number(2, 1.0));
               audio::AudioSource* a = x.tryGet<audio::AudioSource>();
               if (a == nullptr) return false;
               if (n == "lowpass") {
                   a->low_pass = enabled;
                   if (has_value) a->low_pass_cutoff = value;
               } else if (n == "highpass") {
                   a->high_pass = enabled;
                   if (has_value) a->high_pass_cutoff = value;
               } else if (n == "echo") {
                   a->echo = enabled;
                   if (has_value) a->echo_delay = value;
               } else if (n == "reverb") {
                   a->reverb_send = enabled ? value : 0.0f;
               } else if (n == "occlusion") {
                   a->occlusion = enabled;
               } else {
                   return false;
               }
               return true;
           },
           {"\"lowpass\", true, 800", "lowpass/highpass/echo/reverb/occlusion", "bool"});
    // Paredes que tapan ahora este sonido (-1 si no suena).
    property(rt, "soundOcclusion",
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity x = rt.selfEntity(c);
                 return rt.audio != nullptr ? rt.audio->occlusionOf(x) : -1.0f;
             },
             {}, {"", "paredes que tapan su sonido ahora (-1 = no suena)", "numero"});
    // La oclusion del AudioListener de esta entidad (camara).
    property(rt, "audioOcclusion",
             [&rt](api::Call& c) -> api::Value {
                 const audio::AudioListener* l = rt.selfEntity(c).tryGet<audio::AudioListener>();
                 return l != nullptr && l->occlusion;
             },
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity x = rt.selfEntity(c);
                 const bool on = c.boolean(0);
                 if (audio::AudioListener* l = x.tryGet<audio::AudioListener>()) l->occlusion = on;
                 return {};
             },
             {"", "oclusion de su Audio Listener (true/false)", "bool"});

    method(rt, "playAnimation",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity x = rt.selfEntity(c);
               const std::string clip = c.string(0);
               const bool loop = c.boolean(1, true);
               if (ecs::Animator* a = x.tryGet<ecs::Animator>()) {
                   a->clip_name = clip;
                   a->loop = loop;
                   a->playing = true;
                   a->time = 0.0f;
               }
               return {};
           },
           {"\"Correr\", true", "clip del Animator"});
    method(rt, "setAnimatorFloat",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity x = rt.selfEntity(c);
               const std::string n = c.string(0);
               const auto v = static_cast<float>(c.number(1));
               if (ecs::Animator* a = x.tryGet<ecs::Animator>()) a->setFloat(n, v);
               return {};
           },
           {"\"velocidad\", 1.0", "parametro del Animator Controller"});
    method(rt, "setAnimatorBool",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity x = rt.selfEntity(c);
               const std::string n = c.string(0);
               const bool v = c.boolean(1);
               if (ecs::Animator* a = x.tryGet<ecs::Animator>()) a->setBool(n, v);
               return {};
           },
           {"\"saltando\", true", "parametro bool del Animator Controller"});
    method(rt, "setAnimatorTrigger",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity x = rt.selfEntity(c);
               const std::string n = c.string(0);
               if (ecs::Animator* a = x.tryGet<ecs::Animator>()) a->setTrigger(n);
               return {};
           },
           {"\"atacar\"", "trigger del Animator Controller"});
    // Animator.CrossFade: salta a un estado del controlador por su nombre.
    method(rt, "crossFade",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity x = rt.selfEntity(c);
               const std::string state = c.string(0);
               const auto seconds = static_cast<float>(c.number(1, 0.15));
               const bool inertial = c.boolean(2, true);
               if (ecs::Animator* a = x.tryGet<ecs::Animator>()) {
                   a->runtime.cross_fade = state;
                   a->runtime.cross_fade_time = seconds;
                   a->runtime.cross_fade_inertial = inertial;
                   a->playing = true;
               }
               return {};
           },
           {"\"Golpe\", 0.1, true", "salta a un estado del Animator por su nombre (fundido; true = inercial)"});
    method(rt, "animatorState",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Animator* a = rt.selfEntity(c).tryGet<ecs::Animator>();
               if (a == nullptr) return std::string();
               // El pedido este frame (crossFade) cuenta ya como el actual.
               return !a->runtime.cross_fade.empty() ? a->runtime.cross_fade : a->runtime.state_name;
           },
           {"", "nombre del estado actual del Animator", "texto"});
    method(rt, "animatorStateTime",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Animator* a = rt.selfEntity(c).tryGet<ecs::Animator>();
               return a != nullptr && a->runtime.cross_fade.empty() ? a->runtime.state_time : 0.0f;
           },
           {"", "segundos en el estado actual del Animator", "numero"});
}

// --- Interfaz: texto, valor, color, imagen y rectangulo ---
void registerUi(Runtime& rt) {
    // El texto (Texto o Campo), el valor (Slider o Casilla) y si responde.
    property(rt, "text",
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity x = rt.selfEntity(c);
                 if (const auto* t = x.tryGet<ui::Text>()) return t->text;
                 if (const auto* f = x.tryGet<ui::InputField>()) return f->text;
                 return std::string();
             },
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity x = rt.selfEntity(c);
                 const std::string text = c.string(0);
                 if (auto* t = x.tryGet<ui::Text>()) t->text = text;
                 if (auto* f = x.tryGet<ui::InputField>()) f->text = text;
                 return {};
             },
             {"", "texto de su UIText o campo de texto", "texto"});
    property(rt, "value",
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity x = rt.selfEntity(c);
                 if (const auto* s = x.tryGet<ui::Slider>()) return s->value;
                 if (const auto* t = x.tryGet<ui::Toggle>()) return t->on ? 1.0f : 0.0f;
                 return 0.0f;
             },
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity x = rt.selfEntity(c);
                 const auto value = static_cast<float>(c.number(0));
                 if (auto* s = x.tryGet<ui::Slider>()) s->value = std::clamp(value, std::min(s->min, s->max), std::max(s->min, s->max));
                 if (auto* t = x.tryGet<ui::Toggle>()) t->on = value != 0.0f;
                 return {};
             },
             {"", "valor de su Slider o Casilla", "numero"});
    property(rt, "interactable",
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity x = rt.selfEntity(c);
                 if (const auto* b = x.tryGet<ui::Button>()) return b->interactable;
                 if (const auto* s = x.tryGet<ui::Slider>()) return s->interactable;
                 if (const auto* f = x.tryGet<ui::InputField>()) return f->interactable;
                 if (const auto* t = x.tryGet<ui::Toggle>()) return t->interactable;
                 return false;
             },
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity x = rt.selfEntity(c);
                 const bool on = c.boolean(0);
                 if (auto* b = x.tryGet<ui::Button>()) b->interactable = on;
                 if (auto* s = x.tryGet<ui::Slider>()) s->interactable = on;
                 if (auto* f = x.tryGet<ui::InputField>()) f->interactable = on;
                 if (auto* t = x.tryGet<ui::Toggle>()) t->interactable = on;
                 return {};
             },
             {"", "su boton/slider/campo responde", "bool"});
    property(rt, "color",
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity x = rt.selfEntity(c);
                 if (const auto* i = x.tryGet<ui::Image>()) return i->color;
                 if (const auto* t = x.tryGet<ui::Text>()) return t->color;
                 return Vec3{1, 1, 1};
             },
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity x = rt.selfEntity(c);
                 const Vec3 color = c.vec3(0);
                 if (auto* i = x.tryGet<ui::Image>()) i->color = color;
                 if (auto* t = x.tryGet<ui::Text>()) t->color = color;
                 return {};
             },
             {"", "Vec3 color de su UIImage/UIText", "Vec3"});
    property(rt, "texture",
             [&rt](api::Call& c) -> api::Value {
                 const ui::Image* i = rt.selfEntity(c).tryGet<ui::Image>();
                 return i != nullptr ? i->texture : std::string();
             },
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity x = rt.selfEntity(c);
                 const std::string path = c.string(0);
                 if (ui::Image* i = x.tryGet<ui::Image>()) i->texture = path;
                 return {};
             },
             {"", "imagen de su UIImage", "texto"});
    property(rt, "alpha",
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity x = rt.selfEntity(c);
                 if (const auto* i = x.tryGet<ui::Image>()) return i->alpha;
                 if (const auto* t = x.tryGet<ui::Text>()) return t->alpha;
                 return 1.0f;
             },
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity x = rt.selfEntity(c);
                 const auto a = static_cast<float>(c.number(0));
                 if (auto* i = x.tryGet<ui::Image>()) i->alpha = a;
                 if (auto* t = x.tryGet<ui::Text>()) t->alpha = a;
                 return {};
             },
             {"", "transparencia de su UIImage/UIText", "numero"});
    property(rt, "uiPosition",
             [&rt](api::Call& c) -> api::Value {
                 const ui::RectTransform* r = rt.selfEntity(c).tryGet<ui::RectTransform>();
                 return r != nullptr ? Vec3{r->position.x, r->position.y, 0.0f} : Vec3{};
             },
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity x = rt.selfEntity(c);
                 const Vec3 p = c.vec3(0);
                 if (ui::RectTransform* r = x.tryGet<ui::RectTransform>()) r->position = core::Vec2{p.x, p.y};
                 return {};
             },
             {"", "Vec3 posicion de su RectTransform", "Vec3"});
    property(rt, "uiSize",
             [&rt](api::Call& c) -> api::Value {
                 const ui::RectTransform* r = rt.selfEntity(c).tryGet<ui::RectTransform>();
                 return r != nullptr ? Vec3{r->size.x, r->size.y, 0.0f} : Vec3{};
             },
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity x = rt.selfEntity(c);
                 const Vec3 s = c.vec3(0);
                 if (ui::RectTransform* r = x.tryGet<ui::RectTransform>()) r->size = core::Vec2{s.x, s.y};
                 return {};
             },
             {"", "Vec3 tamano de su RectTransform", "Vec3"});
}

// --- Malla, material y sombras del MeshRenderer ---
void registerRendering(Runtime& rt) {
    // La malla creada por codigo del MeshRenderer (lo anade si no hay).
    property(rt, "mesh",
             [&rt](api::Call& c) -> api::Value {
                 const ecs::MeshRenderer* r = rt.selfEntity(c).tryGet<ecs::MeshRenderer>();
                 return r != nullptr ? meshValue(rt, r->mesh) : api::Value{};
             },
             [&rt](api::Call& c) -> api::Value {
                 ecs::Entity x = rt.selfEntity(c);
                 std::shared_ptr<ecs::Mesh> mesh;
                 if (c.has(0)) {
                     mesh = meshOf(c.arg(0));
                     if (!mesh) throw api::Error("se esperaba un Mesh o nil");
                 }
                 ecs::MeshRenderer* r = x.tryGet<ecs::MeshRenderer>();
                 if (r == nullptr) {
                     if (!mesh) return {};
                     r = &x.add<ecs::MeshRenderer>();
                 }
                 r->mesh = std::move(mesh);
                 return {};
             },
             {"", "malla creada por codigo de su MeshRenderer", "Mesh"});
    // Material .crmat de un hueco del MeshRenderer (submalla, desde 0), o nil para el suyo.
    method(rt, "setMaterial",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity x = rt.selfEntity(c);
               const long long slot = c.integer(0);
               const std::string path = c.string(1, "");
               ecs::MeshRenderer* r = x.tryGet<ecs::MeshRenderer>();
               if (r == nullptr || slot < 0) return false;
               assets::AssetRef ref{{}, assets::AssetType::Material};
               if (!path.empty()) {
                   assets::MaterialAsset m;
                   std::filesystem::path file = rt.root / pathFromUtf8(path);
                   if (file.extension() != ".crmat") file += ".crmat";
                   if (!assets::loadMaterial(file, m)) {
                       rt.write(2, "setMaterial: no se puede leer el material \"" + path + "\"");
                       return false;
                   }
                   ref.uuid = m.uuid;
               }
               const auto index = static_cast<std::size_t>(slot);
               if (r->materials.size() <= index) r->materials.resize(index + 1);
               r->materials[index] = ref;
               return true;
           },
           {"0, \"Materials/Brillo.crmat\"", "material .crmat de un hueco de su MeshRenderer", "bool"});
    property(rt, "castShadows",
             [&rt](api::Call& c) -> api::Value {
                 const ecs::MeshRenderer* r = rt.selfEntity(c).tryGet<ecs::MeshRenderer>();
                 return r != nullptr && r->cast_shadows != ecs::ShadowCasting::Off;
             },
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity x = rt.selfEntity(c);
                 const bool on = c.boolean(0);
                 if (ecs::MeshRenderer* r = x.tryGet<ecs::MeshRenderer>()) {
                     r->cast_shadows = on ? ecs::ShadowCasting::On : ecs::ShadowCasting::Off;
                 }
                 return {};
             },
             {"", "su MeshRenderer proyecta sombra", "bool"});
}

// --- Componentes por su nombre, tela, cuerpo blando y campos ---
void registerComponents(Runtime& rt) {
    method(rt, "hasComponent",
           [&rt](api::Call& c) -> api::Value { return componentOf(rt.selfEntity(c), c.string(0)) != nullptr; },
           {"\"Rigidbody\"", "tiene ese componente?", "bool"});
    // "MeshCollider", "Rigidbody", "Light"...
    method(rt, "addComponent",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity x = rt.selfEntity(c);
               const std::string name = c.string(0);
               const ecs::ComponentType* type = ecs::ComponentRegistry::instance().find(name);
               if (type == nullptr || x.world() == nullptr) return false;
               if (!type->has(*x.world(), x.handle())) {
                   type->add(*x.world(), x.handle());
                   // Como Unity: la caja/esfera/capsula nueva toma la medida de la malla.
                   if (rt.physics != nullptr && physics::isFittableCollider(name)) rt.physics->fitColliderToMesh(x, name);
               }
               return true;
           },
           {"\"MeshCollider\"", "anade un componente por su nombre (caja/esfera/capsula: con la medida de la malla)", "bool"});
    // Ajusta sus colliders (caja, esfera, capsula) al AABB de su malla.
    method(rt, "fitColliderToMesh",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity x = rt.selfEntity(c);
               return rt.physics != nullptr && rt.physics->fitColliderToMesh(x);
           },
           {"", "ajusta su BoxCollider/SphereCollider/CapsuleCollider al AABB de su malla", "bool"});
    method(rt, "removeComponent",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity x = rt.selfEntity(c);
               const ecs::ComponentType* type = componentOf(x, c.string(0));
               if (type == nullptr) return false;
               type->remove(*x.world(), x.handle());
               return true;
           },
           {"\"MeshCollider\"", "quita un componente", "bool"});

    // Tela (Cloth) y cuerpo blando (SoftBody): vuelven a su sitio o reciben un empujon (m/s).
    method(rt, "resetCloth",
           [&rt](api::Call& c) -> api::Value {
               physics::ClothRuntime* sim = clothRuntime<physics::Cloth>(rt.selfEntity(c));
               if (sim != nullptr) sim->reset_requested = true;
               return sim != nullptr;
           },
           {"", "su tela (Cloth) vuelve a la pose de reposo", "bool"});
    method(rt, "addClothImpulse",
           [&rt](api::Call& c) -> api::Value {
               physics::ClothRuntime* sim = clothRuntime<physics::Cloth>(rt.selfEntity(c));
               const Vec3 velocity = c.vec3(0);
               if (sim != nullptr) sim->pending_velocity = sim->pending_velocity + velocity;
               return sim != nullptr;
           },
           {"Vec3(0, 0, 3)", "empujon a su tela (m/s a cada particula libre)", "bool"});
    method(rt, "resetSoftBody",
           [&rt](api::Call& c) -> api::Value {
               physics::SoftBodyRuntime* sim = clothRuntime<physics::SoftBody>(rt.selfEntity(c));
               if (sim != nullptr) sim->reset_requested = true;
               return sim != nullptr;
           },
           {"", "su cuerpo blando vuelve a su forma, quieto", "bool"});
    method(rt, "addSoftBodyImpulse",
           [&rt](api::Call& c) -> api::Value {
               physics::SoftBodyRuntime* sim = clothRuntime<physics::SoftBody>(rt.selfEntity(c));
               const Vec3 velocity = c.vec3(0);
               if (sim != nullptr) sim->pending_velocity = sim->pending_velocity + velocity;
               return sim != nullptr;
           },
           {"Vec3(0, 5, 0)", "empujon a su cuerpo blando (m/s a cada particula)", "bool"});

    // Cualquier campo de cualquier componente (la reflexion del Inspector):
    //   getField("Light", "intensity")   setField("PhysBones", "chains[1].pull", 0.5)
    //   (listas desde 1; [n+1] anade)    getFields("Ragdoll") -> {clave = valor}
    method(rt, "getField",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity x = rt.selfEntity(c);
               const std::string component = c.string(0);
               const std::string key = c.string(1);
               const ecs::ComponentType* type = componentOf(x, component);
               if (type == nullptr) return {};
               PostFieldVisitor visitor(PostFieldVisitor::Mode::Get, key, {}, false);
               type->reflect(*x.world(), x.handle(), visitor);
               return visitor.found() ? fieldValue(visitor.value()) : api::Value{};
           },
           {"\"Light\", \"intensity\"", "campo de cualquier componente", "valor"});
    method(rt, "setField",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity x = rt.selfEntity(c);
               const std::string component = c.string(0);
               const std::string key = c.string(1);
               const ecs::ComponentType* type = ecs::ComponentRegistry::instance().find(component);
               if (x.world() == nullptr) return false;
               if (type == nullptr) {
                   rt.write(1, "setField: no hay componente '" + component + "'");
                   return false;
               }
               PostValue v;
               if (!fieldFromValue(rt, c.arg(2), v)) {
                   rt.write(1, "setField " + component + "." + key + ": valor no valido");
                   return false;
               }
               ecs::World& world = *x.world();
               const bool added = !type->has(world, x.handle());
               if (added) type->add(world, x.handle());
               PostFieldVisitor visitor(PostFieldVisitor::Mode::Set, key, v, false);
               type->reflect(world, x.handle(), visitor);
               if (!visitor.found()) {
                   if (added) type->remove(world, x.handle());
                   rt.write(1, "setField: " + component + " no tiene '" + key + "' (mira getFields)");
                   return false;
               }
               if (!visitor.error().empty()) {
                   rt.write(1, "setField " + component + "." + key + ": " + visitor.error());
                   return false;
               }
               return true;
           },
           {"\"Light\", \"intensity\", 2", "cambia un campo (listas: \"chains[1].pull\")", "bool"});
    method(rt, "getFields",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity x = rt.selfEntity(c);
               api::Value out = api::Value::object();
               const ecs::ComponentType* type = componentOf(x, c.string(0));
               if (type == nullptr) return out;
               PostFieldVisitor visitor(PostFieldVisitor::Mode::Collect, {}, {}, false);
               type->reflect(*x.world(), x.handle(), visitor);
               for (const auto& [k, v] : visitor.fields()) out.set(k, fieldValue(v));
               return out;
           },
           {"\"Ragdoll\"", "todos los campos de un componente", "objeto"});
}

}  // namespace

void registerEntityApi(Runtime& rt) {
    registerTransform(rt);
    registerPhysics(rt);
    registerSoundAndAnimation(rt);
    registerUi(rt);
    registerRendering(rt);
    registerComponents(rt);
}

}  // namespace cramion::scripting::native
