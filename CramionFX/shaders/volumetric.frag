#version 450

// Luz volumetrica: los rayos de luz que se ven en el aire cuando atraviesa
// el polvo en suspension ("god rays" de volumen, como la niebla volumetrica
// de Unreal o Frostbite): el sol entrando por una ventana o un agujero, el
// cono de un foco, el halo de una farola o una antorcha.
//
// A media resolucion. Para cada pixel se recorre el rayo camara -> superficie
// en kSteps tramos. En cada punto del aire se suma la luz que le llega de
// cada fuente (sol, luces puntuales y focos), cada una con su sombra: el
// polvo desvia una parte hacia la camara.
//
//   dL = T(t) * sigma_s(p) * sum_luces[ fase(cos) * L_luz(p) * visibilidad(p) ] dt
//   T  = exp(-integral sigma_t)                                   (Beer-Lambert)
//
// Cada tramo se integra de forma analitica (Hillaire, "Physically Based and
// Unified Volumetric Rendering in Frostbite", SIGGRAPH 2015): estable aunque
// los tramos sean largos.
//
// La fase es Henyey-Greenstein (el polvo dispersa sobre todo hacia delante:
// los rayos se ven mucho mas mirando hacia la luz) mezclada con algo de
// isotropa para que tambien se intuyan de espaldas a ella.
//
// Las luces locales solo se evaluan en el tramo del rayo que cruza su esfera
// de alcance: una farola al fondo no cuesta nada en los pixeles cuyo rayo no
// pasa cerca.
//
// Sin ruido ni parpadeo: el punto de partida de los tramos sigue un patron
// Bayer 4x4 FIJO, y lighting.frag promedia una ventana 4x4 (con pesos de
// profundidad), como hace con el SSAO. 16 desplazamientos x 32 tramos = 512
// muestras por pixel sin filtro temporal.
//
// Salida: rgb = luz dispersada hacia la camara, a = transmitancia (cuanto de
// la superficie de detras llega).

// Deben coincidir con lighting.frag (y scene::kMax...).
const int kMaxPointLights = 32;
const int kMaxSpotLights = 8;
const int kShadowCascadeCount = 4;
const int kMaxShadowedSpotLights = 8;
const int kMaxShadowedPointLights = 8;
const int kPointShadowFaceCount = 6;

struct PointLightGpu {
    vec4 position_range;    // xyz = posicion, w = alcance
    vec4 color_intensity;   // rgb = color,    a = intensidad
    vec4 shadow;            // x   = hueco de sombra (-1 = sin sombra)
};

struct SpotLightGpu {
    vec4 position_range;       // xyz = posicion,  w = alcance
    vec4 direction_intensity;  // xyz = direccion, w = intensidad
    vec4 color_inner;          // rgb = color,     a = cos del angulo interior
    vec4 outer_shadow;         // x   = cos del angulo exterior, y = hueco de sombra
};

layout(set = 0, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
} camera;

// El principio de GpuLights (lighting.frag), hasta las luces locales.
layout(set = 0, binding = 1) uniform LightBuffer {
    vec4 sun_direction_intensity;  // xyz = direccion de los rayos, w = intensidad
    vec4 sun_color_ambient;        // rgb = color del sol
    vec4 ambient_color;
    vec4 sky_sun;
    vec4 sky_moon;
    ivec4 counts;                  // x = puntuales, y = focos
    PointLightGpu points[kMaxPointLights];
    SpotLightGpu spots[kMaxSpotLights];
} lights;

layout(set = 0, binding = 2) uniform ShadowBuffer {
    mat4 light_view_projection[kShadowCascadeCount];
    vec4 split_distances;
    vec4 texel_world_sizes;
    vec4 params;  // x = resolucion, y = intensidad de la sombra
} shadows;

layout(set = 0, binding = 3) uniform sampler2DArrayShadow shadow_map;
layout(set = 0, binding = 4) uniform sampler2D g_depth;

