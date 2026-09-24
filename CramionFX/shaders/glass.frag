#version 450

// Vidrio de las ventanas: pasada forward despues de la iluminacion diferida.
// Un G-buffer solo guarda una superficie por pixel, asi que el vidrio no
// entraba (se descartaba y las ventanas quedaban vacias). Aqui se dibuja
// encima de la imagen ya iluminada y se mezcla:
//
//     resultado = reflejo * R + lo_de_detras * T
//
// con R y T los de una lamina fina (dos caras): cada cara refleja
// f = Fresnel(F0 = 0.04, IOR 1.5) y la luz rebota entre las dos. Sumando
// todos los rebotes (serie geometrica, sin absorcion):
//
//     R = 2f / (1 + f),   T = (1 - f) / (1 + f),   R + T = 1
//
// De frente el vidrio refleja ~8%; de canto, casi todo: por eso las ventanas
// vistas de lado son espejos.
//
// El reflejo sale, por orden de preferencia:
//   1. de la propia imagen de este frame (copia sin vidrio): se sigue el
//      rayo reflejado por el depth buffer, como el SSR;
//   2. de la sonda de reflexion (la escena vista desde cerca);
//   3. del entorno (IBL).
// Y se suma el brillo del sol en el cristal, con su sombra.

layout(set = 1, binding = 0) uniform sampler2D albedo_map;

// Debe coincidir con GpuSkinnedPush (y con skinned.vert).
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 base_color;
    vec4 emissive;
    vec4 material;
    uint bone_offset;
    float reflectance;
} push;

const int kShadowCascadeCount = 4;

layout(set = 2, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
} camera;

// Solo el principio de GpuLights (lighting.frag): el sol y las sondas estan
// despues de las luces locales, asi que se declaran tambien esas.
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
} lights;

layout(set = 2, binding = 2) uniform ShadowBuffer {
    mat4 light_view_projection[kShadowCascadeCount];
    vec4 split_distances;
    vec4 texel_world_sizes;
    vec4 params;  // x = resolucion, y = intensidad
} shadows;

layout(set = 2, binding = 3) uniform sampler2DArrayShadow shadow_map;
layout(set = 2, binding = 4) uniform sampler2D g_depth;
// La imagen HDR iluminada de este frame, sin el vidrio.
layout(set = 2, binding = 5) uniform sampler2D scene_color;
layout(set = 2, binding = 6) uniform samplerCube environment_map;
layout(set = 2, binding = 7) uniform samplerCube reflection_probe_0;
layout(set = 2, binding = 8) uniform samplerCube reflection_probe_1;

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_tangent;
layout(location = 3) in vec3 v_world_position;

// rgb = reflejo, a = transmitancia (la mezcla multiplica lo de detras por a).
layout(location = 0) out vec4 out_color;

const float kPi = 3.14159265;
const float kGlassF0 = 0.04;
// Vidrio de ventana: casi liso (un poco de polvo y ondulacion).
const float kGlassRoughness = 0.04;
const float kMaxRadiance = 30.0;

// --- Trazado en pantalla (como ssr.frag, pero sobre este frame) ---
const int kSteps = 40;
const int kRefineSteps = 6;
const float kMaxDistance = 60.0;
const float kFirstStep = 0.05;

// Mismo mapeo de radiancia del disco solar que lighting.frag.
const float kSunDiskRadiance = 900.0;

vec3 toLinear(vec3 color) {
    return pow(color, vec3(2.2));
}

