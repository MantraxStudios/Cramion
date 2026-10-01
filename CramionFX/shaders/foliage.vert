#version 450
#extension GL_GOOGLE_include_directive : require

// Vegetacion instanciada (FoliagePass): la instancia sale de la lista de
// visibles (gl_InstanceIndex incluye el inicio de la lista), se gira, se
// escala y se mueve con un viento jerarquico (TreeGenerator: TreeVertex):
//   1. el tronco entero se inclina (mas arriba, mas);
//   2. cada rama principal gira sobre su pivote (donde nace del tronco), con
//      su fase y su flexibilidad: la rama, sus ramitas y sus hojas a la vez;
//   3. las hojas tiemblan sobre su normal (mas en la punta de la tarjeta).
// Con `shadow` se proyecta con la matriz de la luz (cascadas).

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

#include "foliage_common.glsl"

layout(std430, set = 1, binding = 0) readonly buffer Instances {
    FoliageInstance instances[];
};
layout(std430, set = 1, binding = 1) readonly buffer Visible {
    uint visible[];
};

layout(push_constant) uniform FoliagePush {
    mat4 light_view_projection;
    vec4 params;  // x segundos, y sombra (0/1), z viento, w segundos del frame anterior
    vec4 offset;  // xyz origen flotante
} push;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_color;  // rgb tinte x 0.5 (0.5 = la textura), a oclusion (copa incluida)
layout(location = 3) in float in_wind;  // balanceo del tronco: 0 = rigido (pie), 1 = punta de la copa
layout(location = 4) in vec2 in_uv;
layout(location = 5) in vec2 in_layer_flutter;  // x = capa de textura, y = temblor (0 = corteza)
layout(location = 6) in vec3 in_pivot;          // pivote de la rama principal
layout(location = 7) in vec4 in_anim;           // r fase rama, g flexibilidad, b fase hoja, a extra

layout(location = 0) out vec3 v_world_position;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec4 v_color;
layout(location = 3) out vec4 v_current_clip;
layout(location = 4) out vec4 v_previous_clip;
layout(location = 5) out vec3 v_uv_layer;  // xy uv, z capa
layout(location = 6) out float v_leaf;
layout(location = 7) out float v_extra;     // corteza: musgo / pie oscuro; hoja: translucidez
layout(location = 8) out vec3 v_to_camera;

// Giro de `v` alrededor del eje unitario `axis` (Rodrigues).
vec3 rotateAxis(vec3 v, vec3 axis, float angle) {
    float c = cos(angle);
    float s = sin(angle);
    return v * c + cross(axis, v) * s + axis * dot(axis, v) * (1.0 - c);
}

// Posicion local (a escala 1, sin girar) movida por el viento en el instante t.
vec3 animate(vec3 p, vec3 normal, float t, float tree_phase, float strength, bool leaf, vec2 wind_dir_local) {
    float gust = sin(t * 0.45 + tree_phase * 0.5) * 0.5 + 0.5;
    gust = gust * gust * (3.0 - 2.0 * gust);
    float pulse = 0.25 + 0.75 * gust;

    // 3. Temblor de la hoja (antes de mover la rama: la hoja va con ella).
    if (leaf) {
        float lp = in_anim.b * 6.2831853 + tree_phase;
        float f = sin(t * 7.3 + lp) * 0.6 + sin(t * 11.9 + lp * 1.7) * 0.4;
        float side = sin(t * 5.1 + lp * 2.3);
        p += normal * (f * 0.03 * in_layer_flutter.y * strength * pulse);
        p.xz += wind_dir_local * (side * 0.012 * in_layer_flutter.y * strength * pulse);
    }

    // 2. Rama principal: gira sobre su pivote (sube y baja y se aparta del viento).
    float flex = in_anim.g;
    vec3 rel = p - in_pivot;
    float reach = length(rel);
    if (flex > 0.0 && reach > 1e-3) {
        float bp = in_anim.r * 6.2831853 + tree_phase;
        float swing = sin(t * 1.7 + bp) * 0.65 + sin(t * 2.9 + bp * 1.9) * 0.35;
        vec3 dir = rel / reach;
        // Eje horizontal perpendicular a la rama: la punta sube y baja.
        vec3 lift_axis = cross(dir, vec3(0.0, 1.0, 0.0));
        float la = length(lift_axis);
        float angle = flex * strength * (0.035 + 0.05 * gust) * swing;
        if (la > 1e-3) {
            rel = rotateAxis(rel, lift_axis / la, angle);
        }
        // Y se aparta en la direccion del viento (sin estirarse mucho).
        vec3 push_dir = vec3(wind_dir_local.x, 0.0, wind_dir_local.y);
        rel += push_dir * (reach * flex * strength * (0.012 + 0.03 * gust) * (0.6 + 0.4 * swing));
        p = in_pivot + rel;
    }

    // 1. El tronco: se inclina con el viento, mas arriba que abajo.
    vec2 sway = wind_dir_local * (0.1 + 0.22 * gust) * (0.75 + 0.25 * sin(t * 1.1 + tree_phase)) +
                vec2(-wind_dir_local.y, wind_dir_local.x) * (0.05 * sin(t * 0.83 + tree_phase * 1.3));
    p.xz += sway * in_wind * strength;
    // Al inclinarse baja un poco (no se estira).
    p.y -= dot(sway, sway) * in_wind * strength * strength * 0.15;
    return p;
}

