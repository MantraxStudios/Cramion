// Prefabs (ver Prefab.h): archivos .crprefab, instancias enlazadas, cambios
// propios y actualizacion de las instancias.

#include "CramionCore/ecs/Prefab.h"

#include "CramionCore/ecs/Components.h"
#include "PrefabNesting.h"
#include "SerializerInternal.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <iostream>
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

// Un Bone Socket que sigue a su hueso: RenderSync le pone la Transform en
// cada frame, asi que la suya no es un cambio propio ni se guarda la pose del
// momento en el prefab.
bool followsBone(Entity e) {
    const BoneSocket* socket = e.tryGet<BoneSocket>();
    return socket != nullptr && socket->mode == SocketMode::Follow && !socket->bone.empty();
}

json parse(const std::string& text) {
    json j = json::parse(text, nullptr, false);
    // (Con el operador ?: el resultado era una copia profunda de todo el
    // documento; asi se mueve.)
    if (!j.is_object()) return json::object();
    return j;
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
    detail::notePrefabWrite();
    return true;
}

// Cambia, en el JSON de un componente ya escrito, las referencias a entidades
// (PropertyVisitor::entity) segun `remap`. El componente vivo solo da la
// estructura (claves y listas): no se modifica.
class JsonEntityRefRemapper final : public PropertyVisitor {
public:
    JsonEntityRefRemapper(json& data, const std::unordered_map<std::string, std::string>& remap)
        : stack_{&data}, remap_(remap) {}

    bool field(const Meta&, float&, const FloatRange&) override { return false; }
    bool field(const Meta&, int&, int, int) override { return false; }
    bool field(const Meta&, bool&) override { return false; }
    bool field(const Meta&, std::string&) override { return false; }
    bool field(const Meta&, core::Vec3&, Vec3Kind) override { return false; }
    bool field(const Meta&, core::Vec2&, float) override { return false; }
    bool enumeration(const Meta&, int&, std::span<const char* const>) override { return false; }
    bool asset(const Meta&, assets::AssetRef&, assets::AssetType) override { return false; }
    bool layerMask(const Meta&, std::uint32_t&) override { return false; }
    bool entity(const Meta& m, Uuid&) override {
        json* value = find(m);
        if (value == nullptr || !value->is_string()) return false;
        const auto it = remap_.find(value->get<std::string>());
        if (it != remap_.end()) *value = it->second;
        return false;
    }
    bool beginList(const Meta& m, std::size_t& count) override {
        json* list = find(m);
        if (list == nullptr || !list->is_array() || list->size() != count) return false;
        lists_.push_back(list);
        return true;
    }
    bool beginListItem(std::size_t index) override {
        stack_.push_back(&(*lists_.back())[index]);
        return true;
    }
    void endListItem() override { stack_.pop_back(); }
    int endList() override {
        lists_.pop_back();
        return -1;
    }

private:
    json* find(const Meta& m) {
        json& in = *stack_.back();
        if (!in.is_object()) return nullptr;
        const auto it = in.find(m.key);
        return it == in.end() ? nullptr : &*it;
    }
    std::vector<json*> stack_;
    std::vector<json*> lists_;
    const std::unordered_map<std::string, std::string>& remap_;
};

// Pasa las referencias a entidades del registro de `e` (entityRecord) por `remap`.
void remapRecordRefs(World& world, Entity e, json& record, const std::unordered_map<std::string, std::string>& remap) {
    if (remap.empty() || !record.contains("components") || !record["components"].is_object()) return;
    json& components = record["components"];
    for (auto it = components.begin(); it != components.end(); ++it) {
        const ComponentType* type = ComponentRegistry::instance().find(it.key());
        if (type == nullptr || !type->has(world, e.handle())) continue;
        JsonEntityRefRemapper remapper(it.value(), remap);
        type->reflect(world, e.handle(), remapper);
    }
}

