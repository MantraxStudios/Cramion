#include "CramionCore/ecs/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>

namespace cramion::ecs {

using nlohmann::json;

namespace {

json vec3(const core::Vec3& v) {
    return json::array({v.x, v.y, v.z});
}

bool readFloat(const json& j, float& out) {
    if (!j.is_number()) {
        return false;
    }
    out = j.get<float>();
    return true;
}

// Escribe cada propiedad en un objeto JSON por su clave.
class JsonWriter final : public PropertyVisitor {
public:
    explicit JsonWriter(json& out) : out_(out) {}

    bool field(const Meta& m, float& v, const FloatRange&) override {
        out_[m.key] = v;
        return false;
    }
    bool field(const Meta& m, int& v, int, int) override {
        out_[m.key] = v;
        return false;
    }
    bool field(const Meta& m, bool& v) override {
        out_[m.key] = v;
        return false;
    }
    bool field(const Meta& m, std::string& v) override {
        out_[m.key] = v;
        return false;
    }
    bool field(const Meta& m, core::Vec3& v, Vec3Kind) override {
        out_[m.key] = vec3(v);
        return false;
    }
    bool field(const Meta& m, core::Vec2& v, float) override {
        out_[m.key] = json::array({v.x, v.y});
        return false;
    }
    bool enumeration(const Meta& m, int& v, std::span<const char* const> names) override {
        // Por nombre: reordenar el enum no cambia el significado del archivo.
        if (v >= 0 && v < static_cast<int>(names.size())) {
            out_[m.key] = names[static_cast<std::size_t>(v)];
        } else {
            out_[m.key] = v;
        }
        return false;
    }
    bool asset(const Meta& m, assets::AssetRef& ref, assets::AssetType) override {
        out_[m.key] = ref.valid() ? json(ref.uuid.toString()) : json(nullptr);
        return false;
    }

private:
    json& out_;
};

// Lee las propiedades que esten; las que falten no se tocan.
class JsonReader final : public PropertyVisitor {
public:
    explicit JsonReader(const json& in) : in_(in) {}

    bool field(const Meta& m, float& v, const FloatRange&) override {
        const json* j = find(m);
        return j != nullptr && readFloat(*j, v);
    }
    bool field(const Meta& m, int& v, int, int) override {
        const json* j = find(m);
        if (j == nullptr || !j->is_number()) {
            return false;
        }
        v = j->get<int>();
        return true;
    }
    bool field(const Meta& m, bool& v) override {
        const json* j = find(m);
        if (j == nullptr || !j->is_boolean()) {
            return false;
        }
        v = j->get<bool>();
        return true;
    }
    bool field(const Meta& m, std::string& v) override {
        const json* j = find(m);
        if (j == nullptr || !j->is_string()) {
            return false;
        }
        v = j->get<std::string>();
        return true;
    }
    bool field(const Meta& m, core::Vec3& v, Vec3Kind) override {
        const json* j = find(m);
        if (j == nullptr || !j->is_array() || j->size() < 3) {
            return false;
        }
        core::Vec3 r = v;
        if (!readFloat((*j)[0], r.x) || !readFloat((*j)[1], r.y) || !readFloat((*j)[2], r.z)) {
            return false;
        }
        v = r;
        return true;
    }
    bool field(const Meta& m, core::Vec2& v, float) override {
        const json* j = find(m);
        if (j == nullptr || !j->is_array() || j->size() < 2) {
            return false;
        }
        core::Vec2 r = v;
        if (!readFloat((*j)[0], r.x) || !readFloat((*j)[1], r.y)) {
            return false;
        }
        v = r;
        return true;
    }
    bool enumeration(const Meta& m, int& v, std::span<const char* const> names) override {
        const json* j = find(m);
        if (j == nullptr) {
            return false;
        }
        if (j->is_string()) {
            const std::string name = j->get<std::string>();
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (name == names[i]) {
                    v = static_cast<int>(i);
                    return true;
                }
            }
            return false;
        }
        if (j->is_number_integer()) {
            v = j->get<int>();
            return true;
        }
        return false;
    }
    bool asset(const Meta& m, assets::AssetRef& ref, assets::AssetType type) override {
        const json* j = find(m);
        if (j == nullptr) {
            return false;
        }
        ref.type = type;
        ref.uuid = j->is_string() ? Uuid::parse(j->get<std::string>()) : Uuid{};
        return true;
    }

private:
    const json* find(const Meta& m) const {
        const auto it = in_.find(m.key);
        return it == in_.end() ? nullptr : &*it;
    }
    const json& in_;
};

json entityRecord(World& world, Entity e, bool is_root_of_copy) {
    json record;
    record["uuid"] = e.uuid().toString();
    record["name"] = e.name();
    const Entity parent = e.parent();
    record["parent"] = (parent.valid() && !is_root_of_copy) ? json(parent.uuid().toString())
                                                            : json(nullptr);
    const EntityInfo& info = e.get<EntityInfo>();
    record["active"] = info.active;
    record["tag"] = info.tag;
    record["layer"] = info.layer;
    json components = json::object();
    for (const ComponentType& type : ComponentRegistry::instance().types()) {
        if (!type.has(world, e.handle())) {
            continue;
        }
        json data = json::object();
        JsonWriter writer(data);
        type.reflect(world, e.handle(), writer);
        components[type.name] = std::move(data);
    }
    record["components"] = std::move(components);
    return record;
}

void collectSubtree(World& world, Entity root, json& entities) {
    std::vector<Entity> stack{root};
    while (!stack.empty()) {
        Entity e = stack.back();
        stack.pop_back();
        entities.push_back(entityRecord(world, e, e == root));
        const auto& children = e.children();
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            stack.push_back(world.wrap(*it));
        }
    }
}

