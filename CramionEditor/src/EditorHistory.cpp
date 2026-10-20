// Historial de deshacer por diferencias (ver EditorHistory.h).

#include "EditorHistory.h"

#include <CramionCore/cvar/CVar.h>
#include <CramionCore/ecs/Components.h>
#include <CramionCore/ecs/Reflection.h>
#include <CramionCore/profiling/Profiler.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <iostream>
#include <string_view>
#include <unordered_set>

namespace cramion::editor {

namespace {

cvar::CVar<bool> g_undo_parallel("editor.undo.Parallel", true,
                                 "Reparte las huellas y los registros del historial de deshacer entre los hilos del "
                                 "JobSystem (0 = todo en el hilo principal)");

constexpr std::uint64_t kSeed = 0x6A09E667F3BCC909ull;
constexpr std::size_t kPoolChunk = 512;  // entidades de un pool por tarea

inline std::uint64_t mix64(std::uint64_t x) {
    // Finalizador de splitmix64: cada bit de entrada cambia la mitad de la salida.
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x;
}

inline std::uint64_t mix(std::uint64_t h, std::uint64_t v) { return mix64(h ^ (v + 0x9E3779B97F4A7C15ull)); }

inline std::uint64_t hashText(std::string_view text) { return std::hash<std::string_view>{}(text); }

// Huella de un componente: los mismos campos que escribe el JsonWriter de las
// escenas, sin crear JSON ni reservar memoria. Nunca cambia nada (devuelve
// false: reflect no llama a onChanged), asi que va en cualquier hilo.
class HashVisitor final : public ecs::PropertyVisitor {
public:
    std::uint64_t h = kSeed;

    bool field(const ecs::Meta&, float& v, const ecs::FloatRange&) override {
        h = mix(h, std::bit_cast<std::uint32_t>(v));
        return false;
    }
    bool field(const ecs::Meta&, int& v, int, int) override {
        h = mix(h, static_cast<std::uint32_t>(v));
        return false;
    }
    bool field(const ecs::Meta&, bool& v) override {
        h = mix(h, v ? 0xB1u : 0xB0u);
        return false;
    }
    bool field(const ecs::Meta&, std::string& v) override {
        h = mix(h, hashText(v));
        return false;
    }
    bool field(const ecs::Meta&, core::Vec3& v, ecs::Vec3Kind) override {
        h = mix(h, std::bit_cast<std::uint32_t>(v.x) | (static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(v.y)) << 32));
        h = mix(h, std::bit_cast<std::uint32_t>(v.z));
        return false;
    }
    bool field(const ecs::Meta&, core::Vec2& v, float) override {
        h = mix(h, std::bit_cast<std::uint32_t>(v.x) | (static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(v.y)) << 32));
        return false;
    }
    bool enumeration(const ecs::Meta&, int& v, std::span<const char* const>) override {
        h = mix(h, static_cast<std::uint32_t>(v));
        return false;
    }
    bool asset(const ecs::Meta&, assets::AssetRef& ref, assets::AssetType) override {
        h = mix(mix(h, ref.uuid.high), ref.uuid.low);
        return false;
    }
    bool entity(const ecs::Meta&, Uuid& ref) override {
        h = mix(mix(h, ref.high), ref.low);  // sin pasar por texto (el de PropertyVisitor reserva)
        return false;
    }
    bool layerMask(const ecs::Meta&, std::uint32_t& mask) override {
        h = mix(h, mask);
        return false;
    }
    bool beginList(const ecs::Meta&, std::size_t& count) override {
        h = mix(h, 0x11570000ull ^ count);
        return true;
    }
    bool beginListItem(std::size_t index) override {
        h = mix(h, index);
        return true;
    }
    void endListItem() override {}
    int endList() override {
        h = mix(h, 0x11570001ull);
        return -1;
    }
};

std::uint64_t componentHash(ecs::World& world, entt::entity e, const ecs::ComponentType& type) {
    HashVisitor visitor;
    type.reflect(world, e, visitor);
    return visitor.h;
}

