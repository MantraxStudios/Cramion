#version 450
#extension GL_GOOGLE_include_directive : require

// Vidrio de las ventanas: pasada forward despues de la iluminacion diferida.
// Un G-buffer solo guarda una superficie por pixel, asi que el vidrio no
// entraba (se descartaba y las ventanas quedaban vacias). Aqui se dibuja
// encima de la imagen ya iluminada y se mezcla:
//
//     resultado = reflejo * R + lo_de_detras * T
//
// con R y T los de una lamina fina (dos caras): cada cara refleja
// f = Fresnel(F0 = 0.04, IOR 1.5) y la luz rebota entre las dos. Sumando
// todos los rebotes (serie geometrica, sin absorcion):
//
//     R = 2f / (1 + f),   T = (1 - f) / (1 + f),   R + T = 1
//
// De frente el vidrio refleja ~8%; de canto, casi todo: por eso las ventanas
// vistas de lado son espejos.
//
// El reflejo sale, por orden de preferencia:
//   1. de la propia imagen de este frame (copia sin vidrio): se sigue el
//      rayo reflejado por el depth buffer, como el SSR;
//   2. de la sonda de reflexion (la escena vista desde cerca);
//   3. del entorno (IBL).
// Y se suma el brillo del sol en el cristal, con su sombra.
//
// Transmision (modelo de Disney 2015, material con modelo "Transmision"): el
// vidrio tiene IOR, grosor y rugosidad. Lo de detras se lee desviado por la
// refraccion y, si es rugoso, desenfocado (vidrio esmerilado), y se tine con
// el color del material en vez de mezclarse por alfa.
//
// Los demas transparentes (material con mezcla "alfa" o "aditiva", o sin luz)
// usan la misma pasada y la misma mezcla (src + dst * alfa del shader):
//   alfa      color = iluminado * alfa + emision,   a = 1 - alfa (premultiplicado)
//   aditivo   color = iluminado * alfa + emision,   a = 1 (solo suma)
// Iluminados con el sol (y su sombra), las luces principales, las de los
// clusteres (light_clusters.glsl) y el entorno; sin luz, el color tal cual.
// La emision va en la misma escala HDR que la de lo opaco: entra en el bloom.

layout(set = 1, binding = 0) uniform sampler2D albedo_map;
layout(set = 1, binding = 1) uniform sampler2D metallic_roughness_map;
layout(set = 1, binding = 2) uniform sampler2D normal_map;
layout(set = 1, binding = 4) uniform sampler2D emissive_map;

// Repeticion (xy) y desplazamiento (zw) de las UV del material (tiling del
// .crmat; ver skinned.frag).
layout(set = 1, binding = 9) uniform MaterialUv {
    vec4 material_uv;
};

// Debe coincidir con GpuSkinnedPush (y con skinned.vert).
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 base_color;
    vec4 emissive;
    vec4 material;
    uint bone_offset;
    float reflectance;
    uint pick_id;  // aqui: modelo de sombreado (bits 0-3) y parametros (8-31)
    uint flags;
} push;

const int kShadowCascadeCount = 4;

layout(set = 2, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
} camera;

// Solo el principio de GpuLights (lighting.frag): el sol y las sondas estan
// despues de las luces locales, asi que se declaran tambien esas.
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

layout(set = 2, binding = 1) uniform LightBuffer {
    vec4 sun_direction_intensity;
    vec4 sun_color_ambient;
    vec4 ambient_color;
    vec4 sky_sun;
    vec4 sky_moon;
    ivec4 counts;
    PointLightGpu points[32];
    SpotLightGpu spots[8];
    vec4 probes[2];
    vec4 clouds;  // y, z = niebla (densidad, caida): transparent_fog.glsl
} lights;

layout(set = 2, binding = 2) uniform ShadowBuffer {
    mat4 light_view_projection[kShadowCascadeCount];
    vec4 split_distances;
    vec4 texel_world_sizes;
    vec4 params;  // x = resolucion, y = intensidad
} shadows;

