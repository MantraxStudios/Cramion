#ifndef CRAMION_VK_BAKED_GI_H
#define CRAMION_VK_BAKED_GI_H

// CONTRATO COMPARTIDO (renderizador <-> CramionCore/lighting): iluminacion
// indirecta horneada en volumenes de sondas (irradiance volumes, como los
// Light Probe Groups de Unity o el Volumetric Lightmap de Unreal).
//
// Cada sonda guarda la luz rebotada que le llega (armonicos esfericos L2, RGB)
// y cuanto cielo ve en cada direccion (L1). En modo horneado el renderizador
// no traza rayos ni hace SSGI: un pase a media resolucion interpola las 8
// sondas de alrededor de cada pixel (trilineal) y escribe la misma imagen de
// GI que leeria la iluminacion. Sirve igual para lo estatico y lo dinamico, en
// Android y en PCs sin trazado de rayos.
//
// Formato de una sonda: 8 vec4 (32 floats)
//   [0..26]  9 coeficientes RGB intercalados (c0.r c0.g c0.b c1.r ...): luz
//            rebotada ya convolucionada con el coseno y / pi (el mismo
//            convenio que la irradiancia del cielo, ibl_sh.comp)
//   [27..30] visibilidad del cielo L1 (4 coeficientes, mismo convenio)
//   [31]     validez (0 = dentro de la geometria: no se usa)

#include "CramionFX/core/Math.h"

#include <cstdint>
#include <vector>

namespace cramion::gfx {

inline constexpr std::uint32_t kBakedProbeVec4 = 8;
inline constexpr std::uint32_t kMaxBakedVolumes = 16;

struct BakedProbeVolume {
    core::Vec3 min{};             // esquina en el mundo
    core::Vec3 size{1.0f, 1.0f, 1.0f};
    std::uint32_t nx = 2, ny = 2, nz = 2;  // sondas por eje (>= 2)
    std::uint32_t first = 0;      // primera sonda en BakedLighting::probes (en sondas)
    float intensity = 1.0f;
};

// Lightmap de superficie: texeles en el mundo (celdas de `cell_size`
// metros pegadas a la geometria) en una tabla hash dispersa. Cada texel lleva
// la luz rebotada en L1 RGB y la visibilidad del cielo en L1 (4 vec4: floats
// 0..11 = c0.rgb c1.rgb c2.rgb c3.rgb, 12..15 = cielo), mismo convenio que las
// sondas (convolucionada con el coseno y / pi). La GI horneada lo usa antes
// que las sondas: detalle de lightmap en todo (estatico, terreno, follaje y
// lo que se mueve), sin UV2.
inline constexpr std::uint32_t kSurfaceTexelVec4 = 4;
inline constexpr std::uint32_t kSurfaceEmptyKey = 0xFFFFFFFFu;
struct SurfaceLightmap {
    float cell_size = 0.5f;
    std::uint32_t capacity = 0;      // potencia de 2 (0 = no hay)
    std::vector<std::uint32_t> keys; // 2 por hueco (bajo, alto); kSurfaceEmptyKey x2 = vacio
    std::vector<core::Vec4> texels;  // kSurfaceTexelVec4 por hueco
    std::uint32_t count = 0;         // huecos ocupados
    bool empty() const { return capacity == 0 || count == 0; }
};

// Clave de una celda (x, y, z en celdas; 21 bits cada una con signo).
inline std::uint64_t surfaceCellKey(int x, int y, int z) {
    const auto b = [](int v) { return static_cast<std::uint64_t>(static_cast<std::uint32_t>(v + (1 << 20)) & 0x1FFFFFu); };
    return b(x) | (b(y) << 21) | (b(z) << 42);
}
// El mismo hash que baked_gi.frag.
inline std::uint32_t surfaceCellHash(std::uint32_t lo, std::uint32_t hi) {
    std::uint32_t h = lo * 0x9E3779B1u ^ hi * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

struct BakedLighting {
    std::vector<BakedProbeVolume> volumes;  // del mas pequeno al mas grande (gana el primero que contiene el punto)
    std::vector<core::Vec4> probes;         // kBakedProbeVec4 por sonda
    SurfaceLightmap surface;                // opcional (texel en el mundo)
    bool empty() const { return (volumes.empty() || probes.empty()) && surface.empty(); }
};

enum class LightingMode : int {
    Realtime = 0,  // SSGI o trazado de rayos (lo de siempre)
    Baked = 1,     // las sondas horneadas en lugar de la GI en tiempo real
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_BAKED_GI_H