// La parte de un componente en la huella de la entidad: se suman (el orden en
// que se recorren los pools no importa) y el tipo entra en la mezcla.
inline std::uint64_t contribution(std::size_t type, std::uint64_t component) {
    return mix64(component ^ ((static_cast<std::uint64_t>(type) + 1) * 0x9E3779B97F4A7C15ull));
}

// Nombre, estado y padre (lo del registro que no es un componente).
std::uint64_t entityLevelHash(const entt::registry& reg, entt::entity e) {
    std::uint64_t h = kSeed;
    if (const ecs::NameComponent* name = reg.try_get<ecs::NameComponent>(e)) h = mix(h, hashText(name->name));
    if (const ecs::EntityInfo* info = reg.try_get<ecs::EntityInfo>(e)) {
        h = mix(h, (info->active ? 1u : 0u) | (info->is_static ? 2u : 0u) | (info->static_batched ? 4u : 0u) |
                       (static_cast<std::uint64_t>(static_cast<std::uint32_t>(info->layer)) << 8));
        h = mix(h, hashText(info->tag));
    }
    if (const ecs::Hierarchy* hierarchy = reg.try_get<ecs::Hierarchy>(e); hierarchy != nullptr && hierarchy->parent != entt::null) {
        if (const ecs::IdComponent* id = reg.try_get<ecs::IdComponent>(hierarchy->parent)) {
            h = mix(mix(h, id->uuid.high), id->uuid.low);
        }
    }
    return h;
}

Uuid parentUuid(const entt::registry& reg, entt::entity e) {
    const ecs::Hierarchy* hierarchy = reg.try_get<ecs::Hierarchy>(e);
    if (hierarchy == nullptr || hierarchy->parent == entt::null) return {};
    const ecs::IdComponent* id = reg.try_get<ecs::IdComponent>(hierarchy->parent);
    return id != nullptr ? id->uuid : Uuid{};
}

// Los hijos que se guardan (sin los transitorios), en orden.
void persistentChildren(const entt::registry& reg, const std::vector<entt::entity>& children, std::vector<Uuid>& out) {
    out.clear();
    for (const entt::entity child : children) {
        const ecs::EntityInfo* info = reg.try_get<ecs::EntityInfo>(child);
        if (info == nullptr || info->transient) continue;
        if (const ecs::IdComponent* id = reg.try_get<ecs::IdComponent>(child)) out.push_back(id->uuid);
    }
}

// En trozos por los hilos del JobSystem, o aqui si no merece la pena.
void run(std::size_t count, std::size_t grain, const std::function<void(std::size_t, std::size_t)>& body) {
    if (count == 0) return;
    if (g_undo_parallel.get() && count > grain && jobs::workerCount() > 0) {
        jobs::parallelFor(count, grain, body);
    } else {
        body(0, count);
    }
}

// Deja a los hijos de `parent_uuid` (invalido = raices) en el orden de `order`.
// Los que no estan en la lista (transitorios) se quedan en su sitio; solo se
// mueve lo que no coincide (borrar o crear uno = como mucho un movimiento).
void applyOrder(ecs::World& world, const Uuid& parent_uuid, const std::vector<Uuid>& order) {
    ecs::Entity parent;
    if (parent_uuid.valid()) {
        parent = world.find(parent_uuid);
        if (!parent.valid()) return;
    }
    std::vector<entt::entity> wanted;
    wanted.reserve(order.size());
    for (const Uuid& id : order) {
        const ecs::Entity e = world.find(id);
        if (e.valid() && e.parent() == parent) wanted.push_back(e.handle());
    }
    if (wanted.size() < 2) return;
    const std::vector<entt::entity>& raw = parent.valid() ? parent.children() : world.roots();
    std::vector<entt::entity> current(raw.begin(), raw.end());
    const std::unordered_set<entt::entity> tracked(wanted.begin(), wanted.end());
    std::vector<entt::entity> target = current;
    std::size_t next = 0;
    for (entt::entity& slot : target) {
        if (tracked.count(slot) != 0 && next < wanted.size()) slot = wanted[next++];
    }
    for (std::size_t i = 0; i < current.size(); ++i) {
        if (current[i] == target[i]) continue;
        const entt::entity e = target[i];
        world.wrap(e).setSiblingIndex(static_cast<int>(i));
        current.erase(std::find(current.begin(), current.end(), e));
        current.insert(current.begin() + static_cast<std::ptrdiff_t>(i), e);
    }
}

}  // namespace

