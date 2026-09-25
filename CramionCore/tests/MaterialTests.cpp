// Pruebas de los materiales (consola, sin GPU): el .crmat, crear uno a partir
// de una imagen con sus companeras, que cambia la "estructura" y los huecos y
// sombras del MeshRenderer en la escena. Devuelve 0 si todo va.

#include "CramionCore/asset/AssetDatabase.h"
#include "CramionCore/asset/MaterialAsset.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace cramion;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

void touch(const std::filesystem::path& file) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << "x";
}

void testAsset() {
    std::printf("Material (.crmat)\n");
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_material_assets";
    std::filesystem::remove_all(root);

    assets::MaterialAsset m;
    m.base_color = core::Vec4{0.5f, 0.25f, 0.1f, 1.0f};
    m.roughness = 0.3f;
    m.metallic = 1.0f;
    m.tiling = core::Vec2{4.0f, 2.0f};
    m.albedo = "Textures/ladrillo.png";
    m.mode = assets::MaterialMode::Transparent;
    const std::filesystem::path file = root / "Materials" / "Ladrillo.crmat";
    check(assets::saveMaterial(m, file) && m.uuid.valid(), "guardar le da un UUID");
    assets::MaterialAsset loaded;
    check(assets::loadMaterial(file, loaded) && loaded.uuid == m.uuid && loaded.roughness == 0.3f &&
              loaded.tiling.x == 4.0f && loaded.albedo == m.albedo && loaded.mode == assets::MaterialMode::Transparent,
          "leerlo igual");

    assets::AssetDatabase database;
    database.open(root);
    const auto info = database.find(m.uuid);
    check(info && info->type == assets::AssetType::Material && info->name == "Ladrillo",
          "la base de datos lo encuentra como Material");

    // Solo factores: misma estructura (se cambia en vivo). Textura o tiling: otra.
    const std::uint64_t base = assets::materialStructureHash(m);
    assets::MaterialAsset factors = m;
    factors.base_color.x = 1.0f;
    factors.roughness = 0.9f;
    check(assets::materialStructureHash(factors) == base, "color y rugosidad no cambian la estructura");
    assets::MaterialAsset tiled = m;
    tiled.tiling.x = 8.0f;
    assets::MaterialAsset textured = m;
    textured.normal = "Textures/ladrillo_normal.png";
    check(assets::materialStructureHash(tiled) != base && assets::materialStructureHash(textured) != base,
          "tiling y texturas si");

    const asset::MaterialData data = assets::toMaterialData(m, "Pared");
    check(data.name == "Pared" && data.metallic == 1.0f && data.transparent && data.albedo_texture == -1,
          "a MaterialData (factores, sin texturas)");

    // Pack PBR: piedra_albedo + companeras.
    touch(root / "Textures" / "piedra_albedo.png");
    touch(root / "Textures" / "piedra_normal.png");
    touch(root / "Textures" / "piedra_roughness.png");
    touch(root / "Textures" / "piedra_ao.png");
    touch(root / "Textures" / "otra_normal.png");
    const assets::MaterialAsset pack = assets::materialFromImage(root, "Textures/piedra_albedo.png");
    check(pack.albedo == "Textures/piedra_albedo.png" && pack.normal == "Textures/piedra_normal.png" &&
              pack.roughness_map == "Textures/piedra_roughness.png" && pack.occlusion == "Textures/piedra_ao.png" &&
              pack.metallic_map.empty(),
          "desde una imagen encuentra normal, rugosidad y AO");
    check(pack.roughness == 1.0f, "con mapa de rugosidad, el factor queda en 1");
    std::filesystem::remove_all(root);
}

void testMeshRenderer() {
    std::printf("Mesh Renderer\n");
    ecs::World world;
    ecs::Entity e = world.create("Caja");
    ecs::MeshRenderer& mr = e.add<ecs::MeshRenderer>();
    mr.cast_shadows = ecs::ShadowCasting::ShadowsOnly;
    const Uuid a = Uuid::generate();
    mr.materials.resize(3);
    mr.materials[0] = assets::AssetRef{a, assets::AssetType::Material};
    mr.materials[2] = assets::AssetRef{a, assets::AssetType::Material};

    ecs::World copy;
    ecs::deserializeWorld(copy, ecs::serializeWorld(world));
    const ecs::Entity c = copy.findByName("Caja");
    const ecs::MeshRenderer* read = c.valid() ? c.tryGet<ecs::MeshRenderer>() : nullptr;
    check(read != nullptr && read->cast_shadows == ecs::ShadowCasting::ShadowsOnly, "modo de sombras en la escena");
    check(read != nullptr && read->materials.size() == 3 && read->materials[0].uuid == a &&
              !read->materials[1].valid() && read->materials[2].uuid == a,
          "huecos de material en la escena (vacios incluidos)");

    ecs::Entity light = world.create("Foco");
    ecs::Light& l = light.add<ecs::Light>();
    l.type = ecs::LightType::Spot;
    l.cast_shadows = false;
    ecs::World copy2;
    ecs::deserializeWorld(copy2, ecs::serializeWorld(world));
    const ecs::Entity lc = copy2.findByName("Foco");
    check(lc.valid() && lc.has<ecs::Light>() && !lc.get<ecs::Light>().cast_shadows, "luz sin sombras en la escena");
}

}  // namespace

int main() {
    testAsset();
    testMeshRenderer();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
