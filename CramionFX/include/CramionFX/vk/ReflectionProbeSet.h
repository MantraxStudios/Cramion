#ifndef CRAMION_VK_REFLECTION_PROBE_SET_H
#define CRAMION_VK_REFLECTION_PROBE_SET_H

// Sondas de reflexion colocadas (componente ReflectionProbeBox), como las
// Reflection Probe de Unity o las Box Reflection Capture de Unreal: un cubo
// horneado desde un punto con una caja de influencia. Dentro de la caja, los
// reflejos de respaldo (lo que el SSR no encuentra) salen de su cubo en vez
// de la sonda que sigue a la camara; con "proyeccion de caja" el reflejo se
// corrige con el paralaje de la caja (lo reflejado esta en sus paredes, no en
// el infinito), que es lo que hace creible un interior.
//
// Aqui solo hay datos y cuentas, sin Vulkan (se prueban sin GPU,
// ReflectionProbeTests): la eleccion de las sondas de cada frame, el peso de
// cada una en un punto (el mismo que lighting.frag), la proyeccion de caja y
// el empaquetado para la GPU. Los cubos viven en ReflectionProbe (un array de
// cubos con sus mips prefiltrados) y se hornean con
// VulkanRenderer::bakeReflectionCube.
//
// En el sombreado (lighting_body.glsl, placedProbeReflection) cada pixel
// mezcla las 2 sondas que mas pesan: manda la de mayor prioridad (a igual
// prioridad, la mas pequena: el orden de selectPlacedProbes); la segunda
// rellena lo que la primera deja en su borde de fundido, y lo que queda sin
// sonda colocada se queda con la sonda de la camara o el cielo.

#include "CramionFX/core/Math.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cramion::gfx {

// Cubos horneados en memoria a la vez (capas del array de cubos) y sondas
// que lee la iluminacion en cada frame (las mas cercanas a la camara).
inline constexpr std::uint32_t kMaxPlacedProbeCubes = 64;
inline constexpr std::uint32_t kMaxActivePlacedProbes = 8;

// Identidad de una sonda: el UUID de su objeto (16 bytes, high y low).
struct PlacedProbeId {
    std::uint64_t high = 0;
    std::uint64_t low = 0;
    bool valid() const { return high != 0 || low != 0; }
    friend bool operator==(const PlacedProbeId& a, const PlacedProbeId& b) {
        return a.high == b.high && a.low == b.low;
    }
};

// Una sonda colocada tal como la manda RenderSync cada frame (espacio de la
// escena, la caja sin giro).
struct PlacedReflectionProbe {
    PlacedProbeId id{};
    core::Vec3 position{};  // donde se hornea el cubo (el objeto)
    core::Vec3 box_min{};   // caja de influencia y de proyeccion
    core::Vec3 box_max{};
    float blend_distance = 1.0f;  // m hacia dentro desde el borde en que pasa de 0 a 1
    float intensity = 1.0f;
    std::int32_t priority = 0;
    bool box_projection = true;
};

// Un cubo horneado (lo que guarda el .crrefl): RGBA16F (half), los mips uno
// tras otro y en cada mip las 6 caras en el orden de Vulkan (+X -X +Y -Y +Z
// -Z), filas de arriba a abajo. El alfa no se usa (0).
struct BakedReflectionCube {
    PlacedProbeId id{};
    core::Vec3 position{};  // donde se horneo
    std::uint32_t size = 0;  // lado del mip 0
    std::uint32_t mips = 0;
    std::vector<std::uint16_t> texels;
};

// Halfs que ocupa un cubo de `size` px con `mips` niveles (RGBA).
std::size_t bakedCubeTexelCount(std::uint32_t size, std::uint32_t mips);
// Donde empieza el mip `mip` en BakedReflectionCube::texels (en halfs).
std::size_t bakedCubeMipOffset(std::uint32_t size, std::uint32_t mip);
// El cubo tiene el tamano que dice (y no esta vacio).
bool bakedCubeValid(const BakedReflectionCube& cube);

// Peso (0..1) de la sonda en el punto `p`: 0 fuera de la caja, 1 a mas de
// blend_distance de todas sus caras, y lineal entre medias. Igual que
// placedProbeWeight de lighting_body.glsl.
float placedProbeWeight(const PlacedReflectionProbe& probe, const core::Vec3& p);

// Direccion en la que se lee el cubo para el rayo que sale de `p` (dentro de
// la caja) en la direccion `r` (normalizada): con proyeccion de caja, desde
// el centro de la sonda hacia donde el rayo sale de la caja; sin ella, `r`.
// Igual que boxProjectedDirection de lighting_body.glsl.
core::Vec3 boxProjectedDirection(const PlacedReflectionProbe& probe, const core::Vec3& p, const core::Vec3& r);

// Las sondas de la iluminacion de un frame, ya en el orden de la GPU (mayor
// prioridad primero; a igual prioridad, la caja mas pequena; a igualdad, la
// del id menor, para que el orden no baile entre frames).
struct SelectedPlacedProbe {
    std::uint32_t probe = 0;  // indice en la lista de entrada
    std::uint32_t layer = 0;  // capa del array de cubos con su horneado
};
// De las que tienen cubo horneado (`baked`: el id de cada capa; las que no
// tienen, fuera) e intensidad > 0, las `max_count` mas cercanas a `eye`
// (distancia a su caja: 0 dentro).
std::vector<SelectedPlacedProbe> selectPlacedProbes(const std::vector<PlacedReflectionProbe>& probes,
                                                    const std::vector<PlacedProbeId>& baked, const core::Vec3& eye,
                                                    std::uint32_t max_count = kMaxActivePlacedProbes);

// Mezcla de las sondas en un punto, como la hace lighting.frag: las 2 que mas
// pesan (prioridad y luego peso; empate: la primera de la lista). `weights`
// son los pesos de la primera y de la segunda (0 si no hay); la segunda solo
// rellena lo que deja la primera (w0 + w1 <= 1). Devuelve cuantas entran.
struct PlacedProbeBlend {
    int first = -1;
    int second = -1;
    float first_weight = 0.0f;
    float second_weight = 0.0f;
    float total() const { return first_weight + second_weight; }
};
PlacedProbeBlend blendPlacedProbes(const std::vector<PlacedReflectionProbe>& ordered, const core::Vec3& p);

// --- Para la GPU (GpuLights, al final; lighting_body.glsl, PlacedProbe) ---
struct GpuPlacedProbe {
    core::Vec4 center_intensity{};  // xyz = donde se horneo, w = intensidad
    core::Vec4 box_min_blend{};     // xyz = esquina menor, w = fundido (m)
    core::Vec4 box_max_priority{};  // xyz = esquina mayor, w = prioridad
    core::Vec4 params{};            // x = capa del array, y = 1 proyeccion de caja
};
static_assert(sizeof(GpuPlacedProbe) == 64, "GpuPlacedProbe: 4 vec4 en std140");

// Empaqueta una sonda elegida (con su capa). El centro es el del horneado
// (`baked_position`): el paralaje se mide desde donde se capturo.
GpuPlacedProbe packPlacedProbe(const PlacedReflectionProbe& probe, std::uint32_t layer,
                               const core::Vec3& baked_position);

}  // namespace cramion::gfx

#endif  // CRAMION_VK_REFLECTION_PROBE_SET_H
