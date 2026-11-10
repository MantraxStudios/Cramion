#version 450
#extension GL_GOOGLE_include_directive : require

// TESELADO: relieve teselado de las caras de bloque (voxel.tesc decide cuanto se parte). Cada vertice nuevo se
// hunde a lo largo de la normal de la cara segun la altura de la textura del bloque (z del normal map; blanco =
// alto = sin hundir), hasta push.tess.y bloques, y menos con la distancia (voxelTessFade: plano en push.tess.x).
//   - Bordes de cada bloque fijos: la altura se fuerza a "sin hundir" con una rampa corta (kEdgeRamp) junto a los
//     bordes del bloque (tambien dentro de una cara fusionada de varios). Dos bloques vecinos, o dos caras del
//     mismo bloque, comparten el borde sin moverlo: no se abren grietas.
//   - Hacia dentro (como el parallax): el bloque no crece; lo hundido queda dentro de su celda.
//   - Misma VARIACION que voxel.frag (cada bloque voltea su textura segun su celda) para que el relieve coincida
//     con el color y el normal map.
// Las normales siguen saliendo del normal map en voxel.frag (como sin teselado).

// cw: el dominio de Vulkan tiene el origen arriba a la izquierda (como skinned.tese): asi los triangulos salen
// con el mismo giro que los de la cara (0 1 2 3 antihorario visto de fuera).
layout(quads, fractional_odd_spacing, cw) in;

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

#include "voxel_common.glsl"

layout(set = 1, binding = 1) uniform sampler2DArray normal_map;  // xy normal, z altura, w oclusion

layout(location = 0) in vec3 in_world_position[];
layout(location = 1) in vec2 in_uv[];
layout(location = 2) in uvec2 in_face_layer[];
layout(location = 3) in vec3 in_light[];
layout(location = 4) in vec3 in_tint[];
layout(location = 5) in vec4 in_current_clip[];
layout(location = 6) in vec4 in_previous_clip[];
layout(location = 7) in vec3 in_to_camera[];
layout(location = 8) in vec2 in_box_uv[];

layout(location = 0) out vec3 v_world_position;
layout(location = 1) out vec2 v_uv;
layout(location = 2) flat out uvec2 v_face_layer;
layout(location = 3) out vec3 v_light;
layout(location = 4) out vec3 v_tint;
layout(location = 5) out vec4 v_current_clip;
layout(location = 6) out vec4 v_previous_clip;
layout(location = 7) out vec3 v_to_camera;
layout(location = 8) out vec2 v_box_uv;

const float kEdgeRamp = 0.08;  // bloques: rampa junto al borde (la altura sube a "sin hundir")

vec4 mix4(vec4 a, vec4 b, vec4 c, vec4 d, vec2 t) { return mix(mix(a, b, t.x), mix(d, c, t.x), t.y); }
vec3 mix4(vec3 a, vec3 b, vec3 c, vec3 d, vec2 t) { return mix(mix(a, b, t.x), mix(d, c, t.x), t.y); }
vec2 mix4(vec2 a, vec2 b, vec2 c, vec2 d, vec2 t) { return mix(mix(a, b, t.x), mix(d, c, t.x), t.y); }

void main() {
    vec2 t = gl_TessCoord.xy;
    vec3 world = mix4(in_world_position[0], in_world_position[1], in_world_position[2], in_world_position[3], t);
    vec2 uv = mix4(in_uv[0], in_uv[1], in_uv[2], in_uv[3], t);
    vec2 box_uv = mix4(in_box_uv[0], in_box_uv[1], in_box_uv[2], in_box_uv[3], t);
    vec4 current_clip = mix4(in_current_clip[0], in_current_clip[1], in_current_clip[2], in_current_clip[3], t);
    vec4 previous_clip = mix4(in_previous_clip[0], in_previous_clip[1], in_previous_clip[2], in_previous_clip[3], t);
    uvec2 face_layer = in_face_layer[0];
    uint face = face_layer.x & 7u;
    bool fine = (face_layer.x & 8u) != 0u;

    vec3 offset = vec3(0.0);
    if (face < 6u && !fine) {
        vec3 n = kFaceNormal[face];
        float fade = voxelTessFade(length(camera.position.xyz - world));
        // Distancia (en bloques) al borde del bloque mas cercano: la uv del mundo es entera en los bordes.
        vec2 inside = fract(box_uv);
        vec2 edge2 = min(inside, 1.0 - inside);
        float edge = min(edge2.x, edge2.y);
        float ramp = smoothstep(0.0, kEdgeRamp, edge);
        if (fade > 0.0 && ramp > 0.0) {
            // uv de la textura como voxel.frag (fusionadas: desde la posicion) y su VARIACION por celda.
            bool world_uv = (face_layer.x & 64u) != 0u;
            vec2 tex_uv = world_uv ? inside : uv;
            if ((push.style & 8u) == 0u) {
                vec3 cell = floor(world - n * 0.5 + 0.001);
                uint h = uint(int(cell.x) * 73856093) ^ uint(int(cell.y) * 19349663) ^ uint(int(cell.z) * 83492791);
                h ^= h >> 13u;
                h *= 0x5bd1e995u;
                h ^= h >> 15u;
                bool top_bottom = abs(n.y) > 0.5;
                vec2 flip = vec2(((h & 1u) != 0u) ? -1.0 : 1.0, (top_bottom && ((h & 2u) != 0u)) ? -1.0 : 1.0);
                tex_uv = mix(tex_uv, 1.0 - tex_uv, step(flip, vec2(0.0)));
            }
            // Mip de la altura segun los texeles que cubre cada trozo (con el mip 0 de lejos seria ruido).
            // Bloques que cubre la cara a lo largo de u (0-1) y de v (0-3) del dominio.
            vec2 span = vec2(length(in_world_position[1] - in_world_position[0]),
                             length(in_world_position[3] - in_world_position[0]));
            float size = float(textureSize(normal_map, 0).x);
            float texels = max(size * span.x / max(gl_TessLevelInner[0], 1.0), size * span.y / max(gl_TessLevelInner[1], 1.0));
            float lod = log2(max(texels, 1.0));
            float height = textureLod(normal_map, vec3(tex_uv, float(face_layer.y)), lod).z;
            offset = -n * ((1.0 - height) * push.tess.y * fade * ramp);
        }
    }
    world += offset;

    v_world_position = world;
    v_uv = uv;
    v_face_layer = face_layer;
    v_light = mix4(in_light[0], in_light[1], in_light[2], in_light[3], t);
    v_tint = mix4(in_tint[0], in_tint[1], in_tint[2], in_tint[3], t);
    v_box_uv = box_uv;
    v_to_camera = camera.position.xyz - world;
    // Los recortes son lineales en la posicion: los de las esquinas mezclados + el hundimiento.
    v_current_clip = current_clip + camera.unjittered_view_projection * vec4(offset, 0.0);
    v_previous_clip = previous_clip + camera.previous_view_projection * vec4(offset, 0.0);
    gl_Position = camera.view_projection * vec4(world, 1.0);
}
