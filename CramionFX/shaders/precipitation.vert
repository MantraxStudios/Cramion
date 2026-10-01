#version 450

// Precipitacion (PrecipitationPass): lluvia, nieve, polvo, salpicaduras y el
// trazo de los rayos. No hay buffer de vertices: cada instancia es una
// particula (o un segmento del rayo) y sus 6 vertices forman un quad.
//
// Las particulas viven en una caja alrededor de la camara pero ancladas al
// mundo: su posicion es un punto al azar de la caja + lo que han caido + lo
// que las ha empujado el viento, envuelto (modulo) en la caja que rodea a la
// camara. Al moverse la camara las que salen por un lado entran por el otro.

layout(set = 0, binding = 0) uniform Frame {
    mat4 view_projection;
    mat4 inverse_view_projection;
    mat4 rain_view_projection;
    vec4 camera;     // xyz = camara, w = segundos
    vec4 right;      // xyz = derecha de la camara, w = tan(fov / 2)
    vec4 wind;       // xyz = viento (m/s), w = mapa de lluvia listo
    vec4 drift;      // xyz = empuje acumulado del viento (m), w = alto de la imagen (px)
    vec4 amounts;    // x = lluvia, y = nieve, z = polvo, w = ancho de la imagen (px)
    vec4 light;      // rgb = luz de las gotas (HDR lineal), w = destello del rayo
    vec4 bolt_info;  // x = brillo del rayo, y = segmentos
    vec4 bolt[192];
} frame;

// Mapa de lluvia: profundidad de la escena vista desde arriba (techos).
layout(set = 0, binding = 1) uniform sampler2D rain_map;
// Profundidad de la escena (salpicaduras sobre el suelo que se ve).
layout(set = 0, binding = 2) uniform sampler2D scene_depth;

layout(push_constant) uniform Push {
    int mode;   // 0 lluvia, 1 nieve, 2 polvo, 3 salpicaduras, 4 rayo
    int count;
} push;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
layout(location = 2) flat out int v_mode;
layout(location = 3) out float v_age;