// Crea las entidades de una lista de registros. `remap`: UUIDs nuevos
// (pegar) en lugar de los del archivo. `root_parent`: padre de los registros
// sin padre. Devuelve la primera entidad creada.
Entity buildEntities(World& world, const json& entities, bool remap, Entity root_parent) {
    std::unordered_map<std::string, Entity> by_uuid;
    Entity first;
    for (const json& record : entities) {
        if (!record.is_object()) {
            continue;
        }
        const std::string uuid_text = record.value("uuid", std::string{});
        Entity parent = root_parent;
        if (const auto it = record.find("parent"); it != record.end() && it->is_string()) {
            const auto found = by_uuid.find(it->get<std::string>());
            if (found != by_uuid.end()) {
                parent = found->second;
            } else if (!remap) {
                parent = world.find(Uuid::parse(it->get<std::string>()));
            }
        }
        const Uuid uuid = remap ? Uuid{} : Uuid::parse(uuid_text);
        Entity e = world.create(record.value("name", std::string{"GameObject"}), parent, uuid);
        EntityInfo& info = e.get<EntityInfo>();
        info.active = record.value("active", true);
        info.tag = record.value("tag", std::string{});
        info.layer = record.value("layer", 0);
        by_uuid[uuid_text] = e;
        if (!first.valid()) {
            first = e;
        }

        if (const auto it = record.find("components"); it != record.end() && it->is_object()) {
            for (const auto& [name, data] : it->items()) {
                const ComponentType* type = ComponentRegistry::instance().find(name);
                if (type == nullptr) {
                    std::cerr << "[Escena] Componente desconocido \"" << name << "\" en "
                              << e.name() << ": se ignora\n";
                    continue;
                }
                type->add(world, e.handle());
                JsonReader reader(data);
                type->reflect(world, e.handle(), reader);
            }
        }
    }
    return first;
}

}  // namespace

std::string serializeWorld(const World& const_world) {
    World& world = const_cast<World&>(const_world);  // reflect() no escribe al guardar
    json root;
    root["format"] = "CramionScene";
    root["version"] = kSceneFormatVersion;
    root["uuid"] = world.sceneUuid().toString();
    root["name"] = world.sceneName();
    json entities = json::array();
    for (const entt::entity r : world.roots()) {
        collectSubtree(world, world.wrap(r), entities);
    }
    // Las raices sin "parent": collectSubtree marca la raiz de cada arbol
    // como raiz de copia (parent null), que en una escena es lo correcto.
    root["entities"] = std::move(entities);
    return root.dump(2);
}

bool deserializeWorld(World& world, const std::string& text, std::string* error) {
    json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        if (error) *error = "JSON no valido";
        return false;
    }
    if (root.value("format", std::string{}) != "CramionScene") {
        if (error) *error = "no es una escena de Cramion";
        return false;
    }
    const int version = root.value("version", 0);
    if (version > kSceneFormatVersion) {
        std::cerr << "[Escena] Formato " << version << " mas nuevo que el del motor ("
                  << kSceneFormatVersion << "): se lee lo que se entienda\n";
    }
    world.clear();
    const Uuid uuid = Uuid::parse(root.value("uuid", std::string{}));
    world.setSceneUuid(uuid.valid() ? uuid : Uuid::generate());
    world.setSceneName(root.value("name", std::string{"Escena"}));
    if (const auto it = root.find("entities"); it != root.end() && it->is_array()) {
        buildEntities(world, *it, /*remap=*/false, Entity{});
    }
    return true;
}

bool saveScene(const World& world, const std::filesystem::path& path, std::string* error) {
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary);
        if (!file) {
            if (error) *error = "no se pudo escribir " + path.string();
            return false;
        }
        file << serializeWorld(world);
        if (!file) {
            if (error) *error = "error escribiendo " + path.string();
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        if (error) *error = ec.message();
        return false;
    }
    return true;
}

bool loadScene(World& world, const std::filesystem::path& path, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "no se pudo abrir " + path.string();
        return false;
    }
    std::stringstream text;
    text << file.rdbuf();
    return deserializeWorld(world, text.str(), error);
}

std::string serializeEntity(const World& const_world, Entity entity) {
    World& world = const_cast<World&>(const_world);
    json root;
    root["format"] = "CramionEntities";
    root["version"] = kSceneFormatVersion;
    json entities = json::array();
    if (entity.valid()) {
        collectSubtree(world, entity, entities);
    }
    root["entities"] = std::move(entities);
    return root.dump();
}

Entity pasteEntities(World& world, const std::string& text, Entity parent) {
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return {};
    }
    const auto it = root.find("entities");
    if (it == root.end() || !it->is_array()) {
        return {};
    }
    return buildEntities(world, *it, /*remap=*/true, parent);
}

Uuid readSceneUuid(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    const json root = json::parse(file, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return {};
    }
    return Uuid::parse(root.value("uuid", std::string{}));
}

}  // namespace cramion::ecs
