// Pruebas de native/EntityApi.cpp: transformacion, jerarquia, destroy,
// fisica, sonido, animacion, interfaz, malla/material y campos de componentes.

#include "ApiTest.h"

#include "CramionCore/asset/MaterialAsset.h"
#include "CramionCore/audio/Audio.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/RuntimeMesh.h"
#include "CramionCore/physics/Cloth.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/physics/SoftBody.h"
#include "CramionCore/ui/UI.h"

#include <cmath>
#include <filesystem>
#include <iterator>
#include <memory>

using namespace cramion;
using namespace cramion::apitest;
using core::Vec3;

namespace {

bool near(float a, float b, float eps = 1e-3f) { return std::abs(a - b) < eps; }
bool near(const Vec3& a, const Vec3& b, float eps = 1e-3f) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}

// La llamada falla con un api::Error cuyo texto contiene `part`.
template <typename Fn>
bool fails(Fn&& fn, const std::string& part) {
    try {
        fn();
    } catch (const scripting::api::Error& e) {
        return std::string(e.what()).find(part) != std::string::npos;
    }
    return false;
}

json entityJson(const ecs::Entity& e) { return json{{"$e", static_cast<std::uint64_t>(entt::to_integral(e.handle())) + 1}}; }

void testTransform() {
    std::printf("Transformacion y jerarquia\n");
    ApiFixture t;
    ecs::Entity e = t.world.create("Caja");
    const Value self = t.entity(e);
    check(t.get("Entity:name", self).asString() == "Caja", "Entity:name");
    t.set("name", Value("Caja2"), self);
    t.set("tag", Value("Enemigo"), self);
    check(e.name() == "Caja2" && e.tag() == "Enemigo" && t.get("tag", self).asString() == "Enemigo", "name y tag se escriben");
    t.set("active", Value(false), self);
    check(!e.activeSelf() && !t.get("active", self).truthy(), "active");
    t.set("active", Value(true), self);

    t.set("position", Value(Vec3{1, 2, 3}), self);
    t.call("translate", {Value(Vec3{1, 0, 0})}, self);
    check(near(e.worldPosition(), Vec3{2, 2, 3}) && near(t.get("position", self).asVec3(), Vec3{2, 2, 3}),
          "position y translate");
    t.set("rotation", Value(Vec3{0, 90, 0}), self);
    check(near(t.get("rotation", self).asVec3(), Vec3{0, 90, 0}, 0.01f), "rotation en grados");
    t.call("rotate", {Value(Vec3{0, -90, 0})}, self);
    check(near(t.get("forward", self).asVec3(), Vec3{0, 0, -1}) && near(t.get("right", self).asVec3(), Vec3{1, 0, 0}) &&
              near(t.get("up", self).asVec3(), Vec3{0, 1, 0}),
          "rotate y forward/right/up");
    t.set("quaternion", Value(core::Quat{}), self);
    check(t.get("quaternion", self).isQuat(), "quaternion");
    t.set("scale", Value(Vec3{2, 2, 2}), self);
    check(near(e.localScale(), Vec3{2, 2, 2}) && near(t.get("scale", self).asVec3(), Vec3{2, 2, 2}), "scale");
    // Sin giro: el delante es -Z, asi que translateLocal(0, 0, 1) va hacia atras (+Z).
    t.call("translateLocal", {Value(Vec3{1, 0, 1})}, self);
    check(near(e.worldPosition(), Vec3{3, 2, 4}), "translateLocal en sus ejes");
    // lookAt hacia +X: gira -90 en Y y su delante pasa a ser +X.
    t.call("lookAt", {Value(e.worldPosition() + Vec3{10, 0, 0})}, self);
    check(near(t.get("forward", self).asVec3(), Vec3{1, 0, 0}, 0.01f), "lookAt");
    check(fails([&] { t.call("translate", {Value("no")}, self); }, "Vec3"), "translate con un texto es un error");

    ecs::Entity root = t.world.create("Raiz");
    ecs::Entity a = t.world.create("A", root);
    ecs::Entity c = t.world.create("C", root);
    ecs::Entity eye = t.world.create("Ojo", a);
    a.setLocalPosition(Vec3{0, 1, 0});
    root.setWorldPosition(Vec3{5, 0, 0});
    check(t.get("parent", t.entity(a)).asEntity() == root.handle(), "parent");
    check(t.get("parent", t.entity(root)).isEntity() && t.get("parent", t.entity(root)).asEntity() == entt::null,
          "parent sin padre: entidad no valida");
    check(near(t.get("localPosition", t.entity(a)).asVec3(), Vec3{0, 1, 0}) &&
              near(t.get("position", t.entity(a)).asVec3(), Vec3{5, 1, 0}),
          "localPosition y position con padre");
    t.set("localPosition", Value(Vec3{0, 2, 0}), t.entity(a));
    check(near(a.worldPosition(), Vec3{5, 2, 0}), "localPosition se escribe");
    check(t.call("find", {Value("Ojo")}, t.entity(root)).asEntity() == eye.handle(), "find busca en los nietos");
    check(t.call("find", {Value("C")}, t.entity(root)).asEntity() == c.handle(), "find un hijo");
    check(t.call("find", {Value("Nada")}, t.entity(root)).asEntity() == entt::null, "find sin resultado");
    check(near(static_cast<float>(t.call("distanceTo", {t.entity(c)}, t.entity(root)).asNumber()), 0.0f) &&
              near(static_cast<float>(t.call("distanceTo", {t.entity(a)}, t.entity(root)).asNumber()), 2.0f),
          "distanceTo");
    check(t.call("distanceTo", {Value()}, t.entity(root)).asNumber() == 0.0, "distanceTo(nil) = 0");

    check(t.call("valid", {}, self).truthy(), "valid");
    check(t.call("equals", {self}, self).truthy() && !t.call("equals", {t.entity(root)}, self).truthy() &&
              !t.call("equals", {Value()}, self).truthy(),
          "equals (el == de antes)");
    check(t.call("toString", {}, t.entity(root)).asString() == "Entity(Raiz)", "toString");

    // Por el puente (el mismo JSON que los scripts de C++).
    json r = t.bridge({{"op", "get"}, {"self", entityJson(root)}, {"key", "name"}});
    check(r["ok"] == true && r["result"] == "Raiz", "puente: get name");
    r = t.bridge({{"fn", "translate"}, {"self", entityJson(root)}, {"args", {{{"$v", {0, 0, 1}}}}}});
    check(r["ok"] == true && near(root.worldPosition(), Vec3{5, 0, 1}), "puente: translate");
    r = t.bridge({{"fn", "find"}, {"self", entityJson(root)}, {"args", {"Ojo"}}});
    check(r["ok"] == true && r["result"] == entityJson(eye), "puente: find devuelve la entidad");
    r = t.bridge({{"op", "set"}, {"self", entityJson(root)}, {"key", "forward"}, {"value", {{"$v", {1, 0, 0}}}}});
    check(r["ok"] == false, "puente: forward es de solo lectura");
}

