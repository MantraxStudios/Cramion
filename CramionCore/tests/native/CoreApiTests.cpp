// Pruebas de native/CoreApi.cpp: Time, Scene, Prefs, Game, Profiler, CVar,
// Physics, Audio y Random. Tambien las plantillas de prefab y el pool
// (ecs::PrefabTemplate, SceneStreamingApi.cpp) y el puente binario de los
// scripts de C++ (NativeApi::bridgeBinary).

#include "ApiTest.h"

#include "CramionCore/audio/Audio.h"
#include "CramionCore/cvar/CVar.h"
#include "CramionCore/ecs/Prefab.h"
#include "CramionCore/ecs/PrefabSpawn.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/profiling/Profiler.h"

#include <cramion/CppProtocol.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

using namespace cramion;
using namespace cramion::apitest;
using core::Vec3;

namespace {

const std::filesystem::path kRoot = std::filesystem::temp_directory_path() / "cramion_coreapi_tests";

bool near(double a, double b, double eps = 1e-4) { return std::abs(a - b) < eps; }

bool throws(ApiFixture& t, const std::string& key, const Value::Array& args = {}) {
    try {
        t.call(key, args);
    } catch (const scripting::api::Error&) {
        return true;
    }
    return false;
}

void testTime() {
    std::printf("Time\n");
    ApiFixture t;
    check(t.get("Time.time").asNumber() == 0.0 && t.get("Time.frameCount").asNumber() == 0.0, "al empezar: 0");
    t.frame(0.5f);
    t.frame(0.25f);
    check(near(t.get("Time.time").asNumber(), 0.75) && near(t.get("Time.deltaTime").asNumber(), 0.25) &&
              t.get("Time.frameCount").asNumber() == 2.0,
          "time, deltaTime y frameCount cada frame");
    t.scripts.fixedUpdate(t.world, 0.01f, 2);
    check(near(t.get("Time.fixedDeltaTime").asNumber(), 0.01), "fixedDeltaTime del paso fijo");
    const json r = t.bridge({{"op", "get"}, {"fn", "Time.time"}});
    check(r["ok"] == true && near(r["result"].get<double>(), 0.75), "Time.time por el puente (get)");
    bool read_only = false;
    try {
        t.set("Time.time", Value(5.0));
    } catch (const scripting::api::Error&) {
        read_only = true;
    }
    check(read_only, "Time.time es de solo lectura");
}

void testScene() {
    std::printf("Scene\n");
    {
        ApiFixture none(false);
        check(none.call("Scene.find", {Value("x")}).isNil() && none.call("Scene.create", {Value("x")}).isNil() &&
                  none.call("Scene.findAllWithTag", {Value("x")}).size() == 0 && none.call("Scene.origin").size() == 3,
              "sin escena: nil, lista vacia y origen (0, 0, 0)");
    }
    std::error_code ec;
    std::filesystem::remove_all(kRoot, ec);
    ecs::registerPrefabComponents();
    ApiFixture t(false);
    t.scripts.setAssetsRoot(kRoot);
    ecs::Entity player = t.world.create("Jugador");
    player.setTag("Player");
    ecs::Entity e1 = t.world.create("Enemigo");
    e1.setTag("Enemy");
    ecs::Entity e2 = t.world.create("Enemigo2");
    e2.setTag("Enemy");
    ecs::Entity child = t.world.create("Arma", player);
    child.setLocalPosition(Vec3{0.0f, 1.0f, 0.0f});
    t.scripts.start(t.world);

    check(t.call("Scene.find", {Value("Jugador")}).asEntity() == player.handle(), "Scene.find");
    check(t.call("Scene.find", {Value("Nadie")}).isNil(), "Scene.find de algo que no existe: nil");
    check(t.call("Scene.findWithTag", {Value("Player")}).asEntity() == player.handle() &&
              t.call("Scene.findWithTag", {Value("Nada")}).isNil(),
          "Scene.findWithTag");
    const Value all = t.call("Scene.findAllWithTag", {Value("Enemy")});
    check(all.isArray() && all.size() == 2 && all[0].isEntity() && all[1].isEntity(), "Scene.findAllWithTag: lista de entidades");

    const Value created = t.call("Scene.create", {Value("Caja"), Value(Vec3{1.0f, 2.0f, 3.0f})});
    const ecs::Entity box = t.world.wrap(created.asEntity());
    check(box.valid() && box.name() == "Caja" && near(box.worldPosition().y, 2.0), "Scene.create con posicion");

    const Value copy = t.call("Scene.instantiate", {ApiFixture::entity(player), Value(Vec3{5.0f, 0.0f, 0.0f}),
                                                    Value(Vec3{0.0f, 90.0f, 0.0f})});
    const ecs::Entity c = t.world.wrap(copy.asEntity());
    check(c.valid() && c != player && c.name() == "Jugador" && c.childCount() == 1 && near(c.worldPosition().x, 5.0) &&
              near(c.localEulerDegrees().y, 90.0, 1e-2),
          "Scene.instantiate de una entidad (con hijos, posicion y giro)");

    std::string error;
    check(ecs::createPrefab(t.world, box, kRoot / "Prefabs" / "Caja.crprefab", &error), "(prefab de prueba)");
    const Value p1 = t.call("Scene.instantiate", {Value("Prefabs/Caja"), Value(Vec3{0.0f, 7.0f, 0.0f})});
    const Value p2 = t.call("Scene.instantiate", {Value("Prefabs/Caja.crprefab")});
    check(t.world.wrap(p1.asEntity()).valid() && near(t.world.wrap(p1.asEntity()).worldPosition().y, 7.0) &&
              t.world.wrap(p2.asEntity()).valid() && t.world.wrap(p2.asEntity()).has<ecs::PrefabInstance>(),
          "Scene.instantiate de un prefab (con y sin extension)");
    check(t.call("Scene.instantiate", {Value("Prefabs/NoExiste")}).isNil() && t.logged("no existe el prefab \"Prefabs/NoExiste\""),
          "un prefab que no existe: nil y un error en la consola");

    t.call("Scene.destroy", {ApiFixture::entity(c)});
    check(c.valid(), "Scene.destroy espera al final del frame");
    t.frame();
    check(!c.valid() && player.valid(), "y al final del frame lo destruye");
    t.call("Scene.destroy", {ApiFixture::entity(c)});
    t.call("Scene.destroy", {Value()});
    check(true, "Scene.destroy de algo ya destruido o nil no falla");

    std::filesystem::create_directories(kRoot / "Escenas");
    std::ofstream(kRoot / "Escenas" / "Nivel2.crscene") << "{}";
    check(t.call("Scene.load", {Value("Nivel2")}).truthy() &&
              t.scripts.takeSceneRequest() == kRoot / "Escenas" / "Nivel2.crscene",
          "Scene.load pide la escena por su nombre");
    check(!t.call("Scene.load", {Value("Nivel9")}).truthy() && t.scripts.takeSceneRequest().empty() &&
              t.logged("no existe la escena \"Nivel9\""),
          "Scene.load de una escena que no existe: false");
    t.scripts.setSceneName("Pueblo");
    check(t.call("Scene.name").asString() == "Pueblo", "Scene.name");

    t.world.setOrigin(ecs::DVec3{1000.5, 0.0, -20.0});
    const Value origin = t.call("Scene.origin");
    check(origin.size() == 3 && origin[0].asNumber() == 1000.5 && origin[2].asNumber() == -20.0, "Scene.origin: lista x, y, z");
    const Value absolute = t.call("Scene.toAbsolute", {Value(Vec3{1.0f, 2.0f, 3.0f})});
    check(absolute.size() == 3 && absolute[0].asNumber() == 1001.5 && absolute[1].asNumber() == 2.0 && absolute[2].asNumber() == -17.0,
          "Scene.toAbsolute");
    const Vec3 local = t.call("Scene.toLocal", {Value(1001.5), Value(2.0), Value(-17.0)}).asVec3();
    check(near(local.x, 1.0) && near(local.y, 2.0) && near(local.z, 3.0), "Scene.toLocal");
    const json r = t.bridge({{"fn", "Scene.origin"}});
    check(r["ok"] == true && r["result"].is_array() && r["result"][0] == 1000.5, "Scene.origin por el puente: [x, y, z]");
    const json f = t.bridge({{"fn", "Scene.find"}, {"args", {"Jugador"}}});
    check(f["ok"] == true && f["result"]["$e"] == entt::to_integral(player.handle()) + 1, "Scene.find por el puente");
    check(throws(t, "Scene.find"), "Scene.find sin nombre: error");
}

void testPrefsAndGame() {
    std::printf("Prefs y Game\n");
    const std::filesystem::path file = kRoot / "prefs.txt";
    {
        ApiFixture t;
        t.scripts.setPrefsFile(file);
        t.call("Prefs.setInt", {Value("vidas"), Value(3)});
        t.call("Prefs.setFloat", {Value("volumen"), Value(0.5)});
        t.call("Prefs.setString", {Value("nombre"), Value("Ana")});
        t.call("Prefs.setString", {Value("borrar"), Value("x")});
        check(t.call("Prefs.getInt", {Value("vidas")}).asNumber() == 3.0 && t.call("Prefs.getInt", {Value("nada"), Value(7)}).asNumber() == 7.0,
              "setInt / getInt (con valor por defecto)");
        check(near(t.call("Prefs.getFloat", {Value("volumen")}).asNumber(), 0.5) &&
                  near(t.call("Prefs.getFloat", {Value("nada"), Value(1.5)}).asNumber(), 1.5),
              "setFloat / getFloat");
        check(t.call("Prefs.getString", {Value("nombre")}).asString() == "Ana" &&
                  t.call("Prefs.getString", {Value("nada"), Value("?")}).asString() == "?" &&
                  t.call("Prefs.getString", {Value("nada")}).asString().empty(),
              "setString / getString");
        t.call("Prefs.deleteKey", {Value("borrar")});
        check(t.call("Prefs.hasKey", {Value("nombre")}).truthy() && !t.call("Prefs.hasKey", {Value("borrar")}).truthy(),
              "hasKey y deleteKey");
        t.scripts.stop();
        t.scripts.start(t.world);
        check(t.call("Prefs.getString", {Value("nombre")}).asString() == "Ana", "siguen al parar y volver a empezar");
        check(!t.scripts.takeQuitRequest(), "(sin peticion de salir)");
        t.call("Game.quit");
        check(t.scripts.takeQuitRequest() && !t.scripts.takeQuitRequest(), "Game.quit pide salir (una vez)");
    }
    {
        ApiFixture t;
        t.scripts.setPrefsFile(file);
        check(t.call("Prefs.getInt", {Value("vidas")}).asNumber() == 3.0 && t.call("Prefs.getString", {Value("nombre")}).asString() == "Ana",
              "se guardan en el archivo (otra partida las lee)");
        t.call("Prefs.deleteAll");
        check(!t.call("Prefs.hasKey", {Value("vidas")}).truthy(), "deleteAll");
    }
    {
        ApiFixture t;
        t.scripts.setPrefsFile(file);
        check(!t.call("Prefs.hasKey", {Value("nombre")}).truthy(), "deleteAll tambien vacia el archivo");
    }
}

void testProfiler() {
    std::printf("Profiler\n");
    ApiFixture t;
    prof::reset();
    prof::setEnabled(true);
    prof::setCaptureFolder(kRoot / "Capturas");
    for (int f = 0; f < 3; ++f) {
        prof::beginFrame();
        t.call("Profiler.begin", {Value("MiZona")});
        t.call("Profiler.counter", {Value("Monedas"), Value(7)});
        t.call("Profiler.finish");
        prof::endFrame();
    }
    const Value zones = t.call("Profiler.zones", {Value(5)});
    bool found = false;
    for (const Value& z : zones.items()) found = found || (z["name"].asString() == "MiZona" && z["ms"].isNumber() && z["self"].isNumber());
    check(found, "Profiler.begin / finish: una zona propia en Profiler.zones");
    bool counter = false;
    for (const auto& s : prof::counters(3)) counter = counter || (s.name == "Monedas" && s.last == 7.0);
    check(counter, "Profiler.counter");
    check(t.call("Profiler.frameMs").asNumber() >= 0.0, "Profiler.frameMs");
    const std::string capture = t.call("Profiler.capture", {Value(3)}).asString();
    check(!capture.empty() && std::filesystem::exists(capture), "Profiler.capture guarda un .crtrace y da su ruta");
}

void testCVar() {
    std::printf("CVar\n");
    ApiFixture t;
    check(t.call("CVar.register", {Value("prueba.Vidas"), Value(3), Value("Vidas al empezar")}).asString() == "3",
          "CVar.register de un numero devuelve su valor");
    check(t.call("CVar.get", {Value("prueba.Vidas")}).isNumber() && t.call("CVar.get", {Value("prueba.Vidas")}).asNumber() == 3.0,
          "CVar.get da un numero");
    check(t.call("CVar.set", {Value("prueba.Vidas"), Value(5)}).truthy() && t.call("CVar.get", {Value("prueba.Vidas")}).asNumber() == 5.0,
          "CVar.set");
    check(t.call("CVar.register", {Value("prueba.Vidas"), Value(9)}).asString() == "5", "registrarla otra vez no cambia su valor");
    t.call("CVar.register", {Value("prueba.Activo"), Value(true)});
    t.call("CVar.register", {Value("prueba.Nombre"), Value("Ana")});
    check(t.call("CVar.get", {Value("prueba.Activo")}).isBool() && t.call("CVar.get", {Value("prueba.Nombre")}).asString() == "Ana",
          "bool y texto");
    check(t.call("CVar.register", {Value("prueba.Activo"), Value("otro tipo")}).isNil() && t.logged("con otro tipo"),
          "registrar con otro tipo: nil y un aviso");
    check(t.call("CVar.get", {Value("prueba.NoExiste")}).isNil() && !t.call("CVar.exists", {Value("prueba.NoExiste")}).truthy() &&
              t.call("CVar.exists", {Value("prueba.Vidas")}).truthy(),
          "CVar.get de una que no existe: nil; CVar.exists");
    check(!t.call("CVar.set", {Value("prueba.NoExiste"), Value(1)}).truthy() && t.logged("CVar.set:"), "CVar.set de una que no existe: false");
    const Value list = t.call("CVar.list", {Value("prueba.")});
    std::set<std::string> names;
    for (const Value& v : list.items()) names.insert(v.asString());
    check(names.size() == 3 && names.count("prueba.Vidas") == 1, "CVar.list con filtro");
    check(t.call("CVar.list").size() > list.size(), "CVar.list sin filtro: todas");
    cvar::Registry::instance().clearDynamic();
}

void testPhysicsAndAudio() {
    std::printf("Physics y Audio\n");
    ApiFixture t(false);
    ecs::Entity ground = t.world.create("Suelo");
    ground.add<physics::BoxCollider>().size = Vec3{20.0f, 1.0f, 20.0f};
    ecs::Entity a = t.world.create("A");
    a.add<physics::BoxCollider>();
    a.setWorldPosition(Vec3{30.0f, 0.0f, 0.0f});
    ecs::Entity b = t.world.create("B");
    b.add<physics::BoxCollider>();
    b.setWorldPosition(Vec3{-30.0f, 0.0f, 0.0f});
    t.world.create("Oyente").add<audio::AudioListener>();
    // P6: un techo en la capa 8 ("Techo") sobre una esquina del suelo.
    ecs::Entity roof = t.world.create("Techo");
    roof.add<physics::BoxCollider>().size = Vec3{2.0f, 1.0f, 2.0f};
    roof.setWorldPosition(Vec3{5.0f, 3.0f, 5.0f});
    roof.get<ecs::EntityInfo>().layer = 8;
    t.scripts.start(t.world);
    check(t.call("Physics.raycast", {Value(Vec3{0, 5, 0}), Value(Vec3{0, -1, 0})}).isNil(), "sin fisica: raycast nil");
    physics::PhysicsSystem physics;
    physics.start(t.world);
    physics.update(t.world, 0.0f, false);
    t.scripts.setPhysics(&physics);
    const Value hit = t.call("Physics.raycast", {Value(Vec3{0, 5, 0}), Value(Vec3{0, -1, 0}), Value(100)});
    check(hit.isObject() && hit["entity"].asEntity() == ground.handle() && near(hit["point"].asVec3().y, 0.5, 1e-3) &&
              hit["normal"].asVec3().y > 0.99f && near(hit["distance"].asNumber(), 4.5, 1e-3),
          "Physics.raycast: {entity, point, normal, distance}");
    check(t.call("Physics.raycast", {Value(Vec3{0, 5, 0}), Value(Vec3{0, 1, 0})}).isNil() &&
              t.call("Physics.raycast", {Value(Vec3{0, 5, 0}), Value(Vec3{0, -1, 0}), Value(2)}).isNil(),
          "sin choque (o mas lejos que la distancia): nil");
    t.call("Physics.ignoreCollision", {ApiFixture::entity(a), ApiFixture::entity(b)});
    check(physics.collisionIgnored(a, b), "Physics.ignoreCollision");
    t.call("Physics.ignoreCollision", {ApiFixture::entity(a), ApiFixture::entity(b), Value(false)});
    check(!physics.collisionIgnored(a, b), "Physics.ignoreCollision(a, b, false) lo deshace");

    // P6: mascara de capas, sphereCast y overlapSphere.
    {
        physics::PhysicsSettings settings = physics.settings();
        settings.layer_names[8] = "Techo";
        physics.setSettings(settings);
        const Value down{Vec3{0, -1, 0}};
        const Value top = t.call("Physics.raycast", {Value(Vec3{5, 6, 5}), down, Value(100)});
        check(top.isObject() && top["entity"].asEntity() == roof.handle() && top["layer"].asNumber() == 8.0,
              "Physics.raycast sin mascara: el techo (layer 8)");
        const double no_roof = static_cast<double>(physics::kDefaultRaycastLayers & ~physics::layerBit(8));
        const Value under = t.call("Physics.raycast", {Value(Vec3{5, 6, 5}), down, Value(100), Value(no_roof)});
        check(under.isObject() && under["entity"].asEntity() == ground.handle(), "Physics.raycast con mascara: atraviesa el techo");
        const Value only_roof = t.call("Physics.layerMask", {Value("Techo")});
        check(only_roof.asNumber() == static_cast<double>(physics::layerBit(8)), "Physics.layerMask(\"Techo\")");
        check(t.call("Physics.raycast", {Value(Vec3{0, 6, 0}), down, Value(100), only_roof}).isNil(),
              "solo la capa del techo: el suelo no cuenta");
        const Value ignored = t.call("Physics.raycast", {Value(Vec3{5, 6, 5}), down, Value(100), Value(), ApiFixture::entity(roof)});
        check(ignored.isObject() && ignored["entity"].asEntity() == ground.handle(), "Physics.raycast ignorando una entidad");
        // Una esfera de 0,5 m que baja al lado del techo (a 1,3 m de su centro en X) lo roza.
        const Value sphere = t.call("Physics.sphereCast", {Value(Vec3{6.3f, 6, 5}), Value(0.5), down, Value(100)});
        check(sphere.isObject() && sphere["entity"].asEntity() == roof.handle(), "Physics.sphereCast: la esfera roza el techo");
        const Value thin = t.call("Physics.raycast", {Value(Vec3{6.3f, 6, 5}), down, Value(100)});
        check(thin.isObject() && thin["entity"].asEntity() == ground.handle(), "(el rayo fino por ahi no lo toca)");
        const Value masked = t.call("Physics.sphereCast", {Value(Vec3{6.3f, 6, 5}), Value(0.5), down, Value(100),
                                                           Value(Value::Array{Value("Default")})});
        check(masked.isObject() && masked["entity"].asEntity() == ground.handle(), "Physics.sphereCast con lista de capas");
        const Value near_roof = t.call("Physics.overlapSphere", {Value(Vec3{5, 3, 5}), Value(0.5)});
        check(near_roof.isArray() && near_roof.size() == 1 && near_roof[0].asEntity() == roof.handle(), "Physics.overlapSphere");
        check(t.call("Physics.overlapSphere", {Value(Vec3{5, 3, 5}), Value(0.5), Value("Default")}).size() == 0,
              "Physics.overlapSphere con mascara");
    }
    t.scripts.setPhysics(nullptr);
    physics.stop();

    struct Shot {
        std::string clip;
        Vec3 position;
        float volume = 0.0f;
        bool spatial = false;
    };
    std::vector<Shot> shots;
    audio::setOneShotListener([&](const std::string& clip, const Vec3& p, float volume, bool spatial) {
        shots.push_back(Shot{clip, p, volume, spatial});
    });
    t.call("Audio.playOneShot", {Value("Audio/golpe.wav")});
    check(shots.empty(), "sin audio no suena nada");
    audio::AudioSystem sound{audio::AudioSystem::Offline{}};
    t.scripts.setAudio(&sound);
    t.call("Audio.playOneShot", {Value("Audio/golpe.wav"), Value(Vec3{1, 2, 3}), Value(0.5)});
    t.call("Audio.playOneShot", {Value("Audio/clic.wav")});
    check(shots.size() == 2 && shots[0].clip == "Audio/golpe.wav" && shots[0].spatial && near(shots[0].position.z, 3.0) &&
              near(shots[0].volume, 0.5) && !shots[1].spatial && near(shots[1].volume, 1.0),
          "Audio.playOneShot (3D con posicion, 2D sin ella)");
    audio::setOneShotListener({});
    const audio::AudioListener& listener = t.world.findByName("Oyente").get<audio::AudioListener>();
    check(t.call("Audio.occlusion").truthy(), "Audio.occlusion");
    t.call("Audio.setOcclusion", {Value(false)});
    check(!listener.occlusion && !t.call("Audio.occlusion").truthy(), "Audio.setOcclusion");
    t.call("Audio.setLowPass", {Value(true), Value(500)});
    check(listener.low_pass && near(listener.low_pass_cutoff, 500.0), "Audio.setLowPass");
    t.call("Audio.setLowPass", {Value(false)});
    check(!listener.low_pass && near(listener.low_pass_cutoff, 500.0), "Audio.setLowPass sin frecuencia la conserva");
    check(t.call("Audio.reverbLevel").asNumber() == 0.0, "Audio.reverbLevel");
    t.scripts.setAudio(nullptr);
}

void testRandom() {
    std::printf("Random\n");
    ApiFixture t;
    t.call("Random.seed", {Value(42)});
    const double first = t.call("Random.value").asNumber();
    t.call("Random.seed", {Value(42)});
    check(t.call("Random.value").asNumber() == first && first >= 0.0 && first < 1.0, "Random.seed repite la serie");
    bool ranges = true, ints = true, both = true;
    std::set<int> seen;
    for (int i = 0; i < 200; ++i) {
        const double r = t.call("Random.range", {Value(2), Value(3)}).asNumber();
        ranges = ranges && r >= 2.0 && r <= 3.0;
        const double n = t.call("Random.int", {Value(1), Value(6)}).asNumber();
        ints = ints && n >= 1.0 && n <= 6.0 && n == std::floor(n);
        seen.insert(static_cast<int>(n));
        const double s = t.call("Random.int", {Value(6), Value(1)}).asNumber();
        both = both && s >= 1.0 && s <= 6.0;
    }
    check(ranges, "Random.range");
    check(ints && seen.size() == 6 && both, "Random.int incluye los dos extremos (y en cualquier orden)");
    check(!t.call("Random.chance", {Value(0)}).truthy() && t.call("Random.chance", {Value(1)}).truthy(), "Random.chance");
    const double sign = t.call("Random.sign").asNumber();
    check(sign == 1.0 || sign == -1.0, "Random.sign");
    const auto length = [](const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); };
    check(near(length(t.call("Random.onUnitSphere").asVec3()), 1.0), "Random.onUnitSphere");
    check(length(t.call("Random.insideUnitSphere").asVec3()) <= 1.0001f, "Random.insideUnitSphere");
    const Vec3 circle = t.call("Random.insideUnitCircle").asVec3();
    check(circle.y == 0.0f && length(circle) <= 1.0001f, "Random.insideUnitCircle (en el suelo)");
    const core::Quat q = t.call("Random.rotation").asQuat();
    check(near(std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w), 1.0), "Random.rotation: un Quat unitario");
    const Value list(Value::Array{Value(10), Value(20), Value(30)});
    const double picked = t.call("Random.pick", {list}).asNumber();
    check(picked == 10.0 || picked == 20.0 || picked == 30.0, "Random.pick");
    check(t.call("Random.pick", {Value(Value::Array{})}).isNil(), "Random.pick de una lista vacia: nil");
    const Value shuffled = t.call("Random.shuffle", {Value(Value::Array{Value(1), Value(2), Value(3), Value(4), Value(5)})});
    std::vector<double> items;
    for (const Value& v : shuffled.items()) items.push_back(v.asNumber());
    std::sort(items.begin(), items.end());
    check(items == std::vector<double>{1, 2, 3, 4, 5}, "Random.shuffle: los mismos elementos");
    const json r = t.bridge({{"fn", "Random.int"}, {"args", {3, 3}}});
    check(r["ok"] == true && r["result"] == 3, "Random.int por el puente");
}

