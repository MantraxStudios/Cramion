// VFX Graph (VfxPass): datos compartidos por la simulacion (vfx_sim.comp) y
// el dibujo (vfx.vert / vfx.frag). MISMO orden que VfxPass.cpp.
//
// Antes de incluirlo, la simulacion define VFX_COMPUTE (puede escribir las
// particulas); el dibujo solo las lee.

#ifdef VFX_COMPUTE
#define VFX_ACCESS
#else
#define VFX_ACCESS readonly
#endif

const uint VFX_LUT = 32u;
const uint VFX_NO_TEXTURE = 0xFFFFFFFFu;

// Bits de VfxInstance.flags.z
const uint VFX_WORLD_SPACE = 1u;
const uint VFX_COLOR_OVER_LIFE = 2u;
const uint VFX_SIZE_OVER_LIFE = 4u;
const uint VFX_COLOR_BY_SPEED = 8u;
const uint VFX_LIT = 16u;
// Nacen en la posicion del emisor de su instante dentro del frame (estelas y
// cintas sin escalones con el emisor rapido; solo en el mundo).
const uint VFX_INTERPOLATE = 32u;
// Flipbook: funde el cuadro con el siguiente (indice fraccionario).
const uint VFX_FLIP_BLEND = 4096u;
// Cinta: varias por efecto (bloque RibbonAttributes): orden por (id, edad).
const uint VFX_RIBBON_IDS = 8192u;
// Turbulencia con el ruido rizado horneado (vfx_sim.comp, binding 24; 1 << 27).
const uint VFX_CURL_BAKED = 134217728u;

// Salida (VfxOutput en VfxPass.h): VfxInstance.extra.x.
const uint VFX_OUT_QUADS = 0u;
const uint VFX_OUT_RIBBON = 1u;
const uint VFX_OUT_MESH = 2u;
const uint VFX_NO_EVENTS = 0xFFFFFFFFu;
// Eventos de la GPU por lista y frame (VfxPass::kMaxEvents).
const uint VFX_MAX_EVENTS = 512u;

// Tipos de bloque (VfxBlockType en VfxPass.h).
const uint B_POSITION_SPHERE = 1u;
const uint B_POSITION_BOX = 2u;
const uint B_POSITION_CONE = 3u;
const uint B_POSITION_CIRCLE = 4u;
const uint B_POSITION_LINE = 5u;
const uint B_POSITION_MESH = 6u;
const uint B_VELOCITY_RANDOM = 10u;
const uint B_VELOCITY_FROM_SHAPE = 11u;
const uint B_LIFETIME = 12u;
const uint B_SIZE = 13u;
const uint B_COLOR = 14u;
const uint B_ROTATION = 15u;
const uint B_FLIPBOOK_FRAME = 16u;
const uint B_INHERIT_VELOCITY = 17u;
// a.x id, a.y tiras, a.z ancho (m), a.w giro min (grados); b.x giro max, b.yzw separacion;
// flags 1 = id del evento
const uint B_RIBBON_ATTRIBUTES = 48u;
const uint B_GRAVITY = 20u;
const uint B_WIND = 21u;
const uint B_DRAG = 22u;
const uint B_TURBULENCE = 23u;
const uint B_ATTRACTOR = 24u;
const uint B_VORTEX = 25u;
const uint B_COLLIDE_DEPTH = 26u;   // c.x altura del suelo, c.y 1 = suelo sin la vista
const uint B_COLLIDE_PLANE = 27u;   // flags 1 = en el mundo
const uint B_COLLIDE_SPHERE = 28u;
const uint B_KILL_BOX = 29u;        // flags 1 = matar fuera, 2 = en el mundo
const uint B_SPEED_LIMIT = 30u;
const uint B_CONFORM_SPHERE = 31u;
// Eventos: al morir / al chocar se apunta un evento en la lista de otro
// efecto (info.z = su ranura, info.w = su lista), que nace en EMIT_EVENTS.
const uint B_TRIGGER_DIE = 32u;      // a.x nacen por evento, a.y probabilidad, a.w velocidad heredada
const uint B_TRIGGER_COLLIDE = 33u;  // igual; a.z rapidez minima del choque (m/s)
// Canales de datos (eventos con nombre): los Emit* apuntan {tipo, posicion,
// velocidad, color, valor} en el canal info.z y los que escuchan
// (B_ON_GPU_EVENT, tras sus Update) nacen alli en EMIT_CHANNEL.
const uint B_EMIT_ON_DEATH = 60u;      // a color; b.x tipo, b.y valor, b.z probabilidad; flags 1 = por el color de la particula,
                                       // 2 = valor = id de la particula
