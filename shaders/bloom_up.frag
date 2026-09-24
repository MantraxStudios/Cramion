#version 450

// Bloom, subida: filtro de tienda 3x3 sobre el nivel mas pequeno, que se SUMA
// (mezcla aditiva del pipeline) al nivel del doble de tamano. Al acabar, el
// primer nivel contiene la suma de todos: un halo que combina radios pequenos
// (brillo cerca de la fuente) y enormes (resplandor difuso).

layout(set = 0, binding = 0) uniform sampler2D source;

layout(push_constant) uniform PushConstants {
    vec2 source_texel;  // 1 / resolucion del nivel que se lee
    float first_level;  // no se usa
    float radius;       // separacion de las muestras, en texeles del origen
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

vec3 fetch(float x, float y) {
    return texture(source, v_uv + vec2(x, y) * push.source_texel * push.radius).rgb;
}

void main() {
    vec3 result = fetch(0.0, 0.0) * 4.0;
    result += (fetch(-1.0, 0.0) + fetch(1.0, 0.0) + fetch(0.0, -1.0) + fetch(0.0, 1.0)) * 2.0;
    result += fetch(-1.0, -1.0) + fetch(1.0, -1.0) + fetch(-1.0, 1.0) + fetch(1.0, 1.0);

    out_color = vec4(result / 16.0, 1.0);
}
