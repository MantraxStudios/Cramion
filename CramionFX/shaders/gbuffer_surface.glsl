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
const int kMaxDecals = 256;  // kMaxDecals (GpuTypes.h)
struct Decal {
    mat4 world_to_decal;
    vec4 color;     // rgb = tinte (sRGB), a = opacidad
    vec4 axis;      // xyz = eje de proyeccion (mundo), w = coseno minimo con la superficie
    vec4 params;    // x = tipo (0 estampa, 1 charco, 2 humedad), y = capa del color, z = borde, w = cantidad
    vec4 material;  // x = rugosidad, y = cuanto la sustituye, z = metalicidad, w = fuerza del normal map
    vec4 channels;  // capas del atlas (-1 = ninguna): x = normal, y = ORM, z = emision
    vec4 emission;  // rgb = emision (HDR lineal, color x intensidad)
};

// 9 = los decals (GpuDecalBuffer, std430): cabecera, hasta kMaxDecals y, por
// tesela de pantalla, un bit por decal (la CPU marca en cuales cae cada uno).
layout(std430, set = 0, binding = 9) readonly buffer DecalBuffer {
    uvec4 decal_header;  // x = numero, y = columnas, z = filas (0 = sin teselas), w = uint por tesela
    vec4 decal_tiles;    // xy = teselas por pixel
    Decal decals[kMaxDecals];
    uint decal_masks[];
};

layout(set = 0, binding = 3) uniform WeatherBuffer {
    mat4 rain_view_projection;
    vec4 params;  // x = humedad (0..1), y = charcos (0..1), z = segundos, w = mapa listo
    vec4 flood;   // zona inundada: xy = centro (x, z), zw = radios (0 = sin agua)
    vec4 decal_info;  // x = numero de decals (los datos, en DecalBuffer), yzw = hacia el sol
    vec4 snow;        // x = cobertura (0..1), y = espesor (m), z = humedad al derretirse, w = reservado
    vec4 fire_zones[4];  // xy = esquina minima (x, z), z = lado (m), w = parte del mapa usada (0 = apagada)
    // Lightmaps (GpuWeather::lightmap): x = 0 sin, 1 visibilidad del sol, 2 con
    // irradiancia; y = fuerza de la sombra horneada del sol; z = vista de
    // depuracion (1 irradiancia, 2 visibilidad del sol, 3 UV2/densidad).
    vec4 lightmap;
} weather;
// Atlas de las imagenes de los decals (DecalAtlas.h): capa = imagen (color,
// normal, ORM o emision). Una sola textura tambien en el modo compatible y la
// capa se puede indexar distinta en cada pixel.
layout(set = 0, binding = 4) uniform sampler2DArray decal_atlas;
// 6 = mapa de quemado de las zonas de fuego.
#include "fire_burn.glsl"

layout(location = 0) out vec4 out_albedo;    // rgb = albedo, a = oclusion ambiental
layout(location = 1) out vec4 out_normal;    // rg = normal (octaedrica), b = rugosidad,
                                             // a = reflectancia (F0 no metalico)
layout(location = 2) out vec4 out_material;  // rgb = emision (HDR lineal), a = metalicidad
layout(location = 3) out vec2 out_velocity;  // UV actual - UV anterior (sin jitter)

// Sombra del sol que el material se hace a si mismo (auto-sombra del
// parallax): 1 = nada. Va en el alfa de out_material junto a la
// metalicidad (ver decodeMetallic en lighting.frag).
float surface_sun_shadow = 1.0;

// Modelo de sombreado de Disney (disney_brdf.glsl): r = modelo / 255, gba =
// parametros. 0 = estandar (lo que escriben el terreno, los voxeles...). En
// el modo compatible el G-buffer tiene 4 destinos (el minimo de Vulkan) y todo
// es estandar.
#ifndef CRAMION_COMPAT
layout(location = 4) out vec4 out_shading;
#endif
vec4 surface_shading = vec4(0.0);

