#version 450

// Cielo fisico: dispersion atmosferica simple (Rayleigh + Mie + ozono) sobre
// un planeta del tamano de la Tierra, en la linea del "Sky Atmosphere" de
// Unreal (Hillaire, EGSR 2020), sin sus LUT de transmitancia ni de
// dispersion multiple.
//
// Se calcula cada frame sobre una textura pequena (256x128) con la radiancia
// del cielo en todas las direcciones; la pasada de iluminacion la consulta
// para pintar el cielo, la niebla, los reflejos y la luz ambiente. Por eso el
// azul del mediodia, el blanco del horizonte y el naranja/rosa del ocaso
// salen de la fisica, no de colores puestos a mano.
//
// Parametrizacion: u = azimut (0..1 = -pi..pi), v = elevacion con mas
// resolucion cerca del horizonte (v = 0.5 es el horizonte). Debe coincidir
// con skyLutUv() de lighting.frag.

layout(push_constant) uniform PushConstants {
    vec4 sun;   // xyz = hacia el sol,  w = iluminancia del sol
    vec4 moon;  // xyz = hacia la luna, w = iluminancia de la luna
    // Color del cielo del componente Sky (GpuSkyPush): x = mezcla con el
    // degradado (0 = solo fisico), y = curva, z = numero de paradas (0 = tres
    // colores: cenit, horizonte, suelo), w = brillo del halo del sol.
    vec4 custom;
    vec4 sun_tint;      // rgb = tinte del halo del sol (lineal)
    uvec4 colors[4];    // 8 colores en media precision (rgb + altura en grados)
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

const float kPi = 3.14159265;

// Unidades: kilometros.
const float kGroundRadius = 6360.0;
const float kAtmosphereRadius = 6460.0;
const float kViewerHeight = 0.2;

const vec3 kRayleighScattering = vec3(5.802, 13.558, 33.1) * 1.0e-3;
const float kRayleighScale = 8.0;
const float kMieScattering = 3.996e-3;
const float kMieExtinction = 4.440e-3;
const float kMieScale = 1.2;
const float kMieG = 0.80;
const vec3 kOzoneAbsorption = vec3(0.650, 1.881, 0.085) * 1.0e-3;

// La luz que rebota varias veces en el aire (la que ilumina el cielo del
// crepusculo cuando el sol ya se ha puesto) se aproxima con un termino
// isotropo proporcional a la dispersion simple.
const float kMultipleScattering = 0.6;

const int kViewSteps = 32;
const int kLightSteps = 8;

// Distancia a la esfera de radio `radius` (centrada en el origen) desde `o`
// en la direccion `d`; -1 si no la corta por delante.
float raySphere(vec3 o, vec3 d, float radius) {
    float b = dot(o, d);
    float c = dot(o, o) - radius * radius;
    float discriminant = b * b - c;
    if (discriminant < 0.0) {
        return -1.0;
    }
    float s = sqrt(discriminant);
    float t0 = -b - s;
    float t1 = -b + s;
    if (t0 > 0.0) {
        return t0;
    }
    return t1 > 0.0 ? t1 : -1.0;
}

// rgb = coeficiente de extincion total, a = altura (para las densidades).
void mediumAt(vec3 p, out vec3 rayleigh, out float mie, out vec3 extinction) {
    float height = length(p) - kGroundRadius;
    float rayleigh_density = exp(-height / kRayleighScale);
    float mie_density = exp(-height / kMieScale);
    float ozone_density = max(0.0, 1.0 - abs(height - 25.0) / 15.0);

    rayleigh = kRayleighScattering * rayleigh_density;
    mie = kMieScattering * mie_density;
    extinction = rayleigh + kMieExtinction * mie_density + kOzoneAbsorption * ozone_density;
}

// Transmitancia desde `p` hasta fuera de la atmosfera hacia `direction`.
vec3 transmittanceToSpace(vec3 p, vec3 direction) {
    if (raySphere(p, direction, kGroundRadius) > 0.0) {
        return vec3(0.0);  // el planeta tapa la luz
    }
    float distance_out = raySphere(p, direction, kAtmosphereRadius);
    float dt = distance_out / float(kLightSteps);
    vec3 optical_depth = vec3(0.0);
    for (int i = 0; i < kLightSteps; ++i) {
        vec3 q = p + direction * (dt * (float(i) + 0.5));
        vec3 rayleigh;
        float mie;
        vec3 extinction;
        mediumAt(q, rayleigh, mie, extinction);
        optical_depth += extinction * dt;
    }
    return exp(-optical_depth);
}

float rayleighPhase(float cosine) {
    return 3.0 / (16.0 * kPi) * (1.0 + cosine * cosine);
}

// Cornette-Shanks: Henyey-Greenstein mejorada, el halo brillante alrededor
// del sol que deja la bruma.
float miePhase(float cosine) {
    float g2 = kMieG * kMieG;
    float k = 3.0 / (8.0 * kPi) * (1.0 - g2) / (2.0 + g2);
    return k * (1.0 + cosine * cosine) / pow(1.0 + g2 - 2.0 * kMieG * cosine, 1.5);
}

vec3 directionFromUv(vec2 uv) {
    float azimuth = (uv.x - 0.5) * 2.0 * kPi;
    float s = (uv.y - 0.5) * 2.0;
    float elevation = sign(s) * s * s * 0.5 * kPi;
    return vec3(cos(elevation) * cos(azimuth), sin(elevation), cos(elevation) * sin(azimuth));
}

vec3 integrate(vec3 origin, vec3 direction) {
    float distance_to_ground = raySphere(origin, direction, kGroundRadius);
    float distance_to_top = raySphere(origin, direction, kAtmosphereRadius);
    float max_distance = distance_to_ground > 0.0 ? distance_to_ground : distance_to_top;

    float cos_sun = dot(direction, push.sun.xyz);
    float cos_moon = dot(direction, push.moon.xyz);
    float phase_r_sun = rayleighPhase(cos_sun);
    float phase_m_sun = miePhase(cos_sun);
    float phase_r_moon = rayleighPhase(cos_moon);
    float phase_m_moon = miePhase(cos_moon);
    const float kIsotropic = 1.0 / (4.0 * kPi);

    vec3 radiance = vec3(0.0);
    vec3 transmittance = vec3(1.0);
    float dt = max_distance / float(kViewSteps);

    for (int i = 0; i < kViewSteps; ++i) {
        vec3 p = origin + direction * (dt * (float(i) + 0.5));
        vec3 rayleigh;
        float mie;
        vec3 extinction;
        mediumAt(p, rayleigh, mie, extinction);

        vec3 sun_light = transmittanceToSpace(p, push.sun.xyz) * push.sun.w;
        vec3 moon_light = transmittanceToSpace(p, push.moon.xyz) * push.moon.w;

        vec3 scattering =
            sun_light * (rayleigh * phase_r_sun + mie * phase_m_sun) +
            moon_light * (rayleigh * phase_r_moon + mie * phase_m_moon) +
            (sun_light + moon_light) * (rayleigh + mie) * kIsotropic * kMultipleScattering;

        // Integracion analitica del tramo (estable con pasos largos).
        vec3 step_transmittance = exp(-extinction * dt);
        vec3 integral = (scattering - scattering * step_transmittance) / max(extinction, vec3(1e-7));
        radiance += transmittance * integral;
        transmittance *= step_transmittance;
    }
    return radiance;
}

// Color i del degradado: rgb (lineal, ya escalado) y su altura en grados.
vec4 gradientColor(int i) {
    uvec4 pair = push.colors[i >> 1];
    uvec2 packed_color = (i & 1) == 0 ? pair.xy : pair.zw;
    return vec4(unpackHalf2x16(packed_color.x), unpackHalf2x16(packed_color.y));
}

// Cielo de colores puestos a mano (componente Sky): un degradado por la
// altura de la direccion y un halo alrededor del sol (el disco lo pinta
// lighting.frag).
vec3 customSky(vec3 direction) {
    float h = clamp(direction.y, -1.0, 1.0);
    int stops = int(push.custom.z + 0.5);
    vec3 color;
    if (stops <= 0) {
        // Cenit, horizonte y suelo. Curva 1 = lineal; mas = el color del
        // cenit baja hasta cerca del horizonte (franja fina, como el fisico).
        vec3 zenith = gradientColor(0).rgb;
        vec3 horizon = gradientColor(1).rgb;
        vec3 ground = gradientColor(2).rgb;
        float curve = max(push.custom.y, 0.01);
        if (h >= 0.0) {
            color = mix(horizon, zenith, 1.0 - pow(1.0 - h, curve));
        } else {
            color = mix(horizon, ground, 1.0 - pow(1.0 + h, curve * 4.0));
        }
    } else {
        // Paradas ordenadas por altura (grados).
        float elevation = degrees(asin(h));
        vec4 previous = gradientColor(0);
        color = previous.rgb;
        for (int i = 1; i < 8; ++i) {
            if (i >= stops) {
                break;
            }
            vec4 next = gradientColor(i);
            if (elevation >= next.a) {
                color = next.rgb;
            } else if (elevation > previous.a) {
                color = mix(previous.rgb, next.rgb, (elevation - previous.a) / max(next.a - previous.a, 1e-3));
            }
            previous = next;
        }
    }

    // Halo del sol: dos lobulos (uno ancho y otro cerrado), mas fuerte con el
    // sol bajo, que se apaga cuando el sol se pone.
    float halo = push.custom.w;
    if (halo > 0.0) {
        float c = max(dot(direction, push.sun.xyz), 0.0);
        float sun_up = smoothstep(-0.08, 0.05, push.sun.y);
        float low_sun = mix(1.6, 1.0, smoothstep(0.0, 0.4, push.sun.y));
        float glow = pow(c, 8.0) * 0.12 + pow(c, 64.0) * 0.6 + pow(c, 512.0) * 2.0;
        color += push.sun_tint.rgb * glow * halo * sun_up * low_sun;
    }
    return color;
}

void main() {
    vec3 direction = directionFromUv(v_uv);
    vec3 origin = vec3(0.0, kGroundRadius + kViewerHeight, 0.0);
    float blend = clamp(push.custom.x, 0.0, 1.0);
    vec3 radiance = vec3(0.0);
    if (blend < 1.0) {
        radiance = integrate(origin, direction);
        // Tinte del sol (componente Sky) en el fisico: solo el halo, por la
        // fase de Mie hacia el sol (blanco = sin cambio).
        if (any(notEqual(push.sun_tint.rgb, vec3(1.0)))) {
            float toward_sun = smoothstep(0.6, 1.0, dot(direction, push.sun.xyz));
            radiance *= mix(vec3(1.0), push.sun_tint.rgb, toward_sun);
        }
    }
    if (blend > 0.0) {
        radiance = mix(radiance, customSky(direction), blend);
    }
    out_color = vec4(min(radiance, vec3(60000.0)), 1.0);
}
