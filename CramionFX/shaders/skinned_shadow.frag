#version 450

// Sombras de los modelos con recorte por alfa: lo que el G-buffer descarta
// (hojas, rejas, pelo) tampoco debe proyectar sombra. Sin esto cada hoja
// haria la sombra de su rectangulo entero.

layout(set = 1, binding = 0) uniform sampler2D albedo_map;

// Repeticion (xy) y desplazamiento (zw) de las UV del material (tiling del
// .crmat; ver skinned.frag).
layout(set = 1, binding = 9) uniform MaterialUv {
    vec4 material_uv;
};
// Debe coincidir con GpuSkinnedShadowPush (y con skinned_shadow.vert/.tesc/.tese).
layout(push_constant) uniform PushConstants {
    mat4 light_model_view_projection;
    uint bone_offset;
    float height;
    float max_factor;
    uint flags;
    vec4 camera_model;
    vec4 model_scale;
    float alpha_factor;  // alfa del color base del material (< 0: sin recorte por alfa)
    float opacity;       // opacidad de la instancia (MeshRenderer.alpha)
} push;

layout(location = 0) in vec2 v_uv;

void main() {
    // Textura x factor del material, igual que skinned.frag/surface.frag: un
    // material medio transparente por su color recorta igual en la sombra.
    if (push.alpha_factor >= 0.0 &&
        texture(albedo_map, v_uv * material_uv.xy + material_uv.zw).a * push.alpha_factor < 0.5) {
        discard;
    }
    // Opacidad por instancia < 1: el mismo ruido de gradiente entrelazado que
    // instanceDissolve (lightmap_common.glsl) en los texeles del mapa; el
    // filtrado de la sombra (PCF) lo convierte en una sombra mas clara.
    if (push.opacity < 0.999) {
        float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
        if (noise >= push.opacity) discard;
    }
}