void testDestroy() {
    std::printf("destroy\n");
    ApiFixture t;
    ecs::Entity parent = t.world.create("Padre");
    ecs::Entity child = t.world.create("Hijo", parent);
    ecs::Entity other = t.world.create("Otro");
    const Value self = t.entity(parent);
    t.call("destroy", {}, self);
    check(parent.valid() && child.valid(), "destroy espera al final del frame");
    t.call("destroy", {}, self);  // dos veces en el mismo frame
    t.frame();
    check(!parent.valid() && !child.valid() && other.valid(), "al final del frame se van el y sus hijos");
    check(!t.call("valid", {}, self).truthy(), "valid = false despues");
    check(t.call("toString", {}, self).asString() == "Entity(destruida)", "toString de una destruida");
    bool ok = true;
    try {
        t.call("destroy", {}, self);
    } catch (const scripting::api::Error&) {
        ok = false;
    }
    check(ok, "destruir una que ya no existe no hace nada");
    check(fails([&] { t.get("name", self); }, "ya no existe"), "name de una destruida es un error");
    check(fails([&] { t.call("distanceTo", {self}, t.entity(other)); }, "ya no existe"),
          "una destruida como argumento es un error");
    const json r = t.bridge({{"op", "get"}, {"self", entityJson(parent)}, {"key", "position"}});
    check(r["ok"] == false && r["error"].get<std::string>().find("ya no existe") != std::string::npos,
          "puente: error con una destruida");
}

