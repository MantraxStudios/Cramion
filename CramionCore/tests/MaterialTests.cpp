// Pruebas de los materiales (consola, sin GPU): el .crmat, crear uno a partir
// de una imagen con sus companeras, que cambia la "estructura" y los huecos y
// sombras del MeshRenderer en la escena. Devuelve 0 si todo va.

#include "CramionCore/asset/AssetDatabase.h"
#include "CramionCore/asset/Importer.h"
#include "CramionCore/asset/MaterialAsset.h"
#include "CramionCore/asset/ModelMaterials.h"
#include "CramionCore/asset/RenderTextureAsset.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"

#include <CramionFX/asset/ImageFile.h>

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

// Render Texture (.crrt) y el Target Texture de la camara.
void testRenderTexture() {
    std::printf("Render Texture\n");
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_rt_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Texturas");
    assets::RenderTextureAsset t;
    t.width = 640;
    t.height = 360;
    const std::filesystem::path file = root / "Texturas" / "Monitor.crrt";
    check(assets::saveRenderTexture(t, file) && t.uuid.valid(), "guardar le da un UUID");
    assets::RenderTextureAsset loaded;
    check(assets::loadRenderTexture(file, loaded) && loaded.uuid == t.uuid && loaded.width == 640 && loaded.height == 360,
          "leerla igual");
    assets::RenderTextureAsset big;
    big.width = 100000;
    big.height = 0;
    check(assets::saveRenderTexture(big, root / "Grande.crrt") && big.width == 8192 && big.height == 1,
          "el tamano se limita (1..8192)");
    assets::AssetDatabase database;
    database.open(root);
    const auto info = database.find(t.uuid);
    check(info && info->type == assets::AssetType::RenderTexture && info->name == "Monitor",
          "la base de datos la encuentra como Render Texture");

    ecs::World world;
    ecs::Entity cam = world.create("Seguridad");
    cam.add<ecs::Camera>().target_texture = assets::AssetRef{t.uuid, assets::AssetType::RenderTexture};
    ecs::World copy;
    ecs::deserializeWorld(copy, ecs::serializeWorld(world));
    const ecs::Entity c = copy.findByName("Seguridad");
    check(c.valid() && c.has<ecs::Camera>() && c.get<ecs::Camera>().target_texture.uuid == t.uuid,
          "el Target Texture de la camara se guarda en la escena");
    std::filesystem::remove_all(root);
}

void writeImage(const std::filesystem::path& file, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    asset::ImageRgba8 image;
    image.width = 4;
    image.height = 4;
    for (int i = 0; i < 16; ++i) image.pixels.insert(image.pixels.end(), {r, g, b, 255});
    asset::saveImagePng(file, image);
}

