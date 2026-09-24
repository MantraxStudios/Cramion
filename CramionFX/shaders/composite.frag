#version 450

// Composicion final en HDR -> imagen de 8 bits que lee el FXAA. Todo lo
// configurable sale de PostProcessSettings (como el Volume de Unity), en un
// uniform buffer por frame (GpuCompositeSettings).
//
//   1. Aberracion cromatica: el rojo y el azul se leen desplazados hacia
//                 fuera, mas cuanto mas lejos del centro (como una lente).
//   2. Bloom:     se mezcla una fraccion del halo (con su tinte). El umbral
//                 ya se aplico al generarlo (bloom_down.frag).
//   3. Rayos de luz del sol (light_shafts.frag).
//   4. Exposicion: automatica por histograma (exposure_average.comp) o
//                 manual, mas la compensacion en EV.
//   5. Gradacion en HDR, antes del tono (como Unity): balance de blancos en
//                 espacio LMS, contraste en escala logaritmica alrededor del
//                 gris medio, filtro de color y lift / gamma / gain.
//   6. Tono:      Khronos PBR Neutral (por defecto), ACES o ninguno.
//   7. Saturacion y viveza (satura mas lo apagado).
//   8. Vineta (intensidad, suavidad y color) y grano de pelicula.
//   9. Gamma y ruido de +-1/255 contra el bandeado de los 8 bits.

layout(set = 0, binding = 0) uniform sampler2D scene_color;
layout(set = 0, binding = 1) uniform sampler2D bloom;
layout(set = 0, binding = 2) uniform sampler2D light_shafts;

layout(set = 0, binding = 3) readonly buffer Exposure {
    float exposure;
    float log_exposure;
    float average_luminance;
    float initialized;
} auto_exposure;

// Debe coincidir con GpuCompositeSettings (GpuTypes.h).
layout(set = 0, binding = 4) uniform Settings {
    vec4 exposure;        // x = manual, y = bloom, z = 1 auto, w = compensacion (EV)
    vec4 tone;            // x = rayos de luz, y = tonemapper, z = saturacion, w = contraste
    vec4 look;            // x = viveza, y = vineta, z = suavidad de la vineta, w = aberracion
    vec4 film;            // x = grano, y = frame, zw = 1 / resolucion
    vec4 white_balance;   // rgb = factores LMS
    vec4 color_filter;
    vec4 lift;
    vec4 gamma;
    vec4 gain;
    vec4 vignette_color;
    vec4 bloom_tint;
} settings;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

// Numero de niveles del bloom (BloomChain::kLevels): la subida los suma todos.
const float kBloomLevels = 6.0;

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

// sRGB lineal <-> LMS (CAT02), las de Unity. Construidas por filas: se
// multiplican como `v * M`.
const mat3 kLinearToLms = mat3(
    vec3(3.90405e-1, 5.49941e-1, 8.92632e-3),
    vec3(7.08416e-2, 9.63172e-1, 1.35775e-3),
    vec3(2.31082e-2, 1.28021e-1, 9.36245e-1));
const mat3 kLmsToLinear = mat3(
    vec3(2.85847e+0, -1.62879e+0, -2.48910e-2),
    vec3(-2.10182e-1, 1.15820e+0, 3.24281e-4),
    vec3(-4.18120e-2, -1.18169e-1, 1.06867e+0));

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
// "Contrast" de la gradacion de Unity): no toca el gris ni el negro.
vec3 applyContrast(vec3 color, float contrast) {
    const float kMiddleGrey = 0.18;
    return kMiddleGrey * pow(max(color, vec3(0.0)) / kMiddleGrey, vec3(contrast));
}

float luminance(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    vec2 centered = v_uv - 0.5;

    // --- Aberracion cromatica (desplazamiento radial de rojo y azul) ---
    vec3 color;
    float aberration = settings.look.w;
    if (aberration > 0.0) {
        vec2 offset = centered * dot(centered, centered) * aberration * 0.08;
        color.r = texture(scene_color, v_uv - offset).r;
        color.g = texture(scene_color, v_uv).g;
        color.b = texture(scene_color, v_uv + offset).b;
    } else {
        color = texture(scene_color, v_uv).rgb;
    }

    // --- Bloom ---
    vec3 halo = texture(bloom, v_uv).rgb / kBloomLevels * settings.bloom_tint.rgb;
    color = mix(color, halo, settings.exposure.y);

    // --- Rayos de luz ---
    color += texture(light_shafts, v_uv).rgb * settings.tone.x;

    // --- Exposicion ---
    float exposure = settings.exposure.z > 0.5 && auto_exposure.initialized > 0.5
                         ? auto_exposure.exposure
                         : settings.exposure.x;
    exposure *= exp2(settings.exposure.w);
    color *= exposure;

    // --- Gradacion en HDR ---
    // Balance de blancos: en LMS, cada cono por su factor.
    color = (color * kLinearToLms) * settings.white_balance.rgb * kLmsToLinear;
    color = max(color, vec3(0.0));

    int tonemapper = int(settings.tone.y + 0.5);
    // El neutro deja los medios mas claros que ACES: se compensa para que la
    // auto-exposicion de el mismo brillo con los dos.
    if (tonemapper == 0) {
        color *= 0.85;
    }
    color = applyContrast(color, settings.tone.w);
    color *= settings.color_filter.rgb;

    // Lift / gamma / gain (como las ruedas de color de Unity).
    color = color * settings.gain.rgb + settings.lift.rgb * (1.0 - clamp(color, 0.0, 1.0));
    color = pow(max(color, vec3(0.0)), 1.0 / max(settings.gamma.rgb, vec3(0.01)));

    // --- Tono ---
    if (tonemapper == 1) {
        color = acesFitted(color);
    } else if (tonemapper == 0) {
        color = clamp(pbrNeutral(color), 0.0, 1.0);
    } else {
        color = clamp(color, 0.0, 1.0);
    }

    // --- Saturacion y viveza ---
    color = max(mix(vec3(luminance(color)), color, settings.tone.z), vec3(0.0));
    float chroma = max(color.r, max(color.g, color.b)) - min(color.r, min(color.g, color.b));
    color = max(mix(vec3(luminance(color)), color, 1.0 + settings.look.x * (1.0 - chroma)),
                vec3(0.0));
    color = min(color, vec3(1.0));

    // --- Vineta ---
    // Con suavidad 0.5 es la de siempre (lineal con la distancia al
    // cuadrado); menos suavidad = borde mas definido, mas = mas extendida.
    float falloff = clamp(dot(centered, centered) * 2.0, 0.0, 1.0);
    float exponent = exp2((0.5 - settings.look.z) * 2.0);
    float vignette = clamp(1.0 - settings.look.y * 0.5 * pow(falloff, exponent), 0.0, 1.0);
    color = mix(settings.vignette_color.rgb, color, vignette);

    // --- Grano de pelicula (se nota mas en los medios que en las luces) ---
    if (settings.film.x > 0.0) {
        vec2 pixel = gl_FragCoord.xy + vec2(settings.film.y * 37.0, settings.film.y * 17.0);
        float grain = hash12(pixel) - 0.5;
        float response = 1.0 - smoothstep(0.2, 1.0, luminance(color));
        color = clamp(color + grain * settings.film.x * 0.2 * (0.35 + 0.65 * response), 0.0, 1.0);
    }

    // --- Salida en sRGB (la swapchain es UNORM: la gamma va a mano) ---
    color = pow(color, vec3(1.0 / 2.2));
    color += (hash12(gl_FragCoord.xy) - 0.5) / 255.0;

    out_color = vec4(color, 1.0);
}
