// RenderSync: el lightmap horneado de cada actor (PLAN-LIGHTMAPS.md 2.7).
// Aparte para no tocar el bucle de los actores mas que con una llamada.
//
// Un actor usa su registro del .crbake (pagina y rectangulo del atlas) si: el
// renderizador tiene lightmaps, el registro es de su entidad, de su modelo y
// pieza, su Escala en lightmap no es 0 y la malla que se dibuja lleva la
// MISMA disposicion de UV2 con que se horneo (lightmap_layout_hash). Con
// materiales sustituidos se dibuja la variante, que copia la UV2 del original.

#include "CramionCore/ecs/RenderSync.h"

#include <algorithm>
#include <cmath>

namespace cramion::ecs {

namespace {

core::Vec3 column(const core::Mat4& m, int c) { return core::Vec3{m.m[c][0], m.m[c][1], m.m[c][2]}; }

// La entidad se ha movido desde el horneado (mas de 1 mm o 0.1 grados, o su
// escala)? `offset`: lo que se desplazo el origen del mundo desde entonces.
bool movedSinceBake(const core::Mat4& now, const std::array<float, 12>& baked, const core::Vec3& offset) {
    constexpr float kCosTolerance = 0.9999985f;  // cos(0.1 grados)
    for (int c = 0; c < 3; ++c) {
        const core::Vec3 a = column(now, c);
        const core::Vec3 b{baked[static_cast<std::size_t>(c * 3)], baked[static_cast<std::size_t>(c * 3 + 1)],
                           baked[static_cast<std::size_t>(c * 3 + 2)]};
        const float la = core::length(a), lb = core::length(b);
        if (std::fabs(la - lb) > 1e-3f * std::max(la, lb)) return true;
        if (la > 1e-8f && lb > 1e-8f && core::dot(a, b) / (la * lb) < kCosTolerance) return true;
    }
    const core::Vec3 t = column(now, 3);
    const core::Vec3 tb = core::Vec3{baked[9], baked[10], baked[11]} - offset;
    return core::length(t - tb) > 1e-3f;
}

}  // namespace

void RenderSync::assignLightmap(scene::Actor& actor, Entity e, const MeshRenderer& renderer_component,
                                const asset::ModelData& data, gfx::VulkanRenderer& renderer) {
    // Primer actor de un frame nuevo: lo contado en el anterior.
    if (lightmap_frame_ != frame_) {
        stale_lightmaps_ = stale_counting_;
        lightmapped_actors_ = lightmapped_counting_;
        stale_counting_ = 0;
        lightmapped_counting_ = 0;
        lightmap_frame_ = frame_;
    }
    actor.lightmap_layer = -1;
    actor.lightmap_scale_offset = core::Vec4{1.0f, 1.0f, 0.0f, 0.0f};
    const gfx::BakedLighting& baked = renderer.bakedLighting();
    if (renderer.bakedGeneration() != lightmap_generation_) {
        lightmap_index_.clear();
        for (std::uint32_t i = 0; i < baked.lightmaps.records.size(); ++i) {
            const gfx::LightmapRecord& r = baked.lightmaps.records[i];
            lightmap_index_[Uuid{r.entity[0], r.entity[1]}] = i;
        }
        lightmap_generation_ = renderer.bakedGeneration();
    }
    if (lightmap_index_.empty() || renderer_component.lightmap_scale <= 0.0f || renderer_component.mesh ||
        !renderer_component.model.valid() || data.lightmap_uvs.empty() || data.lightmap_layout_hash == 0) {
        return;
    }
    const auto it = lightmap_index_.find(e.uuid());
    if (it == lightmap_index_.end() || it->second >= baked.lightmaps.records.size()) return;
    const gfx::LightmapRecord& r = baked.lightmaps.records[it->second];
    const Uuid& model = renderer_component.model.uuid;
    if (r.model[0] != model.high || r.model[1] != model.low || r.part != renderer_component.part ||
        r.layout_hash != data.lightmap_layout_hash || r.page == gfx::kLightmapNone) {
        return;
    }
    actor.lightmap_layer = static_cast<std::int32_t>(r.page);
    actor.lightmap_scale_offset = r.scale_offset;
    ++lightmapped_counting_;
    // Se sigue usando aunque se haya movido (como Unity), con un aviso.
    const auto& origin = e.world()->origin();
    const core::Vec3 offset{static_cast<float>(origin.x - baked.world_origin[0]), static_cast<float>(origin.y - baked.world_origin[1]),
                            static_cast<float>(origin.z - baked.world_origin[2])};
    if (movedSinceBake(actor.transform, r.world, offset)) ++stale_counting_;
}

}  // namespace cramion::ecs
