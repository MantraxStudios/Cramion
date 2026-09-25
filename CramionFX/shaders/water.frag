#version 450
#extension GL_GOOGLE_include_directive : require

// Agua procedural (sin texturas), como el agua de los mundos abiertos:
//
//   - Normal: oleaje Gerstner (vertice) + ondulacion fina del viento (ondas
//     pequenas y ruido), que en el rio corre con la corriente.
//   - Grosor del agua (profundidad de la escena detras): el fondo se ve a
//     traves, cada vez mas tenido y oscuro (absorcion por color y luz
//     dispersada en el agua), refractado por la normal.
//   - Reflejo: trazado en pantalla (lo que se ve) y, si no, la sonda o el
//     cielo; Fresnel del agua (F0 = 0.02) y el brillo del sol (GGX) con sombra.
//   - Luz a traves de las crestas (subsurface) mirando hacia el sol.
//   - Espuma: orilla (poco grosor), olas que rompen en la playa, crestas
//     (jacobiano del oleaje) y orillas y remolinos del rio.
//   - Causticas en el fondo poco profundo.

layout(set = 2, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
} camera;

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
} lights;

const int kShadowCascadeCount = 4;
layout(set = 2, binding = 2) uniform ShadowBuffer {
    mat4 light_view_projection[kShadowCascadeCount];
    vec4 split_distances;
    vec4 texel_world_sizes;
    vec4 params;
} shadows;
layout(set = 2, binding = 3) uniform sampler2DArrayShadow shadow_map;
layout(set = 2, binding = 4) uniform sampler2D g_depth;
layout(set = 2, binding = 5) uniform sampler2D scene_color;
layout(set = 2, binding = 6) uniform samplerCube environment_map;
layout(set = 2, binding = 7) uniform samplerCube reflection_probe_0;
layout(set = 2, binding = 8) uniform samplerCube reflection_probe_1;

#include "water_common.glsl"

layout(location = 0) in vec3 v_world_position;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec2 v_flow;
layout(location = 4) in vec2 v_grid;
layout(location = 5) in float v_jacobian;
layout(location = 6) in float v_height;

layout(location = 0) out vec4 out_color;

const float kMaxRadiance = 40.0;
const float kSunDiskRadiance = 900.0;

vec3 toLinear(vec3 c) { return pow(c, vec3(2.2)); }

// --- Ruido ---
float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}
float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 4; ++i) {
        v += a * noise(p);
        p = mat2(1.6, 1.2, -1.2, 1.6) * p;
        a *= 0.5;
    }
    return v;
}

// --- Pantalla ---
float linearDepth(float depth) { return camera.projection[3][2] / (depth + camera.projection[2][2]); }

vec3 worldFromDepth(vec2 uv, float depth) {
    vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 world = camera.inverse_view_projection * clip;
    return world.xyz / world.w;
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
bool insideScreen(vec2 uv) { return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0))); }

// Reflejo en pantalla (como el vidrio): rgb = color, a = confianza.
vec4 traceScreen(vec3 origin, vec3 ray) {
    const int kSteps = 32;
    const float kMaxDistance = 120.0;
    const float kFirstStep = 0.08;
    ivec2 size = textureSize(g_depth, 0);
    float growth = pow(kMaxDistance / kFirstStep, 1.0 / float(kSteps));
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float previous_distance = 0.0;
    float distance_along = kFirstStep * mix(1.0, growth, jitter);
    for (int i = 0; i < kSteps; ++i) {
        vec3 sample_position = origin + ray * distance_along;
        vec2 uv;
        if (!project(sample_position, uv) || !insideScreen(uv)) break;
        ivec2 texel = min(ivec2(uv * vec2(size)), size - 1);
        float scene_depth = texelFetch(g_depth, texel, 0).r;
        float difference = -sample_position.z - linearDepth(scene_depth);
        float thickness = (distance_along - previous_distance) * 1.5 + 0.1;
        if (scene_depth < 1.0 && difference > 0.0 && difference < thickness) {
            vec3 color = min(textureLod(scene_color, uv, 0.0).rgb, vec3(kMaxRadiance));
            vec2 edge = min(uv, 1.0 - uv);
            float edge_fade = smoothstep(0.0, 0.1, min(edge.x, edge.y));
            float distance_fade = 1.0 - smoothstep(0.6, 1.0, distance_along / kMaxDistance);
            return vec4(color, edge_fade * distance_fade);
        }
        previous_distance = distance_along;
        distance_along *= growth;
    }
    return vec4(0.0);
}

