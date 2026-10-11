// Fire, Weather / Environment, Voxel y Fluid: el mundo de la escena (fuego,
// clima, bloques y liquidos).

#include "Modules.h"

#include "CramionCore/environment/Environment.h"
#include "CramionCore/fire/Fire.h"
#include "CramionCore/fluid/Fluid.h"
#include "CramionCore/voxel/Voxel.h"

#include <algorithm>
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
        t.set("placeable", def.placeable);
        t.set("replaceable", def.replaceable);
        t.set("hardness", def.hardness);
        t.set("drop", def.drop != 0 ? static_cast<int>(def.drop) : id);
        t.set("light", static_cast<int>(def.light));
        return t;
    }, {"\"stone\"", "{name, label, solid, hardness...}", "objeto"});
    n.function("Voxel.blockColor", [&rt](api::Call& c) -> api::Value {
        const int id = blockOf(c.arg(0));
        return rt.voxels != nullptr && validBlock(id) ? rt.voxels->blockColor(static_cast<voxel::BlockId>(id))
                                                      : Vec3{1.0f, 1.0f, 1.0f};
    }, {"\"stone\"", "color medio del bloque (Vec3)", "Vec3"});
    n.function("Voxel.surfaceHeight", [&rt](api::Call& c) -> api::Value {
        const int x = cell(c.number(0)), z = cell(c.number(1));
        return rt.voxels != nullptr ? rt.voxels->surfaceHeight(x, z) : 0;
    }, {"x, z", "altura del terreno", "numero"});
    n.function("Voxel.isReady", [&rt](api::Call& c) -> api::Value {
        const Vec3 p = c.vec3(0);
        return rt.voxels != nullptr && rt.voxels->isReady(cell(p.x), cell(p.z));
    }, {"Vec3", "ya esta generado?", "bool"});
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
        return t;
    }, {"origen, direccion, distancia", "nil o {block, normal, id, point, distance}", "objeto o nil"});
    // Mueve una caja (centro, semiejes) contra los bloques. Cuatro resultados
    // (posicion, enSuelo, techo, pared): una lista.
    n.function("Voxel.moveBox", [&rt](api::Call& c) {
        const Vec3 center = c.vec3(0);
        const Vec3 half = c.vec3(1);
        const Vec3 delta = c.vec3(2);
        if (rt.voxels == nullptr) return api::Value(api::Value::Array{center + delta, false, false, false});
        const voxel::VoxelSystem::MoveResult r = rt.voxels->moveBox(center, half, delta);
        return api::Value(api::Value::Array{r.position, r.on_ground, r.hit_ceiling, r.hit_wall});
    }, {"centro, semiejes, delta", "posicion, enSuelo, techo, pared", "lista {Vec3, bool, bool, bool}"});
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

}  // namespace

void registerWorldApi(Runtime& rt) {
    registerFire(rt);
    registerWeather(rt);
    registerVoxel(rt);
    registerFluid(rt);
}

}  // namespace cramion::scripting::native
