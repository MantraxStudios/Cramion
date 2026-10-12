#version 450
#extension GL_GOOGLE_include_directive : require

// Arboles en el G-buffer: corteza y hojas con sus texturas (arboles
// procedurales, TreeGenerator). Texturas:
//   tree_albedo: rgb color (sRGB), a recorte de las hojas
//   tree_normal: rg normal (xy, la z se reconstruye), b oclusion,
//                a rugosidad (corteza) / translucidez (hojas)
// Las hojas se recortan por el alfa, son de dos caras (el enves, mas palido
// y mate) y usan el modelo de Disney "subsurface" con su mapa de
// translucidez: a contraluz la copa se ilumina, menos en el interior (la
// oclusion de la copa, calculada en TreeGenerator, va en el alfa del color).
// La corteza lleva su normal map (base tangente de las derivadas), su
// rugosidad, musgo arriba y en el pie, o el pie oscuro y agrietado del abedul.
//
// Para que la copa se lea como un volumen y no como tarjetas de papel:
//   - La normal de la hoja es la de la copa mezclada con la de la tarjeta
//     (TreeGenerator); la cara que se ve sale del sentido del triangulo
//     (gl_FrontFacing), no de esa normal doblada, y en el enves solo se da la
//     vuelta la parte de la tarjeta (la de la copa se conserva).
//   - Cada hoja de la textura lleva su propia inclinacion en el normal map.
//   - Auto-sombra del sol dentro de la copa (surface_sun_shadow), mas fuerte
//     lejos, donde ya no llega el mapa de sombras.
//   - Variacion de color por zonas de la copa y borde de la hoja con un alfa
//     con ruido (el TAA lo funde: sin escalones en el recorte).

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

layout(set = 1, binding = 2) uniform sampler2DArray tree_albedo;
layout(set = 1, binding = 3) uniform sampler2DArray tree_normal;

// Igual que en foliage.vert (DrawPush de FoliagePass, 112 bytes).
layout(push_constant) uniform FoliagePush {
    mat4 light_view_projection;
    vec4 params;  // x segundos, y sombra (0/1), z viento, w segundos del frame anterior
    vec4 offset;  // xyz origen flotante, w alfa con ruido en las hojas (0/1)
    vec4 sun;     // xyz hacia el sol (mundo), w distancia de las sombras (0 = sin sombras)
} push;

layout(location = 0) in vec3 v_world_position;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec4 v_color;  // rgb tinte (1 = la textura), a oclusion
layout(location = 3) in vec4 v_current_clip;
layout(location = 4) in vec4 v_previous_clip;
layout(location = 5) in vec3 v_uv_layer;
layout(location = 6) in float v_leaf;
layout(location = 7) in float v_extra;  // corteza: musgo / pie oscuro del abedul; hoja: translucidez
layout(location = 8) in vec3 v_to_camera;
layout(location = 9) in float v_young;  // corteza joven (TreeGenerator): lisa; en el pino, anaranjada
layout(location = 10) in float v_crown_sun;  // hoja: luz del sol que entra en la copa (1 = toda)

#include "gbuffer_surface.glsl"

// Capas (TreeGenerator.h, kTreeLayer*).
const int kLayerBark = 0;
const int kLayerLeaves = 1;
const int kLayerNeedles = 2;
const int kLayerFrond = 3;
const int kLayerBirchBark = 4;
const int kLayerPineBark = 5;
const int kLayerBirchLeaves = 6;
const int kLayerWillowLeaves = 7;
const int kLayerFirNeedles = 8;
const int kLayerPineShoot = 10;

// Que cara de la hoja es la "frontal" de Vulkan (gl_FrontFacing). Las
// tarjetas (TreeGenerator: emitCard y las frondas de la palmera) van en el
// orden {p0, p0 + 1, p1} = (+lado, +direccion): la normal geometrica del
// triangulo es -face (el enves). La proyeccion lleva la Y invertida
// (core::perspective, m[1][1] = -f) y el viewport es positivo, asi que la
// imagen no sale espejada: con frontFace CCW (el de por defecto) es frontal
// la cara hacia la que apunta la normal geometrica, el enves. Por eso la haz
// (face) NO es la frontal. Si alguna vez la copa se ve con el enves por
// fuera, se cambia esta constante.
const bool kLeafFrontIsFace = false;

// Mate: una hoja de verdad tiene poco brillo (cuticula fina) salvo con el
// sol rasante; con 0.4 la copa entera brillaba como plastico.
float leafRoughness(int layer) {
    if (layer == kLayerLeaves) return 0.55;        // roble
    if (layer == kLayerFrond) return 0.5;          // palmera: la mas brillante
    if (layer == kLayerBirchLeaves) return 0.58;
    if (layer == kLayerWillowLeaves) return 0.6;
    if (layer == kLayerNeedles || layer == kLayerFirNeedles || layer == kLayerPineShoot) return 0.6;
    return 0.6;
}

