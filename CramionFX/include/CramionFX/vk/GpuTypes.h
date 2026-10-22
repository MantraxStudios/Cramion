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

// Teselas de las luces puntuales: como mucho 64 x 36 (la pantalla 16:9 en
// teselas de 30 px a 1080p; de 18 px a 1129x624). Medido en la Vega 7: en el
// UBO de las luces y con 64 x 36 la iluminacion va mas rapida (5,3 -> 4,9 ms)
// que en un binding aparte o con 32 x 18 (5,2 ms).
inline constexpr std::uint32_t kMaxLightTileColumns = 64;
inline constexpr std::uint32_t kMaxLightTileRows = 36;
inline constexpr std::uint32_t kMaxLightTiles = kMaxLightTileColumns * kMaxLightTileRows;
static_assert(scene::kMaxPointLights <= 32, "un bit por luz puntual en cada tesela");

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
    // w = 1 si el sol esta horneado entero (su directa va en la GI horneada).
    core::Vec4 sky_map_depth{};
    // Confianza (0..1) en cada cubo de la sonda (x = cubo 0, y = cubo 1): baja
    // si la camara se ha ido a donde ese cubo no ve (lo capturado es de otra
    // habitacion). lighting.frag la multiplica por el peso de probes[]; lo
    // que falta lo pone el cielo. (El vidrio, el agua y los liquidos usan solo
    // el fundido: su entorno no sabe del techo y daba destellos de cielo.)
    core::Vec4 probe_trust{1.0f, 1.0f, 0.0f, 0.0f};
    // Luces puntuales por teselas (solo lighting.frag): la imagen, a la
    // resolucion interna, en light_tiles[2] x light_tiles[3] teselas de
    // light_tiles[0] x light_tiles[1] pixeles. Cada tesela tiene un bit por
    // luz puntual (indice en points[]) cuya esfera puede caer en ella; el
    // shader solo recorre esas. light_tiles[2] = 0: sin teselas (todas las
    // luces en todos los pixeles, como antes). En std140, uvec4 light_tiles y
    // uvec4 point_tiles[kMaxLightTiles / 4].
    std::uint32_t light_tiles[4]{};
    std::uint32_t point_tiles[kMaxLightTiles]{};
};
static_assert(kMaxLightTiles % 4 == 0, "point_tiles va en uvec4 en lighting.frag");

// --- Luces locales por clusteres (light_clusters.glsl: lighting.frag,
// bindings 29 y 30; glass.frag, set 2, bindings 9 y 10) ---
// Las principales (las primeras kMaxPointLights puntuales y kMaxSpotLights
// focos que tocan la vista, ya ordenadas por cercania) siguen en GpuLights.
// Todas las demas que tocan la vista, sin tope, van aqui: la vista partida en
// columnas x filas de pantalla x rebanadas de profundidad (exponenciales,
// como Doom 2016 y Godot Forward+) y en cada cluster la lista de las que
// pueden llegar a el. Sin sombras por rayos ni niebla volumetrica (si con su
// hueco de mapa de sombra, si lo tienen). Lo reparte la CPU
// (VulkanRenderer::buildLightClusters): en Ashbound, de ~1300 luces del mapa
// las que se ven son unos cientos y cada una cae en pocos clusteres.
inline constexpr std::uint32_t kLightClusterColumns = 16;
inline constexpr std::uint32_t kLightClusterRows = 9;
inline constexpr std::uint32_t kLightClusterSlices = 24;
inline constexpr std::uint32_t kLightClusterCount = kLightClusterColumns * kLightClusterRows * kLightClusterSlices;

// Una luz local (puntual o foco) del binding 29. std430.
struct GpuClusterLight {
    core::Vec4 position_range{};    // xyz = posicion, w = alcance
    core::Vec4 color_intensity{};   // rgb = color (lineal), a = intensidad
    core::Vec4 direction_inner{};   // foco: xyz = direccion, w = cos(angulo interior)
    // x = cos(angulo exterior) (< -1.5 = puntual: sin cono), y = hueco de
    // sombra (-1 = sin; de los focos o de las puntuales segun x), z = fuerza
    // de la sombra, w = radio de la fuente (m).
    core::Vec4 params{-2.0f, -1.0f, 0.0f, 0.0f};
};
static_assert(sizeof(GpuClusterLight) == 64, "GpuClusterLight debe coincidir con light_clusters.glsl");

