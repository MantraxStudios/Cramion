#version 450
#extension GL_GOOGLE_include_directive : require

// Liquidos: grosor (cuerda de cada esfera, sumado) y propiedades del
// material ponderadas por grosor (mezclas de liquidos), a media resolucion.

#include "fluid_render.glsl"

layout(location = 0) in vec2 v_corner;
layout(location = 1) in vec3 v_center;
layout(location = 2) flat in uint v_material;
layout(location = 3) in float v_foam;
layout(location = 4) in float v_radius;

layout(location = 0) out vec4 out_absorb;    // rgb absorcion * grosor, a grosor
layout(location = 1) out vec4 out_scatter;   // rgb color difuso * grosor, a espuma * grosor
layout(location = 2) out vec4 out_emission;  // rgb emision * grosor, a rugosidad * grosor

void main() {
    // Media resolucion: gl_FragCoord * 2 en la imagen completa.
    vec2 uv = gl_FragCoord.xy * fr.viewport.zw * 2.0;
    vec3 ray = normalize(viewPosition(uv, 1.0));
    float b = dot(ray, v_center);
    float c = dot(v_center, v_center) - v_radius * v_radius;
    float disc = b * b - c;
    if (disc < 0.0) discard;
    float s = sqrt(disc);
    float front = -(ray * (b - s)).z;
    if (front <= 0.0) discard;
    if (front > sceneLinearDepth(uv)) discard;
    // Cuerda de la esfera (lo que atraviesa el rayo).
    float chord = 2.0 * s * fr.params.y;
    FluidMaterialGpu m = fr.materials[v_material];
    out_absorb = vec4(m.absorb_visc.rgb * chord, chord);
    out_scatter = vec4(m.scatter_cohes.rgb * chord, clamp(v_foam, 0.0, 1.0) * chord);
    out_emission = vec4(m.emission_vort.rgb * chord, m.params.y * chord);
}
