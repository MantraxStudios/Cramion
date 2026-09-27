#version 450
#extension GL_GOOGLE_include_directive : require

// Vegetacion en el G-buffer: color por vertice (con el tono de cada arbol),
// oclusion de la copa y superficie mate. Las dos caras: la normal mira
// siempre hacia la camara. Despues lluvia, charcos y decals como cualquier
// superficie (gbuffer_surface.glsl).

layout(location = 0) in vec3 v_world_position;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec4 v_color;
layout(location = 3) in vec4 v_current_clip;
layout(location = 4) in vec4 v_previous_clip;

#include "gbuffer_surface.glsl"

void main() {
    vec3 n = normalize(v_normal);
    if (!gl_FrontFacing) n = -n;
    float occlusion = v_color.a;
    writeSurface(vec4(clamp(v_color.rgb, 0.0, 1.0), 1.0), n, n, vec3(0.0, 0.0, 1.0), n, 0.0, 0.82, occlusion,
                 vec3(0.0), 0.04, v_world_position);
    writeVelocity(v_current_clip, v_previous_clip);
}
