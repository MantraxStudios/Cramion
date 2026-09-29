#version 450

// Vectores de movimiento para FSR 3 y DLSS: los del G-buffer (UV actual - UV
// anterior, sin jitter) y, donde no hay geometria (el cielo), los que da solo
// el movimiento de la camara. Sin esto el cielo no se movia al girar y los
// escaladores dejaban estelas en los bordes contra el cielo.

layout(set = 0, binding = 0) uniform sampler2D scene_depth;
layout(set = 0, binding = 1) uniform sampler2D velocity_map;  // UV actual - UV anterior

layout(push_constant) uniform PushConstants {
    mat4 reproject;  // NDC sin jitter de este frame -> recorte del anterior
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec2 out_velocity;

void main() {
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const float depth = texelFetch(scene_depth, pixel, 0).r;
    if (depth >= 1.0) {
        vec4 previous = push.reproject * vec4(v_uv * 2.0 - 1.0, 1.0, 1.0);
        out_velocity = v_uv - (previous.xy / max(previous.w, 1e-6) * 0.5 + 0.5);
    } else {
        out_velocity = texelFetch(velocity_map, pixel, 0).xy;
    }
}
