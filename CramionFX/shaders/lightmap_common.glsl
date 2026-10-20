// Lightmaps horneados (PLAN-LIGHTMAPS.md 2.6): lo que necesitan skinned.frag
// y surface.frag para saber donde cae su pixel en el atlas. Se incluye
// despues de gbuffer_surface.glsl (con CRAMION_LIGHTMAP definido) y se llama a
// setupLightmap() antes de writeSurface().
//
// Registro de cada instancia (GpuLightmapRecord, VulkanRendererLightmaps.cpp):
// en el mismo hueco que su matriz en el buffer de huesos; info.x = capa del
// atlas (0xFFFFFFFF = sin lightmap) y uv_atlas = uv2 * scale_offset.xy + .zw.

// data = los vec4 propios de la instancia (INSTANCE_DATA(i) de los .crshader,
// MeshRenderer.instance_data). 64 bytes: debe coincidir con GpuLightmapRecord.
struct LightmapRecord {
    vec4 scale_offset;
    uvec4 info;
    vec4 data[2];
};
layout(std430, set = 0, binding = 8) readonly buffer LightmapRecords {
    LightmapRecord lightmap_records[];
};
layout(location = 7) in vec2 v_lightmap_uv;
layout(location = 8) flat in uint v_lightmap_slot;
layout(location = 9) flat in uint v_instance_slot;
layout(location = 10) in vec4 v_color;

// Parametros por instancia (MeshRenderer: tint, alpha, emission_scale) del
// registro de su hueco: rgb = tinte del color base (lineal), a = opacidad;
// y la escala de su emision.
vec4 instanceTint() {
    return unpackUnorm4x8(lightmap_records[v_instance_slot].info.y);
}
float instanceEmission() {
    return unpackHalf2x16(lightmap_records[v_instance_slot].info.z).x;
}
// Dato propio i (0 o 1) de la instancia; fuera de rango, cero.
vec4 instanceData(uint i) {
    return i < 2u ? lightmap_records[v_instance_slot].data[i] : vec4(0.0);
}
// false: no recibe decals (MeshRenderer.receive_decals; info.w bit 0).
bool instanceReceivesDecals() {
    return (lightmap_records[v_instance_slot].info.w & 1u) == 0u;
}

// Disolucion por tramado de lo opaco con opacidad < 1 (no se puede mezclar
// en el G-buffer): ruido de gradiente entrelazado (Jimenez 2014) que cambia
// con el jitter de la camara; el TAA/FSR/DLSS lo promedian en una
// transparencia suave. true = este pixel no se dibuja.
bool instanceDissolve(float opacity, vec2 frag_coord, vec2 jitter) {
    if (opacity >= 0.999) return false;
    vec2 p = frag_coord + jitter * 1013.0;
    float noise = fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
    return noise >= opacity;
}

// --- Fundido de LOD (dither crossfade) ---
// Mientras un rigido cambia de LOD se dibujan los dos niveles, cada uno con
// su hueco (VulkanRenderer: ActorDraw::lod_fade_slot). 16 bits altos de
// info.z: 0 = sin fundido; bits 0..14 = avance t (0..1); bit 15 = nivel que
// sale. El que entra se queda con los pixeles de ruido < t y el que sale con
// el resto (la misma trama que instanceDissolve): entre los dos cubren cada
// pixel una vez. true = este pixel no se dibuja.
bool lodFadeDiscard(vec2 frag_coord, vec2 jitter) {
    uint raw = lightmap_records[v_instance_slot].info.z >> 16u;
    if (raw == 0u) return false;
    float t = float(raw & 0x7FFFu) / 32767.0;
    vec2 p = frag_coord + jitter * 1013.0;
    float noise = fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
    return (raw & 0x8000u) != 0u ? noise < t : noise >= t;
}
// --- Fin del fundido de LOD ---

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
