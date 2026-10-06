#include "CramionCore/environment/FogVolume.h"

#include "CramionCore/ecs/World.h"

#include <CramionFX/vk/GpuTypes.h>
#include <CramionFX/vk/VulkanRenderer.h>

#include <algorithm>
#include <array>
#include <vector>

namespace cramion::environment {

void FogVolume::reflect(ecs::PropertyVisitor& v) {
    static constexpr std::array<const char*, 2> kShapes = {"Caja", "Esfera"};
    v.field({"enabled", "Activo"}, enabled);
    v.enumeration({"shape", "Forma", "La caja mide 1 m (la esfera 1 m de diametro): se escala con el Transform"}, shape,
                  kShapes);
    v.field({"color", "Color", "Color de la luz que dispersa la niebla"}, color, ecs::Vec3Kind::Color);
    v.field({"density", "Densidad", "Cuanto tapa por metro (0.1: a 10 m tapa ~60 %)"}, density,
            ecs::FloatRange{0.0f, 5.0f, 0.001f, "%.3f"});
    v.field({"edge_falloff", "Borde suave"}, edge_falloff, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
    v.field({"noise", "Ruido", "Grumos de niebla que derivan con el tiempo"}, noise,
            ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
    v.field({"noise_scale", "Escala del ruido"}, noise_scale, ecs::FloatRange{0.01f, 5.0f, 0.01f, "%.2f"});
}

void registerFogVolumeComponents() {
    auto& r = ecs::ComponentRegistry::instance();
    if (r.find("FogVolume") == nullptr) r.registerComponent<FogVolume>("FogVolume", "Volumen de niebla", "Entorno");
}

void syncFogVolumes(ecs::World& world, gfx::VulkanRenderer& renderer, const core::Vec3& eye) {
    struct Item {
        float distance;
        gfx::VulkanRenderer::FogVolume volume;
    };
    std::vector<Item> items;
    for (const entt::entity h : world.registry().view<FogVolume>()) {
        const ecs::Entity e = world.wrap(h);
        const FogVolume& f = e.get<FogVolume>();
        if (!f.enabled || f.density <= 0.0f || !e.activeInHierarchy()) continue;
        gfx::VulkanRenderer::FogVolume v;
        v.world = e.worldMatrix();
        v.color = f.color;
        v.density = f.density;
        v.shape = f.shape;
        v.edge_falloff = f.edge_falloff;
        v.noise = f.noise;
        v.noise_scale = f.noise_scale;
        const core::Vec3 c{v.world.m[3][0], v.world.m[3][1], v.world.m[3][2]};
        items.push_back(Item{core::length(c - eye), v});
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.distance < b.distance; });
    std::vector<gfx::VulkanRenderer::FogVolume> out;
    for (const Item& i : items) {
        if (static_cast<int>(out.size()) >= gfx::kMaxFogVolumes) break;
        out.push_back(i.volume);
    }
    renderer.setFogVolumes(std::move(out));
}

}  // namespace cramion::environment
