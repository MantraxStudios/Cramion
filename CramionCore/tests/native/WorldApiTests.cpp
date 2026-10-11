// Pruebas de native/WorldApi.cpp: Fire, Weather / Environment, Voxel y Fluid.

#include "ApiTest.h"

#include "CramionCore/environment/Environment.h"
#include "CramionCore/fire/Fire.h"
#include "CramionCore/fluid/Fluid.h"
#include "CramionCore/voxel/Voxel.h"

#include <cmath>
#include <string>

using namespace cramion;
using namespace cramion::apitest;
using core::Vec3;

namespace {

bool near(double a, double b) { return std::fabs(a - b) < 1e-4; }

bool throws(ApiFixture& t, const std::string& key, const Value::Array& args = {}) {
    try {
        t.call(key, args);
    } catch (const scripting::api::Error&) {
        return true;
    }
    return false;
}

void testFire() {
    std::printf("Fire\n");
    ApiFixture t;
    check(t.call("Fire.ignite", {Value(Vec3{0, 0, 0})}).asNumber() == 0, "Fire.ignite sin zonas: 0");
    check(!t.call("Fire.isActive").truthy(), "Fire.isActive sin zonas: false");
    const Value stats = t.call("Fire.stats");
    check(stats.isObject() && stats["burningCells"].asNumber() == 0 && !stats["active"].truthy() &&
              stats["windZ"].isNumber(),
          "Fire.stats es un objeto con sus campos");
    check(throws(t, "Fire.ignite"), "Fire.ignite sin posicion es un error");

    ecs::Entity zone = t.world.create("Fuego");
    fire::Fire& f = zone.add<fire::Fire>();
    f.ignite_on_start = false;
    f.size = 20.0f;
    check(t.call("Fire.ignite", {Value(Vec3{1, 0, 1}), Value(1.0)}).asNumber() == 1, "Fire.ignite dentro de la zona: 1");
    check(t.call("Fire.ignite", {Value(Vec3{500, 0, 500})}).asNumber() == 0, "Fire.ignite fuera de la zona: 0");
    check(t.call("Fire.extinguish", {Value(Vec3{0, 0, 0})}).asNumber() == 1, "Fire.extinguish en la zona");
    check(near(t.call("Fire.heatAt", {Value(Vec3{0, 0, 0})}).asNumber(), 0.0), "Fire.heatAt sin simular: 0");
    check(!t.call("Fire.isBurning", {Value(Vec3{0, 0, 0})}).truthy(), "Fire.isBurning sin simular: false");
    check(t.call("Fire.burnedAt", {Value(Vec3{0, 0, 0})}).asNumber() ==
              t.call("Fire.charAt", {Value(Vec3{0, 0, 0})}).asNumber(),
          "Fire.charAt es lo mismo que burnedAt");
    t.call("Fire.extinguishAll");
    t.call("Fire.reset");
    check(near(t.call("Fire.burnedFraction").asNumber(), 0.0) && near(t.call("Fire.burningArea").asNumber(), 0.0),
          "Fire.burnedFraction y burningArea");
    const json r = t.bridge({{"fn", "Fire.ignite"}, {"args", {json{{"$v", {0, 0, 0}}}, 2}}});
    check(r["ok"] == true && r["result"] == 1, "Fire.ignite por el puente");
}

void testWeather() {
    std::printf("Weather / Environment\n");
    ApiFixture t;
    // Sin Ambiente en la escena: los valores por defecto.
    const Value date = t.call("Weather.getDate");
    check(date.isArray() && date.size() == 2 && date[0].asNumber() == 21 && date[1].asNumber() == 6,
          "Weather.getDate sin Ambiente: {21, 6}");
    check(t.call("Weather.get").asString() == "Clear", "Weather.get sin Ambiente: Clear");
    check(near(t.call("Environment.getTime").asNumber(), 12.0), "Environment.getTime sin Ambiente: 12");
    check(t.call("Weather.getSeason").asString() == "Summer", "Weather.getSeason sin Ambiente: Summer");
    check(t.call("Weather.getSunDirection").asVec3().y == 1.0f, "Weather.getSunDirection sin Ambiente");
    check(t.world.registry().view<environment::Environment>().size() == 0, "las lecturas no crean el Ambiente");

    // Cada funcion esta en las dos tablas.
    std::size_t weather = 0;
    bool all_aliased = true;
    for (const auto& e : t.api().entries()) {
        if (e.owner != "Weather") continue;
        ++weather;
        if (t.api().find("Environment." + e.name) == nullptr) all_aliased = false;
    }
    std::size_t environment = 0;
    for (const auto& e : t.api().entries()) environment += e.owner == "Environment" ? 1 : 0;
    check(weather == 39 && all_aliased && weather == environment, "Environment tiene todo lo de Weather");

    check(t.call("Weather.set", {Value("Storm")}).truthy(), "Weather.set(\"Storm\")");
    check(t.world.registry().view<environment::Environment>().size() == 1, "Weather.set crea el Ambiente");
    check(t.call("Weather.getTarget").asString() == "Storm" && t.call("Environment.getTarget").asString() == "Storm",
          "Weather.getTarget y Environment.getTarget");
    check(t.call("Environment.setWeather", {Value("Rain"), Value(0)}).truthy() &&
              t.call("Weather.getWeather").asString() == "Rain",
          "Environment.setWeather al instante");
    check(!t.call("Weather.set", {Value("Nada")}).truthy() && t.logged("clima desconocido 'Nada'"),
          "Weather.set con un clima desconocido avisa");
    check(!t.call("Weather.getLabel").asString().empty(), "Weather.getLabel");
    const Value presets = t.call("Environment.presets");
    check(presets.isArray() && presets.size() == static_cast<std::size_t>(environment::kWeatherPresetCount) &&
              presets[0].asString() == "Clear",
          "Environment.presets");

    t.call("Weather.setTime", {Value(25.5)});
    check(near(t.call("Weather.getTime").asNumber(), 1.5), "Weather.setTime da la vuelta a las 24 h");
    t.call("Environment.setTime", {Value(-1)});
    check(near(t.call("Weather.getTime").asNumber(), 23.0), "Environment.setTime con horas negativas");
    t.call("Weather.setDate", {Value(40), Value(13)});
    const Value clamped = t.call("Environment.getDate");
    check(clamped[0].asNumber() == 31 && clamped[1].asNumber() == 12, "Weather.setDate limita dia y mes");
    t.call("Weather.setLatitude", {Value(100)});
    check(near(t.call("Weather.getLatitude").asNumber(), 89.0), "Weather.setLatitude limita a 89");
    t.call("Weather.setTimeScale", {Value(60)});
    check(near(t.call("Weather.getTimeScale").asNumber(), 60.0), "Weather.setTimeScale / getTimeScale");
    t.call("Weather.setDayLength");
    check(near(t.call("Weather.getTimeScale").asNumber(), 0.0), "Weather.setDayLength(nil) para el tiempo");
    t.call("Weather.setSeason", {Value("Winter")});
    check(t.call("Weather.getSeason").asString() == "Winter", "Weather.setSeason fija la estacion");
    t.call("Weather.setSeason", {Value("Monzon")});
    check(t.logged("estacion desconocida 'Monzon'"), "Weather.setSeason desconocida avisa");

    const environment::Environment* env = environment::findEnvironment(t.world);
    t.call("Weather.setWind", {Value(90), Value(2.5)});
    t.call("Weather.setRandom", {Value(true), Value(1), Value(30)});
    t.call("Weather.setLightning", {Value(false), Value(-3)});
    t.call("Weather.setAudio", {Value(false), Value(0.5)});
    t.call("Weather.setPrecipitationDensity", {Value(9)});
    check(env != nullptr && near(env->wind_direction, 90.0) && near(env->wind_strength, 2.5),
          "Weather.setWind: direccion y fuerza");
    check(env != nullptr && env->random_weather && near(env->min_duration, 5.0) && near(env->max_duration, 30.0),
          "Weather.setRandom: minimo de 5 s");
    check(env != nullptr && !env->lightning && near(env->lightning_frequency, 0.0), "Weather.setLightning");
    check(env != nullptr && !env->ambient_audio && near(env->audio_volume, 0.5), "Weather.setAudio");
    check(env != nullptr && near(env->precipitation_density, 3.0), "Weather.setPrecipitationDensity limita a 3");
    t.call("Weather.setSnowCover", {Value(1)});
    t.call("Weather.setWetness", {Value(1), Value(0.5)});
    check(near(t.call("Weather.getSnowCover").asNumber(), 1.0) && near(t.call("Weather.getWetness").asNumber(), 1.0),
          "Weather.setSnowCover y setWetness");
    t.call("Environment.lightning", {Value(500)});
    check(!t.call("Weather.isNight").truthy() && near(t.call("Weather.transition").asNumber(), 1.0),
          "Weather.isNight y transition sin simular");

    const json r = t.bridge({{"fn", "Environment.getDate"}});
    check(r["ok"] == true && r["result"] == json({31.0, 12.0}), "Environment.getDate por el puente: una lista");
}

void testVoxel() {
    std::printf("Voxel\n");
    ApiFixture t;
    check(!t.call("Voxel.isActive").truthy(), "Voxel.isActive sin mundo de bloques");
    check(t.call("Voxel.getBlock", {Value(1.7), Value(2), Value(3)}).asNumber() == 0, "Voxel.getBlock sin mundo: aire");
    check(t.call("Voxel.skyLight", {Value(0), Value(0), Value(0)}).asNumber() == 15, "Voxel.skyLight sin mundo: 15");
    check(!t.call("Voxel.setBlock", {Value(0), Value(0), Value(0), Value("stone")}).truthy(),
          "Voxel.setBlock sin mundo: false");
    check(t.call("Voxel.blockId", {Value("stone")}).asNumber() == voxel::block::Stone, "Voxel.blockId(\"stone\")");
    check(t.call("Voxel.blockCount").asNumber() == voxel::block::Count, "Voxel.blockCount");
    const Value info = t.call("Voxel.blockInfo", {Value("stone")});
    check(info.isObject() && info["name"].asString() == "stone" && info["id"].asNumber() == voxel::block::Stone &&
              info["solid"].truthy(),
          "Voxel.blockInfo(\"stone\")");
    check(t.call("Voxel.blockInfo", {Value(999)}).isNil(), "Voxel.blockInfo de un id que no existe: nil");
    const Value color = t.call("Voxel.blockColor", {Value(1)});
    check(color.isVec3() && color.asVec3().x == 1.0f, "Voxel.blockColor sin mundo: blanco");
    check(t.call("Voxel.raycast", {Value(Vec3{0, 0, 0}), Value(Vec3{0, -1, 0})}).isNil(), "Voxel.raycast sin mundo: nil");
    const Value moved = t.call("Voxel.moveBox", {Value(Vec3{1, 2, 3}), Value(Vec3{0.5f, 1, 0.5f}), Value(Vec3{0, -1, 0})});
    check(moved.isArray() && moved.size() == 4 && moved[0].asVec3().y == 1.0f && !moved[1].truthy() &&
              !moved[2].truthy() && !moved[3].truthy(),
          "Voxel.moveBox: cuatro resultados en una lista");
    check(t.call("Voxel.getMeta", {Value("modo"), Value("creativo")}).asString() == "creativo",
          "Voxel.getMeta sin mundo: el valor por defecto");
    check(t.call("Voxel.listWorlds").isArray() && t.call("Voxel.listWorlds").size() == 0, "Voxel.listWorlds vacia");
    check(t.call("Voxel.worldName").asString().empty() && t.call("Voxel.seed").asNumber() == 0,
          "Voxel.worldName y seed sin mundo");
    check(throws(t, "Voxel.getBlock", {Value(1)}), "Voxel.getBlock sin coordenadas es un error");
    const json r = t.bridge({{"fn", "Voxel.moveBox"},
                             {"args", {json{{"$v", {0, 0, 0}}}, json{{"$v", {1, 1, 1}}}, json{{"$v", {0, 2, 0}}}}}});
    check(r["ok"] == true && r["result"].is_array() && r["result"].size() == 4 && r["result"][1] == false,
          "Voxel.moveBox por el puente");
}

void testFluid() {
    std::printf("Fluid\n");
    ApiFixture t;
    fluid::setActiveSystem(nullptr);
    check(!t.call("Fluid.isActive").truthy(), "Fluid.isActive sin sistema");
    check(t.call("Fluid.count").asNumber() == 0, "Fluid.count sin sistema");
    check(t.call("Fluid.surfaceHeight", {Value(0), Value(0)}).isNil(), "Fluid.surfaceHeight sin sistema: nil");
    check(t.call("Fluid.velocity", {Value(Vec3{0, 0, 0})}).isVec3(), "Fluid.velocity sin sistema: Vec3");
    const Value types = t.call("Fluid.types");
    check(types.isArray() && types.size() == static_cast<std::size_t>(fluid::FluidType::Count) &&
              types[0].asString() == "water",
          "Fluid.types");
    const Value stats = t.call("Fluid.stats");
    check(stats.isObject() && stats["particles"].asNumber() == 0 && stats["memoryMB"].isNumber() &&
              !stats["simulating"].truthy(),
          "Fluid.stats sin sistema");

    ecs::Entity e = t.world.create("Grifo");
    fluid::FluidEmitter& em = e.add<fluid::FluidEmitter>();
    t.call("Fluid.setType", {ApiFixture::entity(e), Value("lava")});
    check(em.fluid == fluid::FluidType::Lava, "Fluid.setType(\"lava\")");
    t.call("Fluid.setType", {ApiFixture::entity(e), Value(99)});
    check(em.fluid == fluid::FluidType::Custom, "Fluid.setType con un numero grande: el ultimo");
    t.call("Fluid.setType", {ApiFixture::entity(e), Value("gasolina")});
    check(em.fluid == fluid::FluidType::Custom && t.logged("Fluid.setType: liquido desconocido"),
          "Fluid.setType desconocido avisa");
    t.call("Fluid.stop", {ApiFixture::entity(e)});
    check(!em.emitting, "Fluid.stop");
    t.call("Fluid.start", {ApiFixture::entity(e)});
    check(em.emitting, "Fluid.start");
    t.call("Fluid.stop", {Value()});  // nil: no hace nada

    {
        fluid::FluidSystem system;
        fluid::setActiveSystem(&system);
        t.call("Fluid.spawn", {Value(Vec3{0, 1, 0}), Value(10), Value("melaza")});
        check(t.logged("Fluid.spawn: liquido desconocido"), "Fluid.spawn con un liquido desconocido avisa");
        t.call("Fluid.spawn", {Value(Vec3{0, 1, 0}), Value(10), Value()});
        t.call("Fluid.stop", {ApiFixture::entity(e)});
        t.call("Fluid.restart", {ApiFixture::entity(e)});
        check(em.emitting, "Fluid.restart vuelve a emitir");
        check(t.call("Fluid.count", {Value("honey")}).asNumber() == 0, "Fluid.count de un tipo");
        t.call("Fluid.clear");
        check(t.call("Fluid.count").asNumber() == 0, "Fluid.clear");
        fluid::setActiveSystem(nullptr);
    }
    const json r = t.bridge({{"fn", "Fluid.types"}});
    check(r["ok"] == true && r["result"].is_array() && r["result"][3] == "lava", "Fluid.types por el puente");
}

}  // namespace

int main() {
    testFire();
    testWeather();
    testVoxel();
    testFluid();
    return finish();
}