void main() {
    FoliageInstance inst = instances[visible[gl_InstanceIndex]];
    float yaw = foliageYaw(inst.packed);
    float scale = foliageScale(inst.packed);
    float tint = foliageTint(inst.packed);
    float c = cos(yaw), s = sin(yaw);
    vec3 base = inst.position - push.offset.xyz;
    bool leaf = in_layer_flutter.y > 0.0;

    // Viento en el espacio local del arbol (sin escala): la direccion del
    // mundo girada al reves que el arbol, asi todos se inclinan hacia el mismo lado.
    float t = push.params.x;
    float strength = push.params.z;
    float tree_phase = dot(inst.position.xz, vec2(0.071, 0.053));
    vec2 wind_world = normalize(vec2(0.8, 0.6));
    vec2 wind_local = vec2(c * wind_world.x - s * wind_world.y, s * wind_world.x + c * wind_world.y);
    // Arboles grandes, menos amplitud relativa (el tronco es mas rigido).
    vec3 local = in_position;
    if (strength > 0.0) {
        local = animate(in_position, in_normal, t, tree_phase, strength / sqrt(max(scale, 0.25)), leaf, wind_local);
    }
    local *= scale;
    vec3 rotated = vec3(c * local.x + s * local.z, local.y, -s * local.x + c * local.z);
    vec3 world = base + rotated;

    v_uv_layer = vec3(in_uv, in_layer_flutter.x);
    v_leaf = leaf ? 1.0 : 0.0;
    v_extra = in_anim.a;

    vec3 n = in_normal;
    v_normal = vec3(c * n.x + s * n.z, n.y, -s * n.x + c * n.z);
    // Tono por arbol: algo mas claro u oscuro y, en las hojas, mas amarillo o
    // mas verde (la corteza varia menos).
    vec3 color = in_color.rgb * 2.0;
    if (leaf) {
        color *= mix(0.84, 1.12, tint);
        float hue = fract(tint * 7.13);
        color *= vec3(mix(0.9, 1.14, hue), 1.0, mix(1.06, 0.86, hue));
    } else {
        color *= mix(0.9, 1.08, tint);
    }
    v_color = vec4(color, in_color.a);
    v_world_position = world;
    v_to_camera = camera.position.xyz - world;
    v_current_clip = camera.unjittered_view_projection * vec4(world, 1.0);
    // La posicion del frame anterior con el viento de entonces (el vector de
    // movimiento del TAA sigue a las hojas y no las emborrona).
    vec3 previous_world = world;
    if (strength > 0.0 && push.params.y < 0.5 && push.params.w > 0.0) {
        vec3 prev_local = animate(in_position, in_normal, t - push.params.w, tree_phase, strength / sqrt(max(scale, 0.25)), leaf,
                                  wind_local) * scale;
        previous_world = base + vec3(c * prev_local.x + s * prev_local.z, prev_local.y, -s * prev_local.x + c * prev_local.z);
    }
    v_previous_clip = camera.previous_view_projection * vec4(previous_world, 1.0);
    gl_Position = push.params.y > 0.5 ? push.light_view_projection * vec4(world, 1.0)
                                      : camera.view_projection * vec4(world, 1.0);
}