float sunShadow(vec3 world_position) {
    float view_depth = -(camera.view * vec4(world_position, 1.0)).z;
    int cascade = kShadowCascadeCount - 1;
    for (int i = 0; i < kShadowCascadeCount; ++i) {
        if (view_depth < shadows.split_distances[i]) {
            cascade = i;
            break;
        }
    }
    vec4 light_clip = shadows.light_view_projection[cascade] * vec4(world_position, 1.0);
    vec3 projected = light_clip.xyz / light_clip.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    if (projected.z > 1.0 || projected.z < 0.0 || !insideScreen(uv)) return 1.0;
    float lit = texture(shadow_map, vec4(uv, float(cascade), projected.z - 0.0015));
    return mix(1.0, lit, shadows.params.y);
}

float distributionGgx(float n_dot_h, float alpha) {
    float alpha2 = alpha * alpha;
    float d = n_dot_h * n_dot_h * (alpha2 - 1.0) + 1.0;
    return alpha2 / (kWaterPi * d * d);
}

// Ondulacion fina: ondas cortas en varias direcciones (y con la corriente).
vec2 detailSlope(vec2 p, float t, vec2 flow, float strength) {
    const vec2 dirs[6] = vec2[](vec2(0.94, 0.34), vec2(-0.57, 0.82), vec2(0.21, -0.98), vec2(-0.99, -0.12),
                                vec2(0.71, 0.70), vec2(-0.30, -0.95));
    const float lengths[6] = float[](1.9, 1.31, 0.83, 0.61, 0.37, 0.23);
    vec2 slope = vec2(0.0);
    for (int i = 0; i < 6; ++i) {
        float k = 2.0 * kWaterPi / lengths[i];
        float omega = sqrt(9.81 * k);
        vec2 q = p - flow * t;
        float theta = k * dot(dirs[i], q) - omega * t * 0.6 + float(i) * 2.1;
        slope += dirs[i] * cos(theta) * (0.011 * lengths[i]) * k;
    }
    // Rizado irregular (que no se vea el patron de ondas).
    vec2 q = (p - flow * t) * 1.7;
    float e = 0.15;
    float n0 = fbm(q + t * 0.35);
    slope += vec2(fbm(q + vec2(e, 0.0) + t * 0.35) - n0, fbm(q + vec2(0.0, e) + t * 0.35) - n0) / e * 0.035;
    return slope * strength;
}

float caustic(vec2 p, float t) {
    vec2 q = p * 0.7;
    float c = 0.0;
    float s = 1.0;
    for (int i = 0; i < 3; ++i) {
        q += vec2(sin(q.y * 1.7 + t * 0.9), cos(q.x * 1.3 - t * 0.7)) * 0.55;
        c += abs(sin(q.x) * cos(q.y)) * s;
        s *= 0.6;
        q *= 1.9;
    }
    return pow(clamp(1.0 - c * 0.55, 0.0, 1.0), 5.0) * 4.0;
}