layout(set = 2, binding = 3) uniform sampler2DArrayShadow shadow_map;
layout(set = 2, binding = 4) uniform sampler2D g_depth;
// La imagen HDR iluminada de este frame, sin el vidrio.
layout(set = 2, binding = 5) uniform sampler2D scene_color;
layout(set = 2, binding = 6) uniform samplerCube environment_map;
// Modo compatible (moviles): sin sondas de reflexion (16 texturas por shader
// como mucho entre los tres sets): el entorno del cielo.
#ifndef CRAMION_COMPAT
layout(set = 2, binding = 7) uniform samplerCube reflection_probe_0;
layout(set = 2, binding = 8) uniform samplerCube reflection_probe_1;
// Las luces por clusteres (10 = cabecera y luces, 11 = listas). En el modo
// compatible el set no las tiene: solo las principales.
#define CLUSTER_SET 2
#define CLUSTER_LIGHTS_BINDING 10
#define CLUSTER_GRID_BINDING 11
#define CLUSTER_TRANSLUCENT  // las luces de particula sin "affectsTranslucent" no llegan
#include "light_clusters.glsl"
#endif

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_tangent;
layout(location = 3) in vec3 v_world_position;
// Hueco de la instancia y color de vertice (skinned.vert).
layout(location = 9) flat in uint v_instance_slot;
layout(location = 10) in vec4 v_color;

// Registro de la instancia (GpuLightmapRecord: lightmap_common.glsl): aqui
// solo sus parametros por instancia (info.y = tinte y opacidad RGBA8, info.z =
// escala de la emision en half float).
struct InstanceRecord {
    vec4 scale_offset;
    uvec4 info;
    vec4 data[2];  // INSTANCE_DATA de los .crshader (aqui sin uso; da el tamano, 64 B)
};
layout(std430, set = 0, binding = 8) readonly buffer InstanceRecords {
    InstanceRecord instance_records[];
};

// rgb = reflejo, a = transmitancia (la mezcla multiplica lo de detras por a).
layout(location = 0) out vec4 out_color;

const float kPi = 3.14159265;
const float kGlassF0 = 0.04;
const int kShadingTransmission = 5;  // disney_brdf.glsl: modelos 0-4, este es del vidrio
float glass_f0 = kGlassF0;
// Vidrio de ventana: casi liso (un poco de polvo y ondulacion).
const float kGlassRoughness = 0.04;
const float kMaxRadiance = 30.0;
// Misma escala de la emision que skinned.frag (lo emisivo opaco).
const float kEmissiveIntensity = 6.0;
// Mezcla del material (SkinnedModel::shadingBits, bits 4-5) y sin luz (bit 6).
const uint kBlendGlass = 0u;
const uint kBlendAdditive = 2u;
const uint kShadingUnlitBit = 1u << 6u;
const uint kShadingVertexColorBit = 1u << 7u;

// --- Trazado en pantalla (como ssr.frag, pero sobre este frame) ---
const int kSteps = 40;
const int kRefineSteps = 6;
const float kMaxDistance = 60.0;
const float kFirstStep = 0.05;

// Mismo mapeo de radiancia del disco solar que lighting.frag.
const float kSunDiskRadiance = 900.0;

vec3 toLinear(vec3 color) {
    return pow(color, vec3(2.2));
}

// Niebla por altura (la de lighting.frag) sobre lo que se suma.
#include "transparent_fog.glsl"

float linearDepth(float depth) {
    return (camera.projection[3][2] - depth * camera.projection[3][3]) / (camera.projection[2][2] - depth * camera.projection[2][3]);
}

bool project(vec3 view_position, out vec2 uv) {
    vec4 clip = camera.projection * vec4(view_position, 1.0);
    if (clip.w <= 0.0) {
        uv = vec2(-1.0);
        return false;
    }
    uv = clip.xy / clip.w * 0.5 + 0.5;
    return true;
}

bool insideScreen(vec2 uv) {
    return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
}

