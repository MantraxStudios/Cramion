#version 450

// Fuego y humo volumetricos (FirePass): un rayo por pixel a traves de la caja
// de cada zona de fuego, hasta lo que hay en la escena (depth). Se dibuja
// sobre la imagen HDR ya iluminada con mezcla premultiplicada:
//   color = luz del volumen + lo de detras x transmitancia.
//
//   - Llamas: emisivas, color de cuerpo negro segun la temperatura, con un
//     ruido turbulento que sube; mas altas donde hay mas calor y tumbadas por
//     el viento.
//   - Humo: la densidad sale del mapa de humo de la zona en el punto de donde
//     vino ese humo (el viento lo arrastra mientras sube); arriba usa el mapa
//     mas difuminado (la columna se ensancha). Ruido fbm que sube y deriva.
//     Luz del sol con unos pasos de sombra hacia el sol, la del cielo y el
//     resplandor naranja del fuego de debajo.
//
// Los pasos son largos en el aire vacio y cortos dentro de las llamas. El
// desplazamiento inicial cambia en cada pixel y frame (ruido de gradiente
// entrelazado) y el TAA lo promedia.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
} camera;

// GpuFire (FirePass.cpp).
layout(set = 0, binding = 1) uniform FireBuffer {
    vec4 box_min[4];      // xyz = caja, w = activa
    vec4 box_max[4];      // xyz = caja, w = altura de referencia del mapa de alturas
    vec4 rect[4];         // xy = esquina minima (x, z), z = lado (m), w = parte del mapa usada
    vec4 flame[4];        // x = altura de las llamas, y = intensidad, z = altura del humo, w = densidad
    vec4 wind[4];         // xy = viento (m/s), z = subida del humo (m/s), w = celda (m)
    vec4 smoke_color[4];  // rgb
    vec4 sun_direction;   // xyz = hacia el sol
    vec4 sun_color;       // rgb = color x intensidad
    vec4 ambient;         // rgb = luz del cielo
    vec4 params;          // x = segundos, y = frame, z = zonas, w = 1 / lado del mapa
} fire;

layout(set = 0, binding = 2) uniform sampler2D g_depth;
// r = carbonizado, g = calor, b = humo, a = humo ancho. Capa = zona.
layout(set = 0, binding = 3) uniform sampler2DArray fire_map;
layout(set = 0, binding = 4) uniform sampler2DArray fire_height;  // suelo - altura de referencia
layout(set = 0, binding = 5) uniform sampler2D noise_texture;

const int kMaxSteps = 120;
const float kPi = 3.14159265;

float linearDepth(float depth) {
    return camera.projection[3][2] / (depth + camera.projection[2][2]);
}

// Ruido de valor 3D con dos lecturas de una textura 2D (capas desplazadas
// (37, 17) texeles: G es R desplazado).
float noise3(vec3 x) {
    vec3 p = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    vec2 uv = p.xy + vec2(37.0, 17.0) * p.z + f.xy;
    vec2 rg = textureLod(noise_texture, (uv + 0.5) / 256.0, 0.0).yx;
    return mix(rg.x, rg.y, f.z);
}

float interleavedGradientNoise(vec2 pixel, float frame) {
    pixel += 5.588238 * mod(frame, 64.0);
    return fract(52.9829189 * fract(0.06711056 * pixel.x + 0.00583715 * pixel.y));
}

// Mapa de la zona en un punto del suelo (0 fuera de la zona).
vec4 mapAt(int z, vec2 xz) {
    vec4 r = fire.rect[z];
    vec2 uv = (xz - r.xy) / r.z;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return vec4(0.0);
    }
    float half_texel = fire.params.w * 0.5;
    vec2 tuv = clamp(uv * r.w, vec2(half_texel), vec2(r.w - half_texel));
    // Se desvanece en el ultimo 4 % de la zona: sin corte recto en su borde.
    vec2 edge = min(uv, 1.0 - uv);
    float fade = smoothstep(0.0, 0.04, min(edge.x, edge.y));
    return textureLod(fire_map, vec3(tuv, float(z)), 0.0) * fade;
}

float groundAt(int z, vec2 xz) {
    vec4 r = fire.rect[z];
    float half_texel = fire.params.w * 0.5;
    vec2 tuv = clamp((xz - r.xy) / r.z * r.w, vec2(half_texel), vec2(r.w - half_texel));
    return textureLod(fire_height, vec3(tuv, float(z)), 0.0).r + fire.box_max[z].w;
}

// Cuerpo negro aproximado (t = temperatura 0..1): rojo oscuro, naranja,
// amarillo y casi blanco.
vec3 blackbody(float t) {
    vec3 c = mix(vec3(0.45, 0.02, 0.0), vec3(1.0, 0.22, 0.02), smoothstep(0.0, 0.35, t));
    c = mix(c, vec3(1.0, 0.5, 0.1), smoothstep(0.3, 0.65, t));
    return mix(c, vec3(1.0, 0.82, 0.5), smoothstep(0.65, 1.0, t));
}

