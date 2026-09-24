#version 450

// Reflejos en espacio de pantalla (SSR), como los "Screen Space Reflections"
// de Unreal: el suelo pulido refleja las columnas, los muros y las ventanas
// que se ven en pantalla. El IBL solo sabe reflejar el cielo.
//
// Para cada pixel poco rugoso se sigue el rayo reflejado por el depth buffer
// (pasos que crecen con la distancia y refinamiento binario al cruzar una
// superficie). Donde choca se toma la imagen HDR iluminada del frame anterior,
// reproyectada: los reflejos incluyen luz, sombras, GI y otros reflejos.
//
// Salida: rgb = color reflejado, a = confianza (0 = sin reflejo: se usa el
// IBL). La confianza baja cerca de los bordes de la pantalla, con la
// rugosidad y en los rayos que vuelven hacia la camara, para que el paso al
// IBL no se vea.

layout(set = 0, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
} camera;

layout(set = 0, binding = 1) uniform sampler2D g_depth;
layout(set = 0, binding = 2) uniform sampler2D g_normal;  // rg = normal, b = rugosidad
layout(set = 0, binding = 3) uniform sampler2D previous_color;

layout(push_constant) uniform PushConstants {
    mat4 previous_view_projection;
    vec4 params;  // x = hay frame anterior valido
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_reflection;

const int kSteps = 48;
const int kRefineSteps = 6;
const float kMaxDistance = 40.0;      // metros
const float kFirstStep = 0.05;        // metros
const float kMaxRoughness = 0.5;      // mas rugoso: solo IBL
const float kMaxRadiance = 30.0;

float linearDepth(float depth) {
    return camera.projection[3][2] / (depth + camera.projection[2][2]);
}

vec3 viewFromDepth(vec2 uv, float depth) {
    float z = linearDepth(depth);
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc.x * z / camera.projection[0][0], ndc.y * z / camera.projection[1][1], -z);
}

vec3 decodeNormal(vec2 e) {
    vec3 n = vec3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y);
    float t = max(-n.y, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.z += n.z >= 0.0 ? -t : t;
    return normalize(n);
}

// Punto de vista -> uv de pantalla. false si queda detras de la camara.
bool project(vec3 view_position, out vec2 uv) {
    vec4 clip = camera.projection * vec4(view_position, 1.0);
    if (clip.w <= 0.0) {
        return false;
    }
    uv = clip.xy / clip.w * 0.5 + 0.5;
    return true;
}

bool insideScreen(vec2 uv) {
    return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
}

