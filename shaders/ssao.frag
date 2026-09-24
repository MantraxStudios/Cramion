#version 450

// Oclusion ambiental en espacio de pantalla (SSAO).
//
// Complementa a la AO horneada de los materiales: recoge el contacto entre
// objetos distintos (una silla con el suelo, una maceta con la pared) y los
// huecos que ninguna textura conoce.
//
// Para cada pixel se lanzan muestras en una semiesfera orientada por la normal
// (en espacio de vista) y se cuenta cuantas quedan por detras de la superficie
// que se ve en el depth. La rotacion de las muestras sigue un patron de 4x4
// pixeles, y la pasada de iluminacion promedia exactamente esa ventana de 4x4
// (con pesos de profundidad), asi que el ruido desaparece sin emborronar
// bordes.
//
// Salida: r = cuanta luz ambiental llega (1 = nada ocluido), g = profundidad
// lineal de vista (la usa el desenfoque para no mezclar superficies).

layout(set = 0, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
} camera;

layout(set = 0, binding = 1) uniform sampler2D g_depth;
layout(set = 0, binding = 2) uniform sampler2D g_normal;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec2 out_ao;

const int kSampleCount = 16;
const float kRadius = 0.6;      // en unidades del mundo (1 = un bloque)
// Separacion minima para contar como oclusion: crece con la distancia porque
// la precision del depth cae con ella.
const float kBias = 0.02;
const float kBiasPerUnit = 0.0015;
// De lejos el radio ocupa pocos pixeles y ya no aporta: se desvanece.
const float kFadeStart = 60.0;
const float kFadeEnd = 120.0;
const float kIntensity = 1.4;
const float kGoldenAngle = 2.39996323;
const float kTwoPi = 6.28318531;

vec3 decodeNormal(vec2 e) {
    vec3 n = vec3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y);
    float t = max(-n.y, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.z += n.z >= 0.0 ? -t : t;
    return normalize(n);
}

// Profundidad lineal (distancia de vista) a partir del depth, deshaciendo solo
// la proyeccion: z = P[3][2] / (depth + P[2][2]).
//
// Hacerlo con la inversa completa de view-projection sumaba el error de
// redondeo de la matriz al del propio depth: a 100 bloques la posicion salia
// desplazada varios centimetros, mas que los umbrales del SSAO y de las
// sombras de contacto, y las caras superiores lejanas se "tapaban a si
// mismas" con un ruido de puntos negros.
float linearDepth(float depth) {
    return camera.projection[3][2] / (depth + camera.projection[2][2]);
}

// Posicion en espacio de vista (la proyeccion no tiene desplazamiento en x/y).
vec3 viewFromDepth(vec2 uv, float depth) {
    float z = linearDepth(depth);
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc.x * z / camera.projection[0][0], ndc.y * z / camera.projection[1][1], -z);
}

// Matriz de Bayer 4x4 normalizada: cada pixel de la ventana tiene un valor
// distinto, asi que 16 pixeles vecinos cubren 16 rotaciones distintas.
float bayer4(ivec2 p) {
    const float kBayer[16] = float[](0.0, 8.0, 2.0, 10.0,
                                     12.0, 4.0, 14.0, 6.0,
                                     3.0, 11.0, 1.0, 9.0,
                                     15.0, 7.0, 13.0, 5.0);
    return (kBayer[(p.y & 3) * 4 + (p.x & 3)] + 0.5) / 16.0;
}

void main() {
    float depth = texture(g_depth, v_uv).r;
    if (depth >= 1.0) {
        out_ao = vec2(1.0, 1.0e6);  // cielo
        return;
    }

    vec3 position = viewFromDepth(v_uv, depth);
    float view_depth = -position.z;
    if (view_depth > kFadeEnd) {
        out_ao = vec2(1.0, view_depth);
        return;
    }
    float bias = kBias + view_depth * kBiasPerUnit;
    vec3 normal = normalize(mat3(camera.view) * decodeNormal(texture(g_normal, v_uv).rg));

    // Base ortonormal alrededor de la normal.
    vec3 helper = abs(normal.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(helper, normal));
    vec3 bitangent = cross(normal, tangent);

    float rotation = bayer4(ivec2(gl_FragCoord.xy)) * kTwoPi;

    float occlusion = 0.0;
    for (int i = 0; i < kSampleCount; ++i) {
        // Espiral de Fibonacci sobre la semiesfera, con distribucion coseno y
        // mas muestras cerca del centro (donde mas ocluye el contacto).
        float t = (float(i) + 0.5) / float(kSampleCount);
        float phi = float(i) * kGoldenAngle + rotation;
        float r = sqrt(t);
        vec3 direction = vec3(r * cos(phi), r * sin(phi), sqrt(1.0 - t));
        float scale = mix(0.15, 1.0, t * t);

        vec3 sample_position = position + (tangent * direction.x + bitangent * direction.y +
                                           normal * direction.z) * (kRadius * scale);

        vec4 clip = camera.projection * vec4(sample_position, 1.0);
        vec2 sample_uv = clip.xy / clip.w * 0.5 + 0.5;
        if (any(lessThan(sample_uv, vec2(0.0))) || any(greaterThan(sample_uv, vec2(1.0)))) {
            continue;
        }

        float scene_depth = texture(g_depth, sample_uv).r;
        float scene_z = -linearDepth(scene_depth);

        // Lo que esta mucho mas cerca de la camara que el punto (otro objeto
        // por delante) no debe oscurecerlo: se desvanece con la distancia.
        float range = smoothstep(0.0, 1.0, kRadius / abs(position.z - scene_z));
        occlusion += (scene_z >= sample_position.z + bias ? 1.0 : 0.0) * range;
    }

    float ao = clamp(pow(1.0 - occlusion / float(kSampleCount), kIntensity), 0.0, 1.0);
    ao = mix(ao, 1.0, smoothstep(kFadeStart, kFadeEnd, view_depth));
    out_ao = vec2(ao, view_depth);
}
