#version 450

// Oclusion ambiental por horizontes (GTAO, "Ground Truth Ambient Occlusion",
// Jimenez et al. 2016), siguiendo XeGTAO de Intel (MIT; THIRD_PARTY_NOTICES.md):
// para cada pixel se buscan, en unos pocos cortes de la pantalla que pasan
// por el, los horizontes a cada lado (lo mas alto que tapa el cielo) y se
// integra la luz que llega por encima de ellos con el coseno de la normal.
// Mas fiel que ssao.frag (que cuenta muestras de una semiesfera): no se
// oscurece en superficies planas ni de refilon y da ademas la normal doblada
// (la direccion media por la que llega la luz), con la que lighting.frag
// mira la irradiancia del cielo y calcula la oclusion especular.
//
// Es la opcion de calidad (CVar render.ao.Gtao, desde Medio): en gama baja y
// en el modo compatible sigue ssao.frag. Mismo set que ssao.frag (camara,
// profundidad y normales) y mismo patron de 4x4 pixeles (rotacion de los
// cortes y desplazamiento de los pasos), asi que el desenfoque de 4x4 de
// lighting.frag quita el ruido sin emborronar bordes. Con TAA o escalado
// temporal el patron cambia cada frame (push.frame) y el TAA acumula: con
// eso, 2 cortes x 4 pasos a cada lado valen lo que muchos mas.
//
// Salida (RGBA16F): r = visibilidad (1 = nada ocluido), g = profundidad lineal
// de vista (como ssao.frag: la usa el desenfoque), ba = normal doblada en el
// espacio del mundo, en octaedro (la misma codificacion que el G-buffer).

layout(set = 0, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
} camera;

layout(set = 0, binding = 1) uniform sampler2D g_depth;
layout(set = 0, binding = 2) uniform sampler2D g_normal;

