// Blueprints de actor (ver Blueprint.h): el bloque "blueprint" de un
// .crprefab, su Event Graph (.crgraph) al lado y la creacion desde un modelo,
// un prefab o vacio.

#include "CramionCore/ecs/Blueprint.h"

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/ecs/ModelInstantiation.h"
#include "CramionCore/ecs/Rigging.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/scripting/VisualScript.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>

namespace cramion::ecs {

using nlohmann::json;

namespace {

// Rutas en UTF-8: los nombres y las rutas de los JSON son UTF-8, pero en
// Windows path(std::string) y path::string() usan la pagina de codigos ANSI
// (un nombre con acentos sale mal o lanza una excepcion).
std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

// Con '/' en cualquier sistema (rutas relativas a Assets/).
std::string genericUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path fromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

json parse(const std::string& text) {
    json j = json::parse(text, nullptr, false);
    return j.is_object() ? j : json::object();
}

bool writeText(const std::filesystem::path& file, const std::string& text, std::string* error) {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path temporary = std::filesystem::path(file).concat(".tmp");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "no se pudo escribir " + utf8(file);
            return false;
        }
        out << text;
        if (!out) {
            if (error) *error = "error escribiendo " + utf8(file);
            return false;
        }
    }
    std::filesystem::rename(temporary, file, ec);
    if (ec) {
        if (error) *error = ec.message();
        return false;
    }
    return true;
}

json infoToJson(const BlueprintInfo& info) {
    json j = json::object();
    j["parent"] = info.parent;
    j["graph_path"] = info.graph_path;
    j["graph"] = info.graph.valid() ? info.graph.toString() : std::string{};
    j["source"] = info.source.valid() ? info.source.toString() : std::string{};
    j["description"] = info.description;
    j["category"] = info.category;
    j["tick"] = info.tick;
    return j;
}

// Los componentes que escribe un Blueprint (el editor ya los tiene; las
// pruebas y herramientas sueltas quiza no).
void ensureComponents() {
    registerPrefabComponents();
    vscript::registerVisualScriptComponents();
    if (ComponentRegistry::instance().find("BoxCollider") == nullptr) physics::registerPhysicsComponents();
}

// Relativo a Assets/ con '/'.
std::string assetsRelative(const std::filesystem::path& file, const std::filesystem::path& assets_root) {
    const std::filesystem::path rel = file.lexically_normal().lexically_relative(assets_root.lexically_normal());
    if (rel.empty() || *rel.begin() == "..") return genericUtf8(file.filename());
    return genericUtf8(rel);
}

// Un nombre libre para el .crprefab y el .crgraph a la vez ("BP_X", "BP_X 1"...).
std::string uniqueStem(const std::filesystem::path& folder, const std::string& stem) {
    std::error_code ec;
    for (int n = 0; n < 10000; ++n) {
        const std::string candidate = n == 0 ? stem : stem + " " + std::to_string(n);
        if (!std::filesystem::exists(folder / fromUtf8(candidate + kPrefabExtension), ec) &&
            !std::filesystem::exists(folder / fromUtf8(candidate + ".crgraph"), ec)) {
            return candidate;
        }
    }
    return stem + " " + Uuid::generate().toString().substr(0, 8);
}

// Event Graph de un Blueprint nuevo: Start y Update sin nada enganchado (como
// BeginPlay y Tick en Unreal). No usa exampleGraph: su Update hace girar el objeto.
vscript::Graph newEventGraph(const std::string& name) {
    vscript::Graph g;
    g.uuid = Uuid::generate();
    g.add(vscript::makeNode("event.start", core::Vec2{0.0f, 0.0f}));
    g.add(vscript::makeNode("event.update", core::Vec2{0.0f, 200.0f}));
    vscript::Node note = vscript::makeNode("comment", core::Vec2{-30.0f, -60.0f});
    note.comment = "Event Graph de " + name;
    note.size = core::Vec2{420.0f, 380.0f};
    g.add(std::move(note));
    return g;
}

