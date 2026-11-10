#version 450
#extension GL_GOOGLE_include_directive : require

// Voxeles (VoxelPass): desempaqueta el vertice (8 bytes) y lo coloca en la
// seccion. Las hojas y las plantas ondean con el viento. Con `shadow` = 1 se
// proyecta con la matriz de la luz (cascadas).

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

#include "voxel_common.glsl"

layout(location = 0) in uvec2 in_packed;

// Personajes que apartan las plantas (VoxelPass::setInteractors): hasta 16,
// los de este frame y los del anterior (vectores de movimiento sin estela en
// el TAA). Cada uno es una capsula a ras de suelo desde su estela (donde
// estuvo hace un momento) hasta los pies: lo que deja atras se endereza poco
// a poco en lugar de saltar. Debe coincidir con GpuFoliageInteractors (VoxelPass.cpp).
const int kMaxFoliageInteractors = 16;
layout(set = 1, binding = 3) uniform FoliageInteractors {
    uvec4 count;                                 // x este frame, y el anterior
    vec4 box_min;                                // xyz: caja de lo que pueden tocar todos
    vec4 box_max;
    vec4 feet[kMaxFoliageInteractors];           // xyz pies, w radio (m)
    vec4 trail[kMaxFoliageInteractors];          // xyz estela, w fuerza 0..1 (aparece / se va)
    vec4 previous_feet[kMaxFoliageInteractors];  // los mismos del frame anterior
    vec4 previous_trail[kMaxFoliageInteractors];
} interactors;

// Empuje (m) de un vertice de planta en reposo `p` por los personajes: hacia
// fuera de cada uno y algo hacia abajo (aplastada), mas fuerte cerca de los pies.
vec3 foliagePush(vec3 p, bool previous) {
    uint n = previous ? interactors.count.y : interactors.count.x;
    vec3 total = vec3(0.0);
    for (uint i = 0u; i < n; ++i) {
        vec4 f = previous ? interactors.previous_feet[i] : interactors.feet[i];
        vec4 t = previous ? interactors.previous_trail[i] : interactors.trail[i];
        float reach = f.w + 0.45;  // un poco mas alla del cuerpo
        // Punto de la capsula (estela -> pies) mas cercano, en el plano del suelo.
        vec2 seg = f.xz - t.xz;
        float seg2 = dot(seg, seg);
        float h = seg2 > 1e-4 ? clamp(dot(p.xz - t.xz, seg) / seg2, 0.0, 1.0) : 1.0;
        vec3 q = mix(t.xyz, f.xyz, h);
        vec2 d = p.xz - q.xz;
        float dist2 = dot(d, d);
        if (dist2 >= reach * reach) continue;
        // Altura: de un poco por debajo de los pies hasta ~2 m por encima.
        float dy = p.y - q.y;
        float vertical = smoothstep(-0.9, -0.3, dy) * (1.0 - smoothstep(1.8, 2.6, dy));
        // La estela empuja menos cuanto mas atras (y cuanto mas larga).
        float along = 1.0 - (1.0 - h) * clamp(sqrt(seg2) / reach, 0.0, 1.0);
        float dist = sqrt(dist2);
        float k = 1.0 - smoothstep(0.0, reach, dist);
        k = k * k * (3.0 - 2.0 * k) * vertical * along * t.w;
        // Hacia fuera; en el mismo centro casi nada de lado (solo aplasta).
        vec2 away = d / max(dist, 0.3);
        total += vec3(away.x * 0.5, -0.38, away.y * 0.5) * k;
    }
    // Varios a la vez no la tumban del todo.
    float side = length(total.xz);
    if (side > 0.6) total.xz *= 0.6 / side;
    total.y = max(total.y, -0.55);
    // Al tumbarse de lado tambien baja (no se estira).
    total.y -= 0.35 * dot(total.xz, total.xz);
    return total;
}

// Desplazamiento del viento en el instante `t`.
vec3 windSway(float t, float phase, float weight) {
    vec2 sway = vec2(sin(t * 1.6 + phase), cos(t * 1.25 + phase * 1.31)) * 0.055;
    return vec3(sway.x, sin(t * 2.1 + phase * 1.7) * 0.012, sway.y) * weight;
}

