#version 450
#extension GL_GOOGLE_include_directive : require

// VFX Graph: dibujo indirecto de las vivas de un efecto (una instancia por
// viva). Tres salidas (VfxInstance.extra.x):
//   Quads   6 vertices por particula: el quad mira a la camara, se estira con
//           la velocidad, queda horizontal o vertical.
//   Cinta   6 vertices por tramo (1..16 tramos por segmento) entre la viva i y
//           la i+1 (vfx_sim.comp las ordena por (id, edad)): una tira
//           continua detras del emisor con ancho (tamano) y color a lo largo
//           de la vida, de cara a la camara, al eje del emisor al nacer o a
//           un vector, con giro, y la U estirada, por distancia o por edad.
//   Malla   los indices de su malla por particula (vertex pulling: sin
//           buffers de vertices propios), girada y escalada.

#include "vfx_common.glsl"

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;        // textura (cuadro del flipbook)
layout(location = 2) out vec2 v_corner;    // -1..1 dentro del quad (disco); cinta: (0, lado)
layout(location = 3) out float v_view_depth;
layout(location = 4) out vec3 v_normal;     // malla: normal en el mundo
layout(location = 5) flat out uint v_mode;  // 0 quad, 1 cinta estirada, 2 cinta por longitud, 3 malla
// Flipbook fundido (VFX_FLIP_BLEND): xy la UV del cuadro siguiente, z cuanto
// de el (la fraccion del indice).
layout(location = 6) out vec3 v_flip;

const vec2 kCorners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0),
                                 vec2(-1.0, 1.0));
// Cinta: de que punto del segmento (0 este, 1 el siguiente) y de que lado
// sale cada vertice de los dos triangulos.
const uint kRibbonEnd[6] = uint[6](0u, 1u, 1u, 0u, 1u, 0u);
const float kRibbonSide[6] = float[6](-1.0, -1.0, 1.0, -1.0, 1.0, 1.0);

// Lo que se ve de una particula: posicion y velocidad en el mundo, color y
// tamano con lo que cambian en la vida (y por la velocidad).
void particleLook(uint slot, uint p, out vec3 world, out vec3 velocity, out vec4 color, out float size) {
    vec4 P0 = particles[p * 4u + 0u];
    vec4 P1 = particles[p * 4u + 1u];
    vec4 P2 = particles[p * 4u + 2u];
    vec4 P3 = particles[p * 4u + 3u];
    uint bits = instances[slot].flags.z;
    float t = P1.w > 0.0 ? clamp(P0.w / P1.w, 0.0, 1.0) : 1.0;
    size = P3.x;
    if ((bits & VFX_SIZE_OVER_LIFE) != 0u) size *= vfxSizeLut(slot, t);
    color = P2;
    if ((bits & VFX_COLOR_OVER_LIFE) != 0u) color *= vfxColorLut(slot, t);
    world = (instances[slot].transform * vec4(P0.xyz, 1.0)).xyz;
    velocity = mat3(instances[slot].transform) * P1.xyz;
    if ((bits & VFX_COLOR_BY_SPEED) != 0u) {
        vec4 range = instances[slot].speed_range;
        float s = clamp((length(velocity) - range.x) / max(range.y - range.x, 1e-4), 0.0, 1.0);
        color *= mix(instances[slot].speed_slow, instances[slot].speed_fast, s);
    }
}

// Fuera de la vista (segmento que no existe o cortado): los dos triangulos
// se descartan al recortar.
void degenerate(uint mode) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    v_color = vec4(0.0);
    v_uv = vec2(0.0);
    v_corner = vec2(0.0);
    v_view_depth = 0.0;
    v_normal = vec3(0.0, 1.0, 0.0);
    v_mode = mode;
    v_flip = vec3(0.0);
}

// Cinta: si dos vivas seguidas de la lista ordenada van unidas: de la misma
// cinta (id) y nacidas a menos de output3.x segundos (el corte: el emisor
// dejo de emitir y volvio a empezar).
bool ribbonLinked(uint slot, uint a, uint b) {
    float gap = instances[slot].output3.x;
    if (gap > 0.0 && abs(particles[a * 4u].w - particles[b * 4u].w) > gap) return false;
    if ((instances[slot].flags.z & VFX_RIBBON_IDS) != 0u &&
        vfxRibbonId(particles[a * 4u + 3u].y) != vfxRibbonId(particles[b * 4u + 3u].y)) {
        return false;
    }
    return true;
}

