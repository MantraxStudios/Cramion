#version 450

// Escritura del G-buffer para los modelos: material PBR metal/rugosidad
// completo (el de glTF 2.0 y Unreal).
//
//   albedo_map              color base (sRGB)
//   metallic_roughness_map  B = metalicidad, G = rugosidad (lineal)
//   normal_map              normal en espacio tangente (lineal)
//   occlusion_map           R = oclusion ambiental horneada (lineal)
//   emissive_map            emision (sRGB), por el factor del material

layout(set = 1, binding = 0) uniform sampler2D albedo_map;
layout(set = 1, binding = 1) uniform sampler2D metallic_roughness_map;
layout(set = 1, binding = 2) uniform sampler2D normal_map;
layout(set = 1, binding = 3) uniform sampler2D occlusion_map;
layout(set = 1, binding = 4) uniform sampler2D emissive_map;

// Debe coincidir con GpuSkinnedPush (y con skinned.vert).
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 base_color;
    vec4 emissive;   // rgb = factor de emision
    vec4 material;   // x = metal, y = rugosidad, z = fuerza de la AO, w = escala normal
                     // (negativa: normal map de convenio DirectX)
    uint bone_offset;
    float reflectance;  // F0 de la parte no metalica
} push;

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_tangent;
layout(location = 3) in vec3 v_world_position;

// --- Lluvia (procedural) ---
// Mapa de lluvia: profundidad de la escena vista desde arriba. Lo que tiene
// algo encima (toldos, soportales, balcones) no se moja.
layout(set = 0, binding = 2) uniform sampler2D rain_map;
layout(set = 0, binding = 3) uniform WeatherBuffer {
    mat4 rain_view_projection;
    vec4 params;  // x = humedad (0..1), y = charcos (0..1), z = segundos, w = mapa listo
} weather;

layout(location = 0) out vec4 out_albedo;    // rgb = albedo, a = oclusion ambiental
layout(location = 1) out vec4 out_normal;    // rg = normal (octaedrica), b = rugosidad,
                                             // a = reflectancia (F0 no metalico)
layout(location = 2) out vec4 out_material;  // rgb = emision (HDR lineal), a = metalicidad

// Los materiales emisivos de un modelo (pantallas, luces del casco) se
// escriben en la misma escala HDR que los bloques luminosos, para que el
// bloom los recoja.
const float kEmissiveIntensity = 6.0;

// Igual que en geometry.frag.
vec2 encodeNormal(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 wrapped = (1.0 - abs(n.zx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);
    return n.y >= 0.0 ? n.xz : wrapped;
}

vec3 toLinear(vec3 color) {
    return pow(color, vec3(2.2));
}

// --- Ruido para la lluvia (todo procedural, en coordenadas del mundo) ---
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec2 hash22(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

float valueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1.0, 0.0)), u.x),
               mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0, 1.0)), u.x), u.y);
}

float fbm(vec2 p) {
    float sum = 0.0;
    float amplitude = 0.5;
    for (int i = 0; i < 4; ++i) {
        sum += valueNoise(p) * amplitude;
        p = p * 2.03 + vec2(17.1, 9.7);
        amplitude *= 0.5;
    }
    return sum;
}

// Ondas de las gotas en un charco: cada celda de la rejilla recibe una gota
// en un instante aleatorio; su anillo crece y se apaga. Devuelve la pendiente
// de la superficie (dh/dx, dh/dz) para inclinar la normal.
vec2 rainRipples(vec2 p, float time) {
    vec2 slope = vec2(0.0);
    for (int layer = 0; layer < 2; ++layer) {
        vec2 q = p * (layer == 0 ? 2.2 : 3.7) + float(layer) * 31.7;
        vec2 cell = floor(q);
        vec2 f = fract(q);
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                vec2 neighbor = vec2(x, y);
                vec2 random = hash22(cell + neighbor);
                // Una gota por celda cada ~1.3 s, cada una con su fase.
                float t = fract(time * 0.75 + random.x);
                vec2 to_center = f - (neighbor + random);
                float distance_to_center = length(to_center);
                float ring = distance_to_center - t * 0.9;
                // Paquete de ondas: seno dentro de una envolvente que se
                // desvanece al crecer el anillo.
                float envelope = smoothstep(0.12, 0.0, abs(ring)) * (1.0 - t) * (1.0 - t);
                float wave = cos(ring * 48.0) * envelope;
                slope += to_center / max(distance_to_center, 1e-4) * wave;
            }
        }
    }
    return slope * 0.35;
}

