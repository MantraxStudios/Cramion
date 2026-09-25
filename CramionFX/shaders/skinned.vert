#version 450

// Pasada de geometria de los modelos con esqueleto: skinning en la GPU.
//
// Cada vertice mezcla hasta cuatro matrices de hueso segun sus pesos. Las
// matrices estan en un storage buffer de longitud variable, compartido por
// todas las instancias: `bone_offset` dice donde empiezan las de esta.

layout(set = 0, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
} camera;

// Sin tamano fijo: el numero de huesos solo lo limita la memoria.
layout(std430, set = 0, binding = 1) readonly buffer BoneBuffer {
    mat4 bones[];
};

// Debe coincidir con GpuSkinnedPush (y con skinned.frag).
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 base_color;
    vec4 emissive;
    vec4 material;
    uint bone_offset;
    float reflectance;
    uint pick_id;
    uint flags;  // bit 0: instanciado (lotes por material del culling en GPU)
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
layout(location = 3) out vec3 v_world_position;  // para la lluvia (skinned.frag)

void main() {
    // Instanciado: push.model es la identidad y la matriz de mundo del actor
    // (modelo rigido de un hueso, ya multiplicada) esta en bones[instancia].
    uint base = (push.flags & 1u) != 0u ? uint(gl_InstanceIndex) : push.bone_offset;
    mat4 skin = in_weights.x * bones[base + in_joints.x] +
                in_weights.y * bones[base + in_joints.y] +
                in_weights.z * bones[base + in_joints.z] +
                in_weights.w * bones[base + in_joints.w];

    vec4 world_position = push.model * (skin * vec4(in_position, 1.0));

    // Huesos y modelo solo rotan, trasladan y escalan de forma uniforme: la
    // parte 3x3 sirve para la normal y la tangente (se normalizan en el
    // fragment shader).
    mat3 to_world = mat3(push.model) * mat3(skin);
    v_normal = to_world * in_normal;
    v_tangent = vec4(to_world * in_tangent.xyz, in_tangent.w);
    v_uv = in_uv;
    v_world_position = world_position.xyz;

    gl_Position = camera.view_projection * world_position;
}
