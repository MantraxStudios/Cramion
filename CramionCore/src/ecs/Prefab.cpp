// Prefabs (ver Prefab.h): archivos .crprefab, instancias enlazadas, cambios
// propios y actualizacion de las instancias.

#include "CramionCore/ecs/Prefab.h"

#include "CramionCore/ecs/Components.h"
#include "SerializerInternal.h"

#include <fstream>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace cramion::ecs {

using detail::buildEntities;
using detail::entityRecord;
using detail::JsonReader;
using nlohmann::json;

namespace {

bool technical(const std::string& component) { return component == "PrefabInstance" || component == "PrefabLink"; }

json parse(const std::string& text) {
    json j = json::parse(text, nullptr, false);
    return j.is_object() ? j : json::object();
}

const json& entitiesOf(const json& prefab) {
    static const json kEmpty = json::array();
    const auto it = prefab.find("entities");
    return it != prefab.end() && it->is_array() ? *it : kEmpty;
}

// Las entidades de una instancia por su id en el prefab (sin entrar en otras
// instancias que cuelguen de ella).
std::unordered_map<std::string, Entity> linkedEntities(World& world, Entity root) {
    std::unordered_map<std::string, Entity> out;
    std::vector<Entity> stack{root};
    while (!stack.empty()) {
        const Entity e = stack.back();
        stack.pop_back();
        if (e != root && e.has<PrefabInstance>()) continue;
        if (const PrefabLink* link = e.tryGet<PrefabLink>(); link != nullptr && !link->source.empty()) out[link->source] = e;
        for (const entt::entity child : e.children()) stack.push_back(world.wrap(child));
    }
    return out;
}

// La entidad y sus descendientes, en profundidad (padres antes que hijos).
std::vector<Entity> subtree(World& world, Entity root) {
    std::vector<Entity> out;
    std::vector<Entity> stack{root};
    while (!stack.empty()) {
        const Entity e = stack.back();
        stack.pop_back();
        out.push_back(e);
        const auto& children = e.children();
        for (auto it = children.rbegin(); it != children.rend(); ++it) stack.push_back(world.wrap(*it));
    }
    return out;
}

bool writeText(const std::filesystem::path& file, const std::string& text, std::string* error) {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path temporary = std::filesystem::path(file).concat(".tmp");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "no se pudo escribir " + file.string();
            return false;
        }
        out << text;
        if (!out) {
            if (error) *error = "error escribiendo " + file.string();
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

// Quita los enlaces de las instancias que haya dentro de `root` (no los suyos).
void flattenNested(World& world, Entity root) {
    for (Entity e : subtree(world, root)) {
        if (e == root) continue;
        if (e.has<PrefabInstance>()) unpackInstance(world, e);
    }
}

}  // namespace

// --- Componentes ---------------------------------------------------------------

void PrefabInstance::reflect(PropertyVisitor& v) {
    v.asset({"prefab", "Prefab", "El asset .crprefab de esta instancia"}, prefab, assets::AssetType::Prefab);
    v.field({"revision", "Revision"}, revision, 0, 1 << 30);
    if (v.wantsAllFields()) {
        listField(v, {"overrides", "Cambios propios"}, overrides,
                  [](std::string& key, PropertyVisitor& item) { item.field({"key", "Clave"}, key); });
    }
}

void PrefabLink::reflect(PropertyVisitor& v) { v.field({"source", "Entidad del prefab"}, source); }

void registerPrefabComponents() {
    ComponentRegistry& registry = ComponentRegistry::instance();
    if (registry.find("PrefabInstance") == nullptr) {
        registry.registerComponent<PrefabInstance>("PrefabInstance", "Instancia de prefab", "Prefab", false, false);
    }
    if (registry.find("PrefabLink") == nullptr) {
        registry.registerComponent<PrefabLink>("PrefabLink", "Enlace de prefab", "Prefab", false, false);
    }
}

// --- Archivos ------------------------------------------------------------------

std::string readPrefabFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

Uuid prefabUuid(const std::string& text) { return Uuid::parse(parse(text).value("uuid", std::string{})); }
int prefabRevision(const std::string& text) { return parse(text).value("revision", 0); }
std::string prefabName(const std::string& text) { return parse(text).value("name", std::string{}); }

// --- Crear e instanciar ----------------------------------------------------------

bool createPrefab(World& world, Entity root, const std::filesystem::path& file, std::string* error) {
    if (!root.valid()) {
        if (error) *error = "no hay entidad";
        return false;
    }
    // Un prefab nuevo: lo que hubiera enlazado a otros prefabs queda suelto.
    if (root.has<PrefabInstance>()) unpackInstance(world, root);
    flattenNested(world, root);
    for (Entity e : subtree(world, root)) {
        if (e.has<PrefabLink>()) e.remove<PrefabLink>();
    }
    PrefabInstance& instance = root.add<PrefabInstance>();
    instance.prefab = assets::AssetRef{Uuid::generate(), assets::AssetType::Prefab};
    std::error_code ec;
    std::filesystem::remove(file, ec);  // no heredar la revision de otro archivo
    return !applyInstance(world, root, file, error).empty();
}

Entity instantiatePrefab(World& world, const std::string& text, Entity parent) {
    const json prefab = parse(text);
    json records = json::array();
    for (const json& record : entitiesOf(prefab)) {
        if (record.is_object()) records.push_back(record);
    }
    if (records.empty()) return {};
    std::vector<Entity> created;
    Entity root = buildEntities(world, records, /*remap=*/true, parent, &created);
    for (std::size_t i = 0; i < created.size() && i < records.size(); ++i) {
        created[i].add<PrefabLink>().source = records[i].value("uuid", std::string{});
    }
    if (root.valid()) {
        PrefabInstance& instance = root.add<PrefabInstance>();
        instance.prefab = assets::AssetRef{Uuid::parse(prefab.value("uuid", std::string{})), assets::AssetType::Prefab};
        instance.revision = prefab.value("revision", 0);
    }
    return root;
}

Entity instantiatePrefab(World& world, const std::filesystem::path& file, Entity parent) {
    const std::string text = readPrefabFile(file);
    return text.empty() ? Entity{} : instantiatePrefab(world, text, parent);
}

// --- Instancias ------------------------------------------------------------------

Entity prefabRoot(Entity e) {
    while (e.valid()) {
        if (e.has<PrefabInstance>()) return e;
        e = e.parent();
    }
    return {};
}

std::vector<Entity> prefabInstances(World& world, const Uuid& prefab) {
    std::vector<Entity> out;
    for (const entt::entity h : world.registry().view<PrefabInstance>()) {
        const Entity e = world.wrap(h);
        if (e.get<PrefabInstance>().prefab.uuid == prefab) out.push_back(e);
    }
    return out;
}

void recordOverrides(World& world, Entity root, const std::string& text) {
    PrefabInstance* instance = root.valid() ? root.tryGet<PrefabInstance>() : nullptr;
    if (instance == nullptr) return;
    const json prefab = parse(text);
    std::set<std::string> overrides(instance->overrides.begin(), instance->overrides.end());
    const auto links = linkedEntities(world, root);
    for (const json& record : entitiesOf(prefab)) {
        if (!record.is_object()) continue;
        const std::string source = record.value("uuid", std::string{});
        const auto it = links.find(source);
        if (it == links.end()) {
            overrides.insert(source + "|-");  // la instancia la borro
            continue;
        }
        const Entity e = it->second;
        const bool is_root = e == root;
        const json mine = entityRecord(world, e, true);
        for (const char* key : {"name", "active", "tag", "layer", "static"}) {
            if (is_root && std::string(key) == "name") continue;  // el nombre de la raiz es de la instancia
            if (mine.value(key, json()) != record.value(key, json())) overrides.insert(source + "|#" + key);
        }
        const json& theirs = record.contains("components") ? record["components"] : json::object();
        const json& ours = mine["components"];
        for (const auto& [name, data] : ours.items()) {
            if (technical(name) || (is_root && name == "Transform")) continue;
            if (!theirs.contains(name)) {
                overrides.insert(source + "|+" + name);
                continue;
            }
            const json& base = theirs[name];
            for (const auto& [key, value] : data.items()) {
                if (!base.contains(key) || base[key] != value) overrides.insert(source + "|" + name + "|" + key);
            }
        }
        for (const auto& [name, data] : theirs.items()) {
            if (!technical(name) && !ours.contains(name)) overrides.insert(source + "|-" + name);
        }
    }
    instance->overrides.assign(overrides.begin(), overrides.end());
}

void syncInstance(World& world, Entity root, const std::string& text) {
    if (!root.valid() || !root.has<PrefabInstance>()) return;
    const json prefab = parse(text);
    const std::vector<std::string> list = root.get<PrefabInstance>().overrides;
    const std::unordered_set<std::string> overrides(list.begin(), list.end());
    const auto overridden = [&](const std::string& key) { return overrides.count(key) != 0; };
    const auto links = linkedEntities(world, root);
    auto resolved = links;
    std::unordered_set<std::string> in_prefab;

    for (const json& record : entitiesOf(prefab)) {
        if (!record.is_object()) continue;
        const std::string source = record.value("uuid", std::string{});
        in_prefab.insert(source);
        const auto parent_it = record.find("parent");
        const std::string parent_source = parent_it != record.end() && parent_it->is_string() ? parent_it->get<std::string>() : std::string{};
        Entity e;
        if (const auto it = resolved.find(source); it != resolved.end()) {
            e = it->second;
        } else {
            // Nueva en el prefab (salvo que la instancia la borrara).
            if (parent_source.empty() || overridden(source + "|-")) continue;
            const auto parent = resolved.find(parent_source);
            if (parent == resolved.end()) continue;
            e = world.create(record.value("name", std::string{"GameObject"}), parent->second);
            e.add<PrefabLink>().source = source;
            resolved[source] = e;
        }
        const bool is_root = e == root;
        if (!is_root && !overridden(source + "|#name")) e.setName(record.value("name", e.name()));
        EntityInfo& info = e.get<EntityInfo>();
        if (!overridden(source + "|#active")) info.active = record.value("active", info.active);
        if (!overridden(source + "|#tag")) info.tag = record.value("tag", info.tag);
        if (!overridden(source + "|#layer")) info.layer = record.value("layer", info.layer);
        if (!overridden(source + "|#static")) info.is_static = record.value("static", false);
        if (!is_root) {
            const auto parent = resolved.find(parent_source);
            if (parent != resolved.end() && e.parent() != parent->second) e.setParent(parent->second, false);
        }
        const json& components = record.contains("components") && record["components"].is_object() ? record["components"] : json::object();
        for (const auto& [name, data] : components.items()) {
            if (technical(name) || (is_root && name == "Transform") || overridden(source + "|-" + name)) continue;
            const ComponentType* type = ComponentRegistry::instance().find(name);
            if (type == nullptr) continue;
            if (!type->has(world, e.handle())) type->add(world, e.handle());
            json filtered = json::object();
            for (const auto& [key, value] : data.items()) {
                if (!overridden(source + "|" + name + "|" + key)) filtered[key] = value;
            }
            JsonReader reader(filtered);
            type->reflect(world, e.handle(), reader);
        }
        // Componentes que el prefab ya no tiene (salvo los que anadio la instancia).
        for (const ComponentType& type : ComponentRegistry::instance().types()) {
            if (technical(type.name) || !type.removable || !type.has(world, e.handle())) continue;
            if (components.contains(type.name) || overridden(source + "|+" + type.name)) continue;
            type.remove(world, e.handle());
        }
    }
    // Entidades que ya no estan en el prefab.
    for (const auto& [source, e] : links) {
        if (e == root || in_prefab.count(source) != 0) continue;
        if (world.registry().valid(e.handle())) world.destroy(e);
    }
    root.get<PrefabInstance>().revision = prefab.value("revision", 0);
}

std::string applyInstance(World& world, Entity root, const std::filesystem::path& file, std::string* error) {
    if (!root.valid() || !root.has<PrefabInstance>()) {
        if (error) *error = "no es una instancia de prefab";
        return {};
    }
    const json previous = parse(readPrefabFile(file));
    flattenNested(world, root);
    // Lo anadido en la instancia pasa a ser parte del prefab.
    const std::vector<Entity> entities = subtree(world, root);
    for (Entity e : entities) {
        if (!e.has<PrefabLink>() || e.get<PrefabLink>().source.empty()) e.add<PrefabLink>().source = Uuid::generate().toString();
    }
    // La posicion de la raiz es de cada instancia: en el prefab se queda la de
    // antes (o en el origen si es nuevo).
    json root_transform;
    for (const json& record : entitiesOf(previous)) {
        const auto parent = record.find("parent");
        if (parent != record.end() && parent->is_string()) continue;
        if (record.contains("components") && record["components"].contains("Transform")) root_transform = record["components"]["Transform"];
        break;
    }
    json records = json::array();
    for (Entity e : entities) {
        json record = entityRecord(world, e, e == root);
        record["uuid"] = e.get<PrefabLink>().source;
        record["parent"] = e == root ? json(nullptr) : json(e.parent().get<PrefabLink>().source);
        json& components = record["components"];
        components.erase("PrefabInstance");
        components.erase("PrefabLink");
        if (e == root && components.contains("Transform")) {
            if (root_transform.is_object()) {
                components["Transform"] = root_transform;
            } else {
                components["Transform"]["position"] = json::array({0.0f, 0.0f, 0.0f});
            }
        }
        records.push_back(std::move(record));
    }
    PrefabInstance& instance = root.get<PrefabInstance>();
    Uuid uuid = Uuid::parse(previous.value("uuid", std::string{}));
    if (!uuid.valid()) uuid = instance.prefab.uuid.valid() ? instance.prefab.uuid : Uuid::generate();
    json out;
    out["format"] = "CramionPrefab";
    out["version"] = 1;
    out["uuid"] = uuid.toString();
    out["name"] = root.name();
    out["revision"] = previous.value("revision", 0) + 1;
    out["entities"] = std::move(records);
    const std::string text = out.dump(2);
    if (!writeText(file, text, error)) return {};
    instance.prefab = assets::AssetRef{uuid, assets::AssetType::Prefab};
    instance.revision = out["revision"].get<int>();
    instance.overrides.clear();
    return text;
}

void revertInstance(World& world, Entity root, const std::string& text) {
    if (!root.valid() || !root.has<PrefabInstance>()) return;
    root.get<PrefabInstance>().overrides.clear();
    syncInstance(world, root, text);
}

void unpackInstance(World& world, Entity root) {
    if (!root.valid() || !root.has<PrefabInstance>()) return;
    for (auto [source, e] : linkedEntities(world, root)) {
        if (e.has<PrefabLink>()) e.remove<PrefabLink>();
    }
    root.remove<PrefabInstance>();
}

void detachCopiedLinks(World& world, Entity copy) {
    if (!copy.valid() || copy.has<PrefabInstance>()) return;
    std::vector<Entity> stack{copy};
    while (!stack.empty()) {
        Entity e = stack.back();
        stack.pop_back();
        if (e.has<PrefabInstance>()) continue;
        if (e.has<PrefabLink>()) e.remove<PrefabLink>();
        for (const entt::entity child : e.children()) stack.push_back(world.wrap(child));
    }
}

int syncOutdatedInstances(World& world, const std::function<std::string(const Uuid&)>& text_of) {
    std::vector<Entity> roots;
    for (const entt::entity h : world.registry().view<PrefabInstance>()) roots.push_back(world.wrap(h));
    struct Known {
        std::string text;
        int revision = 0;
    };
    std::unordered_map<std::string, Known> known;
    int changed = 0;
    for (Entity root : roots) {
        if (!world.registry().valid(root.handle()) || !root.has<PrefabInstance>()) continue;
        const Uuid uuid = root.get<PrefabInstance>().prefab.uuid;
        if (!uuid.valid()) continue;
        auto it = known.find(uuid.toString());
        if (it == known.end()) {
            std::string text = text_of(uuid);
            const int revision = text.empty() ? 0 : prefabRevision(text);
            it = known.emplace(uuid.toString(), Known{std::move(text), revision}).first;
        }
        if (it->second.text.empty() || root.get<PrefabInstance>().revision == it->second.revision) continue;
        syncInstance(world, root, it->second.text);
        ++changed;
    }
    return changed;
}

}  // namespace cramion::ecs
