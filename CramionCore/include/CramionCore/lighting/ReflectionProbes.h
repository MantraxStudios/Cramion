#ifndef CRAMION_CORE_LIGHTING_REFLECTION_PROBES_H
#define CRAMION_CORE_LIGHTING_REFLECTION_PROBES_H

// Sondas de reflexion colocadas, como la Reflection Probe de Unity o la Box
// Reflection Capture de Unreal (CramionFX/vk/ReflectionProbeSet.h):
//
//   ReflectionProbeBox  componente: una caja de influencia (centrada en el
//                       objeto, sin giro) con su fundido en el borde, su
//                       prioridad y la proyeccion de caja (paralaje). Dentro,
//                       los reflejos que el SSR no encuentra salen de su cubo
//                       horneado en vez de la sonda que sigue a la camara.
//   bakeReflectionProbes el horneado: un cubo por sonda desde su posicion,
//                       con la escena tal como se ve (en el editor: boton
//                       Hornear del Inspector y la herramienta MCP
//                       bake_reflection_probes).
//   <escena>.crrefl     los cubos (RGBA16F con sus mips prefiltrados), junto
//                       a la escena; el editor y el juego lo cargan al abrirla
//                       (loadSceneReflectionProbes). Sin archivo, las sondas
//                       no hacen nada y todo se ve como antes.
//
// CVar render.probe.Placed (1): 0 = no se usan (como antes).
//
// .crrefl v1: "CRRF", u32 version, u32 cubos, u32 0; por cubo: u64 high,
// u64 low (UUID del objeto), f32 x3 (posicion del horneado), u32 lado,
// u32 mips, u64 halfs, u64 FNV-1a de los halfs, y los halfs (RGBA16F, mips
// seguidos y en cada uno las 6 caras, como BakedReflectionCube).

#include "CramionCore/Uuid.h"
#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/World.h"

#include <CramionFX/core/Math.h>
#include <CramionFX/vk/ReflectionProbeSet.h>

#include <filesystem>
#include <string>
#include <vector>

namespace cramion::gfx {
class VulkanRenderer;
}

namespace cramion::scene {
class Scene;
}

namespace cramion::lighting {

inline constexpr const char* kReflectionProbesExtension = ".crrefl";
inline constexpr std::uint32_t kReflectionProbesVersion = 1;

struct ReflectionProbeBox {
    core::Vec3 size{10.0f, 5.0f, 10.0f};  // m: caja de influencia y de proyeccion, centrada en el objeto
    float blend_distance = 1.0f;           // m hacia dentro en que la sonda entra fundiendose
    int priority = 0;                      // la de mas prioridad manda donde se solapan
    float intensity = 1.0f;                // multiplica el reflejo
    bool box_projection = true;            // paralaje de caja (interiores); apagado: como un cielo lejano
    void reflect(ecs::PropertyVisitor& v);
};

// Registra el componente (lo llama registerLightingComponents).
void registerReflectionProbeComponent();

// La sonda tal como la ve el renderizador (caja en el mundo).
gfx::PlacedReflectionProbe placedProbeOf(const ReflectionProbeBox& probe, const Uuid& id, const core::Vec3& position);
gfx::PlacedProbeId placedProbeId(const Uuid& id);

// --- Archivo .crrefl ---
std::filesystem::path reflectionProbesFile(const std::filesystem::path& scene_file);
// Se escribe a <archivo>.tmp y se renombra. Sin cubos validos, borra el archivo.
bool saveReflectionProbes(const std::filesystem::path& file, const std::vector<gfx::BakedReflectionCube>& cubes,
                          std::string* error = nullptr);
// Lee v1 (una version posterior o un archivo danado: false, sin cubos).
bool loadReflectionProbes(const std::filesystem::path& file, std::vector<gfx::BakedReflectionCube>& cubes,
                          std::string* error = nullptr);

// --- Con el renderizador ---
// Las sondas activas de la escena, cada frame (RenderSync).
void syncReflectionProbes(ecs::World& world, gfx::VulkanRenderer& renderer);
// El .crrefl de la escena al renderizador (sin archivo: ninguna). Al abrir la
// escena, en el editor y en el juego.
void loadSceneReflectionProbes(const std::filesystem::path& scene_file, gfx::VulkanRenderer& renderer);

struct ReflectionBakeReport {
    int baked = 0;
    int failed = 0;
    int total = 0;   // sondas en la escena
    std::string error;
    std::filesystem::path file;
};
// Hornea las sondas de la escena (`only` vacio: todas; si no, esas), guarda
// el .crrefl junto a `scene_file` (conserva el horneado de las demas y quita
// el de las que ya no existen) y lo pone en el renderizador. Entre frames:
// espera a la GPU en cada sonda.
ReflectionBakeReport bakeReflectionProbes(ecs::World& world, gfx::VulkanRenderer& renderer, const scene::Scene& scene,
                                          const std::filesystem::path& scene_file,
                                          const std::vector<Uuid>& only = {});

}  // namespace cramion::lighting

#endif  // CRAMION_CORE_LIGHTING_REFLECTION_PROBES_H
