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

// Repeticion (xy) y desplazamiento (zw) de las UV del material (tiling del
// .crmat). Aqui, en los vertices: el gancho `vertex` del usuario recibe la UV
// ya repetida, como cuando se horneaba en la malla (surface.frag no la vuelve
// a aplicar).
layout(set = 1, binding = 9) uniform MaterialUv {
    vec4 material_uv;
};

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
// UV de lightmap del modelo (set 1, binding 10; PLAN-LIGHTMAPS 1.6; el 9 es el tiling del material): una por
// vertice, unorm16x2 (u en los 16 bits bajos); 0xFFFFFFFF = sin lightmap.
// Solo con kFlagLightmap (lo estatico rigido con UV2).
layout(std430, set = 1, binding = 10) readonly buffer LightmapUvs {
    uint lightmap_uvs[];
};
layout(location = 7) out vec2 v_lightmap_uv;
layout(location = 8) flat out uint v_lightmap_slot;
const uint kFlagLightmap = 1u << 6;
// Hueco de la instancia (parametros por instancia, lightmap_common.glsl) y
// color de vertice del modelo (set 1, binding 11; solo con kFlagVertexColor):
// VERTEX_COLOR en vertex() y en surface().
layout(location = 9) flat out uint v_instance_slot;
layout(std430, set = 1, binding = 11) readonly buffer VertexColors {
    uint vertex_colors[];
};
layout(location = 10) out vec4 v_color;
const uint kFlagVertexColor = 1u << 7;
vec4 cramion_vertex_color = vec4(1.0);
#define VERTEX_COLOR cramion_vertex_color

#define CRAMION_VERTEX_STAGE 1
#include "surface_common.glsl"
#include "surface_user.glsl"

void main() {
    uint base = (push.flags & 1u) != 0u ? uint(gl_InstanceIndex) : push.bone_offset;
    mat4 skin = in_weights.x * bones[base + in_joints.x] +
                in_weights.y * bones[base + in_joints.y] +
                in_weights.z * bones[base + in_joints.z] +
                in_weights.w * bones[base + in_joints.w];
    v_instance_slot = base;
    if ((push.flags & kFlagVertexColor) != 0u) cramion_vertex_color = unpackUnorm4x8(vertex_colors[gl_VertexIndex]);
    v_color = cramion_vertex_color;

    vec4 world_position = push.model * (skin * vec4(in_position, 1.0));
    mat3 to_world = mat3(push.model) * mat3(skin);
    // Las normales con la inversa traspuesta: con escala no uniforme (una caja
    // estirada, una esfera achatada, algo girado y escalado) la matriz tal
    // cual las inclinaba hacia el eje estirado y la luz caia mal. Va sobre la
    // matriz entera (modelo x huesos): en lo estatico instanciado el modelo es
    // la identidad y la escala del actor esta en el "hueso". Se usa la
    // matriz de cofactores (la inversa traspuesta por el determinante; la
    // normal se normaliza despues): sin division, y con escala 0 en un eje
    // (algo aplanado a proposito) da la normal de la cara plana. La tangente
    // si va con la matriz directa.
    mat3 normal_to_world = mat3(cross(to_world[1], to_world[2]), cross(to_world[2], to_world[0]),
                                cross(to_world[0], to_world[1]));
    // Sin girarla con determinante < 0 (espejo): los cofactores ya la dejan
    // del lado del orden de los vertices, que con escala negativa sale al
    // reves; el raster no cambia la cara delantera, asi que esa cara se ve
    // como trasera y el fragment (!gl_FrontFacing) la gira hacia fuera. Girarla
    // aqui tambien la dejaba mirando hacia dentro (doble giro).
    vec3 normal = normal_to_world * in_normal;
    vec2 uv = in_uv * material_uv.xy + material_uv.zw;

#ifdef CRAMION_HAS_VERTEX
    cramion_time = weather.params.z;
    Vertex v;
    v.position = world_position.xyz;
    v.normal = normalize(normal);
    v.uv = uv;
    vertex(v);
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
    vec4 previous_world = previous_skin * vec4(in_position, 1.0);
#ifdef CRAMION_HAS_VERTEX
    // El vertice del frame anterior pasa otra vez por vertex(), con su pose
    // y su tiempo (TIME - DELTA_TIME): lo que mueve el usuario tambien tiene
    // vector de movimiento. Antes se sumaba el mismo desplazamiento de este
    // frame y una bandera u olas por TIME dejaban estelas con TAA/FSR/DLSS.
    mat3 previous_to_world = mat3(previous_skin);
    mat3 previous_normal_to_world =
        mat3(cross(previous_to_world[1], previous_to_world[2]), cross(previous_to_world[2], previous_to_world[0]),
             cross(previous_to_world[0], previous_to_world[1]));
    cramion_time = weather.params.z - DELTA_TIME;
    Vertex previous_vertex;
    previous_vertex.position = previous_world.xyz;
    previous_vertex.normal = normalize(previous_normal_to_world * in_normal);
    previous_vertex.uv = in_uv * material_uv.xy + material_uv.zw;
    vertex(previous_vertex);
    previous_world.xyz = previous_vertex.position;
#endif
    v_previous_clip = camera.previous_view_projection * previous_world;

    // Lightmap: su UV2 y el hueco de la instancia (el de su matriz en el
    // buffer de huesos: ahi esta su registro, lightmap_common.glsl).
    v_lightmap_uv = vec2(-1.0);
    v_lightmap_slot = 0xFFFFFFFFu;
    if ((push.flags & kFlagLightmap) != 0u) {
        uint packed = lightmap_uvs[gl_VertexIndex];
        if (packed != 0xFFFFFFFFu) {
            v_lightmap_uv = unpackUnorm2x16(packed);
            v_lightmap_slot = (push.flags & 1u) != 0u ? uint(gl_InstanceIndex) : push.bone_offset + 1u;
        }
    }
}