struct Prepared {
    std::filesystem::path folder;  // absoluta
    std::string stem;
    std::filesystem::path prefab_file;
    std::filesystem::path graph_file;
    std::string graph_rel;
};

bool prepare(const BlueprintCreateOptions& options, Prepared& p, std::string* error) {
    if (options.name.empty()) {
        if (error) *error = "el Blueprint necesita un nombre";
        return false;
    }
    if (options.assets_root.empty()) {
        if (error) *error = "falta la carpeta Assets";
        return false;
    }
    p.folder = options.folder.is_absolute() ? options.folder : options.assets_root / options.folder;
    std::error_code ec;
    std::filesystem::create_directories(p.folder, ec);
    if (!std::filesystem::is_directory(p.folder, ec)) {
        if (error) *error = "no se pudo crear la carpeta " + utf8(p.folder);
        return false;
    }
    p.stem = uniqueStem(p.folder, options.name);
    const std::filesystem::path prefab_file = p.folder / fromUtf8(p.stem + kPrefabExtension);
    const std::filesystem::path graph_file = p.folder / fromUtf8(p.stem + ".crgraph");
    p.prefab_file = std::filesystem::absolute(prefab_file, ec);
    if (ec) p.prefab_file = prefab_file;
    p.graph_file = std::filesystem::absolute(graph_file, ec);
    if (ec) p.graph_file = graph_file;
    p.graph_rel = assetsRelative(graph_file, options.assets_root);
    return true;
}

// Escribe el grafo, el prefab desde `root` y el bloque "blueprint" (despues:
// createPrefab borra el archivo antes de escribirlo).
bool finish(World& world, Entity root, const BlueprintCreateOptions& options, const Prepared& p,
            const vscript::Graph& graph, const Uuid& source, BlueprintCreateResult& out, std::string* error) {
    vscript::VisualScript& vs =
        root.has<vscript::VisualScript>() ? root.get<vscript::VisualScript>() : root.add<vscript::VisualScript>();
    vs.graph = p.graph_rel;
    if (options.before_save) options.before_save(world, root);
    if (!vscript::saveGraph(graph, p.graph_file, error)) return false;
    std::error_code ec;
    if (!createPrefab(world, root, p.prefab_file, error)) {
        std::filesystem::remove(p.graph_file, ec);
        return false;
    }
    BlueprintInfo info;
    info.graph_path = p.graph_rel;
    info.graph = graph.uuid;
    info.source = source;
    if (!writeBlueprintInfo(p.prefab_file, info, error)) {
        std::filesystem::remove(p.graph_file, ec);
        std::filesystem::remove(p.prefab_file, ec);
        return false;
    }
    out.prefab_file = p.prefab_file;
    out.graph_file = p.graph_file;
    out.graph_rel = p.graph_rel;
    out.prefab = prefabUuid(readPrefabFile(p.prefab_file));
    return true;
}

// Caja de las mallas en el espacio de `root` (pose de reposo).
bool meshBounds(World& world, Entity root, const assets::ModelAsset& model, const Uuid& model_uuid, core::Vec3& lo,
                core::Vec3& hi) {
    constexpr float kInf = std::numeric_limits<float>::max();
    lo = core::Vec3{kInf, kInf, kInf};
    hi = core::Vec3{-kInf, -kInf, -kInf};
    bool any = false;
    const core::Mat4 to_root = core::inverse(root.worldMatrix());
    std::vector<Entity> stack{root};
    while (!stack.empty()) {
        const Entity e = stack.back();
        stack.pop_back();
        for (const entt::entity c : e.children()) stack.push_back(world.wrap(c));
        const MeshRenderer* mr = e.tryGet<MeshRenderer>();
        if (mr == nullptr || !(mr->model.uuid == model_uuid) || mr->part < 0 ||
            static_cast<std::size_t>(mr->part) >= model.parts.size() || !model.parts[static_cast<std::size_t>(mr->part)]) {
            continue;
        }
        const asset::ModelData& data = *model.parts[static_cast<std::size_t>(mr->part)];
        const core::Mat4 m = to_root * e.worldMatrix();
        for (const asset::SubMesh& sm : data.submeshes) {
            if (sm.index_count == 0) continue;
            for (int corner = 0; corner < 8; ++corner) {
                const core::Vec3 local{(corner & 1) ? sm.bounds_max.x : sm.bounds_min.x,
                                       (corner & 2) ? sm.bounds_max.y : sm.bounds_min.y,
                                       (corner & 4) ? sm.bounds_max.z : sm.bounds_min.z};
                const core::Vec3 p = transformPoint(m, local);
                lo = core::Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = core::Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
                any = true;
            }
        }
    }
    return any;
}

}  // namespace

