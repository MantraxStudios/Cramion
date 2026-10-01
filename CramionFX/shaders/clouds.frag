#version 450
#extension GL_GOOGLE_include_directive : require

// Nubes volumetricas, a media resolucion, en la linea de Horizon Zero Dawn /
// Forbidden West y de las "Volumetric Clouds" de Unreal y HDRP:
//
//   - Capa esferica entre la base y la cima (componente Sky) sobre el planeta
//     (con su curvatura: hacia el horizonte las nubes se ven de canto).
//   - Mapa de clima: a gran escala (decenas de km) la cobertura y el tipo
//     cambian: hay claros, masas agrupadas y torres sueltas, no un manto
//     uniforme.
//   - Tipo de nube por zona: estratos (capa baja y plana), cumulos (base
//     plana, cima redondeada) y cumulonimbos (torres hasta la cima de la capa).
//   - Densidad = forma (Perlin-Worley) recortada por la cobertura y el perfil
//     de altura, erosionada en los bordes por Worley de alta frecuencia (dos
//     escalas cerca de la camara), con una distorsion que la retuerce.
//   - Viento: las nubes se desplazan (el desplazamiento viene acumulado en el
//     origen), las cimas van por delante y la forma cambia despacio.
//   - Luz: por cada muestra, un rayo hacia el sol (o la luna) mide cuanta nube
//     tiene delante (Beer-Lambert). Dispersion multiple por octavas
//     (Wrenninge), fase Henyey-Greenstein de dos lobulos (borde plateado a
//     contraluz), "powder" y el cielo como luz ambiente, mas fuerte arriba.
//   - Rayo adaptativo: pasos largos y baratos por el aire; al tocar nube
//     vuelve atras y avanza fino con todo el detalle.
//
// Modo normal: rgb = luz dispersada hacia la camara, a = transmitancia (lo que
// se ve del cielo de detras). lighting.frag compone: cielo * a + rgb.
// Modo mapa de sombra (wind.w = 1): r = cuanta luz del sol pasa las nubes en
// cada punto del suelo (lo usa la iluminacion).

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
    vec4 params;            // x = numero de frame (ruido), y = densidad, zw = origen del mundo + viento (xz)
    vec4 layer;             // x = base (m), y = cima (m), z = tipo (0 estratos .. 1 cumulonimbos)
    vec4 wind;              // xy = direccion del viento, z = adelanto de las cimas (m), w = 1 mapa de sombra
    vec4 shadow;            // xy = centro del mapa de sombra, z = lado (m), w = fuerza
    vec4 flash;             // rayo: xyz = donde cayo (mundo), w = brillo del destello (0 = nada)
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_clouds;

#include "ibl_common.glsl"

const float kEarthRadius = 6360000.0;
const float kMaxDistance = 60000.0;
const int kMaxSteps = 128;
const int kLightSteps = 6;
const float kShapeScale = 1.0 / 24000.0;   // una repeticion del ruido de forma cada 24 km
const float kDetailScale = 1.0 / 3500.0;
const float kFineScale = 1.0 / 900.0;      // segunda erosion (bordes nitidos de cerca)
const float kWeatherScale = 1.0 / 60000.0; // mapa de clima: manchas de decenas de km
const float kExtinction = 0.045;           // por metro, con densidad 1

float cloudBottom() { return push.layer.x; }
float cloudTop() { return push.layer.y; }

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
    return clamp((length(planet_position) - kEarthRadius - cloudBottom()) /
                 (cloudTop() - cloudBottom()), 0.0, 1.0);
}

// Perfil vertical segun el tipo (Schneider): x..y sube la base, z..w se
// estrecha la cima. Estratos: finos y bajos; cumulos: medio; cumulonimbos:
// toda la capa con la cima ancha (yunque).
float heightProfile(float h, float type) {
    const vec4 stratus = vec4(0.0, 0.04, 0.12, 0.22);
    const vec4 cumulus = vec4(0.0, 0.08, 0.35, 0.62);
    const vec4 cumulonimbus = vec4(0.0, 0.06, 0.82, 1.0);
    vec4 g = type < 0.5 ? mix(stratus, cumulus, type * 2.0) : mix(cumulus, cumulonimbus, type * 2.0 - 1.0);
    return smoothstep(g.x, g.y, h) * (1.0 - smoothstep(g.z, g.w, h));
}

