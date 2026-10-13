#version 450

// Video en un plano de la camara (VideoPlayer en modo Camera Far/Near Plane),
// sobre la imagen ya compuesta (LDR, con gamma), como la UI en el mundo: el
// video no pasa por la exposicion ni el tonemap.
//   - Cercano: encima de todo, con su opacidad.
//   - Lejano: solo donde no hay geometria (profundidad = 1, el cielo).
// Se dibuja con lighting.vert (triangulo a pantalla completa).

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0) uniform sampler2D u_video;  // vista UNORM (ya con gamma)
layout(set = 0, binding = 1) uniform sampler2D u_depth;  // profundidad de la escena (cualquier resolucion)

layout(push_constant) uniform Push {
    vec4 rect;    // xy = origen del contenido (UV de la vista), zw = tamano
    vec4 params;  // x: opacidad; y: 1 = plano lejano
} pc;

void main() {
    vec2 t = (v_uv - pc.rect.xy) / max(pc.rect.zw, vec2(1.0e-6));
    if (t.x < 0.0 || t.y < 0.0 || t.x > 1.0 || t.y > 1.0) discard;
    if (pc.params.y > 0.5) {
        // UV normalizadas: vale igual con escalado (FSR/DLSS/TAA), donde la
        // profundidad tiene la resolucion interna y esta imagen la de salida.
        ivec2 size = textureSize(u_depth, 0);
        ivec2 texel = clamp(ivec2(v_uv * vec2(size)), ivec2(0), size - 1);
        if (texelFetch(u_depth, texel, 0).r < 1.0) discard;
    }
    vec4 c = texture(u_video, t);
    o_color = vec4(c.rgb, clamp(c.a * pc.params.x, 0.0, 1.0));
}
