#version 450

// Particulas (ParticlePass): quads de cara a la camara calculados en la CPU,
// ya en espacio de recorte.

layout(location = 0) in vec4 in_clip;
layout(location = 1) in vec4 in_color;  // rgb lineal HDR, a = opacidad
layout(location = 2) in vec2 in_uv;     // -1..1 dentro del quad

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;

void main() {
    v_color = in_color;
    v_uv = in_uv;
    gl_Position = in_clip;
}
