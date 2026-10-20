#version 450
#extension GL_GOOGLE_include_directive : require

// VFX Graph: color de cada particula sobre la imagen HDR. Textura (o disco
// suave), intensidad HDR, luz real ("lit": sol con sombra, luces principales
// y por clusteres; ver vfxLighting), recorte de alfa y
// particulas suaves (se desvanecen al cruzar la escena). La mezcla la pone
// el pipeline: aditiva, alfa o premultiplicada. Las cintas usan la textura a
// lo largo (o un borde suave sin ella) y las mallas su UV y su normal.

#include "vfx_common.glsl"

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec2 v_corner;
layout(location = 3) in float v_view_depth;
layout(location = 4) in vec3 v_normal;
layout(location = 5) flat in uint v_mode;  // 0 quad, 1 cinta estirada, 2 cinta por longitud, 3 malla
layout(location = 6) in vec3 v_flip;       // flipbook fundido: UV del cuadro siguiente y cuanto de el

layout(location = 0) out vec4 out_color;

// --- Luz real de los efectos "lit" (VfxPass, bindings 13..17) ---
// Solo el principio de GpuLights (como glass.frag): el sol, el ambiente y las
// luces principales (32 puntuales y 8 focos). Las demas van por clusteres y
// el sol con su sombra de cascadas; en el modo compatible solo las principales.
struct VfxPointLight {
    vec4 position_range;
    vec4 color_intensity;
    vec4 shadow;
};

struct VfxSpotLight {
    vec4 position_range;
    vec4 direction_intensity;
    vec4 color_inner;
    vec4 outer_shadow;
};

layout(std140, set = 0, binding = 13) uniform VfxLights {
    vec4 sun_direction_intensity;
    vec4 sun_color_ambient;
    vec4 ambient_color;
    vec4 sky_sun;
    vec4 sky_moon;
    ivec4 counts;
    VfxPointLight points[32];
    VfxSpotLight spots[8];
} lights;

#ifndef CRAMION_COMPAT
layout(std140, set = 0, binding = 14) uniform VfxShadows {
    mat4 light_view_projection[4];
    vec4 split_distances;
    vec4 texel_world_sizes;
    vec4 params;  // x = resolucion, y = intensidad (0 = sin sombras)
} shadows;
layout(set = 0, binding = 15) uniform sampler2DArrayShadow shadow_map;
#define CLUSTER_SET 0
#define CLUSTER_LIGHTS_BINDING 16
#define CLUSTER_GRID_BINDING 17
#define CLUSTER_TRANSLUCENT  // las luces de particula sin "affectsTranslucent" no llegan
#include "light_clusters.glsl"

// Sombra del sol en un punto (un solo texel con comparacion: es humo, no
// hace falta filtrar). Corrido un poco hacia el sol en vez del sesgo de la normal.
float vfxSunShadow(vec3 world_position, vec3 to_sun) {
    if (shadows.params.y <= 0.0) return 1.0;
    float view_depth = -(frame.view * vec4(world_position, 1.0)).z;
    int cascade = 3;
    for (int i = 0; i < 4; ++i) {
        if (view_depth < shadows.split_distances[i]) {
            cascade = i;
            break;
        }
    }
    vec3 p = world_position + to_sun * shadows.texel_world_sizes[cascade] * 2.0;
    vec4 light_clip = shadows.light_view_projection[cascade] * vec4(p, 1.0);
    vec3 projected = light_clip.xyz / light_clip.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    if (projected.z > 1.0 || projected.z < 0.0 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return 1.0;
    }
    float lit = texture(shadow_map, vec4(uv, float(cascade), projected.z));
    return mix(1.0, lit, shadows.params.y);
}
#endif

// Caida de una luz local (la de lighting.frag).
float vfxAttenuation(float d, float range) {
    float ratio = d / max(range, 0.0001);
    float window = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
    return (window * window) / (d * d + 1.0);
}

// Cuanto recibe de una luz: Lambert en las mallas; en quads y cintas, con
// envoltura (el humo deja pasar algo de luz por detras).
float vfxDiffuse(vec3 n, vec3 l, bool mesh) {
    float d = dot(n, l);
    return mesh ? max(d, 0.0) : max((d + 0.5) * (1.0 / 1.5), 0.0);
}

