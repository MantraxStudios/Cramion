// Pruebas de los Blueprints de actor (ecs/Blueprint.h) y de los huesos como
// objetos sin RenderSync (ecs/Rigging.h): el bloque "blueprint" sobrevive a
// guardar el prefab, un prefab normal sale igual que antes, crear desde un
// modelo sintetico (3 nodos, 2 huesos), desde un prefab y vacio, convertir un
// prefab, el orden del esqueleto en reposo, los objetos de hueso y que las
// instancias de otra escena se actualizan, nombres con acentos (UTF-8) y
// renombrar o mover un Blueprint (su .crgraph va con el).
// Sale como CramionApiBlueprintTests.

#include "ApiTest.h"

#include "CramionCore/ecs/Blueprint.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/Prefab.h"
#include "CramionCore/ecs/Rigging.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/scripting/VisualScript.h"

#include <CramionFX/asset/Model.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

using namespace cramion;
using namespace cramion::apitest;
using core::Vec3;

namespace {

bool nearly(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

ecs::Entity child(ecs::Entity e, const char* name) {
    if (!e.valid()) return {};
    for (const entt::entity c : e.children()) {
        const ecs::Entity x = e.world()->wrap(c);
        if (x.name() == name) return x;
    }
    return {};
}

// Esqueleto de prueba: Escena (0) -> Hips (1) -> Spine (2); los dos ultimos son huesos.
std::shared_ptr<asset::ModelData> skeletonData() {
    auto m = std::make_shared<asset::ModelData>();
    m->name = "Prueba";
    asset::Node scene;
    scene.name = "Escena";
    asset::Node hips;
    hips.name = "Hips";
    hips.parent = 0;
    hips.local = core::translate(Vec3{0.0f, 1.0f, 0.0f});
    asset::Node spine;
    spine.name = "Spine";
    spine.parent = 1;
    spine.local = core::translate(Vec3{0.0f, 0.5f, 0.0f});
    m->nodes = {scene, hips, spine};
    asset::Bone b_hips;
    b_hips.name = "Hips";
    b_hips.node = 1;
    asset::Bone b_spine;
    b_spine.name = "Spine";
    b_spine.node = 2;
    m->bones = {b_hips, b_spine};
    asset::SubMesh sm;
    sm.index_count = 3;
    sm.bounds_min = Vec3{-0.5f, 0.0f, -0.5f};
    sm.bounds_max = Vec3{0.5f, 2.0f, 0.5f};
    m->submeshes = {sm};
    return m;
}

assets::ModelAsset modelAsset() {
    assets::ModelAsset model;
    model.uuid = Uuid::generate();
    model.name = "Prueba";
    assets::ModelNode node;
    node.name = "Prueba";
    node.part = 0;
    model.nodes = {node};
    model.parts = {skeletonData()};
    model.animated = true;
    return model;
}

nlohmann::json recordNamed(const std::string& prefab_text, const std::string& name) {
    const nlohmann::json j = nlohmann::json::parse(prefab_text, nullptr, false);
    if (!j.is_object() || !j.contains("entities")) return {};
    for (const nlohmann::json& r : j["entities"]) {
        if (r.value("name", std::string{}) == name) return r;
    }
    return {};
}

void testPrefabKeys(const std::filesystem::path& root) {
    std::printf("Prefab: claves extra\n");
    const std::filesystem::path file = root / "Prefabs" / "Farola.crprefab";
    ecs::World world;
    ecs::Entity lamp = world.create("Farola");
    lamp.add<physics::BoxCollider>().size = Vec3{0.3f, 3.0f, 0.3f};
    world.create("Bombilla", lamp).add<ecs::Light>().intensity = 10.0f;
    check(ecs::createPrefab(world, lamp, file), "crear un prefab normal");
    const std::string v2 = ecs::applyInstance(world, lamp, file);
    // Un prefab normal: solo sus claves de siempre y en el mismo formato.
    const nlohmann::json j = nlohmann::json::parse(v2, nullptr, false);
    nlohmann::json same;
    same["format"] = "CramionPrefab";
    same["version"] = 1;
    same["uuid"] = j.value("uuid", std::string{});
    same["name"] = j.value("name", std::string{});
    same["revision"] = j.value("revision", 0);
    same["entities"] = j.contains("entities") ? j["entities"] : nlohmann::json::array();
    check(j.is_object() && j.size() == 6 && same.dump(2) == v2 && ecs::prefabRevision(v2) == 2,
          "un prefab normal se guarda igual que antes (byte a byte)");
    check(!ecs::isBlueprint(v2), "un prefab normal no es Blueprint");

    ecs::BlueprintInfo info;
    info.graph_path = "Prefabs/Farola.crgraph";
    info.graph = Uuid::generate();
    info.description = "Una farola";
    info.tick = false;
    check(ecs::writeBlueprintInfo(file, info), "escribir el bloque blueprint");
    const std::string with_block = ecs::readPrefabFile(file);
    check(ecs::prefabRevision(with_block) == 2, "escribir el bloque no sube la revision");
    const std::string v3 = ecs::applyInstance(world, lamp, file);
    ecs::BlueprintInfo back;
    check(ecs::isBlueprint(v3) && ecs::blueprintInfo(v3, back) && back.graph_path == info.graph_path &&
              back.graph == info.graph && back.description == "Una farola" && !back.tick && back.parent == "Actor",
          "applyInstance conserva el bloque blueprint");
    check(ecs::prefabRevision(v3) == 3, "y sube la revision");
    // Claves desconocidas de otra version tambien.
    nlohmann::json future = nlohmann::json::parse(v3);
    future["futuro"] = {{"a", 1}};
    std::filesystem::remove(file);
    {
        std::FILE* f = std::fopen(file.string().c_str(), "wb");
        const std::string text = future.dump(2);
        if (f != nullptr) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        }
    }
    const std::string v4 = ecs::applyInstance(world, lamp, file);
    check(v4.find("\"futuro\"") != std::string::npos && ecs::isBlueprint(v4), "y cualquier clave que no conoce");
}

void testBindSkeleton() {
    std::printf("Esqueleto en reposo\n");
    const auto data = skeletonData();
    const std::vector<ecs::BindBone> bones = ecs::bindSkeleton(*data);
    check(bones.size() == 2, "dos huesos (sin el nodo raiz de la escena)");
    if (bones.size() != 2) return;
    check(bones[0].name == "Hips" && bones[0].parent == -1 && bones[0].node == 1, "Hips es la raiz");
    check(bones[1].name == "Spine" && bones[1].parent == 0 && bones[1].node == 2, "Spine cuelga de Hips (padres antes)");
    check(nearly(bones[1].global.m[3][1], 1.5f), "pose global de reposo (Spine a 1.5)");
}

void testBoneEntities() {
    std::printf("Huesos como objetos\n");
    const auto data = skeletonData();
    ecs::World world;
    ecs::Entity mesh = world.create("Malla");
    const std::vector<ecs::Entity> created = ecs::createBoneEntities(world, mesh, *data);
    check(created.size() == 2, "todos los huesos");
    ecs::Entity hips = child(mesh, "Hips");
    ecs::Entity spine = child(hips, "Spine");
    check(hips.valid() && spine.valid(), "anidados como el esqueleto");
    const ecs::BoneSocket* s = spine.valid() ? spine.tryGet<ecs::BoneSocket>() : nullptr;
    check(s != nullptr && s->bone == "Spine" && s->bone_object && s->mode == ecs::SocketMode::Follow && s->drive_position,
          "BoneSocket con bone_object, Seguir y mover la posicion");
    check(spine.valid() && nearly(spine.worldPosition().y, 1.5f), "en la pose de reposo");
    check(ecs::createBoneEntities(world, mesh, *data).empty(), "no repite los que ya existen");
    check(ecs::findBoneObject(mesh, "Spine") == spine, "findBoneObject");
    check(ecs::materializeBone(world, mesh, *data, "Spine") == spine, "materializeBone devuelve el que ya existe");

    ecs::World other;
    ecs::Entity mesh2 = other.create("Malla");
    ecs::Entity socket = ecs::materializeBone(other, mesh2, *data, "Spine");
    check(socket.valid() && socket.parent() == mesh2 && mesh2.childCount() == 1,
          "materializeBone: una sola entidad hija de la malla");
    check(socket.valid() && nearly(socket.worldPosition().y, 1.5f), "en su hueso");
    ecs::World chain;
    ecs::Entity mesh3 = chain.create("Malla");
    ecs::Entity nested = ecs::materializeBone(chain, mesh3, *data, "Spine", true);
    check(nested.valid() && nested.parent().valid() && nested.parent().name() == "Hips" && nested.parent().parent() == mesh3,
          "materializeBone anidado: con su cadena de antepasados");
    // Los objetos de hueso de antes (sin la marca, con el nombre del hueso).
    ecs::World old;
    ecs::Entity mesh4 = old.create("Malla");
    ecs::Entity legacy = old.create("Hips", mesh4);
    legacy.add<ecs::BoneSocket>().bone = "Hips";
    check(ecs::findBoneObject(mesh4, "Hips") == legacy, "findBoneObject acepta los objetos de hueso viejos");
    check(ecs::createBoneEntities(old, mesh4, *data).size() == 1 && child(legacy, "Spine").valid(),
          "y createBoneEntities los reutiliza");
}

void testFromModel(const std::filesystem::path& root) {
    std::printf("Blueprint desde un modelo\n");
    const assets::ModelAsset model = modelAsset();
    ecs::BlueprintCreateOptions options;
    options.name = "BP_Prueba";
    options.assets_root = root / "Assets";
    options.bones = ecs::BoneObjects::All;
    options.add_collider = true;
    bool called = false;
    options.before_save = [&](ecs::World&, ecs::Entity e) { called = e.valid(); };
    ecs::BlueprintCreateResult out;
    std::string error;
    check(ecs::createBlueprintFromModel(model, model.uuid, options, out, &error), "crear");
    if (!error.empty()) std::printf("    %s\n", error.c_str());
    check(called, "before_save con la raiz");
    check(std::filesystem::exists(out.prefab_file) && std::filesystem::exists(out.graph_file) &&
              out.prefab_file.filename() == "BP_Prueba.crprefab" && out.graph_rel == "Blueprints/BP_Prueba.crgraph",
          "escribe el .crprefab y el .crgraph en Assets/Blueprints");
    const std::string text = ecs::readPrefabFile(out.prefab_file);
    ecs::BlueprintInfo info;
    check(ecs::blueprintInfo(text, info) && info.graph_path == out.graph_rel && info.source == model.uuid &&
              info.graph.valid() && out.prefab == ecs::prefabUuid(text) && out.prefab.valid(),
          "el bloque blueprint (grafo, origen) y el uuid");
    vscript::Graph graph;
    check(vscript::loadGraph(out.graph_file, graph) && graph.uuid == info.graph && !graph.nodes.empty(),
          "el Event Graph se carga y su uuid es el del bloque");

    ecs::World world;
    ecs::Entity bp = ecs::instantiatePrefab(world, text);
    const vscript::VisualScript* vs = bp.valid() ? bp.tryGet<vscript::VisualScript>() : nullptr;
    check(bp.valid() && bp.name() == "BP_Prueba" && vs != nullptr && vs->graph == out.graph_rel,
          "la raiz lleva VisualScript con la ruta del grafo");
    ecs::Entity mesh = child(bp, "Malla");
    const ecs::MeshRenderer* mr = mesh.valid() ? mesh.tryGet<ecs::MeshRenderer>() : nullptr;
    check(mr != nullptr && mr->model.uuid == model.uuid && mr->part == 0 && mesh.has<ecs::Animator>(),
          "una entidad Malla con MeshRenderer y Animator");
    ecs::Entity spine = child(child(mesh, "Hips"), "Spine");
    check(spine.valid() && spine.has<ecs::BoneSocket>() && spine.get<ecs::BoneSocket>().bone_object,
          "Huesos: Todos (bone_object se guarda)");
    const physics::BoxCollider* box = bp.valid() ? bp.tryGet<physics::BoxCollider>() : nullptr;
    check(box != nullptr && nearly(box->size.y, 2.0f) && nearly(box->center.y, 1.0f) && nearly(box->size.x, 1.0f),
          "BoxCollider ajustado a la malla");

    ecs::BlueprintCreateResult again;
    check(ecs::createBlueprintFromModel(model, model.uuid, options, again) &&
              again.prefab_file.filename() == "BP_Prueba 1.crprefab" && again.graph_rel == "Blueprints/BP_Prueba 1.crgraph",
          "con el mismo nombre: BP_Prueba 1");
    check(ecs::uniqueAssetPath(root / "Assets" / "Blueprints", "BP_Prueba", ".crprefab").filename() == "BP_Prueba 2.crprefab",
          "uniqueAssetPath");

    // Las instancias de una escena se actualizan al guardar el Blueprint.
    ecs::World scene;
    ecs::Entity a = ecs::instantiatePrefab(scene, text);
    ecs::Entity b = ecs::instantiatePrefab(scene, text);
    ecs::Entity a_spine = child(child(child(a, "Malla"), "Hips"), "Spine");
    // RenderSync mueve los huesos cada frame: no son cambios propios.
    if (a_spine.valid()) a_spine.setLocalPosition(Vec3{3.0f, 4.0f, 5.0f});
    ecs::recordOverrides(scene, a, text);
    check(a.valid() && a.get<ecs::PrefabInstance>().overrides.empty(), "la Transform de un hueso no es un cambio propio");

    ecs::World stage;
    ecs::Entity edit = ecs::instantiatePrefab(stage, text);
    stage.create("Luz", edit).add<ecs::Light>().intensity = 5.0f;
    ecs::Entity edit_spine = child(child(child(edit, "Malla"), "Hips"), "Spine");
    if (edit_spine.valid()) edit_spine.setLocalPosition(Vec3{7.0f, 7.0f, 7.0f});
    const std::string v2 = ecs::applyInstance(stage, edit, out.prefab_file);
    check(ecs::prefabRevision(v2) == ecs::prefabRevision(text) + 1 && ecs::isBlueprint(v2), "guardar: revision + 1 y sigue siendo Blueprint");
    check(recordNamed(v2, "Spine")["components"]["Transform"] == recordNamed(text, "Spine")["components"]["Transform"],
          "el hueso guarda su pose de reposo, no la del momento");
    const int changed = ecs::syncOutdatedInstances(scene, [&](const Uuid& id) {
        return id == out.prefab ? ecs::readPrefabFile(out.prefab_file) : std::string{};
    });
    check(changed == 2 && child(a, "Luz").valid() && child(b, "Luz").valid(), "las dos instancias reciben la luz nueva");
}

void testFromPrefabAndEmpty(const std::filesystem::path& root) {
    std::printf("Blueprint desde un prefab, vacio y convertir\n");
    const std::filesystem::path assets = root / "Assets";
    const std::filesystem::path file = assets / "Prefabs" / "Caja.crprefab";
    ecs::World world;
    ecs::Entity box = world.create("Caja");
    box.add<physics::BoxCollider>();
    world.create("Tapa", box);
    check(ecs::createPrefab(world, box, file), "un prefab normal");
    const std::string text = ecs::readPrefabFile(file);

    ecs::BlueprintCreateOptions options;
    options.name = "BP_Caja";
    options.assets_root = assets;
    ecs::BlueprintCreateResult out;
    check(ecs::createBlueprintFromPrefab(text, ecs::prefabUuid(text), options, out), "crear desde el prefab");
    const std::string bp_text = ecs::readPrefabFile(out.prefab_file);
    ecs::BlueprintInfo info;
    check(ecs::blueprintInfo(bp_text, info) && info.source == ecs::prefabUuid(text) &&
              !(ecs::prefabUuid(bp_text) == ecs::prefabUuid(text)),
          "un asset nuevo con el prefab como origen");
    ecs::World w2;
    ecs::Entity inst = ecs::instantiatePrefab(w2, bp_text);
    check(inst.valid() && inst.has<physics::BoxCollider>() && child(inst, "Tapa").valid() && inst.has<vscript::VisualScript>(),
          "con sus componentes e hijos y VisualScript");

    ecs::BlueprintCreateResult empty;
    options.name = "BP_Vacio";
    check(ecs::createEmptyBlueprint(options, empty) && ecs::isBlueprint(ecs::readPrefabFile(empty.prefab_file)) &&
              std::filesystem::exists(empty.graph_file),
          "Blueprint de actor vacio");

    ecs::BlueprintCreateResult converted;
    const int before = ecs::prefabRevision(text);
    check(ecs::convertPrefabToBlueprint(file, assets, converted), "convertir el prefab en Blueprint");
    const std::string after = ecs::readPrefabFile(file);
    check(ecs::isBlueprint(after) && ecs::prefabUuid(after) == ecs::prefabUuid(text) &&
              ecs::prefabRevision(after) == before + 1 && converted.graph_rel == "Prefabs/Caja.crgraph" &&
              std::filesystem::exists(converted.graph_file),
          "mismo uuid, revision + 1 y su grafo al lado");
    ecs::World w3;
    ecs::Entity c = ecs::instantiatePrefab(w3, after);
    check(c.valid() && c.has<vscript::VisualScript>() && c.get<vscript::VisualScript>().graph == "Prefabs/Caja.crgraph",
          "la raiz lleva el VisualScript");
}

// Nombres con acentos (UTF-8): en Windows no deben pasar por la pagina de
// codigos ANSI al construir las rutas.
void testUtf8Names(const std::filesystem::path& root) {
    std::printf("Blueprint con acentos en el nombre\n");
    const auto u8 = [](const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); };
    const auto text = [](const std::filesystem::path& p) {
        const std::u8string t = p.u8string();
        return std::string(t.begin(), t.end());
    };
    const std::string folder = "H\xC3\xA9roes";          // "Heroes" con e acentuada
    const std::string name = "BP_Ca\xC3\xB1\xC3\xB3n";  // "BP_Canon" con enie y o acentuada
    const std::filesystem::path assets = root / "Assets";
    ecs::BlueprintCreateOptions options;
    options.name = name;
    options.assets_root = assets;
    options.folder = u8(folder);
    ecs::BlueprintCreateResult out;
    std::string error;
    check(ecs::createEmptyBlueprint(options, out, &error), "crear con acentos");
    if (!error.empty()) std::printf("    %s\n", error.c_str());
    check(std::filesystem::exists(assets / u8(folder) / u8(name + ".crprefab")) &&
              std::filesystem::exists(assets / u8(folder) / u8(name + ".crgraph")),
          "los archivos se llaman con sus acentos");
    check(text(out.prefab_file.filename()) == name + ".crprefab" && out.graph_rel == folder + "/" + name + ".crgraph",
          "graph_rel en UTF-8 con '/'");
    const std::string bp_text = ecs::readPrefabFile(out.prefab_file);
    ecs::World world;
    ecs::Entity bp = ecs::instantiatePrefab(world, bp_text);
    check(bp.valid() && bp.name() == name && bp.has<vscript::VisualScript>() &&
              bp.get<vscript::VisualScript>().graph == out.graph_rel,
          "la raiz con su nombre y la ruta del grafo");
    ecs::BlueprintCreateResult again;
    check(ecs::createEmptyBlueprint(options, again) && text(again.prefab_file.filename()) == name + " 1.crprefab",
          "con el mismo nombre: ' 1'");
    check(text(ecs::uniqueAssetPath(assets / u8(folder), name, ".crprefab").filename()) == name + " 2.crprefab",
          "uniqueAssetPath con acentos");