// TESELADO: en las sombras, las caras que miran a la luz se empujan hacia atras a lo largo de su rayo (la luz las ve
// en el mismo sitio: no se abren huecos por donde pase luz) lo que puede hundirse el relieve teselado
// (push.tess.y bloques hacia dentro, voxel.tese). Asi el relieve no se sombrea a si mismo con la cara plana (acne):
// cada punto hundido queda delante de la cara que lo proyecta. Las caras de espaldas a la luz no se mueven (un
// bloque sigue tapando por ellas). No depende de la camara: las cascadas guardadas siguen valiendo.
vec3 shadowCaster(vec3 world, uint face, bool fine) {
    if (push.tess.y <= 0.0 || face >= 6u || fine) return world;
    mat4 m = push.light_view_projection;
    vec4 r0 = vec4(m[0][0], m[1][0], m[2][0], m[3][0]);
    vec4 r1 = vec4(m[0][1], m[1][1], m[2][1], m[3][1]);
    vec4 r2 = vec4(m[0][2], m[1][2], m[2][2], m[3][2]);
    vec4 r3 = vec4(m[0][3], m[1][3], m[2][3], m[3][3]);
    vec3 travel;  // del foco hacia el vertice
    if (dot(r3.xyz, r3.xyz) < 1e-12) {
        travel = normalize(r2.xyz);  // ortografica (sol, lluvia): la profundidad crece hacia donde va la luz
    } else {
        // Perspectiva (antorchas, focos): el foco es el punto con x = y = w = 0 en el recorte.
        mat3 a = transpose(mat3(r0.xyz, r1.xyz, r3.xyz));
        vec3 light = inverse(a) * -vec3(r0.w, r1.w, r3.w);
        travel = normalize(world - light);
    }
    float facing = -dot(kFaceNormal[face], travel);
    if (facing <= 0.0) return world;
    return world + travel * (push.tess.y / max(facing, 0.25));
}

layout(location = 0) out vec3 v_world_position;
layout(location = 1) out vec2 v_uv;
layout(location = 2) flat out uvec2 v_face_layer;  // x cara, y capa
layout(location = 3) out vec3 v_light;             // x oclusion de la esquina, y cielo, z antorchas (0..1)
layout(location = 4) out vec3 v_tint;
layout(location = 5) out vec4 v_current_clip;
layout(location = 6) out vec4 v_previous_clip;
layout(location = 7) out vec3 v_to_camera;
layout(location = 8) out vec2 v_box_uv;  // vertices finos: uv desde la posicion (voxel.frag la usa con fract)

// Terreno lejano (VoxelPass::setFarSection): columnas coloreadas de 2^n
// bloques. Color sRGB y rugosidad por vertice (v_tint y v_light.y). Se empuja
// un poco hacia atras a lo largo del rayo de la camara (misma posicion en
// pantalla, algo mas de profundidad): donde coincide con un chunk de verdad
// (el borde del hueco), gana siempre el chunk.
void farVertex(uint a, uint b) {
    float scale = push.distant.w;
    float y = float(((a >> 5u) & 31u) | (((a >> 23u) & 15u) << 5u));
    vec3 local = vec3(float(a & 31u) * scale, y, float((a >> 10u) & 31u) * scale);
    uint face = (a >> 15u) & 7u;
    vec3 world = push.origin_time.xyz + local;
    vec3 pushed = camera.position.xyz + (world - camera.position.xyz) * 1.004;
    vec3 color = vec3(float(b & 255u), float((b >> 8u) & 255u), float((b >> 16u) & 255u)) / 255.0;
    v_world_position = world;
    v_uv = vec2(float((a >> 18u) & 1u), float((a >> 19u) & 1u));
    // x cara (+16 = lejano, +32 = agua), y rugosidad 0..15.
    v_face_layer = uvec2(face | 16u | (((b >> 28u) & 1u) != 0u ? 32u : 0u), (b >> 24u) & 15u);
    v_box_uv = vec2(dot(local, kFaceU[face]), dot(local, kFaceV[face]));
    v_light = vec3(float((a >> 20u) & 3u) / 3.0, 1.0, 0.0);
    v_tint = color;
    v_to_camera = camera.position.xyz - world;
    v_current_clip = camera.unjittered_view_projection * vec4(pushed, 1.0);
    v_previous_clip = camera.previous_view_projection * vec4(pushed, 1.0);
    gl_Position = push.shadow != 0u ? push.light_view_projection * vec4(world, 1.0) : camera.view_projection * vec4(pushed, 1.0);
}

