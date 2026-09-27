#version 450
#extension GL_GOOGLE_include_directive : require

// Vegetacion instanciada (FoliagePass): la instancia sale de la lista de
// visibles (gl_InstanceIndex incluye el inicio de la lista), se gira, se
// escala y la copa se mece con el viento. Con `shadow` se proyecta con la
// matriz de la luz (cascadas).

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

#include "foliage_common.glsl"

layout(std430, set = 1, binding = 0) readonly buffer Instances {
    FoliageInstance instances[];
};
layout(std430, set = 1, binding = 1) readonly buffer Visible {
    uint visible[];
};

layout(push_constant) uniform FoliagePush {
    mat4 light_view_projection;
    vec4 params;  // x segundos, y sombra (0/1), z viento
    vec4 offset;  // xyz origen flotante
} push;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_color;  // rgb color (sRGB), a oclusion
layout(location = 3) in float in_wind;  // 0 = rigido (tronco), 1 = punta de la copa

layout(location = 0) out vec3 v_world_position;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec4 v_color;
layout(location = 3) out vec4 v_current_clip;
layout(location = 4) out vec4 v_previous_clip;

void main() {
    FoliageInstance inst = instances[visible[gl_InstanceIndex]];
    float yaw = foliageYaw(inst.packed);
    float scale = foliageScale(inst.packed);
    float tint = foliageTint(inst.packed);
    float c = cos(yaw), s = sin(yaw);
    vec3 local = in_position * scale;
    vec3 rotated = vec3(c * local.x + s * local.z, local.y, -s * local.x + c * local.z);
    vec3 base = inst.position - push.offset.xyz;
    vec3 world = base + rotated;

    // Viento: la copa se inclina y tiembla un poco, cada arbol con su fase.
    float t = push.params.x;
    float phase = dot(base.xz, vec2(0.071, 0.053));
    float gust = sin(t * 0.7 + phase * 0.5) * 0.5 + 0.5;
    vec2 sway = vec2(sin(t * 1.3 + phase), cos(t * 1.1 + phase * 1.7)) * (0.12 + 0.18 * gust);
    world.xz += sway * in_wind * push.params.z * scale;

    vec3 n = in_normal;
    v_normal = vec3(c * n.x + s * n.z, n.y, -s * n.x + c * n.z);
    // Tono: cada arbol algo mas claro u oscuro, y mas amarillo o mas verde.
    vec3 color = in_color.rgb * mix(0.82, 1.12, tint);
    color.r *= mix(0.9, 1.15, fract(tint * 7.13));
    v_color = vec4(color, in_color.a);
    v_world_position = world;
    v_current_clip = camera.unjittered_view_projection * vec4(world, 1.0);
    v_previous_clip = camera.previous_view_projection * vec4(world, 1.0);
    gl_Position = push.params.y > 0.5 ? push.light_view_projection * vec4(world, 1.0)
                                      : camera.view_projection * vec4(world, 1.0);
}
