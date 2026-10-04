#version 450
#extension GL_GOOGLE_include_directive : require

// Pasada de iluminacion diferida: lee el G-buffer y acumula todas las luces de
// la escena sobre cada pixel. El coste ya no depende de la geometria, solo de
// la resolucion y del numero de luces.
//
// La posicion del mundo NO viene del G-buffer: se reconstruye a partir del
// depth de 32 bits deshaciendo view-projection. Guardarla en un attachment
// RGBA16F daba saltos de ~0.03 unidades, y como la atenuacion depende de la
// distancia al cuadrado, esos saltos se veian como bandas en los degradados.
//
// El modelo de luz es fisico (PBR metal/rugosidad, como Unreal y glTF):
// difuso lambertiano + especular de Cook-Torrance con GGX, Smith y Fresnel de
// Schlick. Los metales no tienen difuso y tiñen el reflejo con su color.
//
// El ambiente es IBL (image based lighting) del entorno: cielo fisico + suelo,
// regenerado cada frame (IblProbe). El difuso sale de la irradiancia en
// armonicos esfericos; el especular, del cubo prefiltrado por rugosidad y la
// LUT de la BRDF (split-sum de Karis). Todo ello se ocluye con la AO (del
// material) y el SSAO, con rebote multiple.
//
// La luz rebotada entre superficies (GI) sale de ssgi.frag: se suma al
// difuso y su visibilidad de cielo ocluye el IBL a gran escala.
//
// La salida es HDR lineal (RGBA16F): el tono, la exposicion, el bloom y la
// gamma se aplican despues, en composite.frag.

// Deben coincidir con scene::kMaxPointLights, kMaxSpotLights,
// kShadowCascadeCount, kMaxShadowedSpotLights, kMaxShadowedPointLights y
// kPointShadowFaceCount.
const int kMaxPointLights = 32;
const int kMaxSpotLights = 8;
const int kShadowCascadeCount = 4;
const int kMaxShadowedSpotLights = 8;
const int kMaxShadowedPointLights = 8;
const int kPointShadowFaceCount = 6;

struct PointLightGpu {
    vec4 position_range;    // xyz = posicion, w = alcance
    vec4 color_intensity;   // rgb = color,    a = intensidad
    vec4 shadow;            // x   = hueco de sombra (-1 = sin sombra), y = fuerza (0..1)
};

struct SpotLightGpu {
    vec4 position_range;       // xyz = posicion,  w = alcance
    vec4 direction_intensity;  // xyz = direccion, w = intensidad
    vec4 color_inner;          // rgb = color,     a = cos del angulo interior
    vec4 outer_shadow;         // x   = cos del angulo exterior, y = hueco de sombra (-1 = sin), z = fuerza
};

layout(set = 0, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
} camera;

layout(set = 0, binding = 1) uniform sampler2D g_albedo;
layout(set = 0, binding = 2) uniform sampler2D g_normal;
layout(set = 0, binding = 3) uniform sampler2D g_depth;

layout(set = 0, binding = 4) uniform LightBuffer {
    vec4 sun_direction_intensity;  // xyz = direccion de los rayos, w = intensidad
    vec4 sun_color_ambient;        // rgb = color del sol,          a = intensidad ambiental
    vec4 ambient_color;            // rgb = color ambiental
    vec4 sky_sun;                  // xyz = hacia el sol,  w = luz de dia (0..1)
    vec4 sky_moon;                 // xyz = hacia la luna, w = crepusculo (0..1)
    ivec4 counts;                 // x = puntuales, y = focos, z = SSAO, w = GI
    PointLightGpu points[kMaxPointLights];
    SpotLightGpu spots[kMaxSpotLights];
    vec4 probes[2];                // cubos de la sonda: xyz = centro, w = peso (0 = sin usar)
    vec4 clouds;                   // x = 1 si hay nubes volumetricas; y, z = niebla (densidad, caida)
    vec4 environment;              // z = largo de las sombras de contacto (m, 0 = no); x = 1 si el cielo es el mapa HDR (environment_hdr),
                                   // y = 1 si hay luz volumetrica (volumetric_map)
    vec4 rain;                     // (rt_common.glsl)
    vec4 flood;
    vec4 cloud_shadow;             // xy = centro del mapa de sombra de las nubes (x, z), z = lado (m; 0 = no), w = fuerza
    vec4 rt_shadows;               // x = 1 luces locales + 2 sol si rt_shadow_mask vale, y = vision nocturna (0..0.9)
    vec4 sky_map;                  // oclusion del cielo desde arriba: xy esquina (x, z), z lado (m), w = 1 si vale
    vec4 sky_map_depth;            // x = altura desde la que se mira (m), y = profundidad que cubre (m)
} lights;

// Mapa de sombras en cascada. El muestreador compara por hardware: devuelve
// directamente "cuanta luz llega", ya filtrado bilinealmente (PCF 2x2 gratis).
layout(set = 0, binding = 5) uniform sampler2DArrayShadow shadow_map;
// Las mismas cascadas sin comparar (profundidad guardada): la busqueda de lo
// que tapa de las sombras suaves (PCSS).
layout(set = 0, binding = 24) uniform sampler2DArray shadow_depth;
// Sombras por rayos de las luces locales (rt_shadows.comp): por pixel, las 4
// luces que mas aportan, cada canal = (luz + 1) * 32 + visibilidad (0..31).
layout(set = 0, binding = 25) uniform sampler2D rt_shadow_mask;

layout(set = 0, binding = 6) uniform ShadowBuffer {
    mat4 light_view_projection[kShadowCascadeCount];
    vec4 split_distances;    // donde acaba cada cascada, en distancia de vista
    vec4 texel_world_sizes;  // cuanto mide un texel de cada cascada en el mundo
    vec4 params;             // x = resolucion, y = intensidad, z = depurar, w = mezcla
} shadows;

// Sombras de las luces locales. Mismo tipo de muestreador que las cascadas:
// los focos tienen una capa cada uno y las luces puntuales seis consecutivas
// (una por cara del cubo, capa = hueco * 6 + cara).
layout(set = 0, binding = 7) uniform sampler2DArrayShadow spot_shadow_maps;
layout(set = 0, binding = 8) uniform sampler2DArrayShadow point_shadow_maps;

layout(set = 0, binding = 9) uniform LocalShadowBuffer {
    mat4 spot_view_projection[kMaxShadowedSpotLights];
    mat4 point_view_projection[kMaxShadowedPointLights * kPointShadowFaceCount];
    vec4 spot_params[kMaxShadowedSpotLights];    // x = texel en el mundo por unidad de distancia
    vec4 point_params[kMaxShadowedPointLights];  // x = texel por unidad de distancia, y = fundido
    vec4 params;  // x = resolucion focos, y = resolucion puntuales, z = intensidad
} local_shadows;

// Oclusion ambiental de pantalla: r = AO, g = profundidad lineal de vista.
layout(set = 0, binding = 10) uniform sampler2D ssao_map;

// Radiancia del cielo en todas las direcciones (sky_lut.frag).
layout(set = 0, binding = 11) uniform sampler2D sky_lut;

// rgb = emision (radiancia HDR lineal), a = metalicidad.
layout(set = 0, binding = 12) uniform sampler2D g_material;

// --- IBL ---
// Entorno prefiltrado: el mip k corresponde a rugosidad k / (mips - 1).
layout(set = 0, binding = 13) uniform samplerCube environment_map;
// Split-sum: (escala, sesgo) de F0 para cada (NdotV, rugosidad).
layout(set = 0, binding = 14) uniform sampler2D brdf_lut;
// Irradiancia difusa en armonicos esfericos, ya dividida por pi.
layout(set = 0, binding = 15) readonly buffer Irradiance {
    vec4 coefficients[9];
} irradiance_sh;

// Iluminacion global de pantalla, a media resolucion: rgb = luz rebotada,
// a = visibilidad del cielo.
layout(set = 0, binding = 16) uniform sampler2D gi_map;

// Reflejos de pantalla (ssr.frag): rgb = color reflejado, a = confianza.
layout(set = 0, binding = 17) uniform sampler2D ssr_map;

// Sonda de reflexion (ReflectionProbe): la escena vista desde lights.probes[i].xyz,
// prefiltrada por rugosidad como environment_map. a = distancia de la sonda a
// lo que se ve en cada direccion. Dos cubos: la captura nueva se funde con la
// anterior.
layout(set = 0, binding = 18) uniform samplerCube reflection_probe_0;
layout(set = 0, binding = 19) uniform samplerCube reflection_probe_1;

// Nubes volumetricas (clouds.frag), a media resolucion: rgb = luz dispersada,
// a = transmitancia (cuanto cielo de detras se ve).
layout(set = 0, binding = 20) uniform sampler2D clouds_map;

// Mapa de entorno HDR equirectangular (EnvironmentMap), si la escena trae uno:
// sustituye al cielo fisico. u = atan(z, x) / 2pi + 0.5, v = acos(y) / pi.
layout(set = 0, binding = 21) uniform sampler2D environment_hdr;

// Luz volumetrica (volumetric.frag), a media resolucion: rgb = luz del sol
// dispersada por el polvo hacia la camara, a = transmitancia.
layout(set = 0, binding = 22) uniform sampler2D volumetric_map;

// Sombra de las nubes (clouds.frag en modo mapa de sombra): r = cuanta luz del
// sol pasa las nubes, sobre un cuadrado del suelo centrado en la camara.
layout(set = 0, binding = 23) uniform sampler2D cloud_shadow_map;
// Modelo de sombreado de Disney: r = modelo, gba = parametros (disney_brdf.glsl).
layout(set = 0, binding = 26) uniform sampler2D g_shading;
// Oclusion del cielo vista desde arriba (sin trazado de rayos): profundidad de
// solo el terreno y de todo (terreno, escenario, arboles), alrededor de la
// camara.
layout(set = 0, binding = 27) uniform sampler2D sky_map_terrain;
layout(set = 0, binding = 28) uniform sampler2D sky_map_scene;

// Luz del sol que dejan pasar las nubes en ese punto: se lleva el punto al
// suelo a lo largo del rayo del sol (el mapa guarda ese mismo rayo).
float cloudShadow(vec3 world_position, vec3 to_sun) {
    vec4 area = lights.cloud_shadow;
    if (area.z <= 0.0 || to_sun.y <= 0.02) {
        return 1.0;
    }
    vec2 ground = world_position.xz - to_sun.xz * (world_position.y / to_sun.y);
    vec2 uv = (ground - area.xy) / area.z + 0.5;
    float edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
    if (edge <= 0.0) {
        return 1.0;
    }
    float light = textureLod(cloud_shadow_map, uv, 0.0).r;
    return mix(1.0, light, smoothstep(0.0, 0.06, edge));
}

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

const float kPi = 3.14159265;

// Modelo de material de Disney (difuso de Burley, barniz, tela, subsurface,
// anisotropo): comun con el path tracing.
#include "disney_brdf.glsl"

// El modelo de sombreado de este pixel (G-buffer, binding 26); lo usa shade().
ShadingModel surface_model;

// Niveles de mip del cubo de entorno (IblProbe::kEnvironmentMips - 1). La
// sonda de reflexion tiene los mismos (ReflectionProbe::kMips).
const float kEnvironmentMaxLod = 5.0;

