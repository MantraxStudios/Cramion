#version 450

// Reflejos en espacio de pantalla (SSR), como los "Screen Space Reflections"
// de Unreal o el SSSR de AMD FidelityFX: el suelo pulido refleja las columnas,
// los muros y las ventanas que se ven en pantalla. Donde no hay nada en
// pantalla (fuera de ella, detras de la camara) la iluminacion usa la sonda de
// reflexion o el IBL.
//
// Para cada pixel poco rugoso se sigue el rayo reflejado por la piramide de
// profundidad MINIMA (ssr_hiz.comp, SsrPyramids). Si lo mas cercano de un
// bloque de pantalla queda detras del rayo, el rayo cruza el bloque entero de
// un salto y sube de nivel; si no, baja de nivel hasta el pixel. Asi no se
// salta nada fino (patas, marcos) y no depende de un grosor por paso: antes,
// con pasos de hasta un metro, cualquier rayo que pasaba por detras de un
// objeto "chocaba" con el y dejaba estelas estiradas bajo los muebles. Aqui
// solo es choque si el rayo acaba pegado a la superficie (thickness()); si va
// muy por detras, pasa por detras y sigue.
//
// Lo que se ve donde choca se toma de la imagen iluminada del frame anterior
// (con luz, sombras, GI y otros reflejos), reproyectada con la vista-
// proyeccion (con su jitter) con la que se dibujo, y en el mip del cono de la
// rugosidad: una pared satinada refleja una mancha borrosa, como en la sonda,
// y un suelo pulido un reflejo nitido. (Antes se emborronaba con cinco
// muestras en cruz: cuatro copias desplazadas de cada borde.)
//
// Salida: rgb = color reflejado, a = confianza (0 = sin reflejo: se usa la
// sonda o el IBL). La confianza baja cerca de los bordes de la pantalla, con la
// rugosidad, en los rayos que vuelven hacia la camara y en los choques con la
// cara de atras de algo.

layout(set = 0, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 position;
} camera;

layout(set = 0, binding = 1) uniform sampler2D g_depth;
layout(set = 0, binding = 2) uniform sampler2D g_normal;  // rg = normal, b = rugosidad
// Imagen HDR del frame anterior con mips (trilineal).
layout(set = 0, binding = 3) uniform sampler2D previous_color;
// Profundidad minima por niveles (texelFetch; el nivel 0 es el depth buffer).
layout(set = 0, binding = 4) uniform sampler2D depth_pyramid;

