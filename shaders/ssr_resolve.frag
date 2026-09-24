#version 450

// Filtro temporal de los reflejos de pantalla (ssr.frag). Sin el, cada frame
// el rayo acierta o falla por su cuenta: al moverse la camara, o cuando lo
// reflejado sale de la pantalla, el reflejo aparece y desaparece de un frame
// a otro (parpadeo).
//
// Se mezcla el resultado de este frame con el acumulado de los anteriores,
// reproyectado a donde estaba este punto de la superficie. Para no dejar
// estelas, lo acumulado se recorta al rango de colores que hay ahora en la
// vecindad 3x3 del pixel (el "neighborhood clamp" del TAA de Unreal).
//
// Se trabaja con el color premultiplicado por la confianza: donde un frame no
// encontro nada (confianza 0) su color no significa nada y no debe mezclarse.

layout(set = 0, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
} camera;

layout(set = 0, binding = 1) uniform sampler2D g_depth;
layout(set = 0, binding = 2) uniform sampler2D current_reflection;  // ssr.frag de este frame
layout(set = 0, binding = 3) uniform sampler2D history;             // resultado del frame anterior

layout(push_constant) uniform PushConstants {
    mat4 previous_view_projection;
    vec4 params;  // x = hay historia valida, y = peso de la historia
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_reflection;

vec4 premultiply(vec4 reflection) {
    return vec4(reflection.rgb * reflection.a, reflection.a);
}

void main() {
    ivec2 size = textureSize(current_reflection, 0);
    ivec2 pixel = ivec2(gl_FragCoord.xy);

    float depth = texelFetch(g_depth, pixel, 0).r;
    vec4 current = premultiply(texelFetch(current_reflection, pixel, 0));
    if (depth >= 1.0 || push.params.x < 0.5) {
        out_reflection = texelFetch(current_reflection, pixel, 0);
        return;
    }

    // --- Vecindad 3x3 de este frame ---
    vec4 low = current;
    vec4 high = current;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            ivec2 p = clamp(pixel + ivec2(x, y), ivec2(0), size - 1);
            vec4 neighbor = premultiply(texelFetch(current_reflection, p, 0));
            low = min(low, neighbor);
            high = max(high, neighbor);
        }
    }

    // --- Donde estaba este punto de la superficie el frame anterior ---
    vec4 world = camera.inverse_view_projection * vec4(v_uv * 2.0 - 1.0, depth, 1.0);
    world /= world.w;
    vec4 previous_clip = push.previous_view_projection * world;
    vec2 previous_uv = previous_clip.xy / previous_clip.w * 0.5 + 0.5;

    vec4 result = current;
    if (previous_clip.w > 0.0 && all(greaterThanEqual(previous_uv, vec2(0.0))) &&
        all(lessThanEqual(previous_uv, vec2(1.0)))) {
        vec4 previous = premultiply(textureLod(history, previous_uv, 0.0));
        // Un poco de margen sobre el rango: el recorte estricto devuelve el
        // parpadeo que se quiere quitar.
        vec4 margin = (high - low) * 0.25 + vec4(0.002);
        previous = clamp(previous, low - margin, high + margin);
        result = mix(current, previous, push.params.y);
    }

    out_reflection = result.a > 0.0001 ? vec4(result.rgb / result.a, result.a) : vec4(0.0);
}
