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
//   - si no choca con nada en pantalla (sale de ella o no encuentra nada en
//     kRadius metros), se pregunta a la sonda de reflexion, que ve la escena
//     en 3D alrededor de la camara: si en esa direccion hay una superficie,
//     su luz es rebote; si hay cielo, esa fraccion del hemisferio ve el cielo
//     (lo da el IBL). Sin la sonda, lo que no estaba en pantalla contaba como
//     cielo: al girar la camara el rebote de una pared soleada aparecia y
//     desaparecia de golpe.
//
// Salida: rgb = luz rebotada (ya como media sobre el hemisferio, lista para
// multiplicar por el albedo), a = fraccion de rayos que escapan (visibilidad
// del cielo a gran escala). Los rayos cambian de direccion cada frame; el
// filtro SVGF (gi_temporal.comp, gi_atrous.comp) los acumula: con 6 por frame, decenas por
// pixel. (Con un patron fijo por pixel el filtro no tenia nada que promediar y
// los errores se deslizaban por las superficies al moverse.)

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

// Los dos cubos de la sonda de reflexion (prefiltrados; a = distancia).
layout(set = 0, binding = 4) uniform samplerCube reflection_probe_0;
layout(set = 0, binding = 5) uniform samplerCube reflection_probe_1;

layout(push_constant) uniform PushConstants {
    mat4 previous_view_projection;
    vec4 params;  // x = hay frame anterior valido, y = intensidad
    vec4 extra;   // x = numero de frame, y/z = peso de cada cubo de la sonda (0 = sin sonda)
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_gi;

const int kRays = 6;
const int kSteps = 10;
const float kRadius = 3.0;       // metros
// Tope de luminancia por muestra, del orden de una pared blanca al sol. Mas
// alto, un rayo que acierta en algo diminuto y muy brillante (una bombilla
// emisiva, el reflejo del sol) iluminaba el pixel entero: cuadrados de
// colores al ampliar la GI a pantalla completa.
const float kMaxLuminance = 3.0;
const float kGoldenAngle = 2.39996323;
// Mip de la sonda para el rebote (lobulo ancho, como un difuso).
const float kProbeLod = 3.0;
// Mas lejos que esto, lo que ve la sonda es cielo (lighting.frag guarda 5000).
const float kProbeSkyDistance = 2500.0;
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

// Ruido de gradiente entrelazado (Jimenez 2014), distinto cada frame.
float interleavedGradientNoise(vec2 p, float frame) {
    p += 5.588238 * frame;
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

float luminance(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

vec3 capLuminance(vec3 light) {
    light = max(light, vec3(0.0));
    return light * min(1.0, kMaxLuminance / max(luminance(light), 0.0001));
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

    float frame = mod(push.extra.x, 64.0);
    float noise = interleavedGradientNoise(vec2(half_pixel), frame);
    float rotation = interleavedGradientNoise(vec2(half_pixel) + vec2(17.0, 59.0), frame) * kTwoPi;
    float probe_weight = push.extra.y + push.extra.z;
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
                        radiance += capLuminance(light) * facing;
                    }
                }
                break;
            }
        }
        if (!hit) {
            // Nada en pantalla: lo que ve la sonda en esa direccion.
            if (probe_weight > 0.001 && has_history) {
                vec3 world_direction = inverse_view * direction;
                vec4 seen = vec4(0.0);
                if (push.extra.y > 0.001) {
                    seen += vec4(textureLod(reflection_probe_0, world_direction, kProbeLod).rgb,
                                 textureLod(reflection_probe_0, world_direction, 0.0).a) *
                            push.extra.y;
                }
                if (push.extra.z > 0.001) {
                    seen += vec4(textureLod(reflection_probe_1, world_direction, kProbeLod).rgb,
                                 textureLod(reflection_probe_1, world_direction, 0.0).a) *
                            push.extra.z;
                }
                seen /= probe_weight;
                if (seen.a < kProbeSkyDistance) {
                    radiance += capLuminance(seen.rgb);
                } else {
                    escaped += 1.0;
                }
            } else {
                escaped += 1.0;
            }
        }
    }

    out_gi = vec4(radiance / float(kRays) * push.params.y, escaped / float(kRays));
}
