// Pruebas de native/Features24Api.cpp: splines (Entity y Spline.create),
// listas desplegables, scroll, Text.strip, WorldPartition, Accessibility,
// Camera, Voice, Crowd, Mods y Jobs.

#include "ApiTest.h"

#include "CramionCore/ai/Crowd.h"
#include "CramionCore/audio/VoiceChat.h"
#include "CramionCore/gameplay/Accessibility.h"
#include "CramionCore/project/Mods.h"
#include "CramionCore/spline/Spline.h"
#include "CramionCore/ui/UI.h"
#include "CramionCore/world/WorldPartition.h"

#include <cmath>
#include <filesystem>
#include <fstream>

using namespace cramion;
using namespace cramion::apitest;
using core::Vec3;

namespace {

bool near(float a, float b, float eps = 0.05f) { return std::fabs(a - b) <= eps; }
bool near(const Vec3& a, const Vec3& b, float eps = 0.05f) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}

// Lanza scripting::api::Error?
template <typename Fn>
bool throwsApiError(Fn&& fn) {
    try {
        fn();
    } catch (const scripting::api::Error&) {
        return true;
    }
    return false;
}

void testRegistered(ApiFixture& t) {
    std::printf("Registro\n");
    const char* keys[] = {
        "Entity:splinePoint", "Entity:splinePointAt", "Entity:splineTangent", "Entity:splineRight",
        "Entity:splineLength", "Entity:closestSplineDistance", "Entity:closestSplinePoint", "Entity:splinePointCount",
        "Entity:getSplineControlPoint", "Entity:setSplineControlPoint", "Entity:addSplinePoint",
        "Entity:clearSplinePoints", "Entity:playSplineFollower", "Entity:stopSplineFollower", "Entity:splineDistance",
        "Entity:splineSpeed", "Entity:dropdownValue", "Entity:dropdownText", "Entity:setDropdownOptions",
        "Entity:getDropdownOptions", "Entity:scrollY", "Entity:scrollX", "Entity:scrollToFraction",
        "Entity:scrollContentHeight", "Entity:spawnCrowd", "Entity:despawnCrowd", "Text.strip",
        "WorldPartition.active", "WorldPartition.loadAll", "WorldPartition.isLoaded", "WorldPartition.stats",
        "Accessibility.get", "Accessibility.set", "Accessibility.save", "Accessibility.load", "Accessibility.reset",
        "Accessibility.colorblindName", "Camera.shake", "Camera.stopShake", "Voice.start", "Voice.stop",
        "Voice.setMode", "Voice.setTalking", "Voice.setThreshold", "Voice.setVolume", "Voice.setMicGain",
        "Voice.setProximity", "Voice.setMuted", "Voice.isMuted", "Voice.isSpeaking", "Voice.micLevel", "Crowd.stats",
        "Mods.enabled", "Mods.list", "Mods.setEnabled", "Mods.isLoaded", "Jobs.workers", "Jobs.executed",
        "Spline.create"};
    int missing = 0;
    int undocumented = 0;
    for (const char* key : keys) {
        const scripting::api::Entry* e = t.api().find(key);
        if (e == nullptr) {
            std::printf("    falta %s\n", key);
            ++missing;
        } else if (e->doc.description.empty() && std::string(key).find("Voice.is") == std::string::npos &&
                   std::string(key) != "Mods.isLoaded") {
            ++undocumented;
        }
    }
    check(missing == 0, "estan todas las funciones, metodos y propiedades");
    check(undocumented == 0, "todas con documentacion");
    const scripting::api::Entry* length = t.api().find("Entity:splineLength");
    check(length != nullptr && length->kind == scripting::api::Entry::Kind::Property && length->type_member &&
              !length->assign,
          "splineLength es una propiedad de Entity de solo lectura");
    const scripting::api::Entry* speed = t.api().find("Entity:splineSpeed");
    check(speed != nullptr && speed->kind == scripting::api::Entry::Kind::Property && static_cast<bool>(speed->assign),
          "splineSpeed se puede escribir");
}

