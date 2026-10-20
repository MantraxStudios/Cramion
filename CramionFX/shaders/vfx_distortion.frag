#version 450
#extension GL_GOOGLE_include_directive : require

// VFX Graph, mezcla Distorsion (VfxPass::recordDistortion): no pinta; suma al
// buffer de distorsion de la escena (RG16F, el de ParticlePass, mezcla
// aditiva) cuanto se desplaza la lectura de la escena en este pixel, en UV de
// pantalla. composite.frag lo aplica en su unica lectura de la escena.
//
// Fuerza: px a 1080p (VfxInstance.emissive.w, hasta 5) por el alfa de la
// particula (color en la vida...). Direccion:
//   - con textura: un normal map (RG, +Y arriba) o, con mesh0.w = 1, un mapa
//     de alturas (su pendiente). Pasa de la UV de la textura a la pantalla con
//     las derivadas de la UV: vale con cualquier giro, en cintas y en mallas;
//   - sin textura: quads y cintas, una lente blanda que empuja desde el centro
//     (perfil r (1 - r^2): 0 en el centro y en el borde, sin cortes); mallas,
//     la normal en la vista (como un cristal: nada de frente, todo en el borde).

#include "vfx_common.glsl"

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec2 v_corner;
layout(location = 3) in float v_view_depth;
layout(location = 4) in vec3 v_normal;
layout(location = 5) flat in uint v_mode;  // 0 quad, 1/2 cinta, 3 malla

layout(location = 0) out vec2 out_offset;

// Una direccion en la textura (x hacia +u, y hacia -v) a pixeles de la
// pantalla, con su largo: J = (dUV/dx, dUV/dy) y pixel = J^-1 * uv.
vec2 textureToScreen(vec2 t, vec2 ddx, vec2 ddy) {
    float det = ddx.x * ddy.y - ddy.x * ddx.y;
    if (abs(det) < 1e-14) return vec2(0.0);
    vec2 d = vec2(t.x, -t.y);
    vec2 p = vec2(ddy.y * d.x - ddy.x * d.y, -ddx.y * d.x + ddx.x * d.y) / det;
    float len = length(p);
    return len > 1e-8 ? p / len * min(length(t), 1.0) : vec2(0.0);
}

void main() {
    uint slot = push.slot;
    // Derivadas antes de cualquier discard.
    vec2 ddx = dFdx(v_uv);
    vec2 ddy = dFdy(v_uv);
    float pixels = clamp(instances[slot].emissive.w, 0.0, 5.0);
    float alpha = v_color.a * instances[slot].tint.a;
    vec2 dir = vec2(0.0);  // en pixeles de pantalla (y hacia abajo), largo 0..1

    uint texture_slot = instances[slot].flags.w;
    if (texture_slot != VFX_NO_TEXTURE && texture_slot < VFX_TEXTURE_COUNT) {
        vec2 t;
        if (instances[slot].mesh0.w > 0.5) {
            // Mapa de alturas: hacia abajo de la pendiente.
            vec2 e = 1.0 / max(vec2(textureSize(vfx_textures[texture_slot], 0)), vec2(1.0));
            float hx = texture(vfx_textures[texture_slot], v_uv + vec2(e.x, 0.0)).r -
                       texture(vfx_textures[texture_slot], v_uv - vec2(e.x, 0.0)).r;
            float hy = texture(vfx_textures[texture_slot], v_uv + vec2(0.0, e.y)).r -
                       texture(vfx_textures[texture_slot], v_uv - vec2(0.0, e.y)).r;
            t = vec2(-hx, hy) * 4.0;
            alpha *= texture(vfx_textures[texture_slot], v_uv).a;
        } else {
            // Normal map (la textura es RGBA8 UNORM: sin pasar a lineal).
            vec4 n = texture(vfx_textures[texture_slot], v_uv);
            t = n.rg * 2.0 - 1.0;
            alpha *= n.a;
        }
        dir = textureToScreen(t, ddx, ddy);
    } else if (v_mode == 3u) {
        vec3 n = normalize(v_normal);
        if (!gl_FrontFacing) n = -n;
        vec3 nv = mat3(frame.view) * n;
        dir = vec2(nv.x, -nv.y);  // vista +Y arriba; pantalla +Y abajo
    } else {
        // Lente blanda: quads, del centro hacia fuera; cintas, a lo ancho.
        vec2 c = v_mode == 0u ? v_corner : vec2(0.0, -v_corner.y);
        float r2 = dot(c, c);
        if (r2 >= 1.0) discard;
        float r = sqrt(r2);
        float profile = r * (1.0 - r2) * 2.6;  // pico de r (1 - r^2) = 0.385
        dir = textureToScreen(r > 1e-5 ? c / r * profile : vec2(0.0), ddx, ddy);
    }
    if (alpha <= 0.002) discard;
    // px a 1080p -> UV de pantalla (la x por el aspecto).
    out_offset = dir * (alpha * pixels / 1080.0) * vec2(frame.viewport.y * frame.viewport.z, 1.0);
}
