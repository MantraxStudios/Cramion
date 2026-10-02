#version 450
#extension GL_GOOGLE_include_directive : require

// VFX Graph: color de cada particula sobre la imagen HDR. Textura (o disco
// suave), intensidad HDR, luz del frame ("lit"), recorte de alfa y
// particulas suaves (se desvanecen al cruzar la escena). La mezcla la pone
// el pipeline: aditiva, alfa o premultiplicada.

#include "vfx_common.glsl"

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec2 v_corner;
layout(location = 3) in float v_view_depth;

layout(location = 0) out vec4 out_color;

void main() {
    uint slot = push.slot;
    vec4 color = v_color;
    uint texture_slot = instances[slot].flags.w;
    if (texture_slot != VFX_NO_TEXTURE && texture_slot < 16u) {
        color *= texture(vfx_textures[texture_slot], v_uv);
    } else {
        float r2 = dot(v_corner, v_corner);
        if (r2 >= 1.0) discard;
        float falloff = 1.0 - r2;
        color.a *= falloff * falloff;
    }
    if (color.a <= max(instances[slot].speed_range.z, 0.002)) discard;

    // Particulas suaves: contra la profundidad lineal de la escena.
    float soft = instances[slot].output0.z;
    if (soft > 0.0 && frame.camera.w > 0.5) {
        float depth = texelFetch(scene_depth, ivec2(gl_FragCoord.xy), 0).r;
        if (depth < 1.0) {
            vec2 ndc = gl_FragCoord.xy * frame.viewport.zw * 2.0 - 1.0;
            vec4 v = frame.inverse_projection * vec4(ndc, depth, 1.0);
            float scene_depth_linear = abs(v.z / v.w);
            color.a *= clamp((scene_depth_linear - v_view_depth) / soft, 0.0, 1.0);
        }
    }

    vec3 rgb = color.rgb * max(instances[slot].output0.w, 0.0);
    if ((instances[slot].flags.z & VFX_LIT) != 0u) rgb *= frame.light.rgb;
    uint blend = uint(instances[slot].output0.y + 0.5);
    if (blend == 0u) {
        // Aditiva: suma color * opacidad.
        out_color = vec4(rgb * color.a, 0.0);
    } else if (blend == 2u) {
        // Premultiplicada: el color ya lleva la opacidad.
        out_color = vec4(rgb * color.a, color.a);
    } else {
        out_color = vec4(rgb, color.a);
    }
}