void testSplines(ApiFixture& t) {
    std::printf("Splines\n");
    const Value created = t.call("Spline.create", {Value(Value::Array{Value(Vec3{0, 0, 0}), Value(Vec3{0, 0, 20})}),
                                                   Value("Carretera"), Value("Camino"), Value(false)});
    check(created.isEntity() && created.asEntity() != entt::null, "Spline.create devuelve la entidad");
    const ecs::Entity road = t.world.wrap(created.asEntity());
    check(road.valid() && road.name() == "Camino", "Spline.create le pone el nombre");
    check(road.has<spline::Spline>() && road.has<spline::SplineExtrude>() &&
              road.get<spline::SplineExtrude>().shape == spline::ExtrudeShape::Road,
          "forma \"Carretera\" = SplineExtrude de carretera");
    check(t.get("splinePointCount", created).asNumber() == 2, "splinePointCount");
    check(near(static_cast<float>(t.get("splineLength", created).asNumber()), 20.0f, 0.5f), "splineLength (20 m)");
    check(near(t.call("splinePoint", {Value(0.5)}, created).asVec3(), Vec3{0, 0, 10}, 0.3f), "splinePoint(0.5) en el mundo");
    check(near(t.call("splinePointAt", {Value(5.0)}, created).asVec3(), Vec3{0, 0, 5}, 0.3f), "splinePointAt(5)");
    const Vec3 tangent = t.call("splineTangent", {Value(0.5)}, created).asVec3();
    check(near(std::fabs(tangent.z), 1.0f, 0.05f), "splineTangent a lo largo de Z");
    const Vec3 right = t.call("splineRight", {Value(0.5)}, created).asVec3();
    check(near(std::fabs(right.x), 1.0f, 0.05f), "splineRight a lo largo de X");
    check(near(static_cast<float>(t.call("closestSplineDistance", {Value(Vec3{5, 0, 5})}, created).asNumber()), 5.0f, 0.3f),
          "closestSplineDistance");
    check(near(t.call("closestSplinePoint", {Value(Vec3{5, 0, 5})}, created).asVec3(), Vec3{0, 0, 5}, 0.3f),
          "closestSplinePoint");
    check(near(t.call("getSplineControlPoint", {Value(1)}, created).asVec3(), Vec3{0, 0, 0}), "getSplineControlPoint(1) en el mundo");
    check(near(t.call("getSplineControlPoint", {Value(2)}, created).asVec3(), Vec3{0, 0, 20}), "getSplineControlPoint(2)");
    check(t.call("getSplineControlPoint", {Value(3)}, created).isNil() && t.call("getSplineControlPoint", {Value(0)}, created).isNil(),
          "getSplineControlPoint fuera de rango = nil");
    check(t.call("setSplineControlPoint", {Value(2), Value(Vec3{0, 0, 30})}, created).truthy() &&
              near(t.call("getSplineControlPoint", {Value(2)}, created).asVec3(), Vec3{0, 0, 30}),
          "setSplineControlPoint mueve el punto (mundo)");
    check(t.call("setSplineControlPoint", {Value(9), Value(Vec3{0, 0, 30})}, created).isBool() &&
              !t.call("setSplineControlPoint", {Value(9), Value(Vec3{0, 0, 30})}, created).truthy(),
          "setSplineControlPoint fuera de rango = false");
    const std::uint64_t revision = road.get<spline::Spline>().revision;
    check(t.call("addSplinePoint", {Value(Vec3{10, 0, 30})}, created).truthy() && t.get("splinePointCount", created).asNumber() == 3,
          "addSplinePoint anade al final");
    check(road.get<spline::Spline>().revision > revision, "addSplinePoint marca la spline como cambiada");
    check(near(t.call("getSplineControlPoint", {Value(3)}, created).asVec3(), Vec3{10, 0, 30}), "el punto anadido en el mundo");
    check(near(road.get<spline::Spline>().points.back().width, 1.0f), "addSplinePoint: ancho del ultimo punto");
    t.call("addSplinePoint", {Value(Vec3{20, 0, 30}), Value(2.5)}, created);
    check(near(road.get<spline::Spline>().points.back().width, 2.5f), "addSplinePoint con ancho");
    t.call("clearSplinePoints", {}, created);
    check(t.get("splinePointCount", created).asNumber() == 0, "clearSplinePoints");

    // Sin Spline: valores neutros; addSplinePoint la crea (con sus 2 puntos por defecto, como antes).
    ecs::Entity plain = t.world.create("Nada");
    const Value pv = ApiFixture::entity(plain);
    check(t.get("splineLength", pv).asNumber() == 0 && t.get("splinePointCount", pv).asNumber() == 0,
          "sin Spline: splineLength y splinePointCount = 0");
    check(t.call("getSplineControlPoint", {Value(1)}, pv).isNil(), "sin Spline: getSplineControlPoint = nil");
    check(!t.call("setSplineControlPoint", {Value(1), Value(Vec3{})}, pv).truthy(), "sin Spline: setSplineControlPoint = false");
    t.call("clearSplinePoints", {}, pv);
    check(t.call("addSplinePoint", {Value(Vec3{1, 2, 3})}, pv).truthy() && plain.has<spline::Spline>() &&
              t.get("splinePointCount", pv).asNumber() == 3,
          "addSplinePoint crea la Spline si no la tiene");

    // Formas y cerrada.
    const Value none = t.call("Spline.create", {Value(Value::Array{Value(Vec3{0, 0, 0}), Value(Vec3{5, 0, 0}), Value("x")}),
                                                Value("nada"), Value(), Value(true)});
    const ecs::Entity none_e = t.world.wrap(none.asEntity());
    check(none_e.valid() && !none_e.has<spline::SplineExtrude>() && none_e.get<spline::Spline>().closed &&
              none_e.get<spline::Spline>().points.size() == 2 && none_e.name() == "Spline",
          "Spline.create sin forma, cerrada, nombre por defecto y solo los Vec3");
    const Value river = t.call("Spline.create", {Value(Value::Array{Value(Vec3{0, 0, 0}), Value(Vec3{5, 0, 0})}), Value("RIO")});
    check(t.world.wrap(river.asEntity()).get<spline::SplineExtrude>().shape == spline::ExtrudeShape::River, "forma \"RIO\"");

    // Seguidor.
    ecs::Entity cart = t.world.create("Vagoneta");
    spline::SplineFollower& f = cart.add<spline::SplineFollower>();
    f.started = false;
    f.playing = false;
    const Value cv = ApiFixture::entity(cart);
    t.call("playSplineFollower", {}, cv);
    check(f.started && f.playing, "playSplineFollower");
    t.call("stopSplineFollower", {}, cv);
    check(f.started && !f.playing, "stopSplineFollower");
    t.set("splineDistance", Value(4.0), cv);
    check(near(f.distance, 4.0f) && near(static_cast<float>(t.get("splineDistance", cv).asNumber()), 4.0f), "splineDistance");
    t.set("splineSpeed", Value(12.0), cv);
    check(near(f.speed, 12.0f) && near(static_cast<float>(t.get("splineSpeed", cv).asNumber()), 12.0f), "splineSpeed");
    check(t.get("splineSpeed", pv).asNumber() == 0, "sin SplineFollower: splineSpeed = 0");
    t.set("splineDistance", Value(3.0), pv);  // no hace nada
    check(throwsApiError([&] { t.set("splineSpeed", Value("rapido"), cv); }), "splineSpeed con texto = error");

    // Por el puente (el JSON del SDK).
    const json r = t.bridge({{"fn", "Spline.create"},
                             {"args", {json::array({{{"$v", {0, 0, 0}}}, {{"$v", {0, 0, 20}}}}), "road", "Puente"}}});
    check(r["ok"] == true && r["result"].contains("$e"), "Spline.create por el puente");
    const json self = r["result"];
    const json count = t.bridge({{"op", "get"}, {"self", self}, {"key", "splinePointCount"}});
    check(count["ok"] == true && count["result"] == 2, "splinePointCount por el puente");
    const json mid = t.bridge({{"fn", "splinePoint"}, {"self", self}, {"args", json::array({0.5})}});
    check(mid["ok"] == true && mid["result"].contains("$v") && std::fabs(mid["result"]["$v"][2].get<double>() - 10.0) < 0.3,
          "splinePoint por el puente");

    // Entidad destruida.
    ecs::Entity gone = t.world.create("Borrada");
    const Value gv = ApiFixture::entity(gone);
    t.world.destroy(gone);
    check(throwsApiError([&] { t.call("splinePoint", {Value(0.5)}, gv); }), "entidad destruida = error");
}