void main() {
    vec4 albedo = texture(albedo_map, v_uv) * push.base_color;

    // Recorte por alfa (pelo, pestanas): un diferido no puede mezclar
    // transparencias, asi que lo que es casi transparente se descarta.
    if (albedo.a < 0.5) {
        discard;
    }

    // --- Normal map ---
    // Base tangente-bitangente-normal interpolada (ortonormalizada con
    // Gram-Schmidt); w de la tangente da el sentido de la bitangente.
    vec3 n = normalize(v_normal);
    vec3 t = v_tangent.xyz - n * dot(n, v_tangent.xyz);
    vec3 normal = n;
    if (dot(t, t) > 1e-8) {
        t = normalize(t);
        vec3 b = cross(n, t) * (v_tangent.w < 0.0 ? -1.0 : 1.0);
        // Solo X e Y: Z se reconstruye (los normal maps BC5 de los DDS no la
        // guardan, y en los demas asi se corrige el error de compresion).
        vec2 xy = texture(normal_map, v_uv).xy * 2.0 - 1.0;
        vec3 tangent_normal = vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
        // La bitangente va con V creciendo hacia abajo en la imagen (assimp
        // la calcula despues de FlipUVs, y el lector de OBJ igual). El +Y de
        // un normal map OpenGL (glTF) apunta hacia arriba: se invierte. El de
        // uno DirectX (escala negativa) ya apunta hacia abajo.
        float scale = push.material.w;
        tangent_normal.y = scale >= 0.0 ? -tangent_normal.y : tangent_normal.y;
        tangent_normal.xy *= abs(scale);
        normal = normalize(mat3(t, b, n) * tangent_normal);
    }
    // Caras vistas por detras (mallas de una sola cara): la normal mira a la
    // camara.
    if (!gl_FrontFacing) {
        normal = -normal;
    }

    // --- Metal / rugosidad / oclusion ---
    vec4 metallic_roughness = texture(metallic_roughness_map, v_uv);
    float metallic = clamp(push.material.x * metallic_roughness.b, 0.0, 1.0);
    float roughness = clamp(push.material.y * metallic_roughness.g, 0.04, 1.0);
    float occlusion = mix(1.0, texture(occlusion_map, v_uv).r, push.material.z);

    // --- Emision ---
    vec3 emissive = toLinear(texture(emissive_map, v_uv).rgb) * push.emissive.rgb *
                    kEmissiveIntensity;

    // --- Lluvia: superficies mojadas y charcos ---
    // (Lagarde, "Water drop 2: Wet surfaces" / "Water drop 3: Physically
    // based wet surfaces"): el agua que empapa un material poroso lo oscurece
    // y lo alisa; en un charco manda el agua: casi espejo, F0 = 0.02 y la
    // normal de la lamina de agua (plana, con las ondas de las gotas).
    float reflectance = push.reflectance;
    if (weather.params.x > 0.0) {
        // A la intemperie: nada por encima en el mapa de lluvia.
        float exposed = 1.0;
        if (weather.params.w > 0.5) {
            vec4 rain_clip = weather.rain_view_projection * vec4(v_world_position, 1.0);
            vec3 rain = rain_clip.xyz / rain_clip.w;
            vec2 rain_uv = rain.xy * 0.5 + 0.5;
            if (all(greaterThanEqual(rain_uv, vec2(0.0))) && all(lessThanEqual(rain_uv, vec2(1.0)))) {
                float above = textureLod(rain_map, rain_uv, 0.0).r;
                // Margen: ~15 cm (el propio suelo tambien sale en el mapa).
                exposed = smoothstep(0.004, 0.0015, rain.z - above);
            }
        }

        vec3 up_normal = normalize(v_normal);
        float facing_up = smoothstep(0.3, 0.85, up_normal.y);
        // Las paredes se mojan poco (escurre); lo horizontal, del todo.
        float wet = weather.params.x * exposed * mix(0.25, 1.0, facing_up);

        // Charcos: donde el suelo es plano, en las zonas bajas de un ruido
        // grande y en los huecos oscuros entre adoquines.
        float cavity = 0.5 - dot(albedo.rgb, vec3(0.2126, 0.7152, 0.0722));
        float field = fbm(v_world_position.xz * 0.22) + cavity * 0.12;
        float puddle = smoothstep(0.52, 0.58, field) * smoothstep(0.92, 0.98, up_normal.y) *
                       exposed * weather.params.y * (1.0 - metallic);

        // Material empapado: mas oscuro y mas liso (menos en los metales,
        // que no absorben agua).
        float porous = wet * (1.0 - metallic);
        albedo.rgb *= mix(1.0, 0.5, porous);
        roughness = mix(roughness, min(roughness, 0.12), wet * 0.85);

        // Charco: la lamina de agua tapa el material.
        if (puddle > 0.0) {
            vec2 slope = rainRipples(v_world_position.xz, weather.params.z);
            vec3 water_normal = normalize(vec3(-slope.x, 1.0, -slope.y));
            albedo.rgb *= mix(1.0, 0.35, puddle);
            roughness = mix(roughness, 0.02, puddle);
            normal = normalize(mix(normal, water_normal, puddle));
            reflectance = mix(reflectance, 0.02, puddle);
            metallic *= 1.0 - puddle;
        }
    }

    out_albedo = vec4(albedo.rgb, occlusion);
    out_normal = vec4(encodeNormal(normal), roughness, reflectance);
    out_material = vec4(emissive, metallic);
}