// false: el objeto no recibe decals (MeshRenderer.receive_decals: los
// personajes bajo el indicador de un area). Lo pone quien incluye antes de
// writeSurface (skinned.frag y surface.frag, de su registro por instancia).
bool surface_receive_decals = true;

// --- Lightmaps (PLAN-LIGHTMAPS 2.6) ---
// Donde cae el pixel en el atlas: xy = UV del atlas, z = capa (-1 = sin
// lightmap). Los rellena setupLightmap (lightmap_common.glsl) en skinned.frag
// y surface.frag; el terreno, el follaje y los voxeles no (z = -1).
vec3 surface_lightmap = vec3(0.0, 0.0, -1.0);
vec3 surface_view = vec3(0.0);             // hacia la camara (0 = sin Fresnel)
vec2 surface_lightmap_uv2 = vec2(-1.0);     // UV2 del modelo (vista de densidad sin hornear)
uint surface_lightmap_slot = 0xFFFFFFFFu;   // hueco de la instancia (color de la vista de densidad)
#ifdef CRAMION_LIGHTMAP
// Atlas de lightmaps (BakedGi.h, LightmapSet): capa = pagina; rgb = irradiancia
// (difuso = albedo * rgb), a = visibilidad del sol. Una sola textura: en el modo
// compatible el vidrio llega justo a 16 (SkinnedPass).
layout(set = 0, binding = 7) uniform sampler2DArray lightmap_atlas;
#endif
// Ruido de gradiente entrelazado (Jimenez): el dither de los 32 niveles de la
// sombra del sol.
float lightmapDither(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}
// Copia de multiBounceAo (lighting.frag): la oclusion del material sobre la
// luz horneada.
vec3 multiBounceAoSurface(float ao, vec3 albedo) {
    vec3 a = 2.0404 * albedo - 0.3324;
    vec3 b = -4.7951 * albedo + 0.6417;
    vec3 c = 2.7552 * albedo + 0.6903;
    return max(vec3(ao), ((ao * a + b) * ao + c) * ao);
}

