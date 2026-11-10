// Fire, Weather / Environment, Sky, Voxel y Fluid: el mundo de la escena (fuego,
// clima, bloques y liquidos).

#include "Modules.h"

#include "CramionCore/ecs/Components.h"
#include "CramionCore/environment/Environment.h"
#include "CramionCore/fire/Fire.h"
#include "CramionCore/fluid/Fluid.h"
#include "CramionCore/voxel/Voxel.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace cramion::scripting::native {
namespace {

namespace envns = cramion::environment;
using core::Vec3;

// --- Fire ---
// Incendios de las zonas de fuego (fire/Fire.h). Posiciones en el mundo; el
// radio en metros.
void registerFire(Runtime& rt) {
    api::NativeApi& n = rt.native;
    n.function("Fire.ignite", [&rt](api::Call& c) -> api::Value {
        const Vec3 position = c.vec3(0);
        const float radius = static_cast<float>(c.number(1, 2.0));
        return rt.world != nullptr ? fire::ignite(*rt.world, position, std::max(radius, 0.1f)) : 0;
    }, {"posicion, radio", "enciende fuego en las zonas Fuego que tocan el circulo (devuelve cuantas)", "numero"});
    n.function("Fire.extinguish", [&rt](api::Call& c) -> api::Value {
        const Vec3 position = c.vec3(0);
        const float radius = static_cast<float>(c.number(1, 4.0));
        return rt.world != nullptr ? fire::extinguish(*rt.world, position, std::max(radius, 0.1f)) : 0;
    }, {"posicion, radio", "apaga el fuego en el circulo", "numero"});
    n.function("Fire.extinguishAll", [&rt](api::Call&) {
        if (rt.world != nullptr) fire::extinguishAll(*rt.world);
        return api::Value{};
    }, {"", "apaga todo"});
    n.function("Fire.reset", [&rt](api::Call&) {
        if (rt.world != nullptr) fire::resetAll(*rt.world);
        return api::Value{};
    }, {"", "vuelve a empezar: nada quemado (y se reenciende si 'Encender al empezar')"});
    n.function("Fire.isBurning", [&rt](api::Call& c) -> api::Value {
        const Vec3 position = c.vec3(0);
        return rt.world != nullptr && fire::heatAt(*rt.world, position) >= 0.3f;
    }, {"posicion", "hay llamas ahi?", "bool"});
    n.function("Fire.heatAt", [&rt](api::Call& c) -> api::Value {
        const Vec3 position = c.vec3(0);
        return rt.world != nullptr ? fire::heatAt(*rt.world, position) : 0.0f;
    }, {"posicion", "calor 0..1", "numero"});
    const auto burned_at = [&rt](api::Call& c) -> api::Value {
        const Vec3 position = c.vec3(0);
        return rt.world != nullptr ? fire::charAt(*rt.world, position) : 0.0f;
    };
    n.function("Fire.burnedAt", burned_at, {"posicion", "quemado 0..1", "numero"});
    n.function("Fire.charAt", burned_at, {"posicion", "lo mismo que burnedAt", "numero"});
    n.function("Fire.burnedFraction", [&rt](api::Call&) -> api::Value {
        return rt.world != nullptr ? fire::totalStats(*rt.world).burned_fraction : 0.0f;
    }, {"", "0..1 de lo que podia arder", "numero"});
    n.function("Fire.burningArea", [&rt](api::Call&) -> api::Value {
        return rt.world != nullptr ? fire::totalStats(*rt.world).burning_area : 0.0f;
    }, {"", "m2 en llamas", "numero"});
    n.function("Fire.isActive", [&rt](api::Call&) -> api::Value {
        return rt.world != nullptr && fire::totalStats(*rt.world).burning_cells > 0;
    }, {"", "hay algo ardiendo?", "bool"});
    n.function("Fire.stats", [&rt](api::Call&) {
        const fire::FireStats st = rt.world != nullptr ? fire::totalStats(*rt.world) : fire::FireStats{};
        api::Value out = api::Value::object();
        out.set("active", st.active);
        out.set("burningCells", st.burning_cells);
        out.set("smolderingCells", st.smoldering_cells);
        out.set("burnedCells", st.burned_cells);
        out.set("burnableCells", st.burnable_cells);
        out.set("burningArea", st.burning_area);
        out.set("burnedFraction", st.burned_fraction);
        out.set("size", st.size);
        out.set("seconds", st.simulated_seconds);
        out.set("windX", st.wind.x);
        out.set("windZ", st.wind.y);
        return out;
    }, {"", "{burningCells, burnedFraction, burningArea, seconds...}", "objeto"});
}

// --- Weather (alias Environment) ---
// Clima, hora, fecha, estacion y viento del componente Ambiente
// (environment/Environment.h). Los cambios crean el Ambiente si la escena no
// tiene. Cada funcion esta en las dos tablas (Environment es la misma que
// Weather).
void registerWeather(Runtime& rt) {
    const auto both = [&rt](const std::string& name, api::Function fn, const api::Doc& doc) {
        rt.native.function("Weather." + name, fn, doc);
        rt.native.function("Environment." + name, std::move(fn), doc);
    };
    const auto find = [&rt]() -> envns::Environment* {
        return rt.world != nullptr ? envns::findEnvironment(*rt.world) : nullptr;
    };
    const auto ensure = [&rt]() -> envns::Environment* {
        return rt.world != nullptr ? &envns::ensureEnvironment(*rt.world) : nullptr;
    };

    const auto set_weather = [&rt, ensure](api::Call& c) -> api::Value {
        const std::string name = c.string(0);
        envns::WeatherPreset preset{};
        if (!envns::presetFromName(name, preset)) {
            rt.write(1, "Weather.set: clima desconocido '" + name + "' (Clear, Cloudy, Overcast, Foggy, LightRain, "
                        "Rain, Storm, LightSnow, Snow, Blizzard, Sandstorm)");
            return false;
        }
        envns::Environment* env = ensure();
        if (env == nullptr) return false;
        const float seconds = static_cast<float>(c.number(1, env->transition_time));
        envns::setWeather(*env, preset, std::max(seconds, 0.0f));
        return true;
    };
    both("set", set_weather,
         {"\"Storm\", 10",
          "cambia de clima en N segundos (Clear, Cloudy, Overcast, Foggy, LightRain, Rain, Storm, LightSnow, Snow, "
          "Blizzard, Sandstorm)",
          "bool"});
    both("setWeather", set_weather, {"\"Rain\", 5", "lo mismo que set", "bool"});
    const auto get_weather = [find](api::Call&) -> api::Value {
        const envns::Environment* env = find();
        return std::string(env != nullptr ? envns::presetName(envns::currentPreset(*env)) : "Clear");
    };
    both("get", get_weather, {"", "clima actual (\"Storm\")", "texto"});
    both("getWeather", get_weather, {"", "clima actual (\"Storm\")", "texto"});
    both("getTarget", [find](api::Call&) -> api::Value {
        const envns::Environment* env = find();
        return std::string(env != nullptr ? envns::presetName(env->weather) : "Clear");
    }, {"", "clima al que va la transicion", "texto"});
    both("getLabel", [find](api::Call&) -> api::Value {
        const envns::Environment* env = find();
        return std::string(env != nullptr ? envns::presetLabel(envns::currentPreset(*env))
                                          : envns::presetLabel(envns::WeatherPreset::Clear));
    }, {"", "nombre visible (\"Tormenta\")", "texto"});
    both("presets", [](api::Call&) {
        api::Value::Array out;
        for (int i = 0; i < envns::kWeatherPresetCount; ++i) {
            out.emplace_back(std::string(envns::presetName(static_cast<envns::WeatherPreset>(i))));
        }
        return api::Value(std::move(out));
    }, {"", "lista de climas", "lista de texto"});
    // 0..1: cuanto lleva la transicion (1 = terminada).
    both("transition", [find](api::Call&) -> api::Value {
        const envns::Environment* env = find();
        return env != nullptr && env->runtime.initialized ? env->runtime.transition : 1.0f;
    }, {"", "0..1 lo que lleva la transicion", "numero"});
    both("isTransitioning", [find](api::Call&) -> api::Value {
        const envns::Environment* env = find();
        return env != nullptr && env->runtime.initialized && env->runtime.transition < 1.0f;
    }, {"", "esta cambiando?", "bool"});
    both("setRandom", [ensure](api::Call& c) {
        const bool on = c.boolean(0);
        if (envns::Environment* env = ensure()) {
            env->random_weather = on;
            if (c.has(1)) env->min_duration = std::max(static_cast<float>(c.number(1)), 5.0f);
            if (c.has(2)) env->max_duration = std::max(static_cast<float>(c.number(2)), env->min_duration);
            env->runtime.next_change = env->min_duration;
        }
        return api::Value{};
    }, {"true, 120, 360", "clima al azar (segundos min y max)"});

    // --- Hora y fecha ---
    both("setTime", [ensure](api::Call& c) {
        float hours = static_cast<float>(c.number(0));
        if (envns::Environment* env = ensure()) {
            hours = std::fmod(hours, 24.0f);
            env->time_of_day = hours < 0.0f ? hours + 24.0f : hours;
        }
        return api::Value{};
    }, {"18.5", "hora del dia (0..24)"});
    both("getTime", [find](api::Call&) -> api::Value {
        const envns::Environment* env = find();
        return env != nullptr ? env->time_of_day : 12.0f;
    }, {"", "hora del dia", "numero"});
    both("setDate", [ensure](api::Call& c) {
        const int day = static_cast<int>(c.integer(0));
        const int month = static_cast<int>(c.integer(1));
        if (envns::Environment* env = ensure()) {
            env->month = std::clamp(month, 1, 12);
            env->day = std::clamp(day, 1, 31);
        }
        return api::Value{};
    }, {"21, 12", "dia y mes (mueve el sol y la estacion)"});
    // Dos resultados (dia, mes): una lista.
    both("getDate", [find](api::Call&) {
        const envns::Environment* env = find();
        return env != nullptr ? api::Value(api::Value::Array{env->day, env->month})
                              : api::Value(api::Value::Array{21, 6});
    }, {"", "dia, mes", "lista {dia, mes}"});
    both("setLatitude", [ensure](api::Call& c) {
        const float degrees = static_cast<float>(c.number(0));
        if (envns::Environment* env = ensure()) env->latitude = std::clamp(degrees, -89.0f, 89.0f);
        return api::Value{};
    }, {"40", "latitud en grados"});
    both("getLatitude", [find](api::Call&) -> api::Value {
        const envns::Environment* env = find();
        return env != nullptr ? env->latitude : 40.0f;
    }, {"", "latitud", "numero"});
    // Minutos reales por dia de juego; 0 o nil = el tiempo se para.
    both("setDayLength", [ensure](api::Call& c) {
        const bool given = c.has(0);
        const float minutes = given ? static_cast<float>(c.number(0)) : 0.0f;
        if (envns::Environment* env = ensure()) {
            env->time_progress = given && minutes > 0.0f;
            if (env->time_progress) env->day_length = std::max(minutes, 0.05f);
        }
        return api::Value{};
    }, {"24", "minutos reales por dia (nil = el tiempo se para)"});
    // Velocidad del tiempo: 1 = tiempo real (dia de 24 h), 60 = un dia en 24 min; 0 = parado.
    both("setTimeScale", [ensure](api::Call& c) {
        const float scale = static_cast<float>(c.number(0));
        if (envns::Environment* env = ensure()) {
            env->time_progress = scale > 0.0f;
            if (scale > 0.0f) env->day_length = std::max(1440.0f / scale, 0.05f);
        }
        return api::Value{};
    }, {"60", "velocidad del tiempo (1 = real, 0 = parado)"});
    both("getTimeScale", [find](api::Call&) -> api::Value {
        const envns::Environment* env = find();
        return env != nullptr && env->time_progress ? 1440.0f / std::max(env->day_length, 0.05f) : 0.0f;
    }, {"", "velocidad del tiempo", "numero"});

    // --- Estacion ---
    both("setSeason", [&rt, ensure](api::Call& c) {
        const std::string name = c.string(0);
        envns::Environment* env = ensure();
        if (env == nullptr) return api::Value{};
        if (name.empty() || name == "auto" || name == "Auto") {
            env->season_from_date = true;
            return api::Value{};
        }
        envns::Season season{};
        if (!envns::seasonFromName(name, season)) {
            rt.write(1, "Weather.setSeason: estacion desconocida '" + name + "' (Spring, Summer, Autumn, Winter, auto)");
            return api::Value{};
        }
        env->season_from_date = false;
        env->season = season;
        return api::Value{};
    }, {"\"Winter\"", "estacion fija (\"auto\" = por la fecha)"});
    both("getSeason", [find](api::Call&) -> api::Value {
        const envns::Environment* env = find();
        if (env == nullptr) return std::string("Summer");
        return std::string(envns::seasonName(env->runtime.initialized ? env->runtime.season_now : env->season));
    }, {"", "estacion actual", "texto"});
    both("getTemperature", [find](api::Call&) -> api::Value {
        const envns::Environment* env = find();
        return env != nullptr ? env->runtime.temperature : 15.0f;
    }, {"", "grados C", "numero"});

    // --- Viento ---
    both("setWind", [ensure](api::Call& c) {
        const float direction = static_cast<float>(c.number(0));
        if (envns::Environment* env = ensure()) {
            env->wind_direction = direction;
            if (c.has(1)) env->wind_strength = std::max(static_cast<float>(c.number(1)), 0.0f);
        }
        return api::Value{};
    }, {"90, 1.5", "direccion (grados) y fuerza del viento"});
    both("getWind", [find](api::Call&) -> api::Value {
        const envns::Environment* env = find();
        return env != nullptr ? env->runtime.wind : Vec3{};
    }, {"", "Vec3 del viento (m/s)", "Vec3"});
    both("getWindSpeed", [find](api::Call&) -> api::Value {
        const envns::Environment* env = find();
        return env != nullptr ? env->runtime.wind_speed : 0.0f;
    }, {"", "m/s con rachas", "numero"});
    both("getWindDirection", [find](api::Call&) -> api::Value {
        const envns::Environment* env = find();
        return env != nullptr ? env->runtime.wind_direction : 0.0f;
    }, {"", "grados", "numero"});

    // --- Lluvia, nieve y superficies ---
    both("getRain", [find](api::Call&) -> api::Value {
        const envns::Environment* env = find();
        return env != nullptr ? env->runtime.current.rain : 0.0f;
    }, {"", "lluvia 0..1", "numero"});
    both("getSnow", [find](api::Call&) -> api::Value {
        const envns::Environment* env = find();
        return env != nullptr ? env->runtime.current.snow : 0.0f;
    }, {"", "nevada 0..1", "numero"});
    both("getFog", [find](api::Call&) -> api::Value {
        const envns::Environment* env = find();
        return env != nullptr ? env->runtime.current.fog * env->fog_strength : 0.0f;
    }, {"", "densidad de la niebla", "numero"});
    both("getWetness", [find](api::Call&) -> api::Value {
        const envns::Environment* env = find();
        return env != nullptr ? env->runtime.wetness : 0.0f;
    }, {"", "humedad de las superficies 0..1", "numero"});
    both("setWetness", [ensure](api::Call& c) {
        const float wetness = static_cast<float>(c.number(0));
        if (envns::Environment* env = ensure()) {
            envns::setWetness(*env, wetness, static_cast<float>(c.number(1, env->runtime.puddles)));
        }
        return api::Value{};
    }, {"1, 0.6", "humedad y charcos al instante"});
    both("getSnowCover", [find](api::Call&) -> api::Value {
        const envns::Environment* env = find();
        return env != nullptr ? env->runtime.snow_cover : 0.0f;
    }, {"", "nieve acumulada 0..1", "numero"});
    both("setSnowCover", [ensure](api::Call& c) {
        const float cover = static_cast<float>(c.number(0));
        if (envns::Environment* env = ensure()) envns::setSnowCover(*env, cover);
        return api::Value{};
    }, {"1", "nieve acumulada al instante"});
    both("setPrecipitationDensity", [ensure](api::Call& c) {
        const float density = static_cast<float>(c.number(0));
        if (envns::Environment* env = ensure()) env->precipitation_density = std::clamp(density, 0.0f, 3.0f);
        return api::Value{};
    }, {"0.5", "menos gotas (rendimiento)"});

    // --- Rayos ---
    both("lightning", [ensure](api::Call& c) {
        const float distance = static_cast<float>(c.number(0, -1.0));
        if (envns::Environment* env = ensure()) envns::strikeLightning(*env, distance);
        return api::Value{};
    }, {"800", "un rayo ya (distancia en m; sin ella al azar)"});
    both("setLightning", [ensure](api::Call& c) {
        const bool on = c.boolean(0);
        if (envns::Environment* env = ensure()) {
            env->lightning = on;
            if (c.has(1)) env->lightning_frequency = std::max(static_cast<float>(c.number(1)), 0.0f);
        }
        return api::Value{};
    }, {"true, 2", "rayos en las tormentas y su frecuencia"});

    // --- Astros ---
    both("getSunDirection", [find](api::Call&) -> api::Value {
        const envns::Environment* env = find();
        return env != nullptr ? env->runtime.to_sun : Vec3{0.0f, 1.0f, 0.0f};
    }, {"", "Vec3 hacia el sol", "Vec3"});
    both("isNight", [find](api::Call&) -> api::Value {
        const envns::Environment* env = find();
        return env != nullptr && env->runtime.initialized && env->runtime.to_sun.y < -0.05f;
    }, {"", "el sol esta bajo el horizonte?", "bool"});
    both("setAudio", [ensure](api::Call& c) {
        const bool on = c.boolean(0);
        if (envns::Environment* env = ensure()) {
            env->ambient_audio = on;
            if (c.has(1)) env->audio_volume = std::max(static_cast<float>(c.number(1)), 0.0f);
        }
        return api::Value{};
    }, {"true, 0.8", "sonido de lluvia, viento y truenos"});
}

// --- Voxel ---
// El mundo de bloques. Los bloques se nombran por su id (numero) o por su
// nombre ("stone", "Piedra").
int blockOf(const api::Value& block) {
    if (block.isNumber()) return static_cast<int>(block.asNumber());
    if (block.isString()) return static_cast<int>(voxel::blockId(block.asString()));
    return voxel::block::Air;
}

bool validBlock(int id) { return id >= 0 && id < voxel::block::Count; }

// Coordenadas de bloque: cualquier numero (las de un Vec3 son flotantes), hacia abajo.
int cell(double v) { return static_cast<int>(std::floor(v)); }

Vec3 blockVec(const voxel::BlockPos& p) {
    return Vec3{static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)};
}

