#version 450
#extension GL_GOOGLE_include_directive : require

// Voxeles en el G-buffer: texturas PBR de su capa (color, normal + altura +
// oclusion, rugosidad + metal + emision + reflectancia), relieve con
// parallax cerca de la camara, recorte suave de hojas y plantas, tinte por
// bioma y la luz de los bloques:
//   - Luz del cielo (0..15, propagada como en Minecraft): apaga la luz
//     ambiente en cuevas e interiores (la del sol ya la cortan las sombras).
//   - Luz de antorchas: brillo calido (emision) que alumbra las cuevas.
// Despues, lluvia, charcos y decals como cualquier superficie
// (gbuffer_surface.glsl).

#include "voxel_common.glsl"

layout(set = 1, binding = 0) uniform sampler2DArray albedo_map;
layout(set = 1, binding = 1) uniform sampler2DArray normal_map;    // xy normal, z altura, w oclusion
layout(set = 1, binding = 2) uniform sampler2DArray material_map;  // r rugosidad, g metal, b emision, a reflectancia

layout(location = 0) in vec3 v_world_position;
layout(location = 1) in vec2 v_uv;
layout(location = 2) flat in uvec2 v_face_layer;
layout(location = 3) in vec3 v_light;
layout(location = 4) in vec3 v_tint;
layout(location = 5) in vec4 v_current_clip;
layout(location = 6) in vec4 v_previous_clip;
layout(location = 7) in vec3 v_to_camera;
layout(location = 8) in vec2 v_box_uv;

#include "gbuffer_surface.glsl"

// Caras de capas sin huecos (VoxelPass: grupos 0-7, pipeline aparte): sin
// discard, la GPU puede descartar lo tapado antes de sombrear.
layout(constant_id = 10) const bool kVoxelOpaque = false;
// TESELADO: pipeline con relieve teselado (voxel.tesc / voxel.tese): el parallax solo pone lo que el teselado no
// (lejos, donde voxelTessFade baja), sin relieve doble.
layout(constant_id = 11) const bool kVoxelTess = false;

const float kParallaxDepth = 0.045;   // metros de relieve maximo
const float kParallaxDistance = 14.0; // mas lejos, sin relieve (no se ve)

// Terreno lejano (VoxelPass::setFarSection): columnas de color liso (la media
// de la textura del bloque, ya con el tinte del bioma), sin texturas. Dentro
// del circulo de los chunks de verdad no se dibuja (ellos se ven).
void farFragment() {
    vec2 rel = v_world_position.xz - push.distant.xy;
    bool hidden = push.distant.z > 0.0 && dot(rel, rel) < push.distant.z * push.distant.z;
    // Al final de la distancia se deshace con un tramado (el TAA lo suaviza).
    if (push.far_fade_end > 0.0) {
        float fade = smoothstep(0.85, 1.0, length(v_to_camera.xz) / push.far_fade_end);
        float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
        hidden = hidden || noise < fade;
    }
    uint face = v_face_layer.x & 7u;
    bool water = (v_face_layer.x & 32u) != 0u;
    vec3 n = kFaceNormal[face];
    vec3 albedo = v_tint;
    if (!water) {
        // Cada columna con su matiz (como el color liso): sin esto, laderas planas.
        float scale = max(push.distant.w, 1.0);
        vec3 cell = floor((v_world_position - n * 0.5) / vec3(scale, 2.0, scale) + 0.001);
        uint h = uint(int(cell.x) * 73856093) ^ uint(int(cell.y) * 19349663) ^ uint(int(cell.z) * 83492791);
        h ^= h >> 13u;
        h *= 0x5bd1e995u;
        h ^= h >> 15u;
        albedo *= 0.92 + float(h & 1023u) / 1023.0 * 0.14;
    }
    float roughness = water ? 0.06 : clamp(float(v_face_layer.y) / 15.0, 0.3, 1.0);
    float occlusion = mix(0.45, 1.0, v_light.x * v_light.x);
    writeSurface(vec4(clamp(albedo, 0.0, 1.0), 1.0), n, n, vec3(0.0, 0.0, 1.0), n, 0.0, roughness, occlusion, vec3(0.0),
                 water ? 0.02 : 0.04, v_world_position);
    writeVelocity(v_current_clip, v_previous_clip);
    if (!kVoxelOpaque && hidden) discard;
}