void main() {
    uint a = in_packed.x;
    uint b = in_packed.y;
    if (farSection()) {
        farVertex(a, b);
        return;
    }
    vec3 local = vec3(float(a & 31u), float((a >> 5u) & 31u), float((a >> 10u) & 31u));
    uint face = (a >> 15u) & 7u;
    vec2 uv = vec2(float((a >> 18u) & 1u), float((a >> 19u) & 1u));
    float corner_ao = float((a >> 20u) & 3u) / 3.0;
    bool waves = ((a >> 22u) & 1u) != 0u;
    uint layer = b & 1023u;
    float sky = float((b >> 10u) & 15u) / 15.0;
    float torch = float((b >> 14u) & 15u) / 15.0;
    vec3 tint = vec3(float((b >> 18u) & 15u), float((b >> 22u) & 15u), float((b >> 26u) & 15u)) / 15.0;
    // Vertice fino (vallas, faroles; Mesher.cpp emitBox): desplazamiento en
    // dieciseisavos en lugar de uv y oclusion.
    bool fine = ((b >> 30u) & 1u) != 0u;
    if (fine) {
        local += vec3(float((a >> 23u) & 15u), float((a >> 27u) & 15u), float((a >> 18u) & 15u)) / 16.0;
        // HOJAS: bit 31 en un vertice fino = la parte entera va +1 (piezas que sobresalen de la seccion: Mesher.cpp)
        if (((b >> 31u) & 1u) != 0u) local -= vec3(1.0);
        corner_ao = 1.0;
        uv = vec2(0.0);
    }
    // PASTO-HZ: brizna de una mata (Mesher.cpp emitTuft; cara 7, se dibuja como planta): octavos en x (a 27-29) y
    // z (a 30-31 + b 31) y la punta baja (a 20-21) * 3/16. La base mas oscura (sombra dentro de la mata).
    bool blade = face == 7u;
    if (blade) {
        local.x += float((a >> 27u) & 7u) / 8.0;
        local.z += float(((a >> 30u) & 3u) | (((b >> 31u) & 1u) << 2u)) / 8.0;
        local.y -= float((a >> 20u) & 3u) * (3.0 / 16.0);
        corner_ao = 1.0 - 0.25 * uv.y;
        face = 6u;
    }

    vec3 world = push.origin_time.xyz + local;
    vec3 previous_world = world;
    if (waves) {
        // Viento: las plantas se mueven por arriba (v = 0), las hojas enteras y poco.
        // CUERDA: en las plantas (cara 6) los bits 23-26 dicen a que altura de la planta esta el vertice (Mesher.cpp):
        // el peso crece con esa altura y la fase no depende de la y, asi una cana o un alga de varios bloques se
        // dobla entera como una cuerda (antes cada bloque se mecia por su cuenta con su base quieta). Un poco de
        // retraso de fase por altura: la onda sube por la planta.
        float level = face == 6u && !fine ? float((a >> 23u) & 15u) : 0.0;
        float phase = face == 6u ? dot(world.xz, vec2(0.37, 0.29)) - level * 0.22 : dot(world.xz, vec2(0.37, 0.29)) + world.y * 0.21;
        float weight = face == 6u ? 1.6 * pow(level, 0.8) : 0.45;
        world += windSway(push.origin_time.w, phase, weight);
        // Donde estaba el frame anterior: sin esto el vector de movimiento
        // solo veia la camara y el TAA emborronaba las hojas al mecerse.
        previous_world += windSway(push.previous_time, phase, weight);
        // Personajes cerca: las plantas se apartan por arriba (la base fija);
        // las hojas (arbustos a ras de suelo) un poco, enteras. Se calcula con
        // la posicion sin viento, igual en los dos frames: cada esquina
        // compartida se mueve igual (sin grietas).
        vec3 rest = push.origin_time.xyz + local;
        if (interactors.count.x + interactors.count.y > 0u && all(greaterThanEqual(rest, interactors.box_min.xyz)) &&
            all(lessThanEqual(rest, interactors.box_max.xyz))) {
            float bend = face == 6u ? min(level, 1.0) + 0.3 * max(min(level, 4.0) - 1.0, 0.0) : 0.3;  // CUERDA
            world += foliagePush(rest, false) * bend;
            previous_world += foliagePush(rest, true) * bend;
        }
    }

    v_world_position = world;
    v_uv = uv;
    // Cara fusionada (Mesher.cpp, bit 31): un rectangulo de varios bloques; uv desde la posicion (+64).
    bool planar = !fine && !blade && ((b >> 31u) & 1u) != 0u;  // HOJAS: en los finos el bit 31 es el desplazamiento (PASTO-HZ: en las briznas, z)
    v_face_layer = uvec2(face | (fine ? 8u : 0u) | (planar ? 64u : 0u), layer);
    v_box_uv = vec2(dot(local, kFaceU[face]), dot(local, kFaceV[face]));
    v_light = vec3(corner_ao, sky, torch);
    v_tint = tint;
    v_to_camera = camera.position.xyz - world;
    v_current_clip = camera.unjittered_view_projection * vec4(world, 1.0);
    v_previous_clip = camera.previous_view_projection * vec4(previous_world, 1.0);
    gl_Position = push.shadow != 0u ? push.light_view_projection * vec4(shadowCaster(world, face, fine), 1.0)
                                    : camera.view_projection * vec4(world, 1.0);
}
