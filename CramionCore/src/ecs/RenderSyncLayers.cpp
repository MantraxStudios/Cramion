// RenderSync: contornos de juego (componente Outline) y la mascara reactiva de
// DLSS (RM-capas). Aparte para no tocar el resto de la sincronizacion mas que
// con una llamada.

#include "CramionCore/ecs/RenderSync.h"

#include "CramionCore/cvar/CVar.h"
#include "CramionCore/ecs/RenderLayers.h"

#include <CramionFX/vk/VulkanRenderer.h>

#include <cmath>

namespace cramion::ecs {

namespace {
// DLSS con la mascara reactiva (Bias Current Color Mask de NGX): la misma que
// ya recibian FSR 3 y el TAA propio (transparentes y reflejos nitidos).
cvar::CVar<bool> g_dlss_reactive_mask("render.DlssReactiveMask", true,
                                     "DLSS: pasar la mascara reactiva (transparentes, particulas, reflejos nitidos) "
                                     "como Bias Current Color Mask (0 = como antes)",
                                     cvar::Saved);
}  // namespace

void RenderSync::syncOutlines(World& world, gfx::VulkanRenderer& renderer, float delta_seconds) {
    renderer.setDlssReactiveMask(g_dlss_reactive_mask.get());
    // Reloj del pulso (la segunda vista del editor llega con 0: no avanza).
    outline_clock_ = std::fmod(outline_clock_ + std::max(delta_seconds, 0.0f), 3600.0f);
    if (world.registry().view<Outline>().empty()) {
        if (!renderer.gameOutlines().empty()) renderer.setGameOutlines({});
        return;
    }
    renderer.setGameOutlines(collectOutlines(world, outline_clock_, [this](Entity e, bool children) {
        if (children) return actorIndicesInSubtree(e);
        std::vector<std::uint32_t> one;
        if (const int index = actorIndex(e); index >= 0) one.push_back(static_cast<std::uint32_t>(index));
        return one;
    }));
}

}  // namespace cramion::ecs
