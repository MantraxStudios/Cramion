#include "CramionCore/ecs/SceneSerializer.h"

#include "SerializerInternal.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

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
        if (e != root && e.get<EntityInfo>().transient) continue;  // teselas de terreno
        entities.push_back(entityRecord(world, e, e == root));
        const auto& children = e.children();
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            stack.push_back(world.wrap(*it));
        }
    }
}

namespace {

// `skip_existing` (recrear con los mismos UUID: World Partition): un registro
// cuyo UUID ya esta en el mundo no se recrea (ni sus hijos), con un aviso. Si
// no, World::create le daria un UUID nuevo y quedarian dos copias del objeto.
Entity buildEntitiesImpl(World& world, const json& entities, bool remap, Entity root_parent,
                         std::vector<Entity>* created, bool skip_existing) {
    std::unordered_map<std::string, Entity> by_uuid;
    std::unordered_set<std::string> skipped;
    std::vector<Entity> built;
    Entity first;
    for (const json& record : entities) {
        if (!record.is_object()) {
            continue;
        }
        const std::string uuid_text = record.value("uuid", std::string{});
        if (skip_existing && !remap) {
            const auto p = record.find("parent");
            if (p != record.end() && p->is_string() && skipped.count(p->get<std::string>()) != 0) {
                skipped.insert(uuid_text);  // hijo de uno que no se recreo
                continue;
            }
            const Uuid existing_id = Uuid::parse(uuid_text);
            if (existing_id.valid()) {
                if (const Entity existing = world.find(existing_id); existing.valid()) {
                    std::cerr << "[Escena] UUID repetido " << uuid_text << " (\"" << record.value("name", std::string{})
                              << "\"): ya esta en el mundo como \"" << existing.name() << "\", no se recrea\n";
                    skipped.insert(uuid_text);
                    continue;
                }
            }
        }
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
        built.push_back(e);
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
    // Con UUIDs nuevos (pegar, instanciar un prefab), las referencias entre las
    // entidades pegadas pasan a las copias; las de fuera se quedan como estan.
    if (remap) {
        std::unordered_map<Uuid, Uuid> refs;
        for (const auto& [old_text, e] : by_uuid) {
            const Uuid old_uuid = Uuid::parse(old_text);
            if (old_uuid.valid() && e.valid()) refs[old_uuid] = e.uuid();
        }
        if (!refs.empty()) {
            for (const Entity& e : built) world.remapEntityRefs(e, refs);
        }
    }
    return first;
}

}  // namespace

Entity buildEntities(World& world, const json& entities, bool remap, Entity root_parent, std::vector<Entity>* created) {
    return buildEntitiesImpl(world, entities, remap, root_parent, created, /*skip_existing=*/false);
}

}  // namespace detail

struct ParsedEntities {
    json root;
};

using detail::buildEntities;
using detail::entityRecord;
using detail::collectSubtree;

std::string serializeWorld(const World& const_world) { return serializeWorld(const_world, {}); }

std::string serializeWorld(const World& const_world, const std::function<bool(const Entity&)>& skip_root) {
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
        const Entity e = world.wrap(r);
        if (e.get<EntityInfo>().transient || (skip_root && skip_root(e))) continue;
        collectSubtree(world, e, entities);
    }
    // Las raices sin "parent": collectSubtree marca la raiz de cada arbol
    // como raiz de copia (parent null), que en una escena es lo correcto.
    root["entities"] = std::move(entities);
    return root.dump(2);
}