const uint B_EMIT_ON_COLLISION = 61u;  // igual; b.w rapidez minima del choque (m/s)
const uint B_EMIT_EVERY = 62u;         // igual; b.w periodo (s)
const uint B_ON_GPU_EVENT = 63u;       // a.x tipo (< 0 todos), a.y por evento, a.z maximo por frame (0 = sin limite),
                                       // a.w velocidad heredada; flags 1 = color del evento, 2 = valor por el tamano
// Malla: variante de cada particula (Initialize; la guarda en P3.w) y
// velocidad de giro en Update (P3.z, rad/s).
const uint B_MESH_INDEX = 80u;        // a.x min, a.y max (enteros)
const uint B_ANGULAR_VELOCITY = 81u;  // a.x aceleracion (rad/s2), a.y rozamiento (1/s), a.z rad por m recorrido (0 = no rueda)
// Bit de flags.z: tiene bloques Emit* (sin el, UPDATE ni los mira).
const uint VFX_EMITS_CHANNEL = 1048576u;
// Bit de push.pad (el 0 es la paridad): alguien escucha un canal este frame.
const uint VFX_PAD_CHANNELS = 2u;
// MISMOS numeros que VfxPass::kMaxChannels, kMaxChannelEvents y kMaxListens.
const uint VFX_MAX_CHANNELS = 8u;
const uint VFX_MAX_CHANNEL_EVENTS = 2048u;
const uint VFX_MAX_LISTENS = 4u;
// En Counters, detras de las 256 ranuras x 8: 8 por canal ([0]/[1] eventos de
// cada paridad, [4..6] el dispatch indirecto de los que escuchan) y 4 por
// ranura (las que nacieron este frame por cada bloque de escucha).
const uint VFX_CHANNEL_COUNTERS = 2048u;
const uint VFX_LISTEN_COUNTERS = 2048u + 8u * 8u;
// En Events, detras de las listas de los subemisores (32 x 2 x 512 x 2 vec4):
// por canal, dos frames de VFX_MAX_CHANNEL_EVENTS, tres vec4 cada uno.
const uint VFX_CHANNEL_EVENTS = 65536u;

// Etapas de vfx_sim.comp.
const uint STAGE_RESET = 0u;
const uint STAGE_BEGIN = 1u;
const uint STAGE_EMIT = 2u;
const uint STAGE_UPDATE = 3u;
// Orden de las vivas de atras adelante (mezcla alfa): ordenacion bitonica.
const uint STAGE_SORT_LOCAL = 4u;           // bloques de 128 enteros (memoria compartida)
const uint STAGE_SORT_FLIP = 5u;            // paso "flip" de altura push.count
const uint STAGE_SORT_DISPERSE = 6u;        // paso "disperse" de altura push.count
const uint STAGE_SORT_LOCAL_DISPERSE = 7u;  // los disperse de 128 a 2 (memoria compartida)
// Nacen las de los eventos que otros efectos apuntaron el frame anterior.
const uint STAGE_EMIT_EVENTS = 8u;
// Nacen las de los canales que escucha (dispatch indirecto, push.count = el
// bloque de escucha).
const uint STAGE_EMIT_CHANNEL = 20u;

struct VfxBlock {
    uvec4 info;  // x tipo, y flags
    vec4 a;
    vec4 b;
    vec4 c;
};