void main() {
    WaterBody b = water.bodies[push.body];
    float t = water.time_count.x;
    bool river = push.mesh == 2u;

    vec3 to_camera = camera.position.xyz - v_world_position;
    float surface_distance = length(to_camera);
    vec3 view_direction = to_camera / max(surface_distance, 1e-4);
    // Camara bajo el agua: por debajo del nivel medio (sin la ola), no de
    // cada cresta (una ola mas alta que la camara no es verla desde abajo).
    bool below = camera.position.y < v_world_position.y - v_height - 0.05;

    // --- Normal ---
    vec2 flow = river ? v_flow * b.wind.z : vec2(0.0);
    float detail = b.wind.w / (1.0 + surface_distance * 0.06);
    vec2 slope = detailSlope(v_grid, t, flow, detail);
    vec3 normal = normalize(vec3(v_normal.x - slope.x, v_normal.y, v_normal.z - slope.y));
    if (below) normal = -normal;
    float facing = dot(normal, view_direction);
    if (facing < 0.05) normal = normalize(normal + view_direction * (0.05 - facing));
    float n_dot_v = max(dot(normal, view_direction), 1e-3);

    // --- Lo que hay detras (profundidad de la escena) ---
    vec2 size = vec2(textureSize(g_depth, 0));
    vec2 screen_uv = gl_FragCoord.xy / size;
    float scene_depth = texelFetch(g_depth, ivec2(gl_FragCoord.xy), 0).r;
    bool sky_behind = scene_depth >= 1.0;
    vec3 floor_position = worldFromDepth(screen_uv, scene_depth);
    float vertical_depth = sky_behind ? 1000.0 : max(v_world_position.y - floor_position.y, 0.0);

    // Refraccion: desviada por la normal, menos en lo poco profundo y lejos.
    vec2 refract_offset = normal.xz * b.look.y * 0.06 * clamp(vertical_depth, 0.0, 1.0) / (1.0 + surface_distance * 0.05);
    vec2 refract_uv = clamp(screen_uv + refract_offset, vec2(0.001), vec2(0.999));
    float refract_depth = textureLod(g_depth, refract_uv, 0.0).r;
    vec3 refract_floor = worldFromDepth(refract_uv, refract_depth);
    // Si el punto desviado esta delante del agua, se usa el directo.
    if (refract_depth < 1.0 && dot(refract_floor - camera.position.xyz, -view_direction) < surface_distance) {
        refract_uv = screen_uv;
        refract_depth = scene_depth;
        refract_floor = floor_position;
    }
    vec3 refracted = min(textureLod(scene_color, refract_uv, 0.0).rgb, vec3(kMaxRadiance));
    float thickness = refract_depth >= 1.0 ? 1000.0 : max(length(refract_floor - camera.position.xyz) - surface_distance, 0.0);

    // --- Luz ---
    vec3 sun_direction = normalize(-lights.sun_direction_intensity.xyz);
    vec3 sun_radiance = toLinear(lights.sun_color_ambient.rgb) * lights.sun_direction_intensity.w;
    float shadow = sunShadow(v_world_position);
    vec3 sky_light = textureLod(environment_map, vec3(0.0, 1.0, 0.0), 6.0).rgb;
    vec3 ambient = sky_light + toLinear(lights.ambient_color.rgb) * lights.sun_color_ambient.a;
    float sun_height = clamp(sun_direction.y, 0.0, 1.0);

    // --- Absorcion y dispersion en el volumen de agua ---
    float clarity = max(b.shallow.w, 0.05);
    vec3 extinction = (vec3(1.05) - clamp(b.shallow.rgb, 0.0, 1.0)) * (2.3 / clarity);
    vec3 transmittance = exp(-extinction * thickness);
    vec3 in_scatter = b.deep.rgb * (sun_radiance * sun_height * shadow * 0.35 + ambient);
    // Causticas en el fondo poco profundo.
    if (refract_depth < 1.0 && b.look.z > 0.0) {
        float floor_depth = max(v_world_position.y - refract_floor.y, 0.0);
        float c = caustic(refract_floor.xz - flow * t, t * 1.3) * b.look.z * shadow * sun_height *
                  smoothstep(0.0, 0.4, floor_depth) * exp(-floor_depth * 0.35);
        refracted *= 1.0 + c;
    }
    vec3 water_color = refracted * transmittance + in_scatter * (vec3(1.0) - transmittance);

    // Luz a traves de las crestas mirando hacia el sol.
    float crest = clamp(v_height / max(b.waves.x, 0.05), 0.0, 1.0);
    float toward_sun = pow(max(dot(-view_direction, sun_direction) * 0.5 + 0.5, 0.0), 6.0);
    water_color += b.shallow.rgb * sun_radiance * shadow * toward_sun * crest * b.extra.y * 0.12;
    // Las crestas, mas finas, dejan pasar mas luz del cielo: mas claras y verdes.
    water_color += b.shallow.rgb * ambient * crest * crest * b.extra.y * 0.25;

    // --- Reflejo ---
    vec3 reflected = reflect(-view_direction, normal);
    reflected.y = below ? reflected.y : max(reflected.y, 0.02);
    // Lejos, la ondulacion que ya no se ve se vuelve rugosidad (sin centelleo).
    float roughness = clamp(b.look.x + surface_distance * 0.0012 * b.wind.w, 0.01, 0.6);
    vec3 view_position = (camera.view * vec4(v_world_position, 1.0)).xyz;
    vec3 view_ray = normalize(mat3(camera.view) * reflected);
    float toward_camera = smoothstep(0.2, 0.6, view_ray.z);
    vec4 screen = (!below && toward_camera < 1.0) ? traceScreen(view_position + mat3(camera.view) * normal * 0.05, view_ray)
                                                   : vec4(0.0);
    screen.a *= 1.0 - toward_camera;
    float lod = roughness * 6.0;
    vec3 fallback = textureLod(environment_map, reflected, lod).rgb;
    float probe_weight = lights.probes[0].w + lights.probes[1].w;
    if (probe_weight > 0.001) {
        vec3 probe = vec3(0.0);
        if (lights.probes[0].w > 0.001) probe += textureLod(reflection_probe_0, reflected, lod).rgb * lights.probes[0].w;
        if (lights.probes[1].w > 0.001) probe += textureLod(reflection_probe_1, reflected, lod).rgb * lights.probes[1].w;
        fallback = mix(fallback, probe / probe_weight, 0.7);
    }
    vec3 reflection = mix(min(fallback, vec3(kMaxRadiance)), screen.rgb, screen.a);
    float fresnel = 0.02 + 0.98 * pow(1.0 - n_dot_v, 5.0);
    if (below) {
        // Por debajo: reflexion total pasado el angulo critico (~48 grados).
        fresnel = smoothstep(0.72, 0.6, n_dot_v);
    }

    // Brillo del sol (GGX) con su sombra.
    vec3 specular = vec3(0.0);
    float n_dot_l = dot(normal, sun_direction);
    if (n_dot_l > 0.0 && !below) {
        vec3 halfway = normalize(sun_direction + view_direction);
        float alpha = max(roughness * roughness, 0.0015);
        float d = distributionGgx(max(dot(normal, halfway), 0.0), alpha);
        specular = sun_radiance * min(d * fresnel * n_dot_l * 0.25 / n_dot_v, kSunDiskRadiance * 0.05) * shadow;
    }

    vec3 color = water_color * (1.0 - fresnel) + reflection * fresnel + specular;
    if (below) {
        // Desde abajo: lo de arriba (aire) a traves de la superficie, reflexion
        // total pasado el angulo critico, y el agua entre la camara y la
        // superficie la tine con la distancia.
        vec3 above = min(textureLod(scene_color, refract_uv, 0.0).rgb, vec3(kMaxRadiance));
        vec3 total_reflection = in_scatter * 0.8;
        vec3 surface = mix(above, total_reflection, fresnel);
        vec3 fog = exp(-extinction * surface_distance);
        color = surface * fog + in_scatter * (vec3(1.0) - fog);
    }

    // --- Espuma ---
    vec2 foam_uv = v_grid - flow * t;
    float pattern = smoothstep(0.35, 0.75, fbm(foam_uv * 1.4 + t * 0.12) * 0.7 + fbm(foam_uv * 4.3 - t * 0.2) * 0.5);
    float shore_width = max(b.look.w, 0.01);
    float shore = 1.0 - smoothstep(0.0, shore_width, vertical_depth);
    // Olas que llegan a la playa: bandas que avanzan hacia la orilla.
    float bands = 0.0;
    if (b.extra.x > 0.0 && !sky_behind) {
        float phase = vertical_depth * 2.2 - t * 1.4 + fbm(v_grid * 0.15) * 3.0;
        bands = smoothstep(0.55, 1.0, sin(phase)) * (1.0 - smoothstep(0.0, shore_width * 3.0, vertical_depth)) * b.extra.x;
    }
    float crest_foam = smoothstep(0.55, 0.05, v_jacobian) * smoothstep(0.2, 0.8, crest);
    float river_foam = 0.0;
    if (river) {
        float bank = smoothstep(0.32, 0.5, abs(v_uv.x - 0.5));
        river_foam = bank * 0.6 + smoothstep(0.62, 0.85, fbm(foam_uv * 0.8)) * clamp(b.wind.z * 0.25, 0.0, 1.0);
    }
    float foam = clamp((shore * 0.9 + bands + crest_foam + river_foam) * pattern * b.deep.w, 0.0, 1.0);
    vec3 foam_color = vec3(0.92) * (sun_radiance * max(n_dot_l, 0.0) * shadow + ambient);
    color = mix(color, foam_color, foam);

    // Bruma a lo lejos (como el cielo en el horizonte).
    float haze = 1.0 - exp(-surface_distance * 0.00012);
    color = mix(color, textureLod(environment_map, normalize(vec3(-view_direction.x, 0.02, -view_direction.z)), 3.0).rgb, haze);

    // Borde suave donde el agua toca el suelo.
    float alpha = (sky_behind || below) ? 1.0 : clamp(vertical_depth / 0.06, 0.0, 1.0);
    if (any(isnan(color))) color = vec3(0.0);
    out_color = vec4(min(color, vec3(kMaxRadiance)), alpha);
}
