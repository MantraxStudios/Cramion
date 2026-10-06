#version 450
#extension GL_GOOGLE_include_directive : require

// Escritura del G-buffer para los modelos: material PBR metal/rugosidad
// completo (el de glTF 2.0 y Unreal).
//
//   albedo_map              color base (sRGB)
//   metallic_roughness_map  B = metalicidad, G = rugosidad (lineal),
//                           R = reflectancia si hay mapa specular (flag),
//                           A = cavidad (1 = sin grietas)
//   normal_map              normal en espacio tangente (lineal)
//   occlusion_map           R = oclusion ambiental horneada (lineal),
//                           G = altura para el parallax (flag)
//   emissive_map            emision (sRGB), por el factor del material

// Igual que en skinned.vert (aqui solo se usa la posicion, para el parallax).
layout(set = 0, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
    mat4 unjittered_view_projection;
    mat4 previous_view_projection;
    vec4 jitter;
    uvec4 motion;
} camera;

layout(set = 1, binding = 0) uniform sampler2D albedo_map;
layout(set = 1, binding = 1) uniform sampler2D metallic_roughness_map;
layout(set = 1, binding = 2) uniform sampler2D normal_map;
layout(set = 1, binding = 3) uniform sampler2D occlusion_map;
layout(set = 1, binding = 4) uniform sampler2D emissive_map;

// Debe coincidir con GpuSkinnedPush (y con skinned.vert).
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 base_color;
    vec4 emissive;   // rgb = factor de emision, w = relieve del parallax (metros)
    vec4 material;   // x = metal, y = rugosidad, z = fuerza de la AO, w = escala normal
                     // (negativa: normal map de convenio DirectX)
    uint bone_offset;
    float reflectance;  // F0 de la parte no metalica
    uint pick_id;
    uint flags;      // bit 2: mapa specular, bit 3: mapa de alturas
} push;

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_tangent;
layout(location = 3) in vec3 v_world_position;
layout(location = 4) in vec4 v_current_clip;
layout(location = 5) in vec4 v_previous_clip;


// Los materiales emisivos de un modelo (pantallas, luces del casco) se
// escriben en la misma escala HDR que los bloques luminosos, para que el
// bloom los recoja.
const float kEmissiveIntensity = 6.0;

const uint kFlagSpecularMap = 1u << 2;
const uint kFlagHeightMap = 1u << 3;
const uint kFlagParallaxShadow = 1u << 5;

#include "gbuffer_surface.glsl"
const float kPi = 3.14159265;
#include "disney_brdf.glsl"

#include "parallax_common.glsl"