// Luz que recibe la particula (se multiplica por su color): ambiente, sol con
// su sombra, luces principales y las de los clusteres. Sin especular.
vec3 vfxLighting(vec3 world_position, vec3 n, bool mesh) {
    vec3 total = frame.ambient.rgb;
    vec3 to_sun = normalize(-lights.sun_direction_intensity.xyz);
    vec3 sun_radiance = pow(max(lights.sun_color_ambient.rgb, vec3(0.0)), vec3(2.2)) * lights.sun_direction_intensity.w;
    float sun_amount = vfxDiffuse(n, to_sun, mesh);
    if (sun_amount > 0.0 && dot(sun_radiance, sun_radiance) > 0.0) {
#ifndef CRAMION_COMPAT
        sun_amount *= vfxSunShadow(world_position, to_sun);
#endif
        total += sun_radiance * sun_amount;
    }
    int point_count = min(lights.counts.x, 32);
    for (int i = 0; i < point_count; ++i) {
        vec3 to_light = lights.points[i].position_range.xyz - world_position;
        float d = length(to_light);
        if (d > lights.points[i].position_range.w) continue;
        total += lights.points[i].color_intensity.rgb * lights.points[i].color_intensity.a *
                 vfxAttenuation(d, lights.points[i].position_range.w) * vfxDiffuse(n, to_light / max(d, 0.0001), mesh);
    }
    int spot_count = min(lights.counts.y, 8);
    for (int i = 0; i < spot_count; ++i) {
        vec3 to_light = lights.spots[i].position_range.xyz - world_position;
        float d = length(to_light);
        if (d > lights.spots[i].position_range.w) continue;
        vec3 l = to_light / max(d, 0.0001);
        float cosine = dot(-l, normalize(lights.spots[i].direction_intensity.xyz));
        float inner_cos = lights.spots[i].color_inner.a;
        float outer_cos = lights.spots[i].outer_shadow.x;
        float cone = clamp((cosine - outer_cos) / max(inner_cos - outer_cos, 0.0001), 0.0, 1.0);
        if (cone <= 0.0) continue;
        total += lights.spots[i].color_inner.rgb * lights.spots[i].direction_intensity.w * cone * cone *
                 vfxAttenuation(d, lights.spots[i].position_range.w) * vfxDiffuse(n, l, mesh);
    }
#ifndef CRAMION_COMPAT
    uvec2 cluster_range = clusterRange(gl_FragCoord.xy, world_position);
    for (uint k = 0u; k < cluster_range.y; ++k) {
        ClusterLight light = cluster_lights[cluster_items[cluster_range.x + k]];
        vec3 l;
        float d;
        vec3 radiance;
        if (!clusterLightIncoming(light, world_position, l, d, radiance)) continue;
        total += radiance * vfxDiffuse(n, l, mesh);
    }
#endif
    return total;
}

