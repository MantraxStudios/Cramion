// Path tracing: todo menos el punto de entrada. Lo usan path_trace.comp
// (ray queries, cualquier GPU con rayos) y path_trace.rgen (pipeline de rayos
// con Shader Execution Reordering). Antes de incluirlo: rt_common.glsl.

// El rayo de cada rebote: cada punto de entrada lo lanza a su manera.
bool traceBounce(vec3 origin, vec3 direction, float max_distance, out RtHit hit);

layout(set = 0, binding = 7) uniform sampler2D g_albedo;    // rgb = albedo (sRGB), a = oclusion del material
layout(set = 0, binding = 8) uniform sampler2D g_material;  // rgb = emision, a = metalicidad
layout(set = 0, binding = 9, rgba32f) uniform image2D accumulation;
// Modelo de sombreado de Disney del G-buffer (el primer punto de cada camino).
layout(set = 0, binding = 10) uniform sampler2D g_shading;

#include "disney_brdf.glsl"

// push.params: x = numero de camino (semilla), y = caminos ya sumados (0 =
// empezar de nuevo), z = rebotes maximos, w = 1 si este frame suma un camino
// (0 = ya llego al tope: solo se muestra la media).

const float kSunAngularRadius = 0.00465;  // radianes (0.53 grados de disco)
const float kFireflyClamp = 12.0;         // tope de la luz indirecta por camino
const float kMaxDistance = 1000.0;

// --- Numeros aleatorios: PCG por pixel y camino ---
uint rng_state;

uint pcgNext() {
    rng_state = rng_state * 747796405u + 2891336453u;
    uint word = ((rng_state >> ((rng_state >> 28u) + 4u)) ^ rng_state) * 277803737u;
    return (word >> 22u) ^ word;
}

float random01() {
    return float(pcgNext()) * (1.0 / 4294967296.0);
}

// --- Material en un punto ---
struct Surface {
    vec3 position;
    vec3 normal;
    vec3 albedo;
    vec3 emission;
    float metallic;
    float roughness;
    float reflectance;  // F0 de la parte no metalica
    // Modelo de Disney: el del G-buffer en el primer punto; en los rebotes,
    // el estandar (las tablas de materiales de los rayos no lo llevan).
    ShadingModel model;
};

void basis(vec3 n, out vec3 t, out vec3 b) {
    vec3 helper = abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    t = normalize(cross(helper, n));
    b = cross(n, t);
}

vec3 fresnelSchlick(float cosine, vec3 f0) {
    return f0 + (1.0 - f0) * pow(1.0 - clamp(cosine, 0.0, 1.0), 5.0);
}

float ggxD(float n_dot_h, float alpha) {
    float a2 = alpha * alpha;
    float d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}

float smithG1(float n_dot_x, float alpha) {
    float a2 = alpha * alpha;
    return 2.0 * n_dot_x / (n_dot_x + sqrt(a2 + (1.0 - a2) * n_dot_x * n_dot_x));
}

// BRDF x coseno hacia `l`, para la luz directa: el mismo modelo de Disney
// que lighting.frag (alli con pi dentro de las luces, aqui sin el).
vec3 evalBrdf(Surface s, vec3 v, vec3 l) {
    vec3 f0 = disneyF0(s.model, s.albedo, s.reflectance, s.metallic);
    vec3 result = disneyBrdf(s.model, s.normal, v, l, s.albedo, s.roughness, s.metallic, f0, vec3(1.0), 0.0);
    if (dot(s.normal, l) <= 0.0) result = disneyTranslucency(s.model, s.normal, v, l, s.albedo, s.metallic);
    return result / kPi;
}

// Normal visible de GGX (Heitz 2018) en el espacio tangente (z = normal).
vec3 sampleGgxVndf(vec3 v_local, float alpha, vec2 xi) {
    vec3 vh = normalize(vec3(alpha * v_local.x, alpha * v_local.y, v_local.z));
    float len2 = vh.x * vh.x + vh.y * vh.y;
    vec3 t1 = len2 > 0.0 ? vec3(-vh.y, vh.x, 0.0) * inversesqrt(len2) : vec3(1.0, 0.0, 0.0);
    vec3 t2 = cross(vh, t1);
    float r = sqrt(xi.x);
    float phi = 2.0 * kPi * xi.y;
    float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    float s = 0.5 * (1.0 + vh.z);
    p2 = (1.0 - s) * sqrt(1.0 - p1 * p1) + s * p2;
    vec3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
    return normalize(vec3(alpha * nh.x, alpha * nh.y, max(0.0, nh.z)));
}

