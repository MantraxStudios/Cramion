// Contrato B (VideoOutput.h) sobre el renderizador de verdad: las texturas de
// video son Render Textures del VulkanRenderer (mismo espacio de ids), los
// frames van por submitVideoFrame (bufer pendiente; la GPU los recibe en el
// siguiente frame grabado), el Material Override por RenderSync y los planos
// de camara por setVideoPlane.

#include "CramionCore/video/VideoOutput.h"

#include "CramionCore/ecs/RenderSync.h"

#include <CramionFX/vk/VulkanRenderer.h>

namespace cramion::video {

namespace {

gfx::VideoUpload::Layout uploadLayout(PixelLayout layout) {
    switch (layout) {
        case PixelLayout::Rgba8: return gfx::VideoUpload::Layout::Rgba8;
        case PixelLayout::Bgra8: return gfx::VideoUpload::Layout::Bgra8;
        case PixelLayout::Nv12:
        default: return gfx::VideoUpload::Layout::Nv12;
    }
}

class RenderVideoOutput final : public VideoOutput {
public:
    RenderVideoOutput(ecs::RenderSync& sync, gfx::VulkanRenderer& renderer) : sync_(sync), renderer_(renderer) {}

    std::int32_t createTexture(std::uint32_t width, std::uint32_t height) override {
        if (width == 0 || height == 0) return -1;
        return renderer_.createVideoTexture(width, height);
    }

    void destroyTexture(std::int32_t id) override {
        if (id < 0) return;
        renderer_.destroyVideoTexture(id);
    }

    std::int32_t textureForRenderTexture(const Uuid& crrt) override {
        if (!crrt.valid()) return -1;
        return sync_.renderTextureId(crrt);
    }

    // El tamano actual (una .crrt editada en el Inspector cambia de tamano).
    std::uint32_t textureWidth(std::int32_t id) const override { return renderer_.renderTextureExtent(id).width; }
    std::uint32_t textureHeight(std::int32_t id) const override { return renderer_.renderTextureExtent(id).height; }

    bool submitFrame(std::int32_t id, const VideoFrame& frame, const ConvertParams& params) override {
        if (id < 0 || frame.width == 0 || frame.height == 0 || frame.planes[0] == nullptr) return false;
        gfx::VideoUpload upload;
        upload.layout = uploadLayout(frame.layout);
        upload.width = frame.width;
        upload.height = frame.height;
        upload.planes[0] = frame.planes[0];
        upload.planes[1] = frame.planes[1];
        upload.strides[0] = frame.strides[0];
        upload.strides[1] = frame.strides[1];
        upload.matrix = frame.color.matrix == 601 ? 601 : 709;
        upload.full_range = frame.color.full_range;
        upload.aspect = static_cast<int>(params.aspect);
        upload.alpha = params.alpha;
        // Solo API Only la pide (VideoOutput.h): las Render Textures son sRGB.
        upload.linear_out = params.linear_out;
        // Copia ya (frame.owner puede soltar los planos al volver).
        return renderer_.submitVideoFrame(id, upload);
    }

    void setMaterialOverride(entt::entity renderer, const MaterialOverride& o) override {
        if (o.texture < 0) {
            sync_.clearVideoOverride(renderer);
            return;
        }
        sync_.setVideoOverride(renderer, o.slot, static_cast<int>(o.property), o.texture, o.emissive_intensity);
    }

    void clearMaterialOverride(entt::entity renderer) override { sync_.clearVideoOverride(renderer); }

    void releaseTexture(std::int32_t id) override { renderer_.releaseVideoFrames(id); }

    void setCameraPlane(bool near_plane, std::int32_t id, float alpha, AspectMode aspect) override {
        renderer_.setVideoPlane(near_plane ? 1 : 0, id, alpha, static_cast<int>(aspect));
    }

private:
    ecs::RenderSync& sync_;
    gfx::VulkanRenderer& renderer_;
};

}  // namespace

std::unique_ptr<VideoOutput> makeRenderVideoOutput(ecs::RenderSync& sync, gfx::VulkanRenderer& renderer) {
    return std::make_unique<RenderVideoOutput>(sync, renderer);
}

}  // namespace cramion::video
