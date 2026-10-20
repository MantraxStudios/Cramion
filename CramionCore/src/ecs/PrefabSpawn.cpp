// Plantillas de prefab e instanciado por tandas (ver PrefabSpawn.h).

#include "CramionCore/ecs/PrefabSpawn.h"

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/Prefab.h"
#include "SerializerInternal.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <span>
#include <unordered_map>
#include <vector>

namespace cramion::ecs {

using detail::JsonReader;
using nlohmann::json;

struct PrefabDocument {
    json prefab;
    std::vector<const json*> records;  // apuntan dentro de `prefab` (sin copiar)
    Uuid uuid;
    int revision = 0;
};

std::shared_ptr<const PrefabDocument> parsePrefabDocument(const std::string& text) {
    auto document = std::make_shared<PrefabDocument>();
    document->prefab = json::parse(text, nullptr, false);
    if (!document->prefab.is_object()) return nullptr;
    const auto it = document->prefab.find("entities");
    if (it == document->prefab.end() || !it->is_array()) return nullptr;
    document->records.reserve(it->size());
    for (const json& record : *it) {
        if (record.is_object()) document->records.push_back(&record);
    }
    if (document->records.empty()) return nullptr;
    document->uuid = Uuid::parse(document->prefab.value("uuid", std::string{}));
    document->revision = document->prefab.value("revision", 0);
    return document;
}

std::size_t prefabDocumentSize(const PrefabDocument& document) { return document.records.size(); }

// --- Plantilla -------------------------------------------------------------------

namespace {

// Una cinta es, por cada campo que visita el reflect del componente:
//   u8 TapeOp, u32 hash de la clave, u8 cambio (lo que devolvio el lector JSON)
//   y, si cambio, el valor.
// La graba el lector JSON (TapeRecorder) sobre un componente recien creado y
// la reproduce TapeReplayer sobre otro recien creado: el mismo reflect con los
// mismos valores en el mismo orden visita lo mismo, asi que no hace falta
// buscar claves. El hash y el tipo se comprueban igual (si no casan, el
// componente se lee del JSON).
enum class TapeOp : std::uint8_t { Float, Int, Bool, String, Vec3, Vec2, Enum, Asset, Ref, List };

// Las claves pueden ser textos armados (IK: "left_hand_target"): por hash, no por puntero.
std::uint32_t keyHash(const char* key) {
    std::uint32_t h = 2166136261u;
    for (const char* c = key != nullptr ? key : ""; *c != 0; ++c) h = (h ^ static_cast<unsigned char>(*c)) * 16777619u;
    return h;
}

class TapeWriter {
public:
    explicit TapeWriter(std::vector<std::uint8_t>& out) : out_(out) {}
    void u8(std::uint8_t v) { out_.push_back(v); }
    template <typename T>
    void put(const T& v) {
        const auto* b = reinterpret_cast<const std::uint8_t*>(&v);
        out_.insert(out_.end(), b, b + sizeof(T));
    }
    void str(const std::string& s) {
        put(static_cast<std::uint32_t>(s.size()));
        out_.insert(out_.end(), s.begin(), s.end());
    }
    void head(TapeOp op, const Meta& m, bool changed) {
        u8(static_cast<std::uint8_t>(op));
        put(keyHash(m.key));
        u8(changed ? 1 : 0);
    }

private:
    std::vector<std::uint8_t>& out_;
};

// El lector JSON de siempre, grabando lo que hace.
class TapeRecorder final : public PropertyVisitor {
public:
    TapeRecorder(const json& data, std::vector<std::uint8_t>& tape, const std::unordered_map<Uuid, std::uint32_t>& index_of)
        : inner_(data), w_(tape), index_of_(index_of) {}

    std::size_t internalRefs() const { return internal_refs_; }

    bool wantsAllFields() const override { return inner_.wantsAllFields(); }
    bool beginGroup(const char* label, bool open) override { return inner_.beginGroup(label, open); }
    void endGroup() override { inner_.endGroup(); }