// Sigue el rayo reflejado (espacio de vista) por el depth buffer. rgb = color
// de lo que choca, a = confianza (0 = nada en pantalla).
vec4 traceScreen(vec3 origin, vec3 ray) {
    ivec2 size = textureSize(g_depth, 0);
    float growth = pow(kMaxDistance / kFirstStep, 1.0 / float(kSteps));
    vec2 noise_pixel = gl_FragCoord.xy;
    float jitter = fract(52.9829189 * fract(dot(noise_pixel, vec2(0.06711056, 0.00583715))));
    float previous_distance = 0.0;
    float distance_along = kFirstStep * mix(1.0, growth, jitter);

    for (int i = 0; i < kSteps; ++i) {
        vec3 sample_position = origin + ray * distance_along;
        vec2 uv;
        if (!project(sample_position, uv) || !insideScreen(uv)) {
            break;
        }
        ivec2 texel = min(ivec2(uv * vec2(size)), size - 1);
        float scene_depth = texelFetch(g_depth, texel, 0).r;
        float difference = -sample_position.z - linearDepth(scene_depth);
        float thickness = (distance_along - previous_distance) * 1.5 + 0.05;

        if (scene_depth < 1.0 && difference > 0.0 && difference < thickness) {
            float low = previous_distance;
            float high = distance_along;
            for (int r = 0; r < kRefineSteps; ++r) {
                float middle = 0.5 * (low + high);
                vec3 p = origin + ray * middle;
                vec2 puv;
                project(p, puv);
                ivec2 ptexel = clamp(ivec2(puv * vec2(size)), ivec2(0), size - 1);
                float d = -p.z - linearDepth(texelFetch(g_depth, ptexel, 0).r);
                if (d > 0.0) {
                    high = middle;
                } else {
                    low = middle;
                }
            }
            vec3 refined = origin + ray * high;
            vec2 hit_uv;
            project(refined, hit_uv);
            ivec2 refined_texel = clamp(ivec2(hit_uv * vec2(size)), ivec2(0), size - 1);
            float gap = -refined.z - linearDepth(texelFetch(g_depth, refined_texel, 0).r);
            if (insideScreen(hit_uv) && abs(gap) < 0.04 + 0.01 * -refined.z) {
                vec3 color = min(textureLod(scene_color, hit_uv, 0.0).rgb, vec3(kMaxRadiance));
                vec2 edge = min(hit_uv, 1.0 - hit_uv);
                float edge_fade = smoothstep(0.0, 0.08, min(edge.x, edge.y));
                float distance_fade = 1.0 - smoothstep(0.7, 1.0, high / kMaxDistance);
                return vec4(color, edge_fade * distance_fade);
            }
        }
        previous_distance = distance_along;
        distance_along *= growth;
    }
    return vec4(0.0);
}

// --- Sombra del sol (cascadas, una muestra filtrada por hardware) ---
float sunShadow(vec3 world_position, vec3 normal) {
    float view_depth = -(camera.view * vec4(world_position, 1.0)).z;
    int cascade = kShadowCascadeCount - 1;
    for (int i = 0; i < kShadowCascadeCount; ++i) {
        if (view_depth < shadows.split_distances[i]) {
            cascade = i;
            break;
        }
    }
    vec3 offset_position = world_position + normal * shadows.texel_world_sizes[cascade] * 1.5;
    vec4 light_clip = shadows.light_view_projection[cascade] * vec4(offset_position, 1.0);
    vec3 projected = light_clip.xyz / light_clip.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    if (projected.z > 1.0 || projected.z < 0.0 || !insideScreen(uv)) {
        return 1.0;
    }
    float lit = texture(shadow_map, vec4(uv, float(cascade), projected.z));
    return mix(1.0, lit, shadows.params.y);
}

float distributionGgx(float n_dot_h, float alpha) {
    float alpha2 = alpha * alpha;
    float d = n_dot_h * n_dot_h * (alpha2 - 1.0) + 1.0;
    return alpha2 / (kPi * d * d);
}

float visibilitySmith(float n_dot_v, float n_dot_l, float alpha) {
    float alpha2 = alpha * alpha;
    float ggx_v = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - alpha2) + alpha2);
    float ggx_l = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - alpha2) + alpha2);
    return 0.5 / max(ggx_v + ggx_l, 0.00001);
}

float fresnel(float cosine) {
    float m = 1.0 - clamp(cosine, 0.0, 1.0);
    float m2 = m * m;
    return glass_f0 + (1.0 - glass_f0) * m2 * m2 * m;
}