float linearDepth(float depth) {
    return camera.projection[3][2] / (depth + camera.projection[2][2]);
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

bool insideScreen(vec2 uv) {
    return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
}

// Sigue el rayo reflejado (espacio de vista) por el depth buffer. rgb = color
// de lo que choca, a = confianza (0 = nada en pantalla).
vec4 traceScreen(vec3 origin, vec3 ray) {
    ivec2 size = textureSize(g_depth, 0);
    float growth = pow(kMaxDistance / kFirstStep, 1.0 / float(kSteps));
    vec2 noise_pixel = gl_FragCoord.xy;
    float jitter = fract(52.9829189 * fract(dot(noise_pixel, vec2(0.06711056, 0.00583715))));
    float previous_distance = 0.0;
    float distance_along = kFirstStep * mix(1.0, growth, jitter);

    for (int i = 0; i < kSteps; ++i) {
        vec3 sample_position = origin + ray * distance_along;
        vec2 uv;
        if (!project(sample_position, uv) || !insideScreen(uv)) {
            break;
        }
        ivec2 texel = min(ivec2(uv * vec2(size)), size - 1);
        float scene_depth = texelFetch(g_depth, texel, 0).r;
        float difference = -sample_position.z - linearDepth(scene_depth);
        float thickness = (distance_along - previous_distance) * 1.5 + 0.05;

        if (scene_depth < 1.0 && difference > 0.0 && difference < thickness) {
            float low = previous_distance;
            float high = distance_along;
            for (int r = 0; r < kRefineSteps; ++r) {
                float middle = 0.5 * (low + high);
                vec3 p = origin + ray * middle;
                vec2 puv;
                project(p, puv);
                ivec2 ptexel = clamp(ivec2(puv * vec2(size)), ivec2(0), size - 1);
                float d = -p.z - linearDepth(texelFetch(g_depth, ptexel, 0).r);
                if (d > 0.0) {
                    high = middle;
                } else {
                    low = middle;
                }
            }
            vec3 refined = origin + ray * high;
            vec2 hit_uv;
            project(refined, hit_uv);
            ivec2 refined_texel = clamp(ivec2(hit_uv * vec2(size)), ivec2(0), size - 1);
            float gap = -refined.z - linearDepth(texelFetch(g_depth, refined_texel, 0).r);
            if (insideScreen(hit_uv) && abs(gap) < 0.04 + 0.01 * -refined.z) {
                vec3 color = min(textureLod(scene_color, hit_uv, 0.0).rgb, vec3(kMaxRadiance));
                vec2 edge = min(hit_uv, 1.0 - hit_uv);
                float edge_fade = smoothstep(0.0, 0.08, min(edge.x, edge.y));
                float distance_fade = 1.0 - smoothstep(0.7, 1.0, high / kMaxDistance);
                return vec4(color, edge_fade * distance_fade);
            }
        }
        previous_distance = distance_along;
        distance_along *= growth;
    }
    return vec4(0.0);
}

// --- Sombra del sol (cascadas, una muestra filtrada por hardware) ---
float sunShadow(vec3 world_position, vec3 normal) {
    float view_depth = -(camera.view * vec4(world_position, 1.0)).z;
    int cascade = kShadowCascadeCount - 1;
    for (int i = 0; i < kShadowCascadeCount; ++i) {
        if (view_depth < shadows.split_distances[i]) {
            cascade = i;
            break;
        }
    }
    vec3 offset_position = world_position + normal * shadows.texel_world_sizes[cascade] * 1.5;
    vec4 light_clip = shadows.light_view_projection[cascade] * vec4(offset_position, 1.0);
    vec3 projected = light_clip.xyz / light_clip.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    if (projected.z > 1.0 || projected.z < 0.0 || !insideScreen(uv)) {
        return 1.0;
    }
    float lit = texture(shadow_map, vec4(uv, float(cascade), projected.z));
    return mix(1.0, lit, shadows.params.y);
}

float distributionGgx(float n_dot_h, float alpha) {
    float alpha2 = alpha * alpha;
    float d = n_dot_h * n_dot_h * (alpha2 - 1.0) + 1.0;
    return alpha2 / (kPi * d * d);
}

float visibilitySmith(float n_dot_v, float n_dot_l, float alpha) {
    float alpha2 = alpha * alpha;
    float ggx_v = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - alpha2) + alpha2);
    float ggx_l = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - alpha2) + alpha2);
    return 0.5 / max(ggx_v + ggx_l, 0.00001);
}

float fresnel(float cosine) {
    float m = 1.0 - clamp(cosine, 0.0, 1.0);
    float m2 = m * m;
    return kGlassF0 + (1.0 - kGlassF0) * m2 * m2 * m;
}

// Reflectancia de la lamina fina (dos caras, todos los rebotes).
float slabReflectance(float cosine) {
    float f = fresnel(cosine);
    return 2.0 * f / (1.0 + f);
}

