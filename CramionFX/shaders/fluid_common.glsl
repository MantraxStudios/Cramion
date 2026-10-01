// Liquidos (FluidPass): lo que comparten la simulacion y el render.
// MISMO orden que FluidMaterialGpu / FluidRenderParams en FluidPass.cpp.

struct FluidMaterialGpu {
    vec4 absorb_visc;     // rgb absorcion (1/m), a viscosidad XSPH
    vec4 scatter_cohes;   // rgb color difuso, a cohesion (m/s2)
    vec4 emission_vort;   // rgb emision HDR, a vorticidad
    vec4 params;          // x espuma, y rugosidad, z frenado (1/s), w costra (lava)
};

const uint kFluidMaterials = 8u;

// Hash entero (PCG) para el ruido de la lava y las variaciones.
uint fluidPcg(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float fluidHash3(ivec3 c) {
    return float(fluidPcg(uint(c.x) ^ fluidPcg(uint(c.y) ^ fluidPcg(uint(c.z) + 0x9E3779B9u)))) * (1.0 / 4294967295.0);
}
float fluidNoise3(vec3 p) {
    vec3 cell = floor(p);
    ivec3 i = ivec3(cell);
    vec3 f = p - cell;
    vec3 u = f * f * (3.0 - 2.0 * f);
    float n000 = fluidHash3(i);
    float n100 = fluidHash3(i + ivec3(1, 0, 0));
    float n010 = fluidHash3(i + ivec3(0, 1, 0));
    float n110 = fluidHash3(i + ivec3(1, 1, 0));
    float n001 = fluidHash3(i + ivec3(0, 0, 1));
    float n101 = fluidHash3(i + ivec3(1, 0, 1));
    float n011 = fluidHash3(i + ivec3(0, 1, 1));
    float n111 = fluidHash3(i + ivec3(1, 1, 1));
    return mix(mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y), mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y), u.z);
}