void testPhysics() {
    std::printf("Fisica y vehiculos\n");
    {
        ApiFixture t;  // sin PhysicsSystem
        ecs::Entity e = t.world.create("Bola");
        const Value self = t.entity(e);
        t.set("velocity", Value(Vec3{1, 2, 3}), self);
        t.call("addForce", {Value(Vec3{0, 10, 0}), Value("impulse")}, self);
        t.call("setVehicleInput", {Value(1), Value(0)}, self);
        check(near(t.get("velocity", self).asVec3(), Vec3{}) && t.get("speed", self).asNumber() == 0.0 &&
                  t.get("gear", self).asNumber() == 0.0 && t.get("rpm", self).asNumber() == 0.0,
              "sin fisica: valores por defecto y sin errores");
        check(!t.call("fitColliderToMesh", {}, self).truthy(), "sin fisica: fitColliderToMesh = false");
    }
    physics::registerPhysicsComponents();
    ApiFixture t(false);
    physics::PhysicsSystem phys;
    ecs::Entity box = t.world.create("Caja");
    box.setWorldPosition(Vec3{0, 10, 0});
    box.add<physics::BoxCollider>();
    box.add<physics::Rigidbody>();
    phys.start(t.world);
    phys.update(t.world, 1.0f / 60.0f);  // crea los cuerpos
    t.scripts.setPhysics(&phys);
    t.scripts.start(t.world);
    const Value self = t.entity(box);
    t.set("velocity", Value(Vec3{0, 8, 0}), self);
    check(near(t.get("velocity", self).asVec3(), Vec3{0, 8, 0}, 0.01f), "velocity (Rigidbody)");
    t.call("addForce", {Value(Vec3{2, 0, 0}), Value("VelocityChange")}, self);
    check(near(t.get("velocity", self).asVec3().x, 2.0f, 0.01f), "addForce con modo (sin distinguir mayusculas)");
    t.set("angularVelocity", Value(Vec3{0, 1, 0}), self);
    check(near(t.get("angularVelocity", self).asVec3(), Vec3{0, 1, 0}, 0.01f), "angularVelocity");
    t.call("addImpulse", {Value(Vec3{0, 1, 0})}, self);
    t.call("addTorque", {Value(Vec3{0, 1, 0})}, self);
    check(t.get("speed", self).asNumber() == 0.0, "speed de algo que no es un Vehicle");
    t.scripts.stop();
}