// --- Plantillas de prefab (ROADMAP 2 #16) --------------------------------------------

// La entidad y sus descendientes, en profundidad.
std::vector<ecs::Entity> subtreeOf(ecs::World& w, ecs::Entity root) {
    std::vector<ecs::Entity> out;
    std::vector<ecs::Entity> stack{root};
    while (!stack.empty()) {
        const ecs::Entity e = stack.back();
        stack.pop_back();
        out.push_back(e);
        const auto& children = e.children();
        for (auto it = children.rbegin(); it != children.rend(); ++it) stack.push_back(w.wrap(*it));
    }
    return out;
}

// Los registros de una instancia con sus UUID cambiados por su orden (#0, #1...):
// dos instancias iguales dan el mismo texto aunque sus UUID sean otros.
std::string normalized(ecs::World& w, ecs::Entity root) {
    const std::vector<ecs::Entity> list = subtreeOf(w, root);
    std::string all;
    for (const ecs::Entity& e : list) all += ecs::serializeEntityRecord(w, e) + "\n";
    for (std::size_t i = 0; i < list.size(); ++i) {
        const std::string id = list[i].uuid().toString();
        const std::string tag = "#" + std::to_string(i);
        for (std::size_t at = all.find(id); at != std::string::npos; at = all.find(id, at + tag.size())) all.replace(at, id.size(), tag);
    }
    return all;
}

