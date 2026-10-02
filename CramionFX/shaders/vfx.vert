#version 450
#extension GL_GOOGLE_include_directive : require

// VFX Graph: un quad por particula viva (dibujo indirecto: 6 vertices x
// vivas). La particula sale de la lista de vivas del efecto; el quad mira a
// la camara, se estira con la velocidad, queda horizontal o vertical.

#include "vfx_common.glsl"

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;        // textura (cuadro del flipbook)
layout(location = 2) out vec2 v_corner;    // -1..1 dentro del quad (disco)
layout(location = 3) out float v_view_depth;

const vec2 kCorners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0),
                                 vec2(-1.0, 1.0));

void main() {
    uint slot = push.slot;
    uint p = alive_list[instances[slot].range.x + uint(gl_InstanceIndex)];
    vec4 P0 = particles[p * 4u + 0u];
    vec4 P1 = particles[p * 4u + 1u];
    vec4 P2 = particles[p * 4u + 2u];
    vec4 P3 = particles[p * 4u + 3u];
    uint bits = instances[slot].flags.z;

    float t = P1.w > 0.0 ? clamp(P0.w / P1.w, 0.0, 1.0) : 1.0;
    float size = P3.x;
    if ((bits & VFX_SIZE_OVER_LIFE) != 0u) size *= vfxSizeLut(slot, t);
    vec4 color = P2;
    if ((bits & VFX_COLOR_OVER_LIFE) != 0u) color *= vfxColorLut(slot, t);

    vec3 world = (instances[slot].transform * vec4(P0.xyz, 1.0)).xyz;
    vec3 velocity = mat3(instances[slot].transform) * P1.xyz;
    if ((bits & VFX_COLOR_BY_SPEED) != 0u) {
        vec4 range = instances[slot].speed_range;
        float s = clamp((length(velocity) - range.x) / max(range.y - range.x, 1e-4), 0.0, 1.0);
        color *= mix(instances[slot].speed_slow, instances[slot].speed_fast, s);
    }

    vec2 corner = kCorners[gl_VertexIndex % 6];
    vec3 right = frame.right.xyz;
    vec3 up = frame.up.xyz;
    vec3 to_camera = frame.camera.xyz - world;
    vec3 view_dir = length(to_camera) > 1e-5 ? normalize(to_camera) : vec3(0.0, 0.0, 1.0);
    float c = cos(P3.y);
    float s = sin(P3.y);
    float half_size = size * 0.5;
    vec3 offset;
    uint orient = uint(instances[slot].output0.x + 0.5);
    if (orient == 1u && length(velocity) > 1e-3) {
        // Estirada: el eje largo sigue la velocidad (proyectada en la pantalla).
        vec3 along = velocity - view_dir * dot(velocity, view_dir);
        if (length(along) < 1e-4) along = up;
        along = normalize(along);
        vec3 side = normalize(cross(along, view_dir));
        float tail = length(velocity) * max(instances[slot].output1.x, 0.0);
        float half_length = half_size + tail * 0.5;
        // La cabeza en la particula, la estela detras.
        vec3 center = world - along * (tail * 0.5);
        offset = center - world + along * (corner.y * half_length) + side * (corner.x * half_size);
    } else if (orient == 2u) {
        // Horizontal (ondas en el suelo).
        vec3 ax = vec3(c, 0.0, s);
        vec3 az = vec3(-s, 0.0, c);
        offset = (ax * corner.x + az * corner.y) * half_size;
    } else if (orient == 3u) {
        // Vertical: gira solo alrededor de Y (arboles lejanos, llamas).
        vec3 flat_dir = vec3(view_dir.x, 0.0, view_dir.z);
        vec3 r = length(flat_dir) > 1e-4 ? normalize(cross(vec3(0.0, 1.0, 0.0), flat_dir)) : right;
        offset = (r * corner.x + vec3(0.0, 1.0, 0.0) * corner.y) * half_size;
    } else {
        vec3 r = right * c + up * s;
        vec3 u = -right * s + up * c;
        offset = (r * corner.x + u * corner.y) * half_size;
    }
    vec3 position = world + offset;
    gl_Position = frame.view_projection * vec4(position, 1.0);
    v_view_depth = abs((frame.view * vec4(position, 1.0)).z);

    // Flipbook: columnas x filas, por cuadros/s o a lo largo de la vida.
    vec2 uv = vec2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5);
    float cols = max(instances[slot].output1.y, 1.0);
    float rows = max(instances[slot].output1.z, 1.0);
    float frames = cols * rows;
    if (frames > 1.0) {
        float fps = instances[slot].output1.w;
        float f = fps > 0.0 ? mod(P3.w + floor(P0.w * fps), frames) : min(floor(P3.w + t * frames), frames - 1.0);
        vec2 cell = vec2(mod(f, cols), floor(f / cols));
        uv = (cell + uv) / vec2(cols, rows);
    }
    v_uv = uv;
    v_corner = corner;
    v_color = color;
}
