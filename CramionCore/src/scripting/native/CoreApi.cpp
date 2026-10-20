// Time, Scene, Prefs, Game, Profiler, CVar, Physics, Audio y Random: lo basico
// del juego (con los mismos nombres de siempre).

#include "Modules.h"

#include "CramionCore/audio/Audio.h"
#include "CramionCore/cvar/CVar.h"
#include "CramionCore/profiling/Profiler.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>
#include <utility>

namespace cramion::scripting::native {
namespace {

using core::Quat;
using core::Vec3;

constexpr float kPi = 3.14159265358979323846f;

// Una entidad encontrada, o nil si no hay.
api::Value foundEntity(const Runtime& rt, const ecs::Entity& e) { return e.valid() ? rt.entityValue(e) : api::Value{}; }

// --- Time --------------------------------------------------------------------------
// Lo pone ScriptSystem::update / fixedUpdate cada frame (solo lectura).

void registerTime(Runtime& rt) {
    rt.native.property("Time", "deltaTime", [&rt](api::Call&) { return api::Value(rt.delta); }, {},
                       {"", "Segundos desde el frame anterior", "numero"});
    rt.native.property("Time", "time", [&rt](api::Call&) { return api::Value(rt.time); }, {},
                       {"", "Segundos desde el Play", "numero"});
    rt.native.property("Time", "frameCount", [&rt](api::Call&) { return api::Value(static_cast<double>(rt.frame)); }, {},
                       {"", "Frames desde el Play", "numero"});
    rt.native.property("Time", "fixedDeltaTime", [&rt](api::Call&) { return api::Value(rt.fixed_delta); }, {},
                       {"", "Paso fijo de la fisica", "numero"});
}

// --- Scene -------------------------------------------------------------------------

void registerScene(Runtime& rt) {
    rt.native.function("Scene.find", [&rt](api::Call& c) {
        const std::string name = c.string(0);
        return rt.world != nullptr ? foundEntity(rt, rt.world->findByName(name)) : api::Value{};
    }, {"\"nombre\"", "El primer objeto con ese nombre (o nil)", "Entity"});
    rt.native.function("Scene.findWithTag", [&rt](api::Call& c) {
        const std::string tag = c.string(0);
        return rt.world != nullptr ? foundEntity(rt, rt.world->findWithTag(tag)) : api::Value{};
    }, {"\"tag\"", "El primer objeto con ese tag", "Entity"});
    rt.native.function("Scene.findAllWithTag", [&rt](api::Call& c) {
        const std::string tag = c.string(0);
        api::Value::Array out;
        if (rt.world != nullptr) {
            for (const ecs::Entity& e : rt.world->findAllWithTag(tag)) out.push_back(rt.entityValue(e));
        }
        return api::Value(std::move(out));
    }, {"\"tag\"", "Lista de objetos con ese tag", "lista de Entity"});
    rt.native.function("Scene.create", [&rt](api::Call& c) {
        const std::string name = c.string(0);
        if (rt.world == nullptr) return api::Value{};
        ecs::Entity e = rt.world->create(name);
        if (c.has(1)) e.setWorldPosition(c.vec3(1));
        return rt.entityValue(e);
    }, {"\"nombre\", posicion", "Objeto vacio nuevo", "Entity"});
    // Scene.instantiate (con la cache de plantillas de prefab) y el pool:
    // SceneStreamingApi.cpp.
    // Muchas transformaciones en una llamada (los scripts de C++ lo hacen con
    // Scene::setTransforms, sin pasar por aqui). Una lista que falte (nil) no
    // se toca; giros en Quat o en grados.
    rt.native.function("Scene.setTransforms", [&rt](api::Call& c) {
        const api::Value& entities = c.list(0);
        const api::Value& positions = c.list(1);
        const api::Value& rotations = c.list(2);
        const api::Value& scales = c.list(3);
        const bool local = c.boolean(4, false);
        if (rt.world == nullptr) return api::Value(0.0);
        int moved = 0;
        for (std::size_t i = 0; i < entities.size(); ++i) {
            ecs::Entity e = rt.entity(entities[i].asEntity());
            if (!e.valid()) continue;
            const api::Value& r = rotations[i];
            if (r.isQuat()) e.setLocalRotation(core::normalize(r.asQuat()));
            else if (r.isVec3()) e.setLocalEulerDegrees(r.asVec3());
            if (scales[i].isVec3()) e.setLocalScale(scales[i].asVec3());
            if (positions[i].isVec3()) local ? e.setLocalPosition(positions[i].asVec3()) : e.setWorldPosition(positions[i].asVec3());
            ++moved;
        }
        return api::Value(static_cast<double>(moved));
    }, {"entidades, posiciones, rotaciones, escalas, local",
        "Posicion (del mundo; local = true: respecto al padre), giro local (Quat o grados) y escala local de muchos "
        "objetos en una llamada; una lista nil no se toca",
        "numero"});
    rt.native.function("Scene.destroy", [&rt](api::Call& c) {
        const ecs::Entity e = rt.entity(c.entity(0));
        if (e.valid()) rt.destroyLater(e.handle());
        return api::Value{};
    }, {"entity", "Lo destruye al final del frame (en la consola fuera de Play, al terminar el comando)"});
    // Cambiar de escena al terminar el frame (como SceneManager.LoadScene).
    rt.native.function("Scene.load", [&rt](api::Call& c) {
        const std::string name = c.string(0);
        const std::filesystem::path path = rt.findScene(name);
        if (path.empty()) {
            rt.write(2, "Scene.load: no existe la escena \"" + name + "\"");
            return api::Value(false);
        }
        rt.scene_request = path;
        return api::Value(true);
    }, {"\"Nivel2\"", "Cambia de escena al terminar el frame (nombre o ruta del .crscene)", "bool"});
    rt.native.function("Scene.name", [&rt](api::Call&) { return api::Value(rt.scene_name); },
                       {"", "Nombre de la escena actual", "texto"});
    // Origen flotante: la posicion real (doble precision) del (0,0,0) del
    // mundo, y conversiones. Para guardar posiciones en una partida:
    // Scene.toAbsolute al guardar y Scene.toLocal al cargar (el origen puede
    // ser otro). Devuelve una lista de tres numeros (x, y, z).
    rt.native.function("Scene.origin", [&rt](api::Call&) {
        if (rt.world == nullptr) return api::Value(api::Value::Array{0.0, 0.0, 0.0});
        const ecs::DVec3& o = rt.world->origin();
        return api::Value(api::Value::Array{o.x, o.y, o.z});
    }, {"", "x, y, z (doble precision) del origen flotante del mundo", "lista de 3 numeros"});
    rt.native.function("Scene.toAbsolute", [&rt](api::Call& c) {
        const Vec3 local = c.vec3(0);
        if (rt.world == nullptr) {
            return api::Value(api::Value::Array{static_cast<double>(local.x), static_cast<double>(local.y),
                                                static_cast<double>(local.z)});
        }
        const ecs::DVec3 a = rt.world->absolute(local);
        return api::Value(api::Value::Array{a.x, a.y, a.z});
    }, {"posicion", "x, y, z absolutos (para guardar posiciones en una partida)", "lista de 3 numeros"});
    rt.native.function("Scene.toLocal", [&rt](api::Call& c) {
        const ecs::DVec3 absolute{c.number(0), c.number(1), c.number(2)};
        if (rt.world == nullptr) {
            return api::Value(Vec3{static_cast<float>(absolute.x), static_cast<float>(absolute.y),
                                   static_cast<float>(absolute.z)});
        }
        return api::Value(rt.world->local(absolute));
    }, {"x, y, z", "Vec3 local de una posicion absoluta guardada", "Vec3"});
}

// --- Prefs (PlayerPrefs) -------------------------------------------------------------
// Numeros y textos que sobreviven al cambiar de escena y se guardan en disco.

void registerPrefs(Runtime& rt) {
    rt.native.function("Prefs.setInt", [&rt](api::Call& c) {
        rt.prefs[c.string(0)] = std::to_string(static_cast<int>(c.integer(1)));
        rt.savePrefs();
        return api::Value{};
    }, {"\"clave\", 3", "Guarda un entero"});
    rt.native.function("Prefs.getInt", [&rt](api::Call& c) {
        const auto it = rt.prefs.find(c.string(0));
        return api::Value(it != rt.prefs.end() ? std::atoi(it->second.c_str()) : static_cast<int>(c.integer(1, 0)));
    }, {"\"clave\", 0", "Lee un entero (o el valor por defecto)", "numero"});
    rt.native.function("Prefs.setFloat", [&rt](api::Call& c) {
        rt.prefs[c.string(0)] = std::to_string(static_cast<float>(c.number(1)));
        rt.savePrefs();
        return api::Value{};
    }, {"\"clave\", 0.5", "Guarda un numero"});
    rt.native.function("Prefs.getFloat", [&rt](api::Call& c) {
        const auto it = rt.prefs.find(c.string(0));
        return api::Value(it != rt.prefs.end() ? std::strtof(it->second.c_str(), nullptr)
                                               : static_cast<float>(c.number(1, 0.0)));
    }, {"\"clave\", 0.0", "Lee un numero", "numero"});
    rt.native.function("Prefs.setString", [&rt](api::Call& c) {
        rt.prefs[c.string(0)] = c.string(1);
        rt.savePrefs();
        return api::Value{};
    }, {"\"clave\", \"texto\"", "Guarda un texto"});
    rt.native.function("Prefs.getString", [&rt](api::Call& c) {
        const auto it = rt.prefs.find(c.string(0));
        return api::Value(it != rt.prefs.end() ? it->second : c.string(1, ""));
    }, {"\"clave\", \"\"", "Lee un texto", "texto"});
    rt.native.function("Prefs.hasKey", [&rt](api::Call& c) { return api::Value(rt.prefs.count(c.string(0)) != 0); },
                       {"\"clave\"", "Existe?", "bool"});
    rt.native.function("Prefs.deleteKey", [&rt](api::Call& c) {
        rt.prefs.erase(c.string(0));
        rt.savePrefs();
        return api::Value{};
    }, {"\"clave\"", "La borra"});
    rt.native.function("Prefs.deleteAll", [&rt](api::Call&) {
        rt.prefs.clear();
        rt.savePrefs();
        return api::Value{};
    }, {"", "Borra todo"});
}

// --- Game y Profiler -----------------------------------------------------------------

void registerGameAndProfiler(Runtime& rt) {
    rt.native.function("Game.quit", [&rt](api::Call&) {
        rt.quit_request = true;
        return api::Value{};
    }, {"", "Cierra el juego (en el editor, sale de Play)"});

    // Insights desde los scripts: zonas propias, capturas y lo que cuesta cada cosa.
    rt.native.function("Profiler.begin", [](api::Call& c) {
        prof::begin(prof::intern(c.string(0)));
        return api::Value{};
    }, {"\"nombre\"", "Empieza una zona propia del perfilador (Insights)"});
    rt.native.function("Profiler.finish", [](api::Call&) {
        prof::end();
        return api::Value{};
    }, {"", "Termina la ultima zona empezada con Profiler.begin"});
    rt.native.function("Profiler.counter", [](api::Call& c) {
        prof::counter(prof::intern(c.string(0)), c.number(1));
        return api::Value{};
    }, {"\"nombre\", valor", "Apunta el valor de un contador en este frame"});
    rt.native.function("Profiler.frameMs", [](api::Call&) {
        const std::vector<float> f = prof::frameTimes();
        return api::Value(f.empty() ? 0.0 : static_cast<double>(f.back()));
    }, {"", "Duracion del ultimo frame (ms)", "numero"});
    rt.native.function("Profiler.capture", [&rt](api::Call& c) {
        const std::filesystem::path file =
            prof::captureFolder() / ("captura_" + std::to_string(prof::frameIndex()) + ".crtrace");
        std::string error;
        if (!prof::saveTrace(file, static_cast<int>(c.integer(0, 300)), &error)) {
            rt.write(2, "Profiler.capture: " + error);
            return api::Value(std::string{});
        }
        return api::Value(file.string());
    }, {"frames", "Guarda una captura .crtrace de los ultimos frames (300) y devuelve su ruta", "texto"});
    rt.native.function("Profiler.zones", [](api::Call& c) {
        std::vector<prof::ZoneStats> z = prof::stats(120);
        std::sort(z.begin(), z.end(), [](const auto& a, const auto& b) { return a.self_ms > b.self_ms; });
        const std::size_t n = std::min(static_cast<std::size_t>(std::max<long long>(c.integer(0, 10), 0)), z.size());
        api::Value::Array out;
        for (std::size_t i = 0; i < n; ++i) {
            api::Value row = api::Value::object();
            row.set("name", z[i].path);
            row.set("ms", z[i].avg_ms);
            row.set("self", z[i].self_ms);
            row.set("max", z[i].max_ms);
            out.push_back(std::move(row));
        }
        return api::Value(std::move(out));
    }, {"cuantas", "Las zonas que mas cuestan (10): lista de {name, ms, self, max}", "lista de objetos"});
}

// --- CVar: las variables de configuracion del motor y del juego (cvar/CVar.h) ---------

// El valor de un script como texto de CVar ("true", "3", "texto").
std::string cvarText(const api::Value& value) {
    if (value.isBool()) return value.truthy() ? "true" : "false";
    if (value.isNumber()) return cvar::detail::formatNumber(value.asNumber(), false);
    if (value.isString()) return value.asString();
    return {};
}

void registerCVar(Runtime& rt) {
    rt.native.function("CVar.get", [](api::Call& c) {
        const cvar::CVarBase* v = cvar::Registry::instance().find(c.string(0));
        if (v == nullptr) return api::Value{};
        const std::string text = v->toString();
        switch (v->type()) {
            case cvar::Type::Bool: return api::Value(text == "true");
            case cvar::Type::Int:
            case cvar::Type::Float: {
                double d = 0.0;
                cvar::detail::parseNumber(text, d);
                return api::Value(d);
            }
            case cvar::Type::String: break;
        }
        return api::Value(text);
    }, {"\"nombre\"", "Valor de una variable de configuracion (o nil)", "valor"});
    rt.native.function("CVar.set", [&rt](api::Call& c) {
        std::string error;
        const bool ok = cvar::Registry::instance().set(c.string(0), cvarText(c.arg(1)), &error);
        if (!ok) rt.write(1, "CVar.set: " + error);
        return api::Value(ok);
    }, {"\"nombre\", valor", "Cambia una variable de configuracion", "bool"});
    // CVar.register("juego.Vidas", 3, "Vidas al empezar", true) -> el valor actual.
    rt.native.function("CVar.register", [&rt](api::Call& c) {
        const std::string name = c.string(0);
        const api::Value& value = c.arg(1);
        cvar::Type type = cvar::Type::String;
        if (value.isBool()) {
            type = cvar::Type::Bool;
        } else if (value.isNumber()) {
            type = cvar::Type::Float;
        }
        const cvar::CVarBase* v = cvar::Registry::instance().createDynamic(
            name, type, cvarText(value), c.string(2, ""), c.boolean(3, false) ? cvar::Saved : cvar::None);
        if (v == nullptr) {
            rt.write(1, "CVar.register: ya existe '" + name + "' con otro tipo");
            return api::Value{};
        }
        return api::Value(v->toString());
    }, {"\"juego.Vidas\", 3, \"descripcion\", guardada", "Crea una variable del juego (si no existe) y devuelve su valor",
        "texto"});
    rt.native.function("CVar.exists", [](api::Call& c) {
        return api::Value(cvar::Registry::instance().find(c.string(0)) != nullptr);
    }, {"\"nombre\"", "Existe la variable?", "bool"});
    rt.native.function("CVar.list", [](api::Call& c) {
        const bool filtered = c.has(0);
        const std::string filter = c.string(0, "");
        api::Value::Array out;
        for (const cvar::CVarBase* v : cvar::Registry::instance().all()) {
            if (filtered && v->name().find(filter) == std::string::npos) continue;
            out.emplace_back(v->name());
        }
        return api::Value(std::move(out));
    }, {"filtro", "Nombres de las variables (las que tienen el filtro en el nombre)", "lista de texto"});
}

// --- Physics y Audio -----------------------------------------------------------------

// Mascara de capas de un argumento: un numero (bit i = capa i; -1 = todas),
// el nombre de una capa o una lista de nombres (Project Settings > Physics).
// Sin el argumento, la de siempre (todas menos "Ignore Raycast").
std::uint32_t layerMaskArg(Runtime& rt, const api::Call& c, std::size_t i) {
    const api::Value& v = c.arg(i);
    if (v.isNil()) return physics::kDefaultRaycastLayers;
    if (v.isNumber()) return static_cast<std::uint32_t>(static_cast<long long>(std::llround(v.asNumber())));
    const auto bit = [&rt](const std::string& name) -> std::uint32_t {
        const int layer = rt.physics != nullptr ? rt.physics->settings().layerIndex(name) : physics::PhysicsSettings{}.layerIndex(name);
        if (layer < 0) rt.write(1, "Physics: no hay una capa \"" + name + "\" (Project Settings > Physics)");
        return physics::layerBit(layer);
    };
    if (v.isString()) return bit(v.asString());
    if (v.isArray()) {
        std::uint32_t mask = 0;
        for (const api::Value& item : v.items()) {
            if (item.isNumber()) mask |= physics::layerBit(static_cast<int>(item.asNumber()));
            else mask |= bit(item.asString());
        }
        return mask;
    }
    throw api::Error("mascara de capas: numero, nombre de capa o lista de nombres");
}

physics::QueryFilter queryFilter(Runtime& rt, const api::Call& c, std::size_t mask_arg, std::size_t ignore_arg) {
    physics::QueryFilter filter;
    filter.layer_mask = layerMaskArg(rt, c, mask_arg);
    if (c.has(ignore_arg)) filter.ignore = rt.entityArg(c, ignore_arg);
    return filter;
}

api::Value hitValue(Runtime& rt, const physics::RaycastHit& hit) {
    api::Value result = api::Value::object();
    result.set("entity", rt.entityValue(hit.entity));
    result.set("point", hit.point);
    result.set("normal", hit.normal);
    result.set("distance", hit.distance);
    result.set("layer", hit.layer);
    result.set("trigger", hit.trigger);
    return result;
}

void registerPhysicsAndAudio(Runtime& rt) {
    rt.native.function("Physics.raycast", [&rt](api::Call& c) {
        const Vec3 origin = c.vec3(0);
        const Vec3 direction = c.vec3(1);
        const float distance = static_cast<float>(c.number(2, 1000.0));
        if (rt.physics == nullptr) return api::Value{};
        const physics::QueryFilter filter = queryFilter(rt, c, 3, 4);
        physics::RaycastHit hit;
        if (!rt.physics->raycast(origin, direction, distance, hit, filter)) return api::Value{};
        return hitValue(rt, hit);
    }, {"origen, direccion, distancia, mascara, ignorar",
        "nil o {entity, point, normal, distance, layer, trigger}; mascara: numero, capa o lista de capas", "objeto"});
    // Una esfera que avanza (colision de camara, ataques con grosor).
    rt.native.function("Physics.sphereCast", [&rt](api::Call& c) {
        const Vec3 origin = c.vec3(0);
        const float radius = static_cast<float>(c.number(1));
        const Vec3 direction = c.vec3(2);
        const float distance = static_cast<float>(c.number(3, 1000.0));
        if (rt.physics == nullptr) return api::Value{};
        const physics::QueryFilter filter = queryFilter(rt, c, 4, 5);
        physics::RaycastHit hit;
        if (!rt.physics->sphereCast(origin, radius, direction, distance, hit, filter)) return api::Value{};
        return hitValue(rt, hit);
    }, {"origen, radio, direccion, distancia, mascara, ignorar",
        "esfera que avanza: nil o {entity, point, normal, distance, layer, trigger}", "objeto"});
    rt.native.function("Physics.overlapSphere", [&rt](api::Call& c) {
        const Vec3 center = c.vec3(0);
        const float radius = static_cast<float>(c.number(1));
        api::Value::Array out;
        if (rt.physics == nullptr) return api::Value(std::move(out));
        const physics::QueryFilter filter = queryFilter(rt, c, 2, 3);
        for (const ecs::Entity& e : rt.physics->overlapSphere(center, radius, filter)) out.push_back(rt.entityValue(e));
        return api::Value(std::move(out));
    }, {"centro, radio, mascara, ignorar", "los objetos que tocan la esfera", "lista de Entity"});
    // Mascara de capas por nombre: Physics.layerMask("Default", "Terreno").
    rt.native.function("Physics.layerMask", [&rt](api::Call& c) {
        std::uint32_t mask = 0;
        for (std::size_t i = 0; i < c.count(); ++i) {
            const api::Value& v = c.arg(i);
            if (v.isNil()) continue;
            mask |= layerMaskArg(rt, c, i);
        }
        return api::Value(static_cast<double>(mask));
    }, {"\"Default\", \"Terreno\", ...", "mascara (bit i = capa i) de esas capas", "numero"});
    // Que dos objetos no choquen (como Physics.IgnoreCollision de Unity).
    rt.native.function("Physics.ignoreCollision", [&rt](api::Call& c) {
        const ecs::Entity a = rt.entityArg(c, 0);
        const ecs::Entity b = rt.entityArg(c, 1);
        if (rt.physics != nullptr) rt.physics->ignoreCollision(a, b, c.boolean(2, true));
        return api::Value{};
    }, {"a, b, true", "a y b no chocan entre si (false lo deshace)"});
    // Tiempo de fisica simulado (pasos fijos dados): no depende de los FPS ni
    // de max_substeps, para medir velocidades reales.
    rt.native.function("Physics.simulatedTime", [&rt](api::Call&) {
        return api::Value(rt.physics != nullptr ? rt.physics->simulatedTime() : 0.0);
    }, {"", "Segundos de fisica simulados desde el Play (pasos fijos dados)", "numero"});

    rt.native.function("Audio.playOneShot", [&rt](api::Call& c) {
        const std::string clip = c.string(0);
        const bool spatial = c.has(1);
        const Vec3 position = c.vec3(1, Vec3{});
        const float volume = static_cast<float>(c.number(2, 1.0));
        if (rt.audio != nullptr) rt.audio->playOneShot(clip, position, volume, spatial);
        return api::Value{};
    }, {"\"Audio/golpe.wav\", posicion, volumen", "Sonido suelto (sin posicion = 2D)"});
    // Oclusion y paso bajo general de los AudioListener de la escena.
    rt.native.function("Audio.setOcclusion", [&rt](api::Call& c) {
        const bool on = c.boolean(0);
        if (rt.world == nullptr) return api::Value{};
        for (const entt::entity h : rt.world->registry().view<audio::AudioListener>()) {
            rt.world->registry().get<audio::AudioListener>(h).occlusion = on;
        }
        return api::Value{};
    }, {"true", "Las paredes tapan los sonidos (Audio Listener)"});
    rt.native.function("Audio.occlusion", [&rt](api::Call&) {
        if (rt.world == nullptr) return api::Value(false);
        for (const entt::entity h : rt.world->registry().view<audio::AudioListener>()) {
            return api::Value(rt.world->registry().get<audio::AudioListener>(h).occlusion);
        }
        return api::Value(false);
    }, {"", "Esta la oclusion activa?", "bool"});
    rt.native.function("Audio.setLowPass", [&rt](api::Call& c) {
        const bool on = c.boolean(0);
        const bool cutoff = c.has(1);
        const float hz = static_cast<float>(c.number(1, 0.0));
        if (rt.world == nullptr) return api::Value{};
        for (const entt::entity h : rt.world->registry().view<audio::AudioListener>()) {
            audio::AudioListener& l = rt.world->registry().get<audio::AudioListener>(h);
            l.low_pass = on;
            if (cutoff) l.low_pass_cutoff = hz;
        }
        return api::Value{};
    }, {"true, 800", "Todo apagado (bajo el agua, pausa)"});
    rt.native.function("Audio.reverbLevel", [&rt](api::Call&) {
        return api::Value(rt.audio != nullptr ? rt.audio->reverbLevel() : 0.0f);
    }, {"", "Reverberacion que se oye ahora (zonas)", "numero"});
}

// --- Random: aleatorios con semilla (como Random de Unity) ----------------------------

struct Rng {
    std::mt19937 engine{std::random_device{}()};
    float value() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(engine); }
    float range(float a, float b) { return a + (b - a) * value(); }
    int integer(int a, int b) {
        if (a > b) std::swap(a, b);
        return std::uniform_int_distribution<int>(a, b)(engine);
    }
    Vec3 onSphere() {
        std::normal_distribution<float> n(0.0f, 1.0f);
        for (int i = 0; i < 8; ++i) {
            const Vec3 v{n(engine), n(engine), n(engine)};
            const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            if (l > 1e-4f) return Vec3{v.x / l, v.y / l, v.z / l};
        }
        return Vec3{0.0f, 1.0f, 0.0f};
    }
};

void registerRandom(Runtime& rt) {
    auto rng = std::make_shared<Rng>();
    const auto integer = [](const api::Call& c, std::size_t i) { return static_cast<int>(c.integer(i)); };
    rt.native.function("Random.seed", [rng, integer](api::Call& c) {
        rng->engine.seed(static_cast<unsigned>(integer(c, 0)));
        return api::Value{};
    }, {"n", "Fija la semilla"});
    rt.native.function("Random.value", [rng](api::Call&) { return api::Value(rng->value()); }, {"", "0..1", "numero"});
    rt.native.function("Random.range", [rng](api::Call& c) {
        return api::Value(rng->range(static_cast<float>(c.number(0)), static_cast<float>(c.number(1))));
    }, {"min, max", "Decimal entre min y max", "numero"});
    rt.native.function("Random.int", [rng, integer](api::Call& c) {
        return api::Value(rng->integer(integer(c, 0), integer(c, 1)));
    }, {"min, max", "Entero (incluye los dos)", "numero"});
    rt.native.function("Random.chance", [rng](api::Call& c) {
        return api::Value(rng->value() < static_cast<float>(c.number(0)));
    }, {"0.25", "true con esa probabilidad", "bool"});
    rt.native.function("Random.sign", [rng](api::Call&) { return api::Value(rng->value() < 0.5f ? -1 : 1); },
                       {"", "-1 o 1", "numero"});
    rt.native.function("Random.onUnitSphere", [rng](api::Call&) { return api::Value(rng->onSphere()); },
                       {"", "Direccion al azar", "Vec3"});
    rt.native.function("Random.insideUnitSphere", [rng](api::Call&) {
        return api::Value(rng->onSphere() * std::cbrt(rng->value()));
    }, {"", "Punto dentro de la esfera", "Vec3"});
    rt.native.function("Random.insideUnitCircle", [rng](api::Call&) {  // en el plano del suelo (XZ)
        const float a = rng->value() * 2.0f * kPi;
        const float d = std::sqrt(rng->value());
        return api::Value(Vec3{std::cos(a) * d, 0.0f, std::sin(a) * d});
    }, {"", "Punto en el suelo (XZ)", "Vec3"});
    rt.native.function("Random.rotation", [rng](api::Call&) {  // rotacion uniforme al azar
        const float u1 = rng->value(), u2 = rng->value() * 2.0f * kPi, u3 = rng->value() * 2.0f * kPi;
        const float a = std::sqrt(1.0f - u1), b = std::sqrt(u1);
        return api::Value(Quat{a * std::sin(u2), a * std::cos(u2), b * std::sin(u3), b * std::cos(u3)});
    }, {"", "Quat al azar", "Quat"});
    rt.native.function("Random.pick", [rng](api::Call& c) {
        const api::Value::Array& list = c.list(0).items();
        if (list.empty()) return api::Value{};
        return list[static_cast<std::size_t>(rng->integer(1, static_cast<int>(list.size())) - 1)];
    }, {"lista", "Un elemento al azar", "valor"});
    // Devuelve la lista barajada (la del script no cambia: llega por
    // copia).
    rt.native.function("Random.shuffle", [rng](api::Call& c) {
        api::Value::Array list = c.list(0).items();
        for (int i = static_cast<int>(list.size()); i > 1; --i) {
            const int j = rng->integer(1, i);
            std::swap(list[static_cast<std::size_t>(i - 1)], list[static_cast<std::size_t>(j - 1)]);
        }
        return api::Value(std::move(list));
    }, {"lista", "Baraja", "lista"});
}

}  // namespace

void registerCoreApi(Runtime& rt) {
    registerTime(rt);
    registerScene(rt);
    registerPrefs(rt);
    registerGameAndProfiler(rt);
    registerCVar(rt);
    registerPhysicsAndAudio(rt);
    registerRandom(rt);
}

}  // namespace cramion::scripting::native
