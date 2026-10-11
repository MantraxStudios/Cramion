#version 450

// Teselacion de los materiales con relieve teselado (G-buffer): cuanto se
// parte cada triangulo. Cada borde se divide segun los pixeles que ocupa en
// pantalla (un vertice cada ~kTargetPixels), hasta el maximo del material.
// El factor de un borde solo depende de sus dos vertices: dos triangulos
// vecinos lo parten igual y no se abren grietas.

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

// Debe coincidir con GpuSkinnedPush (y con skinned.vert).
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 base_color;
    vec4 emissive;  // w = relieve (metros)
    vec4 material;
    uint bone_offset;
    float reflectance;
    uint pick_id;
    uint flags;  // bits 8-15: teselacion maxima
} push;

layout(location = 0) in vec3 in_normal[];
layout(location = 1) in vec2 in_uv[];
layout(location = 2) in vec4 in_tangent[];
layout(location = 3) in vec3 in_world_position[];
layout(location = 4) in vec4 in_current_clip[];
layout(location = 5) in vec4 in_previous_clip[];
layout(location = 6) in vec3 in_previous_world[];

layout(location = 0) out vec3 out_normal[];
layout(location = 1) out vec2 out_uv[];
layout(location = 2) out vec4 out_tangent[];
layout(location = 3) out vec3 out_world_position[];
layout(location = 6) out vec3 out_previous_world[];

const float kTargetPixels = 10.0;

float maxFactor() {
    return clamp(float((push.flags >> 8) & 0xFFu), 1.0, 64.0);
}

float edgeFactor(vec3 a, vec3 b) {
    float distance_to_camera = max(length(0.5 * (a + b) - camera.position.xyz), 0.05);
    float pixels = length(a - b) * abs(camera.projection[1][1]) * 0.5 * max(camera.jitter.w, 1.0) / distance_to_camera;
    return clamp(pixels / kTargetPixels, 1.0, maxFactor());
}

// Fuera de la vista (con margen por el relieve): no se dibuja.
bool outsideView() {
    float slack = push.emissive.w * max(abs(camera.projection[0][0]), abs(camera.projection[1][1]));
    vec4 c0 = camera.view_projection * vec4(in_world_position[0], 1.0);
    vec4 c1 = camera.view_projection * vec4(in_world_position[1], 1.0);
    vec4 c2 = camera.view_projection * vec4(in_world_position[2], 1.0);
    if (c0.w < -slack && c1.w < -slack && c2.w < -slack) return true;
    if (c0.x > c0.w + slack && c1.x > c1.w + slack && c2.x > c2.w + slack) return true;
    if (c0.x < -c0.w - slack && c1.x < -c1.w - slack && c2.x < -c2.w - slack) return true;
    if (c0.y > c0.w + slack && c1.y > c1.w + slack && c2.y > c2.w + slack) return true;
    if (c0.y < -c0.w - slack && c1.y < -c1.w - slack && c2.y < -c2.w - slack) return true;
    return false;
}

void main() {
    out_normal[gl_InvocationID] = in_normal[gl_InvocationID];
    out_uv[gl_InvocationID] = in_uv[gl_InvocationID];
    out_tangent[gl_InvocationID] = in_tangent[gl_InvocationID];
    out_world_position[gl_InvocationID] = in_world_position[gl_InvocationID];
    out_previous_world[gl_InvocationID] = in_previous_world[gl_InvocationID];

    if (gl_InvocationID == 0) {
        if (outsideView()) {
            gl_TessLevelOuter[0] = 0.0;
            gl_TessLevelOuter[1] = 0.0;
            gl_TessLevelOuter[2] = 0.0;
            gl_TessLevelInner[0] = 0.0;
        } else {
            // El borde i es el opuesto al vertice i.
            float e0 = edgeFactor(in_world_position[1], in_world_position[2]);
            float e1 = edgeFactor(in_world_position[2], in_world_position[0]);
            float e2 = edgeFactor(in_world_position[0], in_world_position[1]);
            gl_TessLevelOuter[0] = e0;
            gl_TessLevelOuter[1] = e1;
            gl_TessLevelOuter[2] = e2;
            gl_TessLevelInner[0] = max(e0, max(e1, e2));
        }
    }
}
