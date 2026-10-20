// Luces locales por clusteres (GpuClusterHeader y GpuClusterLight, GpuTypes.h;
// las reparte VulkanRenderer::buildLightClusters). Las principales (32
// puntuales y 8 focos) siguen en el uniform de las luces; aqui estan todas las
// demas que tocan la vista, sin tope.
//
// La vista va partida en columnas x filas de pantalla x rebanadas de
// profundidad exponenciales. Cada cluster tiene en el binding de la rejilla
// su primer indice y cuantas luces caen en el (dos uint por cluster) y detras
// van los indices de todos seguidos. Un pixel solo recorre las de su cluster.
//
// Antes de incluirlo se define el set y los bindings:
//   CLUSTER_SET, CLUSTER_LIGHTS_BINDING (cabecera + luces), CLUSTER_GRID_BINDING.

struct ClusterLight {
    vec4 position_range;   // xyz = posicion, w = alcance
    vec4 color_intensity;  // rgb = color (lineal), a = intensidad
    vec4 direction_inner;  // foco: xyz = direccion, w = cos del angulo interior
    vec4 params;           // x = cos del angulo exterior (< -1.5 = puntual; -3 = de particula), y = hueco de sombra,
                           // z = fuerza de la sombra, w = radio de la fuente (m)
};

layout(std430, set = CLUSTER_SET, binding = CLUSTER_LIGHTS_BINDING) readonly buffer ClusterLights {
    uvec4 cluster_grid;    // columnas, filas, rebanadas, numero de luces (0 = ninguna)
    vec4 cluster_tile;     // xy = teselas por pixel, rebanada = log2(profundidad) * z + w
    vec4 cluster_axis;     // xyz = eje de la camara, w = dot(eje, camara)
    vec4 cluster_reserved;
    ClusterLight cluster_lights[];
};

layout(std430, set = CLUSTER_SET, binding = CLUSTER_GRID_BINDING) readonly buffer LightClusters {
    uint cluster_items[];
};

// x = donde empieza (en cluster_items) la lista del cluster del pixel, y =
// cuantas luces tiene (0 sin luces por clusteres).
uvec2 clusterRange(vec2 frag_coord, vec3 world_position) {
    if (cluster_grid.w == 0u) return uvec2(0u);
    uvec2 cell = min(uvec2(frag_coord * cluster_tile.xy), cluster_grid.xy - 1u);
    float depth = dot(cluster_axis.xyz, world_position) - cluster_axis.w;
    int slice = int(floor(log2(max(depth, 1e-4)) * cluster_tile.z + cluster_tile.w));
    uint s = uint(clamp(slice, 0, int(cluster_grid.z) - 1));
    uint cluster = (s * cluster_grid.y + cell.y) * cluster_grid.x + cell.x;
    uint first_index = 2u * cluster_grid.x * cluster_grid.y * cluster_grid.z;
    return uvec2(first_index + cluster_items[2u * cluster], cluster_items[2u * cluster + 1u]);
}

// Misma caida que lighting.frag (attenuation): inversa del cuadrado con una
// ventana que llega a 0 en el alcance.
float clusterAttenuation(float distance_to_light, float range) {
    float ratio = distance_to_light / max(range, 0.0001);
    float window = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
    return (window * window) / (distance_to_light * distance_to_light + 1.0);
}

// Lo que llega de la luz al punto, sin sombra: direccion hacia ella,
// distancia y radiancia (con el cono si es un foco). false si no llega.
bool clusterLightIncoming(ClusterLight light, vec3 world_position, out vec3 light_direction,
                          out float distance_to_light, out vec3 radiance) {
    vec3 to_light = light.position_range.xyz - world_position;
    distance_to_light = length(to_light);
    light_direction = to_light / max(distance_to_light, 0.0001);
    radiance = vec3(0.0);
    if (distance_to_light > light.position_range.w) return false;
    // Luces de particula (VFX Graph, salida Luz): params.x = -3, sin sombra.
    // direction_inner.x = exponente de la caida lineal (0 = la fisica de
    // abajo), .y = 1 si tambien iluminan lo transparente. Los shaders de lo
    // transparente definen CLUSTER_TRANSLUCENT antes de incluir este archivo.
    if (light.params.x < -2.5) {
#ifdef CLUSTER_TRANSLUCENT
        if (light.direction_inner.y < 0.5) return false;
#endif
        if (light.direction_inner.x > 0.0) {
            float linear = clamp(1.0 - distance_to_light / max(light.position_range.w, 0.0001), 0.0, 1.0);
            radiance = light.color_intensity.rgb * light.color_intensity.a * pow(linear, light.direction_inner.x);
            return true;
        }
    }
    float cone = 1.0;
    if (light.params.x > -1.5) {
        float cosine = dot(-light_direction, light.direction_inner.xyz);
        cone = clamp((cosine - light.params.x) / max(light.direction_inner.w - light.params.x, 0.0001), 0.0, 1.0);
        if (cone <= 0.0) return false;
        cone *= cone;
    }
    radiance = light.color_intensity.rgb * light.color_intensity.a * cone *
               clusterAttenuation(distance_to_light, light.position_range.w);
    return true;
}
