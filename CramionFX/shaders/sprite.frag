#version 450

// Sprites y tilemaps 2D: la textura (sRGB guardada como UNORM) se pasa a
// lineal, se tine y, si el sprite es iluminado, se multiplica por la luz
// global mas las luces 2D (circulos con caida suave en el plano XY).

layout(set = 0, binding = 0) uniform sampler2D sprite_texture;

struct Light2D {
    vec4 position_radius;
    vec4 color_intensity;
    vec4 params;  // x = caida
};
layout(std430, set = 1, binding = 0) readonly buffer Lights2D {
    Light2D lights[];
};

layout(push_constant) uniform Push {
    mat4 view_projection;
    vec4 ambient;
    uvec4 counts;
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec2 v_params;
layout(location = 3) in vec3 v_world;

layout(location = 0) out vec4 out_color;

void main() {
    vec4 texel = texture(sprite_texture, v_uv);
    if (v_params.y > 0.0 && texel.a < v_params.y) {
        discard;
    }
    float alpha = texel.a * v_color.a;
    if (alpha <= 0.002) {
        discard;
    }
    vec3 color = pow(texel.rgb, vec3(2.2)) * v_color.rgb;
    if (v_params.x > 0.5) {
        vec3 light = pc.ambient.rgb;
        for (uint i = 0u; i < pc.counts.x; ++i) {
            vec2 d = lights[i].position_radius.xy - v_world.xy;
            float radius = lights[i].position_radius.w;
            float distance_to = length(d);
            if (distance_to < radius) {
                float t = 1.0 - distance_to / radius;
                light += lights[i].color_intensity.rgb * lights[i].color_intensity.a * pow(t, lights[i].params.x);
            }
        }
        color *= light;
    }
    out_color = vec4(color, alpha);
}