    // Convertir un prefab con acentos: su grafo al lado, con el mismo nombre.
    const std::filesystem::path file = assets / u8(folder) / u8("Ca\xC3\xB1\xC3\xB3n.crprefab");
    ecs::World w2;
    ecs::Entity cannon = w2.create("Ca\xC3\xB1\xC3\xB3n");
    w2.create("Tubo", cannon);
    check(ecs::createPrefab(w2, cannon, file), "un prefab con acentos");
    ecs::BlueprintCreateResult converted;
    check(ecs::convertPrefabToBlueprint(file, assets, converted, &error) &&
              converted.graph_rel == folder + "/Ca\xC3\xB1\xC3\xB3n.crgraph" &&
              std::filesystem::exists(converted.graph_file),
          "convertir: grafo con acentos al lado");
    ecs::BlueprintCreateResult converted_again;
    check(ecs::convertPrefabToBlueprint(file, assets, converted_again) &&
              converted_again.graph_file == assets / u8(converted.graph_rel),
          "convertir otra vez: encuentra su grafo");
}

std::string readText(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeTextFile(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

// Renombrar o mover un Blueprint: su .crgraph va con el y la ruta cambia en
// el bloque, en su raiz y en las escenas que lo usan.
void testRename(const std::filesystem::path& root) {
    std::printf("Renombrar y mover un Blueprint\n");
    const std::filesystem::path assets = root / "Assets";
    std::string text = R"({"a": {"graph": "Blueprints/X.crgraph", "graph_path": "Blueprints/X.crgraph"}, "b": {"graph":"Blueprints/X.crgraph"},
        "c": {"graph": "Blueprints/X.crgraph2"}})";
    check(ecs::replaceGraphReferences(text, "Blueprints/X.crgraph", "Otra/Y.crgraph") == 2 &&
              text.find("\"graph_path\": \"Blueprints/X.crgraph\"") != std::string::npos &&
              text.find("\"graph\":\"Otra/Y.crgraph\"") != std::string::npos &&
              text.find("\"graph\": \"Blueprints/X.crgraph2\"") != std::string::npos,
          "replaceGraphReferences: solo la clave \"graph\" con esa ruta exacta");

    ecs::BlueprintCreateOptions options;
    options.name = "BP_Viejo";
    options.assets_root = assets;
    ecs::BlueprintCreateResult out;
    check(ecs::createEmptyBlueprint(options, out) && out.graph_rel == "Blueprints/BP_Viejo.crgraph", "un Blueprint para renombrar");
    const std::filesystem::path scene = assets / "Escenas" / "Nivel.crscene";
    writeTextFile(scene, R"({"entities": [{"name": "BP_Viejo", "components": {"VisualScript": {"graph": "Blueprints/BP_Viejo.crgraph", "enabled": true}}}]})");

    // Renombrar (como el navegador: el .crprefab ya se movio).
    const std::filesystem::path renamed = assets / "Blueprints" / "BP_Nuevo.crprefab";
    std::filesystem::rename(out.prefab_file, renamed);
    ecs::BlueprintRelocation moved;
    std::string error;
    check(ecs::relocateBlueprintGraph(out.prefab_file, renamed, assets, moved, &error) && moved.moved &&
              moved.old_graph == "Blueprints/BP_Viejo.crgraph" && moved.new_graph == "Blueprints/BP_Nuevo.crgraph",
          "renombrar: el .crgraph se renombra con el");
    if (!error.empty()) std::printf("    %s\n", error.c_str());
    check(std::filesystem::exists(assets / "Blueprints" / "BP_Nuevo.crgraph") && !std::filesystem::exists(out.graph_file),
          "el archivo viejo ya no esta");
    const std::string bp_text = ecs::readPrefabFile(renamed);
    ecs::BlueprintInfo info;
    ecs::World world;
    const ecs::Entity bp = ecs::instantiatePrefab(world, bp_text);
    check(ecs::blueprintInfo(bp_text, info) && info.graph_path == moved.new_graph && bp.valid() &&
              bp.get<vscript::VisualScript>().graph == moved.new_graph,
          "el bloque blueprint y el VisualScript de la raiz con la ruta nueva");
    const std::string scene_text = readText(scene);
    check(moved.updated_files >= 2 && scene_text.find("BP_Nuevo.crgraph") != std::string::npos &&
              scene_text.find("BP_Viejo.crgraph") == std::string::npos,
          "las escenas que lo usan tambien");

    // Mover a otra carpeta.
    const std::filesystem::path other = assets / "Otra" / "BP_Nuevo.crprefab";
    std::filesystem::create_directories(other.parent_path());
    std::filesystem::rename(renamed, other);
    ecs::BlueprintRelocation moved_again;
    check(ecs::relocateBlueprintGraph(renamed, other, assets, moved_again) && moved_again.new_graph == "Otra/BP_Nuevo.crgraph" &&
              std::filesystem::exists(assets / "Otra" / "BP_Nuevo.crgraph") && readText(scene).find("Otra/BP_Nuevo.crgraph") != std::string::npos,
          "mover a otra carpeta: el .crgraph va con el");

    // Un grafo compartido (con otro nombre) no se mueve.
    ecs::BlueprintInfo shared;
    ecs::blueprintInfo(ecs::readPrefabFile(other), shared);
    const std::filesystem::path common = assets / "Otra" / "Comun.crgraph";
    std::filesystem::rename(assets / "Otra" / "BP_Nuevo.crgraph", common);
    shared.graph_path = "Otra/Comun.crgraph";
    ecs::writeBlueprintInfo(other, shared);
    const std::filesystem::path third = assets / "Otra" / "BP_Tercero.crprefab";
    std::filesystem::rename(other, third);
    ecs::BlueprintRelocation kept;
    check(ecs::relocateBlueprintGraph(other, third, assets, kept) && !kept.moved && kept.new_graph == "Otra/Comun.crgraph" &&
              std::filesystem::exists(common),
          "un grafo con otro nombre (compartido) se queda donde esta");
    ecs::BlueprintRelocation not_bp;
    check(!ecs::relocateBlueprintGraph(scene, scene, assets, not_bp), "un archivo que no es Blueprint: false");
}

}  // namespace

int main() {
    scripting::registerScriptComponents();
    physics::registerPhysicsComponents();
    ecs::registerPrefabComponents();
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_blueprint_tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    testPrefabKeys(root);
    testBindSkeleton();
    testBoneEntities();
    testFromModel(root);
    testFromPrefabAndEmpty(root);
    testUtf8Names(root);
    testRename(root);

    std::filesystem::remove_all(root, ec);
    return finish();
}
