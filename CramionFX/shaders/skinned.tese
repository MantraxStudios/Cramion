#version 450

// Relieve teselado (G-buffer): cada vertice nuevo sube por su normal lo que
// dice el mapa de alturas (G de occlusion_map; blanco = alto), hasta
// push.emissive.w metros. Hacia fuera: la superficie original queda debajo,
// asi los rayos (que ven la malla sin teselar) no la tapan.

// cw: en Vulkan el dominio de teselacion tiene el origen arriba a la
// izquierda, y con ccw los triangulos salian al reves que los de la malla
// (gl_FrontFacing falso: skinned.frag giraba la normal y todo se veia oscuro).
layout(triangles, fractional_odd_spacing, cw) in;

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

layout(set = 1, binding = 3) uniform sampler2D occlusion_map;

// Debe coincidir con GpuSkinnedPush (y con skinned.vert).
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 base_color;
    vec4 emissive;
    vec4 material;
    uint bone_offset;
    float reflectance;
    uint pick_id;
    uint flags;
} push;

layout(location = 0) in vec3 in_normal[];
layout(location = 1) in vec2 in_uv[];
layout(location = 2) in vec4 in_tangent[];
layout(location = 3) in vec3 in_world_position[];
layout(location = 6) in vec3 in_previous_world[];

// Lo mismo que skinned.vert entrega a skinned.frag.
layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec4 v_tangent;
layout(location = 3) out vec3 v_world_position;
layout(location = 4) out vec4 v_current_clip;
layout(location = 5) out vec4 v_previous_clip;

void main() {
    vec3 w = gl_TessCoord;
    vec3 normal = w.x * in_normal[0] + w.y * in_normal[1] + w.z * in_normal[2];
    vec2 uv = w.x * in_uv[0] + w.y * in_uv[1] + w.z * in_uv[2];
    vec3 tangent = w.x * in_tangent[0].xyz + w.y * in_tangent[1].xyz + w.z * in_tangent[2].xyz;
    vec3 world = w.x * in_world_position[0] + w.y * in_world_position[1] + w.z * in_world_position[2];
    vec3 previous = w.x * in_previous_world[0] + w.y * in_previous_world[1] + w.z * in_previous_world[2];

    // Mip de la altura segun lo que mide cada trozo nuevo en texeles: con el
    // mip 0 de lejos el relieve seria ruido.
    vec2 size = vec2(textureSize(occlusion_map, 0));
    float texels = max(max(length((in_uv[1] - in_uv[0]) * size), length((in_uv[2] - in_uv[1]) * size)),
                       length((in_uv[0] - in_uv[2]) * size));
    float lod = max(log2(max(texels / max(gl_TessLevelInner[0], 1.0), 1.0)), 0.0);
    float height = textureLod(occlusion_map, uv, lod).g;

    vec3 offset = normalize(normal) * (height * push.emissive.w);
    world += offset;
    previous += offset;

    v_normal = normal;
    v_uv = uv;
    v_tangent = vec4(tangent, in_tangent[0].w);
    v_world_position = world;
    gl_Position = ((push.flags & 2u) != 0u ? camera.unjittered_view_projection : camera.view_projection) *
                  vec4(world, 1.0);
    v_current_clip = camera.unjittered_view_projection * vec4(world, 1.0);
    v_previous_clip = camera.previous_view_projection * vec4(previous, 1.0);
}
