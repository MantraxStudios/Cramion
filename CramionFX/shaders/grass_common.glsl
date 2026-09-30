// Hierba de los terrenos (TerrainPass): lo que comparten grass_cull.comp,
// grass.vert y grass.frag. Debe coincidir con GpuGrassParams (TerrainPass.cpp).

const int kMaxGrassInteractors = 32;

struct GrassBlade {
    vec4 position;  // xyz = base (mundo), w = uintBits: giro, altura, sequedad, anchura (8 bits cada uno)
    vec4 push;      // xz = empuje de lo que la pisa (m), y = aplastado (0..1), w = fase del viento
};

layout(std140, set = GRASS_SET, binding = 0) uniform GrassParams {
    vec4 grid;         // x = separacion (m), y = celdas por lado, z = distancia maxima, w = distancia del detalle
    vec4 origin;       // xy = esquina de la rejilla (x, z del mundo), z = umbral de la capa, w = tope por lista
    vec4 shape;        // x = altura (m), y = variacion, z = anchura (m), w = curvatura
    vec4 base_color;   // rgb (sRGB), a = variacion de color
    vec4 tip_color;    // rgb, a = viento
    vec4 dry_color;    // rgb, a = interaccion
    vec4 wind;         // xy = direccion, z = segundos, w = segundos del frame anterior
    vec4 camera_pos;   // xyz = camara, w = semilla
    ivec4 layers;      // x = capa donde crece, y = capa seca (-1 = ninguna), z = interactores, w = 0
    mat4 view_projection;
    vec4 interactors[kMaxGrassInteractors];  // xyz = centro, w = radio
} grass;

float grassHash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec2 grassHash2(vec2 p) {
    return vec2(grassHash(p), grassHash(p + vec2(17.13, 31.71)));
}

uint packBlade(float yaw01, float height01, float dry01, float width01) {
    return uint(clamp(yaw01, 0.0, 1.0) * 255.0 + 0.5) | (uint(clamp(height01, 0.0, 1.0) * 255.0 + 0.5) << 8u) |
           (uint(clamp(dry01, 0.0, 1.0) * 255.0 + 0.5) << 16u) | (uint(clamp(width01, 0.0, 1.0) * 255.0 + 0.5) << 24u);
}

vec4 unpackBlade(uint bits) {
    return vec4(float(bits & 255u), float((bits >> 8u) & 255u), float((bits >> 16u) & 255u),
                float((bits >> 24u) & 255u)) / 255.0;
}
