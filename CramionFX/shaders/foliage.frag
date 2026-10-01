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

layout(set = 1, binding = 2) uniform sampler2DArray tree_albedo;
layout(set = 1, binding = 3) uniform sampler2DArray tree_normal;

layout(location = 0) in vec3 v_world_position;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec4 v_color;  // rgb tinte (1 = la textura), a oclusion
layout(location = 3) in vec4 v_current_clip;
layout(location = 4) in vec4 v_previous_clip;
layout(location = 5) in vec3 v_uv_layer;
layout(location = 6) in float v_leaf;
layout(location = 7) in float v_extra;  // corteza: musgo / pie oscuro del abedul; hoja: translucidez
layout(location = 8) in vec3 v_to_camera;

#include "gbuffer_surface.glsl"

// Capas (TreeGenerator.h, kTreeLayer*).
const int kLayerBark = 0;
const int kLayerLeaves = 1;
const int kLayerNeedles = 2;
const int kLayerFrond = 3;
const int kLayerBirchBark = 4;
const int kLayerBirchLeaves = 6;
const int kLayerWillowLeaves = 7;
const int kLayerFirNeedles = 8;

float leafRoughness(int layer) {
    if (layer == kLayerLeaves) return 0.42;        // roble: haz cerosa
    if (layer == kLayerFrond) return 0.38;         // palmera: muy brillante
    if (layer == kLayerBirchLeaves) return 0.48;
    if (layer == kLayerWillowLeaves) return 0.5;
    if (layer == kLayerNeedles || layer == kLayerFirNeedles) return 0.46;
    return 0.5;
}

void main() {
    vec4 texel = texture(tree_albedo, v_uv_layer);
    vec4 maps = texture(tree_normal, v_uv_layer);
    bool leaf = v_leaf > 0.5;
    int layer = int(v_uv_layer.z + 0.5);
    bool cut_out = leaf && texel.a < 0.5;

    // La corteza es un tubo cerrado: su normal de vertice ya mira hacia fuera
    // (no se usa gl_FrontFacing: el orden de los triangulos del tubo no es el
    // "frontal" de Vulkan y la normal quedaba hacia dentro = tronco negro).
    // Las hojas son de dos caras: si se ve la de atras (la normal no mira a
    // la camara), se da la vuelta. Asi no depende del orden de los vertices.
    vec3 n = normalize(v_normal);
    vec3 view = normalize(v_to_camera);
    bool back_side = leaf && dot(n, view) < 0.0;
    if (back_side) n = -n;

    // Normal map con una base tangente de las derivadas (Schuler 2006): sin
    // tangentes en los vertices. Las derivadas fuera de los if (flujo uniforme).
    vec3 dp1 = dFdx(v_world_position);
    vec3 dp2 = dFdy(v_world_position);
    vec2 duv1 = dFdx(v_uv_layer.xy);
    vec2 duv2 = dFdy(v_uv_layer.xy);
    vec3 dp2perp = cross(dp2, n);
    vec3 dp1perp = cross(n, dp1);
    vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(max(dot(t, t), dot(b, b)), 1e-20));
    vec2 txy = maps.rg * 2.0 - 1.0;
    if (leaf) txy *= 0.75;  // hoja: el relieve de los nervios, suave
    vec3 tn = vec3(txy, sqrt(max(1.0 - dot(txy, txy), 0.0)));
    vec3 normal = normalize(mat3(t * invmax, b * invmax, n) * tn);
    vec3 tangent_normal = normalize(tn);

    // Color: textura (sRGB, ya lineal al leerla; writeSurface la quiere en
    // sRGB) por el tinte de la especie y del arbol.
    vec3 albedo = pow(max(texel.rgb, vec3(0.0)), vec3(1.0 / 2.2)) * v_color.rgb;
    float occlusion = v_color.a * mix(1.0, maps.b, leaf ? 0.8 : 1.0);
    float roughness;

    if (leaf) {
        roughness = leafRoughness(layer);
        if (back_side) {
            // Enves: mas palido, algo mas amarillo y mate.
            float luma = dot(albedo, vec3(0.2126, 0.7152, 0.0722));
            albedo = mix(albedo, vec3(luma) * vec3(1.08, 1.1, 0.78), 0.3) * 1.12;
            roughness = min(roughness + 0.18, 1.0);
        }
        // Translucidez: la del mapa (los nervios y lo seco, menos) por la de la
        // tarjeta, y mucho menos dentro de la copa (la luz no llega). Moderada:
        // con mucha, toda la copa brilla igual (plana).
        float translucency = clamp(maps.a * v_extra * mix(0.25, 1.0, v_color.a) * 0.6, 0.0, 1.0);
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
            // Musgo: arriba de las ramas y en el pie, primero en los huecos
            // humedos (oclusion baja) y donde la corteza es mas verdosa.
            float green = texel.g - max(texel.r, texel.b) * 0.8;
            float moss = smoothstep(0.35, 0.75, v_extra + (0.55 - maps.b) * 0.35 + green * 2.0);
            vec3 moss_color = vec3(0.2, 0.27, 0.07) * (0.75 + 0.45 * maps.b);
            albedo = mix(albedo, moss_color, moss * 0.85);
            roughness = mix(roughness, 0.97, moss);
            // El musgo esponjoso borra el relieve fino.
            normal = normalize(mix(normal, n, moss * 0.5));
        }
    }

    writeSurface(vec4(clamp(albedo, 0.0, 1.0), 1.0), n, normal, tangent_normal, n, 0.0, roughness, occlusion, vec3(0.0),
                 leaf ? 0.035 : 0.03, v_world_position);
    writeVelocity(v_current_clip, v_previous_clip);
    if (cut_out) discard;
}