void testUi(ApiFixture& t) {
    std::printf("Dropdown, ScrollView y Text.strip\n");
    ecs::Entity e = t.world.create("Calidad");
    ui::Dropdown& d = e.add<ui::Dropdown>();
    d.options = {"Baja", "Media", "Alta"};
    d.value = 0;
    const Value v = ApiFixture::entity(e);
    check(t.get("dropdownValue", v).asNumber() == 1 && t.get("dropdownText", v).asString() == "Baja",
          "dropdownValue desde 1 y dropdownText");
    t.set("dropdownValue", Value(3), v);
    check(d.value == 2 && t.get("dropdownText", v).asString() == "Alta", "dropdownValue = 3");
    t.set("dropdownValue", Value(10), v);
    check(d.value == 2, "dropdownValue se recorta por arriba");
    t.set("dropdownValue", Value(-4), v);
    check(d.value == 0, "dropdownValue se recorta por abajo");
    t.set("dropdownValue", Value(3), v);
    t.call("setDropdownOptions", {Value(Value::Array{Value("Si"), Value(7), Value("No")})}, v);
    check(d.options.size() == 2 && d.options[0] == "Si" && d.options[1] == "No" && d.value == 1,
          "setDropdownOptions (solo textos) recorta la elegida");
    const Value options = t.call("getDropdownOptions", {}, v);
    check(options.isArray() && options.size() == 2 && options[0].asString() == "Si" && options[1].asString() == "No",
          "getDropdownOptions");
    t.call("setDropdownOptions", {Value::object()}, v);
    check(d.options.empty() && d.value == 0 && t.get("dropdownText", v).asString().empty(),
          "setDropdownOptions con una tabla vacia");
    const json opts = t.bridge({{"fn", "setDropdownOptions"}, {"self", {{"$e", entt::to_integral(e.handle()) + 1}}},
                                {"args", json::array({json::array({"A", "B"})})}});
    check(opts["ok"] == true && d.options.size() == 2, "setDropdownOptions por el puente");
    const json set = t.bridge({{"op", "set"}, {"self", {{"$e", entt::to_integral(e.handle()) + 1}}},
                               {"key", "dropdownValue"}, {"value", 2}});
    check(set["ok"] == true && d.value == 1, "dropdownValue por el puente");

    ecs::Entity plain = t.world.create("Nada");
    const Value pv = ApiFixture::entity(plain);
    check(t.get("dropdownValue", pv).asNumber() == 0 && t.get("dropdownText", pv).asString().empty(),
          "sin Dropdown: 0 y texto vacio");
    check(t.call("getDropdownOptions", {}, pv).isArray() && t.call("getDropdownOptions", {}, pv).size() == 0,
          "sin Dropdown: lista vacia");

    ecs::Entity list = t.world.create("Lista");
    ui::ScrollView& s = list.add<ui::ScrollView>();
    s.content = core::Vec2{300.0f, 400.0f};
    s.velocity = core::Vec2{5.0f, 5.0f};
    const Value lv = ApiFixture::entity(list);
    t.set("scrollY", Value(120.0), lv);
    check(near(s.scroll.y, 120.0f) && s.velocity.x == 0.0f && s.velocity.y == 0.0f &&
              near(static_cast<float>(t.get("scrollY", lv).asNumber()), 120.0f),
          "scrollY (y para la inercia)");
    s.velocity = core::Vec2{5.0f, 5.0f};
    t.set("scrollX", Value(30.0), lv);
    check(near(s.scroll.x, 30.0f) && s.velocity.y == 0.0f && near(static_cast<float>(t.get("scrollX", lv).asNumber()), 30.0f),
          "scrollX");
    t.call("scrollToFraction", {Value(0.5)}, lv);
    check(near(s.scroll.y, 200.0f), "scrollToFraction(0.5)");
    t.call("scrollToFraction", {Value(3.0)}, lv);
    check(near(s.scroll.y, 400.0f), "scrollToFraction se recorta a 1");
    check(near(static_cast<float>(t.get("scrollContentHeight", lv).asNumber()), 400.0f), "scrollContentHeight");
    check(t.get("scrollY", pv).asNumber() == 0 && t.get("scrollContentHeight", pv).asNumber() == 0, "sin ScrollView: 0");

    check(t.call("Text.strip", {Value("<b>Hola</b> mundo")}).asString() == "Hola mundo", "Text.strip");
}

