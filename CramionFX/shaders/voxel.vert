#version 450
#extension GL_GOOGLE_include_directive : require

// Voxeles (VoxelPass): desempaqueta el vertice (8 bytes) y lo coloca en la
// seccion. Las hojas y las plantas ondean con el viento. Con `shadow` = 1 se
// proyecta con la matriz de la luz (cascadas).

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

#include "voxel_common.glsl"

layout(location = 0) in uvec2 in_packed;

layout(location = 0) out vec3 v_world_position;
layout(location = 1) out vec2 v_uv;
layout(location = 2) flat out uvec2 v_face_layer;  // x cara, y capa
layout(location = 3) out vec3 v_light;             // x oclusion de la esquina, y cielo, z antorchas (0..1)
layout(location = 4) out vec3 v_tint;
layout(location = 5) out vec4 v_current_clip;
layout(location = 6) out vec4 v_previous_clip;
layout(location = 7) out vec3 v_to_camera;

void main() {
    uint a = in_packed.x;
    uint b = in_packed.y;
    vec3 local = vec3(float(a & 31u), float((a >> 5u) & 31u), float((a >> 10u) & 31u));
    uint face = (a >> 15u) & 7u;
    vec2 uv = vec2(float((a >> 18u) & 1u), float((a >> 19u) & 1u));
    float corner_ao = float((a >> 20u) & 3u) / 3.0;
    bool waves = ((a >> 22u) & 1u) != 0u;
    uint layer = b & 1023u;
    float sky = float((b >> 10u) & 15u) / 15.0;
    float torch = float((b >> 14u) & 15u) / 15.0;
    vec3 tint = vec3(float((b >> 18u) & 15u), float((b >> 22u) & 15u), float((b >> 26u) & 15u)) / 15.0;

    vec3 world = push.origin_time.xyz + local;
    if (waves) {
        // Viento: las plantas se mueven por arriba (v = 0), las hojas enteras y poco.
        float t = push.origin_time.w;
        float phase = dot(world.xz, vec2(0.37, 0.29)) + world.y * 0.21;
        vec2 sway = vec2(sin(t * 1.6 + phase), cos(t * 1.25 + phase * 1.31)) * 0.055;
        float weight = face == 6u ? (1.0 - uv.y) * 1.6 : 0.45;
        world.xz += sway * weight;
        world.y += sin(t * 2.1 + phase * 1.7) * 0.012 * weight;
    }

    v_world_position = world;
    v_uv = uv;
    v_face_layer = uvec2(face, layer);
    v_light = vec3(corner_ao, sky, torch);
    v_tint = tint;
    v_to_camera = camera.position.xyz - world;
    v_current_clip = camera.unjittered_view_projection * vec4(world, 1.0);
    v_previous_clip = camera.previous_view_projection * vec4(world, 1.0);
    gl_Position = push.shadow != 0u ? push.light_view_projection * vec4(world, 1.0)
                                    : camera.view_projection * vec4(world, 1.0);
}
