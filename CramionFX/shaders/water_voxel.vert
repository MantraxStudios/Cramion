#version 450
#extension GL_GOOGLE_include_directive : require

// Agua de bloques (mundos de voxeles): las caras del agua que hace el mallador
// de CramionCore (Mesher.cpp, meshWaterBlock), dibujadas en el pase del agua
// con el mismo water.frag que el oceano y los lagos (refraccion, reflejos,
// Fresnel, color por hondura, espuma de orilla, causticas, desde abajo).
//
// Vertice (2 x uint32), relativo a la esquina de la seccion (push.origin):
//   a: x (5) | y (5) | z (5) | cara 0..5 (3) | altura sobre y en dieciseisavos (5) |
//      superficie libre (1: ondea) | corriente x (4, +8) | corriente z (4, +8)
//   b: luz del cielo 0..15 (4) | luz de bloque (4) | cae (1: agua encima)
// Caras como voxel_common.glsl: 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z.

layout(set = 0, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
} camera;

#include "water_common.glsl"

layout(location = 0) in uvec2 in_packed;

layout(location = 0) out vec3 v_world_position;
layout(location = 1) out vec3 v_normal;     // bloques: la normal de la cara
layout(location = 2) out vec2 v_uv;         // bloques: luz del cielo y de bloque (0..1)
layout(location = 3) out vec2 v_flow;       // bloques: corriente (xz, -1..1)
layout(location = 4) out vec2 v_grid;       // coordenadas del rizado (en el plano de la cara)
layout(location = 5) out float v_jacobian;
layout(location = 6) out float v_height;
layout(location = 7) flat out vec4 v_hole;
layout(location = 8) out float v_slope;     // bloques: 1 = cae (cascada)

const vec3 kFaceNormals[6] = vec3[](vec3(1.0, 0.0, 0.0), vec3(-1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0),
                                    vec3(0.0, -1.0, 0.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, -1.0));

void main() {
    WaterBody b = water.bodies[push.body];
    float t = water.time_count.x;
    uint a = in_packed.x;
    uint c = in_packed.y;
    vec3 local = vec3(float(a & 31u), float((a >> 5u) & 31u), float((a >> 10u) & 31u));
    uint face = (a >> 15u) & 7u;
    float fraction = float((a >> 18u) & 31u) / 16.0;
    bool surface = ((a >> 23u) & 1u) != 0u;
    vec2 flow = vec2(float((a >> 24u) & 15u) - 8.0, float((a >> 28u) & 15u) - 8.0) / 7.0;
    local.y += fraction;
    vec3 base = push.origin.xyz + local;

    // Solo la superficie libre ondea (un poco: es un bloque), igual en las
    // caras de arriba y en el borde de arriba de las de los lados (sin grietas).
    float lift = 0.0;
    if (surface) {
        vec3 n;
        float j;
        float distance_to_camera = length(camera.position.xz - base.xz);
        lift = gerstnerWaves(b, base.xz - b.origin.xz, t, max(distance_to_camera, 1.0), n, j).y;
        lift += rippleHeight(base.xz);
        lift = clamp(lift, -0.08, 0.08);
    }
    vec3 world = base + vec3(0.0, lift, 0.0);

    vec3 normal = kFaceNormals[min(face, 5u)];
    v_world_position = world;
    v_normal = normal;
    v_uv = vec2(float(c & 15u), float((c >> 4u) & 15u)) / 15.0;
    v_flow = flow;
    // El rizado en el plano de la cara: arriba y abajo xz; a los lados
    // (horizontal, altura).
    if (abs(normal.y) > 0.5) v_grid = base.xz - b.origin.xz;
    else v_grid = vec2(abs(normal.x) > 0.5 ? base.z : base.x, base.y);
    v_jacobian = 1.0;
    v_height = lift;
    v_hole = vec4(0.0);
    v_slope = ((c >> 8u) & 1u) != 0u ? 1.0 : 0.0;
    gl_Position = camera.view_projection * vec4(world, 1.0);
}
