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
// Ademas del sol y las luces, el polvo recibe la luz del CIELO desde todas
// direcciones (el "Sky Light" de la niebla volumetrica de Unreal). Sin ella
// el polvo solo existia mirando hacia el sol (la fase HG con g = 0.6 da ~25
// veces mas luz de frente que de espaldas) y de espaldas el aire quedaba
// perfectamente limpio. Es la radiancia media del cielo (termino constante
// de sus armonicos esfericos); a la sombra del sol se atenua (interiores,
// bajo los arboles: ahi el cielo se ve poco).
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
    vec4 color_intensity;   // rgb = color (lineal), a = intensidad
    vec4 shadow;            // x   = hueco de sombra (-1 = sin sombra)
};

struct SpotLightGpu {
    vec4 position_range;       // xyz = posicion,  w = alcance
    vec4 direction_intensity;  // xyz = direccion, w = intensidad
    vec4 color_inner;          // rgb = color (lineal), a = cos del angulo interior
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

// Irradiancia del cielo en armonicos esfericos, ya dividida por pi (la de
// lighting.frag): el coeficiente 0 da la radiancia media.
layout(set = 0, binding = 8) readonly buffer Irradiance {
    vec4 coefficients[9];
} irradiance_sh;

// Volumenes de niebla locales (GpuFogVolumes): cajas o esferas con su
// densidad, color, borde suave y ruido (niebla de un pantano, humo en una
// habitacion, vaho en una cueva).
const int kMaxFogVolumes = 16;
struct FogVolumeGpu {
    mat4 to_local;        // mundo -> local (caja [-0.5, 0.5]^3, esfera de radio 0.5)
    vec4 color_density;   // rgb = albedo lineal, a = densidad (1/m)
    vec4 params;          // x = forma (0 caja, 1 esfera), y = borde, z = ruido, w = escala del ruido
};
layout(set = 0, binding = 9) uniform FogVolumeBuffer {
    ivec4 count;
    FogVolumeGpu volumes[kMaxFogVolumes];
} fog_volumes;

layout(push_constant) uniform PushConstants {
    // x = densidad del polvo (1/m), y = anisotropia (g de Henyey-Greenstein),
    // z = segundos (deriva del polvo), w = distancia maxima (m)
    vec4 params;
    // x = tramos por rayo (8-32; el presupuesto adaptativo baja a 16)
    vec4 quality;
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_volume;

const float kPi = 3.14159265;
const int kSteps = 32;  // maximo (push.quality.x elige)
// Fraccion isotropa de la fase.
const float kIsotropicMix = 0.25;
// Cuanto de la luz del cielo dispersa el polvo (1 = todo; menos porque el
// suelo y los objetos tapan parte del cielo).
const float kSkyScatter = 0.6;

vec3 toLinear(vec3 color) {
    return pow(color, vec3(2.2));
}

float linearDepth(float depth) {
    return (camera.projection[3][2] - depth * camera.projection[3][3]) / (camera.projection[2][2] - depth * camera.projection[2][3]);
}

vec3 viewFromDepth(vec2 uv, float depth) {
    float z = linearDepth(depth);
    vec2 ndc = uv * 2.0 - 1.0;
    // Con el desplazamiento del centro ([2][0], [2][1]): cada ojo de un casco
    // de VR tiene un campo de vision asimetrico (y el TAA mueve el centro con
    // su jitter). Sin el, en VR todo salia desplazado y la luz se ennegrecia.
    // Forma general (perspectiva y ortografica): w del clip = P[2][3] z_v +
    // P[3][3] con z_v = -z. En perspectiva es lo de siempre, (ndc + P[2][.]) z / P[.][.].
    float w = camera.projection[3][3] - camera.projection[2][3] * z;
    return vec3((ndc.x * w + camera.projection[2][0] * z - camera.projection[3][0]) / camera.projection[0][0],
                (ndc.y * w + camera.projection[2][1] * z - camera.projection[3][1]) / camera.projection[1][1], -z);
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
//
// A lo largo del rayo la profundidad de vista y el clip de cada cascada son
// lineales en t: se calculan una vez antes de la marcha (sunRayStart) y en
// cada paso solo se suman (antes, una matriz por paso con indice dinamico).
float ray_view_depth0;
float ray_view_depth_step;
vec4 ray_light_clip0[kShadowCascadeCount];
vec4 ray_light_clip_step[kShadowCascadeCount];

void sunRayStart(vec3 origin, vec3 direction) {
    ray_view_depth0 = -(camera.view * vec4(origin, 1.0)).z;
    ray_view_depth_step = -(mat3(camera.view) * direction).z;
    for (int i = 0; i < kShadowCascadeCount; ++i) {
        ray_light_clip0[i] = shadows.light_view_projection[i] * vec4(origin, 1.0);
        ray_light_clip_step[i] = shadows.light_view_projection[i] * vec4(direction, 0.0);
    }
}

float sunVisibility(float t) {
    float view_depth = ray_view_depth0 + ray_view_depth_step * t;
    int cascade = kShadowCascadeCount - 1;
    for (int i = 0; i < kShadowCascadeCount; ++i) {
        if (view_depth < shadows.split_distances[i]) {
            cascade = i;
            break;
        }
    }
    vec4 light_clip = ray_light_clip0[cascade] + ray_light_clip_step[cascade] * t;
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

// Densidad de los volumenes de niebla en p; `albedo_sigma` suma su color
// por su densidad (la dispersion con color).
float fogVolumeDensity(vec3 p, float time, out vec3 albedo_sigma) {
    albedo_sigma = vec3(0.0);
    float total = 0.0;
    int count = min(fog_volumes.count.x, kMaxFogVolumes);
    for (int i = 0; i < count; ++i) {
        vec3 local = (fog_volumes.volumes[i].to_local * vec4(p, 1.0)).xyz;
        vec4 prm = fog_volumes.volumes[i].params;
        // 0 en el centro, 1 en el borde (caja: el eje mas cercano al borde).
        float edge = prm.x > 0.5 ? length(local) * 2.0 : max(abs(local.x), max(abs(local.y), abs(local.z))) * 2.0;
        if (edge >= 1.0) continue;
        float soft = max(prm.y, 1e-3);
        float w = clamp((1.0 - edge) / soft, 0.0, 1.0);
        w = w * w * (3.0 - 2.0 * w);
        if (prm.z > 0.0) {
            vec3 drift = vec3(0.11, 0.03, 0.07) * time;
            float n = valueNoise(p * prm.w + drift) * 0.6 + valueNoise(p * prm.w * 2.7 - drift) * 0.4;
            w *= mix(1.0, smoothstep(0.25, 0.75, n) * 1.6, prm.z);
        }
        float sigma = fog_volumes.volumes[i].color_density.a * w;
        total += sigma;
        albedo_sigma += fog_volumes.volumes[i].color_density.rgb * sigma;
    }
    return total;
}

void main() {
    float density = push.params.x;
    bool has_fog_volumes = fog_volumes.count.x > 0;
    if (density <= 0.0 && !has_fog_volumes) {
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
    // Perspectiva: desde la camara. Ortografica (P[2][3] = 0): rayos
    // paralelos (-Z de la vista) desde el plano cercano de cada pixel.
    vec3 view_origin = camera.projection[2][3] == 0.0 ? viewFromDepth(uv, 0.0) : vec3(0.0);
    vec3 ray_origin = camera.position.xyz + transpose(mat3(camera.view)) * view_origin;
    vec3 direction = transpose(mat3(camera.view)) * normalize(view_position - view_origin);
    float max_distance = push.params.w;
    float march_distance =
        depth >= 1.0 ? max_distance : min(length(view_position - view_origin), max_distance);

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
        point_segments[i] = sphereSegment(ray_origin, direction,
                                          lights.points[i].position_range.xyz,
                                          lights.points[i].position_range.w, march_distance);
        any_local = any_local || point_segments[i].x < point_segments[i].y;
    }
    for (int i = 0; i < spot_count; ++i) {
        spot_segments[i] = sphereSegment(ray_origin, direction,
                                         lights.spots[i].position_range.xyz,
                                         lights.spots[i].position_range.w, march_distance);
        any_local = any_local || spot_segments[i].x < spot_segments[i].y;
    }
    // Luz del cielo dispersada: radiancia media L de todas direcciones por
    // la fase isotropa 1/(4 pi) integrada en la esfera (4 pi) = L.
    vec3 sky_source = max(irradiance_sh.coefficients[0].rgb * 0.282095, vec3(0.0)) * kSkyScatter;
    bool has_sky = dot(sky_source, sky_source) > 0.0;
    if (!has_sun && !any_local && !has_sky) {
        out_volume = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    int steps = clamp(int(push.quality.x + 0.5), 8, kSteps);
    float dt = march_distance / float(steps);
    float jitter = bayer4(half_pixel);

    if (has_sun) sunRayStart(ray_origin, direction);

    vec3 scattered = vec3(0.0);
    float transmittance = 1.0;
    for (int i = 0; i < kSteps; ++i) {
        if (i >= steps) {
            break;
        }
        float t = (float(i) + jitter) * dt;
        vec3 p = ray_origin + direction * t;

        // Polvo sin color (dispersa todas las longitudes de onda igual) y sin
        // absorcion: extincion = dispersion.
        float sigma_dust = density > 0.0 ? density * dust(p, push.params.z) : 0.0;
        vec3 fog_albedo_sigma = vec3(0.0);
        float sigma_fog = has_fog_volumes ? fogVolumeDensity(p, push.params.z, fog_albedo_sigma) : 0.0;
        float sigma = sigma_dust + sigma_fog;
        if (sigma <= 1e-7) {
            continue;
        }
        float step_transmittance = exp(-sigma * dt);

        // Luz que llega a este punto del aire, ya por su fase hacia la camara.
        float sun_visible = has_sun ? sunVisibility(t) : 1.0;
        vec3 incoming = has_sun ? sun_source * sun_visible : vec3(0.0);
        incoming += sky_source * mix(0.35, 1.0, sun_visible);

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
            incoming += lights.points[l].color_intensity.rgb *
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
            incoming += lights.spots[l].color_inner.rgb *
                        (lights.spots[l].direction_intensity.w * cone * cone *
                         attenuation(d, lights.spots[l].position_range.w) * phase * visibility);
        }

        // Integral exacta del tramo con la fuente constante (el polvo sin
        // color, los volumenes con el suyo).
        vec3 source = incoming * (vec3(sigma_dust) + fog_albedo_sigma);
        scattered += transmittance * (source - source * step_transmittance) / max(sigma, 1e-6);
        transmittance *= step_transmittance;
    }

    out_volume = vec4(scattered, transmittance);
}