// Una estructura como tabla para los scripts (ver ESTRUCTURAS.md).
api::Value structureValue(const voxel::StructureInfo& s, bool with_room) {
    api::Value t = api::Value::object();
    t.set("id", static_cast<double>(s.id));
    t.set("type", s.type);
    t.set("name", s.name);
    t.set("center", s.center);
    t.set("min", s.min);
    t.set("max", s.max);
    t.set("level", s.level);
    t.set("seed", static_cast<double>(s.seed));
    t.set("dimension", s.dimension);
    api::Value::Array points;
    for (const voxel::StructurePoint& p : s.points) {
        api::Value pt = api::Value::object();
        pt.set("kind", p.kind);
        pt.set("pos", p.position);
        pt.set("dir", p.direction);
        pt.set("tag", p.tag);
        points.push_back(std::move(pt));
    }
    t.set("points", api::Value(std::move(points)));
    if (with_room) t.set("room", s.room);
    return t;
}

void registerVoxel(Runtime& rt) {
    api::NativeApi& n = rt.native;
    n.function("Voxel.isActive", [&rt](api::Call&) -> api::Value {
        return rt.voxels != nullptr && rt.voxels->active();
    }, {"", "hay mundo de bloques?", "bool"});
    n.function("Voxel.getBlock", [&rt](api::Call& c) -> api::Value {
        const int x = cell(c.number(0)), y = cell(c.number(1)), z = cell(c.number(2));
        return rt.voxels != nullptr ? static_cast<int>(rt.voxels->getBlock(x, y, z)) : 0;
    }, {"x, y, z", "numero del bloque (0 = aire)", "numero"});
    n.function("Voxel.getBlockAt", [&rt](api::Call& c) -> api::Value {
        const Vec3 p = c.vec3(0);
        return rt.voxels != nullptr ? static_cast<int>(rt.voxels->getBlock(cell(p.x), cell(p.y), cell(p.z))) : 0;
    }, {"Vec3", "bloque en ese punto", "numero"});
    n.function("Voxel.setBlock", [&rt](api::Call& c) -> api::Value {
        const int x = cell(c.number(0)), y = cell(c.number(1)), z = cell(c.number(2));
        const int id = blockOf(c.arg(3));
        return rt.voxels != nullptr && validBlock(id) && rt.voxels->setBlock(x, y, z, static_cast<voxel::BlockId>(id));
    }, {"x, y, z, \"stone\"", "pone o quita un bloque", "bool"});
    // Solo el dibujo (puertas que el juego anima): no cambia el bloque ni se guarda ni va por red.
    n.function("Voxel.setHidden", [&rt](api::Call& c) -> api::Value {
        const int x = cell(c.number(0)), y = cell(c.number(1)), z = cell(c.number(2));
        if (rt.voxels == nullptr) return false;
        rt.voxels->setBlockHidden(x, y, z, c.boolean(3, true));
        return true;
    }, {"x, y, z, true", "oculta (o vuelve a mostrar) un bloque solo en el dibujo", "bool"});
    n.function("Voxel.isHidden", [&rt](api::Call& c) -> api::Value {
        const int x = cell(c.number(0)), y = cell(c.number(1)), z = cell(c.number(2));
        return rt.voxels != nullptr && rt.voxels->isBlockHidden(x, y, z);
    }, {"x, y, z", "esta oculto en el dibujo (setHidden)", "bool"});
    n.function("Voxel.showAllHidden", [&rt](api::Call&) -> api::Value {
        if (rt.voxels == nullptr) return false;
        rt.voxels->showHiddenBlocks();
        return true;
    }, {"", "vuelve a dibujar todos los bloques ocultos con setHidden", "bool"});
    // --- Rieles y senales de arcanita (voxel/Rails.h) ---
    n.function("Voxel.railInfo", [&rt](api::Call& c) -> api::Value {
        const int x = cell(c.number(0)), y = cell(c.number(1)), z = cell(c.number(2));
        int mask = 0, up = -1;
        if (rt.voxels == nullptr || !rt.voxels->railAt(x, y, z, mask, up)) return api::Value{};
        api::Value t = api::Value::object();
        t.set("mask", mask);
        t.set("up", up);
        t.set("id", static_cast<int>(rt.voxels->getBlock(x, y, z)));
        return t;
    }, {"x, y, z", "via en la celda: mask (bit 0 +X, 1 -X, 2 +Z, 3 -Z), up (rampa hacia 0..3 o -1) e id; nil si no hay via",
        "objeto o nil"});
    n.function("Voxel.signal", [&rt](api::Call& c) -> api::Value {
        const int x = cell(c.number(0)), y = cell(c.number(1)), z = cell(c.number(2));
        return rt.voxels != nullptr ? rt.voxels->signalPower(x, y, z) : 0;
    }, {"x, y, z", "senal de arcanita de la celda (0..15)", "numero"});
    n.function("Voxel.setSignalInput", [&rt](api::Call& c) -> api::Value {
        const int x = cell(c.number(0)), y = cell(c.number(1)), z = cell(c.number(2));
        if (rt.voxels == nullptr) return false;
        rt.voxels->setSignalInput(x, y, z, c.boolean(3, true));
        return true;
    }, {"x, y, z, true", "algo encima de una placa de presion o via detectora (refrescar cada 0.1-0.2 s; caduca a los 0.35 s)",
        "bool"});
    n.function("Voxel.findBlocks", [&rt](api::Call& c) {
        api::Value::Array out;
        const int id = blockOf(c.arg(2));
        if (rt.voxels == nullptr || !validBlock(id) || id == 0) return api::Value(std::move(out));
        const int radius = static_cast<int>(c.number(1, 8.0));
        const int max = static_cast<int>(c.number(3, 64.0));
        for (const voxel::BlockPos& p : rt.voxels->findBlocks(c.vec3(0), radius, static_cast<voxel::BlockId>(id), max)) {
            out.push_back(api::Value(blockVec(p)));
        }
        return api::Value(std::move(out));
    }, {"pos, radio, \"rail\", max", "celdas con ese bloque en el cubo (radio <= 48)", "lista de Vec3"});
    // VFX-VIVO: varios tipos en una pasada. Devuelve una lista plana {x, y, z, id, x, y, z, id, ...}.
    n.function("Voxel.findBlocksAny", [&rt](api::Call& c) {
        api::Value::Array out;
        if (rt.voxels == nullptr || !c.arg(2).isArray()) return api::Value(std::move(out));
        std::vector<bool> wanted(voxel::block::Count, false);
        bool any = false;
        for (const api::Value& v : c.arg(2).items()) {
            const int id = blockOf(v);
            if (!validBlock(id) || id == 0) continue;
            wanted[static_cast<std::size_t>(id)] = true;
            any = true;
        }
        if (!any) return api::Value(std::move(out));
        const int radius = static_cast<int>(c.number(1, 8.0));
        const int perId = static_cast<int>(c.number(3, 64.0));
        const int max = static_cast<int>(c.number(4, 2048.0));
        for (const voxel::VoxelSystem::FoundBlock& f : rt.voxels->findBlocksAny(c.vec3(0), radius, wanted, perId, max)) {
            out.push_back(api::Value(static_cast<double>(f.pos.x)));
            out.push_back(api::Value(static_cast<double>(f.pos.y)));
            out.push_back(api::Value(static_cast<double>(f.pos.z)));
            out.push_back(api::Value(static_cast<double>(f.id)));
        }
        return api::Value(std::move(out));
    }, {"pos, radio, {\"torch\", \"lava\", ...}, maxPorTipo, max", "celdas de varios bloques en el cubo (radio <= 48), en una pasada",
        "lista plana {x, y, z, id, ...}"});
    n.function("Voxel.blockId", [](api::Call& c) -> api::Value {
        return static_cast<int>(voxel::blockId(c.string(0)));
    }, {"\"stone\"", "numero de un bloque", "numero"});
    n.function("Voxel.blockCount", [](api::Call&) -> api::Value {
        return static_cast<int>(voxel::block::Count);
    }, {"", "cuantos tipos de bloque hay", "numero"});
    // Datos de un bloque: name, label, solid, placeable, hardness, drop, light.
    n.function("Voxel.blockInfo", [](api::Call& c) {
        const int id = blockOf(c.arg(0));
        if (!validBlock(id)) return api::Value{};
        const voxel::BlockDef& def = voxel::blockDef(static_cast<voxel::BlockId>(id));
        api::Value t = api::Value::object();
        t.set("id", id);
        t.set("name", def.name);
        t.set("label", def.label);
        t.set("solid", def.solid);
        t.set("opaque", def.opaque);
        t.set("placeable", def.placeable);
        t.set("replaceable", def.replaceable);
        t.set("hardness", def.hardness);
        t.set("drop", def.drop != 0 ? static_cast<int>(def.drop) : id);
        t.set("light", static_cast<int>(def.light));
        t.set("top", def.top);    // texturas (Textures/Bloques/<nombre>.png): el temblor al picar las usa
        t.set("side", def.side);
        return t;
    }, {"\"stone\"", "{name, label, solid, hardness, top, side...}", "objeto"});
    n.function("Voxel.blockColor", [&rt](api::Call& c) -> api::Value {
        const int id = blockOf(c.arg(0));
        return rt.voxels != nullptr && validBlock(id) ? rt.voxels->blockColor(static_cast<voxel::BlockId>(id))
                                                      : Vec3{1.0f, 1.0f, 1.0f};
    }, {"\"stone\"", "color medio del bloque (Vec3)", "Vec3"});
    n.function("Voxel.surfaceHeight", [&rt](api::Call& c) -> api::Value {
        const int x = cell(c.number(0)), z = cell(c.number(1));
        return rt.voxels != nullptr ? rt.voxels->surfaceHeight(x, z) : 0;
    }, {"x, z", "altura del terreno", "numero"});
    n.function("Voxel.waterLevel", [&rt](api::Call& c) -> api::Value {
        const int x = cell(c.number(0)), z = cell(c.number(1));
        return rt.voxels != nullptr ? rt.voxels->waterLevel(x, z) : 0;
    }, {"x, z", "primer bloque por encima del agua de la columna (mar o lago; 0 = sin agua)", "numero"});
    // Oceanos hondos: cuanta agua de mar hay sobre el suelo (segun el generador, sin cargar).
    n.function("Voxel.oceanDepth", [&rt](api::Call& c) -> api::Value {
        const int x = cell(c.number(0)), z = cell(c.number(1));
        return rt.voxels != nullptr ? rt.voxels->oceanDepth(x, z) : 0;
    }, {"x, z", "bloques de mar sobre el suelo (0 = tierra, lago o rio; oceano hondo 30..57)", "numero"});
    n.function("Voxel.isDeepOcean", [&rt](api::Call& c) -> api::Value {
        const int x = cell(c.number(0)), z = cell(c.number(1));
        const int min_depth = c.has(2) ? static_cast<int>(c.integer(2)) : 24;
        return rt.voxels != nullptr && rt.voxels->oceanDepth(x, z) >= min_depth;
    }, {"x, z, minimo = 24", "hay al menos `minimo` bloques de mar sobre el suelo?", "bool"});
    // El punto de oceano hondo mas cercano (rejilla de 16 m hasta `radio`): para soltar
    // criaturas marinas. {pos = Vec3 a media agua, surface, floor, depth} o nil.
    n.function("Voxel.findDeepOcean", [&rt](api::Call& c) -> api::Value {
        const Vec3 from = c.vec3(0);
        const int radius = std::clamp(static_cast<int>(c.number(1, 512.0)), 16, 4096);
        const int min_depth = c.has(2) ? static_cast<int>(c.integer(2)) : 24;
        if (rt.voxels == nullptr) return api::Value{};
        constexpr int kStep = 16;
        const int cx = cell(from.x), cz = cell(from.z);
        for (int ring = 0; ring * kStep <= radius; ++ring) {
            int best = -1, bx = 0, bz = 0;
            float best_d = 1e30f;
            for (int dz = -ring; dz <= ring; ++dz) {
                for (int dx = -ring; dx <= ring; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dz)) != ring) continue;  // solo el borde del anillo
                    const int x = cx + dx * kStep, z = cz + dz * kStep;
                    const int depth = rt.voxels->oceanDepth(x, z);
                    if (depth < min_depth) continue;
                    const float d = static_cast<float>(dx * dx + dz * dz);
                    if (d < best_d) best_d = d, best = depth, bx = x, bz = z;
                }
            }
            if (best < 0) continue;
            const int surface = rt.voxels->waterLevel(bx, bz);  // primer bloque por encima del agua
            const int floor_y = surface - 1 - best;
            api::Value t = api::Value::object();
            t.set("pos", Vec3{static_cast<float>(bx) + 0.5f, static_cast<float>(floor_y + 1 + best / 2), static_cast<float>(bz) + 0.5f});
            t.set("surface", surface);
            t.set("floor", floor_y);
            t.set("depth", best);
            return t;
        }
        return api::Value{};
    }, {"Vec3, radio = 512, minimo = 24", "nil o {pos (a media agua), surface, floor, depth}", "objeto o nil"});
    n.function("Voxel.isWater", [](api::Call& c) -> api::Value {
        const int id = blockOf(c.arg(0));
        return validBlock(id) && voxel::block::isWater(static_cast<voxel::BlockId>(id));
    }, {"id", "es agua (fuente o corriente)?", "bool"});
    n.function("Voxel.isReady", [&rt](api::Call& c) -> api::Value {
        const Vec3 p = c.vec3(0);
        return rt.voxels != nullptr && rt.voxels->isReady(cell(p.x), cell(p.z));
    }, {"Vec3", "ya esta generado?", "bool"});
    n.function("Voxel.farStats", [&rt](api::Call&) -> api::Value {
        if (rt.voxels == nullptr) return api::Value{};
        const voxel::VoxelStats st = rt.voxels->stats();
        api::Value t = api::Value::object();
        t.set("tiles", st.far_tiles);
        t.set("ready", st.far_ready);
        t.set("pending", st.far_pending);
        t.set("quads", static_cast<double>(st.far_quads));
        t.set("last_tile_ms", st.far_last_tile_ms);
        t.set("from_cache", st.far_from_cache);
        t.set("generated", st.far_generated);
        t.set("hole", st.far_hole);
        return t;
    }, {"", "terreno lejano (CVar voxel.far.Distance): teselas, listas, pendientes, caras, radio de los chunks de verdad", "tabla"});
    n.function("Voxel.inWater", [&rt](api::Call& c) -> api::Value {
        const Vec3 p = c.vec3(0);
        return rt.voxels != nullptr && rt.voxels->inWater(p);
    }, {"Vec3", "esta en el agua?", "bool"});
    n.function("Voxel.skyLight", [&rt](api::Call& c) -> api::Value {
        const int x = cell(c.number(0)), y = cell(c.number(1)), z = cell(c.number(2));
        return rt.voxels != nullptr ? rt.voxels->skyLight(x, y, z) : 15;
    }, {"x, y, z", "luz del cielo 0..15", "numero"});
    n.function("Voxel.blockLight", [&rt](api::Call& c) -> api::Value {
        const int x = cell(c.number(0)), y = cell(c.number(1)), z = cell(c.number(2));
        return rt.voxels != nullptr ? rt.voxels->blockLight(x, y, z) : 0;
    }, {"x, y, z", "luz de antorchas 0..15", "numero"});
    // El primer bloque que toca: {block = Vec3, normal = Vec3, id, point, distance}.
    n.function("Voxel.raycast", [&rt](api::Call& c) {
        const Vec3 origin = c.vec3(0);
        const Vec3 direction = c.vec3(1);
        const float distance = static_cast<float>(c.number(2, 8.0));
        if (rt.voxels == nullptr) return api::Value{};
        const voxel::VoxelHit hit = rt.voxels->raycast(origin, direction, distance);
        if (!hit.hit) return api::Value{};
        api::Value t = api::Value::object();
        t.set("block", blockVec(hit.block));
        t.set("normal", blockVec(hit.normal));
        t.set("id", static_cast<int>(hit.id));
        t.set("point", hit.point);
        t.set("distance", hit.distance);
        // COLISION-MODELOS: la caja de lo tocado (celda entera, o las piezas de una valla, silla...).
        t.set("boxMin", hit.box_min);
        t.set("boxMax", hit.box_max);
        return t;
    }, {"origen, direccion, distancia", "nil o {block, normal, id, point, distance, boxMin, boxMax}", "objeto o nil"});
    // Mueve una caja (centro, semiejes) contra los bloques. Cuatro resultados
    // (posicion, enSuelo, techo, pared): una lista.
    n.function("Voxel.moveBox", [&rt](api::Call& c) {
        const Vec3 center = c.vec3(0);
        const Vec3 half = c.vec3(1);
        const Vec3 delta = c.vec3(2);
        if (rt.voxels == nullptr) return api::Value(api::Value::Array{center + delta, false, false, false});
        const voxel::VoxelSystem::MoveResult r = rt.voxels->moveBox(center, half, delta);
        // Lo que anda por el mundo de bloques aparta las plantas por donde
        // pasa (los pies de la caja; las cajas enormes no son personajes).
        if (half.y > 0.05f && half.y < 4.0f && std::max(half.x, half.z) < 3.0f) {
            rt.voxels->noteFoliageInteractor(r.position - Vec3{0.0f, half.y, 0.0f}, std::max(half.x, half.z));
        }
        return api::Value(api::Value::Array{r.position, r.on_ground, r.hit_ceiling, r.hit_wall});
    }, {"centro, semiejes, delta", "posicion, enSuelo, techo, pared", "lista {Vec3, bool, bool, bool}"});
    // Algo que aparta plantas y hojas a ras de suelo este frame (pies, radio):
    // lo que no se mueve con Voxel.moveBox (copias de red, animaciones...).
    // Hay que llamarla cada frame mientras este (dura 0.25 s).
    n.function("Voxel.pushFoliage", [&rt](api::Call& c) -> api::Value {
        if (rt.voxels == nullptr) return false;
        const float radius = static_cast<float>(c.number(1, 0.4));
        rt.voxels->noteFoliageInteractor(c.vec3(0), radius);
        return true;
    }, {"pies, radio", "aparta las plantas alrededor este frame", "bool"});
    n.function("Voxel.boxCollides", [&rt](api::Call& c) -> api::Value {
        const Vec3 center = c.vec3(0);
        const Vec3 half = c.vec3(1);
        return rt.voxels != nullptr && rt.voxels->boxCollides(center, half);
    }, {"centro, semiejes", "toca bloques?", "bool"});

    // Mundos guardados (partidas).
    n.function("Voxel.newWorld", [&rt](api::Call& c) -> api::Value {
        const std::string name = c.string(0);
        const int seed = c.has(1) ? static_cast<int>(c.integer(1)) : static_cast<int>(std::rand());
        return rt.voxels != nullptr && rt.voxels->newWorld(name, seed);
    }, {"\"nombre\", semilla", "mundo nuevo con nombre", "bool"});
    n.function("Voxel.loadWorld", [&rt](api::Call& c) -> api::Value {
        const std::string name = c.string(0);
        return rt.voxels != nullptr && rt.voxels->loadWorld(name);
    }, {"\"nombre\"", "carga un mundo guardado", "bool"});
    n.function("Voxel.saveWorld", [&rt](api::Call&) -> api::Value {
        return rt.voxels != nullptr && rt.voxels->saveWorld();
    }, {"", "guarda", "bool"});
    n.function("Voxel.deleteWorld", [&rt](api::Call& c) -> api::Value {
        const std::string name = c.string(0);
        return rt.voxels != nullptr && rt.voxels->deleteWorld(name);
    }, {"\"nombre\"", "lo borra", "bool"});
    n.function("Voxel.listWorlds", [&rt](api::Call&) {
        api::Value::Array out;
        if (rt.voxels == nullptr) return api::Value(std::move(out));
        for (const voxel::WorldInfo& w : rt.voxels->listWorlds()) {
            api::Value t = api::Value::object();
            t.set("name", w.name);
            t.set("seed", w.seed);
            t.set("lastPlayed", static_cast<double>(w.last_played));
            t.set("mode", w.mode);
            out.push_back(std::move(t));
        }
        return api::Value(std::move(out));
    }, {"", "lista de mundos", "lista de {name, seed, lastPlayed, mode}"});
    n.function("Voxel.worldName", [&rt](api::Call&) -> api::Value {
        return rt.voxels != nullptr ? rt.voxels->worldName() : std::string();
    }, {"", "nombre del mundo", "texto"});
    n.function("Voxel.seed", [&rt](api::Call&) -> api::Value {
        return rt.voxels != nullptr ? rt.voxels->seed() : 0;
    }, {"", "semilla", "numero"});
    n.function("Voxel.setMeta", [&rt](api::Call& c) {
        const std::string key = c.string(0);
        const std::string value = c.string(1);
        if (rt.voxels != nullptr) rt.voxels->setMeta(key, value);
        return api::Value{};
    }, {"\"clave\", \"texto\"", "dato guardado con el mundo"});
    n.function("Voxel.getMeta", [&rt](api::Call& c) -> api::Value {
        const std::string key = c.string(0);
        const std::string fallback = c.string(1, "");
        return rt.voxels != nullptr ? rt.voxels->meta(key, fallback) : fallback;
    }, {"\"clave\", \"\"", "lee un dato", "texto"});
    // PAQUETES-TEXTURAS: paquetes de texturas de bloques del jugador (solo visuales; no pasan por la red).
    n.function("Voxel.texturePacks", [](api::Call&) {
        api::Value::Array out;
        for (const voxel::TexturePackInfo& p : voxel::listTexturePacks()) {
            api::Value t = api::Value::object();
            const auto u8 = [](const std::filesystem::path& f) {
                const std::u8string s = f.u8string();
                return std::string(s.begin(), s.end());
            };
            t.set("id", p.id);
            t.set("name", p.name);
            t.set("author", p.author);
            t.set("version", p.version);
            t.set("description", p.description);
            t.set("folder", u8(p.folder));
            t.set("icon", u8(p.icon));
            t.set("textures", p.textures);
            out.push_back(std::move(t));
        }
        return api::Value(std::move(out));
    }, {"", "paquetes de texturas encontrados", "lista de {id, name, author, version, description, folder, icon, textures}"});
    n.function("Voxel.texturePacksFolder", [](api::Call&) -> api::Value {
        const auto roots = voxel::texturePackRoots();
        if (roots.empty()) return std::string();
        const std::u8string s = roots.front().u8string();
        return std::string(s.begin(), s.end());
    }, {"", "carpeta donde el jugador pone sus paquetes de texturas", "texto"});
    n.function("Voxel.setTexturePack", [](api::Call& c) -> api::Value {
        return voxel::setTexturePack(c.string(0, ""));
    }, {"\"id\"", "usa ese paquete de texturas (\"\" = las del juego); se aplica sin reiniciar", "bool"});
    n.function("Voxel.texturePack", [](api::Call&) -> api::Value { return voxel::texturePack(); },
               {"", "id del paquete de texturas en uso (\"\" = ninguno)", "texto"});
    n.function("Voxel.reloadTextures", [](api::Call&) -> api::Value {
        voxel::reloadTextures();
        return true;
    }, {"", "vuelve a leer las texturas de bloques del disco", "bool"});
    // --- Estructuras (aldeas, ciudades, mazmorras, ruinas, torres, portales) ---
    n.function("Voxel.structuresNear", [&rt](api::Call& c) {
        api::Value::Array out;
        if (rt.voxels == nullptr) return api::Value(std::move(out));
        for (const voxel::StructureInfo& s : rt.voxels->structuresNear(c.vec3(0), static_cast<float>(c.number(1, 64.0)))) {
            out.push_back(structureValue(s, false));
        }
        return api::Value(std::move(out));
    }, {"pos, radio", "estructuras cuya caja toca la esfera",
        "lista de {id, type, name, center, min, max, level, seed, dimension, points = {{kind, pos, dir, tag}}}"});
    n.function("Voxel.structureAt", [&rt](api::Call& c) {
        voxel::StructureInfo s;
        if (rt.voxels == nullptr || !rt.voxels->structureAt(c.vec3(0), s)) return api::Value{};
        return structureValue(s, true);
    }, {"pos", "la estructura que contiene el punto (room = sala de la mazmorra o -1)", "objeto o nil"});
    // CIUDADES-GRANDES: cofres de botin que puso el generador (bloque chest).
    n.function("Voxel.structureChests", [&rt](api::Call& c) {
        api::Value::Array out;
        if (rt.voxels == nullptr) return api::Value(std::move(out));
        for (const voxel::GeneratedChest& g : rt.voxels->structureChests(c.vec3(0), c.vec3(1))) {
            api::Value t = api::Value::object();
            t.set("x", g.x);
            t.set("y", g.y);
            t.set("z", g.z);
            t.set("pos", g.position);
            t.set("tier", g.tier);
            t.set("table", g.table);
            t.set("structure", static_cast<double>(g.structure));
            t.set("dimension", g.dimension);
            t.set("key", g.key);
            out.push_back(std::move(t));
        }
        return api::Value(std::move(out));
    }, {"min, max", "cofres de botin generados en la caja (celda absoluta x y z, pos local, tier 1..4, table, structure, dimension, key)",
        "lista de {x, y, z, pos, tier, table, structure, dimension, key}"});
    // --- Dimensiones (superficie, abismo, cielo, cristal) ---
    n.function("Voxel.dimension", [&rt](api::Call&) -> api::Value {
        return rt.voxels != nullptr && rt.voxels->active() ? rt.voxels->dimension() : std::string("superficie");
    }, {"", "dimension actual (\"superficie\", \"abismo\", \"cielo\", \"cristal\")", "texto"});
    n.function("Voxel.dimensions", [](api::Call&) {
        api::Value::Array out;
        for (const voxel::DimensionInfo& d : voxel::dimensions()) {
            api::Value t = api::Value::object();
            t.set("name", d.name);
            t.set("label", d.label);
            t.set("generator", d.generator);
            t.set("sky", d.sky);
            out.push_back(std::move(t));
        }
        return api::Value(std::move(out));
    }, {"", "dimensiones que hay", "lista de {name, label, generator, sky}"});
    n.function("Voxel.setDimension", [&rt](api::Call& c) -> api::Value {
        const std::string name = c.string(0);
        const bool ambience = c.boolean(1, true);
        return rt.voxels != nullptr && rt.voxels->setDimension(name, ambience);
    }, {"\"abismo\", ambiente?", "cambia de dimension (guarda la actual; ambiente = el motor pone su cielo)", "bool"});
    n.function("Voxel.spawnPoint", [&rt](api::Call& c) -> api::Value {
        if (rt.voxels == nullptr || !rt.voxels->active()) return c.has(0) ? c.vec3(0) : Vec3{};
        return c.has(0) ? rt.voxels->spawnPoint(c.vec3(0)) : rt.voxels->spawnPoint();
    }, {"cerca?", "sitio seguro para los pies en la dimension actual (cerca de un punto o de quien mira)", "Vec3"});
}

