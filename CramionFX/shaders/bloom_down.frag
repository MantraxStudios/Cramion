#version 450

// Bloom, bajada: cada nivel es la mitad del anterior. Filtro de 13 muestras
// de Call of Duty: Advanced Warfare (Jimenez, SIGGRAPH 2014), que evita el
// parpadeo de un simple promedio 2x2 cuando la camara se mueve.
//
// En el primer nivel las muestras se agrupan en cinco cajas y cada caja se
// pondera por 1 / (1 + luma) (promedio de Karis): asi un solo pixel muy
// brillante (el disco del sol, un reflejo) no se convierte en un destello que
// parpadea. Ahi tambien se aplica el umbral, con una rodilla suave (como el
// bloom de Unity): lo que no llega al umbral no brilla, sin un corte seco.

layout(set = 0, binding = 0) uniform sampler2D source;

layout(push_constant) uniform PushConstants {
    vec2 source_texel;  // 1 / resolucion de la imagen de origen
    float first_level;  // 1 = aplicar el promedio de Karis (y el umbral)
    float threshold;    // luminancia desde la que brilla (0 = todo)
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

vec3 fetch(float x, float y) {
    // Los half pueden guardar valores enormes: se acotan para que no se
    // propague un infinito por toda la cadena.
    return min(texture(source, v_uv + vec2(x, y) * push.source_texel).rgb, vec3(1000.0));
}

float karisWeight(vec3 c) {
    return 1.0 / (1.0 + dot(c, vec3(0.2126, 0.7152, 0.0722)));
}

void main() {
    vec3 a = fetch(-2.0, 2.0);
    vec3 b = fetch(0.0, 2.0);
    vec3 c = fetch(2.0, 2.0);
    vec3 d = fetch(-2.0, 0.0);
    vec3 e = fetch(0.0, 0.0);
    vec3 f = fetch(2.0, 0.0);
    vec3 g = fetch(-2.0, -2.0);
    vec3 h = fetch(0.0, -2.0);
    vec3 i = fetch(2.0, -2.0);
    vec3 j = fetch(-1.0, 1.0);
    vec3 k = fetch(1.0, 1.0);
    vec3 l = fetch(-1.0, -1.0);
    vec3 m = fetch(1.0, -1.0);

    vec3 result;
    if (push.first_level > 0.5) {
        vec3 g0 = (a + b + d + e) * 0.25;
        vec3 g1 = (b + c + e + f) * 0.25;
        vec3 g2 = (d + e + g + h) * 0.25;
        vec3 g3 = (e + f + h + i) * 0.25;
        vec3 g4 = (j + k + l + m) * 0.25;
        float w0 = karisWeight(g0) * 0.125;
        float w1 = karisWeight(g1) * 0.125;
        float w2 = karisWeight(g2) * 0.125;
        float w3 = karisWeight(g3) * 0.125;
        float w4 = karisWeight(g4) * 0.5;
        result = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) /
                 max(w0 + w1 + w2 + w3 + w4, 0.0001);

        if (push.threshold > 0.0) {
            float brightness = max(result.r, max(result.g, result.b));
            float knee = push.threshold * 0.5;
            float soft = clamp(brightness - push.threshold + knee, 0.0, 2.0 * knee);
            soft = soft * soft / (4.0 * knee + 1e-5);
            float contribution = max(soft, brightness - push.threshold) / max(brightness, 1e-5);
            result *= contribution;
        }
    } else {
        result = e * 0.125;
        result += (a + c + g + i) * 0.03125;
        result += (b + d + f + h) * 0.0625;
        result += (j + k + l + m) * 0.125;
    }

    out_color = vec4(max(result, vec3(0.0001)), 1.0);
}
