#ifndef CRAMION_DEMO_SCENES_H
#define CRAMION_DEMO_SCENES_H

// Escenas de demostracion que comparten la aplicacion de ejemplo (cramion.exe)
// y el editor (CramionEditor.exe): que modelo cargar, desde donde mirar, que
// cielo, lluvia y ajustes de material lleva cada una.

#include <CramionFX/CramionFX.h>

#include <filesystem>
#include <iostream>
#include <span>

namespace cramion::demo {

// Escenarios disponibles. CMake extrae cada zip que encuentre en la raiz del
// proyecto a assets/<nombre>/ junto al ejecutable. Se elige por la linea de
// comandos (cramion.exe san-miguel); sin argumento, el primero.
// Ajuste de un material por su nombre en el MTL: lo que el OBJ no guarda.
struct MaterialTweak {
    const char* material;
    float roughness;
    float metallic;
    float reflectance = 0.04f;  // F0 de la parte no metalica
    float albedo_scale = 1.0f;  // multiplica el color base (lineal)
    // Color base (lineal) que sustituye al del MTL; negativo = el del MTL.
    cramion::core::Vec3 base_color{-1.0f, -1.0f, -1.0f};
};

struct SceneEntry {
    const char* name;
    const char* file;             // relativo a assets/
    cramion::core::Vec3 camera;   // posicion inicial de la camara
    cramion::core::Vec3 target;   // punto al que mira
    std::span<const MaterialTweak> tweaks;
    // Normal maps de convenio DirectX (+Y hacia abajo): assets de Unreal o
    // Lumberyard. El archivo no lo indica.
    bool directx_normals = false;
    // Cielo fotografiado (HDR equirectangular, relativo a assets/) que
    // sustituye al cielo fisico. nullptr = cielo fisico con ciclo de dia.
    const char* environment = nullptr;
    // Recien llovido: humedad y charcos (0 = seco).
    float wetness = 0.0f;
    float puddles = 0.0f;
    // Zona inundada: centro (x, z) y radios (x, z) de la elipse de agua.
    // Radios 0 = sin agua.
    cramion::core::Vec2 water_center{0.0f, 0.0f};
    cramion::core::Vec2 water_radii{0.0f, 0.0f};
    // Modelo extra estatico (relativo a assets/), p. ej. un suelo para un
    // objeto suelto. nullptr = ninguno.
    const char* ground = nullptr;
};

// Sibenik: el suelo de la nave es marmol pulido (en las fotos refleja las
// columnas como un espejo), pero el MTL lo exporta mate (Ns 8, sin
// especular). Casi liso y con mas reflectancia que la piedra comun (0.04):
// asi refleja tambien mirando hacia abajo, no solo en angulo rasante.
constexpr MaterialTweak kSibenikTweaks[] = {
    {"pod", 0.05f, 0.0f, 0.12f},  // marmol pulido del suelo
    {"pod_rub", 0.3f, 0.0f},   // cenefa de piedra del borde
};

// San Miguel: los cristales de las ventanas (material_0: Kd 0.1, Ns 4096,
// Ks 0.5) son opacos en el MTL (d 1), asi que van al G-buffer como una
// superficie mas. La conversion de Phong no baja de rugosidad 0.3 y los
// dejaba como un panel gris mate. Un vidrio con el interior oscuro detras es
// casi solo reflejo: espejo, sin difuso, y F0 de dos caras (lamina fina:
// 2f / (1 + f) = 0.077 con f = 0.04). El reflejo lo ponen el SSR (o los
// rayos), la sonda y el IBL, igual que en los charcos.
// (material_041, el unico vidrio transparente, son los vasos de las mesas:
// esos van por la pasada de vidrio, glass.frag.)
constexpr MaterialTweak kSanMiguelTweaks[] = {
    {"material_0", 0.02f, 0.0f, 0.077f, 0.1f},  // cristal de las ventanas
};

// Coche deportivo: MTL de 3ds Max (Phong), sin texturas. En PBR:
//   - la pintura es un dielectrico brillante con algo de metal (escamas),
//   - los metales llevan su color en Ks y Kd casi negro: el color base de un
//     metal ES su reflejo, asi que se fija a mano (F0 medidos: acero ~0.56,
//     aluminio ~0.91, plata ~0.95),
//   - los cristales (illum 4) van por la pasada de vidrio (glass.frag).
constexpr MaterialTweak kSportsCarTweaks[] = {
    // --- Carroceria ---
    {"BodyMat", 0.12f, 0.25f, 0.05f},                 // pintura azul metalizada
    {"BodyGlossBlackMat", 0.1f, 0.0f, 0.05f},         // negro brillo
    {"BodyMat_BK", 0.3f, 0.0f},
    {"CarbonBlack", 0.35f, 0.0f, 0.05f},              // fibra de carbono
    {"WheelHubColor", 0.2f, 0.5f},                    // llantas anodizadas
    {"RedMat", 0.25f, 0.0f, 0.04f, 1.0f, {0.5f, 0.02f, 0.02f}},  // pinzas de freno
    // --- Metales ---
    {"MirrorMat", 0.02f, 1.0f, 0.04f, 1.0f, {0.95f, 0.95f, 0.93f}},  // retrovisor (plata)
    {"EngineSilver2", 0.3f, 1.0f, 0.04f, 1.0f, {0.56f, 0.57f, 0.58f}},
    {"Interior_Silver", 0.35f, 1.0f, 0.04f, 1.0f, {0.6f, 0.6f, 0.6f}},
    {"BoltSilver", 0.3f, 1.0f, 0.04f, 1.0f, {0.75f, 0.75f, 0.72f}},
    {"PedalsSilver_mat", 0.35f, 1.0f, 0.04f, 1.0f, {0.8f, 0.8f, 0.8f}},
    {"SusArm_Silver2", 0.4f, 1.0f, 0.04f, 1.0f, {0.55f, 0.55f, 0.55f}},
    {"BrakeRotarySilver", 0.45f, 1.0f, 0.04f, 1.0f, {0.45f, 0.45f, 0.45f}},  // hierro
    // --- Resto ---
    {"TireMat", 0.85f, 0.0f},                         // goma
    {"Chassis_Black", 0.35f, 0.0f},
    {"Suspention_Black", 0.3f, 0.0f},
    {"Interior_Monitor", 0.05f, 0.0f},
};

constexpr SceneEntry kScenes[] = {
    // Catedral de Sibenik (Marko Dabrovic, texturas de Morgan McGuire): en la
    // nave, cerca de la entrada, a la altura de los ojos, mirando al altar.
    {"sibenik", "sibenik/sibenik.obj", {-15.5f, -13.4f, 0.0f}, {10.0f, -11.0f, 0.0f},
     kSibenikTweaks},
    // San Miguel (Guillermo M. Leal Llaguno, version 2017 de Morgan McGuire):
    // en el paso junto a la fachada sur, mirando al centro del patio.
    {"san-miguel", "san-miguel/san-miguel.obj", {10.5f, 1.7f, -10.5f}, {12.0f, 1.8f, 2.0f},
     kSanMiguelTweaks},
    // Amazon Lumberyard Bistro v5.2 (NVIDIA ORCA): la calle y el interior del
    // bistro. Texturas DDS con normal maps DirectX.
    // Su escena original (Falcor) usa como cielo san_giuseppe_bridge_4k.hdr.
    {"bistro", "bistro/Bistro_v5_2/BistroExterior.fbx", {0.0f, 2.0f, 0.0f}, {1.0f, 2.0f, 0.0f},
     {}, true, "bistro/Bistro_v5_2/san_giuseppe_bridge_4k.hdr", 1.0f, 1.0f,
     {-11.5f, 3.5f}, {3.2f, 2.2f}},
    {"bistro-interior", "bistro/Bistro_v5_2/BistroInterior.fbx", {0.0f, 1.7f, 0.0f},
     {1.0f, 1.7f, 0.0f}, {}, true, "bistro/Bistro_v5_2/san_giuseppe_bridge_4k.hdr"},
    // Coche deportivo (sportsCar.zip) sobre un suelo de hormigon, con el cielo
    // HDR de Bistro: lo ilumina y se refleja en la pintura y los cristales.
    {"sportscar", "sportscar/sportsCar.obj", {4.8f, 1.3f, 5.2f}, {0.0f, 0.55f, 0.0f},
     kSportsCarTweaks, false, "bistro/Bistro_v5_2/san_giuseppe_bridge_4k.hdr", 0.0f, 0.0f,
     {0.0f, 0.0f}, {0.0f, 0.0f}, "sportscar/ground.obj"},
};

// Carpeta assets/ junto al ejecutable (donde CMake extrae las escenas).
inline std::filesystem::path assetsDirectory() {
    return gfx::shaders::directory().parent_path() / "assets";
}

// Carga los modelos de la escena, aplica sus ajustes de material y coloca la
// camara. Antes de VulkanRenderer::uploadModels().
inline void populateScene(const SceneEntry& entry, scene::Scene& scene) {
    const std::filesystem::path scene_path = assetsDirectory() / entry.file;
    std::cout << "Escena: " << entry.name << " (" << scene_path.string() << ")\n";
    // Escenario: siempre estatico (culling por trozos aunque traiga
    // animaciones de camaras u objetos).
    const std::uint32_t model = scene.loadModel(scene_path, /*force_static=*/true);
    for (const MaterialTweak& tweak : entry.tweaks) {
        scene.overrideMaterial(model, tweak.material, tweak.roughness, tweak.metallic,
                               tweak.reflectance, tweak.albedo_scale, tweak.base_color);
    }
    scene.setDirectXNormalMaps(model, entry.directx_normals);
    scene.spawnStatic(model);
    if (entry.ground != nullptr) {
        scene.spawnStatic(scene.loadModel(assetsDirectory() / entry.ground, /*force_static=*/true));
    }
    scene.placeCamera(entry.camera, entry.target);
}

// Lluvia, agua y cielo HDR de la escena (con el renderizador ya iniciado).
// Con el HDR, el sol se coloca donde esta en la foto.
inline void configureRenderer(const SceneEntry& entry, gfx::VulkanRenderer& renderer,
                              scene::Scene& scene) {
    renderer.setWeather(entry.wetness, entry.puddles);
    renderer.setWater(entry.water_center, entry.water_radii);
    if (entry.environment != nullptr &&
        renderer.loadEnvironment(assetsDirectory() / entry.environment)) {
        scene.setFixedSun(renderer.environmentSunDirection());
    }
}

}  // namespace cramion::demo

#endif  // CRAMION_DEMO_SCENES_H
