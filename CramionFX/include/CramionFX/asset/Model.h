#ifndef CRAMION_ASSET_MODEL_H
#define CRAMION_ASSET_MODEL_H

#include "CramionFX/core/Math.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
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

// Formato de los pixeles de una textura ya decodificada. Los BC ("block
// compression", DXT) se suben comprimidos a la GPU tal cual vienen del DDS.
enum class TextureFormat : std::uint8_t {
    Rgba8,  // 4 bytes por pixel; los mips los genera la GPU
    Bc1,    // DXT1: color (+ alfa de 1 bit), 8 bytes por bloque de 4x4
    Bc2,    // DXT3: color + alfa explicito
    Bc3,    // DXT5: color + alfa interpolado
    Bc4,    // un canal
    Bc5,    // dos canales (normal maps: X e Y)
    Bc7,    // color de alta calidad
};

// Bytes por bloque de 4x4 de un formato BC (0 para Rgba8).
std::uint32_t blockBytes(TextureFormat format);
// Bytes de un nivel de mip de `width` x `height`.
std::size_t mipByteSize(TextureFormat format, std::uint32_t width, std::uint32_t height);

// Imagen del modelo. Mientras se carga guarda el archivo tal cual (`encoded`,
// PNG/JPG/TGA/DDS) o solo su ruta (`source_path`); al final todas se leen y
// decodifican en paralelo y `encoded` queda vacio.
struct TextureData {
    std::string name;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    TextureFormat format = TextureFormat::Rgba8;
    // Niveles guardados en `pixels`, seguidos (solo los BC traen mas de uno).
    std::uint32_t mip_levels = 1;
    std::vector<std::uint8_t> pixels;
    std::vector<std::uint8_t> encoded;
    // Archivo del que se lee si `encoded` esta vacio. La cache del modelo
    // guarda solo la ruta: no duplica gigas de texturas que ya estan en disco.
    std::string source_path;
    // Mapa de alturas en escala de grises (el "bump" de los OBJ): al
    // decodificarlo se convierte en un normal map en espacio tangente.
    bool height_map = false;
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
    // Reflectancia a incidencia normal (F0) si no es metal: 0.04 es la de
    // casi todos los dielectricos (piedra, madera, plastico). Mas alta, la
    // superficie refleja mas tambien vista de frente (marmol pulido, laca).
    float reflectance = 0.04f;
    // Convenio del normal map: OpenGL (+Y arriba en la imagen, el de glTF) o
    // DirectX (+Y abajo: Unreal, Lumberyard/Bistro). El archivo no lo dice.
    bool normal_map_directx = false;
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

    // Shader de superficie del usuario (.crshader), ya registrado en el
    // renderizador (VulkanRenderer::createSurfaceShader); -1 = el estandar. Sus
    // propiedades (8 vec4) y hasta 4 texturas propias (indices en
    // ModelData::textures; -1 = blanca). No se guardan en la cache del modelo:
    // los pone RenderSync al aplicar un .crmat.
    std::int32_t surface_shader = -1;
    std::array<core::Vec4, 8> surface_params{};
    std::array<std::int32_t, 4> surface_textures{-1, -1, -1, -1};
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
    // De que parte del archivo viene: el nodo (ModelData::nodes) con assimp,
    // o el objeto/grupo ("o"/"g") con los OBJ importados por objetos. -1 =
    // sin dato. El importador de assets de CramionCore la usa para partir el
    // modelo en piezas, como Unity (un hijo por malla).
    std::int32_t node = -1;
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

// Nivel de detalle simplificado (LOD1, LOD2...) de un modelo estatico. Usa los
// mismos vertices; sus submallas (clusteres por material, con su caja) apuntan
// a ModelData::lod_indices. `error`: cuanto se desvia como mucho de la malla
// original, en unidades del modelo; el renderizador lo pasa a pixeles para
// elegir el nivel.
struct MeshLod {
    std::vector<SubMesh> submeshes;
    float error = 0.0f;
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