// UUID en el mundo -> id en el prefab de las entidades enlazadas de una instancia.
std::unordered_map<std::string, std::string> worldToSource(const std::unordered_map<std::string, Entity>& links) {
    std::unordered_map<std::string, std::string> out;
    for (const auto& [source, e] : links) {
        if (e.valid()) out[e.uuid().toString()] = source;
    }
    return out;
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
    // Uno de los de siempre sale tal cual (sin parsear: solo se buscan las claves).
    std::string text = readPrefabFileRaw(file);
    if (!prefabNeedsResolve(text)) return text;
    return resolvePrefabText(text, file);
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
    // Un prefab nuevo: lo que hubiera enlazado a otros prefabs queda suelto
    // (con prefab.SaveNested, las instancias de dentro siguen enlazadas y
    // applyInstance las guarda como anidados).
    if (root.has<PrefabInstance>()) unpackInstance(world, root);
    if (!detail::saveNestedEnabled()) flattenNested(world, root);
    std::vector<Entity> stack{root};
    while (!stack.empty()) {
        Entity e = stack.back();
        stack.pop_back();
        if (e != root && e.has<PrefabInstance>()) continue;  // instancia anidada: conserva sus enlaces
        if (e.has<PrefabLink>()) e.remove<PrefabLink>();
        for (const entt::entity child : e.children()) stack.push_back(world.wrap(child));
    }
    PrefabInstance& instance = root.add<PrefabInstance>();
    instance.prefab = assets::AssetRef{Uuid::generate(), assets::AssetType::Prefab};
    std::error_code ec;
    std::filesystem::remove(file, ec);  // no heredar la revision de otro archivo
    return !applyInstance(world, root, file, error).empty();
}

