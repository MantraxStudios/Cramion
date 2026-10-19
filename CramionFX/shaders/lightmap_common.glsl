// Lightmaps horneados (PLAN-LIGHTMAPS.md 2.6): lo que necesitan skinned.frag
// y surface.frag para saber donde cae su pixel en el atlas. Se incluye
// despues de gbuffer_surface.glsl (con CRAMION_LIGHTMAP definido) y se llama a
// setupLightmap() antes de writeSurface().
//
// Registro de cada instancia (GpuLightmapRecord, VulkanRendererLightmaps.cpp):
// en el mismo hueco que su matriz en el buffer de huesos; info.x = capa del
// atlas (0xFFFFFFFF = sin lightmap) y uv_atlas = uv2 * scale_offset.xy + .zw.

struct LightmapRecord {
    vec4 scale_offset;
    uvec4 info;
};
layout(std430, set = 0, binding = 8) readonly buffer LightmapRecords {
    LightmapRecord lightmap_records[];
};
layout(location = 7) in vec2 v_lightmap_uv;
layout(location = 8) flat in uint v_lightmap_slot;

void setupLightmap(vec3 world_position, vec3 camera_position) {
    surface_lightmap_uv2 = v_lightmap_uv;
    surface_lightmap_slot = v_lightmap_slot;
    if (v_lightmap_slot != 0xFFFFFFFFu && v_lightmap_uv.x >= 0.0) {
        LightmapRecord r = lightmap_records[v_lightmap_slot];
        if (r.info.x != 0xFFFFFFFFu) {
            surface_lightmap = vec3(v_lightmap_uv * r.scale_offset.xy + r.scale_offset.zw, float(r.info.x));
        }
    }
    surface_view = normalize(camera_position - world_position);
}
