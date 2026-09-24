#version 450

// Filtro temporal de la luz rebotada (ssgi.frag), a media resolucion.
//
// El SSGI lanza solo 6 rayos por pixel: cada frame da un resultado distinto,
// y cuando un rayo acierta en algo pequeno y muy brillante (una bombilla de
// una guirnalda) ese pixel recibe mucha mas luz que sus vecinos. Al ampliarlo
// a pantalla completa se veia como cuadrados de colores que parpadeaban al
// moverse; solo se veia bien muy cerca y quieto.
//
// Como en ssr_resolve.frag: se mezcla con lo acumulado en los frames
// anteriores (reproyectado a donde estaba este punto) y lo acumulado se
// recorta al rango de la vecindad 3x3 de este frame. Todo en color comprimido
// (c / (1 + luminancia)), para que un pixel extremo no domine.
//
// rgb = luz rebotada, a = visibilidad del cielo (se filtra igual).

layout(set = 0, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
} camera;

layout(set = 0, binding = 1) uniform sampler2D g_depth;     // resolucion completa
layout(set = 0, binding = 2) uniform sampler2D current_gi;  // ssgi.frag de este frame
layout(set = 0, binding = 3) uniform sampler2D history;     // resultado del frame anterior

layout(push_constant) uniform PushConstants {
    mat4 previous_view_projection;
    vec4 params;  // x = hay historia valida, y = peso de la historia
} push;

layout(location = 0) out vec4 out_gi;

float luminance(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

vec4 compress(vec4 gi) {
    vec3 rgb = max(gi.rgb, vec3(0.0));
    return vec4(rgb / (1.0 + luminance(rgb)), gi.a);
}

vec4 decompress(vec4 gi) {
    vec3 rgb = clamp(gi.rgb, vec3(0.0), vec3(0.999));
    return vec4(rgb / max(1.0 - luminance(rgb), 0.001), gi.a);
}

void main() {
    ivec2 size = textureSize(current_gi, 0);
    ivec2 full_size = textureSize(g_depth, 0);
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    // Pixel de resolucion completa que representa a este (como en ssgi.frag).
    ivec2 full_pixel = min(pixel * 2, full_size - 1);

    vec4 raw = texelFetch(current_gi, pixel, 0);
    float depth = texelFetch(g_depth, full_pixel, 0).r;
    if (depth >= 1.0 || push.params.x < 0.5) {
        out_gi = raw;
        return;
    }

    vec4 current = compress(raw);
    vec4 low = current;
    vec4 high = current;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec4 neighbor = compress(texelFetch(current_gi, clamp(pixel + ivec2(x, y), ivec2(0),
                                                                  size - 1), 0));
            low = min(low, neighbor);
            high = max(high, neighbor);
        }
    }

    vec2 uv = (vec2(full_pixel) + 0.5) / vec2(full_size);
    vec4 world = camera.inverse_view_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
    world /= world.w;
    vec4 previous_clip = push.previous_view_projection * world;
    vec2 previous_uv = previous_clip.xy / previous_clip.w * 0.5 + 0.5;

    vec4 result = current;
    if (previous_clip.w > 0.0 && all(greaterThanEqual(previous_uv, vec2(0.0))) &&
        all(lessThanEqual(previous_uv, vec2(1.0)))) {
        vec4 previous = compress(textureLod(history, previous_uv, 0.0));
        vec4 margin = (high - low) * 0.25 + vec4(0.002);
        previous = clamp(previous, low - margin, high + margin);
        result = mix(current, previous, push.params.y);
    }

    if (any(isnan(result)) || any(isinf(result))) {
        out_gi = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    out_gi = decompress(result);
}