// Distancia que se guarda para el cielo en la captura de la sonda: "muy
// lejos" (la correccion de paralaje apenas lo mueve).
const float kProbeSkyDistance = 5000.0;

// Radiancia del disco solar respecto a la iluminancia del sol en la escena.
const float kSunDiskRadiance = 900.0;

// Niebla exponencial por altura: bruma ligera a ras de suelo que se aclara
// hacia arriba. Tenue: el escenario mide ~70 m y es un patio, no un valle.
const float kFogDensity = 0.0018;
const float kFogBaseHeight = 0.0;
const float kFogHeightFalloff = 0.08;

// Colores de depuracion de las cascadas (tecla C), como el modo de
// visualizacion de cascadas de Unreal.
const vec3 kCascadeColors[kShadowCascadeCount] = vec3[](
    vec3(1.0, 0.35, 0.35),
    vec3(0.35, 1.0, 0.40),
    vec3(0.40, 0.55, 1.0),
    vec3(1.0, 0.90, 0.35)
);

// sRGB -> lineal. Los colores de los bloques estan pensados para verse en
// pantalla, asi que hay que linealizarlos antes de iluminar.
vec3 toLinear(vec3 color) {
    return pow(color, vec3(2.2));
}

// Profundidad lineal (distancia de vista) a partir del depth, deshaciendo solo
// la proyeccion: z = P[3][2] / (depth + P[2][2]).
//
// Hacerlo con la inversa completa de view-projection sumaba el error de
// redondeo de la matriz al del propio depth: a 100 bloques la posicion salia
// desplazada varios centimetros, mas que los umbrales del SSAO, y las caras
// superiores lejanas se "tapaban a si mismas" con un ruido de puntos negros.
float linearDepth(float depth) {
    return camera.projection[3][2] / (depth + camera.projection[2][2]);
}

// Posicion en espacio de vista.
vec3 viewFromDepth(vec2 uv, float depth) {
    float z = linearDepth(depth);
    vec2 ndc = uv * 2.0 - 1.0;
    // Con el desplazamiento del centro ([2][0], [2][1]): cada ojo de un casco
    // de VR tiene un campo de vision asimetrico (y el TAA mueve el centro con
    // su jitter). Sin el, en VR todo salia desplazado y la luz se ennegrecia.
    return vec3((ndc.x + camera.projection[2][0]) * z / camera.projection[0][0],
                (ndc.y + camera.projection[2][1]) * z / camera.projection[1][1], -z);
}

// De coordenadas de pantalla + depth a mundo. La vista es una rotacion y una
// traslacion: su inversa es la traspuesta de la rotacion mas la posicion de
// la camara, sin perdida de precision.
vec3 worldFromDepth(vec2 uv, float depth) {
    return camera.position.xyz + transpose(mat3(camera.view)) * viewFromDepth(uv, depth);
}

// Atenuacion fisica (1/d^2) con un corte suave al llegar al alcance de la luz,
// para que no aparezca un borde duro donde la luz deja de evaluarse.
float attenuation(float distance_to_light, float range) {
    float ratio = distance_to_light / max(range, 0.0001);
    float window = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
    return (window * window) / (distance_to_light * distance_to_light + 1.0);
}

// Normal guardada en octaedro (ver encodeNormal() en geometry.frag).
vec3 decodeNormal(vec2 e) {
    vec3 n = vec3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y);
    float t = max(-n.y, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.z += n.z >= 0.0 ? -t : t;
    return normalize(n);
}

// Distribucion de microfacetas GGX (Trowbridge-Reitz).
float distributionGgx(float n_dot_h, float alpha) {
    float alpha2 = alpha * alpha;
    float d = n_dot_h * n_dot_h * (alpha2 - 1.0) + 1.0;
    return alpha2 / (kPi * d * d);
}

// Visibilidad de Smith-GGX con correlacion de altura (ya dividida por
// 4 * NdotL * NdotV).
float visibilitySmith(float n_dot_v, float n_dot_l, float alpha) {
    float alpha2 = alpha * alpha;
    float ggx_v = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - alpha2) + alpha2);
    float ggx_l = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - alpha2) + alpha2);
    return 0.5 / max(ggx_v + ggx_l, 0.00001);
}

vec3 fresnelSchlick(float cosine, vec3 f0) {
    float m = 1.0 - cosine;
    float m2 = m * m;
    return f0 + (1.0 - f0) * (m2 * m2 * m);
}

// BRDF completa para una luz. Las intensidades de las luces de la escena ya
// llevan el factor pi incluido (el difuso es albedo * radiancia * NdotL), asi
// que el especular se multiplica por pi para quedar en la misma escala.
// `f0`: reflectancia a incidencia normal (la del material si es dielectrico,
// su color si es metal).
// `energy`: compensacion de dispersion multiple (ver energyCompensation).
// `light_size`: tangente del radio angular de la luz (el sol: 0.0047). Una
// luz con tamano no hace un brillo infinitamente pequeno en una superficie
// pulida: se ensancha la distribucion y se normaliza (Karis 2013), sin
// puntos blancos que parpadean.
vec3 shade(vec3 light_direction, vec3 radiance, vec3 normal, vec3 view_direction, vec3 albedo,
           float roughness, float metallic, vec3 f0, vec3 energy, float light_size) {
    return radiance * disneyBrdf(surface_model, normal, view_direction, light_direction, albedo, roughness, metallic,
                                 f0, energy, light_size);
}

// Irradiancia difusa (ya / pi) del entorno para una normal.
vec3 irradianceSh(vec3 n) {
    vec3 result = irradiance_sh.coefficients[0].rgb * 0.282095;
    result += irradiance_sh.coefficients[1].rgb * 0.488603 * n.y;
    result += irradiance_sh.coefficients[2].rgb * 0.488603 * n.z;
    result += irradiance_sh.coefficients[3].rgb * 0.488603 * n.x;
    result += irradiance_sh.coefficients[4].rgb * 1.092548 * n.x * n.y;
    result += irradiance_sh.coefficients[5].rgb * 1.092548 * n.y * n.z;
    result += irradiance_sh.coefficients[6].rgb * 0.315392 * (3.0 * n.z * n.z - 1.0);
    result += irradiance_sh.coefficients[7].rgb * 1.092548 * n.x * n.z;
    result += irradiance_sh.coefficients[8].rgb * 0.546274 * (n.x * n.x - n.y * n.y);
    return max(result, vec3(0.0));
}

// -----------------------------------------------------------------------------
// Sombras en cascada
// -----------------------------------------------------------------------------

// Una muestra del mapa, desplazada (u, v) texeles. El muestreador ya compara y
// filtra bilinealmente, asi que cada llamada cubre 2x2 texeles.
float shadowTap(sampler2DArrayShadow map, vec2 base_uv, float u, float v, float texel_uv,
                float layer, float depth) {
    return texture(map, vec4(base_uv + vec2(u, v) * texel_uv, layer, depth));
}

// PCF de tienda (tent) de 3x3 resuelto con solo 4 muestras, aprovechando que el
// hardware ya interpola: se colocan las muestras en posiciones fraccionarias y
// se ponderan para reproducir exactamente el filtro de 3x3.
//
// Es determinista, asi que no deja grano: la alternativa habitual (disco de
// Poisson rotado por pixel) reparte el error como ruido y necesita un TAA
// detras que lo promedie. Sin TAA, ese ruido se ve, y por eso aqui no se usa.
//
// Lo comparten las cascadas y las luces locales: solo cambia el mapa, su
// resolucion y la capa.
float optimizedPcf(sampler2DArrayShadow map, float map_size, vec2 uv, float depth, float layer) {
    float texel_uv = 1.0 / map_size;

    vec2 uv_texels = uv * map_size;
    vec2 base_texel = floor(uv_texels + 0.5);

    // Posicion dentro del texel: define los pesos de la tienda.
    float s = uv_texels.x + 0.5 - base_texel.x;
    float t = uv_texels.y + 0.5 - base_texel.y;

    vec2 base_uv = (base_texel - 0.5) * texel_uv;

    float uw0 = 3.0 - 2.0 * s;
    float uw1 = 1.0 + 2.0 * s;
    float u0 = (2.0 - s) / uw0 - 1.0;
    float u1 = s / uw1 + 1.0;

    float vw0 = 3.0 - 2.0 * t;
    float vw1 = 1.0 + 2.0 * t;
    float v0 = (2.0 - t) / vw0 - 1.0;
    float v1 = t / vw1 + 1.0;

    float sum = 0.0;
    sum += uw0 * vw0 * shadowTap(map, base_uv, u0, v0, texel_uv, layer, depth);
    sum += uw1 * vw0 * shadowTap(map, base_uv, u1, v0, texel_uv, layer, depth);
    sum += uw0 * vw1 * shadowTap(map, base_uv, u0, v1, texel_uv, layer, depth);
    sum += uw1 * vw1 * shadowTap(map, base_uv, u1, v1, texel_uv, layer, depth);

    return sum / 16.0;
}

// Sombra del sol con penumbra de verdad (PCSS, "percentage-closer soft
// shadows", como las sombras de contacto de Unreal y HDRP). El sol mide 0.53
// grados: la penumbra mide la distancia entre lo que tapa y el suelo por
// tan(0.53) (~1 cm por metro). Nitida al pie de un poste y suave la sombra de
// la copa de un arbol o de un tejado.
//   1. Busqueda: la profundidad media de lo que tapa alrededor del punto.
//   2. Filtro: PCF con el tamano de esa penumbra (disco de Vogel; el giro
//      cambia por pixel y frame con el TAA, fijo sin el).
const float kSunDiameterTan = 0.00925;
const float kGoldenAngle = 2.39996323;

vec2 vogelDisk(int index, int count, float rotation) {
    float r = sqrt((float(index) + 0.5) / float(count));
    float theta = float(index) * kGoldenAngle + rotation;
    return vec2(cos(theta), sin(theta)) * r;
}