// Cabecera del binding 29 (delante de las luces). El binding 30 lleva, por
// cluster, dos uint (primer indice y cuantas) y detras los indices de las
// luces (en el binding 29) de todos los clusteres seguidos.
struct GpuClusterHeader {
    // x = columnas, y = filas, z = rebanadas, w = numero de luces (0 = ninguna).
    std::uint32_t grid[4]{};
    // xy = teselas por pixel (1 / pixeles de una tesela; 0 = una sola tesela),
    // z, w = rebanada = log2(profundidad) * z + w.
    core::Vec4 tile{};
    // xyz = hacia donde mira la camara (unitario), w = dot(eje, camara): la
    // profundidad de un punto es dot(eje, p) - w.
    core::Vec4 view_axis{};
    core::Vec4 reserved{};
};
static_assert(sizeof(GpuClusterHeader) == 64, "GpuClusterHeader debe coincidir con light_clusters.glsl");

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
    // El modelo tiene UV de lightmap (set 1, binding 10: una por vertice). Solo
    // en los dibujos de lo estatico (lotes y per_submesh): el hueco de la
    // instancia es el de su matriz en el buffer de huesos.
    static constexpr std::uint32_t kFlagLightmap = 1u << 6;
    // El modelo tiene color de vertice (set 1, binding 11: RGBA8 por vertice):
    // los vertices lo leen y lo pasan (v_color); si no, blanco.
    static constexpr std::uint32_t kFlagVertexColor = 1u << 7;
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
    // x = peso maximo del frame nuevo (render.taa.FrameWeight; 0,12), y = peso
    // con la mascara reactiva llena (render.taa.ReactiveWeight), z = fuerza de
    // la mascara (render.taa.ReactiveStrength; 0 = sin mascara), w = libre.
    core::Vec4 params{0.12f, 0.5f, 1.0f, 0.0f};
};
static_assert(sizeof(GpuTaaPush) == 128, "GpuTaaPush debe coincidir con taa.frag");

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
    // Alfa del color base del material (skinned_shadow.frag): el recorte de
    // la sombra usa textura x factor, como el pase principal. Negativo = sin
    // recorte por alfa (lo opaco que va por esta pipeline solo por `opacity`).
    float alpha_factor = 1.0f;
    // Opacidad por instancia (MeshRenderer.alpha): por debajo de 1 la sombra
    // se disuelve con el mismo tramado que el G-buffer (skinned_shadow.frag).
    float opacity = 1.0f;
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
static_assert(sizeof(GpuSkinnedShadowPush) == 120,
              "GpuSkinnedShadowPush debe coincidir con skinned_shadow.vert");

// Constante de push del post-proceso (fxaa.frag): tamano de pixel,
// interruptor de FXAA y nitidez adaptativa (0..1).
struct GpuPostProcessPush {
    core::Vec2 inverse_resolution{};
    float enabled = 1.0f;
    float sharpen = 0.0f;
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
    // x = rayos de luz, y = tonemapper (0 Neutral, 1 ACES, 2 ninguno, 3 AgX, 4 AgX Punchy),
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
    // z, w = muestras de la profundidad de campo y del motion blur (0 = las
    // de siempre, 64 y 24; las baja el presupuesto adaptativo).
    core::Vec4 projection_w{-1.0f, 0.0f, 0.0f, 0.0f};
};

// Constante de push de la iluminacion global de pantalla (ssgi.frag). La
// comparten la GI horneada y los reflejos (ssr.frag, ssr_resolve.frag), que
// solo declaran el principio del bloque.
struct GpuSsgiPush {
    core::Mat4 previous_view_projection = core::Mat4::identity();
    // x = hay frame anterior valido, y = intensidad. (ssr.frag: z/w = niveles
    // de sus piramides.)
    core::Vec4 params{0.0f, 1.0f, 0.0f, 0.0f};
    // Solo ssgi.frag: x = numero de frame, y/z = peso de cada cubo de la sonda.
    core::Vec4 extra{};
    // Solo ssgi.frag: donde se capturo cada cubo (xyz). Desde ahi se mira la
    // sonda hacia un punto golpeado que no estaba en el frame anterior.
    core::Vec4 probe_center[2]{};
};
static_assert(sizeof(GpuSsgiPush) == 128, "GpuSsgiPush: 128 bytes como mucho (minimo garantizado de push constants)");

