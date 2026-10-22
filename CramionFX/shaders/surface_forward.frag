#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_GOOGLE_cpp_style_line_directive : enable

// Plantilla de fragmentos de los .crshader con `domain transparent`: se
// dibujan en la pasada del vidrio (forward, despues de la iluminacion
// diferida, ordenados de atras adelante) sobre la imagen HDR, y pueden leer
// el color y la profundidad de lo opaco (sceneColor, sceneDepth, SCREEN_UV,
// depthFade): distorsion de calor, ondas de choque, refraccion, escudos y
// hologramas que se funden con la escena.
//
// Sin luz (unlit): lo que se ve es
//     color = albedo (como se ve) * alpha + emision
// y se mezcla premultiplicado (ONE, ONE_MINUS_SRC_ALPHA): alpha 0 suma
// (brillo aditivo), alpha 1 sustituye (con sceneColor desplazado: distorsion).
// Mismo layout que el vidrio (SkinnedPass::glassLayout): set 2 = glass set.

layout(set = 0, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
    mat4 unjittered_view_projection;
    mat4 previous_view_projection;
    vec4 jitter;
    uvec4 motion;
} camera;

// Solo los segundos (TIME).
layout(set = 0, binding = 3) uniform WeatherBuffer {
    mat4 rain_view_projection;
    vec4 params;  // z = segundos
} weather;

layout(set = 1, binding = 0) uniform sampler2D albedo_map;
layout(set = 1, binding = 1) uniform sampler2D metallic_roughness_map;
layout(set = 1, binding = 2) uniform sampler2D normal_map;
layout(set = 1, binding = 3) uniform sampler2D occlusion_map;
layout(set = 1, binding = 4) uniform sampler2D emissive_map;

// Del set del vidrio: la profundidad de lo opaco y la copia de la imagen sin
// lo transparente (VulkanRenderer::copySceneForTransparency).
layout(set = 2, binding = 4) uniform sampler2D cramion_scene_depth;
layout(set = 2, binding = 5) uniform sampler2D cramion_scene_color;

// La luz, del mismo set del vidrio (glass.frag): el sol con su sombra, las
// luces principales y las de los clusteres, y el entorno o la sonda. Sin
// `domain transparent unlit` se ilumina como un .crmat con mezcla alfa; en
// los dos casos lleva la niebla por altura (transparent_fog.glsl). Los .crshader
// no existen en el modo compatible: el set tiene todos los bindings.
struct CramionPointLight {
    vec4 position_range;
    vec4 color_intensity;
    vec4 shadow;
};
struct CramionSpotLight {
    vec4 position_range;
    vec4 direction_intensity;
    vec4 color_inner;
    vec4 outer_shadow;
};
// El principio de GpuLights (lighting.frag), hasta la niebla.
layout(set = 2, binding = 1) uniform LightBuffer {
    vec4 sun_direction_intensity;
    vec4 sun_color_ambient;
    vec4 ambient_color;
    vec4 sky_sun;
    vec4 sky_moon;
    ivec4 counts;
    CramionPointLight points[32];
    CramionSpotLight spots[8];
    vec4 probes[2];
    vec4 clouds;  // y, z = niebla (densidad, caida)
} lights;
const int kCramionShadowCascades = 4;
layout(set = 2, binding = 2) uniform ShadowBuffer {
    mat4 light_view_projection[kCramionShadowCascades];
    vec4 split_distances;
    vec4 texel_world_sizes;
    vec4 params;  // y = intensidad, z = vista del editor
} shadows;
layout(set = 2, binding = 3) uniform sampler2DArrayShadow cramion_shadow_map;
layout(set = 2, binding = 6) uniform samplerCube environment_map;
layout(set = 2, binding = 7) uniform samplerCube cramion_reflection_probe_0;
layout(set = 2, binding = 8) uniform samplerCube cramion_reflection_probe_1;
#define CLUSTER_SET 2
#define CLUSTER_LIGHTS_BINDING 10
#define CLUSTER_GRID_BINDING 11
#define CLUSTER_TRANSLUCENT  // las luces de particula sin "affectsTranslucent" no llegan
#include "light_clusters.glsl"

