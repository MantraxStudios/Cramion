// Capas de render, viewmodel y contornos de juego (RM-capas): ver
// CramionCore/ecs/RenderLayers.h.

#include "CramionCore/ecs/RenderLayers.h"

#include "CramionCore/ecs/World.h"
#include "CramionCore/physics/PhysicsSettings.h"

#include <algorithm>

namespace cramion::ecs {

namespace {
constexpr float kDegToRad = core::kPi / 180.0f;
}

void applyCameraLayers(const Camera& camera, scene::Camera& view) {
    view.setCullingMask(camera.culling_mask);
    scene::Camera::Viewmodel vm{};
    vm.layers = camera.viewmodel_layers;
    vm.fov_y = std::clamp(camera.viewmodel_fov, 10.0f, 150.0f) * kDegToRad;
    vm.near_plane = std::clamp(camera.viewmodel_near, 0.001f, 1.0f);
    vm.depth_range = std::clamp(camera.viewmodel_depth, 0.0005f, 0.5f);
    view.setViewmodel(vm);
}

scene::OutlineStyle outlineStyle(const Outline& outline, float time_seconds) {
    scene::OutlineStyle style{};
    style.color = core::Vec3{std::clamp(outline.color.x, 0.0f, 1.0f), std::clamp(outline.color.y, 0.0f, 1.0f),
                             std::clamp(outline.color.z, 0.0f, 1.0f)};
    style.alpha = std::clamp(outline.opacity, 0.0f, 1.0f) *
                  scene::outlinePulse(time_seconds, outline.pulse_speed, outline.pulse_min);
    style.width = std::clamp(outline.width, 0.5f, 15.0f);
    style.mode = static_cast<scene::OutlineMode>(std::clamp(static_cast<int>(outline.mode), 0, 2));
    style.occluded_fill = std::clamp(outline.occluded_fill, 0.0f, 1.0f);
    return style;
}

scene::GameOutlines collectOutlines(World& world, float time_seconds,
                                    const std::function<std::vector<std::uint32_t>(Entity, bool)>& actors_of) {
    scene::GameOutlines result;
    for (const entt::entity handle : world.registry().view<Outline>()) {
        const Outline& outline = world.registry().get<Outline>(handle);
        if (!outline.enabled || outline.opacity <= 0.0f) continue;
        const Entity e = world.wrap(handle);
        if (!e.activeInHierarchy()) continue;
        const std::vector<std::uint32_t> actors = actors_of(e, outline.include_children);
        if (actors.empty()) continue;
        const std::uint32_t style = result.addStyle(outlineStyle(outline, time_seconds));
        for (const std::uint32_t actor : actors) result.actors.push_back(scene::OutlinedActor{actor, style});
    }
    if (result.actors.empty()) result.styles.clear();
    return result;
}

std::uint32_t renderLayerMask(std::initializer_list<std::string_view> names) {
    return physics::projectPhysicsSettings().mask(names);
}

}  // namespace cramion::ecs
