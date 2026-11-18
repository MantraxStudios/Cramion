#version 450

// Salida HDR (DisplayOutput.h): compone en la swapchain HDR10 (PQ, BT.2020)
// o scRGB (lineal, 1.0 = 80 nits) lo que en SDR iria directo a la pantalla:
//
//   - La escena ya terminada (composicion + FXAA + la UI en el mundo), en
//     sRGB de 8 bits, al brillo de papel (paper_white nits).
//   - Las luces de la escena por encima del papel: mapa de ganancia. Se lee
//     la imagen HDR de antes del tono (la misma que la composicion) y se
//     compara la curva AgX de SDR con la de HDR (el mismo AgX con el hombro
//     llevado hasta max_nits): donde la SDR ya se satura, el pixel se
//     multiplica por lo que de mas daria la pantalla HDR. Los medios tonos no
//     cambian (siguen al brillo de papel).
//   - La UI de pantalla (ImGui / UiRenderer), dibujada aparte sobre negro
//     transparente (alfa premultiplicado), encima y siempre al brillo de
//     papel (sin ganancia: un texto blanco no deslumbra).

layout(set = 0, binding = 0) uniform sampler2D scene_sdr;
layout(set = 0, binding = 1) uniform sampler2D ui_layer;
layout(set = 0, binding = 2) uniform sampler2D scene_hdr;

layout(set = 0, binding = 3) readonly buffer Exposure {
    float exposure;
    float log_exposure;
    float average_luminance;
    float initialized;
} auto_exposure;

// Debe coincidir con GpuHdrOutputPush (GpuTypes.h).
layout(push_constant) uniform Push {
    float encoding;         // 1 = HDR10 (PQ), 2 = scRGB
    float paper_white;      // nits
    float max_nits;
    float manual_exposure;
    float auto_on;
    float exposure_compensation;
    float gain_strength;    // 0..1
    float tonemapper;
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

vec3 srgbToLinear(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

float luminance(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

float agxContrast(float x) {
    float x2 = x * x;
    float x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

// Curva AgX en luminancia (sin la matriz de primarios): escena -> pantalla
// lineal 0..1 (la del tono de composite.frag).
float agxCurve(float l) {
    const float kMinEv = -12.47393;
    const float kMaxEv = 4.026069;
    float x = clamp((log2(max(l, 1e-10)) - kMinEv) / (kMaxEv - kMinEv), 0.0, 1.0);
    return pow(max(agxContrast(x), 0.0), 2.2);
}

// AgX adaptado al rango de la pantalla: igual que el SDR en sombras y medios
// tonos; donde el SDR se acerca a su blanco, el hombro sigue subiendo hasta
// `peak` (max_nits / paper_white).
float agxHdrCurve(float l, float peak) {
    float sdr = agxCurve(l);
    float extended = peak * agxCurve(l / peak);
    float w = smoothstep(0.5, 0.95, sdr);
    return mix(sdr, max(sdr, extended), w);
}

// PQ (SMPTE ST 2084): nits -> 0..1.
vec3 pqEncode(vec3 nits) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    vec3 y = clamp(nits / 10000.0, 0.0, 1.0);
    vec3 p = pow(y, vec3(m1));
    return pow((c1 + c2 * p) / (1.0 + c3 * p), vec3(m2));
}

// BT.709 / sRGB lineal -> BT.2020 lineal.
const mat3 kRec709ToRec2020 = mat3(vec3(0.6274, 0.0691, 0.0164), vec3(0.3293, 0.9195, 0.0880),
                                   vec3(0.0433, 0.0113, 0.8956));

void main() {
    float paper = max(push.paper_white, 1.0);
    float peak = max(push.max_nits / paper, 1.0);

    vec3 scene = srgbToLinear(texture(scene_sdr, v_uv).rgb);
    vec4 ui = texture(ui_layer, v_uv);
    float ui_alpha = clamp(ui.a, 0.0, 1.0);

    // --- Mapa de ganancia (luces por encima del papel) ---
    float gain = 1.0;
    if (push.gain_strength > 0.0 && peak > 1.001 && ui_alpha < 0.999) {
        bool auto_on = push.auto_on > 0.5 && auto_exposure.initialized > 0.5;
        float exposure = (auto_on ? auto_exposure.exposure : push.manual_exposure) * exp2(push.exposure_compensation);
        float l = luminance(max(texture(scene_hdr, v_uv).rgb, vec3(0.0))) * exposure;
        float sdr = agxCurve(l);
        float hdr = agxHdrCurve(l, peak);
        gain = clamp(hdr / max(sdr, 1e-4), 1.0, peak);
        gain = mix(1.0, gain, clamp(push.gain_strength, 0.0, 1.0));
    }
    vec3 color = scene * gain;

    // --- UI encima (alfa premultiplicado en sRGB) ---
    if (ui_alpha > 0.0) {
        vec3 ui_linear = srgbToLinear(ui.rgb / ui_alpha);
        color = ui_linear * ui_alpha + color * (1.0 - ui_alpha);
    }

    // --- Codificacion de la pantalla ---
    vec3 nits = color * paper;
    if (push.encoding > 1.5) {
        // scRGB: lineal BT.709, 1.0 = 80 nits.
        out_color = vec4(min(nits, vec3(push.max_nits)) / 80.0, 1.0);
    } else {
        vec3 rec2020 = max(kRec709ToRec2020 * nits, vec3(0.0));
        out_color = vec4(pqEncode(min(rec2020, vec3(push.max_nits))), 1.0);
    }
}
