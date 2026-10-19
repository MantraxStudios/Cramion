#ifndef CRAMION_CORE_ASSET_BAKED_LIGHTING_FILE_H
#define CRAMION_CORE_ASSET_BAKED_LIGHTING_FILE_H

// <Escena>.crbake: la iluminacion horneada de una escena (sondas, lightmaps y
// las UV de lightmap con que se hornearon). Implementacion: CramionAssets
// (src/asset/BakedLightingFile.cpp; necesita zstd). PLAN-LIGHTMAPS.md 2.4.
//
// v1-v3 (se siguen leyendo): "CRBK", u32 version, u32 modo, volumenes, sondas,
// lightmap de superficie (v2) y luces horneadas (v3), seguidos.
//
// v4: cabecera de 32 B y trozos con una tabla de contenido al final, cada
// trozo comprimido con zstd si conviene:
//   cabecera  "CRBK", u32 4, u32 modo (offset 8, como v1-3), u32 flags
//             (bit0 visibilidad del sol, bit1 lightmaps, bit2 irradiancia),
//             u64 offset de la tabla, u32 trozos, u32 0
//   INFO  version del motor, UUID de la escena, origen del mundo, ajustes
//         (JSON) y el sol al hornear
//   VOLS  volumenes de sondas (como v3)        PRBS  sondas (zstd)
//   LGTS  sol (visibilidad, direccion) y UUID de las locales Horneadas
//   LMAP  paginas, ancho, alto, formato, texeles/m, limites, irradiancia
//   PAGE  una por pagina: indice, ancho, alto + texeles half x4 (zstd)
//   LMRC  registros por instancia (112 B cada uno)
//   UVLY  una por disposicion: UUID del modelo, pieza + encodeLightmapUvLayout
//         (zstd; no se descomprime al leer: LightmapUvSet la lee al cargar el
//         modelo)
//   tabla {char id[4]; u32 flags (bit0 zstd); u64 offset, guardado, crudo; u64 fnv}

#include "CramionCore/Uuid.h"
#include "CramionCore/asset/LightmapUv.h"

#include <CramionFX/vk/BakedGi.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace cramion::lighting {

inline constexpr std::uint32_t kBakedLightingVersion = 4;

// Lo que acompana al horneado v4 (INFO y UVLY).
struct BakedLightingExtras {
    std::string engine_version;
    std::string settings_json;  // ajustes usados (JSON)
    core::Vec3 sun_direction{};
    core::Vec3 sun_color{};
    float sun_intensity = 0.0f;
    std::uint32_t sun_mode = 0;  // ecs::LightBakeMode
    struct Layout {
        Uuid model{};
        int part = 0;
        std::shared_ptr<const LightmapUvLayout> layout;
    };
    std::vector<Layout> layouts;
};

// Se escriben a <archivo>.tmp y se renombra. data.version <= 3: el formato de
// siempre (sondas y lightmap de superficie); >= 4: el de trozos (con
// `extras` si los hay).
bool saveBakedLighting(const std::filesystem::path& file, const gfx::BakedLighting& data, gfx::LightingMode mode,
                       std::string* error = nullptr, const BakedLightingExtras* extras = nullptr);
// Lee v1-v4 (una version posterior se rechaza: "archivo de iluminacion
// danado"). Con `uvs`, las disposiciones UVLY del archivo quedan apuntadas
// (sin leerlas) para cargar los modelos con ellas.
bool loadBakedLighting(const std::filesystem::path& file, gfx::BakedLighting& data, gfx::LightingMode& mode,
                       std::string* error = nullptr, LightmapUvSet* uvs = nullptr, BakedLightingExtras* extras = nullptr);
// Solo los registros de los lightmaps (LMRC; vacio con v1-v3): el lote
// estatico al exportar deja fuera lo que lleva lightmap.
bool loadLightmapRecords(const std::filesystem::path& file, std::vector<gfx::LightmapRecord>& records);
// Cambia el modo (el u32 del offset 8) sin reescribir el archivo: vale para
// v1-v4 (las paginas no se tocan).
bool setBakedLightingMode(const std::filesystem::path& file, gfx::LightingMode mode, std::string* error = nullptr);

}  // namespace cramion::lighting

#endif  // CRAMION_CORE_ASSET_BAKED_LIGHTING_FILE_H
