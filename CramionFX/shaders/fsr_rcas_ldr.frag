#version 450
#extension GL_GOOGLE_include_directive : require

// FSR 1 RCAS sobre la imagen LDR escalada por fsr_easu_ldr.frag (ya en [0, 1]:
// sin la compresion HDR de fsr_rcas.frag). `bypass` = 1 copia sin tocar.

layout(set = 0, binding = 0) uniform sampler2D source;

layout(push_constant) uniform PushConstants {
    uvec4 con;
    uint bypass;
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"

#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) {
    // RCAS lee los vecinos (p +- 1): se recorta al borde de la imagen.
    p = clamp(p, ASU2(0), ASU2(textureSize(source, 0)) - ASU2(1));
    return vec4(texelFetch(source, p, 0).rgb, 1.0);
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "ffx_fsr1.h"

void main() {
    if (push.bypass != 0u) {
        out_color = vec4(texelFetch(source, ivec2(gl_FragCoord.xy), 0).rgb, 1.0);
        return;
    }
    AF1 r, g, b;
    FsrRcasF(r, g, b, AU2(gl_FragCoord.xy), push.con);
    vec3 color = vec3(r, g, b);
    // En zonas negras el lobulo de RCAS puede dar NaN: el pixel sin nitidez.
    if (any(isnan(color))) color = texelFetch(source, ivec2(gl_FragCoord.xy), 0).rgb;
    out_color = vec4(clamp(color, vec3(0.0), vec3(1.0)), 1.0);
}