// GpuBloomPush: texel = 1 / tamano, frame = semilla del patron temporal
// (0 = fijo), radius = radio del efecto (m).
layout(push_constant) uniform Push {
    vec2 texel;
    float frame;
    float radius;
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_ao;

const float kPi = 3.14159265;
const float kHalfPi = 1.57079633;
const int kSliceCount = 2;
const int kStepsPerSide = 4;
// XeGTAO: falloff del ultimo 61,5 % del radio (lo lejano pesa menos) y la
// potencia final (mas contraste, como su "FinalValuePower").
const float kFalloffRange = 0.615;
const float kFinalPower = 2.2;
// Pixeles minimos del primer paso: el propio pixel no se tapa a si mismo.
const float kMinPixels = 1.3;
// De lejos el radio ocupa pocos pixeles y ya no aporta (como ssao.frag).
const float kFadeStart = 60.0;
const float kFadeEnd = 120.0;

vec3 decodeNormal(vec2 e) {
    vec3 n = vec3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y);
    float t = max(-n.y, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.z += n.z >= 0.0 ? -t : t;
    return normalize(n);
}

// Inversa de decodeNormal (gbuffer_surface.glsl): octaedro con el eje Y.
vec2 encodeNormal(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 wrapped = (1.0 - abs(n.zx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);
    return n.y >= 0.0 ? n.xz : wrapped;
}

// Igual que ssao.frag: profundidad lineal y posicion de vista sin la inversa
// completa (precision a distancia; perspectiva, ortografica y VR).
float linearDepth(float depth) {
    return (camera.projection[3][2] - depth * camera.projection[3][3]) /
           (camera.projection[2][2] - depth * camera.projection[2][3]);
}

vec3 viewFromDepth(vec2 uv, float depth) {
    float z = linearDepth(depth);
    vec2 ndc = uv * 2.0 - 1.0;
    float w = camera.projection[3][3] - camera.projection[2][3] * z;
    return vec3((ndc.x * w + camera.projection[2][0] * z - camera.projection[3][0]) / camera.projection[0][0],
                (ndc.y * w + camera.projection[2][1] * z - camera.projection[3][1]) / camera.projection[1][1], -z);
}

vec3 viewAt(vec2 uv) {
    return viewFromDepth(uv, textureLod(g_depth, uv, 0.0).r);
}

float bayer4(ivec2 p) {
    const float kBayer[16] = float[](0.0, 8.0, 2.0, 10.0,
                                     12.0, 4.0, 14.0, 6.0,
                                     3.0, 11.0, 1.0, 9.0,
                                     15.0, 7.0, 13.0, 5.0);
    return (kBayer[(p.y & 3) * 4 + (p.x & 3)] + 0.5) / 16.0;
}

// acos rapido (XeGTAO_FastACos), error < 0,0007.
float fastAcos(float x) {
    float a = abs(x);
    float r = (-0.156583 * a + kHalfPi) * sqrt(max(1.0 - a, 0.0));
    return x >= 0.0 ? r : kPi - r;
}

void main() {
    float depth = textureLod(g_depth, v_uv, 0.0).r;
    // Normal doblada por defecto: (0, 1, 0) codificada.
    if (depth >= 1.0) {
        out_ao = vec4(1.0, 1.0e6, 0.0, 0.0);  // cielo
        return;
    }
    vec3 position = viewFromDepth(v_uv, depth);
    float view_depth = -position.z;
    vec3 world_normal = decodeNormal(textureLod(g_normal, v_uv, 0.0).rg);
    if (view_depth > kFadeEnd) {
        out_ao = vec4(1.0, view_depth, encodeNormal(world_normal));
        return;
    }
    vec3 normal = normalize(mat3(camera.view) * world_normal);
    // Hacia la camara (en ortografica, el eje de la vista).
    bool ortho = camera.projection[2][3] == 0.0;
    vec3 view_vec = ortho ? vec3(0.0, 0.0, 1.0) : normalize(-position);

    float radius = push.radius > 0.0 ? push.radius : 0.6;
    float falloff_range = kFalloffRange * radius;
    float falloff_from = radius * (1.0 - kFalloffRange);
    float falloff_mul = -1.0 / falloff_range;
    float falloff_add = falloff_from / falloff_range + 1.0;

    // El radio en la pantalla (uv): x_ndc = x * P00 / w. Si cabe en menos de
    // un pixel, no hay nada que buscar.
    float w = camera.projection[3][3] - camera.projection[2][3] * view_depth;
    vec2 uv_radius = abs(vec2(camera.projection[0][0], camera.projection[1][1])) * radius / (2.0 * max(w, 1e-4));
    float pixel_radius = uv_radius.x / max(push.texel.x, 1e-6);
    if (pixel_radius < kMinPixels) {
        out_ao = vec4(1.0, view_depth, encodeNormal(world_normal));
        return;
    }
    float min_s = kMinPixels / pixel_radius;

    ivec2 pixel = ivec2(gl_FragCoord.xy);
    // Patron de 4x4 (y temporal, si push.frame): rotacion de los cortes y
    // desplazamiento de los pasos, cada uno con su propio orden.
    float temporal = fract(push.frame * 0.618034);
    float slice_noise = fract(bayer4(pixel) + temporal);
    float step_noise = fract(bayer4(pixel.yx + ivec2(1, 2)) + temporal * 0.7548777);

    // Del uv a la vista: el signo de P00 y P11 (en Vulkan la Y del clip baja).
    vec2 view_sign = sign(vec2(camera.projection[0][0], camera.projection[1][1]));

    float visibility = 0.0;
    vec3 bent_normal = vec3(0.0);
    for (int slice = 0; slice < kSliceCount; ++slice) {
        float phi = (float(slice) + slice_noise) / float(kSliceCount) * kPi;
        vec2 omega = vec2(cos(phi), sin(phi));
        // La direccion del corte en la vista (a profundidad constante).
        vec3 direction = vec3(omega * view_sign, 0.0);
        vec3 ortho_direction = direction - dot(direction, view_vec) * view_vec;
        vec3 axis = normalize(cross(ortho_direction, view_vec));
        vec3 projected_normal = normal - axis * dot(normal, axis);
        float projected_length = length(projected_normal);
        float sign_n = sign(dot(ortho_direction, projected_normal));
        float cos_n = clamp(dot(projected_normal, view_vec) / max(projected_length, 1e-5), 0.0, 1.0);
        float n = sign_n * fastAcos(cos_n);

        // Horizontes de partida: los de la propia cara (no tapa nada).
        float low0 = cos(n + kHalfPi);
        float low1 = cos(n - kHalfPi);
        float horizon0 = low0;
        float horizon1 = low1;

        for (int s = 0; s < kStepsPerSide; ++s) {
            float t = (float(s) + step_noise) / float(kStepsPerSide);
            t = t * t;  // mas pasos cerca (XeGTAO: SampleDistributionPower 2)
            t += min_s;
            vec2 offset = omega * uv_radius * t;
            vec2 uv0 = v_uv + offset;
            vec2 uv1 = v_uv - offset;

            // +omega
            if (all(greaterThanEqual(uv0, vec2(0.0))) && all(lessThanEqual(uv0, vec2(1.0)))) {
                vec3 delta = viewAt(uv0) - position;
                float len = length(delta);
                float weight = clamp(len * falloff_mul + falloff_add, 0.0, 1.0);
                float shc = dot(delta / max(len, 1e-5), view_vec);
                horizon0 = max(horizon0, mix(low0, shc, weight));
            }
            // -omega
            if (all(greaterThanEqual(uv1, vec2(0.0))) && all(lessThanEqual(uv1, vec2(1.0)))) {
                vec3 delta = viewAt(uv1) - position;
                float len = length(delta);
                float weight = clamp(len * falloff_mul + falloff_add, 0.0, 1.0);
                float shc = dot(delta / max(len, 1e-5), view_vec);
                horizon1 = max(horizon1, mix(low1, shc, weight));
            }
        }

        // Angulos de los horizontes desde la vista (h1 hacia +omega), dentro
        // de la semiesfera de la normal, y la integral del coseno entre ellos.
        projected_length = mix(projected_length, 1.0, 0.05);
        float h0 = -fastAcos(horizon1);
        float h1 = fastAcos(horizon0);
        h0 = n + clamp(h0 - n, -kHalfPi, kHalfPi);
        h1 = n + clamp(h1 - n, -kHalfPi, kHalfPi);
        float sin_n = sin(n);
        float arc0 = (cos_n + 2.0 * h0 * sin_n - cos(2.0 * h0 - n)) * 0.25;
        float arc1 = (cos_n + 2.0 * h1 * sin_n - cos(2.0 * h1 - n)) * 0.25;
        visibility += projected_length * (arc0 + arc1);

        // Normal doblada del corte (XeGTAO, "Algorithm 2"): t0 a lo largo del
        // corte, t1 hacia la camara.
        float t0 = (6.0 * sin(h0 - n) - sin(3.0 * h0 - n) + 6.0 * sin(h1 - n) - sin(3.0 * h1 - n) +
                    16.0 * sin_n - 3.0 * (sin(h0 + n) + sin(h1 + n))) / 12.0;
        float t1 = (-cos(3.0 * h0 - n) - cos(3.0 * h1 - n) + 8.0 * cos(n) -
                    3.0 * (cos(h0 + n) + cos(h1 + n))) / 12.0;
        float ortho_length = length(ortho_direction);
        vec3 along = ortho_length > 1e-5 ? ortho_direction / ortho_length : direction;
        bent_normal += (along * t0 + view_vec * t1) * projected_length;
    }

    visibility = clamp(visibility / float(kSliceCount), 0.0, 1.0);
    visibility = max(pow(visibility, kFinalPower), 0.03);
    visibility = mix(visibility, 1.0, smoothstep(kFadeStart, kFadeEnd, view_depth));

    // Al mundo (la vista es una rotacion: su inversa es la traspuesta). Si
    // sale degenerada o de espaldas, la normal de la superficie.
    vec3 bent_world = transpose(mat3(camera.view)) * bent_normal;
    float bent_length = length(bent_world);
    bent_world = bent_length > 1e-4 ? bent_world / bent_length : world_normal;
    if (dot(bent_world, world_normal) < 0.0) bent_world = world_normal;
    out_ao = vec4(visibility, view_depth, encodeNormal(bent_world));
}