// Debe coincidir con GpuSkinnedPush.
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 base_color;
    vec4 emissive;
    vec4 material;  // x = metal, y = rugosidad, z = fuerza de la AO, w = escala normal
    uint bone_offset;
    float reflectance;
    uint pick_id;  // aqui: bloque de propiedades del material
    uint flags;
} push;

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_tangent;
layout(location = 3) in vec3 v_world_position;
// Hueco de la instancia (surface.vert): su registro, como en glass.frag.
layout(location = 9) flat in uint v_instance_slot;

// Registro de la instancia (GpuLightmapRecord: lightmap_common.glsl): aqui
// los parametros por instancia del MeshRenderer (info.y = tinte y opacidad
// RGBA8, info.z = escala de la emision en half float) e INSTANCE_DATA.
struct InstanceRecord {
    vec4 scale_offset;
    uvec4 info;
    vec4 data[2];
};
layout(std430, set = 0, binding = 8) readonly buffer InstanceRecords {
    InstanceRecord instance_records[];
};
vec4 instanceData(uint i) {
    return i < 2u ? instance_records[v_instance_slot].data[i] : vec4(0.0);
}
#define INSTANCE_DATA(i) instanceData(uint(i))

layout(location = 0) out vec4 out_color;

const float kEmissiveIntensity = 6.0;

// Las de gbuffer_surface.glsl (el codigo del usuario puede llamarlas).
vec3 toLinear(vec3 color) {
    return pow(color, vec3(2.2));
}
vec3 toSrgb(vec3 color) {
    return pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));
}

#define CRAMION_FRAGMENT_STAGE 1
#define CRAMION_SCENE_TEXTURES 1
#define CRAMION_TRANSPARENT 1
#include "surface_common.glsl"
#include "surface_user.glsl"

// Niebla por altura (la de lighting.frag) sobre lo que se suma.
#include "transparent_fog.glsl"

// s.refraction: lo opaco de detras (sceneColor) en SCREEN_UV desviado por la
// normal en la vista. Si el punto desviado esta delante de la superficie (un
// objeto que la tapa), el de detras sin desviar: sin fantasmas de lo de delante.
vec3 cramionRefraction(Surface s) {
    vec2 uv = SCREEN_UV;
    vec3 view_normal = mat3(camera.view) * normalize(s.normal);
    // En la pantalla de Vulkan la y crece hacia abajo.
    vec2 refracted = uv + vec2(view_normal.x, -view_normal.y) * s.refraction;
    float surface_depth = -(camera.view * vec4(s.worldPosition, 1.0)).z;
    if (sceneLinearDepth(refracted) < surface_depth) refracted = uv;
    return sceneColor(refracted);
}

#ifndef CRAMION_UNLIT
// --- Luz (la de los transparentes de glass.frag) ---
const float kCramionPi = 3.14159265;

// Sombra del sol: cascadas, una muestra filtrada por hardware.
float cramionSunShadow(vec3 world_position, vec3 normal) {
    float view_depth = -(camera.view * vec4(world_position, 1.0)).z;
    int cascade = kCramionShadowCascades - 1;
    for (int i = 0; i < kCramionShadowCascades; ++i) {
        if (view_depth < shadows.split_distances[i]) {
            cascade = i;
            break;
        }
    }
    vec3 offset_position = world_position + normal * shadows.texel_world_sizes[cascade] * 1.5;
    vec4 light_clip = shadows.light_view_projection[cascade] * vec4(offset_position, 1.0);
    vec3 projected = light_clip.xyz / light_clip.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    if (projected.z > 1.0 || projected.z < 0.0 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return 1.0;
    }
    float lit = texture(cramion_shadow_map, vec4(uv, float(cascade), projected.z));
    return mix(1.0, lit, shadows.params.y);
}

// Difuso de Lambert + GGX de una luz (las luces ya llevan pi).
vec3 cramionLight(vec3 light_direction, vec3 radiance, vec3 normal, vec3 view_direction, vec3 diffuse_color,
                  vec3 f0, float roughness) {
    float n_dot_l = dot(normal, light_direction);
    if (n_dot_l <= 0.0) return vec3(0.0);
    vec3 halfway = normalize(light_direction + view_direction);
    float a = max(roughness * roughness, 0.002);
    float a2 = a * a;
    float n_dot_v = max(dot(normal, view_direction), 1e-4);
    float n_dot_h = max(dot(normal, halfway), 0.0);
    float dd = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    float distribution = a2 / (kCramionPi * dd * dd);
    float ggx_v = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2);
    float ggx_l = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2);
    float visibility = 0.5 / max(ggx_v + ggx_l, 0.00001);
    float m = 1.0 - clamp(dot(view_direction, halfway), 0.0, 1.0);
    float m2 = m * m;
    vec3 f = f0 + (1.0 - f0) * (m2 * m2 * m);
    return radiance * n_dot_l * (diffuse_color * (1.0 - f) + f * distribution * visibility * kCramionPi);
}

