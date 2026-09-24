#ifndef CRAMION_CORE_PHYSICS_SETTINGS_H
#define CRAMION_CORE_PHYSICS_SETTINGS_H

// Ajustes de fisica del proyecto (como Project Settings > Physics de Unity):
// gravedad, paso fijo, las 32 capas con nombre y la matriz de colisiones
// entre capas. Se guardan en ProjectSettings/Physics.json.
//
// La capa de cada entidad es EntityInfo::layer (0..31). Dos cuerpos chocan
// (y se detectan como trigger) solo si sus capas colisionan en la matriz.
// Las consultas (raycast, overlap) y las particulas filtran con una mascara
// de capas (bit i = capa i), como el LayerMask de Unity.

#include <CramionFX/core/Math.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>

namespace cramion::physics {

inline constexpr int kLayerCount = 32;
inline constexpr std::uint32_t kAllLayers = 0xFFFFFFFFu;

// Capas integradas (las de Unity).
inline constexpr int kLayerDefault = 0;
inline constexpr int kLayerTransparentFX = 1;
inline constexpr int kLayerIgnoreRaycast = 2;
inline constexpr int kLayerWater = 4;
inline constexpr int kLayerUI = 5;

// Mascara por defecto de las consultas: todas menos "Ignore Raycast".
inline constexpr std::uint32_t kDefaultRaycastLayers = ~(1u << kLayerIgnoreRaycast);

constexpr std::uint32_t layerBit(int layer) {
    return layer >= 0 && layer < kLayerCount ? (1u << static_cast<unsigned>(layer)) : 0u;
}

struct PhysicsSettings {
    core::Vec3 gravity{0.0f, -9.81f, 0.0f};
    float fixed_step = 1.0f / 60.0f;  // segundos por paso de simulacion
    int max_substeps = 4;             // pasos por frame como mucho (si no, se ralentiza)
    int collision_steps = 1;          // subdivisiones de colision por paso (mas = mas estable)
    bool queries_hit_triggers = true; // los raycast tocan triggers salvo que se pida lo contrario
    float sleep_threshold = 0.05f;    // m/s por debajo de la cual un cuerpo se duerme
    std::uint32_t max_bodies = 65536;
    std::uint32_t worker_threads = 0;  // 0 = nucleos - 1

    std::array<std::string, kLayerCount> layer_names{};
    // layer_collisions[i] bit j = las capas i y j colisionan (simetrica).
    std::array<std::uint32_t, kLayerCount> layer_collisions{};

    PhysicsSettings();

    bool layersCollide(int a, int b) const;
    void setLayersCollide(int a, int b, bool collide);
    // Indice de la capa con ese nombre, o -1.
    int layerIndex(std::string_view name) const;
    // Mascara de las capas con esos nombres (los que no existen se ignoran).
    std::uint32_t mask(std::initializer_list<std::string_view> names) const;
    // Nombre visible: el suyo o "Capa N" si no tiene.
    std::string layerLabel(int layer) const;
};

// Lee/guarda el JSON. load() deja los valores por defecto en lo que falte;
// false si el archivo no existe o no se pudo leer.
bool loadPhysicsSettings(const std::filesystem::path& file, PhysicsSettings& settings);
bool savePhysicsSettings(const std::filesystem::path& file, const PhysicsSettings& settings);

// Ajustes del proyecto abierto: los usa el Inspector para los nombres de las
// capas (el editor los actualiza al abrir un proyecto o editarlos).
const PhysicsSettings& projectPhysicsSettings();
void setProjectPhysicsSettings(const PhysicsSettings& settings);

}  // namespace cramion::physics

#endif  // CRAMION_CORE_PHYSICS_SETTINGS_H
