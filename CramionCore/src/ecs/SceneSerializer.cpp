#include "CramionCore/ecs/SceneSerializer.h"

#include "SerializerInternal.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>

namespace cramion::ecs {

using nlohmann::json;

namespace detail {

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
    // Solo si estan puestos: las escenas de siempre no cambian.
    if (info.is_static) record["static"] = true;
    if (info.static_batched) record["static_batched"] = true;
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

Entity buildEntities(World& world, const json& entities, bool remap, Entity root_parent, std::vector<Entity>* created) {
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
        info.is_static = record.value("static", false);
        info.static_batched = record.value("static_batched", false);
        by_uuid[uuid_text] = e;
        if (created != nullptr) created->push_back(e);
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

}  // namespace detail

using detail::buildEntities;
using detail::entityRecord;
using detail::collectSubtree;

std::string serializeWorld(const World& const_world) {
    World& world = const_cast<World&>(const_world);  // reflect() no escribe al guardar
    json root;
    root["format"] = "CramionScene";
    root["version"] = kSceneFormatVersion;
    root["uuid"] = world.sceneUuid().toString();
    root["name"] = world.sceneName();
    // Origen flotante: las posiciones son relativas a el (sin perder precision).
    if (world.origin() != DVec3{}) {
        root["origin"] = json::array({world.origin().x, world.origin().y, world.origin().z});
    }
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
    if (const auto it = root.find("origin"); it != root.end() && it->is_array() && it->size() == 3) {
        world.setOrigin(DVec3{(*it)[0].get<double>(), (*it)[1].get<double>(), (*it)[2].get<double>()});
    }
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

std::string componentToJson(World& world, Entity entity, const std::string& component) {
    const ComponentType* type = ComponentRegistry::instance().find(component);
    if (type == nullptr || !entity.valid() || !type->has(world, entity.handle())) return {};
    json out = json::object();
    detail::JsonWriter writer(out);
    type->reflect(world, entity.handle(), writer);
    return out.dump();
}

bool componentFromJson(World& world, Entity entity, const std::string& component, const std::string& fields,
                       std::string* error) {
    const ComponentType* type = ComponentRegistry::instance().find(component);
    if (type == nullptr) {
        if (error) *error = "componente desconocido: " + component;
        return false;
    }
    if (!entity.valid()) {
        if (error) *error = "la entidad no existe";
        return false;
    }
    const json values = fields.empty() ? json::object() : json::parse(fields, nullptr, false);
    if (!values.is_object()) {
        if (error) *error = "los valores deben ser un objeto JSON";
        return false;
    }
    if (!type->has(world, entity.handle())) type->add(world, entity.handle());
    detail::JsonReader reader(values);
    type->reflect(world, entity.handle(), reader);
    return true;
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