void main() {
    // --- Base tangente-bitangente-normal ---
    // Interpolada (ortonormalizada con Gram-Schmidt); w de la tangente da el
    // sentido de la bitangente.
    vec3 n = normalize(v_normal);
    vec3 t = v_tangent.xyz - n * dot(n, v_tangent.xyz);
    bool has_tangent = dot(t, t) > 1e-8;
    vec3 b = vec3(0.0);
    if (has_tangent) {
        t = normalize(t);
        b = cross(n, t) * (v_tangent.w < 0.0 ? -1.0 : 1.0);
    }

    // --- Parallax (mapa de alturas / displacement) ---
    // Se apaga con la distancia: de lejos no se nota y cuesta 8-32 lecturas.
    vec2 uv = v_uv;
    // Derivadas fuera de cualquier if (dentro no estan definidas).
    vec2 uv_dx = dFdx(v_uv);
    vec2 uv_dy = dFdy(v_uv);
    vec3 pos_dx = dFdx(v_world_position);
    vec3 pos_dy = dFdy(v_world_position);
    if ((push.flags & kFlagHeightMap) != 0u && has_tangent) {
        uv = applyParallax(occlusion_map, vec4(0.0, 1.0, 0.0, 0.0), push.emissive.w,
                           (push.flags & kFlagParallaxShadow) != 0u, v_uv, uv_dx, uv_dy, pos_dx, pos_dy,
                           v_world_position, t, b, n, surface_sun_shadow);
    }

    // El G-buffer guarda el albedo en sRGB (lighting.frag lo linealiza), pero
    // el factor del material es lineal (glTF): se lleva a sRGB antes de
    // multiplicar. Si no, lighting.frag lo elevaba a 2.2 otra vez y los
    // materiales sin textura salian mas oscuros y saturados (y distintos de
    // los mismos materiales vistos por los rayos, rt_common.glsl).
    vec4 albedo = textureGrad(albedo_map, uv, uv_dx, uv_dy) *
                  vec4(pow(max(push.base_color.rgb, vec3(0.0)), vec3(1.0 / 2.2)), push.base_color.a);

    // Recorte por alfa (pelo, pestanas): un diferido no puede mezclar
    // transparencias, asi que lo que es casi transparente se descarta.
    bool cut_out = albedo.a < 0.5;

    // Todas las texturas con las derivadas de la UV original (textureGrad):
    // con parallax la UV desplazada salta entre pixeles y texture() elegia un
    // mip equivocado en cada escalon (aliasing); y el normal map va dentro de
    // un if, donde las derivadas implicitas no estan definidas.

    // --- Normal map ---
    vec3 normal = n;
    // Normal en espacio tangente (z = 1: plano). Da la "altura" de la
    // superficie para el agua: lo inclinado son los bordes de los adoquines.
    vec3 tangent_normal = vec3(0.0, 0.0, 1.0);
    if (has_tangent) {
        // Solo X e Y: Z se reconstruye (los normal maps BC5 de los DDS no la
        // guardan, y en los demas asi se corrige el error de compresion).
        vec2 xy = textureGrad(normal_map, uv, uv_dx, uv_dy).xy * 2.0 - 1.0;
        tangent_normal = vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
        // La bitangente va con V creciendo hacia abajo en la imagen (assimp
        // la calcula despues de FlipUVs, y el lector de OBJ igual). El +Y de
        // un normal map OpenGL (glTF) apunta hacia arriba: se invierte. El de
        // uno DirectX (escala negativa) ya apunta hacia abajo.
        float scale = push.material.w;
        tangent_normal.y = scale >= 0.0 ? -tangent_normal.y : tangent_normal.y;
        tangent_normal.xy *= abs(scale);
        normal = normalize(mat3(t, b, n) * tangent_normal);
        tangent_normal = normalize(tangent_normal);
    }
    // Caras vistas por detras (mallas de una sola cara): la normal mira a la
    // camara.
    if (!gl_FrontFacing) {
        normal = -normal;
        n = -n;
    }
    // Normal para el antialiasing especular: la geometrica (sin normal map,
    // como en Filament; el normal map daria bloques de 2x2 pixeles).
    vec3 aa_normal = n;

    // --- Metal / rugosidad / specular / cavidad / oclusion ---
    vec4 metallic_roughness = textureGrad(metallic_roughness_map, uv, uv_dx, uv_dy);
    float metallic = clamp(push.material.x * metallic_roughness.b, 0.0, 1.0);
    float roughness = clamp(push.material.y * metallic_roughness.g, 0.04, 1.0);
    float occlusion = mix(1.0, textureGrad(occlusion_map, uv, uv_dx, uv_dy).r, push.material.z);
    // Mapa specular (el de Unreal): 0.5 = la reflectancia del material.
    float reflectance = push.reflectance;
    if ((push.flags & kFlagSpecularMap) != 0u) {
        reflectance *= metallic_roughness.r * 2.0;
    }
    // Cavidad: las grietas no reciben reflejos ni tanta luz ambiental.
    float cavity = metallic_roughness.a;
    reflectance *= cavity;
    occlusion *= mix(1.0, cavity, 0.6);

    // --- Emision ---
    vec3 emissive = toLinear(textureGrad(emissive_map, uv, uv_dx, uv_dy).rgb) * push.emissive.rgb *
                    kEmissiveIntensity;

    // --- Modelo de sombreado de Disney (push.pick_id: bits 0-3 modelo, 8-31
    // tres parametros de 8 bits) ---
    {
        uint packed = push.pick_id;
        int model = int(packed & 0xFu);
        vec3 params = vec3(float((packed >> 8u) & 0xFFu), float((packed >> 16u) & 0xFFu),
                           float((packed >> 24u) & 0xFFu)) / 255.0;
        if (model == kShadingAnisotropic || model == kShadingHair) {
            // La direccion del brillo: la tangente de la malla girada lo que
            // diga el material, sobre la normal final; se guarda como angulo.
            vec3 b1;
            vec3 b2;
            shadingBasis(normal, b1, b2);
            vec3 direction = has_tangent ? t : b1;
            float turn = params.y * kPi;
            vec3 side = cross(n, direction);
            direction = direction * cos(turn) + side * sin(turn);
            direction = direction - normal * dot(normal, direction);
            if (dot(direction, direction) < 1e-8) direction = b1;
            params.y = shadingEncodeAngle(normal, normalize(direction));
        }
        surface_shading = vec4(float(model) / 255.0, params);
    }

    writeSurface(albedo, n, normal, tangent_normal, aa_normal, metallic, roughness, occlusion, emissive,
                 reflectance, v_world_position);
    writeVelocity(v_current_clip, v_previous_clip);
    // El recorte se hace al final: un discard antes dejaba sin definir las
    // derivadas (dFdx/dFdy, texturas con mipmap) de los vecinos del cuadro 2x2
    // en los bordes recortados, y writeSurface las usa (antialiasing
    // especular, decals).
    if (cut_out) discard;
}
