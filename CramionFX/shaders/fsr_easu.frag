#version 450
#extension GL_GOOGLE_include_directive : require

// AMD FidelityFX Super Resolution 1, EASU: escalado espacial de la imagen HDR
// (resolucion interna) a la de pantalla. FSR 1 espera la imagen en un espacio
// perceptual: se le da comprimida con x / (1 + x) (reversible) y se deshace.

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
AF4 FsrEasuRF(AF2 p) { AF4 c = textureGather(source, p, 0); return c / (1.0 + c); }
AF4 FsrEasuGF(AF2 p) { AF4 c = textureGather(source, p, 1); return c / (1.0 + c); }
AF4 FsrEasuBF(AF2 p) { AF4 c = textureGather(source, p, 2); return c / (1.0 + c); }
#include "ffx_fsr1.h"

void main() {
    AF3 color;
    FsrEasuF(color, AU2(gl_FragCoord.xy), push.con0, push.con1, push.con2, push.con3);
    color = clamp(color, vec3(0.0), vec3(0.999));
    out_color = vec4(color / (1.0 - color), 1.0);
}
