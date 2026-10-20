// Lo que ve un shader de superficie del usuario (.crshader), en los dos
// stages (surface.vert y surface.frag lo incluyen tras declarar su push
// constant y la camara).
//
// Propiedades del material: 8 vec4 por material en un storage buffer del
// frame. En el pase de geometria push.pick_id no se usa para el picking: dice
// en que bloque de 8 empiezan las de este material.
layout(std430, set = 0, binding = 5) readonly buffer SurfaceParams {
    vec4 surface_params[];
};
#define CRAMION_PARAM(i) surface_params[push.pick_id * 8u + uint(i)]

// Color de vertice del modelo (COLOR_0, RGBA 0..1; blanco si no lo trae).
// Lo definen surface.vert y surface.frag; en las demas plantillas, blanco.
#ifndef VERTEX_COLOR
#define VERTEX_COLOR vec4(1.0)
#endif

// Datos propios de cada objeto (como el Custom Primitive Data de Unreal):
// INSTANCE_DATA(0) e INSTANCE_DATA(1), dos vec4 de su MeshRenderer
// (instance_data, entity().setMaterialParam(0, ...)). No rompen el lote ni
// piden otro .crmat. Los dan surface.frag y surface_forward.frag (en
// surface()); en vertex() y en la sombra valen cero.
#ifndef INSTANCE_DATA
#define INSTANCE_DATA(i) vec4(0.0)
#endif

// Texturas propias del material (set 1, 5..8): blancas si no se asignan.
layout(set = 1, binding = 5) uniform sampler2D cramion_texture0;
layout(set = 1, binding = 6) uniform sampler2D cramion_texture1;
layout(set = 1, binding = 7) uniform sampler2D cramion_texture2;
layout(set = 1, binding = 8) uniform sampler2D cramion_texture3;

// Segundos desde el inicio (los de la lluvia) y la camara. DELTA_TIME: lo que
// avanzo TIME en este frame (0 en las vistas aisladas: sondas, Render Textures).
//
// En los vertices TIME es una variable: el motor llama a vertex() dos veces,
// con el tiempo de este frame y con el del anterior (TIME - DELTA_TIME), y la
// diferencia es el vector de movimiento (TAA, FSR, DLSS). Asi una bandera o
// unas olas por TIME no dejan estelas.
#ifdef CRAMION_VERTEX_STAGE
float cramion_time = 0.0;
#define TIME cramion_time
#else
#define TIME (weather.params.z)
#endif
#define DELTA_TIME (uintBitsToFloat(camera.motion.y))
#define CAMERA_POSITION (camera.position.xyz)

// Lo que el shader lee y cambia de la superficie. Los colores van como se
// ven (el mismo espacio que las texturas); la emision es luz (1 = la
// emision 1 de un material).
struct Surface {
    vec3 albedo;         // color base
    float alpha;         // < 0.5 no se dibuja (recorte)
    vec3 normal;         // normal en el mundo (ya con el normal map)
    float metallic;      // 0..1
    float roughness;     // 0..1
    float occlusion;     // 0..1 (1 = sin sombra ambiental)
    vec3 emission;       // brillo propio
    // Solo domain transparent: cuanto se desvia lo de detras por la normal
    // (fraccion de la pantalla; 0 = sin refraccion, 0.02..0.1 vidrio o agua).
    // Con refraccion, lo de detras (sceneColor) se ve por (1 - alpha).
    float refraction;
    // Solo lectura:
    vec2 uv;
    vec3 worldPosition;
    vec3 vertexNormal;   // normal de la malla (sin normal map)
    vec3 viewDirection;  // del punto hacia la camara
};

// Vertice en el mundo (ya animado), antes de proyectarlo.
struct Vertex {
    vec3 position;
    vec3 normal;
    vec2 uv;
};

