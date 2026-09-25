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

// Oleaje Gerstner: 8 ondas alrededor del viento. MISMOS valores que
// CramionCore/src/water/Water.cpp (water::gerstner), que da la altura a la
// fisica. `fade_distance` > 0 apaga las ondas cortas a lo lejos (sin aliasing).
const float kWaveAngles[8] = float[](0.0, 0.83, -0.61, 1.37, -1.19, 0.29, -0.93, 1.71);

vec3 gerstnerWaves(WaterBody b, vec2 p, float t, float fade_distance, out vec3 normal, out float jacobian) {
    vec3 offset = vec3(0.0);
    vec3 n = vec3(0.0, 1.0, 0.0);
    jacobian = 1.0;
    float len = max(b.waves.y, 0.05);
    float amplitude = b.waves.x * 0.5;
    float steep = clamp(b.waves.w, 0.0, 1.0);
    for (int i = 0; i < 8; ++i) {
        float fade = fade_distance > 0.0 ? clamp(1.0 - fade_distance / (len * 60.0), 0.0, 1.0) : 1.0;
        float a = amplitude * fade;
        float angle = b.wind.x + kWaveAngles[i] * b.wind.y;
        vec2 d = vec2(cos(angle), sin(angle));
        float k = 2.0 * kWaterPi / len;
        float omega = sqrt(9.81 * k) * b.waves.z;
        float q = amplitude > 1e-6 ? steep / (k * amplitude * 8.0) : 0.0;
        float theta = k * dot(d, p) - omega * t + 1.7 * float(i);
        float c = cos(theta);
        float s = sin(theta);
        offset.xz += q * a * d * c;
        offset.y += a * s;
        n.xz -= d * k * a * c;
        n.y -= q * k * a * s;
        jacobian -= q * k * a * s;
        len *= 0.64;
        amplitude *= 0.60;
    }
    normal = normalize(n);
    return offset;
}
