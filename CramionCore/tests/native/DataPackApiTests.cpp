// Pruebas de native/DataPackApi.cpp: DataPack.info / load / loadScene /
// instantiate / list / isLoaded / unload con dos paquetes de verdad (uno
// con escenas y otro con un prefab).

#include "ApiTest.h"

#include "CramionCore/ecs/Prefab.h"
#include "CramionCore/project/DataPack.h"

#include <cmath>
#include <filesystem>
#include <fstream>

using namespace cramion;
using namespace cramion::apitest;

namespace {

void write(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

bool near(const core::Vec3& a, const core::Vec3& b) {
    return std::fabs(a.x - b.x) < 1e-3f && std::fabs(a.y - b.y) < 1e-3f && std::fabs(a.z - b.z) < 1e-3f;
}

}  // namespace

int main() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_datapack_api_test";
    std::filesystem::remove_all(root);

    // Los paquetes, hechos desde otro proyecto.
    const std::filesystem::path source = root / "origen" / "Assets";
    write(source / "Escenas" / "Nivel2.crscene", "{\"entities\":[]}");
    write(source / "Escenas" / "Extra.crscene", "{\"entities\":[]}");
    std::string error;
    bool built = false;
    {
        ecs::World prefab_world;
        ecs::Entity coche = prefab_world.create("Coche");
        built = ecs::createPrefab(prefab_world, coche, source / "Prefabs" / "Coche.crprefab", &error);
    }
    const std::filesystem::path packs = root / "DataPacks";
    project::DataPackCollection scenes;
    scenes.files = {source / "Escenas" / "Nivel2.crscene", source / "Escenas" / "Extra.crscene"};
    built = built && project::writeDataPack(packs / "Nivel2.datapack", "Nivel2", scenes.files, scenes, source, "test", 3, {}, &error);
    project::DataPackCollection objects;
    objects.files = {source / "Prefabs" / "Coche.crprefab"};
    built = built && project::writeDataPack(packs / "Skins.datapack", "Skins", objects.files, objects, source, "test", 3, {}, &error);
    std::printf("DataPack\n");
    check(built, "los paquetes de prueba se escriben");
    if (!built) std::printf("    (%s)\n", error.c_str());

    // El juego: Assets vacio junto a la carpeta DataPacks/.
    const std::filesystem::path assets = root / "Assets";
    std::filesystem::create_directories(assets);
    ApiFixture t(false);
    t.scripts.setAssetsRoot(assets);
    int changed = 0;
    t.scripts.setAssetsChangedCallback([&changed] { ++changed; });
    t.scripts.start(t.world);

    // --- info (sin montar) ---
    const Value info = t.call("DataPack.info", {Value("Nivel2")});
    check(info["name"].asString() == "Nivel2" && info["version"].asString() == "1.0" && info["scenes"].size() == 2 &&
              info["objects"].size() == 0 && info["files"].asNumber() == 2.0,
          "DataPack.info: el manifiesto");
    check(!std::filesystem::exists(assets / "Escenas") && !t.call("DataPack.isLoaded", {Value("Nivel2")}).truthy(),
          "DataPack.info no monta nada");
    check(t.call("DataPack.info", {Value("Nada")}).isNil(), "DataPack.info de un paquete que no existe: nil");

    // --- load ---
    const Value loaded = t.call("DataPack.load", {Value("Nivel2")});
    check(loaded["name"].asString() == "Nivel2" && loaded["version"].asString().empty() && loaded["scenes"].size() == 2 &&
              loaded["scenes"][0].asString() == "Escenas/Nivel2.crscene" && loaded["files"].asNumber() == 2.0,
          "DataPack.load: {name, version, scenes, objects, files}");
    check(std::filesystem::exists(assets / "Escenas" / "Extra.crscene") && changed == 1, "DataPack.load extrae en Assets");
    t.call("DataPack.load", {Value("Nivel2")});
    const Value list = t.call("DataPack.list");
    check(list.size() == 1 && list[0].asString() == "Nivel2" && changed == 1, "DataPack.list (sin montar dos veces)");
    check(t.call("DataPack.isLoaded", {Value("Nivel2")}).truthy() &&
              t.call("DataPack.isLoaded", {Value((packs / "Nivel2.datapack").string())}).truthy(),
          "DataPack.isLoaded por nombre y por ruta");
    check(t.call("DataPack.load", {Value("NoExiste")}).isNil() && t.logged("no existe el DataPack \"NoExiste\""),
          "DataPack.load de un paquete que no existe: nil y error");

    // --- loadScene ---
    check(t.call("DataPack.loadScene", {Value("Nivel2"), Value("extra")}).truthy() &&
              t.scripts.takeSceneRequest() == assets / "Escenas" / "Extra.crscene",
          "DataPack.loadScene por nombre de escena");
    check(t.call("DataPack.loadScene", {Value("Nivel2")}).truthy() &&
              t.scripts.takeSceneRequest() == assets / "Escenas" / "Nivel2.crscene",
          "DataPack.loadScene: la primera escena");
    check(!t.call("DataPack.loadScene", {Value("Nivel2"), Value("Nada")}).truthy() && t.logged("no tiene la escena \"Nada\"") &&
              t.scripts.takeSceneRequest().empty(),
          "DataPack.loadScene de una escena que no tiene");

    // --- instantiate ---
    const Value car = t.call("DataPack.instantiate",
                             {Value("Skins"), Value("Coche"), Value(core::Vec3{1.0f, 2.0f, 3.0f}), Value(core::Vec3{0.0f, 90.0f, 0.0f})});
    const ecs::Entity copy = car.isEntity() ? t.world.wrap(car.asEntity()) : ecs::Entity{};
    check(copy.valid() && copy.name() == "Coche" && near(copy.worldPosition(), {1.0f, 2.0f, 3.0f}) &&
              near(copy.localEulerDegrees(), {0.0f, 90.0f, 0.0f}),
          "DataPack.instantiate: el prefab en la posicion y rotacion");
    check(t.call("DataPack.instantiate", {Value("Skins")}).isEntity(), "DataPack.instantiate: el primer objeto");
    check(t.call("DataPack.instantiate", {Value("Skins"), Value("Nada")}).isNil() && t.logged("no tiene el objeto \"Nada\""),
          "DataPack.instantiate de un objeto que no tiene");
    check(t.call("DataPack.instantiate", {Value("Nivel2")}).isNil() && t.logged("no tiene objetos"),
          "DataPack.instantiate de un paquete sin objetos");
    check(!t.call("DataPack.loadScene", {Value("Skins")}).truthy() && t.logged("\"Skins\" no tiene escenas"),
          "DataPack.loadScene de un paquete sin escenas");

    // Por el puente de los scripts de C++.
    const json r = t.bridge({{"fn", "DataPack.list"}});
    check(r["ok"] == true && r["result"].is_array() && r["result"].size() == 2, "DataPack.list por el puente");

    // --- unload (los montajes sobreviven a stop/start) ---
    t.scripts.stop();
    t.scripts.start(t.world);
    check(t.call("DataPack.list").size() == 2, "los paquetes siguen montados tras cambiar de escena");
    check(t.call("DataPack.unload", {Value("Nivel2")}).truthy() && !std::filesystem::exists(assets / "Escenas") &&
              !t.call("DataPack.unload", {Value("Nivel2")}).truthy(),
          "DataPack.unload borra lo que extrajo");
    check(t.call("DataPack.unload", {Value((packs / "Skins.datapack").string())}).truthy() && t.call("DataPack.list").size() == 0,
          "DataPack.unload por ruta");

    t.scripts.stop();
    std::filesystem::remove_all(root);
    return finish();
}