float softSunShadow(int cascade, vec2 uv, float depth, float texel_world, vec3 normal) {
    float map_size = shadows.params.x;
    float texel_uv = 1.0 / map_size;
    // Proyeccion ortografica: cuanto cambia la profundidad por metro hacia el sol.
    mat4 m = shadows.light_view_projection[cascade];
    vec3 row_x = vec3(m[0][0], m[1][0], m[2][0]);
    vec3 row_y = vec3(m[0][1], m[1][1], m[2][1]);
    vec3 row_z = vec3(m[0][2], m[1][2], m[2][2]);
    float depth_per_meter = max(length(row_z), 1e-6);
    float uv_per_meter = 1.0 / max(texel_world * map_size, 1e-6);

    // Plano del receptor: cada muestra del disco se compara con la
    // profundidad que tiene la superficie EN SU SITIO, no con la del centro.
    // En una ladera, las muestras de un lado miden su propia superficie mas
    // cerca del sol: comparadas con el centro "tapaban" y la penumbra de un
    // arbol o de un tejado salia mas oscura, ancha y a manchas. Con la
    // proyeccion ortografica M = S R, la normal del plano en el espacio del
    // mapa es (M n) / s^2 por eje; uv = ndc / 2 + 1/2.
    vec3 plane = vec3(dot(row_x, normal) / max(dot(row_x, row_x), 1e-12),
                      dot(row_y, normal) / max(dot(row_y, row_y), 1e-12),
                      dot(row_z, normal) / max(dot(row_z, row_z), 1e-12));
    vec2 depth_gradient = abs(plane.z) > 1e-9 ? -2.0 * plane.xy / plane.z : vec2(0.0);
    // A ras (el plano casi paralelo a los rayos) se limita: como mucho 1 m de
    // profundidad a lo ancho de la busqueda.
    float max_gradient = depth_per_meter / max(32.0 * texel_uv, 1e-6);
    float gradient_length = length(depth_gradient);
    if (gradient_length > max_gradient) depth_gradient *= max_gradient / gradient_length;

    float rotation = 0.0;
    if (lights.environment.w >= 0.0) {
        vec2 p = gl_FragCoord.xy + 5.588238 * lights.environment.w;
        rotation = fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))) * 6.2831853;
    }

    // 1. Lo que tapa: hasta ~40 m por encima (penumbra de ~37 cm).
    float search_uv = clamp(40.0 * kSunDiameterTan * uv_per_meter, 2.0 * texel_uv, 32.0 * texel_uv);
    float blocker_sum = 0.0;
    float blockers = 0.0;
    const int kSearch = 12;
    for (int i = 0; i < kSearch; ++i) {
        vec2 offset = vogelDisk(i, kSearch, rotation) * search_uv;
        float stored = textureLod(shadow_depth, vec3(uv + offset, float(cascade)), 0.0).r;
        float receiver = depth + dot(depth_gradient, offset);
        if (stored < receiver - 1e-5) {
            // Lo que tapa, medido desde el receptor en ese sitio.
            blocker_sum += receiver - stored;
            blockers += 1.0;
        }
    }
    if (blockers < 0.5) {
        return 1.0;  // nada tapa: iluminado
    }
    float blocker_distance = (blocker_sum / blockers) / depth_per_meter;  // metros
    float penumbra_uv = blocker_distance * kSunDiameterTan * uv_per_meter;
    // Penumbra menor que el propio filtro: la sombra dura de siempre.
    if (penumbra_uv < 1.5 * texel_uv) {
        return optimizedPcf(shadow_map, map_size, uv, depth, float(cascade));
    }
    float radius = min(penumbra_uv, 32.0 * texel_uv);
    const int kTaps = 16;
    float lit = 0.0;
    for (int i = 0; i < kTaps; ++i) {
        vec2 offset = vogelDisk(i, kTaps, rotation + 1.3) * radius;
        lit += texture(shadow_map, vec4(uv + offset, float(cascade), depth + dot(depth_gradient, offset)));
    }
    return lit / float(kTaps);
}

// --- Oclusion del cielo desde arriba (sin trazado de rayos) ---
// Lo que tapa el cielo de un punto suele estar ENCIMA: la copa de los arboles,
// un tejado, un voladizo. La GI de pantalla no lo ve cuando no sale en
// pantalla (mirando al suelo de un bosque, las copas quedan fuera) y el
// sotobosque recibia todo el cielo, como un prado. Se buscan alrededor del
// punto (disco de 8 m) cosas por encima de el que no sean el propio relieve
// (al menos 1 m sobre el terreno de ahi); cada una pesa mas cerca y segun mire
// la normal hacia ella (una pared que da la espalda a la casa no la ve).
float skyMapVisibility(vec3 p, vec3 n) {
    if (lights.sky_map.w < 0.5) return 1.0;
    vec2 corner = lights.sky_map.xy;
    float extent = lights.sky_map.z;
    float top = lights.sky_map_depth.x;
    float range = lights.sky_map_depth.y;
    vec2 uv_center = (p.xz - corner) / extent;
    float border = min(min(uv_center.x, uv_center.y), min(1.0 - uv_center.x, 1.0 - uv_center.y));
    float fade = smoothstep(0.04, 0.12, border);
    if (fade <= 0.0) return 1.0;

    float rotation = 0.0;
    if (lights.environment.w >= 0.0) {
        vec2 q = gl_FragCoord.xy + 7.123 * lights.environment.w;
        rotation = fract(52.9829189 * fract(dot(q, vec2(0.06711056, 0.00583715)))) * 6.2831853;
    }
    vec3 origin = p + n * 0.3;
    const int kTaps = 12;
    float blocked = 0.0;
    float total = 0.0;
    for (int i = 0; i < kTaps; ++i) {
        vec2 disk = vogelDisk(i, kTaps, rotation);
        vec2 offset = disk * 8.0;
        vec2 uv = (origin.xz + offset - corner) / extent;
        float scene_top = top - textureLod(sky_map_scene, uv, 0.0).r * range;
        float terrain_depth = textureLod(sky_map_terrain, uv, 0.0).r;
        float above = scene_top - origin.y;
        // Sin terreno ahi (suelo de malla): lo que pase de 1 m sobre el punto.
        bool over_ground = terrain_depth < 0.99999 ? scene_top - (top - terrain_depth * range) > 1.0 : above > 1.0;
        float weight = 1.0 - 0.5 * length(disk);
        total += weight;
        if (above > 0.4 && over_ground) {
            vec3 to_occluder = normalize(vec3(offset.x, above, offset.y));
            blocked += weight * clamp(dot(n, to_occluder) * 0.6 + 0.4, 0.0, 1.0);
        }
    }
    float occlusion = blocked / max(total, 1e-4);
    return 1.0 - 0.85 * occlusion * fade;
}

// Muestrea una cascada concreta. Devuelve 1 = totalmente iluminado, 0 = en
// sombra.
float sampleCascade(int cascade, vec3 world_position, vec3 normal, float n_dot_l) {
    float texel_world = shadows.texel_world_sizes[cascade];

    // Normal offset bias: en vez de sesgar la profundidad (que despega las
    // sombras del suelo, el "peter panning"), se mueve el punto de muestreo a
    // lo largo de la normal, como mucho poco mas de un texel.
    //
    // El desplazamiento va con el seno del angulo luz-normal: es cero cuando la
    // luz incide de frente (no hay acne posible) y maximo cuando es rasante,
    // que es el unico caso en que hace falta.
    float sin_theta = sqrt(clamp(1.0 - n_dot_l * n_dot_l, 0.0, 1.0));
    // Mas lo que se desvian los LODs (local_shadows.params.w = error del LOD
    // de la camara por metro; 0 sin LODs): la camara puede dibujar el objeto
    // con una malla simplificada unos centimetros por dentro de la del mapa
    // (que ademas puede salirse medio texel). Sin este margen, con muchos
    // objetos (el presupuesto sube el LOD) el objeto se sombreaba a si mismo a
    // manchas que aparecian y desaparecian.
    float lod_error = local_shadows.params.w > 0.0
                          ? texel_world * 0.5 + local_shadows.params.w * length(world_position - camera.position.xyz)
                          : 0.0;
    vec3 offset_position = world_position + normal * (texel_world * (0.35 + 1.1 * sin_theta) + lod_error);

    vec4 light_clip = shadows.light_view_projection[cascade] * vec4(offset_position, 1.0);
    vec3 projected = light_clip.xyz / light_clip.w;
    vec2 uv = projected.xy * 0.5 + 0.5;

    // Fuera del mapa de la cascada no hay informacion: se considera iluminado.
    if (projected.z > 1.0 || projected.z < 0.0 ||
        any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return 1.0;
    }

    // Cascadas cercanas: sombra suave de contacto (PCSS). Lejanas: PCF de
    // tienda (con un solo muestreo bilineal las cascadas lejanas dejaban el
    // borde en escalera, y al moverse la camara esa escalera "hervia").
    if (cascade <= 1) {
        return softSunShadow(cascade, uv, projected.z, texel_world, normal);
    }
    return optimizedPcf(shadow_map, shadows.params.x, uv, projected.z, float(cascade));
}

// Elige la cascada segun la distancia de vista y mezcla con la siguiente en la
// franja de solape, para que el cambio de resolucion no se vea como una
// costura.
float shadowFactor(vec3 world_position, vec3 normal, float n_dot_l, out int cascade_index) {
    float view_depth = -(camera.view * vec4(world_position, 1.0)).z;

    int cascade = kShadowCascadeCount - 1;
    for (int i = 0; i < kShadowCascadeCount; ++i) {
        if (view_depth < shadows.split_distances[i]) {
            cascade = i;
            break;
        }
    }
    cascade_index = cascade;

    float shadow = sampleCascade(cascade, world_position, normal, n_dot_l);

    float split = shadows.split_distances[cascade];
    float band = split * shadows.params.w;
    if (cascade + 1 < kShadowCascadeCount && view_depth > split - band) {
        float blend = clamp((view_depth - (split - band)) / max(band, 0.0001), 0.0, 1.0);
        shadow = mix(shadow, sampleCascade(cascade + 1, world_position, normal, n_dot_l), blend);
    }

    // Al llegar al limite de la ultima cascada las sombras se desvanecen en vez
    // de cortarse de golpe.
    float max_distance = shadows.split_distances[kShadowCascadeCount - 1];
    float fade = clamp((view_depth - max_distance * 0.85) / (max_distance * 0.15), 0.0, 1.0);

    return mix(shadow, 1.0, fade);
}

// -----------------------------------------------------------------------------
// Sombras de las luces locales
// -----------------------------------------------------------------------------

// Mismo desplazamiento por normal que las cascadas, pero el texel de una
// proyeccion en perspectiva crece con la distancia a la luz, asi que su
// tamano en el mundo llega ya multiplicado por ella.
//
// Mas el error del LOD de la camara: de lejos la camara dibuja una malla
// simplificada cuyas caras pueden quedar hasta `lod_error` por detras de las
// del mapa (que usa la malla fina, estable). Sin este margen la superficie se
// sombreaba a si misma a parches (cuadros negros que desaparecian al
// acercarse). Es como mucho ~1 pixel de pantalla: no despega la sombra.
vec3 localNormalOffset(vec3 world_position, vec3 normal, float n_dot_l, float texel_world) {
    float sin_theta = sqrt(clamp(1.0 - n_dot_l * n_dot_l, 0.0, 1.0));
    float lod_error = local_shadows.params.w * length(world_position - camera.position.xyz);
    return world_position + normal * (texel_world * (0.5 + 1.5 * sin_theta) + lod_error);
}

// Proyecta en el mapa de la luz y filtra. Fuera del frustum: iluminado.
float localShadowLookup(sampler2DArrayShadow map, float map_size, vec4 light_clip, float layer) {
    if (light_clip.w <= 0.0) {
        return 1.0;
    }

    vec3 projected = light_clip.xyz / light_clip.w;
    vec2 uv = projected.xy * 0.5 + 0.5;

    if (projected.z > 1.0 || projected.z < 0.0 ||
        any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return 1.0;
    }

    return optimizedPcf(map, map_size, uv, projected.z, layer);
}

float spotShadow(int slot, float distance_to_light, vec3 world_position, vec3 normal,
                 float n_dot_l) {
    float texel_world = local_shadows.spot_params[slot].x * distance_to_light;
    vec3 offset_position = localNormalOffset(world_position, normal, n_dot_l, texel_world);

    vec4 light_clip = local_shadows.spot_view_projection[slot] * vec4(offset_position, 1.0);
    return localShadowLookup(spot_shadow_maps, local_shadows.params.x, light_clip, float(slot));
}

