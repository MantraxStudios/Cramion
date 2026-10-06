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
// Lo que aporta luz rebotada: los objetos "Static" (Inspector) con malla; si
// no hay ninguno marcado, todas las mallas que no se mueven con fisica.

#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/World.h"

#include <CramionFX/core/Math.h>
#include <CramionFX/scene/Light.h>
#include <CramionFX/vk/BakedGi.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace cramion::assets {
class AssetManager;
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
};

struct BakeScene {
    std::vector<BakeTriangle> triangles;
    scene::LightSet lights;  // sol, puntuales y focos (unidades del renderizador)
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
    double seconds = 0.0;
};

// Hornea las sondas de todos los volumenes. false si se cancelo o no hay nada.
bool bakeProbes(const BakeScene& scene, gfx::BakedLighting& out, BakeProgress* progress = nullptr,
                BakeStats* stats = nullptr, std::string* error = nullptr);

// Lo que hace falta del mundo: triangulos de las mallas (con el albedo de sus
// materiales) y las cajas de los LightProbeVolume. `extra` anade geometria
// (el terreno, desde la fisica del editor).
using ExtraGeometry = std::function<void(std::vector<BakeTriangle>& triangles)>;
void gatherBakeScene(ecs::World& world, assets::AssetManager& assets, BakeScene& scene, const ExtraGeometry& extra = {});

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