void testWorldPartition(ApiFixture& t) {
    std::printf("WorldPartition\n");
    worldpart::setActivePartition(nullptr);
    check(!t.call("WorldPartition.active").truthy(), "sin particion: active = false");
    check(t.call("WorldPartition.isLoaded", {Value(Vec3{1000, 0, 1000})}).truthy(), "sin particion: todo cargado");
    const Value empty = t.call("WorldPartition.stats");
    check(empty.isObject() && empty.size() == 0, "sin particion: stats vacio");
    t.call("WorldPartition.loadAll");
    worldpart::WorldPartitionSystem partition;
    worldpart::setActivePartition(&partition);
    const Value stats = t.call("WorldPartition.stats");
    check(stats.isObject() && stats.size() == 6 && stats["cells"].isNumber() && stats["loadedCells"].isNumber() &&
              stats["objects"].isNumber() && stats["unloadedObjects"].isNumber() && stats["storedBytes"].isNumber() &&
              stats["active"].isBool(),
          "stats con sus campos");
    check(!t.call("WorldPartition.active").truthy() && t.call("WorldPartition.isLoaded", {Value(Vec3{})}).truthy(),
          "particion sin empezar: inactiva y todo cargado");
    t.call("WorldPartition.loadAll");
    worldpart::setActivePartition(nullptr);
    check(throwsApiError([&] { t.call("WorldPartition.isLoaded"); }), "isLoaded sin posicion = error");
}

