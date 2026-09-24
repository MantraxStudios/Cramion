#version 450

// Geometria de ayuda en 3D: OverlayPass. Se dibuja dos veces con prueba de
// profundidad contra la escena: lo visible con su opacidad y lo tapado con
// `alpha_scale` (rayos X), para ver donde atraviesa un objeto.

layout(push_constant) uniform PushConstants {
    float half_width;   // semiancho de las lineas en pixeles
    float alpha_scale;  // 1 = visible, occluded_alpha = tapado
} push;

layout(location = 0) in vec4 v_color;
layout(location = 1) in float v_edge;

layout(location = 0) out vec4 out_color;

void main() {
    // Antialias del borde de las lineas: cobertura del pixel (el quad se
    // expandio medio pixel de mas). En los triangulos v_edge = 0.
    float coverage = clamp(push.half_width + 0.5 - abs(v_edge), 0.0, 1.0);
    float alpha = v_color.a * coverage * push.alpha_scale;
    if (alpha <= 0.002) {
        discard;
    }
    out_color = vec4(v_color.rgb, alpha);
}
