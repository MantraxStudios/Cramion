#version 450
#extension GL_GOOGLE_include_directive : require

// Agua (oceano, lagos y rios), como Crest:
//
//   - Normal: oceano, la del oleaje FFT (derivadas de las 4 cascadas, con
//     mipmaps: lo que no cabe en el pixel pasa a rugosidad); lagos y rios,
//     Gerstner por pixel + ondulacion fina del viento, que en el rio corre
//     con la corriente (flow map en dos fases, mas rapida en los rapidos).
//   - Grosor del agua (profundidad de la escena detras): el fondo se ve a
//     traves, cada vez mas tenido y oscuro (absorcion de Beer-Lambert por
//     color y luz dispersada), refractado por la normal, con causticas.
//   - Reflejo: trazado en pantalla y, si no, la sonda o el cielo (con las
//     nubes de la pantalla); Fresnel del agua y el brillo del sol (GGX) con
//     sombra.
//   - Luz a traves de las crestas (subsurface) a contraluz.
//   - Espuma: orilla, olas que rompen en la playa, crestas del oceano (la
//     que deja el jacobiano del FFT y se deshace poco a poco), orillas y
//     rapidos del rio, salpicaduras.
//   - Desde abajo: ventana de Snell (lo de fuera refractado, Fresnel exacto
//     agua -> aire) y reflexion total fuera de ella, sin espuma de orilla
//     (antes la orilla salia en todo: el "blanco" al bucear), atenuado por
//     el agua hasta la camara.

layout(set = 2, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
} camera;

struct PointLightGpu {
    vec4 position_range;
    vec4 color_intensity;
    vec4 shadow;
};
struct SpotLightGpu {
    vec4 position_range;
    vec4 direction_intensity;
    vec4 color_inner;
    vec4 outer_shadow;
};
layout(set = 2, binding = 1) uniform LightBuffer {
    vec4 sun_direction_intensity;
    vec4 sun_color_ambient;
    vec4 ambient_color;
    vec4 sky_sun;
    vec4 sky_moon;
    ivec4 counts;
    PointLightGpu points[32];
    SpotLightGpu spots[8];
    vec4 probes[2];
    vec4 clouds;
    vec4 environment;  // y = 1 si hay luz volumetrica
} lights;

const int kShadowCascadeCount = 4;
layout(set = 2, binding = 2) uniform ShadowBuffer {
    mat4 light_view_projection[kShadowCascadeCount];
    vec4 split_distances;
    vec4 texel_world_sizes;
    vec4 params;
} shadows;
layout(set = 2, binding = 3) uniform sampler2DArrayShadow shadow_map;
layout(set = 2, binding = 4) uniform sampler2D g_depth;
layout(set = 2, binding = 5) uniform sampler2D scene_color;
layout(set = 2, binding = 6) uniform samplerCube environment_map;
layout(set = 2, binding = 7) uniform samplerCube reflection_probe_0;
layout(set = 2, binding = 8) uniform samplerCube reflection_probe_1;
// Luz volumetrica (volumetric.frag, media resolucion): rgb = luz dispersada,
// a = transmitancia, integradas hasta lo opaco (o 60 m).
layout(set = 2, binding = 9) uniform sampler2D volumetric_map;

// Niebla por altura: MISMOS valores que lighting.frag, para que el agua quede
// dentro de la niebla como todo lo demas (no "por encima").
const float kFogDensity = 0.0018;
const float kFogBaseHeight = 0.0;
const float kFogHeightFalloff = 0.08;
const float kVolumetricDistance = 60.0;  // push.params.w de volumetric.frag

#include "water_common.glsl"

layout(location = 0) in vec3 v_world_position;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec2 v_flow;
layout(location = 4) in vec2 v_grid;
layout(location = 5) in float v_jacobian;
layout(location = 6) in float v_height;
layout(location = 7) flat in vec4 v_hole;
layout(location = 8) in float v_slope;

layout(location = 0) out vec4 out_color;

const float kMaxRadiance = 40.0;
const float kSunDiskRadiance = 900.0;
// Lo que se ve desde abajo por la ventana de Snell: tope de su brillo (el
// cielo HDR entero, con la exposicion de bajo el agua, quemaba en blanco).
const float kWindowRadiance = 12.0;
const float kWaterIor = 1.333;

vec3 toLinear(vec3 c) { return pow(c, vec3(2.2)); }

// --- Ruido ---
// Hash entero (PCG) de la celda: estable con coordenadas grandes del mundo.
uint pcg(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
uint cellHash(ivec2 c) { return pcg(uint(c.x) ^ pcg(uint(c.y) + 0x9E3779B9u)); }
float hash(ivec2 c) { return float(cellHash(c)) * (1.0 / 4294967295.0); }
float noise(vec2 p) {
    vec2 cell = floor(p);
    ivec2 i = ivec2(cell);
    vec2 f = p - cell;
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + ivec2(1, 0)), u.x), mix(hash(i + ivec2(0, 1)), hash(i + ivec2(1, 1)), u.x), u.y);
}
// fbm filtrado: las octavas mas finas que el pixel (`footprint`, en unidades
// de p) se sustituyen por su media en vez de parpadear.
float fbm(vec2 p, float footprint) {
    float v = 0.0;
    float a = 0.5;
    float frequency = 1.0;
    for (int i = 0; i < 4; ++i) {
        float keep = 1.0 - smoothstep(0.25, 0.5, frequency * footprint);
        v += a * mix(0.5, noise(p), keep);
        p = mat2(1.6, 1.2, -1.2, 1.6) * p;
        a *= 0.5;
        frequency *= 2.0;
    }
    return v;
}

