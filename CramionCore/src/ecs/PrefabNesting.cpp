// Prefabs anidados y variantes (ver "Anidados y variantes" en Prefab.h).
//
// Todo pasa por el documento resuelto: un .crprefab con enlaces a otros
// prefabs se convierte, al leerlo, en el documento plano de siempre (las
// entidades de la base o del anidado con sus diferencias aplicadas), y al
// guardarlo se hace lo inverso: lo que difiere de la base o del anidado queda
// como diferencia ("patches") y lo demas se sigue heredando. Asi instanciar
// por JSON, por plantilla compilada, en tandas o desde el pool da lo mismo, y
// las instancias de las escenas (PrefabLink con los ids del documento
// resuelto) no notan si un prefab es plano, anidado o variante.
//
// Ids: las entidades de una variante tienen los mismos ids que en su base.
// Las de un anidado, uno derivado del id de su raiz en el prefab que lo
// contiene y del suyo en el hijo (estable: no cambia al guardar ni al cambiar
// el hijo); la raiz del anidado tiene el id de su registro.

#include "PrefabNesting.h"

#include "CramionCore/asset/AssetDatabase.h"
#include "CramionCore/cvar/CVar.h"
#include "CramionCore/ecs/Prefab.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace cramion::ecs {

using nlohmann::json;

