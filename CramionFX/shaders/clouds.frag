#version 450
#extension GL_GOOGLE_include_directive : require

// Nubes volumetricas (cumulos), a media resolucion, en la linea de Horizon
// Zero Dawn y de las "Volumetric Clouds" de Unreal:
//
//   - Capa esferica entre kCloudBottom y kCloudTop sobre el planeta (con su
//     curvatura: hacia el horizonte las nubes se ven de canto y se aplanan).
//   - Densidad = forma (ruido Perlin-Worley) recortada por la cobertura y
//     por un perfil de altura (base plana, cima redondeada), y erosionada en
//     los bordes por Worley de alta frecuencia.
//   - Luz: por cada muestra, un rayo corto hacia el sol (o la luna) mide cuanta
//     nube tiene delante (Beer-Lambert). La dispersion multiple se aproxima
//     sumando octavas con menos extincion y fase mas isotropa (Wrenninge);
//     la fase es Henyey-Greenstein de dos lobulos (borde plateado a contraluz).
//     Mas el cielo como luz ambiente, mas fuerte arriba.
//
// Salida: rgb = luz dispersada hacia la camara, a = transmitancia (lo que se
// ve del cielo de detras). lighting.frag compone: cielo * a + rgb.

layout(set = 0, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
} camera;

layout(set = 0, binding = 1) uniform sampler3D cloud_noise;
layout(set = 0, binding = 2) uniform sampler2D sky_lut;

layout(push_constant) uniform PushConstants {
    vec4 to_light_time;     // xyz = hacia la luz direccional activa, w = segundos
    vec4 light_coverage;    // rgb = su radiancia, a = cobertura (0..1)
    vec4 params;            // x = numero de frame (ruido), y = densidad
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_clouds;

#include "ibl_common.glsl"

const float kEarthRadius = 6360000.0;
const float kCloudBottom = 1500.0;
const float kCloudTop = 4000.0;
const float kMaxDistance = 60000.0;
const int kSteps = 64;
const int kLightSteps = 6;
const float kShapeScale = 1.0 / 24000.0;   // una repeticion del ruido de forma cada 24 km
const float kDetailScale = 1.0 / 3500.0;
const float kExtinction = 0.045;            // por metro, con densidad 1
const vec3 kWind = vec3(12.0, 0.0, 4.0);    // m/s

float remap(float value, float low, float high, float new_low, float new_high) {
    return new_low + (value - low) / max(high - low, 0.0001) * (new_high - new_low);
}

// Distancia a la esfera de radio `radius` desde dentro (salida del rayo).
float sphereExit(vec3 origin, vec3 direction, float radius) {
    float b = dot(origin, direction);
    float c = dot(origin, origin) - radius * radius;
    float discriminant = b * b - c;
    return discriminant < 0.0 ? -1.0 : -b + sqrt(discriminant);
}

float heightFraction(vec3 planet_position) {
    return clamp((length(planet_position) - kEarthRadius - kCloudBottom) /
                 (kCloudTop - kCloudBottom), 0.0, 1.0);
}

// Densidad en un punto (0 = aire). `detailed`: con la erosion de los bordes
// (el rayo de luz se la ahorra).
float cloudDensity(vec3 planet_position, bool detailed) {
    float h = heightFraction(planet_position);
    vec3 world = vec3(planet_position.x, length(planet_position) - kEarthRadius,
                      planet_position.z) + kWind * push.to_light_time.w;

    vec4 shape = textureLod(cloud_noise, world * kShapeScale, 0.0);
    float cells = shape.g * 0.625 + shape.b * 0.25 + shape.a * 0.125;
    float base = remap(shape.r, cells - 1.0, 1.0, 0.0, 1.0);

    // Perfil de cumulo: aparece enseguida en la base y se estrecha arriba.
    base *= smoothstep(0.0, 0.08, h) * smoothstep(1.0, 0.55, h);

    float coverage = push.light_coverage.a;
    base = clamp(remap(base, 1.0 - coverage, 1.0, 0.0, 1.0), 0.0, 1.0) * coverage;
    if (base <= 0.0 || !detailed) {
        return base;
    }

    // Erosion: hilachas abajo, coliflor arriba.
    vec4 detail = textureLod(cloud_noise, world * kDetailScale, 0.0);
    float detail_fbm = detail.g * 0.625 + detail.b * 0.25 + detail.a * 0.125;
    float erosion = mix(detail_fbm, 1.0 - detail_fbm, clamp(h * 4.0, 0.0, 1.0)) * 0.35;
    return clamp(remap(base, erosion, 1.0, 0.0, 1.0), 0.0, 1.0);
}

float henyeyGreenstein(float cos_angle, float g) {
    float g2 = g * g;
    // Normalizada a media 1 (x 4 pi): la radiancia de la luz es la de la escena.
    return (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * cos_angle, 1e-4), 1.5);
}

