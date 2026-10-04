#version 450

// UI en el mundo: color del vertice por la textura (letras, imagenes; las
// formas lisas usan una blanca). Como en la pantalla: color con gamma.

layout(set = 0, binding = 0) uniform sampler2D u_texture;

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = v_color * texture(u_texture, v_uv);
}
