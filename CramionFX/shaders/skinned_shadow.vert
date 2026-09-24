#version 450

// Sombras de los modelos con esqueleto: el mismo skinning que skinned.vert,
// pero solo la posicion y proyectada desde la luz. La matriz llega ya
// multiplicada (proyeccion de la luz x vista de la luz x modelo).

layout(std430, set = 0, binding = 1) readonly buffer BoneBuffer {
    mat4 bones[];
};

layout(push_constant) uniform PushConstants {
    mat4 light_model_view_projection;
    uint bone_offset;
} push;

layout(location = 0) in vec3 in_position;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in uvec4 in_joints;
layout(location = 4) in vec4 in_weights;

// Para el recorte por alfa de skinned_shadow.frag.
layout(location = 0) out vec2 v_uv;

void main() {
    uint base = push.bone_offset;
    mat4 skin = in_weights.x * bones[base + in_joints.x] +
                in_weights.y * bones[base + in_joints.y] +
                in_weights.z * bones[base + in_joints.z] +
                in_weights.w * bones[base + in_joints.w];

    v_uv = in_uv;
    gl_Position = push.light_model_view_projection * (skin * vec4(in_position, 1.0));
}