const vec2 kCorners[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

uint hashU(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

vec3 hash3(uint i) {
    return vec3(hashU(i), hashU(i ^ 0x9e3779b9u), hashU(i ^ 0x85ebca6bu)) * (1.0 / 4294967295.0);
}

// Fuera de la pantalla (triangulo degenerado: no se rasteriza).
void hide() {
    gl_Position = vec4(-4.0, -4.0, 0.5, 1.0);
    v_color = vec4(0.0);
}

// 1 a la intemperie, 0 bajo techo (`margin` en profundidad del mapa).
float exposure(vec3 world, float margin) {
    if (frame.wind.w < 0.5) return 1.0;
    vec4 clip = frame.rain_view_projection * vec4(world, 1.0);
    vec3 r = clip.xyz / clip.w;
    vec2 uv = r.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
    float above = textureLod(rain_map, uv, 0.0).r;
    return r.z - above > margin ? 0.0 : 1.0;
}

// Metros que mide un pixel a `distance` de la camara.
float pixelSize(float distance) {
    return 2.0 * distance * frame.right.w / max(frame.drift.w, 1.0);
}

vec3 worldFromDepth(vec2 uv, float depth) {
    vec4 p = frame.inverse_view_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return p.xyz / p.w;
}

// Quad a lo largo de `axis` (largo `len`) y de ancho `width`, de cara a la camara.
void emitRibbon(vec3 center, vec3 axis, float len, float width, vec2 corner) {
    vec3 to_camera = normalize(frame.camera.xyz - center);
    vec3 side = cross(axis, to_camera);
    side = dot(side, side) > 1e-6 ? normalize(side) : frame.right.xyz;
    vec3 world = center + axis * (corner.y * len * 0.5) + side * (corner.x * width * 0.5);
    gl_Position = frame.view_projection * vec4(world, 1.0);
}

void fallingParticle(uint id, vec2 corner) {
    vec3 seed = hash3(id * 3u + uint(push.mode) * 7919u);
    float t = frame.camera.w;
    // Por tipo: radio y alto de la caja, velocidad de caida, tamano, opacidad.
    float radius;
    float height;
    float fall;
    float size;
    float alpha;
    vec3 tint = vec3(1.0);
    if (push.mode == 0) {
        radius = 22.0;
        height = 20.0;
        fall = 7.5 + 2.5 * seed.y;  // gotas de 1-3 mm: 6-9 m/s
        size = 0.0055;
        alpha = 0.22 + 0.18 * frame.amounts.x;
    } else if (push.mode == 1) {
        radius = 18.0;
        height = 16.0;
        fall = 0.9 + 0.7 * fract(seed.y * 7.31);
        size = 0.010 + 0.016 * fract(seed.x * 13.7);
        alpha = 0.9;
    } else {
        radius = 26.0;
        height = 14.0;
        fall = 0.4 * seed.y - 0.15;
        size = 0.0035 + 0.004 * seed.x;
        alpha = 0.55;
        tint = vec3(0.85, 0.62, 0.38);
    }
    vec3 box = vec3(radius * 2.0, height, radius * 2.0);
    vec3 box_min = frame.camera.xyz - vec3(radius, height * 0.45, radius);
    vec3 velocity = frame.wind.xyz * (push.mode == 2 ? 1.15 : 1.0) + vec3(0.0, -fall, 0.0);
    vec3 p = seed * box + frame.drift.xyz * (push.mode == 2 ? 1.15 : 1.0) + vec3(0.0, -fall * t, 0.0);
    if (push.mode == 1) {
        // Los copos se mecen.
        float phase = seed.z * 40.0;
        p.x += sin(t * 1.3 + phase) * 0.35;
        p.z += cos(t * 1.1 + phase * 1.7) * 0.35;
        velocity.x += cos(t * 1.3 + phase) * 0.45;
        velocity.z -= sin(t * 1.1 + phase * 1.7) * 0.38;
    }
    vec3 world = box_min + mod(p - box_min, box);

    vec3 rel = world - frame.camera.xyz;
    float dist = length(rel);
    float fade = (1.0 - smoothstep(radius * 0.6, radius * 0.98, length(rel.xz))) *
                 (1.0 - smoothstep(height * 0.45, height * 0.55, rel.y)) *
                 smoothstep(-height * 0.45, -height * 0.35, rel.y) * smoothstep(0.3, 1.2, dist);
    if (fade <= 0.001 || exposure(world, 0.0015) < 0.5) {
        hide();
        return;
    }

    // Ancho minimo de ~1 pixel (con menos opacidad: la misma "cantidad").
    float pixel = pixelSize(dist);
    float width = max(size, pixel * 0.85);
    alpha *= mix(1.0, size / width, 0.8);
    float speed = length(velocity);
    vec3 axis = speed > 1e-3 ? velocity / speed : vec3(0.0, -1.0, 0.0);
    // Estela del movimiento durante la exposicion (motion blur de la gota).
    float len = push.mode == 0 ? clamp(speed * 0.05, 0.25, 1.4)
                               : max(width, speed * 0.016);
    if (push.mode == 1) len = max(len, width);
    // Las gotas lejanas se funden (si no, una cortina de lineas).
    alpha *= fade;
    if (push.mode == 0) alpha *= 1.0 - smoothstep(10.0, radius, dist) * 0.5;

    emitRibbon(world, axis, len, width, corner);
    vec3 lit = frame.light.rgb + vec3(0.8, 0.85, 1.0) * frame.light.w;
    v_color = vec4(lit * tint * (push.mode == 0 ? 1.4 : 1.0), alpha);
}

void splash(uint id, vec2 corner) {
    const float kLife = 0.32;
    float t = frame.camera.w;
    vec3 h0 = hash3(id * 5u + 17u);
    float cycle_time = t / kLife + h0.x;
    float cycle = floor(cycle_time);
    float age = fract(cycle_time);
    vec3 h = hash3(id * 7919u + uint(cycle) * 104729u);
    // Un punto al azar del suelo alrededor de la camara: se proyecta a la
    // altura de los pies y se toma lo que se ve en ese pixel.
    float r = sqrt(h.x) * 18.0 + 0.8;
    float a = h.y * 6.2831853;
    vec3 guess = frame.camera.xyz + vec3(cos(a) * r, -1.7, sin(a) * r);
    vec4 clip = frame.view_projection * vec4(guess, 1.0);
    if (clip.w <= 0.01) {
        hide();
        return;
    }
    vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.002))) || any(greaterThan(uv, vec2(0.998)))) {
        hide();
        return;
    }
    float depth = textureLod(scene_depth, uv, 0.0).r;
    if (depth >= 1.0) {
        hide();
        return;
    }
    vec3 p = worldFromDepth(uv, depth);
    vec2 texel = vec2(2.0 / max(frame.amounts.w, 1.0), 2.0 / max(frame.drift.w, 1.0));
    vec3 px = worldFromDepth(uv + vec2(texel.x, 0.0), textureLod(scene_depth, uv + vec2(texel.x, 0.0), 0.0).r);
    vec3 py = worldFromDepth(uv + vec2(0.0, texel.y), textureLod(scene_depth, uv + vec2(0.0, texel.y), 0.0).r);
    vec3 n = cross(py - p, px - p);
    n = dot(n, n) > 1e-12 ? normalize(n) : vec3(0.0, 1.0, 0.0);
    if (n.y < 0.0) n = -n;
    float dist = length(p - frame.camera.xyz);
    if (n.y < 0.7 || dist > 28.0 || exposure(p, 0.004) < 0.5) {
        hide();
        return;
    }
    float size = 0.05 + 0.16 * age;
    vec3 side = normalize(vec3(frame.right.x, 0.0, frame.right.z) + vec3(1e-4, 0.0, 0.0));
    vec3 world = p + n * 0.01 + side * (corner.x * size * 0.6) +
                 vec3(0.0, 1.0, 0.0) * ((corner.y * 0.5 + 0.5) * size * 0.55 * (1.0 - 0.4 * age));
    gl_Position = frame.view_projection * vec4(world, 1.0);
    v_age = age;
    float alpha = frame.amounts.x * pow(1.0 - age, 1.5) * 0.55 * (1.0 - smoothstep(14.0, 28.0, dist));
    vec3 lit = frame.light.rgb * 1.3 + vec3(0.8, 0.85, 1.0) * frame.light.w;
    v_color = vec4(lit, alpha);
}