// Movimiento en pantalla de este pixel, de las posiciones de recorte (sin
// jitter) de este frame y del anterior.
// Si el punto estaba detras de la camara el frame anterior (w <= 0) no hay
// movimiento valido: dividir por 1e-6 daba valores que se salian de RG16F
// (inf) y el motion blur/TAA/escaladores los convertian en NaN. Va a cero y,
// por si acaso, acotado (mas de 16 pantallas no significa nada).
void writeVelocity(vec4 current_clip, vec4 previous_clip) {
    if (current_clip.w <= 1e-6 || previous_clip.w <= 1e-6) {
        out_velocity = vec2(0.0);
        return;
    }
    vec2 current = current_clip.xy / current_clip.w;
    vec2 previous = previous_clip.xy / previous_clip.w;
    out_velocity = clamp((current - previous) * 0.5, vec2(-16.0), vec2(16.0));
}

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
            exposed += (1.0 - smoothstep(0.0015, 0.004, rain.z - above)) * wx[x] * wy[y];
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
    // al eje de proyeccion. Las imagenes estan en el atlas (una capa cada una):
    // la capa puede ser distinta en cada pixel, asi que se recorren solo los
    // bits marcados de la tesela (no hace falta que el indice sea uniforme).
    float decal_water = 0.0;
    float decal_wet = 0.0;
    uint decal_count = surface_receive_decals ? min(decal_header.x, uint(kMaxDecals)) : 0u;
    // Solo los de la tesela de este pixel (la CPU marca en cuales cae cada
    // uno); sin teselas (sondas, Render Textures, VR), todos.
    bool decal_tiled = decal_header.y > 0u && decal_header.z > 0u;
    uint decal_tile_first = 0u;
    if (decal_tiled && decal_count > 0u) {
        uvec2 tile = min(uvec2(gl_FragCoord.xy * decal_tiles.xy), decal_header.yz - 1u);
        decal_tile_first = (tile.y * decal_header.y + tile.x) * decal_header.w;
    }
    uint decal_words = (decal_count + 31u) >> 5u;
    for (uint word = 0u; word < decal_words; ++word) {
        uint bits = decal_tiled ? decal_masks[decal_tile_first + word] : 0xFFFFFFFFu;
        uint left = decal_count - word * 32u;
        if (left < 32u) bits &= (1u << left) - 1u;
        while (bits != 0u) {
        uint i = word * 32u + uint(findLSB(bits));
        bits &= bits - 1u;
        mat4 world_to_decal = decals[i].world_to_decal;
        vec3 p = (world_to_decal * vec4(world_position, 1.0)).xyz;
        if (any(greaterThan(abs(p), vec3(0.5)))) {
            continue;
        }
        vec4 axis = decals[i].axis;
        vec4 params = decals[i].params;
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

        // Derivadas explicitas: el bucle no es uniforme en el quad.
        vec2 uv = vec2(p.x + 0.5, 0.5 - p.z);
        mat3 to_decal = mat3(world_to_decal);
        vec2 duvdx = (to_decal * dpdx).xz * vec2(1.0, -1.0);
        vec2 duvdy = (to_decal * dpdy).xz * vec2(1.0, -1.0);
        bool has_color = params.y >= 0.0;
        vec4 texel = has_color ? textureGrad(decal_atlas, vec3(uv, params.y), duvdx, duvdy) : vec4(1.0);
        vec4 color = decals[i].color;
        int type = int(params.x + 0.5);
        if (type == 0) {
            // Estampa: color/textura encima del material, y sus canales.
            float a = clamp(texel.a * color.a * mask, 0.0, 1.0);
            vec4 material = decals[i].material;
            vec4 channels = decals[i].channels;
            // Un decal solo de relieve, ORM o emision no pinta el color (sin
            // ninguna imagen, si: la estampa de un color).
            if (has_color || (channels.x < 0.0 && channels.y < 0.0 && channels.z < 0.0)) {
                albedo.rgb = mix(albedo.rgb, texel.rgb * color.rgb, a);
            }
            if (channels.y >= 0.0) {
                // ORM (como los materiales): r = oclusion, g = rugosidad, b = metalicidad.
                vec3 orm = textureGrad(decal_atlas, vec3(uv, channels.y), duvdx, duvdy).rgb;
                occlusion = mix(occlusion, occlusion * orm.r, a);
                roughness = mix(roughness, clamp(orm.g, 0.04, 1.0), a);
                metallic = mix(metallic, orm.b, a);
            } else {
                roughness = mix(roughness, clamp(material.x, 0.04, 1.0), a * material.y);
                metallic = mix(metallic, material.z, a * material.y);
            }
            if (channels.x >= 0.0 && material.w != 0.0) {
                // Normal map en el espacio de la caja: X = derecha de la imagen,
                // +Z = arriba (v = 0.5 - z); verde hacia arriba (OpenGL/glTF),
                // fuerza negativa = verde invertido (DirectX). Se apoya en la
                // normal que ya tiene la superficie.
                vec2 xy = textureGrad(decal_atlas, vec3(uv, channels.x), duvdx, duvdy).xy * 2.0 - 1.0;
                vec3 tangent_decal = vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
                if (material.w < 0.0) tangent_decal.y = -tangent_decal.y;
                tangent_decal.xy *= abs(material.w);
                tangent_decal = normalize(tangent_decal);
                vec3 base = normal;
                vec3 right = vec3(world_to_decal[0][0], world_to_decal[1][0], world_to_decal[2][0]);
                vec3 image_up = vec3(world_to_decal[0][2], world_to_decal[1][2], world_to_decal[2][2]);
                right -= base * dot(base, right);
                if (dot(right, right) > 1e-12) {
                    right = normalize(right);
                    vec3 up_axis = cross(base, right);
                    if (dot(up_axis, image_up) < 0.0) up_axis = -up_axis;
                    vec3 decal_normal = normalize(right * tangent_decal.x + up_axis * tangent_decal.y + base * tangent_decal.z);
                    normal = normalize(mix(normal, decal_normal, a));
                    aa_normal = normalize(mix(aa_normal, decal_normal, a));
                }
            }
            vec3 glow = decals[i].emission.rgb;
            if (dot(glow, glow) > 0.0) {
                // Emision: su imagen o, sin ella, el color de la estampa.
                vec3 source = channels.z >= 0.0 ? toLinear(textureGrad(decal_atlas, vec3(uv, channels.z), duvdx, duvdy).rgb)
                                                : (has_color ? toLinear(texel.rgb) : vec3(1.0));
                emissive += source * glow * a;
            }
        } else if (type == 1) {
            // Charco: la textura (su alfa o su luminancia) da la forma; sin
            // textura, una mancha con la orilla irregular.
            float shape = has_color ? texel.a * dot(texel.rgb, vec3(0.333))
                                    : clamp((1.0 - length(p.xz) * 2.0 +
                                             (rainFbm(world_position.xz * 1.3 + float(i) * 7.1) - 0.5) * 0.6) /
                                                0.35, 0.0, 1.0);
            decal_water = max(decal_water, shape * mask * params.w * color.a);
        } else {
            // Humedad: moja sin encharcar.
            decal_wet = max(decal_wet, texel.a * mask * params.w * color.a);
        }
        }
    }

    // --- Estacion (sistema de ambiente): lo verde amarillea y enrojece en
    // otono (hierba, hojas, capas de hierba del terreno). snow.w = 0 verde ..
    // 1 otono. Se conserva la luminancia: solo cambia el tono.
    if (weather.snow.w > 0.0) {
        vec3 c = albedo.rgb;
        float greenness = clamp((c.g - max(c.r, c.b)) / max(c.g, 0.02) * 3.0, 0.0, 1.0);
        if (greenness > 0.0) {
            float leaf_noise = rainValueNoise(world_position.xz * 0.45 + world_position.y * 0.3);
            vec3 palette = mix(vec3(0.78, 0.5, 0.1), vec3(0.6, 0.17, 0.06), smoothstep(0.35, 0.85, leaf_noise));
            float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
            vec3 autumn = palette * (lum / max(dot(palette, vec3(0.2126, 0.7152, 0.0722)), 0.01)) * 1.1;
            albedo.rgb = mix(c, clamp(autumn, 0.0, 1.0), greenness * weather.snow.w * (0.65 + 0.35 * leaf_noise));
        }
    }

    // --- Fuego: suelo, hierba y arboles quemados (fire_burn.glsl) ---
    // Carbon negro con manchas de ceniza gris, mate; y brasas que brillan
    // donde aun hay calor.
    vec2 burn = fireBurnAt(world_position, weather.fire_zones);
    if (burn.x > 0.002 || burn.y > 0.002) {
        float ash = smoothstep(0.45, 0.8, rainValueNoise(world_position.xz * 0.9) * 0.7 +
                                              rainValueNoise(world_position.xz * 4.3) * 0.3);
        vec3 charred = mix(vec3(0.045, 0.04, 0.036), vec3(0.32, 0.31, 0.3), ash * 0.6);
        float c = smoothstep(0.0, 1.0, burn.x);
        albedo.rgb = mix(albedo.rgb, charred, c * 0.94);
        roughness = mix(roughness, 0.97, c);
        metallic *= 1.0 - c;
        float embers = smoothstep(0.55, 0.95, rainValueNoise(world_position.xz * 2.7 + weather.params.z * 0.15));
        emissive += vec3(1.0, 0.27, 0.04) * (burn.y * burn.y * (0.6 + 6.0 * embers)) * (1.0 - ash * 0.7);
    }

    float flood = max(floodLevel(world_position.xz, weather.flood), decal_water);
    if (weather.params.x > 0.0 || weather.params.y > 0.0 || flood > 0.0 || decal_wet > 0.0 || weather.snow.z > 0.0) {
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
                        max(decal_wet, weather.snow.z * exposed * mix(0.5, 1.0, facing_up)));  // + nieve que se derrite

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

    // --- Nieve acumulada (sistema de ambiente) ---
    // snow.x = cobertura, y = espesor (m), z = humedad al derretirse. Cubre lo
    // que mira hacia arriba y esta a la intemperie (mapa de lluvia): primero
    // lo plano y las zonas altas de un ruido del mundo; con mas cobertura
    // llega a las pendientes. Blanca, rugosa, tapa el relieve fino del
    // material (mas cuanto mas gruesa) y tiene destellos de los cristales.
    if (weather.snow.x > 0.0) {
        float exposed_snow = rainExposure(world_position);
        float up = smoothstep(0.15, 0.85, n.y);
        float detail_up = mix(up, smoothstep(0.1, 0.9, normal.y), 0.35);
        float drift_noise = rainFbm(world_position.xz * 0.35 + 11.0) * 0.6 +
                            rainValueNoise(world_position.xz * 3.7) * 0.4;
        float field = detail_up * mix(0.65, 1.0, drift_noise);
        float threshold = 1.0 - weather.snow.x * 1.15;
        float snow = smoothstep(threshold, threshold + 0.18, field) * smoothstep(0.2, 0.45, n.y) * exposed_snow;
        if (snow > 0.0) {
            // Relieve propio de la nieve (montones suaves).
            vec2 q = world_position.xz * 2.3;
            float h0 = rainValueNoise(q);
            vec3 snow_normal = normalize(n + vec3(h0 - rainValueNoise(q + vec2(0.07, 0.0)), 0.0,
                                                  h0 - rainValueNoise(q + vec2(0.0, 0.07))) * 1.4);
            float hide_detail = snow * clamp(0.55 + weather.snow.y * 6.0, 0.55, 1.0);
            // Derritiendose: mas gris, mas lisa (agua) y con menos espesor.
            float melting = weather.snow.z;
            vec3 snow_color = mix(vec3(0.9, 0.92, 0.96), vec3(0.8, 0.83, 0.88), drift_noise * 0.5);
            snow_color = mix(snow_color, vec3(0.62, 0.65, 0.68), melting * 0.6);
            float snow_roughness = mix(0.72, 0.35, melting);
            float snow_reflectance = 0.025;
            // Destellos: algun cristal de cada ~1.5 cm con la cara al azar
            // (brilla cuando refleja el sol). Se apagan si no caben en el pixel.
            vec2 cell = floor(world_position.xz / 0.015);
            float glint = step(0.982, rainHash12(cell)) * (1.0 - smoothstep(0.004, 0.02, footprint)) * (1.0 - melting);
            vec3 glint_normal = normalize(snow_normal + vec3(rainHash22(cell) - 0.5, 0.0).xzy * 1.3);
            snow_normal = normalize(mix(snow_normal, glint_normal, glint));
            snow_roughness = mix(snow_roughness, 0.1, glint);
            snow_reflectance = mix(snow_reflectance, 0.5, glint);

            albedo.rgb = mix(albedo.rgb, snow_color, snow);
            normal = normalize(mix(normal, snow_normal, hide_detail));
            aa_normal = normalize(mix(aa_normal, snow_normal, hide_detail));
            roughness = mix(roughness, snow_roughness, snow);
            reflectance = mix(reflectance, snow_reflectance, snow);
            metallic *= 1.0 - snow;
            emissive *= 1.0 - snow;
            occlusion = mix(occlusion, max(occlusion, 0.85), snow);
        }
    }

    // --- Lightmap (PLAN-LIGHTMAPS 2.6 y 3.6) ---
    // La visibilidad horneada del sol va con la sombra propia; la irradiancia,
    // con la emision (difuso = albedo * rgb), y el pixel se marca con el signo
    // del alfa de out_material (lighting.frag no le suma el cielo ni la GI).
    bool lightmapped = surface_lightmap.z >= 0.0 && weather.lightmap.x > 0.5;
    float sun_visible = clamp(surface_sun_shadow, 0.0, 1.0);
    int lightmap_debug = int(weather.lightmap.z + 0.5);