    bool field(const Meta& m, float& v, const FloatRange& r) override {
        const bool c = inner_.field(m, v, r);
        w_.head(TapeOp::Float, m, c);
        if (c) w_.put(v);
        return c;
    }
    bool field(const Meta& m, int& v, int mn, int mx) override {
        const bool c = inner_.field(m, v, mn, mx);
        w_.head(TapeOp::Int, m, c);
        if (c) w_.put(static_cast<std::int32_t>(v));
        return c;
    }
    bool field(const Meta& m, bool& v) override {
        const bool c = inner_.field(m, v);
        w_.head(TapeOp::Bool, m, c);
        if (c) w_.u8(v ? 1 : 0);
        return c;
    }
    bool field(const Meta& m, std::string& v) override {
        const bool c = inner_.field(m, v);
        w_.head(TapeOp::String, m, c);
        if (c) w_.str(v);
        return c;
    }
    bool field(const Meta& m, core::Vec3& v, Vec3Kind k) override {
        const bool c = inner_.field(m, v, k);
        w_.head(TapeOp::Vec3, m, c);
        if (c) {
            w_.put(v.x);
            w_.put(v.y);
            w_.put(v.z);
        }
        return c;
    }
    bool field(const Meta& m, core::Vec2& v, float s) override {
        const bool c = inner_.field(m, v, s);
        w_.head(TapeOp::Vec2, m, c);
        if (c) {
            w_.put(v.x);
            w_.put(v.y);
        }
        return c;
    }
    bool enumeration(const Meta& m, int& v, std::span<const char* const> names) override {
        const bool c = inner_.enumeration(m, v, names);
        w_.head(TapeOp::Enum, m, c);
        if (c) w_.put(static_cast<std::int32_t>(v));
        return c;
    }
    bool asset(const Meta& m, assets::AssetRef& ref, assets::AssetType type) override {
        const bool c = inner_.asset(m, ref, type);
        w_.head(TapeOp::Asset, m, c);
        if (c) {
            w_.put(ref.uuid.high);
            w_.put(ref.uuid.low);
        }
        return c;
    }
    // Como PropertyVisitor::entity (por texto, que es como lo lee el JSON),
    // pero la referencia a una entidad del propio prefab queda como indice:
    // es la lista de campos que referencian entidades, ya resuelta.
    bool entity(const Meta& m, Uuid& ref) override {
        std::string text = ref.valid() ? ref.toString() : std::string();
        const bool c = inner_.field(m, text);
        w_.head(TapeOp::Ref, m, c);
        if (!c) return false;
        ref = text.empty() ? Uuid{} : Uuid::parse(text);
        const auto it = ref.valid() ? index_of_.find(ref) : index_of_.end();
        if (it != index_of_.end()) {
            w_.u8(1);
            w_.put(it->second);
            ++internal_refs_;
        } else {
            w_.u8(0);  // de fuera del prefab (o ninguna): se queda tal cual
            w_.put(ref.high);
            w_.put(ref.low);
        }
        return true;
    }
    bool beginList(const Meta& m, std::size_t& count) override {
        const bool open = inner_.beginList(m, count);
        w_.head(TapeOp::List, m, open);
        if (open) w_.put(static_cast<std::uint32_t>(count));
        return open;
    }
    bool beginListItem(std::size_t index) override { return inner_.beginListItem(index); }
    void endListItem() override { inner_.endListItem(); }
    int endList() override { return inner_.endList(); }

private:
    JsonReader inner_;
    TapeWriter w_;
    const std::unordered_map<Uuid, std::uint32_t>& index_of_;
    std::size_t internal_refs_ = 0;
};

// Reproduce una cinta sobre un componente recien creado. Las referencias
// internas van a los UUID nuevos de la instancia (`uuids`, por registro).
class TapeReplayer final : public PropertyVisitor {
public:
    TapeReplayer(const std::uint8_t* data, std::size_t size, std::span<const Uuid> uuids)
        : p_(data), end_(data + size), uuids_(uuids) {}

    // Toda la cinta leida y casando con el reflect.
    bool finished() const { return !failed_ && p_ == end_; }