// --- Frozen --------------------------------------------------------------------

std::string UndoHistory::Frozen::text() const {
    std::vector<const std::string*> texts;
    texts.reserve(records.size());
    for (const RecordPtr& r : records) texts.push_back(r.get());
    return ecs::assembleScene(header, texts);
}

// --- Ciclo de vida ---------------------------------------------------------------

UndoHistory::UndoHistory() = default;
UndoHistory::~UndoHistory() = default;  // una tarea de texto en marcha solo usa su TextJob

UndoHistory::UndoHistory(UndoHistory&& other) noexcept { *this = std::move(other); }

UndoHistory& UndoHistory::operator=(UndoHistory&& other) noexcept {
    if (this == &other) return *this;
    if (job_) job_->discard = true;
    // Mover el mapa no mueve sus nodos: los Entry* de slots_ siguen valiendo.
    entries_ = std::move(other.entries_);
    roots_ = std::move(other.roots_);
    header_ = std::move(other.header_);
    valid_ = other.valid_;
    slots_ = std::move(other.slots_);
    slot_of_ = std::move(other.slot_of_);
    slots_version_ = other.slots_version_;
    slots_world_ = other.slots_world_;
    epoch_ = other.epoch_;
    undo_ = std::move(other.undo_);
    redo_ = std::move(other.redo_);
    steps_bytes_ = other.steps_bytes_;
    snapshot_bytes_ = other.snapshot_bytes_;
    version_ = other.version_;
    job_ = std::move(other.job_);
    job_handle_ = std::move(other.job_handle_);
    text_ = std::move(other.text_);
    text_version_ = other.text_version_;
    text_at_ = other.text_at_;
    stats_ = other.stats_;
    other.entries_.clear();
    other.roots_.clear();
    other.valid_ = false;
    other.slots_.clear();
    other.slot_of_.clear();
    other.slots_version_ = ~0ull;
    other.slots_world_ = nullptr;
    other.undo_.clear();
    other.redo_.clear();
    other.steps_bytes_ = 0;
    other.snapshot_bytes_ = 0;
    other.text_.clear();
    other.text_version_ = ~0ull;
    return *this;
}

void UndoHistory::dropText() {
    if (job_) job_->discard = true;
    text_.clear();
    text_version_ = ~0ull;
}

// --- Lista de entidades y huellas ---------------------------------------------------

int UndoHistory::slotOf(entt::entity e) const {
    const auto index = static_cast<std::size_t>(entt::to_entity(e));
    if (index >= slot_of_.size()) return -1;
    const std::uint32_t s = slot_of_[index];
    if (s == 0 || slots_[s - 1].handle != e) return -1;  // otra version del mismo indice
    return static_cast<int>(s - 1);
}

bool UndoHistory::refreshSlots(ecs::World& world) {
    if (slots_world_ == &world && slots_version_ == world.structureVersion()) return false;
    slots_world_ = &world;
    slots_version_ = world.structureVersion();
    ++epoch_;
    slots_.clear();
    slots_.reserve(world.entityCount());
    const entt::registry& reg = world.registry();
    std::vector<entt::entity> stack(world.roots().rbegin(), world.roots().rend());
    std::uint32_t max_index = 0;
    while (!stack.empty()) {
        const entt::entity e = stack.back();
        stack.pop_back();
        // Como serializeWorld: lo transitorio (teselas de terreno) no se
        // guarda, ni ello ni lo que cuelgue.
        const ecs::EntityInfo* info = reg.try_get<ecs::EntityInfo>(e);
        if (info == nullptr || info->transient) continue;
        const ecs::IdComponent* id = reg.try_get<ecs::IdComponent>(e);
        if (id == nullptr) continue;
        Slot slot{e, id->uuid, nullptr};
        if (const auto found = entries_.find(id->uuid); found != entries_.end()) {
            slot.entry = &found->second;
            found->second.seen = epoch_;
        }
        slots_.push_back(slot);
        max_index = std::max(max_index, static_cast<std::uint32_t>(entt::to_entity(e)));
        const std::vector<entt::entity>& children = reg.get<ecs::Hierarchy>(e).children;
        stack.insert(stack.end(), children.rbegin(), children.rend());
    }
    slot_of_.assign(slots_.empty() ? 0 : static_cast<std::size_t>(max_index) + 1, 0);
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        slot_of_[static_cast<std::size_t>(entt::to_entity(slots_[i].handle))] = static_cast<std::uint32_t>(i + 1);
    }
    return true;
}