// Lo que se ve a traves del vidrio: la imagen sin vidrio leida donde llega el
// rayo refractado (a `thickness` metros), con desenfoque si es rugoso. Si ese
// punto esta por delante del vidrio (otro objeto), sin desviar.
vec3 refractedScene(vec3 view_position, vec3 view_direction, vec3 normal, float ior, float thickness,
                    float roughness) {
    vec2 size = vec2(textureSize(scene_color, 0));
    vec2 base_uv = gl_FragCoord.xy / size;
    vec3 refracted = refract(-view_direction, normal, 1.0 / ior);
    if (dot(refracted, refracted) < 1e-6) refracted = -view_direction;  // reflexion total
    vec3 target = view_position + normalize(mat3(camera.view) * refracted) * thickness;
    vec2 uv;
    if (!project(target, uv) || !insideScreen(uv)) uv = base_uv;
    ivec2 texel = clamp(ivec2(uv * size), ivec2(0), ivec2(size) - 1);
    if (linearDepth(texelFetch(g_depth, texel, 0).r) < -view_position.z) uv = base_uv;

    // Esmerilado: 12 muestras en un disco que crece con la rugosidad.
    float radius = roughness * roughness * 0.06;
    if (radius < 0.5 / size.y) {
        return min(textureLod(scene_color, uv, 0.0).rgb, vec3(kMaxRadiance));
    }
    float rotation = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) * 6.2831853;
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 12; ++i) {
        float r = sqrt((float(i) + 0.5) / 12.0) * radius;
        float a = float(i) * 2.3999632 + rotation;
        vec2 offset = vec2(cos(a), sin(a)) * r * vec2(size.y / size.x, 1.0);
        sum += min(textureLod(scene_color, clamp(uv + offset, vec2(0.0), vec2(1.0)), 0.0).rgb, vec3(kMaxRadiance));
    }
    return sum / 12.0;
}

// Reflectancia de la lamina fina (dos caras, todos los rebotes).
float slabReflectance(float cosine) {
    float f = fresnel(cosine);
    return 2.0 * f / (1.0 + f);
}

// --- Transparentes iluminados (mezcla alfa o aditiva) ---
// Difuso de Lambert + GGX de una luz (la escala del motor: las luces ya
// llevan pi, el difuso es albedo x N.L).
vec3 transparentLight(vec3 light_direction, vec3 radiance, vec3 normal, vec3 view_direction, vec3 diffuse_color,
                      vec3 f0, float roughness) {
    float n_dot_l = dot(normal, light_direction);
    if (n_dot_l <= 0.0) return vec3(0.0);
    vec3 halfway = normalize(light_direction + view_direction);
    float alpha = max(roughness * roughness, 0.002);
    float n_dot_v = max(dot(normal, view_direction), 1e-4);
    float specular = distributionGgx(max(dot(normal, halfway), 0.0), alpha) * visibilitySmith(n_dot_v, n_dot_l, alpha);
    float m = 1.0 - clamp(dot(view_direction, halfway), 0.0, 1.0);
    float m2 = m * m;
    vec3 f = f0 + (1.0 - f0) * (m2 * m2 * m);
    return radiance * n_dot_l * (diffuse_color * (1.0 - f) + f * specular * kPi);
}

// Caida de una luz local (la de lighting.frag).
float localAttenuation(float d, float range) {
    float ratio = d / max(range, 0.0001);
    float window = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
    return (window * window) / (d * d + 1.0);
}

