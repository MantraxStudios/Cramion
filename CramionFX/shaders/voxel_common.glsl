// Voxeles: datos comunes a voxel.vert, voxel.frag y voxel_shadow.frag.
//
// Caras: 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z, 6 planta (cruz, normal hacia
// arriba). Los ejes u (derecha) y v (hacia abajo en la imagen) de cada cara
// son los mismos que usa el mallador de CramionCore (voxel/Mesher.cpp).

layout(push_constant) uniform VoxelPush {
    vec4 origin_time;  // xyz esquina de la seccion, w segundos
    mat4 light_view_projection;
    uint shadow;
} push;

const vec3 kFaceNormal[7] = vec3[](vec3(1.0, 0.0, 0.0), vec3(-1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0),
                                   vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, -1.0), vec3(0.0, 1.0, 0.0));
const vec3 kFaceU[7] = vec3[](vec3(0.0, 0.0, -1.0), vec3(0.0, 0.0, 1.0), vec3(1.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0),
                              vec3(1.0, 0.0, 0.0), vec3(-1.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0));
const vec3 kFaceV[7] = vec3[](vec3(0.0, -1.0, 0.0), vec3(0.0, -1.0, 0.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, -1.0),
                              vec3(0.0, -1.0, 0.0), vec3(0.0, -1.0, 0.0), vec3(0.0, 0.0, 1.0));
