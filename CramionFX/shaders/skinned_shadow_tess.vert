#version 450

// Sombras de los materiales con relieve teselado: el skinning de
// skinned_shadow.vert, pero entrega la posicion y la normal del modelo a la
// teselacion (skinned_shadow.tesc/.tese), que sube los vertices y proyecta.

layout(std430, set = 0, binding = 1) readonly buffer BoneBuffer {
    mat4 bones[];
};

// Debe coincidir con GpuSkinnedShadowPush.
layout(push_constant) uniform PushConstants {
    mat4 light_model_view_projection;
    uint bone_offset;
} push;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in uvec4 in_joints;
layout(location = 4) in vec4 in_weights;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec3 v_position;  // espacio del modelo (tras los huesos)
layout(location = 2) out vec3 v_normal;

void main() {
    uint base = push.bone_offset;
    mat4 skin = in_weights.x * bones[base + in_joints.x] +
                in_weights.y * bones[base + in_joints.y] +
                in_weights.z * bones[base + in_joints.z] +
                in_weights.w * bones[base + in_joints.w];
    v_uv = in_uv;
    v_position = (skin * vec4(in_position, 1.0)).xyz;
    v_normal = mat3(skin) * in_normal;
}