// Cubo de sombras resuelto a mano: se elige la cara por el eje dominante de la
// direccion luz -> punto, con el mismo orden que la CPU (+X, -X, +Y, -Y, +Z,
// -Z). Cada cara tiene algo mas de 90 grados de FOV, asi que el PCF de los
// pixeles junto a una arista no se sale del mapa.
float pointShadow(int slot, vec3 light_position, vec3 world_position, vec3 normal,
                  float n_dot_l) {
    // El texel de la cara crece con la distancia a lo largo de su eje.
    vec3 axis_distance = abs(world_position - light_position);
    float axial = max(axis_distance.x, max(axis_distance.y, axis_distance.z));
    float texel_world = local_shadows.point_params[slot].x * axial;
    vec3 offset_position = localNormalOffset(world_position, normal, n_dot_l, texel_world);

    vec3 direction = offset_position - light_position;
    vec3 magnitude = abs(direction);
    int face;
    if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z) {
        face = direction.x > 0.0 ? 0 : 1;
    } else if (magnitude.y >= magnitude.z) {
        face = direction.y > 0.0 ? 2 : 3;
    } else {
        face = direction.z > 0.0 ? 4 : 5;
    }

    int layer = slot * kPointShadowFaceCount + face;
    vec4 light_clip = local_shadows.point_view_projection[layer] * vec4(offset_position, 1.0);
    return localShadowLookup(point_shadow_maps, local_shadows.params.y, light_clip, float(layer));
}

// Hueco de sombra guardado como float en el uniform buffer; -1 = sin sombra.
int shadowSlot(float encoded) {
    return int(floor(encoded + 0.5));
}

// Hash de una celda 3D a [0, 1).
float hash13(vec3 cell) {
    return fract(sin(dot(cell, vec3(12.9898, 78.233, 45.164))) * 43758.5453);
}

// --- Vision nocturna (efecto Purkinje) ---
// Con poca luz ven los bastones del ojo: no distinguen colores y son mas
// sensibles al azul-verde (507 nm). Se aplica SOLO a la luz de la luna y del
// cielo nocturno: lo que alumbra una farola o una antorcha lo ven los conos y
// conserva su color, este cerca o lejos de la luz y se mire donde se mire.
// (Hecho sobre la imagen final, lo iluminado de lejos por una farola quedaba
// gris al apartar la vista de la bombilla.)
// Luminancia escotopica de Larson et al. (XYZ), normalizada al blanco.
vec3 rodVision(vec3 linear_srgb) {
    float strength = lights.rt_shadows.y;
    if (strength <= 0.0) return linear_srgb;
    const mat3 kSrgbToXyz = mat3(0.4124, 0.2126, 0.0193, 0.3576, 0.7152, 0.1192, 0.1805, 0.0722, 0.9505);
    vec3 xyz = kSrgbToXyz * max(linear_srgb, vec3(0.0));
    float scotopic = xyz.x > 1e-7 ? max(xyz.y * (1.33 * (1.0 + (xyz.y + xyz.z) / xyz.x) - 1.68), 0.0) / 2.573 : xyz.y;
    // Gris azulado de los bastones (tinte de luminancia 1).
    const vec3 kRodTint = vec3(0.88, 1.01, 1.26);
    return mix(linear_srgb, scotopic * kRodTint, strength);
}

// --- Sombras por rayos de las luces locales ---
// Los 3x3 vecinos de la misma superficie (profundidad y normal parecidas) se
// leen una vez por pixel; cada luz promedia su visibilidad en los que la
// tienen. Asi el ruido de la penumbra (un rayo por luz y pixel) se suaviza sin
// que la sombra se corra a otra superficie.
vec4 rt_neighbor_mask[9];
bool rt_neighbor_ok[9];

void loadRtShadowNeighborhood(float center_z, vec3 normal) {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    ivec2 size = textureSize(rt_shadow_mask, 0);
    int n = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            ivec2 q = clamp(pixel + ivec2(dx, dy), ivec2(0), size - 1);
            rt_neighbor_mask[n] = texelFetch(rt_shadow_mask, q, 0);
            bool ok = true;
            if (dx != 0 || dy != 0) {
                float z = linearDepth(texelFetch(g_depth, q, 0).r);
                vec3 nq = decodeNormal(texelFetch(g_normal, q, 0).rg);
                ok = abs(z - center_z) <= center_z * 0.05 && dot(nq, normal) > 0.8;
            }
            rt_neighbor_ok[n] = ok;
            ++n;
        }
    }
}

// El sol en la mascara de rt_shadows.comp (despues de las 32 + 8 locales).
const int kRtSunId = kMaxPointLights + kMaxSpotLights;

// Visibilidad (0..1) de la luz `id` (puntuales 0..31, focos 32..39, sol 40),
// o -1 si no es de las 4 que el pase trazo en este pixel.
float rtLocalShadow(int id) {
    float base = float(id + 1) * 32.0;
    vec4 own = rt_neighbor_mask[4];
    vec4 inside = step(vec4(base), own) * (1.0 - step(vec4(base + 32.0), own));
    if (dot(inside, vec4(1.0)) < 0.5) {
        return -1.0;
    }
    float sum = 0.0;
    float count = 0.0;
    for (int n = 0; n < 9; ++n) {
        if (!rt_neighbor_ok[n]) continue;
        vec4 m = rt_neighbor_mask[n];
        for (int c = 0; c < 4; ++c) {
            if (m[c] >= base && m[c] < base + 32.0) {
                sum += (m[c] - base) / 31.0;
                count += 1.0;
            }
        }
    }
    return count > 0.0 ? sum / count : -1.0;
}

// Ruido de valor 3D suave (Via Lactea, relieve de la luna).
float valueNoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash13(i), n100 = hash13(i + vec3(1, 0, 0));
    float n010 = hash13(i + vec3(0, 1, 0)), n110 = hash13(i + vec3(1, 1, 0));
    float n001 = hash13(i + vec3(0, 0, 1)), n101 = hash13(i + vec3(1, 0, 1));
    float n011 = hash13(i + vec3(0, 1, 1)), n111 = hash13(i + vec3(1, 1, 1));
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
               mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}

float fbm(vec3 p) {
    float sum = 0.0;
    float amplitude = 0.5;
    for (int i = 0; i < 5; ++i) {
        sum += valueNoise(p) * amplitude;
        p = p * 2.03 + vec3(17.1, 3.7, 9.2);
        amplitude *= 0.5;
    }
    return sum;
}

// Estrellas: la esfera del cielo se parte en una rejilla y solo unas pocas
// celdas tienen estrella. Dos capas: pocas brillantes y muchas tenues (como
// las magnitudes reales: por cada estrella brillante hay decenas debiles), con
// el color de su temperatura (azuladas, blancas, amarillas, anaranjadas).
// Van fijas a la direccion, asi que no se mueven al desplazarse la camara.
vec3 starLayer(vec3 view_direction, float density, float chance_min, float strength) {
    vec3 grid = view_direction * density;
    vec3 cell = floor(grid);
    float chance = hash13(cell);
    if (chance < chance_min) return vec3(0.0);
    // Posicion dentro de la celda (no todas en el centro).
    vec3 center = cell + 0.25 + 0.5 * vec3(hash13(cell + 7.1), hash13(cell + 3.3), hash13(cell + 5.7));
    float falloff = smoothstep(0.32, 0.0, length(grid - center));
    float magnitude = (chance - chance_min) / (1.0 - chance_min);
    float brightness = magnitude * magnitude * magnitude * strength;
    float temperature = hash13(cell + 11.0);
    vec3 tint = temperature < 0.25 ? vec3(0.75, 0.85, 1.0)
              : temperature < 0.75 ? vec3(1.0, 0.98, 0.95)
              : temperature < 0.92 ? vec3(1.0, 0.88, 0.70)
                                   : vec3(1.0, 0.72, 0.50);
    return tint * falloff * brightness;
}

vec3 starField(vec3 view_direction) {
    return starLayer(view_direction, 170.0, 0.985, 1.6) + starLayer(view_direction, 420.0, 0.97, 0.35);
}

// Via Lactea: una banda tenue a lo largo de un circulo maximo inclinado, con
// nubes de polvo oscuro (ruido) y el nucleo mas brillante hacia un lado.
vec3 milkyWay(vec3 view_direction) {
    const vec3 kGalacticPole = normalize(vec3(0.35, 0.55, -0.76));
    float band = 1.0 - abs(dot(view_direction, kGalacticPole));
    band = pow(clamp(band, 0.0, 1.0), 14.0);
    if (band < 0.002) return vec3(0.0);
    float clouds = fbm(view_direction * 7.0);
    float dust = smoothstep(0.35, 0.75, fbm(view_direction * 13.0 + 4.0));
    const vec3 kCore = normalize(vec3(-0.7, 0.35, 0.62));
    float core = 0.6 + 0.9 * pow(max(dot(view_direction, kCore), 0.0), 3.0);
    return vec3(0.85, 0.88, 1.0) * band * clouds * (1.0 - 0.75 * dust) * core * 0.035;
}

// Luna: disco con los mares oscuros (ruido sobre su superficie), el borde un
// poco mas oscuro y un halo suave alrededor (la luz dispersada en el aire).
vec3 moonDisk(vec3 view_direction, vec3 to_moon) {
    float moon_cos = dot(view_direction, to_moon);
    const float kMoonCos = 0.99955;  // radio aparente ~1.7 grados (un poco mayor que el real: se lee mejor)
    vec3 color = vec3(0.0);
    if (moon_cos > kMoonCos - 0.0002) {
        // Coordenadas sobre el disco para el relieve.
        vec3 side = normalize(cross(to_moon, vec3(0.0, 1.0, 0.0) + vec3(1e-4)));
        vec3 up = cross(side, to_moon);
        vec2 disk = vec2(dot(view_direction, side), dot(view_direction, up)) / sqrt(1.0 - kMoonCos * kMoonCos);
        float r2 = dot(disk, disk);
        float mare = smoothstep(0.45, 0.62, fbm(vec3(disk * 2.2, 3.0)));
        float craters = fbm(vec3(disk * 9.0, 7.0));
        float albedo = mix(1.0, 0.55, mare) * (0.85 + 0.3 * craters);
        float limb = sqrt(max(1.0 - r2, 0.0));
        float edge = smoothstep(kMoonCos - 0.0002, kMoonCos + 0.0001, moon_cos);
        color += vec3(0.92, 0.94, 1.0) * albedo * (0.55 + 0.45 * limb) * edge * 0.9;
    }
    // Halo (aureola de la bruma).
    color += vec3(0.05, 0.06, 0.09) * pow(max(moon_cos, 0.0), 400.0);
    color += vec3(0.015, 0.02, 0.035) * pow(max(moon_cos, 0.0), 30.0);
    return color;
}

// Coordenadas de una direccion en la LUT del cielo (ver sky_lut.frag).
vec2 skyLutUv(vec3 direction) {
    float azimuth = atan(direction.z, direction.x);
    float elevation = asin(clamp(direction.y, -1.0, 1.0));
    float v = 0.5 + 0.5 * sign(elevation) * sqrt(abs(elevation) / (0.5 * kPi));
    return vec2(azimuth / (2.0 * kPi) + 0.5, v);
}

vec3 skyRadiance(vec3 direction) {
    return texture(sky_lut, skyLutUv(direction)).rgb;
}

