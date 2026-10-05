#version 450
#extension GL_GOOGLE_include_directive : require

// Liquidos: sombreado de la superficie reconstruida (pantalla completa).
//
//   - Normal: de la profundidad suavizada (la diferencia mas pequena entre
//     el vecino de un lado y el del otro, para no cruzar bordes).
//   - Cuerpo: la escena de detras refractada por la normal y atenuada por
//     Beer-Lambert con el grosor (absorcion por canal); lo que no deja pasar
//     se ve del color difuso del liquido (iluminado por el sol y el cielo).
//   - Reflejo de Fresnel del cielo / sonda y brillo del sol (GGX) con sombra.
//   - Espuma (blanca, difusa) y emision (lava, con costra de ruido).

#include "fluid_render.glsl"

layout(set = 0, binding = 7, r32f) uniform readonly image2D depth_smooth;
layout(set = 0, binding = 8) uniform sampler2D thick_absorb;
layout(set = 0, binding = 9) uniform sampler2D thick_scatter;
layout(set = 0, binding = 10) uniform sampler2D thick_emission;

// --- Set 1: el del vidrio (VulkanRenderer::updateGlassDescriptors) ---
layout(set = 1, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
} camera;

struct PointLightGpu {
    vec4 position_range;
    vec4 color_intensity;
    vec4 shadow;
};
struct SpotLightGpu {
    vec4 position_range;
    vec4 direction_intensity;
    vec4 color_inner;
    vec4 outer_shadow;
};
layout(set = 1, binding = 1) uniform LightBuffer {
    vec4 sun_direction_intensity;
    vec4 sun_color_ambient;
    vec4 ambient_color;
    vec4 sky_sun;
    vec4 sky_moon;
    ivec4 counts;
    PointLightGpu points[32];
    SpotLightGpu spots[8];
    vec4 probes[2];
    vec4 clouds;
    vec4 environment;
} lights;

const int kShadowCascadeCount = 4;
layout(set = 1, binding = 2) uniform ShadowBuffer {
    mat4 light_view_projection[kShadowCascadeCount];
    vec4 split_distances;
    vec4 texel_world_sizes;
    vec4 params;
} shadows;
layout(set = 1, binding = 3) uniform sampler2DArrayShadow shadow_map;
layout(set = 1, binding = 4) uniform sampler2D g_depth;
layout(set = 1, binding = 5) uniform sampler2D scene_color;
layout(set = 1, binding = 6) uniform samplerCube environment_map;
// Modo compatible (moviles): el set del vidrio tiene 7 texturas y no hay
// sondas de reflexion (como glass.frag).
#ifndef CRAMION_COMPAT
layout(set = 1, binding = 7) uniform samplerCube reflection_probe_0;
layout(set = 1, binding = 8) uniform samplerCube reflection_probe_1;
#endif

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

const float kPi = 3.14159265;
const float kMaxRadiance = 40.0;

vec3 toLinear(vec3 c) { return pow(c, vec3(2.2)); }

