#version 450

// Efectos de la camara sobre la imagen HDR (antes del bloom y el tono):
//
//   Modo 0, profundidad de campo: el circulo de confusion de una lente real
//   (focal, numero f y distancia de enfoque; sensor de 24 mm de alto) y un
//   desenfoque de disco (bokeh) por recogida: cada muestra del disco cuenta si
//   SU circulo llega hasta este pixel. Lo de delante se esparce sobre lo
//   enfocado; lo de detras no mancha lo que esta delante y nitido.
//
//   Modo 1, motion blur: muestras a lo largo del vector de movimiento del
//   pixel (el del G-buffer; el cielo con la reproyeccion de la camara),
//   durante la fraccion del frame que el obturador esta abierto. Con prueba de
//   profundidad para que el fondo quieto no se arrastre sobre lo que se mueve
//   delante (McGuire 2012, simplificado).

layout(set = 0, binding = 0) uniform sampler2D source;        // copia de la imagen HDR
layout(set = 0, binding = 1) uniform sampler2D depth_map;
layout(set = 0, binding = 2) uniform sampler2D velocity_map;  // UV actual - UV anterior

layout(push_constant) uniform Push {
    mat4 reproject;  // clip actual -> clip anterior (cielo)
    vec4 params;     // x = modo, y = intensidad del motion blur, z = rastro maximo, w = frame
    vec4 dof;        // x = enfoque (m, < 0 = auto), y = numero f, z = focal (mm), w = desenfoque maximo
    vec4 camera;     // x, y = proyeccion [3][2] y [2][2], z = ancho / alto, w = 1 / alto
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

const float kSensorHeight = 0.024;  // metros (35 mm de fotografia)
const float kGoldenAngle = 2.39996323;

float linearDepth(float depth) {
    return push.camera.x / (depth + push.camera.y);
}

float sceneDepth(vec2 uv) {
    return linearDepth(textureLod(depth_map, uv, 0.0).r);
}

float interleavedGradientNoise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// --- Profundidad de campo ---

// Circulo de confusion en fraccion del alto de la imagen, con signo (< 0 =
// mas cerca que el enfoque).
float circleOfConfusion(float z, float focus) {
    float f = push.dof.z * 0.001;             // focal en metros
    float aperture = f / push.dof.y;          // diametro de la pupila
    float coc = aperture * f * (z - focus) / (z * max(focus - f, 1e-4));
    return clamp(coc / kSensorHeight, -push.dof.w, push.dof.w);
}

vec3 depthOfField() {
    float focus = push.dof.x;
    if (focus < 0.0) {
        // Autoenfoque: lo que hay en el centro (5 muestras, la mas cercana).
        focus = sceneDepth(vec2(0.5));
        for (int i = 0; i < 4; ++i) {
            vec2 o = vec2(i < 2 ? (i == 0 ? -0.02 : 0.02) : 0.0, i >= 2 ? (i == 2 ? -0.02 : 0.02) : 0.0);
            focus = min(focus, sceneDepth(vec2(0.5) + o));
        }
        focus = min(focus, 2000.0);
    }

    float center_z = sceneDepth(v_uv);
    float center_coc = circleOfConfusion(center_z, focus);
    vec3 center = textureLod(source, v_uv, 0.0).rgb;

    // Radio del disco: el mayor desenfoque posible (lo de delante puede
    // esparcirse sobre este pixel aunque este enfocado).
    float radius = push.dof.w;
    const int kSamples = 64;
    float rotation = interleavedGradientNoise(gl_FragCoord.xy) * 6.2831853;
    float pixel = push.camera.w;  // un pixel en fraccion del alto

    vec3 sum = center;
    float weight_sum = 1.0;
    // Cuanto del disco lo cubre algo desenfocado de DELANTE: eso tapa el
    // pixel aunque este enfocado (el borde de un objeto cercano se esparce).
    float front = 0.0;
    float counted = 0.0;
    for (int i = 0; i < kSamples; ++i) {
        float t = (float(i) + 0.5) / float(kSamples);
        float r = sqrt(t) * radius;
        float a = float(i) * kGoldenAngle + rotation;
        vec2 offset = vec2(cos(a) / push.camera.z, sin(a)) * r;
        vec2 uv = v_uv + offset;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
            continue;
        }
        counted += 1.0;
        float z = sceneDepth(uv);
        float coc = circleOfConfusion(z, focus);
        // Lo de detras no puede esparcirse sobre algo de delante mas nitido.
        float reach = z > center_z ? min(abs(coc), abs(center_coc)) : abs(coc);
        // Cuenta si su circulo llega hasta aqui (borde suave de un pixel).
        float w = clamp((reach - r) / pixel + 1.0, 0.0, 1.0);
        // Energia: un circulo grande reparte su luz en mas pixeles; los
        // brillos fuertes pesan algo mas (el bokeh se ve en las luces).
        vec3 c = textureLod(source, uv, 0.0).rgb;
        float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
        if (z < center_z && abs(coc) > abs(center_coc)) {
            front += w;
        }
        w *= 1.0 + min(luma, 8.0) * 0.1;
        sum += c * w;
        weight_sum += w;
    }
    vec3 blurred = sum / weight_sum;
    // Enfocado y sin nada delante que lo tape: la imagen tal cual.
    float blend = clamp(abs(center_coc) / (2.0 * pixel), 0.0, 1.0);
    float covered = clamp(2.0 * front / max(counted, 1.0), 0.0, 1.0);
    return mix(center, blurred, max(blend, covered));
}