// Caida de una luz local (la de lighting.frag).
float cramionAttenuation(float d, float range) {
    float ratio = d / max(range, 0.0001);
    float window = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
    return (window * window) / (d * d + 1.0);
}

// Radiancia (lineal) que refleja la superficie hacia la camara.
vec3 cramionShade(Surface s) {
    vec3 base = toLinear(clamp(s.albedo, 0.0, 1.0));
    vec3 view_direction = normalize(camera.position.xyz - v_world_position);
    vec3 normal = normalize(s.normal);
    // Por las dos caras (escudos, burbujas): la normal mira a la camara.
    if (dot(normal, view_direction) < 0.0 && dot(s.vertexNormal, view_direction) < 0.0) normal = -normal;
    float metallic = clamp(s.metallic, 0.0, 1.0);
    float roughness = clamp(s.roughness, 0.04, 1.0);
    vec3 f0 = mix(vec3(push.reflectance), base, metallic);
    vec3 diffuse_color = base * (1.0 - metallic);

    // Sol, con su sombra.
    vec3 sun_direction = normalize(-lights.sun_direction_intensity.xyz);
    vec3 sun_radiance = toLinear(lights.sun_color_ambient.rgb) * lights.sun_direction_intensity.w;
    vec3 lit = cramionLight(sun_direction, sun_radiance, normal, view_direction, diffuse_color, f0, roughness);
    if (dot(lit, lit) > 0.0) lit *= cramionSunShadow(v_world_position, normal);

    // Luces principales (sin sombras: son pocos pixeles) y las de los clusteres.
    int point_count = min(lights.counts.x, 32);
    for (int i = 0; i < point_count; ++i) {
        vec3 to_light = lights.points[i].position_range.xyz - v_world_position;
        float d = length(to_light);
        if (d > lights.points[i].position_range.w) continue;
        vec3 radiance = lights.points[i].color_intensity.rgb * lights.points[i].color_intensity.a *
                        cramionAttenuation(d, lights.points[i].position_range.w);
        lit += cramionLight(to_light / max(d, 0.0001), radiance, normal, view_direction, diffuse_color, f0, roughness);
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
                        cramionAttenuation(d, lights.spots[i].position_range.w);
        lit += cramionLight(l, radiance, normal, view_direction, diffuse_color, f0, roughness);
    }
    uvec2 cluster_range = clusterRange(gl_FragCoord.xy, v_world_position);
    for (uint k = 0u; k < cluster_range.y; ++k) {
        ClusterLight light = cluster_lights[cluster_items[cluster_range.x + k]];
        vec3 l;
        float d;
        vec3 radiance;
        if (!clusterLightIncoming(light, v_world_position, l, d, radiance)) continue;
        lit += cramionLight(l, radiance, normal, view_direction, diffuse_color, f0, roughness);
    }

    // Ambiente: el entorno muy filtrado como difuso (con la oclusion) y su
    // reflejo (la sonda si la hay) con el Fresnel de la rugosidad.
    vec3 reflected = reflect(-view_direction, normal);
    float n_dot_v = max(dot(normal, view_direction), 1e-4);
    vec3 env_fresnel = f0 + (max(vec3(1.0 - roughness), f0) - f0) * pow(1.0 - n_dot_v, 5.0);
    vec3 environment = textureLod(environment_map, reflected, roughness * 5.0).rgb;
    float probe_weight = lights.probes[0].w + lights.probes[1].w;
    if (probe_weight > 0.001) {
        vec3 probe = vec3(0.0);
        if (lights.probes[0].w > 0.001) {
            probe += textureLod(cramion_reflection_probe_0, reflected, roughness * 5.0).rgb * lights.probes[0].w;
        }
        if (lights.probes[1].w > 0.001) {
            probe += textureLod(cramion_reflection_probe_1, reflected, roughness * 5.0).rgb * lights.probes[1].w;
        }
        environment = probe / probe_weight;
    }
    vec3 ambient = textureLod(environment_map, normal, 5.0).rgb;
    float occlusion = clamp(s.occlusion, 0.0, 1.0);
    lit += (diffuse_color * (1.0 - env_fresnel) * ambient + min(environment, vec3(30.0)) * env_fresnel) * occlusion;
    return lit;
}
#endif