// Punto de la capa en coordenadas del ruido (metros): con el origen del mundo,
// el viento acumulado y las cimas adelantadas en la direccion del viento.
vec3 noisePosition(vec3 planet_position, float h) {
    vec3 world = vec3(planet_position.x, length(planet_position) - kEarthRadius, planet_position.z);
    world.xz += push.params.zw;
    world.xz += push.wind.xy * (h * h * push.wind.z);
    return world;
}

// Cobertura y tipo de nube en ese sitio (mapa de clima).
vec2 weather(vec3 world) {
    vec4 w = textureLod(cloud_noise, vec3(world.xz * kWeatherScale, 0.37), 0.0);
    float coverage = push.light_coverage.a;
    // Manchas: de claro a masa. Con el cielo casi cubierto, cubierto del todo.
    float patchy = coverage * (0.55 + 0.9 * w.r);
    float local = clamp(mix(patchy, coverage, coverage * coverage * coverage), 0.0, 1.0);
    float type = clamp(push.layer.z + (w.g - 0.5) * 0.6, 0.0, 1.0);
    return vec2(local, type);
}

// Densidad en un punto (0 = aire). `detail`: 0 sin erosion (rayo de luz,
// busqueda), 1 erosion, 2 erosion fina (cerca de la camara).
float cloudDensity(vec3 planet_position, int detail) {
    float h = heightFraction(planet_position);
    vec3 world = noisePosition(planet_position, h);
    vec2 wm = weather(world);
    float coverage = wm.x;
    if (coverage <= 0.001) {
        return 0.0;
    }

    float time = push.to_light_time.w;
    vec4 shape = textureLod(cloud_noise, (world + vec3(0.0, time * 2.0, 0.0)) * kShapeScale, 0.0);
    float cells = shape.g * 0.625 + shape.b * 0.25 + shape.a * 0.125;
    float base = remap(shape.r, cells - 1.0, 1.0, 0.0, 1.0);

    // Mas cobertura, nubes algo mas altas (se desarrollan).
    base *= heightProfile(h, clamp(wm.y + coverage * 0.15, 0.0, 1.0));

    base = clamp(remap(base, 1.0 - coverage, 1.0, 0.0, 1.0), 0.0, 1.0) * coverage;
    if (base <= 0.0 || detail == 0) {
        return base;
    }

    // Erosion: hilachas abajo, coliflor arriba. La posicion se retuerce con
    // el propio ruido (como el curl noise) para que no se vean celdas.
    vec3 twist = (shape.gba - 0.5) * 900.0;
    vec3 detail_position = world + twist + vec3(0.0, time * 6.0, 0.0);
    vec4 d = textureLod(cloud_noise, detail_position * kDetailScale, 0.0);
    float detail_fbm = d.g * 0.625 + d.b * 0.25 + d.a * 0.125;
    float erosion = mix(detail_fbm, 1.0 - detail_fbm, clamp(h * 4.0, 0.0, 1.0)) * 0.35;
    if (detail > 1) {
        vec4 f = textureLod(cloud_noise, detail_position * kFineScale, 0.0);
        float fine = f.g * 0.625 + f.b * 0.25 + f.a * 0.125;
        erosion += mix(fine, 1.0 - fine, clamp(h * 4.0, 0.0, 1.0)) * 0.12;
    }
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
    float step_length = 90.0;
    vec3 p = planet_position;
    for (int i = 0; i < kLightSteps; ++i) {
        p += to_light * step_length;
        depth += cloudDensity(p, i < 2 ? 1 : 0) * step_length;
        step_length *= 1.7;
    }
    return depth * kExtinction * push.params.y;
}

