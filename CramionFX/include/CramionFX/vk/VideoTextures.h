#ifndef CRAMION_VK_VIDEO_TEXTURES_H
#define CRAMION_VK_VIDEO_TEXTURES_H

// Video en la GPU (VideoPlayer de CramionCore, por VulkanRenderer):
//
//   - submit(): copia el frame decodificado a un bufer de CPU pendiente por
//     textura (solo el ultimo: si llegan dos antes de grabar, gana el nuevo).
//     No toca la GPU: el staging del hueco de frame lo puede estar leyendo.
//   - recordUploads(): al principio del siguiente frame que se grabe (tras
//     esperar su fence), memcpy al staging persistente de ese hueco de frame,
//     copia a los planos Y (R8) y UV (RG8) internos, video_yuv.comp (matriz
//     BT.601/709, rango, escalado bilineal con el modo de aspecto) a una
//     imagen RGBA8 UNORM interna con storage y copia a la imagen de la Render
//     Texture (UNORM con vista sRGB: no se cambia su creacion). RGBA del mismo
//     tamano: copia directa. BGRA: por el compute (intercambio de canales).
//   - recordPlanes(): los planos de camara (lejano y cercano) sobre la imagen
//     ya compuesta, como la UI en el mundo (video_plane.frag).
//
// Las texturas de destino son Render Textures normales del renderizador (mismo
// espacio de ids): la imagen nunca cambia de tamano por el video; si cambia la
// de la Render Texture (.crrt editada), la imagen interna se rehace al grabar.

#include "CramionFX/vk/ComputePass.h"
#include "CramionFX/vk/FullscreenPass.h"
#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCommon.h"
#include "CramionFX/vk/VulkanImage.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace cramion::gfx {

class VulkanDevice;

// Un frame de video para subir (los planos solo tienen que vivir durante la
// llamada: se copian).
struct VideoUpload {
    enum class Layout : int { Rgba8 = 0, Bgra8, Nv12 } layout = Layout::Nv12;
    std::uint32_t width = 0, height = 0;
    const std::uint8_t* planes[2] = {nullptr, nullptr};  // Nv12: Y, UV; Rgba8/Bgra8: [0]
    std::uint32_t strides[2] = {0, 0};                   // bytes por fila; 0 = sin relleno
    int matrix = 709;          // 601 | 709
    bool full_range = false;
    int aspect = 1;            // video::AspectMode (0 Stretch, 1 FitInside, 2 FitOutside,
                               // 3 FitHorizontally, 4 FitVertically, 5 NoScaling)
    float alpha = 1.0f;
    bool linear_out = false;   // solo API Only (las Render Textures van siempre en sRGB)
};

// Rectangulo del contenido (origen y tamano, en las unidades de dst) de una
// imagen src x src_h dentro de dst_w x dst_h con el modo de aspecto.
struct VideoRect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};
VideoRect videoContentRect(int aspect, float src_w, float src_h, float dst_w, float dst_h);

// La imagen de una Render Texture (la resuelve el renderizador al grabar).
struct VideoTarget {
    vk::Image image{};
    vk::ImageView view{};  // vista UNORM (los bytes con gamma, para los planos)
    vk::Extent2D extent{0, 0};
    bool valid() const { return static_cast<bool>(image) && extent.width > 0 && extent.height > 0; }
};

class VideoTextures {
public:
    using Resolve = std::function<VideoTarget(std::int32_t)>;

    void create(const VulkanDevice& device, vk::Format ldr_format, std::uint32_t frames_in_flight);
    void destroy();
    bool created() const { return created_; }

    // Copia el frame al bufer pendiente de `id` (cualquier Render Texture).
    bool submit(std::int32_t id, const VideoUpload& upload);
    // Lo de `id` fuera (textura de video destruida): pendiente, planos y sets
    // se retiran unos frames despues. Tambien la quita de los planos.
    void forget(std::int32_t id);
    bool hasPending() const;

    // Al principio del command buffer del frame `frame` (fuera de cualquier
    // renderizado), con su fence ya esperada.
    void recordUploads(const VulkanDevice& device, const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                       const Resolve& resolve);

    // Planos de camara: 0 = lejano (detras de la escena), 1 = cercano. id -1 = quitar.
    void setPlane(int plane, std::int32_t id, float alpha, int aspect);
    bool hasPlanes() const { return planes_[0].id >= 0 || planes_[1].id >= 0; }
    // Sobre `color` (layout ShaderReadOnly; vuelve a el), con la profundidad
    // de la escena `depth` (layout de solo lectura, a cualquier resolucion).
    void recordPlanes(const VulkanDevice& device, const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                      const VulkanImage& color, const VulkanImage& depth, vk::ImageLayout depth_layout,
                      const Resolve& resolve);

    // Una vez por frame de pantalla: destruye lo retirado que ya no usa la GPU.
    void collectRetired();

private:
    struct Slot {
        // Pendiente (CPU): planos sin relleno, uno tras otro.
        std::vector<std::uint8_t> data;
        std::size_t offset1 = 0;  // inicio del plano UV
        VideoUpload params{};     // sin punteros
        bool dirty = false;
        // GPU
        VulkanImage plane0, plane1;  // Y (R8) / UV (RG8), o RGBA8 en plane0
        VideoUpload::Layout plane_layout = VideoUpload::Layout::Nv12;
        std::uint32_t plane_width = 0, plane_height = 0;
        VulkanImage output;          // RGBA8 UNORM con storage, tamano del destino
        std::vector<VulkanBuffer> staging;                 // uno por frame en vuelo
        std::vector<vk::raii::DescriptorSet> sets;         // uno por frame en vuelo
    };
    Slot& slotFor(std::int32_t id);
    void retire(std::unique_ptr<Slot> slot);
    void retireImage(VulkanImage& image);

    bool created_ = false;
    std::uint32_t frames_in_flight_ = 2;
    ComputePass convert_;
    FullscreenPass plane_pass_;
    vk::raii::Sampler linear_sampler_{nullptr};
    vk::raii::Sampler nearest_sampler_{nullptr};
    vk::raii::DescriptorPool pool_{nullptr};
    std::vector<vk::raii::DescriptorSet> plane_sets_;  // [frame * 2 + plano]

    mutable std::mutex mutex_;  // submit puede llegar de otro hilo
    std::unordered_map<std::int32_t, std::unique_ptr<Slot>> slots_;

    struct Plane {
        std::int32_t id = -1;
        float alpha = 1.0f;
        int aspect = 1;
    };
    std::array<Plane, 2> planes_{};

    struct Retired {
        std::unique_ptr<Slot> slot;
        VulkanImage image;
        std::uint32_t frames_left = 0;
    };
    std::vector<Retired> retired_;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_VIDEO_TEXTURES_H