namespace {

cvar::CVar<bool> g_save_nested("prefab.SaveNested", false,
                               "Al crear o aplicar un prefab, las instancias de otros prefabs que tenga dentro se "
                               "guardan como enlaces (prefabs anidados) en vez de como copias sueltas",
                               cvar::Saved);

std::atomic<std::uint64_t> g_prefab_writes{0};

// --- Rutas y lectura ---------------------------------------------------------------

std::filesystem::path fromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string readText(const std::filesystem::path& file) {
    if (file.empty()) return {};
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

json parseObject(const std::string& text) {
    json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return json::object();
    return j;
}

const json& entitiesOf(const json& prefab) {
    static const json kEmpty = json::array();
    const auto it = prefab.find("entities");
    return it != prefab.end() && it->is_array() ? *it : kEmpty;
}

const json& patchesOf(const json& holder) {
    static const json kEmpty = json::array();
    const auto it = holder.find("patches");
    return it != holder.end() && it->is_array() ? *it : kEmpty;
}

bool isNestedRecord(const json& record) {
    if (!record.is_object()) return false;
    const auto it = record.find(kPrefabNestedKey);
    return it != record.end() && it->is_object();
}

bool isVariant(const json& raw) {
    const auto it = raw.find(kPrefabVariantKey);
    return it != raw.end() && it->is_object();
}

std::string idOf(const json& record) {
    const auto it = record.find("uuid");
    return it != record.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::string parentOf(const json& record) {
    const auto it = record.find("parent");
    return it != record.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

bool writeFileAtomic(const std::filesystem::path& file, const std::string& text, std::string* error) {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path temporary = std::filesystem::path(file).concat(".tmp");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "no se pudo escribir " + toUtf8(file);
            return false;
        }
        out << text;
        if (!out) {
            if (error) *error = "error escribiendo " + toUtf8(file);
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

// --- Donde esta cada prefab ------------------------------------------------------------

using LocationMap = std::unordered_map<Uuid, std::filesystem::path>;
std::mutex g_locations_mutex;
std::shared_ptr<const LocationMap> g_locations;

// --- Ids y revisiones --------------------------------------------------------------------

std::uint64_t fnv(std::string_view text, std::uint64_t hash) {
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001B3ull;
    }
    return hash;
}

std::uint64_t mix64(std::uint64_t x) {
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x;
}

// La revision del documento resuelto: la propia si no usa nada (los prefabs
// de siempre no cambian); si no, una mezcla con la de cada cosa que usa, para
// que cambiar la base o un anidado deje atras a las instancias.
int mixRevision(int own, const std::vector<int>& dependencies) {
    if (dependencies.empty()) return own;
    std::uint64_t h = mix64(0x9E3779B97F4A7C15ull ^ static_cast<std::uint32_t>(own));
    for (const int d : dependencies) h = mix64(h ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(d)) + 0x632BE59BD9B4E019ull));
    return static_cast<int>(h % 0x3FFFFFFFull) + 1;
}

// --- Carga de un enlace --------------------------------------------------------------------

struct Loaded {
    json raw;
    std::filesystem::path file;
    Uuid uuid;
};

// {"uuid", "path"}: primero la ruta relativa a `from` (si es ese prefab),
// luego la tabla de ubicaciones por UUID.
bool loadLink(const json& link, const std::filesystem::path& from, Loaded& out) {
    const Uuid wanted = Uuid::parse(link.value("uuid", std::string{}));
    const std::string relative = link.value("path", std::string{});
    const auto attempt = [&](const std::filesystem::path& candidate) {
        if (candidate.empty()) return false;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(candidate, ec)) return false;
        json raw = parseObject(readText(candidate));
        if (raw.empty()) return false;
        const Uuid found = Uuid::parse(raw.value("uuid", std::string{}));
        if (wanted.valid() && found != wanted) return false;
        out.raw = std::move(raw);
        out.file = candidate;
        out.uuid = found;
        return true;
    };
    if (!relative.empty() && !from.empty() && attempt((from.parent_path() / fromUtf8(relative)).lexically_normal())) return true;
    if (wanted.valid() && attempt(locatePrefab(wanted))) return true;
    return false;
}

std::string linkName(const json& link) {
    std::string name = link.value("name", std::string{});
    if (name.empty()) name = toUtf8(fromUtf8(link.value("path", std::string{})).stem());
    return name.empty() ? link.value("uuid", std::string{}) : name;
}

// --- Resolver --------------------------------------------------------------------------------

struct Origin {
    std::string nested;  // raiz del anidado en este prefab ("" = base de la variante)
    std::string source;  // id en el prefab de origen
    Uuid prefab;
    std::filesystem::path file;
};

struct Resolution {
    json doc;
    std::unordered_map<std::string, Origin> origins;  // primer nivel (base o anidado directo)
    std::vector<Uuid> dependencies;                   // todas, sin repetir
};

struct Context {
    std::vector<Uuid> stack;  // prefabs que se estan resolviendo (ciclos)
    std::vector<std::string> names;
    std::string error;
};

void addDependency(Resolution& out, const Uuid& id, const std::vector<Uuid>& more) {
    const auto add = [&](const Uuid& u) {
        if (u.valid() && std::find(out.dependencies.begin(), out.dependencies.end(), u) == out.dependencies.end()) {
            out.dependencies.push_back(u);
        }
    };
    add(id);
    for (const Uuid& u : more) add(u);
}

// Cambia en todo el JSON los textos que son ids de `map` (referencias entre
// entidades, "uuid" y "parent"). Los UUID son aleatorios de 128 bits: un texto
// igual a uno es esa entidad.
void remapStrings(json& j, const std::unordered_map<std::string, std::string>& map) {
    if (j.is_string()) {
        const auto it = map.find(j.get_ref<const std::string&>());
        if (it != map.end()) j = it->second;
    } else if (j.is_array() || j.is_object()) {
        for (json& item : j) remapStrings(item, map);
    }
}

json defaultInfo(const std::string& key) {
    if (key == "active") return true;
    if (key == "tag" || key == "name") return std::string{};
    if (key == "layer") return 0;
    if (key == "static" || key == "static_batched") return false;
    return nullptr;  // parent
}

constexpr const char* kInfoKeys[] = {"name", "active", "tag", "layer", "static", "static_batched", "parent"};

bool technicalComponent(const std::string& name) { return name == "PrefabInstance" || name == "PrefabLink"; }

// Aplica diferencias a `records` (in situ). `map`: id del origen -> id aqui
// (nullptr: los mismos). La raiz de la parte (`part_root`) no se borra.
void applyPatches(std::vector<json>& records, const json& patches, const std::unordered_map<std::string, std::string>* map,
                  const std::string& part_root) {
    if (!patches.is_array() || patches.empty()) return;
    std::unordered_map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < records.size(); ++i) index[idOf(records[i])] = i;
    std::unordered_set<std::string> removed;
    for (const json& patch : patches) {
        if (!patch.is_object()) continue;
        std::string target = patch.value("target", std::string{});
        if (map != nullptr) {
            const auto m = map->find(target);
            if (m == map->end()) continue;
            target = m->second;
        }
        const auto at = index.find(target);
        if (at == index.end()) continue;  // ya no esta en la base: se ignora
        json& record = records[at->second];
        const std::string op = patch.value("op", std::string{});
        const std::string component = patch.value("component", std::string{});
        const auto value = patch.find("value");
        if (op == "set") {
            if (value == patch.end()) continue;
            auto comps = record.find("components");
            if (comps == record.end() || !comps->is_object()) continue;
            auto comp = comps->find(component);
            if (comp == comps->end() || !comp->is_object()) continue;  // la base lo quito
            (*comp)[patch.value("field", std::string{})] = *value;
        } else if (op == "info") {
            if (value == patch.end()) continue;
            const std::string field = patch.value("field", std::string{});
            if (field == "parent" && target == part_root) continue;
            record[field] = *value;
        } else if (op == "addComponent") {
            if (value == patch.end() || !value->is_object()) continue;
            if (!record.contains("components") || !record["components"].is_object()) record["components"] = json::object();
            record["components"][component] = *value;
        } else if (op == "removeComponent") {
            auto comps = record.find("components");
            if (comps != record.end() && comps->is_object()) comps->erase(component);
        } else if (op == "removeEntity") {
            if (target != part_root) removed.insert(target);
        }
    }
    if (removed.empty()) return;
    // Con sus descendientes.
    for (bool grew = true; grew;) {
        grew = false;
        for (const json& record : records) {
            const std::string parent = parentOf(record);
            if (!parent.empty() && removed.count(parent) != 0 && removed.insert(idOf(record)).second) grew = true;
        }
    }
    records.erase(std::remove_if(records.begin(), records.end(), [&](const json& r) { return removed.count(idOf(r)) != 0; }),
                  records.end());
}