void main() {
    uint slot = push.slot;
    // tint: el color base del material de la malla (1 sin material).
    vec4 color = v_color * instances[slot].tint;
    bool mesh = v_mode == 3u;
    // Alfa de la textura aparte: en la mezcla premultiplicada su color ya la
    // lleva y no se vuelve a multiplicar.
    float texture_alpha = 1.0;
    uint texture_slot = instances[slot].flags.w;
    if (texture_slot != VFX_NO_TEXTURE && texture_slot < VFX_TEXTURE_COUNT) {
        // Las texturas de los efectos son RGBA8 UNORM (sRGB sin convertir):
        // a lineal, como en sprite.frag. Sin esto el humo y el fuego salian
        // lavados y demasiado claros en la imagen HDR.
        vec4 texel;
        if (v_mode == 2u) {
            // Cinta por longitud: la U se repite; las derivadas de la U sin
            // repetir (fract partiria el mip en cada vuelta: una raya).
            // La V se recorta (escala/desplazamiento de la UV de la cinta) y,
            // con celda del atlas, la UV va dentro de esa celda.
            vec2 uv = vec2(fract(v_uv.x), clamp(v_uv.y, 0.0, 1.0));
            vec2 ddx = dFdx(v_uv);
            vec2 ddy = dFdy(v_uv);
            float cell = instances[slot].ribbon0.z;
            if (cell >= 0.0) {
                vec2 grid = max(instances[slot].output1.yz, vec2(1.0));
                uint frame_count = max(uint(grid.x * grid.y + 0.5), 1u);
                uint index = uint(cell + 0.5) % frame_count;
                uint columns = max(uint(grid.x + 0.5), 1u);
                vec2 origin = vec2(float(index % columns), float(index / columns));
                uv = (origin + uv) / grid;
                ddx /= grid;
                ddy /= grid;
            }
            texel = textureGrad(vfx_textures[texture_slot], uv, ddx, ddy);
        } else {
            texel = texture(vfx_textures[texture_slot], v_uv);
            // Flipbook fundido: el cuadro siguiente por la fraccion del indice.
            if (v_mode == 0u && v_flip.z > 0.0 && (instances[slot].flags.z & VFX_FLIP_BLEND) != 0u) {
                texel = mix(texel, texture(vfx_textures[texture_slot], v_flip.xy), v_flip.z);
            }
        }
        texel.rgb = pow(texel.rgb, vec3(2.2));
        texture_alpha = texel.a;
        color *= texel;
    } else if (!mesh) {
        // Disco suave (quad) o bordes suaves a lo ancho (cinta).
        float r2 = dot(v_corner, v_corner);
        if (r2 >= 1.0) discard;
        float falloff = 1.0 - r2;
        color.a *= falloff * falloff;
    }
    if (color.a <= max(instances[slot].speed_range.z, 0.002)) discard;

    // Particulas suaves: contra la profundidad lineal de la escena (las mallas
    // no: son solidas y se cortan contra la escena como cualquier objeto).
    float soft = instances[slot].output0.z;
    if (soft > 0.0 && frame.camera.w > 0.5 && !mesh) {
        float depth = texelFetch(scene_depth, ivec2(gl_FragCoord.xy), 0).r;
        if (depth < 1.0) {
            vec2 ndc = gl_FragCoord.xy * frame.viewport.zw * 2.0 - 1.0;
            vec4 v = frame.inverse_projection * vec4(ndc, depth, 1.0);
            float scene_depth_linear = abs(v.z / v.w);
            color.a *= clamp((scene_depth_linear - v_view_depth) / soft, 0.0, 1.0);
        }
    }

    vec3 rgb = color.rgb * max(instances[slot].output0.w, 0.0);
    if ((instances[slot].flags.z & VFX_LIT) != 0u) {
        if (frame.ambient.w > 0.5) {
            // Luz real: el punto en el mundo sale del depth del propio
            // fragmento (la misma matriz con la que se dibujo).
            vec2 ndc = gl_FragCoord.xy * frame.viewport.zw * 2.0 - 1.0;
            vec4 world_h = frame.inverse_view_projection * vec4(ndc, gl_FragCoord.z, 1.0);
            vec3 world_position = world_h.xyz / world_h.w;
            vec3 to_camera = frame.camera.xyz - world_position;
            vec3 n;
            if (mesh) {
                n = normalize(v_normal);
                if (!gl_FrontFacing) n = -n;
            } else if (v_mode == 0u) {
                n = normalize(v_normal);  // normal de esfera del quad (vfx.vert)
            } else {
                n = length(to_camera) > 1e-5 ? normalize(to_camera) : frame.up.xyz;  // cinta: de cara a la camara
            }
            rgb *= vfxLighting(world_position, n, mesh);
        } else {
            // Sin las luces del frame: la luz del frame con un termino de
            // Lambert hacia el sol en las mallas; el resto, plana.
            float shape = 1.0;
            if (mesh) {
                vec3 n = normalize(v_normal);
                if (!gl_FrontFacing) n = -n;
                shape = 0.45 + 0.9 * max(dot(n, frame.sun.xyz), 0.0);
            }
            rgb *= frame.light.rgb * shape;
        }
    }
    // Emision del material (no la toca la luz; 0 sin material).
    rgb += instances[slot].emissive.rgb;
    uint blend = uint(instances[slot].output0.y + 0.5);
    if (blend == 4u) {
        // Opaca (material opaco): sin mezcla.
        out_color = vec4(rgb, 1.0);
    } else if (blend == 0u) {
        // Aditiva: suma color * opacidad.
        out_color = vec4(rgb * color.a, 0.0);
    } else if (blend == 2u) {
        // Premultiplicada: el color de la textura ya lleva su opacidad; solo
        // se aplica el resto (la de la particula y la de las suaves). Antes
        // salia igual que la de alfa, con el alfa de la textura dos veces
        // (bordes oscuros).
        out_color = vec4(rgb * (color.a / max(texture_alpha, 1e-4)), color.a);
    } else {
        out_color = vec4(rgb, color.a);
    }
}