// Escenas guardadas con versiones anteriores del formato: valores por defecto
// que cambiaron. Solo se toca lo que tenia EXACTAMENTE el valor por defecto
// viejo (lo ajustado a mano se respeta).
static void migrateScene(json& root, int version) {
    if (version >= 2) return;
    auto it = root.find("entities");
    if (it == root.end() || !it->is_array()) return;
    for (json& entity : *it) {
        auto components = entity.find("components");
        if (components == entity.end() || !components->is_object()) continue;
        auto post = components->find("PostProcessing");
        if (post == components->end() || !post->is_object()) continue;
        auto density = post->find("volumetric_density");
        if (density != post->end() && density->is_number() && std::abs(density->get<double>() - 0.02) < 1e-6) {
            *density = 0.004;
        }
    }
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
    migrateScene(root, version);
    // El origen de la escena, antes de vaciar: las teselas de terreno que se
    // quedan (deshacer, parar Play) se recolocan con el (World::clearPersistent)
    // y no saltan (origen_viejo - origen_nuevo) en el mundo real.
    DVec3 origin{};
    if (const auto it = root.find("origin"); it != root.end() && it->is_array() && it->size() == 3 &&
                                             (*it)[0].is_number() && (*it)[1].is_number() && (*it)[2].is_number()) {
        origin = DVec3{(*it)[0].get<double>(), (*it)[1].get<double>(), (*it)[2].get<double>()};
    }
    world.clearPersistent(origin);  // las teselas de terreno se quedan (en su sitio)
    const Uuid uuid = Uuid::parse(root.value("uuid", std::string{}));
    world.setSceneUuid(uuid.valid() ? uuid : Uuid::generate());
    world.setSceneName(root.value("name", std::string{"Escena"}));
    if (const auto it = root.find("entities"); it != root.end() && it->is_array()) {
        buildEntities(world, *it, /*remap=*/false, Entity{});
    }
    return true;
}

bool saveScene(const World& world, const std::filesystem::path& path, std::string* error) {
    return saveScene(world, path, error, {});
}

bool saveScene(const World& world, const std::filesystem::path& path, std::string* error,
               const std::function<bool(const Entity&)>& skip_root) {
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary);
        if (!file) {
            if (error) *error = "no se pudo escribir " + path.string();
            return false;
        }
        file << serializeWorld(world, skip_root);
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
    // Otra escena: nada de la anterior se queda (ni sus teselas transitorias,
    // que clearPersistent conservaria) y los trabajos en otros hilos de la
    // anterior se invalidan (generacion nueva). Quien cambia de escena llama
    // antes a resetSceneState (SceneReset.h) para vaciar tambien los
    // sistemas; esto es la red de seguridad si alguno no lo hizo.
    world.clear();
    world.beginNewSceneGeneration();
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

Entity restoreEntities(World& world, const std::string& text, Entity parent) {
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return {};
    }
    const auto it = root.find("entities");
    if (it == root.end() || !it->is_array()) {
        return {};
    }
    return detail::buildEntitiesImpl(world, *it, /*remap=*/false, parent, nullptr, /*skip_existing=*/true);
}

std::shared_ptr<const ParsedEntities> parseEntities(const std::string& text) {
    auto parsed = std::make_shared<ParsedEntities>();
    parsed->root = json::parse(text, nullptr, false);
    if (parsed->root.is_discarded() || !parsed->root.is_object()) return nullptr;
    const auto it = parsed->root.find("entities");
    if (it == parsed->root.end() || !it->is_array()) return nullptr;
    return parsed;
}

Entity restoreParsedEntities(World& world, const ParsedEntities& parsed, Entity parent) {
    const auto it = parsed.root.find("entities");
    if (it == parsed.root.end() || !it->is_array()) return {};
    return detail::buildEntitiesImpl(world, *it, /*remap=*/false, parent, nullptr, /*skip_existing=*/true);
}

