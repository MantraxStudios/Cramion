#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_GOOGLE_cpp_style_line_directive : enable

// Plantilla de fragmentos de los shaders de superficie del usuario
// (.crshader): calcula la superficie como skinned.frag (texturas y factores
// del material), se la pasa a `surface(inout Surface s)` para que la cambie y
// escribe el G-buffer con el resultado. Asi un shader propio recibe la luz,
// las sombras, los reflejos, la lluvia y los decals como cualquier material.

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

layout(set = 1, binding = 0) uniform sampler2D albedo_map;
layout(set = 1, binding = 1) uniform sampler2D metallic_roughness_map;
layout(set = 1, binding = 2) uniform sampler2D normal_map;
layout(set = 1, binding = 3) uniform sampler2D occlusion_map;
layout(set = 1, binding = 4) uniform sampler2D emissive_map;

// Debe coincidir con GpuSkinnedPush.
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 base_color;
    vec4 emissive;
    vec4 material;  // x = metal, y = rugosidad, z = fuerza de la AO, w = escala normal
    uint bone_offset;
    float reflectance;
    uint pick_id;  // aqui: bloque de propiedades del material
    uint flags;
} push;

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_tangent;
layout(location = 3) in vec3 v_world_position;
layout(location = 4) in vec4 v_current_clip;
layout(location = 5) in vec4 v_previous_clip;

const float kEmissiveIntensity = 6.0;

#include "gbuffer_surface.glsl"

#define CRAMION_FRAGMENT_STAGE 1
#include "surface_common.glsl"
#include "surface_user.glsl"

void main() {
    vec4 albedo = texture(albedo_map, v_uv) *
                  vec4(pow(max(push.base_color.rgb, vec3(0.0)), vec3(1.0 / 2.2)), push.base_color.a);

    // Base tangente y normal map, como skinned.frag.
    vec3 n = normalize(v_normal);
    vec3 t = v_tangent.xyz - n * dot(n, v_tangent.xyz);
    vec3 normal = n;
    mat3 tbn = mat3(vec3(1, 0, 0), vec3(0, 1, 0), n);
    bool has_tangent = dot(t, t) > 1e-8;
    if (has_tangent) {
        t = normalize(t);
        vec3 b = cross(n, t) * (v_tangent.w < 0.0 ? -1.0 : 1.0);
        vec2 xy = texture(normal_map, v_uv).xy * 2.0 - 1.0;
        vec3 tangent_normal = vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
        float scale = push.material.w;
        tangent_normal.y = scale >= 0.0 ? -tangent_normal.y : tangent_normal.y;
        tangent_normal.xy *= abs(scale);
        tbn = mat3(t, b, n);
        normal = normalize(tbn * tangent_normal);
    }
    if (!gl_FrontFacing) {
        normal = -normal;
        n = -n;
        tbn[2] = -tbn[2];
    }

    vec4 metallic_roughness = texture(metallic_roughness_map, v_uv);

    Surface s;
    s.albedo = albedo.rgb;
    s.alpha = albedo.a;
    s.normal = normal;
    s.metallic = clamp(push.material.x * metallic_roughness.b, 0.0, 1.0);
    s.roughness = clamp(push.material.y * metallic_roughness.g, 0.0, 1.0);
    s.occlusion = mix(1.0, texture(occlusion_map, v_uv).r, push.material.z);
    s.emission = toLinear(texture(emissive_map, v_uv).rgb) * push.emissive.rgb;
    s.uv = v_uv;
    s.worldPosition = v_world_position;
    s.vertexNormal = n;
    s.viewDirection = normalize(camera.position.xyz - v_world_position);

    surface(s);

    if (s.alpha < 0.5) {
        discard;
    }
    vec3 final_normal = normalize(s.normal);
    // Normal en espacio tangente (para el agua de la lluvia).
    vec3 tangent_normal = has_tangent ? normalize(transpose(tbn) * final_normal) : vec3(0.0, 0.0, 1.0);

    writeSurface(vec4(clamp(s.albedo, 0.0, 1.0), s.alpha), n, final_normal, tangent_normal, n,
                 clamp(s.metallic, 0.0, 1.0), clamp(s.roughness, 0.04, 1.0), clamp(s.occlusion, 0.0, 1.0),
                 max(s.emission, vec3(0.0)) * kEmissiveIntensity, push.reflectance, v_world_position);
    writeVelocity(v_current_clip, v_previous_clip);
}
