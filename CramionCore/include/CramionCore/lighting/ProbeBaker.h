#ifndef CRAMION_CORE_LIGHTING_PROBE_BAKER_H
#define CRAMION_CORE_LIGHTING_PROBE_BAKER_H

// Iluminacion horneada (como el Lighting > Generate Lighting de Unity): la luz
// rebotada se calcula una vez en la CPU (trazado de caminos con un BVH, en
// todos los nucleos) y se guarda en volumenes de sondas (CramionFX/vk/BakedGi.h).
// En el juego cuesta casi nada: sirve para Android y para PCs sin rayos.
//
//   LightProbeVolume   componente: una caja (centrada en el objeto) llena de
//                      sondas cada `spacing` metros.
//   bakeProbes         el horneado (sol, luces puntuales y focos con sombras,
//                      superficies emisivas, cielo y N rebotes).
//   <escena>.crbake    el resultado, junto a la escena (va con el juego
//                      exportado). El editor y el juego lo cargan al abrirla.
//
// Lo que aporta luz rebotada: los objetos "Static" (Inspector) con malla (o
// hijos de un objeto Static: basta marcar la raiz de un modelo o de una
// instancia de prefab); si no hay ninguno, todas las mallas que no se mueven
// con fisica. El terreno siempre (desde su mapa de alturas).
//
// Luces "Horneadas" (Light > Modo): su luz directa y sus sombras suaves
// tambien se hornean (sondas + lightmap de superficie) y en modo horneado no
// se dibujan en tiempo real. Las "Mixtas" solo aportan su rebote.

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/World.h"

#include <CramionFX/core/Math.h>
#include <CramionFX/scene/Light.h>
#include <CramionFX/vk/BakedGi.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cramion::assets {
class AssetManager;
}

namespace cramion::terrain {
class TerrainData;
}

namespace cramion::lighting {

inline constexpr const char* kBakedLightingExtension = ".crbake";

struct LightProbeVolume {
    core::Vec3 size{20.0f, 8.0f, 20.0f};  // m (la caja, centrada en el objeto; sin giro)
    float spacing = 2.0f;                 // m entre sondas
    float intensity = 1.0f;               // multiplica la luz horneada
    void reflect(ecs::PropertyVisitor& v);
};

void registerLightingComponents();

struct BakeTriangle {
    core::Vec3 a, b, c;
    core::Vec3 albedo{0.5f, 0.5f, 0.5f};  // lineal
    core::Vec3 emission{};                // lineal (unidades del renderizador)
};

struct BakeVolume {
    core::Vec3 min{};
    core::Vec3 size{1.0f, 1.0f, 1.0f};
    std::uint32_t nx = 2, ny = 2, nz = 2;
    float intensity = 1.0f;
};

struct BakeSettings {
    int rays = 256;           // por sonda
    int bounces = 2;          // rebotes despues del primero (0 = un rebote)
    int threads = 0;          // 0 = todos los nucleos menos uno
    float sky_intensity = 1.0f;
    float max_probes = 200000;  // tope (memoria y tiempo)
    // Lightmap de superficie (texeles en el mundo pegados a la geometria): 0 =
    // no; si no, el lado del texel en metros (0.25..2). Da el detalle de un
    // lightmap a la luz rebotada (rincones, bajo las mesas, junto a paredes).
    float surface_texel = 0.0f;
    int surface_rays = 96;
    std::uint32_t max_surface_texels = 2000000;
    // Luces horneadas: rayos de sombra por luz y texel (sombras suaves con el
    // radio de la fuente; el sol, con su disco de medio grado).
    int direct_samples = 16;
    // Si hay luces horneadas y el lightmap de superficie esta apagado, se
    // enciende con este texel (sin el, las sombras horneadas serian las de
    // las sondas: muy borrosas). 0 = no encenderlo.
    float auto_surface_texel = 0.5f;
};

struct BakeScene {
    std::vector<BakeTriangle> triangles;
    // Sol, puntuales y focos que aportan al horneado (unidades del
    // renderizador). Las "Solo tiempo real" no estan.
    scene::LightSet lights;
    // Paralelos a lights.points / lights.spots: 1 = horneada (tambien su luz
    // directa y sus sombras). Si faltan, 0 (solo rebote, lo de antes).
    std::vector<std::uint8_t> point_baked;
    std::vector<std::uint8_t> spot_baked;
    bool sun_baked = false;  // el sol tambien en directa (y sombras)
    // UUID (alto, bajo) de las puntuales/focos horneados (van al .crbake).
    std::vector<std::array<std::uint64_t, 2>> baked_light_ids;
    core::Vec3 sky_zenith{0.30f, 0.46f, 0.85f};
    core::Vec3 sky_horizon{0.72f, 0.80f, 0.90f};
    std::vector<BakeVolume> volumes;
    BakeSettings settings;
};

struct BakeProgress {
    std::atomic<float> fraction{0.0f};
    std::atomic<bool> cancel{false};
    std::mutex mutex;
    std::string stage;
    void setStage(const std::string& text) {
        std::lock_guard lock(mutex);
        stage = text;
    }
    std::string currentStage() {
        std::lock_guard lock(mutex);
        return stage;
    }
};

struct BakeStats {
    std::size_t triangles = 0;
    std::size_t probes = 0;
    std::size_t invalid_probes = 0;
    std::size_t surface_texels = 0;
    std::size_t baked_lights = 0;  // con su luz directa horneada (el sol cuenta)
    double seconds = 0.0;
};

// Por que una malla no entro en el horneado.
enum class BakeSkip : int {
    Inactive = 0,    // desactivada (o un padre)
    Hidden,          // MeshRenderer invisible
    DynamicBody,     // Rigidbody Dynamic y no Static
    AnimatorDriven,  // un Animator la mueve
    NoModel,         // el modelo no se pudo cargar
    BadPart,         // la pieza no existe en el modelo
    NotStatic,       // hay objetos Static y este no lo es
    Count
};

// Lo que encontro gatherBakeScene (para los avisos del editor y el MCP).
struct BakeGatherReport {
    std::size_t meshes_seen = 0;    // entidades con MeshRenderer
    std::size_t meshes_baked = 0;
    std::size_t static_marked = 0;  // mallas validas Static (propias o heredadas)
    std::size_t terrains = 0;
    std::size_t triangles = 0;
    std::size_t terrain_triangles = 0;
    std::size_t skinned_rest_pose = 0;     // con huesos: en su pose de reposo
    std::size_t static_with_dynamic_rb = 0;
    std::size_t static_with_animator = 0;  // Static con un Animator que reproduce: en reposo
    std::size_t skipped[static_cast<int>(BakeSkip::Count)] = {};
    std::vector<std::string> examples[static_cast<int>(BakeSkip::Count)];  // hasta 3 nombres por motivo
    bool used_all_non_moving = false;  // nada Static: todas las mallas que no se mueven
    // Luces (gatherBakeLights)
    std::size_t lights_mixed = 0, lights_baked = 0, lights_realtime = 0;
    bool sun_baked = false;
    bool sun_realtime_only = false;

