#ifndef CRAMION_VK_GPU_TYPES_H
#define CRAMION_VK_GPU_TYPES_H

#include "CramionFX/core/Math.h"
#include "CramionFX/scene/Light.h"
#include "CramionFX/scene/LocalLightShadows.h"
#include "CramionFX/scene/ShadowCascades.h"

#include <cstdint>

namespace cramion::gfx {

// Estructuras que se copian tal cual a los uniform buffers. Todos los campos
// son vec4 (o ivec4) a proposito: asi el empaquetado coincide con las reglas
// std140 de GLSL sin necesidad de relleno manual.
//
// Cualquier cambio aqui hay que reflejarlo en shaders/lighting.frag y
// shaders/geometry.vert.

struct GpuCamera {
    core::Mat4 view = core::Mat4::identity();
    core::Mat4 projection = core::Mat4::identity();
    core::Mat4 view_projection = core::Mat4::identity();
    // Deshace la proyeccion y la vista: con ella el shader de iluminacion
    // reconstruye la posicion del mundo a partir del depth, y el rayo de vision
    // para el cielo. Se invierte una vez por frame en la CPU.
    core::Mat4 inverse_view_projection = core::Mat4::identity();
    core::Vec4 position{};
};

struct GpuPointLight {
    core::Vec4 position_range{};   // xyz = posicion, w = alcance
    core::Vec4 color_intensity{};  // rgb = color, a = intensidad
    core::Vec4 shadow{-1.0f, 0.0f, 0.0f, 0.0f};  // x = hueco de sombra (-1 = sin sombra)
};

struct GpuSpotLight {
    core::Vec4 position_range{};       // xyz = posicion, w = alcance
    core::Vec4 direction_intensity{};  // xyz = direccion, w = intensidad
    core::Vec4 color_inner{};          // rgb = color, a = cos(angulo interior)
    core::Vec4 outer_shadow{};         // x = cos(angulo exterior), y = hueco de sombra (-1 = sin)
};

struct GpuLights {
    core::Vec4 sun_direction_intensity{};
    core::Vec4 sun_color_ambient{};
    core::Vec4 ambient_color{};
    // Para pintar el cielo: direccion hacia cada astro y cuanto dia hay.
    core::Vec4 sky_sun{};   // xyz = hacia el sol,  w = luz de dia (0..1)
    core::Vec4 sky_moon{};  // xyz = hacia la luna, w = crepusculo (0..1)

    std::int32_t point_count = 0;
    std::int32_t spot_count = 0;
    std::int32_t ssao_enabled = 1;  // 0 = ignorar la oclusion de pantalla
    std::int32_t gi_enabled = 1;    // 0 = sin luz rebotada (SSGI)

    GpuPointLight points[scene::kMaxPointLights]{};
    GpuSpotLight spots[scene::kMaxSpotLights]{};

    // Los dos cubos de la sonda de reflexion: xyz = centro, w = peso (0 = no
    // se usa; los dos suman 1 mientras uno se funde con el otro).
    core::Vec4 probes[2]{};

    // Nubes volumetricas: x = 1 si se componen sobre el cielo.
    core::Vec4 clouds{};

    // Mapa de entorno HDR: x = 1 si sustituye al cielo fisico.
    core::Vec4 environment{};

    // Lluvia, para los rayos (rt_common.glsl): los charcos que ven los
    // reflejos fuera de pantalla. Igual que GpuWeather.params y .flood.
    core::Vec4 rain{};   // x = humedad, y = charcos, z = segundos
    core::Vec4 flood{};  // xy = centro (x, z), zw = radios (0 = sin agua)
};

// Constante de push de los modelos con esqueleto (pasada de geometria). Son
// exactamente 128 bytes, el minimo que garantiza Vulkan.
struct GpuSkinnedPush {
    core::Mat4 model = core::Mat4::identity();
    core::Vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f};
    core::Vec4 emissive{0.0f, 0.0f, 0.0f, 0.0f};  // rgb = factor de emision
    // x = metalicidad, y = rugosidad, z = fuerza de la oclusion, w = escala
    // del normal map (negativa si es de convenio DirectX).
    core::Vec4 material{0.0f, 0.8f, 1.0f, 1.0f};
    // Primer hueso de esta instancia dentro del storage buffer compartido.
    std::uint32_t bone_offset = 0;
    // F0 de la parte no metalica (va al alfa del G-buffer de normales).
    float reflectance = 0.04f;
    std::uint32_t pad[2] = {0, 0};
};

// Constante de push de las sombras de los modelos con esqueleto.
struct GpuSkinnedShadowPush {
    core::Mat4 light_model_view_projection = core::Mat4::identity();
    std::uint32_t bone_offset = 0;
    std::uint32_t pad[3] = {0, 0, 0};
};

static_assert(sizeof(GpuSkinnedPush) == 128, "GpuSkinnedPush debe coincidir con skinned.vert");
static_assert(sizeof(GpuSkinnedShadowPush) == 80,
              "GpuSkinnedShadowPush debe coincidir con skinned_shadow.vert");

// Constante de push del post-proceso: tamano de pixel e interruptor de FXAA.
struct GpuPostProcessPush {
    core::Vec2 inverse_resolution{};
    float enabled = 1.0f;
    float unused = 0.0f;
};

