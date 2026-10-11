// Pruebas de native/CoreApi.cpp: Time, Scene, Prefs, Game, Profiler, CVar,
// Physics, Audio y Random.

#include "ApiTest.h"

#include "CramionCore/audio/Audio.h"
#include "CramionCore/cvar/CVar.h"
#include "CramionCore/ecs/Prefab.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/profiling/Profiler.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

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

}  // namespace

int main() {
    testTime();
    testScene();
    testPrefsAndGame();
    testProfiler();
    testCVar();
    testPhysicsAndAudio();
    testRandom();
    std::error_code ec;
    std::filesystem::remove_all(kRoot, ec);
    return finish();
}
