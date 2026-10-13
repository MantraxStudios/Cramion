#include "CramionCore/ecs/World.h"

#include "CramionCore/ecs/MathUtil.h"

#include <algorithm>
#include <cmath>

namespace cramion::ecs {

using core::Mat4;
using core::Quat;
using core::Vec3;

namespace {
const std::vector<entt::entity> kNoChildren;
const std::string kNoName;
const Uuid kNoUuid{};
}  // namespace

// -----------------------------------------------------------------------------
// Entity
// -----------------------------------------------------------------------------

bool Entity::valid() const {
    return world_ != nullptr && world_->registry_.valid(handle_);
}

const Uuid& Entity::uuid() const {
    const IdComponent* id = tryGet<IdComponent>();
    return id != nullptr ? id->uuid : kNoUuid;
}

const std::string& Entity::name() const {
    const NameComponent* name = tryGet<NameComponent>();
    return name != nullptr ? name->name : kNoName;
}

void Entity::setName(std::string name) {
    get<NameComponent>().name = std::move(name);
}

const std::string& Entity::tag() const {
    static const std::string kUntaggedName = "Untagged";
    const EntityInfo* info = tryGet<EntityInfo>();
    return info == nullptr || info->tag.empty() ? kUntaggedName : info->tag;
}

void Entity::setTag(std::string tag) {
    if (EntityInfo* info = tryGet<EntityInfo>()) info->tag = tag == "Untagged" ? std::string() : std::move(tag);
}

bool Entity::compareTag(std::string_view tag) const {
    return tag == this->tag();
}

bool Entity::activeSelf() const {
    const EntityInfo* info = tryGet<EntityInfo>();
    return info == nullptr || info->active;
}

bool Entity::activeInHierarchy() const {
    for (Entity e = *this; e.valid(); e = e.parent()) {
        if (!e.activeSelf()) {
            return false;
        }
    }
    return true;
}

void Entity::setActive(bool active) {
    get<EntityInfo>().active = active;
    ++world_->structure_version_;
}

Entity Entity::parent() const {
    const Hierarchy* h = tryGet<Hierarchy>();
    if (h == nullptr || h->parent == entt::null) {
        return {};
    }
    return Entity{h->parent, world_};
}

const std::vector<entt::entity>& Entity::children() const {
    const Hierarchy* h = tryGet<Hierarchy>();
    return h != nullptr ? h->children : kNoChildren;
}

Entity Entity::child(std::size_t index) const {
    const auto& c = children();
    return index < c.size() ? Entity{c[index], world_} : Entity{};
}

bool Entity::isAncestorOf(Entity other) const {
    for (Entity e = other.parent(); e.valid(); e = e.parent()) {
        if (e == *this) {
            return true;
        }
    }
    return false;
}

bool Entity::setParent(Entity parent, bool keep_world, int index) {
    if (!valid() || parent == *this || (parent.valid() && isAncestorOf(parent))) {
        return false;  // ciclo
    }
    const Mat4 world = worldMatrix();
    world_->detach(handle_);
    world_->attach(handle_, parent.valid() ? parent.handle() : entt::null, index);
    if (keep_world) {
        setWorldMatrix(world);
    } else {
        world_->markTransformDirty(handle_);
    }
    return true;
}

int Entity::siblingIndex() const {
    World& w = *world_;
    const auto& siblings = w.siblingsOf(handle_);
    const auto it = std::find(siblings.begin(), siblings.end(), handle_);
    return it == siblings.end() ? -1 : static_cast<int>(it - siblings.begin());
}

void Entity::setSiblingIndex(int index) {
    auto& siblings = world_->siblingsOf(handle_);
    const auto it = std::find(siblings.begin(), siblings.end(), handle_);
    if (it == siblings.end()) {
        return;
    }
    siblings.erase(it);
    const auto clamped = std::clamp<std::ptrdiff_t>(index, 0, static_cast<std::ptrdiff_t>(siblings.size()));
    siblings.insert(siblings.begin() + clamped, handle_);
    ++world_->structure_version_;
}

Transform& Entity::transform() const {
    return get<Transform>();
}

Vec3 Entity::localPosition() const {
    return transform().position;
}

void Entity::setLocalPosition(const Vec3& position) {
    transform().position = position;
    world_->markTransformDirty(handle_);
}

Quat Entity::localRotation() const {
    return transform().rotation;
}

void Entity::setLocalRotation(const Quat& rotation) {
    Transform& t = transform();
    t.rotation = core::normalize(rotation);
    t.euler = quatToEulerDegrees(t.rotation);
    world_->markTransformDirty(handle_);
}

Vec3 Entity::localEulerDegrees() const {
    return transform().euler;
}

void Entity::setLocalEulerDegrees(const Vec3& degrees) {
    Transform& t = transform();
    t.euler = degrees;
    t.rotation = quatFromEulerDegrees(degrees);
    world_->markTransformDirty(handle_);
}

Vec3 Entity::localScale() const {
    return transform().scale;
}

void Entity::setLocalScale(const Vec3& scale) {
    transform().scale = scale;
    world_->markTransformDirty(handle_);
}

void Entity::setLocalTrs(const Vec3& position, const Quat& rotation, const Vec3& scale) {
    Transform& t = transform();
    t.position = position;
    t.rotation = core::normalize(rotation);
    t.euler = quatToEulerDegrees(t.rotation);
    t.scale = scale;
    world_->markTransformDirty(handle_);
}

const Mat4& Entity::localMatrix() const {
    world_->computeWorld(handle_);
    return transform().local_matrix;
}

const Mat4& Entity::worldMatrix() const {
    return world_->computeWorld(handle_);
}

Vec3 Entity::worldPosition() const {
    const Mat4& m = worldMatrix();
    return Vec3{m.m[3][0], m.m[3][1], m.m[3][2]};
}

void Entity::setWorldPosition(const Vec3& position) {
    Mat4 world = worldMatrix();
    world.m[3][0] = position.x;
    world.m[3][1] = position.y;
    world.m[3][2] = position.z;
    setWorldMatrix(world);
}

void Entity::setWorldMatrix(const Mat4& world) {
    Mat4 local = world;
    if (Entity p = parent(); p.valid()) {
        local = core::inverse(p.worldMatrix()) * world;
    }
    Vec3 position{};
    Quat rotation{};
    Vec3 scale{};
    decomposeMatrix(local, position, rotation, scale);
    Transform& t = transform();
    // Conserva los grados que ya se veian si la rotacion no cambio (evita
    // saltos de 180 a -180 al mover solo la posicion con el gizmo). Se compara
    // con la rotacion de esos grados (no con la anterior): muchos giros
    // pequenos seguidos tambien acaban actualizandolos.
    const Quat before = quatFromEulerDegrees(t.euler);
    const float same = std::abs(before.x * rotation.x + before.y * rotation.y +
                                before.z * rotation.z + before.w * rotation.w);
    t.position = position;
    t.scale = scale;
    // La rotacion siempre se asigna (giros pequenos y acumulados no se pierden);
    // solo los grados se recalculan si cambio de verdad, y se desenrollan al
    // valor mas cercano al anterior (sin saltos de 360).
    t.rotation = rotation;
    if (same < 0.999999f) {
        const Vec3 previous = t.euler;
        Vec3 euler = quatToEulerDegrees(rotation);
        const auto unwrap = [](float value, float closest) {
            return value + 360.0f * std::round((closest - value) / 360.0f);
        };
        euler.x = unwrap(euler.x, previous.x);
        euler.y = unwrap(euler.y, previous.y);
        euler.z = unwrap(euler.z, previous.z);
        t.euler = euler;
    }
    world_->markTransformDirty(handle_);
}

Vec3 Entity::forward() const {
    return core::normalize(transformDirection(worldMatrix(), Vec3{0.0f, 0.0f, -1.0f}));
}

Vec3 Entity::right() const {
    return core::normalize(transformDirection(worldMatrix(), Vec3{1.0f, 0.0f, 0.0f}));
}

Vec3 Entity::up() const {
    return core::normalize(transformDirection(worldMatrix(), Vec3{0.0f, 1.0f, 0.0f}));
}

// -----------------------------------------------------------------------------
// World
// -----------------------------------------------------------------------------

World::World() {
    ComponentRegistry::instance();  // registra los componentes del motor
}

World::~World() = default;

Entity World::create(std::string name, Entity parent, const Uuid& uuid) {
    const entt::entity e = registry_.create();
    const Uuid id = uuid.valid() && !uuid_map_.contains(uuid) ? uuid : Uuid::generate();
    registry_.emplace<IdComponent>(e, id);
    registry_.emplace<NameComponent>(e, std::move(name));
    registry_.emplace<EntityInfo>(e);
    registry_.emplace<Hierarchy>(e);
    registry_.emplace<Transform>(e);
    uuid_map_[id] = e;
    attach(e, parent.valid() ? parent.handle() : entt::null, -1);
    return Entity{e, this};
}

void World::destroy(Entity entity) {
    if (!entity.valid() || entity.world() != this) {
        return;
    }
    detach(entity.handle());
    destroyRecursive(entity.handle());
    ++structure_version_;
}

void World::destroyRecursive(entt::entity e) {
    // Copia: destruir un hijo no toca la lista del padre (ya desenganchado),
    // pero asi no dependemos de ello.
    const std::vector<entt::entity> children = registry_.get<Hierarchy>(e).children;
    for (const entt::entity child : children) {
        destroyRecursive(child);
    }
    // Solo si el UUID es de ESTA entidad: borrar una copia con el mismo UUID
    // no debe dejar la buena imposible de encontrar.
    if (const auto it = uuid_map_.find(registry_.get<IdComponent>(e).uuid); it != uuid_map_.end() && it->second == e) {
        uuid_map_.erase(it);
    }
    registry_.destroy(e);
}

Entity World::duplicate(Entity entity) {
    if (!entity.valid()) {
        return {};
    }
    const Hierarchy& h = registry_.get<Hierarchy>(entity.handle());
    const int index = entity.siblingIndex() + 1;
    std::unordered_map<Uuid, Uuid> remap;
    std::vector<entt::entity> copies;
    const entt::entity copy = duplicateRecursive(entity.handle(), h.parent, index, remap, copies);
    // Las referencias dentro de lo copiado apuntan a las copias (una camara
    // que sigue a su hijo sigue al hijo copiado); las de fuera no cambian.
    for (const entt::entity e : copies) {
        remapEntityRefs(wrap(e), remap);
    }
    return Entity{copy, this};
}

entt::entity World::duplicateRecursive(entt::entity source, entt::entity parent, int index,
                                       std::unordered_map<Uuid, Uuid>& remap,
                                       std::vector<entt::entity>& copies) {
    Entity copy = create(registry_.get<NameComponent>(source).name, wrap(parent));
    // create() lo pone al final: se recoloca donde toca.
    if (index >= 0) {
        copy.setSiblingIndex(index);
    }
    registry_.get<EntityInfo>(copy.handle()) = registry_.get<EntityInfo>(source);
    // La copia es del usuario: una tesela de terreno duplicada no puede quedar
    // transitoria (no se guardaria ni la borraria nadie).
    registry_.get<EntityInfo>(copy.handle()).transient = false;
    remap[registry_.get<IdComponent>(source).uuid] = copy.uuid();
    copies.push_back(copy.handle());
    for (const ComponentType& type : ComponentRegistry::instance().types()) {
        if (type.copy) {
            type.copy(*this, source, copy.handle());
        }
    }
    const std::vector<entt::entity> children = registry_.get<Hierarchy>(source).children;
    for (const entt::entity child : children) {
        duplicateRecursive(child, copy.handle(), -1, remap, copies);
    }
    markTransformDirty(copy.handle());
    return copy.handle();
}

namespace {
// Visitante que solo toca las referencias a entidades.
class EntityRefRemapper final : public PropertyVisitor {
public:
    explicit EntityRefRemapper(const std::unordered_map<Uuid, Uuid>& remap) : remap_(remap) {}