void main() {
    if (farSection()) {
        farFragment();
        return;
    }
    uint face = v_face_layer.x & 7u;
    bool fine = (v_face_layer.x & 8u) != 0u;  // pieza de valla o farol: uv del mundo
    float layer = float(v_face_layer.y);
    vec3 n = kFaceNormal[face];
    vec3 u_axis = kFaceU[face];
    vec3 v_axis = kFaceV[face];
    vec3 up_axis = -v_axis;  // "arriba" en la imagen (verde del normal map)

    // Caras fusionadas (Mesher.cpp: rectangulos de varios bloques): la textura se
    // repite por bloque, con uv desde la posicion como los vertices finos.
    bool world_uv = fine || (v_face_layer.x & 64u) != 0u;
    // Derivadas fuera de cualquier if (despues el parallax desplaza las uv). De
    // la uv sin fract: con fract saltaban en el borde de cada bloque (mip
    // equivocado, una linea de pixeles).
    vec2 uv = world_uv ? fract(v_box_uv) : v_uv;
    vec2 duv_dx = dFdx(world_uv ? v_box_uv : v_uv);
    vec2 duv_dy = dFdy(world_uv ? v_box_uv : v_uv);

    bool lowest = (push.style & 8u) != 0u;
    // VARIACION: la misma textura en todos los bloques dibujaba una cuadricula (la misma grieta en cada piedra).
    // Cada bloque voltea su textura segun un hash de su celda: en los lados solo en horizontal (lo de arriba sigue
    // arriba: hierba, troncos, ladrillos), arriba / abajo en los dos ejes (sin girar: los tablones no cambian de eje).
    // Nada en plantas (cara 6), piezas finas (puertas, vallas) ni en el modo minimo.
    vec2 flip = vec2(1.0);
    if (face != 6u && !fine && !lowest) {
        vec3 cell = floor(v_world_position - n * 0.5 + 0.001);
        uint h = uint(int(cell.x) * 73856093) ^ uint(int(cell.y) * 19349663) ^ uint(int(cell.z) * 83492791);
        h ^= h >> 13u;
        h *= 0x5bd1e995u;
        h ^= h >> 15u;
        bool top_bottom = abs(n.y) > 0.5;
        flip = vec2(((h & 1u) != 0u) ? -1.0 : 1.0, (top_bottom && ((h & 2u) != 0u)) ? -1.0 : 1.0);
        uv = mix(uv, 1.0 - uv, step(flip, vec2(0.0)));
        duv_dx *= flip;
        duv_dy *= flip;
    }

    bool flat_style = (push.style & 1u) != 0u;
    // bit 2: sin relieve (movil en Baja y Media: hasta 13 lecturas de textura por pixel)
    bool no_parallax = (push.style & 4u) != 0u;
    // bit 3: lo minimo (movil en Baja): sin normal map (la normal de la cara) y
    // lecturas con derivadas implicitas donde se puede (textureGrad va a un
    // cuarto de velocidad en muchas GPU de movil). (lowest: arriba, antes de la VARIACION)

    // --- Relieve (parallax con busqueda por pasos) ---
    // (En color liso no hay relieve: la textura es plana.)
    float distance_to_camera = length(v_to_camera);
    float parallax_weight = 1.0;
    if (kVoxelTess && !fine) parallax_weight = 1.0 - voxelTessFade(distance_to_camera);
    if (!flat_style && !no_parallax && face != 6u && distance_to_camera < kParallaxDistance && parallax_weight > 0.01) {
        vec3 view = v_to_camera / max(distance_to_camera, 1e-4);
        vec3 view_ts = vec3(dot(view, u_axis), dot(view, up_axis), dot(view, n));
        float fade = 1.0 - smoothstep(kParallaxDistance * 0.6, kParallaxDistance, distance_to_camera);
        // Direccion en el espacio de la textura (v hacia abajo).
        vec2 shift = vec2(view_ts.x, -view_ts.y) / max(view_ts.z, 0.25) * kParallaxDepth * fade * parallax_weight * flip;
        const int kSteps = 12;
        float step_depth = 1.0 / float(kSteps);
        float layer_depth = 0.0;
        vec2 current = uv;
        float depth_here = 1.0 - textureGrad(normal_map, vec3(current, layer), duv_dx, duv_dy).z;
        vec2 previous = current;
        float previous_depth = depth_here;
        for (int i = 0; i < kSteps && layer_depth < depth_here; ++i) {
            previous = current;
            previous_depth = depth_here - layer_depth;
            current -= shift * step_depth;
            layer_depth += step_depth;
            depth_here = 1.0 - textureGrad(normal_map, vec3(current, layer), duv_dx, duv_dy).z;
        }
        // Interpolacion entre los dos ultimos pasos.
        float after = depth_here - layer_depth;
        float weight = after / min(after - previous_depth, -1e-4);
        uv = mix(current, previous, clamp(weight, 0.0, 1.0));
    }

    // Lo minimo: la uv continua (sin fract; el sampler repite) con derivadas implicitas.
    vec2 continuous_uv = world_uv ? v_box_uv : v_uv;
    vec4 albedo = lowest ? texture(albedo_map, vec3(continuous_uv, layer))
                         : textureGrad(albedo_map, vec3(uv, layer), duv_dx, duv_dy);
    // Recorte con borde nitido a cualquier distancia (las hojas no se
    // deshacen con los mipmaps).
    float coverage = (albedo.a - 0.5) / max(length(vec2(dFdx(albedo.a), dFdy(albedo.a))), 1e-4) + 0.5;
    bool cut_out = coverage < 0.5;
    albedo.rgb *= v_tint;
    if (flat_style) {
        // Cada bloque con su propio matiz (el bloque: hacia dentro desde la
        // cara). Sin esto, el color liso son paredes planas de un solo tono.
        // (Las plantas son cruces dentro de su celda: sin desplazar.)
        vec3 cell = floor(v_world_position - (face == 6u ? vec3(0.0) : n * 0.5) + 0.001);
        uint h = uint(int(cell.x) * 73856093) ^ uint(int(cell.y) * 19349663) ^ uint(int(cell.z) * 83492791);
        h ^= h >> 13u;
        h *= 0x5bd1e995u;
        h ^= h >> 15u;
        float shade = float(h & 1023u) / 1023.0;
        float hue = float((h >> 10u) & 1023u) / 1023.0;
        albedo.rgb *= 0.9 + shade * 0.17;
        albedo.rgb *= mix(vec3(1.0, 0.98, 0.95), vec3(0.96, 1.0, 1.03), hue);
    }

    vec4 nt = lowest ? vec4(0.5, 0.5, 1.0, 1.0) : textureGrad(normal_map, vec3(uv, layer), duv_dx, duv_dy);
    vec4 material = lowest ? texture(material_map, vec3(continuous_uv, layer))
                           : textureGrad(material_map, vec3(uv, layer), duv_dx, duv_dy);
    vec2 xy = nt.xy * 2.0 - 1.0;
    xy *= flip;  // VARIACION: la normal sigue al volteo de la textura
    vec3 tangent_normal = vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
    vec3 normal = normalize(u_axis * tangent_normal.x + up_axis * tangent_normal.y + n * tangent_normal.z);
    if (face == 6u) {
        // Plantas: normal hacia la camara (de las dos caras) e inclinada hacia arriba.
        vec3 side = normalize(vec3(v_to_camera.x, 0.0, v_to_camera.z) + vec3(1e-4));
        normal = normalize(side * 0.35 + vec3(0.0, 1.0, 0.0));
        n = normal;
    }

    // --- Luz de los bloques ---
    float corner = mix(0.28, 1.0, v_light.x * v_light.x);
    float sky = pow(v_light.y, 1.6);
    float torch = v_light.z;
    float occlusion = corner * nt.w * mix(0.025, 1.0, sky);
    vec3 linear_albedo = toLinear(albedo.rgb);
    vec3 emissive = linear_albedo * vec3(1.0, 0.68, 0.38) * pow(torch, 2.4) * 2.2 * corner;
    emissive += linear_albedo * material.b * 7.0;
    if (fine && material.b > 0.0) {
        // Faroles: la llama titila (fase por celda, cada farol a su ritmo).
        float phase = dot(floor(v_world_position - n * 0.01), vec3(12.9898, 78.233, 37.719));
        float t = push.origin_time.w;
        emissive *= 0.84 + 0.09 * sin(t * 9.0 + phase) + 0.07 * sin(t * 23.0 + phase * 1.7);
    }

    // Marca de voxel en el G-buffer (modelo estandar con b = a = 1; el estandar
    // no usa b ni a): lighting.frag no les suma las luces de los bloques
    // (PointLight::skip_voxels), cuya luz ya va arriba en `emissive`.
    surface_shading = vec4(0.0, 0.0, 1.0, 1.0);
    writeSurface(vec4(clamp(albedo.rgb, 0.0, 1.0), 1.0), n, normal, tangent_normal, n, material.g,
                 clamp(material.r, 0.04, 1.0), occlusion, emissive, material.a * 0.08, v_world_position);
    writeVelocity(v_current_clip, v_previous_clip);
    // El recorte se hace al final: un discard antes dejaba sin definir las
    // derivadas (dFdx/dFdy, texturas con mipmap) de los vecinos del cuadro 2x2
    // en los bordes recortados, y writeSurface las usa (antialiasing
    // especular, decals).
    if (!kVoxelOpaque && cut_out) discard;
}