vec3 ribbonWorld(uint slot, uint p) {
    return (instances[slot].transform * vec4(particles[p * 4u].xyz, 1.0)).xyz;
}

vec3 safeNormalize(vec3 v, vec3 fallback) {
    return dot(v, v) > 1e-12 ? normalize(v) : fallback;
}

// Cinta con suavizado: el segmento entre la viva i y la i+1 en
// ribbon0.y tramos (6 vertices cada uno) por una curva de Hermite con las
// tangentes de Catmull-Rom (tension 0) a rectas (tension 1). Ancho, color y
// U se interpolan entre los dos puntos; las juntas usan la misma tangente
// en los dos segmentos (sin grietas en los giros de 90 grados).
void ribbon(uint slot) {
    uint offset = instances[slot].range.x;
    uint alive = uint(max(counters[instances[slot].range.z * 8u + 1u], 0));
    uint segment = uint(gl_InstanceIndex);
    uint corner = uint(gl_VertexIndex) % 6u;
    uint piece = uint(gl_VertexIndex) / 6u;
    bool ids = (instances[slot].flags.z & VFX_RIBBON_IDS) != 0u;
    // UV: 0 estirada (orden en la lista), 1 por distancia, 2 por edad. Con
    // varias cintas la lista no va de una sola cabeza a su cola: por edad.
    uint uv_mode = uint(instances[slot].output2.w + 0.5);
    if (uv_mode == 0u && ids) uv_mode = 2u;
    vec4 uv_transform = instances[slot].ribbon1;
    float cell = instances[slot].ribbon0.z;
    bool wrap = uv_mode == 1u || cell >= 0.0 || uv_transform != vec4(1.0, 1.0, 0.0, 0.0);
    uint mode = wrap ? 2u : 1u;
    // La ultima viva no tiene siguiente: su instancia no dibuja nada.
    if (segment + 1u >= alive) {
        degenerate(mode);
        return;
    }
    uint a = alive_list[offset + segment];
    uint b = alive_list[offset + segment + 1u];
    if (!ribbonLinked(slot, a, b)) {
        degenerate(mode);
        return;
    }
    // Vecinos de la misma cinta (en un extremo, el propio punto).
    uint before = a;
    if (segment > 0u) {
        uint c = alive_list[offset + segment - 1u];
        if (ribbonLinked(slot, c, a)) before = c;
    }
    uint after = b;
    if (segment + 2u < alive) {
        uint c = alive_list[offset + segment + 2u];
        if (ribbonLinked(slot, b, c)) after = c;
    }

    vec3 wa;
    vec3 wb;
    vec3 velocity;
    vec4 ca;
    vec4 cb;
    float sa;
    float sb;
    particleLook(slot, a, wa, velocity, ca, sa);
    particleLook(slot, b, wb, velocity, cb, sb);
    vec3 ta = wb - ribbonWorld(slot, before);
    vec3 tb = ribbonWorld(slot, after) - wa;
    vec3 chord = safeNormalize(wb - wa, frame.up.xyz);
    vec3 da = safeNormalize(ta, chord);
    vec3 db = safeNormalize(tb, chord);

    // Tramos: fijos o, adaptativos, segun lo que gira el segmento (90 grados
    // o mas = todos; recto = uno, el resto se descarta).
    float pieces = float(clamp(int(abs(instances[slot].ribbon0.y) + 0.5), 1, 16));
    if (instances[slot].ribbon0.y < 0.0 && pieces > 1.0) {
        float turn = acos(clamp(dot(da, db), -1.0, 1.0));
        pieces = clamp(ceil(pieces * turn / 1.5707963), 1.0, pieces);
    }
    if (float(piece) >= pieces) {
        degenerate(mode);
        return;
    }
    float t = (float(piece) + float(kRibbonEnd[corner])) / pieces;

    // Hermite (en t = 0 y t = 1, exactamente los puntos: sin suavizar es la
    // cinta de siempre).
    float tension = clamp(instances[slot].ribbon0.x, 0.0, 1.0);
    vec3 m0 = ta * (0.5 * (1.0 - tension));
    vec3 m1 = tb * (0.5 * (1.0 - tension));
    float t2 = t * t;
    float t3 = t2 * t;
    vec3 center = (2.0 * t3 - 3.0 * t2 + 1.0) * wa + (t3 - 2.0 * t2 + t) * m0 + (-2.0 * t3 + 3.0 * t2) * wb +
                  (t3 - t2) * m1;
    vec3 tangent = safeNormalize(mix(da, db, t), chord);
    vec4 color = mix(ca, cb, t);
    float size = mix(sa, sb, t);
    vec4 Pa3 = particles[a * 4u + 3u];
    vec4 Pb3 = particles[b * 4u + 3u];

    // Hacia donde se abre el ancho.
    uint facing = uint(instances[slot].output2.z + 0.5);
    vec3 custom = mat3(instances[slot].local_to_world) * instances[slot].output3.yzw;
    vec3 side;
    if (facing == 1u) {
        // De cara al emisor: su eje Y cuando nacio cada punto (espadas, alas).
        side = safeNormalize(mix(vfxRibbonAxis(Pa3.y), vfxRibbonAxis(Pb3.y), t), vfxRibbonAxis(Pa3.y));
    } else if (facing == 3u) {
        // CustomSide: el ancho por el vector (quitada la parte de la tangente).
        side = safeNormalize(custom - tangent * dot(custom, tangent), safeNormalize(custom, frame.right.xyz));
    } else {
        // De cara a la camara o al vector (CustomFacing): tangente x direccion.
        vec3 toward;
        if (facing == 2u) {
            toward = safeNormalize(custom, vec3(0.0, 1.0, 0.0));
        } else {
            toward = safeNormalize(frame.camera.xyz - center, vec3(0.0, 0.0, 1.0));
        }
        side = safeNormalize(cross(tangent, toward), frame.right.xyz);
    }
    // Giro de la cinta (RibbonAttributes) alrededor de la tangente.
    float twist = mix(Pa3.z, Pb3.z, t);
    if (twist != 0.0) side = side * cos(twist) + cross(tangent, side) * sin(twist);

    float s = kRibbonSide[corner];
    vec3 position = center + side * (s * size * 0.5);
    gl_Position = frame.view_projection * vec4(position, 1.0);
    v_view_depth = abs((frame.view * vec4(position, 1.0)).z);

    // U: de la cabeza (0) a la cola (1) por la lista, los metros que llevaba
    // el emisor al nacer cada punto (la textura queda pegada a la tira: no
    // resbala) o la edad / vida de cada punto. La de la CPU da la vuelta cada
    // 1024 repeticiones: el segundo punto se corrige respecto al primero para
    // no barrer la textura entera en un segmento.
    float ua;
    float ub;
    if (uv_mode == 1u) {
        ua = Pa3.w;
        ub = Pb3.w;
        if (ub - ua > 512.0) ub -= 1024.0;
        if (ua - ub > 512.0) ub += 1024.0;
    } else if (uv_mode == 2u) {
        float la = particles[a * 4u + 1u].w;
        float lb = particles[b * 4u + 1u].w;
        ua = la > 0.0 ? clamp(particles[a * 4u].w / la, 0.0, 1.0) : 1.0;
        ub = lb > 0.0 ? clamp(particles[b * 4u].w / lb, 0.0, 1.0) : 1.0;
    } else {
        float last = float(max(alive - 1u, 1u));
        ua = float(segment) / last;
        ub = float(segment + 1u) / last;
    }
    v_uv = vec2(mix(ua, ub, t), s * 0.5 + 0.5) * uv_transform.xy + uv_transform.zw;
    v_corner = vec2(0.0, s);
    v_color = color;
    v_normal = vec3(0.0, 1.0, 0.0);
    v_mode = mode;
    v_flip = vec3(0.0);
}