    bool field(const Meta&, float&, const FloatRange&) override { return false; }
    bool field(const Meta&, int&, int, int) override { return false; }
    bool field(const Meta&, bool&) override { return false; }
    bool field(const Meta&, std::string&) override { return false; }
    bool field(const Meta&, Vec3&, Vec3Kind) override { return false; }
    bool field(const Meta&, core::Vec2&, float) override { return false; }
    bool enumeration(const Meta&, int&, std::span<const char* const>) override { return false; }
    bool asset(const Meta&, assets::AssetRef&, assets::AssetType) override { return false; }
    bool layerMask(const Meta&, std::uint32_t&) override { return false; }
    bool entity(const Meta&, Uuid& ref) override {
        if (!ref.valid()) return false;
        const auto it = remap_.find(ref);
        if (it == remap_.end() || it->second == ref) return false;
        ref = it->second;
        return true;
    }
    // Entra en las listas sin cambiar su tamano.
    bool beginList(const Meta&, std::size_t&) override { return true; }

private:
    const std::unordered_map<Uuid, Uuid>& remap_;
};
}  // namespace

void World::remapEntityRefs(Entity entity, const std::unordered_map<Uuid, Uuid>& remap) {
    if (!entity.valid() || entity.world() != this || remap.empty()) {
        return;
    }
    EntityRefRemapper remapper(remap);
    for (const ComponentType& type : ComponentRegistry::instance().types()) {
        if (type.reflect && type.has(*this, entity.handle())) {
            type.reflect(*this, entity.handle(), remapper);
        }
    }
}

void World::clear() {
    registry_.clear();
    roots_.clear();
    uuid_map_.clear();
    origin_ = DVec3{};
    ++structure_version_;
}

void World::clearPersistent(const DVec3& new_origin) {
    std::vector<entt::entity> doomed;
    for (const entt::entity root : roots_) {
        if (!registry_.get<EntityInfo>(root).transient) doomed.push_back(root);
    }
    if (doomed.size() == roots_.size()) {
        clear();
        origin_ = new_origin;
        return;
    }
    for (const entt::entity root : doomed) destroy(wrap(root));
    // Las que se quedan (teselas) siguen en el mismo sitio del mundo real.
    rebaseOrigin(new_origin);
    ++structure_version_;
}

void World::rebaseOrigin(const DVec3& origin) {
    // Diferencia en doble precision (los origenes pueden estar a 100 km):
    // solo el resultado, pequeno, pasa a float.
    const core::Vec3 delta{static_cast<float>(origin_.x - origin.x), static_cast<float>(origin_.y - origin.y),
                           static_cast<float>(origin_.z - origin.z)};
    origin_ = origin;
    if (delta.x == 0.0f && delta.y == 0.0f && delta.z == 0.0f) return;
    for (const entt::entity root : roots_) {
        Entity e = wrap(root);
        e.setLocalPosition(e.localPosition() + delta);
    }
}

void World::shiftOrigin(const core::Vec3& offset) {
    if (offset.x == 0.0f && offset.y == 0.0f && offset.z == 0.0f) return;
    // Solo las raices: los hijos son relativos a su padre.
    for (const entt::entity root : roots_) {
        Entity e = wrap(root);
        e.setLocalPosition(e.localPosition() - offset);
    }
    origin_.x += offset.x;
    origin_.y += offset.y;
    origin_.z += offset.z;
}

Entity World::find(const Uuid& uuid) const {
    const auto it = uuid_map_.find(uuid);
    return it == uuid_map_.end() ? Entity{} : wrap(it->second);
}

Entity World::findWithTag(std::string_view tag) const {
    Entity found;
    forEachDepthFirst([&](Entity e) {
        // La etiqueta antes que subir por la jerarquia (casi nadie la tiene).
        if (!found.valid() && e.compareTag(tag) && e.activeInHierarchy()) found = e;
    });
    return found;
}

std::vector<Entity> World::findAllWithTag(std::string_view tag) const {
    std::vector<Entity> found;
    forEachDepthFirst([&](Entity e) {
        if (e.compareTag(tag) && e.activeInHierarchy()) found.push_back(e);
    });
    return found;
}

Entity World::findByName(std::string_view name) const {
    Entity found;
    forEachDepthFirst([&](Entity e) {
        if (!found.valid() && e.name() == name) {
            found = e;
        }
    });
    return found;
}

std::vector<entt::entity>& World::siblingsOf(entt::entity entity) {
    const entt::entity parent = registry_.get<Hierarchy>(entity).parent;
    return parent == entt::null ? roots_ : registry_.get<Hierarchy>(parent).children;
}

void World::detach(entt::entity entity) {
    auto& siblings = siblingsOf(entity);
    siblings.erase(std::remove(siblings.begin(), siblings.end(), entity), siblings.end());
    registry_.get<Hierarchy>(entity).parent = entt::null;
    ++structure_version_;
}

void World::attach(entt::entity entity, entt::entity parent, int index) {
    registry_.get<Hierarchy>(entity).parent = parent;
    auto& siblings = parent == entt::null ? roots_ : registry_.get<Hierarchy>(parent).children;
    if (index < 0 || index >= static_cast<int>(siblings.size())) {
        siblings.push_back(entity);
    } else {
        siblings.insert(siblings.begin() + index, entity);
    }
    markTransformDirty(entity);
    ++structure_version_;
}

// Marca la entidad y todo su subarbol: sus matrices de mundo dependen de
// esta. Si ya estaba sucia, sus hijos tambien (se marcaron entonces).
void World::markTransformDirty(entt::entity entity) {
    std::vector<entt::entity> stack{entity};
    bool first = true;
    while (!stack.empty()) {
        const entt::entity e = stack.back();
        stack.pop_back();
        Transform& t = registry_.get<Transform>(e);
        ++t.version;
        if (t.dirty && !first) {
            continue;
        }
        t.dirty = true;
        first = false;
        const Hierarchy& h = registry_.get<Hierarchy>(e);
        stack.insert(stack.end(), h.children.begin(), h.children.end());
    }
}

const Mat4& World::worldMatrix(entt::entity entity) const {
    return computeWorld(entity);
}

const Mat4& World::computeWorld(entt::entity entity) const {
    const Transform& t = registry_.get<Transform>(entity);
    if (!t.dirty) {
        return t.world_matrix;
    }
    t.local_matrix = core::composeTrs(t.position, t.rotation, t.scale);
    const entt::entity parent = registry_.get<Hierarchy>(entity).parent;
    t.world_matrix = parent == entt::null ? t.local_matrix : computeWorld(parent) * t.local_matrix;
    t.dirty = false;
    return t.world_matrix;
}

}  // namespace cramion::ecs