// --- Motion blur ---

vec2 pixelVelocity(vec2 uv, float depth) {
    if (depth >= 1.0) {
        vec4 previous = push.reproject * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
        return uv - (previous.xy / max(previous.w, 1e-6) * 0.5 + 0.5);
    }
    return textureLod(velocity_map, uv, 0.0).xy;
}

vec3 motionBlur() {
    vec3 center = textureLod(source, v_uv, 0.0).rgb;
    float center_depth_raw = textureLod(depth_map, v_uv, 0.0).r;
    vec2 velocity = pixelVelocity(v_uv, center_depth_raw) * push.params.y;
    // Tope del rastro (fraccion del alto) medido en pantalla.
    vec2 in_height = vec2(velocity.x * push.camera.z, velocity.y);
    float length_height = length(in_height);
    if (length_height > push.params.z) {
        velocity *= push.params.z / length_height;
        length_height = push.params.z;
    }
    if (length_height < 0.5 * push.camera.w) {
        return center;  // menos de medio pixel
    }

    float center_z = linearDepth(center_depth_raw);
    const int kSamples = 24;
    float jitter = interleavedGradientNoise(gl_FragCoord.xy + push.params.w * 5.588238) - 0.5;
    vec3 sum = center;
    float weight_sum = 1.0;
    for (int i = 0; i < kSamples; ++i) {
        // De -0.5 a 0.5 del vector (el obturador centrado en el frame).
        float t = (float(i) + 0.5 + jitter) / float(kSamples) - 0.5;
        if (abs(t) < 1e-3) {
            continue;
        }
        vec2 uv = v_uv + velocity * t;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
            continue;
        }
        float raw = textureLod(depth_map, uv, 0.0).r;
        float z = linearDepth(raw);
        vec2 sample_velocity = pixelVelocity(uv, raw) * push.params.y;
        float distance_height = abs(t) * length_height;
        float sample_length = length(vec2(sample_velocity.x * push.camera.z, sample_velocity.y));
        // Delante (algo que se mueve y pasa por aqui): cuenta si su rastro
        // llega hasta este pixel. Detras: cuenta si el rastro de este pixel
        // lo cubre (el fondo se ve a traves del objeto en movimiento).
        bool in_front = z < center_z * 0.98;
        float reach = in_front ? sample_length * 0.5 : length_height * 0.5;
        float w = clamp((reach - distance_height) / push.camera.w + 1.0, 0.0, 1.0);
        sum += textureLod(source, uv, 0.0).rgb * w;
        weight_sum += w;
    }
    return sum / weight_sum;
}

void main() {
    vec3 color = push.params.x < 0.5 ? depthOfField() : motionBlur();
    out_color = vec4(color, 1.0);
}
