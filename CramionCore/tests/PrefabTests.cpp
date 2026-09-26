// Pruebas de los prefabs (consola, sin GPU): crear un prefab desde una
// entidad, instanciarlo, cambios propios de cada instancia, aplicar una
// instancia al prefab y actualizar las demas, hijos anadidos y borrados,
// componentes anadidos, revertir, desempaquetar, guardar la escena y
// Scene.instantiate("Prefabs/...") desde Lua. Devuelve 0 si todo va.

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/Prefab.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/scripting/Scripting.h"

#include <cmath>
#include <cstdio>
#include <filesystem>

#include <nlohmann/json.hpp>

using namespace cramion;
using core::Vec3;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

ecs::Entity child(ecs::Entity e, const char* name) {
    for (const entt::entity c : e.children()) {
        const ecs::Entity x = e.world()->wrap(c);
        if (x.name() == name) return x;
    }
    return {};
}

}  // namespace

int main() {
    physics::registerPhysicsComponents();
    ecs::registerPrefabComponents();
    scripting::registerScriptComponents();
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_prefab_tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const std::filesystem::path file = root / "Prefabs" / "Farola.crprefab";

    std::printf("Crear e instanciar\n");
    ecs::World world;
    ecs::Entity lamp = world.create("Farola");
    lamp.setWorldPosition(Vec3{10.0f, 0.0f, 5.0f});
    lamp.add<physics::BoxCollider>().size = Vec3{0.3f, 3.0f, 0.3f};
    ecs::Entity bulb = world.create("Bombilla", lamp);
    bulb.setLocalPosition(Vec3{0.0f, 3.0f, 0.0f});
    ecs::Light& light = bulb.add<ecs::Light>();
    light.type = ecs::LightType::Point;
    light.intensity = 10.0f;
    light.color = Vec3{1.0f, 0.8f, 0.5f};
    std::string error;
    check(ecs::createPrefab(world, lamp, file, &error) && std::filesystem::exists(file), "crear el prefab desde una entidad");
    const std::string v1 = ecs::readPrefabFile(file);
    check(ecs::prefabRevision(v1) == 1 && ecs::prefabName(v1) == "Farola" && ecs::prefabUuid(v1).valid() &&
              lamp.has<ecs::PrefabInstance>() && bulb.has<ecs::PrefabLink>() &&
              lamp.get<ecs::PrefabInstance>().prefab.uuid == ecs::prefabUuid(v1),
          "el archivo tiene revision, nombre y UUID; la entidad pasa a ser su instancia");
    check(v1.find("PrefabLink") == std::string::npos && v1.find("\"position\": [\n") != std::string::npos,
          "el archivo no guarda los enlaces");
    ecs::Entity a = ecs::instantiatePrefab(world, file);
    ecs::Entity b = ecs::instantiatePrefab(world, file);
    a.setWorldPosition(Vec3{0.0f, 0.0f, 0.0f});
    b.setWorldPosition(Vec3{20.0f, 0.0f, 0.0f});
    check(a.valid() && b.valid() && a.uuid() != b.uuid() && child(a, "Bombilla").valid() &&
              child(a, "Bombilla").get<ecs::Light>().intensity == 10.0f && a.has<physics::BoxCollider>(),
          "instanciar dos copias con sus hijos y componentes");
    check(ecs::prefabInstances(world, ecs::prefabUuid(v1)).size() == 3 && ecs::prefabRoot(child(a, "Bombilla")) == a,
          "prefabInstances y prefabRoot");
    ecs::recordOverrides(world, a, v1);
    ecs::recordOverrides(world, lamp, v1);
    check(a.get<ecs::PrefabInstance>().overrides.empty() && lamp.get<ecs::PrefabInstance>().overrides.empty(),
          "una instancia recien creada no tiene cambios propios (ni por su posicion)");

    std::printf("Cambios propios y aplicar\n");
    // A: su propia intensidad y un componente mas.
    child(a, "Bombilla").get<ecs::Light>().intensity = 25.0f;
    a.add<physics::SphereCollider>();
    ecs::recordOverrides(world, a, v1);
    const auto& ov = a.get<ecs::PrefabInstance>().overrides;
    check(ov.size() == 2, "se apuntan los cambios propios de una instancia");
    // B se cambia y se aplica al prefab: color nuevo y un hijo nuevo.
    child(b, "Bombilla").get<ecs::Light>().color = Vec3{0.2f, 0.6f, 1.0f};
    child(b, "Bombilla").get<ecs::Light>().intensity = 12.0f;
    ecs::Entity sign = world.create("Cartel", b);
    sign.add<physics::BoxCollider>();
    const std::string v2 = ecs::applyInstance(world, b, file, &error);
    check(!v2.empty() && ecs::prefabRevision(v2) == 2 && sign.has<ecs::PrefabLink>() && b.get<ecs::PrefabInstance>().overrides.empty(),
          "aplicar una instancia crea la revision 2 (con el hijo nuevo)");
    {
        const nlohmann::json j = nlohmann::json::parse(v2, nullptr, false);
        const nlohmann::json& pos = j["entities"][0]["components"]["Transform"]["position"];
        check(b.worldPosition().x == 20.0f && pos.is_array() && pos[0].get<float>() == 0.0f,
              "la posicion de la instancia no pasa al prefab");
    }
    ecs::syncInstance(world, a, v2);
    ecs::syncInstance(world, lamp, v2);
    const ecs::Light& la = child(a, "Bombilla").get<ecs::Light>();
    check(std::abs(la.color.z - 1.0f) < 1e-5f && la.intensity == 25.0f,
          "la otra instancia recibe el color nuevo y conserva su intensidad propia");
    check(child(a, "Cartel").valid() && child(a, "Cartel").has<physics::BoxCollider>() && child(lamp, "Cartel").valid(),
          "y el hijo nuevo del prefab aparece en todas");
    check(a.has<physics::SphereCollider>() && a.worldPosition().x == 0.0f && a.get<ecs::PrefabInstance>().revision == 2,
          "conserva su componente anadido y su posicion; queda en la revision 2");
    check(std::abs(child(lamp, "Bombilla").get<ecs::Light>().intensity - 12.0f) < 1e-5f, "una instancia sin cambios lo recibe todo");

    std::printf("Borrar, revertir y desempaquetar\n");
    // A borra el cartel: al actualizar no vuelve.
    world.destroy(child(a, "Cartel"));
    ecs::recordOverrides(world, a, v2);
    ecs::syncInstance(world, a, v2);
    check(!child(a, "Cartel").valid(), "un hijo borrado en la instancia no vuelve al actualizar");
    // Revertir: todo como el prefab.
    ecs::revertInstance(world, a, v2);
    check(child(a, "Cartel").valid() && std::abs(child(a, "Bombilla").get<ecs::Light>().intensity - 12.0f) < 1e-5f &&
              !a.has<physics::SphereCollider>() && a.get<ecs::PrefabInstance>().overrides.empty() && a.worldPosition().x == 0.0f,
          "revertir: igual que el prefab (salvo su posicion)");
    // El prefab quita un hijo: desaparece de las instancias.
    world.destroy(child(b, "Cartel"));
    const std::string v3 = ecs::applyInstance(world, b, file, &error);
    ecs::syncInstance(world, a, v3);
    check(!child(a, "Cartel").valid() && ecs::prefabRevision(v3) == 3, "quitar un hijo del prefab lo quita de las instancias");
    ecs::unpackInstance(world, lamp);
    check(!lamp.has<ecs::PrefabInstance>() && !child(lamp, "Bombilla").has<ecs::PrefabLink>() &&
              ecs::prefabInstances(world, ecs::prefabUuid(v3)).size() == 2,
          "desempaquetar: entidades normales");

    std::printf("Escenas y Lua\n");
    child(a, "Bombilla").get<ecs::Light>().intensity = 40.0f;
    ecs::recordOverrides(world, a, v3);
    const std::string scene = ecs::serializeWorld(world);
    ecs::World loaded;
    check(ecs::deserializeWorld(loaded, scene), "guardar y cargar la escena");
    const auto instances = ecs::prefabInstances(loaded, ecs::prefabUuid(v3));
    ecs::Entity la2;
    for (ecs::Entity e : instances) {
        if (e.uuid() == a.uuid()) la2 = e;
    }
    check(instances.size() == 2 && la2.valid() && la2.get<ecs::PrefabInstance>().overrides.size() == 1 &&
              child(la2, "Bombilla").get<ecs::PrefabLink>().source == child(a, "Bombilla").get<ecs::PrefabLink>().source,
          "la escena guarda las instancias, sus enlaces y sus cambios propios");
    // Una revision nueva con la escena cerrada: al abrirla se actualiza (y respeta el cambio propio).
    child(b, "Bombilla").get<ecs::Light>().range = 33.0f;
    const std::string v4 = ecs::applyInstance(world, b, file, &error);
    const int synced = ecs::syncOutdatedInstances(loaded, [&](const Uuid& id) { return id == ecs::prefabUuid(v4) ? v4 : std::string{}; });
    check(synced == 2 && la2.get<ecs::PrefabInstance>().revision == 4 && child(la2, "Bombilla").get<ecs::Light>().range == 33.0f && child(la2, "Bombilla").get<ecs::Light>().intensity == 40.0f,
          "al abrir una escena con una revision vieja, se actualiza respetando lo propio");

    scripting::ScriptSystem scripts;
    scripts.setAssetsRoot(root);
    scripts.start(world);
    std::string out;
    const bool ok = scripts.run(R"(
        local f = Scene.instantiate("Prefabs/Farola", Vec3(1, 2, 3), Vec3(0, 90, 0))
        local g = Scene.instantiate("Prefabs/Farola.crprefab")
        local nada = Scene.instantiate("Prefabs/NoExiste")
        return f.name .. " " .. tostring(f.position.y) .. " " .. tostring(f:find("Bombilla") ~= nil) .. " " ..
               tostring(g ~= nil) .. " " .. tostring(nada == nil)
    )", &out);
    std::printf("  (%s)\n", out.c_str());
    check(ok && out == "Farola 2.0 true true true" && ecs::prefabInstances(world, ecs::prefabUuid(v4)).size() == 4,
          "Scene.instantiate(\"Prefabs/Farola\", posicion, giro) desde Lua");
    scripts.stop();

    std::filesystem::remove_all(root, ec);
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
