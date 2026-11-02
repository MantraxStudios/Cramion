// Pruebas de la animacion por claves (consola, sin GPU): curvas (constante,
// lineal, Bezier, asas automaticas, extrapolacion), acceso a propiedades por
// ruta, el sistema en Play y guardar/leer las curvas. Devuelve 0 si todo va.

#include "CramionCore/anim/Keyframes.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"

#include <chrono>
#include <cmath>
#include <cstdio>

using namespace cramion;
using namespace cramion::anim;
using core::Vec3;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

bool near(float a, float b, float eps = 1e-3f) { return std::abs(a - b) < eps; }

void testCurves() {
    std::printf("Curvas\n");
    AnimationCurve c;
    c.insert(0.0f, 0.0f, Interpolation::Linear);
    c.insert(2.0f, 10.0f);
    check(c.keys.size() == 2 && c.keys[1].interpolation == Interpolation::Linear, "la clave nueva hereda la interpolacion");
    check(near(c.evaluate(1.0f), 5.0f), "lineal a mitad");
    check(near(c.evaluate(-1.0f), 0.0f) && near(c.evaluate(5.0f), 10.0f), "extrapolacion constante");
    c.extrapolation = Extrapolation::Linear;
    c.recalcHandles();
    check(near(c.evaluate(3.0f), 15.0f), "extrapolacion lineal");
    c.extrapolation = Extrapolation::Cycle;
    check(near(c.evaluate(3.0f), 5.0f), "extrapolacion en ciclo");

    c.keys[0].interpolation = Interpolation::Constant;
    check(near(c.evaluate(1.9f), 0.0f), "constante hasta la siguiente clave");

    AnimationCurve b;
    b.insert(0.0f, 0.0f);
    b.insert(1.0f, 1.0f);
    check(near(b.evaluate(0.5f), 0.5f, 1e-2f), "Bezier simetrica pasa por el centro");
    check(b.evaluate(0.25f) < 0.25f && b.evaluate(0.75f) > 0.75f, "Bezier con asas planas: entra y sale suave");
    check(near(b.evaluate(0.0f), 0.0f) && near(b.evaluate(1.0f), 1.0f), "pasa por las claves");

    // Automatica sin pasarse: 0 -> 1 -> 1 -> 0 no debe superar 1.
    AnimationCurve o;
    o.insert(0.0f, 0.0f);
    o.insert(1.0f, 1.0f);
    o.insert(2.0f, 1.0f);
    o.insert(3.0f, 0.0f);
    float peak = 0.0f;
    for (int i = 0; i <= 300; ++i) peak = std::max(peak, o.evaluate(static_cast<float>(i) / 100.0f));
    check(peak <= 1.0f + 1e-3f, "asas automaticas limitadas: sin pasarse");

    // Reemplazar en el mismo tiempo.
    const int index = o.insert(1.0f, 2.0f);
    check(o.keys.size() == 4 && index == 1 && near(o.keys[1].value, 2.0f), "insertar en el mismo tiempo sustituye");

    // Vector: tramo recto.
    AnimationCurve v;
    v.insert(0.0f, 0.0f);
    v.insert(1.0f, 4.0f);
    for (Keyframe& k : v.keys) k.left_type = k.right_type = HandleType::Vector;
    v.recalcHandles();
    check(near(v.evaluate(0.5f), 2.0f, 1e-2f), "asas vector: recta");

    // Texto.
    o.keys[2].interpolation = Interpolation::Linear;
    o.keys[2].right_type = HandleType::Free;
    const std::vector<Keyframe> decoded = decodeKeys(encodeKeys(o.keys));
    bool same = decoded.size() == o.keys.size();
    for (std::size_t i = 0; same && i < decoded.size(); ++i) {
        same = decoded[i].time == o.keys[i].time && decoded[i].value == o.keys[i].value &&
               decoded[i].interpolation == o.keys[i].interpolation && decoded[i].right_type == o.keys[i].right_type &&
               decoded[i].left.x == o.keys[i].left.x && decoded[i].right.y == o.keys[i].right.y;
    }
    check(same, "claves a texto y de vuelta, exactas");
}

