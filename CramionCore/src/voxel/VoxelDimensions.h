#ifndef CRAMION_CORE_VOXEL_DIMENSIONS_H
#define CRAMION_CORE_VOXEL_DIMENSIONS_H

// Dimensiones del mundo de bloques (VoxelSystem::setDimension): la tabla
// (nombre, etiqueta, generador, ambiente) y el ambiente de cada una, que el
// motor aplica a la escena al cambiar (cielo, hora, clima, niebla, sol y el
// mar del motor) y deshace al volver a la superficie.

#include "CramionCore/ecs/Components.h"
#include "CramionCore/environment/Environment.h"
#include "CramionCore/voxel/Voxel.h"
#include "CramionCore/water/Water.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cramion::voxel {

inline constexpr const char* kSurfaceDimension = "superficie";

// La dimension por nombre (nullptr si no existe). `surface` = el generador de
// la superficie (el del VoxelWorld o el del mundo guardado).
const DimensionInfo* findDimension(const std::string& name);
VoxelGenerator dimensionGenerator(const std::string& name, VoxelGenerator surface);
// Capa del Reino Profundo de una dimension: "profundo" 1, "profundo2".."profundo8" 2..8; 0 si no es.
int deepLayerOf(const std::string& name);
// Y al reves: la dimension de la capa (1 -> "profundo").
std::string deepLayerName(int layer);

// Lo que habia en la escena antes de salir de la superficie.
struct AmbienceSnapshot {
    bool taken = false;
    std::optional<std::pair<entt::entity, environment::Environment>> environment;
    std::optional<std::pair<entt::entity, ecs::Sky>> sky;
    std::optional<std::pair<entt::entity, ecs::Light>> sun;
    std::optional<std::pair<entt::entity, ecs::PostProcessing>> post;
    struct Sea {
        entt::entity entity = entt::null;
        bool active = true;
        water::WaterBody body;
    };
    std::vector<Sea> seas;
};

// Aplica el ambiente de `dimension` a la escena (la superficie: lo que habia).
void applyDimensionAmbience(ecs::World& world, const std::string& dimension, AmbienceSnapshot& snapshot);

}  // namespace cramion::voxel

#endif  // CRAMION_CORE_VOXEL_DIMENSIONS_H
