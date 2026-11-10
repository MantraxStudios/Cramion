#ifndef CRAMION_VK_VOXEL_OCCUPANCY_H
#define CRAMION_VK_VOXEL_OCCUPANCY_H

#include "CramionFX/core/Math.h"
#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCommon.h"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace cramion::gfx {

class VulkanDevice;

// Ocupacion de los bloques alrededor de la camara para los rayos (sombras,
// GI, reflejos y sondas por trazado de rayos: rt_common.glsl,
// voxel_occupancy.glsl).
//
// Los bloques no estan en la escena de rayos (TLAS): sus mallas cambian al
// romper y poner bloques y son demasiadas para una BLAS cada una. Sin esto,
// los rayos de sombra de las luces locales y del sol, la GI y la visibilidad
// del cielo atravesaban el terreno de bloques (luz dentro de las cuevas, de
// las casas al atardecer, de una antorcha al otro lado de una pared). Aqui se
// guarda, por celda de 1 m, si hay un bloque opaco y su color (6x7x6 niveles
// en sRGB, para la luz rebotada), y los shaders la recorren con un DDA en dos
// niveles (ladrillos vacios de 16^3 de un salto). Los rayos chocan con la
// celda entera venga de donde venga: proyecta por las dos caras, como en
// Unreal, aunque el mallado solo tenga la cara que da al aire.
//
// Se rellena con las mismas mallas que dibuja VoxelPass (cada cara dice que
// celda solida tiene detras): basta la corteza (las celdas junto al aire),
// porque un rayo que entra en lo solido pasa siempre por una de ellas. No
// entran el agua, las plantas en cruz, los bloques finos (vallas, faroles) ni
// las capas con huecos (hojas, cristal), que siguen con los mapas de sombras.
//
// En la GPU: una ventana de kDim ladrillos de 16^3 alrededor de la camara
// (320 x 256 x 320 m; en vertical por defecto ~192 m por encima y el resto por debajo, setUpReach), en un anillo (el ladrillo (x, y, z) va al hueco
// (x, y, z) mod kDim), con la etiqueta de que ladrillo hay en cada hueco. Se
// sube poco a poco (los mas cercanos primero) al moverse o al cambiar bloques.
class VoxelOccupancy {
public:
    static constexpr std::int32_t kBrick = 16;
    static constexpr std::array<std::int32_t, 3> kDim = {20, 16, 20};
    static constexpr std::uint32_t kSlots = 20u * 16u * 20u;
    static constexpr std::uint32_t kBrickBytes = 16u * 16u * 16u;  // 1 byte por celda
    // Cabecera del buffer de informacion (debe coincidir con voxel_occupancy.glsl).
    struct GpuHeader {
        std::int32_t anchor[4];        // xyz: celda absoluta de la esquina de la ventana; w: 1 = activa
        float anchor_world[4];         // xyz: esa esquina en el mundo (origen flotante)
        std::int32_t dims[4];          // xyz: ladrillos por eje
        std::int32_t window_min[4];    // xyz: primer ladrillo absoluto de la ventana
    };

    VoxelOccupancy() = default;
    VoxelOccupancy(const VoxelOccupancy&) = delete;
    VoxelOccupancy& operator=(const VoxelOccupancy&) = delete;

    void create(const VulkanDevice& device, std::uint32_t frames_in_flight);
    void destroy();

    // Color medio (lineal) y si es opaca, por capa de textura (VoxelPass::setTextures).
    struct LayerInfo {
        core::Vec3 albedo{0.5f, 0.5f, 0.5f};
        bool opaque = true;
    };
    void setLayers(std::vector<LayerInfo> layers) { layers_ = std::move(layers); }

    // Malla de una seccion (formato de VoxelPass::setSection): `origin` en el
    // mundo actual. Sin caras que tapen, se quita.
    void setSection(std::uint64_t key, const core::Vec3& origin, const std::uint32_t* vertices, std::uint32_t quads);
    void removeSection(std::uint64_t key);
    void clear();
    void shiftOrigin(const core::Vec3& offset);
    // Cuanto mira la ventana hacia arriba desde la camara (m, en ladrillos de
    // 16: de 64 a 224; lo demas, hasta 256, hacia abajo).
    // GraphicsSettings::shadow_height (automatico: 192).
    void setUpReach(float meters) {
        const int bricks = static_cast<int>(meters / 16.0f + 0.5f);
        up_bricks_ = bricks < 4 ? 4 : (bricks > 14 ? 14 : bricks);
    }

    // Sube lo que cambio (ventana alrededor de la camara, ladrillos nuevos)
    // y deja los buffers listos para los shaders de este frame.
    void record(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, const core::Vec3& camera_position);

    bool ready() const { return info_.isValid() && cells_.isValid(); }
    const VulkanBuffer& infoBuffer() const { return info_; }
    const VulkanBuffer& cellBuffer() const { return cells_; }
    // Cambia cuando se crean los buffers (hay que reescribir los descriptores).
    std::uint64_t revision() const { return revision_; }
    std::size_t brickCount() const { return bricks_.size(); }
    std::uint32_t pendingSlots() const { return pending_slots_; }

private:
    struct Brick {
        std::vector<std::uint8_t> cells;  // kBrickBytes
        std::uint64_t mask = 0;           // bloques de 4^3 con alguna celda llena
        std::uint64_t version = 0;
    };
    struct Slot {
        std::array<std::int32_t, 3> brick{0, 0, 0};
        std::uint64_t version = 0;  // 0 = vacio
    };
    static std::uint64_t pack(std::int32_t x, std::int32_t y, std::int32_t z);
    void allocate();

    const VulkanDevice* device_ = nullptr;
    std::uint32_t frames_ = 2;
    VulkanBuffer info_;   // cabecera + etiquetas (ivec4 por hueco)
    VulkanBuffer cells_;  // kSlots * kBrickBytes
    std::vector<VulkanBuffer> staging_;
    std::uint64_t revision_ = 0;
    std::uint64_t next_version_ = 1;
    std::uint32_t pending_slots_ = 0;
    bool tags_initialized_ = false;

    std::vector<LayerInfo> layers_;
    std::unordered_map<std::uint64_t, Brick> bricks_;            // por ladrillo absoluto
    std::unordered_map<std::uint64_t, std::uint64_t> sections_;  // clave de seccion -> ladrillo
    std::vector<Slot> slots_;                                    // lo que hay en la GPU
    // Etiqueta de un hueco (voxel_occupancy.glsl, VoxelSlotTag): ladrillo,
    // cargado y la mascara de bloques de 4^3.
    struct Tag {
        std::int32_t brick[3] = {0, 0, 0};
        std::int32_t loaded = 0;
        std::uint32_t mask[2] = {0u, 0u};
        std::uint32_t pad[2] = {0u, 0u};
    };
    std::vector<Tag> tags_;                                      // espejo de las etiquetas
    // Origen flotante: lo desplazado en total (mundo absoluto = mundo + shift).
    double shift_[3] = {0.0, 0.0, 0.0};
    std::int32_t up_bricks_ = 12;  // ladrillos de la ventana por encima del de la camara
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_VOXEL_OCCUPANCY_H
