// Pruebas de native/EffectsApi.cpp: VFX, fisica 2D (Rigidbody2D, Physics2D y
// los eventos OnCollisionEnter2D...), sprites, tilemaps, destruccion (y
// OnBreak), vehiculos, Motion Matching y Replay.

#include "ApiTest.h"

#include "CramionCore/Uuid.h"
#include "CramionCore/anim/MotionMatching.h"
#include "CramionCore/asset/AssetDatabase.h"
#include "CramionCore/asset/AssetManager.h"
#include "CramionCore/physics/Destruction.h"
#include "CramionCore/physics/Fracture.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/replay/Replay.h"
#include "CramionCore/twod/System2D.h"
#include "CramionCore/vfx/VisualEffect.h"

#include <cmath>
#include <filesystem>
#include <string>
#include <tuple>
#include <vector>

using namespace cramion;
using namespace cramion::apitest;
using core::Vec3;

namespace {

bool near(const Vec3& a, const Vec3& b, float eps = 1e-3f) { return core::length(a - b) < eps; }

std::uint64_t entityId(const ecs::Entity& e) { return static_cast<std::uint64_t>(entt::to_integral(e.handle())) + 1; }

json selfOf(const ecs::Entity& e) { return json{{"$e", entityId(e)}}; }

// Los mensajes que llegan a los scripts de C++ (Script::onMessage).
struct Messages {
    std::vector<std::tuple<entt::entity, std::string, json>> list;
    void listen(scripting::ScriptSystem& scripts) {
        scripts.setMessageListener([this](ecs::Entity e, const std::string& method, const std::string& value) {
            list.emplace_back(e.handle(), method, json::parse(value, nullptr, false));
        });
    }
    const json* find(const ecs::Entity& target, const std::string& method) const {
        for (const auto& [h, m, v] : list) {
            if (h == target.handle() && m == method) return &v;
        }
        return nullptr;
    }
    int count(const std::string& method) const {
        int n = 0;
        for (const auto& [h, m, v] : list) n += m == method ? 1 : 0;
        return n;
    }
};

void testVfx() {
    std::printf("VFX\n");
    ApiFixture t;
    ecs::Entity e = t.world.create("Humo");
    const Value self = ApiFixture::entity(e);
    // Sin sistema de VFX: no hace nada (y no falla).
    t.call("Entity:playEffect", {}, self);
    check(!t.call("Entity:isEffectPlaying", {}, self).truthy() && t.get("effectParticles", self).asNumber() == 0 &&
              t.call("Entity:getEffectFloat", {Value("Rate")}, self).isNil(),
          "sin VfxSystem: parado, 0 particulas, parametro nil");
    vfx::VfxSystem system;
    vfx::setActiveSystem(&system);
    t.call("Entity:playEffect", {}, self);
    check(t.call("Entity:isEffectPlaying", {}, self).truthy(), "playEffect");
    t.call("Entity:pauseEffect", {}, self);
    check(!t.call("Entity:isEffectPlaying", {}, self).truthy(), "pauseEffect() pausa");
    t.call("Entity:pauseEffect", {Value(false)}, self);
    check(t.call("Entity:isEffectPlaying", {}, self).truthy(), "pauseEffect(false) sigue");
    t.call("Entity:stopEffect", {Value(true)}, self);
    check(!t.call("Entity:isEffectPlaying", {}, self).truthy(), "stopEffect");
    const json r = t.bridge({{"fn", "setEffectColor"}, {"self", selfOf(e)}, {"args", {"Color", {{"$v", {1, 0.5, 0}}}}}});
    t.call("Entity:setEffectFloat", {Value("Rate"), Value(200)}, self);
    t.call("Entity:setEffectVector", {Value("Viento"), Value(Vec3{1, 0, 0})}, self);
    t.call("Entity:setEffectBool", {Value("Activo"), Value(true)}, self);
    t.call("Entity:sendEffectEvent", {Value("Explode")}, self);
    check(r["ok"] == true, "parametros y eventos (tambien por el puente)");
    const json bad = t.bridge({{"fn", "setEffectFloat"}, {"self", selfOf(e)}, {"args", {"Rate", "mucho"}}});
    check(bad["ok"] == false && bad["error"].get<std::string>().find("numero") != std::string::npos, "un argumento mal: error");
    vfx::setActiveSystem(nullptr);
}

void testPhysics2D() {
    std::printf("Fisica 2D y eventos\n");
    ApiFixture t;
    Messages messages;
    messages.listen(t.scripts);
    ecs::Entity ground = t.world.create("Suelo");
    ground.add<twod::BoxCollider2D>().size = core::Vec2{20.0f, 1.0f};
    ecs::Entity box = t.world.create("Caja");
    box.setWorldPosition(Vec3{0.0f, 5.0f, 0.0f});
    box.add<twod::BoxCollider2D>();
    box.add<twod::Rigidbody2D>();
    const Value self = ApiFixture::entity(box);

    // Sin System2D: valores por defecto.
    check(near(t.get("velocity2D", self).asVec3(), Vec3{}) && t.call("Physics2D.raycast", {Value(Vec3{0, 5, 0}), Value(Vec3{0, -1, 0})}).isNil() &&
              t.call("Physics2D.overlapPoint", {Value(Vec3{})}).size() == 0 &&
              near(t.call("Physics2D.getGravity").asVec3(), Vec3{0, -9.81f, 0}),
          "sin fisica 2D: cero, nil y listas vacias");

    twod::System2D system;
    system.start(t.world);
    t.set("velocity2D", Value(Vec3{3, 0, 0}), self);
    check(near(t.get("velocity2D", self).asVec3(), Vec3{3, 0, 0}), "velocity2D");
    t.set("angularVelocity2D", Value(90), self);
    check(std::abs(t.get("angularVelocity2D", self).asNumber() - 90.0) < 0.01, "angularVelocity2D en grados/s");
    t.set("velocity2D", Value(Vec3{}), self);
    t.set("angularVelocity2D", Value(0), self);
    t.call("Entity:addForce2D", {Value(Vec3{0, 5, 0}), Value("impulse")}, self);
    check(std::abs(t.get("velocity2D", self).asVec3().y - 5.0f) < 0.01f, "addForce2D impulse (masa 1)");
    t.call("Entity:addForce2D", {Value(Vec3{2, 0, 0}), Value("VelocityChange")}, self);
    check(std::abs(t.get("velocity2D", self).asVec3().x - 2.0f) < 0.01f, "addForce2D velocity (mayusculas da igual)");
    t.set("velocity2D", Value(Vec3{}), self);
    t.call("Entity:addTorque2D", {Value(1), Value("impulse")}, self);
    t.call("Entity:addForceAtPosition2D", {Value(Vec3{0, 0, 0}), Value(Vec3{0, 5, 0})}, self);
    t.call("Entity:movePosition2D", {Value(Vec3{0, 5, 0})}, self);
    t.set("angularVelocity2D", Value(0), self);
    check(!t.call("Entity:isSleeping2D", {}, self).truthy(), "isSleeping2D");
    t.call("Physics2D.setGravity", {Value(Vec3{0, -20, 0})});
    check(near(t.call("Physics2D.getGravity").asVec3(), Vec3{0, -20, 0}), "setGravity / getGravity");
    t.call("Physics2D.setGravity", {Value(Vec3{0, -9.81f, 0})});

    // Cae sobre el suelo: OnCollisionEnter2D a los dos (fase Events).
    for (int i = 0; i < 240; ++i) system.update(t.world, 1.0f / 60.0f, twod::System2D::Mode::Play);
    check(box.worldPosition().y > 0.8f && box.worldPosition().y < 1.2f, "la caja reposa en el suelo");
    t.frame();
    const json* on_box = messages.find(box, "OnCollisionEnter2D");
    const json* on_ground = messages.find(ground, "OnCollisionEnter2D");
    check(on_box != nullptr && (*on_box)["other"]["$e"] == entityId(ground) && (*on_box)["point"].contains("$v") &&
              (*on_box)["normal"].contains("$v") && (*on_box)["relativeVelocity"].contains("$v"),
          "OnCollisionEnter2D {other, point, normal, relativeVelocity} al que cae");
    check(on_ground != nullptr && (*on_ground)["other"]["$e"] == entityId(box), "y al suelo (con el otro)");
    check(on_box != nullptr && on_ground != nullptr &&
              std::abs((*on_box)["normal"]["$v"][1].get<double>() + (*on_ground)["normal"]["$v"][1].get<double>()) < 1e-4,
          "la normal, del uno hacia el otro");
    const std::size_t before = messages.list.size();
    t.frame();
    check(messages.list.size() == before, "cada evento se entrega una vez");
    for (int i = 0; i < 10; ++i) system.update(t.world, 1.0f / 60.0f, twod::System2D::Mode::Play);
    t.frame();
    check(messages.count("OnCollisionStay2D") > 0, "OnCollisionStay2D");

    // Consultas.
    const Value hit = t.call("Physics2D.raycast", {Value(Vec3{3, 5, 0}), Value(Vec3{0, -1, 0}), Value(20)});
    check(hit.isObject() && hit["entity"].asEntity() == ground.handle() && std::abs(hit["point"].asVec3().y - 0.5f) < 0.05f &&
              near(hit["normal"].asVec3(), Vec3{0, 1, 0}, 0.01f) && hit["distance"].asNumber() > 4.0,
          "Physics2D.raycast {entity, point, normal, distance, fraction}");
    check(t.call("Physics2D.raycast", {Value(Vec3{3, 5, 0}), Value(Vec3{0, 1, 0}), Value(20)}).isNil(), "raycast sin choque: nil");
    check(t.call("Physics2D.raycast", {Value(Vec3{3, 5, 0}), Value(Vec3{0, -1, 0}), Value(20), Value(0)}).isNil(),
          "mascara 0: nada");
    const Value all = t.call("Physics2D.raycastAll", {Value(Vec3{0, 5, 0}), Value(Vec3{0, -1, 0}), Value(20)});
    check(all.size() == 2 && all[0]["entity"].asEntity() == box.handle() && all[1]["entity"].asEntity() == ground.handle(),
          "raycastAll: del mas cercano al mas lejano");
    const Value circle = t.call("Physics2D.overlapCircle", {Value(box.worldPosition()), Value(0.3)});
    check(circle.size() == 1 && circle[0].asEntity() == box.handle(), "overlapCircle");
    check(t.call("Physics2D.overlapBox", {Value(Vec3{0, 0.5f, 0}), Value(Vec3{30, 4, 0}), Value(0)}).size() == 2, "overlapBox");
    check(t.call("Physics2D.overlapPoint", {Value(Vec3{8, 0.2f, 0})}).size() == 1, "overlapPoint");
    system.stop();

    // Sin listener de mensajes: los eventos se sacan igual (no se acumulan).
    t.scripts.setMessageListener(nullptr);
    t.frame();
    check(true, "sin listener no falla");
}

void testSprites() {
    std::printf("Sprites y tilemaps\n");
    ApiFixture t;
    ecs::Entity e = t.world.create("Heroe");
    const Value self = ApiFixture::entity(e);
    check(t.get("spriteFrame", self).asNumber() == 0 && !t.get("flipX", self).truthy() &&
              near(t.get("spriteColor", self).asVec3(), Vec3{1, 1, 1}) && t.get("spriteAnimation", self).asString().empty() &&
              !t.call("Entity:playSpriteAnimation", {Value("Correr")}, self).truthy(),
          "sin sprite: valores por defecto");
    e.add<twod::SpriteRenderer>();
    twod::SpriteAnimator& anim = e.add<twod::SpriteAnimator>();
    anim.clips.push_back(twod::SpriteClip{"Quieto", "0", 12.0f, true});
    anim.clips.push_back(twod::SpriteClip{"Correr", "1-4", 12.0f, false});
    check(t.call("Entity:playSpriteAnimation", {Value("Correr")}, self).truthy() && t.get("spriteAnimation", self).asString() == "Correr",
          "playSpriteAnimation y spriteAnimation");
    check(!t.call("Entity:playSpriteAnimation", {Value("Volar")}, self).truthy(), "un clip que no existe: false");
    check(!t.get("spriteAnimationFinished", self).truthy(), "spriteAnimationFinished");
    t.set("spriteFrame", Value(-3), self);
    check(t.get("spriteFrame", self).asNumber() == 0, "spriteFrame no baja de 0");
    t.set("spriteFrame", Value(4), self);
    t.set("flipX", Value(true), self);
    t.set("spriteColor", Value(Vec3{1, 0, 0}), self);
    const twod::SpriteRenderer& r = e.get<twod::SpriteRenderer>();
    check(r.frame == 4 && r.flip_x && near(r.color, Vec3{1, 0, 0}) && t.get("flipX", self).truthy(), "spriteFrame, flipX y spriteColor");

    ecs::Entity grid = t.world.create("Mapa");
    grid.setWorldPosition(Vec3{10, 0, 0});
    const Value map = ApiFixture::entity(grid);
    check(!t.call("Entity:setTile", {Value(1), Value(2), Value(5)}, map).truthy() &&
              t.call("Entity:getTile", {Value(1), Value(2)}, map).asNumber() == 0,
          "sin Tilemap: false y 0");
    grid.add<twod::Tilemap>();
    check(t.call("Entity:setTile", {Value(1), Value(2), Value(5)}, map).truthy() &&
              t.call("Entity:getTile", {Value(1), Value(2)}, map).asNumber() == 5 && grid.get<twod::Tilemap>().getTile(1, 2, 0) == 5,
          "setTile / getTile (capa 1 = la primera)");
    check(!t.call("Entity:setTile", {Value(1), Value(2), Value(5), Value(3)}, map).truthy(), "una capa que no existe: false");
    const Value center = t.call("Entity:cellToWorld", {Value(1), Value(2)}, map);
    const Value cell = t.call("Entity:worldToCell", {center}, map);
    check(cell.size() == 2 && cell[0].asNumber() == 1 && cell[1].asNumber() == 2 && center.asVec3().x > 10.0f,
          "worldToCell(cellToWorld(1, 2)) = {1, 2} (con la posicion del objeto)");
}

void testDestruction() {
    std::printf("Destruccion y OnBreak\n");
    physics::registerPhysicsComponents();
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_effects_api_tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "Fracturas");
    physics::FractureData data;
    physics::FractureSettings settings;
    settings.pieces = 6;
    const bool fractured = physics::fractureMesh(physics::boxSource(Vec3{1, 1, 1}), settings, data);
    data.uuid = Uuid::generate();
    const bool saved = physics::saveFracture(root / "Fracturas" / "Caja.crfracture", data);
    check(fractured && saved, "(un .crfracture de prueba)");
    assets::AssetDatabase database;
    database.open(root);
    assets::AssetManager manager(database);
    physics::PhysicsSystem physics;
    physics.setAssetManager(&manager);
    {
        ApiFixture t(false);
        Messages messages;
        messages.listen(t.scripts);
        // Sin fisica: nada.
        ecs::Entity lone = t.world.create("Suelto");
        t.scripts.start(t.world);
        check(!t.call("Entity:fracture", {}, ApiFixture::entity(lone)).truthy() && t.get("health", ApiFixture::entity(lone)).asNumber() == 0 &&
                  !t.get("isBroken", ApiFixture::entity(lone)).truthy(),
              "sin fisica: fracture false, health 0");
        t.scripts.stop();

        t.scripts.setPhysics(&physics);
        physics.start(t.world);
        t.scripts.start(t.world);
        ecs::Entity crate = t.world.create("Caja");
        crate.setWorldPosition(Vec3{0, 2, 0});
        physics::Destructible& d = crate.add<physics::Destructible>();
        d.fracture.uuid = data.uuid;
        d.health = 100.0f;
        const Value self = ApiFixture::entity(crate);
        check(t.get("health", self).asNumber() == 100 && !t.get("isBroken", self).truthy(), "health e isBroken");
        t.call("Entity:damage", {Value(30), Value(Vec3{0, 2, 0})}, self);
        check(std::abs(t.get("health", self).asNumber() - 70.0) < 1e-4, "damage quita vida");
        check(!t.call("Entity:fracture", {}, ApiFixture::entity(lone)).truthy(), "fracture sin Destructible: false");
        check(t.call("Entity:fracture", {Value(Vec3{0, 2.5f, 0}), Value(3)}, self).truthy(), "fracture");
        physics.update(t.world, 1.0f / 60.0f);
        check(t.get("isBroken", self).truthy(), "isBroken tras romperse");
        t.frame();
        const json* broke = messages.find(crate, "OnBreak");
        check(broke != nullptr && (*broke)["pieces"].is_array() && (*broke)["pieces"].size() >= 2 &&
                  (*broke)["pieces"][0].contains("$e") && std::abs((*broke)["point"]["$v"][1].get<double>() - 2.5) < 1e-4,
              "OnBreak {pieces, point} al objeto que se rompio");
        t.frame();
        check(messages.count("OnBreak") == 1, "una vez");

        // Parar y volver a empezar: un solo listener (no se duplica).
        t.scripts.stop();
        t.scripts.start(t.world);
        ecs::Entity second = t.world.create("Caja2");
        second.add<physics::Destructible>().fracture.uuid = data.uuid;
        t.call("Entity:fracture", {}, ApiFixture::entity(second));
        physics.update(t.world, 1.0f / 60.0f);
        t.frame();
        check(messages.count("OnBreak") == 2, "tras parar y empezar: un OnBreak por rotura");
        // Al parar se quita el listener: una rotura fuera de Play no se apunta.
        t.scripts.stop();
        ecs::Entity third = t.world.create("Caja3");
        third.add<physics::Destructible>().fracture.uuid = data.uuid;
        physics.destruction().fracture(third, Vec3{}, -1.0f);
        physics.update(t.world, 1.0f / 60.0f);
        t.scripts.start(t.world);
        t.frame();
        check(messages.count("OnBreak") == 2, "parado no se apuntan roturas");

        // Vehiculos: un objeto que no lo es.
        check(t.call("Entity:vehicleState", {}, self).isNil(), "vehicleState de algo que no es un vehiculo: nil");
        t.call("Entity:setGear", {Value(2)}, self);
        t.call("Entity:shiftGear", {Value(-1)}, self);
        check(true, "setGear / shiftGear sin vehiculo no fallan");
        t.scripts.stop();
        physics.stop();
    }
    std::filesystem::remove_all(root, ec);
}

