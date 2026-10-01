#version 450
#extension GL_GOOGLE_include_directive : require

// Liquidos: cada particula es un cuadrado de cara a la camara (6 vertices,
// sin buffer de vertices); el fragmento lo recorta a una esfera.

#include "fluid_render.glsl"

layout(location = 0) out vec2 v_corner;
layout(location = 1) out vec3 v_center;
layout(location = 2) flat out uint v_material;
layout(location = 3) out float v_foam;
layout(location = 4) out float v_radius;

const vec2 kCorners[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

void main() {
    uint id = uint(gl_VertexIndex) / 6u;
    vec2 corner = kCorners[uint(gl_VertexIndex) % 6u];
    vec4 p = fluid_pos[id];
    vec4 info = fluid_info[id];
    // Las sueltas (pocos vecinos: gotas y salpicaduras) algo mas pequenas.
    float radius = fr.params.x * mix(0.75, 1.0, smoothstep(0.15, 0.6, info.y));
    vec3 center = (fr.view * vec4(p.xyz, 1.0)).xyz;
    v_corner = corner;
    v_center = center;
    v_material = min(uint(p.w + 0.5), kFluidMaterials - 1u);
    v_foam = info.z;
    v_radius = radius;
    // El cuadrado se adelanta hacia la camara (la cara de la esfera) y se
    // agranda un poco para cubrir su silueta en perspectiva.
    vec3 corner_view = center + vec3(corner * radius * 1.15, radius);
    if (-corner_view.z < 0.01) corner_view.z = min(center.z, -0.01);
    gl_Position = fr.projection * vec4(corner_view, 1.0);
}