// UV de un cuadro del flipbook (columnas x filas). Con enteros: el indice /
// columnas en coma flotante puede quedarse justo por debajo en los multiplos
// exactos (una fila menos y la columna "cols": un cuadro equivocado).
vec2 flipbookUv(float index, float cols, float rows, vec2 corner_uv) {
    uint frame_count = max(uint(cols * rows + 0.5), 1u);
    uint frame_index = uint(index + 0.5) % frame_count;
    uint column_count = max(uint(cols + 0.5), 1u);
    vec2 cell = vec2(float(frame_index % column_count), float(frame_index / column_count));
    return (cell + corner_uv) / vec2(cols, rows);
}

// Cuaternion (xyzw) -> matriz de giro (columnas: los ejes girados).
mat3 quatToMat(vec4 q) {
    q = dot(q, q) > 1e-8 ? normalize(q) : vec4(0.0, 0.0, 0.0, 1.0);
    float x = q.x, y = q.y, z = q.z, w = q.w;
    return mat3(1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y + z * w), 2.0 * (x * z - y * w),
                2.0 * (x * y - z * w), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z + x * w),
                2.0 * (x * z + y * w), 2.0 * (y * z - x * w), 1.0 - 2.0 * (x * x + y * y));
}

// Giro de `angle` alrededor de un eje unitario.
mat3 axisAngle(vec3 axis, float angle) {
    float c = cos(angle);
    float s = sin(angle);
    float t = 1.0 - c;
    return mat3(t * axis.x * axis.x + c, t * axis.x * axis.y + s * axis.z, t * axis.x * axis.z - s * axis.y,
                t * axis.x * axis.y - s * axis.z, t * axis.y * axis.y + c, t * axis.y * axis.z + s * axis.x,
                t * axis.x * axis.z + s * axis.y, t * axis.y * axis.z - s * axis.x, t * axis.z * axis.z + c);
}