void testAccessibility(ApiFixture& t) {
    std::printf("Accessibility y Camera\n");
    t.call("Accessibility.reset");
    gameplay::AccessibilitySettings& a = gameplay::accessibility();
    Value options = Value::object();
    options.set("colorblind", 7);
    options.set("textScale", 1.3);
    options.set("reduceMotion", true);
    options.set("cameraShake", 5.0);
    options.set("colorblindCorrect", "no");  // tipo equivocado: se queda como estaba
    t.call("Accessibility.set", {options});
    check(a.colorblind_mode == 4 && near(a.text_scale, 1.3f) && a.reduce_motion && near(a.camera_shake, 2.0f) &&
              a.colorblind_correct,
          "Accessibility.set (recorta y solo cambia lo que viene)");
    const Value got = t.call("Accessibility.get");
    check(got["colorblind"].asNumber() == 4 && near(static_cast<float>(got["textScale"].asNumber()), 1.3f) &&
              got["reduceMotion"].truthy() && got["colorblindCorrect"].truthy() && got["highContrast"].isBool() &&
              got["subtitleScale"].isNumber() && got["subtitleBackground"].isBool() &&
              got["colorblindStrength"].isNumber() && got["cameraShake"].isNumber(),
          "Accessibility.get");
    const json r = t.bridge({{"fn", "Accessibility.set"}, {"args", json::array({json::array()})}});
    check(r["ok"] == true && a.colorblind_mode == 4, "Accessibility.set con una tabla vacia ([])");
    check(throwsApiError([&] { t.call("Accessibility.set", {Value(3)}); }), "Accessibility.set con un numero = error");

    const std::filesystem::path file = std::filesystem::temp_directory_path() / "cramion_features24_tests" / "access.json";
    gameplay::setAccessibilityFile(file);
    check(t.call("Accessibility.save").truthy(), "Accessibility.save");
    t.call("Accessibility.reset");
    check(a.colorblind_mode == 0 && near(a.text_scale, 1.0f) && !a.reduce_motion, "Accessibility.reset");
    check(t.call("Accessibility.load").truthy() && a.colorblind_mode == 4 && near(a.text_scale, 1.3f),
          "Accessibility.load");
    gameplay::setAccessibilityFile({});
    check(!t.call("Accessibility.save").truthy(), "Accessibility.save sin archivo = false");
    check(t.call("Accessibility.colorblindName", {Value(2)}).asString() == gameplay::colorblindModeName(2),
          "Accessibility.colorblindName");
    t.call("Accessibility.reset");

    Vec3 position{};
    Vec3 rotation{};
    t.call("Camera.shake", {Value(0.8), Value(1.0)});
    check(gameplay::cameraShakeOffset(0.016f, position, rotation), "Camera.shake");
    t.call("Camera.stopShake");
    check(!gameplay::cameraShakeOffset(0.016f, position, rotation), "Camera.stopShake");
    t.call("Camera.shake", {Value(0.5)});
    check(gameplay::cameraShakeOffset(0.016f, position, rotation), "Camera.shake con los valores por defecto");
    t.call("Camera.stopShake");
}