// Cuanto arrastro el viento el humo que esta a `hg` m sobre el suelo. Una
// columna de humo real sube recta cerca del fuego (el aire caliente sube
// rapido, ~8-10 m/s) y se va tumbando con la altura, al enfriarse y frenar:
// el desvio crece con el cuadrado de la altura (antes, lineal desde el suelo:
// el humo salia en diagonal y se veia como un arco hacia un lado).
vec2 smokeDrift(float hg, float smoke_h, vec2 wind, float rise) {
    float h = max(hg, 0.0);
    float h01 = clamp(h / max(smoke_h, 1.0), 0.0, 1.0);
    float buoyant_rise = max(rise, 0.2) * 2.5;  // la subida del penacho, mas rapida que la del humo frio
    return wind * (h / buoyant_rise) * h01;
}

// Humo (sin ruido) a una altura sobre el suelo: para la sombra hacia el sol.
float smokeCoarse(int z, vec3 p, float ground, float smoke_h, vec2 wind, float rise) {
    float hg = p.y - ground;
    if (hg < 0.3 || hg > smoke_h) return 0.0;
    vec4 s = mapAt(z, p.xz - smokeDrift(hg, smoke_h, wind, rise));
    float h01 = hg / smoke_h;
    return mix(s.b, s.a, smoothstep(0.0, 0.5, h01)) * pow(1.0 - h01, 1.2);
}

void march(int z, vec3 ro, vec3 rd, float t_start, float t_end, float jitter, inout vec3 radiance,
           inout float transmittance) {
    float flame_h = fire.flame[z].x;
    float intensity = fire.flame[z].y;
    float smoke_h = fire.flame[z].z;
    float density = fire.flame[z].w;
    vec2 wind = fire.wind[z].xy;
    float rise = max(fire.wind[z].z, 0.2);
    float cell = fire.wind[z].w;
    float seconds = fire.params.x;
    vec3 to_sun = fire.sun_direction.xyz;
    vec3 sun = fire.sun_color.rgb;
    vec3 sky = fire.ambient.rgb;
    vec3 smoke_albedo = fire.smoke_color[z].rgb;
    // Fase: algo de dispersion hacia delante (el humo a contraluz brilla).
    float cos_sun = dot(rd, to_sun);
    float g = 0.45;
    float hg_phase = (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * cos_sun, 1e-4), 1.5);
    float phase = mix(1.0, hg_phase, 0.6);

    float segment = t_end - t_start;
    float coarse = clamp(segment / 48.0, 0.5, 10.0);
    float t = t_start + coarse * jitter;
    float flame_top = flame_h * 1.25;
    for (int i = 0; i < kMaxSteps; ++i) {
        if (t >= t_end || transmittance < 0.01) {
            break;
        }
        vec3 p = ro + rd * t;
        float ground = groundAt(z, p.xz);
        float hg = p.y - ground;
        float ds = coarse;
        // Lejos, pasos finos mas largos (las llamas ocupan menos pixeles).
        float fine = max(0.28 * max(1.0, t / 35.0), cell * 0.35);

        // --- Llamas ---
        if (hg < flame_top && hg > -0.6) {
            vec2 lean = wind * (max(hg, 0.0) * 0.05);
            vec4 m = mapAt(z, p.xz - lean);
            float heat = m.g;
            if (heat > 0.015) {
                ds = fine;
                // Cada mata de llamas con su altura (no un muro parejo).
                float column = noise3(vec3(p.xz * 0.35, seconds * 0.35));
                float local_h = flame_h * (0.3 + 0.7 * heat) * (0.45 + 0.9 * column);
                float y = max(hg, 0.0) / local_h;
                vec3 q = vec3(p.x, hg, p.z);
                float n = noise3(q * vec3(0.5, 0.32, 0.5) - vec3(0.0, seconds * 1.5, 0.0)) * 0.62 +
                          noise3(q * vec3(1.5, 0.95, 1.5) - vec3(0.0, seconds * 3.4, 0.0)) * 0.38;
                // Lenguas: huecos entre llamas que suben con ellas.
                float tongue = noise3(vec3(p.x * 0.9, hg * 0.35 - seconds * 2.2, p.z * 0.9));
                tongue = smoothstep(0.32, 0.68, tongue + 0.25 * (1.0 - y));
                float flame = clamp(heat * 1.3 - y * (0.45 + 1.0 * n) - (n - 0.5) * 0.45, 0.0, 1.0) * tongue;
                if (flame > 0.0) {
                    float temperature = clamp(flame * (1.15 - 0.55 * y), 0.0, 1.0);
                    vec3 emission = blackbody(temperature) * (temperature * temperature * 6.0 + 0.4 * temperature) *
                                    intensity;
                    float sigma = flame * 0.6 + 0.02;
                    float step_t = exp(-sigma * ds);
                    radiance += transmittance * emission * (1.0 - step_t) / sigma;
                    transmittance *= step_t;
                }
            } else if (m.a < 0.004 && m.b < 0.004) {
                // Sin fuego cerca: se puede saltar.
                ds = coarse;
            } else {
                ds = min(coarse, max(cell * 1.2, fine));
            }
        } else if (rd.y < 0.0 && hg >= flame_top) {
            // Bajando hacia el suelo: no pasarse la capa de las llamas.
            ds = min(coarse, (hg - flame_top) / -rd.y + fine * 0.5);
        }

        // --- Humo ---
        if (hg > 0.3 && hg < smoke_h) {
            float h01 = hg / smoke_h;
            vec2 source = p.xz - smokeDrift(hg, smoke_h, wind, rise);
            // Columna irregular: el punto de origen se tuerce con la altura.
            vec2 warp = vec2(noise3(vec3(p.xz * 0.04, hg * 0.05 + 3.1)), noise3(vec3(p.zx * 0.04, hg * 0.05 + 9.7))) - 0.5;
            source += warp * (2.0 + hg * 0.6);
            vec4 s = mapAt(z, source);
            float smoke = mix(s.b, s.a, smoothstep(0.0, 0.5, h01));
            if (smoke > 0.003) {
                vec3 q = p * 0.055 - vec3(wind.x, rise * 1.2, wind.y) * (seconds * 0.055);
                float n = noise3(q) * 0.58 + noise3(q * 2.63 + 7.1) * 0.29 + noise3(q * 6.1 + 3.7) * 0.13;
                float shape = smoothstep(0.0, 1.0, clamp(n * 2.4 - 1.0 + smoke * 0.5, 0.0, 1.0));
                float d = smoke * density * shape * smoothstep(0.3, 2.5, hg) * pow(1.0 - h01, 1.2);
                if (d > 0.0005) {
                    float sigma = d * 0.3;
                    // Sombra hacia el sol (tres pasos largos sin ruido).
                    float toward = 0.0;
                    if (to_sun.y > -0.05) {
                        for (int k = 1; k <= 3; ++k) {
                            vec3 sp = p + to_sun * (float(k) * float(k) * 3.0);
                            toward += smokeCoarse(z, sp, ground, smoke_h, wind, rise) * float(k) * 3.0;
                        }
                    }
                    float sun_t = exp(-toward * density * 0.3 * 0.7);
                    // Resplandor del fuego de debajo (de noche, el humo naranja).
                    float glow_amount = (s.g * 1.5 + s.a * 0.2) * exp(-hg / (flame_h * 1.5 + 3.0));
                    vec3 glow = vec3(1.0, 0.36, 0.07) * glow_amount * intensity * 0.9;
                    vec3 lit = smoke_albedo * (sun * sun_t * phase + sky * (0.7 + 0.3 * h01)) + glow;
                    float step_t = exp(-sigma * ds);
                    radiance += transmittance * lit * (1.0 - step_t);
                    transmittance *= step_t;
                }
            }
        }
        t += max(ds, 0.05);
    }
}

