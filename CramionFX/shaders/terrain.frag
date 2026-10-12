#version 450
#extension GL_GOOGLE_include_directive : require

// Terreno en el G-buffer: normal de la textura de alturas (por pixel), hasta
// 8 capas mezcladas por los mapas de pesos (splat), cada una con su color o
// textura (con una segunda escala para romper la repeticion), normal map,
// rugosidad y metal. Despues, lluvia, charcos, humedad y decals como
// cualquier suelo (gbuffer_surface.glsl). Bajo la hierba de la GPU, el suelo
// toma el color del cesped (oscuro de cerca, el de las puntas de lejos): los
// huecos entre briznas y la hierba lejana se leen como pelo, no como tierra.

// La misma que terrain.vert (set 0 del frame: visible en el fragmento).
layout(set = 0, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
    mat4 unjittered_view_projection;
    mat4 previous_view_projection;
    vec4 jitter;
    uvec4 motion;
} camera;

layout(set = 1, binding = 0) uniform sampler2D heightmap;
layout(set = 1, binding = 1) uniform TerrainParams {
    vec4 origin_size;
    vec4 info;               // x altura maxima, y resolucion, z 1/resolucion, w capas
    vec4 layer_params[8];    // x repeticion (m), y rugosidad, z metal, w fuerza normal
    vec4 layer_tint[8];      // rgb tinte, a = tiene textura
    vec4 layer_extra[8];     // x = tiene normal map
    vec4 grass_floor;        // x = capa de la hierba (-1 = sin hierba), y = fuerza, z = empieza lo lejano, w = distancia maxima
    vec4 grass_floor_color;  // rgb = color del cesped visto de arriba (sRGB), a = umbral de la capa
} terrain;
layout(set = 1, binding = 2) uniform sampler2D splat0;
layout(set = 1, binding = 3) uniform sampler2D splat1;
layout(set = 1, binding = 4) uniform sampler2DArray layer_albedo;
layout(set = 1, binding = 5) uniform sampler2DArray layer_normal;

layout(location = 0) in vec3 v_world_position;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_current_clip;
layout(location = 3) in vec4 v_previous_clip;

#include "gbuffer_surface.glsl"

float heightAt(vec2 uv) {
    float res = terrain.info.y;
    vec2 texel = (clamp(uv, 0.0, 1.0) * (res - 1.0) + 0.5) / res;
    return textureLod(heightmap, texel, 0.0).r * terrain.info.x;
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float valueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1, 0)), u.x), mix(hash12(i + vec2(0, 1)), hash12(i + vec2(1, 1)), u.x),
               u.y);
}

