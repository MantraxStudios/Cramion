// Agua: datos compartidos por water.vert, water.frag y water_under.frag.

struct WaterBody {
    vec4 origin;   // xyz origen, w = giro en Y
    vec4 extent;   // xy = medio tamano (lago), z = tipo (0 oceano, 1 lago, 2 rio)
    vec4 shallow;  // rgb dispersion, w = transparencia (m)
    vec4 deep;     // rgb color profundo, w = espuma
    vec4 waves;    // altura, longitud de onda, velocidad, crestas
    vec4 wind;     // viento (rad), dispersion (rad), corriente, ondulacion fina
    vec4 look;     // rugosidad, refraccion, causticas, espuma de orilla (m)
    vec4 extra;    // olas de playa, luz en las crestas, superficie en la camara (rio), -
    vec4 under;    // rayos de sol, particulas, -, -
};

layout(set = 1, binding = 0) uniform WaterBuffer {
    vec4 time_count;     // x = tiempo, y = cuerpos, z = cuerpo con la camara dentro (-1), w = tiempo del oceano
    vec4 ripple;         // olas interactivas: xy = esquina (x, z), z = celda (m), w = lado (0 = no hay)
    vec4 cascade_size;   // oceano FFT: lado de cada cascada (m); 0 = sin espectro
    vec4 cascade_slope;  // varianza de la pendiente de cada cascada
    vec4 ocean;          // x choppiness, y altura significativa, z niveles del clipmap
    WaterBody bodies[16];
} water;

// Olas interactivas (water::RippleSimulation): alturas por filas (z).
layout(std430, set = 1, binding = 1) readonly buffer RippleBuffer {
    float ripple_heights[];
};

// Oceano FFT (water_fft.comp): una capa por cascada, con mipmaps.
layout(set = 1, binding = 2) uniform sampler2DArray ocean_displacement;  // xyz desplazamiento, w espuma
layout(set = 1, binding = 3) uniform sampler2DArray ocean_derivatives;   // dY/dx, dY/dz, dX/dx, dZ/dz

layout(push_constant) uniform PushConstants {
    uint body;
    uint mesh;  // 0 lago, 1 oceano, 2 rio
    uvec2 pad;
} push;

const float kWaterPi = 3.14159265358979;
const float kOceanTexels = 128.0;

float rippleCell(ivec2 c, int n) {
    c = clamp(c, ivec2(0), ivec2(n - 1));
    return ripple_heights[c.y * n + c.x];
}

// Altura de las olas interactivas en (x, z) del mundo, interpolada y
// apagada en el borde de la rejilla.
float rippleHeight(vec2 xz) {
    int n = int(water.ripple.w);
    if (n < 2) return 0.0;
    vec2 g = (xz - water.ripple.xy) / water.ripple.z;
    if (any(lessThan(g, vec2(0.0))) || any(greaterThanEqual(g, vec2(float(n - 1))))) return 0.0;
    ivec2 i = ivec2(floor(g));
    vec2 f = g - vec2(i);
    float a = mix(rippleCell(i, n), rippleCell(i + ivec2(1, 0), n), f.x);
    float b = mix(rippleCell(i + ivec2(0, 1), n), rippleCell(i + ivec2(1, 1), n), f.x);
    vec2 edge = min(g, vec2(float(n - 1)) - g);
    float fade = smoothstep(0.0, 10.0, min(edge.x, edge.y));
    return mix(a, b, f.y) * fade;
}

// Pendiente (dh/dx, dh/dz) de las olas interactivas.
vec2 rippleSlope(vec2 xz) {
    if (water.ripple.w < 2.0) return vec2(0.0);
    float e = water.ripple.z;
    return vec2(rippleHeight(xz + vec2(e, 0.0)) - rippleHeight(xz - vec2(e, 0.0)),
                rippleHeight(xz + vec2(0.0, e)) - rippleHeight(xz - vec2(0.0, e))) / (2.0 * e);
}

// --- Oceano FFT ---
bool oceanAvailable() { return water.cascade_size.x > 0.0; }

// Coordenada de textura de la cascada `c` para el punto `p` (m, relativo al
// origen del agua). El texel n guarda el valor en x = n * L / N (la CPU usa
// lo mismo): de ahi el medio texel.
vec3 oceanUv(vec2 p, int c) { return vec3(p / water.cascade_size[c] + 0.5 / kOceanTexels, float(c)); }

