#version 450
#extension GL_GOOGLE_include_directive : require

// Hierba en el G-buffer. Para que el cesped se lea como pelo (una superficie
// de hebras) y no como tiras de papel:
//   - Normal: la cara de la hoja vuelta hacia la camara (sin suponer el
//     sentido de los triangulos) y redondeada por pixel (nervio central).
//     Lejos y cerca de la raiz se inclina hacia arriba: el campo entero se
//     ve como una sola superficie de pelo.
//   - Auto-sombra del sol y oclusion dentro del cesped, analiticas (la hierba
//     no esta en el mapa de sombras): cuanto cesped hay entre el punto y el
//     sol o el cielo (extincion de grass.sun.w).
//   - Modelo de sombreado 7 (hierba, disney_brdf.glsl): difuso envolvente,
//     brillo de hebra a lo largo de la hoja y translucidez de hoja fina (a
//     contraluz se ilumina, como la de verdad).

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

#define GRASS_SET 2
#include "grass_common.glsl"

layout(location = 0) in vec3 v_world_position;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in float v_height;
layout(location = 3) in vec3 v_color;
layout(location = 4) in vec4 v_current_clip;
layout(location = 5) in vec4 v_previous_clip;
layout(location = 6) in vec3 v_across;
layout(location = 7) in float v_side;
layout(location = 8) in vec3 v_strand;
layout(location = 9) in float v_above;
layout(location = 10) in float v_dist;

#include "gbuffer_surface.glsl"
const float kPi = 3.14159265;
#include "disney_brdf.glsl"  // solo shadingEncodeAngle

void main() {
    // --- Normal ---
    // Primero la cara que se ve (hacia la camara), luego el redondeo: asi el
    // "hacia arriba" y el nervio son los mismos por las dos caras.
    vec3 to_camera = normalize(camera.position.xyz - v_world_position);
    vec3 face = normalize(v_normal);
    if (dot(face, to_camera) < 0.0) face = -face;
    vec3 across = dot(v_across, v_across) > 1e-8 ? normalize(v_across) : vec3(1.0, 0.0, 0.0);
    float side = clamp(v_side, -1.0, 1.0);
    vec3 n = normalize(face + across * side * 0.55 + vec3(0.0, 0.35, 0.0));
    // El campo se lee como una sola superficie de pelo: hacia arriba con la
    // distancia y cerca de la raiz.
    float ground = mix(0.2, 0.7, smoothstep(4.0, max(grass.grid.z * 0.6, 4.5), v_dist)) * mix(1.0, 0.6, v_height);
    n = normalize(mix(n, vec3(0.0, 1.0, 0.0), ground));

    // --- Auto-sombra del sol y oclusion dentro del cesped ---
    // Cuanto cesped queda por encima de este punto (m) y lo que deja pasar:
    // hacia el sol, mas cuanto mas bajo esta el sol (atraviesa mas hierba).
    float depth_in = max(grass.shape.x * 1.1 - v_above, 0.0);
    float sigma = grass.sun.w;
    float occlusion = mix(0.25, 1.0, exp(-sigma * depth_in * 1.5));
    float sun_t = exp(-sigma * depth_in / max(grass.sun.y, 0.15));
    // Huecos de luz: en unos sitios el sol entra mas hondo que en otros.
    float gap = grassHash(floor(v_world_position.xz * 30.0));
    sun_t = pow(sun_t, mix(0.6, 1.5, gap));
    // Ruido de gradiente intercalado (con el jitter del TAA, cambia cada
    // frame) antes de los 8 niveles con los que se guarda: el TAA lo funde.
    vec2 noise_pixel = gl_FragCoord.xy + camera.jitter.xy * 4096.0;
    float ign = fract(52.9829189 * fract(dot(noise_pixel, vec2(0.06711056, 0.00583715))));
    surface_sun_shadow = clamp(sun_t + (ign - 0.5) / 7.0, 0.0, 1.0);

    // --- Color: el nervio mas claro, los bordes mas oscuros y vetas finas ---
    float s = abs(side);
    vec3 color = v_color * mix(1.06, 0.84, s * s) * (0.95 + 0.05 * sin(v_height * 40.0 + v_world_position.x * 13.0));

    // --- Modelo 7 (hierba): g = translucidez, b = direccion de la hebra,
    // a = brillo (0 ancho, 1 concentrado). La hebra es la subida de la hoja
    // sobre el plano de la normal; se guarda la tangente T = S x n para que
    // decodeShading devuelva bitangent = n x T = S.
    vec3 strand = v_strand - n * dot(n, v_strand);
    strand = dot(strand, strand) > 1e-8 ? normalize(strand) : normalize(cross(across, n) + vec3(0.0, 1e-4, 0.0));
    float translucency = mix(0.4, 0.7, v_height) * mix(0.35, 1.0, sun_t);
    surface_shading = vec4(7.0 / 255.0, translucency, shadingEncodeAngle(n, cross(strand, n)), mix(0.3, 0.7, v_height));
    // aa_normal = la cara sin redondear: el redondeo por pixel no debe subir
    // la rugosidad del antialiasing especular.
    writeSurface(vec4(clamp(color, 0.0, 1.0), 1.0), n, n, vec3(0.0, 0.0, 1.0), face, 0.0, 0.5, occlusion, vec3(0.0),
                 0.03, v_world_position);
    writeVelocity(v_current_clip, v_previous_clip);
}