// "Crear materiales y asignarlos": un OBJ sin texturas cuyas imagenes estan
// en Assets (Madera) y junto al archivo original, fuera (Piedra).
void testModelMaterialsSearch() {
    std::printf("Crear materiales de un modelo (texturas por nombre)\n");
    const std::filesystem::path base = std::filesystem::temp_directory_path() / "cramion_model_materials";
    std::filesystem::remove_all(base);
    const std::filesystem::path root = base / "Assets";
    const std::filesystem::path source = base / "Descargas" / "Mueble";

    writeImage(root / "Textures" / "Madera_BaseColor.png", 150, 90, 40);
    writeImage(root / "Textures" / "Madera_Normal.png", 128, 128, 255);
    writeImage(root / "Textures" / "Metal_BaseColor.png", 200, 200, 200);  // de otro material
    writeImage(source / "Textures" / "Piedra_Albedo.png", 120, 120, 120);
    writeImage(source / "Textures" / "Piedra_Roughness.png", 200, 200, 200);

    std::filesystem::create_directories(source);
    std::ofstream(source / "mueble.mtl") << "newmtl Madera\nKd 0.8 0.8 0.8\n\nnewmtl Piedra\nKd 0.8 0.8 0.8\n";
    std::ofstream(source / "mueble.obj")
        << "mtllib mueble.mtl\n"
           "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 2 0 0\nv 3 0 0\nv 3 1 0\nv 2 1 0\n"
           "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
           "usemtl Madera\nf 1/1 2/2 3/3\nf 1/1 3/3 4/4\n"
           "usemtl Piedra\nf 5/1 6/2 7/3\nf 5/1 7/3 8/4\n";

    const assets::ImportResult imported = assets::importModel(source / "mueble.obj", root / "Models");
    check(imported.ok, "importar el OBJ");
    if (!imported.ok) return;
    assets::AssetDatabase database;
    database.open(root);
    const auto info = database.find(imported.info.uuid);
    check(info.has_value(), "el modelo esta en la base de datos");
    if (!info) return;

    const assets::ExtractMaterialsResult r = assets::extractModelMaterials(*info, root);
    check(r.ok && r.created == 2 && r.map.size() == 2 && r.textures_found == 2 && r.without_color.empty(),
          "dos materiales, los dos con textura encontrada");
    assets::MaterialAsset madera;
    assets::MaterialAsset piedra;
    const bool both = r.map.count("Madera") && r.map.count("Piedra") &&
                      assets::loadMaterial(r.material_folder / "Madera.crmat", madera) &&
                      assets::loadMaterial(r.material_folder / "Piedra.crmat", piedra);
    check(both, "Madera.crmat y Piedra.crmat en Materials/<modelo>");
    check(madera.albedo == "Textures/Madera_BaseColor.png" && madera.normal == "Textures/Madera_Normal.png",
          "Madera: su color y su normal del proyecto (no los de Metal)");
    check(madera.base_color.x == 1.0f, "con textura, el gris del archivo no la oscurece");
    check(!piedra.albedo.empty() && std::filesystem::exists(root / piedra.albedo) && piedra.albedo.find("Models/Textures/") == 0 &&
              !piedra.roughness_map.empty(),
          "Piedra: su juego de texturas se copia de junto al original a Textures/<modelo>");

    const assets::ExtractMaterialsResult again = assets::extractModelMaterials(*info, root);
    check(again.ok && again.created == 0 && again.reused == 2 && again.map == r.map,
          "otra vez: reutiliza los .crmat (no pisa los editados)");

    const std::filesystem::path settings = base / "ProjectSettings";
    check(assets::saveModelMaterialMap(settings, info->uuid, r.map) &&
              assets::loadModelMaterialMap(settings, info->uuid) == r.map &&
              assets::loadModelMaterialMap(settings, Uuid::generate()).empty(),
          "el mapa del modelo se guarda y se lee por su UUID");
}

// Un glTF con las texturas dentro (DamagedHelmet): se sacan a imagenes y el
// metal/rugosidad empaquetado se parte en dos mapas.
void testModelMaterialsEmbedded() {
    std::printf("Crear materiales de un modelo (texturas incrustadas)\n");
    std::filesystem::path helmet;
    for (const char* candidate : {"../assets/DamagedHelmet.glb", "assets/DamagedHelmet.glb",
                                  "../../build/assets/DamagedHelmet.glb", "../build/assets/DamagedHelmet.glb"}) {
        if (std::filesystem::exists(candidate)) {
            helmet = candidate;
            break;
        }
    }
    if (helmet.empty()) {
        std::printf("  (sin DamagedHelmet.glb: se salta)\n");
        return;
    }
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_model_materials_glb" / "Assets";
    std::filesystem::remove_all(root.parent_path());
    const assets::ImportResult imported = assets::importModel(helmet, root / "Casco");
    check(imported.ok, "importar DamagedHelmet.glb");
    if (!imported.ok) return;
    assets::AssetDatabase database;
    database.open(root);
    const auto info = database.find(imported.info.uuid);
    if (!info) return;
    const assets::ExtractMaterialsResult r = assets::extractModelMaterials(*info, root);
    check(r.ok && r.created >= 1 && r.textures_extracted >= 4, "un material y sus texturas extraidas");
    assets::MaterialAsset m;
    const bool read = !r.map.empty() && assets::loadMaterial(
        r.material_folder / (r.map.begin()->first + ".crmat"), m);
    check(read && !m.albedo.empty() && std::filesystem::exists(root / m.albedo) && !m.normal.empty() &&
              !m.roughness_map.empty() && !m.metallic_map.empty() && !m.emissive_map.empty() && !m.occlusion.empty(),
          "color, normal, rugosidad, metal, emision y oclusion en archivos de Assets");
}

}  // namespace

int main() {
    testAsset();
    testMeshRenderer();
    testRenderTexture();
    testModelMaterialsSearch();
    testModelMaterialsEmbedded();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