    void skip(BakeSkip reason, const std::string& name);
    // Resumen en espanol, una linea por aviso (la primera es la principal).
    std::string summary() const;
};

const char* bakeSkipLabel(BakeSkip reason);

// Hornea las sondas de todos los volumenes. false si se cancelo o no hay nada.
bool bakeProbes(const BakeScene& scene, gfx::BakedLighting& out, BakeProgress* progress = nullptr,
                BakeStats* stats = nullptr, std::string* error = nullptr);

// Lo que hace falta del mundo: triangulos de las mallas (con el albedo de sus
// materiales), del terreno y las cajas de los LightProbeVolume. `extra`
// anade geometria; `terrains` da los datos de cada terreno (sin el, no hay
// terreno). `report` cuenta lo que entra y lo que no, y por que.
using ExtraGeometry = std::function<void(std::vector<BakeTriangle>& triangles)>;
using TerrainProvider = std::function<std::shared_ptr<const terrain::TerrainData>(ecs::Entity)>;
void gatherBakeScene(ecs::World& world, assets::AssetManager& assets, BakeScene& scene, const ExtraGeometry& extra = {},
                     const TerrainProvider& terrains = {}, BakeGatherReport* report = nullptr);

// Puede entrar en el horneado (Static propio o de algun padre)?
bool staticInHierarchy(ecs::Entity entity);

// Las luces del mundo (todas, sin el tope por frame del renderizador) con su
// modo. El sol (direccion, color, intensidad: los da la hora del dia) viene
// de `sun`; su modo, del Light Direccional si lo hay, o si no `sun_mode`.
void gatherBakeLights(ecs::World& world, const scene::LightSet& sun, BakeScene& scene,
                      ecs::LightBakeMode sun_mode = ecs::LightBakeMode::Mixed, BakeGatherReport* report = nullptr);

// Volumen por defecto que cubre toda la geometria (si la escena no tiene).
BakeVolume volumeAround(const std::vector<BakeTriangle>& triangles, float spacing);

// <escena>.crbake
std::filesystem::path bakedLightingFile(const std::filesystem::path& scene_file);
bool saveBakedLighting(const std::filesystem::path& file, const gfx::BakedLighting& data, gfx::LightingMode mode,
                       std::string* error = nullptr);
bool loadBakedLighting(const std::filesystem::path& file, gfx::BakedLighting& data, gfx::LightingMode& mode,
                       std::string* error = nullptr);

// Irradiancia de una sonda en una direccion (la misma cuenta que el shader;
// para las pruebas y la vista del editor).
core::Vec3 probeIrradiance(const core::Vec4* probe, const core::Vec3& normal);
float probeSkyVisibility(const core::Vec4* probe, const core::Vec3& normal);

}  // namespace cramion::lighting

#endif  // CRAMION_CORE_LIGHTING_PROBE_BAKER_H