    bool field(const Meta& m, float& v, const FloatRange&) override { return value(TapeOp::Float, m, v); }
    bool field(const Meta& m, int& v, int, int) override {
        std::int32_t i = 0;
        if (!value(TapeOp::Int, m, i)) return false;
        v = i;
        return true;
    }
    bool field(const Meta& m, bool& v) override {
        std::uint8_t b = 0;
        if (!value(TapeOp::Bool, m, b)) return false;
        v = b != 0;
        return true;
    }
    bool field(const Meta& m, std::string& v) override {
        if (!head(TapeOp::String, m)) return false;
        std::uint32_t n = 0;
        if (!take(n) || static_cast<std::size_t>(end_ - p_) < n) return fail();
        v.assign(reinterpret_cast<const char*>(p_), n);
        p_ += n;
        return true;
    }
    bool field(const Meta& m, core::Vec3& v, Vec3Kind) override {
        if (!head(TapeOp::Vec3, m)) return false;
        core::Vec3 r;
        if (!take(r.x) || !take(r.y) || !take(r.z)) return fail();
        v = r;
        return true;
    }
    bool field(const Meta& m, core::Vec2& v, float) override {
        if (!head(TapeOp::Vec2, m)) return false;
        core::Vec2 r;
        if (!take(r.x) || !take(r.y)) return fail();
        v = r;
        return true;
    }
    bool enumeration(const Meta& m, int& v, std::span<const char* const>) override {
        std::int32_t i = 0;
        if (!value(TapeOp::Enum, m, i)) return false;
        v = i;
        return true;
    }
    bool asset(const Meta& m, assets::AssetRef& ref, assets::AssetType type) override {
        if (!head(TapeOp::Asset, m)) return false;
        Uuid u;
        if (!take(u.high) || !take(u.low)) return fail();
        ref.type = type;
        ref.uuid = u;
        return true;
    }
    bool entity(const Meta& m, Uuid& ref) override {
        if (!head(TapeOp::Ref, m)) return false;
        std::uint8_t internal = 0;
        if (!take(internal)) return fail();
        if (internal != 0) {
            std::uint32_t index = 0;
            if (!take(index) || index >= uuids_.size()) return fail();
            ref = uuids_[index];
        } else {
            Uuid u;
            if (!take(u.high) || !take(u.low)) return fail();
            ref = u;
        }
        return true;
    }
    bool beginList(const Meta& m, std::size_t& count) override {
        if (!head(TapeOp::List, m)) return false;
        std::uint32_t n = 0;
        if (!take(n)) return fail();
        count = n;
        return true;
    }

private:
    bool fail() {
        failed_ = true;
        return false;
    }
    template <typename T>
    bool take(T& out) {
        if (static_cast<std::size_t>(end_ - p_) < sizeof(T)) return false;
        std::memcpy(&out, p_, sizeof(T));
        p_ += sizeof(T);
        return true;
    }
    // La cabecera del campo; false si no cambio (o si la cinta no casa: ya no
    // se toca nada mas y el componente se lee del JSON).
    bool head(TapeOp op, const Meta& m) {
        if (failed_) return false;
        std::uint8_t got = 0;
        std::uint32_t hash = 0;
        std::uint8_t changed = 0;
        if (!take(got) || !take(hash) || !take(changed) || got != static_cast<std::uint8_t>(op) || hash != keyHash(m.key)) {
            return fail();
        }
        return changed != 0;
    }
    template <typename T>
    bool value(TapeOp op, const Meta& m, T& out) {
        if (!head(op, m)) return false;
        if (!take(out)) return fail();
        return true;
    }

