#version 450

// Sombra de los .crshader con vertex() (surface_shadow.vert): el recorte por
// alfa de skinned_shadow.frag con la push constant de GpuSurfaceShadowPush.
// Lo opaco no descarta nada (bit 1 de flags apagado).

layout(set = 1, binding = 0) uniform sampler2D albedo_map;

// Debe coincidir con GpuSurfaceShadowPush (y con surface_shadow.vert).
layout(push_constant) uniform PushConstants {
    mat4 light_view_projection;
    uint bone_offset;
    float alpha_factor;  // alfa del color base del material
    uint pick_id;
    uint flags;          // 1 = recorte por alfa; bits 8-15 = opacidad de la instancia (255 = opaco)
    vec4 model_rows[3];
} push;

layout(location = 0) in vec2 v_uv;

void main() {
    // La UV ya trae el tiling del material (surface_shadow.vert).
    if ((push.flags & 1u) != 0u && texture(albedo_map, v_uv).a * push.alpha_factor < 0.5) {
        discard;
    }
    // Opacidad por instancia < 1 (MeshRenderer.alpha): el tramado de
    // skinned_shadow.frag.
    float opacity = float((push.flags >> 8u) & 0xFFu) / 255.0;
    if (opacity < 0.999) {
        float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
        if (noise >= opacity) discard;
    }
}