// Cielo con ciclo dia/noche. El color de fondo (azul, horizonte blanquecino,
// halo del sol, naranja del ocaso) sale de la dispersion atmosferica.
//
// `celestial` pinta los discos de sol y luna y las estrellas. La niebla y los
// reflejos lo desactivan: pintan el cielo "detras" del terreno y ahi no deben
// aparecer.
vec3 skyColor(vec3 view_direction, bool celestial) {
    // Cielo fotografiado: la foto ya trae el sol, las nubes y el horizonte.
    // Para la niebla (sin astros) un mip borroso: el color del aire, no el
    // detalle de la foto.
    if (lights.environment.x > 0.5) {
        vec2 uv = vec2(atan(view_direction.z, view_direction.x) / (2.0 * kPi) + 0.5,
                       acos(clamp(view_direction.y, -1.0, 1.0)) / kPi);
        return textureLod(environment_hdr, uv, celestial ? 0.0 : 5.0).rgb;
    }

    vec3 to_sun = lights.sky_sun.xyz;
    vec3 to_moon = lights.sky_moon.xyz;
    float daylight = lights.sky_sun.w;
    float night = 1.0 - daylight;

    // Bajo el horizonte la atmosfera choca enseguida con el planeta y la LUT
    // es casi negra: ahi se ve la bruma del horizonte, que se apaga poco a
    // poco hacia el suelo (como un mar o una llanura lejana).
    vec3 sky = skyRadiance(view_direction);
    if (view_direction.y < 0.0) {
        vec3 horizon = skyRadiance(normalize(vec3(view_direction.x, 0.0, view_direction.z)));
        sky = horizon * mix(0.85, 0.2, smoothstep(0.0, 0.15, -view_direction.y));
    }

    // Brillo de fondo minimo de la noche (luz de estrellas y airglow).
    float height = clamp(view_direction.y * 0.5 + 0.5, 0.0, 1.0);
    sky += mix(vec3(0.010, 0.016, 0.035), vec3(0.002, 0.004, 0.012), height) * night;

    if (celestial) {
        // --- Disco solar con oscurecimiento del limbo ---
        float sun_cos = dot(view_direction, to_sun);
        float sun_visible = smoothstep(-0.02, 0.02, to_sun.y);
        float disk = smoothstep(0.99955, 0.99975, sun_cos);
        float limb = sqrt(clamp((sun_cos - 0.99955) / (1.0 - 0.99955), 0.0, 1.0));
        vec3 sun_tint = mix(vec3(1.0, 0.35, 0.10), vec3(1.0, 0.96, 0.90),
                            smoothstep(0.0, 0.35, to_sun.y));
        sky += sun_tint * disk * (0.4 + 0.6 * limb) * kSunDiskRadiance * sun_visible;

        // --- Luna, estrellas y Via Lactea ---
        // Cerca del horizonte las estrellas se apagan (mas aire delante).
        float moon_visible = smoothstep(-0.05, 0.05, to_moon.y) * night;
        float above_horizon = smoothstep(0.0, 0.25, view_direction.y);
        vec3 stars = starField(view_direction) * 0.5 + milkyWay(view_direction);
        // La luna tapa las estrellas que tiene detras.
        float behind_moon = smoothstep(0.99945, 0.99955, dot(view_direction, to_moon)) * moon_visible;
        sky += stars * night * night * above_horizon * (1.0 - behind_moon);
        sky += moonDisk(view_direction, to_moon) * moon_visible;
    }

    return sky;
}

// AO con rebote multiple (Jimenez et al., GTAO): en superficies claras la luz
// rebota dentro de los huecos, asi que la oclusion oscurece menos que en las
// oscuras. Evita que la nieve y la piedra clara queden sucias en las esquinas.
vec3 multiBounceAo(float ao, vec3 albedo) {
    vec3 a = 2.0404 * albedo - 0.3324;
    vec3 b = -4.7951 * albedo + 0.6417;
    vec3 c = 2.7552 * albedo + 0.6903;
    return max(vec3(ao), ((ao * a + b) * ao + c) * ao);
}


// SSAO desenfocado: media de la ventana de 4x4 pixeles (la que cubre las 16
// rotaciones del patron de ssao.frag). Solo se mezclan vecinos de la misma
// superficie: normal parecida y profundidad parecida.
//
// La tolerancia de profundidad es relativa (10%): con una absoluta, en suelos
// vistos de refilon los vecinos de la propia superficie ya diferian lo
// bastante para descartarse, la media quedaba incompleta y el patron 4x4 se
// veia como ruido dentro de las sombras.
float blurredSsao(vec3 normal) {
    ivec2 size = textureSize(ssao_map, 0);
    ivec2 center = ivec2(gl_FragCoord.xy);
    float center_depth = texelFetch(ssao_map, center, 0).g;
    float tolerance = center_depth * 0.10 + 0.05;

    float sum = 0.0;
    float weight_sum = 0.0;
    for (int y = -2; y <= 1; ++y) {
        for (int x = -2; x <= 1; ++x) {
            ivec2 p = clamp(center + ivec2(x, y), ivec2(0), size - 1);
            vec2 s = texelFetch(ssao_map, p, 0).rg;
            vec3 n = decodeNormal(texelFetch(g_normal, p, 0).rg);
            float w = (abs(s.g - center_depth) < tolerance && dot(n, normal) > 0.8) ? 1.0 : 0.0;
            sum += s.r * w;
            weight_sum += w;
        }
    }
    return weight_sum > 0.0 ? sum / weight_sum : 1.0;
}

// Normal geometrica (la de la cara del triangulo) reconstruida del depth
// buffer: se toma, en cada eje, el vecino mas cercano en profundidad para no
// mezclar dos objetos en los bordes.
//
// El desplazamiento anti-acne de las sombras tiene que hacerse con esta
// normal, no con la de sombreado: la de sombreado es suave (interpolada entre
// vertices) y ademas lleva el normal map, asi que en mallas facetadas no
// coincide con la cara real y cada triangulo acababa sombreandose a si mismo
// con su propio patron ("sombras triangulares").
vec3 geometricNormal(float depth, vec3 world_position, vec3 view_direction) {
    ivec2 size = textureSize(g_depth, 0);
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float center_z = linearDepth(depth);

    vec3 axes[2];
    for (int axis = 0; axis < 2; ++axis) {
        ivec2 step_offset = axis == 0 ? ivec2(1, 0) : ivec2(0, 1);
        ivec2 forward = clamp(pixel + step_offset, ivec2(0), size - 1);
        ivec2 backward = clamp(pixel - step_offset, ivec2(0), size - 1);
        float depth_forward = texelFetch(g_depth, forward, 0).r;
        float depth_backward = texelFetch(g_depth, backward, 0).r;

        bool use_forward = abs(linearDepth(depth_forward) - center_z) <=
                           abs(linearDepth(depth_backward) - center_z);
        ivec2 neighbor = use_forward ? forward : backward;
        float neighbor_depth = use_forward ? depth_forward : depth_backward;
        vec3 neighbor_position =
            worldFromDepth((vec2(neighbor) + 0.5) / vec2(size), neighbor_depth);
        axes[axis] = (neighbor_position - world_position) * (use_forward ? 1.0 : -1.0);
    }

    vec3 n = cross(axes[0], axes[1]);
    float n_length = length(n);
    if (n_length < 1e-10) {
        return vec3(0.0);  // Sin vecinos utiles: se usara la de sombreado.
    }
    n /= n_length;
    return dot(n, view_direction) < 0.0 ? -n : n;
}

// Direccion en la que hay que leer una sonda de reflexion (centro `center`)
// para el rayo que sale de `position` en la direccion `direction`. La sonda
// vio la escena desde su centro, no desde este punto: se busca donde choca el
// rayo con lo que la sonda ve y se lee en la direccion de ese punto visto
// desde el centro.
//
// La sonda guarda en el alfa la distancia a la superficie que ve en cada
// direccion, asi que es un "depth buffer" esferico: se avanza por el rayo
// (pasos que crecen con la distancia) hasta que un punto queda por detras de
// esa superficie, y el cruce se afina por biseccion. Solo cuenta el paso de
// "delante" a "detras": el tramo inicial que la sonda no ve (el punto que
// refleja puede estar tapado desde ella) no es un choque.
const int kProbeSteps = 24;
const int kProbeRefineSteps = 6;
const float kProbeFirstStep = 0.05;  // metros
const float kProbeMaxDistance = 150.0;

vec3 probeDirection(samplerCube probe, vec3 center, vec3 position, vec3 direction) {
    vec3 offset = position - center;
    float growth = pow(kProbeMaxDistance / kProbeFirstStep, 1.0 / float(kProbeSteps));

    float previous_t = 0.0;
    float t = kProbeFirstStep;
    bool seen_in_front = false;
    for (int i = 0; i < kProbeSteps; ++i) {
        vec3 p = offset + direction * t;
        float p_distance = length(p);
        float surface = textureLod(probe, p / max(p_distance, 0.0001), 0.0).a;
        // Margen: la superficie de la sonda tiene la resolucion de un texel.
        bool behind = p_distance > surface + 0.02 * surface + 0.03;
        if (behind && seen_in_front) {
            float low = previous_t;
            float high = t;
            for (int r = 0; r < kProbeRefineSteps; ++r) {
                float middle = 0.5 * (low + high);
                vec3 q = offset + direction * middle;
                float q_distance = length(q);
                float q_surface = textureLod(probe, q / max(q_distance, 0.0001), 0.0).a;
                if (q_distance > q_surface) {
                    high = middle;
                } else {
                    low = middle;
                }
            }
            return normalize(offset + direction * high);
        }
        seen_in_front = seen_in_front || !behind;
        previous_t = t;
        t *= growth;
    }
    // Sin choque: muy lejos (cielo), la direccion casi no cambia.
    return normalize(offset + direction * kProbeMaxDistance);
}

// GI a resolucion completa: media de la ventana 4x4 de media resolucion (las
// 16 rotaciones de ssgi.frag), solo con los texeles de la misma superficie
// (profundidad y normal parecidas) para no sangrar luz entre objetos.
vec4 upsampledGi(float center_depth, vec3 normal) {
    ivec2 half_size = textureSize(gi_map, 0);
    ivec2 full_size = textureSize(g_depth, 0);
    ivec2 base = ivec2(gl_FragCoord.xy) / 2;
    float tolerance = center_depth * 0.08 + 0.05;

    // Centro de este pixel en coordenadas de texel de media resolucion: cada
    // texel pesa segun lo cerca que este (filtro en tienda). Con pesos iguales
    // en la ventana 4x4, un texel mas brillante se veia como un cuadrado.
    //
    // El texel h de media resolucion representa al pixel completo 2h (ver
    // ssgi.frag / rt_gi.comp), de centro 2h + 0.5: h = (frag - 0.5) / 2. Con
    // "- 0.5" el filtro quedaba desplazado un cuarto de texel y la GI se
    // corria hacia arriba a la izquierda.
    vec2 center = gl_FragCoord.xy * 0.5 - 0.25;

    // center vale base o base + 0.5: la tienda (radio 2) solo tiene peso en
    // base - 1 .. base + 2. La ventana antigua (-2..1) cortaba la tienda por
    // un lado en los pixeles impares.
    vec4 sum = vec4(0.0);
    float weight_sum = 0.0;
    for (int y = -1; y <= 2; ++y) {
        for (int x = -1; x <= 2; ++x) {
            ivec2 p = clamp(base + ivec2(x, y), ivec2(0), half_size - 1);
            ivec2 source = min(p * 2, full_size - 1);
            float depth = linearDepth(texelFetch(g_depth, source, 0).r);
            vec3 n = decodeNormal(texelFetch(g_normal, source, 0).rg);
            vec2 offset = abs(vec2(base + ivec2(x, y)) - center);
            float tent = max(1.0 - offset.x * 0.5, 0.0) * max(1.0 - offset.y * 0.5, 0.0);
            float w = (abs(depth - center_depth) < tolerance ? 1.0 : 0.0) *
                      max(dot(n, normal), 0.0) * tent;
            sum += texelFetch(gi_map, p, 0) * w;
            weight_sum += w;
        }
    }
    // Sin vecinos de la misma superficie (siluetas de un pixel): sin rebote y
    // cielo entero. No el texel mas cercano: en una silueta ese texel es de
    // OTRO objeto (una pared en sombra, un interior) con visibilidad de cielo
    // casi 0, y los bordes se quedaban negros.
    return weight_sum > 0.001 ? sum / weight_sum : vec4(0.0, 0.0, 0.0, 1.0);
}

