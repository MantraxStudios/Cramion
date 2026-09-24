#ifndef CRAMION_CORE_ECS_WORLD_H
#define CRAMION_CORE_ECS_WORLD_H

// El mundo de una escena: un entt::registry (ECS por sparse sets: cada tipo
// de componente en su propio pool contiguo, sin reservas sueltas por
// entidad) con la capa de Unity encima: UUID, nombre, activo, jerarquia
// ordenada y Transform con matrices en cache.

#include "CramionCore/Uuid.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/Reflection.h"

#include <entt/entity/registry.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cramion::ecs {

class World;

// Asa ligera a una entidad (se copia por valor). Invalida si la entidad se
// destruye: comprobar valid() antes de usar una guardada.
class Entity {
public:
    Entity() = default;
    Entity(entt::entity handle, World* world) : handle_(handle), world_(world) {}

    bool valid() const;
    explicit operator bool() const { return valid(); }
    entt::entity handle() const { return handle_; }
    World* world() const { return world_; }

    const Uuid& uuid() const;
    const std::string& name() const;
    void setName(std::string name);

    bool activeSelf() const;
    bool activeInHierarchy() const;
    void setActive(bool active);

    // --- Jerarquia ---
    Entity parent() const;
    const std::vector<entt::entity>& children() const;
    std::size_t childCount() const { return children().size(); }
    Entity child(std::size_t index) const;
    // Emparenta (Entity{} = raiz). keep_world: conserva la posicion en el
    // mundo (como arrastrar en la Jerarquia de Unity). `index` = posicion
    // entre los hermanos (-1 = al final). Falla si crearia un ciclo.
    bool setParent(Entity parent, bool keep_world = true, int index = -1);
    int siblingIndex() const;
    void setSiblingIndex(int index);
    bool isAncestorOf(Entity other) const;

    // --- Componentes ---
    template <typename T, typename... Args>
    T& add(Args&&... args);
    template <typename T>
    T& get() const;
    template <typename T>
    T* tryGet() const;
    template <typename T>
    bool has() const;
    template <typename T>
    void remove();

    // --- Transform (atajos que mantienen la cache) ---
    Transform& transform() const;
    core::Vec3 localPosition() const;
    void setLocalPosition(const core::Vec3& position);
    core::Quat localRotation() const;
    void setLocalRotation(const core::Quat& rotation);
    core::Vec3 localEulerDegrees() const;
    void setLocalEulerDegrees(const core::Vec3& degrees);
    core::Vec3 localScale() const;
    void setLocalScale(const core::Vec3& scale);
    void setLocalTrs(const core::Vec3& position, const core::Quat& rotation, const core::Vec3& scale);
    const core::Mat4& localMatrix() const;
    const core::Mat4& worldMatrix() const;
    core::Vec3 worldPosition() const;
    void setWorldPosition(const core::Vec3& position);
    // Para los gizmos: fija la matriz de mundo (se pasa a local respecto al padre).
    void setWorldMatrix(const core::Mat4& world);
    core::Vec3 forward() const;  // -Z local en el mundo (como la camara del motor)
    core::Vec3 right() const;    // +X
    core::Vec3 up() const;       // +Y

    friend bool operator==(const Entity& a, const Entity& b) {
        return a.handle_ == b.handle_ && a.world_ == b.world_;
    }

private:
    entt::entity handle_ = entt::null;
    World* world_ = nullptr;
};

class World {
public:
    World();
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // --- Entidades ---
    // Nueva entidad con Id, nombre, EntityInfo, Hierarchy y Transform.
    Entity create(std::string name = "GameObject", Entity parent = {}, const Uuid& uuid = {});
    // Destruye la entidad y todos sus descendientes.
    void destroy(Entity entity);
    // Copia profunda (componentes e hijos) con UUIDs nuevos, justo despues
    // del original entre sus hermanos.
    Entity duplicate(Entity entity);
    void clear();

    Entity find(const Uuid& uuid) const;
    Entity findByName(std::string_view name) const;  // la primera
    Entity wrap(entt::entity handle) const {
        return Entity{handle, const_cast<World*>(this)};
    }
    bool valid(entt::entity handle) const { return registry_.valid(handle); }
    std::size_t entityCount() const { return uuid_map_.size(); }

    // Raices en orden (las entidades sin padre, como en la Jerarquia).
    const std::vector<entt::entity>& roots() const { return roots_; }
    // Recorre en profundidad (padre antes que hijos, en orden de hermanos).
    template <typename Fn>
    void forEachDepthFirst(Fn&& fn) const;

    // --- Transform ---
    void markTransformDirty(entt::entity entity);
    const core::Mat4& worldMatrix(entt::entity entity) const;

    // --- Escena ---
    const Uuid& sceneUuid() const { return scene_uuid_; }
    void setSceneUuid(const Uuid& uuid) { scene_uuid_ = uuid; }
    const std::string& sceneName() const { return scene_name_; }
    void setSceneName(std::string name) { scene_name_ = std::move(name); }

    // Sube con cada cambio de estructura (crear, destruir, emparentar): los
    // sistemas lo usan para saber cuando rehacer sus listas.
    std::uint64_t structureVersion() const { return structure_version_; }

    entt::registry& registry() { return registry_; }
    const entt::registry& registry() const { return registry_; }

private:
    friend class Entity;

    void detach(entt::entity entity);
    void attach(entt::entity entity, entt::entity parent, int index);
    void destroyRecursive(entt::entity entity);
    entt::entity duplicateRecursive(entt::entity source, entt::entity parent, int index);
    const core::Mat4& computeWorld(entt::entity entity) const;
    std::vector<entt::entity>& siblingsOf(entt::entity entity);

