// Voxeles: datos comunes a voxel.vert, voxel.frag y voxel_shadow.frag.
//
// Caras: 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z, 6 planta (cruz, normal hacia
// arriba). Los ejes u (derecha) y v (hacia abajo en la imagen) de cada cara
// son los mismos que usa el mallador de CramionCore (voxel/Mesher.cpp).

layout(push_constant) uniform VoxelPush {
    vec4 origin_time;  // xyz esquina de la seccion, w segundos
    mat4 light_view_projection;
    uint shadow;
    float previous_time;  // segundos del frame anterior (vectores de movimiento)
    uint style;           // bit 0 color liso (VoxelWorld::flat_colors), bit 1 terreno lejano (VoxelPass::setFarSection), bit 2 sin relieve (VoxelPass::setLowQuality)
    float far_fade_end;   // lejano: distancia (m) a la que acaba de deshacerse en el cielo (0 = sin)
    vec4 distant;             // lejano: xy (x, z) centro del hueco, z su radio, w bloques por columna
    // RELIEVE TESELADO (voxel.tesc / voxel.tese): x distancia (m) hasta la que hay relieve, y profundidad (bloques)
    // hacia dentro, z trozos por bloque de cerca, w 0. y = 0: apagado (sin teselado ni empuje de las sombras).
    vec4 tess;
} push;

// TESELADO: cuanto relieve teselado hay a `d` m de la camara (1 cerca, 0 desde push.tess.x). Lo mismo en el
// teselado (profundidad), en voxel.frag (el parallax hace lo que falta) y en voxel.tesc (trozos).
float voxelTessFade(float d) {
    if (push.tess.y <= 0.0 || push.tess.x <= 0.0) return 0.0;
    return 1.0 - smoothstep(push.tess.x * 0.6, push.tess.x, d);
}

// Terreno lejano: vertice con y de 9 bits y color por vertice (VoxelPass.h).
bool farSection() { return (push.style & 2u) != 0u; }

const vec3 kFaceNormal[7] = vec3[](vec3(1.0, 0.0, 0.0), vec3(-1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0),
                                   vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, -1.0), vec3(0.0, 1.0, 0.0));
const vec3 kFaceU[7] = vec3[](vec3(0.0, 0.0, -1.0), vec3(0.0, 0.0, 1.0), vec3(1.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0),
                              vec3(1.0, 0.0, 0.0), vec3(-1.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0));
const vec3 kFaceV[7] = vec3[](vec3(0.0, -1.0, 0.0), vec3(0.0, -1.0, 0.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, -1.0),
                              vec3(0.0, -1.0, 0.0), vec3(0.0, -1.0, 0.0), vec3(0.0, 0.0, 1.0));