void testComponents() {
    std::printf("Componentes y campos\n");
    ApiFixture t;
    ecs::Entity e = t.world.create("Lampara");
    const Value self = t.entity(e);
    check(!t.call("hasComponent", {Value("Light")}, self).truthy(), "hasComponent (no)");
    check(t.call("addComponent", {Value("Light")}, self).truthy() && e.has<ecs::Light>() &&
              t.call("hasComponent", {Value("Light")}, self).truthy(),
          "addComponent");
    check(t.call("addComponent", {Value("Light")}, self).truthy(), "addComponent otra vez: true");
    check(!t.call("addComponent", {Value("NoExiste")}, self).truthy(), "addComponent de algo que no existe");
    check(t.call("setField", {Value("Light"), Value("intensity"), Value(5)}, self).truthy() &&
              near(e.get<ecs::Light>().intensity, 5.0f),
          "setField numero");
    check(t.call("getField", {Value("Light"), Value("intensity")}, self).asNumber() == 5.0, "getField numero");
    check(t.call("setField", {Value("Light"), Value("color"), Value(Vec3{1, 0, 0})}, self).truthy() &&
              near(t.call("getField", {Value("Light"), Value("color")}, self).asVec3(), Vec3{1, 0, 0}),
          "setField / getField Vec3");
    check(t.call("setField", {Value("Light"), Value("type"), Value("Foco")}, self).truthy() &&
              t.call("getField", {Value("Light"), Value("type")}, self).asString() == "Foco",
          "enumeracion por su nombre");
    check(t.call("getField", {Value("Light"), Value("nada")}, self).isNil(), "getField de un campo que no hay: nil");
    check(t.call("getField", {Value("Rigidbody"), Value("mass")}, self).isNil(), "getField sin el componente: nil");
    const Value fields = t.call("getFields", {Value("Light")}, self);
    check(fields.isObject() && fields["intensity"].asNumber() == 5.0 && fields["type"].asString() == "Foco",
          "getFields: objeto clave -> valor");
    check(t.call("getFields", {Value("Rigidbody")}, self).isObject() && t.call("getFields", {Value("Rigidbody")}, self).size() == 0,
          "getFields sin el componente: vacio");
    t.log.clear();
    check(!t.call("setField", {Value("Light"), Value("nada"), Value(1)}, self).truthy() && t.logged("no tiene 'nada'"),
          "setField de un campo que no hay avisa");
    check(!t.call("setField", {Value("Light"), Value("intensity"), Value(Value::Array{})}, self).truthy() &&
              t.logged("valor no valido"),
          "setField con una lista avisa");
    check(!t.call("setField", {Value("NoExiste"), Value("x"), Value(1)}, self).truthy() && t.logged("no hay componente"),
          "setField de un componente que no existe avisa");
    check(!t.call("setField", {Value("Light"), Value("type"), Value("Raro")}, self).truthy() && t.logged("no es un valor"),
          "setField con una opcion que no existe avisa");
    // Sin el componente: lo anade (y lo quita si el campo no existe).
    ecs::Entity bare = t.world.create("Vacia");
    check(t.call("setField", {Value("Light"), Value("intensity"), Value(2)}, t.entity(bare)).truthy() &&
              bare.has<ecs::Light>(),
          "setField anade el componente");
    ecs::Entity bare2 = t.world.create("Vacia2");
    check(!t.call("setField", {Value("Light"), Value("nada"), Value(2)}, t.entity(bare2)).truthy() &&
              !bare2.has<ecs::Light>(),
          "setField de un campo que no hay no deja el componente");
    check(t.call("removeComponent", {Value("Light")}, self).truthy() && !e.has<ecs::Light>(), "removeComponent");
    check(!t.call("removeComponent", {Value("Light")}, self).truthy(), "removeComponent sin el componente: false");

    // Tela y cuerpo blando.
    check(!t.call("resetCloth", {}, self).truthy() && !t.call("addSoftBodyImpulse", {Value(Vec3{0, 1, 0})}, self).truthy(),
          "sin Cloth/SoftBody: false");
    ecs::Entity cloth = t.world.create("Tela");
    cloth.add<physics::Cloth>();
    check(t.call("resetCloth", {}, t.entity(cloth)).truthy() && cloth.get<physics::Cloth>().runtime.ptr &&
              cloth.get<physics::Cloth>().runtime.ptr->reset_requested,
          "resetCloth");
    t.call("addClothImpulse", {Value(Vec3{0, 0, 3})}, t.entity(cloth));
    t.call("addClothImpulse", {Value(Vec3{0, 0, 3})}, t.entity(cloth));
    check(near(cloth.get<physics::Cloth>().runtime.ptr->pending_velocity, Vec3{0, 0, 6}), "addClothImpulse se suma");
    ecs::Entity soft = t.world.create("Blando");
    soft.add<physics::SoftBody>();
    check(t.call("resetSoftBody", {}, t.entity(soft)).truthy() &&
              t.call("addSoftBodyImpulse", {Value(Vec3{0, 5, 0})}, t.entity(soft)).truthy() &&
              near(soft.get<physics::SoftBody>().runtime.ptr->pending_velocity, Vec3{0, 5, 0}),
          "resetSoftBody y addSoftBodyImpulse");
}