// Padres antes que hijos (buildEntities los necesita asi), conservando el
// orden. Los que cuelgan de una entidad que ya no esta se quitan.
void orderParentsFirst(std::vector<json>& records) {
    std::unordered_set<std::string> ids;
    for (const json& r : records) ids.insert(idOf(r));
    std::unordered_set<std::string> emitted;
    std::vector<json> out;
    out.reserve(records.size());
    std::vector<bool> done(records.size(), false);
    for (bool progress = true; progress && out.size() < records.size();) {
        progress = false;
        for (std::size_t i = 0; i < records.size(); ++i) {
            if (done[i]) continue;
            const std::string parent = parentOf(records[i]);
            if (!parent.empty() && emitted.count(parent) == 0) continue;
            done[i] = true;
            progress = true;
            emitted.insert(idOf(records[i]));
            out.push_back(std::move(records[i]));
        }
    }
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (!done[i]) {
            std::cerr << "[Prefab] \"" << records[i].value("name", std::string{}) << "\" cuelga de una entidad que ya no esta: se quita\n";
        }
    }
    records = std::move(out);
}

bool resolveImpl(const json& raw, const std::filesystem::path& file, Context& ctx, Resolution& out);

// Las entidades de un hijo ya resuelto con los ids de este prefab: la raiz
// pasa a `nested` y cuelga de `parent`. `forward`: id del hijo -> id aqui.
std::vector<json> expandChild(const json& child_doc, const std::string& nested, const json& parent,
                              std::unordered_map<std::string, std::string>& forward) {
    std::vector<json> records;
    const json& entities = entitiesOf(child_doc);
    bool root_seen = false;
    for (const json& r : entities) {
        if (!r.is_object()) continue;
        const std::string id = idOf(r);
        if (id.empty()) continue;
        forward[id] = !root_seen ? nested : detail::nestedEntityId(nested, id);
        root_seen = true;
        records.push_back(r);
    }
    for (std::size_t i = 0; i < records.size(); ++i) {
        remapStrings(records[i], forward);
        if (i == 0) records[i]["parent"] = parent.is_string() ? parent : json(nullptr);
    }
    return records;
}

// Las entidades propias de `list` (registros normales y anidados expandidos).
bool expandList(const json& list, const std::filesystem::path& file, Context& ctx, std::vector<json>& records,
                Resolution& out, std::vector<int>& revisions) {
    for (const json& record : list) {
        if (!record.is_object()) continue;
        if (!isNestedRecord(record)) {
            records.push_back(record);
            continue;
        }
        const std::string nested = idOf(record);
        if (nested.empty()) continue;
        const json& link = record[kPrefabNestedKey];
        const json parent = record.contains("parent") ? record["parent"] : json(nullptr);
        Loaded child;
        if (!loadLink(link, file, child)) {
            // Perdido: un objeto vacio en su sitio. Al guardar, el enlace se
            // conserva tal cual (por si vuelve).
            std::cerr << "[Prefab] No se encuentra el prefab anidado \"" << linkName(link) << "\"\n";
            json placeholder = {{"uuid", nested}, {"name", "(prefab perdido) " + linkName(link)}, {"parent", parent},
                                {"active", true},  {"tag", ""},                                    {"layer", 0},
                                {"components", json::object()}};
            records.push_back(std::move(placeholder));
            out.origins[nested] = Origin{nested, {}, Uuid::parse(link.value("uuid", std::string{})), {}};
            continue;
        }
        Resolution sub;
        if (!resolveImpl(child.raw, child.file, ctx, sub)) return false;
        addDependency(out, child.uuid, sub.dependencies);
        revisions.push_back(sub.doc.value("revision", 0));
        std::unordered_map<std::string, std::string> forward;
        std::vector<json> expanded = expandChild(sub.doc, nested, parent, forward);
        for (const auto& [source, id] : forward) out.origins[id] = Origin{nested, source, child.uuid, child.file};
        applyPatches(expanded, patchesOf(record), &forward, nested);
        for (json& r : expanded) records.push_back(std::move(r));
    }
    return true;
}

bool resolveImpl(const json& raw, const std::filesystem::path& file, Context& ctx, Resolution& out) {
    const Uuid self = Uuid::parse(raw.value("uuid", std::string{}));
    const std::string name = raw.value("name", toUtf8(file.stem()));
    if (self.valid() && std::find(ctx.stack.begin(), ctx.stack.end(), self) != ctx.stack.end()) {
        std::string chain;
        for (const std::string& n : ctx.names) chain += n + " -> ";
        ctx.error = "ciclo de prefabs: " + chain + name;
        return false;
    }
    if (ctx.stack.size() > 64) {
        ctx.error = "demasiados niveles de prefabs anidados";
        return false;
    }
    ctx.stack.push_back(self);
    ctx.names.push_back(name);
    struct Pop {
        Context& c;
        ~Pop() {
            c.stack.pop_back();
            c.names.pop_back();
        }
    } pop{ctx};

    std::vector<json> records;
    std::vector<int> revisions;
    json top = json::object();
    if (isVariant(raw)) {
        const json& link = raw[kPrefabVariantKey];
        Loaded base;
        if (!loadLink(link, file, base)) {
            ctx.error = "no se encuentra la base \"" + linkName(link) + "\" de la variante \"" + name + "\"";
            return false;
        }
        Resolution sub;
        if (!resolveImpl(base.raw, base.file, ctx, sub)) return false;
        addDependency(out, base.uuid, sub.dependencies);
        revisions.push_back(sub.doc.value("revision", 0));
        for (const json& r : entitiesOf(sub.doc)) {
            if (!r.is_object()) continue;
            const std::string id = idOf(r);
            out.origins[id] = Origin{{}, id, base.uuid, base.file};
            records.push_back(r);
        }
        const std::string base_root = records.empty() ? std::string{} : idOf(records.front());
        applyPatches(records, patchesOf(raw), nullptr, base_root);
        // Lo que la base tiene arriba (el bloque "blueprint"...) se hereda si
        // la variante no lo cambia.
        for (auto it = sub.doc.begin(); it != sub.doc.end(); ++it) {
            const std::string& key = it.key();
            if (key == "entities" || key == "format" || key == "version" || key == "uuid" || key == "name" || key == "revision") continue;
            top[key] = it.value();
        }
    }
    if (!expandList(entitiesOf(raw), file, ctx, records, out, revisions)) return false;
    orderParentsFirst(records);
    for (auto it = raw.begin(); it != raw.end(); ++it) {
        const std::string& key = it.key();
        if (key == "entities" || key == kPrefabVariantKey || key == "patches") continue;
        top[key] = it.value();
    }
    top["revision"] = mixRevision(raw.value("revision", 0), revisions);
    top["entities"] = json::array();
    json& entities = top["entities"];
    for (json& r : records) entities.push_back(std::move(r));
    out.doc = std::move(top);
    return true;
}