// --- Fluid ---
// Liquidos por particulas (fluid/Fluid.h). Los tipos se nombran por clave
// ("water", "honey", "lava"...), por nombre ("Miel") o numero.
constexpr int kFluidTypes = static_cast<int>(fluid::FluidType::Count);

// -1 si no existe; nil = agua.
int fluidTypeOf(const api::Value& value) {
    if (value.isNumber()) return std::clamp(static_cast<int>(value.asNumber()), 0, kFluidTypes - 1);
    if (value.isString()) return fluid::fluidTypeFromName(value.asString());
    return 0;
}

fluid::FluidEmitter* emitterOf(const ecs::Entity& e) { return e.valid() ? e.tryGet<fluid::FluidEmitter>() : nullptr; }

void registerFluid(Runtime& rt) {
    api::NativeApi& n = rt.native;
    n.function("Fluid.isActive", [](api::Call&) -> api::Value {
        const fluid::FluidSystem* f = fluid::activeSystem();
        return f != nullptr && f->stats().active;
    }, {"", "hay liquidos en la escena?", "bool"});
    n.function("Fluid.spawn", [&rt](api::Call& c) {
        const Vec3 position = c.vec3(0);
        const int count = static_cast<int>(c.integer(1));
        const Vec3 velocity = c.vec3(3, Vec3{});
        const float radius = static_cast<float>(c.number(4, 0.0));
        const float lifetime = static_cast<float>(c.number(5, 0.0));
        fluid::FluidSystem* f = fluid::activeSystem();
        if (f == nullptr) return api::Value{};
        int t = fluidTypeOf(c.arg(2));
        if (t < 0) {
            rt.write(1, "Fluid.spawn: liquido desconocido (water, oil, honey, lava, mud, blood, acid, custom)");
            t = 0;
        }
        f->spawn(position, count, static_cast<fluid::FluidType>(t), velocity, radius, lifetime);
        return api::Value{};
    }, {"pos, cantidad, \"water\", vel, radio, vida", "crea liquido (bola de particulas)"});
    n.function("Fluid.clear", [](api::Call&) {
        if (fluid::FluidSystem* f = fluid::activeSystem()) f->clear();
        return api::Value{};
    }, {"", "borra todo el liquido"});
    n.function("Fluid.count", [](api::Call& c) -> api::Value {
        const fluid::FluidSystem* f = fluid::activeSystem();
        if (f == nullptr) return 0;
        if (!c.has(0)) return static_cast<int>(f->particleCount());
        const int t = fluidTypeOf(c.arg(0));
        return t < 0 ? 0 : static_cast<int>(f->particleCount(static_cast<fluid::FluidType>(t)));
    }, {"\"honey\"", "particulas (todas o de un tipo)", "numero"});
    n.function("Fluid.density", [](api::Call& c) -> api::Value {
        const Vec3 position = c.vec3(0);
        const float radius = static_cast<float>(c.number(1, 0.0));
        const fluid::FluidSystem* f = fluid::activeSystem();
        return f != nullptr ? f->densityAt(position, radius) : 0.0f;
    }, {"pos, radio", "0 = seco, ~1 = lleno", "numero"});
    n.function("Fluid.isInside", [](api::Call& c) -> api::Value {
        const Vec3 position = c.vec3(0);
        const fluid::FluidSystem* f = fluid::activeSystem();
        return f != nullptr && f->densityAt(position) > 0.25f;
    }, {"pos", "hay liquido ahi?", "bool"});
    n.function("Fluid.velocity", [](api::Call& c) -> api::Value {
        const Vec3 position = c.vec3(0);
        const float radius = static_cast<float>(c.number(1, 0.0));
        const fluid::FluidSystem* f = fluid::activeSystem();
        return f != nullptr ? f->velocityAt(position, radius) : Vec3{};
    }, {"pos, radio", "velocidad media del liquido (Vec3)", "Vec3"});
    n.function("Fluid.surfaceHeight", [](api::Call& c) -> api::Value {
        const float x = static_cast<float>(c.number(0));
        const float z = static_cast<float>(c.number(1));
        const float radius = static_cast<float>(c.number(2, 0.0));
        const fluid::FluidSystem* f = fluid::activeSystem();
        float height = 0.0f;
        if (f == nullptr || !f->surfaceHeight(x, z, height, radius)) return {};
        return height;
    }, {"x, z", "nil o la altura de la superficie", "numero o nil"});
    n.function("Fluid.start", [&rt](api::Call& c) {
        if (fluid::FluidEmitter* em = emitterOf(rt.entityArg(c, 0))) em->emitting = true;
        return api::Value{};
    }, {"entidad", "el emisor empieza"});
    n.function("Fluid.stop", [&rt](api::Call& c) {
        if (fluid::FluidEmitter* em = emitterOf(rt.entityArg(c, 0))) em->emitting = false;
        return api::Value{};
    }, {"entidad", "el emisor para"});
    n.function("Fluid.restart", [&rt](api::Call& c) {
        const ecs::Entity e = rt.entityArg(c, 0);
        fluid::FluidSystem* f = fluid::activeSystem();
        if (fluid::FluidEmitter* em = emitterOf(e); em != nullptr && f != nullptr) {
            em->emitting = true;
            f->restart(e);
        }
        return api::Value{};
    }, {"entidad", "vuelve a llenar una caja/esfera"});
    n.function("Fluid.setType", [&rt](api::Call& c) {
        fluid::FluidEmitter* em = emitterOf(rt.entityArg(c, 0));
        const int t = fluidTypeOf(c.arg(1));
        if (em == nullptr) return api::Value{};
        if (t < 0) {
            rt.write(1, "Fluid.setType: liquido desconocido");
            return api::Value{};
        }
        em->fluid = static_cast<fluid::FluidType>(t);
        return api::Value{};
    }, {"entidad, \"lava\"", "cambia el liquido del emisor"});
    n.function("Fluid.types", [](api::Call&) {
        api::Value::Array out;
        for (int i = 0; i < kFluidTypes; ++i) {
            out.emplace_back(std::string(fluid::fluidTypeKeys()[static_cast<std::size_t>(i)]));
        }
        return api::Value(std::move(out));
    }, {"", "lista de tipos", "lista de texto"});
    n.function("Fluid.stats", [](api::Call&) {
        const fluid::FluidSystem* f = fluid::activeSystem();
        const fluid::FluidSystemStats s = f != nullptr ? f->stats() : fluid::FluidSystemStats{};
        api::Value out = api::Value::object();
        out.set("particles", s.particles);
        out.set("capacity", s.capacity);
        out.set("substeps", s.substeps);
        out.set("colliders", s.shapes);
        out.set("emitters", s.emitters);
        out.set("floatingBodies", s.floating_bodies);
        out.set("memoryMB", static_cast<double>(s.memory_bytes) / (1024.0 * 1024.0));
        out.set("simulating", s.simulating);
        return out;
    }, {"", "{particles, capacity, emitters...}", "objeto"});
}

