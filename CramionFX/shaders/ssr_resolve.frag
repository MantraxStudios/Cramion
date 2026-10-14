#version 450

// Filtro espacial de los reflejos de pantalla (ssr.frag).
//
// El trazado ya no tiene ruido (un rayo exacto por la piramide de
// profundidad, sin saltos al azar) y las superficies satinadas ya traen el
// reflejo borroso (el mip del cono de su rugosidad). Lo que quedaba duro era
// el borde entre "hay reflejo" y "no hay" (ahi se ve la sonda): una silueta
// de un pixel, recortada a tijera, sobre un reflejo borroso. Aqui se promedia
// una vecindad pequena, mas ancha cuanto mas rugosa es la superficie, solo
// entre pixeles de la misma superficie (profundidad y normal parecidas). Los
// espejos pasan tal cual.
//
// (Antes era un filtro temporal: mezclaba con lo de los frames anteriores
// reproyectado con el movimiento de la superficie, que no es el del reflejo,
// y al andar los reflejos se arrastraban y dejaban estelas.)
//
// Se mezcla el color premultiplicado por la confianza: donde un pixel no
// encontro nada (confianza 0) su color no significa nada.

layout(set = 0, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
} camera;

layout(set = 0, binding = 1) uniform sampler2D g_depth;
layout(set = 0, binding = 2) uniform sampler2D current_reflection;  // ssr.frag de este frame
layout(set = 0, binding = 3) uniform sampler2D g_normal;            // rg = normal, b = rugosidad

layout(push_constant) uniform PushConstants {
    mat4 unused;
    vec4 params;  // x = hay reflejos
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_reflection;

const float kMaxRadiance = 1000.0;  // igual que en ssr.frag
// Radio (en pixeles) para las superficies mas rugosas que traza el SSR.
const float kMaxRadius = 3.0;

float linearDepth(float depth) {
    return (camera.projection[3][2] - depth * camera.projection[3][3]) / (camera.projection[2][2] - depth * camera.projection[2][3]);
}

vec3 decodeNormal(vec2 e) {
    vec3 n = vec3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y);
    float t = max(-n.y, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.z += n.z >= 0.0 ? -t : t;
    return normalize(n);
}

void main() {
    ivec2 size = textureSize(current_reflection, 0);
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    if (push.params.x < 0.5) {
        out_reflection = vec4(0.0);
        return;
    }

    vec4 center = texelFetch(current_reflection, pixel, 0);
    float depth = texelFetch(g_depth, pixel, 0).r;
    vec4 normal_sample = texelFetch(g_normal, pixel, 0);
    float radius = kMaxRadius * smoothstep(0.12, 0.5, normal_sample.b);
    if (depth >= 1.0 || radius < 0.5) {
        out_reflection = vec4(min(center.rgb, vec3(kMaxRadiance)), center.a);
        return;
    }

    float center_distance = linearDepth(depth);
    vec3 normal = decodeNormal(normal_sample.rg);
    // Diferencia de profundidad que aun es la misma superficie (algo mas lejos
    // cuanto mas lejos esta: la pendiente en pantalla crece).
    float depth_tolerance = 0.02 + 0.02 * center_distance;
    int step = max(int(radius + 0.5), 1);

    vec4 sum = vec4(center.rgb * center.a, center.a);
    float weight_sum = 1.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            if (x == 0 && y == 0) {
                continue;
            }
            ivec2 p = clamp(pixel + ivec2(x, y) * step, ivec2(0), size - 1);
            float neighbor_depth = texelFetch(g_depth, p, 0).r;
            if (neighbor_depth >= 1.0) {
                continue;
            }
            vec4 neighbor_normal = texelFetch(g_normal, p, 0);
            float depth_weight =
                1.0 - smoothstep(0.0, depth_tolerance, abs(linearDepth(neighbor_depth) - center_distance));
            float normal_weight = pow(max(dot(decodeNormal(neighbor_normal.rg), normal), 0.0), 16.0);
            // Las diagonales algo menos (vecindad redondeada).
            float weight = depth_weight * normal_weight * ((x != 0 && y != 0) ? 0.5 : 0.75);
            if (weight <= 0.0) {
                continue;
            }
            vec4 neighbor = texelFetch(current_reflection, p, 0);
            sum += vec4(neighbor.rgb * neighbor.a, neighbor.a) * weight;
            weight_sum += weight;
        }
    }

    float confidence = sum.a / weight_sum;
    vec3 color = sum.a > 1e-4 ? sum.rgb / sum.a : vec3(0.0);
    if (any(isnan(color)) || any(isinf(color)) || isnan(confidence)) {
        out_reflection = vec4(0.0);
        return;
    }
    out_reflection = vec4(min(color, vec3(kMaxRadiance)), clamp(confidence, 0.0, 1.0));
}
