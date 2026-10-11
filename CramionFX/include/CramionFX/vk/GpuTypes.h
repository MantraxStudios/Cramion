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
    core::Vec4 color_intensity{};  // rgb = color (lineal), a = intensidad
    // x = hueco de sombra (-1 = sin sombra), y = fuerza, z = radio de la fuente (m)
    core::Vec4 shadow{-1.0f, 0.0f, 0.0f, 0.0f};
};

struct GpuSpotLight {
    core::Vec4 position_range{};       // xyz = posicion, w = alcance
    core::Vec4 direction_intensity{};  // xyz = direccion, w = intensidad
    core::Vec4 color_inner{};          // rgb = color (lineal), a = cos(angulo interior)
    core::Vec4 outer_shadow{};         // x = cos(angulo exterior), y = hueco de sombra (-1 = sin),
                                       // z = fuerza, w = radio de la fuente (m)
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
    // Sombra de las nubes (lighting.frag, binding 23): xy = centro del mapa
    // (x, z de la escena), z = lado (m; 0 = sin sombras), w = fuerza.
    core::Vec4 cloud_shadow{};
    // Sombras por rayos de las luces locales (rt_shadows.comp): x = 1 si la
    // mascara de este frame vale. y = vision nocturna, z = destello de un
    // rayo (ambiente), w = niebla en el horizonte del cielo (0..1).
    core::Vec4 rt_shadows{};
    // Oclusion del cielo vista desde arriba (lighting.frag, bindings 27 y 28):
    // xy = esquina del mapa (x, z), z = lado (m), w = 1 si vale este frame.
    core::Vec4 sky_map{};
    // x = altura del plano desde el que se mira (m), y = profundidad que cubre (m).
    core::Vec4 sky_map_depth{};
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
    // Bit 1: camara sin jitter. Bits 2 y 3: del material (ver abajo).
    std::uint32_t flags = 0;

    // R de metallic_roughness_map = reflectancia (mapa specular).
    static constexpr std::uint32_t kFlagSpecularMap = 1u << 2;
    // G de occlusion_map = altura (parallax occlusion mapping, emissive.w).
    static constexpr std::uint32_t kFlagHeightMap = 1u << 3;
    // Relieve teselado (skinned.tesc/.tese): la altura sube los vertices.
    static constexpr std::uint32_t kFlagTessellation = 1u << 4;
    // Auto-sombra del parallax hacia el sol.
    static constexpr std::uint32_t kFlagParallaxShadow = 1u << 5;
    // Bits 8-15: teselacion maxima del material (1..64).
    static constexpr std::uint32_t kTessFactorShift = 8;
    static constexpr std::uint32_t kTessFactorMask = 0xFFu << kTessFactorShift;
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
    // Relieve teselado (skinned_shadow.tesc/.tese): altura en metros,
    // teselacion maxima, la camara en el espacio del modelo y la escala del
    // modelo por eje. Con la escala, los bordes y las distancias se miden en
    // metros como en skinned.tesc: la sombra se parte igual que lo que ve la
    // camara (si no, la superficie de la sombra no coincide y se sombrea a
    // si misma).
    float height = 0.0f;
    float max_factor = 1.0f;
    std::uint32_t flags = 0;
    core::Vec4 camera_model{};
    core::Vec4 model_scale{1.0f, 1.0f, 1.0f, 0.0f};
};

// Un meshlet (mesh shaders): esfera, cono de normales (meshoptimizer) y
// donde estan sus vertices y triangulos. Debe coincidir con meshlet_common.glsl.
struct GpuMeshlet {
    core::Vec4 center_radius{};     // en el espacio del modelo
    core::Vec4 cone_axis_cutoff{};  // cutoff 1 = sin cono (no se descarta)
    core::Vec4 cone_apex{};
    std::uint32_t vertex_offset = 0;    // en meshlet_vertices
    std::uint32_t triangle_offset = 0;  // byte en meshlet_triangles
    std::uint32_t vertex_count = 0;
    std::uint32_t triangle_count = 0;
};
static_assert(sizeof(GpuMeshlet) == 64, "GpuMeshlet debe coincidir con meshlet_common.glsl");

// Sombras de las cascadas con mesh shaders (shadow_meshlet.task/.mesh).
struct GpuMeshletShadowPush {
    core::Mat4 light_model_view_projection = core::Mat4::identity();
    std::uint32_t first_meshlet = 0;
    std::uint32_t meshlet_count = 0;
    std::uint32_t bone_offset = 0;
    std::uint32_t flags = 0;         // bit 0: descartar los meshlets de espaldas a la luz (malla cerrada)
    core::Vec4 light_direction{};    // hacia donde van los rayos, en el espacio del modelo
};
static_assert(sizeof(GpuMeshletShadowPush) == 96, "GpuMeshletShadowPush debe coincidir con shadow_meshlet.task");

static_assert(sizeof(GpuSkinnedPush) == 128, "GpuSkinnedPush debe coincidir con skinned.vert");
static_assert(sizeof(GpuSkinnedShadowPush) == 112,
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
    // x = distorsion de la lente, y = destellos del sol, zw = sol en pantalla (UV)
    core::Vec4 lens{0.0f, 0.0f, 0.0f, 0.0f};
    // x = el sol cuenta (0..1: en pantalla y sobre el horizonte), y = aspecto,
    // z = vision nocturna (0..1), w = luminancia de adaptacion con exposicion manual
    core::Vec4 flare{0.0f, 1.0f, 0.0f, 0.0f};
};

