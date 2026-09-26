// Codigo comun del trazado de rayos por hardware (rt_gi.comp,
// rt_reflections.comp): la escena para los rayos, el lanzamiento con recorte
// por alfa y la luz que sale de un punto de impacto.
//
// Set 0 (por frame): camara, G-buffer, imagen del frame anterior, salida,
// luces y entorno. Set 1 (la escena, RayTracing): estructura de aceleracion,
// vertices, indices, materiales y todas las texturas.

#extension GL_EXT_ray_query : require
#extension GL_EXT_nonuniform_qualifier : require

// Deben coincidir con scene::kMaxPointLights y kMaxSpotLights.
const int kMaxPointLights = 32;
const int kMaxSpotLights = 8;

struct PointLightGpu {
    vec4 position_range;
    vec4 color_intensity;
    vec4 shadow;
};

struct SpotLightGpu {
    vec4 position_range;
    vec4 direction_intensity;
    vec4 color_inner;
    vec4 outer_shadow;
};

layout(set = 0, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
} camera;

layout(set = 0, binding = 1) uniform sampler2D g_depth;
layout(set = 0, binding = 2) uniform sampler2D g_normal;       // rg = normal, b = rugosidad
layout(set = 0, binding = 3) uniform sampler2D previous_color;  // HDR del frame anterior
layout(set = 0, binding = 4, rgba16f) uniform writeonly image2D output_image;

// Igual que en lighting.frag (GpuLights).
layout(set = 0, binding = 5) uniform LightBuffer {
    vec4 sun_direction_intensity;
    vec4 sun_color_ambient;
    vec4 ambient_color;
    vec4 sky_sun;
    vec4 sky_moon;
    ivec4 counts;
    PointLightGpu points[kMaxPointLights];
    SpotLightGpu spots[kMaxSpotLights];
    vec4 probes[2];
    vec4 clouds;
    vec4 environment;
    vec4 rain;   // x = humedad, y = charcos, z = segundos
    vec4 flood;  // zona inundada: xy = centro (x, z), zw = radios
} lights;

// Entorno del cielo prefiltrado (IblProbe): lo que ve un rayo que no choca.
layout(set = 0, binding = 6) uniform samplerCube environment_map;

layout(push_constant) uniform PushConstants {
    mat4 previous_view_projection;
    vec4 params;  // x = numero de frame (0..1023), y = hay frame anterior valido
} push;

// --- La escena ---
struct RtVertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};

struct RtMaterial {
    vec4 base_color;
    vec4 emissive;   // rgb = factor de emision
    vec4 params;     // x = metalicidad, y = rugosidad
    uint albedo_texture;
    uint metallic_roughness_texture;
    uint emissive_texture;
    uint flags;      // 1 = recortado por alfa
};

layout(set = 1, binding = 0) uniform accelerationStructureEXT scene_tlas;
layout(std430, set = 1, binding = 1) readonly buffer RtVertices { RtVertex rt_vertices[]; };
layout(std430, set = 1, binding = 2) readonly buffer RtIndices { uint rt_indices[]; };
layout(std430, set = 1, binding = 3) readonly buffer RtTriangleMaterials { uint rt_triangle_materials[]; };
layout(std430, set = 1, binding = 4) readonly buffer RtMaterials { RtMaterial rt_materials[]; };
// Por modelo: x = primer triangulo de la geometria 0, y = de la geometria 1.
layout(std430, set = 1, binding = 5) readonly buffer RtModels { uvec4 rt_models[]; };
layout(std430, set = 1, binding = 6) readonly buffer Irradiance { vec4 coefficients[9]; } irradiance_sh;
layout(set = 1, binding = 7) uniform sampler2D rt_textures[];

