#ifndef CRAMION_CORE_TERRAIN_H
#define CRAMION_CORE_TERRAIN_H

// Terreno de mapa de alturas (como el Landscape de Unreal / Terrain de Unity):
//
//   Terrain       el componente: tamano, altura maxima, capas de textura y el
//                 archivo de datos (.crterrain en Assets). Su entidad marca la
//                 esquina (x, z minimas) y la altura 0; no gira ni escala.
//   TerrainData   las alturas (0..1) y los pesos de hasta 8 capas (dos mapas
//                 RGBA8), con las regiones cambiadas para subir solo eso.
//   TerrainStore  carga, crea y guarda los datos por ruta.
//
// Herramientas (TerrainTools): esculpir, suavizar, aplanar, rampa, ruido,
// erosion termica e hidraulica, terrazas, pintar capas y generar un relieve.

#include "CramionCore/ecs/Reflection.h"

#include <CramionFX/core/Math.h>

#include <array>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace cramion::ecs {
class Entity;
}

namespace cramion::terrain {

inline constexpr int kMaxLayers = 8;

struct TerrainLayer {
    std::string name = "Capa";
    std::string albedo;  // imagen en Assets (vacia = solo el color)
    std::string normal;  // normal map en Assets (opcional)
    core::Vec3 tint{1.0f, 1.0f, 1.0f};
    float tiling = 8.0f;  // metros por repeticion
    float roughness = 0.9f;
    float metallic = 0.0f;
    float normal_strength = 1.0f;
};

struct Terrain {
    std::string data;  // ruta del .crterrain dentro de Assets
    float size = 256.0f;
    float height = 80.0f;        // altura maxima (el 0..1 de los datos)
    int resolution = 257;        // vertices por lado al crear los datos (2^n + 1)
    int splat_resolution = 512;  // texeles de los pesos por lado
    std::vector<TerrainLayer> layers;
    bool cast_shadows = true;
    bool collision = true;
    float friction = 0.8f;
    float lod_distance = 2.0f;

    Terrain();
    void reflect(ecs::PropertyVisitor& v);
};

// Rectangulo de texeles cambiados (inclusivo).
struct DirtyRegion {
    int x0 = INT_MAX, y0 = INT_MAX, x1 = -1, y1 = -1;
    bool valid() const { return x1 >= x0 && y1 >= y0; }
    void add(int ax0, int ay0, int ax1, int ay1);
    void clear() { *this = DirtyRegion{}; }
};

class TerrainData {
public:
    void create(std::uint32_t resolution, std::uint32_t splat_resolution, float initial_height = 0.0f);
    bool load(const std::filesystem::path& file);
    bool save(const std::filesystem::path& file) const;

    std::uint32_t resolution() const { return resolution_; }
    std::uint32_t splatResolution() const { return splat_resolution_; }
    std::vector<float>& heights() { return heights_; }
    const std::vector<float>& heights() const { return heights_; }
    std::vector<std::uint8_t>& splat0() { return splat0_; }
    std::vector<std::uint8_t>& splat1() { return splat1_; }
    const std::vector<std::uint8_t>& splat0() const { return splat0_; }
    const std::vector<std::uint8_t>& splat1() const { return splat1_; }

    // Altura 0..1 en uv (0..1), bilineal.
    float sample(float u, float v) const;
    // Peso (0..255) de una capa en un texel de los pesos.
    std::uint8_t weight(int layer, std::uint32_t x, std::uint32_t y) const;
    std::uint8_t* weightPtr(int layer, std::uint32_t x, std::uint32_t y);

    // Marcar cambios: regiones para el render, versiones para el resto.
    void markHeights(int x0, int y0, int x1, int y1);
    void markSplat(int x0, int y0, int x1, int y1);
    void markAll();
    DirtyRegion takeDirtyHeights();
    DirtyRegion takeDirtySplat();
    std::uint64_t version() const { return version_; }
    // La colision (Jolt) se rehace cuando sube esta version: el editor la
    // sube al terminar cada trazo (rehacerla en cada toque seria lento).
    std::uint64_t collisionVersion() const { return collision_version_; }
    void commitCollision() { collision_version_ = version_; }
    bool unsaved() const { return unsaved_; }
    void setSaved() const { unsaved_ = false; }

private:
    std::uint32_t resolution_ = 0;
    std::uint32_t splat_resolution_ = 0;
    std::vector<float> heights_;
    std::vector<std::uint8_t> splat0_;
    std::vector<std::uint8_t> splat1_;
    DirtyRegion dirty_heights_;
    DirtyRegion dirty_splat_;
    std::uint64_t version_ = 1;
    std::uint64_t collision_version_ = 1;
    mutable bool unsaved_ = false;
};

class TerrainStore {
public:
    void setRoot(const std::filesystem::path& assets_root) { root_ = assets_root; }
    const std::filesystem::path& root() const { return root_; }
    std::filesystem::path absolute(const std::string& relative) const { return root_ / std::filesystem::path(relative); }

    // Los datos de un Terrain (se leen, o se crean con su resolucion si el
    // archivo no existe). nullptr si la ruta esta vacia.
    std::shared_ptr<TerrainData> get(const Terrain& terrain);
    // Solo si ya estan cargados (sin leer ni crear nada).
    std::shared_ptr<TerrainData> find(const Terrain& terrain) const { return findPath(terrain.data); }
    std::shared_ptr<TerrainData> findPath(const std::string& relative) const {
        const auto it = data_.find(relative);
        return it != data_.end() ? it->second : nullptr;
    }
    // Guarda los que tienen cambios. Devuelve cuantos.
    int saveAll();
    void clear() { data_.clear(); }

private:
    std::filesystem::path root_;
    std::unordered_map<std::string, std::shared_ptr<TerrainData>> data_;
};

// Registra el componente Terrain (idempotente).
void registerTerrainComponents();

}  // namespace cramion::terrain

#endif  // CRAMION_CORE_TERRAIN_H
