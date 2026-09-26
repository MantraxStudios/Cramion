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
    // Vectores de movimiento (TAA, FSR, DLSS): vista-proyeccion sin jitter de
    // este frame y la del anterior.
    core::Mat4 unjittered_view_projection = core::Mat4::identity();
    core::Mat4 previous_view_projection = core::Mat4::identity();
    core::Vec4 jitter{};  // xy = desplazamiento de este frame en NDC
    // x = donde empiezan, en el buffer de huesos, las matrices del frame
    // anterior (en el mundo) de cada hueso/instancia.
    std::uint32_t motion[4] = {0, 0, 0, 0};
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
    // Picking por ID (pick.frag): indice del actor + 1 (0 = nada).
    std::uint32_t pick_id = 0;
    // Bit 0: dibujo instanciado (skinned.vert lee la matriz con gl_InstanceIndex).
    std::uint32_t flags = 0;
};

// Escalado temporal / TAA (taa.frag).
struct GpuTaaPush {
    core::Vec4 render_size{};  // ancho, alto, 1/ancho, 1/alto (interna)
    core::Vec4 output_size{};  // igual, en pantalla
    core::Vec4 jitter{};       // xy = jitter en UV, z = historia valida
    core::Mat4 reproject = core::Mat4::identity();  // NDC actual -> recorte anterior
};
static_assert(sizeof(GpuTaaPush) == 112, "GpuTaaPush debe coincidir con taa.frag");

// FSR 1 (fsr_easu.frag y fsr_rcas.frag): constantes de FsrEasuCon/FsrRcasCon.
struct GpuEasuPush {
    std::uint32_t con[16] = {};
};
struct GpuRcasPush {
    std::uint32_t con[4] = {};
    std::uint32_t bypass = 0;
    std::uint32_t pad[3] = {0, 0, 0};
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

// Ajustes de la composicion final (composite.frag), un uniform buffer por
// frame en vuelo: no caben en los 128 bytes garantizados de push constants.
// Salen de PostProcessSettings (VulkanRenderer::setPostProcess).
struct GpuCompositeSettings {
    // x = exposicion manual, y = intensidad del bloom, z = 1 auto-exposicion,
    // w = compensacion (EV)
    core::Vec4 exposure{1.0f, 0.06f, 1.0f, 0.0f};
    // x = rayos de luz, y = tonemapper (0 Neutral, 1 ACES, 2 ninguno),
    // z = saturacion, w = contraste
    core::Vec4 tone{0.6f, 0.0f, 1.12f, 1.08f};
    // x = viveza, y = vineta (intensidad), z = vineta (suavidad),
    // w = aberracion cromatica
    core::Vec4 look{0.25f, 0.35f, 0.5f, 0.0f};
    // x = grano, y = frame (para animar el grano), z/w = 1 / resolucion
    core::Vec4 film{0.0f, 0.0f, 0.0f, 0.0f};
    // rgb = balance de blancos (factores en espacio LMS), a = sin uso
    core::Vec4 white_balance{1.0f, 1.0f, 1.0f, 0.0f};
    core::Vec4 color_filter{1.0f, 1.0f, 1.0f, 0.0f};
    core::Vec4 lift{0.0f, 0.0f, 0.0f, 0.0f};
    core::Vec4 gamma{1.0f, 1.0f, 1.0f, 0.0f};
    core::Vec4 gain{1.0f, 1.0f, 1.0f, 0.0f};
    core::Vec4 vignette_color{0.0f, 0.0f, 0.0f, 0.0f};
    core::Vec4 bloom_tint{1.0f, 1.0f, 1.0f, 0.0f};
};

// Constante de push de la iluminacion global de pantalla (ssgi.frag).
struct GpuSsgiPush {
    core::Mat4 previous_view_projection = core::Mat4::identity();
    // x = hay frame anterior valido, y = intensidad.
    core::Vec4 params{0.0f, 1.0f, 0.0f, 0.0f};
    // Solo ssgi.frag: x = numero de frame, y/z = peso de cada cubo de la sonda.
    core::Vec4 extra{};
};

// Decal (estampa, charco o mancha de humedad) proyectado sobre el G-buffer
// en la pasada de geometria. Caja unidad [-0.5, 0.5]^3 en su espacio; se
// proyecta a lo largo de su eje Y local.
inline constexpr std::uint32_t kMaxDecals = 64;
inline constexpr std::uint32_t kMaxDecalTextures = 8;

struct GpuDecal {
    core::Mat4 world_to_decal = core::Mat4::identity();
    core::Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};  // rgb = tinte (sRGB), a = opacidad
    core::Vec4 axis{0.0f, 1.0f, 0.0f, 0.2f};   // xyz = eje de proyeccion (mundo), w = coseno minimo
    // x = tipo (0 estampa, 1 charco, 2 humedad), y = textura (-1 = ninguna),
    // z = suavidad del borde (0..0.5), w = cantidad (nivel del agua / humedad)
    core::Vec4 params{0.0f, -1.0f, 0.1f, 1.0f};
    // x = rugosidad, y = cuanto la sustituye (0..1), z = metalicidad, w = normal (0 = conservar)
    core::Vec4 material{0.5f, 0.0f, 0.0f, 0.0f};
};

// Lluvia de la pasada de geometria (skinned.frag): mapa de lluvia y estado.
struct GpuWeather {
    core::Mat4 rain_view_projection = core::Mat4::identity();
    // x = humedad (0..1), y = charcos (0..1), z = segundos, w = mapa listo.
    core::Vec4 params{};
    // Zona inundada: xy = centro (x, z), zw = radios (x, z); 0 = sin agua.
    core::Vec4 flood{};
    // x = numero de decals.
    core::Vec4 decal_info{};
    GpuDecal decals[kMaxDecals]{};
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
    core::Vec4 params{};          // x = numero de frame, y = densidad, zw = origen del mundo xz (mod 168 km)
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
    // Limites del ajuste de la exposicion (log2) y velocidades de adaptacion
    // (PostProcessSettings: min_ev, max_ev, adaptation_speed_up/down).
    float min_log_exposure = -2.0f;
    float max_log_exposure = 1.4f;
    float speed_up = 3.0f;
    float speed_down = 1.2f;
};

// Estado de la auto-exposicion en la GPU (exposure_average.comp). Persiste
// entre frames: de ahi sale la adaptacion gradual.
struct GpuExposureState {
    float exposure = 1.0f;
    float log_exposure = 0.0f;
    float average_luminance = 0.0f;
    float initialized = 0.0f;
};

static_assert(sizeof(GpuCompositeSettings) == 11 * 16,
              "GpuCompositeSettings debe coincidir con composite.frag (std140)");
static_assert(sizeof(GpuExposurePush) == 32, "GpuExposurePush debe coincidir con exposure_average.comp");

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

static_assert(sizeof(GpuCamera) == 6 * 64 + 16 + 16 + 16, "GpuCamera debe seguir el layout std140");
static_assert(sizeof(GpuPointLight) == 48, "GpuPointLight debe seguir el layout std140");
static_assert(sizeof(GpuSpotLight) == 64, "GpuSpotLight debe seguir el layout std140");

}  // namespace cramion::gfx

#endif  // CRAMION_VK_GPU_TYPES_H
