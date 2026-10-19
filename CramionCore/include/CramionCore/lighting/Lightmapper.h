#ifndef CRAMION_CORE_LIGHTING_LIGHTMAPPER_H
#define CRAMION_CORE_LIGHTING_LIGHTMAPPER_H

// Lightmapper de CPU por texel de UV2 (PLAN-LIGHTMAPS.md, Fases 2 y 3), al
// estilo del Progressive Lightmapper de Unity: para los MeshRenderer Static y
// rigidos.
//
//   A. UV2 (xatlas, cache en Library)       E. raster de texeles (4x4 submuestras)
//   B. escala por instancia y presupuesto   F. validez (caras traseras cerca)
//   C. paginas (skyline)                    G. visibilidad del sol (disco de 0.53 grados)
//   D. geometria del trazador (BakeTracer)  H. dilatacion, I. salida, J. sondas
//
// Paginas RGBA16F: rgb = irradiancia (difuso = albedo x rgb; luces locales
// Horneadas y luz rebotada, Fase 3), a = visibilidad del sol. La luz DIRECTA
// del sol no se hornea nunca: el render la evalua en tiempo real con la
// visibilidad como sombra.
//
// Solo en el editor (Android no compila los Lightmapper*.cpp). Cuatro unidades
// de compilacion (Scene, Raster, Light, Filter) para compilar con poca RAM.

#include "CramionCore/Uuid.h"
#include "CramionCore/asset/LightmapUv.h"
#include "CramionCore/lighting/ProbeBaker.h"