std::uint64_t UndoHistory::hashEntity(ecs::World& world, entt::entity e) const {
    std::uint64_t h = entityLevelHash(world.registry(), e);
    const std::vector<ecs::ComponentType>& types = ecs::ComponentRegistry::instance().types();
    for (std::size_t t = 0; t < types.size(); ++t) {
        if (types[t].has(world, e)) h += contribution(t, componentHash(world, e, types[t]));
    }
    return h;
}

void UndoHistory::hashAll(ecs::World& world) {
    CR_PROFILE_SCOPE("Deshacer: huellas");
    const std::size_t count = slots_.size();
    hashes_.resize(count);
    const entt::registry& reg = world.registry();
    run(count, 1024, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) hashes_[i] = entityLevelHash(reg, slots_[i].handle);
    });
    // Los componentes, por pool: solo las entidades que tienen cada tipo (sin
    // preguntar `has` a todas por cada uno de los ~120 tipos). Una sola pasada
    // repartida en trozos (tipo, tramo del pool); cada entidad esta una vez por
    // pool, pero varios tipos pueden ir a la vez: la suma es atomica.
    struct Work {
        std::uint32_t type;
        const entt::sparse_set* pool;  // nullptr = sin pool: se pregunta `has` a cada slot
        std::size_t begin;
        std::size_t end;
    };
    const std::vector<ecs::ComponentType>& types = ecs::ComponentRegistry::instance().types();
    std::vector<Work> work;
    work.reserve(types.size() + count / kPoolChunk + 8);
    for (std::size_t t = 0; t < types.size(); ++t) {
        const entt::sparse_set* pool = types[t].pool != nullptr ? types[t].pool(world) : nullptr;
        if (types[t].pool != nullptr && (pool == nullptr || pool->empty())) continue;
        const std::size_t size = pool != nullptr ? pool->size() : count;
        for (std::size_t b = 0; b < size; b += kPoolChunk) {
            work.push_back(Work{static_cast<std::uint32_t>(t), pool, b, std::min(size, b + kPoolChunk)});
        }
    }
    run(work.size(), 1, [&](std::size_t begin, std::size_t end) {
        for (std::size_t w = begin; w < end; ++w) {
            const Work& item = work[w];
            const ecs::ComponentType& type = types[item.type];
            for (std::size_t k = item.begin; k < item.end; ++k) {
                int slot = -1;
                entt::entity e = entt::null;
                if (item.pool != nullptr) {
                    e = item.pool->data()[k];
                    if (e == entt::tombstone) continue;  // hueco de un pool con borrado en su sitio
                    slot = slotOf(e);
                } else {
                    e = slots_[k].handle;
                    slot = type.has(world, e) ? static_cast<int>(k) : -1;
                }
                if (slot < 0) continue;  // transitoria o fuera de la escena
                const std::uint64_t value = contribution(item.type, componentHash(world, e, type));
                std::atomic_ref<std::uint64_t>(hashes_[static_cast<std::size_t>(slot)]).fetch_add(value, std::memory_order_relaxed);
            }
        }
    });
}

// --- Comparar con la instantanea ------------------------------------------------------

