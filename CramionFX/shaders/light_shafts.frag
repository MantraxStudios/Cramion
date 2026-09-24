#version 450

// Rayos de luz del sol ("Light Shafts" / "God Rays" de Unreal), en espacio de
// pantalla y a media resolucion.
//
// 1. Mascara: solo cuenta el cielo (depth = 1) cerca del sol; el terreno, los
//    arboles y los personajes lo tapan.
// 2. Desenfoque radial: cada pixel promedia la mascara a lo largo de la recta
//    que va hacia el sol, con un decaimiento por muestra. Donde la geometria
//    corta la luz quedan franjas oscuras entre franjas iluminadas.
//
// El resultado se suma en la composicion, antes de la exposicion.

layout(set = 0, binding = 0) uniform sampler2D g_depth;
layout(set = 0, binding = 1) uniform sampler2D scene_color;

layout(push_constant) uniform PushConstants {
    vec2 sun_uv;       // posicion del sol en pantalla (puede estar fuera)
    float intensity;   // 0 = sol fuera de vista o bajo el horizonte
    float aspect;      // ancho / alto
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

const int kSamples = 48;
const float kDensity = 0.85;   // fraccion del camino hasta el sol que se recorre
const float kDecay = 0.965;
// Tope del brillo de cada muestra: el disco del sol tiene una radiancia
// enorme y sin tope todo el efecto seria un borron blanco.
const float kMaxSample = 6.0;

float interleavedGradientNoise(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

vec3 maskAt(vec2 uv) {
    if (texture(g_depth, uv).r < 1.0) {
        return vec3(0.0);
    }
    // Solo el cielo proximo al sol: el resto del cielo no debe hacer rayos.
    vec2 offset = (uv - push.sun_uv) * vec2(push.aspect, 1.0);
    float falloff = exp(-dot(offset, offset) * 18.0);
    return min(texture(scene_color, uv).rgb, vec3(kMaxSample)) * falloff;
}

void main() {
    if (push.intensity <= 0.0) {
        out_color = vec4(0.0);
        return;
    }

    vec2 step_uv = (push.sun_uv - v_uv) * (kDensity / float(kSamples));
    // Desplazamiento aleatorio del inicio: cambia las bandas por ruido fino,
    // que el bloom y el FXAA suavizan.
    vec2 uv = v_uv + step_uv * interleavedGradientNoise(gl_FragCoord.xy);

    vec3 sum = vec3(0.0);
    float weight = 1.0;
    for (int i = 0; i < kSamples; ++i) {
        sum += maskAt(uv) * weight;
        weight *= kDecay;
        uv += step_uv;
    }

    out_color = vec4(sum / float(kSamples) * push.intensity, 1.0);
}
