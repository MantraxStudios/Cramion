#version 450

// Sombras del relieve teselado: el mismo desplazamiento que skinned.tese,
// en el espacio del modelo, proyectado desde la luz.

// cw: en Vulkan el dominio de teselacion tiene el origen arriba a la
// izquierda, y con ccw los triangulos salian al reves que los de la malla
// (gl_FrontFacing falso: skinned.frag giraba la normal y todo se veia oscuro).
layout(triangles, fractional_odd_spacing, cw) in;

layout(set = 1, binding = 3) uniform sampler2D occlusion_map;

// Debe coincidir con GpuSkinnedShadowPush.
layout(push_constant) uniform PushConstants {
    mat4 light_model_view_projection;
    uint bone_offset;
    float height;
    float max_factor;
    uint flags;
    vec4 camera_model;
    vec4 model_scale;
} push;

layout(location = 0) in vec2 in_uv[];
layout(location = 1) in vec3 in_position[];
layout(location = 2) in vec3 in_normal[];

// Para el recorte por alfa de skinned_shadow.frag.
layout(location = 0) out vec2 v_uv;

void main() {
    vec3 w = gl_TessCoord;
    vec2 uv = w.x * in_uv[0] + w.y * in_uv[1] + w.z * in_uv[2];
    vec3 position = w.x * in_position[0] + w.y * in_position[1] + w.z * in_position[2];
    vec3 normal = w.x * in_normal[0] + w.y * in_normal[1] + w.z * in_normal[2];

    vec2 size = vec2(textureSize(occlusion_map, 0));
    float texels = max(max(length((in_uv[1] - in_uv[0]) * size), length((in_uv[2] - in_uv[1]) * size)),
                       length((in_uv[0] - in_uv[2]) * size));
    float lod = max(log2(max(texels / max(gl_TessLevelInner[0], 1.0), 1.0)), 0.0);
    float height = textureLod(occlusion_map, uv, lod).g;

    // Los mismos metros que skinned.tese, por la normal del mundo, llevados
    // al espacio del modelo (con escala no uniforme la normal del mundo es
    // n / s y un vector del mundo vuelve al modelo dividido por s).
    vec3 s = max(push.model_scale.xyz, vec3(1e-4));
    vec3 n_over_s = normal / s;
    position += (n_over_s / s) * (height * push.height / max(length(n_over_s), 1e-6));
    v_uv = uv;
    gl_Position = push.light_model_view_projection * vec4(position, 1.0);
}