// Luz volumetrica a resolucion completa. volumetric.frag desplaza el inicio
// de sus tramos con un patron Bayer 4x4 fijo: la media de la ventana 4x4 de
// media resolucion junta los 16 desplazamientos y el grano desaparece. Solo
// se mezclan texeles de profundidad parecida (el cielo con el cielo), para
// que los rayos no se derramen por las siluetas.
vec4 upsampledVolume(float center_depth) {
    ivec2 half_size = textureSize(volumetric_map, 0);
    ivec2 full_size = textureSize(g_depth, 0);
    ivec2 base = ivec2(gl_FragCoord.xy) / 2;
    // Ventana deslizante (no bloques alineados, que dejarian escalones): una
    // ventana 4x4 cualquiera de un patron de periodo 4 tiene los 16 valores.
    ivec2 corner = base - 2;
    float tolerance = center_depth * 0.1 + 0.1;

    vec4 sum = vec4(0.0);
    float weight_sum = 0.0;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            ivec2 p = clamp(corner + ivec2(x, y), ivec2(0), half_size - 1);
            float raw = texelFetch(g_depth, min(p * 2, full_size - 1), 0).r;
            float depth = raw >= 1.0 ? 1.0e6 : linearDepth(raw);
            float w = abs(depth - center_depth) < tolerance ? 1.0 : 0.0;
            sum += texelFetch(volumetric_map, p, 0) * w;
            weight_sum += w;
        }
    }
    return weight_sum > 0.0 ? sum / weight_sum
                            : texelFetch(volumetric_map, clamp(base, ivec2(0), half_size - 1), 0);
}

// Niebla exponencial por altura integrada a lo largo del rayo camara -> punto.
float heightFog(float distance_to_point, vec3 ray_direction) {
    // Densidad y caida del post-procesado (0.0018 y 0.08 por defecto).
    float falloff = lights.clouds.z > 0.0 ? lights.clouds.z : kFogHeightFalloff;
    float density = lights.clouds.y * exp(-(camera.position.y - kFogBaseHeight) * falloff);
    float b = falloff * ray_direction.y;
    float integral = abs(b) > 0.0001 ? (1.0 - exp(-distance_to_point * b)) / b
                                     : distance_to_point;
    return clamp(1.0 - exp(-density * integral), 0.0, 1.0);
}

// Compensacion de energia por dispersion multiple (Kulla-Conty; forma de
// Fdez-Aguera como en Filament/Unreal). GGX con un solo rebote pierde la luz
// que rebota varias veces entre microfacetas: en superficies rugosas (sobre
// todo metales) se veian mas oscuras de lo que son. La LUT de la BRDF da la
// energia que si refleja (A + B) y se devuelve el resto, tenido del color.
vec3 energyCompensation(vec2 brdf, vec3 f0) {
    return 1.0 + f0 * (1.0 / max(brdf.x + brdf.y, 0.001) - 1.0);
}

// Ruido de gradiente entrelazado (Jimenez 2014): desplaza el inicio de los
// rayos por pixel; el TAA lo promedia.
float interleavedGradientNoise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// Sombras de contacto en pantalla (las "contact shadows" de Unreal/HDRP): un
// rayo corto hacia el sol marchado en ESPACIO DE VISTA sobre el depth buffer.
// Las cascadas no tienen detalle para las sombras pequenas (los pies en el
// suelo, piedras, huecos entre objetos). Solo para el sol, a menos de 60 m y
// si la cascada no lo ha oscurecido ya. Devuelve 1 = iluminado.
//
// Como la referencia (HDRP; h3r2tic, "depth buffer raymarching"):
//   - todo en profundidad lineal de vista: el sesgo es RELATIVO a la
//     profundidad (la precision del depth buffer cae con la distancia) mas
//     el tamano de un pixel (con abs: en Vulkan projection[1][1] es
//     negativo y sin abs salian sesgos negativos y todo negro);
//   - el rayo sale un poco separado de la superficie por su normal
//     geometrica (sin tocarse a si misma: nada de acne en cilindros);
//   - choque solo si el rayo se mete detras de lo que se ve MENOS que un
//     grosor (si no, lo que esta lejos detras de un objeto haria sombra);
//   - la sombra se suaviza con cuanto penetra el rayo y con la distancia
//     recorrida (penumbra); con el sol rasante se desvanece (ahi manda la
//     cascada).
// Recorrido EN PANTALLA, un depth por pixel (como las "screen space shadows"
// de Bend Studio en Days Gone): con pasos fijos en metros el rayo se saltaba
// lo fino (piernas, dedos) y el borde salia en escalones ("peine" alrededor de
// lo que toca el suelo) cuando no hay TAA, que es lo normal (Upscaler::Off).
// Pixel a pixel no quedan huecos: el borde es tan fino como la pantalla.
// La profundidad del rayo se interpola en 1/z (correcta en perspectiva) y el
// depth se lee con texelFetch: filtrado mezclaria el personaje con el fondo en
// su silueta y saldrian falsos choques (ruido alrededor del personaje).
// Con TAA (lights.environment.w >= 0) el inicio se desplaza con ruido
// distinto cada frame, que el TAA promedia; sin TAA, sin ruido.
//
// LARGO: solo lo que la cascada no resuelve (~10 texeles de su mapa, lo que
// abarca su PCF), no el largo entero del ajuste. El depth buffer solo tiene
// la cara que ve la camara: con un rayo de medio metro la sombra de todo el
// personaje salia de la pantalla, duplicaba la de la cascada con otra forma
// (escalones en el borde) y cambiaba al mover la camara (lo que hay detras
// del personaje se adivina con el grosor). Como Unreal/HDRP: la sombra de
// contacto es solo el ultimo tramo, pegado al suelo.
float contactShadow(vec3 view_position, vec3 view_normal, vec3 view_to_light, float geometric_n_dot_l,
                    float cascade_texel) {
    float ray_length = lights.environment.z;
    float view_depth = -view_position.z;
    if (ray_length <= 0.0 || view_depth > 60.0) return 1.0;
    float grazing_fade = smoothstep(0.08, 0.3, geometric_n_dot_l);
    if (grazing_fade <= 0.0) return 1.0;

    ray_length = min(ray_length, max(cascade_texel * 10.0, 0.05));
    ivec2 size = textureSize(g_depth, 0);
    // Tamano de un pixel en metros a esa distancia (projection[1][1] es
    // negativo en Vulkan: abs).
    float pixel_size = 2.0 * view_depth / (abs(camera.projection[1][1]) * float(size.y));
    vec3 origin = view_position + view_normal * max(pixel_size * 1.5, view_depth * 0.001);
    // Hacia la camara el rayo no puede cruzar el plano cercano.
    if (view_to_light.z > 0.0) {
        ray_length = min(ray_length, (-origin.z - 0.05) / view_to_light.z);
        if (ray_length <= pixel_size) return 1.0;
    }
    vec3 target = origin + view_to_light * ray_length;

    vec4 clip0 = camera.projection * vec4(origin, 1.0);
    vec4 clip1 = camera.projection * vec4(target, 1.0);
    vec2 pixel0 = (clip0.xy / clip0.w * 0.5 + 0.5) * vec2(size);
    vec2 pixel1 = (clip1.xy / clip1.w * 0.5 + 0.5) * vec2(size);
    float inv_depth0 = 1.0 / -origin.z;
    float inv_depth1 = 1.0 / -target.z;

    // Un paso por pixel (hasta 48; si el rayo es mas largo en pantalla se
    // estira el paso). Los primeros 1.5 pixeles son la propia superficie.
    float pixel_count = length(pixel1 - pixel0);
    if (pixel_count < 1.5) return 1.0;
    const int kMaxSteps = 48;
    int steps = int(min(ceil(pixel_count), float(kMaxSteps)));
    float ds = 1.0 / float(steps);
    float s0 = min(1.5 / pixel_count, 1.0);

    // El grosor supuesto de lo que tapa, y el sesgo: relativo a la
    // profundidad (precision del depth) + medio pixel de pendiente.
    float thickness = clamp(ray_length * 1.5, 0.05, 0.5);
    float frame = lights.environment.w;
    float jitter = frame >= 0.0 ? interleavedGradientNoise(gl_FragCoord.xy + 5.588238 * frame) : 0.0;

    float occlusion = 0.0;
    for (int i = 0; i < kMaxSteps; ++i) {
        if (i >= steps) break;
        float s = s0 + (float(i) + jitter) * ds * (1.0 - s0);
        if (s > 1.0) break;
        vec2 pixel = mix(pixel0, pixel1, s);
        ivec2 texel = ivec2(floor(pixel));
        if (any(lessThan(texel, ivec2(0))) || any(greaterThanEqual(texel, size))) break;
        float ray_depth = 1.0 / mix(inv_depth0, inv_depth1, s);
        float scene_depth = linearDepth(texelFetch(g_depth, texel, 0).r);
        float bias = scene_depth * 0.003 + pixel_size * (scene_depth / view_depth);
        // Cuanto esta el rayo por detras de lo que se ve.
        float penetration = ray_depth - scene_depth - bias;
        if (penetration > 0.0 && penetration < thickness) {
            vec2 uv = pixel / vec2(size);
            vec2 edge = min(uv, 1.0 - uv);
            float edge_fade = clamp(min(edge.x, edge.y) * 20.0, 0.0, 1.0);
            // Entra suave (sin corte duro en el sesgo) y sale suave al
            // acercarse al grosor; lejos del origen, mas debil (penumbra).
            float soft = smoothstep(0.0, bias + pixel_size, penetration) *
                         smoothstep(1.0, 0.5, penetration / thickness);
            occlusion = max(occlusion, soft * (1.0 - s * s) * edge_fade);
            if (occlusion > 0.98) break;
        }
    }
    return 1.0 - occlusion * grazing_fade;
}