void testEntityFieldValue() {
    std::printf("setField con una entidad\n");
    ApiFixture t;
    // Un campo de texto cualquiera recibe el UUID de la entidad (o "" con nil).
    ecs::Entity e = t.world.create("Objeto");
    ecs::Entity target = t.world.create("Objetivo");
    const Value self = t.entity(e);
    t.call("addComponent", {Value("Animator")}, self);
    check(t.call("setField", {Value("Animator"), Value("clip_name"), t.entity(target)}, self).truthy() &&
              e.get<ecs::Animator>().clip_name == target.uuid().toString(),
          "una entidad se guarda como su UUID");
    check(t.call("setField", {Value("Animator"), Value("clip_name"), Value()}, self).truthy() &&
              e.get<ecs::Animator>().clip_name.empty(),
          "nil deja el texto vacio");
}

void testSoundAndAnimation() {
    std::printf("Sonido y animacion\n");
    ApiFixture t;  // sin AudioSystem: no suena pero no falla
    ecs::Entity e = t.world.create("Radio");
    const Value self = t.entity(e);
    t.call("playSound", {}, self);
    t.call("stopSound", {}, self);
    check(!t.call("isPlayingSound", {}, self).truthy() && t.get("soundOcclusion", self).asNumber() == -1.0,
          "sin audio: no suena y soundOcclusion = -1");
    check(!t.call("setSoundEffect", {Value("lowpass"), Value(true)}, self).truthy(), "setSoundEffect sin AudioSource: false");
    audio::AudioSource& source = e.add<audio::AudioSource>();
    check(t.call("setSoundEffect", {Value("LowPass"), Value(true), Value(800)}, self).truthy() && source.low_pass &&
              near(source.low_pass_cutoff, 800.0f),
          "setSoundEffect lowpass con valor");
    t.call("setSoundEffect", {Value("highpass"), Value(true)}, self);
    t.call("setSoundEffect", {Value("echo"), Value(true), Value(0.5)}, self);
    check(source.high_pass && near(source.high_pass_cutoff, 300.0f) && source.echo && near(source.echo_delay, 0.5f),
          "highpass (sin valor no cambia el corte) y echo");
    t.call("setSoundEffect", {Value("reverb"), Value(true)}, self);
    check(near(source.reverb_send, 1.0f), "reverb sin valor = 1");
    t.call("setSoundEffect", {Value("reverb"), Value(false), Value(0.4)}, self);
    check(near(source.reverb_send, 0.0f), "reverb apagada = 0");
    t.call("setSoundEffect", {Value("occlusion"), Value(false)}, self);
    check(!source.occlusion && !t.call("setSoundEffect", {Value("raro"), Value(true)}, self).truthy(),
          "occlusion y efecto desconocido");
    check(!t.get("audioOcclusion", self).truthy(), "audioOcclusion sin AudioListener: false");
    audio::AudioListener& listener = e.add<audio::AudioListener>();
    t.set("audioOcclusion", Value(false), self);
    check(!listener.occlusion && !t.get("audioOcclusion", self).truthy(), "audioOcclusion");
    t.set("audioOcclusion", Value(true), self);
    check(listener.occlusion, "audioOcclusion = true");

    check(t.call("animatorState", {}, self).asString().empty() && t.call("animatorStateTime", {}, self).asNumber() == 0.0,
          "sin Animator: estado vacio");
    t.call("playAnimation", {Value("Correr")}, self);  // sin Animator: nada
    ecs::Animator& anim = e.add<ecs::Animator>();
    anim.playing = false;
    anim.time = 3.0f;
    t.call("playAnimation", {Value("Correr")}, self);
    check(anim.clip_name == "Correr" && anim.loop && anim.playing && anim.time == 0.0f, "playAnimation (bucle por defecto)");
    t.call("playAnimation", {Value("Saltar"), Value(false)}, self);
    check(anim.clip_name == "Saltar" && !anim.loop, "playAnimation sin bucle");
    t.call("setAnimatorFloat", {Value("velocidad"), Value(1.5)}, self);
    t.call("setAnimatorBool", {Value("saltando"), Value(true)}, self);
    t.call("setAnimatorTrigger", {Value("atacar")}, self);
    check(near(anim.runtime.values["velocidad"], 1.5f) && anim.runtime.values["saltando"] == 1.0f &&
              anim.runtime.values["atacar"] == 1.0f,
          "parametros del Animator");
    anim.runtime.state_name = "Idle";
    anim.runtime.state_time = 2.0f;
    check(t.call("animatorState", {}, self).asString() == "Idle" && t.call("animatorStateTime", {}, self).asNumber() == 2.0,
          "animatorState y animatorStateTime");
    anim.playing = false;
    t.call("crossFade", {Value("Golpe")}, self);
    check(anim.runtime.cross_fade == "Golpe" && near(anim.runtime.cross_fade_time, 0.15f) && anim.runtime.cross_fade_inertial &&
              anim.playing,
          "crossFade (por defecto 0.15 s, inercial)");
    t.call("crossFade", {Value("Golpe2"), Value(0.3), Value(false)}, self);
    check(near(anim.runtime.cross_fade_time, 0.3f) && !anim.runtime.cross_fade_inertial, "crossFade con tiempo e inercia");
    check(t.call("animatorState", {}, self).asString() == "Golpe2" && t.call("animatorStateTime", {}, self).asNumber() == 0.0,
          "el estado pedido este frame cuenta ya");
}