// Un .crprefab de los de siempre (sin anidados ni variante): no hay nada que
// resolver. Solo se buscan las claves, sin parsear (el editor pregunta por
// origenes y diferencias de prefabs que pueden ser muy grandes).
bool plainPrefabFile(const std::filesystem::path& file) { return !prefabNeedsResolve(readText(file)); }

bool resolveFile(const std::filesystem::path& file, json* raw_out, Resolution& out, std::string* error) {
    json raw = parseObject(readText(file));
    if (raw.empty()) {
        if (error) *error = "no se pudo leer " + toUtf8(file);
        return false;
    }
    Context ctx;
    if (!resolveImpl(raw, file, ctx, out)) {
        if (error) *error = ctx.error;
        return false;
    }
    if (raw_out != nullptr) *raw_out = std::move(raw);
    return true;
}

// --- Diferencias -------------------------------------------------------------------------------

// Lo que cambia `records` (los registros nuevos, por id en `by_id`) respecto
// a `baseline` (la base o el anidado, ya con los ids de aqui). `to_target`:
// id de aqui -> id del origen (nullptr: los mismos). Las entidades de la
// base que ya estan se apuntan en `consumed`.
void diffPart(const std::vector<json>& baseline, const json& records, const std::unordered_map<std::string, std::size_t>& by_id,
              const std::unordered_map<std::string, std::string>* to_target, const std::string& part_root, json& patches,
              std::unordered_set<std::string>& consumed) {
    std::unordered_set<std::string> base_ids;
    for (const json& b : baseline) base_ids.insert(idOf(b));
    const auto target_of = [&](const std::string& id) {
        if (to_target == nullptr) return id;
        const auto it = to_target->find(id);
        return it != to_target->end() ? it->second : id;
    };
    for (const json& b : baseline) {
        const std::string id = idOf(b);
        const std::string target = target_of(id);
        const auto it = by_id.find(id);
        if (it == by_id.end()) {
            if (id == part_root) continue;
            const std::string parent = parentOf(b);
            // Solo el de mas arriba (sus hijos se van con el).
            if (!parent.empty() && base_ids.count(parent) != 0 && by_id.count(parent) == 0) continue;
            patches.push_back({{"op", "removeEntity"}, {"target", target}});
            continue;
        }
        consumed.insert(id);
        const json& now = records[it->second];
        for (const char* key : kInfoKeys) {
            const std::string k = key;
            if (k == "parent" && id == part_root) continue;
            const json before = b.contains(k) ? b[k] : defaultInfo(k);
            const json after = now.contains(k) ? now[k] : defaultInfo(k);
            if (before != after) patches.push_back({{"op", "info"}, {"target", target}, {"field", k}, {"value", after}});
        }
        static const json kNone = json::object();
        const json& theirs = b.contains("components") && b["components"].is_object() ? b["components"] : kNone;
        const json& ours = now.contains("components") && now["components"].is_object() ? now["components"] : kNone;
        for (auto c = ours.begin(); c != ours.end(); ++c) {
            if (technicalComponent(c.key())) continue;
            const auto base_comp = theirs.find(c.key());
            if (base_comp == theirs.end() || !base_comp->is_object()) {
                patches.push_back({{"op", "addComponent"}, {"target", target}, {"component", c.key()}, {"value", c.value()}});
                continue;
            }
            if (!c.value().is_object()) continue;
            for (auto f = c.value().begin(); f != c.value().end(); ++f) {
                const auto base_value = base_comp->find(f.key());
                if (base_value == base_comp->end() || *base_value != f.value()) {
                    patches.push_back({{"op", "set"}, {"target", target}, {"component", c.key()}, {"field", f.key()}, {"value", f.value()}});
                }
            }
        }
        for (auto c = theirs.begin(); c != theirs.end(); ++c) {
            if (!technicalComponent(c.key()) && !ours.contains(c.key())) {
                patches.push_back({{"op", "removeComponent"}, {"target", target}, {"component", c.key()}});
            }
        }
    }
}