layout(push_constant) uniform PushConstants {
    // Vista-proyeccion, con su jitter, con la que se dibujo previous_color.
    mat4 previous_view_projection;
    // x = trazar (hay frame anterior y reflejos), z = niveles de la piramide
    // de profundidad, w = mips de previous_color.
    vec4 params;
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_reflection;

const int kMaxIterations = 96;
const float kMaxRoughness = 0.6;    // mas rugoso: solo la sonda / el IBL
const float kFadeRoughness = 0.45;  // desde aqui se funde con ellos
const float kMaxRadiance = 1000.0;  // fuera NaN, infinitos y lo absurdo
const float kFloatMax = 3.402823466e+38;
// Lobulo GGX: semiangulo del reflejo a media altura ~ 1.29 * alfa (alfa =
// rugosidad^2). Con el, el diametro de la mancha a la distancia del choque.
const float kConeSlope = 1.29;

float linearDepth(float depth) {
    return (camera.projection[3][2] - depth * camera.projection[3][3]) / (camera.projection[2][2] - depth * camera.projection[2][3]);
}

vec3 viewFromDepth(vec2 uv, float depth) {
    float z = linearDepth(depth);
    vec2 ndc = uv * 2.0 - 1.0;
    // Con el desplazamiento del centro ([2][0], [2][1]): cada ojo de un casco
    // de VR tiene un campo de vision asimetrico (y el TAA mueve el centro con
    // su jitter). Sin el, en VR todo salia desplazado y la luz se ennegrecia.
    // Forma general (perspectiva y ortografica): w del clip = P[2][3] z_v +
    // P[3][3] con z_v = -z. En perspectiva es lo de siempre, (ndc + P[2][.]) z / P[.][.].
    float w = camera.projection[3][3] - camera.projection[2][3] * z;
    return vec3((ndc.x * w + camera.projection[2][0] * z - camera.projection[3][0]) / camera.projection[0][0],
                (ndc.y * w + camera.projection[2][1] * z - camera.projection[3][1]) / camera.projection[1][1], -z);
}

// Punto de vista -> (uv de pantalla, profundidad del depth buffer). El punto
// tiene que estar delante del plano cercano.
vec3 projectToScreen(vec3 view_position) {
    vec4 clip = camera.projection * vec4(view_position, 1.0);
    vec3 ndc = clip.xyz / clip.w;
    return vec3(ndc.xy * 0.5 + 0.5, ndc.z);
}

vec3 decodeNormal(vec2 e) {
    vec3 n = vec3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y);
    float t = max(-n.y, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.z += n.z >= 0.0 ? -t : t;
    return normalize(n);
}

vec3 viewAt(ivec2 pixel, ivec2 size) {
    ivec2 p = clamp(pixel, ivec2(0), size - 1);
    return viewFromDepth((vec2(p) + 0.5) / vec2(size), texelFetch(g_depth, p, 0).r);
}

// Normal real de la superficie (la del depth buffer, sin normal maps), en
// espacio de vista y mirando a la camara. De cada lado se toma el vecino de
// profundidad mas parecida (no cruzar un borde); si los dos estan lejos (un
// objeto de un pixel de ancho), se queda la del material.
vec3 geometricNormal(ivec2 pixel, ivec2 size, vec3 center, vec3 fallback, vec3 to_camera) {
    vec3 left = viewAt(pixel - ivec2(1, 0), size);
    vec3 right = viewAt(pixel + ivec2(1, 0), size);
    vec3 below = viewAt(pixel - ivec2(0, 1), size);
    vec3 above = viewAt(pixel + ivec2(0, 1), size);
    float limit = 0.05 * -center.z + 0.02;
    float dl = abs(center.z - left.z);
    float dr = abs(right.z - center.z);
    float db = abs(center.z - below.z);
    float da = abs(above.z - center.z);
    if (min(dl, dr) > limit || min(db, da) > limit) {
        return fallback;
    }
    vec3 dx = dr < dl ? right - center : center - left;
    vec3 dy = da < db ? above - center : center - below;
    vec3 n = cross(dx, dy);
    float len = length(n);
    if (len < 1e-8) {
        return fallback;
    }
    n /= len;
    return dot(n, to_camera) < 0.0 ? -n : n;
}

// Lo que se acepta como choque: el rayo pegado a la superficie del pixel. Si
// va mas por detras, pasa por detras de algo (una pata, un marco, el canto de
// una mesa) y lo que hay en ese pixel no es lo que refleja. Fino: con mas, el
// reflejo de lo delgado salia casi el doble de ancho. (Si el plano del pixel
// en el punto del rayo queda mas lejos que su centro, se compara con el plano:
// vista de lado, una superficie cambia de profundidad mas que esto dentro de
// un mismo pixel.)
float thickness(float surface_distance) {
    return 0.03 + 0.005 * surface_distance;
}

void main() {
    ivec2 size = textureSize(g_depth, 0);
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec2 screen = vec2(size);

    float depth = texelFetch(g_depth, pixel, 0).r;
    vec4 normal_sample = texelFetch(g_normal, pixel, 0);
    float roughness = normal_sample.b;
    if (push.params.x < 0.5 || depth >= 1.0 || roughness > kMaxRoughness) {
        out_reflection = vec4(0.0);
        return;
    }

    vec2 uv = (vec2(pixel) + 0.5) / screen;
    vec3 position = viewFromDepth(uv, depth);
    vec3 normal = normalize(mat3(camera.view) * decodeNormal(normal_sample.rg));
    // Ortografica (P[2][3] = 0): todos los rayos de la camara van por -Z.
    bool orthographic = camera.projection[2][3] == 0.0;
    vec3 view_direction = orthographic ? vec3(0.0, 0.0, -1.0) : normalize(position);
    vec3 geometric = geometricNormal(pixel, size, position, normal, -view_direction);

    vec3 ray = reflect(view_direction, normal);
    // Con normal maps el reflejo puede apuntar por debajo de la superficie
    // real y chocar con ella misma a unos centimetros (reflejos sucios, con la
    // textura del propio suelo): se levanta lo justo por encima.
    float under = dot(ray, geometric);
    if (under < 0.02) {
        ray = normalize(ray + geometric * (0.02 - under));
    }

    // Los rayos que vuelven hacia la camara salen enseguida de lo que hay en
    // pantalla (lo que tienen delante esta detras de la camara): se desvanecen.
    float toward_camera = orthographic ? 0.0 : smoothstep(0.3, 0.75, ray.z);
    if (toward_camera >= 1.0) {
        out_reflection = vec4(0.0);
        return;
    }

    // --- Rayo en espacio de pantalla: (uv, profundidad), una recta ---
    // Origen algo separado de la superficie por su normal real.
    vec3 origin = position + geometric * (0.003 + 0.002 * -position.z);
    // El final solo da la direccion; tiene que quedar delante del plano
    // cercano (si no, la proyeccion se da la vuelta).
    float ray_length = max(-origin.z, 1.0);
    if (!orthographic && ray.z > 0.0) {
        float near_plane = linearDepth(0.0);
        ray_length = min(ray_length, 0.9 * (-near_plane - origin.z) / ray.z);
    }
    vec3 start = projectToScreen(origin);
    vec3 direction = projectToScreen(origin + ray * max(ray_length, 1e-4)) - start;
    vec3 inverse_direction = vec3(direction.x != 0.0 ? 1.0 / direction.x : kFloatMax,
                                  direction.y != 0.0 ? 1.0 / direction.y : kFloatMax,
                                  direction.z != 0.0 ? 1.0 / direction.z : kFloatMax);
    // Los planos de los bordes del bloque hacia donde va el rayo, y un empujon
    // minimo hacia el bloque siguiente (no quedarse en el borde).
    vec2 floor_offset = vec2(direction.x < 0.0 ? 0.0 : 1.0, direction.y < 0.0 ? 0.0 : 1.0);
    vec2 uv_offset = 0.005 / screen * vec2(direction.x < 0.0 ? -1.0 : 1.0, direction.y < 0.0 ? -1.0 : 1.0);

    // --- Recorrido por la piramide (FidelityFX SSSR, profundidad normal: 0 =
    // cerca, 1 = lejos) ---
    int top_level = max(int(push.params.z) - 1, 0);
    int level = 0;
    vec2 level_size = screen;
    // Primer paso: al borde del pixel de partida, para no chocar consigo mismo.
    vec2 first_boundary = (floor(start.xy * level_size) + floor_offset) / level_size + uv_offset;
    vec2 first_t = (first_boundary - start.xy) * inverse_direction.xy;
    float t = min(first_t.x, first_t.y);
    vec3 current = start + direction * t;

    bool hit = false;
    // 0..1: lo que el rayo va por detras de la superficie, sobre lo admitido.
    float hit_gap = 0.0;
    for (int i = 0; i < kMaxIterations; ++i) {
        if (any(lessThan(current.xy, vec2(0.0))) || any(greaterThan(current.xy, vec2(1.0))) ||
            current.z <= 0.0 || current.z >= 1.0) {
            break;  // fuera de la pantalla o del rango de profundidad: sin choque
        }
        vec2 cell = current.xy * level_size;
        ivec2 texel = min(ivec2(cell), textureSize(depth_pyramid, level) - 1);
        float surface = texelFetch(depth_pyramid, texel, level).r;

        // Hasta el borde del bloque, o hasta la profundidad de lo mas cercano
        // del bloque si el rayo va hacia ella.
        vec2 boundary = (floor(cell) + floor_offset) / level_size + uv_offset;
        vec2 t_boundary = (boundary - start.xy) * inverse_direction.xy;
        float t_cell = min(t_boundary.x, t_boundary.y);
        float t_surface = direction.z > 0.0 ? (surface - start.z) * inverse_direction.z : kFloatMax;

        if (current.z < surface) {
            // Por delante de todo lo del bloque.
            if (t_cell < t_surface) {
                // Lo cruza entero sin tocar nada: al siguiente, un nivel mas grueso.
                t = t_cell;
                level = min(level + 1, top_level);
            } else if (level == 0) {
                // Llega a la superficie de este pixel: choque.
                t = max(t_surface, t);
                current = start + direction * t;
                hit = true;
                break;
            } else {
                // Llega a lo mas cercano del bloque: se avanza hasta ahi y se
                // mira mas fino.
                t = max(t_surface, t);
                --level;
            }
        } else if (level > 0) {
            // Por detras de lo mas cercano del bloque: puede chocar dentro.
            --level;
        } else {
            // Pixel exacto y el rayo por detras de su profundidad: choque si va
            // pegado a la superficie; si va muy por detras, pasa por detras y
            // sigue. La superficie es el plano del pixel (su normal) en el uv
            // del rayo, no la profundidad de su centro.
            float surface_distance = linearDepth(surface);
            float ray_distance = linearDepth(current.z);
            float slack = thickness(surface_distance);
            vec3 pixel_normal = mat3(camera.view) * decodeNormal(texelFetch(g_normal, texel, 0).rg);
            vec3 pixel_center = viewFromDepth((vec2(texel) + 0.5) / screen, surface);
            // El rayo de camara de este uv: a + b * z (z = distancia, como en viewFromDepth).
            vec2 ndc = current.xy * 2.0 - 1.0;
            vec3 a = vec3((ndc.x * camera.projection[3][3] - camera.projection[3][0]) / camera.projection[0][0],
                          (ndc.y * camera.projection[3][3] - camera.projection[3][1]) / camera.projection[1][1], 0.0);
            vec3 b = vec3((camera.projection[2][0] - ndc.x * camera.projection[2][3]) / camera.projection[0][0],
                          (camera.projection[2][1] - ndc.y * camera.projection[2][3]) / camera.projection[1][1], -1.0);
            // El plano solo puede alejar la superficie, nunca acercarla: si no,
            // un rayo que sube del suelo hacia la cara de abajo (oculta) de una
            // mesa pasaba "por detras" de su tablero y reflejaba la pared del
            // fondo bajo la mesa.
            float plane_distance = surface_distance;
            float denominator = dot(pixel_normal, b);
            if (abs(denominator) > 1e-4) {
                plane_distance = clamp(dot(pixel_normal, pixel_center - a) / denominator, surface_distance,
                                       surface_distance + 4.0 * slack);
            }
            // Mas lo que avanza el propio rayo en profundidad al cruzar el pixel.
            float exit_depth = clamp(start.z + direction.z * t_cell, 0.0, 0.9999999);
            float tolerance = slack + min(abs(linearDepth(exit_depth) - ray_distance), 4.0 * slack);
            if (ray_distance - plane_distance <= tolerance) {
                hit_gap = max(ray_distance - plane_distance, 0.0) / tolerance;
                hit = true;
                break;
            }
            t = t_cell;
        }
        level_size = screen * exp2(-float(level));
        current = start + direction * t;
    }

    if (!hit) {
        out_reflection = vec4(0.0);
        return;
    }

    // --- Lo que hay en el punto de choque ---
    vec2 hit_uv = current.xy;
    ivec2 hit_texel = clamp(ivec2(hit_uv * screen), ivec2(0), size - 1);
    float hit_depth = texelFetch(g_depth, hit_texel, 0).r;
    if (hit_depth >= 1.0) {
        out_reflection = vec4(0.0);
        return;
    }
    vec3 hit_view = viewFromDepth(hit_uv, hit_depth);
    vec3 hit_normal = normalize(mat3(camera.view) * decodeNormal(texelFetch(g_normal, hit_texel, 0).rg));
    // La superficie golpeada tiene que mirar hacia el rayo: si no, es la cara
    // de atras de algo (el reflejo seria el de su interior).
    float facing = smoothstep(-0.05, 0.1, dot(hit_normal, -ray));
    // El propio plano a un pixel de distancia: es el mismo suelo, no un reflejo.
    vec2 travelled = abs(hit_uv - uv) * screen;
    if (max(travelled.x, travelled.y) < 1.5 && dot(hit_normal, geometric) > 0.95) {
        out_reflection = vec4(0.0);
        return;
    }

    // --- Reproyeccion al frame anterior ---
    vec3 hit_world = camera.position.xyz + transpose(mat3(camera.view)) * hit_view;
    vec4 previous_clip = push.previous_view_projection * vec4(hit_world, 1.0);
    if (previous_clip.w <= 0.0) {
        out_reflection = vec4(0.0);
        return;
    }
    vec2 previous_uv = previous_clip.xy / previous_clip.w * 0.5 + 0.5;
    if (any(lessThan(previous_uv, vec2(0.0))) || any(greaterThan(previous_uv, vec2(1.0)))) {
        out_reflection = vec4(0.0);
        return;
    }

    // --- Cono de la rugosidad: el mip de la imagen anterior que lo cubre ---
    float alpha = roughness * roughness;
    float footprint = 2.0 * length(hit_view - position) * kConeSlope * alpha;  // diametro, metros
    float pixels_per_meter = 0.5 * screen.y * abs(camera.projection[1][1]);
    if (!orthographic) {
        pixels_per_meter /= max(-hit_view.z, 1e-3);
    }
    float cone_lod = log2(max(footprint * pixels_per_meter, 1.0));
    float lod = clamp(cone_lod, 0.0, max(push.params.w - 1.0, 0.0));
    vec3 color = textureLod(previous_color, previous_uv, lod).rgb;
    color = clamp(mix(color, vec3(0.0), isnan(color)), vec3(0.0), vec3(kMaxRadiance));

    // --- Confianza ---
    vec2 hit_edge = min(hit_uv, 1.0 - hit_uv);
    vec2 previous_edge = min(previous_uv, 1.0 - previous_uv);
    float edge_fade = smoothstep(0.0, 0.06, min(hit_edge.x, hit_edge.y)) *
                      smoothstep(0.0, 0.03, min(previous_edge.x, previous_edge.y));
    float roughness_fade = 1.0 - smoothstep(kFadeRoughness, kMaxRoughness, roughness);
    // Cerca del limite del grosor, suave (sin bordes de tijera en lo que asoma).
    float gap_fade = 1.0 - smoothstep(0.5, 1.0, hit_gap);
    // Conos muy anchos (rugoso y lejos): ese mip es la media de un trozo grande
    // de pantalla, con lo que tapa por delante; ahi responde mejor la sonda.
    // (En mips de una pantalla de 1080 de alto: a otra resolucion, el mismo
    // trozo de pantalla. Con el cono entero, no con el mip que hay: a 1440p y
    // 4K los mips se acaban antes y los conos anchos no se apagaban nunca.)
    float cone_fade = 1.0 - smoothstep(6.0, 8.0, cone_lod - log2(screen.y / 1080.0));
    float confidence = edge_fade * roughness_fade * facing * gap_fade * cone_fade * (1.0 - toward_camera);

    out_reflection = vec4(color, confidence);
}