void testVoice(ApiFixture& t) {
    std::printf("Voice\n");
    // Voice.start no se prueba: abriria el microfono.
    t.call("Voice.setMode", {Value("open")});
    check(audio::activeVoiceChat() != nullptr && audio::activeVoiceChat()->mode() == audio::VoiceMode::Open,
          "Voice.setMode(\"open\") crea el chat de voz");
    t.call("Voice.setMode", {Value("OFF")});
    check(audio::activeVoiceChat()->mode() == audio::VoiceMode::Off, "Voice.setMode(\"OFF\")");
    t.call("Voice.setMode", {Value("push")});
    check(audio::activeVoiceChat()->mode() == audio::VoiceMode::PushToTalk, "Voice.setMode(\"push\")");
    t.call("Voice.setMode", {Value("voz")});
    check(audio::activeVoiceChat()->mode() == audio::VoiceMode::Open, "Voice.setMode(\"voz\")");
    t.call("Voice.setMuted", {Value(5), Value(true)});
    check(t.call("Voice.isMuted", {Value(5)}).truthy() && !t.call("Voice.isMuted", {Value(6)}).truthy(),
          "Voice.setMuted / isMuted");
    t.call("Voice.setMuted", {Value(5), Value(false)});
    check(!t.call("Voice.isMuted", {Value(5)}).truthy(), "Voice.setMuted(false)");
    check(t.call("Voice.isSpeaking").isBool() && !t.call("Voice.isSpeaking", {Value(5)}).truthy(), "Voice.isSpeaking");
    check(t.call("Voice.micLevel").isNumber(), "Voice.micLevel");
    t.call("Voice.setTalking", {Value(true)});
    t.call("Voice.setThreshold", {Value(0.02)});
    t.call("Voice.setVolume", {Value(0.8)});
    t.call("Voice.setMicGain", {Value(1.2)});
    t.call("Voice.setProximity", {Value(30)});
    t.call("Voice.setTalking", {Value(false)});
    t.call("Voice.stop");
    check(!audio::activeVoiceChat()->running(), "Voice.stop");
    check(throwsApiError([&] { t.call("Voice.setVolume"); }), "Voice.setVolume sin valor = error");
}

