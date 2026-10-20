// Niebla por altura de lo transparente (glass.frag y surface_forward.frag):
// la misma formula que lighting.frag (heightFog) y water.frag, para que el
// vidrio, los escudos y los .crshader transparentes queden dentro de la
// niebla como lo opaco de detras (antes se veian "por encima", nitidos).
//
// Antes de incluirlo tienen que existir: camera (position), lights (hasta
// clouds: y = densidad, z = caida), shadows (params.z = vista del editor),
// environment_map y toLinear().
//
// Mezcla: lo transparente se suma a lo de detras, que ya lleva su niebla. Con
// `coverage` = cuanto tapa (1 - lo que deja pasar de detras):
//     rgb' = mix(rgb, color_niebla * coverage, niebla)
// Asi lo aditivo (coverage 0) solo se apaga y lo opaco del todo (coverage 1)
// se funde con la niebla como una pared.

const float kTransparentFogBaseHeight = 0.0;      // kFogBaseHeight de lighting.frag
const float kTransparentFogHeightFalloff = 0.08;  // kFogHeightFalloff de lighting.frag

// Fraccion de niebla entre la camara y `world_position` (0 = nada).
float transparentFogAmount(vec3 world_position) {
    // Vistas del editor sin luz (Unlit 2, Wireframe 3): sin niebla, como lo opaco.
    if (lights.clouds.y <= 0.0 || shadows.params.z > 1.5) return 0.0;
    vec3 to_point = world_position - camera.position.xyz;
    float distance_to_point = length(to_point);
    if (distance_to_point < 1e-4) return 0.0;
    vec3 ray_direction = to_point / distance_to_point;
    // Forma estable (ver heightFog en lighting.frag): sin desbordes ni NaN.
    float falloff = lights.clouds.z > 0.0 ? lights.clouds.z : kTransparentFogHeightFalloff;
    float b = falloff * ray_direction.y;
    float a0 = -(camera.position.y - kTransparentFogBaseHeight) * falloff;
    float a1 = a0 - b * distance_to_point;
    float a_max = max(a0, a1);
    float optical = abs(b) > 0.0001
        ? lights.clouds.y * exp(min(a_max, 80.0)) * (1.0 - exp(min(a0, a1) - a_max)) / abs(b)
        : lights.clouds.y * exp(min(a0, 80.0)) * distance_to_point;
    return clamp(1.0 - exp(-max(optical, 0.0)), 0.0, 1.0);
}

// Color de la niebla hacia `world_position`: el entorno muy filtrado (como el
// agua; aqui no estan los armonicos del cielo) mas el halo hacia el sol.
vec3 transparentFogColor(vec3 world_position) {
    vec3 ray_direction = normalize(world_position - camera.position.xyz);
    vec3 color = textureLod(environment_map,
                            normalize(vec3(ray_direction.x, max(ray_direction.y, 0.02), ray_direction.z)), 3.0).rgb;
    vec3 sun_direction = normalize(-lights.sun_direction_intensity.xyz);
    float sun_alignment = max(dot(ray_direction, sun_direction), 0.0);
    color += toLinear(lights.sun_color_ambient.rgb) * lights.sun_direction_intensity.w * pow(sun_alignment, 10.0) *
             0.35 * smoothstep(-0.05, 0.1, sun_direction.y);
    return min(color, vec3(30.0));
}

// Aplica la niebla al color que se suma (rgb) segun cuanto tapa (coverage).
vec3 applyTransparentFog(vec3 color, float coverage, vec3 world_position) {
    float fog = transparentFogAmount(world_position);
    if (fog <= 0.0) return color;
    return mix(color, transparentFogColor(world_position) * clamp(coverage, 0.0, 1.0), fog);
}