void meshParticle(uint slot) {
    uint p = alive_list[instances[slot].range.x + uint(gl_InstanceIndex)];
    vec3 world;
    vec3 velocity;
    vec4 color;
    float size;
    particleLook(slot, p, world, velocity, color, size);
    float angle = particles[p * 4u + 3u].y;
    float dist = length(world - frame.camera.xyz);
    vec3 scale = instances[slot].output3.xyz * size;
    // Escala al azar por eje (de su hueco en el pool, como el giro al azar).
    vec3 jitter = instances[slot].mesh_jitter.xyz;
    if (dot(jitter, jitter) > 0.0) {
        vec3 r = vec3(uvec3(vfxHash(p * 5u + 7u), vfxHash(p * 5u + 11u), vfxHash(p * 5u + 13u))) * (1.0 / 4294967296.0);
        scale *= max(vec3(1.0) + jitter * (r * 2.0 - 1.0), vec3(0.0));
    }

    // Lejos (mesh0.z > 0): un quad de cara a la camara con los 12 primeros
    // vertices (el segundo, con la otra cara: el que no mira se recorta).
    float sprite_distance = instances[slot].mesh0.z;
    if (sprite_distance > 0.0 && dist > sprite_distance) {
        uint v = uint(gl_VertexIndex);
        if (v >= 12u) {
            degenerate(0u);
            return;
        }
        vec2 corner = kCorners[v % 6u];
        if (v >= 6u) corner.x = -corner.x;
        float c = cos(angle);
        float s = sin(angle);
        vec3 r = frame.right.xyz * c + frame.up.xyz * s;
        vec3 u = -frame.right.xyz * s + frame.up.xyz * c;
        float half_size = max(max(abs(scale.x), abs(scale.y)), abs(scale.z)) * 0.5;
        vec3 position = world + (r * corner.x + u * corner.y) * half_size;
        gl_Position = frame.view_projection * vec4(position, 1.0);
        v_view_depth = abs((frame.view * vec4(position, 1.0)).z);
        v_uv = vec2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5);
        v_corner = corner;
        v_color = color;
        vec3 to_camera = frame.camera.xyz - world;
        v_normal = dot(to_camera, to_camera) > 1e-10 ? normalize(to_camera) : vec3(0.0, 0.0, 1.0);
        v_mode = 0u;
        v_flip = vec3(0.0);
        return;
    }

    // Variante: la de su distancia (un nivel cada mesh0.y m) o la de la
    // particula (bloque MeshIndex). Sobran vertices en las mas pequenas.
    uint variants = clamp(uint(instances[slot].mesh0.x + 0.5), 1u, 4u);
    uint variant = 0u;
    if (variants > 1u) {
        float lod_step = instances[slot].mesh0.y;
        variant = lod_step > 0.0 ? min(uint(dist / lod_step), variants - 1u)
                                 : uint(max(particles[p * 4u + 3u].w, 0.0)) % variants;
    }
    if (uint(gl_VertexIndex) >= instances[slot].variant_count[variant]) {
        degenerate(3u);
        return;
    }
    // El vertice: indices ya con el primer vertice de su malla.
    uint index = mesh_indices[instances[slot].variant_first[variant] + uint(gl_VertexIndex)];
    vec4 A = mesh_vertices[index * 2u];
    vec4 B = mesh_vertices[index * 2u + 1u];

    mat3 rotation;
    uint orient = uint(instances[slot].output2.z + 0.5);
    if (orient == 1u && dot(velocity, velocity) > 1e-8) {
        // Su +Z sigue la velocidad (proyectiles, casquillos en vuelo) y el giro
        // de la particula es el alabeo.
        vec3 forward = normalize(velocity);
        vec3 up = abs(forward.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        vec3 right = normalize(cross(up, forward));
        up = cross(forward, right);
        float c = cos(angle);
        float s = sin(angle);
        rotation = mat3(right * c + up * s, -right * s + up * c, forward);
    } else if (orient == 2u) {
        // Fija: el cuaternion de la salida (identidad = la de antes) y el giro
        // de la particula alrededor de su Y.
        rotation = quatToMat(instances[slot].mesh_rotation) * axisAngle(vec3(0.0, 1.0, 0.0), angle);
    } else if (orient == 3u) {
        // Hacia la camara: su +Z mira a la camara y el giro es el alabeo.
        vec3 to_camera = frame.camera.xyz - world;
        vec3 forward = dot(to_camera, to_camera) > 1e-8 ? normalize(to_camera) : vec3(0.0, 0.0, 1.0);
        vec3 right = cross(frame.up.xyz, forward);
        right = dot(right, right) > 1e-8 ? normalize(right) : frame.right.xyz;
        vec3 up = cross(forward, right);
        float c = cos(angle);
        float s = sin(angle);
        rotation = mat3(right * c + up * s, -right * s + up * c, forward);
    } else {
        // Al azar: un eje y un angulo de partida por particula (de su hueco en
        // el pool: no cambian en su vida) mas el giro de la particula.
        uvec3 h = uvec3(vfxHash(p * 3u + 1u), vfxHash(p * 3u + 2u), vfxHash(p * 3u + 3u));
        vec3 axis = vec3(h) * (2.0 / 4294967296.0) - 1.0;
        axis = dot(axis, axis) > 1e-6 ? normalize(axis) : vec3(0.0, 1.0, 0.0);
        rotation = axisAngle(axis, angle + float(h.x >> 8u) * (6.28318530718 / 16777216.0));
    }
    vec3 position = world + rotation * (A.xyz * scale);
    gl_Position = frame.view_projection * vec4(position, 1.0);
    v_view_depth = abs((frame.view * vec4(position, 1.0)).z);
    // La normal con la escala inversa (vale con escala no uniforme).
    vec3 safe_scale = vec3(abs(scale.x) > 1e-6 ? scale.x : 1e-6, abs(scale.y) > 1e-6 ? scale.y : 1e-6,
                           abs(scale.z) > 1e-6 ? scale.z : 1e-6);
    v_normal = normalize(rotation * (B.xyz / safe_scale));
    v_uv = vec2(A.w, B.w);
    v_corner = vec2(0.0);
    v_color = color;
    v_mode = 3u;
    v_flip = vec3(0.0);
}

