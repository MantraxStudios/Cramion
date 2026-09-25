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
// NaN -> 0, negativos -> 0, +inf -> maximo del half float (ver fsr_easu.frag).
vec3 sanitize(vec3 c) {
    return clamp(mix(c, vec3(0.0), isnan(c)), vec3(0.0), vec3(65504.0));
}
AF4 FsrRcasLoadF(ASU2 p) {
    vec3 c = sanitize(texelFetch(source, p, 0).rgb);
    return vec4(c / (1.0 + c), 1.0);
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "ffx_fsr1.h"

void main() {
    if (push.bypass != 0u) {
        out_color = vec4(sanitize(texelFetch(source, ivec2(gl_FragCoord.xy), 0).rgb), 1.0);
        return;
    }
    AF1 r, g, b;
    FsrRcasF(r, g, b, AU2(gl_FragCoord.xy), push.con);
    // En zonas negras el lobulo de RCAS hace 0 * rcp(0); segun el driver, max()
    // lo descarta o devuelve NaN. Si sale NaN, el pixel sin nitidez.
    vec3 color = vec3(r, g, b);
    if (any(isnan(color))) {
        vec3 c = sanitize(texelFetch(source, ivec2(gl_FragCoord.xy), 0).rgb);
        color = c / (1.0 + c);
    }
    color = clamp(color, vec3(0.0), vec3(0.999));
    out_color = vec4(color / (1.0 - color), 1.0);
}