Uuid parsedRootUuid(const ParsedEntities& parsed) {
    const auto it = parsed.root.find("entities");
    if (it == parsed.root.end() || !it->is_array() || it->empty() || !(*it)[0].is_object()) return {};
    return Uuid::parse((*it)[0].value("uuid", std::string{}));
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

// --- Registros sueltos (historial de deshacer) ------------------------------------

std::string serializeEntityRecord(const World& const_world, Entity entity) {
    World& world = const_cast<World&>(const_world);  // reflect() no escribe al guardar
    if (!entity.valid()) return {};
    return entityRecord(world, entity, /*is_root_of_copy=*/false).dump();
}

bool applyEntityRecord(World& world, Entity e, const std::string& text, const std::string* current, Uuid* parent,
                       std::string* error) {
    if (!e.valid()) {
        if (error) *error = "la entidad no existe";
        return false;
    }
    const json record = json::parse(text, nullptr, false);
    if (record.is_discarded() || !record.is_object()) {
        if (error) *error = "registro no valido";
        return false;
    }
    // Como esta ahora (si se sabe): los componentes que no cambian no se tocan.
    json now;
    if (current != nullptr && !current->empty()) {
        now = json::parse(*current, nullptr, false);
        if (now.is_discarded() || !now.is_object()) now = json();
    }
    const std::string name = record.value("name", std::string{"GameObject"});
    if (e.name() != name) e.setName(name);
    {
        EntityInfo& info = e.get<EntityInfo>();
        info.tag = record.value("tag", std::string{});
        info.layer = record.value("layer", 0);
        info.is_static = record.value("static", false);
        info.static_batched = record.value("static_batched", false);
    }
    // setActive sube structureVersion: los recorridos de lo activo se rehacen.
    const bool active = record.value("active", true);
    if (e.activeSelf() != active) e.setActive(active);

    static const json kNone = json::object();
    const auto wanted_it = record.find("components");
    const json& wanted = wanted_it != record.end() && wanted_it->is_object() ? *wanted_it : kNone;
    const json* before = nullptr;
    if (now.is_object()) {
        if (const auto it = now.find("components"); it != now.end() && it->is_object()) before = &*it;
    }
    ComponentRegistry& registry = ComponentRegistry::instance();
    for (const ComponentType& type : registry.types()) {
        if (!wanted.contains(type.name) && type.has(world, e.handle())) type.remove(world, e.handle());
    }
    for (const auto& [component, data] : wanted.items()) {
        const ComponentType* type = registry.find(component);
        if (type == nullptr) {
            std::cerr << "[Escena] Componente desconocido \"" << component << "\" en " << e.name() << ": se ignora\n";
            continue;
        }
        const bool has = type->has(world, e.handle());
        if (has && before != nullptr) {
            const auto it = before->find(component);
            if (it != before->end() && *it == data) continue;  // igual que ahora
        }
        // Desde cero, como al leer la escena: nada del estado de ejecucion
        // viejo (maquina del Animator, malla creada por codigo...). El
        // Transform se escribe en su sitio (su version sigue subiendo).
        if (has && component != "Transform") type->remove(world, e.handle());
        if (!type->has(world, e.handle())) type->add(world, e.handle());
        detail::JsonReader reader(data);
        type->reflect(world, e.handle(), reader);
    }
    if (parent != nullptr) {
        const auto it = record.find("parent");
        *parent = it != record.end() && it->is_string() ? Uuid::parse(it->get<std::string>()) : Uuid{};
    }
    return true;
}

std::string assembleScene(const SceneHeader& header, std::span<const std::string* const> records) {
    json head;
    head["format"] = "CramionScene";
    head["version"] = kSceneFormatVersion;
    head["uuid"] = header.uuid.toString();
    head["name"] = header.name;
    if (header.origin != DVec3{}) head["origin"] = json::array({header.origin.x, header.origin.y, header.origin.z});
    std::string out = head.dump();
    std::size_t total = out.size() + 16;
    for (const std::string* r : records) total += (r != nullptr ? r->size() : 0) + 1;
    out.reserve(total);
    out.pop_back();  // la '}' del final: las entidades van dentro
    out += ",\"entities\":[";
    bool first = true;
    for (const std::string* r : records) {
        if (r == nullptr || r->empty()) continue;
        if (!first) out += ',';
        out += *r;
        first = false;
    }
    out += "]}";
    return out;
}

}  // namespace cramion::ecs