// --- Cache de radiancia en el mundo (ver "Cache de radiancia" mas abajo) ---
// Tabla hash: clave de la celda (0 = libre), lo sumado este frame (rgb en
// punto fijo, numero de muestras, edad) y lo resuelto (rgb = irradiancia / pi,
// w = frames acumulados). Deben coincidir con RayTracing.cpp.
struct CacheEntry {
    uint r;
    uint g;
    uint b;
    uint count;
    uint age;
    uint pad0;
    uint pad1;
    uint pad2;
};
layout(std430, set = 1, binding = 8) buffer CacheKeys { uint cache_keys[]; };
layout(std430, set = 1, binding = 9) buffer CacheAccum { CacheEntry cache_accum[]; };
layout(std430, set = 1, binding = 10) buffer CacheResolved { vec4 cache_resolved[]; };

#include "rain_common.glsl"

const float kPi = 3.14159265;
const float kEmissiveIntensity = 6.0;  // igual que skinned.frag
const float kMaxRadiance = 30.0;
// Fraccion de cielo que se supone que ve un punto de impacto fuera de
// pantalla (no se traza un rayo mas para medirlo).
const float kHitSkyVisibility = 0.5;

float luminance(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

vec3 toLinear(vec3 color) {
    return pow(color, vec3(2.2));
}

float linearDepth(float depth) {
    return camera.projection[3][2] / (depth + camera.projection[2][2]);
}

vec3 worldFromDepth(vec2 uv, float depth) {
    vec4 world = camera.inverse_view_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return world.xyz / world.w;
}

vec3 decodeNormal(vec2 e) {
    vec3 n = vec3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y);
    float t = max(-n.y, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.z += n.z >= 0.0 ? -t : t;
    return normalize(n);
}

// Numeros aleatorios por pixel para los rayos. El patron en pantalla es fijo
// (ruido de gradiente entrelazado, Jimenez 2014) y cada pixel avanza en el
// tiempo por la secuencia R2 (Roberts 2018, la version 2D de la razon
// aurea): sus muestras recorren el cuadrado de forma uniforme frame a frame.
// (Desplazar el patron por la pantalla cada frame, como antes, hacia que el
// ruido se viera deslizarse por las superficies.)
float spatialNoise(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

vec2 sampleNoise(vec2 pixel, float offset) {
    vec2 base = vec2(spatialNoise(pixel + offset * vec2(47.0, 17.0)),
                     spatialNoise(pixel + offset * vec2(13.0, 71.0) + vec2(5.0, 29.0)));
    return fract(base + push.params.x * vec2(0.7548776662, 0.5698402910));
}

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

float attenuation(float distance_to_light, float range) {
    float ratio = distance_to_light / max(range, 0.0001);
    float window = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
    return window * window / (distance_to_light * distance_to_light + 1.0);
}

uint rtTriangle(uint model, uint geometry, uint primitive) {
    return (geometry == 0u ? rt_models[model].x : rt_models[model].y) + primitive;
}

vec2 rtTriangleUv(uint triangle, vec2 barycentrics) {
    RtVertex a = rt_vertices[rt_indices[triangle * 3u]];
    RtVertex b = rt_vertices[rt_indices[triangle * 3u + 1u]];
    RtVertex c = rt_vertices[rt_indices[triangle * 3u + 2u]];
    vec3 w = vec3(1.0 - barycentrics.x - barycentrics.y, barycentrics);
    return vec2(a.u, a.v) * w.x + vec2(b.u, b.v) * w.y + vec2(c.u, c.v) * w.z;
}

// Candidato de una geometria recortada por alfa: cuenta si el color base es
// opaco ahi (como el discard del raster, alfa >= 0.5).
bool candidateIsOpaque(rayQueryEXT query) {
    uint model = rayQueryGetIntersectionInstanceCustomIndexEXT(query, false);
    uint geometry = rayQueryGetIntersectionGeometryIndexEXT(query, false);
    uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT(query, false);
    uint triangle = rtTriangle(model, geometry, primitive);
    RtMaterial material = rt_materials[rt_triangle_materials[triangle]];
    vec2 uv = rtTriangleUv(triangle, rayQueryGetIntersectionBarycentricsEXT(query, false));
    float alpha = textureLod(rt_textures[nonuniformEXT(material.albedo_texture)], uv, 0.0).a *
                  material.base_color.a;
    return alpha >= 0.5;
}

struct RtHit {
    vec3 position;
    vec3 normal;
    vec2 uv;
    uint material;
    float distance;
    vec3 direction;  // del rayo que llego
};

// Lanza un rayo y devuelve el primer impacto (con recorte por alfa).
bool traceRay(vec3 origin, vec3 direction, float max_distance, out RtHit hit) {
    rayQueryEXT query;
    rayQueryInitializeEXT(query, scene_tlas, gl_RayFlagsNoneEXT, 0xFF, origin, 0.0, direction,
                          max_distance);
    while (rayQueryProceedEXT(query)) {
        if (rayQueryGetIntersectionTypeEXT(query, false) ==
                gl_RayQueryCandidateIntersectionTriangleEXT &&
            candidateIsOpaque(query)) {
            rayQueryConfirmIntersectionEXT(query);
        }
    }
    if (rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
        return false;
    }

    uint model = rayQueryGetIntersectionInstanceCustomIndexEXT(query, true);
    uint geometry = rayQueryGetIntersectionGeometryIndexEXT(query, true);
    uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT(query, true);
    uint triangle = rtTriangle(model, geometry, primitive);
    vec2 barycentrics = rayQueryGetIntersectionBarycentricsEXT(query, true);
    vec3 w = vec3(1.0 - barycentrics.x - barycentrics.y, barycentrics);

    RtVertex a = rt_vertices[rt_indices[triangle * 3u]];
    RtVertex b = rt_vertices[rt_indices[triangle * 3u + 1u]];
    RtVertex c = rt_vertices[rt_indices[triangle * 3u + 2u]];
    vec3 normal = vec3(a.nx, a.ny, a.nz) * w.x + vec3(b.nx, b.ny, b.nz) * w.y +
                  vec3(c.nx, c.ny, c.nz) * w.z;
    // Las normales se transforman con la inversa traspuesta: con la matriz
    // del objeto tal cual, una escala no uniforme las inclinaba y la luz de
    // los impactos salia mal orientada. n * M^-1 == (M^-1)^T * n.
    mat4x3 world_to_object = rayQueryGetIntersectionWorldToObjectEXT(query, true);

    hit.distance = rayQueryGetIntersectionTEXT(query, true);
    hit.direction = direction;
    hit.position = origin + direction * hit.distance;
    hit.normal = normalize(normal * mat3(world_to_object));
    // Mallas de una cara vistas por detras: la normal mira al rayo.
    if (dot(hit.normal, direction) > 0.0) {
        hit.normal = -hit.normal;
    }
    hit.uv = vec2(a.u, a.v) * w.x + vec2(b.u, b.v) * w.y + vec2(c.u, c.v) * w.z;
    hit.material = rt_triangle_materials[triangle];
    return true;
}

// true si nada tapa el segmento (rayo de sombra: basta el primer impacto).
bool unoccluded(vec3 origin, vec3 direction, float max_distance) {
    rayQueryEXT query;
    rayQueryInitializeEXT(query, scene_tlas, gl_RayFlagsTerminateOnFirstHitEXT, 0xFF, origin,
                          0.0, direction, max_distance);
    while (rayQueryProceedEXT(query)) {
        if (rayQueryGetIntersectionTypeEXT(query, false) ==
                gl_RayQueryCandidateIntersectionTriangleEXT &&
            candidateIsOpaque(query)) {
            rayQueryConfirmIntersectionEXT(query);
        }
    }
    return rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT;
}

// Luz que sale de un punto de impacto hacia el rayo: emision + sol (con su
// rayo de sombra) + cielo + luces locales (sin sombra), sobre el difuso del
// material. `lod`: mip de las texturas (lejos o rugoso, mas alto).
//
// `from_screen`: si el punto esta en pantalla, se toma de la imagen del frame
// anterior (su luz completa, con reflejos y brillos). Solo para los reflejos,
// que dependen de la vista igual que esa imagen. La luz rebotada NO: los
// brillos y reflejos de esa imagen cambian con la camara, y su "rebote" se
// movia por los modelos al moverse (ademas de realimentarse frame a frame).
// -----------------------------------------------------------------------------
// Cache de radiancia en el mundo (como SHaRC de NVIDIA o la surface cache de
// Lumen, en una tabla hash).
//
// El mundo se parte en celdas (mas grandes lejos de la camara) y cada celda,
// por cada una de las 6 orientaciones de la normal, guarda la irradiancia que
// llega a esa superficie: cielo que ve de verdad + luz rebotada. La llenan
// los pixeles de la GI (lo que miden sus rayos) y, para lo que no esta en
// pantalla, rayos secundarios desde los puntos de impacto. Al chocar un rayo
// con una superficie se lee su celda en vez de suponer medio cielo: los
// rebotes se encadenan frame a frame (rebotes infinitos), los interiores no
// reciben cielo que no ven y el resultado es estable (la media vive en el
// mundo, no en los pixeles).

const uint kCacheSize = 1u << 19;   // entradas (potencia de dos)
const uint kCacheProbes = 8u;       // huecos que se prueban por clave
const float kCacheFixed = 256.0;    // punto fijo de la suma (atomicAdd en uint)

uint cacheHash(uint x) {
    // PCG (Jarzynski y Olano 2020).
    uint state = x * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Hueco inicial y comprobacion (nunca 0) de la celda de un punto.
void cacheCell(vec3 position, vec3 normal, out uint slot, out uint check) {
    // Celdas de 25 cm hasta 8 m de la camara; el doble cada vez que se
    // dobla la distancia (una celda ocupa mas o menos lo mismo en pantalla).
    float distance_to_camera = length(position - camera.position.xyz);
    float level = clamp(floor(log2(max(distance_to_camera, 8.0) / 8.0)), 0.0, 12.0);
    float cell_size = 0.25 * exp2(level);
    ivec3 q = ivec3(floor(position / cell_size));
    // Orientacion: el eje dominante de la normal y su signo (6 caras).
    vec3 a = abs(normal);
    uint face = a.x > a.y && a.x > a.z ? (normal.x > 0.0 ? 0u : 1u)
              : (a.y > a.z ? (normal.y > 0.0 ? 2u : 3u) : (normal.z > 0.0 ? 4u : 5u));
    uint h = cacheHash(uint(q.x) + cacheHash(uint(q.y) + cacheHash(uint(q.z) + cacheHash(uint(level) * 6u + face))));
    slot = h & (kCacheSize - 1u);
    check = cacheHash(h ^ 0x9E3779B9u) | 1u;
}

// Suma una estimacion de irradiancia (/ pi) a la celda del punto.
void cacheAdd(vec3 position, vec3 normal, vec3 irradiance) {
    if (any(isnan(irradiance)) || any(isinf(irradiance))) {
        return;
    }
    uint slot;
    uint check;
    cacheCell(position, normal, slot, check);
    uvec3 value = uvec3(min(irradiance, vec3(kMaxRadiance)) * kCacheFixed + 0.5);
    for (uint i = 0u; i < kCacheProbes; ++i) {
        uint index = (slot + i) & (kCacheSize - 1u);
        uint previous = atomicCompSwap(cache_keys[index], 0u, check);
        if (previous == 0u || previous == check) {
            atomicAdd(cache_accum[index].r, value.r);
            atomicAdd(cache_accum[index].g, value.g);
            atomicAdd(cache_accum[index].b, value.b);
            atomicAdd(cache_accum[index].count, 1u);
            return;
        }
    }
}

// Irradiancia (/ pi) guardada en la celda del punto; false si aun no hay.
bool cacheLookup(vec3 position, vec3 normal, out vec3 irradiance) {
    uint slot;
    uint check;
    cacheCell(position, normal, slot, check);
    for (uint i = 0u; i < kCacheProbes; ++i) {
        uint index = (slot + i) & (kCacheSize - 1u);
        uint key = cache_keys[index];
        if (key == check) {
            vec4 resolved = cache_resolved[index];
            irradiance = resolved.rgb;
            return resolved.w > 0.0;
        }
        if (key == 0u) {
            break;
        }
    }
    irradiance = vec3(0.0);
    return false;
}

vec3 hitRadiance(RtHit hit, float lod, bool from_screen) {
    // --- En pantalla: la imagen del frame anterior ---
    vec4 clip = camera.view_projection * vec4(hit.position, 1.0);
    if (from_screen && clip.w > 0.0 && push.params.y > 0.5) {
        vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
        if (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)))) {
            ivec2 size = textureSize(g_depth, 0);
            ivec2 texel = clamp(ivec2(uv * vec2(size)), ivec2(0), size - 1);
            float scene_depth = linearDepth(texelFetch(g_depth, texel, 0).r);
            if (abs(scene_depth - clip.w) < 0.02 * clip.w + 0.05) {
                vec4 previous_clip = push.previous_view_projection * vec4(hit.position, 1.0);
                vec2 previous_uv = previous_clip.xy / previous_clip.w * 0.5 + 0.5;
                if (previous_clip.w > 0.0 && all(greaterThanEqual(previous_uv, vec2(0.0))) &&
                    all(lessThanEqual(previous_uv, vec2(1.0)))) {
                    return min(textureLod(previous_color, previous_uv, 0.0).rgb,
                               vec3(kMaxRadiance));
                }
            }
        }
    }

    // --- Fuera de pantalla: se calcula ---
    RtMaterial material = rt_materials[hit.material];
    vec3 albedo = toLinear(textureLod(rt_textures[nonuniformEXT(material.albedo_texture)],
                                      hit.uv, lod).rgb) *
                  material.base_color.rgb;
    float metallic = clamp(material.params.x *
                               textureLod(rt_textures[nonuniformEXT(
                                              material.metallic_roughness_texture)],
                                          hit.uv, lod).b,
                           0.0, 1.0);
    vec3 n = hit.normal;
    vec3 origin = hit.position + n * 0.02;

    // --- Lluvia: el mismo agua que pinta skinned.frag (rain_common.glsl) ---
    // Un reflejo que cae en un charco fuera de pantalla ve el charco: lo de
    // debajo, oscurecido, y encima el espejo del agua reflejando el cielo.
    float water_fresnel = 0.0;
    vec3 water_reflection = vec3(0.0);
    if ((lights.rain.x > 0.0 || lights.rain.y > 0.0 || lights.flood.z > 0.0) && n.y > 0.3) {
        float puddle = puddleLevel(hit.position.xz, lights.rain.y);
        float flood = floodLevel(hit.position.xz, lights.flood);
        // A la intemperie: aqui no hay mapa de lluvia, se pregunta con un
        // rayo hacia arriba. Solo donde hay charco (que bajo techo no
        // existe): lanzarlo en cada impacto con lluvia costaba un rayo mas
        // en casi todos los rayos de la GI y los reflejos. Para el simple
        // oscurecimiento por humedad de un rebote, suponer intemperie no se
        // nota.
        float exposed = 1.0;
        if (puddle > 0.0 && n.y > 0.9) {
            exposed = unoccluded(origin, vec3(0.0, 1.0, 0.0), 200.0) ? 1.0 : 0.0;
        }
        float facing_up = smoothstep(0.3, 0.85, n.y);
        float flat_ground = smoothstep(0.92, 0.98, n.y);
        float level = max(puddle * exposed, flood) * flat_ground * (1.0 - metallic * 0.5);
        // Sin normal map: la altura sale solo del brillo (en sRGB, como en
        // skinned.frag) y de una cara plana.
        float brightness = luminance(pow(albedo, vec3(1.0 / 2.2)));
        float height = 0.55 * smoothstep(0.05, 0.45, brightness) + 0.45;
        float water = waterCoverage(level, height);
        float wet = max(lights.rain.x * exposed * mix(0.25, 1.0, facing_up),
                        clamp(level * 3.0, 0.0, 1.0));

        float roughness =
            clamp(material.params.y *
                      textureLod(rt_textures[nonuniformEXT(material.metallic_roughness_texture)],
                                 hit.uv, lod).g,
                  0.04, 1.0);
        albedo *= wetFactors(roughness, metallic, wet).x;
        albedo *= mix(vec3(1.0), exp(-kPuddleAbsorption * 2.0 * waterDepth(level, height)), water);
        if (water > 0.0) {
            vec3 mirror = reflect(hit.direction, vec3(0.0, 1.0, 0.0));
            water_fresnel = waterFresnel(-hit.direction.y) * water;
            water_reflection = textureLod(environment_map, mirror, 0.0).rgb * water_fresnel;
        }
    }

    // Los metales no tienen difuso; su reflejo se aproxima con algo de su
    // color (sin trazar otro rayo).
    vec3 diffuse = albedo * (1.0 - metallic) + albedo * metallic * 0.25;

    vec3 radiance = toLinear(textureLod(rt_textures[nonuniformEXT(material.emissive_texture)],
                                        hit.uv, lod).rgb) *
                    material.emissive.rgb * kEmissiveIntensity;

    // Sol (o luna), con sombra.
    vec3 to_light = -normalize(lights.sun_direction_intensity.xyz);
    float n_dot_l = dot(n, to_light);
    if (n_dot_l > 0.0 && unoccluded(origin, to_light, 10000.0)) {
        vec3 sun = toLinear(lights.sun_color_ambient.rgb) * lights.sun_direction_intensity.w;
        radiance += diffuse * sun * n_dot_l;
    }

    // Cielo y luz rebotada que llegan a este punto: de la cache de radiancia
    // (lo medido ahi: el cielo que ve de verdad y los rebotes anteriores). Sin
    // dato aun, la aproximacion de antes: medio cielo visible.
    vec3 cached;
    vec3 ambient = cacheLookup(hit.position, n, cached) ? cached : irradianceSh(n) * kHitSkyVisibility;
    // Y el relleno minimo de noche de lighting.frag.
    radiance += diffuse * (ambient + toLinear(lights.ambient_color.rgb) * lights.sun_color_ambient.a *
                                         (1.0 - lights.sky_sun.w) * 0.25);

    // Luces locales, sin sombra.
    for (int i = 0; i < min(lights.counts.x, kMaxPointLights); ++i) {
        vec3 to_point = lights.points[i].position_range.xyz - hit.position;
        float d = length(to_point);
        if (d > lights.points[i].position_range.w) {
            continue;
        }
        float nl = max(dot(n, to_point / max(d, 0.0001)), 0.0);
        radiance += diffuse * toLinear(lights.points[i].color_intensity.rgb) *
                    lights.points[i].color_intensity.a *
                    attenuation(d, lights.points[i].position_range.w) * nl;
    }
    for (int i = 0; i < min(lights.counts.y, kMaxSpotLights); ++i) {
        vec3 to_spot = lights.spots[i].position_range.xyz - hit.position;
        float d = length(to_spot);
        if (d > lights.spots[i].position_range.w) {
            continue;
        }
        vec3 l = to_spot / max(d, 0.0001);
        float cosine = dot(-l, normalize(lights.spots[i].direction_intensity.xyz));
        float inner_cos = lights.spots[i].color_inner.a;
        float outer_cos = lights.spots[i].outer_shadow.x;
        float cone = clamp((cosine - outer_cos) / max(inner_cos - outer_cos, 0.0001), 0.0, 1.0);
        radiance += diffuse * toLinear(lights.spots[i].color_inner.rgb) *
                    lights.spots[i].direction_intensity.w * cone * cone *
                    attenuation(d, lights.spots[i].position_range.w) * max(dot(n, l), 0.0);
    }

    // La lamina de agua refleja una parte (Fresnel) y deja pasar el resto.
    radiance = radiance * (1.0 - water_fresnel) + water_reflection;
    return min(radiance, vec3(kMaxRadiance));
}