// Elige la direccion del siguiente rebote y devuelve el peso (BRDF x coseno /
// probabilidad). false si el camino se acaba.
bool sampleBrdf(Surface s, vec3 v, out vec3 direction, out vec3 weight) {
    vec3 n = s.normal;
    vec3 t;
    vec3 b;
    basis(n, t, b);
    float n_dot_v = max(dot(n, v), 1e-4);
    vec3 f0 = mix(vec3(s.reflectance), s.albedo, s.metallic);
    // Probabilidad de ir por el especular: lo que refleja de frente.
    float p_specular = clamp(mix(luminance(fresnelSchlick(n_dot_v, f0)), 1.0, s.metallic), 0.1, 0.9);
    vec2 xi = vec2(random01(), random01());

    if (random01() < p_specular) {
        float alpha = max(s.roughness * s.roughness, 0.002);
        vec3 v_local = vec3(dot(v, t), dot(v, b), n_dot_v);
        vec3 h_local = sampleGgxVndf(v_local, alpha, xi);
        vec3 h = t * h_local.x + b * h_local.y + n * h_local.z;
        direction = reflect(-v, h);
        float n_dot_l = dot(n, direction);
        if (n_dot_l <= 0.0) {
            return false;
        }
        // Con VNDF el peso se queda en F * G2 / G1(v).
        vec3 f = fresnelSchlick(max(dot(v, h), 0.0), f0);
        float g1_l = smithG1(n_dot_l, alpha);
        weight = f * g1_l / p_specular;
        return true;
    }

    // Difuso: coseno (el coseno y el 1/pi se van con la probabilidad).
    float phi = 2.0 * kPi * xi.y;
    float sin_theta = sqrt(xi.x);
    direction = normalize(t * (sin_theta * cos(phi)) + b * (sin_theta * sin(phi)) + n * sqrt(1.0 - xi.x));
    vec3 f = fresnelSchlick(max(dot(n, direction), 0.0), f0);
    weight = (1.0 - f) * s.albedo * (1.0 - s.metallic) / (1.0 - p_specular);
    return true;
}

// Visibilidad de un rayo de sombra con la fuerza de sombra de la luz
// (1 = sombra completa: tapado no llega nada).
float shadowVisibility(vec3 origin, vec3 direction, float distance, float strength) {
    if (strength <= 0.0) {
        return 1.0;
    }
    return unoccluded(origin, direction, distance) ? 1.0 : 1.0 - strength;
}

