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
const uint B_GRAVITY = 20u;
const uint B_WIND = 21u;
const uint B_DRAG = 22u;
const uint B_TURBULENCE = 23u;
const uint B_ATTRACTOR = 24u;
const uint B_VORTEX = 25u;
const uint B_COLLIDE_DEPTH = 26u;
const uint B_COLLIDE_PLANE = 27u;
const uint B_COLLIDE_SPHERE = 28u;
const uint B_KILL_BOX = 29u;
const uint B_SPEED_LIMIT = 30u;
const uint B_CONFORM_SPHERE = 31u;

// Etapas de vfx_sim.comp.
const uint STAGE_RESET = 0u;
const uint STAGE_BEGIN = 1u;
const uint STAGE_EMIT = 2u;
const uint STAGE_UPDATE = 3u;

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
    vec4 light;     // rgb luz del frame (HDR lineal)
} frame;

// 4 vec4 por particula: (pos, edad) (vel, vida; <= 0 = muerta) (color rgba)
// (tamano, giro, velocidad de giro, cuadro inicial).
layout(std430, set = 0, binding = 1) VFX_ACCESS buffer Particles { vec4 particles[]; };
layout(std430, set = 0, binding = 2) VFX_ACCESS buffer Dead { uint dead_list[]; };
layout(std430, set = 0, binding = 3) VFX_ACCESS buffer Alive { uint alive_list[]; };
// 8 por ranura: [0] muertas en la pila, [1] vivas, [4..7] dibujo indirecto.
layout(std430, set = 0, binding = 4) VFX_ACCESS buffer Counters { int counters[]; };
layout(std430, set = 0, binding = 5) readonly buffer Instances { VfxInstance instances[]; };
layout(std430, set = 0, binding = 6) readonly buffer Blocks { VfxBlock blocks[]; };
// Superficie de mallas: pares (posicion, normal).
layout(std430, set = 0, binding = 7) readonly buffer Points { vec4 points[]; };
layout(set = 0, binding = 8) uniform sampler2D scene_depth;
layout(set = 0, binding = 9) uniform sampler2D vfx_textures[16];

layout(push_constant) uniform VfxPush {
    uint stage;
    uint slot;   // indice de la instancia
    uint count;
    uint pad;
} push;

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