void testProperties() {
    std::printf("Propiedades\n");
    ecs::World world;
    ecs::Entity e = world.create("Cubo");
    e.add<ecs::Light>();
    const std::vector<AnimatableProperty> props = animatableProperties(world, e, "Transform");
    check(props.size() == 3 && props[0].path == "position" && props[0].channels == 3, "Transform: posicion, rotacion, escala");
    check(!animatableProperties(world, e, "Light").empty(), "la luz tiene propiedades animables");

    check(writeProperties(world, e, "Transform", {{"position", 1, 3.5f}, {"rotation", 1, 90.0f}}), "escribir dos canales");
    check(near(e.localPosition().y, 3.5f) && near(e.localEulerDegrees().y, 90.0f), "la posicion y el giro cambian");
    float value = 0.0f;
    check(readProperty(world, e, "Transform", "position", 1, value) && near(value, 3.5f), "leer un canal");
    check(!readProperty(world, e, "Transform", "position", 3, value), "canal fuera de rango");
    check(!readProperty(world, e, "Camera", "fov", 0, value), "componente que no tiene");
    check(propertyChannels(world, e, "Transform", "scale") == 3, "canales de un Vec3");
}

void testSystem() {
    std::printf("Sistema\n");
    ecs::World world;
    ecs::Entity e = world.create("Animado");
    ecs::Entity other = world.create("Otro");
    KeyframeAnimation& a = e.add<KeyframeAnimation>();
    a.start = 0.0f;
    a.end = 2.0f;
    a.loop = LoopMode::Once;
    AnimationCurve& x = a.curve(Uuid{}, "Transform", "position", 0);
    x.insert(0.0f, 0.0f, Interpolation::Linear);
    x.insert(2.0f, 4.0f, Interpolation::Linear);
    AnimationCurve& y = a.curve(other.uuid(), "Transform", "position", 1);
    y.insert(0.0f, 1.0f, Interpolation::Constant);
    a.curve(Uuid{}, "Transform", "position", 2);  // vacia: no se aplica
    check(a.curves.size() == 3 && a.curves[1].channel == 2, "los canales de la misma propiedad, juntos y en orden");

    KeyframeSystem system;
    system.update(world, 0.5f);
    check(near(e.localPosition().x, 1.0f), "en Play avanza (0.5 s)");
    check(near(other.localPosition().y, 1.0f), "anima a otra entidad");
    system.update(world, 5.0f);
    check(near(e.localPosition().x, 4.0f) && !system.isPlaying(e), "una vez: se para al final");

    a.loop = LoopMode::Loop;
    system.play(e);
    system.update(world, 2.5f);
    check(near(e.localPosition().x, 1.0f), "en bucle: vuelve a empezar");
    a.loop = LoopMode::PingPong;
    system.play(e);
    system.update(world, 3.0f);
    check(near(e.localPosition().x, 2.0f), "ida y vuelta");

    KeyframeSystem::applyAllAt(world, 0.25f);
    check(near(e.localPosition().x, 0.5f), "vista previa en un tiempo");
}

void testSerialization() {
    std::printf("Guardar y leer\n");
    ecs::World world;
    ecs::Entity e = world.create("Animado");
    ecs::Entity other = world.create("Otro");
    KeyframeAnimation& a = e.add<KeyframeAnimation>();
    a.end = 3.0f;
    a.loop = LoopMode::PingPong;
    AnimationCurve& c = a.curve(other.uuid(), "Transform", "scale", 2);
    c.insert(0.0f, 1.0f);
    c.insert(1.5f, 2.0f);
    c.extrapolation = Extrapolation::Cycle;

    ecs::World copy;
    ecs::deserializeWorld(copy, ecs::serializeWorld(world));
    const ecs::Entity loaded = copy.findByName("Animado");
    const KeyframeAnimation* b = loaded.valid() ? loaded.tryGet<KeyframeAnimation>() : nullptr;
    check(b != nullptr && b->end == 3.0f && b->loop == LoopMode::PingPong, "ajustes de reproduccion");
    check(b != nullptr && b->curves.size() == 1 && b->curves[0].target == other.uuid() &&
              b->curves[0].component == "Transform" && b->curves[0].property == "scale" && b->curves[0].channel == 2 &&
              b->curves[0].extrapolation == Extrapolation::Cycle,
          "la curva y su canal");
    check(b != nullptr && b->curves[0].keys.size() == 2 && b->curves[0].keys[1].value == 2.0f &&
              near(b->curves[0].evaluate(0.75f), c.evaluate(0.75f)),
          "las claves y la forma de la curva");
}