    const std::uint8_t* p_;
    const std::uint8_t* end_;
    std::span<const Uuid> uuids_;
    bool failed_ = false;
};

}  // namespace

struct TemplateRecord {
    std::string name;
    std::string tag;
    std::string source;  // su UUID en el prefab (PrefabLink)
    int parent = -1;     // indice de un registro anterior; -1: la raiz o sin padre en el prefab
    int layer = 0;
    bool active = true;
    bool is_static = false;
    bool static_batched = false;
    std::uint32_t first = 0;  // sus componentes en PrefabTemplate::components
    std::uint32_t count = 0;
};

struct TemplateComponent {
    std::size_t type = 0;          // indice en ComponentRegistry::types() (los punteros cambian al registrar mas)
    std::uint32_t offset = 0;      // su cinta en PrefabTemplate::tape
    std::uint32_t size = 0;
    const json* source = nullptr;  // su JSON en el documento (si la cinta no casa)
};

struct PrefabTemplate {
    std::shared_ptr<const PrefabDocument> document;
    std::vector<TemplateRecord> records;
    std::vector<TemplateComponent> components;
    std::vector<std::uint8_t> tape;
    std::vector<Uuid> old_uuids;  // el UUID de cada registro en el prefab
    std::size_t internal_refs = 0;
    Uuid uuid;
    int revision = 0;
};

std::shared_ptr<const PrefabTemplate> compilePrefabTemplate(std::shared_ptr<const PrefabDocument> document) {
    if (!document || document->records.empty()) return nullptr;
    auto t = std::make_shared<PrefabTemplate>();
    t->uuid = document->uuid;
    t->revision = document->revision;
    const std::vector<const json*>& records = document->records;
    const std::size_t n = records.size();
    t->records.reserve(n);
    t->old_uuids.reserve(n);

    // Las referencias pueden ir a cualquier registro (tambien a uno de mas
    // abajo); si un UUID se repite gana el ultimo, como en el remapeo de antes.
    std::unordered_map<Uuid, std::uint32_t> index_of;
    index_of.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Uuid u = Uuid::parse(records[i]->value("uuid", std::string{}));
        t->old_uuids.push_back(u);
        if (u.valid()) index_of[u] = static_cast<std::uint32_t>(i);
    }

    // Los componentes se leen una vez, en un mundo propio (no toca el del
    // juego; los reflect solo escriben sus campos).
    World scratch;
    const std::vector<ComponentType>& types = ComponentRegistry::instance().types();
    // Padres: solo los registros anteriores, como al crear desde el JSON.
    std::unordered_map<std::string, int> seen;
    seen.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const json& record = *records[i];
        TemplateRecord r;
        r.source = record.value("uuid", std::string{});
        r.name = record.value("name", std::string{"GameObject"});
        r.tag = record.value("tag", std::string{});
        r.layer = record.value("layer", 0);
        r.active = record.value("active", true);
        r.is_static = record.value("static", false);
        r.static_batched = record.value("static_batched", false);
        if (const auto it = record.find("parent"); it != record.end() && it->is_string()) {
            if (const auto found = seen.find(it->get<std::string>()); found != seen.end()) r.parent = found->second;
        }
        r.first = static_cast<std::uint32_t>(t->components.size());
        if (const auto it = record.find("components"); it != record.end() && it->is_object()) {
            Entity probe = scratch.create("probe");
            for (const auto& [name, data] : it->items()) {
                const ComponentType* type = ComponentRegistry::instance().find(name);
                if (type == nullptr) {
                    std::cerr << "[Escena] Componente desconocido \"" << name << "\" en " << r.name << ": se ignora\n";
                    continue;
                }
                TemplateComponent c;
                c.type = static_cast<std::size_t>(type - types.data());
                c.offset = static_cast<std::uint32_t>(t->tape.size());
                c.source = &data;
                type->add(scratch, probe.handle());
                TapeRecorder recorder(data, t->tape, index_of);
                type->reflect(scratch, probe.handle(), recorder);
                c.size = static_cast<std::uint32_t>(t->tape.size() - c.offset);
                t->internal_refs += recorder.internalRefs();
                t->components.push_back(c);
            }
            scratch.destroy(probe);
        }
        r.count = static_cast<std::uint32_t>(t->components.size()) - r.first;
        if (!r.source.empty()) seen[r.source] = static_cast<int>(i);
        t->records.push_back(std::move(r));
    }
    t->tape.shrink_to_fit();
    t->document = std::move(document);
    return t;
}

std::shared_ptr<const PrefabTemplate> compilePrefabTemplate(const std::string& text) {
    return compilePrefabTemplate(parsePrefabDocument(text));
}

std::size_t prefabTemplateSize(const PrefabTemplate& prefab) { return prefab.records.size(); }

std::size_t prefabTemplateBytes(const PrefabTemplate& prefab) {
    std::size_t bytes = prefab.tape.capacity() + prefab.components.capacity() * sizeof(TemplateComponent) +
                        prefab.old_uuids.capacity() * sizeof(Uuid);
    for (const TemplateRecord& r : prefab.records) {
        bytes += sizeof(TemplateRecord) + r.name.capacity() + r.tag.capacity() + r.source.capacity();
    }
    return bytes;
}

