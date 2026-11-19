#version 450

// Contornos de juego (componente Outline, RM-capas) sobre la imagen final.
// La mascara (game_outline_mask.frag) guarda en R el estilo de la silueta
// entera y en G el de la parte que se ve (0 = nada). Por estilo (push
// constants, ver scene::GameOutlines::pack):
//   word0: RGBA8 (alfa con el pulso ya aplicado)
//   word1: grosor en 1/16 px (bits 0-7), modo (bits 8-9: 0 siempre,
//          1 solo tapado, 2 solo visible), relleno de lo tapado (bits 16-23)
// Hueco 15: x = cuantos estilos, y = radio de busqueda (px).
//
// Un pixel fuera de la region de un estilo con un vecino dentro a menos de su
// grosor es contorno; la cobertura cae en el ultimo medio pixel (suavizado).
// Con el modo "tapado" la region es la parte de la silueta que no se ve
// (silueta a traves de paredes, como en un MOBA) y se puede rellenar.

layout(set = 0, binding = 0) uniform sampler2D mask;

layout(push_constant) uniform PushConstants {
    uvec2 styles[16];
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

const float kPi = 3.14159265;

uvec2 idsAt(ivec2 p, ivec2 size) {
    vec2 m = texelFetch(mask, clamp(p, ivec2(0), size - 1), 0).rg;
    return uvec2(m * 255.0 + 0.5);
}

uint styleMode(uint s) { return (push.styles[s - 1u].y >> 8) & 3u; }
float styleWidth(uint s) { return float(push.styles[s - 1u].y & 255u) / 16.0; }
vec4 styleColor(uint s) { return unpackUnorm4x8(push.styles[s - 1u].x); }
float styleFill(uint s) { return float((push.styles[s - 1u].y >> 16) & 255u) / 255.0; }

// El estilo al que pertenece el pixel segun el modo de `s` (0 = ninguno):
// siempre -> silueta entera; visible -> lo visto; tapado -> lo no visto.
bool inRegion(uvec2 ids, uint s) {
    uint mode = styleMode(s);
    if (mode == 2u) return ids.g == s;
    if (mode == 1u) return ids.r == s && ids.g != s;
    return ids.r == s;
}

void main() {
    ivec2 size = textureSize(mask, 0);
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    uint count = min(push.styles[15].x, 15u);
    int radius = int(min(push.styles[15].y, 16u));
    if (count == 0u) {
        out_color = vec4(0.0);
        return;
    }
    uvec2 center = idsAt(pixel, size);

    // Relleno de la parte tapada (modos siempre y tapado).
    vec4 result = vec4(0.0);
    if (center.r != 0u && center.r <= count && center.g != center.r && styleMode(center.r) != 2u) {
        float fill = styleFill(center.r);
        if (fill > 0.0) {
            vec4 c = styleColor(center.r);
            result = vec4(c.rgb, c.a * fill);
        }
    }

    // Contorno: el estilo mas cercano cuyo borde llegue a este pixel.
    float best = 0.0;
    vec4 best_color = vec4(0.0);
    for (int r = 1; r <= radius; ++r) {
        int dirs = r <= 2 ? 8 : 16;
        for (int k = 0; k < dirs; ++k) {
            float a = (float(k) + (r % 2 == 0 ? 0.5 : 0.0)) * (2.0 * kPi / float(dirs));
            ivec2 offset = ivec2(round(vec2(cos(a), sin(a)) * float(r)));
            uvec2 ids = idsAt(pixel + offset, size);
            // Los dos estilos que puede haber en el vecino.
            for (int c = 0; c < 2; ++c) {
                uint s = c == 0 ? ids.r : ids.g;
                if (s == 0u || s > count) continue;
                if (!inRegion(ids, s) || inRegion(center, s)) continue;
                // Tapado: solo por fuera de la silueta entera (no sobre lo visto).
                if (styleMode(s) == 1u && center.r == s) continue;
                float d = length(vec2(offset));
                float coverage = clamp(styleWidth(s) + 0.5 - d, 0.0, 1.0);
                if (coverage > best) {
                    best = coverage;
                    best_color = styleColor(s);
                }
            }
        }
        if (best >= 1.0) break;
    }
    if (best > 0.0) {
        float alpha = best_color.a * best;
        // Encima del relleno.
        result = vec4(mix(result.rgb, best_color.rgb, alpha / max(alpha + result.a * (1.0 - alpha), 1e-4)),
                      alpha + result.a * (1.0 - alpha));
    }
    // La imagen compuesta ya esta en sRGB (8 bits): el color va tal cual.
    out_color = result;
}