void testUi() {
    std::printf("Interfaz\n");
    ApiFixture t;
    ecs::Entity label = t.world.create("Titulo");
    ui::Text& text = label.add<ui::Text>();
    const Value l = t.entity(label);
    t.set("text", Value("Hola"), l);
    check(text.text == "Hola" && t.get("text", l).asString() == "Hola", "text de un UIText");
    t.set("color", Value(Vec3{1, 0, 0}), l);
    t.set("alpha", Value(0.5), l);
    check(near(text.color, Vec3{1, 0, 0}) && near(text.alpha, 0.5f) && near(static_cast<float>(t.get("alpha", l).asNumber()), 0.5f),
          "color y alpha de un UIText");

    ecs::Entity field = t.world.create("Campo");
    ui::InputField& input = field.add<ui::InputField>();
    t.set("text", Value("abc"), t.entity(field));
    check(input.text == "abc" && t.get("text", t.entity(field)).asString() == "abc", "text de un campo de texto");

    ecs::Entity slider = t.world.create("Volumen");
    ui::Slider& s = slider.add<ui::Slider>();
    s.min = 0.0f;
    s.max = 10.0f;
    t.set("value", Value(4), t.entity(slider));
    check(near(s.value, 4.0f) && t.get("value", t.entity(slider)).asNumber() == 4.0, "value de un Slider");
    t.set("value", Value(50), t.entity(slider));
    check(near(s.value, 10.0f), "value del Slider se limita a su rango");
    t.set("interactable", Value(false), t.entity(slider));
    check(!s.interactable && !t.get("interactable", t.entity(slider)).truthy(), "interactable");

    ecs::Entity toggle = t.world.create("Casilla");
    ui::Toggle& tg = toggle.add<ui::Toggle>();
    t.set("value", Value(1), t.entity(toggle));
    check(tg.on && t.get("value", t.entity(toggle)).asNumber() == 1.0, "value de una Casilla");

    ecs::Entity image = t.world.create("Imagen");
    ui::Image& img = image.add<ui::Image>();
    ui::RectTransform& rect = image.add<ui::RectTransform>();
    const Value i = t.entity(image);
    t.set("texture", Value("UI/boton.png"), i);
    t.set("color", Value(Vec3{0, 1, 0}), i);
    t.set("uiPosition", Value(Vec3{10, 20, 0}), i);
    t.set("uiSize", Value(Vec3{300, 50, 0}), i);
    check(img.texture == "UI/boton.png" && t.get("texture", i).asString() == "UI/boton.png", "texture de un UIImage");
    check(near(img.color, Vec3{0, 1, 0}) && near(t.get("color", i).asVec3(), Vec3{0, 1, 0}), "color de un UIImage");
    check(near(rect.position.x, 10.0f) && near(rect.size.y, 50.0f) && near(t.get("uiPosition", i).asVec3(), Vec3{10, 20, 0}) &&
              near(t.get("uiSize", i).asVec3(), Vec3{300, 50, 0}),
          "uiPosition y uiSize");

    ecs::Entity plain = t.world.create("Nada");
    const Value p = t.entity(plain);
    check(t.get("text", p).asString().empty() && t.get("value", p).asNumber() == 0.0 && !t.get("interactable", p).truthy() &&
              near(t.get("color", p).asVec3(), Vec3{1, 1, 1}) && t.get("alpha", p).asNumber() == 1.0 &&
              near(t.get("uiSize", p).asVec3(), Vec3{}),
          "sin componentes de interfaz: valores por defecto");
}