// --- Sky ---
// Color del cielo del componente Sky (el primero activo de la escena, el
// que se dibuja): degradado de cenit/horizonte/suelo, paradas por altura y
// tinte del sol. Los colores son Vec3 en sRGB 0..1. Lo demas del Sky
// (nubes, hora) se cambia con Entity.setField("Sky", ...) o con Weather.
void registerSky(Runtime& rt) {
    api::NativeApi& n = rt.native;
    const auto find = [&rt]() -> ecs::Sky* {
        if (rt.world == nullptr) return nullptr;
        ecs::Sky* found = nullptr;
        ecs::Sky* inactive = nullptr;
        rt.world->forEachDepthFirst([&](ecs::Entity e) {
            if (found != nullptr) return;
            ecs::Sky* sky = e.tryGet<ecs::Sky>();
            if (sky == nullptr) return;
            if (e.activeInHierarchy()) found = sky;
            else if (inactive == nullptr) inactive = sky;
        });
        return found != nullptr ? found : inactive;
    };
    // Sin Sky en la escena: uno nuevo (cielo fisico, sin HDR), como Weather.
    const auto ensure = [&rt, find]() -> ecs::Sky* {
        if (ecs::Sky* sky = find()) return sky;
        if (rt.world == nullptr) return nullptr;
        ecs::Entity holder = rt.world->create("Ambiente");
        ecs::Sky& sky = holder.add<ecs::Sky>();
        sky.use_hdr = false;
        return &sky;
    };
    const auto parse_mode = [](const api::Value& v, ecs::SkyColorMode& out) {
        if (v.isNumber()) {
            const int i = static_cast<int>(v.asNumber());
            if (i < 0 || i > 2) return false;
            out = static_cast<ecs::SkyColorMode>(i);
            return true;
        }
        std::string name = v.asString();
        for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (name == "physical" || name == "fisico") {
            out = ecs::SkyColorMode::Physical;
        } else if (name == "gradient" || name == "degradado") {
            out = ecs::SkyColorMode::Gradient;
        } else if (name == "gradientstops" || name == "stops" || name == "paradas" || name == "degradado por paradas") {
            out = ecs::SkyColorMode::GradientStops;
        } else {
            return false;
        }
        return true;
    };
    static constexpr std::array<const char*, 3> kModeNames = {"Physical", "Gradient", "GradientStops"};

    n.function("Sky.setColorMode", [&rt, ensure, parse_mode](api::Call& c) {
        ecs::SkyColorMode mode{};
        if (!parse_mode(c.arg(0), mode)) {
            rt.write(1, "Sky.setColorMode: modo desconocido '" + c.arg(0).asString() +
                            "' (Physical, Gradient, GradientStops)");
            return api::Value{};
        }
        if (ecs::Sky* sky = ensure()) sky->color_mode = mode;
        return api::Value{};
    }, {"\"Gradient\"", "color del cielo: Physical (fisico), Gradient (3 colores) o GradientStops (paradas)"});
    n.function("Sky.getColorMode", [find](api::Call&) -> api::Value {
        const ecs::Sky* sky = find();
        const auto mode = sky != nullptr ? sky->color_mode : ecs::SkyColorMode::Physical;
        return std::string(kModeNames[static_cast<std::size_t>(mode)]);
    }, {"", "Physical, Gradient o GradientStops", "texto"});
    n.function("Sky.setColors", [ensure](api::Call& c) {
        if (ecs::Sky* sky = ensure()) {
            sky->zenith_color = c.vec3(0, sky->zenith_color);
            sky->horizon_color = c.vec3(1, sky->horizon_color);
            sky->ground_color = c.vec3(2, sky->ground_color);
            if (sky->color_mode == ecs::SkyColorMode::Physical) sky->color_mode = ecs::SkyColorMode::Gradient;
        }
        return api::Value{};
    }, {"cenit, horizonte, suelo", "colores del degradado (Vec3 sRGB 0..1; nil = el de antes). Pasa a Gradient si era Physical"});
    n.function("Sky.getColors", [find](api::Call&) -> api::Value {
        const ecs::Sky* sky = find();
        const ecs::Sky fallback{};
        const ecs::Sky& s = sky != nullptr ? *sky : fallback;
        return api::Value(api::Value::Array{s.zenith_color, s.horizon_color, s.ground_color});
    }, {"", "{cenit, horizonte, suelo}", "lista de Vec3"});
    n.function("Sky.setSunsetColors", [ensure](api::Call& c) {
        if (ecs::Sky* sky = ensure()) {
            sky->sunset_zenith_color = c.vec3(0, sky->sunset_zenith_color);
            sky->sunset_horizon_color = c.vec3(1, sky->sunset_horizon_color);
            sky->sunset_ground_color = c.vec3(2, sky->sunset_ground_color);
            sky->time_colors = true;
        }
        return api::Value{};
    }, {"cenit, horizonte, suelo", "colores del atardecer (activa los colores por hora)"});
    n.function("Sky.setNightColors", [ensure](api::Call& c) {
        if (ecs::Sky* sky = ensure()) {
            sky->night_zenith_color = c.vec3(0, sky->night_zenith_color);
            sky->night_horizon_color = c.vec3(1, sky->night_horizon_color);
            sky->night_ground_color = c.vec3(2, sky->night_ground_color);
            sky->time_colors = true;
        }
        return api::Value{};
    }, {"cenit, horizonte, suelo", "colores de la noche (activa los colores por hora)"});
    n.function("Sky.setTimeColors", [ensure](api::Call& c) {
        if (ecs::Sky* sky = ensure()) sky->time_colors = c.boolean(0, true);
        return api::Value{};
    }, {"true", "mezclar dia, atardecer y noche segun la altura del sol"});
    // Lista de paradas: {altura, color} o {elevation = altura, color = color}.
    n.function("Sky.setGradient", [&rt, ensure](api::Call& c) {
        const api::Value& list = c.list(0);
        std::vector<ecs::SkyGradientStop> stops;
        for (std::size_t i = 0; i < list.size(); ++i) {
            const api::Value& item = list[i];
            if (stops.size() >= ecs::kMaxSkyGradientStops) break;
            ecs::SkyGradientStop stop;
            if (item.isObject()) {
                stop.elevation = static_cast<float>(item["elevation"].asNumber(0.0));
                if (item["color"].isVec3()) stop.color = item["color"].asVec3();
            } else if (item.isArray() && item.size() >= 2) {
                stop.elevation = static_cast<float>(item[0].asNumber(0.0));
                if (item[1].isVec3()) stop.color = item[1].asVec3();
            } else {
                rt.write(1, "Sky.setGradient: cada parada es {altura, color}");
                return api::Value{};
            }
            stop.elevation = std::clamp(stop.elevation, -90.0f, 90.0f);
            stops.push_back(stop);
        }
        if (ecs::Sky* sky = ensure()) {
            sky->gradient = std::move(stops);
            sky->color_mode = ecs::SkyColorMode::GradientStops;
        }
        return api::Value{};
    }, {"{{-90, suelo}, {0, horizonte}, {90, cenit}}", "degradado por paradas (altura en grados, hasta 8); pasa a GradientStops"});
    n.function("Sky.setCurve", [ensure](api::Call& c) {
        if (ecs::Sky* sky = ensure()) sky->gradient_curve = std::clamp(static_cast<float>(c.number(0)), 0.1f, 16.0f);
        return api::Value{};
    }, {"3", "curva del degradado (1 = lineal, mas = horizonte mas fino)"});
    n.function("Sky.setIntensity", [ensure](api::Call& c) {
        if (ecs::Sky* sky = ensure()) sky->sky_intensity = std::max(static_cast<float>(c.number(0)), 0.0f);
        return api::Value{};
    }, {"1", "brillo de los colores del cielo"});
    n.function("Sky.setBlend", [ensure](api::Call& c) {
        if (ecs::Sky* sky = ensure()) sky->color_blend = std::clamp(static_cast<float>(c.number(0)), 0.0f, 1.0f);
        return api::Value{};
    }, {"0.5", "0 = cielo fisico .. 1 = solo los colores"});
    n.function("Sky.setSunTint", [ensure](api::Call& c) {
        if (ecs::Sky* sky = ensure()) sky->sun_tint = c.vec3(0, Vec3{1.0f, 1.0f, 1.0f});
        return api::Value{};
    }, {"Vec3(1, 0.8, 0.6)", "tinte del disco solar y su halo (blanco = sin cambio)"});
    n.function("Sky.setSunHalo", [ensure](api::Call& c) {
        if (ecs::Sky* sky = ensure()) sky->sun_halo = std::max(static_cast<float>(c.number(0)), 0.0f);
        return api::Value{};
    }, {"1", "halo del sol sobre el degradado (0 = sin halo)"});
}

}  // namespace

void registerWorldApi(Runtime& rt) {
    registerFire(rt);
    registerWeather(rt);
    registerSky(rt);
    registerVoxel(rt);
    registerFluid(rt);
}

}  // namespace cramion::scripting::native
