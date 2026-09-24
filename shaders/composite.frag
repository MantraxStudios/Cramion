#version 450

// Composicion final en HDR -> imagen de 8 bits que lee el FXAA.
//
//   1. Bloom:     se mezcla una fraccion pequena del halo con la imagen. No hay
//                 umbral: todo brilla un poco, pero solo lo muy intenso (sol,
//                 bloques luminosos, antorchas) deja un halo visible, como en
//                 una lente real.
//   2. Rayos de luz del sol (light_shafts.frag).
//   3. Exposicion: la calcula la auto-exposicion por histograma
//                 (exposure_average.comp) o, si esta apagada, la CPU.
//   4. Tono:      por defecto Khronos PBR Neutral: casi lineal hasta los
//                 brillos, asi que el color base de los materiales llega a
//                 pantalla con su tono y saturacion (el rojo de una teja es
//                 ese rojo). Solo comprime y desatura lo muy brillante.
//                 Alternativa: ACES (ajuste de Stephen Hill), mas contraste
//                 "de cine" pero que apaga y desplaza los colores saturados.
//   5. Gradacion: un poco de contraste, saturacion, viveza (satura mas lo que
//                 esta menos saturado, sin quemar lo que ya lo esta) y vineta.
//   6. Gamma y ruido de +-1/255 contra el bandeado de los 8 bits.

layout(set = 0, binding = 0) uniform sampler2D scene_color;
layout(set = 0, binding = 1) uniform sampler2D bloom;
layout(set = 0, binding = 2) uniform sampler2D light_shafts;

layout(set = 0, binding = 3) readonly buffer Exposure {
    float exposure;
    float log_exposure;
    float average_luminance;
    float initialized;
} auto_exposure;

layout(push_constant) uniform PushConstants {
    float exposure;                // exposicion manual (auto-exposicion apagada)
    float bloom_strength;          // 0 = sin bloom
    float vignette;
    float saturation;
    float auto_exposure_enabled;   // 1 = usar la del histograma
    float exposure_compensation;   // en EV, como en Unreal
    float light_shaft_strength;    // 0 = sin rayos
    float tonemapper;              // 0 = PBR Neutral, 1 = ACES
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

// Numero de niveles del bloom (BloomChain::kLevels): la subida los suma todos.
const float kBloomLevels = 6.0;

const float kVibrance = 0.25;

// sRGB -> AP1 con la transformacion RRT_SAT incluida.
const mat3 kAcesInput = mat3(
    0.59719, 0.07600, 0.02840,
    0.35458, 0.90834, 0.13383,
    0.04823, 0.01566, 0.83777);

// ODT_SAT -> XYZ -> D60->D65 -> sRGB.
const mat3 kAcesOutput = mat3(
     1.60475, -0.10208, -0.00327,
    -0.53108,  1.10813, -0.07276,
    -0.07367, -0.00605,  1.07602);

vec3 rrtAndOdtFit(vec3 v) {
    vec3 a = v * (v + 0.0245786) - 0.000090537;
    vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}

vec3 acesFitted(vec3 color) {
    color = kAcesInput * color;
    color = rrtAndOdtFit(color);
    color = kAcesOutput * color;
    return clamp(color, 0.0, 1.0);
}

// Khronos PBR Neutral (2024): https://github.com/KhronosGroup/ToneMapping
vec3 pbrNeutral(vec3 color) {
    const float kStartCompression = 0.8 - 0.04;
    const float kDesaturation = 0.15;

    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));
    if (peak < kStartCompression) {
        return color;
    }

    const float d = 1.0 - kStartCompression;
    float new_peak = 1.0 - d * d / (peak + d - kStartCompression);
    color *= new_peak / peak;

    float g = 1.0 - 1.0 / (kDesaturation * (peak - new_peak) + 1.0);
    return mix(color, vec3(new_peak), g);
}

// Contraste alrededor del gris medio, en escala logaritmica (como el
// "Contrast" de la gradacion de Unreal): no toca el gris ni el negro.
vec3 applyContrast(vec3 color, float contrast) {
    const float kMiddleGrey = 0.18;
    return kMiddleGrey * pow(max(color, vec3(0.0)) / kMiddleGrey, vec3(contrast));
}

float luminance(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

float ditherNoise(vec2 fragment_coordinate) {
    return fract(sin(dot(fragment_coordinate, vec2(12.9898, 78.233))) * 43758.5453);
}

void main() {
    vec3 color = texture(scene_color, v_uv).rgb;

    // --- Bloom ---
    vec3 halo = texture(bloom, v_uv).rgb / kBloomLevels;
    color = mix(color, halo, push.bloom_strength);

    // --- Rayos de luz ---
    color += texture(light_shafts, v_uv).rgb * push.light_shaft_strength;

    // --- Exposicion y tono ---
    float exposure = push.auto_exposure_enabled > 0.5 && auto_exposure.initialized > 0.5
                         ? auto_exposure.exposure
                         : push.exposure;
    exposure *= exp2(push.exposure_compensation);
    color *= exposure;

    if (push.tonemapper > 0.5) {
        color = acesFitted(color);
    } else {
        // El neutro deja los medios mas claros que ACES: se compensa para que
        // la auto-exposicion de el mismo brillo con los dos. Y algo de
        // contraste, que el neutro por si solo es plano.
        color = applyContrast(color * 0.85, 1.08);
        color = clamp(pbrNeutral(color), 0.0, 1.0);
    }

    // --- Gradacion ---
    color = max(mix(vec3(luminance(color)), color, push.saturation), vec3(0.0));

    // Viveza: sube la saturacion de los colores apagados mas que la de los
    // vivos, que ya estan cerca del limite.
    float chroma = max(color.r, max(color.g, color.b)) - min(color.r, min(color.g, color.b));
    color = max(mix(vec3(luminance(color)), color, 1.0 + kVibrance * (1.0 - chroma)), vec3(0.0));
    color = min(color, vec3(1.0));

    vec2 centered = v_uv - 0.5;
    float vignette = 1.0 - dot(centered, centered) * push.vignette;
    color *= clamp(vignette, 0.0, 1.0);

    // --- Salida en sRGB (la swapchain es UNORM: la gamma va a mano) ---
    color = pow(color, vec3(1.0 / 2.2));
    color += (ditherNoise(gl_FragCoord.xy) - 0.5) / 255.0;

    out_color = vec4(color, 1.0);
}