// Luz directa en un punto: el sol y una luz local al azar.
//
// La intensidad de las luces del motor va "por pi" (lighting.frag: difuso =
// albedo x luz x N.L, sin el 1/pi): se multiplica por pi para que el path
// tracing vea lo mismo que el raster. El cielo si es radiancia fisica.
vec3 directLight(Surface s, vec3 v) {
    vec3 result = vec3(0.0);
    vec3 origin = s.position + s.normal * (0.01 + (0.0005 + lights.rain.w * 1.5) * length(s.position - camera.position.xyz));

    // --- Sol: una direccion dentro de su disco ---
    vec3 to_sun = -normalize(lights.sun_direction_intensity.xyz);
    if (lights.sun_direction_intensity.w > 0.0 && dot(s.normal, to_sun) > -kSunAngularRadius) {
        vec3 t;
        vec3 b;
        basis(to_sun, t, b);
        float r = kSunAngularRadius * sqrt(random01());
        float phi = 2.0 * kPi * random01();
        vec3 l = normalize(to_sun + t * (r * cos(phi)) + b * (r * sin(phi)));
        vec3 brdf = evalBrdf(s, v, l);
        if (max(brdf.r, max(brdf.g, brdf.b)) > 0.0) {
            vec3 sun = toLinear(lights.sun_color_ambient.rgb) * lights.sun_direction_intensity.w * kPi;
            result += brdf * sun * shadowVisibility(origin, l, 10000.0, lights.ambient_color.w);
        }
    }

    // --- Una luz local (puntual o foco), elegida al azar ---
    int points = min(lights.counts.x, kMaxPointLights);
    int spots = min(lights.counts.y, kMaxSpotLights);
    int total = points + spots;
    if (total > 0) {
        int pick = min(int(random01() * float(total)), total - 1);
        vec3 position;
        float range;
        vec3 radiance;
        float strength;
        float cone = 1.0;
        if (pick < points) {
            position = lights.points[pick].position_range.xyz;
            range = lights.points[pick].position_range.w;
            radiance = toLinear(lights.points[pick].color_intensity.rgb) * lights.points[pick].color_intensity.a;
            // y = fuerza de la sombra (0 si la luz no proyecta).
            strength = lights.points[pick].shadow.y;
        } else {
            int i = pick - points;
            position = lights.spots[i].position_range.xyz;
            range = lights.spots[i].position_range.w;
            radiance = toLinear(lights.spots[i].color_inner.rgb) * lights.spots[i].direction_intensity.w;
            strength = lights.spots[i].outer_shadow.z;
            vec3 dir = normalize(position - s.position);
            float cosine = dot(-dir, normalize(lights.spots[i].direction_intensity.xyz));
            float inner_cos = lights.spots[i].color_inner.a;
            float outer_cos = lights.spots[i].outer_shadow.x;
            float c = clamp((cosine - outer_cos) / max(inner_cos - outer_cos, 0.0001), 0.0, 1.0);
            cone = c * c;
        }
        vec3 to_light = position - s.position;
        float d = length(to_light);
        if (d < range && cone > 0.0) {
            vec3 l = to_light / max(d, 0.0001);
            vec3 brdf = evalBrdf(s, v, l);
            if (max(brdf.r, max(brdf.g, brdf.b)) > 0.0) {
                float visibility = shadowVisibility(origin, l, max(d - 0.05, 0.0), strength);
                result += brdf * radiance * kPi * attenuation(d, range) * cone * visibility * float(total);
            }
        }
    }
    return result;
}

// Material de un punto de impacto de un rayo (sin normal map).
Surface hitSurface(RtHit hit, float lod) {
    RtMaterial material = rt_materials[hit.material];
    Surface s;
    s.position = hit.position;
    s.normal = hit.normal;
    s.albedo = toLinear(textureLod(rt_textures[nonuniformEXT(material.albedo_texture)], hit.uv, lod).rgb) *
               material.base_color.rgb;
    vec4 mr = textureLod(rt_textures[nonuniformEXT(material.metallic_roughness_texture)], hit.uv, lod);
    s.metallic = clamp(material.params.x * mr.b, 0.0, 1.0);
    s.roughness = clamp(material.params.y * mr.g, 0.04, 1.0);
    s.reflectance = 0.04;
    s.model = standardShading();
    s.emission = toLinear(textureLod(rt_textures[nonuniformEXT(material.emissive_texture)], hit.uv, lod).rgb) *
                 material.emissive.rgb * kEmissiveIntensity;
    return s;
}

// Niebla por altura (igual que lighting.frag): el path tracing sustituye a
// la iluminacion, asi que la pone el. `sky_visibility`: si el primer rebote de
// este camino vio el cielo (la media de muchos caminos es la visibilidad del
// cielo que la iluminacion saca de la GI).
const float kFogBaseHeight = 0.0;
const float kFogHeightFalloff = 0.08;

vec3 applyFog(vec3 color, vec3 position, float sky_visibility) {
    vec3 to_point = position - camera.position.xyz;
    float distance_to_point = length(to_point);
    vec3 ray_direction = to_point / max(distance_to_point, 1e-4);
    float falloff = lights.clouds.z > 0.0 ? lights.clouds.z : kFogHeightFalloff;
    float density = lights.clouds.y * exp(-(camera.position.y - kFogBaseHeight) * falloff);
    float b = falloff * ray_direction.y;
    float integral = abs(b) > 0.0001 ? (1.0 - exp(-distance_to_point * b)) / b : distance_to_point;
    float fog = clamp(1.0 - exp(-density * integral), 0.0, 1.0);

    vec3 fog_color = max(irradiance_sh.coefficients[0].rgb * 0.282095, vec3(0.0)) * (1.0 / kPi);
    float sun_alignment = max(dot(ray_direction, lights.sky_sun.xyz), 0.0);
    fog_color += toLinear(lights.sun_color_ambient.rgb) * lights.sun_direction_intensity.w *
                 pow(sun_alignment, 10.0) * 0.35 * smoothstep(-0.05, 0.1, lights.sky_sun.y);
    fog_color *= mix(0.08, 1.0, sky_visibility);
    fog_color += toLinear(lights.ambient_color.rgb) * lights.sun_color_ambient.a * 0.05;
    return mix(color, fog_color, fog);
}