float phase(float cos_angle, float scale) {
    return mix(henyeyGreenstein(cos_angle, 0.8 * scale), henyeyGreenstein(cos_angle, -0.3 * scale),
               0.3);
}

// Profundidad optica hacia la luz desde un punto de la nube.
float lightOpticalDepth(vec3 planet_position, vec3 to_light) {
    float depth = 0.0;
    float step_length = 120.0;
    vec3 p = planet_position;
    for (int i = 0; i < kLightSteps; ++i) {
        p += to_light * step_length;
        depth += cloudDensity(p, i < 2) * step_length;
        step_length *= 1.6;
    }
    return depth * kExtinction;
}

void main() {
    // Direccion de la vista (un punto cualquiera del rayo sirve).
    vec4 world = camera.inverse_view_projection * vec4(v_uv * 2.0 - 1.0, 0.5, 1.0);
    vec3 direction = normalize(world.xyz / world.w - camera.position.xyz);

    // La escena esta en el origen del planeta, a ras de suelo.
    vec3 origin = vec3(camera.position.x, kEarthRadius + max(camera.position.y, 0.0) + 2.0,
                       camera.position.z);
    // Mirando hacia abajo el rayo choca con el suelo antes que con la capa.
    if (direction.y < -0.01) {
        out_clouds = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float t_start = sphereExit(origin, direction, kEarthRadius + kCloudBottom);
    float t_end = min(sphereExit(origin, direction, kEarthRadius + kCloudTop), kMaxDistance);
    if (t_start < 0.0 || t_start >= t_end) {
        out_clouds = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 to_light = normalize(push.to_light_time.xyz);
    vec3 light_radiance = push.light_coverage.rgb;
    float cos_angle = dot(direction, to_light);

    // Luz ambiente: el cielo de arriba (mas en la cima de la nube) y el suelo.
    vec3 sky_ambient = skyAverage();
    vec3 ground_ambient = groundRadiance(light_radiance, to_light) * 0.5;

    // Primer paso desplazado al azar por pixel y frame: las bandas de un paso
    // fijo pasan a ser ruido fino.
    vec2 noise_pixel = gl_FragCoord.xy + 5.588238 * mod(push.params.x, 64.0);
    float jitter = fract(52.9829189 * fract(dot(noise_pixel, vec2(0.06711056, 0.00583715))));

    float step_length = (t_end - t_start) / float(kSteps);
    float t = t_start + step_length * jitter;
    float transmittance = 1.0;
    vec3 scattered = vec3(0.0);

    for (int i = 0; i < kSteps && transmittance > 0.01; ++i) {
        vec3 p = origin + direction * t;
        float density = cloudDensity(p, true) * push.params.y;
        if (density > 0.001) {
            float extinction = density * kExtinction;
            float light_depth = lightOpticalDepth(p, to_light);

            // Dispersion multiple: tres octavas con menos extincion, menos
            // energia y fase mas isotropa en cada una.
            float energy = 0.0;
            float a = 1.0;
            float b = 1.0;
            float c = 1.0;
            for (int octave = 0; octave < 3; ++octave) {
                energy += a * exp(-light_depth * b) * phase(cos_angle, c);
                a *= 0.5;
                b *= 0.4;
                c *= 0.5;
            }
            // "Powder": los bordes de cara al sol son algo mas oscuros que el
            // interior (la luz aun no se ha dispersado hacia fuera).
            float powder = 1.0 - exp(-extinction * 400.0);
            energy *= mix(1.0, powder, 0.5 * (1.0 - clamp(cos_angle, 0.0, 1.0)));

            float h = heightFraction(p);
            vec3 ambient = mix(ground_ambient, sky_ambient, h * 0.7 + 0.3) *
                           mix(0.5, 1.0, h);
            vec3 in_scattered = light_radiance * energy + ambient;

            // Integracion exacta del tramo (Hillaire 2015): estable aunque el
            // paso sea largo frente a la extincion.
            float step_transmittance = exp(-extinction * step_length);
            scattered += transmittance * in_scattered * (1.0 - step_transmittance);
            transmittance *= step_transmittance;
        }
        t += step_length;
    }

    // Perspectiva aerea: las nubes lejanas se funden con el cielo del
    // horizonte en vez de acabar en un borde.
    float fade = exp(-t_start / 30000.0);
    scattered *= fade;
    transmittance = mix(1.0, transmittance, fade);

    out_clouds = vec4(scattered, transmittance);
}
