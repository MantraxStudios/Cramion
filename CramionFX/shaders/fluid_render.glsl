// Liquidos (FluidPass): set 0 del render (profundidad, grosor, suavizado y
// sombreado). MISMO orden que FluidRenderParams en FluidPass.cpp.

#include "fluid_common.glsl"

layout(std140, set = 0, binding = 0) uniform FluidRenderParams {
    mat4 view;
    mat4 projection;          // con el jitter de la escena
    mat4 inverse_projection;
    mat4 inverse_view;
    vec4 viewport;            // xy tamano, zw 1 / tamano
    vec4 params;              // x radio de las esferas (m), y escala del grosor, z suavizado (m), w maximo (px)
    vec4 params2;             // x refraccion, y segundos, z radio de particula, w ver particulas (0/1)
    FluidMaterialGpu materials[kFluidMaterials];
} fr;

layout(std430, set = 0, binding = 1) readonly buffer FluidPos { vec4 fluid_pos[]; };
layout(std430, set = 0, binding = 2) readonly buffer FluidVel { vec4 fluid_vel[]; };
layout(std430, set = 0, binding = 3) readonly buffer FluidInfo { vec4 fluid_info[]; };
layout(set = 0, binding = 4) uniform sampler2D fluid_scene_depth;

// Profundidad lineal (m, positiva) de la escena en uv.
float sceneLinearDepth(vec2 uv) {
    float d = textureLod(fluid_scene_depth, uv, 0.0).r;
    vec4 v = fr.inverse_projection * vec4(uv * 2.0 - 1.0, d, 1.0);
    return -v.z / v.w;
}

// Punto de vista (espacio de la camara) en uv a la profundidad lineal z.
vec3 viewPosition(vec2 uv, float z) {
    vec4 v = fr.inverse_projection * vec4(uv * 2.0 - 1.0, 0.5, 1.0);
    vec3 ray = v.xyz / v.w;
    return ray * (z / -ray.z);
}
