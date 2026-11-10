#version 450
#extension GL_GOOGLE_include_directive : require

// TESELADO: relieve teselado de las caras de bloque cercanas (VoxelPass, grupos opacos 0-5 de las secciones a
// menos de push.tess.x m). Cada cara (4 vertices de voxel.vert, en orden 0 1 2 3 alrededor) es un parche de
// cuadrado; aqui se decide cuanto se parte:
//   - Por bloque: push.tess.z trozos de cerca (una cara fusionada de varios bloques, por cada uno), sin pasar de
//     un trozo cada ~kTargetPixels pixeles en pantalla, y bajando con la distancia (voxelTessFade) hasta 1 (plano).
//   - El factor de un borde solo depende de sus dos vertices: dos caras que lo comparten lo parten igual.
//   - Piezas finas (vallas, faroles) y plantas: sin partir (nivel 1: la cara tal cual).
// voxel.tese mueve los vertices nuevos; los bordes de cada bloque no se mueven (sin grietas).

layout(vertices = 4) out;

layout(set = 0, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
    mat4 unjittered_view_projection;
    mat4 previous_view_projection;
    vec4 jitter;  // zw = resolucion interna (pixeles)
    uvec4 motion;
} camera;

#include "voxel_common.glsl"

// Lo que voxel.vert entrega a voxel.frag.
layout(location = 0) in vec3 in_world_position[];
layout(location = 1) in vec2 in_uv[];
layout(location = 2) in uvec2 in_face_layer[];
layout(location = 3) in vec3 in_light[];
layout(location = 4) in vec3 in_tint[];
layout(location = 5) in vec4 in_current_clip[];
layout(location = 6) in vec4 in_previous_clip[];
layout(location = 7) in vec3 in_to_camera[];
layout(location = 8) in vec2 in_box_uv[];

layout(location = 0) out vec3 out_world_position[];
layout(location = 1) out vec2 out_uv[];
layout(location = 2) out uvec2 out_face_layer[];
layout(location = 3) out vec3 out_light[];
layout(location = 4) out vec3 out_tint[];
layout(location = 5) out vec4 out_current_clip[];
layout(location = 6) out vec4 out_previous_clip[];
layout(location = 7) out vec3 out_to_camera[];
layout(location = 8) out vec2 out_box_uv[];

const float kTargetPixels = 4.0;  // un vertice cada tantos pixeles como mucho (mas fino no se ve)
const float kMaxLevel = 64.0;     // maxTessellationGenerationLevel minimo de Vulkan

float edgeFactor(vec3 a, vec3 b) {
    vec3 middle = 0.5 * (a + b);
    float distance_to_camera = max(length(middle - camera.position.xyz), 0.05);
    float fade = voxelTessFade(distance_to_camera);
    if (fade <= 0.0) return 1.0;
    float blocks = length(a - b);  // 1 m por bloque
    float pixels = blocks * abs(camera.projection[1][1]) * 0.5 * max(camera.jitter.w, 1.0) / distance_to_camera;
    float wanted = min(blocks * push.tess.z, pixels / kTargetPixels);
    return clamp(1.0 + (wanted - 1.0) * fade, 1.0, kMaxLevel);
}

// Fuera de la vista (con margen por el relieve): no se dibuja.
bool outsideView() {
    float slack = push.tess.y * max(abs(camera.projection[0][0]), abs(camera.projection[1][1]));
    vec4 c[4];
    for (int i = 0; i < 4; ++i) c[i] = camera.view_projection * vec4(in_world_position[i], 1.0);
    bvec4 out_of;
    out_of = bvec4(true);
    for (int i = 0; i < 4; ++i) {
        out_of.x = out_of.x && c[i].x > c[i].w + slack;
        out_of.y = out_of.y && c[i].x < -c[i].w - slack;
        out_of.z = out_of.z && c[i].y > c[i].w + slack;
        out_of.w = out_of.w && c[i].y < -c[i].w - slack;
    }
    bool behind = c[0].w < -slack && c[1].w < -slack && c[2].w < -slack && c[3].w < -slack;
    return behind || any(out_of);
}

void main() {
    out_world_position[gl_InvocationID] = in_world_position[gl_InvocationID];
    out_uv[gl_InvocationID] = in_uv[gl_InvocationID];
    out_face_layer[gl_InvocationID] = in_face_layer[gl_InvocationID];
    out_light[gl_InvocationID] = in_light[gl_InvocationID];
    out_tint[gl_InvocationID] = in_tint[gl_InvocationID];
    out_current_clip[gl_InvocationID] = in_current_clip[gl_InvocationID];
    out_previous_clip[gl_InvocationID] = in_previous_clip[gl_InvocationID];
    out_to_camera[gl_InvocationID] = in_to_camera[gl_InvocationID];
    out_box_uv[gl_InvocationID] = in_box_uv[gl_InvocationID];

    if (gl_InvocationID == 0) {
        uint face = in_face_layer[0].x & 7u;
        bool fine = (in_face_layer[0].x & 8u) != 0u;
        if (outsideView()) {
            gl_TessLevelOuter[0] = 0.0;
            gl_TessLevelOuter[1] = 0.0;
            gl_TessLevelOuter[2] = 0.0;
            gl_TessLevelOuter[3] = 0.0;
            gl_TessLevelInner[0] = 0.0;
            gl_TessLevelInner[1] = 0.0;
        } else if (face >= 6u || fine) {
            gl_TessLevelOuter[0] = 1.0;
            gl_TessLevelOuter[1] = 1.0;
            gl_TessLevelOuter[2] = 1.0;
            gl_TessLevelOuter[3] = 1.0;
            gl_TessLevelInner[0] = 1.0;
            gl_TessLevelInner[1] = 1.0;
        } else {
            // Dominio (u, v): vertice 0 en (0, 0), 1 en (1, 0), 2 en (1, 1), 3 en (0, 1) (voxel.tese).
            // Bordes: 0 = u 0 (3-0), 1 = v 0 (0-1), 2 = u 1 (1-2), 3 = v 1 (2-3).
            float e0 = edgeFactor(in_world_position[3], in_world_position[0]);
            float e1 = edgeFactor(in_world_position[0], in_world_position[1]);
            float e2 = edgeFactor(in_world_position[1], in_world_position[2]);
            float e3 = edgeFactor(in_world_position[2], in_world_position[3]);
            gl_TessLevelOuter[0] = e0;
            gl_TessLevelOuter[1] = e1;
            gl_TessLevelOuter[2] = e2;
            gl_TessLevelOuter[3] = e3;
            gl_TessLevelInner[0] = max(e1, e3);  // a lo largo de u
            gl_TessLevelInner[1] = max(e0, e2);  // a lo largo de v
        }
    }
}
