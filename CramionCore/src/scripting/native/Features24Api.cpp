// Lo nuevo de la 2.4: splines (Entity y Spline.create), listas desplegables,
// scroll, Text.strip, WorldPartition, Accessibility, Camera, Voice, Crowd,
// Mods y Jobs. (Lo de red de la 2.4, Network.stats / positionAt..., va con
// Network.)
//
//   e.call("splinePoint", 0.5)                 mitad de la curva (mundo)
//   e.call("closestSplineDistance", pos)
//   Spline::create(Values{Vec3(0,0,0), Vec3(0,0,20), Vec3(10,0,30)}, "road")
//   seguidor.call("playSplineFollower")        seguidor.set("splineDistance", 0)

#include "Modules.h"

#include "CramionCore/ai/Crowd.h"
#include "CramionCore/audio/VoiceChat.h"
#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/gameplay/Accessibility.h"
#include "CramionCore/jobs/JobSystem.h"
#include "CramionCore/project/Mods.h"
#include "CramionCore/spline/Spline.h"
#include "CramionCore/ui/UI.h"
#include "CramionCore/world/WorldPartition.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace cramion::scripting::native {
namespace {

using core::Vec3;

// La curva de la entidad en el mundo (vacia si no tiene Spline).
spline::SplinePath splinePath(const ecs::Entity& e) {
    spline::SplinePath p;
    if (e.valid() && e.has<spline::Spline>()) p.build(e.get<spline::Spline>(), e.worldMatrix());
    return p;
}

int splineShapeFromName(const std::string& raw) {
    const std::string n = lowerText(raw);
    if (n == "road" || n == "carretera") return 0;
    if (n == "path" || n == "camino") return 1;
    if (n == "river" || n == "rio") return 2;
    if (n == "wall" || n == "muro") return 3;
    if (n == "fence" || n == "valla") return 4;
    if (n == "pipe" || n == "tuberia") return 5;
    if (n == "rails" || n == "railes") return 6;
    if (n == "ribbon" || n == "cinta") return 7;
    return -1;
}

spline::Spline* splineOf(const ecs::Entity& e) { return e.valid() ? e.tryGet<spline::Spline>() : nullptr; }
spline::SplineFollower* followerOf(const ecs::Entity& e) { return e.valid() ? e.tryGet<spline::SplineFollower>() : nullptr; }
ui::Dropdown* dropdownOf(const ecs::Entity& e) { return e.valid() ? e.tryGet<ui::Dropdown>() : nullptr; }
ui::ScrollView* scrollViewOf(const ecs::Entity& e) { return e.valid() ? e.tryGet<ui::ScrollView>() : nullptr; }

// Indice de un punto de control (desde 1) o -1 si no existe.
int controlIndex(const spline::Spline* s, long long index) {
    if (s == nullptr || index < 1 || index > static_cast<long long>(s->points.size())) return -1;
    return static_cast<int>(index - 1);
}

// El chat de voz del juego (lo crea el primer uso, como antes).
audio::VoiceChat& voice() {
    if (audio::activeVoiceChat() == nullptr) {
        static audio::VoiceChat shared;
        audio::setActiveVoiceChat(&shared);
    }
    return *audio::activeVoiceChat();
}

// Un objeto de opciones ({clave = valor}); una lista vacia tambien vale
// (una tabla vacia puede llegar como [] por el puente).
const api::Value& optionsArg(const api::Call& c, std::size_t i) {
    const api::Value& v = c.arg(i);
    if (v.isArray() && v.size() == 0) return api::Value::nil();
    return c.object(i);
}

// Una lista; un objeto vacio tambien vale como lista vacia.
const api::Value::Array& listArg(const api::Call& c, std::size_t i) {
    const api::Value& v = c.arg(i);
    if (v.isObject() && v.size() == 0) return api::Value::nil().items();
    return c.list(i).items();
}

// Los campos de las opciones que vengan con su tipo (los demas se quedan como estaban).
float numberField(const api::Value& o, std::string_view key, float fallback) {
    const api::Value& v = o[key];
    return v.isNumber() ? static_cast<float>(v.asNumber()) : fallback;
}

int intField(const api::Value& o, std::string_view key, int fallback) {
    const api::Value& v = o[key];
    return v.isNumber() ? static_cast<int>(std::lround(v.asNumber())) : fallback;
}

bool boolField(const api::Value& o, std::string_view key, bool fallback) {
    const api::Value& v = o[key];
    return v.isBool() ? v.truthy() : fallback;
}

void registerSplines(Runtime& rt) {
    api::NativeApi& n = rt.native;
    const auto curve = [&rt](const api::Call& c) { return splinePath(rt.selfEntity(c)); };

    n.method("Entity", "splinePoint",
             [curve](api::Call& c) { return api::Value(curve(c).atNormalized(static_cast<float>(c.number(0))).position); },
             {"0.5", "Spline: punto (mundo) a esa fraccion de la curva", "Vec3"});
    n.method("Entity", "splinePointAt",
             [curve](api::Call& c) { return api::Value(curve(c).atDistance(static_cast<float>(c.number(0))).position); },
             {"metros", "Spline: punto a esa distancia desde el principio", "Vec3"});
    n.method("Entity", "splineTangent",
             [curve](api::Call& c) { return api::Value(curve(c).atNormalized(static_cast<float>(c.number(0))).tangent); },
             {"0.5", "Spline: direccion de la curva ahi", "Vec3"});
    n.method("Entity", "splineRight",
             [curve](api::Call& c) { return api::Value(curve(c).atNormalized(static_cast<float>(c.number(0))).right); },
             {"0.5", "Spline: derecha de la curva ahi (con el peralte)", "Vec3"});
    n.property("Entity", "splineLength", [curve](api::Call& c) { return api::Value(curve(c).length()); }, {},
               {"", "metros de su Spline", "numero"}, true);
    n.method("Entity", "closestSplineDistance",
             [curve](api::Call& c) { return api::Value(curve(c).closestDistance(c.vec3(0))); },
             {"posicion", "Spline: distancia en la curva del punto mas cercano", "numero"});
    n.method("Entity", "closestSplinePoint",
             [curve](api::Call& c) {
                 const Vec3 p = c.vec3(0);
                 Vec3 closest{};
                 curve(c).closestDistance(p, &closest);
                 return api::Value(closest);
             },
             {"posicion", "Spline: el punto de la curva mas cercano", "Vec3"});
    n.property("Entity", "splinePointCount",
               [&rt](api::Call& c) {
                   const spline::Spline* s = splineOf(rt.selfEntity(c));
                   return api::Value(s != nullptr ? static_cast<int>(s->points.size()) : 0);
               },
               {}, {"", "puntos de control de su Spline", "numero"}, true);

    // Puntos de control en el mundo (desde 1).
    n.method("Entity", "getSplineControlPoint",
             [&rt](api::Call& c) {
                 const ecs::Entity e = rt.selfEntity(c);
                 const spline::Spline* s = splineOf(e);
                 const int i = controlIndex(s, c.integer(0));
                 if (i < 0) return api::Value{};
                 return api::Value(ecs::transformPoint(e.worldMatrix(), s->points[static_cast<std::size_t>(i)].position));
             },
             {"1", "Spline: punto de control (mundo) o nil", "Vec3"});
    n.method("Entity", "setSplineControlPoint",
             [&rt](api::Call& c) {
                 const ecs::Entity e = rt.selfEntity(c);
                 spline::Spline* s = splineOf(e);
                 const int i = controlIndex(s, c.integer(0));
                 const Vec3 world_pos = c.vec3(1);
                 if (i < 0) return api::Value(false);
                 s->points[static_cast<std::size_t>(i)].position = ecs::transformPoint(core::inverse(e.worldMatrix()), world_pos);
                 s->markModified();
                 return api::Value(true);
             },
             {"1, Vec3", "Spline: mueve un punto de control (mundo)", "bool"});
    n.method("Entity", "addSplinePoint",
             [&rt](api::Call& c) {
                 ecs::Entity e = rt.selfEntity(c);
                 const Vec3 world_pos = c.vec3(0);
                 spline::Spline& s = e.has<spline::Spline>() ? e.get<spline::Spline>() : e.add<spline::Spline>();
                 spline::SplinePoint p;
                 p.position = ecs::transformPoint(core::inverse(e.worldMatrix()), world_pos);
                 p.width = static_cast<float>(c.number(1, s.points.empty() ? 1.0 : s.points.back().width));
                 s.points.push_back(p);
                 s.markModified();
                 return api::Value(true);
             },
             {"Vec3, ancho", "Spline: anade un punto al final (la crea si no tiene)", "bool"});
    n.method("Entity", "clearSplinePoints",
             [&rt](api::Call& c) {
                 if (spline::Spline* s = splineOf(rt.selfEntity(c))) {
                     s->points.clear();
                     s->markModified();
                 }
                 return api::Value{};
             },
             {"", "Spline: quita todos los puntos"});

    // Seguidor.
    n.method("Entity", "playSplineFollower",
             [&rt](api::Call& c) {
                 if (spline::SplineFollower* f = followerOf(rt.selfEntity(c))) {
                     f->started = true;
                     f->playing = true;
                 }
                 return api::Value{};
             },
             {"", "SplineFollower: empieza a moverse"});
    n.method("Entity", "stopSplineFollower",
             [&rt](api::Call& c) {
                 if (spline::SplineFollower* f = followerOf(rt.selfEntity(c))) {
                     f->started = true;
                     f->playing = false;
                 }
                 return api::Value{};
             },
             {"", "SplineFollower: se para"});
    n.property(
        "Entity", "splineDistance",
        [&rt](api::Call& c) {
            const spline::SplineFollower* f = followerOf(rt.selfEntity(c));
            return api::Value(f != nullptr ? f->distance : 0.0f);
        },
        [&rt](api::Call& c) {
            const float d = static_cast<float>(c.number(0));
            if (spline::SplineFollower* f = followerOf(rt.selfEntity(c))) f->distance = d;
            return api::Value{};
        },
        {"", "SplineFollower: donde va (m)", "numero"}, true);
    n.property(
        "Entity", "splineSpeed",
        [&rt](api::Call& c) {
            const spline::SplineFollower* f = followerOf(rt.selfEntity(c));
            return api::Value(f != nullptr ? f->speed : 0.0f);
        },
        [&rt](api::Call& c) {
            const float v = static_cast<float>(c.number(0));
            if (spline::SplineFollower* f = followerOf(rt.selfEntity(c))) f->speed = v;
            return api::Value{};
        },
        {"", "SplineFollower: m/s", "numero"}, true);

    // Spline.create(puntos, forma, nombre, cerrada) -> entidad
    n.function("Spline.create",
               [&rt](api::Call& c) {
                   std::vector<Vec3> points;
                   for (const api::Value& v : listArg(c, 0)) {
                       if (v.isVec3()) points.push_back(v.asVec3());
                   }
                   const int shape = splineShapeFromName(c.string(1, ""));
                   const std::string name = c.string(2, "Spline");
                   const bool closed = c.boolean(3, false);
                   if (rt.world == nullptr) return api::Value{};
                   return rt.entityValue(spline::createSplineEntity(*rt.world, points, shape, name, closed));
               },
               {"{Vec3(0,0,0), Vec3(0,0,20)}, \"road\", \"Camino\", cerrada",
                "crea una spline (road, path, river, wall, fence, pipe, rails, ribbon o nada)", "Entity"});
}

void registerUi(Runtime& rt) {
    api::NativeApi& n = rt.native;

    // --- Dropdown (la opcion elegida, desde 1) ---
    n.property(
        "Entity", "dropdownValue",
        [&rt](api::Call& c) {
            const ui::Dropdown* d = dropdownOf(rt.selfEntity(c));
            return api::Value(d != nullptr ? d->value + 1 : 0);
        },
        [&rt](api::Call& c) {
            const long long v = c.integer(0);
            if (ui::Dropdown* d = dropdownOf(rt.selfEntity(c))) {
                const long long last = std::max(static_cast<long long>(d->options.size()) - 1, 0LL);
                d->value = static_cast<int>(std::clamp(v - 1, 0LL, last));
            }
            return api::Value{};
        },
        {"", "Desplegable: la elegida (desde 1)", "numero"}, true);
    n.property("Entity", "dropdownText",
               [&rt](api::Call& c) {
                   const ui::Dropdown* d = dropdownOf(rt.selfEntity(c));
                   if (d == nullptr || d->value < 0 || d->value >= static_cast<int>(d->options.size())) {
                       return api::Value(std::string());
                   }
                   return api::Value(d->options[static_cast<std::size_t>(d->value)]);
               },
               {}, {"", "Desplegable: texto de la elegida", "texto"}, true);
    n.method("Entity", "setDropdownOptions",
             [&rt](api::Call& c) {
                 ui::Dropdown* d = dropdownOf(rt.selfEntity(c));
                 const api::Value::Array& options = listArg(c, 0);
                 if (d == nullptr) return api::Value{};
                 d->options.clear();
                 for (const api::Value& v : options) {
                     if (v.isString()) d->options.push_back(v.asString());
                 }
                 d->value = std::clamp(d->value, 0, std::max(static_cast<int>(d->options.size()) - 1, 0));
                 return api::Value{};
             },
             {"{\"Baja\", \"Alta\"}", "Desplegable: cambia las opciones"});
    n.method("Entity", "getDropdownOptions",
             [&rt](api::Call& c) {
                 api::Value::Array list;
                 if (const ui::Dropdown* d = dropdownOf(rt.selfEntity(c))) {
                     for (const std::string& option : d->options) list.emplace_back(option);
                 }
                 return api::Value(std::move(list));
             },
             {"", "Desplegable: lista de opciones", "lista de texto"});

    // --- ScrollView ---
    n.property(
        "Entity", "scrollY",
        [&rt](api::Call& c) {
            const ui::ScrollView* s = scrollViewOf(rt.selfEntity(c));
            return api::Value(s != nullptr ? s->scroll.y : 0.0f);
        },
        [&rt](api::Call& c) {
            const float v = static_cast<float>(c.number(0));
            if (ui::ScrollView* s = scrollViewOf(rt.selfEntity(c))) {
                s->scroll.y = v;
                s->velocity = core::Vec2{};
            }
            return api::Value{};
        },
        {"", "Scroll View: desplazamiento vertical", "numero"}, true);
    n.property(
        "Entity", "scrollX",
        [&rt](api::Call& c) {
            const ui::ScrollView* s = scrollViewOf(rt.selfEntity(c));
            return api::Value(s != nullptr ? s->scroll.x : 0.0f);
        },
        [&rt](api::Call& c) {
            const float v = static_cast<float>(c.number(0));
            if (ui::ScrollView* s = scrollViewOf(rt.selfEntity(c))) {
                s->scroll.x = v;
                s->velocity = core::Vec2{};
            }
            return api::Value{};
        },
        {"", "Scroll View: desplazamiento horizontal", "numero"}, true);
    // 0..1 del recorrido (0 = arriba, 1 = abajo del todo).
    n.method("Entity", "scrollToFraction",
             [&rt](api::Call& c) {
                 const float t = static_cast<float>(c.number(0));
                 if (ui::ScrollView* s = scrollViewOf(rt.selfEntity(c))) {
                     s->velocity = core::Vec2{};
                     s->scroll.y = std::max(s->content.y, 0.0f) * std::clamp(t, 0.0f, 1.0f);  // se recorta al maquetar
                 }
                 return api::Value{};
             },
             {"0", "Scroll View: 0 = arriba, 1 = abajo del todo"});
    n.property("Entity", "scrollContentHeight",
               [&rt](api::Call& c) {
                   const ui::ScrollView* s = scrollViewOf(rt.selfEntity(c));
                   return api::Value(s != nullptr ? s->content.y : 0.0f);
               },
               {}, {"", "Scroll View: alto del contenido", "numero"}, true);

    // De la tabla Text (el resto, Text.get / Text.language..., esta en GameplayApi).
    n.function("Text.strip", [](api::Call& c) { return api::Value(ui::stripRichText(c.string(0))); },
               {"\"<b>Hola</b>\"", "el texto sin las etiquetas del texto enriquecido", "texto"});
}

void registerWorldPartition(Runtime& rt) {
    api::NativeApi& n = rt.native;
    n.function("WorldPartition.active",
               [](api::Call&) {
                   const worldpart::WorldPartitionSystem* p = worldpart::activePartition();
                   return api::Value(p != nullptr && p->active());
               },
               {"", "esta repartiendo el mundo en celdas?", "bool"});
    n.function("WorldPartition.loadAll",
               [&rt](api::Call&) {
                   worldpart::WorldPartitionSystem* p = worldpart::activePartition();
                   if (p != nullptr && rt.world != nullptr) p->loadAll(*rt.world);
                   return api::Value{};
               },
               {"", "carga todas las celdas ya"});
    n.function("WorldPartition.isLoaded",
               [](api::Call& c) {
                   const Vec3 position = c.vec3(0);
                   const worldpart::WorldPartitionSystem* p = worldpart::activePartition();
                   return api::Value(p == nullptr || !p->active() || p->cellLoaded(p->cellOf(position)));
               },
               {"posicion", "la celda de ese punto esta cargada?", "bool"});
    n.function("WorldPartition.stats",
               [](api::Call&) {
                   api::Value t = api::Value::object();
                   if (const worldpart::WorldPartitionSystem* p = worldpart::activePartition()) {
                       const worldpart::PartitionStats& s = p->stats();
                       t.set("active", s.active);
                       t.set("cells", s.cells);
                       t.set("loadedCells", s.loaded_cells);
                       t.set("objects", s.streamed_entities);
                       t.set("unloadedObjects", s.unloaded_entities);
                       t.set("storedBytes", static_cast<double>(s.stored_bytes));
                   }
                   return t;
               },
               {"", "{cells, loadedCells, objects, unloadedObjects, storedBytes}", "objeto"});
}

void registerAccessibility(Runtime& rt) {
    api::NativeApi& n = rt.native;
    n.function("Accessibility.get",
               [](api::Call&) {
                   const gameplay::AccessibilitySettings& a = gameplay::accessibility();
                   api::Value t = api::Value::object();
                   t.set("colorblind", a.colorblind_mode);
                   t.set("colorblindStrength", a.colorblind_strength);
                   t.set("colorblindCorrect", a.colorblind_correct);
                   t.set("textScale", a.text_scale);
                   t.set("subtitleScale", a.subtitle_scale);
                   t.set("subtitleBackground", a.subtitle_background);
                   t.set("reduceMotion", a.reduce_motion);
                   t.set("cameraShake", a.camera_shake);
                   t.set("highContrast", a.high_contrast_ui);
                   return t;
               },
               {"", "{colorblind, colorblindStrength, colorblindCorrect, textScale, subtitleScale, ...}", "objeto"});
    // Accessibility.set{ colorblind = 2, textScale = 1.25, reduceMotion = true }
    n.function("Accessibility.set",
               [](api::Call& c) {
                   const api::Value& t = optionsArg(c, 0);
                   gameplay::AccessibilitySettings& a = gameplay::accessibility();
                   a.colorblind_mode = std::clamp(intField(t, "colorblind", a.colorblind_mode), 0, 4);
                   a.colorblind_strength = std::clamp(numberField(t, "colorblindStrength", a.colorblind_strength), 0.0f, 1.0f);
                   a.colorblind_correct = boolField(t, "colorblindCorrect", a.colorblind_correct);
                   a.text_scale = std::clamp(numberField(t, "textScale", a.text_scale), 0.5f, 3.0f);
                   a.subtitle_scale = std::clamp(numberField(t, "subtitleScale", a.subtitle_scale), 0.5f, 3.0f);
                   a.subtitle_background = boolField(t, "subtitleBackground", a.subtitle_background);
                   a.reduce_motion = boolField(t, "reduceMotion", a.reduce_motion);
                   a.camera_shake = std::clamp(numberField(t, "cameraShake", a.camera_shake), 0.0f, 2.0f);
                   a.high_contrast_ui = boolField(t, "highContrast", a.high_contrast_ui);
                   return api::Value{};
               },
               {"{colorblind = 2, textScale = 1.3, reduceMotion = true}", "cambia opciones (las que vengan)"});
    n.function("Accessibility.save", [](api::Call&) { return api::Value(gameplay::saveAccessibility()); },
               {"", "las guarda (por jugador)", "bool"});
    n.function("Accessibility.load", [](api::Call&) { return api::Value(gameplay::loadAccessibility()); },
               {"", "las vuelve a leer", "bool"});
    n.function("Accessibility.reset",
               [](api::Call&) {
                   gameplay::accessibility() = gameplay::AccessibilitySettings{};
                   return api::Value{};
               },
               {"", "por defecto"});
    n.function("Accessibility.colorblindName",
               [](api::Call& c) {
                   return api::Value(std::string(gameplay::colorblindModeName(static_cast<int>(c.integer(0)))));
               },
               {"2", "nombre del tipo de daltonismo", "texto"});

    // Camera.shake(intensidad 0..1, duracion s, frecuencia Hz)
    n.function("Camera.shake",
               [](api::Call& c) {
                   gameplay::addCameraShake(static_cast<float>(c.number(0)), static_cast<float>(c.number(1, 0.5)),
                                            static_cast<float>(c.number(2, 18.0)));
                   return api::Value{};
               },
               {"0.6, 0.4, 18", "temblor de camara (intensidad 0..1, segundos, Hz)"});
    n.function("Camera.stopShake",
               [](api::Call&) {
                   gameplay::clearCameraShake();
                   return api::Value{};
               },
               {"", "lo para"});
}

void registerVoice(Runtime& rt) {
    api::NativeApi& n = rt.native;
    n.function("Voice.start", [](api::Call&) { return api::Value(voice().start()); },
               {"", "abre el microfono y la salida", "bool"});
    n.function("Voice.stop",
               [](api::Call&) {
                   voice().stop();
                   return api::Value{};
               },
               {"", "los cierra"});
    n.function("Voice.setMode",
               [](api::Call& c) {
                   const std::string m = lowerText(c.string(0));
                   voice().setMode(m == "open" || m == "voz" ? audio::VoiceMode::Open
                                   : m == "off"              ? audio::VoiceMode::Off
                                                             : audio::VoiceMode::PushToTalk);
                   return api::Value{};
               },
               {"\"push\"", "push (pulsar para hablar), open (por voz) u off"});
    n.function("Voice.setTalking",
               [](api::Call& c) {
                   voice().setTalking(c.boolean(0));
                   return api::Value{};
               },
               {"true", "modo push: hablando"});
    n.function("Voice.setThreshold",
               [](api::Call& c) {
                   voice().setVoiceThreshold(static_cast<float>(c.number(0)));
                   return api::Value{};
               },
               {"0.02", "modo open: volumen minimo"});
    n.function("Voice.setVolume",
               [](api::Call& c) {
                   voice().setVolume(static_cast<float>(c.number(0)));
                   return api::Value{};
               },
               {"1", "volumen de los demas"});
    n.function("Voice.setMicGain",
               [](api::Call& c) {
                   voice().setMicGain(static_cast<float>(c.number(0)));
                   return api::Value{};
               },
               {"1", "ganancia del microfono"});
    n.function("Voice.setProximity",
               [](api::Call& c) {
                   voice().setProximity(static_cast<float>(c.number(0)));
                   return api::Value{};
               },
               {"30", "volumen por distancia (0 = todos igual)"});
    n.function("Voice.setMuted",
               [](api::Call& c) {
                   voice().setMuted(static_cast<std::uint32_t>(c.integer(0)), c.boolean(1));
                   return api::Value{};
               },
               {"id, true", "silenciar a un jugador"});
    n.function("Voice.isMuted",
               [](api::Call& c) { return api::Value(voice().muted(static_cast<std::uint32_t>(c.integer(0)))); },
               {"id", "", "bool"});
    n.function("Voice.isSpeaking",
               [&rt](api::Call& c) {
                   if (!c.has(0)) return api::Value(voice().localSpeaking());
                   const auto player = static_cast<std::uint32_t>(c.integer(0));
                   if (rt.network && player == rt.network->localId()) return api::Value(voice().localSpeaking());
                   return api::Value(voice().isSpeaking(player));
               },
               {"id", "habla ahora? (sin id = yo)", "bool"});
    n.function("Voice.micLevel", [](api::Call&) { return api::Value(voice().micLevel()); },
               {"", "0..1 del microfono", "numero"});
}

void registerCrowdModsJobs(Runtime& rt) {
    api::NativeApi& n = rt.native;

    // --- Multitudes ---
    n.method("Entity", "spawnCrowd",
             [&rt](api::Call& c) {
                 const ecs::Entity e = rt.selfEntity(c);
                 ai::CrowdSystem* crowds = ai::activeCrowds();
                 return api::Value(crowds != nullptr && rt.world != nullptr ? crowds->spawn(*rt.world, e) : 0);
             },
             {"", "Crowd Spawner: crea (o recrea) la multitud; devuelve cuantos", "numero"});
    n.method("Entity", "despawnCrowd",
             [&rt](api::Call& c) {
                 const ecs::Entity e = rt.selfEntity(c);
                 if (ai::CrowdSystem* crowds = ai::activeCrowds(); crowds != nullptr && rt.world != nullptr) {
                     crowds->despawn(*rt.world, e);
                 }
                 return api::Value{};
             },
             {"", "Crowd Spawner: quita su multitud"});
    n.function("Crowd.stats",
               [](api::Call&) {
                   api::Value t = api::Value::object();
                   if (const ai::CrowdSystem* crowds = ai::activeCrowds()) {
                       t.set("agents", crowds->stats().agents);
                       t.set("visible", crowds->stats().visible);
                       t.set("spawners", crowds->stats().spawners);
                   }
                   return t;
               },
               {"", "{agents, visible, spawners}", "objeto"});

    // --- Mods ---
    n.function("Mods.enabled", [](api::Call&) { return api::Value(project::activeMods() != nullptr); },
               {"", "el juego carga mods?", "bool"});
    n.function("Mods.list",
               [](api::Call&) {
                   api::Value::Array list;
                   if (const project::ModManager* m = project::activeMods()) {
                       for (const project::ModInfo& info : m->mods()) {
                           api::Value row = api::Value::object();
                           row.set("id", info.id);
                           row.set("name", info.name);
                           row.set("version", info.version);
                           row.set("author", info.author);
                           row.set("description", info.description);
                           row.set("enabled", info.enabled);
                           bool loaded = false;
                           for (const project::ModMount& mm : m->mounted()) loaded = loaded || mm.id == info.id;
                           row.set("loaded", loaded);
                           list.push_back(std::move(row));
                       }
                   }
                   return api::Value(std::move(list));
               },
               {"", "{id, name, version, author, description, enabled, loaded}", "lista de objetos"});
    // Se aplica al volver a abrir el juego.
    n.function("Mods.setEnabled",
               [](api::Call& c) {
                   const std::string id = c.string(0);
                   const bool enabled = c.boolean(1);
                   project::ModManager* m = project::activeMods();
                   if (m == nullptr || !m->setEnabled(id, enabled)) return api::Value(false);
                   return api::Value(m->saveState());
               },
               {"id, false", "activa o desactiva (al volver a abrir)", "bool"});
    n.function("Mods.isLoaded",
               [](api::Call& c) {
                   const std::string id = c.string(0);
                   const project::ModManager* m = project::activeMods();
                   if (m == nullptr) return api::Value(false);
                   for (const project::ModMount& mm : m->mounted()) {
                       if (mm.id == id) return api::Value(true);
                   }
                   return api::Value(false);
               },
               {"id", "", "bool"});

    // --- Job system ---
    n.function("Jobs.workers", [](api::Call&) { return api::Value(jobs::workerCount()); },
               {"", "hilos del job system", "numero"});
    n.function("Jobs.executed", [](api::Call&) { return api::Value(static_cast<double>(jobs::stats().executed)); },
               {"", "tareas ejecutadas", "numero"});
}

}  // namespace

void registerFeatures24Api(Runtime& rt) {
    registerSplines(rt);
    registerUi(rt);
    registerWorldPartition(rt);
    registerAccessibility(rt);
    registerVoice(rt);
    registerCrowdModsJobs(rt);
}

}  // namespace cramion::scripting::native
