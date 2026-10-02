#version 450
#extension GL_GOOGLE_include_directive : require

// Una brizna de hierba sin malla: los vertices salen de gl_VertexIndex. De
// cerca, 7 tramos y la punta (45 vertices); de lejos, 3 tramos y la punta (21).
// Se curva hacia delante, se mece con el viento (rachas que recorren el campo)
// y se aparta y se aplasta con lo que la pisa (grass_cull.comp).

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

#define GRASS_SET 2
#include "grass_common.glsl"

// El principio de WeatherBuffer (gbuffer_surface.glsl): solo hasta las zonas
// de fuego. La hierba quemada se queda en rastrojo negro.
layout(set = 0, binding = 3) uniform WeatherBuffer {
    mat4 rain_view_projection;
    vec4 params;
    vec4 flood;
    vec4 decal_info;
    vec4 snow;
    vec4 fire_zones[4];
} weather;
#include "fire_burn.glsl"
float burn_height = 1.0;  // altura que le queda a la brizna (fuego)

layout(std430, set = 2, binding = 1) readonly buffer Blades {
    GrassBlade blades[];
};

layout(push_constant) uniform Push {
    uint segments;  // tramos (sin la punta)
} push;

layout(location = 0) out vec3 v_world_position;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out float v_height;   // 0 en la base, 1 en la punta
layout(location = 3) out vec3 v_color;     // sRGB
layout(location = 4) out vec4 v_current_clip;
layout(location = 5) out vec4 v_previous_clip;

// Viento: una ola que recorre el campo mas un temblor rapido por brizna.
vec2 windOffset(vec2 xz, float phase, float seconds) {
    vec2 dir = grass.wind.xy;
    float along = dot(xz, dir);
    float gust = sin(seconds * 1.3 - along * 0.18) * 0.5 + 0.5;
    gust = gust * gust * (0.6 + 0.4 * sin(seconds * 0.37 - along * 0.05));
    float flutter = sin(seconds * 4.7 + phase) * 0.12 + sin(seconds * 2.3 + phase * 1.7) * 0.08;
    return dir * (gust * 0.55 + flutter) * grass.tip_color.a;
}

// Punto de la brizna a la altura t (0..1) y al lado `side` (-1..1).
vec3 bladePoint(GrassBlade blade, vec4 info, float t, float side, float seconds, out vec3 tangent_up,
                out vec3 across) {
    float yaw = info.x * 6.2831853;
    float height = info.y * grass.shape.x * 2.5 * burn_height;
    float width = grass.shape.z * (1.0 + info.w * 2.0);
    vec3 facing = vec3(cos(yaw), 0.0, sin(yaw));
    across = vec3(-facing.z, 0.0, facing.x);

    // Inclinacion: la curvatura propia + el viento + el empuje (horizontal, m).
    float phase = float(blade.look & 255u) / 255.0 * 6.2831853;
    vec2 lean = facing.xz * grass.shape.w * height + windOffset(blade.position.xz, phase, seconds) * height +
                blade.push.xz;
    float lean_length = length(lean);
    // Que la brizna no se estire: al inclinarse baja (aprox. de arco).
    float press = blade.push.y;
    float up = height * (1.0 - press * 0.75) * inversesqrt(1.0 + (lean_length * lean_length) / max(height * height, 1e-4));
    float bend = t * t;  // la base casi fija, la punta se inclina
    vec3 point = blade.position.xyz + vec3(lean.x * bend, up * t, lean.y * bend);
    // Perfil de una hoja de verdad: estrecha en la base, lo mas ancho hacia
    // el primer cuarto y afilandose en punta.
    float w = width * (0.55 + 0.45 * smoothstep(0.0, 0.18, t)) * pow(1.0 - t, 0.9);
    point += across * side * w * 0.5;
    tangent_up = normalize(vec3(lean.x * 2.0 * t, up, lean.y * 2.0 * t));
    return point;
}