vec3 skyRadiance(vec3 direction) {
    return textureLod(environment_map, direction, 0.0).rgb;
}

void pathTracePixel(ivec2 pixel, ivec2 size) {
    float depth = texelFetch(g_depth, pixel, 0).r;
    if (depth >= 1.0) {
        return;  // Cielo: lo que pinto la iluminacion.
    }

    vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
    vec3 position = worldFromDepth(uv, depth);
    float distance_to_camera = length(position - camera.position.xyz);

    vec4 accumulated = push.params.y > 0.5 ? imageLoad(accumulation, pixel) : vec4(0.0);

    if (push.params.w > 0.5) {
        rng_state = uint(pixel.x) * 1973u + uint(pixel.y) * 9277u + uint(push.params.x) * 26699u;
        pcgNext();

        // --- Primer punto: el G-buffer ---
        vec4 normal_sample = texelFetch(g_normal, pixel, 0);
        vec4 material_sample = texelFetch(g_material, pixel, 0);
        Surface s;
        s.position = position;
        s.normal = decodeNormal(normal_sample.rg);
        s.roughness = clamp(normal_sample.b, 0.04, 1.0);
        s.reflectance = normal_sample.a;
        s.albedo = toLinear(texelFetch(g_albedo, pixel, 0).rgb);
        // Alfa = metalicidad + 2 x sombra propia del relieve (gbuffer_surface.glsl).
        s.metallic = material_sample.a - 2.0 * floor(material_sample.a * 0.5);
        s.emission = material_sample.rgb;
        s.model = decodeShading(texelFetch(g_shading, pixel, 0), s.normal);

        vec3 v = normalize(camera.position.xyz - position);
        // La normal del normal map puede mirar de espaldas a la camara.
        if (dot(s.normal, v) < 0.0) {
            s.normal = normalize(s.normal + v * (0.01 - dot(s.normal, v)));
        }

        vec3 radiance = s.emission;
        vec3 throughput = vec3(1.0);
        float sky_visibility = 0.0;
        int max_bounces = int(push.params.z);

        for (int bounce = 0; bounce <= max_bounces; ++bounce) {
            vec3 direct = directLight(s, v);
            // Lo indirecto (a partir del primer rebote) con tope: sin
            // "luciernagas" de caminos improbables.
            vec3 contribution = throughput * direct;
            if (bounce > 0) {
                contribution *= min(1.0, kFireflyClamp / max(luminance(contribution), 1e-4));
            }
            radiance += contribution;
            if (bounce == max_bounces) {
                break;
            }

            vec3 direction;
            vec3 weight;
            if (!sampleBrdf(s, v, direction, weight)) {
                break;
            }
            throughput *= weight;

            // Ruleta rusa desde el tercer rebote.
            if (bounce >= 2) {
                float survive = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05, 0.95);
                if (random01() > survive) {
                    break;
                }
                throughput /= survive;
            }

            vec3 origin = s.position + s.normal * (0.01 + (0.0005 + lights.rain.w * 1.5) * length(s.position - camera.position.xyz));
            RtHit hit;
            if (!traceBounce(origin, direction, kMaxDistance, hit)) {
                if (bounce == 0) sky_visibility = 1.0;
                vec3 sky = throughput * skyRadiance(direction);
                radiance += sky * min(1.0, kFireflyClamp / max(luminance(sky), 1e-4));
                break;
            }
            s = hitSurface(hit, bounce == 0 ? 1.0 : 3.0);
            v = -direction;
            vec3 emitted = throughput * s.emission;
            radiance += emitted * min(1.0, kFireflyClamp / max(luminance(emitted), 1e-4));
        }

        radiance = applyFog(radiance, position, sky_visibility);
        if (!any(isnan(radiance)) && !any(isinf(radiance))) {
            accumulated += vec4(radiance, 1.0);
        }
        imageStore(accumulation, pixel, accumulated);
    }

    vec3 average = accumulated.w > 0.0 ? accumulated.rgb / accumulated.w : vec3(0.0);
    // a: distancia a la camara, como lighting.frag (la captura de la sonda).
    imageStore(output_image, pixel, vec4(average, distance_to_camera));
}