std::size_t prefabTemplateInternalRefs(const PrefabTemplate& prefab) { return prefab.internal_refs; }

namespace {

// Crea el registro i bajo `parent` con sus componentes desde la cinta. Si una
// cinta no casa, ese componente se lee del JSON y la entidad va a `remap`.
Entity createRecord(World& world, const PrefabTemplate& t, std::size_t i, Entity parent, std::span<const Uuid> uuids,
                    std::vector<Entity>& remap) {
    const TemplateRecord& r = t.records[i];
    Entity e = world.create(r.name, parent, uuids[i]);
    EntityInfo& info = e.get<EntityInfo>();
    info.active = r.active;
    info.tag = r.tag;
    info.layer = r.layer;
    info.is_static = r.is_static;
    info.static_batched = r.static_batched;
    const std::vector<ComponentType>& types = ComponentRegistry::instance().types();
    bool fallback = false;
    for (std::uint32_t k = r.first; k < r.first + r.count; ++k) {
        const TemplateComponent& c = t.components[k];
        const ComponentType& type = types[c.type];
        type.add(world, e.handle());
        TapeReplayer replay(t.tape.data() + c.offset, c.size, uuids);
        type.reflect(world, e.handle(), replay);
        if (!replay.finished() && c.source != nullptr) {
            JsonReader reader(*c.source);
            type.reflect(world, e.handle(), reader);
            fallback = true;
        }
    }
    if (fallback) remap.push_back(e);
    e.add<PrefabLink>().source = r.source;
    return e;
}

// Los UUID nuevos de una instancia, antes de crear nada: las referencias a
// entidades de mas abajo ya se escriben bien.
std::vector<Uuid> freshUuids(std::size_t n) {
    std::vector<Uuid> uuids(n);
    for (Uuid& u : uuids) u = Uuid::generate();
    return uuids;
}

// UUID del prefab -> de la instancia (solo para lo leido del JSON).
std::unordered_map<Uuid, Uuid> instanceRefs(const PrefabTemplate& t, std::span<const Uuid> uuids) {
    std::unordered_map<Uuid, Uuid> refs;
    for (std::size_t i = 0; i < t.old_uuids.size() && i < uuids.size(); ++i) {
        if (t.old_uuids[i].valid()) refs[t.old_uuids[i]] = uuids[i];
    }
    return refs;
}

void markInstance(Entity root, const PrefabTemplate& t) {
    PrefabInstance& instance = root.add<PrefabInstance>();
    instance.prefab = assets::AssetRef{t.uuid, assets::AssetType::Prefab};
    instance.revision = t.revision;
}

}  // namespace

Entity instantiatePrefabTemplate(World& world, const PrefabTemplate& t, Entity parent, bool active) {
    const std::size_t n = t.records.size();
    if (n == 0) return {};
    const std::vector<Uuid> uuids = freshUuids(n);
    std::vector<Entity> built;
    built.reserve(n);
    std::vector<Entity> remap;
    for (std::size_t i = 0; i < n; ++i) {
        const int p = t.records[i].parent;
        Entity e = createRecord(world, t, i, p >= 0 ? built[static_cast<std::size_t>(p)] : parent, uuids, remap);
        // La raiz desactivada antes de colgarle nada (pool): nada se activa a medias.
        if (i == 0 && !active) e.get<EntityInfo>().active = false;
        built.push_back(e);
    }
    if (!remap.empty()) {
        const auto refs = instanceRefs(t, uuids);
        for (const Entity& e : remap) world.remapEntityRefs(e, refs);
    }
    markInstance(built.front(), t);
    return built.front();
}

// --- Por tandas ------------------------------------------------------------------

struct PrefabSpawn::State {
    std::size_t next = 0;        // siguiente registro que crear
    std::size_t remap_next = 0;  // siguiente entidad que remapear (las de cintas que no casaron)
    std::vector<Entity> built;
    std::vector<Uuid> uuids;
    std::vector<Entity> remap;
    std::unordered_map<Uuid, Uuid> refs;  // solo si hay algo en remap
};

PrefabSpawn::PrefabSpawn(std::shared_ptr<const PrefabTemplate> prefab, Entity parent)
    : template_(std::move(prefab)), state_(std::make_unique<State>()), parent_(parent) {
    if (!template_) stage_ = Stage::Failed;
}

