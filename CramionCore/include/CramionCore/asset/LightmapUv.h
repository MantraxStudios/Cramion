#ifndef CRAMION_CORE_ASSET_LIGHTMAP_UV_H
#define CRAMION_CORE_ASSET_LIGHTMAP_UV_H

// UV de lightmap (UV2) generadas por el motor, como el "Generate Lightmap UVs"
// de Unity (PLAN-LIGHTMAPS.md, Fase 1). Implementacion: LightmapUv.cpp (hash,
// aplicar, E/S y el conjunto de disposiciones) y LightmapUnwrap.cpp (el
// desenvuelto con xatlas: solo en el editor, Android no lo compila). Target
// CramionAssets (la usa AssetManager::readModel).
//
// La disposicion es COMPACTA: los vertices de la pieza siguen en su sitio; los
// que parte una costura de las cartas se anaden al final (duplicates) y solo
// cambian las esquinas que apuntan a uno anadido (patch). Mismos triangulos en
// el mismo orden. Se aplica al cargar el modelo, antes de reagrupar y de los
// LODs: el lightmap horneado y el que se dibuja usan siempre la MISMA
// disposicion. Lo generado va en Library/Cache/LightmapUV (.cruv) y dentro del
// .crbake de la escena (trozo UVLY); nunca en el .crdata.

#include "CramionCore/Uuid.h"

#include <CramionFX/asset/Model.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace cramion::lighting {

// Version del desenvuelto (entra en LightmapUvSettings::hash): subirla si
// cambia el algoritmo o el commit de xatlas.
inline constexpr std::uint32_t kUnwrapVersion = 1;
// UV2 que no tiene lightmap (pieza diminuta o cara degenerada): la iluminan
// las sondas y el sol en tiempo real.
inline constexpr std::uint32_t kLightmapUvNone = 0xFFFFFFFFu;

struct LightmapUvSettings {
    float texels_per_unit = 20.0f;        // texeles por unidad del modelo
    std::uint32_t padding = 2;            // texeles entre cartas (a esa densidad)
    float min_component_texels = 2.0f;    // piezas sueltas mas pequenas: sin UV2 (sondas)
    std::uint32_t max_size = 4096;        // lado maximo de la disposicion
    int chart_iterations = 1;             // xatlas ChartOptions::maxIterations (2 en Alta)
    std::uint64_t hash() const;           // texels_per_unit redondeado a 1/64 + kUnwrapVersion + commit de xatlas
};

// Disposicion COMPACTA: los vertices originales siguen en su sitio; los partidos
// por las costuras de las cartas se anaden al final; solo cambian las esquinas
// que apuntan a un vertice anadido. Mismos triangulos, mismo orden.
struct LightmapUvLayout {
    std::uint64_t source_hash = 0;        // lightmapMeshHash de la pieza tal como sale del .crdata
    std::uint32_t source_vertices = 0, source_indices = 0;
    std::uint64_t settings_hash = 0;
    std::uint64_t layout_hash = 0;        // huella de lo de abajo (la guarda cada registro del .crbake)
    std::uint32_t width = 0, height = 0;  // texeles a texels_per_unit
    float texels_per_unit = 0.0f;
    std::vector<std::uint32_t> duplicates;    // vertice original de cada anadido (indice V + i)
    std::vector<std::uint32_t> patch_corner;  // esquina (posicion en indices) ...
    std::vector<std::uint32_t> patch_vertex;  // ... y su vertice nuevo
    std::vector<std::uint32_t> uv;            // V + D valores unorm16x2 (u en los 16 bits bajos); 0xFFFFFFFF = sin lightmap
    std::uint32_t charts = 0, ignored_triangles = 0;
    float utilization = 0.0f;                 // texeles cubiertos / (width * height)
    float lightmapped_area = 0.0f;            // unidades del modelo al cuadrado
};

