#version 450
#extension GL_GOOGLE_include_directive : require

// Voxeles en las sombras: solo el recorte (hojas y plantas dejan pasar la
// luz entre sus huecos). La profundidad la escribe el rasterizador.

#include "voxel_common.glsl"

layout(set = 1, binding = 0) uniform sampler2DArray albedo_map;

layout(location = 1) in vec2 v_uv;
layout(location = 2) flat in uvec2 v_face_layer;

void main() {
    if (texture(albedo_map, vec3(v_uv, float(v_face_layer.y))).a < 0.5) discard;
}