// Espuma: vetas y burbujas de ruido deformado (domain warping), que se estira
// con el agua. Devuelve la densidad 0..1; la cobertura decide cuanta se ve.
// `footprint` = metros por pixel.
float foamDensity(vec2 p, float t, float footprint) {
    vec2 q = p * 0.35;
    vec2 warp = vec2(fbm(q + vec2(t * 0.020, 0.0), footprint * 0.35),
                     fbm(q + vec2(5.2, 1.3) - vec2(0.0, t * 0.015), footprint * 0.35)) - 0.5;
    float streaks = fbm(p * 0.9 + warp * 3.0 + t * 0.03, footprint * 0.9);
    float bubbles = fbm(p * 4.2 + warp * 1.5 - t * 0.05, footprint * 4.2);
    // Huecos redondeados en la espuma (se deshace en agujeros, no en celdas).
    float holes = smoothstep(0.35, 0.65, fbm(p * 2.1 - warp * 2.0 + t * 0.02, footprint * 2.1));
    return clamp(streaks * 0.85 + bubbles * 0.35 - holes * 0.25 - 0.05, 0.0, 1.0);
}

// Filtro de una onda de longitud `wavelength` para un pixel de `footprint` m:
// 1 si se ve bien, 0 si ya es mas fina que dos pixeles (Nyquist).
float waveKeep(float wavelength, float footprint) {
    return 1.0 - smoothstep(0.5, 1.0, 2.5 * footprint / wavelength);
}

// Oleaje Gerstner por pixel (la misma formula que water_common.glsl). Las
// ondas mas finas que el pixel se quitan y su pendiente pasa a
// `lost_variance` (rugosidad). `jacobian` solo cuenta las olas largas.
vec3 gerstnerNormal(WaterBody b, vec2 p, float t, float footprint, out float jacobian, out float lost_variance) {
    vec3 n = vec3(0.0, 1.0, 0.0);
    jacobian = 1.0;
    lost_variance = 0.0;
    float peak = max(b.waves.y, 0.05);
    float steep = clamp(b.waves.w, 0.0, 1.0);
    for (int i = 0; i < kWaveCount; ++i) {
        float len = peak * kWaveLengths[i];
        float amplitude = b.waves.x * kWaveAmplitudes[i];
        float keep = waveKeep(len, footprint);
        if (keep <= 0.0) {
            float lost_slope = 2.0 * kWaterPi / len * amplitude;
            lost_variance += 0.5 * lost_slope * lost_slope;
            continue;
        }
        float angle = b.wind.x + kWaveDirections[i] * b.wind.y;
        vec2 d = vec2(cos(angle), sin(angle));
        float k = 2.0 * kWaterPi / len;
        float omega = sqrt(9.81 * k) * b.waves.z;
        float q = amplitude > 1e-6 ? steep / (k * amplitude * float(kWaveCount)) : 0.0;
        float theta = k * dot(d, p) - omega * t + kWavePhases[i];
        float a = amplitude * keep;
        float c = cos(theta);
        float s = sin(theta);
        n.xz -= d * k * a * c;
        n.y -= q * k * a * s;
        if (i < 14) jacobian -= q * k * amplitude * s;
        float slope = k * amplitude;
        lost_variance += 0.5 * slope * slope * (1.0 - keep * keep);
    }
    return normalize(n);
}

// Oceano FFT por pixel: normal de la superficie desplazada (pendiente /
// (1 + compresion)), jacobiano (crestas que rompen) y la espuma que dura.
// La pendiente de las ondas que el mipmap promedia pasa a `lost_variance`.
vec3 oceanNormal(vec2 p, float footprint, out float jacobian, out float foam, out float lost_variance) {
    vec4 d = vec4(0.0);
    foam = 0.0;
    lost_variance = 0.0;
    for (int c = 0; c < 4; ++c) {
        vec3 uv = oceanUv(p, c);
        d += texture(ocean_derivatives, uv);
        foam += texture(ocean_displacement, uv).w;
        // Texels de esta cascada por pixel: cada mip pierde la octava de
        // arriba (las tres grandes no tienen energia por encima de ~15 ondas
        // por lado; la ultima llega hasta el final).
        float lod = log2(max(footprint * kOceanTexels / water.cascade_size[c], 1e-6));
        lost_variance += water.cascade_slope[c] * smoothstep(c == 3 ? 0.0 : 1.5, 4.5, lod);
    }
    vec2 slope = d.xy / max(vec2(1.0) + d.zw, vec2(0.2));
    jacobian = (1.0 + d.z) * (1.0 + d.w);
    foam = clamp(foam, 0.0, 1.0);
    return normalize(vec3(-slope.x, 1.0, -slope.y));
}

// --- Pantalla ---
float linearDepth(float depth) { return camera.projection[3][2] / (depth + camera.projection[2][2]); }

vec3 worldFromDepth(vec2 uv, float depth) {
    vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 world = camera.inverse_view_projection * clip;
    return world.xyz / world.w;
}