void main() {
    // Tinte del vidrio (casi siempre blanco): el factor es lineal, la
    // textura sRGB.
    vec3 tint = toLinear(texture(albedo_map, v_uv).rgb) * push.base_color.rgb;

    vec3 to_camera = camera.position.xyz - v_world_position;
    vec3 view_direction = normalize(to_camera);
    // El vidrio es una lamina: se ve igual por las dos caras.
    vec3 normal = normalize(v_normal);
    if (dot(normal, view_direction) < 0.0) {
        normal = -normal;
    }
    float n_dot_v = max(dot(normal, view_direction), 1e-4);

    float reflectance = slabReflectance(n_dot_v);
    float transmittance = 1.0 - reflectance;

    // --- Reflejo ---
    vec3 reflected = reflect(-view_direction, normal);
    vec3 view_position = (camera.view * vec4(v_world_position, 1.0)).xyz;
    vec3 view_normal = normalize(mat3(camera.view) * normal);
    vec3 view_ray = normalize(mat3(camera.view) * reflected);
    vec3 origin = view_position + view_normal * (0.01 + 0.002 * -view_position.z);
    // Los rayos que vuelven hacia la camara no encuentran nada en pantalla.
    float toward_camera = smoothstep(0.2, 0.6, view_ray.z);
    vec4 screen = toward_camera < 1.0 ? traceScreen(origin, view_ray) : vec4(0.0);
    screen.a *= 1.0 - toward_camera;

    // Respaldo: la sonda (la escena alrededor) o, sin ella, el entorno.
    float lod = kGlassRoughness * 5.0;
    vec3 fallback = textureLod(environment_map, reflected, lod).rgb;
    float probe_weight = lights.probes[0].w + lights.probes[1].w;
    if (probe_weight > 0.001) {
        vec3 probe = vec3(0.0);
        if (lights.probes[0].w > 0.001) {
            probe += textureLod(reflection_probe_0, reflected, lod).rgb * lights.probes[0].w;
        }
        if (lights.probes[1].w > 0.001) {
            probe += textureLod(reflection_probe_1, reflected, lod).rgb * lights.probes[1].w;
        }
        fallback = probe / probe_weight;
    }
    vec3 reflection = mix(min(fallback, vec3(kMaxRadiance)), screen.rgb, screen.a);

    // --- Brillo del sol en el cristal ---
    // La radiancia del sol reflejada en el cristal casi liso: el lobulo GGX
    // de la lamina (dos caras) iluminado por el sol, con su sombra.
    vec3 sun_direction = normalize(-lights.sun_direction_intensity.xyz);
    float n_dot_l = dot(normal, sun_direction);
    vec3 sun = vec3(0.0);
    if (n_dot_l > 0.0) {
        vec3 halfway = normalize(sun_direction + view_direction);
        float alpha = kGlassRoughness * kGlassRoughness;
        float specular = distributionGgx(max(dot(normal, halfway), 0.0), alpha) *
                         visibilitySmith(n_dot_v, n_dot_l, alpha) *
                         slabReflectance(max(dot(view_direction, halfway), 0.0)) * kPi;
        vec3 radiance = toLinear(lights.sun_color_ambient.rgb) * lights.sun_direction_intensity.w;
        sun = radiance * n_dot_l * specular * sunShadow(v_world_position, normal);
        // Sin tope el pico de GGX con esta rugosidad es enorme: se limita al
        // brillo del disco solar.
        sun = min(sun, radiance * kSunDiskRadiance * 0.05);
    }

    // El color del material (Kd del MTL) es el difuso, no cuanto deja pasar:
    // un vidrio con Kd 0.1 no es opaco. Solo se toma su tono (normalizado a
    // su canal mas alto) y lo que atraviesa se atenua segun ese tono. Usar el
    // Kd tal cual dejaba los vasos y botellas casi negros.
    float tint_peak = max(tint.r, max(tint.g, tint.b));
    vec3 hue = tint_peak > 1e-4 ? tint / tint_peak : vec3(1.0);
    float tint_transmission = dot(hue, vec3(0.2126, 0.7152, 0.0722));
    out_color = vec4(reflection * reflectance + sun,
                     transmittance * clamp(tint_transmission, 0.0, 1.0));
}
