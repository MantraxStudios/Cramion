#version 450

// Teselacion de las sombras del relieve teselado: los mismos factores que
// skinned.tesc (pixeles en la camara), calculados en el espacio del modelo
// con la camara llevada a el. Asi la sombra tiene la misma forma que lo que
// se ve.

layout(vertices = 3) out;

layout(set = 0, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
    mat4 unjittered_view_projection;
    mat4 previous_view_projection;
    vec4 jitter;  // zw = resolucion interna (pixeles)
    uvec4 motion;
} camera;

// Debe coincidir con GpuSkinnedShadowPush.
layout(push_constant) uniform PushConstants {
    mat4 light_model_view_projection;
    uint bone_offset;
    float height;      // relieve en metros
    float max_factor;  // teselacion maxima del material
    uint flags;
    vec4 camera_model;  // xyz = camara en el espacio del modelo
    vec4 model_scale;   // xyz = escala del modelo por eje
} push;

layout(location = 0) in vec2 in_uv[];
layout(location = 1) in vec3 in_position[];
layout(location = 2) in vec3 in_normal[];

layout(location = 0) out vec2 out_uv[];
layout(location = 1) out vec3 out_position[];
layout(location = 2) out vec3 out_normal[];

const float kTargetPixels = 10.0;

// Lo mismo que skinned.tesc, en metros: un giro no cambia las longitudes,
// asi que basta con la escala por eje.
float edgeFactor(vec3 a, vec3 b) {
    vec3 s = push.model_scale.xyz;
    float distance_to_camera = max(length((0.5 * (a + b) - push.camera_model.xyz) * s), 0.05);
    float pixels = length((a - b) * s) * camera.projection[1][1] * 0.5 * max(camera.jitter.w, 1.0) / distance_to_camera;
    return clamp(pixels / kTargetPixels, 1.0, clamp(push.max_factor, 1.0, 64.0));
}

bool outsideLight() {
    mat4 m = push.light_model_view_projection;
    float scale = max(min(push.model_scale.x, min(push.model_scale.y, push.model_scale.z)), 1e-4);
    float slack = push.height / scale * max(length(m[0].xyz), max(length(m[1].xyz), length(m[2].xyz)));
    vec4 c0 = m * vec4(in_position[0], 1.0);
    vec4 c1 = m * vec4(in_position[1], 1.0);
    vec4 c2 = m * vec4(in_position[2], 1.0);
    if (c0.x > c0.w + slack && c1.x > c1.w + slack && c2.x > c2.w + slack) return true;
    if (c0.x < -c0.w - slack && c1.x < -c1.w - slack && c2.x < -c2.w - slack) return true;
    if (c0.y > c0.w + slack && c1.y > c1.w + slack && c2.y > c2.w + slack) return true;
    if (c0.y < -c0.w - slack && c1.y < -c1.w - slack && c2.y < -c2.w - slack) return true;
    return false;
}

void main() {
    out_uv[gl_InvocationID] = in_uv[gl_InvocationID];
    out_position[gl_InvocationID] = in_position[gl_InvocationID];
    out_normal[gl_InvocationID] = in_normal[gl_InvocationID];

    if (gl_InvocationID == 0) {
        if (outsideLight()) {
            gl_TessLevelOuter[0] = 0.0;
            gl_TessLevelOuter[1] = 0.0;
            gl_TessLevelOuter[2] = 0.0;
            gl_TessLevelInner[0] = 0.0;
        } else {
            float e0 = edgeFactor(in_position[1], in_position[2]);
            float e1 = edgeFactor(in_position[2], in_position[0]);
            float e2 = edgeFactor(in_position[0], in_position[1]);
            gl_TessLevelOuter[0] = e0;
            gl_TessLevelOuter[1] = e1;
            gl_TessLevelOuter[2] = e2;
            gl_TessLevelInner[0] = max(e0, max(e1, e2));
        }
    }
}
