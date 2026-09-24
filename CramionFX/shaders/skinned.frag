#version 450
#extension GL_GOOGLE_include_directive : require

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
// Decals (GpuDecal): caja unidad proyectada a lo largo de su eje Y local.
const int kMaxDecals = 64;
struct Decal {
    mat4 world_to_decal;
    vec4 color;     // rgb = tinte (sRGB), a = opacidad
    vec4 axis;      // xyz = eje de proyeccion (mundo), w = coseno minimo con la superficie
    vec4 params;    // x = tipo (0 estampa, 1 charco, 2 humedad), y = textura, z = borde, w = cantidad
    vec4 material;  // x = rugosidad, y = cuanto la sustituye, z = metalicidad
};

layout(set = 0, binding = 3) uniform WeatherBuffer {
    mat4 rain_view_projection;
    vec4 params;  // x = humedad (0..1), y = charcos (0..1), z = segundos, w = mapa listo
    vec4 flood;   // zona inundada: xy = centro (x, z), zw = radios (0 = sin agua)
    vec4 decal_info;  // x = numero de decals
    Decal decals[kMaxDecals];
} weather;
layout(set = 0, binding = 4) uniform sampler2D decal_textures[8];

layout(location = 0) out vec4 out_albedo;    // rgb = albedo, a = oclusion ambiental
layout(location = 1) out vec4 out_normal;    // rg = normal (octaedrica), b = rugosidad,
                                             // a = reflectancia (F0 no metalico)
layout(location = 2) out vec4 out_material;  // rgb = emision (HDR lineal), a = metalicidad

#include "rain_common.glsl"

// Los materiales emisivos de un modelo (pantallas, luces del casco) se
// escriben en la misma escala HDR que los bloques luminosos, para que el
// bloom los recoja.
const float kEmissiveIntensity = 6.0;

// Antialiasing especular geometrico (Tokuyoshi y Kaplanyan, "Improved
// Geometric Specular Antialiasing", I3D 2019; lo usan Filament y HDRP): si la
// normal cambia mucho dentro de un pixel, el reflejo de ese pixel es la media
// de muchas direcciones, no un espejo. Se ensancha el lobulo GGX con la
// varianza de la normal en pantalla. Sin esto el agua con ondas, vista de
// lejos, era un hervidero de puntos brillantes.
const float kSpecularAaVariance = 0.15;
const float kSpecularAaThreshold = 0.1;

// Igual que en geometry.frag.
vec2 encodeNormal(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 wrapped = (1.0 - abs(n.zx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);
    return n.y >= 0.0 ? n.xz : wrapped;
}

vec3 toLinear(vec3 color) {
    return pow(color, vec3(2.2));
}

vec3 toSrgb(vec3 color) {
    return pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));
}

// Cuanto se moja este punto: 1 a la intemperie, 0 bajo techo.
//
// El mapa de lluvia tiene texeles de ~10 cm: leerlo en un solo texel dejaba
// el borde entre lo mojado y lo seco en escalones. Se filtra la COMPARACION
// (como el PCF de las sombras) con un B-spline cubico de 4x4 texeles, que da
// una transicion suave de ~20 cm, y la posicion se desplaza con un ruido del
// mundo: la linea de goteo de un toldo real no es recta.
float rainExposure(vec3 world_position) {
    if (weather.params.w < 0.5) {
        return 1.0;
    }
    vec4 rain_clip = weather.rain_view_projection * vec4(world_position, 1.0);
    vec3 rain = rain_clip.xyz / rain_clip.w;
    vec2 rain_uv = rain.xy * 0.5 + 0.5;
    if (any(lessThan(rain_uv, vec2(0.0))) || any(greaterThan(rain_uv, vec2(1.0)))) {
        return 1.0;
    }

    ivec2 size = textureSize(rain_map, 0);
    vec2 wobble = vec2(rainValueNoise(world_position.xz * 3.1),
                       rainValueNoise(world_position.xz * 3.1 + 17.3)) - 0.5;
    vec2 texel = rain_uv * vec2(size) - 0.5 + wobble * 1.5;
    ivec2 base = ivec2(floor(texel));
    vec2 f = fract(texel);

    // Pesos del B-spline cubico uniforme para los texeles -1, 0, 1, 2.
    vec2 f2 = f * f;
    vec2 f3 = f2 * f;
    vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    vec2 w3 = f3 / 6.0;
    float wx[4] = float[](w0.x, w1.x, w2.x, w3.x);
    float wy[4] = float[](w0.y, w1.y, w2.y, w3.y);

    float exposed = 0.0;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            ivec2 p = clamp(base + ivec2(x - 1, y - 1), ivec2(0), size - 1);
            float above = texelFetch(rain_map, p, 0).r;
            // Margen: ~15 cm (el propio suelo tambien sale en el mapa).
            exposed += smoothstep(0.004, 0.0015, rain.z - above) * wx[x] * wy[y];
        }
    }
    return exposed;
}

