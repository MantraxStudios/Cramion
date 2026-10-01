// Mapa de quemado de las zonas de fuego (FirePass): capa N = zona N,
// r = carbonizado (0..1), g = calor (llamas encima ahora). Lo leen la
// geometria (gbuffer_surface.glsl) y la hierba (grass.vert).
//
// `zones[i]`: xy = esquina minima (x, z) del mundo, z = lado (m), w = parte
// del mapa usada (resolucion / 256; 0 = zona apagada). Viene de
// WeatherBuffer.fire_zones (GpuWeather en GpuTypes.h).

layout(set = 0, binding = 6) uniform sampler2DArray burn_map;

// rg del mapa en un punto del mundo (0 fuera de toda zona).
vec2 fireBurnAt(vec3 world_position, vec4 zones[4]) {
    vec2 result = vec2(0.0);
    for (int i = 0; i < 4; ++i) {
        vec4 zone = zones[i];
        if (zone.w <= 0.0) {
            continue;
        }
        vec2 uv = (world_position.xz - zone.xy) / zone.z;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
            continue;
        }
        const float half_texel = 0.5 / 256.0;
        vec2 tuv = clamp(uv * zone.w, vec2(half_texel), vec2(zone.w - half_texel));
        result = max(result, textureLod(burn_map, vec3(tuv, float(i)), 0.0).rg);
    }
    return result;
}
