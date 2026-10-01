#version 450
#extension GL_GOOGLE_include_directive : require

// Agua: la malla a su sitio en el mundo, desplazada por el oleaje.
//   Oceano: clipmap centrado en la camara (instancia = nivel; cada nivel con
//           el doble de separacion que el anterior), oleaje FFT.
//   Lago:   cuadricula en su rectangulo, Gerstner.
//   Rio:    la cinta que llega hecha de la CPU, Gerstner suave.

layout(set = 0, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
} camera;

#include "water_common.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec2 in_flow;
layout(location = 3) in float in_slope;

layout(location = 0) out vec3 v_world_position;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec2 v_flow;
layout(location = 4) out vec2 v_grid;       // xz sin desplazar, relativo al agua (ruido que no se estira)
layout(location = 5) out float v_jacobian;  // < 1: crestas que rompen (espuma)
layout(location = 6) out float v_height;    // sobre el nivel del agua
layout(location = 7) flat out vec4 v_hole;  // oceano: xy = centro del nivel interior, z = medio lado (0 = no hay)
layout(location = 8) out float v_slope;     // rio: desnivel

void main() {
    WaterBody b = water.bodies[push.body];
    float t = water.time_count.x;

    vec3 base;
    v_hole = vec4(0.0);
    float spacing = 1.0;
    if (push.mesh == 1u) {
        // Clipmap: el nivel `level` va pegado a una rejilla del doble de su
        // separacion (asi el siguiente nivel encaja); en su borde exterior los
        // vertices impares se funden con los pares (la malla del siguiente
        // nivel, sin grietas ni saltos).
        int levels = int(water.ocean.z);
        int level = gl_InstanceIndex;
        float s = oceanBaseSpacing(camera.position.y - b.origin.y) * exp2(float(level));
        vec2 center = floor(camera.position.xz / (2.0 * s) + 0.5) * 2.0 * s;
        vec2 grid = in_position.xz;
        float r = max(abs(grid.x), abs(grid.y)) / 64.0;
        float morph = level < levels - 1 ? clamp((r - 0.72) / 0.2, 0.0, 1.0) : 0.0;
        vec2 odd = grid - 2.0 * floor(grid * 0.5);
        vec2 xz = center + (grid - odd * morph) * s;
        spacing = s * (1.0 + morph);
        if (level == levels - 1 && r > 0.999) {
            // Falda: el borde del ultimo nivel hasta el horizonte.
            xz = center + grid * s * 40.0;
            spacing = s * 4096.0;
        }
        base = vec3(xz.x, b.origin.y, xz.y);
        if (level > 0) {
            float inner = s * 0.5;
            vec2 inner_center = floor(camera.position.xz / (2.0 * inner) + 0.5) * 2.0 * inner;
            v_hole = vec4(inner_center, 64.0 * inner, 0.0);
        }
    } else if (push.mesh == 0u) {
        vec2 local = (in_position.xz - 0.5) * 2.0 * b.extent.xy;
        float c = cos(b.origin.w);
        float s = sin(b.origin.w);
        vec2 turned = vec2(c * local.x + s * local.y, -s * local.x + c * local.y);
        base = vec3(b.origin.x + turned.x, b.origin.y, b.origin.z + turned.y);
    } else {
        base = in_position;
    }

    vec3 normal = vec3(0.0, 1.0, 0.0);
    float jacobian = 1.0;
    vec3 offset;
    if (push.mesh == 1u && oceanAvailable()) {
        // Relativo al origen del agua (como water::oceanDisplacement en la CPU).
        offset = oceanDisplacementAt(base.xz - b.origin.xz, spacing).xyz;
    } else {
        float distance_to_camera = length(camera.position.xz - base.xz);
        // Olas relativas al origen del cuerpo de agua: no saltan cuando el
        // mundo se desplaza (origen flotante) y coinciden con water::gerstner.
        offset = gerstnerWaves(b, base.xz - b.origin.xz, t, max(distance_to_camera, 1.0), normal, jacobian);
    }
    vec3 world = base + offset;
    // Olas de lo que cae o se mueve por el agua.
    world.y += rippleHeight(base.xz);

    v_world_position = world;
    v_normal = normal;
    v_uv = in_uv;
    v_flow = in_flow;
    v_grid = base.xz - b.origin.xz;  // espacio de las olas (ver arriba)
    v_jacobian = jacobian;
    v_height = offset.y;
    v_slope = in_slope;
    gl_Position = camera.view_projection * vec4(world, 1.0);
}