// Constante de push del motion blur y la profundidad de campo (camera_fx.frag).
struct GpuCameraFxPush {
    // Clip actual (sin jitter) -> clip del frame anterior: movimiento del cielo.
    core::Mat4 reproject = core::Mat4::identity();
    // x = modo (0 profundidad de campo, 1 motion blur), y = intensidad del
    // motion blur, z = rastro maximo (fraccion del alto), w = frame
    core::Vec4 params{};
    // x = distancia de enfoque (m; < 0 = autoenfoque), y = numero f,
    // z = focal (mm), w = desenfoque maximo (fraccion del alto)
    core::Vec4 dof{};
    // x, y = proyeccion [3][2] y [2][2] (profundidad lineal), z = ancho / alto,
    // w = 1 / alto de la imagen
    core::Vec4 camera{};
    // x, y = proyeccion [2][3] y [3][3]: con ellos la profundidad lineal vale
    // tambien con la camara ortografica (-1, 0 en perspectiva; 0, 1 en orto).
    core::Vec4 projection_w{-1.0f, 0.0f, 0.0f, 0.0f};
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
inline constexpr std::uint32_t kMaxDecalTextures = 32;
// En el modo compatible (VulkanCompat.h, moviles): 16 texturas por shader como
// mucho, asi que 4 ranuras (gbuffer_surface.glsl, CRAMION_COMPAT).
inline constexpr std::uint32_t kCompatDecalTextures = 4;
inline std::uint32_t decalTextureSlots(bool compat) { return compat ? kCompatDecalTextures : kMaxDecalTextures; }

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
    // Nieve (sistema de ambiente): x = cobertura (0..1), y = espesor de la
    // capa acumulada (m), z = humedad al derretirse (0..1), w = reservado.
    core::Vec4 snow{};
    // Zonas de fuego (hasta kMaxFireZones): xy = esquina minima (x, z) del
    // mundo, z = lado (m), w = parte del mapa usada (resolucion / 256; 0 =
    // apagada). La capa N del mapa de quemado (binding 6 del set de la
    // geometria, FirePass) es la zona N.
    core::Vec4 fire_zones[4]{};
    GpuDecal decals[kMaxDecals]{};
};
inline constexpr std::uint32_t kMaxFireZones = 4;

// Constante de push de las nubes volumetricas (clouds.frag).
// Constante de push de la luz volumetrica (volumetric.frag).
// Volumenes de niebla locales (volumetric.frag, binding 9). Caja: lo local
// en [-0.5, 0.5]^3; esfera: radio 0.5 en local.
inline constexpr int kMaxFogVolumes = 16;
struct GpuFogVolume {
    core::Mat4 to_local = core::Mat4::identity();  // mundo -> local del volumen
    core::Vec4 color_density{1.0f, 1.0f, 1.0f, 0.0f};  // rgb = albedo (lineal), a = densidad (1/m)
    core::Vec4 params{};  // x = forma (0 caja, 1 esfera), y = borde suave (0..1), z = ruido (0..1), w = escala del ruido (1/m)
};
struct GpuFogVolumes {
    std::int32_t count[4] = {0, 0, 0, 0};
    GpuFogVolume volumes[kMaxFogVolumes]{};
};

struct GpuVolumetricPush {
    // x = densidad del polvo (1/m), y = anisotropia (g de Henyey-Greenstein),
    // z = segundos (deriva del polvo), w = distancia maxima del rayo (m).
    core::Vec4 params{};
    // x = tramos por rayo (8-32).
    core::Vec4 quality{32.0f, 0.0f, 0.0f, 0.0f};
};

struct GpuCloudPush {
    core::Vec4 to_light_time{};   // xyz = hacia la luz direccional, w = segundos
    core::Vec4 light_coverage{};  // rgb = su radiancia, a = cobertura (0..1)
    core::Vec4 params{};          // x = numero de frame, y = densidad, zw = origen del mundo + viento xz (mod 168 km)
    core::Vec4 layer{};           // x = base (m), y = cima (m), z = tipo (0 estratos .. 1 cumulonimbos)
    core::Vec4 wind{};            // xy = direccion del viento (x, z), z = inclinacion con la altura (m), w = 1 mapa de sombra
    core::Vec4 shadow{};          // xy = centro del mapa de sombra (x, z de la escena), z = lado (m), w = fuerza
    core::Vec4 flash{};           // rayo (ambiente): xyz = donde cayo (mundo), w = brillo del destello
};
static_assert(sizeof(GpuCloudPush) == 112, "GpuCloudPush debe coincidir con clouds.frag");

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

static_assert(sizeof(GpuCompositeSettings) == 13 * 16,
              "GpuCompositeSettings debe coincidir con composite.frag (std140)");
static_assert(sizeof(GpuExposurePush) == 32, "GpuExposurePush debe coincidir con exposure_average.comp");
static_assert(sizeof(GpuCameraFxPush) == 128, "GpuCameraFxPush debe coincidir con camera_fx.frag");

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
    // z = intensidad de la sombra (0 = sin sombras), w = error de los LOD de
    // la camara en metros por metro de distancia (sesgo del receptor).
    core::Vec4 params{};
};

static_assert(scene::kShadowCascadeCount == 4,
              "El shader de iluminacion espera exactamente 4 cascadas");

static_assert(sizeof(GpuCamera) == 6 * 64 + 16 + 16 + 16, "GpuCamera debe seguir el layout std140");
static_assert(sizeof(GpuPointLight) == 48, "GpuPointLight debe seguir el layout std140");
static_assert(sizeof(GpuSpotLight) == 64, "GpuSpotLight debe seguir el layout std140");

}  // namespace cramion::gfx

#endif  // CRAMION_VK_GPU_TYPES_H
