// Agua: datos compartidos por water.vert y water.frag.

struct WaterBody {
    vec4 origin;   // xyz origen, w = giro en Y
    vec4 extent;   // xy = medio tamano (lago), z = tipo (0 oceano, 1 lago, 2 rio)
    vec4 shallow;  // rgb dispersion, w = transparencia (m)
    vec4 deep;     // rgb color profundo, w = espuma
    vec4 waves;    // altura, longitud de onda, velocidad, crestas
    vec4 wind;     // viento (rad), dispersion (rad), corriente, ondulacion fina
    vec4 look;     // rugosidad, refraccion, causticas, espuma de orilla (m)
    vec4 extra;    // olas de playa, luz en las crestas
};

layout(set = 1, binding = 0) uniform WaterBuffer {
    vec4 time_count;  // x = tiempo
    WaterBody bodies[16];
} water;

layout(push_constant) uniform PushConstants {
    uint body;
    uint mesh;  // 0 lago, 1 oceano, 2 rio
    uvec2 pad;
} push;

const float kWaterPi = 3.14159265358979;

// Oleaje: espectro JONSWAP (el del mar real, medido en el Mar del Norte)
// muestreado en 24 ondas de Gerstner. Longitudes de 2 a 0.06 veces la
// principal; la amplitud de cada una sale del espectro (el pico, cerca de la
// principal) y esta normalizada para que la altura sea la ALTURA SIGNIFICATIVA
// (la media del tercio mas alto de las olas, la que usan los oceanografos).
// Direcciones repartidas alrededor del viento (mas abiertas cuanto mas corta
// la onda) y fases pseudoaleatorias: sin el patron regular de pocas ondas.
// MISMAS tablas en CramionCore/src/water/Water.cpp (la flotacion).
const int kWaveCount = 24;
const float kWaveLengths[24] = float[](2.000000, 1.717188, 1.474368, 1.265883, 1.086880, 0.933189, 0.801230, 0.687931, 0.590654, 0.507132, 0.435420, 0.373849, 0.320985, 0.275596, 0.236625, 0.203165, 0.174436, 0.149770, 0.128591, 0.110408, 0.094795, 0.081391, 0.069882, 0.060000);
const float kWaveAmplitudes[24] = float[](0.029095, 0.048279, 0.068845, 0.097571, 0.152326, 0.166827, 0.122764, 0.094758, 0.084349, 0.076526, 0.068538, 0.060708, 0.053334, 0.046573, 0.040490, 0.035085, 0.030329, 0.026171, 0.022553, 0.019417, 0.016704, 0.014363, 0.012346, 0.010609);
const float kWaveDirections[24] = float[](-0.083431, 0.569487, 0.071680, -0.290316, 0.415380, -0.021591, -0.618493, 0.222443, -0.215726, 0.735936, 0.037831, -0.551784, 0.467343, -0.104373, -1.029364, 0.192298, -0.424478, 0.817133, -0.002207, -0.904800, 0.451188, -0.257296, 1.281280, 0.120902);
const float kWavePhases[24] = float[](1.193805, 5.936841, 4.396692, 2.856543, 1.316394, 6.059431, 4.519282, 2.979132, 1.438983, 6.182020, 4.641871, 3.101722, 1.561573, 0.021424, 4.764460, 3.224311, 1.684162, 0.144013, 4.887049, 3.346900, 1.806751, 0.266602, 5.009638, 3.469489);

// `fade_distance` > 0 apaga las ondas cortas a lo lejos (sin aliasing en la
// malla). `p` = posicion relativa al origen del cuerpo de agua.
vec3 gerstnerWaves(WaterBody b, vec2 p, float t, float fade_distance, out vec3 normal, out float jacobian) {
    vec3 offset = vec3(0.0);
    vec3 n = vec3(0.0, 1.0, 0.0);
    jacobian = 1.0;
    float peak = max(b.waves.y, 0.05);
    float height = b.waves.x;
    float steep = clamp(b.waves.w, 0.0, 1.0);
    for (int i = 0; i < kWaveCount; ++i) {
        float len = peak * kWaveLengths[i];
        float amplitude = height * kWaveAmplitudes[i];
        float fade = fade_distance > 0.0 ? clamp(1.0 - fade_distance / (len * 60.0), 0.0, 1.0) : 1.0;
        float a = amplitude * fade;
        float angle = b.wind.x + kWaveDirections[i] * b.wind.y;
        vec2 d = vec2(cos(angle), sin(angle));
        float k = 2.0 * kWaterPi / len;
        float omega = sqrt(9.81 * k) * b.waves.z;
        // Suma de Q*k*A = steep <= 1: crestas afiladas sin bucles.
        float q = amplitude > 1e-6 ? steep / (k * amplitude * float(kWaveCount)) : 0.0;
        float theta = k * dot(d, p) - omega * t + kWavePhases[i];
        float c = cos(theta);
        float s = sin(theta);
        offset.xz += q * a * d * c;
        offset.y += a * s;
        n.xz -= d * k * a * c;
        n.y -= q * k * a * s;
        jacobian -= q * k * a * s;
    }
    normal = normalize(n);
    return offset;
}