    // LODs automaticos (generateLods): se crean al cargar, no se guardan en
    // la cache ni en el .crdata. LOD0 son `indices`/`submeshes`.
    std::vector<std::uint32_t> lod_indices;
    std::vector<MeshLod> lods;
};

// Genera los LODs de un modelo estatico (sin animaciones, un hueso) con
// meshoptimizer: cada nivel con ~la mitad de triangulos que el anterior,
// quitando las piezas sueltas que ya no se ven (hojas, briznas). No hace nada
// con modelos animados o de pocos triangulos. Devuelve los niveles creados.
std::size_t generateLods(ModelData& model);

// Parte en clusteres unas submallas cualesquiera (como clusterSubmeshes) y
// calcula sus cajas. Para los LODs, que tienen sus propios indices.
void clusterIndexRanges(const std::vector<SkinnedVertex>& vertices, std::vector<std::uint32_t>& indices,
                        std::vector<SubMesh>& submeshes);

// Carga un modelo (FBX y cualquier formato que tenga assimp activado) con su
// esqueleto, sus animaciones y sus texturas, incrustadas o en archivos junto
// al modelo. Lanza std::runtime_error si no se puede leer.
//
// `force_static`: para escenarios. Hornea la jerarquia en los vertices aunque
// el archivo traiga animaciones (se descartan), para que el frustum culling
// trabaje por trozos. Sin el, solo se hornea si nada se anima.
ModelData loadModel(const std::filesystem::path& path, bool force_static = false);

// Calcula la caja de cada submalla a partir de sus vertices (pose de reposo).
void computeSubmeshBounds(ModelData& model);

// --- Por pasos (el importador de assets de CramionCore) ---
// loadModel() = importModelSource() + cache + finalizeModel().

// Lee el archivo original (OBJ con el lector propio, el resto con assimp) sin
// usar ni escribir la cache y SIN decodificar las texturas: quedan tal como
// vienen (`encoded`) o solo con su ruta (`source_path`).
// Avance de la lectura (0..1), llamado desde el hilo que importa. Con OBJ
// no se llama (el lector propio no informa).
using ImportProgressCallback = std::function<void(float)>;

ModelData importModelSource(const std::filesystem::path& path, bool force_static = false,
                            const ImportProgressCallback& on_progress = {});

// Como importModelSource(), pero conservando la jerarquia para partir el
// modelo en piezas: sin hornear los nodos (cada malla queda en el espacio de
// SU nodo, SubMesh::node indica cual), sin animaciones y sin clusteres. En
// los OBJ, SubMesh::node es el indice del objeto/grupo y `nodes` trae uno por
// objeto (transform identidad).
ModelData importModelHierarchy(const std::filesystem::path& path,
                               const ImportProgressCallback& on_progress = {});

// Junta las submallas del mismo material y nodo y las parte en clusteres
// para el culling (rejilla relativa a su caja, con un minimo de triangulos
// por cluster: no depende de la unidad del archivo), y recalcula las cajas.
void clusterSubmeshes(ModelData& model);

// true si un modelo estatico tiene muchas mas submallas de las que daria
// clusterSubmeshes() (p. ej. un .crdata importado en centimetros con las
// celdas fijas de 5 unidades de versiones anteriores): conviene reagrupar.
bool isOverClustered(const ModelData& model);

// Lee a `encoded` las texturas que solo tenian ruta y la borra: el modelo
// queda autonomo (no depende de archivos sueltos). Devuelve cuantas no se
// encontraron.
std::uint32_t embedTextures(ModelData& model);

// Lado maximo de las texturas de los modelos al decodificarlas (0 = sin
// limite). Las mayores se reducen a la mitad las veces necesarias (media de
// 2x2), y las comprimidas (DDS) pierden sus mips mas grandes: ocupan menos RAM
// y VRAM. Lo pone el renderizador segun el perfil de hardware (calidad de
// texturas); afecta a lo que se carga despues.
void setMaxTextureSize(std::uint32_t size);
std::uint32_t maxTextureSize();

// Decodifica todas las texturas (en paralelo) y comprueba que haya
// triangulos. Lanza std::runtime_error si el modelo esta vacio. `label` es
// para los mensajes.
void finalizeModel(ModelData& model, const std::string& label);

}  // namespace cramion::asset

#endif  // CRAMION_ASSET_MODEL_H