void testMotionMatching() {
    std::printf("Motion Matching\n");
    ApiFixture t;
    ecs::Entity e = t.world.create("Personaje");
    const Value self = ApiFixture::entity(e);
    t.call("Entity:setMotionVelocity", {Value(Vec3{1, 0, 0})}, self);
    check(t.get("motionClip", self).asString().empty(), "sin MotionMatching: no hace nada");
    anim::MotionMatching& mm = e.add<anim::MotionMatching>();
    t.call("Entity:setMotionVelocity", {Value(Vec3{0, 0, 4})}, self);
    t.call("Entity:setMotionFacing", {Value(Vec3{1, 0, 0})}, self);
    t.call("Entity:setMotionTags", {Value("agachado")}, self);
    mm.runtime.clip_label = "Andar";
    check(near(mm.runtime.desired_velocity, Vec3{0, 0, 4}) && near(mm.runtime.desired_facing, Vec3{1, 0, 0}) &&
              mm.runtime.extra_tags == "agachado" && t.get("motionClip", self).asString() == "Andar",
          "setMotionVelocity, setMotionFacing, setMotionTags y motionClip");
    // Una entidad destruida: error claro.
    t.world.destroy(e);
    const json dead = t.bridge({{"fn", "setMotionTags"}, {"self", json{{"$e", entityId(e)}}}, {"args", {"x"}}});
    check(dead["ok"] == false && dead["error"].get<std::string>().find("ya no existe") != std::string::npos,
          "en una entidad destruida: error");
}