bool project(vec3 view_position, out vec2 uv) {
    vec4 clip = camera.projection * vec4(view_position, 1.0);
    if (clip.w <= 0.0) {
        uv = vec2(-1.0);
        return false;
    }
    uv = clip.xy / clip.w * 0.5 + 0.5;
    return true;
}
bool insideScreen(vec2 uv) { return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0))); }

// El cielo que se ve en pantalla en una direccion (un punto en el infinito):
// asi el reflejo lleva las nubes volumetricas y el color real del cielo.
// a = confianza (0 si esa parte del cielo no esta en pantalla o la tapa algo).
vec4 screenSky(vec3 direction) {
    vec4 clip = camera.view_projection * vec4(direction, 0.0);
    if (clip.w <= 1e-4) return vec4(0.0);
    vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
    if (!insideScreen(uv)) return vec4(0.0);
    if (textureLod(g_depth, uv, 0.0).r < 1.0) return vec4(0.0);  // tapado: no es cielo
    vec2 edge = min(uv, 1.0 - uv);
    return vec4(min(textureLod(scene_color, uv, 0.0).rgb, vec3(kMaxRadiance)), smoothstep(0.0, 0.08, min(edge.x, edge.y)));
}

// Reflejo en pantalla (como el vidrio): rgb = color, a = confianza.
vec4 traceScreen(vec3 origin, vec3 ray) {
    const int kSteps = 32;
    const float kMaxDistance = 120.0;
    const float kFirstStep = 0.08;
    ivec2 size = textureSize(g_depth, 0);
    float growth = pow(kMaxDistance / kFirstStep, 1.0 / float(kSteps));
    // Ruido que cambia cada frame (el TAA lo promedia).
    vec2 noise_pixel = gl_FragCoord.xy + 5.588238 * mod(floor(water.time_count.x * 60.0), 64.0);
    float jitter = fract(52.9829189 * fract(dot(noise_pixel, vec2(0.06711056, 0.00583715))));
    float previous_distance = 0.0;
    float distance_along = kFirstStep * mix(1.0, growth, jitter);
    for (int i = 0; i < kSteps; ++i) {
        vec3 sample_position = origin + ray * distance_along;
        vec2 uv;
        if (!project(sample_position, uv) || !insideScreen(uv)) break;
        ivec2 texel = min(ivec2(uv * vec2(size)), size - 1);
        float scene_depth = texelFetch(g_depth, texel, 0).r;
        float difference = -sample_position.z - linearDepth(scene_depth);
        float thickness = (distance_along - previous_distance) * 1.5 + 0.1;
        if (scene_depth < 1.0 && difference > 0.0 && difference < thickness) {
            vec3 color = min(textureLod(scene_color, uv, 0.0).rgb, vec3(kMaxRadiance));
            vec2 edge = min(uv, 1.0 - uv);
            float edge_fade = smoothstep(0.0, 0.1, min(edge.x, edge.y));
            float distance_fade = 1.0 - smoothstep(0.6, 1.0, distance_along / kMaxDistance);
            return vec4(color, edge_fade * distance_fade);
        }
        previous_distance = distance_along;
        distance_along *= growth;
    }
    return vec4(0.0);
}

float sunShadow(vec3 world_position) {
    float view_depth = -(camera.view * vec4(world_position, 1.0)).z;
    int cascade = kShadowCascadeCount - 1;
    for (int i = 0; i < kShadowCascadeCount; ++i) {
        if (view_depth < shadows.split_distances[i]) {
            cascade = i;
            break;
        }
    }
    vec4 light_clip = shadows.light_view_projection[cascade] * vec4(world_position, 1.0);
    vec3 projected = light_clip.xyz / light_clip.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    if (projected.z > 1.0 || projected.z < 0.0 || !insideScreen(uv)) return 1.0;
    float lit = texture(shadow_map, vec4(uv, float(cascade), projected.z - 0.0015));
    return mix(1.0, lit, shadows.params.y);
}

float distributionGgx(float n_dot_h, float alpha) {
    float alpha2 = alpha * alpha;
    float d = n_dot_h * n_dot_h * (alpha2 - 1.0) + 1.0;
    return alpha2 / (kWaterPi * d * d);
}

// Visibilidad de Smith-GGX con correlacion de altura (/ 4 NdotL NdotV), la
// misma que lighting.frag.
float visibilitySmith(float n_dot_v, float n_dot_l, float alpha) {
    float alpha2 = alpha * alpha;
    float ggx_v = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - alpha2) + alpha2);
    float ggx_l = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - alpha2) + alpha2);
    return 0.5 / max(ggx_v + ggx_l, 1e-5);
}

// Fresnel de un dielectrico sin polarizar (exacto): cosenos del rayo que
// llega (indice n1) y del que sale (n2).
float fresnelDielectric(float cos_i, float cos_t, float n1, float n2) {
    float rs = (n1 * cos_i - n2 * cos_t) / max(n1 * cos_i + n2 * cos_t, 1e-5);
    float rp = (n1 * cos_t - n2 * cos_i) / max(n1 * cos_t + n2 * cos_i, 1e-5);
    return clamp(0.5 * (rs * rs + rp * rp), 0.0, 1.0);
}