// Constante de push de las pasadas de bloom (bloom_down.frag / bloom_up.frag).
struct GpuBloomPush {
    core::Vec2 source_texel{};  // 1 / resolucion de la imagen que se lee
    float first_level = 0.0f;   // bajada: 1 = promedio de Karis
    float radius = 1.0f;        // subida: separacion del filtro de tienda
};

// Constante de push de la composicion final (composite.frag).
struct GpuCompositePush {
    float exposure = 1.0f;  // exposicion manual (auto-exposicion apagada)
    float bloom_strength = 0.05f;
    float vignette = 0.35f;
    float saturation = 1.05f;
    float auto_exposure_enabled = 1.0f;
    float exposure_compensation = 0.0f;  // en EV
    float light_shaft_strength = 1.0f;
    float tonemapper = 0.0f;  // 0 = Khronos PBR Neutral, 1 = ACES
};

// Constante de push de la iluminacion global de pantalla (ssgi.frag).
struct GpuSsgiPush {
    core::Mat4 previous_view_projection = core::Mat4::identity();
    // x = hay frame anterior valido, y = intensidad.
    core::Vec4 params{0.0f, 1.0f, 0.0f, 0.0f};
    // Solo ssgi.frag: x = numero de frame, y/z = peso de cada cubo de la sonda.
    core::Vec4 extra{};
};

// Lluvia de la pasada de geometria (skinned.frag): mapa de lluvia y estado.
struct GpuWeather {
    core::Mat4 rain_view_projection = core::Mat4::identity();
    // x = humedad (0..1), y = charcos (0..1), z = segundos, w = mapa listo.
    core::Vec4 params{};
    // Zona inundada: xy = centro (x, z), zw = radios (x, z); 0 = sin agua.
    core::Vec4 flood{};
};

// Constante de push de las nubes volumetricas (clouds.frag).
// Constante de push de la luz volumetrica (volumetric.frag).
struct GpuVolumetricPush {
    // x = densidad del polvo (1/m), y = anisotropia (g de Henyey-Greenstein),
    // z = segundos (deriva del polvo), w = distancia maxima del rayo (m).
    core::Vec4 params{};
};

struct GpuCloudPush {
    core::Vec4 to_light_time{};   // xyz = hacia la luz direccional, w = segundos
    core::Vec4 light_coverage{};  // rgb = su radiancia, a = cobertura (0..1)
    core::Vec4 params{};          // x = numero de frame, y = densidad
};

// Constante de push de la LUT del cielo (sky_lut.frag).
struct GpuSkyPush {
    core::Vec4 sun{};   // xyz = hacia el sol,  w = iluminancia
    core::Vec4 moon{};  // xyz = hacia la luna, w = iluminancia
};

// Constante de push de los rayos de luz (light_shafts.frag).
struct GpuLightShaftPush {
    core::Vec2 sun_uv{};
    float intensity = 0.0f;
    float aspect = 1.0f;
};

// Constante de push del promedio de la auto-exposicion.
struct GpuExposurePush {
    float delta_seconds = 0.0f;
    float low_percent = 0.5f;
    float high_percent = 0.92f;
    float unused = 0.0f;
};

// Estado de la auto-exposicion en la GPU (exposure_average.comp). Persiste
// entre frames: de ahi sale la adaptacion gradual.
struct GpuExposureState {
    float exposure = 1.0f;
    float log_exposure = 0.0f;
    float average_luminance = 0.0f;
    float initialized = 0.0f;
};

static_assert(sizeof(GpuCompositePush) == 32, "GpuCompositePush debe coincidir con composite.frag");

// Datos de las cascadas para el shader de iluminacion.
struct GpuShadows {
    core::Mat4 light_view_projection[scene::kShadowCascadeCount]{};

    // Distancia de vista donde acaba cada cascada.
    core::Vec4 split_distances{};
    // Tamano en el mundo de un texel de cada cascada (para el sesgo).
    core::Vec4 texel_world_sizes{};

    // x = resolucion del mapa, y = intensidad de la sombra (0 = sin sombras),
    // z = pintar el color de cada cascada, w = ancho de la mezcla entre ellas.
    core::Vec4 params{};
};

// Sombras de las luces locales. Los huecos los indica cada luz (campo
// `shadow` de GpuPointLight, `outer_shadow.y` de GpuSpotLight).
struct GpuLocalShadows {
    core::Mat4 spot_view_projection[scene::kMaxShadowedSpotLights]{};
    // Seis matrices por luz puntual: indice = hueco * 6 + cara.
    core::Mat4 point_view_projection[scene::kMaxShadowedPointLights *
                                     scene::kPointShadowFaceCount]{};

    // x = tamano de texel en el mundo por unidad de distancia a la luz.
    core::Vec4 spot_params[scene::kMaxShadowedSpotLights]{};
    core::Vec4 point_params[scene::kMaxShadowedPointLights]{};

    // x = resolucion de los focos, y = resolucion de las puntuales,
    // z = intensidad de la sombra (0 = sin sombras).
    core::Vec4 params{};
};

static_assert(scene::kShadowCascadeCount == 4,
              "El shader de iluminacion espera exactamente 4 cascadas");

static_assert(sizeof(GpuCamera) == 4 * 64 + 16, "GpuCamera debe seguir el layout std140");
static_assert(sizeof(GpuPointLight) == 48, "GpuPointLight debe seguir el layout std140");
static_assert(sizeof(GpuSpotLight) == 64, "GpuSpotLight debe seguir el layout std140");

}  // namespace cramion::gfx

#endif  // CRAMION_VK_GPU_TYPES_H