// Color premultiplicado (rgb) y lo que deja pasar de detras (a) de un
// transparente que no es vidrio.
vec4 shadeTransparent(uint blend, bool unlit, vec2 uv) {
    vec4 texel = texture(albedo_map, uv);
    // Factor lineal, textura sRGB; con el tinte y la opacidad de la instancia
    // y el color de vertice si el material lo pide.
    vec4 instance_tint = unpackUnorm4x8(instance_records[v_instance_slot].info.y);
    float instance_emission = unpackHalf2x16(instance_records[v_instance_slot].info.z).x;
    vec3 base = toLinear(texel.rgb) * push.base_color.rgb * instance_tint.rgb;
    float alpha = clamp(texel.a * push.base_color.a * instance_tint.a, 0.0, 1.0);
    if ((push.pick_id & kShadingVertexColorBit) != 0u) base *= v_color.rgb;
    vec3 emission = toLinear(texture(emissive_map, uv).rgb) * push.emissive.rgb * kEmissiveIntensity *
                    instance_emission;

    vec3 lit = base;
    if (!unlit) {
        vec3 view_direction = normalize(camera.position.xyz - v_world_position);
        // Normal map (como skinned.frag) y por las dos caras.
        vec3 n = normalize(v_normal);
        vec3 normal = n;
        vec3 t = v_tangent.xyz - n * dot(n, v_tangent.xyz);
        if (dot(t, t) > 1e-8) {
            t = normalize(t);
            vec3 b = cross(n, t) * (v_tangent.w < 0.0 ? -1.0 : 1.0);
            vec2 xy = texture(normal_map, uv).xy * 2.0 - 1.0;
            vec3 tangent_normal = vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
            float scale = push.material.w;
            tangent_normal.y = scale >= 0.0 ? -tangent_normal.y : tangent_normal.y;
            tangent_normal.xy *= abs(scale);
            normal = normalize(mat3(t, b, n) * tangent_normal);
        }
        if (dot(n, view_direction) < 0.0) normal = -normal;

        vec4 metallic_roughness = texture(metallic_roughness_map, uv);
        float metallic = clamp(push.material.x * metallic_roughness.b, 0.0, 1.0);
        float roughness = clamp(push.material.y * metallic_roughness.g, 0.04, 1.0);
        vec3 f0 = mix(vec3(push.reflectance), base, metallic);
        vec3 diffuse_color = base * (1.0 - metallic);

        // Sol, con su sombra.
        vec3 sun_direction = normalize(-lights.sun_direction_intensity.xyz);
        vec3 sun_radiance = toLinear(lights.sun_color_ambient.rgb) * lights.sun_direction_intensity.w;
        lit = transparentLight(sun_direction, sun_radiance, normal, view_direction, diffuse_color, f0, roughness);
        if (dot(lit, lit) > 0.0) lit *= sunShadow(v_world_position, normal);

        // Luces principales (sin teselas ni sombras: son pocos pixeles) y las
        // de los clusteres.
        int point_count = min(lights.counts.x, 32);
        for (int i = 0; i < point_count; ++i) {
            vec3 to_light = lights.points[i].position_range.xyz - v_world_position;
            float d = length(to_light);
            if (d > lights.points[i].position_range.w) continue;
            vec3 radiance = lights.points[i].color_intensity.rgb * lights.points[i].color_intensity.a *
                            localAttenuation(d, lights.points[i].position_range.w);
            lit += transparentLight(to_light / max(d, 0.0001), radiance, normal, view_direction, diffuse_color, f0,
                                    roughness);
        }
        int spot_count = min(lights.counts.y, 8);
        for (int i = 0; i < spot_count; ++i) {
            vec3 to_light = lights.spots[i].position_range.xyz - v_world_position;
            float d = length(to_light);
            if (d > lights.spots[i].position_range.w) continue;
            vec3 l = to_light / max(d, 0.0001);
            float cosine = dot(-l, normalize(lights.spots[i].direction_intensity.xyz));
            float inner_cos = lights.spots[i].color_inner.a;
            float outer_cos = lights.spots[i].outer_shadow.x;
            float cone = clamp((cosine - outer_cos) / max(inner_cos - outer_cos, 0.0001), 0.0, 1.0);
            if (cone <= 0.0) continue;
            vec3 radiance = lights.spots[i].color_inner.rgb * lights.spots[i].direction_intensity.w * cone * cone *
                            localAttenuation(d, lights.spots[i].position_range.w);
            lit += transparentLight(l, radiance, normal, view_direction, diffuse_color, f0, roughness);
        }
#ifndef CRAMION_COMPAT
        uvec2 cluster_range = clusterRange(gl_FragCoord.xy, v_world_position);
        for (uint k = 0u; k < cluster_range.y; ++k) {
            ClusterLight light = cluster_lights[cluster_items[cluster_range.x + k]];
            vec3 l;
            float d;
            vec3 radiance;
            if (!clusterLightIncoming(light, v_world_position, l, d, radiance)) continue;
            lit += transparentLight(l, radiance, normal, view_direction, diffuse_color, f0, roughness);
        }
#endif

        // Ambiente: el entorno muy filtrado como difuso, y su reflejo (la
        // sonda si la hay) con el Fresnel de la rugosidad.
        vec3 reflected = reflect(-view_direction, normal);
        float n_dot_v = max(dot(normal, view_direction), 1e-4);
        vec3 env_fresnel = f0 + (max(vec3(1.0 - roughness), f0) - f0) * pow(1.0 - n_dot_v, 5.0);
        vec3 environment = textureLod(environment_map, reflected, roughness * 5.0).rgb;
#ifndef CRAMION_COMPAT
        float probe_weight = lights.probes[0].w + lights.probes[1].w;
        if (probe_weight > 0.001) {
            vec3 probe = vec3(0.0);
            if (lights.probes[0].w > 0.001) {
                probe += textureLod(reflection_probe_0, reflected, roughness * 5.0).rgb * lights.probes[0].w;
            }
            if (lights.probes[1].w > 0.001) {
                probe += textureLod(reflection_probe_1, reflected, roughness * 5.0).rgb * lights.probes[1].w;
            }
            environment = probe / probe_weight;
        }
#endif
        vec3 ambient = textureLod(environment_map, normal, 5.0).rgb;
        lit += diffuse_color * (1.0 - env_fresnel) * ambient + min(environment, vec3(kMaxRadiance)) * env_fresnel;
    }

    vec3 color = lit * alpha + max(emission, vec3(0.0));
    return vec4(color, blend == kBlendAdditive ? 1.0 : 1.0 - alpha);
}