// --- Bloque "blueprint" ------------------------------------------------------------

bool isBlueprint(const std::string& prefab_text) {
    if (prefab_text.find(kBlueprintKey) == std::string::npos) return false;  // sin parsear lo comun
    const json j = parse(prefab_text);
    const auto it = j.find(kBlueprintKey);
    return it != j.end() && it->is_object();
}

bool blueprintInfo(const std::string& prefab_text, BlueprintInfo& out) {
    out = BlueprintInfo{};
    if (prefab_text.find(kBlueprintKey) == std::string::npos) return false;
    const json j = parse(prefab_text);
    const auto it = j.find(kBlueprintKey);
    if (it == j.end() || !it->is_object()) return false;
    const json& b = *it;
    const auto text = [&](const char* key, const std::string& fallback) {
        const auto f = b.find(key);
        return f != b.end() && f->is_string() ? f->get<std::string>() : fallback;
    };
    out.parent = text("parent", out.parent);
    out.graph_path = text("graph_path", {});
    out.graph = Uuid::parse(text("graph", {}));
    out.source = Uuid::parse(text("source", {}));
    out.description = text("description", {});
    out.category = text("category", {});
    if (const auto f = b.find("tick"); f != b.end() && f->is_boolean()) out.tick = f->get<bool>();
    return true;
}

std::string withBlueprintInfo(const std::string& prefab_text, const BlueprintInfo& info) {
    json j = json::parse(prefab_text, nullptr, false);
    if (!j.is_object()) return prefab_text;
    j[kBlueprintKey] = infoToJson(info);
    return j.dump(2);
}

bool writeBlueprintInfo(const std::filesystem::path& prefab_file, const BlueprintInfo& info, std::string* error) {
    const std::string text = readPrefabFile(prefab_file);
    const json j = json::parse(text, nullptr, false);
    if (text.empty() || !j.is_object()) {
        if (error) *error = "no se pudo leer el prefab " + utf8(prefab_file);
        return false;
    }
    return writeText(prefab_file, withBlueprintInfo(text, info), error);
}

// --- Crear ---------------------------------------------------------------------------

std::filesystem::path uniqueAssetPath(const std::filesystem::path& folder, const std::string& stem,
                                      const std::string& extension) {
    std::error_code ec;
    for (int n = 0; n < 10000; ++n) {
        const std::filesystem::path candidate =
            folder / fromUtf8((n == 0 ? stem : stem + " " + std::to_string(n)) + extension);
        if (!std::filesystem::exists(candidate, ec)) return candidate;
    }
    return folder / fromUtf8(stem + " " + Uuid::generate().toString().substr(0, 8) + extension);
}

