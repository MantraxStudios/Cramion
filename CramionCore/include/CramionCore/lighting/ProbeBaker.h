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
// con fisica. El terreno siempre (desde su mapa de alturas). Lo transparente
// (vidrio, agua) deja pasar la luz.
//
// Luces "Horneadas" (Light > Modo): su luz directa y sus sombras suaves
// tambien se hornean (sondas + lightmap de superficie) y en modo horneado no
// se dibujan en tiempo real. Las "Mixtas" solo aportan su rebote.

#include "CramionCore/Uuid.h"
#include "CramionCore/asset/BakedLightingFile.h"
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

namespace cramion::asset {
struct ModelData;
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

inline constexpr std::uint32_t kNoBakeMaterial = 0xFFFFFFFFu;

struct BakeTriangle {
    core::Vec3 a, b, c;
    core::Vec3 albedo{0.5f, 0.5f, 0.5f};  // lineal (albedo de rebote, ya limitado a 0.95)
    core::Vec3 emission{};                // lineal (unidades del renderizador)
    // Indice en BakeScene::materials (kNoBakeMaterial: el color de arriba vale tal cual).
    std::uint32_t material = kNoBakeMaterial;
};

// De donde sale el color de un material (PLAN-LIGHTMAPS 0.2). gatherBakeScene
// solo apunta las rutas (hilo principal); resolveBakeMaterials lee las
// texturas en el hilo del horneado y pone el color de cada triangulo.
struct BakeMaterial {
    // Material de un modelo del asset: su .crdata, pieza e indice.
    Uuid model{};
    std::filesystem::path model_file;
    int part = -1;
    std::uint32_t index = 0;
    // .crmat o malla de codigo: imagenes en disco (vacio = sin textura).
    bool from_files = false;
    std::filesystem::path albedo_file;
    std::filesystem::path emissive_file;
    // Factores lineales (sin texturas), emision ya x6 (skinned.frag).
    core::Vec3 base_albedo{0.5f, 0.5f, 0.5f};
    core::Vec3 base_emission{};
    float metallic = 0.0f;
    float roughness = 0.8f;
    // Resultado (resolveBakeMaterials).
    core::Vec3 albedo{0.5f, 0.5f, 0.5f};  // de rebote, limitado a 0.95
    core::Vec3 emission{};
    float coverage = 1.0f;  // texeles opacos de la textura de color (recorte por alfa)
    bool resolved = false;
};

// Lo que gatherBakeScene puso de cada malla de modelo (los lightmaps la
// cambian por su copia con UV2 y usan sus materiales).
struct BakeMeshEntry {
    Uuid entity{};
    Uuid model{};
    int part = 0;
    std::uint32_t first = 0, count = 0;            // sus triangulos en BakeScene::triangles
    std::vector<std::uint32_t> materials;          // por material de la pieza: indice en BakeScene::materials
    std::vector<std::uint8_t> transparent;         // por material de la pieza
};

// Albedo de rebote (convenio del meta pass de Unity, PLAN-LIGHTMAPS 1.5):
// difuso sin la parte metalica mas un poco del especular; limitado a 0.95.
core::Vec3 bounceAlbedo(const core::Vec3& albedo, float metallic, float roughness);

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
    // .crbake v4 (lightmaps): la luz DIRECTA del sol no va a las sondas (se
    // evalua siempre en tiempo real); su rebote si (PLAN-LIGHTMAPS 1.2).
    bool exclude_sun_direct = false;
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
    // Fuerza de la sombra del sol (Light.shadow_strength; 0 si no proyecta).
    float sun_shadow_strength = 1.0f;
    // El cielo del render: los 9 coeficientes de su irradiancia (ya / pi,
    // VulkanRenderer::readSkyIrradianceSh). Sin ellos (pruebas), el degradado
    // de abajo.
    std::array<core::Vec3, 9> sky_sh{};
    bool has_sky_sh = false;
    core::Vec3 sky_zenith{0.30f, 0.46f, 0.85f};
    core::Vec3 sky_horizon{0.72f, 0.80f, 0.90f};
    std::vector<BakeVolume> volumes;
    std::vector<BakeMaterial> materials;  // los de BakeTriangle::material
    std::vector<BakeMeshEntry> meshes;    // las mallas de modelo que entraron (gatherBakeScene)
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
    std::size_t materials = 0;          // resolveBakeMaterials: materiales con color leido
    std::size_t material_textures = 0;  // ... de ellos con textura
    std::size_t probes = 0;
    std::size_t invalid_probes = 0;
    std::size_t surface_texels = 0;
    std::size_t baked_lights = 0;  // con su luz directa horneada (el sol cuenta)
    // Lado del texel del lightmap de superficie que se uso (0 = sin el). Si
    // se encendio solo y no cabia en el tope, se agranda: surface_texel_grown.
    float surface_texel = 0.0f;
    bool surface_texel_grown = false;
    // Texeles fuera de los volumenes, donde llega la luz directa de las luces
    // horneadas. Si no cabian (outside_texels_dropped), no hay ninguno: esas
    // superficies se quedan sin esa luz (los de dentro no cambian).
    std::size_t outside_texels = 0;
    bool outside_texels_dropped = false;
    std::size_t dropped_volumes = 0;  // mas de gfx::kMaxBakedVolumes: los mas grandes no entran
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
    NoGeometry,      // entraba, pero la malla de su modelo no se pudo leer (ni de la escena ni del disco)
    Transparent,     // todos sus materiales son transparentes (vidrio, agua): no tapan la luz
    EmptyMesh,       // su malla (de codigo o la pieza leida) no tiene ningun triangulo
    Count
};