void testCrowdModsJobs(ApiFixture& t) {
    std::printf("Crowd, Mods y Jobs\n");
    ecs::Entity spawner = t.world.create("Plaza");
    const Value sv = ApiFixture::entity(spawner);
    ai::setActiveCrowds(nullptr);
    check(t.call("spawnCrowd", {}, sv).asNumber() == 0, "sin multitudes: spawnCrowd = 0");
    t.call("despawnCrowd", {}, sv);
    check(t.call("Crowd.stats").isObject() && t.call("Crowd.stats").size() == 0, "sin multitudes: stats vacio");
    ai::CrowdSystem crowds;
    ai::setActiveCrowds(&crowds);
    check(t.call("spawnCrowd", {}, sv).asNumber() == 0, "spawnCrowd sin CrowdSpawner = 0");
    t.call("despawnCrowd", {}, sv);
    const Value cs = t.call("Crowd.stats");
    check(cs["agents"].asNumber() == 0 && cs["visible"].isNumber() && cs["spawners"].isNumber(), "Crowd.stats");
    ai::setActiveCrowds(nullptr);

    project::setActiveMods(nullptr);
    check(!t.call("Mods.enabled").truthy(), "sin mods: Mods.enabled = false");
    check(t.call("Mods.list").isArray() && t.call("Mods.list").size() == 0, "sin mods: lista vacia");
    check(!t.call("Mods.setEnabled", {Value("x"), Value(false)}).truthy() && !t.call("Mods.isLoaded", {Value("x")}).truthy(),
          "sin mods: setEnabled e isLoaded = false");

    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / "cramion_features24_tests";
    const fs::path mod = base / "Mods" / "MasCoches";
    std::error_code ec;
    fs::remove_all(base / "Mods", ec);
    fs::create_directories(mod / "Assets", ec);
    {
        std::ofstream out(mod / "mod.json");
        out << R"({"name": "Mas coches", "version": "1.2", "author": "Ana", "description": "Coches nuevos"})";
    }
    project::ModManager mods;
    mods.setSearchFolders({base / "Mods"});
    mods.setStateFile(base / "mods_state.json");
    mods.scan();
    project::setActiveMods(&mods);
    check(t.call("Mods.enabled").truthy(), "Mods.enabled");
    const Value list = t.call("Mods.list");
    check(list.size() == 1 && list[0]["id"].asString() == "MasCoches" && list[0]["name"].asString() == "Mas coches" &&
              list[0]["version"].asString() == "1.2" && list[0]["author"].asString() == "Ana" &&
              list[0]["description"].asString() == "Coches nuevos" && list[0]["enabled"].truthy() &&
              list[0]["loaded"].isBool() && !list[0]["loaded"].truthy(),
          "Mods.list");
    check(t.call("Mods.setEnabled", {Value("MasCoches"), Value(false)}).truthy() && !t.call("Mods.list")[0]["enabled"].truthy() &&
              fs::exists(base / "mods_state.json"),
          "Mods.setEnabled guarda el estado");
    check(!t.call("Mods.setEnabled", {Value("NoExiste"), Value(true)}).truthy(), "Mods.setEnabled de un mod que no existe");
    check(!t.call("Mods.isLoaded", {Value("MasCoches")}).truthy(), "Mods.isLoaded (sin montar)");
    project::setActiveMods(nullptr);
    fs::remove_all(base, ec);

    check(t.call("Jobs.workers").isNumber() && t.call("Jobs.workers").asNumber() >= 0, "Jobs.workers");
    check(t.call("Jobs.executed").isNumber() && t.call("Jobs.executed").asNumber() >= 0, "Jobs.executed");
}

}  // namespace

int main() {
    std::printf("Features24\n");
    {
        ApiFixture t;
        testRegistered(t);
        testSplines(t);
        testUi(t);
        testWorldPartition(t);
        testAccessibility(t);
        testVoice(t);
        testCrowdModsJobs(t);
    }
    {
        ApiFixture idle(false);  // sin escena
        check(idle.call("Spline.create", {Value(Value::Array{Value(Vec3{}), Value(Vec3{0, 0, 1})})}).isNil(),
              "Spline.create sin escena = nil");
    }
    return finish();
}