PrefabSpawn::PrefabSpawn(std::shared_ptr<const PrefabDocument> document, Entity parent)
    : document_(std::move(document)), state_(std::make_unique<State>()), parent_(parent) {
    if (!document_) stage_ = Stage::Failed;
}

PrefabSpawn::~PrefabSpawn() = default;

float PrefabSpawn::progress() const {
    if (stage_ == Stage::Done) return 1.0f;
    if (!template_ || template_->records.empty()) return 0.0f;
    const auto total = static_cast<float>(template_->records.size());
    // Crear pesa casi todo; el remapeo (solo si alguna cinta no caso), el resto.
    const float created = static_cast<float>(state_->next) / total;
    const float remapped =
        state_->remap.empty() ? 0.0f : static_cast<float>(state_->remap_next) / static_cast<float>(state_->remap.size());
    return std::min(0.95f * created + 0.05f * remapped, 0.999f);
}

void PrefabSpawn::cancel(World& world) {
    if (done()) return;
    if (root_.valid()) world.destroy(root_);
    root_ = {};
    stage_ = Stage::Failed;
}

bool PrefabSpawn::step(World& world, double budget_ms) {
    if (done()) return true;
    const auto started = std::chrono::steady_clock::now();
    const auto out_of_time = [&]() {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() >= budget_ms;
    };
    State& s = *state_;
    // Con el documento: se compila ahora (una vez, en este hilo).
    if (!template_) {
        template_ = compilePrefabTemplate(document_);
        document_.reset();
        if (!template_) {
            stage_ = Stage::Failed;
            return true;
        }
    }
    const PrefabTemplate& t = *template_;
    // La raiz se destruyo por fuera (cambio de escena, un script): se aborta.
    if (s.next > 0 && !root_.valid()) {
        stage_ = Stage::Failed;
        return true;
    }
    // El padre pedido ya no existe: se cuelga de la raiz del mundo.
    if (parent_.valid() == false) parent_ = {};

    if (stage_ == Stage::Create) {
        if (s.uuids.empty()) {
            s.uuids = freshUuids(t.records.size());
            s.built.reserve(t.records.size());
        }
        bool first_in_step = true;
        while (s.next < t.records.size()) {
            if (!first_in_step && out_of_time()) return false;
            first_in_step = false;
            const std::size_t i = s.next++;
            const int p = t.records[i].parent;
            // Mientras se crea, todo cuelga de la raiz: los que no tienen padre
            // en el prefab tambien, para que nada se vea a medias.
            Entity parent = root_.valid() ? root_ : parent_;
            if (p >= 0 && s.built[static_cast<std::size_t>(p)].valid()) parent = s.built[static_cast<std::size_t>(p)];
            Entity e = createRecord(world, t, i, parent, s.uuids, s.remap);
            s.built.push_back(e);
            if (!root_.valid()) {
                // La raiz, desactivada hasta terminar (los hijos no se ven).
                EntityInfo& info = e.get<EntityInfo>();
                root_ = e;
                root_active_ = info.active;
                info.active = false;
                // La raiz lleva su Transform del prefab: la posicion pedida va despues.
                if (has_position_) e.setWorldPosition(position_);
                if (has_euler_) e.setLocalEulerDegrees(euler_);
            }
        }
        stage_ = Stage::Remap;
        if (!s.remap.empty()) {
            s.refs = instanceRefs(t, s.uuids);
            if (out_of_time()) return false;
        }
    }

    if (stage_ == Stage::Remap) {
        // Solo las entidades con alguna cinta que no caso (normalmente ninguna).
        while (s.remap_next < s.remap.size()) {
            const Entity e = s.remap[s.remap_next++];
            if (e.valid()) world.remapEntityRefs(e, s.refs);
            if (out_of_time() && s.remap_next < s.remap.size()) return false;
        }
        if (!root_.valid()) {
            stage_ = Stage::Failed;
            return true;
        }
        markInstance(root_, t);
        // Una sola activacion al final (con sus avisos: RenderSync, fisica...).
        root_.setActive(root_active_);
        stage_ = Stage::Done;
        s = State{};  // suelta las listas
    }
    return true;
}

}  // namespace cramion::ecs