void testReplay() {
    std::printf("Replay\n");
    ApiFixture t;
    check(!t.call("Replay.start").truthy() && !t.call("Replay.isRecording").truthy() && t.call("Replay.list").size() == 0 &&
              t.call("Replay.time").asNumber() == 0 && !t.call("Replay.play").truthy(),
          "sin ReplaySystem: false, listas vacias y 0");
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "cramion_effects_api_replays";
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    std::filesystem::create_directories(folder);
    replay::ReplaySystem r;
    r.setFolder(folder);
    replay::setActiveSystem(&r);
    ecs::Entity ball = t.world.create("Pelota");
    Value options = Value::object();
    options.set("rate", 30);
    options.set("maxSeconds", 10);
    options.set("audio", false);
    check(t.call("Replay.start", {options}).truthy() && t.call("Replay.isRecording").truthy() && r.options().rate == 30.0f &&
              r.options().max_seconds == 10.0f && !r.options().record_audio && r.options().record_animation,
          "Replay.start con opciones");
    for (int i = 0; i <= 60; ++i) {
        ball.setWorldPosition(Vec3{static_cast<float>(i) * 0.1f, 0, 0});
        r.update(t.world, 1.0f / 60.0f);
    }
    t.call("Replay.mark", {Value("Gol"), Value("equipo1")});
    t.call("Replay.stop");
    check(!t.call("Replay.isRecording").truthy() && t.call("Replay.duration").asNumber() > 0.9, "Replay.stop y duration");
    check(!r.events().empty() && r.events().back().name == "Gol", "Replay.mark");
    check(t.call("Replay.save", {Value("gol")}).truthy() && t.call("Replay.list").size() == 1 &&
              t.call("Replay.list")[0].asString() == "gol",
          "Replay.save y Replay.list");
    check(t.call("Replay.load", {Value("gol")}).truthy() && !t.call("Replay.load", {Value("no_existe")}).truthy(), "Replay.load");
    check(t.call("Replay.play", {Value(0), Value(1)}).truthy() && t.call("Replay.isPlaying").truthy(), "Replay.play");
    t.call("Replay.pause");
    t.call("Replay.seek", {Value(0.5)});
    t.call("Replay.setSpeed", {Value(0.25)});
    t.call("Replay.setLoop", {Value(true)});
    t.call("Replay.setFreeCamera", {Value(true)});
    check(r.paused() && std::abs(t.call("Replay.time").asNumber() - 0.5) < 1e-4 && r.speed() == 0.25f && r.loop() && r.freeCamera(),
          "pause, seek, setSpeed, setLoop, setFreeCamera");
    t.call("Replay.pause", {Value(false)});
    check(!r.paused(), "pause(false)");
    t.call("Replay.stopPlayback");
    check(!t.call("Replay.isPlaying").truthy(), "Replay.stopPlayback");
    replay::setActiveSystem(nullptr);
    std::filesystem::remove_all(folder, ec);
}

}  // namespace

int main() {
    std::printf("Effects\n");
    testVfx();
    testPhysics2D();
    testSprites();
    testDestruction();
    testMotionMatching();
    testReplay();
    return finish();
}
