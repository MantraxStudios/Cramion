#version 450

// Picking por ID (el clic del editor): cada actor se dibuja con skinned.vert
// (la misma profundidad que el G-buffer) solo en el pixel pedido y escribe su
// indice + 1. El pipeline prueba "menor o igual" contra el depth de la escena:
// queda el objeto que se ve en ese pixel, con su forma exacta.

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

layout(location = 0) out uint out_id;

void main() {
    out_id = push.pick_id;
}
