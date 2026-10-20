#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_GOOGLE_cpp_style_line_directive : enable

// Plantilla de la sombra de los .crshader con `vertex()`: el mismo skinning y
// el mismo gancho que surface.vert, proyectado desde la luz (cascadas del sol,
// luces locales, mapa de lluvia). Sin esto la sombra se quedaba con la malla
// sin mover (una bandera ondeando con la sombra quieta). Mismo layout que la
// sombra de siempre (SkinnedPass::shadowLayout); la push constant es
// GpuSurfaceShadowPush (el mismo tamano que GpuSkinnedShadowPush).

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

// Debe coincidir con GpuSurfaceShadowPush.
layout(push_constant) uniform PushConstants {
    mat4 light_view_projection;  // sin el modelo: vertex() trabaja en el mundo
    uint bone_offset;
    float alpha_factor;          // surface_shadow.frag
    uint pick_id;                // bloque de propiedades del material
    uint flags;                  // 1 = recorte por alfa
    vec4 model_rows[3];          // matriz del modelo (filas, afin)
} push;

// Tiling del material (como surface.vert: vertex() recibe la UV ya repetida).
layout(set = 1, binding = 9) uniform MaterialUv {
    vec4 material_uv;
};

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in uvec4 in_joints;
layout(location = 4) in vec4 in_weights;

// Para el recorte por alfa de surface_shadow.frag.
layout(location = 0) out vec2 v_uv;

#define CRAMION_VERTEX_STAGE 1
#define CRAMION_SHADOW_STAGE 1
#include "surface_common.glsl"
#include "surface_user.glsl"

void main() {
    uint base = push.bone_offset;
    mat4 skin = in_weights.x * bones[base + in_joints.x] +
                in_weights.y * bones[base + in_joints.y] +
                in_weights.z * bones[base + in_joints.z] +
                in_weights.w * bones[base + in_joints.w];
    mat4 model = transpose(mat4(push.model_rows[0], push.model_rows[1], push.model_rows[2], vec4(0.0, 0.0, 0.0, 1.0)));
    vec4 world_position = model * (skin * vec4(in_position, 1.0));
    vec2 uv = in_uv * material_uv.xy + material_uv.zw;

#ifdef CRAMION_HAS_VERTEX
    // La normal como en surface.vert (cofactores: vale con escala no uniforme).
    mat3 to_world = mat3(model) * mat3(skin);
    mat3 normal_to_world = mat3(cross(to_world[1], to_world[2]), cross(to_world[2], to_world[0]),
                                cross(to_world[0], to_world[1]));
    cramion_time = weather.params.z;
    Vertex v;
    v.position = world_position.xyz;
    v.normal = normalize(normal_to_world * in_normal);
    v.uv = uv;
    vertex(v);
    world_position.xyz = v.position;
    uv = v.uv;
#endif

    // Ya con el tiling (y lo que cambie vertex()): surface_shadow.frag no lo
    // vuelve a aplicar.
    v_uv = uv;
    gl_Position = push.light_view_projection * world_position;
}