struct VfxInstance {
    mat4 transform;          // espacio de la simulacion -> mundo (identidad si va en el mundo)
    mat4 local_to_world;     // el emisor (siempre)
    mat4 world_to_local;
    uvec4 range;             // x offset en el pool, y capacidad, z ranura (contadores), w nacen este frame
    uvec4 blocks;            // x primer Initialize, y cuantos, z primer Update, w cuantos
    vec4 time;               // x dt, y reloj del efecto
    uvec4 flags;             // x semilla, y frame, z bits VFX_*, w textura
    vec4 output0;            // x orientacion, y mezcla, z distancia suave (m), w intensidad
    vec4 output1;            // x estela (s), y columnas, z filas, w cuadros/s (0 = a lo largo de la vida)
    vec4 speed_slow;         // color con la velocidad minima
    vec4 speed_fast;         // con la maxima
    vec4 speed_range;        // x min, y max (m/s), z recorte de alfa
    vec4 shift;              // xyz desplazamiento del origen pendiente (mundo)
    vec4 emitter_velocity;   // xyz (mundo)
    vec4 color_lut[32];
    vec4 size_lut[8];        // 32 valores
    uvec4 extra;             // x salida (VFX_OUT_*), y primer indice de la malla, z indices, w lista de eventos
    vec4 output2;            // cinta: x metros por repeticion de la textura, y metros recorridos por el emisor,
                             // z cara (0 camara, 1 emisor, 2 hacia el vector, 3 lado = vector),
                             // w UV (0 estirada, 1 por longitud, 2 por edad)
                             // malla: z orientacion (0 al azar, 1 velocidad, 2 fija)
    vec4 output3;            // malla: xyz escala; cinta: x corte (s entre puntos; 0 = nunca), yzw vector
                             // de CustomFacing / CustomSide (espacio del emisor)
    vec4 ribbon0;            // cinta: x tension (0 Catmull-Rom .. 1 recta), y tramos por segmento (1..16;
                             // negativo = adaptativo por el angulo), z celda del atlas (-1 = entera)
    vec4 ribbon1;            // cinta: UV, xy escala, zw desplazamiento
    // Malla: variantes (la 0 = extra.y / extra.z; count 0 = no hay), LOD,
    // giro de Fija, escala al azar y material. Y la mezcla Distorsion.
    uvec4 variant_first;     // primer indice de cada variante
    uvec4 variant_count;     // indices de cada variante
    vec4 mesh0;              // x variantes (1..4), y m por nivel de LOD (0 = el indice de la particula),
                             // z distancia a la que pasa a sprite (0 = nunca), w distorsion: 1 = mapa de alturas
    vec4 mesh_rotation;      // cuaternion (xyzw) de la orientacion Fija
    vec4 mesh_jitter;        // xyz escala al azar por eje (0..1, por particula)
    vec4 tint;               // material: color base (multiplica; 1 sin material)
    vec4 emissive;           // material: rgb emision (se suma tras la luz); w distorsion (px a 1080p)
};

layout(std140, set = 0, binding = 0) uniform VfxFrame {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    mat4 inverse_projection;
    vec4 camera;    // xyz posicion, w depth disponible (0/1)
    vec4 right;     // xyz derecha de la camara
    vec4 up;        // xyz arriba de la camara
    vec4 viewport;  // ancho, alto, 1/ancho, 1/alto (del depth)
    vec4 light;     // rgb luz del frame (HDR lineal; sin las luces de abajo)
    vec4 sun;       // xyz hacia el sol (mallas)
    // "lit" con luz real (vfx.frag, bindings 13..17): rgb el ambiente (sin el
    // sol), w = 1 si las luces del frame estan enlazadas; 0 = solo `light`.
    vec4 ambient;
} frame;

// 4 vec4 por particula: (pos, edad) (vel, vida; <= 0 = muerta) (color rgba)
// (tamano, giro, velocidad de giro, cuadro inicial).
// Cuadro inicial: la parte entera; la fraccion (si no es 0) es la velocidad
// del flipbook - 0.5 (variacion por particula). Cintas: (ancho, id y eje
// empaquetados [vfxRibbonPack], giro de la cinta (rad), U por distancia).
layout(std430, set = 0, binding = 1) VFX_ACCESS buffer Particles { vec4 particles[]; };
layout(std430, set = 0, binding = 2) VFX_ACCESS buffer Dead { uint dead_list[]; };
layout(std430, set = 0, binding = 3) VFX_ACCESS buffer Alive { uint alive_list[]; };
// 8 por ranura: [0] muertas en la pila, [1] vivas, [2]/[3] eventos recibidos
// (uno por frame, alternando con push.pad: el par escribe [2] y lee [3]), [4..7] dibujo
// indirecto (vertices: 6 por quad o segmento de cinta, o los indices de la
// malla; instancias: las vivas).
layout(std430, set = 0, binding = 4) VFX_ACCESS buffer Counters { int counters[]; };
layout(std430, set = 0, binding = 5) readonly buffer Instances { VfxInstance instances[]; };
layout(std430, set = 0, binding = 6) readonly buffer Blocks { VfxBlock blocks[]; };
// Superficie de mallas: pares (posicion, normal).
layout(std430, set = 0, binding = 7) readonly buffer Points { vec4 points[]; };
layout(set = 0, binding = 8) uniform sampler2D scene_depth;
// Ranuras de textura: las pone VfxPass (VfxPass::textureLimit(), 16..256) por
// esta constante de especializacion.
layout(constant_id = 0) const uint VFX_TEXTURE_COUNT = 16u;
layout(set = 0, binding = 9) uniform sampler2D vfx_textures[VFX_TEXTURE_COUNT];
// Mallas de la salida Malla: pares (posicion, u) (normal, v) por vertice e
// indices ya con el primer vertice de cada malla sumado.
layout(std430, set = 0, binding = 10) readonly buffer MeshVertices { vec4 mesh_vertices[]; };
layout(std430, set = 0, binding = 11) readonly buffer MeshIndices { uint mesh_indices[]; };
// Eventos: por lista, dos frames de VFX_MAX_EVENTS, dos vec4 cada uno:
// (posicion en el mundo, cuantas nacen) (velocidad en el mundo, -).
layout(std430, set = 0, binding = 12) VFX_ACCESS buffer Events { vec4 events[]; };