struct LightmapUvPartKey {
    Uuid model{};
    int part = 0;
    bool operator==(const LightmapUvPartKey&) const = default;
};
struct LightmapUvPartKeyHash {
    std::size_t operator()(const LightmapUvPartKey& k) const {
        return std::hash<Uuid>{}(k.model) ^ (static_cast<std::size_t>(static_cast<std::uint32_t>(k.part)) * 0x9E3779B97F4A7C15ull);
    }
};

// Donde esta cada disposicion (un .cruv de Library o un trozo UVLY de un .crbake).
// Se lee perezosamente en los hilos de carga.
struct LightmapUvSet {
    struct Source {
        std::filesystem::path file;
        std::uint64_t offset = 0, stored = 0, raw = 0;  // stored = 0: el archivo entero es un .cruv
        bool zstd = true;
        // Ya en memoria (el boton "Generar UV de lightmap" o el horneado):
        // no se lee nada.
        std::shared_ptr<const LightmapUvLayout> layout;
    };
    std::unordered_map<LightmapUvPartKey, Source, LightmapUvPartKeyHash> parts;
    std::shared_ptr<const LightmapUvLayout> load(const Uuid& model, int part) const;  // nullptr si no hay o esta danada
    bool empty() const { return parts.empty(); }
};

std::uint64_t lightmapMeshHash(const asset::ModelData& model);       // se mantiene (FNV de indices + posiciones)
// Superficie de la malla en unidades del modelo (al cuadrado).
float meshSurfaceArea(const asset::ModelData& model);
#if CRAMION_HAS_LIGHTMAP_UNWRAP
bool unwrapLightmapUvs(const asset::ModelData& part, const LightmapUvSettings& s, LightmapUvLayout& out,
                       const std::function<bool(float)>& progress, std::string* error);  // false si se cancela
#endif
bool applyLightmapUvs(asset::ModelData& model, const LightmapUvLayout& layout);  // false sin tocar nada si no cuadra
std::vector<std::uint8_t> encodeLightmapUvLayout(const LightmapUvLayout&);       // binario sin comprimir
bool decodeLightmapUvLayout(const std::uint8_t* data, std::size_t size, LightmapUvLayout& out);
bool saveLightmapUvLayout(const std::filesystem::path&, const LightmapUvLayout&);  // .cruv v2: "CRUV", v2, zstd(encode)
bool loadLightmapUvLayout(const std::filesystem::path&, LightmapUvLayout&);

// UV2 empaquetada (unorm16x2, u en los 16 bits bajos) -> [0, 1]^2; (-1, -1) si es kLightmapUvNone.
inline core::Vec2 unpackLightmapUv(std::uint32_t packed) {
    if (packed == kLightmapUvNone) return core::Vec2{-1.0f, -1.0f};
    return core::Vec2{static_cast<float>(packed & 0xFFFFu) / 65535.0f, static_cast<float>(packed >> 16) / 65535.0f};
}
// Al reves (con clamp a [0, 1]); un valor negativo es kLightmapUvNone.
inline std::uint32_t packLightmapUv(const core::Vec2& uv) {
    if (uv.x < 0.0f || uv.y < 0.0f) return kLightmapUvNone;
    const auto q = [](float v) {
        const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<std::uint32_t>(c * 65535.0f + 0.5f);
    };
    const std::uint32_t packed = q(uv.x) | (q(uv.y) << 16);
    // (1, 1) daria 0xFFFFFFFF, el centinela: se queda a un paso.
    return packed == kLightmapUvNone ? 0xFFFEFFFFu : packed;
}

// <cache>/<source_hash>_<settings_hash>.cruv (Library/Cache/LightmapUV).
std::filesystem::path lightmapUvCacheFile(const std::filesystem::path& cache_folder, std::uint64_t source_hash,
                                          std::uint64_t settings_hash);

}  // namespace cramion::lighting

#endif  // CRAMION_CORE_ASSET_LIGHTMAP_UV_H
