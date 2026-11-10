// Ocupacion de los bloques para los rayos (VoxelOccupancy.h). Los voxeles no
// estan en la TLAS: los rayos (sombras, GI, reflejos, sondas, path tracing)
// recorren ademas esta rejilla de celdas de 1 m alrededor de la camara con un
// DDA en tres niveles (un ladrillo de 16^3 vacio o no cargado, o un bloque de
// 4^3 vacio, se salta entero).
// Una celda llena tapa venga el rayo de donde venga (por las dos caras, como
// las sombras de Unreal): el mallado solo tiene la cara que da al aire, pero
// aqui el bloque es solido.
//
// Set 0: binding 11 = cabecera + etiquetas de los huecos, 12 = celdas (1 byte
// cada una: 0 aire, 1..252 color sRGB en 6 x 7 x 6 niveles).

// Etiqueta de un hueco (VoxelOccupancy::Tag): ladrillo absoluto (xyz), w = 1
// si esta cargado; mask = sus 64 bloques de 4^3 con alguna celda llena.
struct VoxelSlotTag {
    ivec4 brick;
    uvec4 mask;  // xy
};

layout(std430, set = 0, binding = 11) readonly buffer VoxelGridInfo {
    ivec4 anchor;        // xyz: celda absoluta de la esquina de la ventana; w: 1 = activa
    vec4 anchor_world;   // xyz: esa esquina en el mundo (origen flotante)
    ivec4 dims;          // xyz: ladrillos por eje
    ivec4 window_min;    // xyz: primer ladrillo absoluto de la ventana
    VoxelSlotTag tags[];
} voxel_grid;
layout(std430, set = 0, binding = 12) readonly buffer VoxelGridCells { uint voxel_cells[]; };

// Pasos como mucho (celdas y saltos de bloques de 4^3 o de 16^3 vacios) y
// alcance. La media es de ~30 pasos, pero un rayo casi rasante por la corteza
// del terreno avanza celda a celda y en un warp manda el mas largo: sin tope,
// la GI y las sombras por rayos pasaban de ~1 ms a decenas. Mas alla, los
// bloques los siguen tapando las cascadas del sol y la sonda (lighting.frag se
// queda con la mas oscura).
const int kVoxelMaxSteps = 32;
const float kVoxelMaxDistance = 40.0;

bool voxelGridActive() {
    return voxel_grid.anchor.w != 0;
}

// Hueco del ladrillo `local` (0..dims-1 en la ventana), o -1 si no esta cargado.
int voxelSlot(ivec3 local) {
    ivec3 brick = voxel_grid.window_min.xyz + local;
    ivec3 d = voxel_grid.dims.xyz;
    ivec3 m = brick - d * ivec3(floor(vec3(brick) / vec3(d)));
    int slot = m.x + d.x * (m.y + d.y * m.z);
    ivec4 tag = voxel_grid.tags[slot].brick;
    return (tag.w != 0 && all(equal(tag.xyz, brick))) ? slot : -1;
}

uint voxelCell(int slot, ivec3 cell) {
    uint index = uint(cell.x + 16 * (cell.y + 16 * cell.z));
    uint word = voxel_cells[uint(slot) * 1024u + (index >> 2u)];
    return (word >> ((index & 3u) * 8u)) & 255u;
}

// TESELADO: 1 si la celda del punto `world` esta llena, 0 si es aire, -1 fuera de la ventana o sin cargar.
int voxelFilledAt(vec3 world) {
    ivec3 cell = ivec3(floor(world - voxel_grid.anchor_world.xyz));
    ivec3 cells = voxel_grid.dims.xyz * 16;
    if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, cells))) return -1;
    int slot = voxelSlot(cell >> 4);
    if (slot < 0) return -1;
    return voxelCell(slot, cell & 15) != 0u ? 1 : 0;
}

// TESELADO: el relieve teselado de los bloques (voxel.tese) hunde la superficie hasta ~0.12 m dentro de su
// bloque; un rayo que saliera de ahi empezaria dentro de una celda llena y se taparia a si mismo (voxelTrace: 0).
// Si el origen esta dentro de una celda llena a poca distancia (kVoxelLiftMax) de su cara hacia `normal` (eje
// dominante) y la celda de al lado es aire, sale a esa cara (+1 cm). Lo demas, tal cual.
const float kVoxelLiftMax = 0.2;
vec3 voxelLiftOrigin(vec3 origin, vec3 normal) {
    if (!voxelGridActive() || voxelFilledAt(origin) != 1) return origin;
    vec3 a = abs(normal);
    int axis = (a.x >= a.y && a.x >= a.z) ? 0 : (a.y >= a.z ? 1 : 2);
    float s = normal[axis] >= 0.0 ? 1.0 : -1.0;
    float local = origin[axis] - voxel_grid.anchor_world[axis];
    float boundary = s > 0.0 ? floor(local) + 1.0 : floor(local);
    if (abs(boundary - local) > kVoxelLiftMax) return origin;
    vec3 lifted = origin;
    lifted[axis] = voxel_grid.anchor_world[axis] + boundary + s * 0.01;
    return voxelFilledAt(lifted) == 0 ? lifted : origin;
}

// Color (lineal) de un codigo de celda.
vec3 voxelAlbedo(uint code) {
    uint i = max(code, 1u) - 1u;
    vec3 srgb = vec3(float(i % 6u) / 5.0, float((i / 6u) % 7u) / 6.0, float(i / 42u) / 5.0);
    return pow(srgb, vec3(2.2));
}