// --- Ayudas ---
float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float hash(vec3 p) {
    return hash(p.xy + p.z * 17.13);
}
// Ruido de valor suave, 0..1.
float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1, 0)), u.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), u.x), u.y);
}
float noise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);
    float a = mix(mix(hash(i), hash(i + vec3(1, 0, 0)), u.x), mix(hash(i + vec3(0, 1, 0)), hash(i + vec3(1, 1, 0)), u.x), u.y);
    float b = mix(mix(hash(i + vec3(0, 0, 1)), hash(i + vec3(1, 0, 1)), u.x),
                  mix(hash(i + vec3(0, 1, 1)), hash(i + vec3(1, 1, 1)), u.x), u.y);
    return mix(a, b, u.z);
}
// Varias capas de ruido (mas detalle), 0..1.
float fbm(vec2 p) {
    float sum = 0.0;
    float amplitude = 0.5;
    for (int i = 0; i < 5; ++i) {
        sum += noise(p) * amplitude;
        p *= 2.03;
        amplitude *= 0.5;
    }
    return sum / 0.96875;
}

// --- Ruido de gradiente, simplex y celdas ---
// Con hash de enteros (PCG, Jarzynski y Olano 2020): sin las celdas que deja
// el hash de fract() de hash()/noise(), sin bloques con coordenadas grandes y
// con derivada continua. noise() y fbm() siguen igual (el Shader Graph los
// replica en la CPU para su vista previa).
uint pcgHash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
uvec2 pcgHash2(uvec2 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    return v;
}
uvec3 pcgHash3(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}
// Blanco (sin suavizar), 0..1: un valor distinto para cada punto.
float random(vec2 p) {
    return float(pcgHash2(floatBitsToUint(p)).x) * (1.0 / 4294967296.0);
}
float random(vec3 p) {
    return float(pcgHash3(floatBitsToUint(p)).x) * (1.0 / 4294967296.0);
}
// Gradiente (unitario) de una esquina de la rejilla.
vec2 cramionGradient(ivec2 cell) {
    float a = float(pcgHash2(uvec2(cell)).x) * (6.28318530718 / 4294967296.0);
    return vec2(cos(a), sin(a));
}
vec3 cramionGradient(ivec3 cell) {
    uvec3 h = pcgHash3(uvec3(cell));
    float z = float(h.x) * (2.0 / 4294967296.0) - 1.0;
    float a = float(h.y) * (6.28318530718 / 4294967296.0);
    float r = sqrt(max(1.0 - z * z, 0.0));
    return vec3(r * cos(a), r * sin(a), z);
}
// Ruido de gradiente (Perlin con quintica) y su derivada: x = valor (-1..1),
// yz (yzw en 3D) = derivada respecto a p. Con la derivada se saca la normal
// de un relieve sin derivadas de pantalla (tambien en vertex()).
vec3 gradientNoiseD(vec2 p) {
    ivec2 i = ivec2(floor(p));
    vec2 f = p - floor(p);
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    vec2 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
    vec2 ga = cramionGradient(i);
    vec2 gb = cramionGradient(i + ivec2(1, 0));
    vec2 gc = cramionGradient(i + ivec2(0, 1));
    vec2 gd = cramionGradient(i + ivec2(1, 1));
    float va = dot(ga, f);
    float vb = dot(gb, f - vec2(1.0, 0.0));
    float vc = dot(gc, f - vec2(0.0, 1.0));
    float vd = dot(gd, f - vec2(1.0, 1.0));
    float k = va - vb - vc + vd;
    float value = va + u.x * (vb - va) + u.y * (vc - va) + u.x * u.y * k;
    vec2 d = ga + u.x * (gb - ga) + u.y * (gc - ga) + u.x * u.y * (ga - gb - gc + gd) +
             du * (u.yx * k + vec2(vb, vc) - va);
    // Con gradientes unitarios el valor llega a +-0.71: a -1..1.
    return vec3(value, d) * 1.41421356;
}
vec4 gradientNoiseD(vec3 p) {
    ivec3 i = ivec3(floor(p));
    vec3 f = p - floor(p);
    vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    vec3 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
    vec3 ga = cramionGradient(i);
    vec3 gb = cramionGradient(i + ivec3(1, 0, 0));
    vec3 gc = cramionGradient(i + ivec3(0, 1, 0));
    vec3 gd = cramionGradient(i + ivec3(1, 1, 0));
    vec3 ge = cramionGradient(i + ivec3(0, 0, 1));
    vec3 gf = cramionGradient(i + ivec3(1, 0, 1));
    vec3 gg = cramionGradient(i + ivec3(0, 1, 1));
    vec3 gh = cramionGradient(i + ivec3(1, 1, 1));
    float va = dot(ga, f);
    float vb = dot(gb, f - vec3(1.0, 0.0, 0.0));
    float vc = dot(gc, f - vec3(0.0, 1.0, 0.0));
    float vd = dot(gd, f - vec3(1.0, 1.0, 0.0));
    float ve = dot(ge, f - vec3(0.0, 0.0, 1.0));
    float vf = dot(gf, f - vec3(1.0, 0.0, 1.0));
    float vg = dot(gg, f - vec3(0.0, 1.0, 1.0));
    float vh = dot(gh, f - vec3(1.0, 1.0, 1.0));
    float value = va + u.x * (vb - va) + u.y * (vc - va) + u.z * (ve - va) + u.x * u.y * (va - vb - vc + vd) +
                  u.y * u.z * (va - vc - ve + vg) + u.z * u.x * (va - vb - ve + vf) +
                  u.x * u.y * u.z * (-va + vb + vc - vd + ve - vf - vg + vh);
    vec3 d = ga + u.x * (gb - ga) + u.y * (gc - ga) + u.z * (ge - ga) + u.x * u.y * (ga - gb - gc + gd) +
             u.y * u.z * (ga - gc - ge + gg) + u.z * u.x * (ga - gb - ge + gf) +
             u.x * u.y * u.z * (-ga + gb + gc - gd + ge - gf - gg + gh) +
             du * (vec3(vb - va, vc - va, ve - va) +
                   u.yzx * vec3(va - vb - vc + vd, va - vc - ve + vg, va - vb - ve + vf) +
                   u.zxy * vec3(va - vb - ve + vf, va - vb - vc + vd, va - vc - ve + vg) +
                   u.yzx * u.zxy * (-va + vb + vc - vd + ve - vf - vg + vh));
    // Con gradientes unitarios el valor llega a +-0.87: a -1..1.
    return vec4(value, d) * 1.1547;
}
float gradientNoise(vec2 p) {
    return gradientNoiseD(p).x;
}
float gradientNoise(vec3 p) {
    return gradientNoiseD(p).x;
}
// Simplex (Gustavson), -1..1: mas barato que el de gradiente en 3D y sin
// direcciones preferidas.
float simplexNoise(vec2 p) {
    const vec4 C = vec4(0.211324865405187, 0.366025403784439, -0.577350269189626, 0.024390243902439);
    vec2 i = floor(p + dot(p, C.yy));
    vec2 x0 = p - i + dot(i, C.xx);
    vec2 i1 = x0.x > x0.y ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    vec2 x1 = x0 - i1 + C.xx;
    vec2 x2 = x0 + C.zz;
    ivec2 cell = ivec2(i);
    vec3 m = max(0.5 - vec3(dot(x0, x0), dot(x1, x1), dot(x2, x2)), 0.0);
    m = m * m;
    m = m * m;
    vec3 n = vec3(dot(cramionGradient(cell), x0), dot(cramionGradient(cell + ivec2(i1)), x1),
                  dot(cramionGradient(cell + ivec2(1)), x2));
    return 70.0 * dot(m, n);
}
float simplexNoise(vec3 p) {
    const vec2 C = vec2(1.0 / 6.0, 1.0 / 3.0);
    vec3 i = floor(p + dot(p, C.yyy));
    vec3 x0 = p - i + dot(i, C.xxx);
    vec3 g = step(x0.yzx, x0.xyz);
    vec3 l = 1.0 - g;
    vec3 i1 = min(g.xyz, l.zxy);
    vec3 i2 = max(g.xyz, l.zxy);
    vec3 x1 = x0 - i1 + C.xxx;
    vec3 x2 = x0 - i2 + C.yyy;
    vec3 x3 = x0 - 0.5;
    ivec3 cell = ivec3(i);
    vec4 m = max(0.6 - vec4(dot(x0, x0), dot(x1, x1), dot(x2, x2), dot(x3, x3)), 0.0);
    m = m * m;
    m = m * m;
    vec4 n = vec4(dot(cramionGradient(cell), x0), dot(cramionGradient(cell + ivec3(i1)), x1),
                  dot(cramionGradient(cell + ivec3(i2)), x2), dot(cramionGradient(cell + ivec3(1)), x3));
    return 32.0 * dot(m, n);
}
// Fractal de ruido de gradiente, -1..1 (octaves 1..8). Cada octava va girada
// y desplazada: sin la rejilla que se ve en el fbm() de siempre.
float fbmNoise(vec2 p, int octaves) {
    const mat2 rotate = mat2(0.8, 0.6, -0.6, 0.8);
    float sum = 0.0;
    float amplitude = 0.5;
    float total = 0.0;
    for (int i = 0; i < clamp(octaves, 1, 8); ++i) {
        sum += gradientNoise(p) * amplitude;
        total += amplitude;
        p = rotate * p * 2.02 + vec2(17.31, -9.17);
        amplitude *= 0.5;
    }
    return sum / total;
}
float fbmNoise(vec3 p, int octaves) {
    const mat3 rotate = mat3(0.00, 0.80, 0.60, -0.80, 0.36, -0.48, -0.60, -0.48, 0.64);
    float sum = 0.0;
    float amplitude = 0.5;
    float total = 0.0;
    for (int i = 0; i < clamp(octaves, 1, 8); ++i) {
        sum += gradientNoise(p) * amplitude;
        total += amplitude;
        p = rotate * p * 2.02 + vec3(17.31, -9.17, 5.43);
        amplitude *= 0.5;
    }
    return sum / total;
}
// Crestas (montanas, grietas, rayos), 0..1.
float ridgedNoise(vec2 p, int octaves) {
    const mat2 rotate = mat2(0.8, 0.6, -0.6, 0.8);
    float sum = 0.0;
    float amplitude = 0.5;
    float total = 0.0;
    float weight = 1.0;
    for (int i = 0; i < clamp(octaves, 1, 8); ++i) {
        float r = 1.0 - abs(gradientNoise(p));
        r *= r * weight;
        weight = clamp(r * 2.0, 0.0, 1.0);
        sum += r * amplitude;
        total += amplitude;
        p = rotate * p * 2.02 + vec2(17.31, -9.17);
        amplitude *= 0.5;
    }
    return sum / total;
}
float ridgedNoise(vec3 p, int octaves) {
    const mat3 rotate = mat3(0.00, 0.80, 0.60, -0.80, 0.36, -0.48, -0.60, -0.48, 0.64);
    float sum = 0.0;
    float amplitude = 0.5;
    float total = 0.0;
    float weight = 1.0;
    for (int i = 0; i < clamp(octaves, 1, 8); ++i) {
        float r = 1.0 - abs(gradientNoise(p));
        r *= r * weight;
        weight = clamp(r * 2.0, 0.0, 1.0);
        sum += r * amplitude;
        total += amplitude;
        p = rotate * p * 2.02 + vec3(17.31, -9.17, 5.43);
        amplitude *= 0.5;
    }
    return sum / total;
}
// Celdas (Worley): x = distancia al centro mas cercano, y = al segundo
// (y - x: los bordes entre celdas), z = un valor por celda (0..1).
vec3 voronoi(vec2 p) {
    ivec2 cell = ivec2(floor(p));
    vec2 f = p - floor(p);
    float d1 = 8.0;
    float d2 = 8.0;
    float id = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            ivec2 offset = ivec2(x, y);
            uvec2 h = pcgHash2(uvec2(cell + offset));
            vec2 r = vec2(offset) + vec2(h) * (1.0 / 4294967296.0) - f;
            float d = dot(r, r);
            if (d < d1) {
                d2 = d1;
                d1 = d;
                id = float(pcgHash(h.x ^ h.y)) * (1.0 / 4294967296.0);
            } else if (d < d2) {
                d2 = d;
            }
        }
    }
    return vec3(sqrt(d1), sqrt(d2), id);
}
vec3 voronoi(vec3 p) {
    ivec3 cell = ivec3(floor(p));
    vec3 f = p - floor(p);
    float d1 = 8.0;
    float d2 = 8.0;
    float id = 0.0;
    for (int z = -1; z <= 1; ++z) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                ivec3 offset = ivec3(x, y, z);
                uvec3 h = pcgHash3(uvec3(cell + offset));
                vec3 r = vec3(offset) + vec3(h) * (1.0 / 4294967296.0) - f;
                float d = dot(r, r);
                if (d < d1) {
                    d2 = d1;
                    d1 = d;
                    id = float(pcgHash(h.x ^ h.y ^ h.z)) * (1.0 / 4294967296.0);
                } else if (d < d2) {
                    d2 = d;
                }
            }
        }
    }
    return vec3(sqrt(d1), sqrt(d2), id);
}
// Campo sin divergencia (remolinos, viento en vertex()): el rotacional de
// tres ruidos de gradiente, con sus derivadas exactas.
vec3 curlNoise(vec3 p) {
    vec3 a = gradientNoiseD(p).yzw;
    vec3 b = gradientNoiseD(p + vec3(31.416, -47.853, 12.679)).yzw;
    vec3 c = gradientNoiseD(p + vec3(-19.123, 7.271, -83.117)).yzw;
    return vec3(c.y - b.z, a.z - c.x, b.x - a.y);
}

