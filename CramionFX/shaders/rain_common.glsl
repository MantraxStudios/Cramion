// Agua de lluvia sobre las superficies: lo comparten la pasada de geometria
// (skinned.frag), los rayos (rt_common.glsl) y el IBL (ibl_common.glsl), para
// que un charco sea el mismo charco se vea directamente, en un reflejo o en
// la luz ambiente.
//
// Modelo (Lagarde, "Water drop 3b - Physically based wet surfaces", 2013):
//
//   1. Superficie mojada: el agua que empapa un material poroso rellena sus
//      poros; la luz se dispersa mas veces dentro antes de salir y se
//      absorbe mas: el difuso se oscurece (hasta x0.2) y el material se alisa.
//      La porosidad sale de la rugosidad (lo liso no absorbe agua) y los
//      metales no se empapan.
//   2. Agua acumulada: el agua llena primero lo bajo (las juntas entre
//      adoquines) y al subir el nivel cubre tambien lo alto. Donde cubre, la
//      superficie visible es la lamina de agua: F0 = 0.02 (IOR 1.33), casi
//      espejo, normal horizontal con las ondas de las gotas. Lo de debajo se
//      ve a traves del agua, atenuado por su absorcion (Beer-Lambert, ida y
//      vuelta).
//
// Todo en coordenadas del mundo: los charcos no se mueven con la camara.

#ifndef RAIN_COMMON_GLSL
#define RAIN_COMMON_GLSL

const float kWaterF0 = 0.02;
// Agua quieta: casi espejo. No 0: el reflejo del sol seria un punto de
// radiancia infinita.
const float kWaterRoughness = 0.02;
// Absorcion del agua de un charco (1/m): agua con algo de barro, que se come
// antes el rojo. Con pocos centimetros solo oscurece y enfria un poco.
const vec3 kPuddleAbsorption = vec3(22.0, 14.0, 10.0);
// Profundidad del agua donde el nivel cubre del todo, en metros.
const float kPuddleMaxDepth = 0.025;

// --- Ruido (en coordenadas del mundo) ---
float rainHash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec2 rainHash22(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

float rainValueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(rainHash12(i), rainHash12(i + vec2(1.0, 0.0)), u.x),
               mix(rainHash12(i + vec2(0.0, 1.0)), rainHash12(i + vec2(1.0, 1.0)), u.x), u.y);
}

float rainFbm(vec2 p) {
    float sum = 0.0;
    float amplitude = 0.5;
    for (int i = 0; i < 4; ++i) {
        sum += rainValueNoise(p) * amplitude;
        p = p * 2.03 + vec2(17.1, 9.7);
        amplitude *= 0.5;
    }
    return sum;
}

// Fresnel de Schlick del agua.
float waterFresnel(float cosine) {
    float m = 1.0 - clamp(cosine, 0.0, 1.0);
    float m2 = m * m;
    return kWaterF0 + (1.0 - kWaterF0) * m2 * m2 * m;
}

// --- Nivel del agua ---
// 0 = no hay agua acumulada, 1 = el agua cubre hasta lo mas alto. Entre
// medias solo se llenan las zonas bajas (ver waterCoverage).

// Charcos de la lluvia: las zonas bajas de un ruido grande. Mas lluvia
// (`puddles`): el umbral baja y los charcos crecen y se unen.
float puddleLevel(vec2 xz, float puddles) {
    if (puddles <= 0.0) {
        return 0.0;
    }
    float field = rainFbm(xz * 0.22);
    float threshold = 0.6 - 0.08 * puddles;
    return smoothstep(threshold, threshold + 0.1, field) * min(puddles * 4.0, 1.0);
}

// Zona inundada: elipse (centro xy, radios zw) con la orilla irregular. No
// depende de que llueva ahora ni de que haya techo: el agua ya esta ahi.
float floodLevel(vec2 xz, vec4 flood) {
    if (flood.z <= 0.0 || flood.w <= 0.0) {
        return 0.0;
    }
    float r = length((xz - flood.xy) / flood.zw);
    float shore = (rainFbm(xz * 0.9 + 3.7) - 0.5) * 0.35;
    return clamp((1.0 - r + shore) / 0.3, 0.0, 1.0);
}