Entity instantiatePrefab(World& world, const std::string& raw_text, Entity parent) {
    // Con anidados o variante, el documento resuelto (plano).
    std::string resolved;
    if (prefabNeedsResolve(raw_text)) resolved = resolvePrefabText(raw_text);
    const std::string& text = resolved.empty() ? raw_text : resolved;
    const json prefab = parse(text);
    // Sin copiar los registros (antes, una copia profunda de todo el JSON en
    // cada instancia: ~45 ms con Valley.crprefab). buildEntities se salta lo
    // que no es un objeto, igual que esta lista: created[i] es records[i].
    const json& entities = entitiesOf(prefab);
    std::vector<const json*> records;
    if (entities.is_array()) records.reserve(entities.size());
    for (const json& record : entities) {
        if (record.is_object()) records.push_back(&record);
    }
    if (records.empty()) return {};
    std::vector<Entity> created;
    created.reserve(records.size());
    Entity root = buildEntities(world, entities, /*remap=*/true, parent, &created);
    for (std::size_t i = 0; i < created.size() && i < records.size(); ++i) {
        created[i].add<PrefabLink>().source = records[i]->value("uuid", std::string{});
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

struct ParsedPrefab {
    json root;
    int revision = 0;
};

std::shared_ptr<const ParsedPrefab> parsePrefabText(const std::string& raw_text) {
    std::string resolved;
    if (prefabNeedsResolve(raw_text)) resolved = resolvePrefabText(raw_text);
    const std::string& text = resolved.empty() ? raw_text : resolved;
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return nullptr;
    auto doc = std::make_shared<ParsedPrefab>();
    doc->revision = j.value("revision", 0);
    doc->root = std::move(j);
    return doc;
}

int prefabRevision(const ParsedPrefab& prefab) { return prefab.revision; }

void recordOverrides(World& world, Entity root, const std::string& text) {
    const std::shared_ptr<const ParsedPrefab> doc = parsePrefabText(text);
    recordOverrides(world, root, doc != nullptr ? *doc : ParsedPrefab{json::object(), 0});
}

void recordOverrides(World& world, Entity root, const ParsedPrefab& document) {
    PrefabInstance* instance = root.valid() ? root.tryGet<PrefabInstance>() : nullptr;
    if (instance == nullptr) return;
    const json& prefab = document.root;
    // Se recalculan desde cero (solo se llama con la misma revision): lo que
    // vuelve a tener el valor del prefab deja de ser un cambio propio.
    std::set<std::string> overrides;
    const auto links = linkedEntities(world, root);
    // Las referencias a entidades de la instancia se comparan con los ids del prefab.
    const auto to_source = worldToSource(links);
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
        const bool follows = followsBone(e);
        json mine = entityRecord(world, e, true);
        remapRecordRefs(world, e, mine, to_source);
        for (const char* key : {"name", "active", "tag", "layer", "static"}) {
            if (is_root && std::string(key) == "name") continue;  // el nombre de la raiz es de la instancia
            if (mine.value(key, json()) != record.value(key, json())) overrides.insert(source + "|#" + key);
        }
        const json& theirs = record.contains("components") ? record["components"] : json::object();
        const json& ours = mine["components"];
        for (const auto& [name, data] : ours.items()) {
            if (technical(name) || ((is_root || follows) && name == "Transform")) continue;
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
    // Solo si cambia (el historial de deshacer no ve un cambio que no hay).
    if (!std::equal(overrides.begin(), overrides.end(), instance->overrides.begin(), instance->overrides.end())) {
        instance->overrides.assign(overrides.begin(), overrides.end());
    }
}

void syncInstance(World& world, Entity root, const std::string& raw_text) {
    if (!root.valid() || !root.has<PrefabInstance>()) return;
    std::string resolved_text;
    if (prefabNeedsResolve(raw_text)) resolved_text = resolvePrefabText(raw_text);
    const json prefab = parse(resolved_text.empty() ? raw_text : resolved_text);
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
    // Las referencias que vienen del prefab (ids del prefab) pasan a las
    // entidades de esta instancia.
    std::unordered_map<Uuid, Uuid> refs;
    for (const auto& [source, e] : resolved) {
        const Uuid id = Uuid::parse(source);
        if (id.valid() && world.registry().valid(e.handle())) refs[id] = e.uuid();
    }
    for (const auto& [source, e] : resolved) {
        if (world.registry().valid(e.handle())) world.remapEntityRefs(e, refs);
    }
    root.get<PrefabInstance>().revision = prefab.value("revision", 0);
}

// Con prefab.SaveNested: las instancias de otros prefabs que hay dentro de
// `root` pasan a ser partes anidadas (sus entidades toman los ids derivados
// que tendran en el documento resuelto). Rechaza los ciclos.
static bool linkInnerInstances(World& world, Entity root, const Uuid& self, std::vector<detail::NewNestedPart>& added,
                        std::string* error) {
    for (Entity e : subtree(world, root)) {
        if (e == root || !world.registry().valid(e.handle()) || !e.has<PrefabInstance>()) continue;
        const Uuid child = e.get<PrefabInstance>().prefab.uuid;
        const std::filesystem::path child_file = locatePrefab(child);
        if (child_file.empty()) {
            std::cerr << "[Prefab] \"" << e.name() << "\": no se encuentra su prefab; se guarda como copia suelta\n";
            continue;  // flattenNested la deja suelta
        }
        if (child == self || (self.valid() && detail::prefabFileDependsOn(child_file, self))) {
            if (error) *error = "ciclo de prefabs: \"" + e.name() + "\" contiene (o es) el prefab que se guarda";
            return false;
        }
        const std::string nested = Uuid::generate().toString();
        for (auto& [source, linked] : linkedEntities(world, e)) {
            linked.get<PrefabLink>().source = linked == e ? nested : detail::nestedEntityId(nested, source);
        }
        e.remove<PrefabInstance>();
        added.push_back(detail::NewNestedPart{nested, child, child_file});
    }
    return true;
}

std::string applyInstance(World& world, Entity root, const std::filesystem::path& file, std::string* error) {
    if (!root.valid() || !root.has<PrefabInstance>()) {
        if (error) *error = "no es una instancia de prefab";
        return {};
    }
    const json previous = parse(readPrefabFileRaw(file));
    // Con anidados o variante: la estructura se conserva (lo que difiere de la
    // base o del anidado queda como diferencia) y la posicion de la raiz y de
    // los huesos sale del documento resuelto.
    const bool structured = detail::hasPrefabStructure(previous);
    json previous_resolved;
    if (structured && !detail::resolvePrefabJson(previous, file, previous_resolved, error)) return {};
    const json& previous_flat = structured ? previous_resolved : previous;
    Uuid self_uuid = Uuid::parse(previous.value("uuid", std::string{}));
    if (!self_uuid.valid()) self_uuid = root.get<PrefabInstance>().prefab.uuid;
    std::vector<detail::NewNestedPart> added;
    if (detail::saveNestedEnabled() && !linkInnerInstances(world, root, self_uuid, added, error)) return {};
    flattenNested(world, root);
    // Lo anadido en la instancia pasa a ser parte del prefab.
    const std::vector<Entity> entities = subtree(world, root);
    for (Entity e : entities) {
        if (!e.has<PrefabLink>() || e.get<PrefabLink>().source.empty()) e.add<PrefabLink>().source = Uuid::generate().toString();
    }
    // La posicion de la raiz es de cada instancia: en el prefab se queda la de
    // antes (o en el origen si es nuevo).
    json root_transform;
    for (const json& record : entitiesOf(previous_flat)) {
        const auto parent = record.find("parent");
        if (parent != record.end() && parent->is_string()) continue;
        if (record.contains("components") && record["components"].contains("Transform")) root_transform = record["components"]["Transform"];
        break;
    }
    // Los que siguen a un hueso guardan la Transform que ya tenian (la de
    // reposo al crearlos), no la pose animada del momento.
    std::unordered_map<std::string, json> bone_transforms;
    for (const json& record : entitiesOf(previous_flat)) {
        if (!record.is_object() || !record.contains("components") || !record["components"].is_object()) continue;
        const json& components = record["components"];
        if (components.contains("BoneSocket") && components.contains("Transform")) {
            bone_transforms[record.value("uuid", std::string{})] = components["Transform"];
        }
    }
    // Las referencias entre entidades de la instancia se guardan con los ids
    // del prefab: al instanciarlo (o sincronizar) apuntan a las nuevas copias.
    std::unordered_map<std::string, std::string> to_source;
    for (Entity e : entities) to_source[e.uuid().toString()] = e.get<PrefabLink>().source;
    json records = json::array();
    for (Entity e : entities) {
        json record = entityRecord(world, e, e == root);
        remapRecordRefs(world, e, record, to_source);
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
        } else if (e != root && components.contains("Transform") && followsBone(e)) {
            if (const auto it = bone_transforms.find(record["uuid"].get<std::string>()); it != bone_transforms.end()) {
                components["Transform"] = it->second;
            }
        }
        records.push_back(std::move(record));
    }
    PrefabInstance& instance = root.get<PrefabInstance>();
    Uuid uuid = Uuid::parse(previous.value("uuid", std::string{}));
    if (!uuid.valid()) uuid = instance.prefab.uuid.valid() ? instance.prefab.uuid : Uuid::generate();
    // Las claves que no son de este archivo (el bloque "blueprint" de un
    // Blueprint de actor, o lo que anada otra version) se conservan tal cual.
    // Un prefab normal no tiene ninguna: sale igual que antes.
    json out = json::object();
    for (auto it = previous.begin(); it != previous.end(); ++it) {
        const std::string& key = it.key();
        if (key == "format" || key == "version" || key == "uuid" || key == "name" || key == "revision" ||
            key == "entities" || key == kPrefabVariantKey || key == "patches") {
            continue;
        }
        out[key] = it.value();
    }
    out["format"] = "CramionPrefab";
    out["version"] = 1;
    out["uuid"] = uuid.toString();
    // Una variante conserva su nombre (el de la raiz es el de la base).
    out["name"] = previous.contains(kPrefabVariantKey) && previous.contains("name") ? previous["name"] : json(root.name());
    out["revision"] = previous.value("revision", 0) + 1;
    const bool keeps_structure = structured || !added.empty();
    if (keeps_structure) {
        if (!detail::unresolvePrefab(previous, file, uuid, records, added, out, error)) return {};
    } else {
        out["entities"] = std::move(records);
    }
    const std::string text = out.dump(2);
    if (!writeText(file, text, error)) return {};
    instance.prefab = assets::AssetRef{uuid, assets::AssetType::Prefab};
    instance.overrides.clear();
    if (!keeps_structure) {
        instance.revision = out["revision"].get<int>();
        return text;
    }
    // Las instancias usan el documento resuelto (y su revision mezclada).
    std::string resolved = resolvePrefabText(text, file, error);
    instance.revision = resolved.empty() ? out["revision"].get<int>() : prefabRevision(resolved);
    return resolved;
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

// --- Anidados y variantes: instancias y cambios propios sueltos -------------------

namespace {

struct OverrideKey {
    std::string source;
    std::string what;       // "Comp|campo", "#clave", "+Comp", "-Comp" o "-"
    std::string component;  // con "Comp|campo", "+Comp" y "-Comp"
    std::string field;      // con "Comp|campo" y "#clave"
    bool info = false;
};

std::optional<OverrideKey> parseOverrideKey(const std::string& key) {
    const std::size_t bar = key.find('|');
    if (bar == std::string::npos || bar == 0 || bar + 1 >= key.size()) return std::nullopt;
    OverrideKey k;
    k.source = key.substr(0, bar);
    k.what = key.substr(bar + 1);
    if (k.what == "-") return k;
    if (k.what[0] == '#') {
        k.info = true;
        k.field = k.what.substr(1);
    } else if (k.what[0] == '+' || k.what[0] == '-') {
        k.component = k.what.substr(1);
    } else if (const std::size_t f = k.what.find('|'); f != std::string::npos) {
        k.component = k.what.substr(0, f);
        k.field = k.what.substr(f + 1);
    } else {
        return std::nullopt;
    }
    return k;
}

json infoDefault(const std::string& key) {
    if (key == "active") return true;
    if (key == "tag" || key == "name") return std::string{};
    if (key == "layer") return 0;
    return false;  // static, static_batched
}

// El valor de `k` en el registro de una entidad (con los ids del prefab).
bool valueOf(const json& record, const OverrideKey& k, json& value) {
    if (k.info) {
        value = record.contains(k.field) ? record[k.field] : infoDefault(k.field);
        return true;
    }
    if (k.what == "-" || k.what[0] == '-') return true;  // no lleva valor
    const auto comps = record.find("components");
    if (comps == record.end() || !comps->contains(k.component)) return false;
    const json& comp = (*comps)[k.component];
    if (k.what[0] == '+') {
        value = comp;
        return true;
    }
    if (!comp.contains(k.field)) return false;
    value = comp[k.field];
    return true;
}

// Pone en los registros (documento resuelto) el cambio `k` con `value`.
bool writeKey(json& records, const std::string& target, const OverrideKey& k, const json& value) {
    for (json& r : records) {
        if (!r.is_object() || r.value("uuid", std::string{}) != target) continue;
        if (k.info) {
            r[k.field] = value;
            return true;
        }
        if (k.what == "-") return false;  // borrar entidades: con Aplicar (toda la instancia)
        if (!r.contains("components") || !r["components"].is_object()) r["components"] = json::object();
        json& comps = r["components"];
        if (k.what[0] == '+') {
            comps[k.component] = value;
        } else if (k.what[0] == '-') {
            comps.erase(k.component);
        } else {
            if (!comps.contains(k.component) || !comps[k.component].is_object()) return false;
            comps[k.component][k.field] = value;
        }
        return true;
    }
    return false;
}

// Registro de `e` con las referencias pasadas a los ids del prefab.
json linkedRecord(World& world, Entity root, Entity e) {
    const auto links = linkedEntities(world, root);
    json record = entityRecord(world, e, e == root);
    remapRecordRefs(world, e, record, worldToSource(links));
    return record;
}

// Pone en la entidad `e` (de la instancia `root`) el valor `value` de `k`
// (con ids del prefab, que pasan a los de la instancia).
bool setOnEntity(World& world, Entity root, Entity e, const OverrideKey& k, const json& value, const json& full_record) {
    if (k.info) {
        EntityInfo& info = e.get<EntityInfo>();
        if (k.field == "name") e.setName(value.is_string() ? value.get<std::string>() : e.name());
        else if (k.field == "active") info.active = value.is_boolean() ? value.get<bool>() : info.active;
        else if (k.field == "tag") info.tag = value.is_string() ? value.get<std::string>() : info.tag;
        else if (k.field == "layer") info.layer = value.is_number_integer() ? value.get<int>() : info.layer;
        else if (k.field == "static") info.is_static = value.is_boolean() && value.get<bool>();
        else return false;
        return true;
    }
    if (k.what == "-") return false;
    const ComponentType* type = ComponentRegistry::instance().find(k.component);
    if (type == nullptr) return false;
    if (k.what[0] == '+') {
        if (type->has(world, e.handle()) && type->removable) type->remove(world, e.handle());
        return true;
    }
    json data;
    if (k.what[0] == '-') {
        const auto comps = full_record.find("components");
        if (comps == full_record.end() || !comps->contains(k.component)) return false;
        data = (*comps)[k.component];
    } else {
        data = json::object();
        data[k.field] = value;
    }
    if (!type->has(world, e.handle())) type->add(world, e.handle());
    JsonReader reader(data);
    type->reflect(world, e.handle(), reader);
    // Referencias con ids del prefab -> entidades de la instancia.
    std::unordered_map<Uuid, Uuid> refs;
    for (const auto& [source, linked] : linkedEntities(world, root)) {
        const Uuid id = Uuid::parse(source);
        if (id.valid()) refs[id] = linked.uuid();
    }
    world.remapEntityRefs(e, refs);
    return true;
}

}  // namespace

std::string createVariantFromInstance(World& world, Entity root, const std::filesystem::path& base_file,
                                      const std::filesystem::path& variant_file, std::string* error) {
    if (!root.valid() || !root.has<PrefabInstance>()) {
        if (error) *error = "no es una instancia de prefab";
        return {};
    }
    if (!createPrefabVariant(base_file, variant_file, error)) return {};
    const Uuid variant = prefabUuid(readPrefabFileRaw(variant_file));
    root.get<PrefabInstance>().prefab = assets::AssetRef{variant, assets::AssetType::Prefab};
    // Aplicar contra la variante (vacia): sus cambios propios quedan como
    // diferencias de la variante respecto a la base.
    return applyInstance(world, root, variant_file, error);
}

bool revertOverride(World& world, Entity root, const std::string& key, const std::string& prefab_text) {
    if (!root.valid() || !root.has<PrefabInstance>()) return false;
    std::vector<std::string>& overrides = root.get<PrefabInstance>().overrides;
    const auto it = std::find(overrides.begin(), overrides.end(), key);
    if (it == overrides.end()) return false;
    overrides.erase(it);
    // Sincronizar: lo que ya no es propio toma el valor del prefab.
    syncInstance(world, root, prefab_text);
    return true;
}

std::string applyOverride(World& world, Entity root, const std::string& key, const std::filesystem::path& file,
                          bool to_base, std::string* error) {
    if (!root.valid() || !root.has<PrefabInstance>()) {
        if (error) *error = "no es una instancia de prefab";
        return {};
    }
    const std::optional<OverrideKey> k = parseOverrideKey(key);
    if (!k || k->what == "-") {
        if (error) *error = "ese cambio se aplica con Aplicar (toda la instancia)";
        return {};
    }
    const auto links = linkedEntities(world, root);
    const auto at = links.find(k->source);
    if (at == links.end()) {
        if (error) *error = "la entidad ya no esta en la instancia";
        return {};
    }
    const Entity e = at->second;
    if (k->info && k->field == "name" && e == root) {
        if (error) *error = "el nombre de la raiz es de cada instancia";
        return {};
    }
    const json record = linkedRecord(world, root, e);
    json value;
    if (!valueOf(record, *k, value)) {
        if (error) *error = "la entidad no tiene ese campo";
        return {};
    }
    std::string resolved;
    if (!to_base) {
        if (!detail::editPrefabResolved(file, [&](json& records) { return writeKey(records, k->source, *k, value); }, error,
                                        &resolved)) {
            return {};
        }
    } else {
        json base_record;
        PrefabOrigin where;
        if (!detail::originRecord(file, k->source, base_record, &where, error)) return {};
        detail::toOriginSpace(file, k->source, value);
        if (!detail::editPrefabResolved(where.file, [&](json& records) { return writeKey(records, where.source, *k, value); },
                                        error, nullptr)) {
            return {};
        }
        // La diferencia que tuviera este prefab en ese campo ya sobra: se hereda.
        if (!detail::removeOwnPatches(file, k->source, k->what, error)) return {};
        resolved = readPrefabFile(file);
    }
    PrefabInstance& instance = root.get<PrefabInstance>();
    instance.overrides.erase(std::remove(instance.overrides.begin(), instance.overrides.end(), key), instance.overrides.end());
    if (!resolved.empty()) instance.revision = prefabRevision(resolved);
    return resolved;
}

bool revertInheritedOverride(World& world, Entity root, const std::string& key, const std::filesystem::path& file) {
    const std::optional<OverrideKey> k = parseOverrideKey(key);
    if (!root.valid() || !k) return false;
    const auto links = linkedEntities(world, root);
    const auto at = links.find(k->source);
    if (at == links.end()) return false;
    json base_record;
    if (!detail::originRecord(file, k->source, base_record, nullptr, nullptr)) return false;
    json value;
    if (!valueOf(base_record, *k, value)) return false;
    return setOnEntity(world, root, at->second, *k, value, base_record);
}

bool applyInheritedOverride(World& world, Entity root, const std::string& key, const std::filesystem::path& file,
                            std::string* error) {
    const std::optional<OverrideKey> k = parseOverrideKey(key);
    if (!root.valid() || !k || k->what == "-") {
        if (error) *error = "ese cambio no se puede aplicar suelto";
        return false;
    }
    const auto links = linkedEntities(world, root);
    const auto at = links.find(k->source);
    if (at == links.end()) {
        if (error) *error = "la entidad ya no esta";
        return false;
    }
    json value;
    if (!valueOf(linkedRecord(world, root, at->second), *k, value)) {
        if (error) *error = "la entidad no tiene ese campo";
        return false;
    }
    json base_record;
    PrefabOrigin where;
    if (!detail::originRecord(file, k->source, base_record, &where, error)) return false;
    detail::toOriginSpace(file, k->source, value);
    return detail::editPrefabResolved(where.file, [&](json& records) { return writeKey(records, where.source, *k, value); },
                                      error, nullptr);
}

}  // namespace cramion::ecs