// Ruido de valor 2D (manchas de color de la copa).
float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float valueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Ruido de gradiente entrelazado (Jimenez 2014): uniforme en pantalla y el
// TAA lo funde (con el jitter cambia cada frame).
float interleavedNoise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

void main() {
    vec4 texel = texture(tree_albedo, v_uv_layer);
    vec4 maps = texture(tree_normal, v_uv_layer);
    bool leaf = v_leaf > 0.5;
    int layer = int(v_uv_layer.z + 0.5);
    // Recorte con un umbral con ruido (alfa "hashed"): el borde de la hoja
    // sale suave tras el TAA. En el nivel Bajo (offset.w = 0), 0.5 fijo. Las
    // sombras (foliage_shadow.frag) recortan siempre en 0.5.
    vec2 noise_pixel = gl_FragCoord.xy + camera.jitter.xy * 4096.0;
    float alpha_noise = interleavedNoise(noise_pixel);
    float alpha_cut = push.offset.w > 0.5 ? mix(0.35, 0.65, alpha_noise) : 0.5;
    bool cut_out = leaf && texel.a < alpha_cut;

    // Derivadas fuera de los if (flujo uniforme): la base tangente del normal
    // map (Schuler 2006, sin tangentes en los vertices) y la cara real.
    vec3 dp1 = dFdx(v_world_position);
    vec3 dp2 = dFdy(v_world_position);
    vec2 duv1 = dFdx(v_uv_layer.xy);
    vec2 duv2 = dFdy(v_uv_layer.xy);

    // La corteza es un tubo cerrado: su normal de vertice ya mira hacia fuera
    // (no se usa gl_FrontFacing: el orden de los triangulos del tubo no es el
    // "frontal" de Vulkan y la normal quedaba hacia dentro = tronco negro).
    // Las hojas son de dos caras. Su normal de vertice es 0.4 tarjeta + 0.6
    // copa: mirar si apunta a la camara decia "enves" en media copa aunque se
    // viera la haz (y al reves). La cara vista sale del orden de los
    // triangulos (kLeafFrontIsFace); en el enves solo se refleja la parte de
    // la tarjeta (2 x 0.4 de la cara, que alli es -g) y la de la copa sigue.
    vec3 n = normalize(v_normal);
    vec3 view = normalize(v_to_camera);
    vec3 g = cross(dp1, dp2);
    g = dot(g, g) > 1e-20 ? normalize(g) : n;
    if (dot(g, view) < 0.0) g = -g;  // la cara que se ve
    bool back_side = leaf && (gl_FrontFacing != kLeafFrontIsFace);
    if (back_side) n = normalize(n + 0.8 * g);
    // En la hoja la base tangente se hace sobre la cara vista (g): la normal
    // doblada hacia la copa puede quedar casi en el plano de la tarjeta y la
    // base salia degenerada. El relieve (lo que el mapa aparta de la cara) se
    // suma despues a la normal de la copa.
    vec3 frame_n = leaf ? g : n;
    vec3 dp2perp = cross(dp2, frame_n);
    vec3 dp1perp = cross(frame_n, dp1);
    vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(max(dot(t, t), dot(b, b)), 1e-20));
    // Hoja: nervios e inclinacion propia de cada hoja (TreeGenerator), enteros.
    vec2 txy = maps.rg * 2.0 - 1.0;
    vec3 tn = vec3(txy, sqrt(max(1.0 - dot(txy, txy), 0.0)));
    vec3 mapped = normalize(mat3(t * invmax, b * invmax, frame_n) * tn);
    vec3 normal = leaf ? normalize(n + (mapped - frame_n)) : mapped;
    vec3 tangent_normal = normalize(tn);

    // Color: textura (sRGB, ya lineal al leerla; writeSurface la quiere en
    // sRGB) por el tinte de la especie y del arbol.
    vec3 albedo = pow(max(texel.rgb, vec3(0.0)), vec3(1.0 / 2.2)) * v_color.rgb;
    float occlusion = v_color.a * mix(1.0, maps.b, leaf ? 0.8 : 1.0);
    float roughness;

    if (leaf) {
        roughness = leafRoughness(layer);
        // Variacion por zonas de la copa (mas amarilla o mas azulada) y el
        // interior (oclusion de la copa) mas apagado y frio que el borde al sol.
        float patch_noise = valueNoise(v_world_position.xz * 0.6 + v_world_position.y * 0.4);
        albedo *= mix(vec3(0.9, 0.95, 1.02), vec3(1.1, 1.06, 0.9), patch_noise);
        albedo *= mix(vec3(0.85, 0.92, 0.88), vec3(1.06, 1.06, 0.92), smoothstep(0.3, 0.9, v_color.a));
        if (back_side) {
            // Enves: mas palido, algo mas amarillo y mate.
            float luma = dot(albedo, vec3(0.2126, 0.7152, 0.0722));
            albedo = mix(albedo, vec3(luma) * vec3(1.08, 1.1, 0.78), 0.3) * 1.12;
            roughness = min(roughness + 0.12, 1.0);
        }
        // Auto-sombra del sol en la copa: las hojas del lado de la sombra y
        // las de dentro. Cerca la da sobre todo el mapa de sombras (los
        // arboles lo proyectan); lejos, o sin sombras, esto es lo unico.
        // Con ruido antes de los 8 niveles del G-buffer (el TAA lo funde).
        float sun_fade = push.sun.w > 0.0
                             ? 0.3 + 0.7 * smoothstep(0.5 * push.sun.w, push.sun.w, length(v_to_camera))
                             : 1.0;
        float crown_shadow = mix(1.0, clamp(v_crown_sun, 0.0, 1.0), 0.85 * sun_fade);
        float shadow_noise = interleavedNoise(noise_pixel + vec2(17.0, 59.0));
        surface_sun_shadow = clamp(crown_shadow + (shadow_noise - 0.5) / 7.0, 0.0, 1.0);
        // Translucidez: la del mapa (los nervios y lo seco, menos) por la de la
        // tarjeta, y menos dentro de la copa (alli llega menos luz; la sombra de
        // la cara de atras ya oscurece lo tapado). Con 0.6 y 0.25 dentro, las
        // copas a contraluz salian casi negras.
        float translucency = clamp(maps.a * v_extra * mix(0.45, 1.0, v_color.a) * 0.9, 0.0, 1.0);
        // Modelo de Disney 3: hoja fina (g subsurface, b translucidez, a grosor).
        surface_shading = vec4(3.0 / 255.0, 0.25, translucency, 0.1);
    } else {
        roughness = clamp(maps.a, 0.3, 1.0);
        if (layer == kLayerBirchBark) {
            // Pie del abedul: corteza oscura, gruesa y agrietada.
            float foot = clamp(v_extra, 0.0, 1.0);
            vec3 dark = vec3(0.16, 0.14, 0.12) * (0.55 + 0.7 * maps.b);
            albedo = mix(albedo, dark, foot * 0.85);
            roughness = mix(roughness, 0.95, foot);
            occlusion *= mix(1.0, 0.75 + 0.25 * maps.b, foot);
        } else {
            // Corteza joven: las ramas finas, lisas (sin las placas ni los
            // surcos del tronco viejo); en el pino, anaranjada y en laminas,
            // como la parte alta del tronco del pino silvestre.
            float young = clamp(v_young, 0.0, 1.0);
            if (young > 0.0) {
                if (layer == kLayerPineBark) {
                    float flake = maps.b * 0.6 + texel.r * 0.8;
                    vec3 orange = vec3(0.6, 0.37, 0.22) * (0.75 + 0.35 * flake) * v_color.rgb;
                    albedo = mix(albedo, orange, young * 0.85);
                    roughness = mix(roughness, 0.72, young);
                }
                normal = normalize(mix(normal, n, young * 0.65));
                occlusion = mix(occlusion, v_color.a, young * 0.5);
            }
            // Musgo: arriba de las ramas y en el pie, primero en los huecos
            // humedos (oclusion baja). Verde oliva apagado (lima no: el musgo
            // de un tronco a la sombra es oscuro y poco saturado).
            float moss = smoothstep(0.4, 0.8, v_extra + (0.55 - maps.b) * 0.45) * (1.0 - young * 0.7);
            vec3 moss_color = vec3(0.15, 0.19, 0.075) * (0.7 + 0.5 * maps.b);
            albedo = mix(albedo, moss_color, moss * 0.8);
            roughness = mix(roughness, 0.97, moss);
            // El musgo esponjoso borra el relieve fino.
            normal = normalize(mix(normal, n, moss * 0.5));
        }
    }

    // Reflectancia de la hoja: menos en los nervios y huecos (oclusion del
    // mapa) y dentro de la copa (alli no llega el cielo que se reflejaria).
    float reflectance = leaf ? 0.035 * mix(0.4, 1.0, maps.b) * mix(0.3, 1.0, v_color.a) : 0.03;
    writeSurface(vec4(clamp(albedo, 0.0, 1.0), 1.0), n, normal, tangent_normal, n, 0.0, roughness, occlusion, vec3(0.0),
                 reflectance, v_world_position);
    writeVelocity(v_current_clip, v_previous_clip);
    if (cut_out) discard;
}