// --- Color y profundidad de la escena (solo domain transparent) ---
// Lo opaco ya iluminado, sin lo transparente (la copia del vidrio), y su
// profundidad. Para distorsion de calor, ondas de choque, refraccion y
// fundido con la escena (orillas, campos de fuerza). El G-buffer (lo opaco)
// no las tiene: aun no hay imagen que leer.
#ifdef CRAMION_SCENE_TEXTURES
// Este fragmento en la pantalla, 0..1 (camera.jitter.zw = resolucion interna).
#define SCREEN_UV (gl_FragCoord.xy / camera.jitter.zw)
vec3 sceneColor(vec2 uv) {
    return textureLod(cramion_scene_color, clamp(uv, vec2(0.0), vec2(1.0)), 0.0).rgb;
}
// Profundidad del depth buffer (0 cerca .. 1 lejos; 1 = cielo).
float sceneDepth(vec2 uv) {
    return textureLod(cramion_scene_depth, clamp(uv, vec2(0.0), vec2(1.0)), 0.0).r;
}
// Punto del mundo que se ve en `uv` (lo opaco).
vec3 sceneWorldPosition(vec2 uv) {
    vec4 p = camera.inverse_view_projection * vec4(uv * 2.0 - 1.0, sceneDepth(uv), 1.0);
    return p.xyz / p.w;
}
// Distancia (m, a lo largo de la vista) de lo opaco en `uv`; 1e6 en el cielo.
float sceneLinearDepth(vec2 uv) {
    if (sceneDepth(uv) >= 1.0) return 1.0e6;
    return -(camera.view * vec4(sceneWorldPosition(uv), 1.0)).z;
}
// sceneColor en las unidades de s.emission (la plantilla la multiplica por
// kEmissiveIntensity). Distorsion: s.albedo = vec3(0), s.alpha = 1 y
// s.emission = sceneEmission(SCREEN_UV + desplazamiento).
vec3 sceneEmission(vec2 uv) {
    return sceneColor(uv) * (1.0 / kEmissiveIntensity);
}
// 0 donde la superficie toca lo opaco, 1 a `distance` metros por delante.
float depthFade(Surface s, float distance) {
    float surface_depth = -(camera.view * vec4(s.worldPosition, 1.0)).z;
    return clamp((sceneLinearDepth(SCREEN_UV) - surface_depth) / max(distance, 1e-4), 0.0, 1.0);
}
#endif
// (En los vertices de un domain transparent las pone vacias el motor, ver
// SurfaceShader.cpp: funciones propias que las usan fuera de surface().)

// Borde brillante: 0 de frente, 1 en el contorno.
float fresnel(Surface s, float power) {
    return pow(1.0 - clamp(dot(normalize(s.normal), normalize(s.viewDirection)), 0.0, 1.0), power);
}
float remap(float v, float a, float b, float c, float d) {
    return c + (v - a) / (b - a) * (d - c);
}