float sunShadow(vec3 world_position, float view_depth) {
    int cascade = kShadowCascadeCount - 1;
    for (int i = 0; i < kShadowCascadeCount; ++i) {
        if (view_depth < shadows.split_distances[i]) {
            cascade = i;
            break;
        }
    }
    vec4 light_clip = shadows.light_view_projection[cascade] * vec4(world_position, 1.0);
    vec3 projected = light_clip.xyz / light_clip.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    if (projected.z > 1.0 || projected.z < 0.0 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
    float lit = texture(shadow_map, vec4(uv, float(cascade), projected.z - 0.002));
    return mix(1.0, lit, shadows.params.y);
}

float distributionGgx(float n_dot_h, float alpha) {
    float alpha2 = alpha * alpha;
    float d = n_dot_h * n_dot_h * (alpha2 - 1.0) + 1.0;
    return alpha2 / (kPi * d * d);
}

// Grosor y material suavizados: cada esfera suma su cuerda (gruesa en el
// centro, nada en el borde), asi que leidos tal cual dejan un circulo por
// particula en el color, la absorcion y la espuma. Disco de 12 muestras +
// centro con el mismo radio en metros que el suavizado de la profundidad.
const vec2 kThickTaps[12] = vec2[](vec2(0.0, 1.0), vec2(0.866, 0.5), vec2(0.866, -0.5), vec2(0.0, -1.0),
                                   vec2(-0.866, -0.5), vec2(-0.866, 0.5), vec2(0.0, 0.5), vec2(0.433, -0.25),
                                   vec2(-0.433, -0.25), vec2(0.433, 0.25), vec2(-0.433, 0.25), vec2(0.0, -0.5));

void sampleThickness(vec2 uv, float z, out vec4 absorb, out vec4 scatter, out vec4 emission) {
    absorb = textureLod(thick_absorb, uv, 0.0);
    scatter = textureLod(thick_scatter, uv, 0.0);
    emission = textureLod(thick_emission, uv, 0.0);
    float pixels = fr.params.z * fr.projection[1][1] * 0.5 * fr.viewport.y / max(z, 1e-3);
    pixels = min(pixels, 48.0);
    if (pixels < 1.5) return;
    vec2 r = pixels * fr.viewport.zw;
    float weight = 1.0;
    for (int i = 0; i < 12; ++i) {
        vec2 q = uv + kThickTaps[i] * r;
        vec4 a = textureLod(thick_absorb, q, 0.0);
        if (a.a <= 0.0) continue;  // fuera del liquido: no aclara el borde
        float w = i < 6 ? 0.6 : 0.85;
        absorb += a * w;
        scatter += textureLod(thick_scatter, q, 0.0) * w;
        emission += textureLod(thick_emission, q, 0.0) * w;
        weight += w;
    }
    absorb /= weight;
    scatter /= weight;
    emission /= weight;
}

float depthAt(ivec2 p) {
    ivec2 size = ivec2(fr.viewport.xy);
    return imageLoad(depth_smooth, clamp(p, ivec2(0), size - 1)).r;
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float z = depthAt(pixel);
    if (z <= 0.0) discard;
    vec2 uv = gl_FragCoord.xy * fr.viewport.zw;
    vec3 p = viewPosition(uv, z);

    // --- Normal (espacio de la camara) ---
    vec2 texel = fr.viewport.zw;
    float zr = depthAt(pixel + ivec2(1, 0));
    float zl = depthAt(pixel - ivec2(1, 0));
    float zd = depthAt(pixel + ivec2(0, 1));
    float zu = depthAt(pixel - ivec2(0, 1));
    vec3 ddx = zr > 0.0 ? viewPosition(uv + vec2(texel.x, 0.0), zr) - p : vec3(0.0);
    vec3 ddx2 = zl > 0.0 ? p - viewPosition(uv - vec2(texel.x, 0.0), zl) : vec3(0.0);
    if (zr <= 0.0 || (zl > 0.0 && abs(ddx2.z) < abs(ddx.z))) ddx = ddx2;
    vec3 ddy = zd > 0.0 ? viewPosition(uv + vec2(0.0, texel.y), zd) - p : vec3(0.0);
    vec3 ddy2 = zu > 0.0 ? p - viewPosition(uv - vec2(0.0, texel.y), zu) : vec3(0.0);
    if (zd <= 0.0 || (zu > 0.0 && abs(ddy2.z) < abs(ddy.z))) ddy = ddy2;
    vec3 n = cross(ddy, ddx);
    if (dot(n, n) < 1e-14) n = vec3(0.0, 0.0, 1.0);
    n = normalize(n);
    if (dot(n, -p) < 0.0) n = -n;

    // --- Grosor y material (media resolucion, bilineal) ---
    vec4 absorb;
    vec4 scatter;
    vec4 emission;
    sampleThickness(uv, z, absorb, scatter, emission);
    float thickness = max(absorb.a, 1e-4);
    vec3 sigma = absorb.rgb / thickness;
    vec3 diffuse_color = scatter.rgb / thickness;
    float foam = clamp(scatter.a / thickness, 0.0, 1.0);
    vec3 emissive = emission.rgb / thickness;
    float roughness = clamp(emission.a / thickness, 0.02, 1.0);

    // --- Mundo ---
    vec3 world = (fr.inverse_view * vec4(p, 1.0)).xyz;
    vec3 normal = normalize(mat3(fr.inverse_view) * n);
    vec3 view_direction = normalize(camera.position.xyz - world);
    float n_dot_v = clamp(dot(normal, view_direction), 0.0, 1.0);

    // --- Luz ---
    vec3 sun_direction = normalize(-lights.sun_direction_intensity.xyz);
    vec3 sun_radiance = toLinear(lights.sun_color_ambient.rgb) * lights.sun_direction_intensity.w;
    float shadow = sunShadow(world, z);
    vec3 sky_light = textureLod(environment_map, vec3(0.0, 1.0, 0.0), 6.0).rgb;
    vec3 ambient = sky_light + toLinear(lights.ambient_color.rgb) * lights.sun_color_ambient.a;
    float n_dot_l = max(dot(normal, sun_direction), 0.0);

    // --- Refraccion (lo de detras, desplazado por la normal) ---
    vec2 offset = vec2(n.x, -n.y) * 0.035 * fr.params2.x * clamp(thickness * 2.0, 0.0, 1.0);
    vec2 refract_uv = clamp(uv + offset, vec2(0.001), vec2(0.999));
    // Si lo que se ve ahi esta delante del liquido, sin desplazar.
    if (sceneLinearDepth(refract_uv) < z) refract_uv = uv;
    vec3 background = min(textureLod(scene_color, refract_uv, 0.0).rgb, vec3(kMaxRadiance));
    vec3 transmittance = exp(-sigma * thickness);
    vec3 lit_diffuse = diffuse_color * (sun_radiance * (n_dot_l * 0.8 + 0.2) * shadow + ambient);
    vec3 body = background * transmittance + lit_diffuse * (vec3(1.0) - transmittance);

    // --- Reflejo ---
    vec3 reflected = reflect(-view_direction, normal);
    float lod = roughness * 6.0;
    vec3 env_direction = normalize(vec3(reflected.x, max(reflected.y, 0.02 + roughness * 0.4), reflected.z));
    vec3 reflection = textureLod(environment_map, env_direction, lod).rgb;
#ifndef CRAMION_COMPAT
    float probe_weight = lights.probes[0].w + lights.probes[1].w;
    if (probe_weight > 0.001) {
        vec3 probe = vec3(0.0);
        if (lights.probes[0].w > 0.001) probe += textureLod(reflection_probe_0, reflected, lod).rgb * lights.probes[0].w;
        if (lights.probes[1].w > 0.001) probe += textureLod(reflection_probe_1, reflected, lod).rgb * lights.probes[1].w;
        reflection = mix(reflection, probe / probe_weight, 0.7);
    }
#endif
    reflection = min(reflection, vec3(kMaxRadiance));
    float fresnel = 0.02 + 0.98 * pow(1.0 - n_dot_v, 5.0);
    fresnel *= 1.0 - roughness * 0.6;

    vec3 h = normalize(view_direction + sun_direction);
    float alpha = max(roughness * roughness, 0.002);
    float specular = distributionGgx(max(dot(normal, h), 0.0), alpha) * fresnel * n_dot_l * 0.25;
    vec3 color = body * (1.0 - fresnel) + reflection * fresnel + sun_radiance * shadow * min(specular, 60.0);

    // --- Espuma ---
    vec3 foam_color = vec3(0.92) * (sun_radiance * (n_dot_l * 0.7 + 0.3) * shadow + ambient);
    color = mix(color, foam_color, foam * 0.85);

    // --- Emision (lava): costra oscura que se abre en grietas brillantes ---
    if (dot(emissive, emissive) > 0.0) {
        float crust = 0.0;
        for (int m = 0; m < int(kFluidMaterials); ++m) crust = max(crust, fr.materials[m].params.w * step(0.001, dot(fr.materials[m].emission_vort.rgb, vec3(1.0))));
        float t = fr.params2.y;
        float noise = fluidNoise3(world * 3.0 + vec3(0.0, t * 0.15, 0.0)) * 0.65 + fluidNoise3(world * 9.0 - vec3(t * 0.1)) * 0.35;
        float cracks = mix(1.0, smoothstep(0.35, 0.7, noise) * 1.6 + 0.08, crust);
        // Lo grueso brilla mas (el interior esta mas caliente).
        float hot = mix(0.6, 1.0, clamp(thickness * 3.0, 0.0, 1.0));
        color = mix(color, color * 0.25, crust * 0.5) + emissive * cracks * hot;
    }

    out_color = vec4(max(color, vec3(0.0)), 1.0);
}
