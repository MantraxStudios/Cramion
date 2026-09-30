// Meshlets (mesh shaders): los datos de un modelo partido en grupos de hasta
// 64 vertices y 124 triangulos (SkinnedModel::buildMeshlets, meshoptimizer).
// MESHLET_SET: el set donde van (1 en las sombras, 2 en el G-buffer).

#ifndef MESHLET_SET
#define MESHLET_SET 1
#endif

// Debe coincidir con GpuMeshlet (GpuTypes.h).
struct Meshlet {
    vec4 center_radius;     // esfera en el espacio del modelo
    vec4 cone_axis_cutoff;  // cono de normales (cutoff 1 = sin cono)
    vec4 cone_apex;
    uint vertex_offset;     // en meshlet_vertices
    uint triangle_offset;   // byte en meshlet_triangles
    uint vertex_count;
    uint triangle_count;
};

// Los vertices del modelo tal cual (asset::SkinnedVertex, 80 bytes): posicion,
// normal, uv, tangente, huesos (uint) y pesos.
layout(std430, set = MESHLET_SET, binding = 0) readonly buffer MeshVertices { float vertex_data[]; };
layout(std430, set = MESHLET_SET, binding = 1) readonly buffer Meshlets { Meshlet meshlets[]; };
layout(std430, set = MESHLET_SET, binding = 2) readonly buffer MeshletVertices { uint meshlet_vertices[]; };
// Tres bytes por triangulo (indices locales del meshlet), empaquetados en uint.
layout(std430, set = MESHLET_SET, binding = 3) readonly buffer MeshletTriangles { uint meshlet_triangles[]; };

const uint kVertexFloats = 20u;

vec3 vertexPosition(uint v) {
    uint b = v * kVertexFloats;
    return vec3(vertex_data[b], vertex_data[b + 1u], vertex_data[b + 2u]);
}

vec3 vertexNormal(uint v) {
    uint b = v * kVertexFloats + 3u;
    return vec3(vertex_data[b], vertex_data[b + 1u], vertex_data[b + 2u]);
}

vec2 vertexUv(uint v) {
    uint b = v * kVertexFloats + 6u;
    return vec2(vertex_data[b], vertex_data[b + 1u]);
}

vec4 vertexTangent(uint v) {
    uint b = v * kVertexFloats + 8u;
    return vec4(vertex_data[b], vertex_data[b + 1u], vertex_data[b + 2u], vertex_data[b + 3u]);
}

uvec4 vertexJoints(uint v) {
    uint b = v * kVertexFloats + 12u;
    return uvec4(floatBitsToUint(vertex_data[b]), floatBitsToUint(vertex_data[b + 1u]),
                 floatBitsToUint(vertex_data[b + 2u]), floatBitsToUint(vertex_data[b + 3u]));
}

vec4 vertexWeights(uint v) {
    uint b = v * kVertexFloats + 16u;
    return vec4(vertex_data[b], vertex_data[b + 1u], vertex_data[b + 2u], vertex_data[b + 3u]);
}

uint meshletTriangleByte(uint byte_index) {
    return (meshlet_triangles[byte_index >> 2u] >> ((byte_index & 3u) * 8u)) & 0xFFu;
}

uvec3 meshletTriangle(Meshlet m, uint t) {
    uint b = m.triangle_offset + t * 3u;
    return uvec3(meshletTriangleByte(b), meshletTriangleByte(b + 1u), meshletTriangleByte(b + 2u));
}
