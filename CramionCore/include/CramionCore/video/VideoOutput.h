#ifndef CRAMION_VIDEO_OUTPUT_H
#define CRAMION_VIDEO_OUTPUT_H

// Contrato B. Lo usa WP1 (VideoSystem); lo implementan WP4
// (src/video/RenderVideoOutput.cpp) y las pruebas con un objeto falso.

#include "CramionCore/video/VideoDecoder.h"
#include "CramionCore/video/VideoPlayer.h"
#include "CramionCore/Uuid.h"
#include <entt/entity/entity.hpp>
#include <cstdint>
#include <memory>

namespace cramion::ecs { class RenderSync; }
namespace cramion::gfx { class VulkanRenderer; }

namespace cramion::video {

struct ConvertParams {
    AspectMode aspect = AspectMode::FitInside;
    float alpha = 1.0f;
    // Solo tiene efecto en RenderMode::ApiOnly. Render Texture, Material
    // Override, planos de camara y UI se escriben siempre en sRGB (el slot es
    // UNORM con vista sRGB: datos lineales se decodificarian dos veces).
    bool linear_out = false;   // ColorOutput::Linear
};

struct MaterialOverride {
    int slot = -1;                                  // -1 = todos los huecos
    MaterialProperty property = MaterialProperty::Emissive;
    std::int32_t texture = -1;
    // No entra en la clave de variante de RenderSync (se cuantiza o se aplica aparte).
    float emissive_intensity = 1.0f;
};

class VideoOutput {
public:
    virtual ~VideoOutput() = default;
    virtual std::int32_t createTexture(std::uint32_t width, std::uint32_t height) = 0;  // -1 si falla
    virtual void destroyTexture(std::int32_t id) = 0;
    virtual std::int32_t textureForRenderTexture(const Uuid& crrt) = 0;                 // -1 si no existe
    // Tamano actual (una .crrt cambia de tamano si se edita en el Inspector).
    virtual std::uint32_t textureWidth(std::int32_t id) const = 0;
    virtual std::uint32_t textureHeight(std::int32_t id) const = 0;
    // Copia los planos en el acto a un bufer de CPU pendiente por id (solo se
    // guarda el ultimo, nunca se acumulan); se suben al staging de
    // current_frame_ dentro del siguiente frame que se grabe, despues de su fence.
    virtual bool submitFrame(std::int32_t id, const VideoFrame& frame, const ConvertParams& params) = 0;
    virtual void setMaterialOverride(entt::entity renderer, const MaterialOverride& o) = 0;
    virtual void clearMaterialOverride(entt::entity renderer) = 0;
    virtual void setCameraPlane(bool near_plane, std::int32_t id, float alpha, AspectMode aspect) = 0;  // id -1 = quitar
    // Se deja de escribir en una textura ajena (.crrt, vista previa) que sigue
    // viva: suelta lo interno de la subida (planos, pendiente). Por defecto nada.
    virtual void releaseTexture(std::int32_t /*id*/) {}
    // Sin uiTexture: ImGuiLayer vive en el editor, que usa
    // imgui_.renderTexture(videoSystem.previewTexture()).
};

std::unique_ptr<VideoOutput> makeRenderVideoOutput(ecs::RenderSync& sync, gfx::VulkanRenderer& renderer);

}  // namespace cramion::video
#endif
