#version 450
#extension GL_GOOGLE_include_directive : require

// Bajo el agua (la camara dentro de un oceano, lago o rio): lo que se ve por
// debajo de la superficie se tine y se pierde con la distancia (la misma
// absorcion y luz dispersada que la superficie vista desde abajo en
// water.frag). Se decide por pixel con la ola en el plano cercano: si la
// linea del agua cruza la pantalla, cada mitad se ve como toca, con el
// menisco (una linea oscura y borrosa) entre las dos.
//
//   - Causticas del sol (con su sombra) en lo que hay bajo la superficie.
//   - Rayos de sol: la luz que entra por las olas, en haces a lo largo de la
//     direccion del sol, con la sombra de lo que hay encima.
//   - Particulas en suspension (motas que brillan con la luz del agua).
//
// Lo que se ve fuera del agua (el cielo detras de la superficie) lo pinta
// despues water.frag con la ventana de Snell: aqui el cielo de la copia de la
// escena se trata como agua lejana, nunca como luz directa (antes el cielo
// HDR sin atenuar salia en blanco).

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
    vec4 environment;
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

#include "water_common.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

const float kMaxRadiance = 40.0;
const float kMaxViewDistance = 300.0;  // el cielo detras: agua "infinita"

vec3 toLinear(vec3 c) { return pow(c, vec3(2.2)); }

vec3 worldFromDepth(vec2 uv, float depth) {
    vec4 world = camera.inverse_view_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return world.xyz / world.w;
}

// Sombra del sol en un punto (la misma que water.frag).
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
    if (projected.z > 1.0 || projected.z < 0.0 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return 1.0;
    }
    float lit = texture(shadow_map, vec4(uv, float(cascade), projected.z - 0.0015));
    return mix(1.0, lit, shadows.params.y);
}

uint pcg(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
uint cellHash3(ivec3 c) { return pcg(uint(c.x) ^ pcg(uint(c.y) ^ pcg(uint(c.z) + 0x9E3779B9u))); }
vec3 hash3(ivec3 c) {
    uint h = cellHash3(c);
    uint h2 = pcg(h);
    return vec3(float(h & 0x3FFu), float((h >> 10u) & 0x3FFu), float(h2 & 0x3FFu)) * (1.0 / 1023.0);
}

// Henyey-Greenstein: la luz dispersada en el agua va sobre todo hacia delante.
float phaseHg(float cos_theta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * kWaterPi * pow(max(1.0 + g2 - 2.0 * g * cos_theta, 1e-4), 1.5));
}