void main() {
    // Base tangente y normal map, como surface.frag (sin parallax).
    vec3 n = normalize(v_normal);
    vec3 t = v_tangent.xyz - n * dot(n, v_tangent.xyz);
    vec3 normal = n;
    if (dot(t, t) > 1e-8) {
        t = normalize(t);
        vec3 b = cross(n, t) * (v_tangent.w < 0.0 ? -1.0 : 1.0);
        vec2 xy = texture(normal_map, v_uv).xy * 2.0 - 1.0;
        vec3 tangent_normal = vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
        float scale = push.material.w;
        tangent_normal.y = scale >= 0.0 ? -tangent_normal.y : tangent_normal.y;
        tangent_normal.xy *= abs(scale);
        normal = normalize(mat3(t, b, n) * tangent_normal);
    }
    if (!gl_FrontFacing) {
        normal = -normal;
        n = -n;
    }

    vec4 albedo = texture(albedo_map, v_uv) *
                  vec4(pow(max(push.base_color.rgb, vec3(0.0)), vec3(1.0 / 2.2)), push.base_color.a);
    vec4 metallic_roughness = texture(metallic_roughness_map, v_uv);

    Surface s;
    s.albedo = albedo.rgb;
    s.alpha = albedo.a;
    s.normal = normal;
    s.metallic = clamp(push.material.x * metallic_roughness.b, 0.0, 1.0);
    s.roughness = clamp(push.material.y * metallic_roughness.g, 0.0, 1.0);
    s.occlusion = mix(1.0, texture(occlusion_map, v_uv).r, push.material.z);
    s.emission = toLinear(texture(emissive_map, v_uv).rgb) * push.emissive.rgb;
    s.refraction = 0.0;
    s.uv = v_uv;
    s.worldPosition = v_world_position;
    s.vertexNormal = n;
    s.viewDirection = normalize(camera.position.xyz - v_world_position);

    surface(s);

    // Parametros por instancia (MeshRenderer) sobre lo que deje el shader,
    // como surface.frag: tinte (lineal, al sRGB del albedo), opacidad (aqui
    // mezcla) y escala de la emision.
    vec4 instance_tint = unpackUnorm4x8(instance_records[v_instance_slot].info.y);
    s.albedo *= pow(instance_tint.rgb, vec3(1.0 / 2.2));
    s.alpha *= instance_tint.a;
    s.emission *= unpackHalf2x16(instance_records[v_instance_slot].info.z).x * instance_tint.a;

    // Premultiplicado. El alfa de salida tambien se acumula en el del destino
    // (la mascara reactiva del TAA y de FSR 3; SkinnedPass::createSurfacePipeline).
    float alpha = clamp(s.alpha, 0.0, 1.0);
#ifdef CRAMION_UNLIT
    vec3 lit = toLinear(clamp(s.albedo, 0.0, 1.0));
#else
    // Iluminado: lo que refleja (difuso y brillo) por alpha, como los .crmat
    // con mezcla alfa. albedo = 0 deja solo el brillo (vidrio, agua).
    vec3 lit = cramionShade(s);
#endif
    vec3 color = lit * alpha + max(s.emission, vec3(0.0)) * kEmissiveIntensity;
    // Niebla: tapa en proporcion a alpha (alpha 0 = aditivo: solo se apaga).
    color = applyTransparentFog(color, alpha, v_world_position);
    // Refraccion: lo de detras (ya con su niebla) entra desviado por
    // (1 - alpha) y sustituye al destino (alfa 1 con la mezcla premultiplicada).
    // Lo transparente que se dibujo antes detras no sale en la copia.
    if (s.refraction > 0.0) {
        out_color = vec4(color + cramionRefraction(s) * (1.0 - alpha), 1.0);
        return;
    }
    out_color = vec4(color, alpha);
}
