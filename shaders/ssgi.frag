#version 450

// Iluminacion global en espacio de pantalla (SSGI), a media resolucion.
//
// La luz que rebota en las superficies: en un patio, las paredes de color
// terracota tiñen de calido las sombras, la vegetacion de verde, el suelo
// soleado ilumina los techos de los porticos. Sin esto todo lo que no ve el
// sol recibe solo la luz azul del cielo.
//
// Para cada pixel se lanzan kRays rayos en la semiesfera de su normal (con
// distribucion coseno) y se recorre el depth buffer hasta kRadius metros:
//
//   - si el rayo choca con una superficie, se toma la luz que esa superficie
//     reflejaba en el frame ANTERIOR (imagen HDR ya iluminada, reproyectada
//     con la view-projection anterior). Como esa imagen ya incluia la GI del
//     frame previo, los rebotes se acumulan: un rebote por frame.
//   - si escapa, esa fraccion del hemisferio ve el cielo: la da el IBL.
//
// Salida: rgb = luz rebotada (ya como media sobre el hemisferio, lista para
// multiplicar por el albedo), a = fraccion de rayos que escapan (visibilidad
// del cielo a gran escala). La rotacion de los rayos sigue un patron 4x4 que
// la pasada de iluminacion promedia al reescalar.

layout(set = 0, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
} camera;

layout(set = 0, binding = 1) uniform sampler2D g_depth;
layout(set = 0, binding = 2) uniform sampler2D g_normal;
// Imagen HDR iluminada del frame anterior.
layout(set = 0, binding = 3) uniform sampler2D previous_color;

layout(push_constant) uniform PushConstants {
    mat4 previous_view_projection;
    vec4 params;  // x = hay frame anterior valido, y = intensidad
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_gi;

const int kRays = 6;
const int kSteps = 10;
const float kRadius = 3.0;       // metros
const float kMaxRadiance = 12.0; // tope por muestra: sin destellos del sol
const float kGoldenAngle = 2.39996323;
const float kTwoPi = 6.28318531;

float linearDepth(float depth) {
    return camera.projection[3][2] / (depth + camera.projection[2][2]);
}

vec3 viewFromDepth(vec2 uv, float depth) {
    float z = linearDepth(depth);
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc.x * z / camera.projection[0][0], ndc.y * z / camera.projection[1][1], -z);
}

vec3 decodeNormal(vec2 e) {
    vec3 n = vec3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y);
    float t = max(-n.y, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.z += n.z >= 0.0 ? -t : t;
    return normalize(n);
}

float bayer4(ivec2 p) {
    const float kBayer[16] = float[](0.0, 8.0, 2.0, 10.0,
                                     12.0, 4.0, 14.0, 6.0,
                                     3.0, 11.0, 1.0, 9.0,
                                     15.0, 7.0, 13.0, 5.0);
    return (kBayer[(p.y & 3) * 4 + (p.x & 3)] + 0.5) / 16.0;
}

void main() {
    ivec2 full_size = textureSize(g_depth, 0);
    // Pixel de resolucion completa que representa a este de media.
    ivec2 half_pixel = ivec2(gl_FragCoord.xy);
    ivec2 pixel = min(half_pixel * 2, full_size - 1);

    float depth = texelFetch(g_depth, pixel, 0).r;
    if (depth >= 1.0) {
        out_gi = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec2 uv = (vec2(pixel) + 0.5) / vec2(full_size);
    vec3 position = viewFromDepth(uv, depth);
    vec3 normal_world = decodeNormal(texelFetch(g_normal, pixel, 0).rg);
    vec3 normal = normalize(mat3(camera.view) * normal_world);
    mat3 inverse_view = transpose(mat3(camera.view));

    // Base ortonormal alrededor de la normal.
    vec3 helper = abs(normal.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(helper, normal));
    vec3 bitangent = cross(normal, tangent);

    float noise = bayer4(half_pixel);
    float rotation = noise * kTwoPi;
    bool has_history = push.params.x > 0.5;

    // Punto de partida algo separado de la superficie: sin esto el primer
    // paso choca con el propio pixel por falta de precision.
    vec3 origin = position + normal * (0.02 + 0.002 * -position.z);

    vec3 radiance = vec3(0.0);
    float escaped = 0.0;

    for (int r = 0; r < kRays; ++r) {
        // Direccion con distribucion coseno (espiral de Fibonacci rotada).
        float u = (float(r) + 0.5) / float(kRays);
        float phi = float(r) * kGoldenAngle + rotation;
        float sin_theta = sqrt(u);
        vec3 local = vec3(sin_theta * cos(phi), sin_theta * sin(phi), sqrt(1.0 - u));
        vec3 direction = tangent * local.x + bitangent * local.y + normal * local.z;

        bool hit = false;
        for (int s = 0; s < kSteps; ++s) {
            // Pasos que crecen: finos cerca (contactos), largos lejos.
            float t = (float(s) + noise) / float(kSteps);
            vec3 sample_position = origin + direction * (kRadius * t * t + 0.05);

            vec4 clip = camera.projection * vec4(sample_position, 1.0);
            if (clip.w <= 0.0) {
                break;
            }
            vec2 sample_uv = clip.xy / clip.w * 0.5 + 0.5;
            if (any(lessThan(sample_uv, vec2(0.0))) || any(greaterThan(sample_uv, vec2(1.0)))) {
                break;  // Fuera de pantalla: no se sabe, cuenta como cielo.
            }

            ivec2 texel = min(ivec2(sample_uv * vec2(full_size)), full_size - 1);
            float scene_depth = texelFetch(g_depth, texel, 0).r;
            if (scene_depth >= 1.0) {
                continue;
            }
            float scene_z = linearDepth(scene_depth);
            float sample_z = -sample_position.z;
            float thickness = 0.3 + 0.1 * sample_z * t;

            if (sample_z > scene_z + 0.01 && sample_z < scene_z + thickness) {
                hit = true;
                if (has_history) {
                    // La superficie golpeada, del lado que mira al rayo.
                    vec3 hit_normal = decodeNormal(texelFetch(g_normal, texel, 0).rg);
                    vec3 hit_normal_view = mat3(camera.view) * hit_normal;
                    float facing = clamp(dot(hit_normal_view, -direction) * 2.0, 0.0, 1.0);

                    // Reproyeccion al frame anterior.
                    vec3 hit_view = viewFromDepth((vec2(texel) + 0.5) / vec2(full_size),
                                                  scene_depth);
                    vec3 hit_world = camera.position.xyz + inverse_view * hit_view;
                    vec4 previous_clip = push.previous_view_projection * vec4(hit_world, 1.0);
                    vec2 previous_uv = previous_clip.xy / previous_clip.w * 0.5 + 0.5;
                    if (previous_clip.w > 0.0 && all(greaterThanEqual(previous_uv, vec2(0.0))) &&
                        all(lessThanEqual(previous_uv, vec2(1.0)))) {
                        vec3 light = textureLod(previous_color, previous_uv, 0.0).rgb;
                        radiance += min(light, vec3(kMaxRadiance)) * facing;
                    }
                }
                break;
            }
        }
        if (!hit) {
            escaped += 1.0;
        }
    }

    out_gi = vec4(radiance / float(kRays) * push.params.y, escaped / float(kRays));
}
