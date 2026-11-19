// Fuzzing de las escenas y prefabs (ecs/SceneSerializer.h): el JSON de un
// .crscene / .crprefab entra por deserializeWorld (abrir una escena), por
// parseSceneText + restoreParsedEntities (Scene.loadAsync) y por
// pasteEntities (instanciar un prefab, pegar). Las escenas llegan de mods,
// DataPacks y paquetes de la Store.

#include "FuzzCommon.h"

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"

using namespace cramion;

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {
    const std::filesystem::path seeds = fuzz::seedsFolder(argc, argv);
    if (seeds.empty()) return 0;
    std::error_code e;
    std::filesystem::create_directories(seeds, e);
    // Una escena pequena con jerarquia y componentes de varios tipos.
    ecs::World world;
    ecs::Entity lamp = world.create("Farola");
    lamp.setLocalPosition({1.0f, 2.0f, 3.0f});
    ecs::Light& light = lamp.add<ecs::Light>();
    light.type = ecs::LightType::Spot;
    light.intensity = 4.0f;
    ecs::Entity bulb = world.create("Bombilla", lamp);
    bulb.setLocalEulerDegrees({0.0f, 45.0f, 0.0f});
    bulb.add<ecs::MeshRenderer>();
    world.create("Volumen").add<ecs::PostProcessing>();
    fuzz::writeFile(seeds / "escena.crscene", ecs::serializeWorld(world));
    fuzz::writeFile(seeds / "prefab.crprefab", ecs::serializeEntity(world, lamp));
    fuzz::writeFile(seeds / "vacia.crscene", "{\"entities\":[]}");
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string text(reinterpret_cast<const char*>(data), size);
    {
        ecs::World world;
        std::string error;
        if (ecs::deserializeWorld(world, text, &error)) {
            // Lo cargado se vuelve a guardar y a cargar (ida y vuelta).
            ecs::World again;
            ecs::deserializeWorld(again, ecs::serializeWorld(world), &error);
        }
    }
    {
        ecs::World world;
        ecs::SceneHeader header;
        std::string error;
        if (const auto parsed = ecs::parseSceneText(text, &header, &error)) {
            ecs::parsedEntityCount(*parsed);
            ecs::parsedSceneModels(*parsed);
            ecs::restoreParsedEntities(world, *parsed);
        }
    }
    {
        ecs::World world;
        ecs::Entity parent = world.create("Padre");
        ecs::pasteEntities(world, text, parent);
    }
    return 0;
}