void testPrefabTemplate() {
    std::printf("Plantillas de prefab\n");
    std::error_code ec;
    std::filesystem::remove_all(kRoot, ec);
    ecs::registerPrefabComponents();
    ecs::World w;
    // Torre (luz) > Cabeza (IK que apunta a la raiz, a la Cola de mas abajo y a algo de fuera) y Cola (PhysBones -> Cabeza).
    ecs::Entity outside = w.create("Fuera");
    ecs::Entity root = w.create("Torre");
    root.setLocalPosition(Vec3{1.0f, 2.0f, 3.0f});
    root.setTag("Enemy");
    root.add<ecs::Light>().intensity = 3.5f;
    ecs::Entity head = w.create("Cabeza", root);
    ecs::Entity tail = w.create("Cola", root);
    tail.get<ecs::EntityInfo>().active = false;
    auto& ik = head.add<ecs::InverseKinematics>();
    ik.left_hand.target = root.uuid();
    ik.right_hand.target = outside.uuid();
    ik.look_at = tail.uuid();
    ik.look_bone = "Cuello";
    tail.add<ecs::PhysBones>().colliders = {head.uuid(), outside.uuid()};
    std::string error;
    check(ecs::createPrefab(w, root, kRoot / "Prefabs" / "Torre.crprefab", &error), "(prefab de prueba)");
    const std::string text = ecs::readPrefabFile(kRoot / "Prefabs" / "Torre.crprefab");

    const auto prefab = ecs::compilePrefabTemplate(text);
    check(prefab != nullptr && ecs::prefabTemplateSize(*prefab) == 3 && ecs::prefabTemplateInternalRefs(*prefab) == 3 &&
              ecs::prefabTemplateBytes(*prefab) > 0,
          "compilePrefabTemplate: 3 registros y 3 referencias internas ya resueltas como indices");
    check(ecs::compilePrefabTemplate(std::string("{}")) == nullptr && ecs::compilePrefabTemplate(std::string("no")) == nullptr,
          "un texto que no es un prefab: nullptr");
    if (prefab == nullptr) return;

    const ecs::Entity by_json = ecs::instantiatePrefab(w, text);
    const ecs::Entity by_template = ecs::instantiatePrefabTemplate(w, *prefab);
    check(by_json.valid() && by_template.valid() && normalized(w, by_json) == normalized(w, by_template),
          "instantiatePrefabTemplate da lo mismo que instantiatePrefab (componentes, estado, PrefabLink, PrefabInstance)");

    const std::vector<ecs::Entity> copy = subtreeOf(w, by_template);
    const ecs::Entity c_head = copy.size() == 3 ? copy[1] : ecs::Entity{};
    const ecs::Entity c_tail = copy.size() == 3 ? copy[2] : ecs::Entity{};
    const auto* c_ik = c_head.valid() ? c_head.tryGet<ecs::InverseKinematics>() : nullptr;
    const auto* c_bones = c_tail.valid() ? c_tail.tryGet<ecs::PhysBones>() : nullptr;
    check(c_ik != nullptr && c_ik->left_hand.target == by_template.uuid() && c_ik->look_at == c_tail.uuid() &&
              c_ik->right_hand.target == outside.uuid() && c_ik->look_bone == "Cuello",
          "referencias a la copia (tambien a una entidad de mas abajo) y las de fuera tal cual");
    check(c_bones != nullptr && c_bones->colliders.size() == 2 && c_bones->colliders[0] == c_head.uuid() &&
              c_bones->colliders[1] == outside.uuid(),
          "listas con referencias");
    check(by_template.uuid() != root.uuid() && near(by_template.localPosition().y, by_json.localPosition().y) && by_template.tag() == "Enemy" &&
              !c_tail.activeSelf() && by_template.get<ecs::Light>().intensity == 3.5f &&
              by_template.has<ecs::PrefabInstance>() && head.has<ecs::PrefabLink>() && c_head.get<ecs::PrefabLink>().source == head.get<ecs::PrefabLink>().source,
          "UUID nuevos, Transform, tag, activo, componentes y enlaces del prefab");

    const ecs::Entity parked = ecs::instantiatePrefabTemplate(w, *prefab, {}, /*active=*/false);
    check(parked.valid() && !parked.activeSelf() && !subtreeOf(w, parked)[1].activeInHierarchy(),
          "active = false: la raiz queda desactivada (pool)");

    // Por tandas, con presupuesto 0 (una entidad por paso).
    ecs::PrefabSpawn spawn(prefab);
    spawn.setPosition(Vec3{0.0f, 9.0f, 0.0f});
    int steps = 0;
    while (!spawn.step(w, 0.0) && steps < 100) {
        ++steps;
        check(steps > 1 || !spawn.root().activeSelf(), "(mientras se crea, la raiz esta desactivada)");
    }
    check(spawn.done() && !spawn.failed() && steps >= 2 && spawn.root().activeSelf() && near(spawn.root().worldPosition().y, 9.0) &&
              spawn.progress() == 1.0f,
          "PrefabSpawn con la plantilla: por tandas y activa al terminar");
    spawn.root().setLocalPosition(by_template.localPosition());
    check(normalized(w, spawn.root()) == normalized(w, by_template), "y lo mismo que de una vez");
    ecs::PrefabSpawn legacy(ecs::parsePrefabDocument(text));
    while (!legacy.step(w, 100.0)) {
    }
    check(legacy.done() && !legacy.failed() && normalized(w, legacy.root()) == normalized(w, by_json),
          "PrefabSpawn con el documento (compila en el primer paso)");
}

