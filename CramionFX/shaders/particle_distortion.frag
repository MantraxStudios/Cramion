#version 450

// Particulas de distorsion (ParticlePass::recordDistortion): no pintan, suman
// al buffer de distorsion (RG16F, mezcla aditiva) cuanto se desplaza donde se
// lee la escena en este pixel, en UV de pantalla. composite.frag lo aplica.
//
// Cada particula es una lente blanda: atrae lo de detras hacia su centro
// (aumenta) con un perfil r * (1 - r^2) que vale 0 en el centro y en el
// borde, asi no hay cortes. Fuerza 1 = hasta ~40% de su radio en pantalla;
// negativa, hacia fuera.

layout(location = 0) in vec4 v_color;  // xy = del centro a este pixel (UV de pantalla), w = fuerza
layout(location = 1) in vec2 v_uv;     // -1..1 dentro del quad

layout(location = 0) out vec2 out_offset;

void main() {
    float r2 = dot(v_uv, v_uv);
    if (r2 >= 1.0) {
        discard;
    }
    // v_color.xy ya crece con r: por (1 - r^2) da el perfil r * (1 - r^2).
    out_offset = -v_color.xy * (1.0 - r2) * v_color.w;
}
