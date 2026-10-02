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

struct BakedLighting {
    std::vector<BakedProbeVolume> volumes;  // del mas pequeno al mas grande (gana el primero que contiene el punto)
    std::vector<core::Vec4> probes;         // kBakedProbeVec4 por sonda
    bool empty() const { return volumes.empty() || probes.empty(); }
};

enum class LightingMode : int {
    Realtime = 0,  // SSGI o trazado de rayos (lo de siempre)
    Baked = 1,     // las sondas horneadas en lugar de la GI en tiempo real
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_BAKED_GI_H
