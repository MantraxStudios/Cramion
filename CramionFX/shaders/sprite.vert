#version 450

// Sprites y tilemaps 2D (SpritePass): cuadrados con textura en el mundo.

layout(location = 0) in vec3 in_position;  // mundo
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;     // tinte lineal, a = opacidad
layout(location = 3) in vec2 in_params;    // x = iluminado, y = corte alfa

layout(push_constant) uniform Push {
    mat4 view_projection;
    vec4 ambient;
    uvec4 counts;  // x = luces 2D
} pc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec2 v_params;
layout(location = 3) out vec3 v_world;

void main() {
    v_uv = in_uv;
    v_color = in_color;
    v_params = in_params;
    v_world = in_position;
    gl_Position = pc.view_projection * vec4(in_position, 1.0);
}
