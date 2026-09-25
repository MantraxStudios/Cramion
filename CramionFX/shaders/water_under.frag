#version 450
#extension GL_GOOGLE_include_directive : require

// Bajo el agua (la camara dentro de un oceano o lago): lo que se ve por
// debajo de la superficie se tine y se pierde con la distancia (la misma
// absorcion y luz dispersada que el agua vista desde arriba). Se decide por
// pixel con la ola en el plano cercano: si la linea del agua cruza la
// pantalla, cada mitad se ve como toca.

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

void main() {
    WaterBody b = water.bodies[push.body];
    float t = water.time_count.x;

    // Punto del plano cercano en este pixel: esta bajo la ola?
    vec3 near_point = worldFromDepth(v_uv, 0.0);
    vec3 normal;
    float jacobian;
    vec3 wave = gerstnerWaves(b, near_point.xz, t, 0.0, normal, jacobian);
    if (near_point.y > b.origin.y + wave.y) discard;

    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(g_depth, pixel, 0).r;
    vec3 scene = texelFetch(scene_color, pixel, 0).rgb;
    float distance = depth >= 1.0 ? 300.0 : length(worldFromDepth(v_uv, depth) - camera.position.xyz);

    vec3 sun_direction = normalize(-lights.sun_direction_intensity.xyz);
    vec3 sun_radiance = toLinear(lights.sun_color_ambient.rgb) * lights.sun_direction_intensity.w;
    vec3 ambient = textureLod(environment_map, vec3(0.0, 1.0, 0.0), 6.0).rgb +
                   toLinear(lights.ambient_color.rgb) * lights.sun_color_ambient.a;
    // Mas oscuro cuanto mas hondo (la luz llega de arriba).
    float depth_below = max(b.origin.y - near_point.y, 0.0);
    float light_left = exp(-depth_below * 0.08);
    vec3 in_scatter = mix(b.deep.rgb, b.shallow.rgb, 0.35) *
                      (sun_radiance * clamp(sun_direction.y, 0.0, 1.0) * 0.35 + ambient) * light_left;

    float clarity = max(b.shallow.w, 0.05);
    vec3 extinction = (vec3(1.05) - clamp(b.shallow.rgb, 0.0, 1.0)) * (2.3 / clarity);
    vec3 transmittance = exp(-extinction * distance);
    out_color = vec4(scene * transmittance + in_scatter * (vec3(1.0) - transmittance), 1.0);
}