// Ondulacion fina (el rizado del viento): 10 ondas cortas con direcciones
// repartidas alrededor del viento y fases distintas, mas ruido. `p` ya va
// desplazado por la corriente (el rio la pasa en dos fases).
vec2 detailSlope(vec2 p, float t, float wind, float strength, float footprint, inout float lost_variance) {
    vec2 slope = vec2(0.0);
    float len = 1.8;
    for (int i = 0; i < 10; ++i) {
        float angle = wind + (fract(float(i) * 0.618034 + 0.3) - 0.5) * 3.4;
        vec2 dir = vec2(cos(angle), sin(angle));
        float k = 2.0 * kWaterPi / len;
        float omega = sqrt(9.81 * k);
        float keep = waveKeep(len, footprint);
        float s = 0.006 * len * k * strength;
        if (keep <= 0.0) {
            lost_variance += 0.5 * s * s;
            len *= 0.76;
            continue;
        }
        float theta = k * dot(dir, p) - omega * t * 0.6 + fract(float(i) * 0.754878) * 6.2831;
        slope += dir * cos(theta) * s * keep;
        lost_variance += 0.5 * s * s * (1.0 - keep * keep);
        len *= 0.76;
    }
    // Rizado irregular (que no se vea el patron de ondas).
    vec2 q = p * 1.7;
    float e = 0.15;
    float fp = footprint * 1.7;
    float n0 = fbm(q + t * 0.35, fp);
    slope += vec2(fbm(q + vec2(e, 0.0) + t * 0.35, fp) - n0, fbm(q + vec2(0.0, e) + t * 0.35, fp) - n0) / e * 0.03 *
             strength;
    return slope;
}

// Lo que se ve desde abajo al otro lado de la superficie en la direccion
// `direction` (ya refractada): lo de la pantalla si es aire (por encima del
// agua), si no el cielo del cubo.
vec3 airRadiance(vec3 from, vec3 direction, float surface_y) {
    vec3 sky = textureLod(environment_map, normalize(vec3(direction.x, max(direction.y, 0.02), direction.z)), 1.0).rgb;
    vec2 uv;
    vec3 target = (camera.view * vec4(from + direction * 30.0, 1.0)).xyz;
    if (project(target, uv) && insideScreen(uv)) {
        float depth = textureLod(g_depth, uv, 0.0).r;
        vec3 seen = worldFromDepth(uv, depth);
        if (depth >= 1.0 || seen.y > surface_y + 0.05) {
            vec2 edge = min(uv, 1.0 - uv);
            float confidence = smoothstep(0.0, 0.08, min(edge.x, edge.y));
            sky = mix(sky, textureLod(scene_color, uv, 0.0).rgb, confidence);
        }
    }
    return min(sky, vec3(kWindowRadiance));
}