void main() {
    GrassBlade blade = blades[gl_InstanceIndex];
    vec4 info = unpackBlade(floatBitsToUint(blade.position.w));

    // Vertice -> (tramo, esquina): tramos de 6 vertices y la punta (3).
    uint vid = uint(gl_VertexIndex);
    uint quad = vid / 6u;
    uint corner = vid % 6u;
    float segments = float(push.segments);
    float t;
    float side;
    if (quad < push.segments) {
        // Dos triangulos: (0,-1) (0,+1) (1,-1) / (1,-1) (0,+1) (1,+1)
        const float rows[6] = float[6](0.0, 0.0, 1.0, 1.0, 0.0, 1.0);
        const float sides[6] = float[6](-1.0, 1.0, -1.0, -1.0, 1.0, 1.0);
        // Tramos mas cortos arriba (donde se curva).
        float row = float(quad) + rows[corner];
        t = 1.0 - pow(1.0 - row / (segments + 1.0), 1.4);
        side = sides[corner];
    } else {
        // Punta.
        uint c = vid - push.segments * 6u;
        float base_row = segments / (segments + 1.0);
        float base_t = 1.0 - pow(1.0 - base_row, 1.4);
        t = c == 2u ? 1.0 : base_t;
        side = c == 0u ? -1.0 : (c == 1u ? 1.0 : 0.0);
    }

    vec2 burn = fireBurnAt(blade.position.xyz, weather.fire_zones);
    burn_height = 1.0 - 0.92 * smoothstep(0.05, 0.6, burn.x);

    vec3 tangent_up;
    vec3 across;
    vec3 world = bladePoint(blade, info, t, side, grass.wind.z, tangent_up, across);
    vec3 unused_up;
    vec3 unused_across;
    vec3 previous = bladePoint(blade, info, t, side, grass.wind.w, unused_up, unused_across);

    // Normal: la de la hoja, algo redondeada hacia los lados y hacia arriba
    // (la hierba de verdad no es un plano: asi no se ve "de carton").
    vec3 n = normalize(cross(across, tangent_up));
    // Hoja con el nervio central: la normal gira hacia los bordes (se ve
    // redondeada, no un plano) y algo hacia arriba.
    n = normalize(n + across * side * 0.65 + vec3(0.0, 0.3, 0.0));

    // Color. La brizna es casi toda de su color (el de la punta); el de la
    // base solo abajo: lo oscuro del pie ya lo pone la oclusion (antes el
    // degradado entero, mas la oclusion, dejaba el cesped casi negro).
    // Variacion como en un prado de verdad (cuanta, "Variacion de color"):
    //   - por mata, el tono: unas mas amarillas, otras mas azuladas;
    //   - por manchas del campo: lo frondoso, mas oscuro y verde; lo pobre,
    //     mas claro y amarillento;
    //   - por brizna, el brillo, alguna punta seca y alguna muerta (paja).
    vec4 look = unpackBlade(blade.look);  // x fase, y tono de la mata, z frondosa, w muerta
    float amount = clamp(grass.base_color.a * 4.0, 0.0, 2.0);  // 1 con la variacion por defecto (0.25)
    float variation = (fract(info.x * 91.7 + info.y * 13.1) - 0.5) * grass.base_color.a;
    vec3 green = mix(grass.base_color.rgb, grass.tip_color.rgb, smoothstep(0.0, 0.45, t));
    float hue = (look.y - 0.5) * 2.0 * amount;
    green *= hue > 0.0 ? mix(vec3(1.0), vec3(1.12, 1.04, 0.7), min(hue, 1.0) * 0.7)
                       : mix(vec3(1.0), vec3(0.86, 0.98, 1.12), min(-hue, 1.0) * 0.7);
    green *= mix(vec3(1.0), mix(vec3(1.14, 1.1, 0.8), vec3(0.86, 0.9, 0.86), look.z), min(amount, 1.0));
    vec3 dry = mix(grass.base_color.rgb * 1.3, grass.dry_color.rgb, smoothstep(0.0, 1.0, t));
    vec3 straw = grass.dry_color.rgb * mix(0.7, 1.05, t);
    float tip_dry = smoothstep(0.75, 1.0, t) * step(0.75, fract(info.y * 37.3 + info.x * 5.1)) * 0.35 * min(amount, 1.0);
    vec3 color = mix(green, dry, max(info.z, tip_dry));
    color = mix(color, straw, look.w * min(amount, 1.0));
    v_color = clamp(color * (1.0 + variation), 0.0, 1.0);
    v_color = mix(v_color, vec3(0.05, 0.045, 0.04), smoothstep(0.0, 0.4, burn.x));

    v_world_position = world;
    v_normal = n;
    v_height = t;
    gl_Position = camera.view_projection * vec4(world, 1.0);
    v_current_clip = camera.unjittered_view_projection * vec4(world, 1.0);
    v_previous_clip = camera.previous_view_projection * vec4(previous, 1.0);
}