#include <CramionFX/core/Math.h>
#include <CramionFX/vk/BakedGi.h>

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cramion::lighting {

struct LightmapReceiver {              // un MeshRenderer Static rigido
    Uuid entity{}, model{};
    int part = 0;
    // La de la entidad (va al registro: RenderSync avisa si se movio). La
    // instancia se hornea con world x reposo de la pieza (restPoseMatrix de la
    // malla leida), como la dibuja RenderSync.
    core::Mat4 world = core::Mat4::identity();
    float lightmap_scale = 1.0f;
    std::filesystem::path file;  // .crdata del modelo (vacio: primitiva)
    std::string name;
    std::vector<Uuid> material_overrides;  // .crmat por hueco (nulo = el del modelo)
    // Indices en BakeScene::materials por material de la pieza (los rellena
    // el editor con gatherBakeScene: kNoBakeMaterial = el color del modelo) y
    // si cada uno es transparente (vidrio, agua: ni tapa ni lleva lightmap).
    std::vector<std::uint32_t> materials;
    std::vector<std::uint8_t> transparent;
    // Sus triangulos en BakeScene::triangles (gatherBakeScene): el trazador
    // los cambia por los de la copia partida con UV2 (mismas posiciones).
    std::uint32_t scene_first = 0, scene_count = 0;
};

struct LightmapBakeSettings {
    bool enabled = true;
    float texels_per_meter = 20.0f;
    std::uint32_t max_page_size = 2048;
    std::uint32_t texel_budget = 8u << 20;
    int sun_samples = 64;              // Vista previa 16, Alta 128
    int indirect_samples = 128;        // Fase 3 (32 / 128 / 512)
    int bounces = 2;                   // rebotes de la luz rebotada (Fase 3)
    int direct_samples = 16;           // rayos de sombra por luz local Horneada y texel
    bool denoise = true;               // Fase 3
    float min_component_texels = 2.0f;
    std::uint32_t padding = 2;         // texeles entre cartas (UV2)
    int chart_iterations = 1;          // xatlas (2 en Alta)
    int threads = 0;                   // 0 = nucleos - 2 (minimo 1), prioridad por debajo de la normal
    bool probes = true;                // J: hornear tambien las sondas (las pruebas lo apagan)
};

// Ajustes predefinidos de la ventana de Iluminacion (Vista previa / Media / Alta).
enum class LightmapPreset { Preview, Medium, High };
LightmapBakeSettings lightmapPreset(LightmapPreset preset);

struct LightmapBakeInput {
    BakeScene scene;                   // luces, sky_sh, volumenes y triangulos que no llevan lightmap
    std::vector<LightmapReceiver> receivers;
    LightmapBakeSettings lightmaps;
    std::filesystem::path uv_cache;    // <proyecto>/Library/Cache/LightmapUV (vacio = sin cache)
    Uuid scene_uuid{};
    std::array<double, 3> world_origin{};
    // Pruebas: la geometria de cada receptor ya en memoria (si no, se lee
    // del .crdata con readModelGeometry). Indice = el del receptor.
    std::vector<std::shared_ptr<const asset::ModelData>> receiver_meshes;
};

struct LightmapBakeStats {
    std::size_t receivers = 0, probe_only = 0, layouts = 0, layout_cache_hits = 0;
    std::size_t pages = 0, texels = 0, valid_texels = 0, invalid_texels = 0, dilated_texels = 0;
    float texels_per_meter = 0.0f, effective_texels_per_meter = 0.0f;
    double rays = 0.0, rays_per_second = 0.0;
    double seconds_uv = 0, seconds_geometry = 0, seconds_bvh = 0, seconds_raster = 0,
           seconds_validity = 0, seconds_sun = 0, seconds_indirect = 0, seconds_denoise = 0,
           seconds_probes = 0, seconds_save = 0;
    std::vector<std::string> warnings;  // "<objeto> no cabe en una pagina", densidad bajada...
    BakeStats probes;                    // las de la etapa J
};

// Hornea lightmaps y sondas. `layouts`: las disposiciones de UV2 usadas (una
// por modelo y pieza; van al .crbake). false si se cancelo o fallo (`error`).
bool bakeLighting(const LightmapBakeInput& in, gfx::BakedLighting& out,
                  std::vector<std::pair<LightmapUvPartKey, std::shared_ptr<const LightmapUvLayout>>>& layouts,
                  BakeProgress* progress, LightmapBakeStats* stats, std::string* error);

// --- Piezas sueltas (las usan bakeLighting y las pruebas) ---

// Paquete de rectangulos en paginas de `page_size` (skyline bottom-left, por
// alto descendente) con `gap` texeles alrededor de cada uno. Cada pagina se
// recorta luego a la potencia de 2 minima (>= 256). Un rect mayor que la
// pagina se escala para caber (scaled = true).
struct LightmapRect {
    std::uint32_t width = 0, height = 0;   // pedido (texeles, sin el hueco)
    std::uint32_t page = 0, x = 0, y = 0;  // donde quedo (esquina del rect, sin el hueco)
    std::uint32_t placed_width = 0, placed_height = 0;  // tras escalar si no cabia
    bool scaled = false;
};
struct LightmapPacking {
    std::uint32_t pages = 0;
    std::vector<std::uint32_t> page_width, page_height;
    std::uint32_t width = 0, height = 0;  // del array (los maximos)
};
LightmapPacking packLightmapRects(std::vector<LightmapRect>& rects, std::uint32_t page_size, std::uint32_t gap);
// scale_offset de un rect empaquetado: (0,0) y (1,1) de su UV2 caen en sus esquinas.
core::Vec4 lightmapScaleOffset(const LightmapRect& rect, const LightmapPacking& packing);

// Float <-> half (las paginas RGBA16F).
std::uint16_t floatToHalf(float value);
float halfToFloat(std::uint16_t value);

// Muestra una pagina con filtro bilineal (como la GPU, clamp a los bordes):
// para las pruebas. uv en [0, 1] de la pagina; devuelve rgba.
core::Vec4 sampleLightmapPage(const gfx::LightmapPage& page, float u, float v);

}  // namespace cramion::lighting

#endif  // CRAMION_CORE_LIGHTING_LIGHTMAPPER_H