void main() {
    uint slot = push.slot;
    uint output_kind = instances[slot].extra.x;
    if (output_kind == VFX_OUT_RIBBON) {
        ribbon(slot);
        return;
    }
    if (output_kind == VFX_OUT_MESH) {
        meshParticle(slot);
        return;
    }

    uint p = alive_list[instances[slot].range.x + uint(gl_InstanceIndex)];
    vec4 P0 = particles[p * 4u + 0u];
    vec4 P1 = particles[p * 4u + 1u];
    vec4 P3 = particles[p * 4u + 3u];
    float t = P1.w > 0.0 ? clamp(P0.w / P1.w, 0.0, 1.0) : 1.0;
    vec3 world;
    vec3 velocity;
    vec4 color;
    float size;
    particleLook(slot, p, world, velocity, color, size);

    vec2 corner = kCorners[gl_VertexIndex % 6];
    vec3 right = frame.right.xyz;
    vec3 up = frame.up.xyz;
    vec3 to_camera = frame.camera.xyz - world;
    vec3 view_dir = length(to_camera) > 1e-5 ? normalize(to_camera) : vec3(0.0, 0.0, 1.0);
    float c = cos(P3.y);
    float s = sin(P3.y);
    float half_size = size * 0.5;
    vec3 offset;
    uint orient = uint(instances[slot].output0.x + 0.5);
    if (orient == 1u && length(velocity) > 1e-3) {
        // Estirada: el eje largo sigue la velocidad (proyectada en la pantalla).
        vec3 along = velocity - view_dir * dot(velocity, view_dir);
        if (length(along) < 1e-4) along = up;
        along = normalize(along);
        vec3 side = normalize(cross(along, view_dir));
        float tail = length(velocity) * max(instances[slot].output1.x, 0.0);
        float half_length = half_size + tail * 0.5;
        // La cabeza en la particula, la estela detras.
        vec3 center = world - along * (tail * 0.5);
        offset = center - world + along * (corner.y * half_length) + side * (corner.x * half_size);
    } else if (orient == 2u) {
        // Horizontal (ondas en el suelo).
        vec3 ax = vec3(c, 0.0, s);
        vec3 az = vec3(-s, 0.0, c);
        offset = (ax * corner.x + az * corner.y) * half_size;
    } else if (orient == 3u) {
        // Vertical: gira solo alrededor de Y (arboles lejanos, llamas).
        vec3 flat_dir = vec3(view_dir.x, 0.0, view_dir.z);
        vec3 r = length(flat_dir) > 1e-4 ? normalize(cross(vec3(0.0, 1.0, 0.0), flat_dir)) : right;
        offset = (r * corner.x + vec3(0.0, 1.0, 0.0) * corner.y) * half_size;
    } else {
        vec3 r = right * c + up * s;
        vec3 u = -right * s + up * c;
        offset = (r * corner.x + u * corner.y) * half_size;
    }
    vec3 position = world + offset;
    gl_Position = frame.view_projection * vec4(position, 1.0);
    v_view_depth = abs((frame.view * vec4(position, 1.0)).z);

    // Flipbook: columnas x filas, por cuadros/s o a lo largo de la vida.
    vec2 uv = vec2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5);
    float cols = max(instances[slot].output1.y, 1.0);
    float rows = max(instances[slot].output1.z, 1.0);
    float frames = cols * rows;
    vec3 flip = vec3(0.0);
    if (frames > 1.0) {
        float fps = instances[slot].output1.w;
        // Cuadro inicial (la parte entera) y velocidad de esta particula (la
        // fraccion + 0.5; sin fraccion, 1x: los .crvfx de antes igual).
        float start = floor(P3.w);
        float speed = P3.w > start ? P3.w - start + 0.5 : 1.0;
        float f;  // indice continuo: la fraccion funde con el siguiente
        float current;
        float next;
        if (fps > 0.0) {
            f = start + P0.w * fps * speed;
            current = mod(floor(f), frames);
            next = mod(current + 1.0, frames);
        } else {
            f = start + t * frames * speed;
            current = min(floor(f), frames - 1.0);
            next = min(current + 1.0, frames - 1.0);
        }
        vec2 corner_uv = uv;
        uv = flipbookUv(current, cols, rows, corner_uv);
        if ((instances[slot].flags.z & VFX_FLIP_BLEND) != 0u) {
            flip = vec3(flipbookUv(next, cols, rows, corner_uv), next == current ? 0.0 : fract(f));
        }
    }
    v_uv = uv;
    v_flip = flip;
    v_corner = corner;
    v_color = color;
    // Normal de "esfera" para los quads iluminados: hacia la camara en el
    // centro y abierta hacia los bordes (el humo toma volumen con las luces).
    v_normal = normalize(offset / max(half_size, 1e-5) * 0.75 + view_dir);
    v_mode = 0u;
}