bool createBlueprintFromModel(const assets::ModelAsset& model, const Uuid& model_uuid,
                              const BlueprintCreateOptions& options, BlueprintCreateResult& out, std::string* error) {
    ensureComponents();
    Prepared p;
    if (!prepare(options, p, error)) return false;
    const Uuid uuid = model_uuid.valid() ? model_uuid : model.uuid;
    World world;
    Entity root = world.create(p.stem);

    // Un modelo animado (una sola pieza con esqueleto) es una sola entidad
    // "Malla" con su MeshRenderer y su Animator, como el SkeletalMesh de
    // Unreal: reimportar el modelo no tiene un arbol de nodos copiado que
    // romper. Los estaticos llevan su jerarquia como al ponerlos en la escena.
    int skinned_part = -1;
    if (model.animated) {
        for (std::size_t i = 0; i < model.parts.size(); ++i) {
            if (model.parts[i] && !model.parts[i]->bones.empty()) {
                skinned_part = static_cast<int>(i);
                break;
            }
        }
        if (skinned_part < 0 && model.parts.size() == 1 && model.parts[0]) skinned_part = 0;
    }
    Entity mesh;
    if (skinned_part >= 0 && model.parts.size() == 1) {
        mesh = world.create("Malla", root);
        // La pieza va donde la pondria su nodo (escala de cm a m de un FBX...).
        if (!model.nodes.empty()) {
            std::vector<core::Mat4> globals(model.nodes.size(), core::Mat4::identity());
            int node = -1;
            for (std::size_t i = 0; i < model.nodes.size(); ++i) {
                const int parent = model.nodes[i].parent;
                globals[i] = parent >= 0 && static_cast<std::size_t>(parent) < i
                                 ? globals[static_cast<std::size_t>(parent)] * model.nodes[i].local
                                 : model.nodes[i].local;
                if (node < 0 && model.nodes[i].part == skinned_part) node = static_cast<int>(i);
            }
            if (node >= 0) {
                core::Vec3 position{};
                core::Quat rotation{};
                core::Vec3 scale{1.0f, 1.0f, 1.0f};
                decomposeMatrix(globals[static_cast<std::size_t>(node)], position, rotation, scale);
                mesh.setLocalTrs(position, rotation, scale);
            }
        }
        MeshRenderer& mr = mesh.add<MeshRenderer>();
        mr.model = assets::AssetRef{uuid, assets::AssetType::Model};
        mr.part = skinned_part;
        mesh.add<Animator>();
        if (options.bones == BoneObjects::All) {
            createBoneEntities(world, mesh, *model.parts[static_cast<std::size_t>(skinned_part)]);
        }
    } else {
        const Entity copy = instantiateModel(world, model, root);
        if (!copy.valid()) {
            if (error) *error = "el modelo no tiene nada que poner";
            return false;
        }
        if (!(uuid == model.uuid)) {
            std::vector<Entity> stack{copy};
            while (!stack.empty()) {
                const Entity e = stack.back();
                stack.pop_back();
                if (MeshRenderer* mr = e.tryGet<MeshRenderer>(); mr != nullptr && mr->model.uuid == model.uuid) {
                    mr->model.uuid = uuid;
                }
                for (const entt::entity c : e.children()) stack.push_back(world.wrap(c));
            }
        }
    }
    if (options.add_collider) {
        core::Vec3 lo{};
        core::Vec3 hi{};
        if (meshBounds(world, root, model, uuid, lo, hi)) {
            physics::BoxCollider& box = root.add<physics::BoxCollider>();
            box.size = core::Vec3{std::max(hi.x - lo.x, 0.01f), std::max(hi.y - lo.y, 0.01f), std::max(hi.z - lo.z, 0.01f)};
            box.center = (lo + hi) * 0.5f;
        }
    }
    return finish(world, root, options, p, newEventGraph(p.stem), uuid, out, error);
}

bool createBlueprintFromPrefab(const std::string& prefab_text, const Uuid& prefab_uuid,
                               const BlueprintCreateOptions& options, BlueprintCreateResult& out, std::string* error) {
    ensureComponents();
    Prepared p;
    if (!prepare(options, p, error)) return false;
    World world;
    Entity root = instantiatePrefab(world, prefab_text);
    if (!root.valid()) {
        if (error) *error = "el prefab esta vacio o danado";
        return false;
    }
    unpackInstance(world, root);
    root.setName(p.stem);
    root.setLocalPosition(core::Vec3{});
    // Su propio Event Graph: si la raiz ya tenia uno, una copia (no se comparte).
    vscript::Graph graph = newEventGraph(p.stem);
    if (const vscript::VisualScript* vs = root.tryGet<vscript::VisualScript>(); vs != nullptr && !vs->graph.empty()) {
        vscript::Graph existing;
        if (vscript::loadGraph(options.assets_root / fromUtf8(vs->graph), existing)) {
            graph = std::move(existing);
            graph.uuid = Uuid::generate();
        }
    }
    return finish(world, root, options, p, graph, prefab_uuid, out, error);
}

