// Escritura del G-buffer comun a todo lo que es suelo o superficie opaca
// (modelos, terreno): lluvia y charcos, decals, humedad, antialiasing
// especular y codificacion. Quien lo incluye declara antes su material y
// llama a writeSurface() al final de su main().
//
// Usa el set 0 de la geometria: 2 = mapa de lluvia, 3 = clima y decals,
// 4 = texturas de decals.

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


// `n` es la normal geometrica (sin normal map), `normal` la final,
// `tangent_normal` la del normal map en espacio tangente (z = 1 si no hay) y
// `aa_normal` la que usa el antialiasing especular.
void writeSurface(vec4 albedo, vec3 n, vec3 normal, vec3 tangent_normal, vec3 aa_normal, float metallic,
                  float roughness, float occlusion, vec3 emissive, float reflectance, vec3 world_position) {
    // --- Lluvia: superficies mojadas y agua acumulada (rain_common.glsl) ---
    // Tamano del pixel en el mundo, para apagar las ondas que no caben. Las
    // derivadas se toman aqui, fuera de los if: dentro de un flujo que no es
    // uniforme en el quad no estan definidas.
    vec3 dpdx = dFdx(world_position);
    vec3 dpdy = dFdy(world_position);
    float footprint = max(length(dpdx), length(dpdy));

    // --- Decals: estampas, charcos y humedad locales (como los de Unreal) ---
    // Cada uno es una caja: lo que cae dentro recibe su textura/color, su agua
    // o su humedad, con el borde suavizado y sin pintar las caras que no miran
    // al eje de proyeccion. El indice de textura sale del buffer: es el mismo
    // para todo el grupo (uniforme dinamico), se puede indexar el array.
    float decal_water = 0.0;
    float decal_wet = 0.0;
    int decal_count = int(weather.decal_info.x);
    for (int i = 0; i < decal_count; ++i) {
        vec3 p = (weather.decals[i].world_to_decal * vec4(world_position, 1.0)).xyz;
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
                                                      (rainFbm(world_position.xz * 1.3 + float(i) * 7.1) - 0.5) * 0.6) /
                                                         0.35, 0.0, 1.0);
            decal_water = max(decal_water, shape * mask * params.w * color.a);
        } else {
            // Humedad: moja sin encharcar.
            decal_wet = max(decal_wet, texel.a * mask * params.w * color.a);
        }
    }

    float flood = max(floodLevel(world_position.xz, weather.flood), decal_water);
    if (weather.params.x > 0.0 || weather.params.y > 0.0 || flood > 0.0 || decal_wet > 0.0) {
        float exposed = rainExposure(world_position);

        // El agua se queda en lo horizontal; las paredes escurren.
        float facing_up = smoothstep(0.3, 0.85, n.y);
        float flat_ground = smoothstep(0.92, 0.98, n.y);

        // Nivel del agua: charcos de la lluvia (solo a la intemperie) y la
        // zona inundada (esta aunque haya techo).
        float level = max(puddleLevel(world_position.xz, weather.params.y) * exposed, flood) *
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
            vec2 slope = rainRipples(world_position.xz, weather.params.z,
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
