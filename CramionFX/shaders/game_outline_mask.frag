#version 450

// Mascara de los contornos de juego (componente Outline, RM-capas): como
// outline_mask.frag, pero cada objeto escribe el numero de su estilo
// (pick_id = estilo + 1, de 1 a 15) en lugar de 1. Sin prueba de profundidad
// en R (la silueta entera) y con prueba contra la profundidad de la escena en
// G (lo que se ve). Es un "Custom Depth/Stencil" propio: game_outline.frag
// sabe de cada pixel a que estilo pertenece y si esta tapado.

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 base_color;
    vec4 emissive;
    vec4 material;
    uint bone_offset;
    float reflectance;
    uint pick_id;
    uint pad;
} push;

layout(location = 0) out vec4 out_mask;

void main() {
    out_mask = vec4(float(push.pick_id) / 255.0);
}