void main() {
    vec2 ndc = v_uv * 2.0 - 1.0;
    // Con el desplazamiento del centro (ojos de VR asimetricos, jitter del TAA).
    vec3 view_ray = vec3((ndc.x + camera.projection[2][0]) / camera.projection[0][0],
                         (ndc.y + camera.projection[2][1]) / camera.projection[1][1], -1.0);
    vec3 ray = transpose(mat3(camera.view)) * view_ray;
    float ray_scale = length(ray);
    vec3 rd = ray / ray_scale;
    vec3 ro = camera.position.xyz;
    float depth = textureLod(g_depth, v_uv, 0.0).r;
    float scene_t = depth >= 0.999999 ? 1e9 : linearDepth(depth) * ray_scale;

    vec3 radiance = vec3(0.0);
    float transmittance = 1.0;
    float jitter = interleavedGradientNoise(gl_FragCoord.xy, fire.params.y);
    vec3 inv = 1.0 / mix(rd, vec3(1e-6), lessThan(abs(rd), vec3(1e-6)));
    for (int z = 0; z < 4; ++z) {
        if (fire.box_min[z].w < 0.5) {
            continue;
        }
        vec3 a = (fire.box_min[z].xyz - ro) * inv;
        vec3 b = (fire.box_max[z].xyz - ro) * inv;
        vec3 near = min(a, b);
        vec3 far = max(a, b);
        float enter = max(max(near.x, near.y), max(near.z, 0.0));
        float leave = min(min(far.x, far.y), min(far.z, scene_t));
        if (leave <= enter) {
            continue;
        }
        march(z, ro, rd, enter, leave, jitter, radiance, transmittance);
    }
    out_color = vec4(radiance, transmittance);
}