void main() {
    WaterBody b = water.bodies[push.body];
    float t = water.time_count.x;
    bool river = push.mesh == 2u;
    bool fft_ocean = push.mesh == 1u && oceanAvailable();

    vec3 to_camera = camera.position.xyz - v_world_position;
    float surface_distance = length(to_camera);
    vec3 view_direction = to_camera / max(surface_distance, 1e-4);

    // --- Todo lo que usa derivadas de pantalla, antes de cualquier discard ---
    float footprint = max(max(length(dFdx(v_grid)), length(dFdy(v_grid))), 1e-4);  // metros por pixel
    // Cara de abajo: la normal del triangulo (hacia arriba) no mira a la
    // camara. Solo cuenta si la camara esta en este agua o a ras de ella (la
    // cara de atras de una ola vista desde arriba no es "desde abajo").
    vec3 geometric = cross(dFdx(v_world_position), dFdy(v_world_position));
    geometric = dot(geometric, geometric) > 1e-20 ? normalize(geometric) : vec3(0.0, 1.0, 0.0);
    if (geometric.y < 0.0) geometric = -geometric;
    bool camera_in_this = int(water.time_count.z + 0.5) == int(push.body) && water.time_count.z > -0.5;
    bool below = camera_in_this && dot(geometric, to_camera) < 0.0;

    float jacobian = 1.0;
    float lost_variance = 0.0;
    float fft_foam = 0.0;
    vec3 wave_normal;
    if (fft_ocean) {
        wave_normal = oceanNormal(v_grid, footprint, jacobian, fft_foam, lost_variance);
    } else {
        wave_normal = gerstnerNormal(b, v_grid, t, footprint, jacobian, lost_variance);
    }

    // Oceano: lo que ya cubre el nivel interior del clipmap (sin dibujar dos
    // veces el mismo sitio con transparencia).
    if (v_hole.z > 0.0) {
        vec2 local = v_grid + b.origin.xz - v_hole.xy;
        if (max(abs(local.x), abs(local.y)) < v_hole.z) discard;
    }

    // --- Rio: coordenadas del rio y corriente ---
    // x = metros a traves, y = metros a lo largo: sigue las curvas.
    float river_width = river ? max(length(v_flow), 0.01) : 1.0;
    vec2 river_dir = river ? v_flow / river_width : vec2(1.0, 0.0);
    vec2 river_side = vec2(-river_dir.y, river_dir.x);
    vec2 river_uv = vec2((v_uv.x - 0.5) * river_width, v_uv.y);
    // Mas rapido en el centro y donde baja (rapidos).
    float across = abs(v_uv.x - 0.5) * 2.0;
    float rapids = river ? smoothstep(0.015, 0.12, v_slope) : 0.0;
    float river_speed = b.wind.z * (1.0 - 0.45 * across * across) * (1.0 + clamp(v_slope * 12.0, 0.0, 3.0));
    // Flow map en dos fases: dos copias del rizado arrastradas por la
    // corriente que se reinician por turnos (la que se reinicia no se ve):
    // la velocidad puede cambiar de un sitio a otro sin estirar el agua.
    const float kFlowCycle = 1.6;
    float cycle = t / kFlowCycle;
    float phase0 = fract(cycle);
    float phase1 = fract(cycle + 0.5);
    float flow_weight = abs(2.0 * phase0 - 1.0);  // peso de la fase 1
    vec2 jump0 = vec2(0.37, 0.61) * floor(cycle);
    vec2 jump1 = vec2(0.53, 0.29) * floor(cycle + 0.5);
    vec2 river_uv0 = river_uv - vec2(0.0, river_speed) * phase0 * kFlowCycle + jump0;
    vec2 river_uv1 = river_uv - vec2(0.0, river_speed) * phase1 * kFlowCycle + jump1;

    // --- Normal (por pixel, filtrada por su tamano en el suelo) ---
    float detail_far = smoothstep(0.03, 0.25, footprint);
    float detail_strength = b.wind.w * (1.0 - detail_far) * (river ? 1.0 + rapids * 1.5 : 1.0);
    vec2 slope;
    if (river) {
        // El rizado corre a lo largo (angulo 90 grados = +y del rio).
        float lost0 = 0.0;
        float lost1 = 0.0;
        vec2 s0 = detailSlope(river_uv0, t, 1.5707963, detail_strength, footprint, lost0);
        vec2 s1 = detailSlope(river_uv1, t, 1.5707963, detail_strength, footprint, lost1);
        slope = mix(s0, s1, flow_weight);
        lost_variance += mix(lost0, lost1, flow_weight);
        slope = river_side * slope.x + river_dir * slope.y;
    } else if (fft_ocean) {
        // El FFT ya lleva el rizado (la cascada fina): solo un poco de ruido.
        slope = detailSlope(v_grid, t, b.wind.x, detail_strength * 0.35, footprint, lost_variance);
    } else {
        slope = detailSlope(v_grid, t, b.wind.x, detail_strength, footprint, lost_variance);
    }
    lost_variance += detail_far * 0.004 * b.wind.w * b.wind.w;
    // Olas interactivas (salpicaduras y estelas).
    vec2 ripple_slope = rippleSlope(v_world_position.xz);
    slope += ripple_slope;
    vec3 normal_up = normalize(vec3(wave_normal.x - slope.x, wave_normal.y, wave_normal.z - slope.y));

    // --- Luz ---
    vec3 sun_direction = normalize(-lights.sun_direction_intensity.xyz);
    vec3 sun_radiance = toLinear(lights.sun_color_ambient.rgb) * lights.sun_direction_intensity.w;
    float shadow = sunShadow(v_world_position);
    vec3 sky_light = textureLod(environment_map, vec3(0.0, 1.0, 0.0), 6.0).rgb;
    vec3 ambient = sky_light + toLinear(lights.ambient_color.rgb) * lights.sun_color_ambient.a;
    float sun_height = clamp(sun_direction.y, 0.0, 1.0);
    vec3 extinction = waterExtinction(b);

    vec2 size = vec2(textureSize(g_depth, 0));
    vec2 screen_uv = gl_FragCoord.xy / size;
    float roughness = clamp(sqrt(b.look.x * b.look.x + lost_variance * 0.5), 0.01, 0.6);

    // =====================================================================
    // Desde abajo: ventana de Snell
    // =====================================================================
    if (below) {
        vec3 n = -normal_up;  // hacia la camara (abajo)
        float facing = dot(n, view_direction);
        if (facing < 0.05) n = normalize(n + view_direction * (0.05 - facing));
        vec3 incident = -view_direction;  // de la camara hacia la superficie
        float cos_i = clamp(dot(n, view_direction), 0.0, 1.0);
        // Del agua al aire: pasado el angulo critico (~48.6 grados) no sale
        // nada (reflexion total); dentro, el cielo y la orilla comprimidos en
        // un circulo de 97 grados.
        vec3 transmitted = refract(incident, n, kWaterIor);
        float fresnel = 1.0;
        vec3 air = vec3(0.0);
        if (dot(transmitted, transmitted) > 1e-6) {
            transmitted = normalize(transmitted);
            float cos_t = clamp(-dot(transmitted, n), 0.0, 1.0);
            fresnel = fresnelDielectric(cos_i, cos_t, kWaterIor, 1.0);
            air = airRadiance(v_world_position, transmitted, v_world_position.y);
            // El sol visto a traves de la superficie (un disco que tiembla).
            float to_sun = max(dot(transmitted, sun_direction), 0.0);
            air += sun_radiance * shadow * pow(to_sun, 900.0) * 6.0;
            air = min(air, vec3(kWindowRadiance));
        }
        // Reflexion total: el propio agua (luz dispersada, mas oscura hacia
        // el fondo).
        float camera_depth = max(v_world_position.y - camera.position.y, 0.0);
        vec3 deep_view = waterInScatter(b, sun_radiance * shadow, sun_height, ambient, camera_depth + 4.0);
        vec3 surface = air * (1.0 - fresnel) + deep_view * fresnel;
        // Espuma de las crestas vista desde abajo: tapa la luz (mas oscura).
        if (fft_ocean) surface = mix(surface, deep_view * 1.6 + ambient * 0.08, fft_foam * 0.6 * b.deep.w);
        // El agua entre la camara y la superficie (la misma que water_under).
        vec3 in_scatter = waterInScatter(b, sun_radiance, sun_height, ambient, camera_depth);
        vec3 transmittance = exp(-extinction * surface_distance);
        vec3 color = surface * transmittance + in_scatter * (vec3(1.0) - transmittance);
        if (any(isnan(color))) color = vec3(0.0);
        out_color = vec4(min(color, vec3(kMaxRadiance)), 1.0);
        return;
    }

    // =====================================================================
    // Desde arriba
    // =====================================================================
    vec3 normal = normal_up;
    float facing = dot(normal, view_direction);
    if (facing < 0.05) normal = normalize(normal + view_direction * (0.05 - facing));
    float n_dot_v = max(dot(normal, view_direction), 1e-3);

    // --- Lo que hay detras (profundidad de la escena) ---
    float scene_depth = texelFetch(g_depth, ivec2(gl_FragCoord.xy), 0).r;
    bool sky_behind = scene_depth >= 1.0;
    vec3 floor_position = worldFromDepth(screen_uv, scene_depth);
    float vertical_depth = sky_behind ? 1000.0 : max(v_world_position.y - floor_position.y, 0.0);

    // Refraccion: desviada por la normal, menos en lo poco profundo y lejos.
    vec2 refract_offset = normal.xz * b.look.y * 0.06 * clamp(vertical_depth, 0.0, 1.0) / (1.0 + surface_distance * 0.05);
    vec2 refract_uv = clamp(screen_uv + refract_offset, vec2(0.001), vec2(0.999));
    float refract_depth = textureLod(g_depth, refract_uv, 0.0).r;
    vec3 refract_floor = worldFromDepth(refract_uv, refract_depth);
    // Si el punto desviado esta delante del agua, se usa el directo.
    if (refract_depth < 1.0 && dot(refract_floor - camera.position.xyz, -view_direction) < surface_distance) {
        refract_uv = screen_uv;
        refract_depth = scene_depth;
        refract_floor = floor_position;
    }
    vec3 refracted = min(textureLod(scene_color, refract_uv, 0.0).rgb, vec3(kMaxRadiance));
    float thickness = refract_depth >= 1.0 ? 1000.0 : max(length(refract_floor - camera.position.xyz) - surface_distance, 0.0);

    // --- Absorcion y dispersion en el volumen de agua (Beer-Lambert) ---
    vec3 transmittance = exp(-extinction * thickness);
    vec3 in_scatter = b.deep.rgb * (sun_radiance * sun_height * shadow * 0.35 + ambient);
    // Causticas en el fondo poco profundo (la luz que las olas concentran).
    if (refract_depth < 1.0 && b.look.z > 0.0) {
        float floor_depth = max(v_world_position.y - refract_floor.y, 0.0);
        vec2 entry = refract_floor.xz + sun_direction.xz / max(sun_direction.y, 0.2) * floor_depth;
        vec2 caustic_uv = river ? river_uv0 : entry - b.origin.xz;
        float c = caustic(caustic_uv, t * 1.3) * b.look.z * shadow * sun_height *
                  smoothstep(0.0, 0.4, floor_depth) * exp(-floor_depth * 0.35);
        refracted *= 1.0 + c;
    }
    vec3 water_color = refracted * transmittance + in_scatter * (vec3(1.0) - transmittance);

    // Dispersion subsuperficial (modelo de Atlas, GDC 2019; el de Crest): la
    // luz del sol atraviesa las crestas y sale tenida del color del agua
    // (el verde turquesa a contraluz).
    float wave_scale = fft_ocean ? max(water.ocean.y * 0.5, 0.05) : max(b.waves.x, 0.05);
    float crest = clamp(v_height / wave_scale, 0.0, 1.0);
    float toward_sun = pow(max(dot(-view_direction, sun_direction) * 0.5 + 0.5, 0.0), 6.0);
    float backlit = pow(max(dot(-view_direction, sun_direction), 0.0), 4.0) *
                    pow(clamp(0.5 - 0.5 * dot(sun_direction, normal), 0.0, 1.0), 3.0);
    float facing_camera = pow(max(dot(view_direction, normal), 0.0), 2.0);
    // La luz entra por la cara iluminada y sale por la fina: mas en las
    // crestas y en las olas empinadas (con el horizonte detras).
    float thin = fft_ocean ? clamp(1.0 - normal.y, 0.0, 0.4) * 2.5 : 0.0;
    float sss = (crest + thin * 0.5) * backlit * 2.2 + (0.5 + crest) * facing_camera * 0.03 + toward_sun * crest * 0.06;
    water_color += b.shallow.rgb * sun_radiance * shadow * sun_height * sss * b.extra.y;
    // Las crestas, mas finas, dejan pasar mas luz del cielo: mas claras y verdes.
    water_color += b.shallow.rgb * ambient * crest * crest * b.extra.y * 0.25;

    // --- Reflejo ---
    vec3 reflected = reflect(-view_direction, normal);
    reflected.y = max(reflected.y, 0.02);
    reflected = normalize(reflected);
    vec3 view_position = (camera.view * vec4(v_world_position, 1.0)).xyz;
    vec3 view_ray = normalize(mat3(camera.view) * reflected);
    float toward_camera = smoothstep(0.2, 0.6, view_ray.z);
    vec4 screen = toward_camera < 1.0 ? traceScreen(view_position + mat3(camera.view) * normal * 0.05, view_ray)
                                      : vec4(0.0);
    screen.a *= 1.0 - toward_camera;
    float lod = roughness * 6.0;
    // El cubo filtrado mezcla el suelo de debajo del horizonte (marron) en
    // los reflejos rugosos: la direccion se sube con la rugosidad.
    vec3 env_direction = normalize(vec3(reflected.x, max(reflected.y, 0.02 + roughness * 0.4), reflected.z));
    vec3 fallback = textureLod(environment_map, env_direction, lod).rgb;
    float probe_weight = lights.probes[0].w + lights.probes[1].w;
    if (probe_weight > 0.001) {
        vec3 probe = vec3(0.0);
        if (lights.probes[0].w > 0.001) probe += textureLod(reflection_probe_0, reflected, lod).rgb * lights.probes[0].w;
        if (lights.probes[1].w > 0.001) probe += textureLod(reflection_probe_1, reflected, lod).rgb * lights.probes[1].w;
        fallback = mix(fallback, probe / probe_weight, 0.7);
    }
    // El cielo de verdad (con nubes) si esa parte se ve en pantalla.
    vec4 sky = screenSky(reflected);
    fallback = mix(fallback, sky.rgb, sky.a * (1.0 - smoothstep(0.1, 0.4, roughness)));
    vec3 reflection = mix(min(fallback, vec3(kMaxRadiance)), screen.rgb, screen.a);
    // Fresnel del aire al agua (exacto: a ras se refleja todo).
    float sin_t = sqrt(max(1.0 - n_dot_v * n_dot_v, 0.0)) / kWaterIor;
    float cos_t = sqrt(max(1.0 - sin_t * sin_t, 0.0));
    float fresnel = fresnelDielectric(n_dot_v, cos_t, 1.0, kWaterIor);

    // Brillo del sol (GGX) con su sombra.
    vec3 specular = vec3(0.0);
    float n_dot_l = dot(normal, sun_direction);
    if (n_dot_l > 0.0) {
        vec3 halfway = normalize(sun_direction + view_direction);
        // El sol mide 0.53 grados: la distribucion se ensancha a su tamano y
        // se normaliza (un disco de brillo, no un punto que parpadea).
        float alpha = max(roughness * roughness, 0.002);
        const float kSunSize = 0.00465;
        float alpha_sun = min(alpha + kSunSize * 0.5, 1.0);
        float d = distributionGgx(max(dot(normal, halfway), 0.0), alpha_sun) * (alpha / alpha_sun) * (alpha / alpha_sun);
        float v_dot_h = max(dot(view_direction, halfway), 0.0);
        float sun_fresnel = 0.02 + 0.98 * pow(1.0 - v_dot_h, 5.0);
        float brdf = d * visibilitySmith(n_dot_v, n_dot_l, alpha) * sun_fresnel;
        specular = sun_radiance * min(brdf * n_dot_l, kSunDiskRadiance * 0.015) * shadow;
    }

    vec3 color = water_color * (1.0 - fresnel) + reflection * fresnel + specular;

    // --- Espuma ---
    // Cobertura (donde hay espuma y cuanta) por un lado y su textura (encaje
    // de burbujas) por otro: con poca cobertura solo asoman las partes mas
    // densas, que es como se deshace la espuma de verdad.
    float shore_width = max(b.look.w, 0.01);
    float shore = sky_behind ? 0.0 : 1.0 - smoothstep(0.0, shore_width, vertical_depth);
    // Olas que llegan a la playa: bandas que avanzan hacia la orilla.
    float bands = 0.0;
    if (b.extra.x > 0.0 && !sky_behind) {
        float phase = vertical_depth * 2.2 - t * 1.4 + fbm(v_grid * 0.15, footprint * 0.15) * 3.0;
        bands = smoothstep(0.55, 1.0, sin(phase)) * (1.0 - smoothstep(0.0, shore_width * 3.0, vertical_depth)) * b.extra.x;
    }
    float crest_foam = 0.0;
    if (fft_ocean) {
        // Borreguitos del FFT: la espuma que deja cada ola al romper
        // (jacobiano) y que se va deshaciendo (water_fft.comp), mas la que
        // se forma ahora en las crestas comprimidas.
        crest_foam = fft_foam * 1.2 + smoothstep(0.55, 0.1, jacobian) * 0.6 * clamp(b.deep.w, 0.0, 1.0);
    } else {
        crest_foam = smoothstep(0.35, -0.1, jacobian) * smoothstep(0.35, 0.8, crest) * b.deep.w;
        if (!river) {
            float sea_state = smoothstep(0.3, 1.5, b.waves.x);
            float patches = smoothstep(0.45, 0.75, fbm(v_grid * 0.035 + vec2(t * 0.03, -t * 0.02), footprint * 0.035));
            float breaking = smoothstep(0.55, 0.95, crest) * smoothstep(0.95, 0.6, jacobian);
            float trailing = smoothstep(0.25, 0.6, crest) * smoothstep(0.9, 0.75, jacobian) * 0.35;
            crest_foam = max(crest_foam, (breaking + trailing) * patches * sea_state * b.deep.w);
        }
    }
    float river_foam = 0.0;
    if (river) {
        float bank = smoothstep(0.32, 0.5, abs(v_uv.x - 0.5));
        float streaks = mix(fbm(river_uv0 * 0.8, footprint * 0.8), fbm(river_uv1 * 0.8, footprint * 0.8), flow_weight);
        river_foam = bank * 0.6 + smoothstep(0.62, 0.85, streaks) * clamp(b.wind.z * 0.25, 0.0, 1.0) + rapids * 0.9;
    }
    // Espuma en las crestas fuertes de las olas interactivas (salpicaduras).
    float splash_foam = smoothstep(0.35, 1.2, length(ripple_slope)) * smoothstep(0.01, 0.06, rippleHeight(v_world_position.xz));
    float coverage = clamp((shore * 0.9 + bands + river_foam + splash_foam) * b.deep.w + crest_foam, 0.0, 1.2);
    float foam = 0.0;
    if (coverage > 0.001) {
        float density;
        if (river) {
            density = mix(foamDensity(river_uv0, t, footprint), foamDensity(river_uv1, t, footprint), flow_weight);
        } else {
            density = foamDensity(v_grid, t, footprint);
        }
        // Umbral con borde suave de al menos un pixel (sin escalones). Del
        // tamano del pixel y no de fwidth: dentro de este if las derivadas no
        // estan definidas.
        float edge = 0.06 + footprint * 2.5;
        float threshold = 1.0 - coverage;
        foam = smoothstep(threshold - edge, threshold + edge, density);
        // Espuma fina: deja ver el agua (translucida); la densa, opaca.
        foam *= mix(0.55, 1.0, smoothstep(0.4, 1.0, coverage));
        // Muy lejos la textura ya no se resuelve: su media.
        float far = smoothstep(0.08, 0.35, footprint);
        foam = mix(foam, coverage * 0.6, far);
    }
    // Luz de la espuma: difusa (mira casi hacia arriba aunque la ola se
    // incline), algo de luz que la atraviesa y el cielo.
    vec3 foam_normal = normalize(normal + vec3(0.0, 1.5, 0.0));
    float foam_diffuse = max(dot(foam_normal, sun_direction), 0.0) * 0.8 + 0.2 * toward_sun;
    vec3 foam_color = vec3(0.9, 0.93, 0.95) * (sun_radiance * foam_diffuse * shadow + ambient * 1.1);
    color = mix(color, foam_color, clamp(foam, 0.0, 1.0));

    // --- Niebla por altura (la misma formula que lighting.frag) ---
    vec3 ray_direction = -view_direction;
    float fog_falloff = lights.clouds.z > 0.0 ? lights.clouds.z : kFogHeightFalloff;
    float fog_density = lights.clouds.y * exp(-(camera.position.y - kFogBaseHeight) * fog_falloff);
    float fog_b = fog_falloff * ray_direction.y;
    float fog_integral = abs(fog_b) > 0.0001 ? (1.0 - exp(-surface_distance * fog_b)) / fog_b : surface_distance;
    float fog = clamp(1.0 - exp(-fog_density * fog_integral), 0.0, 1.0);
    vec3 fog_color = textureLod(environment_map, normalize(vec3(ray_direction.x, max(ray_direction.y, 0.02), ray_direction.z)), 3.0).rgb;
    float sun_alignment = max(dot(ray_direction, sun_direction), 0.0);
    fog_color += sun_radiance * pow(sun_alignment, 10.0) * 0.35 * smoothstep(-0.05, 0.1, sun_direction.y);
    // Muy lejos, el agua se funde con el horizonte (bruma del aire).
    fog = max(fog, 1.0 - exp(-surface_distance * 0.00012));
    color = mix(color, fog_color, fog);

    // --- Luz volumetrica hasta la superficie ---
    // El mapa esta integrado hasta el fondo que hay detras del agua: se usa
    // solo el tramo camara -> superficie.
    if (lights.environment.y > 0.5) {
        vec4 volume = textureLod(volumetric_map, screen_uv, 0.0);
        float behind = sky_behind ? kVolumetricDistance : min(length(floor_position - camera.position.xyz), kVolumetricDistance);
        float fraction = clamp(min(surface_distance, kVolumetricDistance) / max(behind, 0.001), 0.0, 1.0);
        float transmittance_part = pow(clamp(volume.a, 1e-4, 1.0), fraction);
        vec3 scattered = volume.a < 0.999 ? volume.rgb * (1.0 - transmittance_part) / (1.0 - volume.a)
                                          : volume.rgb * fraction;
        color = color * transmittance_part + scattered;
    }

    // Borde suave donde el agua toca el suelo.
    float alpha = sky_behind ? 1.0 : clamp(vertical_depth / 0.06, 0.0, 1.0);
    if (any(isnan(color))) color = vec3(0.0);
    out_color = vec4(min(color, vec3(kMaxRadiance)), alpha);
}
