#ifndef CRAMION_VK_DECAL_ATLAS_H
#define CRAMION_VK_DECAL_ATLAS_H

// Atlas de las texturas de los decals: una imagen 2D array RGBA8 UNORM con
// mips (capa = una imagen: color, normal, ORM o emision), vista SIEMPRE de
// array (gbuffer_surface.glsl la lee como sampler2DArray en el binding 4 del
// set de la geometria). Cada imagen se escala en la GPU (blit lineal) al lado
// de la capa: la caja del decal ya da la proporcion. Asi un decal no gasta
// una ranura de textura del shader por canal y la capa se puede indexar con
// un indice distinto en cada pixel (los decals por teselas).
//
// Crece al doble (4, 8, 16...) hasta el tope: copia las capas que ya tenia y
// la imagen anterior vive hasta que acaba ese envio; quien la tenga en un set
// lo reescribe (VulkanRenderer: decal_sets_dirty_). Sin
// ninguna imagen, un ficticio blanco de 1x1x1 (el binding siempre es valido).

#include "CramionFX/vk/GpuMemoryStats.h"
#include "CramionFX/vk/VulkanCommon.h"

#include <cstdint>
#include <memory>

namespace cramion::gfx {

class VulkanDevice;

class DecalAtlas {
public:
    // `layer_size`: lado de cada capa (potencia de 2); `max_layers`: tope (se
    // recorta a maxImageArrayLayers). Deja el ficticio.
    void create(const VulkanDevice& device, std::uint32_t layer_size, std::uint32_t max_layers);
    void destroy();

    // Sube una imagen RGBA8 en la siguiente capa libre y devuelve su indice
    // (-1 si no quedan capas). Hilo principal, al empezar un frame (antes de
    // grabarlo): se envia sin esperar y la cola lo ordena delante de ese
    // frame. Si el array crece, view() cambia: quien lo tenga en un set debe
    // reescribirlo antes de su siguiente frame.
    int add(const VulkanDevice& device, std::uint32_t width, std::uint32_t height, const std::uint8_t* rgba);

    vk::ImageView view() const;
    std::uint32_t layers() const { return used_; }
    std::uint32_t capacity() const { return capacity_; }
    std::uint32_t maxLayers() const { return max_layers_; }
    std::uint32_t layerSize() const { return layer_size_; }

private:
    // La imagen con su memoria y su vista (se retira entera al crecer).
    struct Storage {
        GpuMemoryTicket ticket;
        vk::raii::DeviceMemory memory{nullptr};
        vk::raii::Image image{nullptr};
        vk::raii::ImageView view{nullptr};
    };
    std::shared_ptr<Storage> createStorage(const VulkanDevice& device, std::uint32_t size, std::uint32_t levels,
                                           std::uint32_t layers) const;

    std::shared_ptr<Storage> storage_;
    std::uint32_t layer_size_ = 1024;
    std::uint32_t levels_ = 1;
    std::uint32_t capacity_ = 0;  // 0 = el ficticio
    std::uint32_t used_ = 0;
    std::uint32_t max_layers_ = 64;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_DECAL_ATLAS_H
