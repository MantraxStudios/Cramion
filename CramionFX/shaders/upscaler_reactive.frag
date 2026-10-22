#version 450

// Mascara reactiva para FSR 3 (reactive y transparencyAndComposition): el
// alfa de la imagen HDR, que en la vista principal es la opacidad acumulada
// de los transparentes (VFX Graph, particulas, sprites, lluvia y mallas
// .crshader transparentes; lighting.frag lo deja a 0 en lo opaco). Sin ella
// FSR 3 tomaba el movimiento del fondo para los efectos y dejaba estela.
// El TAA del motor la lee directamente del alfa (taa.frag).

layout(set = 0, binding = 0) uniform sampler2D scene_color;

layout(push_constant) uniform PushConstants {
    vec4 params;  // x = fuerza (render.taa.ReactiveStrength), y = tope (FSR recomienda < 1)
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out float out_reactive;

void main() {
    float coverage = texelFetch(scene_color, ivec2(gl_FragCoord.xy), 0).a;
    out_reactive = clamp(coverage * push.params.x, 0.0, push.params.y);
}