void main() {
    float depth = texture(g_depth, v_uv).r;

    // El depth se limpia a 1.0: ese valor significa "aqui no hay geometria".
    vec3 world_position = worldFromDepth(v_uv, depth);
    vec3 color;
    // Distancia de la camara a lo que se ve: la guarda la captura de la sonda
    // de reflexion (en la imagen de pantalla no la lee nadie).
    float surface_distance = kProbeSkyDistance;

    if (depth >= 1.0) {
        // --- Cielo ---
        color = skyColor(normalize(world_position - camera.position.xyz), true);
        // Las nubes tapan el cielo (y el disco del sol) y anaden su luz.
        if (lights.clouds.x > 0.5) {
            vec4 clouds = texture(clouds_map, v_uv);
            color = color * clouds.a + clouds.rgb;
        }
        // Sistema de ambiente: niebla en el horizonte (dias de niebla,
        // ventisca, arena) y el destello de un rayo en todo el cielo.
        if (lights.rt_shadows.w > 0.0 || lights.rt_shadows.z > 0.0) {
            vec3 sky_direction = normalize(world_position - camera.position.xyz);
            vec3 haze = max(irradiance_sh.coefficients[0].rgb * 0.282095, vec3(0.0)) * (1.0 / 3.14159265) +
                        vec3(0.75, 0.8, 1.0) * lights.rt_shadows.z * 0.25;
            float horizon = 1.0 - smoothstep(-0.05, 0.45, sky_direction.y);
            color = mix(color, haze, clamp(lights.rt_shadows.w * mix(0.55, 1.0, horizon), 0.0, 1.0));
            color += vec3(0.75, 0.8, 1.0) * lights.rt_shadows.z * 0.15;
        }
        // El cielo nocturno tambien lo ven los bastones.
        color = rodVision(color);
        // Vista Escena Unlit / Wireframe (shadows.params.z 2 / 3, con
        // exposicion 1): fondo liso.
        if (shadows.params.z > 1.5) {
            color = shadows.params.z > 2.5 ? vec3(0.03, 0.035, 0.045) : vec3(0.32, 0.38, 0.46);
        }
    } else {
        vec4 normal_sample = texture(g_normal, v_uv);
        vec4 albedo_sample = texture(g_albedo, v_uv);

        vec3 normal = decodeNormal(normal_sample.rg);
        float roughness = clamp(normal_sample.b, 0.04, 1.0);
        // F0 de la parte no metalica (0.04 casi siempre; el marmol pulido, mas).
        float reflectance = normal_sample.a;
        vec4 material_sample = texture(g_material, v_uv);
        vec3 emission = material_sample.rgb;
        // Alfa = metalicidad + 2 x sombra propia del relieve (gbuffer_surface.glsl).
        float self_shadow_level = floor(material_sample.a * 0.5);
        float metallic = material_sample.a - 2.0 * self_shadow_level;
        float relief_shadow = 1.0 - self_shadow_level / 7.0;
        vec3 albedo = toLinear(albedo_sample.rgb);

        vec3 to_camera = camera.position.xyz - world_position;
        float distance_to_camera = length(to_camera);
        vec3 view_direction = to_camera / max(distance_to_camera, 0.0001);
        float n_dot_v = max(dot(normal, view_direction), 0.0001);
        float distance_to_camera_z = linearDepth(depth);

        // Normal de la cara real, para el desplazamiento de las sombras.
        vec3 geometric_normal = geometricNormal(depth, world_position, view_direction);
        if (dot(geometric_normal, geometric_normal) < 0.5) {
            geometric_normal = normal;
        }

        // AO horneada del material x SSAO (contactos).
        float ssao = lights.counts.z != 0 ? blurredSsao(normal) : 1.0;
        float ao = albedo_sample.a * ssao;

        vec3 sun_radiance = toLinear(lights.sun_color_ambient.rgb) *
                            lights.sun_direction_intensity.w;
        vec3 sun_direction = normalize(-lights.sun_direction_intensity.xyz);

        // --- Ambiente: IBL del entorno (cielo + suelo) ---
        // Modelo de Disney de este pixel (y el tinte especular en F0).
        surface_model = decodeShading(texture(g_shading, v_uv), normal);
        vec3 f0 = disneyF0(surface_model, albedo, reflectance, metallic);
        // Subsurface: lo que atraviesa se mira en la cara de atras, a este grosor.
        bool translucent = surface_model.model == kShadingSubsurface && surface_model.params.y > 0.0;
        float translucent_thickness = mix(0.01, 0.3, surface_model.params.z);
        // Fresnel con rugosidad (Lagarde): en superficies rugosas el borde
        // brilla menos.
        vec3 env_fresnel = f0 + (max(vec3(1.0 - roughness), f0) - f0) * pow(1.0 - n_dot_v, 5.0);

        // GI: luz rebotada y cuanto cielo ve el punto a gran escala.
        vec4 gi = lights.counts.w != 0 ? upsampledGi(distance_to_camera_z, normal)
                                       : vec4(0.0, 0.0, 0.0, 1.0);
        // La visibilidad se suaviza un poco: el SSAO ya oscurece los contactos
        // cercanos y aplicar ambas enteras los oscureceria dos veces. Pero
        // poco: un suelo de 15% de cielo en todas partes llenaba los
        // interiores de una luz azul plana; ahi la luz es sobre todo la
        // rebotada (calida, de las zonas al sol).
        float sky_visibility = mix(1.0, gi.a, 0.95);
        // Sin rayos: lo que lo tapa por encima aunque no salga en pantalla
        // (copas, tejados); se queda la mas oscura de las dos.
        sky_visibility = min(sky_visibility, skyMapVisibility(world_position, normal));

        // Difuso: irradiancia del cielo (armonicos esfericos) en la parte que
        // ve el cielo, mas la luz rebotada. Relleno minimo de noche (luz de
        // estrellas, rebotes lejanos) para no llegar al negro absoluto.
        // Cielo cubierto: las nubes reparten la luz del sol por todo el cielo,
        // asi que la luz ambiente pasa del azul del cielo despejado a un gris
        // neutro (algo mas clara), como en un dia nublado.
        vec3 sky_irradiance = irradianceSh(normal);
        if (lights.clouds.x > 0.5 && lights.clouds.w > 0.0) {
            float overcast = lights.clouds.w * lights.clouds.w;
            float luminance_sky = dot(sky_irradiance, vec3(0.2126, 0.7152, 0.0722));
            sky_irradiance = mix(sky_irradiance, vec3(luminance_sky * 1.15), overcast * 0.85);
        }
        vec3 diffuse_light = sky_irradiance * sky_visibility + gi.rgb +
                             toLinear(lights.ambient_color.rgb) * lights.sun_color_ambient.a *
                                 (1.0 - lights.sky_sun.w) * 0.25;
        // Destello de un rayo (sistema de ambiente): luz blanca azulada de
        // todo el cielo, mas en lo que mira hacia arriba y ve el cielo.
        if (lights.rt_shadows.z > 0.0) {
            diffuse_light += vec3(0.75, 0.8, 1.0) * lights.rt_shadows.z * (0.45 + 0.55 * max(normal.y, 0.0)) *
                             sky_visibility;
        }
        vec3 diffuse_ibl = diffuse_light * albedo * (1.0 - env_fresnel) * (1.0 - metallic);
        // Hojas y demas subsurface: la luz del cielo que les llega por detras
        // tambien las atraviesa (contra el cielo, una copa no se ve negra).
        if (translucent) {
            diffuse_ibl += irradianceSh(-normal) * sky_visibility * translucencyColor(surface_model, albedo) *
                           surface_model.params.y * 0.5 * (1.0 - metallic);
        }

        // Especular: entorno prefiltrado en la direccion del reflejo, al mip
        // de su rugosidad, por la integral de la BRDF.
        // Anisotropo: el reflejo del entorno se estira como el brillo
        // (normal doblada, Filament / McAuley).
        vec3 reflection_normal = normal;
        if (surface_model.model == kShadingAnisotropic) {
            vec3 aniso_direction = surface_model.bitangent;
            vec3 aniso_tangent = cross(aniso_direction, view_direction);
            vec3 aniso_normal = cross(aniso_tangent, aniso_direction);
            float bend = surface_model.params.x * clamp(5.0 * roughness, 0.0, 1.0);
            reflection_normal = normalize(mix(normal, aniso_normal, bend));
        }
        vec3 reflected = reflect(-view_direction, reflection_normal);
        vec3 prefiltered = textureLod(environment_map, reflected,
                                      roughness * kEnvironmentMaxLod).rgb;
        vec2 brdf = texture(brdf_lut, vec2(n_dot_v, roughness)).rg;
        vec3 energy = energyCompensation(brdf, f0);
        // Donde el SSR encontro algo, refleja la escena. Donde no (lo que
        // queda fuera de la pantalla o detras de la camara), la sonda de
        // reflexion, que es la escena vista desde cerca; sin sonda, el cielo
        // del IBL, que en un interior solo se ve por donde se ve el cielo
        // (visibilidad de la GI).
        vec3 fallback = prefiltered * sky_visibility;
        float probe_weight = lights.probes[0].w + lights.probes[1].w;
        if (probe_weight > 0.001) {
            float lod = roughness * kEnvironmentMaxLod;
            vec3 probe_light = vec3(0.0);
            if (lights.probes[0].w > 0.001) {
                vec3 d = probeDirection(reflection_probe_0, lights.probes[0].xyz,
                                        world_position, reflected);
                probe_light += textureLod(reflection_probe_0, d, lod).rgb * lights.probes[0].w;
            }
            if (lights.probes[1].w > 0.001) {
                vec3 d = probeDirection(reflection_probe_1, lights.probes[1].xyz,
                                        world_position, reflected);
                probe_light += textureLod(reflection_probe_1, d, lod).rgb * lights.probes[1].w;
            }
            fallback = probe_light / probe_weight;
        }
        vec4 ssr = texelFetch(ssr_map, ivec2(gl_FragCoord.xy), 0);
        vec3 reflected_light = mix(fallback, ssr.rgb, ssr.a);
        vec3 specular_ibl = reflected_light * (f0 * brdf.x + brdf.y) * energy;
        // Oclusion del horizonte: con normal maps, el reflejo puede apuntar por
        // debajo de la superficie real y traer luz que alli no llega.
        float horizon = min(1.0 + dot(reflected, geometric_normal), 1.0);
        specular_ibl *= horizon * horizon;

        // Oclusion especular (Lagarde): el especular se ocluye mas que el
        // difuso en angulos rasantes.
        float specular_ao = clamp(pow(n_dot_v + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao,
                                  0.0, 1.0);

        color = diffuse_ibl * multiBounceAo(ao, albedo) + specular_ibl * specular_ao;
        // Tela: el sheen tambien con la luz del cielo (sobre todo de canto).
        if (surface_model.model == kShadingCloth) {
            vec3 sheen_color = mix(vec3(1.0), disneyTint(albedo), surface_model.params.y);
            color += diffuse_light * sheen_color * surface_model.params.x * (1.0 - metallic) *
                     (0.1 + 0.5 * schlickWeight(n_dot_v)) * ao;
        }
        // Barniz: su propio reflejo del entorno, nitido, encima de todo.
        if (surface_model.model == kShadingClearcoat && surface_model.params.x > 0.0) {
            float coat_fresnel = (0.04 + 0.96 * schlickWeight(n_dot_v)) * surface_model.params.x;
            vec3 coat_reflected = reflect(-view_direction, normal);
            vec3 coat_light = textureLod(environment_map, coat_reflected,
                                         surface_model.params.y * kEnvironmentMaxLod).rgb * sky_visibility;
            float coat_horizon = min(1.0 + dot(coat_reflected, geometric_normal), 1.0);
            color = color * (1.0 - coat_fresnel) + coat_light * coat_fresnel * coat_horizon * coat_horizon * specular_ao;
        }

        // --- Sombras por rayos (si el pase corrio): 1 = luces locales, 2 = sol ---
        int rt_flags = int(lights.rt_shadows.x + 0.5);
        bool rt_local = (rt_flags & 1) != 0;
        bool rt_sun = (rt_flags & 2) != 0;
        if (rt_flags != 0) {
            loadRtShadowNeighborhood(distance_to_camera_z, normal);
        }

        // --- Luz direccional (sol), con sombras en cascada ---
        float sun_n_dot_l = max(dot(normal, sun_direction), 0.0);

        int cascade_index = 0;
        float shadow = 1.0;
        if (sun_n_dot_l > 0.0) {
            float geometric_n_dot_l = max(dot(geometric_normal, sun_direction), 0.0);
            shadow = shadowFactor(world_position, geometric_normal, geometric_n_dot_l,
                                  cascade_index);
            // Detalle cercano que la cascada no resuelve.
            if (shadow > 0.02) {
                shadow *= contactShadow(viewFromDepth(v_uv, depth), mat3(camera.view) * geometric_normal,
                                        mat3(camera.view) * sun_direction, geometric_n_dot_l,
                                        shadows.texel_world_sizes[cascade_index]);
            }
            // Con rayos: la mas oscura de las dos. Los rayos ven el escenario y
            // el terreno con su forma exacta (sin acne ni sombras despegadas) y
            // mas alla de las cascadas (montanas que tapan el sol a kilometros);
            // el mapa, ademas, los personajes, la hierba y los arboles que se
            // mecen.
            if (rt_sun) {
                float traced = rtLocalShadow(kRtSunId);
                if (traced >= 0.0) shadow = min(shadow, traced);
            }
            shadow = mix(1.0, shadow, shadows.params.y);
        }
        // Las nubes que pasan por delante del sol.
        shadow *= cloudShadow(world_position, sun_direction);
        // Y las piedras del propio material (auto-sombra del parallax).
        shadow *= relief_shadow;

        // El sol mide 0.53 grados: tangente de su radio angular.
        const float kSunSize = 0.00465;
        color += shade(sun_direction, sun_radiance, normal, view_direction, albedo, roughness,
                       metallic, f0, energy, kSunSize) *
                 shadow;
        // Subsurface: el sol que atraviesa la hoja, la oreja o la cera. La
        // sombra es la de la cara de atras (un poco hacia dentro): una hoja fina
        // la tiene iluminada; un muro, no, y no deja pasar nada.
        if (translucent && dot(normal, sun_direction) < 0.2) {
            int back_cascade = 0;
            vec3 back_position = world_position - geometric_normal * translucent_thickness;
            float back_shadow = shadowFactor(back_position, -geometric_normal,
                                             max(dot(-geometric_normal, sun_direction), 0.0), back_cascade);
            back_shadow = mix(1.0, back_shadow, shadows.params.y) * cloudShadow(world_position, sun_direction);
            color += sun_radiance * disneyTranslucency(surface_model, normal, view_direction, sun_direction, albedo,
                                                       metallic) *
                     back_shadow;
        }

        // Luz de la luna y del cielo: la ven los bastones de noche. Las luces
        // locales, que se suman despues, conservan su color.
        color = rodVision(color);

        // --- Luces puntuales ---
        int point_count = min(lights.counts.x, kMaxPointLights);
        for (int i = 0; i < point_count; ++i) {
            vec3 to_light = lights.points[i].position_range.xyz - world_position;
            float distance_to_light = length(to_light);
            if (distance_to_light > lights.points[i].position_range.w) {
                continue;
            }

            vec3 light_direction = to_light / max(distance_to_light, 0.0001);
            float n_dot_l = dot(normal, light_direction);
            // Por detras solo cuenta la translucidez (subsurface).
            bool from_behind = n_dot_l <= 0.0;
            if (from_behind && !translucent) {
                continue;
            }
            vec3 shadow_position = from_behind ? world_position - geometric_normal * translucent_thickness : world_position;
            vec3 shadow_normal = from_behind ? -geometric_normal : geometric_normal;

            vec3 radiance = toLinear(lights.points[i].color_intensity.rgb) *
                            lights.points[i].color_intensity.a *
                            attenuation(distance_to_light, lights.points[i].position_range.w);

            float point_shadow = 1.0;
            int slot = shadowSlot(lights.points[i].shadow.x);
            if (slot >= 0 && local_shadows.params.z > 0.0) {
                point_shadow = pointShadow(slot, lights.points[i].position_range.xyz,
                                           shadow_position, shadow_normal,
                                           max(dot(shadow_normal, light_direction), 0.0));
                point_shadow = mix(1.0, point_shadow,
                                   local_shadows.params.z * local_shadows.point_params[slot].y *
                                       lights.points[i].shadow.y);
            }
            // Con rayos: la mas oscura de las dos (los rayos ven el escenario
            // real; el mapa, ademas, los personajes y el terreno).
            if (rt_local && !from_behind) {
                float rt = rtLocalShadow(i);
                if (rt >= 0.0) point_shadow = min(point_shadow, mix(1.0, rt, lights.points[i].shadow.y));
            }

            // La bombilla tiene tamano (Radio de la fuente): su brillo en una
            // superficie pulida es un disco que se ensancha al acercarse, no un
            // punto infinitamente pequeno que parpadea.
            float point_size = lights.points[i].shadow.z / max(distance_to_light, 0.01);
            color += (from_behind ? radiance * disneyTranslucency(surface_model, normal, view_direction,
                                                                  light_direction, albedo, metallic)
                                  : shade(light_direction, radiance, normal, view_direction, albedo, roughness,
                                          metallic, f0, energy, min(point_size, 1.0))) *
                     point_shadow;
        }

        // --- Focos ---
        int spot_count = min(lights.counts.y, kMaxSpotLights);
        for (int i = 0; i < spot_count; ++i) {
            vec3 to_light = lights.spots[i].position_range.xyz - world_position;
            float distance_to_light = length(to_light);
            if (distance_to_light > lights.spots[i].position_range.w) {
                continue;
            }

            vec3 light_direction = to_light / max(distance_to_light, 0.0001);

            // Cono: 1 dentro del angulo interior, 0 fuera del exterior.
            float cosine = dot(-light_direction,
                               normalize(lights.spots[i].direction_intensity.xyz));
            float inner_cos = lights.spots[i].color_inner.a;
            float outer_cos = lights.spots[i].outer_shadow.x;
            float cone = clamp((cosine - outer_cos) / max(inner_cos - outer_cos, 0.0001),
                               0.0, 1.0);
            if (cone <= 0.0) {
                continue;
            }

            vec3 radiance = toLinear(lights.spots[i].color_inner.rgb) *
                            lights.spots[i].direction_intensity.w * cone * cone *
                            attenuation(distance_to_light, lights.spots[i].position_range.w);

            float n_dot_l = dot(normal, light_direction);
            bool from_behind = n_dot_l <= 0.0;
            if (from_behind && !translucent) {
                continue;
            }
            vec3 shadow_position = from_behind ? world_position - geometric_normal * translucent_thickness : world_position;
            vec3 shadow_normal = from_behind ? -geometric_normal : geometric_normal;

            float spot_shadow = 1.0;
            int slot = shadowSlot(lights.spots[i].outer_shadow.y);
            if (slot >= 0 && local_shadows.params.z > 0.0) {
                spot_shadow = spotShadow(slot, distance_to_light, shadow_position,
                                         shadow_normal,
                                         max(dot(shadow_normal, light_direction), 0.0));
                spot_shadow = mix(1.0, spot_shadow, local_shadows.params.z * lights.spots[i].outer_shadow.z);
            }
            if (rt_local && !from_behind) {
                float rt = rtLocalShadow(kMaxPointLights + i);
                if (rt >= 0.0) spot_shadow = min(spot_shadow, mix(1.0, rt, lights.spots[i].outer_shadow.z));
            }

            float spot_size = lights.spots[i].outer_shadow.w / max(distance_to_light, 0.01);
            color += (from_behind ? radiance * disneyTranslucency(surface_model, normal, view_direction,
                                                                  light_direction, albedo, metallic)
                                  : shade(light_direction, radiance, normal, view_direction, albedo, roughness,
                                          metallic, f0, energy, min(spot_size, 1.0))) *
                     spot_shadow;
        }

        // --- Emision propia (bloques luminosos, materiales emisivos) ---
        color += emission;

        // --- Visualizacion de cascadas ---
        // Se sustituye el color, no se multiplica: mezclado con el albedo y las
        // luces no se distinguirian unas cascadas de otras.
        if (shadows.params.z > 0.5 && shadows.params.z < 1.5) {
            color = kCascadeColors[cascade_index] * (0.25 + 0.75 * shadow);
        }
        // --- Vista Escena del editor: Unlit (el color del material, sin luz)
        // o Wireframe (el G-buffer solo tiene las lineas: claras) ---
        bool flat_view = shadows.params.z > 1.5;
        if (flat_view) {
            color = shadows.params.z > 2.5 ? vec3(0.75, 0.78, 0.82) : albedo + emission;
        }

        // --- Niebla por altura hacia el color del cielo ---
        // Mirando hacia el sol la niebla se ilumina (dispersion hacia delante),
        // lo que da la sensacion de aire entre la camara y el horizonte.
        // El aire solo dispersa la luz que le llega: en un interior o una
        // calle estrecha no ve el cielo ni el sol, asi que su niebla es mucho
        // mas oscura (si no, todo queda velado de azul). Se aproxima con la
        // visibilidad del cielo del punto que se mira.
        // El color es la luz MEDIA del cielo (el termino constante de su
        // irradiancia), no el cielo de esa direccion: con el cielo la niebla
        // "reflejaba" el skybox (su degradado, las nubes, la luna) sobre las
        // paredes. Mas el halo hacia el sol y un minimo nocturno.
        vec3 ray_direction = -view_direction;
        vec3 fog_color = max(irradiance_sh.coefficients[0].rgb * 0.282095, vec3(0.0)) * (1.0 / 3.14159265);
        float sun_alignment = max(dot(ray_direction, lights.sky_sun.xyz), 0.0);
        fog_color += toLinear(lights.sun_color_ambient.rgb) * lights.sun_direction_intensity.w *
                     pow(sun_alignment, 10.0) * 0.35 * smoothstep(-0.05, 0.1, lights.sky_sun.y);
        fog_color *= mix(0.08, 1.0, gi.a);
        fog_color += toLinear(lights.ambient_color.rgb) * lights.sun_color_ambient.a * 0.05;
        float fog = flat_view ? 0.0 : heightFog(distance_to_camera, ray_direction);
        color = mix(color, fog_color, fog);
        surface_distance = distance_to_camera;
    }

    // --- Luz volumetrica: el polvo iluminado entre la camara y lo que se ve ---
    if (lights.environment.y > 0.5) {
        vec4 volume = upsampledVolume(depth >= 1.0 ? 1.0e6 : linearDepth(depth));
        color = color * volume.a + volume.rgb;
    }

    // Salida HDR lineal: composite.frag aplica exposicion, tono y gamma.
    // El destino es half float: por encima de 65504 se guarda +inf (el disco
    // del sol con el sol alto lo supera) y los filtros que lo leen (FSR, bloom)
    // lo convertian en NaN. Se limita aqui, en el origen.
    color = min(mix(color, vec3(0.0), isnan(color)), vec3(65504.0));
    out_color = vec4(color, surface_distance);
}
