#version 450
#extension GL_GOOGLE_include_directive : require

// Hierba en el G-buffer: color de la brizna, oclusion en la base (dentro del
// cesped llega menos luz) y el modelo de Disney "subsurface" con
// translucidez: a contraluz la hierba se ilumina, como la de verdad.

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

layout(location = 0) in vec3 v_world_position;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in float v_height;
layout(location = 3) in vec3 v_color;
layout(location = 4) in vec4 v_current_clip;
layout(location = 5) in vec4 v_previous_clip;

#include "gbuffer_surface.glsl"

void main() {
    vec3 n = normalize(v_normal);
    if (!gl_FrontFacing) n = -n;
    // Que mire mas bien hacia la camara que hacia abajo (hojas finas).
    vec3 to_camera = normalize(camera.position.xyz - v_world_position);
    if (dot(n, to_camera) < 0.0) n = normalize(n + to_camera * (0.2 - dot(n, to_camera)));

    float occlusion = mix(0.35, 1.0, smoothstep(0.0, 0.7, v_height));
    // Modelo de Disney 3 (subsurface): poco aplanado, mucha translucidez, hoja fina.
    surface_shading = vec4(3.0 / 255.0, 0.3, 0.45, 0.0);
    writeSurface(vec4(v_color, 1.0), n, n, vec3(0.0, 0.0, 1.0), n, 0.0, 0.55, occlusion, vec3(0.0), 0.03,
                 v_world_position);
    writeVelocity(v_current_clip, v_previous_clip);
}