void boltSegment(uint id, vec2 corner) {
    if (int(id) >= int(frame.bolt_info.y)) {
        hide();
        return;
    }
    vec4 a = frame.bolt[id * 2u];
    vec4 b = frame.bolt[id * 2u + 1u];
    vec3 center = (a.xyz + b.xyz) * 0.5;
    vec3 d = b.xyz - a.xyz;
    float len = length(d);
    float dist = length(center - frame.camera.xyz);
    // Ancho minimo de 2 pixeles: a kilometros el canal real (cm) no se veria.
    float width = max(a.w, pixelSize(dist) * 2.0) * 3.0;
    emitRibbon(center, d / max(len, 1e-3), len + width * 0.5, width, corner);
    if (gl_Position.w <= 0.0) {
        hide();
        return;
    }
    // En el cielo: detras de toda la geometria, delante del fondo.
    gl_Position.z = gl_Position.w * 0.999995;
    v_color = vec4(vec3(0.75, 0.82, 1.0) * frame.bolt_info.x * b.w * 60.0, 1.0);
}

void main() {
    uint id = uint(gl_InstanceIndex);
    vec2 corner = kCorners[gl_VertexIndex % 6];
    v_uv = corner;
    v_mode = push.mode;
    v_age = 0.0;
    if (push.mode == 4) {
        boltSegment(id, corner);
    } else if (push.mode == 3) {
        splash(id, corner);
    } else {
        fallingParticle(id, corner);
    }
}