// Desplazamiento (xyz) y espuma (w) filtrados para vertices separados
// `spacing` m: las olas mas cortas que dos vertices se promedian (mipmap).
vec4 oceanDisplacementAt(vec2 p, float spacing) {
    vec4 d = vec4(0.0);
    for (int c = 0; c < 4; ++c) {
        float lod = max(log2(spacing * 2.0 * kOceanTexels / water.cascade_size[c]), 0.0);
        d += textureLod(ocean_displacement, oceanUv(p, c), lod);
    }
    return d;
}

// Altura del oceano en el punto `p` (relativo al origen): el que la ola
// lleva hasta alli (se deshace el desplazamiento horizontal).
float oceanHeight(vec2 p, float spacing) {
    vec2 q = p;
    for (int i = 0; i < 3; ++i) {
        q = p - oceanDisplacementAt(q, spacing).xz;
    }
    return oceanDisplacementAt(q, spacing).y;
}

// Separacion de la malla del oceano en el nivel 0: mas grande cuanto mas alta
// la camara (a ras de agua, 20 cm; desde un avion, metros).
float oceanBaseSpacing(float camera_height) {
    return 0.2 * exp2(floor(log2(max(abs(camera_height), 1.0) / 8.0 + 1.0)));
}

// Causticas: la luz del sol que las olas concentran en el fondo (0 .. ~4).
// Las usan water.frag (el fondo visto a traves de la superficie) y
// water_under.frag (lo que hay bajo el agua visto desde dentro).
float caustic(vec2 p, float t) {
    vec2 q = p * 0.7;
    float c = 0.0;
    float s = 1.0;
    for (int i = 0; i < 3; ++i) {
        q += vec2(sin(q.y * 1.7 + t * 0.9), cos(q.x * 1.3 - t * 0.7)) * 0.55;
        c += abs(sin(q.x) * cos(q.y)) * s;
        s *= 0.6;
        q *= 1.9;
    }
    return pow(clamp(1.0 - c * 0.55, 0.0, 1.0), 5.0) * 4.0;
}

// Oleaje de lagos y rios: espectro JONSWAP muestreado en 24 ondas de
// Gerstner. Longitudes de 2 a 0.06 veces la principal; la amplitud de cada una
// sale del espectro y esta normalizada para que la altura sea la ALTURA
// SIGNIFICATIVA. Direcciones repartidas alrededor del viento y fases
// pseudoaleatorias. MISMAS tablas en CramionCore/src/water/Water.cpp.
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

// Lago: dentro de su rectangulo (con margen).
bool insideLake(WaterBody b, vec2 xz, float margin) {
    float c = cos(b.origin.w);
    float s = sin(b.origin.w);
    vec2 d = xz - b.origin.xz;
    vec2 local = vec2(d.x * c + d.y * s, -d.x * s + d.y * c);
    return all(lessThanEqual(abs(local), b.extent.xy + vec2(margin)));
}

// Altura de la superficie del cuerpo en (x, z) del mundo (para decidir que
// esta bajo el agua). Rio: la de la camara (extra.z).
float waterSurfaceHeight(WaterBody b, vec2 xz, float t) {
    int type = int(b.extent.z + 0.5);
    if (type == 2) return b.extra.z;
    if (type == 0 && oceanAvailable()) return b.origin.y + oceanHeight(xz - b.origin.xz, 0.1);
    vec3 n;
    float j;
    return b.origin.y + gerstnerWaves(b, xz - b.origin.xz, t, 0.0, n, j).y;
}

// --- Luz bajo el agua (comun a la superficie vista desde abajo y al volumen) ---
// Coeficiente de extincion por canal (1/m): el agua se traga antes el rojo;
// `shallow` (el color de la dispersion) es lo que sobrevive.
vec3 waterExtinction(WaterBody b) {
    float clarity = max(b.shallow.w, 0.05);
    return (vec3(1.05) - clamp(b.shallow.rgb, 0.0, 1.0)) * (2.3 / clarity);
}

// Radiancia del agua "infinita" (lo que se ve mirando a lo hondo) a `depth`
// metros bajo la superficie: luz del cielo y del sol dispersada, cada vez mas
// oscura y azul con la profundidad.
vec3 waterInScatter(WaterBody b, vec3 sun_radiance, float sun_height, vec3 ambient, float depth) {
    vec3 light_left = exp(-waterExtinction(b) * max(depth, 0.0) * 0.35);
    vec3 albedo = mix(b.deep.rgb, b.shallow.rgb, 0.35);
    return albedo * (sun_radiance * sun_height * 0.35 + ambient) * light_left;
}
