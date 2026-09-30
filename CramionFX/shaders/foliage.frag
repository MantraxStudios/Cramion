#version 450
#extension GL_GOOGLE_include_directive : require

// Arboles en el G-buffer: corteza y hojas con sus texturas (arboles
// procedurales, TreeGenerator). Las hojas se recortan por el alfa y usan el
// modelo de Disney "subsurface" con translucidez: a contraluz la copa se
// ilumina. La corteza lleva su normal map (base tangente de las derivadas).

layout(set = 1, binding = 2) uniform sampler2DArray tree_albedo;
layout(set = 1, binding = 3) uniform sampler2DArray tree_normal;

layout(location = 0) in vec3 v_world_position;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec4 v_color;
layout(location = 3) in vec4 v_current_clip;
layout(location = 4) in vec4 v_previous_clip;
layout(location = 5) in vec3 v_uv_layer;
layout(location = 6) in float v_leaf;

#include "gbuffer_surface.glsl"

void main() {
    vec4 texel = texture(tree_albedo, v_uv_layer);
    bool leaf = v_leaf > 0.5;
    bool cut_out = leaf && texel.a < 0.5;

    // La corteza es un tubo cerrado: su normal de vertice ya mira hacia fuera
    // (no se usa gl_FrontFacing: el orden de los triangulos del tubo no es el
    // "frontal" de Vulkan y la normal quedaba hacia dentro = tronco negro).
    vec3 n = normalize(v_normal);
    vec3 normal = n;
    vec3 tangent_normal = vec3(0.0, 0.0, 1.0);
    if (!leaf) {
        // Normal map de la corteza con una base tangente de las derivadas
        // (Schuler 2006): sin tangentes en los vertices.
        vec3 dp1 = dFdx(v_world_position);
        vec3 dp2 = dFdy(v_world_position);
        vec2 duv1 = dFdx(v_uv_layer.xy);
        vec2 duv2 = dFdy(v_uv_layer.xy);
        vec3 dp2perp = cross(dp2, n);
        vec3 dp1perp = cross(n, dp1);
        vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
        vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;
        float invmax = inversesqrt(max(dot(t, t), dot(b, b)) + 1e-12);
        vec3 tn = texture(tree_normal, v_uv_layer).xyz * 2.0 - 1.0;
        tangent_normal = normalize(tn);
        normal = normalize(mat3(t * invmax, b * invmax, n) * tn);
    }
    // Color: textura (sRGB, ya lineal al leerla) por el color de la especie.
    vec3 albedo = pow(texel.rgb, vec3(1.0 / 2.2)) * v_color.rgb;
    float occlusion = v_color.a;
    if (leaf) {
        // Modelo de Disney 3: hoja fina, bastante translucida.
        // Translucidez moderada: con mucha, toda la copa brilla igual (plana).
        surface_shading = vec4(3.0 / 255.0, 0.25, 0.3, 0.0);
    }
    writeSurface(vec4(clamp(albedo, 0.0, 1.0), 1.0), n, normal, tangent_normal, n, 0.0, leaf ? 0.6 : 0.88,
                 occlusion, vec3(0.0), leaf ? 0.035 : 0.03, v_world_position);
    writeVelocity(v_current_clip, v_previous_clip);
    if (cut_out) discard;
}