// Primer bloque que corta el rayo en [0, max_distance]: su distancia (-1 =
// ninguno), la normal de la cara por la que entra (hacia el rayo) y su codigo
// de color. Si el origen ya esta dentro de un bloque, distancia 0 y la normal
// contra el rayo. DDA por celdas que salta de una vez los ladrillos de 16^3
// vacios (o sin cargar) y los bloques de 4^3 vacios de los cargados.
float voxelTrace(vec3 origin, vec3 direction, float max_distance, out vec3 normal, out uint code) {
    normal = -direction;
    code = 0u;
    if (voxel_grid.anchor.w == 0 || max_distance <= 0.0) return -1.0;
    max_distance = min(max_distance, kVoxelMaxDistance);
    vec3 p = origin - voxel_grid.anchor_world.xyz;
    ivec3 cells = voxel_grid.dims.xyz * 16;
    vec3 size = vec3(cells);
    // Sin componentes nulas (1/0): un rayo exactamente paralelo a un eje.
    vec3 dir = direction;
    dir.x = abs(dir.x) < 1e-8 ? 1e-8 : dir.x;
    dir.y = abs(dir.y) < 1e-8 ? 1e-8 : dir.y;
    dir.z = abs(dir.z) < 1e-8 ? 1e-8 : dir.z;
    vec3 inv = 1.0 / dir;
    vec3 t_a = -p * inv;
    vec3 t_b = (size - p) * inv;
    vec3 t_near = min(t_a, t_b);
    vec3 t_far = max(t_a, t_b);
    float t_enter = max(max(t_near.x, t_near.y), max(t_near.z, 0.0));
    float t_exit = min(min(t_far.x, t_far.y), min(t_far.z, max_distance));
    if (t_enter >= t_exit) return -1.0;

    ivec3 step_dir = ivec3(sign(dir));
    vec3 positive = vec3(greaterThan(step_dir, ivec3(0)));
    vec3 delta = abs(inv);
    float t = t_enter;
    // Eje por el que entra (-1: el origen esta dentro de la ventana).
    int axis = -1;
    if (t_enter > 0.0) axis = t_near.x >= t_near.y && t_near.x >= t_near.z ? 0 : (t_near.y >= t_near.z ? 1 : 2);
    ivec3 cell = clamp(ivec3(floor(p + dir * t)), ivec3(0), cells - 1);
    vec3 next = (vec3(cell) + positive - p) * inv;
    ivec3 cached_brick = ivec3(-1);
    int slot = -1;

    for (int i = 0; i < kVoxelMaxSteps; ++i) {
        ivec3 brick = cell >> 4;
        if (any(notEqual(brick, cached_brick))) {
            cached_brick = brick;
            slot = voxelSlot(brick);
        }
        // Lo que se salta entero: el ladrillo (vacio o sin cargar) o su bloque de 4^3 vacio.
        int skip = 0;
        ivec3 base = ivec3(0);
        if (slot < 0) {
            skip = 16;
            base = brick * 16;
        } else {
            ivec3 block = (cell & 15) >> 2;
            uint bit = uint(block.x + 4 * (block.y + 4 * block.z));
            uvec4 mask = voxel_grid.tags[slot].mask;
            uint word = bit < 32u ? mask.x : mask.y;
            if ((word & (1u << (bit & 31u))) == 0u) {
                skip = 4;
                base = brick * 16 + block * 4;
            }
        }
        if (skip > 0) {
            // A la salida de esa caja de un salto (nunca hacia atras: con el
            // redondeo la celda puede no ser la del punto exacto).
            vec3 box_next = (vec3(base) + positive * float(skip) - p) * inv;
            float t_out = max(min(min(box_next.x, box_next.y), box_next.z), t);
            if (t_out >= t_exit) return -1.0;
            axis = t_out == box_next.x ? 0 : (t_out == box_next.y ? 1 : 2);
            t = t_out;
            // La celda justo pasado el borde.
            cell = ivec3(floor(p + dir * (t + 1e-3)));
            if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, cells))) return -1.0;
            next = (vec3(cell) + positive - p) * inv;
            continue;
        }
        uint value = voxelCell(slot, cell & 15);
        if (value != 0u) {
            code = value;
            if (axis >= 0) {
                normal = vec3(0.0);
                normal[axis] = -float(step_dir[axis]);
            }
            return t;
        }
        // A la celda siguiente.
        axis = next.x < next.y ? (next.x < next.z ? 0 : 2) : (next.y < next.z ? 1 : 2);
        t = max(next[axis], t);
        if (t >= t_exit) return -1.0;
        cell[axis] += step_dir[axis];
        next[axis] += delta[axis];
        if (cell[axis] < 0 || cell[axis] >= cells[axis]) return -1.0;
    }
    return -1.0;
}

// Rayo de sombra hacia una luz: la distancia al primer bloque que tapa (-1 =
// ninguno). Un bloque que contiene el final del rayo no cuenta: es el de la
// propia luz (una luz de bloque, p. ej. una lampara, esta en el centro de su
// celda) y no hay nada antes que tape.
float voxelShadowDistance(vec3 origin, vec3 direction, float max_distance) {
    vec3 normal;
    uint code;
    float t = voxelTrace(origin, direction, max_distance, normal, code);
    if (t < 0.0) return -1.0;
    vec3 corner = voxel_grid.anchor_world.xyz;
    ivec3 hit_cell = ivec3(floor(origin + direction * (t + 1e-3) - corner));
    ivec3 end_cell = ivec3(floor(origin + direction * max_distance - corner));
    return all(equal(hit_cell, end_cell)) ? -1.0 : t;
}

// Solo si algo tapa (rayo de sombra).
bool voxelOccluded(vec3 origin, vec3 direction, float max_distance) {
    return voxelShadowDistance(origin, direction, max_distance) >= 0.0;
}
