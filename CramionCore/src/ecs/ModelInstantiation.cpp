#include "CramionCore/ecs/ModelInstantiation.h"

#include "CramionCore/ecs/MathUtil.h"

#include <vector>

namespace cramion::ecs {

Entity instantiateModel(World& world, const assets::ModelAsset& model, Entity parent) {
    if (model.nodes.empty()) {
        // Sin jerarquia: una entidad por pieza bajo una raiz.
        Entity root = world.create(model.name, parent);
        for (std::size_t i = 0; i < model.parts.size(); ++i) {
            Entity target = model.parts.size() == 1 ? root : world.create(model.name, root);
            MeshRenderer& mr = target.add<MeshRenderer>();
            mr.model = assets::AssetRef{model.uuid, assets::AssetType::Model};
            mr.part = static_cast<int>(i);
            if (model.animated) {
                target.add<Animator>();
            }
        }
        return root;
    }

    std::vector<Entity> created(model.nodes.size());
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const assets::ModelNode& node = model.nodes[i];
        Entity node_parent = parent;
        if (node.parent >= 0 && static_cast<std::size_t>(node.parent) < i) {
            node_parent = created[static_cast<std::size_t>(node.parent)];
        }
        // La raiz se llama como el asset (como el prefab de un FBX en Unity).
        const std::string& name = i == 0 ? model.name : node.name;
        Entity e = world.create(name.empty() ? std::string("Nodo") : name, node_parent);
        core::Vec3 position{};
        core::Quat rotation{};
        core::Vec3 scale{1.0f, 1.0f, 1.0f};
        decomposeMatrix(node.local, position, rotation, scale);
        e.setLocalTrs(position, rotation, scale);
        if (node.part >= 0) {
            MeshRenderer& mr = e.add<MeshRenderer>();
            mr.model = assets::AssetRef{model.uuid, assets::AssetType::Model};
            mr.part = node.part;
            if (model.animated) {
                e.add<Animator>();
            }
        }
        created[i] = e;
    }
    return created.front();
}

Entity createPrimitive(World& world, const Uuid& builtin, std::string name, Entity parent) {
    Entity e = world.create(std::move(name), parent);
    MeshRenderer& mr = e.add<MeshRenderer>();
    mr.model = assets::AssetRef{builtin, assets::AssetType::Model};
    mr.part = 0;
    return e;
}

Entity createEmpty(World& world, Entity parent) {
    return world.create("GameObject", parent);
}

Entity createLight(World& world, LightType type, Entity parent) {
    static constexpr const char* kNames[] = {"Luz direccional", "Luz puntual", "Foco"};
    Entity e = world.create(kNames[static_cast<int>(type)], parent);
    Light& light = e.add<Light>();
    light.type = type;
    if (type == LightType::Directional) {
        light.color = core::Vec3{1.0f, 1.0f, 1.0f};
        light.intensity = 1.0f;
        // Sol de media manana: rayos hacia abajo y en diagonal.
        e.setLocalEulerDegrees(core::Vec3{-50.0f, -30.0f, 0.0f});
    } else if (type == LightType::Spot) {
        light.intensity = 25.0f;
        light.range = 40.0f;
        light.color = core::Vec3{1.0f, 1.0f, 0.95f};
        e.setLocalEulerDegrees(core::Vec3{-90.0f, 0.0f, 0.0f});  // hacia abajo
    }
    return e;
}

Entity createCamera(World& world, Entity parent) {
    Entity e = world.create("Main Camera", parent);
    e.add<Camera>();
    e.setLocalPosition(core::Vec3{0.0f, 1.7f, 6.0f});
    return e;
}

void populateDefaultScene(World& world) {
    createCamera(world);
    createLight(world, LightType::Directional);
    Entity environment = world.create("Entorno");
    environment.add<Sky>();
    environment.add<PostProcessing>();
}

}  // namespace cramion::ecs
