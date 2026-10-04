#version 450

// UI en el mundo: la textura del Canvas (color premultiplicado por su
// opacidad, de pintarla sobre transparente) encima de la imagen final.

layout(set = 0, binding = 0) uniform sampler2D u_texture;

layout(push_constant) uniform Push {
    mat4 transform;
    vec4 params;  // x = opacidad
} push;

layout(location = 0) in vec2 v_uv;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = texture(u_texture, v_uv) * push.params.x;
}
