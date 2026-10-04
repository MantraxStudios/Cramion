#version 450

// UI en el mundo (WorldUiPass): pinta la lista de un Canvas en modo Mundo en
// su textura. Los vertices son los de ImGui (posicion en pixeles de la
// textura, uv y color RGBA8); scale/translate los llevan a NDC.

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(push_constant) uniform Push {
    vec2 scale;
    vec2 translate;
} push;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;

void main() {
    v_color = in_color;
    v_uv = in_uv;
    gl_Position = vec4(in_position * push.scale + push.translate, 0.0, 1.0);
}
