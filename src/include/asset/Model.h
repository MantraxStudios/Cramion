#ifndef CRAMION_ASSET_MODEL_H
#define CRAMION_ASSET_MODEL_H

#include "core/Math.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cramion::asset {

// Influencias por vertice. Cuatro es el estandar (y lo que deja
// aiProcess_LimitBoneWeights): el numero de HUESOS del modelo no tiene limite,
// solo cuantos afectan a cada vertice.
inline constexpr std::uint32_t kMaxBoneInfluences = 4;

// Vertice de un modelo con esqueleto. 80 bytes, dato puro: la descripcion para
// Vulkan vive en gfx::SkinnedPass.
struct SkinnedVertex {
    core::Vec3 position;
    core::Vec3 normal;
    core::Vec2 uv;
    // Tangente para el normal map: xyz = direccion de +U en la superficie,
    // w = +-1 segun la orientacion de la bitangente (UV reflejadas).
    core::Vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
    // Indices en ModelData::bones. Son uint32, asi que tampoco limitan el
    // numero de huesos.
    std::uint32_t joints[kMaxBoneInfluences] = {0, 0, 0, 0};
    float weights[kMaxBoneInfluences] = {0.0f, 0.0f, 0.0f, 0.0f};
};

// Imagen del modelo. Mientras se carga guarda el archivo comprimido tal cual
// (`encoded`, PNG/JPG); al final todas se decodifican a RGBA8 en paralelo y
// `encoded` queda vacio. La cache del modelo guarda la version comprimida.
struct TextureData {
    std::string name;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
    std::vector<std::uint8_t> encoded;
};

// Material PBR metal/rugosidad (el de glTF 2.0 y Unreal).
struct MaterialData {
    std::string name;
    core::Vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f};
    core::Vec3 emissive{0.0f, 0.0f, 0.0f};
    float metallic = 0.0f;
    float roughness = 0.8f;
    float occlusion_strength = 1.0f;
    float normal_scale = 1.0f;
    // Vidrio, agua: materiales semitransparentes sin textura con alfa. Un
    // renderizador diferido no puede mezclarlos, asi que no se dibujan.
    bool transparent = false;

    // Indices en ModelData::textures; -1 = sin textura (solo los factores).
    std::int32_t albedo_texture = -1;
    // glTF: B = metalicidad, G = rugosidad (R libre u oclusion).
    std::int32_t metallic_roughness_texture = -1;
    std::int32_t normal_texture = -1;
    std::int32_t occlusion_texture = -1;
    std::int32_t emissive_texture = -1;
};

// Tramo del buffer de indices que se dibuja con un material. Los escenarios
// se parten ademas en celdas del espacio (ver ObjLoader), asi que un mismo
// material puede tener varias submallas: cada una con su caja, para
// descartar las que quedan fuera de camara o de una sombra.
struct SubMesh {
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    std::uint32_t material = 0;
    // Caja envolvente en el espacio del modelo (pose de reposo).
    core::Vec3 bounds_min{0.0f, 0.0f, 0.0f};
    core::Vec3 bounds_max{0.0f, 0.0f, 0.0f};
};


// Nodo de la jerarquia del modelo. Se guarda la jerarquia entera, no solo los
// huesos: los nodos intermedios tambien pueden estar animados o llevar una
// transformacion (la escala de cm a m de un FBX, por ejemplo).
//
// Los nodos estan ordenados de forma que un padre siempre va antes que sus
// hijos, asi la pose global se calcula en una sola pasada.
struct Node {
    std::string name;
    std::int32_t parent = -1;
    core::Mat4 local = core::Mat4::identity();  // Transformacion en reposo.
};

// Hueso: un nodo que deforma vertices. `offset` (la "inverse bind matrix")
// lleva un vertice del espacio de la malla al espacio del hueso en reposo.
struct Bone {
    std::string name;
    std::int32_t node = -1;
    core::Mat4 offset = core::Mat4::identity();
};

struct VectorKey {
    float time = 0.0f;  // Segundos.
    core::Vec3 value;
};

struct QuatKey {
    float time = 0.0f;  // Segundos.
    core::Quat value;
};

// Pista de animacion de un nodo. Si alguna lista esta vacia, esa componente
// se toma de la pose de reposo.
struct AnimationChannel {
    std::int32_t node = -1;
    std::vector<VectorKey> positions;
    std::vector<QuatKey> rotations;
    std::vector<VectorKey> scales;
};

struct AnimationClip {
    std::string name;
    float duration = 0.0f;  // Segundos.
    std::vector<AnimationChannel> channels;
};

// Modelo completo en CPU, listo para subir a la GPU y animar.
struct ModelData {
    std::string name;

    std::vector<SkinnedVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<SubMesh> submeshes;

    std::vector<MaterialData> materials;
    std::vector<TextureData> textures;

    std::vector<Node> nodes;
    std::vector<Bone> bones;
    std::vector<AnimationClip> animations;
};

// Carga un modelo (FBX y cualquier formato que tenga assimp activado) con su
// esqueleto, sus animaciones y sus texturas, incrustadas o en archivos junto
// al modelo. Lanza std::runtime_error si no se puede leer.
ModelData loadModel(const std::filesystem::path& path);

// Calcula la caja de cada submalla a partir de sus vertices (pose de reposo).
void computeSubmeshBounds(ModelData& model);

}  // namespace cramion::asset

#endif  // CRAMION_ASSET_MODEL_H