layout(push_constant) uniform VfxPush {
    uint stage;
    uint slot;   // indice de la instancia
    uint count;  // SORT: altura del paso
    uint pad;    // SORT: tamano de la ordenacion (potencia de 2, >= 128); el resto: paridad de los eventos
} push;

// Hueco de la lista `list` en el frame de paridad `parity`.
uint vfxEventIndex(uint list, uint parity, uint e) {
    return ((list * 2u + parity) * VFX_MAX_EVENTS + e) * 2u;
}

// Evento `e` del canal `channel` en el frame de paridad `parity`: (posicion
// en el mundo, tipo) (velocidad en el mundo, valor) (color rgba).
uint vfxChannelIndex(uint channel, uint parity, uint e) {
    return VFX_CHANNEL_EVENTS + ((channel * 2u + parity) * VFX_MAX_CHANNEL_EVENTS + e) * 3u;
}

// Direccion unitaria <-> 2 numeros (octaedrica): el eje de una cinta "de cara
// al emisor" se guarda en la particula al nacer.
vec2 vfxEncodeDirection(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z) + 1e-8;
    vec2 p = n.xy;
    if (n.z < 0.0) p = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return p;
}
vec3 vfxDecodeDirection(vec2 p) {
    vec3 n = vec3(p, 1.0 - abs(p.x) - abs(p.y));
    float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

// Cinta: el id (0..255) y el eje "de cara al emisor" (octaedrico, 8 bits por
// componente) en un solo float entero (< 2^24: exacto, sin NaN), en el hueco
// del giro (VisualEffect.cpp ribbonPack hace la misma cuenta para los tests).
float vfxRibbonPack(uint id, vec3 axis) {
    uvec2 q = uvec2(clamp(floor((vfxEncodeDirection(axis) * 0.5 + 0.5) * 255.0 + 0.5), vec2(0.0), vec2(255.0)));
    return float((min(id, 255u) << 16u) | (q.x << 8u) | q.y);
}
uint vfxRibbonId(float packed_value) {
    return uint(max(packed_value, 0.0)) >> 16u;
}
vec3 vfxRibbonAxis(float packed_value) {
    uint bits = uint(max(packed_value, 0.0));
    vec2 p = vec2(float((bits >> 8u) & 255u), float(bits & 255u)) * (2.0 / 255.0) - 1.0;
    return vfxDecodeDirection(p);
}

// --- Aleatorios (PCG) ---
uint vfxHash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float vfxRand(inout uint s) {
    s = vfxHash(s);
    return float(s) * (1.0 / 4294967296.0);
}

float vfxRange(inout uint s, float a, float b) {
    return mix(a, b, vfxRand(s));
}

vec3 vfxRandomDirection(inout uint s) {
    float z = vfxRand(s) * 2.0 - 1.0;
    float phi = vfxRand(s) * 6.28318530718;
    float r = sqrt(max(1.0 - z * z, 0.0));
    return vec3(r * cos(phi), z, r * sin(phi));
}

vec4 vfxColorLut(uint slot, float t) {
    float x = clamp(t, 0.0, 1.0) * float(VFX_LUT - 1u);
    uint i = uint(floor(x));
    uint j = min(i + 1u, VFX_LUT - 1u);
    return mix(instances[slot].color_lut[i], instances[slot].color_lut[j], fract(x));
}

float vfxSizeValue(uint slot, uint i) {
    vec4 v = instances[slot].size_lut[i >> 2u];
    uint k = i & 3u;
    return k == 0u ? v.x : (k == 1u ? v.y : (k == 2u ? v.z : v.w));
}

float vfxSizeLut(uint slot, float t) {
    float x = clamp(t, 0.0, 1.0) * float(VFX_LUT - 1u);
    uint i = uint(floor(x));
    uint j = min(i + 1u, VFX_LUT - 1u);
    return mix(vfxSizeValue(slot, i), vfxSizeValue(slot, j), fract(x));
}