void main() {
    // El G-buffer guarda el albedo en sRGB (lighting.frag lo linealiza), pero
    // el factor del material es lineal (glTF): se lleva a sRGB antes de
    // multiplicar. Si no, lighting.frag lo elevaba a 2.2 otra vez y los
    // materiales sin textura salian mas oscuros y saturados (y distintos de
    // los mismos materiales vistos por los rayos, rt_common.glsl).
    vec4 albedo = texture(albedo_map, v_uv) *
                  vec4(pow(max(push.base_color.rgb, vec3(0.0)), vec3(1.0 / 2.2)), push.base_color.a);

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
    // Normal en espacio tangente (z = 1: plano). Da la "altura" de la
    // superficie para el agua: lo inclinado son los bordes de los adoquines.
    vec3 tangent_normal = vec3(0.0, 0.0, 1.0);
    if (dot(t, t) > 1e-8) {
        t = normalize(t);
        vec3 b = cross(n, t) * (v_tangent.w < 0.0 ? -1.0 : 1.0);
        // Solo X e Y: Z se reconstruye (los normal maps BC5 de los DDS no la
        // guardan, y en los demas asi se corrige el error de compresion).
        vec2 xy = texture(normal_map, v_uv).xy * 2.0 - 1.0;
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

    // --- Metal / rugosidad / oclusion ---
    vec4 metallic_roughness = texture(metallic_roughness_map, v_uv);
    float metallic = clamp(push.material.x * metallic_roughness.b, 0.0, 1.0);
    float roughness = clamp(push.material.y * metallic_roughness.g, 0.04, 1.0);
    float occlusion = mix(1.0, texture(occlusion_map, v_uv).r, push.material.z);

    // --- Emision ---
    vec3 emissive = toLinear(texture(emissive_map, v_uv).rgb) * push.emissive.rgb *
                    kEmissiveIntensity;

    // --- Lluvia: superficies mojadas y agua acumulada (rain_common.glsl) ---
    // Tamano del pixel en el mundo, para apagar las ondas que no caben. Las
    // derivadas se toman aqui, fuera de los if: dentro de un flujo que no es
    // uniforme en el quad no estan definidas.
    vec3 dpdx = dFdx(v_world_position);
    vec3 dpdy = dFdy(v_world_position);
    float footprint = max(length(dpdx), length(dpdy));
    float reflectance = push.reflectance;

    // --- Decals: estampas, charcos y humedad locales (como los de Unreal) ---
    // Cada uno es una caja: lo que cae dentro recibe su textura/color, su agua
    // o su humedad, con el borde suavizado y sin pintar las caras que no miran
    // al eje de proyeccion. El indice de textura sale del buffer: es el mismo
    // para todo el grupo (uniforme dinamico), se puede indexar el array.
    float decal_water = 0.0;
    float decal_wet = 0.0;
    int decal_count = int(weather.decal_info.x);
    for (int i = 0; i < decal_count; ++i) {
        vec3 p = (weather.decals[i].world_to_decal * vec4(v_world_position, 1.0)).xyz;
        if (any(greaterThan(abs(p), vec3(0.5)))) {
            continue;
        }
        vec4 axis = weather.decals[i].axis;
        vec4 params = weather.decals[i].params;
        float facing = dot(n, axis.xyz);
        if (facing < axis.w) {
            continue;
        }
        // Borde suave en las cuatro caras laterales y a lo largo del eje.
        float edge = params.z;
        vec2 side = smoothstep(vec2(0.5), vec2(0.5 - edge), abs(p.xz));
        float depth_fade = smoothstep(0.5, 0.5 - edge * 0.5, abs(p.y));
        float angle_fade = smoothstep(axis.w, min(axis.w + 0.25, 1.0), facing);
        float mask = side.x * side.y * depth_fade * angle_fade;

        vec2 uv = vec2(p.x + 0.5, 0.5 - p.z);
        mat3 to_decal = mat3(weather.decals[i].world_to_decal);
        vec2 duvdx = (to_decal * dpdx).xz * vec2(1.0, -1.0);
        vec2 duvdy = (to_decal * dpdy).xz * vec2(1.0, -1.0);
        int texture_index = int(params.y);
        vec4 texel = texture_index >= 0 ? textureGrad(decal_textures[texture_index], uv, duvdx, duvdy)
                                        : vec4(1.0);
        vec4 color = weather.decals[i].color;
        int type = int(params.x + 0.5);
        if (type == 0) {
            // Estampa: color/textura encima del material.
            float a = clamp(texel.a * color.a * mask, 0.0, 1.0);
            albedo.rgb = mix(albedo.rgb, texel.rgb * color.rgb, a);
            vec4 material = weather.decals[i].material;
            roughness = mix(roughness, clamp(material.x, 0.04, 1.0), a * material.y);
            metallic = mix(metallic, material.z, a * material.y);
        } else if (type == 1) {
            // Charco: la textura (su alfa o su luminancia) da la forma; sin
            // textura, una mancha con la orilla irregular.
            float shape = texture_index >= 0 ? texel.a * dot(texel.rgb, vec3(0.333))
                                             : clamp((1.0 - length(p.xz) * 2.0 +
                                                      (rainFbm(v_world_position.xz * 1.3 + float(i) * 7.1) - 0.5) * 0.6) /
                                                         0.35, 0.0, 1.0);
            decal_water = max(decal_water, shape * mask * params.w * color.a);
        } else {
            // Humedad: moja sin encharcar.
            decal_wet = max(decal_wet, texel.a * mask * params.w * color.a);
        }
    }

    float flood = max(floodLevel(v_world_position.xz, weather.flood), decal_water);
    if (weather.params.x > 0.0 || weather.params.y > 0.0 || flood > 0.0 || decal_wet > 0.0) {
        float exposed = rainExposure(v_world_position);

        // El agua se queda en lo horizontal; las paredes escurren.
        float facing_up = smoothstep(0.3, 0.85, n.y);
        float flat_ground = smoothstep(0.92, 0.98, n.y);

        // Nivel del agua: charcos de la lluvia (solo a la intemperie) y la
        // zona inundada (esta aunque haya techo).
        float level = max(puddleLevel(v_world_position.xz, weather.params.y) * exposed, flood) *
                      flat_ground * (1.0 - metallic * 0.5);

        // Altura relativa del punto: las juntas son oscuras y estan
        // inclinadas en el normal map; las caras de los adoquines, claras y
        // planas.
        float brightness = dot(albedo.rgb, vec3(0.2126, 0.7152, 0.0722));
        float height = 0.55 * smoothstep(0.05, 0.45, brightness) +
                       0.45 * smoothstep(0.75, 0.98, tangent_normal.z);
        float water = waterCoverage(level, height);
        float depth = waterDepth(level, height);

        // Empapado: la lluvia de ahora, y del todo junto al agua (la orilla
        // de un charco esta saturada aunque el agua no la cubra).
        float wet = max(max(weather.params.x * exposed * mix(0.25, 1.0, facing_up),
                            clamp(level * 3.0, 0.0, 1.0)),
                        decal_wet);

        // Material empapado (Lagarde): en lineal, no sobre el sRGB.
        vec2 wet_factors = wetFactors(roughness, metallic, wet);
        vec3 linear_albedo = toLinear(albedo.rgb) * wet_factors.x;
        roughness = max(roughness * wet_factors.y, 0.04);

        if (water > 0.0) {
            // Lo de debajo se ve a traves del agua: la luz cruza la lamina
            // dos veces (entrar y salir).
            linear_albedo *= mix(vec3(1.0), exp(-kPuddleAbsorption * 2.0 * depth), water);

            // Superficie del agua: horizontal, con las ondas de las gotas
            // donde llueve (bajo techo el agua de la inundacion esta quieta).
            vec2 slope = rainRipples(v_world_position.xz, weather.params.z,
                                     weather.params.x * exposed, footprint) *
                         exposed;
            vec3 water_normal = normalize(vec3(-slope.x, 1.0, -slope.y));

            // Con poca agua (juntas medio llenas) aun manda el relieve del
            // material; con agua de sobra, la lamina.
            float cover = smoothstep(0.0, 0.6, water);
            normal = normalize(mix(normal, water_normal, cover));
            aa_normal = normalize(mix(aa_normal, water_normal, cover));
            roughness = mix(roughness, kWaterRoughness, water);
            reflectance = mix(reflectance, kWaterF0, water);
            metallic *= 1.0 - water;
        }
        albedo.rgb = toSrgb(linear_albedo);
    }

    // --- Antialiasing especular (Tokuyoshi 2019) ---
    vec3 du = dFdx(aa_normal);
    vec3 dv = dFdy(aa_normal);
    float variance = kSpecularAaVariance * (dot(du, du) + dot(dv, dv));
    float alpha = roughness * roughness;
    float alpha2 = clamp(alpha * alpha + min(2.0 * variance, kSpecularAaThreshold), 0.0, 1.0);
    roughness = clamp(sqrt(sqrt(alpha2)), 0.0, 1.0);

    out_albedo = vec4(albedo.rgb, occlusion);
    out_normal = vec4(encodeNormal(normal), roughness, reflectance);
    out_material = vec4(emissive, metallic);
}
