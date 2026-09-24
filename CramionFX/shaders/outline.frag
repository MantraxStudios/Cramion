#version 450

// Contorno de seleccion, como el de Unity: una linea naranja de ~2 px por
// fuera de la silueta de lo seleccionado, intensa donde el objeto se ve y
// tenue donde algo lo tapa. Se mezcla (alfa) sobre la imagen ya compuesta.
//
// La mascara (outline_mask.frag) tiene R = silueta entera y G = parte
// visible. Un pixel es borde si esta fuera de la silueta y tiene cerca algo
// dentro: la cobertura del vecindario (disco de radio ~2 px) da un borde
// suavizado.

layout(set = 0, binding = 0) uniform sampler2D mask;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

const vec3 kOutlineColor = vec3(1.0, 0.604, 0.122);  // #FF9A1F
const float kHiddenAlpha = 0.35;

void main() {
    ivec2 size = textureSize(mask, 0);
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec2 center = texelFetch(mask, pixel, 0).rg;

    // Dentro de la silueta no hay contorno (el objeto se ve tal cual).
    if (center.r > 0.5) {
        out_color = vec4(0.0);
        return;
    }

    // Cobertura del vecindario, ponderada por la distancia: 1 px -> linea
    // llena, 2.5 px -> desvanecida.
    float inside = 0.0;
    float visible = 0.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            float d = length(vec2(x, y));
            if (d > 2.6 || (x == 0 && y == 0)) {
                continue;
            }
            float w = clamp(2.6 - d, 0.0, 1.0);
            vec2 m = texelFetch(mask, clamp(pixel + ivec2(x, y), ivec2(0), size - 1), 0).rg;
            inside = max(inside, m.r * w);
            visible = max(visible, m.g * w);
        }
    }
    if (inside <= 0.0) {
        out_color = vec4(0.0);
        return;
    }
    float alpha = inside * mix(kHiddenAlpha, 1.0, visible);
    // La imagen compuesta ya esta en sRGB (8 bits): el color va tal cual.
    out_color = vec4(kOutlineColor, alpha);
}
