// Parallax occlusion mapping y su auto-sombra, para skinned.frag y los
// shaders de superficie del usuario (surface.frag). Antes de incluirlo hay
// que declarar `camera` y `weather` (gbuffer_surface.glsl).
//
// La altura se lee de `heights` con `mask` (p. ej. vec4(0, 1, 0, 0) = canal
// G). Blanco = alto.

// Se recorre el rayo de la vista dentro del relieve en capas hasta cruzar el
// mapa de alturas y se interpola entre las dos ultimas (Tatarchuk 2006).
// `view_ts`: hacia la camara, en espacio tangente (x = U, y = V). `scale`:
// profundidad del relieve en UV. Las derivadas se toman fuera del bucle.
vec2 parallaxMarch(sampler2D heights, vec4 mask, vec2 uv, vec2 dx, vec2 dy, vec3 view_ts, float scale) {
    // Mas capas mirando de refilon (donde el desplazamiento es mayor).
    float layers = mix(32.0, 8.0, clamp(view_ts.z, 0.0, 1.0));
    float layer_depth = 1.0 / layers;
    // Desplazamiento limitado: xy / z se dispara de refilon y la textura se
    // estiraba en rayas ("offset limiting", Welsh 2004, suavizado).
    vec2 step_uv = view_ts.xy / (view_ts.z + 0.42) * scale / layers;

    vec2 current_uv = uv;
    float current_depth = 0.0;
    float surface_depth = 1.0 - dot(textureGrad(heights, current_uv, dx, dy), mask);
    for (int i = 0; i < 32 && current_depth < surface_depth; ++i) {
        current_uv -= step_uv;
        current_depth += layer_depth;
        surface_depth = 1.0 - dot(textureGrad(heights, current_uv, dx, dy), mask);
    }
    // Interpolacion entre la capa de antes y la de despues del cruce.
    vec2 previous_uv = current_uv + step_uv;
    float after = surface_depth - current_depth;
    float before = (1.0 - dot(textureGrad(heights, previous_uv, dx, dy), mask)) - (current_depth - layer_depth);
    float weight = after / min(after - before, -1e-5);
    return mix(current_uv, previous_uv, clamp(weight, 0.0, 1.0));
}

// Auto-sombra: desde el punto que encontro el parallax se sube hacia el sol;
// si algun relieve queda por encima del rayo, ese punto esta a la sombra de
// sus propias piedras. Suave: cuenta cuanto sobresale y lo cerca que esta.
// `light_ts`: hacia el sol, en espacio tangente. 1 = al sol.
float parallaxShadowMarch(sampler2D heights, vec4 mask, vec2 uv, vec2 dx, vec2 dy, vec3 light_ts, float scale) {
    if (light_ts.z <= 0.0) return 1.0;  // de espaldas al sol: ya lo apaga N.L
    float start = dot(textureGrad(heights, uv, dx, dy), mask);
    float layers = mix(16.0, 6.0, clamp(light_ts.z, 0.0, 1.0));
    float layer_height = (1.0 - start) / layers;
    if (layer_height <= 1e-4) return 1.0;  // en lo mas alto: nada lo tapa
    vec2 step_uv = light_ts.xy / (light_ts.z + 0.42) * scale * layer_height;
    float blocked = 0.0;
    for (int i = 1; i <= 16; ++i) {
        if (float(i) > layers) break;
        float ray = start + float(i) * layer_height;
        float height = dot(textureGrad(heights, uv + step_uv * float(i), dx, dy), mask);
        blocked = max(blocked, (height - ray) * 6.0 * (1.0 - float(i) / (layers + 1.0)));
    }
    return 1.0 - clamp(blocked, 0.0, 1.0);
}

// Todo junto: la UV desplazada y, si `self_shadow`, la sombra propia hacia el
// sol en `sun_shadow`. `depth_meters`: metros del negro al blanco. Se apaga
// entre 15 y 30 m (de lejos no se nota y cuesta 8-32 lecturas). Las derivadas
// (uv_dx...) se toman en el llamador, fuera de cualquier if.
vec2 applyParallax(sampler2D heights, vec4 mask, float depth_meters, bool self_shadow, vec2 uv, vec2 uv_dx,
                   vec2 uv_dy, vec3 pos_dx, vec3 pos_dy, vec3 world_position, vec3 t, vec3 b, vec3 n,
                   inout float sun_shadow) {
    vec3 to_camera = camera.position.xyz - world_position;
    float distance_fade = 1.0 - smoothstep(15.0, 30.0, length(to_camera));
    if (distance_fade <= 0.0) return uv;
    vec3 view = normalize(to_camera);
    // La bitangente apunta a V creciente (hacia abajo en la imagen).
    vec3 view_ts = vec3(dot(view, t), dot(view, b), dot(view, n));
    if (!gl_FrontFacing) view_ts.z = -view_ts.z;
    // La profundidad va en metros: se pasa a UV con la densidad de la textura
    // en este punto (UV por metro), asi un mismo valor sirve para un suelo que
    // se repite y para un escaneo con toda la malla en una sola UV.
    float meters = length(pos_dx) + length(pos_dy);
    float uv_per_meter = (length(uv_dx) + length(uv_dy)) / max(meters, 1e-6);
    if (view_ts.z <= 0.0 || meters <= 1e-6) return uv;
    float scale = depth_meters * uv_per_meter * distance_fade;
    vec2 result = parallaxMarch(heights, mask, uv, uv_dx, uv_dy, view_ts, scale);
    // Hacia el sol (weather.decal_info.yzw; 0 = sin sol).
    vec3 to_sun = weather.decal_info.yzw;
    if (self_shadow && dot(to_sun, to_sun) > 0.5) {
        vec3 light_ts = vec3(dot(to_sun, t), dot(to_sun, b), dot(to_sun, n));
        if (!gl_FrontFacing) light_ts.z = -light_ts.z;
        sun_shadow = mix(1.0, parallaxShadowMarch(heights, mask, result, uv_dx, uv_dy, light_ts, scale), distance_fade);
    }
    return result;
}
