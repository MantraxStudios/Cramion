#version 450

// FXAA (Fast Approximate Anti-Aliasing), variante compacta de la 3.11 de
// NVIDIA. Es la ultima pasada: suaviza los bordes de la imagen ya iluminada.
//
// Por que hace falta: un renderizador diferido no puede usar MSAA sin
// multiplicar el coste del G-buffer, asi que sin una pasada como esta TODOS los
// bordes quedan en escalera: las siluetas de la geometria y tambien los bordes
// de sombra. FXAA trabaja sobre el color final, no sobre la geometria, asi que
// arregla las dos cosas de una vez.
//
// Como funciona: mide el contraste de luminancia en una cruz de 4 vecinos; si
// no hay borde, deja el pixel igual; si lo hay, deduce su direccion y promedia
// a lo largo de ella, nunca a traves, para no emborronar la imagen.

layout(set = 0, binding = 0) uniform sampler2D scene_color;

layout(push_constant) uniform PushConstants {
    vec2 inverse_resolution;  // 1 / (ancho, alto)
    float enabled;            // 0 = pasar la imagen tal cual
    float unused;
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

// Umbral relativo: por debajo de este contraste no se considera borde.
const float kEdgeThreshold = 0.125;
// Umbral absoluto: evita tocar zonas casi planas y muy oscuras.
const float kEdgeThresholdMin = 0.0312;
// Cuanto se puede desplazar la busqueda, en pixeles.
const float kSpanMax = 8.0;
const float kReduceMul = 0.125;
const float kReduceMin = 1.0 / 128.0;

float luma(vec3 color) {
    // Pesos perceptuales: el verde pesa mas que el rojo y mucho mas que el azul.
    return dot(color, vec3(0.299, 0.587, 0.114));
}

vec3 sampleOffset(vec2 uv, vec2 offset_pixels) {
    return texture(scene_color, uv + offset_pixels * push.inverse_resolution).rgb;
}

void main() {
    vec3 center = texture(scene_color, v_uv).rgb;

    if (push.enabled < 0.5) {
        out_color = vec4(center, 1.0);
        return;
    }

    // --- 1) Luminancia del pixel y de sus cuatro diagonales ---
    float luma_center = luma(center);
    float luma_nw = luma(sampleOffset(v_uv, vec2(-1.0, -1.0)));
    float luma_ne = luma(sampleOffset(v_uv, vec2(1.0, -1.0)));
    float luma_sw = luma(sampleOffset(v_uv, vec2(-1.0, 1.0)));
    float luma_se = luma(sampleOffset(v_uv, vec2(1.0, 1.0)));

    float luma_min = min(luma_center, min(min(luma_nw, luma_ne), min(luma_sw, luma_se)));
    float luma_max = max(luma_center, max(max(luma_nw, luma_ne), max(luma_sw, luma_se)));
    float contrast = luma_max - luma_min;

    // --- 2) Sin contraste suficiente no hay borde que suavizar ---
    if (contrast < max(kEdgeThresholdMin, luma_max * kEdgeThreshold)) {
        out_color = vec4(center, 1.0);
        return;
    }

    // --- 3) Direccion del borde, deducida del gradiente de luminancia ---
    vec2 direction;
    direction.x = -((luma_nw + luma_ne) - (luma_sw + luma_se));
    direction.y = ((luma_nw + luma_sw) - (luma_ne + luma_se));

    // En bordes muy tenues el gradiente es ruido: este termino lo amortigua.
    float reduce = max((luma_nw + luma_ne + luma_sw + luma_se) * 0.25 * kReduceMul, kReduceMin);
    float inverse_min = 1.0 / (min(abs(direction.x), abs(direction.y)) + reduce);

    direction = clamp(direction * inverse_min, vec2(-kSpanMax), vec2(kSpanMax));

    // --- 4) Promedio a lo largo del borde ---
    // Dos muestras cortas...
    vec3 near_average = 0.5 * (sampleOffset(v_uv, direction * (1.0 / 3.0 - 0.5)) +
                               sampleOffset(v_uv, direction * (2.0 / 3.0 - 0.5)));

    // ...y dos mas alejadas, que suavizan los bordes casi horizontales o
    // casi verticales (los que mas escalera producen).
    vec3 far_average = near_average * 0.5 + 0.25 * (sampleOffset(v_uv, direction * -0.5) +
                                                    sampleOffset(v_uv, direction * 0.5));

    // Si el promedio largo se sale del rango de luminancia del vecindario, es
    // que ha cruzado a otra superficie: en ese caso vale el promedio corto.
    float luma_far = luma(far_average);
    vec3 result = (luma_far < luma_min || luma_far > luma_max) ? near_average : far_average;

    out_color = vec4(result, 1.0);
}