void main() {
    ivec2 size = textureSize(g_depth, 0);
    ivec2 pixel = ivec2(gl_FragCoord.xy);

    float depth = texelFetch(g_depth, pixel, 0).r;
    vec4 normal_sample = texelFetch(g_normal, pixel, 0);
    float roughness = normal_sample.b;
    if (depth >= 1.0 || roughness > kMaxRoughness || push.params.x < 0.5) {
        out_reflection = vec4(0.0);
        return;
    }

    vec3 position = viewFromDepth(v_uv, depth);
    vec3 normal = normalize(mat3(camera.view) * decodeNormal(normal_sample.rg));
    vec3 view_direction = normalize(position);
    vec3 ray = normalize(reflect(view_direction, normal));

    // Los rayos que vuelven hacia la camara salen enseguida del depth buffer
    // (lo que tienen delante no esta en pantalla): se desvanecen.
    float toward_camera = smoothstep(0.2, 0.6, ray.z);
    if (toward_camera >= 1.0) {
        out_reflection = vec4(0.0);
        return;
    }

    vec3 origin = position + normal * (0.01 + 0.002 * -position.z);

    // --- Marcha con pasos que crecen de forma geometrica ---
    float growth = pow(kMaxDistance / kFirstStep, 1.0 / float(kSteps));
    float previous_distance = 0.0;
    float distance_along = kFirstStep;
    bool hit = false;
    vec2 hit_uv = vec2(0.0);

    for (int i = 0; i < kSteps; ++i) {
        vec3 sample_position = origin + ray * distance_along;
        vec2 uv;
        if (!project(sample_position, uv) || !insideScreen(uv)) {
            break;
        }

        ivec2 texel = min(ivec2(uv * vec2(size)), size - 1);
        float scene_depth = texelFetch(g_depth, texel, 0).r;
        float difference = -sample_position.z - linearDepth(scene_depth);
        // Grosor supuesto de lo que se cruza: del orden del paso.
        float thickness = (distance_along - previous_distance) * 1.5 + 0.05;

        if (scene_depth < 1.0 && difference > 0.0 && difference < thickness) {
            // --- Refinamiento binario entre el paso anterior y este ---
            float low = previous_distance;
            float high = distance_along;
            for (int r = 0; r < kRefineSteps; ++r) {
                float middle = 0.5 * (low + high);
                vec3 p = origin + ray * middle;
                vec2 puv;
                project(p, puv);
                ivec2 ptexel = clamp(ivec2(puv * vec2(size)), ivec2(0), size - 1);
                float d = -p.z - linearDepth(texelFetch(g_depth, ptexel, 0).r);
                if (d > 0.0) {
                    high = middle;
                } else {
                    low = middle;
                }
            }
            project(origin + ray * high, hit_uv);
            hit = insideScreen(hit_uv);
            break;
        }

        previous_distance = distance_along;
        distance_along *= growth;
    }

    if (!hit) {
        out_reflection = vec4(0.0);
        return;
    }

    // La superficie golpeada tiene que mirar hacia el rayo: si no, es la cara
    // trasera de algo (el reflejo seria el de su interior).
    ivec2 hit_texel = clamp(ivec2(hit_uv * vec2(size)), ivec2(0), size - 1);
    vec3 hit_normal = normalize(mat3(camera.view) *
                                decodeNormal(texelFetch(g_normal, hit_texel, 0).rg));
    float facing = smoothstep(-0.05, 0.2, dot(hit_normal, -ray));

    // --- Reproyeccion al frame anterior ---
    float hit_depth = texelFetch(g_depth, hit_texel, 0).r;
    vec3 hit_view = viewFromDepth((vec2(hit_texel) + 0.5) / vec2(size), hit_depth);
    vec3 hit_world = camera.position.xyz + transpose(mat3(camera.view)) * hit_view;
    vec4 previous_clip = push.previous_view_projection * vec4(hit_world, 1.0);
    vec2 previous_uv = previous_clip.xy / previous_clip.w * 0.5 + 0.5;
    if (previous_clip.w <= 0.0 || !insideScreen(previous_uv)) {
        out_reflection = vec4(0.0);
        return;
    }

    // Reflejo algo borroso segun la rugosidad y la distancia recorrida: cinco
    // muestras en cruz (sin mips en la imagen HDR).
    vec2 texel_size = 1.0 / vec2(textureSize(previous_color, 0));
    float blur = roughness * roughness * 40.0 * clamp(distance_along * 0.1, 0.2, 1.0);
    vec3 color = textureLod(previous_color, previous_uv, 0.0).rgb * 2.0;
    color += textureLod(previous_color, previous_uv + vec2(blur, 0.0) * texel_size, 0.0).rgb;
    color += textureLod(previous_color, previous_uv - vec2(blur, 0.0) * texel_size, 0.0).rgb;
    color += textureLod(previous_color, previous_uv + vec2(0.0, blur) * texel_size, 0.0).rgb;
    color += textureLod(previous_color, previous_uv - vec2(0.0, blur) * texel_size, 0.0).rgb;
    color = min(color / 6.0, vec3(kMaxRadiance));

    // --- Confianza ---
    vec2 edge = min(hit_uv, 1.0 - hit_uv);
    float edge_fade = smoothstep(0.0, 0.08, min(edge.x, edge.y));
    float distance_fade = 1.0 - smoothstep(0.7, 1.0, distance_along / kMaxDistance);
    float roughness_fade = 1.0 - smoothstep(kMaxRoughness * 0.6, kMaxRoughness, roughness);
    float confidence = edge_fade * distance_fade * roughness_fade * facing * (1.0 - toward_camera);

    out_reflection = vec4(color, confidence);
}
