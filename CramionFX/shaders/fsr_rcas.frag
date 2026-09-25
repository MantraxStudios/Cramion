#version 450
#extension GL_GOOGLE_include_directive : require

// AMD FidelityFX Super Resolution 1, RCAS: nitidez adaptativa tras el
// escalado (FSR 1) o el TAA. Igual que el EASU, sobre la imagen HDR
// comprimida con x / (1 + x). `bypass` = 1 copia sin tocar.

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
    vec4 c = texelFetch(source, p, 0);
    return vec4(c.rgb / (1.0 + c.rgb), 1.0);
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
    vec3 color = clamp(vec3(r, g, b), vec3(0.0), vec3(0.999));
    out_color = vec4(color / (1.0 - color), 1.0);
}
