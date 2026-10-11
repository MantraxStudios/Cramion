#version 450

// GI horneada (BakedGi.h): a media resolucion, como ssgi.frag, y con la misma
// salida (rgb = luz rebotada lista para multiplicar por el albedo, a =
// visibilidad del cielo). En lugar de recorrer la pantalla, interpola las 8
// sondas del volumen que contiene el punto (trilineal), con pesos que
// descartan las sondas metidas en la geometria y las que estan detras de la
// superficie (sin fugas de luz a traves de las paredes finas).

layout(set = 0, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
} camera;

layout(set = 0, binding = 1) uniform sampler2D g_depth;
layout(set = 0, binding = 2) uniform sampler2D g_normal;

layout(set = 0, binding = 3) readonly buffer Probes {
    vec4 data[];
} probes;

struct Volume {
    vec4 min_intensity;  // xyz = esquina, w = intensidad
    vec4 size;           // xyz = tamano
    uvec4 counts;        // xyz = sondas por eje, w = primera sonda
};

layout(set = 0, binding = 4) uniform Volumes {
    uvec4 info;  // x = cuantos volumenes, y = huecos del lightmap de superficie, z = lado del texel (bits de float)
    Volume volumes[16];
} baked;

// Lightmap de superficie (BakedGi.h): tabla hash de texeles en el mundo.
layout(set = 0, binding = 5) readonly buffer SurfaceKeys { uint keys[]; } surface_keys;
layout(set = 0, binding = 6) readonly buffer SurfaceTexels { vec4 data[]; } surface_texels;

