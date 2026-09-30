#version 450

// Sombras de los arboles: las hojas proyectan la sombra de las hojas, no la
// de sus tarjetas (recorte por el alfa de la textura).

layout(set = 1, binding = 2) uniform sampler2DArray tree_albedo;

layout(location = 5) in vec3 v_uv_layer;
layout(location = 6) in float v_leaf;

void main() {
    if (v_leaf > 0.5 && texture(tree_albedo, v_uv_layer).a < 0.5) discard;
}