// --- Pool: Scene.pool / acquire / release ------------------------------------------------

void testPool() {
    std::printf("Pool\n");
    std::error_code ec;
    std::filesystem::remove_all(kRoot, ec);
    ecs::registerPrefabComponents();
    ApiFixture t(false);
    t.scripts.setAssetsRoot(kRoot);
    {
        ecs::Entity bullet = t.world.create("Bala");
        bullet.setLocalScale(Vec3{0.5f, 0.5f, 0.5f});
        t.world.create("Estela", bullet);
        std::string error;
        check(ecs::createPrefab(t.world, bullet, kRoot / "Prefabs" / "Bala.crprefab", &error), "(prefab de prueba)");
        t.world.destroy(bullet);
    }
    t.scripts.start(t.world);
    const auto count = [&t]() { return t.call("Scene.poolCount", {Value("Prefabs/Bala")}).asNumber(); };
    check(t.call("Scene.pool", {Value("Prefabs/Bala"), Value(3)}).truthy(), "Scene.pool: true");
    for (int i = 0; i < 2000 && count() < 3; ++i) {
        t.frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::size_t parked = 0;
    for (const entt::entity h : t.world.registry().view<ecs::PrefabInstance>()) {
        const ecs::Entity e = t.world.wrap(h);
        if (e.name() == "Bala" && !e.activeSelf()) ++parked;
    }
    check(count() == 3.0 && parked == 3, "se llena por tandas: 3 copias desactivadas");

    const Value a = t.call("Scene.acquire", {Value("Prefabs/Bala"), Value(Vec3{4.0f, 5.0f, 6.0f}), Value(Vec3{0.0f, 90.0f, 0.0f})});
    ecs::Entity ea = t.world.wrap(a.asEntity());
    check(ea.valid() && ea.activeSelf() && near(ea.worldPosition().z, 6.0) && near(ea.localEulerDegrees().y, 90.0, 1e-2) &&
              count() == 2.0,
          "Scene.acquire: sale del pool activa, en su sitio y con su giro");
    ea.setLocalScale(Vec3{3.0f, 3.0f, 3.0f});
    check(t.call("Scene.release", {a}).truthy() && !ea.activeSelf() && count() == 3.0, "Scene.release: desactivada y de vuelta");
    check(t.call("Scene.release", {a}).truthy() && count() == 3.0, "devolverla dos veces no la duplica");
    ecs::Entity parent = t.world.create("Arma");
    const Value b = t.call("Scene.acquire", {Value("Prefabs/Bala"), Value(), Value(), ApiFixture::entity(parent)});
    ecs::Entity eb = t.world.wrap(b.asEntity());
    check(eb == ea && eb.parent() == parent && near(eb.localScale().x, 0.5) && eb.activeSelf(),
          "la misma copia otra vez: con el Transform del prefab y bajo el padre pedido");
    t.call("Scene.release", {b});
    check(!eb.parent().valid() && !eb.activeSelf(), "al devolverla vuelve a la raiz (sobrevive a su padre)");

    // Mas de las que hay: se crean al momento.
    std::vector<Value> out;
    for (int i = 0; i < 5; ++i) out.push_back(t.call("Scene.acquire", {Value("Prefabs/Bala.crprefab")}));
    std::set<entt::entity> distinct;
    for (const Value& v : out) distinct.insert(v.asEntity());
    check(distinct.size() == 5 && count() == 0.0, "pool vacio: crea copias nuevas al momento");
    for (const Value& v : out) t.call("Scene.release", {v});
    check(count() == 5.0, "y todas vuelven");

    // Una copia que se destruye por fuera sale del pool.
    const Value d = t.call("Scene.acquire", {Value("Prefabs/Bala")});
    t.call("Scene.destroy", {d});
    t.frame();
    check(!t.world.wrap(d.asEntity()).valid() && count() == 4.0 && t.call("Scene.acquire", {Value("Prefabs/Bala")}).isEntity(),
          "destruida por fuera: el pool la olvida");

    ecs::Entity loose = t.world.create("Suelta");
    check(!t.call("Scene.release", {ApiFixture::entity(loose)}).truthy(), "Scene.release de algo que no es de un pool: false");
    t.frame();
    check(!loose.valid(), "y la destruye como Scene.destroy");
    check(t.call("Scene.acquire", {Value("Prefabs/NoExiste")}).isNil() && t.logged("Scene.acquire: no existe el prefab"),
          "un prefab que no existe: nil y un error");

    // Scene.instantiateAsync con la misma cache de plantillas.
    const double spawn = t.call("Scene.instantiateAsync", {Value("Prefabs/Bala"), Value(Vec3{0.0f, 3.0f, 0.0f})}).asNumber();
    for (int i = 0; i < 2000 && t.call("Scene.spawnProgress", {Value(spawn)}).asNumber() < 1.0; ++i) {
        t.frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const ecs::Entity spawned = t.world.wrap(t.call("Scene.spawnResult", {Value(spawn)}).asEntity());
    check(spawned.valid() && spawned.activeSelf() && spawned.childCount() == 1 && near(spawned.worldPosition().y, 3.0) &&
              spawned.has<ecs::PrefabInstance>(),
          "Scene.instantiateAsync con la plantilla de la cache");

    // Scene.setTransforms (la de la API; los scripts de C++ usan su RPC).
    ecs::Entity m1 = t.world.create("M1");
    ecs::Entity m2 = t.world.create("M2", m1);
    const Value moved = t.call("Scene.setTransforms",
                               {Value(Value::Array{ApiFixture::entity(m1), ApiFixture::entity(m2)}),
                                Value(Value::Array{Value(Vec3{1.0f, 0.0f, 0.0f}), Value(Vec3{0.0f, 2.0f, 0.0f})}),
                                Value(Value::Array{Value(core::Quat{0.0f, 0.7071068f, 0.0f, 0.7071068f}), Value()}),
                                Value(Value::Array{Value(), Value(Vec3{2.0f, 2.0f, 2.0f})}), Value(true)});
    check(moved.asNumber() == 2.0 && near(m1.localPosition().x, 1.0) && near(m2.localPosition().y, 2.0) &&
              near(m1.localEulerDegrees().y, 90.0, 1e-2) && near(m2.localScale().z, 2.0) && near(m1.localScale().x, 1.0),
          "Scene.setTransforms: posiciones locales, giros y escalas (nil = no tocar)");
}

// --- Puente binario (ROADMAP 2 #18) --------------------------------------------------------

using cppproto::ValueTag;
using Bridge = scripting::api::NativeApi::BridgeOp;

void putNil(cppproto::Writer& w) { w.u8(static_cast<std::uint8_t>(ValueTag::Nil)); }
[[maybe_unused]] void putNumber(cppproto::Writer& w, double d) { w.u8(static_cast<std::uint8_t>(ValueTag::Number)).f64(d); }
void putString(cppproto::Writer& w, std::string_view s) { w.u8(static_cast<std::uint8_t>(ValueTag::String)).str(s); }
void putVec3(cppproto::Writer& w, const Vec3& v) { w.u8(static_cast<std::uint8_t>(ValueTag::Vec3)).f32(v.x).f32(v.y).f32(v.z); }
void putEntity(cppproto::Writer& w, const ecs::Entity& e) {
    w.u8(static_cast<std::uint8_t>(ValueTag::Entity)).u64(e.valid() ? entt::to_integral(e.handle()) + 1ull : 0ull);
}

struct Slot {
    std::uint32_t epoch = 0, id = 0, flags = 0;
};

Slot resolve(ApiFixture& t, std::string_view key) {
    cppproto::Writer w;
    w.str(key);
    std::vector<std::uint8_t> reply;
    t.api().bridgeBinary(Bridge::Resolve, w.bytes.data(), w.bytes.size(), reply);
    cppproto::Reader r(reply.data(), reply.size());
    Slot s;
    s.epoch = r.u32();
    s.id = r.u32();
    s.flags = r.u32();
    return s;
}

// Una llamada: la cabecera y lo que escriba `body` (self, n y los argumentos).
template <typename Body>
std::vector<std::uint8_t> callBinary(ApiFixture& t, Bridge op, const Slot& s, cppproto::ApiOp what, Body&& body,
                                     std::string* warning = nullptr) {
    cppproto::Writer w;
    w.u32(s.epoch).u32(s.id).u32(static_cast<std::uint32_t>(what));
    body(w);
    std::vector<std::uint8_t> reply;
    const std::string message = t.api().bridgeBinary(op, w.bytes.data(), w.bytes.size(), reply);
    if (warning != nullptr) *warning = message;
    return reply;
}

void testBinaryBridge() {
    std::printf("Puente binario\n");
    ApiFixture t(false);
    ecs::Entity player = t.world.create("Jugador");
    t.scripts.start(t.world);

    const Slot find = resolve(t, "Scene.find");
    const Slot again = resolve(t, "Scene.find");
    check(find.epoch != 0 && find.epoch == t.api().bridgeEpoch() && find.id == again.id && find.flags == 0,
          "ApiResolve: un id por clave, con la epoca del NativeApi (Scene.find devuelve algo: no va en lote)");
    check((resolve(t, "Scene.destroy").flags & cppproto::ApiBatchable) != 0 && resolve(t, "Entity:translate").flags != 0,
          "las que no devuelven nada (su Doc) pueden ir en lote");

    auto reply = callBinary(t, Bridge::Call, find, cppproto::ApiOp::Call, [](cppproto::Writer& w) {
        putNil(w);
        w.u32(1);
        putString(w, "Jugador");
    });
    {
        cppproto::Reader r(reply.data(), reply.size());
        const auto status = static_cast<cppproto::ApiStatus>(r.u32());
        const auto tag = static_cast<ValueTag>(r.u8());
        const std::uint64_t id = r.u64();
        check(r.ok() && status == cppproto::ApiStatus::Ok && tag == ValueTag::Entity && id == entt::to_integral(player.handle()) + 1ull,
              "ApiCall por id: Scene.find devuelve la entidad en binario");
    }

    // Metodo de Entity (clave fija) y del tipo de self (":translate").
    const Slot translate = resolve(t, "Entity:translate");
    const Slot member = resolve(t, ":translate");
    for (const Slot& s : {translate, member}) {
        reply = callBinary(t, Bridge::Call, s, cppproto::ApiOp::Call, [&](cppproto::Writer& w) {
            putEntity(w, player);
            w.u32(1);
            putVec3(w, Vec3{1.0f, 0.0f, 0.0f});
        });
    }
    check(near(player.worldPosition().x, 2.0), "Entity:translate y \":translate\" (el metodo del tipo de self)");

    // Propiedades: get con respuesta, set en lote (Send) sin respuesta.
    const Slot name = resolve(t, "Entity:name");
    std::string warning;
    reply = callBinary(t, Bridge::Send, name, cppproto::ApiOp::Set, [&](cppproto::Writer& w) {
        putEntity(w, player);
        w.u32(1);
        putString(w, "Heroe");
    }, &warning);
    check(reply.empty() && warning.empty() && player.name() == "Heroe", "set en lote (ApiCallSend): sin respuesta");
    reply = callBinary(t, Bridge::Call, name, cppproto::ApiOp::Get, [&](cppproto::Writer& w) {
        putEntity(w, player);
        w.u32(0);
    });
    {
        cppproto::Reader r(reply.data(), reply.size());
        const auto status = static_cast<cppproto::ApiStatus>(r.u32());
        const auto tag = static_cast<ValueTag>(r.u8());
        check(status == cppproto::ApiStatus::Ok && tag == ValueTag::String && r.str() == "Heroe", "get de una propiedad");
    }
    t.frame(0.5f);
    reply = callBinary(t, Bridge::Call, resolve(t, "Time.time"), cppproto::ApiOp::Get, [](cppproto::Writer& w) {
        putNil(w);
        w.u32(0);
    });
    {
        cppproto::Reader r(reply.data(), reply.size());
        r.u32();
        const auto tag = static_cast<ValueTag>(r.u8());
        check(tag == ValueTag::Number && near(r.f64(), 0.5), "propiedad de una tabla (Time.time)");
    }
    reply = callBinary(t, Bridge::Call, resolve(t, "Scene.origin"), cppproto::ApiOp::Call, [](cppproto::Writer& w) {
        putNil(w);
        w.u32(0);
    });
    {
        cppproto::Reader r(reply.data(), reply.size());
        r.u32();
        const auto tag = static_cast<ValueTag>(r.u8());
        check(tag == ValueTag::Array && r.u32() == 3, "listas en binario");
    }

    // Errores: otra epoca, algo que no existe, self que ya no existe y un valor en lote.
    Slot stale = find;
    stale.epoch += 1000;
    reply = callBinary(t, Bridge::Call, stale, cppproto::ApiOp::Call, [](cppproto::Writer& w) {
        putNil(w);
        w.u32(0);
    });
    check(cppproto::Reader(reply.data(), reply.size()).u32() == static_cast<std::uint32_t>(cppproto::ApiStatus::Stale),
          "un id de otra epoca: Stale (el SDK los vuelve a pedir)");
    reply = callBinary(t, Bridge::Call, resolve(t, "Scene.noExiste"), cppproto::ApiOp::Call, [](cppproto::Writer& w) {
        putNil(w);
        w.u32(0);
    });
    {
        cppproto::Reader r(reply.data(), reply.size());
        const auto status = static_cast<cppproto::ApiStatus>(r.u32());
        check(status == cppproto::ApiStatus::Error && r.str().find("no existe") != std::string::npos,
              "lo que no existe: el error de siempre");
    }
    reply = callBinary(t, Bridge::Call, translate, cppproto::ApiOp::Call, [](cppproto::Writer& w) {
        putEntity(w, ecs::Entity{});
        w.u32(0);
    });
    {
        cppproto::Reader r(reply.data(), reply.size());
        const auto status = static_cast<cppproto::ApiStatus>(r.u32());
        check(status == cppproto::ApiStatus::Error && r.str().find("ya no existe") != std::string::npos, "self nulo: error");
    }
    callBinary(t, Bridge::Send, find, cppproto::ApiOp::Call, [](cppproto::Writer& w) {
        putNil(w);
        w.u32(1);
        putString(w, "Heroe");
    }, &warning);
    check(warning.find("devolvio un valor") != std::string::npos, "una funcion con resultado enviada en lote: aviso");
    std::vector<std::uint8_t> broken{1, 2, 3};
    reply.clear();
    t.api().bridgeBinary(Bridge::Call, broken.data(), broken.size(), reply);
    check(cppproto::Reader(reply.data(), reply.size()).u32() == static_cast<std::uint32_t>(cppproto::ApiStatus::Error),
          "un mensaje roto: error, sin leer de mas");
}

}  // namespace

int main() {
    testTime();
    testScene();
    testPrefsAndGame();
    testProfiler();
    testCVar();
    testPhysicsAndAudio();
    testRandom();
    testPrefabTemplate();
    testPool();
    testBinaryBridge();
    std::error_code ec;
    std::filesystem::remove_all(kRoot, ec);
    return finish();
}
