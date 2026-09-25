#version 450

// Antialiasing temporal con escalado (TAA / TAAU, como el de Unreal):
//
//   - La camara se desplaza cada frame una fraccion de pixel (jitter, Halton
//     2,3): con los frames acumulados cada pixel de pantalla recibe muestras
//     de posiciones distintas, asi que la resolucion interna puede ser menor
//     que la de pantalla.
//   - La historia (frame anterior, en pantalla) se reproyecta con los vectores
//     de movimiento (G-buffer; el cielo, con la camara) del mas cercano de los
//     3x3 vecinos, para que los bordes de lo que se mueve no se arrastren.
//   - Se recorta al rango de colores de los vecinos (clip en YCoCg con media y
//     varianza): lo que ya no esta alli no deja fantasmas.
//   - Se mezcla en un espacio comprimido (x / (1 + luma)) para que un punto
//     muy brillante no parpadee.

layout(set = 0, binding = 0) uniform sampler2D scene_color;   // resolucion interna
layout(set = 0, binding = 1) uniform sampler2D scene_depth;
layout(set = 0, binding = 2) uniform sampler2D velocity_map;  // UV actual - UV anterior
layout(set = 0, binding = 3) uniform sampler2D history;       // pantalla, frame anterior

layout(push_constant) uniform PushConstants {
    vec4 render_size;  // ancho, alto, 1/ancho, 1/alto (resolucion interna)
    vec4 output_size;  // igual, en pantalla
    vec4 jitter;       // xy = desplazamiento de este frame en UV; z = historia valida
    mat4 reproject;    // NDC sin jitter de este frame -> recorte del anterior (cielo)
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 compress(vec3 c) { return c / (1.0 + luma(c)); }
vec3 expand(vec3 c) { return c / max(1.0 - luma(c), 1e-4); }

vec3 toYCoCg(vec3 c) {
    return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
vec3 fromYCoCg(vec3 c) {
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// Catmull-Rom con 5 lecturas bilineales (historia nitida).
vec3 sampleHistory(vec2 uv) {
    vec2 size = push.output_size.xy;
    vec2 position = uv * size;
    vec2 center = floor(position - 0.5) + 0.5;
    vec2 f = position - center;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 tc12 = (center + w2 / w12) * push.output_size.zw;
    vec2 tc0 = (center - 1.0) * push.output_size.zw;
    vec2 tc3 = (center + 2.0) * push.output_size.zw;
    vec3 result = textureLod(history, vec2(tc12.x, tc0.y), 0.0).rgb * (w12.x * w0.y) +
                  textureLod(history, vec2(tc0.x, tc12.y), 0.0).rgb * (w0.x * w12.y) +
                  textureLod(history, vec2(tc12.x, tc12.y), 0.0).rgb * (w12.x * w12.y) +
                  textureLod(history, vec2(tc3.x, tc12.y), 0.0).rgb * (w3.x * w12.y) +
                  textureLod(history, vec2(tc12.x, tc3.y), 0.0).rgb * (w12.x * w3.y);
    float weight = (w12.x * w0.y) + (w0.x * w12.y) + (w12.x * w12.y) + (w3.x * w12.y) + (w12.x * w3.y);
    return max(result / weight, vec3(0.0));
}

void main() {
    vec2 uv = v_uv;
    // La imagen interna esta desplazada por el jitter: donde leer el punto uv.
    vec2 source_uv = uv + push.jitter.xy;
    vec2 source_pixel = source_uv * push.render_size.xy;
    ivec2 center = ivec2(floor(source_pixel));
    ivec2 last = ivec2(push.render_size.xy) - 1;

    // --- Vecinos: rango de color y el mas cercano (vectores de movimiento) ---
    vec3 m1 = vec3(0.0);
    vec3 m2 = vec3(0.0);
    float nearest_depth = 1.0;
    ivec2 nearest = center;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            ivec2 p = clamp(center + ivec2(x, y), ivec2(0), last);
            vec3 c = toYCoCg(compress(texelFetch(scene_color, p, 0).rgb));
            m1 += c;
            m2 += c * c;
            float d = texelFetch(scene_depth, p, 0).r;
            if (d < nearest_depth) {
                nearest_depth = d;
                nearest = p;
            }
        }
    }
    vec3 mean = m1 / 9.0;
    vec3 sigma = sqrt(max(m2 / 9.0 - mean * mean, vec3(0.0)));
    vec3 box_min = mean - 1.25 * sigma;
    vec3 box_max = mean + 1.25 * sigma;

    vec3 current = max(textureLod(scene_color, source_uv, 0.0).rgb, vec3(0.0));

    // --- Donde estaba este punto el frame anterior ---
    vec2 velocity;
    if (nearest_depth >= 1.0) {
        // Cielo (sin geometria): solo se movio la camara.
        vec4 previous = push.reproject * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
        velocity = uv - (previous.xy / max(previous.w, 1e-6) * 0.5 + 0.5);
    } else {
        velocity = texelFetch(velocity_map, nearest, 0).xy;
    }
    vec2 history_uv = uv - velocity;

    bool valid = push.jitter.z > 0.5 && all(greaterThanEqual(history_uv, vec2(0.0))) &&
                 all(lessThanEqual(history_uv, vec2(1.0)));
    if (!valid) {
        out_color = vec4(current, 1.0);
        return;
    }

    vec3 previous = toYCoCg(compress(sampleHistory(history_uv)));
    // Recorte hacia la media (no solo al borde de la caja): menos fantasmas.
    vec3 offset = previous - mean;
    vec3 extent = max(box_max - mean, vec3(1e-5));
    vec3 ratio = abs(offset / extent);
    float largest = max(ratio.x, max(ratio.y, ratio.z));
    if (largest > 1.0) previous = mean + offset / largest;

    // Peso de la muestra nueva: mayor cuanto mas cerca cae del centro del
    // pixel de pantalla (escalado) y cuanto mas rapido se mueve.
    vec2 texel_offset = source_pixel - (vec2(center) + 0.5);
    float closeness = exp(-2.29 * dot(texel_offset, texel_offset));
    float scale = push.render_size.x * push.output_size.z;  // < 1 al escalar
    float alpha = mix(0.04, 0.12, closeness) * mix(0.6, 1.0, scale);
    float speed = length(velocity * push.output_size.xy);
    alpha = clamp(alpha + speed * 0.01, 0.03, 0.5);

    vec3 now = toYCoCg(compress(current));
    vec3 result = mix(previous, now, alpha);
    result = expand(max(fromYCoCg(result), vec3(0.0)));
    if (any(isnan(result)) || any(isinf(result))) result = current;
    out_color = vec4(result, 1.0);
}
