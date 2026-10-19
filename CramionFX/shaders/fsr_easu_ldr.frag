#version 450
#extension GL_GOOGLE_include_directive : require

// FSR 1 EASU sobre la imagen ya compuesta (LDR, 8 bits, espacio perceptual):
// el orden que recomienda AMD (tonemap y antialiasing a la resolucion interna,
// despues EASU y RCAS). Sin la compresion x / (1 + x) de fsr_easu.frag: la
// imagen ya esta en [0, 1]. Ver VulkanRenderer::recordEarlyPostChain.

layout(set = 0, binding = 0) uniform sampler2D source;

layout(push_constant) uniform PushConstants {
    uvec4 con0;
    uvec4 con1;
    uvec4 con2;
    uvec4 con3;
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"

#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return textureGather(source, p, 0); }
AF4 FsrEasuGF(AF2 p) { return textureGather(source, p, 1); }
AF4 FsrEasuBF(AF2 p) { return textureGather(source, p, 2); }
#include "ffx_fsr1.h"

void main() {
    AF3 color;
    FsrEasuF(color, AU2(gl_FragCoord.xy), push.con0, push.con1, push.con2, push.con3);
    out_color = vec4(clamp(color, vec3(0.0), vec3(1.0)), 1.0);
}