// Lo que encontro gatherBakeScene (para los avisos del editor y el MCP).
struct BakeGatherReport {
    std::size_t meshes_seen = 0;    // entidades con MeshRenderer
    std::size_t meshes_baked = 0;
    std::size_t static_marked = 0;  // mallas validas Static (propias o heredadas)
    std::size_t terrains = 0;
    std::size_t triangles = 0;
    std::size_t mesh_triangles = 0;  // los de las mallas (van primero en BakeScene::triangles)
    std::size_t terrain_triangles = 0;
    std::size_t transparent_triangles = 0;  // de materiales transparentes: fuera (dejan pasar la luz)
    std::size_t reread_models = 0;          // modelos cuya geometria se volvio a leer del disco (no las primitivas)
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
//
// La malla de cada pieza de modelo sale, por orden: de `parts` (con el
// renderizador en marcha, RenderSync::partModelData: al dibujar una pieza
// RenderSync la mueve del AssetManager a la escena y alli queda vacia); de
// la pieza del AssetManager si aun la tiene; y si no, se relee solo la
// geometria del disco (una vez por modelo y sin texturas; se suelta al
// terminar). Sin `parts` (pruebas, herramientas) sirve igual.
using ExtraGeometry = std::function<void(std::vector<BakeTriangle>& triangles)>;
using TerrainProvider = std::function<std::shared_ptr<const terrain::TerrainData>(ecs::Entity)>;
using ModelPartProvider = std::function<const asset::ModelData*(const Uuid& model, int part)>;
void gatherBakeScene(ecs::World& world, assets::AssetManager& assets, BakeScene& scene, const ExtraGeometry& extra = {},
                     const TerrainProvider& terrains = {}, BakeGatherReport* report = nullptr,
                     const ModelPartProvider& parts = {});

// Lee las texturas de los materiales (BakeScene::materials: medias de color y
// de emision, recorte por alfa) y pone el color de cada triangulo. En el hilo
// del horneado: lee los .crdata con texturas. Sin materiales no hace nada.
void resolveBakeMaterials(BakeScene& scene, BakeProgress* progress = nullptr, BakeStats* stats = nullptr);

// Parte de los triangulos de las mallas (los `mesh_triangles` primeros) cuyo
// centroide cae dentro de algun volumen de sondas (1 sin volumenes). En
// `uncovered`, los que quedan fuera.
float probeCoverage(const BakeScene& scene, std::size_t mesh_triangles, std::vector<std::uint32_t>* uncovered = nullptr);

// La matriz de reposo de una pieza rigida (un hueso: nodo x offset, como la
// dibuja RenderSync sin Animator). Identidad sin huesos.
core::Mat4 restPoseMatrix(const asset::ModelData& data);

// Puede entrar en el horneado (Static propio o de algun padre)?
bool staticInHierarchy(ecs::Entity entity);

// Las luces del mundo (todas, sin el tope por frame del renderizador) con su
// modo. El sol (direccion, color, intensidad: los da la hora del dia) viene
// de `sun`; su modo, del Light Direccional si lo hay, o si no `sun_mode`.
void gatherBakeLights(ecs::World& world, const scene::LightSet& sun, BakeScene& scene,
                      ecs::LightBakeMode sun_mode = ecs::LightBakeMode::Mixed, BakeGatherReport* report = nullptr);

// Volumen por defecto que cubre la geometria (si la escena no tiene): toda
// (mallas y terreno) si cabe en 400 x 60 x 400 m; si no, ese tope alrededor
// de los primeros `bounds_count` triangulos (> 0: las mallas,
// BakeGatherReport::mesh_triangles; un terreno de kilometros alejaria la caja
// de ellas) sin salirse de la geometria. De alto, desde abajo (el suelo, donde
// se mueve lo dinamico). Si con `spacing` salen mas de `max_probes` sondas
// (0 = sin tope), las separa mas: la separacion final es size / (n - 1).
BakeVolume volumeAround(const std::vector<BakeTriangle>& triangles, float spacing, float max_probes = 0.0f,
                        std::size_t bounds_count = 0);
// Lo mismo alrededor de un subconjunto (indices en `triangles`): el volumen
// automatico extra cuando los del usuario no cubren lo Static (probeCoverage).
BakeVolume volumeAround(const std::vector<BakeTriangle>& triangles, const std::vector<std::uint32_t>& subset,
                        float spacing, float max_probes = 0.0f);

// <escena>.crbake (saveBakedLighting / loadBakedLighting: asset/BakedLightingFile.h).
std::filesystem::path bakedLightingFile(const std::filesystem::path& scene_file);

// Irradiancia de una sonda en una direccion (la misma cuenta que el shader;
// para las pruebas y la vista del editor).
core::Vec3 probeIrradiance(const core::Vec4* probe, const core::Vec3& normal);
float probeSkyVisibility(const core::Vec4* probe, const core::Vec3& normal);

}  // namespace cramion::lighting

#endif  // CRAMION_CORE_LIGHTING_PROBE_BAKER_H