float interleavedNoise(vec2 pixel) {
    vec2 p = pixel + 5.588238 * mod(push.params.x, 64.0);
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// --- Mapa de sombra: cuanta luz del sol llega a cada punto del suelo ---
void shadowMap() {
    vec3 to_light = normalize(push.to_light_time.xyz);
    // Con el sol en el horizonte la sombra se estira kilometros: se apaga.
    float elevation = smoothstep(0.03, 0.15, to_light.y);
    if (elevation <= 0.0) {
        out_clouds = vec4(1.0);
        return;
    }
    vec2 xz = push.shadow.xy + (v_uv - 0.5) * push.shadow.z;
    vec3 ground = vec3(xz.x, kEarthRadius, xz.y);
    // Tramo del rayo hacia el sol dentro de la capa (suelo casi plano aqui).
    float t0 = cloudBottom() / to_light.y;
    float t1 = min(cloudTop() / to_light.y, t0 + 12000.0);
    const int kSteps = 16;
    float step_length = (t1 - t0) / float(kSteps);
    float t = t0 + step_length * interleavedNoise(gl_FragCoord.xy);
    float depth = 0.0;
    for (int i = 0; i < kSteps; ++i) {
        depth += cloudDensity(ground + to_light * t, 0) * step_length;
        t += step_length;
    }
    // Algo de luz atraviesa y rebota dentro: la sombra de una nube no es negra.
    float transmittance = exp(-depth * kExtinction * push.params.y * 0.5);
    transmittance = mix(1.0, transmittance, push.shadow.w * elevation);
    out_clouds = vec4(transmittance, transmittance, transmittance, 1.0);
}

void main() {
    if (push.wind.w > 0.5) {
        shadowMap();
        return;
    }

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

    float t_start = sphereExit(origin, direction, kEarthRadius + cloudBottom());
    float t_end = min(sphereExit(origin, direction, kEarthRadius + cloudTop()), kMaxDistance);
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

    // Paso fino: la capa en ~96 tramos, mas largos lejos (el detalle no se
    // ve y el horizonte llega a decenas de km).
    float span = t_end - t_start;
    float fine_step = clamp(span / 96.0, 40.0, 450.0) * (1.0 + t_start / 25000.0);
    float jitter = interleavedNoise(gl_FragCoord.xy);
    float t = t_start + fine_step * jitter;
    float transmittance = 1.0;
    vec3 scattered = vec3(0.0);
    float weighted_distance = 0.0;  // distancia media de lo que se ve (niebla)
    float weight_sum = 0.0;
    bool searching = true;          // pasos largos sin detalle hasta tocar nube
    int empty_steps = 0;

    for (int i = 0; i < kMaxSteps && t < t_end && transmittance > 0.01; ++i) {
        vec3 p = origin + direction * t;
        if (searching) {
            if (cloudDensity(p, 0) > 0.0) {
                // Nube: vuelve un paso atras y sigue fino.
                searching = false;
                empty_steps = 0;
                t = max(t - fine_step, t_start);
            } else {
                t += fine_step * 2.5;
            }
            continue;
        }

        float density = cloudDensity(p, t < 9000.0 ? 2 : 1) * push.params.y;
        if (density > 0.001) {
            empty_steps = 0;
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

            // Ambiente: arriba ve el cielo, abajo lo tapa la propia nube (bases
            // grises, cimas claras).
            float h = heightFraction(p);
            vec3 ambient = mix(ground_ambient, sky_ambient, h * 0.7 + 0.3) * mix(0.35, 1.0, sqrt(h));
            vec3 in_scattered = light_radiance * energy + ambient;
            // Rayo: la nube se ilumina por dentro alrededor de la descarga
            // (y un poco toda la capa, por la luz que rebota dentro).
            if (push.flash.w > 0.0) {
                vec3 strike = vec3(push.flash.x, kEarthRadius + cloudBottom() + 300.0, push.flash.z);
                float d = length(p - strike);
                in_scattered += vec3(0.75, 0.8, 1.0) * push.flash.w * (exp(-d / 1400.0) * 6.0 + 0.35);
            }

            // Integracion exacta del tramo (Hillaire 2015): estable aunque el
            // paso sea largo frente a la extincion.
            float step_transmittance = exp(-extinction * fine_step);
            float absorbed = transmittance * (1.0 - step_transmittance);
            scattered += in_scattered * absorbed;
            weighted_distance += t * absorbed;
            weight_sum += absorbed;
            transmittance *= step_transmittance;
        } else if (++empty_steps > 6) {
            searching = true;  // otra vez aire: pasos largos
        }
        t += fine_step;
    }

    // Perspectiva aerea: las nubes lejanas se funden con el cielo del
    // horizonte (por la distancia de lo que se ve, no de la entrada a la capa).
    float seen = weight_sum > 1e-4 ? weighted_distance / weight_sum : t_start;
    float fade = exp(-seen / 38000.0);
    scattered *= fade;
    transmittance = mix(1.0, transmittance, fade);

    out_clouds = vec4(scattered, transmittance);
}