// Decal (estampa, charco o mancha de humedad) proyectado sobre el G-buffer
// en la pasada de geometria. Caja unidad [-0.5, 0.5]^3 en su espacio; se
// proyecta a lo largo de su eje Y local. Van en un storage buffer propio
// (GpuDecalBuffer, binding 9 del set de la geometria) y sus imagenes en el
// atlas de decals (DecalAtlas.h, binding 4): capa = indice.
inline constexpr std::uint32_t kMaxDecals = 256;
// Teselas de pantalla de los decals (GpuDecalBuffer: masks).
inline constexpr std::uint32_t kDecalTileColumns = 16;
inline constexpr std::uint32_t kDecalTileRows = 9;
inline constexpr std::uint32_t kDecalTileCount = kDecalTileColumns * kDecalTileRows;
// Un bit por decal en cada tesela: kMaxDecals / 32 uint por tesela.
inline constexpr std::uint32_t kDecalMaskWords = kMaxDecals / 32;
static_assert(kMaxDecals % 32 == 0, "un uint por cada 32 decals");
// Atlas: lado de cada capa y tope de capas (el modo compatible, moviles, mas
// pequeno). Crece al doble segun se cargan imagenes.
inline constexpr std::uint32_t kDecalAtlasLayerSize = 1024;
inline constexpr std::uint32_t kDecalAtlasMaxLayers = 64;
inline constexpr std::uint32_t kCompatDecalAtlasLayerSize = 512;
inline constexpr std::uint32_t kCompatDecalAtlasMaxLayers = 32;
// Imagenes distintas de decal (ranuras de VulkanRenderer::loadDecalTexture:
// cada una acaba en una capa del atlas).
inline constexpr std::uint32_t kMaxDecalTextures = kDecalAtlasMaxLayers;

struct GpuDecal {
    core::Mat4 world_to_decal = core::Mat4::identity();
    core::Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};  // rgb = tinte (sRGB), a = opacidad
    core::Vec4 axis{0.0f, 1.0f, 0.0f, 0.2f};   // xyz = eje de proyeccion (mundo), w = coseno minimo
    // x = tipo (0 estampa, 1 charco, 2 humedad), y = capa del color en el
    // atlas (-1 = ninguna), z = suavidad del borde (0..0.5), w = cantidad
    // (nivel del agua / humedad)
    core::Vec4 params{0.0f, -1.0f, 0.1f, 1.0f};
    // x = rugosidad, y = cuanto la sustituye (0..1), z = metalicidad, w =
    // fuerza del normal map (negativa: verde invertido, convencion DirectX)
    core::Vec4 material{0.5f, 0.0f, 0.0f, 1.0f};
    // Capas del atlas (-1 = ninguna): x = normal map, y = ORM (r oclusion, g
    // rugosidad, b metalicidad), z = emision; w = reservado.
    core::Vec4 channels{-1.0f, -1.0f, -1.0f, 0.0f};
    // rgb = emision (HDR lineal: color x intensidad; 0 = sin emision), a = reservado.
    core::Vec4 emission{};
};
static_assert(sizeof(GpuDecal) == 160, "GpuDecal debe coincidir con gbuffer_surface.glsl (std430)");

// Storage buffer de los decals (gbuffer_surface.glsl, DecalBuffer, std430).
// x = numero de decals, y = columnas, z = filas (0 = sin teselas: todos en
// todos los pixeles; sondas, Render Textures, VR), w = uint por tesela.
struct GpuDecalHeader {
    std::uint32_t count = 0;
    std::uint32_t tile_columns = 0;
    std::uint32_t tile_rows = 0;
    std::uint32_t mask_words = kDecalMaskWords;
    core::Vec4 tiles{};  // xy = teselas por pixel
};
static_assert(sizeof(GpuDecalHeader) == 32);
// Detras de la cabecera: GpuDecal decals[kMaxDecals] y uint masks[kDecalTileCount
// * kDecalMaskWords] (por tesela, un bit por decal cuya caja proyectada puede
// caer en ella; el pixel solo prueba esos).
inline constexpr std::size_t kDecalBufferDecalsOffset = sizeof(GpuDecalHeader);
inline constexpr std::size_t kDecalBufferMasksOffset = kDecalBufferDecalsOffset + sizeof(GpuDecal) * kMaxDecals;
inline constexpr std::size_t kDecalBufferBytes =
    kDecalBufferMasksOffset + sizeof(std::uint32_t) * kDecalTileCount * kDecalMaskWords;