    entt::registry registry_;
    std::vector<entt::entity> roots_;
    std::unordered_map<Uuid, entt::entity> uuid_map_;
    Uuid scene_uuid_ = Uuid::generate();
    std::string scene_name_ = "Escena";
    std::uint64_t structure_version_ = 0;
};

// --- Plantillas ---------------------------------------------------------------

template <typename T, typename... Args>
T& Entity::add(Args&&... args) {
    T& component = world_->registry_.emplace_or_replace<T>(handle_, std::forward<Args>(args)...);
    ++world_->structure_version_;
    return component;
}

template <typename T>
T& Entity::get() const {
    return world_->registry_.get<T>(handle_);
}

template <typename T>
T* Entity::tryGet() const {
    return world_ != nullptr ? world_->registry_.try_get<T>(handle_) : nullptr;
}

template <typename T>
bool Entity::has() const {
    return world_ != nullptr && world_->registry_.valid(handle_) &&
           world_->registry_.all_of<T>(handle_);
}

template <typename T>
void Entity::remove() {
    world_->registry_.remove<T>(handle_);
    ++world_->structure_version_;
}

template <typename Fn>
void World::forEachDepthFirst(Fn&& fn) const {
    std::vector<entt::entity> stack(roots_.rbegin(), roots_.rend());
    while (!stack.empty()) {
        const entt::entity e = stack.back();
        stack.pop_back();
        fn(wrap(e));
        const Hierarchy& h = registry_.get<Hierarchy>(e);
        stack.insert(stack.end(), h.children.rbegin(), h.children.rend());
    }
}

namespace detail {
template <typename T>
concept HasOnChanged = requires(World& w, entt::entity e) { T::onChanged(w, e); };
}

template <typename T>
void ComponentRegistry::registerComponent(std::string name, std::string label,
                                          std::string category, bool removable, bool addable) {
    if (find(name) != nullptr) {
        return;
    }
    ComponentType type;
    type.name = std::move(name);
    type.label = std::move(label);
    type.category = std::move(category);
    type.removable = removable;
    type.addable = addable;
    type.has = [](const World& w, entt::entity e) { return w.registry().all_of<T>(e); };
    type.add = [](World& w, entt::entity e) {
        if (!w.registry().all_of<T>(e)) {
            w.wrap(e).add<T>();
        }
    };
    type.remove = [](World& w, entt::entity e) {
        if (w.registry().all_of<T>(e)) {
            w.wrap(e).remove<T>();
        }
    };
    type.reflect = [](World& w, entt::entity e, PropertyVisitor& visitor) {
        T* component = w.registry().try_get<T>(e);
        if (component == nullptr) {
            return false;
        }
        const bool changed = [&] {
            // reflect() devuelve void: se mira si algun campo cambio con un
            // visitante que cuenta los cambios.
            struct Counting final : PropertyVisitor {
                PropertyVisitor& inner;
                bool changed = false;
                explicit Counting(PropertyVisitor& v) : inner(v) {}
                bool wantsAllFields() const override { return inner.wantsAllFields(); }
                bool beginGroup(const char* l, bool o) override { return inner.beginGroup(l, o); }
                void endGroup() override { inner.endGroup(); }
                bool mark(bool c) { changed |= c; return c; }
                bool field(const Meta& m, float& v, const FloatRange& r) override { return mark(inner.field(m, v, r)); }
                bool field(const Meta& m, int& v, int mn, int mx) override { return mark(inner.field(m, v, mn, mx)); }
                bool field(const Meta& m, bool& v) override { return mark(inner.field(m, v)); }
                bool field(const Meta& m, std::string& v) override { return mark(inner.field(m, v)); }
                bool field(const Meta& m, core::Vec3& v, Vec3Kind k) override { return mark(inner.field(m, v, k)); }
                bool field(const Meta& m, core::Vec2& v, float s) override { return mark(inner.field(m, v, s)); }
                bool enumeration(const Meta& m, int& v, std::span<const char* const> n) override { return mark(inner.enumeration(m, v, n)); }
                bool asset(const Meta& m, assets::AssetRef& r, assets::AssetType t) override { return mark(inner.asset(m, r, t)); }
                bool layerMask(const Meta& m, std::uint32_t& v) override { return mark(inner.layerMask(m, v)); }
                bool entity(const Meta& m, Uuid& r) override { return mark(inner.entity(m, r)); }
                bool beginList(const Meta& m, std::size_t& n) override {
                    const std::size_t before = n;
                    const bool open = inner.beginList(m, n);
                    mark(n != before);
                    return open;
                }
                bool beginListItem(std::size_t i) override { return inner.beginListItem(i); }
                void endListItem() override { inner.endListItem(); }
                int endList() override {
                    const int remove = inner.endList();
                    mark(remove >= 0);
                    return remove;
                }
            } counting(visitor);
            component->reflect(counting);
            return counting.changed;
        }();
        if (changed) {
            if constexpr (detail::HasOnChanged<T>) {
                T::onChanged(w, e);
            }
        }
        return changed;
    };
    type.copy = [](World& w, entt::entity from, entt::entity to) {
        if (const T* source = w.registry().try_get<T>(from)) {
            w.registry().emplace_or_replace<T>(to, *source);
            if constexpr (detail::HasOnChanged<T>) {
                T::onChanged(w, to);
            }
        }
    };
    types_.push_back(std::move(type));
}

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_ECS_WORLD_H
