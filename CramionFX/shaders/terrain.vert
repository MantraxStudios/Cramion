#version 450

// Terreno (TerrainPass): un trozo de la malla de celdas repetido con LOD. La
// altura sale de la textura de alturas (0..1 por la altura maxima); los
// vertices del faldon bajan para tapar las grietas entre LOD distintos.
// Con `shadow` = 1 se proyecta con la matriz de la luz (cascadas).

layout(set = 0, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
    mat4 unjittered_view_projection;
    mat4 previous_view_projection;
    vec4 jitter;
    uvec4 motion;  // x = primer hueso del frame anterior
} camera;

layout(set = 1, binding = 0) uniform sampler2D heightmap;
layout(set = 1, binding = 1) uniform TerrainParams {
    vec4 origin_size;   // xyz origen (esquina), w tamano
    vec4 info;          // x altura maxima, y resolucion, z 1/resolucion, w capas
    vec4 layer_params[8];
    vec4 layer_tint[8];
    vec4 layer_extra[8];
} terrain;

layout(push_constant) uniform PushConstants {
    vec4 chunk;  // xy = esquina (0..1), z = tamano (0..1), w = faldon (m)
    mat4 light_view_projection;
    uint shadow;
} push;

layout(location = 0) in vec3 in_grid;  // xy = 0..1 dentro del trozo, z = 1 en el faldon

layout(location = 0) out vec3 v_world_position;
layout(location = 1) out vec2 v_uv;  // 0..1 en todo el terreno
layout(location = 2) out vec4 v_current_clip;   // el terreno no se mueve: solo la camara
layout(location = 3) out vec4 v_previous_clip;

// Altura en uv (0..1): los texeles de la textura son los vertices del mapa.
float heightAt(vec2 uv) {
    float res = terrain.info.y;
    vec2 texel = (clamp(uv, 0.0, 1.0) * (res - 1.0) + 0.5) / res;
    return textureLod(heightmap, texel, 0.0).r;
}

void main() {
    vec2 uv = clamp(push.chunk.xy + in_grid.xy * push.chunk.z, 0.0, 1.0);
    float size = terrain.origin_size.w;
    vec3 world = terrain.origin_size.xyz + vec3(uv.x * size, heightAt(uv) * terrain.info.x, uv.y * size);
    world.y -= in_grid.z * push.chunk.w;
    v_world_position = world;
    v_uv = uv;
    v_current_clip = camera.unjittered_view_projection * vec4(world, 1.0);
    v_previous_clip = camera.previous_view_projection * vec4(world, 1.0);
    gl_Position = push.shadow != 0u ? push.light_view_projection * vec4(world, 1.0)
                                    : camera.view_projection * vec4(world, 1.0);
}
