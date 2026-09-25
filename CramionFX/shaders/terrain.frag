#version 450
#extension GL_GOOGLE_include_directive : require

// Terreno en el G-buffer: normal de la textura de alturas (por pixel), hasta
// 8 capas mezcladas por los mapas de pesos (splat), cada una con su color o
// textura (con una segunda escala para romper la repeticion), normal map,
// rugosidad y metal. Despues, lluvia, charcos, humedad y decals como
// cualquier suelo (gbuffer_surface.glsl).

layout(set = 1, binding = 0) uniform sampler2D heightmap;
layout(set = 1, binding = 1) uniform TerrainParams {
    vec4 origin_size;
    vec4 info;               // x altura maxima, y resolucion, z 1/resolucion, w capas
    vec4 layer_params[8];    // x repeticion (m), y rugosidad, z metal, w fuerza normal
    vec4 layer_tint[8];      // rgb tinte, a = tiene textura
    vec4 layer_extra[8];     // x = tiene normal map
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
    // --- Normal del terreno (diferencias centrales en la textura) ---
    float res = terrain.info.y;
    float step_uv = 1.0 / (res - 1.0);
    float cell = terrain.origin_size.w * step_uv;
    float hl = heightAt(v_uv - vec2(step_uv, 0.0));
    float hr = heightAt(v_uv + vec2(step_uv, 0.0));
    float hd = heightAt(v_uv - vec2(0.0, step_uv));
    float hu = heightAt(v_uv + vec2(0.0, step_uv));
    vec3 n = normalize(vec3(hl - hr, 2.0 * cell, hd - hu));

    // --- Pesos de las capas ---
    vec4 w0 = texture(splat0, v_uv);
    vec4 w1 = texture(splat1, v_uv);
    float weights[8] = float[](w0.r, w0.g, w0.b, w0.a, w1.r, w1.g, w1.b, w1.a);
    float total = 0.0;
    for (int i = 0; i < 8; ++i) total += weights[i];
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
    int layers = int(terrain.info.w);
    for (int i = 0; i < 8; ++i) {
        float w = weights[i] / total;
        if (w < 0.002 || i >= layers) continue;
        vec4 p = terrain.layer_params[i];
        vec2 uv = v_world_position.xz / p.x;
        vec3 color;
        if (terrain.layer_tint[i].a > 0.5) {
            // Dos escalas mezcladas: la repeticion de la textura no se nota.
            vec3 near_color = texture(layer_albedo, vec3(uv, float(i))).rgb;
            vec3 far_color = texture(layer_albedo, vec3(uv * 0.23 + 0.37, float(i))).rgb;
            float mix_amount = smoothstep(0.3, 0.7, valueNoise(v_world_position.xz * 0.05 + float(i) * 13.0));
            color = mix(near_color, far_color, mix_amount * 0.45);
        } else {
            // Sin textura: su color con una variacion suave (manchas).
            float variation = valueNoise(v_world_position.xz * 0.35) * 0.6 + valueNoise(v_world_position.xz * 2.1) * 0.4;
            color = vec3(0.85 + 0.3 * variation);
        }
        albedo += color * terrain.layer_tint[i].rgb * w;
        vec3 tn = vec3(0.0, 0.0, 1.0);
        if (terrain.layer_extra[i].x > 0.5) {
            vec2 xy = texture(layer_normal, vec3(uv, float(i))).xy * 2.0 - 1.0;
            xy *= p.w;
            tn = vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
        }
        tangent_normal += tn * w;
        roughness += p.y * w;
        metallic += p.z * w;
    }
    tangent_normal = normalize(tangent_normal);
    vec3 normal = normalize(mat3(t, b, n) * vec3(tangent_normal.x, -tangent_normal.y, tangent_normal.z));

    writeSurface(vec4(clamp(albedo, 0.0, 1.0), 1.0), n, normal, tangent_normal, n, clamp(metallic, 0.0, 1.0),
                 clamp(roughness, 0.04, 1.0), 1.0, vec3(0.0), 0.04, v_world_position);
    writeVelocity(v_current_clip, v_previous_clip);
}