// Sombras de las luces locales (como en lighting.frag): los focos una capa
// cada uno, las puntuales seis (una por cara del cubo).
layout(set = 0, binding = 5) uniform sampler2DArrayShadow spot_shadow_maps;
layout(set = 0, binding = 6) uniform sampler2DArrayShadow point_shadow_maps;
layout(set = 0, binding = 7) uniform LocalShadowBuffer {
    mat4 spot_view_projection[kMaxShadowedSpotLights];
    mat4 point_view_projection[kMaxShadowedPointLights * kPointShadowFaceCount];
    vec4 spot_params[kMaxShadowedSpotLights];
    vec4 point_params[kMaxShadowedPointLights];  // y = fundido
    vec4 params;  // z = intensidad de la sombra
} local_shadows;

layout(push_constant) uniform PushConstants {
    // x = densidad del polvo (1/m), y = anisotropia (g de Henyey-Greenstein),
    // z = segundos (deriva del polvo), w = distancia maxima (m)
    vec4 params;
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_volume;

const float kPi = 3.14159265;
const int kSteps = 32;
// Fraccion isotropa de la fase.
const float kIsotropicMix = 0.25;

vec3 toLinear(vec3 color) {
    return pow(color, vec3(2.2));
}

float linearDepth(float depth) {
    return camera.projection[3][2] / (depth + camera.projection[2][2]);
}

vec3 viewFromDepth(vec2 uv, float depth) {
    float z = linearDepth(depth);
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc.x * z / camera.projection[0][0], ndc.y * z / camera.projection[1][1], -z);
}

// Matriz de Bayer 4x4: cada pixel de la ventana tiene un desplazamiento
// distinto (lighting.frag promedia la ventana).
float bayer4(ivec2 p) {
    const float kBayer[16] = float[](0.0, 8.0, 2.0, 10.0,
                                     12.0, 4.0, 14.0, 6.0,
                                     3.0, 11.0, 1.0, 9.0,
                                     15.0, 7.0, 13.0, 5.0);
    return (kBayer[(p.y & 3) * 4 + (p.x & 3)] + 0.5) / 16.0;
}

// `cosine`: entre la direccion en que viaja la luz y la que sale hacia la
// camara (1 = sigue recta: dispersion hacia delante).
float henyeyGreenstein(float cosine, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * kPi * pow(max(1.0 + g2 - 2.0 * g * cosine, 1e-4), 1.5));
}

float phaseFunction(float cosine) {
    return mix(henyeyGreenstein(cosine, push.params.y), 1.0 / (4.0 * kPi), kIsotropicMix);
}

// --- Polvo: ruido de valor 3D, que deriva despacio ---
float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

float valueNoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);
    float n000 = hash13(i);
    float n100 = hash13(i + vec3(1.0, 0.0, 0.0));
    float n010 = hash13(i + vec3(0.0, 1.0, 0.0));
    float n110 = hash13(i + vec3(1.0, 1.0, 0.0));
    float n001 = hash13(i + vec3(0.0, 0.0, 1.0));
    float n101 = hash13(i + vec3(1.0, 0.0, 1.0));
    float n011 = hash13(i + vec3(0.0, 1.0, 1.0));
    float n111 = hash13(i + vec3(1.0, 1.0, 1.0));
    return mix(mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y),
               mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y), u.z);
}

// Densidad relativa del polvo (media ~1): nubes suaves de ~1.5 m que se
// desplazan con una corriente de aire lenta.
float dust(vec3 p, float time) {
    vec3 drift = vec3(0.07, 0.02, 0.05) * time;
    float n = valueNoise(p * 0.7 + drift) * 0.65 + valueNoise(p * 1.9 - drift * 1.7) * 0.35;
    return 0.35 + 1.3 * n;
}

// --- Sol ---

// 1 si al punto le llega el sol, 0 si esta en sombra. Fuera de las cascadas,
// iluminado (como en lighting.frag).
float sunVisibility(vec3 world_position) {
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
    if (projected.z > 1.0 || projected.z < 0.0 || any(lessThan(uv, vec2(0.0))) ||
        any(greaterThan(uv, vec2(1.0)))) {
        return 1.0;
    }
    float lit = texture(shadow_map, vec4(uv, float(cascade), projected.z));
    return mix(1.0, lit, shadows.params.y);
}

// --- Luces locales ---