void main() {
    // Transparentes que no son vidrio (mezcla alfa o aditiva, o sin luz).
    uint blend = (push.pick_id >> 4u) & 3u;
    bool unlit = (push.pick_id & kShadingUnlitBit) != 0u;
    vec2 surface_uv = v_uv * material_uv.xy + material_uv.zw;
    if (blend != kBlendGlass || unlit) {
        out_color = shadeTransparent(blend, unlit, surface_uv);
        // Niebla: tapa lo que no deja pasar (a = lo que pasa de detras).
        out_color.rgb = applyTransparentFog(out_color.rgb, 1.0 - out_color.a, v_world_position);
        return;
    }
    // Emision del vidrio (una pantalla, un letrero): se suma a lo que refleja.
    vec3 glass_emission = toLinear(texture(emissive_map, surface_uv).rgb) * push.emissive.rgb * kEmissiveIntensity *
                          unpackHalf2x16(instance_records[v_instance_slot].info.z).x;

    // Tinte del vidrio (casi siempre blanco): el factor es lineal, la
    // textura sRGB.
    vec3 tint = toLinear(texture(albedo_map, v_uv * material_uv.xy + material_uv.zw).rgb) * push.base_color.rgb;

    vec3 to_camera = camera.position.xyz - v_world_position;
    vec3 view_direction = normalize(to_camera);
    // El vidrio es una lamina: se ve igual por las dos caras. Con su normal
    // map (como skinned.frag): vidrio ondulado, escarchado o con gotas
    // desvia el reflejo, el brillo del sol y lo refractado.
    vec3 geometric_normal = normalize(v_normal);
    vec3 normal = geometric_normal;
    vec3 glass_tangent = v_tangent.xyz - geometric_normal * dot(geometric_normal, v_tangent.xyz);
    if (dot(glass_tangent, glass_tangent) > 1e-8) {
        glass_tangent = normalize(glass_tangent);
        vec3 glass_bitangent = cross(geometric_normal, glass_tangent) * (v_tangent.w < 0.0 ? -1.0 : 1.0);
        vec2 xy = texture(normal_map, surface_uv).xy * 2.0 - 1.0;
        vec3 tangent_normal = vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
        float scale = push.material.w;
        tangent_normal.y = scale >= 0.0 ? -tangent_normal.y : tangent_normal.y;
        tangent_normal.xy *= abs(scale);
        normal = normalize(mat3(glass_tangent, glass_bitangent, geometric_normal) * tangent_normal);
    }
    if (dot(geometric_normal, view_direction) < 0.0) {
        normal = -normal;
    }
    float n_dot_v = max(dot(normal, view_direction), 1e-4);

    // Transmision de Disney: IOR (1..2.5), grosor (0..0.2 m) y rugosidad.
    bool transmission = int(push.pick_id & 0xFu) == kShadingTransmission;
    float ior = 1.5;
    float thickness = 0.0;
    float roughness = kGlassRoughness;
    if (transmission) {
        ior = 1.0 + float((push.pick_id >> 8u) & 0xFFu) / 255.0 * 1.5;
        thickness = float((push.pick_id >> 16u) & 0xFFu) / 255.0 * 0.2;
        roughness = clamp(push.material.y, 0.02, 1.0);
        float f = (ior - 1.0) / (ior + 1.0);
        glass_f0 = f * f;
    }

    float reflectance = slabReflectance(n_dot_v);
    float transmittance = 1.0 - reflectance;

    // --- Reflejo ---
    vec3 reflected = reflect(-view_direction, normal);
    vec3 view_position = (camera.view * vec4(v_world_position, 1.0)).xyz;
    vec3 view_normal = normalize(mat3(camera.view) * normal);
    vec3 view_ray = normalize(mat3(camera.view) * reflected);
    vec3 origin = view_position + view_normal * (0.01 + 0.002 * -view_position.z);
    // Los rayos que vuelven hacia la camara no encuentran nada en pantalla.
    float toward_camera = smoothstep(0.2, 0.6, view_ray.z);
    vec4 screen = toward_camera < 1.0 ? traceScreen(origin, view_ray) : vec4(0.0);
    screen.a *= 1.0 - toward_camera;

    // Respaldo: la sonda (la escena alrededor) o, sin ella, el entorno.
    float lod = roughness * 5.0;
    vec3 fallback = textureLod(environment_map, reflected, lod).rgb;
#ifndef CRAMION_COMPAT
    float probe_weight = lights.probes[0].w + lights.probes[1].w;
    if (probe_weight > 0.001) {
        vec3 probe = vec3(0.0);
        if (lights.probes[0].w > 0.001) {
            probe += textureLod(reflection_probe_0, reflected, lod).rgb * lights.probes[0].w;
        }
        if (lights.probes[1].w > 0.001) {
            probe += textureLod(reflection_probe_1, reflected, lod).rgb * lights.probes[1].w;
        }
        fallback = probe / probe_weight;
    }
#endif
    vec3 reflection = mix(min(fallback, vec3(kMaxRadiance)), screen.rgb, screen.a);

    // --- Brillo del sol en el cristal ---
    // La radiancia del sol reflejada en el cristal casi liso: el lobulo GGX
    // de la lamina (dos caras) iluminado por el sol, con su sombra.
    vec3 sun_direction = normalize(-lights.sun_direction_intensity.xyz);
    float n_dot_l = dot(normal, sun_direction);
    vec3 sun = vec3(0.0);
    if (n_dot_l > 0.0) {
        vec3 halfway = normalize(sun_direction + view_direction);
        float alpha = roughness * roughness;
        float specular = distributionGgx(max(dot(normal, halfway), 0.0), alpha) *
                         visibilitySmith(n_dot_v, n_dot_l, alpha) *
                         slabReflectance(max(dot(view_direction, halfway), 0.0)) * kPi;
        vec3 radiance = toLinear(lights.sun_color_ambient.rgb) * lights.sun_direction_intensity.w;
        sun = radiance * n_dot_l * specular * sunShadow(v_world_position, normal);
        // Sin tope el pico de GGX con esta rugosidad es enorme: se limita al
        // brillo del disco solar.
        sun = min(sun, radiance * kSunDiskRadiance * 0.05);
    }

    // El color del material (Kd del MTL) es el difuso, no cuanto deja pasar:
    // un vidrio con Kd 0.1 no es opaco. Solo se toma su tono (normalizado a
    // su canal mas alto) y lo que atraviesa se atenua segun ese tono. Usar el
    // Kd tal cual dejaba los vasos y botellas casi negros.
    float tint_peak = max(tint.r, max(tint.g, tint.b));
    vec3 hue = tint_peak > 1e-4 ? tint / tint_peak : vec3(1.0);
    float tint_transmission = dot(hue, vec3(0.2126, 0.7152, 0.0722));
    if (transmission) {
        // Lo de detras ya va dentro (refractado y tenido): sustituye.
        vec3 behind = refractedScene(view_position, view_direction, normal, ior, thickness, roughness);
        // Niebla solo en lo que pone el vidrio: lo de detras ya trae la suya.
        vec3 surface_light = applyTransparentFog(reflection * reflectance + sun + glass_emission, reflectance,
                                                 v_world_position);
        out_color = vec4(surface_light + behind * transmittance * hue, 0.0);
        return;
    }
    float glass_pass = transmittance * clamp(tint_transmission, 0.0, 1.0);
    out_color = vec4(applyTransparentFog(reflection * reflectance + sun + glass_emission, 1.0 - glass_pass,
                                         v_world_position),
                     glass_pass);
}
