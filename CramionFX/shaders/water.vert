#version 450
#extension GL_GOOGLE_include_directive : require

// Agua: la malla (disco del oceano, cuadricula del lago o cinta del rio) a su
// sitio en el mundo, desplazada por el oleaje Gerstner.

layout(set = 0, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
} camera;

#include "water_common.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec2 in_flow;

layout(location = 0) out vec3 v_world_position;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec2 v_flow;
layout(location = 4) out vec2 v_grid;      // xz sin desplazar (ruido que no se estira)
layout(location = 5) out float v_jacobian;  // < 1: crestas que rompen (espuma)
layout(location = 6) out float v_height;    // sobre el nivel del agua

void main() {
    WaterBody b = water.bodies[push.body];
    float t = water.time_count.x;

    vec3 base;
    if (push.mesh == 1u) {
        // Oceano: el disco sigue a la camara (sin moverse a saltos visibles).
        vec2 center = floor(camera.position.xz / 4.0) * 4.0;
        base = vec3(center.x + in_position.x, b.origin.y, center.y + in_position.z);
    } else if (push.mesh == 0u) {
        vec2 local = (in_position.xz - 0.5) * 2.0 * b.extent.xy;
        float c = cos(b.origin.w);
        float s = sin(b.origin.w);
        vec2 turned = vec2(c * local.x + s * local.y, -s * local.x + c * local.y);
        base = vec3(b.origin.x + turned.x, b.origin.y, b.origin.z + turned.y);
    } else {
        base = in_position;
    }

    vec3 normal;
    float jacobian;
    float distance_to_camera = length(camera.position.xz - base.xz);
    vec3 offset = gerstnerWaves(b, base.xz, t, max(distance_to_camera, 1.0), normal, jacobian);
    vec3 world = base + offset;

    v_world_position = world;
    v_normal = normal;
    v_uv = in_uv;
    v_flow = in_flow;
    v_grid = base.xz;
    v_jacobian = jacobian;
    v_height = offset.y;
    gl_Position = camera.view_projection * vec4(world, 1.0);
}