// Igual que lighting.frag: 1/d^2 con corte suave al llegar al alcance.
float attenuation(float distance_to_light, float range) {
    float ratio = distance_to_light / max(range, 0.0001);
    float window = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
    return (window * window) / (distance_to_light * distance_to_light + 1.0);
}

int shadowSlot(float encoded) {
    return int(floor(encoded + 0.5));
}

// Una muestra filtrada por hardware; fuera del frustum de la luz, iluminado.
float localShadowLookup(sampler2DArrayShadow map, vec4 light_clip, float layer) {
    if (light_clip.w <= 0.0) {
        return 1.0;
    }
    vec3 projected = light_clip.xyz / light_clip.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    if (projected.z > 1.0 || projected.z < 0.0 || any(lessThan(uv, vec2(0.0))) ||
        any(greaterThan(uv, vec2(1.0)))) {
        return 1.0;
    }
    return texture(map, vec4(uv, layer, projected.z));
}

float spotVisibility(int slot, vec3 p) {
    vec4 light_clip = local_shadows.spot_view_projection[slot] * vec4(p, 1.0);
    float lit = localShadowLookup(spot_shadow_maps, light_clip, float(slot));
    return mix(1.0, lit, local_shadows.params.z);
}

// Cara del cubo por el eje dominante (mismo orden que la CPU: +X, -X, +Y,
// -Y, +Z, -Z).
float pointVisibility(int slot, vec3 light_position, vec3 p) {
    vec3 direction = p - light_position;
    vec3 magnitude = abs(direction);
    int face;
    if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z) {
        face = direction.x > 0.0 ? 0 : 1;
    } else if (magnitude.y >= magnitude.z) {
        face = direction.y > 0.0 ? 2 : 3;
    } else {
        face = direction.z > 0.0 ? 4 : 5;
    }
    int layer = slot * kPointShadowFaceCount + face;
    vec4 light_clip = local_shadows.point_view_projection[layer] * vec4(p, 1.0);
    float lit = localShadowLookup(point_shadow_maps, light_clip, float(layer));
    return mix(1.0, lit, local_shadows.params.z * local_shadows.point_params[slot].y);
}

// Tramo [entrada, salida] del rayo dentro de la esfera de alcance de una
// luz; vacio (x > y) si no la toca.
vec2 sphereSegment(vec3 origin, vec3 direction, vec3 center, float radius, float max_t) {
    vec3 offset = origin - center;
    float b = dot(offset, direction);
    float c = dot(offset, offset) - radius * radius;
    float discriminant = b * b - c;
    if (discriminant <= 0.0) {
        return vec2(1.0, 0.0);
    }
    float s = sqrt(discriminant);
    return vec2(max(-b - s, 0.0), min(-b + s, max_t));
}

