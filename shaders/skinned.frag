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

    out_albedo = vec4(albedo.rgb, occlusion);
    out_normal = vec4(encodeNormal(normal), roughness, push.reflectance);
    out_material = vec4(emissive, metallic);
}