// Cache de enlaces del sistema (curva -> campo del componente): mismos valores
// que la via lenta y se rehace cuando cambian las curvas, el componente o la
// entidad objetivo.
void testBindingCache() {
    std::printf("Cache de enlaces\n");
    ecs::World world;
    ecs::Entity e = world.create("Animado");
    ecs::Entity lamp = world.create("Lampara");
    ecs::Light& light = lamp.add<ecs::Light>();
    light.type = ecs::LightType::Point;
    KeyframeAnimation& a = e.add<KeyframeAnimation>();
    a.start = 0.0f;
    a.end = 10.0f;
    a.loop = LoopMode::Once;
    const auto linear = [](AnimationCurve& c, float v0, float v1) {
        c.insert(0.0f, v0, Interpolation::Linear);
        c.insert(10.0f, v1, Interpolation::Linear);
    };
    linear(a.curve(Uuid{}, "Transform", "position", 1), 0.0f, 10.0f);
    linear(a.curve(lamp.uuid(), "Light", "intensity", 0), -100.0f, 100.0f);  // limitada a [0, 2000]
    linear(a.curve(lamp.uuid(), "Light", "cast_shadows", 0), 0.0f, 0.0f);
    linear(a.curve(lamp.uuid(), "Light", "type", 0), 5.0f, 5.0f);            // enum: como mucho 2
    linear(a.curve(lamp.uuid(), "Light", "no_existe", 0), 1.0f, 1.0f);       // via lenta, sin efecto
    linear(a.curve(lamp.uuid(), "Light", "intensity", 2), 7.0f, 7.0f);       // canal que no tiene

    // Giro puesto por la fisica o un script sin tocar los grados: animar la
    // posicion no debe rehacerlo desde `euler`.
    ecs::Transform& t = e.transform();
    t.rotation = core::Quat{0.0f, 0.38268343f, 0.0f, 0.92387953f};
    const core::Quat rotation = t.rotation;

    KeyframeSystem system;
    system.update(world, 2.0f);
    check(system.bindCount() == 1, "se resuelve una vez");
    check(near(e.localPosition().y, 2.0f), "posicion por la cache");
    check(near(lamp.get<ecs::Light>().intensity, 0.0f), "float limitado a su rango");
    check(!lamp.get<ecs::Light>().cast_shadows, "bool");
    check(lamp.get<ecs::Light>().type == ecs::LightType::Spot, "enum limitado a sus nombres");
    const ecs::Transform& after = e.transform();
    check(near(after.rotation.y, rotation.y) && near(after.rotation.w, rotation.w),
          "animar la posicion no pisa el giro");
    check(near(e.worldPosition().y, 2.0f), "la matriz de mundo se rehace");

    system.update(world, 6.0f);
    check(system.bindCount() == 1, "sin cambios no se vuelve a resolver");
    check(near(e.localPosition().y, 8.0f) && near(lamp.get<ecs::Light>().intensity, 60.0f), "siguiente frame");

    // Mismos valores que la vista previa (via lenta) en el mismo tiempo.
    ecs::World slow_world;
    ecs::Entity se = slow_world.create("Animado");
    ecs::Entity sl = slow_world.create("Lampara", {}, lamp.uuid());
    sl.add<ecs::Light>().type = ecs::LightType::Point;
    KeyframeSystem::applyAt(slow_world, se, a, 8.0f);
    check(near(se.localPosition().y, e.localPosition().y) &&
              near(sl.get<ecs::Light>().intensity, lamp.get<ecs::Light>().intensity) &&
              sl.get<ecs::Light>().type == lamp.get<ecs::Light>().type,
          "igual que la via lenta");

    // Curvas nuevas: se rehace.
    linear(a.curve(Uuid{}, "Transform", "rotation", 1), 0.0f, 90.0f);
    system.play(e, 5.0f);
    system.update(world, 0.0f);
    check(system.bindCount() == 2, "curva nueva: se resuelve otra vez");
    check(near(e.localEulerDegrees().y, 45.0f), "el giro animado se aplica (por los grados)");
    const core::Quat expected = ecs::quatFromEulerDegrees(core::Vec3{0.0f, 45.0f, 0.0f});
    check(near(std::abs(e.transform().rotation.y), std::abs(expected.y)), "y rehace el cuaternion");

    // Quitar el componente y volver a ponerlo.
    lamp.remove<ecs::Light>();
    system.play(e, 3.0f);
    system.update(world, 0.0f);
    check(!lamp.has<ecs::Light>(), "sin el componente no se escribe ni falla");
    lamp.add<ecs::Light>().type = ecs::LightType::Point;
    system.play(e, 7.0f);
    system.update(world, 0.0f);
    check(near(lamp.get<ecs::Light>().intensity, 40.0f), "componente nuevo: valor correcto");

    // El objetivo se destruye y vuelve con el mismo UUID (deshacer).
    const Uuid lamp_uuid = lamp.uuid();
    world.destroy(lamp);
    system.play(e, 7.5f);
    system.update(world, 0.0f);
    check(true, "objetivo destruido: no falla");
    ecs::Entity again = world.create("Lampara", {}, lamp_uuid);
    again.add<ecs::Light>().type = ecs::LightType::Point;
    system.play(e, 9.0f);
    system.update(world, 0.0f);
    check(near(again.get<ecs::Light>().intensity, 80.0f), "objetivo recreado: se anima el nuevo");

    // Entidades animadas destruidas: su estado se olvida.
    ecs::Entity temp = world.create("Temporal");
    linear(temp.add<KeyframeAnimation>().curve(Uuid{}, "Transform", "position", 0), 0.0f, 1.0f);
    system.update(world, 0.1f);
    const std::size_t with_temp = system.stateCount();
    world.destroy(temp);
    system.update(world, 0.1f);
    check(system.stateCount() + 1 == with_temp, "el estado de una entidad destruida se poda");
}