bool createEmptyBlueprint(const BlueprintCreateOptions& options, BlueprintCreateResult& out, std::string* error) {
    ensureComponents();
    Prepared p;
    if (!prepare(options, p, error)) return false;
    World world;
    Entity root = world.create(p.stem);
    return finish(world, root, options, p, newEventGraph(p.stem), Uuid{}, out, error);
}

bool convertPrefabToBlueprint(const std::filesystem::path& prefab_file, const std::filesystem::path& assets_root,
                              BlueprintCreateResult& out, std::string* error) {
    ensureComponents();
    const std::string text = readPrefabFile(prefab_file);
    if (text.empty() || !json::parse(text, nullptr, false).is_object()) {
        if (error) *error = "no se pudo leer el prefab " + utf8(prefab_file);
        return false;
    }
    std::error_code ec;
    out.prefab_file = std::filesystem::absolute(prefab_file, ec);
    if (ec) out.prefab_file = prefab_file;
    out.prefab = prefabUuid(text);
    BlueprintInfo info;
    if (blueprintInfo(text, info)) {  // ya lo es
        out.graph_rel = info.graph_path;
        out.graph_file = assets_root / fromUtf8(info.graph_path);
        return true;
    }
    World world;
    Entity root = instantiatePrefab(world, text);
    if (!root.valid()) {
        if (error) *error = "el prefab esta vacio o danado";
        return false;
    }
    bool add_component = true;
    vscript::Graph graph;
    if (const vscript::VisualScript* vs = root.tryGet<vscript::VisualScript>(); vs != nullptr && !vs->graph.empty() &&
                                                                               std::filesystem::exists(assets_root / fromUtf8(vs->graph), ec)) {
        // Reutiliza el grafo de la raiz (puede que lo usen otras entidades).
        out.graph_rel = vs->graph;
        out.graph_file = assets_root / fromUtf8(vs->graph);
        if (vscript::loadGraph(out.graph_file, graph)) info.graph = graph.uuid;
        std::cerr << "[Blueprint] " << utf8(prefab_file.filename()) << ": usa el grafo que ya tenia ("
                  << vs->graph << "); si otras entidades lo usan, tambien cambiaran\n";
        add_component = false;
    } else {
        const std::filesystem::path folder = prefab_file.parent_path();
        const std::filesystem::path graph_file =
            uniqueAssetPath(folder, utf8(prefab_file.stem()), ".crgraph");
        graph = newEventGraph(utf8(prefab_file.stem()));
        if (!vscript::saveGraph(graph, graph_file, error)) return false;
        out.graph_file = std::filesystem::absolute(graph_file, ec);
        if (ec) out.graph_file = graph_file;
        out.graph_rel = assetsRelative(graph_file, assets_root);
        info.graph = graph.uuid;
        vscript::VisualScript& script = root.has<vscript::VisualScript>() ? root.get<vscript::VisualScript>()
                                                                          : root.add<vscript::VisualScript>();
        script.graph = out.graph_rel;
    }
    info.graph_path = out.graph_rel;  // source: invalido (no viene de otro asset)
    // Primero el bloque; applyInstance lo conserva al subir la revision (asi
    // las instancias de las escenas reciben el VisualScript).
    if (!writeBlueprintInfo(prefab_file, info, error)) return false;
    if (add_component && applyInstance(world, root, prefab_file, error).empty()) return false;
    return true;
}

}  // namespace cramion::ecs