// Cuanta superficie tapa el agua en un punto de altura relativa `height`
// (0 = junta o hueco, 1 = cara superior de un adoquin) con el nivel `level`.
// Con el nivel a medias las juntas ya son agua y los adoquines asoman: la
// orilla de un charco real, no un borde recortado.
float waterExcess(float level, float height) {
    return level * 1.25 - 0.25 - height * 0.75;
}

float waterCoverage(float level, float height) {
    return smoothstep(0.0, 0.2, waterExcess(level, height));
}

// Profundidad del agua en metros (para la absorcion).
float waterDepth(float level, float height) {
    return clamp(waterExcess(level, height), 0.0, 1.0) * kPuddleMaxDepth;
}

// Oscurecimiento y alisado de un material empapado (Lagarde). `wet` 0..1.
// Devuelve x = factor del albedo (lineal), y = factor de la rugosidad.
vec2 wetFactors(float roughness, float metallic, float wet) {
    float porosity = clamp((roughness - 0.5) / 0.4, 0.0, 1.0);
    float factor = mix(1.0, 0.2, (1.0 - metallic) * porosity);
    return vec2(mix(1.0, factor, wet), mix(1.0, factor, 0.5 * wet));
}

// --- Ondas de las gotas ---
// Cada celda de una rejilla recibe gotas cada cierto tiempo, en un punto al
// azar distinto en cada gota. Cada impacto lanza un tren de ondas capilares
// (longitud de onda de pocos centimetros) que se expande, se ensancha y se
// amortigua:
//
//   h(r, t) = A (1 - t)^2 exp(-x^2 / w^2) sin(k x),   x = r - v t
//
// Se devuelve la pendiente de la superficie (dh/dx, dh/dz) con la derivada
// analitica de h respecto a r. `intensity` (0..1) es cuanta lluvia cae.
// `footprint` es lo que mide un pixel en metros: las ondas mas finas que
// dos pixeles se apagan (si no, aliasing: el brillo parpadea en puntos).
vec2 rainRipples(vec2 p, float time, float intensity, float footprint) {
    vec2 slope = vec2(0.0);
    for (int layer = 0; layer < 2; ++layer) {
        float cells_per_meter = layer == 0 ? 2.6 : 4.3;
        // Longitud de onda en celdas: 1.5 cm .. 4 cm en el mundo.
        const float kWavelength = 0.1;
        float wavelength_m = kWavelength / cells_per_meter;
        float resolvable = 1.0 - smoothstep(0.25, 0.5, footprint / wavelength_m);
        if (resolvable <= 0.0) {
            continue;
        }

        vec2 q = p * cells_per_meter + float(layer) * 31.7;
        vec2 cell = floor(q);
        vec2 f = fract(q);
        const float k = 6.2831853 / kWavelength;

        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                vec2 neighbor = vec2(x, y);
                vec2 id = cell + neighbor;
                vec2 random = rainHash22(id);
                float period = 0.9 + random.y * 0.8;
                float phase = time / period + random.x;
                float drop = floor(phase);
                float t = fract(phase);

                // Con poca lluvia no todas las celdas reciben gota.
                if (rainHash12(id + drop * vec2(7.31, 1.93)) > 0.25 + 0.75 * intensity) {
                    continue;
                }

                vec2 center = neighbor + 0.15 + 0.7 * rainHash22(id + drop * vec2(3.17, 5.71));
                vec2 to_point = f - center;
                float r = length(to_point);
                float front = t * 0.8;
                float width = 0.05 + 0.12 * t;
                float xr = r - front;
                float envelope = exp(-xr * xr / (width * width));
                float amplitude = (1.0 - t) * (1.0 - t);
                float s = sin(k * xr);
                float c = cos(k * xr);
                // dh/dr en unidades de celda.
                float dh = amplitude * envelope * (k * c - 2.0 * xr / (width * width) * s);
                slope += to_point / max(r, 1e-4) * dh * resolvable;
            }
        }
    }
    // Pendiente maxima de ~0.12: las ondas de lluvia son sutiles.
    return slope * 0.0019;
}

#endif  // RAIN_COMMON_GLSL
