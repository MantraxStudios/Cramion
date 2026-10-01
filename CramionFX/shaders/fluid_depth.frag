#version 450
#extension GL_GOOGLE_include_directive : require

// Liquidos: profundidad de la cara visible de cada esfera (la mas cercana
// gana con la prueba de profundidad). Se descarta lo que tapa la escena.

#include "fluid_render.glsl"

layout(location = 0) in vec2 v_corner;
layout(location = 1) in vec3 v_center;
layout(location = 2) flat in uint v_material;
layout(location = 3) in float v_foam;
layout(location = 4) in float v_radius;

layout(location = 0) out float out_depth;

void main() {
    // Rayo del pixel (desde la camara) contra la esfera: la profundidad
    // exacta de su cara visible.
    vec2 uv = gl_FragCoord.xy * fr.viewport.zw;
    vec3 ray = normalize(viewPosition(uv, 1.0));
    float b = dot(ray, v_center);
    float c = dot(v_center, v_center) - v_radius * v_radius;
    float disc = b * b - c;
    if (disc < 0.0) discard;
    float t = b - sqrt(disc);
    vec3 hit = ray * t;
    float depth = -hit.z;
    if (depth <= 0.0) discard;
    if (depth > sceneLinearDepth(uv)) discard;
    out_depth = depth;
    vec4 clip = fr.projection * vec4(hit, 1.0);
    gl_FragDepth = clamp(clip.z / clip.w, 0.0, 1.0);
}