void main() {
    float density = push.params.x;
    if (density <= 0.0) {
        out_volume = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Pixel de resolucion completa que representa a este de media (como la
    // GI: lighting.frag hace la misma cuenta al ampliar).
    ivec2 full_size = textureSize(g_depth, 0);
    ivec2 half_pixel = ivec2(gl_FragCoord.xy);
    ivec2 pixel = min(half_pixel * 2, full_size - 1);
    float depth = texelFetch(g_depth, pixel, 0).r;
    vec2 uv = (vec2(pixel) + 0.5) / vec2(full_size);

    // Rayo en el mundo hasta la superficie (o hasta la distancia maxima, en
    // el cielo).
    vec3 view_position = viewFromDepth(uv, min(depth, 0.999999));
    vec3 direction = transpose(mat3(camera.view)) * normalize(view_position);
    float max_distance = push.params.w;
    float march_distance =
        depth >= 1.0 ? max_distance : min(length(view_position), max_distance);

    // --- Sol: la fase es la misma en todo el rayo ---
    vec3 sun_radiance = toLinear(lights.sun_color_ambient.rgb) * lights.sun_direction_intensity.w;
    bool has_sun = dot(sun_radiance, sun_radiance) > 0.0;
    vec3 to_sun = normalize(-lights.sun_direction_intensity.xyz);
    vec3 sun_source = sun_radiance * phaseFunction(dot(direction, to_sun));

    // --- Que tramo del rayo cruza cada luz local (las que no toca, fuera) ---
    int point_count = min(lights.counts.x, kMaxPointLights);
    int spot_count = min(lights.counts.y, kMaxSpotLights);
    vec2 point_segments[kMaxPointLights];
    vec2 spot_segments[kMaxSpotLights];
    bool any_local = false;
    for (int i = 0; i < point_count; ++i) {
        point_segments[i] = sphereSegment(camera.position.xyz, direction,
                                          lights.points[i].position_range.xyz,
                                          lights.points[i].position_range.w, march_distance);
        any_local = any_local || point_segments[i].x < point_segments[i].y;
    }
    for (int i = 0; i < spot_count; ++i) {
        spot_segments[i] = sphereSegment(camera.position.xyz, direction,
                                         lights.spots[i].position_range.xyz,
                                         lights.spots[i].position_range.w, march_distance);
        any_local = any_local || spot_segments[i].x < spot_segments[i].y;
    }
    if (!has_sun && !any_local) {
        out_volume = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float dt = march_distance / float(kSteps);
    float jitter = bayer4(half_pixel);

    vec3 scattered = vec3(0.0);
    float transmittance = 1.0;
    for (int i = 0; i < kSteps; ++i) {
        float t = (float(i) + jitter) * dt;
        vec3 p = camera.position.xyz + direction * t;

        // Polvo sin color (dispersa todas las longitudes de onda igual) y sin
        // absorcion: extincion = dispersion.
        float sigma = density * dust(p, push.params.z);
        float step_transmittance = exp(-sigma * dt);

        // Luz que llega a este punto del aire, ya por su fase hacia la camara.
        vec3 incoming = has_sun ? sun_source * sunVisibility(p) : vec3(0.0);

        for (int l = 0; l < point_count; ++l) {
            if (t < point_segments[l].x || t > point_segments[l].y) {
                continue;
            }
            vec3 from_light = p - lights.points[l].position_range.xyz;
            float d = length(from_light);
            // La luz viaja de la lampara al punto y de ahi a la camara (-direction).
            float phase = phaseFunction(dot(from_light / max(d, 1e-4), -direction));
            float visibility = 1.0;
            int slot = shadowSlot(lights.points[l].shadow.x);
            if (slot >= 0 && local_shadows.params.z > 0.0) {
                visibility = pointVisibility(slot, lights.points[l].position_range.xyz, p);
            }
            incoming += toLinear(lights.points[l].color_intensity.rgb) *
                        (lights.points[l].color_intensity.a *
                         attenuation(d, lights.points[l].position_range.w) * phase * visibility);
        }

        for (int l = 0; l < spot_count; ++l) {
            if (t < spot_segments[l].x || t > spot_segments[l].y) {
                continue;
            }
            vec3 from_light = p - lights.spots[l].position_range.xyz;
            float d = length(from_light);
            vec3 travel = from_light / max(d, 1e-4);
            // Cono: 1 dentro del angulo interior, 0 fuera del exterior.
            float cone_cos = dot(travel, normalize(lights.spots[l].direction_intensity.xyz));
            float inner_cos = lights.spots[l].color_inner.a;
            float outer_cos = lights.spots[l].outer_shadow.x;
            float cone =
                clamp((cone_cos - outer_cos) / max(inner_cos - outer_cos, 0.0001), 0.0, 1.0);
            if (cone <= 0.0) {
                continue;
            }
            float phase = phaseFunction(dot(travel, -direction));
            float visibility = 1.0;
            int slot = shadowSlot(lights.spots[l].outer_shadow.y);
            if (slot >= 0 && local_shadows.params.z > 0.0) {
                visibility = spotVisibility(slot, p);
            }
            incoming += toLinear(lights.spots[l].color_inner.rgb) *
                        (lights.spots[l].direction_intensity.w * cone * cone *
                         attenuation(d, lights.spots[l].position_range.w) * phase * visibility);
        }

        // Integral exacta del tramo con la fuente constante.
        vec3 source = incoming * sigma;
        scattered += transmittance * (source - source * step_transmittance) / max(sigma, 1e-6);
        transmittance *= step_transmittance;
    }

    out_volume = vec4(scattered, transmittance);
}
