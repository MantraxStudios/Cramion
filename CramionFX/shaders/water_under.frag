#version 450
#extension GL_GOOGLE_include_directive : require

// Bajo el agua (la camara dentro de un oceano o lago): lo que se ve por
// debajo de la superficie se tine y se pierde con la distancia (la misma
// absorcion y luz dispersada que el agua vista desde arriba). Se decide por
// pixel con la ola en el plano cercano: si la linea del agua cruza la
// pantalla, cada mitad se ve como toca.
//
// Lo que hay bajo la superficie (fondo, rocas, el jugador) recibe las
// causticas del sol, con su sombra: desde fuera ya las pinta water.frag al
// mirar a traves del agua, pero desde dentro se ve directamente.

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

#include "water_common.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

vec3 toLinear(vec3 c) { return pow(c, vec3(2.2)); }

vec3 worldFromDepth(vec2 uv, float depth) {
    vec4 world = camera.inverse_view_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return world.xyz / world.w;
}

// Sombra del sol en un punto (la misma que water.frag).
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
    if (projected.z > 1.0 || projected.z < 0.0 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return 1.0;
    }
    float lit = texture(shadow_map, vec4(uv, float(cascade), projected.z - 0.0015));
    return mix(1.0, lit, shadows.params.y);
}

void main() {
    WaterBody b = water.bodies[push.body];
    // El punto de la escena en este pixel y su normal (por las derivadas de
    // la posicion): antes del discard, despues no estan definidas.
    vec3 surface_point = worldFromDepth(v_uv, texelFetch(g_depth, ivec2(gl_FragCoord.xy), 0).r);
    vec3 surface_normal = normalize(cross(dFdx(surface_point), dFdy(surface_point)));
    if (dot(surface_normal, camera.position.xyz - surface_point) < 0.0) surface_normal = -surface_normal;
    float t = water.time_count.x;

    // Punto del plano cercano en este pixel: esta bajo la ola?
    vec3 near_point = worldFromDepth(v_uv, 0.0);
    vec3 normal;
    float jacobian;
    vec3 wave = gerstnerWaves(b, near_point.xz - b.origin.xz, t, 0.0, normal, jacobian);
    int type = int(b.extent.z + 0.5);
    // Rio: la superficie donde esta la camara (extra.z), sin olas.
    float surface = type == 2 ? b.extra.z : b.origin.y + wave.y;
    if (near_point.y > surface) discard;
    // Lago: solo dentro de su rectangulo (fuera no hay agua).
    if (type == 1) {
        float c = cos(b.origin.w);
        float s = sin(b.origin.w);
        vec2 d = near_point.xz - b.origin.xz;
        vec2 local = vec2(d.x * c + d.y * s, -d.x * s + d.y * c);
        if (any(greaterThan(abs(local), b.extent.xy + vec2(0.3)))) discard;
    }

    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(g_depth, pixel, 0).r;
    vec3 scene = texelFetch(scene_color, pixel, 0).rgb;
    float distance = depth >= 1.0 ? 300.0 : length(surface_point - camera.position.xyz);

    vec3 sun_direction = normalize(-lights.sun_direction_intensity.xyz);

    // --- Causticas sobre lo que hay bajo el agua ---
    if (depth < 1.0 && b.look.z > 0.0 && sun_direction.y > 0.0) {
        vec3 n_unused;
        float j_unused;
        vec3 wave_here = gerstnerWaves(b, surface_point.xz - b.origin.xz, t, 0.0, n_unused, j_unused);
        float water_above = (type == 2 ? surface : b.origin.y + wave_here.y) - surface_point.y;  // metros de agua encima
        if (water_above > 0.0) {
            // Proyectadas desde el sol hasta la superficie (se mueven con el
            // punto de entrada de la luz, no con la camara).
            vec2 entry = surface_point.xz + sun_direction.xz / max(sun_direction.y, 0.2) * water_above;
            float facing = smoothstep(-0.1, 0.6, dot(surface_normal, sun_direction));
            float c = caustic(entry - b.origin.xz, t * 1.3) * b.look.z * sunShadow(surface_point) *
                      clamp(sun_direction.y, 0.0, 1.0) * facing *
                      smoothstep(0.0, 0.4, water_above) * exp(-water_above * 0.35);
            scene *= 1.0 + c;
        }
    }
    vec3 sun_radiance = toLinear(lights.sun_color_ambient.rgb) * lights.sun_direction_intensity.w;
    vec3 ambient = textureLod(environment_map, vec3(0.0, 1.0, 0.0), 6.0).rgb +
                   toLinear(lights.ambient_color.rgb) * lights.sun_color_ambient.a;
    // Mas oscuro cuanto mas hondo (la luz llega de arriba).
    float depth_below = max(surface - near_point.y, 0.0);
    float light_left = exp(-depth_below * 0.08);
    vec3 in_scatter = mix(b.deep.rgb, b.shallow.rgb, 0.35) *
                      (sun_radiance * clamp(sun_direction.y, 0.0, 1.0) * 0.35 + ambient) * light_left;

    float clarity = max(b.shallow.w, 0.05);
    vec3 extinction = (vec3(1.05) - clamp(b.shallow.rgb, 0.0, 1.0)) * (2.3 / clarity);
    vec3 transmittance = exp(-extinction * distance);
    out_color = vec4(scene * transmittance + in_scatter * (vec3(1.0) - transmittance), 1.0);
}
