#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_GOOGLE_cpp_style_line_directive : enable

// Plantilla de vertices de los shaders de superficie del usuario (.crshader):
// la de skinned.vert con un gancho `vertex(inout Vertex v)` que puede mover
// los vertices ya en el mundo (olas, viento, latidos). Mismo layout que la
// geometria (ver SkinnedPass::createSurfacePipeline).

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

layout(std430, set = 0, binding = 1) readonly buffer BoneBuffer {
    mat4 bones[];
};

// Solo lo que hace falta del clima: los segundos (TIME).
layout(set = 0, binding = 3) uniform WeatherBuffer {
    mat4 rain_view_projection;
    vec4 params;  // z = segundos
} weather;

// Debe coincidir con GpuSkinnedPush.
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 base_color;
    vec4 emissive;
    vec4 material;
    uint bone_offset;
    float reflectance;
    uint pick_id;  // aqui: bloque de propiedades del material
    uint flags;
} push;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in uvec4 in_joints;
layout(location = 4) in vec4 in_weights;
layout(location = 5) in vec4 in_tangent;

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec4 v_tangent;
layout(location = 3) out vec3 v_world_position;
layout(location = 4) out vec4 v_current_clip;
layout(location = 5) out vec4 v_previous_clip;

#define CRAMION_VERTEX_STAGE 1
#include "surface_common.glsl"
#include "surface_user.glsl"

void main() {
    uint base = (push.flags & 1u) != 0u ? uint(gl_InstanceIndex) : push.bone_offset;
    mat4 skin = in_weights.x * bones[base + in_joints.x] +
                in_weights.y * bones[base + in_joints.y] +
                in_weights.z * bones[base + in_joints.z] +
                in_weights.w * bones[base + in_joints.w];

    vec4 world_position = push.model * (skin * vec4(in_position, 1.0));
    mat3 to_world = mat3(push.model) * mat3(skin);
    vec3 normal = to_world * in_normal;
    vec2 uv = in_uv;
    vec3 offset = vec3(0.0);

#ifdef CRAMION_HAS_VERTEX
    Vertex v;
    v.position = world_position.xyz;
    v.normal = normalize(normal);
    v.uv = uv;
    vertex(v);
    offset = v.position - world_position.xyz;
    world_position.xyz = v.position;
    normal = v.normal;
    uv = v.uv;
#endif

    v_normal = normal;
    v_tangent = vec4(to_world * in_tangent.xyz, in_tangent.w);
    v_uv = uv;
    v_world_position = world_position.xyz;

    gl_Position = ((push.flags & 2u) != 0u ? camera.unjittered_view_projection : camera.view_projection) *
                  world_position;

    uint previous = camera.motion.x + base;
    mat4 previous_skin = in_weights.x * bones[previous + in_joints.x] +
                         in_weights.y * bones[previous + in_joints.y] +
                         in_weights.z * bones[previous + in_joints.z] +
                         in_weights.w * bones[previous + in_joints.w];
    v_current_clip = camera.unjittered_view_projection * world_position;
    // El desplazamiento del usuario se toma igual en el frame anterior.
    vec4 previous_world = previous_skin * vec4(in_position, 1.0);
    previous_world.xyz += offset;
    v_previous_clip = camera.previous_view_projection * previous_world;
}