layout(push_constant) uniform PushConstants {
    mat4 previous_view_projection;
    vec4 params;
    vec4 extra;
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_gi;

vec3 decodeNormal(vec2 e) {
    vec3 n = vec3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y);
    float t = max(-n.y, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.z += n.z >= 0.0 ? -t : t;
    return normalize(n);
}

float probeFloat(uint probe, uint index) {
    vec4 v = probes.data[probe * 8u + index / 4u];
    uint c = index % 4u;
    return c == 0u ? v.x : (c == 1u ? v.y : (c == 2u ? v.z : v.w));
}

vec3 probeCoefficient(uint probe, uint k) {
    return vec3(probeFloat(probe, k * 3u), probeFloat(probe, k * 3u + 1u), probeFloat(probe, k * 3u + 2u));
}

// Irradiancia de una sonda en la direccion n (mismo convenio que irradianceSh).
vec3 probeIrradiance(uint p, vec3 n) {
    vec3 r = probeCoefficient(p, 0u) * 0.282095;
    r += probeCoefficient(p, 1u) * 0.488603 * n.y;
    r += probeCoefficient(p, 2u) * 0.488603 * n.z;
    r += probeCoefficient(p, 3u) * 0.488603 * n.x;
    r += probeCoefficient(p, 4u) * 1.092548 * n.x * n.y;
    r += probeCoefficient(p, 5u) * 1.092548 * n.y * n.z;
    r += probeCoefficient(p, 6u) * 0.315392 * (3.0 * n.z * n.z - 1.0);
    r += probeCoefficient(p, 7u) * 1.092548 * n.x * n.z;
    r += probeCoefficient(p, 8u) * 0.546274 * (n.x * n.x - n.y * n.y);
    return max(r, vec3(0.0));
}

float probeVisibility(uint p, vec3 n) {
    float v = probeFloat(p, 27u) * 0.282095 + probeFloat(p, 28u) * 0.488603 * n.y + probeFloat(p, 29u) * 0.488603 * n.z +
              probeFloat(p, 30u) * 0.488603 * n.x;
    return clamp(v, 0.0, 1.0);
}

// El mismo hash que gfx::surfaceCellHash.
uint surfaceHash(uint lo, uint hi) {
    uint h = (lo * 0x9E3779B1u) ^ (hi * 0x85EBCA77u);
    h ^= h >> 15u;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12u;
    return h;
}

// Hueco del texel (x, y, z), o -1.
int surfaceSlot(ivec3 c) {
    uint capacity = baked.info.y;
    uvec3 b = uvec3(c + ivec3(1 << 20)) & uvec3(0x1FFFFFu);
    uint lo = b.x | (b.y << 21u);
    uint hi = (b.y >> 11u) | (b.z << 10u);
    uint slot = surfaceHash(lo, hi) & (capacity - 1u);
    for (int i = 0; i < 48; ++i) {
        uint klo = surface_keys.keys[slot * 2u];
        uint khi = surface_keys.keys[slot * 2u + 1u];
        if (klo == 0xFFFFFFFFu && khi == 0xFFFFFFFFu) return -1;
        if (klo == lo && khi == hi) return int(slot);
        slot = (slot + 1u) & (capacity - 1u);
    }
    return -1;
}

// Lightmap de superficie en p con normal n: los 8 texeles de alrededor
// (trilineal), sin los de detras de la superficie. false si no hay ninguno.
bool surfaceLightmap(vec3 world, vec3 n, out vec4 result) {
    result = vec4(0.0);
    if (baked.info.y == 0u) return false;
    float cell = uintBitsToFloat(baked.info.z);
    if (cell <= 0.0) return false;
    vec3 q = (world + n * (cell * 0.25)) / cell - 0.5;
    ivec3 base = ivec3(floor(q));
    vec3 f = q - vec3(base);
    vec3 sum = vec3(0.0);
    float vis = 0.0;
    float weight_sum = 0.0;
    for (int i = 0; i < 8; ++i) {
        ivec3 o = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        int slot = surfaceSlot(base + o);
        if (slot < 0) continue;
        vec3 tri = mix(1.0 - f, f, vec3(o));
        float w = tri.x * tri.y * tri.z;
        vec3 center = (vec3(base + o) + 0.5) * cell;
        vec3 to_cell = center - world;
        float len = length(to_cell);
        if (len > 1e-3) w *= mix(0.1, 1.0, smoothstep(-0.5, 0.2, dot(to_cell / len, n)));
        uint b4 = uint(slot) * 4u;
        vec4 t0 = surface_texels.data[b4];
        vec4 t1 = surface_texels.data[b4 + 1u];
        vec4 t2 = surface_texels.data[b4 + 2u];
        vec4 t3 = surface_texels.data[b4 + 3u];
        // floats 0..11 = c0.rgb c1.rgb c2.rgb c3.rgb; 12..15 = cielo L1
        vec3 c0 = t0.xyz;
        vec3 c1 = vec3(t0.w, t1.x, t1.y);
        vec3 c2 = vec3(t1.z, t1.w, t2.x);
        vec3 c3 = vec3(t2.y, t2.z, t2.w);
        vec3 irr = c0 * 0.282095 + c1 * 0.488603 * n.y + c2 * 0.488603 * n.z + c3 * 0.488603 * n.x;
        float sky = t3.x * 0.282095 + t3.y * 0.488603 * n.y + t3.z * 0.488603 * n.z + t3.w * 0.488603 * n.x;
        sum += max(irr, vec3(0.0)) * w;
        vis += clamp(sky, 0.0, 1.0) * w;
        weight_sum += w;
    }
    if (weight_sum < 0.02) return false;
    result = vec4(sum / weight_sum, vis / weight_sum);
    return true;
}

void main() {
    // El texel h de media resolucion representa al pixel completo 2h (como
    // ssgi.frag; lighting.frag lo reconstruye asi). Con v_uv el centro caia
    // justo entre los pixeles 2h y 2h + 1 y el muestreador elegia uno u otro.
    ivec2 full_size = textureSize(g_depth, 0);
    ivec2 pixel = min(ivec2(gl_FragCoord.xy) * 2, full_size - 1);
    float depth = texelFetch(g_depth, pixel, 0).r;
    if (depth >= 1.0) {
        out_gi = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec2 uv = (vec2(pixel) + 0.5) / vec2(full_size);
    vec4 world_h = camera.inverse_view_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec3 world = world_h.xyz / world_h.w;
    vec3 normal = decodeNormal(texelFetch(g_normal, pixel, 0).rg);
    // Lightmap de superficie primero (mas detalle); si no cubre el punto, las sondas.
    vec4 surface;
    if (surfaceLightmap(world, normal, surface)) {
        out_gi = surface;
        return;
    }
    // Un poco hacia fuera: el punto no cae justo en la pared.
    vec3 p = world + normal * 0.05;

    uint count = min(baked.info.x, 16u);
    for (uint v = 0u; v < count; ++v) {
        Volume vol = baked.volumes[v];
        vec3 local = (p - vol.min_intensity.xyz) / max(vol.size.xyz, vec3(1e-4));
        if (any(lessThan(local, vec3(-0.001))) || any(greaterThan(local, vec3(1.001)))) continue;
        uvec3 n = max(vol.counts.xyz, uvec3(2u));
        vec3 g = clamp(local, 0.0, 1.0) * vec3(n - 1u);
        uvec3 g0 = min(uvec3(floor(g)), n - 2u);
        vec3 f = g - vec3(g0);
        vec3 cell = vol.size.xyz / vec3(n - 1u);

        vec3 sum = vec3(0.0);
        float vis = 0.0;
        float weight_sum = 0.0;
        for (uint i = 0u; i < 8u; ++i) {
            uvec3 o = uvec3(i & 1u, (i >> 1u) & 1u, (i >> 2u) & 1u);
            uvec3 c = g0 + o;
            uint probe = vol.counts.w + c.x + n.x * (c.y + n.y * c.z);
            float valid = probeFloat(probe, 31u);
            if (valid <= 0.0) continue;
            vec3 tri = mix(1.0 - f, f, vec3(o));
            float w = tri.x * tri.y * tri.z;
            // Las sondas detras de la superficie pesan menos (fugas por paredes).
            vec3 probe_position = vol.min_intensity.xyz + vec3(c) * cell;
            vec3 to_probe = probe_position - world;
            float len = length(to_probe);
            if (len > 1e-3) {
                float facing = dot(to_probe / len, normal);
                w *= mix(0.05, 1.0, smoothstep(-0.2, 0.25, facing));
            }
            w = max(w, 1e-5);
            sum += probeIrradiance(probe, normal) * w;
            vis += probeVisibility(probe, normal) * w;
            weight_sum += w;
        }
        if (weight_sum <= 0.0) continue;
        out_gi = vec4(sum / weight_sum * vol.min_intensity.w, vis / weight_sum);
        return;
    }
    // Fuera de todos los volumenes: cielo abierto, sin rebote.
    out_gi = vec4(0.0, 0.0, 0.0, 1.0);
}