bool UndoHistory::diff(ecs::World& world, const BeforeStep* before, Step* step) {
    const bool rebuilt = refreshSlots(world);
    hashAll(world);
    const entt::registry& reg = world.registry();

    changed_.clear();
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const Entry* entry = slots_[i].entry;
        if (entry == nullptr || entry->hash != hashes_[i]) changed_.push_back(static_cast<std::uint32_t>(i));
    }
    destroyed_.clear();
    if (rebuilt) {
        for (const auto& [uuid, entry] : entries_) {
            if (entry.seen != epoch_) destroyed_.push_back(uuid);
        }
    }

    // Lo que el editor apunta antes del paso (cambios propios de las
    // instancias de prefab a las que pertenece lo que cambio).
    if (before != nullptr && *before && (!changed_.empty() || !destroyed_.empty())) {
        std::vector<ecs::Entity> touched_in;
        touched_in.reserve(changed_.size() + destroyed_.size());
        for (const std::uint32_t i : changed_) touched_in.push_back(world.wrap(slots_[i].handle));
        for (const Uuid& uuid : destroyed_) {
            const ecs::Entity parent = world.find(entries_[uuid].parent);
            if (parent.valid()) touched_in.push_back(parent);
        }
        std::vector<ecs::Entity> touched;
        (*before)(touched_in, touched);
        for (const ecs::Entity& e : touched) {
            const int slot = e.valid() ? slotOf(e.handle()) : -1;
            if (slot < 0) continue;
            const std::uint64_t h = hashEntity(world, e.handle());
            if (h == hashes_[static_cast<std::size_t>(slot)]) continue;
            hashes_[static_cast<std::size_t>(slot)] = h;
            const auto s = static_cast<std::uint32_t>(slot);
            if (std::find(changed_.begin(), changed_.end(), s) == changed_.end()) changed_.push_back(s);
        }
    }

    // Registros de los que tienen otra huella (en paralelo si son muchos).
    {
        CR_PROFILE_SCOPE("Deshacer: registros");
        texts_.resize(changed_.size());
        run(changed_.size(), 32, [&](std::size_t begin, std::size_t end) {
            for (std::size_t k = begin; k < end; ++k) {
                texts_[k] = ecs::serializeEntityRecord(world, world.wrap(slots_[changed_[k]].handle));
            }
        });
    }
    bool any = false;
    std::size_t recorded = 0;
    for (std::size_t k = 0; k < changed_.size(); ++k) {
        Slot& slot = slots_[changed_[k]];
        Entry* entry = slot.entry;
        const std::uint64_t h = hashes_[changed_[k]];
        if (entry != nullptr && entry->record && *entry->record == texts_[k]) {
            entry->hash = h;  // la huella cambio pero el registro no (-0 / 0...)
            continue;
        }
        RecordPtr after = std::make_shared<const std::string>(std::move(texts_[k]));
        if (step != nullptr) step->changes.push_back(Change{slot.uuid, entry != nullptr ? entry->record : nullptr, after});
        if (entry == nullptr) {
            entry = &entries_[slot.uuid];
            entry->seen = epoch_;
            slot.entry = entry;
        }
        snapshot_bytes_ += after->size();
        snapshot_bytes_ -= entry->record ? std::min(snapshot_bytes_, entry->record->size()) : 0;
        entry->record = std::move(after);
        entry->hash = h;
        entry->parent = parentUuid(reg, slot.handle);
        any = true;
        ++recorded;
    }
    for (const Uuid& uuid : destroyed_) {
        const auto it = entries_.find(uuid);
        if (it == entries_.end()) continue;
        if (step != nullptr) {
            step->changes.push_back(Change{uuid, it->second.record, nullptr});
            // Para recrearlo con sus hijos en orden.
            if (!it->second.children.empty()) step->orders.push_back(Order{uuid, it->second.children, {}});
        }
        snapshot_bytes_ -= it->second.record ? std::min(snapshot_bytes_, it->second.record->size()) : 0;
        entries_.erase(it);
        any = true;
        ++recorded;
    }

    // Orden de los hijos: solo puede cambiar con la estructura.
    if (rebuilt) {
        for (const Slot& slot : slots_) {
            if (slot.entry == nullptr) continue;
            persistentChildren(reg, reg.get<ecs::Hierarchy>(slot.handle).children, children_tmp_);
            if (children_tmp_ == slot.entry->children) continue;
            if (step != nullptr) step->orders.push_back(Order{slot.uuid, slot.entry->children, children_tmp_});
            slot.entry->children = children_tmp_;
            any = true;
        }
        persistentChildren(reg, world.roots(), children_tmp_);
        if (children_tmp_ != roots_) {
            if (step != nullptr) step->orders.push_back(Order{Uuid{}, roots_, children_tmp_});
            roots_ = children_tmp_;
            any = true;
        }
    }

    // La escena (nombre y UUID); el origen flotante no es un cambio del usuario.
    if (world.sceneName() != header_.name || world.sceneUuid() != header_.uuid) {
        if (step != nullptr) {
            step->scene_before = header_;
            step->scene_after = ecs::SceneHeader{world.sceneUuid(), world.sceneName(), world.origin()};
            step->scene_changed = true;
        }
        header_.uuid = world.sceneUuid();
        header_.name = world.sceneName();
        any = true;
    }
    header_.origin = world.origin();

    stats_.entities = slots_.size();
    stats_.changed = changed_.size();
    stats_.recorded = recorded;
    if (any) ++version_;
    return any;
}