void main() {
    // Texturas y ruido relativos a la esquina del terreno: no saltan cuando el
    // mundo se desplaza (origen flotante).
    vec2 pattern_xz = v_world_position.xz - terrain.origin_size.xz;
    // --- Normal del terreno (diferencias centrales en la textura) ---
    float res = terrain.info.y;
    float step_uv = 1.0 / (res - 1.0);
    float cell = terrain.origin_size.w * step_uv;
    float hl = heightAt(v_uv - vec2(step_uv, 0.0));
    float hr = heightAt(v_uv + vec2(step_uv, 0.0));
    float hd = heightAt(v_uv - vec2(0.0, step_uv));
    float hu = heightAt(v_uv + vec2(0.0, step_uv));
    // Se divide por la distancia que de verdad hay entre las muestras: en el
    // borde del terreno heightAt recorta la UV y la diferencia es de un solo
    // lado (una celda, no dos). Con 2 * cell fijo la pendiente salia a la
    // mitad en los bordes y se veia una costura de luz entre trozos.
    vec2 uv_low = clamp(v_uv - vec2(step_uv), 0.0, 1.0);
    vec2 uv_high = clamp(v_uv + vec2(step_uv), 0.0, 1.0);
    vec2 span = max((uv_high - uv_low) * terrain.origin_size.w, vec2(max(cell * 1e-3, 1e-6)));
    vec3 n = normalize(vec3((hl - hr) / span.x, 1.0, (hd - hu) / span.y));

    // --- Pesos de las capas ---
    vec4 w0 = texture(splat0, v_uv);
    vec4 w1 = texture(splat1, v_uv);
    float weights[8] = float[](w0.r, w0.g, w0.b, w0.a, w1.r, w1.g, w1.b, w1.a);
    int layers = int(terrain.info.w);
    // Solo cuentan las capas que existen: el peso que quede en el canal de una
    // capa borrada no debe oscurecer el resto (ni dejar el punto sin normal).
    float total = 0.0;
    for (int i = 0; i < 8; ++i) {
        if (i >= layers) weights[i] = 0.0;
        total += weights[i];
    }
    if (total < 1e-4) {
        weights[0] = 1.0;
        total = 1.0;
    }

    // Base tangente del terreno (X y Z del mundo sobre la superficie).
    vec3 t = normalize(vec3(1.0, 0.0, 0.0) - n * n.x);
    vec3 b = normalize(cross(t, n));
    vec3 albedo = vec3(0.0);
    vec3 tangent_normal = vec3(0.0);
    float roughness = 0.0;
    float metallic = 0.0;
    // Derivadas del patron FUERA del bucle: dentro, las capas que se saltan
    // (peso casi 0) dejan a los vecinos del cuadro 2x2 sin derivadas y el mip
    // de texture() salia indefinido justo en la mezcla entre capas (brillos y
    // costuras). Cada capa usa textureGrad con estas derivadas a su escala.
    vec2 pattern_dx = dFdx(pattern_xz);
    vec2 pattern_dy = dFdy(pattern_xz);
    for (int i = 0; i < 8; ++i) {
        float w = weights[i] / total;
        if (w < 0.002 || i >= layers) continue;
        vec4 p = terrain.layer_params[i];
        vec2 uv = pattern_xz / p.x;
        vec2 uv_dx = pattern_dx / p.x;
        vec2 uv_dy = pattern_dy / p.x;
        vec3 color;
        if (terrain.layer_tint[i].a > 0.5) {
            // Dos escalas mezcladas: la repeticion de la textura no se nota.
            vec3 near_color = textureGrad(layer_albedo, vec3(uv, float(i)), uv_dx, uv_dy).rgb;
            vec3 far_color = textureGrad(layer_albedo, vec3(uv * 0.23 + 0.37, float(i)), uv_dx * 0.23, uv_dy * 0.23).rgb;
            float mix_amount = smoothstep(0.3, 0.7, valueNoise(pattern_xz * 0.05 + float(i) * 13.0));
            color = mix(near_color, far_color, mix_amount * 0.45);
        } else {
            // Sin textura: su color con una variacion suave (manchas).
            float variation = valueNoise(pattern_xz * 0.35) * 0.6 + valueNoise(pattern_xz * 2.1) * 0.4;
            color = vec3(0.85 + 0.3 * variation);
        }
        albedo += color * terrain.layer_tint[i].rgb * w;
        vec3 tn = vec3(0.0, 0.0, 1.0);
        if (terrain.layer_extra[i].x > 0.5) {
            vec2 xy = textureGrad(layer_normal, vec3(uv, float(i)), uv_dx, uv_dy).xy * 2.0 - 1.0;
            xy *= p.w;
            tn = vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
        }
        tangent_normal += tn * w;
        roughness += p.y * w;
        metallic += p.z * w;
    }
    // Sin ninguna capa (terreno recien creado) la suma es 0: normal plana.
    tangent_normal = dot(tangent_normal, tangent_normal) > 1e-8 ? normalize(tangent_normal) : vec3(0.0, 0.0, 1.0);
    vec3 normal = normalize(mat3(t, b, n) * vec3(tangent_normal.x, -tangent_normal.y, tangent_normal.z));

    // --- Suelo del cesped: donde crece la hierba, el color de la hierba ---
    // De cerca, el de debajo de las briznas (oscuro y ocluido); de lejos, el
    // de las puntas: la hierba lejana (pocas briznas) se funde con el suelo.
    float occlusion = 1.0;
    if (terrain.grass_floor.x >= 0.0) {
        int grass_layer = clamp(int(terrain.grass_floor.x + 0.5), 0, 7);
        float threshold = terrain.grass_floor_color.a;
        float grass_weight = smoothstep(threshold, min(threshold + 0.35, 1.0), weights[grass_layer] / total) *
                             terrain.grass_floor.y;
        // Solo donde hay briznas: se apaga igual que ellas (z = empieza el
        // desvanecido, w = distancia maxima). Antes seguia hasta el borde de
        // la tesela: un cuadrado "con mas hierba" lejos y un circulo oscuro cerca.
        float d = distance(camera.position.xz, v_world_position.xz);
        float fade_start = terrain.grass_floor.z;
        float far_mix = smoothstep(fade_start * 0.25, fade_start, d);
        grass_weight *= 1.0 - smoothstep(fade_start, terrain.grass_floor.w, d);
        vec3 top = terrain.grass_floor_color.rgb;
        vec3 under = top * 0.7;
        albedo = mix(albedo, mix(under, top, far_mix), grass_weight);
        roughness = mix(roughness, 0.85, grass_weight);
        occlusion = mix(1.0, mix(0.75, 0.95, far_mix), grass_weight);
    }

    writeSurface(vec4(clamp(albedo, 0.0, 1.0), 1.0), n, normal, tangent_normal, n, clamp(metallic, 0.0, 1.0),
                 clamp(roughness, 0.04, 1.0), occlusion, vec3(0.0), 0.04, v_world_position);
    writeVelocity(v_current_clip, v_previous_clip);
}