// El documento resuelto (plano) de `file`, o el texto tal cual si no tiene estructura.
bool resolvedDocument(const json& raw, const std::filesystem::path& file, json& out, std::string* error) {
    if (!detail::hasPrefabStructure(raw)) {
        out = raw;
        return true;
    }
    return detail::resolvePrefabJson(raw, file, out, error);
}

// El prefab de `origin` en su espacio, para escribir en el.
struct PartMaps {
    std::unordered_map<std::string, std::string> to_origin;    // id aqui -> id en el origen
    std::unordered_map<std::string, std::string> from_origin;  // id en el origen -> id aqui
};

PartMaps partMaps(const Resolution& r, const std::string& nested, bool variant_base) {
    PartMaps maps;
    for (const auto& [id, origin] : r.origins) {
        if (origin.source.empty()) continue;
        if (variant_base ? !origin.nested.empty() : origin.nested != nested) continue;
        maps.to_origin[id] = origin.source;
        maps.from_origin[origin.source] = id;
    }
    return maps;
}

}  // namespace

// --- detail ---------------------------------------------------------------------------------------

namespace detail {

bool saveNestedEnabled() { return g_save_nested; }

void notePrefabWrite() { g_prefab_writes.fetch_add(1, std::memory_order_relaxed); }

bool hasPrefabStructure(const json& raw) {
    if (!raw.is_object()) return false;
    if (isVariant(raw)) return true;
    for (const json& record : entitiesOf(raw)) {
        if (isNestedRecord(record)) return true;
    }
    return false;
}

bool resolvePrefabJson(const json& raw, const std::filesystem::path& file, json& out, std::string* error) {
    Resolution r;
    Context ctx;
    if (!resolveImpl(raw, file, ctx, r)) {
        if (error) *error = ctx.error;
        return false;
    }
    out = std::move(r.doc);
    return true;
}

std::string nestedEntityId(const std::string& nested, const std::string& child) {
    const std::uint64_t a = fnv(child, fnv("/", fnv(nested, 0xCBF29CE484222325ull)));
    Uuid id{mix64(a), mix64(a ^ 0x9E3779B97F4A7C15ull)};
    id.high = (id.high & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
    id.low = (id.low & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;
    return id.toString();
}

std::string prefabLinkPath(const std::filesystem::path& target, const std::filesystem::path& from) {
    std::error_code ec;
    const std::filesystem::path a = std::filesystem::absolute(target, ec).lexically_normal();
    const std::filesystem::path b = std::filesystem::absolute(from, ec).lexically_normal().parent_path();
    std::filesystem::path relative = a.lexically_relative(b);
    if (relative.empty()) relative = target.filename();
    return toUtf8(relative);
}

bool prefabFileDependsOn(const std::filesystem::path& file, const Uuid& target) {
    json raw = parseObject(readText(file));
    if (raw.empty()) return false;
    if (Uuid::parse(raw.value("uuid", std::string{})) == target) return true;
    Resolution r;
    Context ctx;
    ctx.stack.push_back(target);  // si lo vuelve a encontrar, ciclo
    ctx.names.push_back("(este prefab)");
    if (!resolveImpl(raw, file, ctx, r)) return ctx.error.rfind("ciclo", 0) == 0;
    return std::find(r.dependencies.begin(), r.dependencies.end(), target) != r.dependencies.end();
}

bool unresolvePrefab(const json& previous_raw, const std::filesystem::path& file, const Uuid& self, const json& records,
                     const std::vector<NewNestedPart>& added, json& out, std::string* error) {
    std::unordered_map<std::string, std::size_t> by_id;
    for (std::size_t i = 0; i < records.size(); ++i) by_id[idOf(records[i])] = i;
    std::unordered_set<std::string> consumed;
    Context ctx;
    ctx.stack.push_back(self);
    ctx.names.push_back(previous_raw.value("name", toUtf8(file.stem())));

    // 1. La base de la variante.
    if (isVariant(previous_raw)) {
        const json& link = previous_raw[kPrefabVariantKey];
        Loaded base;
        if (!loadLink(link, file, base)) {
            if (error) *error = "no se encuentra la base \"" + linkName(link) + "\"";
            return false;
        }
        Resolution sub;
        if (!resolveImpl(base.raw, base.file, ctx, sub)) {
            if (error) *error = ctx.error;
            return false;
        }
        std::vector<json> baseline;
        for (const json& r : entitiesOf(sub.doc)) {
            if (r.is_object()) baseline.push_back(r);
        }
        json patches = json::array();
        const std::string root = baseline.empty() ? std::string{} : idOf(baseline.front());
        diffPart(baseline, records, by_id, nullptr, root, patches, consumed);
        json variant_link = link;
        variant_link["path"] = detail::prefabLinkPath(base.file, file);
        variant_link["name"] = sub.doc.value("name", linkName(link));
        out[kPrefabVariantKey] = std::move(variant_link);
        out["patches"] = std::move(patches);
    }

    // 2. Los anidados (los que ya habia y los nuevos).
    struct Part {
        std::string id;
        json link;
        const json* previous = nullptr;
    };
    std::vector<Part> parts;
    for (const json& r : entitiesOf(previous_raw)) {
        if (isNestedRecord(r) && !idOf(r).empty()) parts.push_back(Part{idOf(r), r[kPrefabNestedKey], &r});
    }
    for (const NewNestedPart& a : added) {
        json link = {{"uuid", a.prefab.toString()}, {"path", detail::prefabLinkPath(a.file, file)}};
        parts.push_back(Part{a.id, std::move(link), nullptr});
    }
    std::unordered_map<std::string, json> part_records;
    for (const Part& part : parts) {
        const auto at = by_id.find(part.id);
        if (at == by_id.end() || consumed.count(part.id) != 0) continue;  // se borro
        const json parent = records[at->second].contains("parent") ? records[at->second]["parent"] : json(nullptr);
        Loaded child;
        if (!loadLink(part.link, file, child)) {
            // Perdido: el enlace se conserva tal cual.
            json keep = part.previous != nullptr ? *part.previous : json{{"uuid", part.id}, {kPrefabNestedKey, part.link}};
            keep["parent"] = parent;
            part_records[part.id] = std::move(keep);
            consumed.insert(part.id);
            continue;
        }
        Resolution sub;
        if (!resolveImpl(child.raw, child.file, ctx, sub)) {
            if (error) *error = ctx.error;
            return false;
        }
        std::unordered_map<std::string, std::string> forward;
        const std::vector<json> baseline = expandChild(sub.doc, part.id, parent, forward);
        std::unordered_map<std::string, std::string> reverse;
        for (const auto& [source, id] : forward) reverse[id] = source;
        json patches = json::array();
        diffPart(baseline, records, by_id, &reverse, part.id, patches, consumed);
        json link = part.link;
        link["uuid"] = child.uuid.toString();
        link["path"] = detail::prefabLinkPath(child.file, file);
        link["name"] = sub.doc.value("name", linkName(part.link));
        json record = {{"uuid", part.id}, {"parent", parent}, {kPrefabNestedKey, std::move(link)}};
        if (!patches.empty()) record["patches"] = std::move(patches);
        part_records[part.id] = std::move(record);
    }

    // 3. Lo propio, en el orden de los registros nuevos.
    json entities = json::array();
    for (const json& r : records) {
        const std::string id = idOf(r);
        if (const auto p = part_records.find(id); p != part_records.end()) {
            entities.push_back(std::move(p->second));
            part_records.erase(p);
        } else if (consumed.count(id) == 0) {
            entities.push_back(r);
        }
    }
    out["entities"] = std::move(entities);
    return true;
}

// Edita el documento resuelto de `file` y lo guarda (revision + 1) con su
// estructura (variante / anidados). `resolved_out`: el texto resuelto nuevo.
bool editPrefabResolved(const std::filesystem::path& file, const std::function<bool(json& records)>& edit,
                        std::string* error, std::string* resolved_out) {
    const json raw = parseObject(readText(file));
    if (raw.empty()) {
        if (error) *error = "no se pudo leer " + toUtf8(file);
        return false;
    }
    json doc;
    if (!resolvedDocument(raw, file, doc, error)) return false;
    json records = entitiesOf(doc);
    if (!edit(records)) {
        if (error && error->empty()) *error = "no hay nada que cambiar";
        return false;
    }
    json out = json::object();
    for (auto it = raw.begin(); it != raw.end(); ++it) {
        const std::string& key = it.key();
        if (key == "entities" || key == "revision" || key == kPrefabVariantKey || key == "patches") continue;
        out[key] = it.value();
    }
    out["revision"] = raw.value("revision", 0) + 1;
    if (hasPrefabStructure(raw)) {
        if (!unresolvePrefab(raw, file, Uuid::parse(raw.value("uuid", std::string{})), records, {}, out, error)) return false;
    } else {
        out["entities"] = std::move(records);
    }
    const std::string text = out.dump(2);
    if (!writeFileAtomic(file, text, error)) return false;
    if (resolved_out != nullptr) *resolved_out = resolvePrefabText(text, file);
    return true;
}

// Las diferencias de `file` para una entidad (id resuelto) y una clave
// ("Comp|campo" o "#clave"): se quitan (revision + 1).
bool removeOwnPatches(const std::filesystem::path& file, const std::string& source, const std::string& what,
                      std::string* error) {
    json raw = parseObject(readText(file));
    if (raw.empty()) return false;
    Resolution r;
    Context ctx;
    if (!resolveImpl(raw, file, ctx, r)) {
        if (error) *error = ctx.error;
        return false;
    }
    const auto origin = r.origins.find(source);
    if (origin == r.origins.end()) return true;  // propia: no hay diferencia
    const std::string target = origin->second.source;
    std::string component;
    std::string field;
    std::string info;
    if (!what.empty() && what[0] == '#') {
        info = what.substr(1);
    } else if (const std::size_t bar = what.find('|'); bar != std::string::npos) {
        component = what.substr(0, bar);
        field = what.substr(bar + 1);
    } else {
        return false;
    }
    const auto drop = [&](json& patches) {
        if (!patches.is_array()) return false;
        const std::size_t before = patches.size();
        json kept = json::array();
        for (json& p : patches) {
            const bool match = p.is_object() && p.value("target", std::string{}) == target &&
                               (info.empty() ? (p.value("op", std::string{}) == "set" && p.value("component", std::string{}) == component &&
                                                p.value("field", std::string{}) == field)
                                             : (p.value("op", std::string{}) == "info" && p.value("field", std::string{}) == info));
            if (!match) kept.push_back(std::move(p));
        }
        patches = std::move(kept);
        return patches.size() != before;
    };
    bool changed = false;
    if (origin->second.nested.empty()) {
        if (raw.contains("patches")) changed = drop(raw["patches"]);
    } else if (raw.contains("entities") && raw["entities"].is_array()) {
        for (json& rec : raw["entities"]) {
            if (isNestedRecord(rec) && idOf(rec) == origin->second.nested && rec.contains("patches")) {
                changed = drop(rec["patches"]) || changed;
                if (rec["patches"].empty()) rec.erase("patches");
            }
        }
    }
    if (!changed) return true;
    raw["revision"] = raw.value("revision", 0) + 1;
    return writeFileAtomic(file, raw.dump(2), error);
}

// El registro de `source` como lo da la base o el anidado del que viene (sin
// las diferencias de `file`), con los ids de este prefab.
bool originRecord(const std::filesystem::path& file, const std::string& source, json& record, PrefabOrigin* where,
                  std::string* error) {
    json raw;
    Resolution r;
    if (!resolveFile(file, &raw, r, error)) return false;
    const auto origin = r.origins.find(source);
    if (origin == r.origins.end() || origin->second.file.empty()) {
        if (error) *error = "la entidad no viene de una base ni de un prefab anidado";
        return false;
    }
    Resolution sub;
    std::string sub_error;
    if (!resolveFile(origin->second.file, nullptr, sub, &sub_error)) {
        if (error) *error = sub_error;
        return false;
    }
    const PartMaps maps = partMaps(r, origin->second.nested, origin->second.nested.empty());
    for (const json& rec : entitiesOf(sub.doc)) {
        if (idOf(rec) != origin->second.source) continue;
        record = rec;
        remapStrings(record, maps.from_origin);
        if (where != nullptr) {
            *where = PrefabOrigin{origin->second.prefab, origin->second.file, origin->second.source, origin->second.nested,
                                  origin->second.nested.empty()};
        }
        return true;
    }
    if (error) *error = "la base ya no tiene esa entidad";
    return false;
}

// Pasa un valor con ids de este prefab a los del origen de `source`.
void toOriginSpace(const std::filesystem::path& file, const std::string& source, json& value) {
    Resolution r;
    if (!resolveFile(file, nullptr, r, nullptr)) return;
    const auto origin = r.origins.find(source);
    if (origin == r.origins.end()) return;
    remapStrings(value, partMaps(r, origin->second.nested, origin->second.nested.empty()).to_origin);
}

}  // namespace detail

// --- Publico -------------------------------------------------------------------------------------

void setPrefabLocations(std::vector<std::pair<Uuid, std::filesystem::path>> locations) {
    auto map = std::make_shared<LocationMap>();
    map->reserve(locations.size());
    for (auto& [id, path] : locations) {
        if (id.valid() && !path.empty()) (*map)[id] = std::move(path);
    }
    std::lock_guard lock(g_locations_mutex);
    g_locations = std::move(map);
}

std::filesystem::path locatePrefab(const Uuid& prefab) {
    std::shared_ptr<const LocationMap> map;
    {
        std::lock_guard lock(g_locations_mutex);
        map = g_locations;
    }
    if (!map || !prefab.valid()) return {};
    const auto it = map->find(prefab);
    return it != map->end() ? it->second : std::filesystem::path{};
}

std::string readPrefabFileRaw(const std::filesystem::path& file) { return readText(file); }

std::uint64_t prefabWriteGeneration() { return g_prefab_writes.load(std::memory_order_relaxed); }

bool prefabNeedsResolve(const std::string& text) {
    return text.find("\"nestedPrefab\"") != std::string::npos || text.find("\"variantOf\"") != std::string::npos;
}

std::string resolvePrefabText(const std::string& text, const std::filesystem::path& file, std::string* error) {
    if (!prefabNeedsResolve(text)) return text;
    const json raw = parseObject(text);
    if (!detail::hasPrefabStructure(raw)) return text;
    json doc;
    std::string why;
    if (!detail::resolvePrefabJson(raw, file, doc, &why)) {
        std::cerr << "[Prefab] " << (file.empty() ? raw.value("name", std::string{}) : toUtf8(file.filename())) << ": " << why << "\n";
        if (error) *error = why;
        return {};
    }
    return doc.dump(2);
}

Uuid prefabVariantBase(const std::string& raw_text) {
    if (raw_text.find("\"variantOf\"") == std::string::npos) return {};
    const json raw = parseObject(raw_text);
    if (!isVariant(raw)) return {};
    return Uuid::parse(raw[kPrefabVariantKey].value("uuid", std::string{}));
}

std::vector<Uuid> prefabDependencies(const std::filesystem::path& file) {
    if (plainPrefabFile(file)) return {};
    Resolution r;
    if (!resolveFile(file, nullptr, r, nullptr)) return {};
    return r.dependencies;
}

bool createPrefabVariant(const std::filesystem::path& base_file, const std::filesystem::path& variant_file, std::string* error) {
    std::error_code ec;
    if (std::filesystem::exists(variant_file, ec) && std::filesystem::equivalent(base_file, variant_file, ec)) {
        if (error) *error = "una variante no puede ser su propia base";
        return false;
    }
    const json base = parseObject(readText(base_file));
    const Uuid base_uuid = Uuid::parse(base.value("uuid", std::string{}));
    if (!base_uuid.valid()) {
        if (error) *error = "la base no es un prefab valido";
        return false;
    }
    json doc;
    if (!resolvedDocument(base, base_file, doc, error)) return false;
    json out = json::object();
    out["format"] = "CramionPrefab";
    out["version"] = 1;
    out["uuid"] = Uuid::generate().toString();
    out["name"] = toUtf8(variant_file.stem());
    out["revision"] = 1;
    out[kPrefabVariantKey] = {{"uuid", base_uuid.toString()},
                              {"path", detail::prefabLinkPath(base_file, variant_file)},
                              {"name", base.value("name", toUtf8(base_file.stem()))}};
    out["patches"] = json::array();
    out["entities"] = json::array();
    return writeFileAtomic(variant_file, out.dump(2), error);
}

bool detachPrefabVariant(const std::filesystem::path& file, std::string* error) {
    json raw;
    Resolution r;
    if (!resolveFile(file, &raw, r, error)) return false;
    if (!isVariant(raw)) {
        if (error) *error = "no es una variante";
        return false;
    }
    // Los anidados propios siguen como enlaces; lo de la base pasa a ser propio.
    json out = r.doc;
    json entities = json::array();
    std::unordered_set<std::string> own_nested_done;
    for (const json& rec : entitiesOf(r.doc)) {
        const std::string id = idOf(rec);
        const auto origin = r.origins.find(id);
        if (origin != r.origins.end() && !origin->second.nested.empty()) {
            // De un anidado propio de la variante: va su registro de enlace.
            const std::string nested = origin->second.nested;
            if (own_nested_done.insert(nested).second) {
                for (const json& raw_rec : entitiesOf(raw)) {
                    if (isNestedRecord(raw_rec) && idOf(raw_rec) == nested) entities.push_back(raw_rec);
                }
            }
            continue;
        }
        entities.push_back(rec);
    }
    out["entities"] = std::move(entities);
    out["revision"] = raw.value("revision", 0) + 1;
    out.erase(kPrefabVariantKey);
    out.erase("patches");
    return writeFileAtomic(file, out.dump(2), error);
}

bool unlinkNestedPrefab(const std::filesystem::path& file, const std::string& nested, std::string* error) {
    json raw;
    Resolution r;
    if (!resolveFile(file, &raw, r, error)) return false;
    json entities = json::array();
    bool found = false;
    for (const json& rec : entitiesOf(raw)) {
        if (!(isNestedRecord(rec) && idOf(rec) == nested)) {
            entities.push_back(rec);
            continue;
        }
        found = true;
        for (const json& resolved : entitiesOf(r.doc)) {
            const auto origin = r.origins.find(idOf(resolved));
            if (origin != r.origins.end() && origin->second.nested == nested) entities.push_back(resolved);
        }
    }
    if (!found) {
        if (error) *error = "no hay un prefab anidado con ese id";
        return false;
    }
    raw["entities"] = std::move(entities);
    raw["revision"] = raw.value("revision", 0) + 1;
    return writeFileAtomic(file, raw.dump(2), error);
}

std::optional<PrefabOrigin> prefabEntityOrigin(const std::filesystem::path& file, const std::string& source) {
    if (plainPrefabFile(file)) return std::nullopt;
    Resolution r;
    if (!resolveFile(file, nullptr, r, nullptr)) return std::nullopt;
    const auto it = r.origins.find(source);
    if (it == r.origins.end()) return std::nullopt;
    return PrefabOrigin{it->second.prefab, it->second.file, it->second.source, it->second.nested, it->second.nested.empty()};
}

std::vector<std::string> prefabInheritedOverrides(const std::filesystem::path& file) {
    std::vector<std::string> out;
    if (plainPrefabFile(file)) return out;
    json raw;
    Resolution r;
    if (!resolveFile(file, &raw, r, nullptr)) return out;
    const auto add = [&](const json& patches, const PartMaps& maps) {
        for (const json& p : patches) {
            if (!p.is_object()) continue;
            const std::string target = p.value("target", std::string{});
            const auto m = maps.from_origin.find(target);
            if (m == maps.from_origin.end()) continue;
            const std::string& id = m->second;
            const std::string op = p.value("op", std::string{});
            if (op == "set") out.push_back(id + "|" + p.value("component", std::string{}) + "|" + p.value("field", std::string{}));
            else if (op == "info") out.push_back(id + "|#" + p.value("field", std::string{}));
            else if (op == "addComponent") out.push_back(id + "|+" + p.value("component", std::string{}));
            else if (op == "removeComponent") out.push_back(id + "|-" + p.value("component", std::string{}));
            else if (op == "removeEntity") out.push_back(id + "|-");
        }
    };
    if (isVariant(raw)) add(patchesOf(raw), partMaps(r, {}, true));
    for (const json& rec : entitiesOf(raw)) {
        if (isNestedRecord(rec)) add(patchesOf(rec), partMaps(r, idOf(rec), false));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}  // namespace cramion::ecs