// Lluvia de la pasada de geometria (skinned.frag): mapa de lluvia y estado.
struct GpuWeather {
    core::Mat4 rain_view_projection = core::Mat4::identity();
    // x = humedad (0..1), y = charcos (0..1), z = segundos, w = mapa listo.
    core::Vec4 params{};
    // Zona inundada: xy = centro (x, z), zw = radios (x, z); 0 = sin agua.
    core::Vec4 flood{};
    // x = numero de decals (los datos van en GpuDecalBuffer); yzw = hacia el
    // sol (parallax).
    core::Vec4 decal_info{};
    // Nieve (sistema de ambiente): x = cobertura (0..1), y = espesor de la
    // capa acumulada (m), z = humedad al derretirse (0..1), w = reservado.
    core::Vec4 snow{};
    // Zonas de fuego (hasta kMaxFireZones): xy = esquina minima (x, z) del
    // mundo, z = lado (m), w = parte del mapa usada (resolucion / 256; 0 =
    // apagada). La capa N del mapa de quemado (binding 6 del set de la
    // geometria, FirePass) es la zona N.
    core::Vec4 fire_zones[4]{};
    // (Los decals estaban aqui, 64 como mucho: ahora en su storage buffer.)
    // Lightmaps (gbuffer_surface.glsl; al final: los shaders que leen solo el
    // principio del bloque no cambian). x = 0 sin lightmaps, 1 solo la
    // visibilidad del sol, 2 con irradiancia (Fase 3); y = fuerza de la
    // sombra horneada del sol (0 = sin sombras del sol); z = vista de
    // depuracion (1 irradiancia, 2 visibilidad del sol, 3 UV2/densidad).
    // (PLAN-LIGHTMAPS 2.5 lo ponia en decal_info.yzw, que ya usa el parallax.)
    core::Vec4 lightmap{};
};
static_assert(sizeof(GpuWeather) <= 16384, "GpuWeather: uniform buffer de 16 KB como mucho (minimo de Vulkan)");
inline constexpr std::uint32_t kMaxFireZones = 4;

// Registro de una instancia (set 0, binding 8, std430): mismo hueco que su
// matriz en el buffer de huesos. Lightmap: layer = capa del atlas
// (0xFFFFFFFF = sin lightmap); uv_atlas = uv2 * scale_offset.xy + .zw.
// Parametros por instancia (MeshRenderer, lightmap_common.glsl): tint =
// RGBA8 (rgb = tinte del color base, a = opacidad; 0xFFFFFFFF = sin cambio),
// emission = escala de la emision en half float (0x3C00 = 1), flags: bit 0 =
// no recibe decals. data = los vec4 propios de la instancia
// (MeshRenderer.instance_data, INSTANCE_DATA(i) en los .crshader). Los lee el
// fragment shader en el hueco que le pasa el de vertices (v_instance_slot):
// gl_InstanceIndex en los lotes, bone_offset en los demas.
inline constexpr std::uint32_t kInstanceDataCount = 2;
struct GpuLightmapRecord {
    core::Vec4 scale_offset{1.0f, 1.0f, 0.0f, 0.0f};
    std::uint32_t layer = 0xFFFFFFFFu;
    std::uint32_t tint = 0xFFFFFFFFu;
    std::uint32_t emission = 0x3C00u;
    std::uint32_t flags = 0;
    core::Vec4 data[kInstanceDataCount]{};
    static constexpr std::uint32_t kFlagNoDecals = 1u << 0;
};
static_assert(sizeof(GpuLightmapRecord) == 64, "GpuLightmapRecord debe coincidir con lightmap_common.glsl y glass.frag");

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
    // x = tramos por rayo (8-32), y = 1 sin luces locales (solo el sol y el cielo).
    core::Vec4 quality{32.0f, 0.0f, 0.0f, 0.0f};
};

struct GpuCloudPush {
    core::Vec4 to_light_time{};   // xyz = hacia la luz direccional, w = segundos
    core::Vec4 light_coverage{};  // rgb = su radiancia, a = cobertura (0..1)
    core::Vec4 params{};          // x = numero de frame, y = densidad, zw = origen del mundo + viento xz (mod 168 km)
    // x = base (m), y = cima (m), z = tipo (0 estratos .. 1 cumulonimbos),
    // w = pasos maximos por rayo (0 = 128; los de la luz salen de ahi)
    core::Vec4 layer{};
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
    // Ventana de la medicion (AutoExposureMath.h: kLowPercent/kHighPercent).
    // 0.35..0.96 en vez de 0.5..0.92: de noche una zona iluminada por una
    // farola entra en la media de forma gradual, no de golpe.
    float low_percent = 0.35f;
    float high_percent = 0.96f;
    float reset = 0.0f;  // 1 = corte de camara: sin transicion
    // Limites del ajuste de la exposicion (log2) y velocidades de adaptacion
    // (PostProcessSettings: min_ev, max_ev, adaptation_speed_up/down).
    float min_log_exposure = -2.0f;
    float max_log_exposure = 1.4f;
    float speed_up = 3.0f;
    float speed_down = 1.2f;
    // 0 de dia .. 1 de noche (1 - luz del dia): la rodilla y la luz fuerte de
    // AutoExposureMath.h solo actuan de noche.
    float night = 0.0f;
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
static_assert(sizeof(GpuExposurePush) == 36, "GpuExposurePush debe coincidir con exposure_average.comp");
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
