// Lo que ve un shader de superficie del usuario (.crshader), en los dos
// stages (surface.vert y surface.frag lo incluyen tras declarar su push
// constant y la camara).
//
// Propiedades del material: 8 vec4 por material en un storage buffer del
// frame. En el pase de geometria push.pick_id no se usa para el picking: dice
// en que bloque de 8 empiezan las de este material.
layout(std430, set = 0, binding = 5) readonly buffer SurfaceParams {
    vec4 surface_params[];
};
#define CRAMION_PARAM(i) surface_params[push.pick_id * 8u + uint(i)]

// Texturas propias del material (set 1, 5..8): blancas si no se asignan.
layout(set = 1, binding = 5) uniform sampler2D cramion_texture0;
layout(set = 1, binding = 6) uniform sampler2D cramion_texture1;
layout(set = 1, binding = 7) uniform sampler2D cramion_texture2;
layout(set = 1, binding = 8) uniform sampler2D cramion_texture3;

// Segundos desde el inicio (los de la lluvia) y la camara.
#define TIME (weather.params.z)
#define CAMERA_POSITION (camera.position.xyz)

// Lo que el shader lee y cambia de la superficie. Los colores van como se
// ven (el mismo espacio que las texturas); la emision es luz (1 = la
// emision 1 de un material).
struct Surface {
    vec3 albedo;         // color base
    float alpha;         // < 0.5 no se dibuja (recorte)
    vec3 normal;         // normal en el mundo (ya con el normal map)
    float metallic;      // 0..1
    float roughness;     // 0..1
    float occlusion;     // 0..1 (1 = sin sombra ambiental)
    vec3 emission;       // brillo propio
    // Solo lectura:
    vec2 uv;
    vec3 worldPosition;
    vec3 vertexNormal;   // normal de la malla (sin normal map)
    vec3 viewDirection;  // del punto hacia la camara
};

// Vertice en el mundo (ya animado), antes de proyectarlo.
struct Vertex {
    vec3 position;
    vec3 normal;
    vec2 uv;
};

// --- Ayudas ---
float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float hash(vec3 p) {
    return hash(p.xy + p.z * 17.13);
}
// Ruido de valor suave, 0..1.
float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1, 0)), u.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), u.x), u.y);
}
float noise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);
    float a = mix(mix(hash(i), hash(i + vec3(1, 0, 0)), u.x), mix(hash(i + vec3(0, 1, 0)), hash(i + vec3(1, 1, 0)), u.x), u.y);
    float b = mix(mix(hash(i + vec3(0, 0, 1)), hash(i + vec3(1, 0, 1)), u.x),
                  mix(hash(i + vec3(0, 1, 1)), hash(i + vec3(1, 1, 1)), u.x), u.y);
    return mix(a, b, u.z);
}
// Varias capas de ruido (mas detalle), 0..1.
float fbm(vec2 p) {
    float sum = 0.0;
    float amplitude = 0.5;
    for (int i = 0; i < 5; ++i) {
        sum += noise(p) * amplitude;
        p *= 2.03;
        amplitude *= 0.5;
    }
    return sum / 0.96875;
}
// Borde brillante: 0 de frente, 1 en el contorno.
float fresnel(Surface s, float power) {
    return pow(1.0 - clamp(dot(normalize(s.normal), normalize(s.viewDirection)), 0.0, 1.0), power);
}
float remap(float v, float a, float b, float c, float d) {
    return c + (v - a) / (b - a) * (d - c);
}