// --- API ---------------------------------------------------------------------------

void UndoHistory::reset(ecs::World& world) {
    CR_PROFILE_SCOPE("Deshacer: instantanea");
    dropText();
    undo_.clear();
    redo_.clear();
    steps_bytes_ = 0;
    snapshot_bytes_ = 0;
    entries_.clear();
    roots_.clear();
    header_ = ecs::SceneHeader{world.sceneUuid(), world.sceneName(), world.origin()};
    slots_world_ = nullptr;  // todo nuevo
    refreshSlots(world);
    hashAll(world);
    texts_.resize(slots_.size());
    run(slots_.size(), 32, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) texts_[i] = ecs::serializeEntityRecord(world, world.wrap(slots_[i].handle));
    });
    const entt::registry& reg = world.registry();
    entries_.reserve(slots_.size());
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        Slot& slot = slots_[i];
        Entry& entry = entries_[slot.uuid];
        snapshot_bytes_ += texts_[i].size();
        entry.record = std::make_shared<const std::string>(std::move(texts_[i]));
        entry.hash = hashes_[i];
        entry.parent = parentUuid(reg, slot.handle);
        persistentChildren(reg, reg.get<ecs::Hierarchy>(slot.handle).children, entry.children);
        entry.seen = epoch_;
        slot.entry = &entry;
    }
    texts_.clear();
    persistentChildren(reg, world.roots(), roots_);
    valid_ = true;
    ++version_;
}