void main() {
    WaterBody b = water.bodies[push.body];
    int type = int(b.extent.z + 0.5);
    float t = water.time_count.x;
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(g_depth, pixel, 0).r;

    // El punto de la escena en este pixel y su normal (por las derivadas de
    // la posicion), y la altura de la linea del agua en el plano cercano:
    // todo antes del discard (despues las derivadas no estan definidas).
    vec3 surface_point = worldFromDepth(v_uv, depth);
    vec3 surface_normal = normalize(cross(dFdx(surface_point), dFdy(surface_point)));
    if (dot(surface_normal, camera.position.xyz - surface_point) < 0.0) surface_normal = -surface_normal;
    vec3 near_point = worldFromDepth(v_uv, 0.0);
    // Rio: la superficie donde esta la camara (extra.z), sin olas.
    float surface = waterSurfaceHeight(b, near_point.xz, t);
    float above_line = near_point.y - surface;  // > 0: este pixel esta fuera del agua
    // Menisco: unos pixeles a cada lado de la linea del agua.
    float line_pixels = max(fwidth(above_line), 1e-6);
    float meniscus = exp(-abs(above_line) / (line_pixels * 2.5));

    bool outside_lake = false;
    if (type == 1) outside_lake = !insideLake(b, near_point.xz, 0.3);
    vec3 scene = texelFetch(scene_color, pixel, 0).rgb;
    if (above_line > 0.0 || outside_lake) {
        // Fuera del agua: solo el menisco (la gota de agua pegada al cristal).
        if (outside_lake || meniscus < 0.02) discard;
        out_color = vec4(scene * (1.0 - 0.6 * meniscus), 1.0);
        return;
    }

    vec3 ray = near_point - camera.position.xyz;
    ray = dot(ray, ray) > 1e-12 ? normalize(ray) : normalize(surface_point - camera.position.xyz);
    float distance = depth >= 1.0 ? kMaxViewDistance : length(surface_point - camera.position.xyz);
    scene = min(scene, vec3(kMaxRadiance));

    vec3 sun_direction = normalize(-lights.sun_direction_intensity.xyz);
    float sun_height = clamp(sun_direction.y, 0.0, 1.0);
    vec3 sun_radiance = toLinear(lights.sun_color_ambient.rgb) * lights.sun_direction_intensity.w;
    vec3 ambient = textureLod(environment_map, vec3(0.0, 1.0, 0.0), 6.0).rgb +
                   toLinear(lights.ambient_color.rgb) * lights.sun_color_ambient.a;
    vec3 extinction = waterExtinction(b);
    // Nivel medio del agua (para la luz que baja): el rio, el de la camara.
    float level = type == 2 ? b.extra.z : b.origin.y;

    // --- Causticas sobre lo que hay bajo el agua ---
    if (depth < 1.0 && b.look.z > 0.0 && sun_direction.y > 0.0) {
        float water_above = level - surface_point.y;  // metros de agua encima
        if (water_above > 0.0) {
            // Proyectadas desde el sol hasta la superficie (se mueven con el
            // punto de entrada de la luz, no con la camara).
            vec2 entry = surface_point.xz + sun_direction.xz / max(sun_direction.y, 0.2) * water_above;
            float facing = smoothstep(-0.1, 0.6, dot(surface_normal, sun_direction));
            float c = caustic(entry - b.origin.xz, t * 1.3) * b.look.z * sunShadow(surface_point) *
                      sun_height * facing * smoothstep(0.0, 0.4, water_above) * exp(-water_above * 0.35);
            scene *= 1.0 + c;
        }
    }

    // --- Niebla del agua (absorcion + luz dispersada) ---
    // Mas oscura cuanto mas hondo (la luz llega de arriba).
    float depth_below = max(surface - camera.position.y, 0.0);
    vec3 in_scatter = waterInScatter(b, sun_radiance, sun_height, ambient, depth_below);
    // Mirando hacia abajo la luz dispersada se oscurece (llega menos) y hacia
    // arriba se aclara un poco (el brillo de la superficie).
    in_scatter *= mix(0.55, 1.25, ray.y * 0.5 + 0.5);
    vec3 transmittance = exp(-extinction * distance);
    vec3 color = scene * transmittance + in_scatter * (vec3(1.0) - transmittance);

    // --- Rayos de sol ---
    if (b.under.x > 0.0 && sun_direction.y > 0.02) {
        const int kSteps = 14;
        float march = min(distance, 40.0);
        float step_length = march / float(kSteps);
        float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy + 5.588238 * mod(floor(t * 60.0), 64.0),
                                                        vec2(0.06711056, 0.00583715))));
        vec3 shafts = vec3(0.0);
        for (int i = 0; i < kSteps; ++i) {
            float along = (float(i) + jitter) * step_length;
            vec3 p = camera.position.xyz + ray * along;
            float below_surface = level - p.y;
            if (below_surface <= 0.0) continue;
            // Patron de las olas proyectado por el sol: haces paralelos a el.
            vec2 entry = p.xz + sun_direction.xz / max(sun_direction.y, 0.2) * below_surface;
            float pattern = 0.3 + 0.7 * clamp(caustic((entry - b.origin.xz) * 0.35, t * 0.7) * 0.35, 0.0, 1.0);
            vec3 down = exp(-extinction * (below_surface / max(sun_direction.y, 0.2)));
            vec3 back = exp(-extinction * along);
            shafts += pattern * sunShadow(p) * down * back * step_length;
        }
        float phase = phaseHg(dot(ray, sun_direction), 0.6);
        // Coeficiente de dispersion: lo que no absorbe el agua (su color).
        vec3 scattering = clamp(b.shallow.rgb, 0.0, 1.0) * 0.12;
        color += sun_radiance * scattering * shafts * phase * b.under.x * 4.0;
    }

    // --- Particulas en suspension ---
    if (b.under.y > 0.0) {
        float motes = 0.0;
        float cell = 0.45;
        for (int layer = 0; layer < 3; ++layer) {
            float d = 1.1 * exp2(float(layer));
            if (d > distance) break;
            // Las motas derivan despacio con el agua.
            vec3 drift = vec3(0.05, -0.015, 0.03) * t;
            vec3 p = camera.position.xyz + ray * d + drift;
            ivec3 c = ivec3(floor(p / cell));
            vec3 center = (vec3(c) + 0.2 + 0.6 * hash3(c)) * cell;
            vec3 h = hash3(c + ivec3(17, 31, 7));
            if (h.x > 0.35) {
                cell *= 2.0;
                continue;
            }
            // Distancia del centro de la mota al rayo (sin depender de `d`).
            vec3 to_center = center - drift - camera.position.xyz;
            float along = dot(to_center, ray);
            if (along > 0.0 && along < distance) {
                float off_ray = length(to_center - ray * along);
                float radius = 0.004 * along + 0.002;
                float twinkle = 0.6 + 0.4 * sin(t * (1.0 + h.y * 2.0) + h.z * 6.283);
                motes += (1.0 - smoothstep(radius * 0.3, radius, off_ray)) * twinkle * exp(-along * 0.15);
            }
            cell *= 2.0;
        }
        vec3 mote_light = waterInScatter(b, sun_radiance, sun_height, ambient, depth_below) * 3.0 +
                          sun_radiance * sun_height * 0.05;
        color += mote_light * motes * b.under.y;
    }

    // Menisco en la linea del agua (por debajo).
    color *= 1.0 - 0.6 * meniscus;
    if (any(isnan(color))) color = vec3(0.0);
    out_color = vec4(min(color, vec3(kMaxRadiance)), 1.0);
}
