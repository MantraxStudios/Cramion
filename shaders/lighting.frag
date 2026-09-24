#version 450

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
    vec4 shadow;            // x   = hueco de sombra (-1 = sin sombra)
};

struct SpotLightGpu {
    vec4 position_range;       // xyz = posicion,  w = alcance
    vec4 direction_intensity;  // xyz = direccion, w = intensidad
    vec4 color_inner;          // rgb = color,     a = cos del angulo interior
    vec4 outer_shadow;         // x   = cos del angulo exterior, y = hueco de sombra (-1 = sin)
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
} lights;

// Mapa de sombras en cascada. El muestreador compara por hardware: devuelve
// directamente "cuanta luz llega", ya filtrado bilinealmente (PCF 2x2 gratis).
layout(set = 0, binding = 5) uniform sampler2DArrayShadow shadow_map;

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

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

const float kPi = 3.14159265;

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

// Posicion en espacio de vista (la proyeccion no tiene desplazamiento en x/y).
vec3 viewFromDepth(vec2 uv, float depth) {
    float z = linearDepth(depth);
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc.x * z / camera.projection[0][0], ndc.y * z / camera.projection[1][1], -z);
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
vec3 shade(vec3 light_direction, vec3 radiance, vec3 normal, vec3 view_direction, vec3 albedo,
           float roughness, float metallic, vec3 f0) {
    float n_dot_l = max(dot(normal, light_direction), 0.0);
    if (n_dot_l <= 0.0) {
        return vec3(0.0);
    }

    vec3 halfway = normalize(light_direction + view_direction);
    float n_dot_v = max(dot(normal, view_direction), 0.0001);
    float n_dot_h = max(dot(normal, halfway), 0.0);
    float v_dot_h = max(dot(view_direction, halfway), 0.0);

    // Metales: nada de difuso.
    float alpha = max(roughness * roughness, 0.002);
    vec3 fresnel = fresnelSchlick(v_dot_h, f0);
    vec3 specular = distributionGgx(n_dot_h, alpha) * visibilitySmith(n_dot_v, n_dot_l, alpha) *
                    fresnel * kPi;

    // Lo que refleja el especular no entra en el difuso (conservacion).
    vec3 diffuse = albedo * (1.0 - fresnel) * (1.0 - metallic);

    return radiance * n_dot_l * (diffuse + specular);
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
    vec3 offset_position = world_position + normal * (texel_world * (0.35 + 1.1 * sin_theta));

    vec4 light_clip = shadows.light_view_projection[cascade] * vec4(offset_position, 1.0);
    vec3 projected = light_clip.xyz / light_clip.w;
    vec2 uv = projected.xy * 0.5 + 0.5;

    // Fuera del mapa de la cascada no hay informacion: se considera iluminado.
    if (projected.z > 1.0 || projected.z < 0.0 ||
        any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return 1.0;
    }

    // PCF de tienda en todas las cascadas, igual que los focos. Con un solo
    // muestreo bilineal las cascadas lejanas dejaban el borde en escalera, y
    // al moverse la camara esa escalera "hervia" como ruido.
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
vec3 localNormalOffset(vec3 world_position, vec3 normal, float n_dot_l, float texel_world) {
    float sin_theta = sqrt(clamp(1.0 - n_dot_l * n_dot_l, 0.0, 1.0));
    return world_position + normal * (texel_world * (0.5 + 1.5 * sin_theta));
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

// Estrellas: la esfera del cielo se parte en una rejilla y solo unas pocas
// celdas tienen estrella, con brillo aleatorio. Van fijas a la direccion, asi
// que no se mueven al desplazarse la camara.
float starField(vec3 view_direction) {
    vec3 grid = view_direction * 170.0;
    vec3 cell = floor(grid);
    float chance = hash13(cell);
    if (chance < 0.985) {
        return 0.0;
    }

    vec3 center = cell + 0.5;
    float falloff = smoothstep(0.45, 0.0, length(grid - center));
    float brightness = (chance - 0.985) / 0.015;
    return falloff * brightness * brightness;
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

        // --- Luna y estrellas ---
        float moon_cos = dot(view_direction, to_moon);
        float moon_visible = smoothstep(-0.05, 0.05, to_moon.y) * night;
        sky += vec3(0.80, 0.86, 1.00) * smoothstep(0.99935, 0.99955, moon_cos) * 0.6 * moon_visible;
        sky += vec3(0.02, 0.03, 0.06) * pow(max(moon_cos, 0.0), 48.0) * moon_visible;

        float above_horizon = smoothstep(0.0, 0.12, view_direction.y);
        sky += vec3(0.9, 0.93, 1.0) * 0.5 * starField(view_direction) * night * night *
               above_horizon;
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

    vec4 sum = vec4(0.0);
    float weight_sum = 0.0;
    for (int y = -2; y <= 1; ++y) {
        for (int x = -2; x <= 1; ++x) {
            ivec2 p = clamp(base + ivec2(x, y), ivec2(0), half_size - 1);
            ivec2 source = min(p * 2, full_size - 1);
            float depth = linearDepth(texelFetch(g_depth, source, 0).r);
            vec3 n = decodeNormal(texelFetch(g_normal, source, 0).rg);
            float w = (abs(depth - center_depth) < tolerance ? 1.0 : 0.0) *
                      max(dot(n, normal), 0.0);
            sum += texelFetch(gi_map, p, 0) * w;
            weight_sum += w;
        }
    }
    return weight_sum > 0.001 ? sum / weight_sum : vec4(0.0, 0.0, 0.0, 1.0);
}

// Niebla exponencial por altura integrada a lo largo del rayo camara -> punto.
float heightFog(float distance_to_point, vec3 ray_direction) {
    float density = kFogDensity *
                    exp(-(camera.position.y - kFogBaseHeight) * kFogHeightFalloff);
    float b = kFogHeightFalloff * ray_direction.y;
    float integral = abs(b) > 0.0001 ? (1.0 - exp(-distance_to_point * b)) / b
                                     : distance_to_point;
    return clamp(1.0 - exp(-density * integral), 0.0, 1.0);
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
    } else {
        vec4 normal_sample = texture(g_normal, v_uv);
        vec4 albedo_sample = texture(g_albedo, v_uv);

        vec3 normal = decodeNormal(normal_sample.rg);
        float roughness = clamp(normal_sample.b, 0.04, 1.0);
        // F0 de la parte no metalica (0.04 casi siempre; el marmol pulido, mas).
        float reflectance = normal_sample.a;
        vec4 material_sample = texture(g_material, v_uv);
        vec3 emission = material_sample.rgb;
        float metallic = material_sample.a;
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
        vec3 f0 = mix(vec3(reflectance), albedo, metallic);
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

        // Difuso: irradiancia del cielo (armonicos esfericos) en la parte que
        // ve el cielo, mas la luz rebotada. Relleno minimo de noche (luz de
        // estrellas, rebotes lejanos) para no llegar al negro absoluto.
        vec3 diffuse_light = irradianceSh(normal) * sky_visibility + gi.rgb +
                             toLinear(lights.ambient_color.rgb) * lights.sun_color_ambient.a *
                                 (1.0 - lights.sky_sun.w) * 0.25;
        vec3 diffuse_ibl = diffuse_light * albedo * (1.0 - env_fresnel) * (1.0 - metallic);

        // Especular: entorno prefiltrado en la direccion del reflejo, al mip
        // de su rugosidad, por la integral de la BRDF.
        vec3 reflected = reflect(-view_direction, normal);
        vec3 prefiltered = textureLod(environment_map, reflected,
                                      roughness * kEnvironmentMaxLod).rgb;
        vec2 brdf = texture(brdf_lut, vec2(n_dot_v, roughness)).rg;
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
        vec3 specular_ibl = reflected_light * (f0 * brdf.x + brdf.y);

        // Oclusion especular (Lagarde): el especular se ocluye mas que el
        // difuso en angulos rasantes.
        float specular_ao = clamp(pow(n_dot_v + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao,
                                  0.0, 1.0);

        color = diffuse_ibl * multiBounceAo(ao, albedo) + specular_ibl * specular_ao;

        // --- Luz direccional (sol), con sombras en cascada ---
        float sun_n_dot_l = max(dot(normal, sun_direction), 0.0);

        int cascade_index = 0;
        float shadow = 1.0;
        if (sun_n_dot_l > 0.0) {
            float geometric_n_dot_l = max(dot(geometric_normal, sun_direction), 0.0);
            shadow = shadowFactor(world_position, geometric_normal, geometric_n_dot_l,
                                  cascade_index);
            shadow = mix(1.0, shadow, shadows.params.y);
        }

        color += shade(sun_direction, sun_radiance, normal, view_direction, albedo, roughness,
                       metallic, f0) *
                 shadow;

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
            if (n_dot_l <= 0.0) {
                continue;
            }

            vec3 radiance = toLinear(lights.points[i].color_intensity.rgb) *
                            lights.points[i].color_intensity.a *
                            attenuation(distance_to_light, lights.points[i].position_range.w);

            float point_shadow = 1.0;
            int slot = shadowSlot(lights.points[i].shadow.x);
            if (slot >= 0 && local_shadows.params.z > 0.0) {
                point_shadow = pointShadow(slot, lights.points[i].position_range.xyz,
                                           world_position, geometric_normal,
                                           max(dot(geometric_normal, light_direction), 0.0));
                point_shadow = mix(1.0, point_shadow,
                                   local_shadows.params.z * local_shadows.point_params[slot].y);
            }

            color += shade(light_direction, radiance, normal, view_direction, albedo, roughness,
                           metallic, f0) *
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
            if (n_dot_l <= 0.0) {
                continue;
            }

            float spot_shadow = 1.0;
            int slot = shadowSlot(lights.spots[i].outer_shadow.y);
            if (slot >= 0 && local_shadows.params.z > 0.0) {
                spot_shadow = spotShadow(slot, distance_to_light, world_position,
                                         geometric_normal,
                                         max(dot(geometric_normal, light_direction), 0.0));
                spot_shadow = mix(1.0, spot_shadow, local_shadows.params.z);
            }

            color += shade(light_direction, radiance, normal, view_direction, albedo, roughness,
                           metallic, f0) *
                     spot_shadow;
        }

        // --- Emision propia (bloques luminosos, materiales emisivos) ---
        color += emission;

        // --- Visualizacion de cascadas ---
        // Se sustituye el color, no se multiplica: mezclado con el albedo y las
        // luces no se distinguirian unas cascadas de otras.
        if (shadows.params.z > 0.5) {
            color = kCascadeColors[cascade_index] * (0.25 + 0.75 * shadow);
        }

        // --- Niebla por altura hacia el color del cielo ---
        // Mirando hacia el sol la niebla se ilumina (dispersion hacia delante),
        // lo que da la sensacion de aire entre la camara y el horizonte.
        // El aire solo dispersa la luz que le llega: en un interior o una
        // calle estrecha no ve el cielo ni el sol, asi que su niebla es mucho
        // mas oscura (si no, todo queda velado de azul). Se aproxima con la
        // visibilidad del cielo del punto que se mira.
        vec3 ray_direction = -view_direction;
        vec3 fog_color = skyColor(normalize(vec3(ray_direction.x, max(ray_direction.y, 0.0),
                                                 ray_direction.z)), false);
        float sun_alignment = max(dot(ray_direction, lights.sky_sun.xyz), 0.0);
        fog_color += toLinear(lights.sun_color_ambient.rgb) * lights.sun_direction_intensity.w *
                     pow(sun_alignment, 10.0) * 0.35 * smoothstep(-0.05, 0.1, lights.sky_sun.y);
        fog_color *= mix(0.08, 1.0, gi.a);
        float fog = heightFog(distance_to_camera, ray_direction);
        color = mix(color, fog_color, fog);
        surface_distance = distance_to_camera;
    }

    // Salida HDR lineal: composite.frag aplica exposicion, tono y gamma.
    out_color = vec4(color, surface_distance);
}