bool UndoHistory::commit(ecs::World& world, const BeforeStep& before) {
    if (!valid_) {
        reset(world);  // sin instantanea todavia: esta es la primera
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    Step step;
    step.origin = world.origin();
    diff(world, before ? &before : nullptr, &step);
    stats_.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    CR_PROFILE_COUNTER("Deshacer: entidades en el paso", stats_.recorded);
    if (step.changes.empty() && step.orders.empty() && !step.scene_changed) return false;
    step.bytes = stepBytes(step);
    clearRedo();
    steps_bytes_ += step.bytes;
    undo_.push_back(std::move(step));
    return true;
}

void UndoHistory::sync(ecs::World& world) {
    if (!valid_) {
        reset(world);
        return;
    }
    diff(world, nullptr, nullptr);
}

bool UndoHistory::revert(ecs::World& world, std::uint64_t frozen_version) {
    // La instantanea tiene que ser la del freeze (nada la toco en Play) y los
    // registros de las raices estar en el mismo origen flotante que el mundo.
    if (!valid_ || frozen_version != version_ || world.origin() != header_.origin) return false;
    CR_PROFILE_SCOPE("Deshacer: volver de Play");
    // La diferencia Play -> instantanea como un paso (que no se guarda): diff
    // deja la instantanea con el mundo de Play y apply hacia atras devuelve
    // las entidades tocadas a su registro (y acaba con otro diff: la
    // instantanea vuelve a ser el mundo).
    Step step;
    step.origin = header_.origin;
    diff(world, nullptr, &step);
    if (!step.changes.empty() || !step.orders.empty() || step.scene_changed) {
        apply(world, step, /*forward=*/false);
    }
    // Como World::clearPersistent al leer la escena: lo transitorio que cuelga
    // de algo de la escena (lo que crearon los sistemas en Play) fuera; las
    // raices transitorias (teselas de terreno) se quedan.
    std::vector<entt::entity> doomed;
    {
        const entt::registry& reg = world.registry();
        std::vector<entt::entity> stack;
        for (const entt::entity root : world.roots()) {
            const ecs::EntityInfo* info = reg.try_get<ecs::EntityInfo>(root);
            if (info == nullptr || !info->transient) stack.push_back(root);
        }
        while (!stack.empty()) {
            const entt::entity e = stack.back();
            stack.pop_back();
            for (const entt::entity child : reg.get<ecs::Hierarchy>(e).children) {
                const ecs::EntityInfo* info = reg.try_get<ecs::EntityInfo>(child);
                if (info != nullptr && info->transient) {
                    doomed.push_back(child);
                } else {
                    stack.push_back(child);
                }
            }
        }
    }
    for (const entt::entity e : doomed) {
        if (world.valid(e)) world.destroy(world.wrap(e));
    }
    return true;
}

void UndoHistory::clearRedo() {
    for (const Step& s : redo_) steps_bytes_ -= std::min(steps_bytes_, s.bytes);
    redo_.clear();
}

std::size_t UndoHistory::trim(std::size_t max_steps, std::size_t max_bytes) {
    std::size_t dropped = 0;
    while (!undo_.empty() && (undo_.size() > max_steps || steps_bytes_ + snapshot_bytes_ > max_bytes)) {
        steps_bytes_ -= std::min(steps_bytes_, undo_.front().bytes);
        undo_.pop_front();
        ++dropped;
    }
    return dropped;
}

std::size_t UndoHistory::stepBytes(const Step& step) {
    std::size_t bytes = sizeof(Step);
    for (const Change& c : step.changes) {
        bytes += sizeof(Change) + (c.before ? c.before->size() : 0) + (c.after ? c.after->size() : 0);
    }
    for (const Order& o : step.orders) bytes += sizeof(Order) + (o.before.size() + o.after.size()) * sizeof(Uuid);
    return bytes;
}

bool UndoHistory::undo(ecs::World& world) {
    if (undo_.empty()) return false;
    Step step = std::move(undo_.back());
    undo_.pop_back();
    apply(world, step, /*forward=*/false);
    redo_.push_back(std::move(step));
    return true;
}

bool UndoHistory::redo(ecs::World& world) {
    if (redo_.empty()) return false;
    Step step = std::move(redo_.back());
    redo_.pop_back();
    apply(world, step, /*forward=*/true);
    undo_.push_back(std::move(step));
    return true;
}

void UndoHistory::apply(ecs::World& world, const Step& step, bool forward) {
    CR_PROFILE_SCOPE("Deshacer: aplicar");
    struct Todo {
        ecs::Entity entity;
        Uuid parent;
    };
    std::vector<Todo> todo;
    todo.reserve(step.changes.size());
    // Con el origen flotante movido desde el paso, el Transform de las raices
    // hay que leerlo siempre del registro (y recolocarlo en el paso 3): sin
    // comparar con como estan ahora.
    const bool shifted = step.origin != world.origin();
    // 1. Contenido: las que faltan se crean (con su UUID, en la raiz) y cada
    // una toma su registro; dentro solo se rehacen los componentes que
    // difieren de como estan ahora (la instantanea).
    for (const Change& change : step.changes) {
        const RecordPtr& target = forward ? change.after : change.before;
        if (!target) continue;  // se borra al final
        ecs::Entity e = world.find(change.uuid);
        const std::string* current = nullptr;
        if (!e.valid()) {
            e = world.create("GameObject", {}, change.uuid);
            if (e.uuid() != change.uuid) {
                std::cerr << "[Deshacer] No se pudo recrear " << change.uuid.toString() << " con su UUID\n";
                world.destroy(e);
                continue;
            }
        } else if (!shifted) {
            if (const auto it = entries_.find(change.uuid); it != entries_.end() && it->second.record) {
                current = it->second.record.get();
            }
        }
        Uuid parent;
        std::string error;
        if (!ecs::applyEntityRecord(world, e, *target, current, &parent, &error)) {
            std::cerr << "[Deshacer] " << e.name() << ": " << error << "\n";
            continue;
        }
        todo.push_back(Todo{e, parent});
    }
    // 2. Padres: primero se sueltan los que cambian de padre (asi nunca se
    // forma un ciclo al colgar uno de otro que aun estaba debajo) y luego cada
    // uno va al suyo. La posicion local es la del registro (keep_world = false).
    for (Todo& t : todo) {
        const ecs::Entity want = t.parent.valid() ? world.find(t.parent) : ecs::Entity{};
        const ecs::Entity now = t.entity.parent();
        if (now.valid() && !(now == want)) t.entity.setParent(ecs::Entity{}, false);
    }
    for (Todo& t : todo) {
        const ecs::Entity want = t.parent.valid() ? world.find(t.parent) : ecs::Entity{};
        if (want.valid() && !(t.entity.parent() == want)) t.entity.setParent(want, false);
    }
    // 3. Origen flotante movido desde el paso: las raices del registro estan
    // en el origen de entonces (en doble precision, como World::rebaseOrigin).
    if (shifted) {
        const ecs::DVec3 o = world.origin();
        for (Todo& t : todo) {
            if (t.parent.valid() || t.entity.parent().valid()) continue;
            const core::Vec3 p = t.entity.localPosition();
            t.entity.setLocalPosition(core::Vec3{static_cast<float>(p.x + (step.origin.x - o.x)),
                                                 static_cast<float>(p.y + (step.origin.y - o.y)),
                                                 static_cast<float>(p.z + (step.origin.z - o.z))});
        }
    }
    // 4. Las que no existian: fuera (de las hojas hacia arriba).
    for (auto it = step.changes.rbegin(); it != step.changes.rend(); ++it) {
        const RecordPtr& target = forward ? it->after : it->before;
        if (target) continue;
        if (const ecs::Entity e = world.find(it->uuid); e.valid()) world.destroy(e);
    }
    // 5. Orden de los hijos.
    for (const Order& order : step.orders) applyOrder(world, order.parent, forward ? order.after : order.before);
    // 6. La escena.
    if (step.scene_changed) {
        const ecs::SceneHeader& scene = forward ? step.scene_after : step.scene_before;
        world.setSceneUuid(scene.uuid);
        world.setSceneName(scene.name);
    }
    // La instantanea vuelve a ser el mundo (solo se reescriben las tocadas).
    diff(world, nullptr, nullptr);
}

UndoHistory::Frozen UndoHistory::frozenNow() const {
    Frozen frozen;
    frozen.header = header_;
    frozen.version = version_;
    frozen.records.reserve(slots_.size());
    for (const Slot& slot : slots_) {
        if (slot.entry != nullptr && slot.entry->record) frozen.records.push_back(slot.entry->record);
    }
    return frozen;
}

UndoHistory::Frozen UndoHistory::freeze(ecs::World& world) {
    sync(world);
    return frozenNow();
}

const std::string* UndoHistory::backgroundText(bool want, double min_seconds) {
    if (job_handle_ && job_handle_->done()) {
        if (job_ && !job_->discard && !job_handle_->failed()) {
            text_ = std::move(job_->text);
            text_version_ = job_->version;
        }
        job_handle_.reset();
        job_.reset();
    }
    const auto now = std::chrono::steady_clock::now();
    if (want && valid_ && !job_handle_ && text_version_ != version_ &&
        std::chrono::duration<double>(now - text_at_).count() >= min_seconds) {
        // Solo se copian punteros: el texto se pega en otro hilo.
        job_ = std::make_shared<TextJob>();
        job_->frozen = frozenNow();
        job_->version = version_;
        text_at_ = now;
        job_handle_ = jobs::schedule(
            [job = job_] {
                job->text = job->frozen.text();
                job->frozen.clear();
            },
            nullptr, jobs::Priority::Background, "Deshacer: texto de la escena");
    }
    return text_.empty() ? nullptr : &text_;
}

}  // namespace cramion::editor