#ifdef CRAMION_LIGHTMAP
    if (lightmapped) {
        vec4 lm = textureLod(lightmap_atlas, surface_lightmap, 0.0);
        sun_visible *= mix(1.0, clamp(lm.a, 0.0, 1.0), weather.lightmap.y);
        if (weather.lightmap.x > 1.5) {
            vec3 lin = toLinear(albedo.rgb);
            vec3 f0 = mix(vec3(reflectance), lin, metallic);
            float nv = dot(surface_view, surface_view) > 0.0 ? clamp(dot(normal, surface_view), 1e-4, 1.0) : 1.0;
            vec3 fresnel = f0 + (max(vec3(1.0 - roughness), f0) - f0) * pow(1.0 - nv, 5.0);
            emissive += max(lm.rgb, vec3(0.0)) * lin * (1.0 - fresnel) * (1.0 - metallic) *
                        multiBounceAoSurface(occlusion, lin);
        }
        if (lightmap_debug == 1) emissive = max(lm.rgb, vec3(0.0));
        if (lightmap_debug == 2) emissive = vec3(clamp(lm.a, 0.0, 1.0));
        if (lightmap_debug == 3) {
            // Un cuadro por texel del atlas, con un tono por instancia.
            vec2 texel = floor(surface_lightmap.xy * vec2(textureSize(lightmap_atlas, 0).xy));
            float checker = mod(texel.x + texel.y, 2.0);
            float hue = fract(float(surface_lightmap_slot) * 0.61803399);
            vec3 tint = clamp(abs(fract(hue + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
            emissive = mix(tint * 0.35, tint, checker);
        }
    } else if (lightmap_debug == 3 && surface_lightmap_uv2.x >= 0.0) {
        // Sin hornear: las UV2 en un tablero de 32 x 32 (colores = fract(uv)).
        vec2 cell = floor(surface_lightmap_uv2 * 32.0);
        float checker = mod(cell.x + cell.y, 2.0);
        emissive = vec3(fract(surface_lightmap_uv2 * 4.0), 0.5) * mix(0.35, 1.0, checker);
        lightmapped = true;
    }
#endif
    if (lightmap_debug > 0 && !lightmapped) emissive = vec3(0.0);

    // --- Antialiasing especular (Tokuyoshi 2019) ---
    vec3 du = dFdx(aa_normal);
    vec3 dv = dFdy(aa_normal);
    float variance = kSpecularAaVariance * (dot(du, du) + dot(dv, dv));
    float alpha = roughness * roughness;
    float alpha2 = clamp(alpha * alpha + min(2.0 * variance, kSpecularAaThreshold), 0.0, 1.0);
    roughness = clamp(sqrt(sqrt(alpha2)), 0.0, 1.0);

    out_albedo = vec4(albedo.rgb, occlusion);
    out_normal = vec4(encodeNormal(normal), roughness, reflectance);
    // Metalicidad (0..1) + 2 x nivel de sombra del sol (32 niveles con dither:
    // la propia del relieve por la horneada); negativo = con lightmap.
    float level = clamp(floor((1.0 - sun_visible) * 31.0 + lightmapDither(gl_FragCoord.xy)), 0.0, 31.0);
    float packed_a = clamp(metallic, 0.0, 1.0) + 2.0 * level;
    out_material = vec4(emissive, lightmapped ? -(packed_a + 0.5) : packed_a);
#ifndef CRAMION_COMPAT
    out_shading = surface_shading;
#endif
}