void testRendering() {
    std::printf("Malla, material y sombras\n");
    ApiFixture t;
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_entity_api_assets";
    std::filesystem::create_directories(root / "Materials");
    assets::MaterialAsset red;
    red.uuid = Uuid::generate();
    check(assets::saveMaterial(red, root / "Materials" / "Rojo.crmat"), "(material de prueba guardado)");
    t.scripts.setAssetsRoot(root);

    ecs::Entity a = t.world.create("A");
    ecs::Entity b = t.world.create("B");
    const Value va = t.entity(a);
    const Value vb = t.entity(b);
    check(t.get("mesh", va).isNil(), "mesh sin MeshRenderer: nil");
    const auto mesh = std::make_shared<ecs::Mesh>();
    a.add<ecs::MeshRenderer>().mesh = mesh;
    const Value handle = t.get("mesh", va);
    check(handle.isHandle() && handle.asHandle()->typeName() == "Mesh", "mesh: un handle Mesh");
    check(t.get("mesh", va).asHandle() == handle.asHandle(), "el mismo handle para la misma malla");
    t.set("mesh", Value(), vb);
    check(!b.has<ecs::MeshRenderer>(), "mesh = nil sin MeshRenderer no lo anade");
    t.set("mesh", handle, vb);
    check(b.has<ecs::MeshRenderer>() && b.get<ecs::MeshRenderer>().mesh == mesh, "asignar mesh anade el MeshRenderer");
    t.set("mesh", Value(), vb);
    check(b.has<ecs::MeshRenderer>() && !b.get<ecs::MeshRenderer>().mesh, "mesh = nil la quita");
    check(fails([&] { t.set("mesh", Value(3), vb); }, "Mesh"), "mesh con otra cosa es un error");
    // Una malla del modulo Mesh (Mesh.new) vale aqui: es el mismo handle.
    const Value created = t.call("Mesh.new", {Value("Rampa")});
    t.set("mesh", created, vb);
    check(b.get<ecs::MeshRenderer>().mesh && b.get<ecs::MeshRenderer>().mesh->name == "Rampa" &&
              t.get("mesh", vb).asHandle() == created.asHandle(),
          "mesh = Mesh.new(...)");
    const json r = t.bridge({{"op", "get"}, {"self", entityJson(a)}, {"key", "mesh"}});
    check(r["ok"] == true && r["result"].contains("$h"), "puente: mesh viaja como handle");

    check(t.call("setMaterial", {Value(1), Value("Materials/Rojo")}, va).truthy(), "setMaterial (sin la extension)");
    const ecs::MeshRenderer& renderer = a.get<ecs::MeshRenderer>();
    check(renderer.materials.size() == 2 && renderer.materials[1].uuid == red.uuid &&
              renderer.materials[1].type == assets::AssetType::Material,
          "setMaterial pone el material en su hueco");
    check(t.call("setMaterial", {Value(1)}, va).truthy() && !renderer.materials[1].valid(), "setMaterial(hueco, nil) lo quita");
    t.log.clear();
    check(!t.call("setMaterial", {Value(0), Value("Materials/NoExiste.crmat")}, va).truthy() &&
              t.logged("no se puede leer el material"),
          "setMaterial con un material que no existe avisa");
    check(!t.call("setMaterial", {Value(-1), Value("Materials/Rojo")}, va).truthy(), "setMaterial con hueco negativo: false");
    check(!t.call("setMaterial", {Value(0), Value("Materials/Rojo")}, t.entity(t.world.create("C"))).truthy(),
          "setMaterial sin MeshRenderer: false");

    check(t.get("castShadows", va).truthy(), "castShadows (por defecto)");
    t.set("castShadows", Value(false), va);
    check(renderer.cast_shadows == ecs::ShadowCasting::Off && !t.get("castShadows", va).truthy(), "castShadows = false");
    t.scripts.stop();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

void testRegistry() {
    std::printf("Registro\n");
    ApiFixture t(false);
    static const char* const kNames[] = {
        "valid", "equals", "toString", "name", "tag", "active", "position", "localPosition", "rotation", "quaternion",
        "scale", "forward", "right", "up", "parent", "translate", "translateLocal", "rotate", "lookAt", "distanceTo",
        "destroy", "find", "velocity", "angularVelocity", "addForce", "addImpulse", "addTorque", "setVehicleInput",
        "speed", "rpm", "gear", "playSound", "stopSound", "isPlayingSound", "setSoundEffect", "soundOcclusion",
        "audioOcclusion", "playAnimation", "setAnimatorFloat", "setAnimatorBool", "setAnimatorTrigger", "crossFade",
        "animatorState", "animatorStateTime", "text", "value", "interactable", "color", "texture", "alpha",
        "uiPosition", "uiSize", "mesh", "setMaterial", "castShadows", "hasComponent", "addComponent",
        "fitColliderToMesh", "removeComponent", "resetCloth", "addClothImpulse", "resetSoftBody", "addSoftBodyImpulse",
        "getField", "setField", "getFields"};
    int found = 0;
    bool documented = true;
    for (const char* name : kNames) {
        const scripting::api::Entry* e = t.api().find(std::string("Entity:") + name);
        if (e == nullptr) {
            std::printf("  (falta Entity:%s)\n", name);
            continue;
        }
        ++found;
        if (e->doc.description.empty() || !e->member()) documented = false;
    }
    std::printf("  (%d de %d miembros de Entity)\n", found, static_cast<int>(std::size(kNames)));
    check(found == static_cast<int>(std::size(kNames)), "los miembros de Entity estan registrados");
    check(documented, "todos con documentacion y como miembros del tipo");
    const auto* parent = t.api().find("Entity:parent");
    const auto* translate = t.api().find("Entity:translate");
    check(parent != nullptr && parent->kind == scripting::api::Entry::Kind::Property && !parent->assign,
          "Entity:parent es una propiedad de solo lectura");
    check(translate != nullptr && translate->kind == scripting::api::Entry::Kind::Method, "Entity:translate es un metodo");
    check(t.api().find("Entity:getScript") == nullptr, "getScript no se porta (era solo de Lua)");
}

}  // namespace

int main() {
    testTransform();
    testDestroy();
    testPhysics();
    testComponents();
    testEntityFieldValue();
    testSoundAndAnimation();
    testUi();
    testRendering();
    testRegistry();
    return finish();
}