// Tiempo por frame con muchas animaciones: la cache contra la via lenta
// (applyAllAt, lo que hacia update antes). Solo informa; comprueba que la
// cache no sea mas lenta.
void benchBindingCache() {
    std::printf("Rendimiento (informativo)\n");
    ecs::World world;
    constexpr int kAnimated = 500;
    for (int i = 0; i < kAnimated; ++i) {
        ecs::Entity e = world.create("Animado");
        e.add<ecs::Light>().type = ecs::LightType::Point;
        KeyframeAnimation& a = e.add<KeyframeAnimation>();
        a.end = 4.0f;
        for (int axis = 0; axis < 3; ++axis) {
            AnimationCurve& c = a.curve(Uuid{}, "Transform", "position", axis);
            c.insert(0.0f, 0.0f);
            c.insert(4.0f, static_cast<float>(i + axis));
        }
        AnimationCurve& l = a.curve(Uuid{}, "Light", "intensity", 0);
        l.insert(0.0f, 1.0f);
        l.insert(4.0f, 10.0f);
    }
    constexpr int kFrames = 200;
    KeyframeSystem system;
    system.update(world, 0.0f);
    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < kFrames; ++f) system.update(world, 1.0f / 60.0f);
    const auto t1 = std::chrono::steady_clock::now();
    for (int f = 0; f < kFrames; ++f) KeyframeSystem::applyAllAt(world, 0.01f * static_cast<float>(f));
    const auto t2 = std::chrono::steady_clock::now();
    const double cached = std::chrono::duration<double, std::micro>(t1 - t0).count() / kFrames;
    const double slow = std::chrono::duration<double, std::micro>(t2 - t1).count() / kFrames;
    std::printf("  %d animaciones (4 canales, 2 componentes): cache %.1f us/frame, via lenta %.1f us/frame (x%.1f)\n",
                kAnimated, cached, slow, cached > 0.0 ? slow / cached : 0.0);
    check(cached < slow, "la cache es mas rapida que la via lenta");
}

}  // namespace

int main() {
    testCurves();
    testProperties();
    testSystem();
    testSerialization();
    testBindingCache();
    benchBindingCache();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
