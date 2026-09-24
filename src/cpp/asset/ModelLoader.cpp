#include "asset/Model.h"
#include "asset/ModelCache.h"
#include "asset/ObjLoader.h"

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <stb_image.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace cramion::asset {
namespace {

using core::Mat4;
using core::Quat;
using core::Vec2;
using core::Vec3;
using core::Vec4;

// Assimp guarda las matrices por filas (a1..a4 es la primera fila); Mat4 va
// por columnas, igual que GLSL.
Mat4 toMat4(const aiMatrix4x4& a) {
    Mat4 m{};
    m.m[0][0] = a.a1; m.m[1][0] = a.a2; m.m[2][0] = a.a3; m.m[3][0] = a.a4;
    m.m[0][1] = a.b1; m.m[1][1] = a.b2; m.m[2][1] = a.b3; m.m[3][1] = a.b4;
    m.m[0][2] = a.c1; m.m[1][2] = a.c2; m.m[2][2] = a.c3; m.m[3][2] = a.c4;
    m.m[0][3] = a.d1; m.m[1][3] = a.d2; m.m[2][3] = a.d3; m.m[3][3] = a.d4;
    return m;
}

Vec3 toVec3(const aiVector3D& v) {
    return {v.x, v.y, v.z};
}

Quat toQuat(const aiQuaternion& q) {
    return {q.x, q.y, q.z, q.w};
}

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

// Decodifica una imagen comprimida (PNG, JPG, TGA...) a RGBA8.
bool decodeImage(const std::uint8_t* data, std::size_t size, TextureData& out) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(data, static_cast<int>(size), &width, &height,
                                            &channels, STBI_rgb_alpha);
    if (pixels == nullptr) {
        return false;
    }

    out.width = static_cast<std::uint32_t>(width);
    out.height = static_cast<std::uint32_t>(height);
    out.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return true;
}

bool readFile(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !bytes.empty();
}

// Convierte la escena de assimp al formato del motor.
class Converter {
public:
    Converter(const aiScene& scene, std::filesystem::path directory, ModelData& out)
        : scene_(scene), directory_(std::move(directory)), out_(out) {}

    void run() {
        addNode(scene_.mRootNode, -1);
        addMaterials();
        addMeshes(scene_.mRootNode);
        addAnimations();
    }

private:
    // --- Jerarquia ---------------------------------------------------------

    void addNode(const aiNode* node, std::int32_t parent) {
        const auto index = static_cast<std::int32_t>(out_.nodes.size());
        out_.nodes.push_back(Node{node->mName.C_Str(), parent, toMat4(node->mTransformation)});
        node_index_.try_emplace(node->mName.C_Str(), index);
        node_pointer_index_[node] = index;

        for (unsigned int i = 0; i < node->mNumChildren; ++i) {
            addNode(node->mChildren[i], index);
        }
    }

    std::int32_t findNode(const aiString& name) const {
        const auto it = node_index_.find(name.C_Str());
        return (it != node_index_.end()) ? it->second : -1;
    }

    // --- Huesos ------------------------------------------------------------

    // Los huesos se comparten entre mallas por nombre: el mismo "mixamorig:Hips"
    // de la malla del cuerpo y de la de los ojos es un unico hueso.
    std::uint32_t boneFor(const aiBone& bone) {
        const auto it = bone_index_.find(bone.mName.C_Str());
        if (it != bone_index_.end()) {
            return it->second;
        }

        const std::int32_t node = findNode(bone.mName);
        if (node < 0) {
            std::cerr << "[Modelo] Hueso sin nodo en la jerarquia: " << bone.mName.C_Str() << "\n";
        }

        const auto index = static_cast<std::uint32_t>(out_.bones.size());
        out_.bones.push_back(Bone{bone.mName.C_Str(), node, toMat4(bone.mOffsetMatrix)});
        bone_index_.emplace(bone.mName.C_Str(), index);
        return index;
    }

    // Las mallas rigidas (sin huesos) se tratan como si tuvieran un unico
    // hueso: el nodo que las contiene, con offset identidad. Asi todo el
    // modelo pasa por el mismo shader y los nodos animados las mueven igual.
    std::uint32_t nodeBone(std::int32_t node) {
        const auto it = node_bone_index_.find(node);
        if (it != node_bone_index_.end()) {
            return it->second;
        }

        const auto index = static_cast<std::uint32_t>(out_.bones.size());
        out_.bones.push_back(Bone{out_.nodes[static_cast<std::size_t>(node)].name, node,
                                  Mat4::identity()});
        node_bone_index_.emplace(node, index);
        return index;
    }

    // --- Mallas ------------------------------------------------------------

    void addMeshes(const aiNode* node) {
        const std::int32_t node_index = node_pointer_index_.at(node);
        for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
            addMesh(*scene_.mMeshes[node->mMeshes[i]], node->mMeshes[i], node_index);
        }
        for (unsigned int i = 0; i < node->mNumChildren; ++i) {
            addMeshes(node->mChildren[i]);
        }
    }

    void addMesh(const aiMesh& mesh, unsigned int mesh_index, std::int32_t node) {
        if ((mesh.mPrimitiveTypes & aiPrimitiveType_TRIANGLE) == 0) {
            return;  // Puntos o lineas sueltas: no son superficie.
        }
        // Una malla con esqueleto no depende del nodo que la referencia; si
        // aparece en varios nodos se dibujaria dos veces en el mismo sitio.
        if (mesh.HasBones() && !skinned_meshes_done_.insert(mesh_index).second) {
            return;
        }

        const auto base_vertex = static_cast<std::uint32_t>(out_.vertices.size());
        const std::uint32_t fallback_bone = nodeBone(node);

        // --- Vertices ---
        for (unsigned int v = 0; v < mesh.mNumVertices; ++v) {
            SkinnedVertex vertex{};
            vertex.position = toVec3(mesh.mVertices[v]);
            vertex.normal = mesh.HasNormals() ? toVec3(mesh.mNormals[v]) : Vec3{0.0f, 1.0f, 0.0f};
            if (mesh.HasTextureCoords(0)) {
                vertex.uv = Vec2{mesh.mTextureCoords[0][v].x, mesh.mTextureCoords[0][v].y};
            }
            if (mesh.HasTangentsAndBitangents()) {
                // El signo de w dice si la bitangente de assimp coincide con
                // cross(normal, tangente) o va al reves (UV en espejo).
                const Vec3 tangent = toVec3(mesh.mTangents[v]);
                const Vec3 bitangent = toVec3(mesh.mBitangents[v]);
                const float handedness =
                    core::dot(core::cross(vertex.normal, tangent), bitangent) < 0.0f ? -1.0f
                                                                                     : 1.0f;
                vertex.tangent = core::Vec4{tangent.x, tangent.y, tangent.z, handedness};
            }
            out_.vertices.push_back(vertex);
        }

        // --- Influencias ---
        // Se quedan las cuatro de mas peso (LimitBoneWeights ya lo garantiza,
        // pero no cuesta nada ser robusto) y se normalizan a suma 1.
        if (mesh.HasBones()) {
            for (unsigned int b = 0; b < mesh.mNumBones; ++b) {
                const aiBone& bone = *mesh.mBones[b];
                const std::uint32_t bone_index = boneFor(bone);

                for (unsigned int w = 0; w < bone.mNumWeights; ++w) {
                    const aiVertexWeight& weight = bone.mWeights[w];
                    if (weight.mVertexId >= mesh.mNumVertices || weight.mWeight <= 0.0f) {
                        continue;
                    }
                    addInfluence(out_.vertices[base_vertex + weight.mVertexId], bone_index,
                                 weight.mWeight);
                }
            }
        }

        for (unsigned int v = 0; v < mesh.mNumVertices; ++v) {
            SkinnedVertex& vertex = out_.vertices[base_vertex + v];
            float sum = 0.0f;
            for (float weight : vertex.weights) {
                sum += weight;
            }

            if (sum <= 0.0f) {
                // Malla rigida, o vertice que el artista dejo sin pintar.
                vertex.joints[0] = fallback_bone;
                vertex.weights[0] = 1.0f;
                continue;
            }
            for (float& weight : vertex.weights) {
                weight /= sum;
            }
        }

        // --- Indices ---
        SubMesh submesh{};
        submesh.first_index = static_cast<std::uint32_t>(out_.indices.size());
        submesh.material = std::min(mesh.mMaterialIndex,
                                    static_cast<unsigned int>(out_.materials.size() - 1));

        for (unsigned int f = 0; f < mesh.mNumFaces; ++f) {
            const aiFace& face = mesh.mFaces[f];
            if (face.mNumIndices != 3) {
                continue;
            }
            for (unsigned int k = 0; k < 3; ++k) {
                out_.indices.push_back(base_vertex + face.mIndices[k]);
            }
        }

        submesh.index_count = static_cast<std::uint32_t>(out_.indices.size()) - submesh.first_index;
        if (submesh.index_count > 0) {
            out_.submeshes.push_back(submesh);
        }
    }

    static void addInfluence(SkinnedVertex& vertex, std::uint32_t bone, float weight) {
        // Hueco libre o, si no lo hay, el de menos peso si este pesa mas.
        std::size_t slot = 0;
        for (std::size_t i = 1; i < kMaxBoneInfluences; ++i) {
            if (vertex.weights[i] < vertex.weights[slot]) {
                slot = i;
            }
        }
        if (weight > vertex.weights[slot]) {
            vertex.joints[slot] = bone;
            vertex.weights[slot] = weight;
        }
    }

    // --- Materiales y texturas ---------------------------------------------

    void addMaterials() {
        for (unsigned int i = 0; i < scene_.mNumMaterials; ++i) {
            const aiMaterial& material = *scene_.mMaterials[i];

            MaterialData data{};
            data.name = material.GetName().C_Str();
            data.albedo_texture =
                loadTexture(material, {aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE});
            data.metallic_roughness_texture =
                loadTexture(material, {aiTextureType_GLTF_METALLIC_ROUGHNESS,
                                       aiTextureType_METALNESS, aiTextureType_DIFFUSE_ROUGHNESS});
            data.normal_texture = loadTexture(material, {aiTextureType_NORMALS});
            if (data.normal_texture < 0) {
                data.normal_texture = loadNormalFromBump(material);
            }
            // glTF guarda la oclusion como LIGHTMAP en assimp.
            data.occlusion_texture =
                loadTexture(material, {aiTextureType_LIGHTMAP, aiTextureType_AMBIENT_OCCLUSION});
            data.emissive_texture = loadTexture(material, {aiTextureType_EMISSIVE});

            float factor = 0.0f;
            if (material.Get(AI_MATKEY_METALLIC_FACTOR, factor) == AI_SUCCESS) {
                data.metallic = factor;
            }
            if (material.Get(AI_MATKEY_ROUGHNESS_FACTOR, factor) == AI_SUCCESS) {
                data.roughness = factor;
            } else {
                data.roughness = roughnessFromPhong(material);
            }
            aiColor3D emissive{0.0f, 0.0f, 0.0f};
            if (material.Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS) {
                data.emissive = Vec3{emissive.r, emissive.g, emissive.b};
            }
            // Una textura emisiva sin factor (FBX) se toma a intensidad 1.
            if (data.emissive_texture >= 0 && data.emissive.x <= 0.0f && data.emissive.y <= 0.0f &&
                data.emissive.z <= 0.0f) {
                data.emissive = Vec3{1.0f, 1.0f, 1.0f};
            }

            // Con textura, el color difuso de un FBX suele ser un gris 0.8 por
            // defecto que solo oscureceria la imagen: se ignora.
            if (data.albedo_texture < 0) {
                aiColor4D color{1.0f, 1.0f, 1.0f, 1.0f};
                if (material.Get(AI_MATKEY_BASE_COLOR, color) == AI_SUCCESS ||
                    material.Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS) {
                    data.base_color = Vec4{color.r, color.g, color.b, 1.0f};
                }
            }

            // Semitransparente (vidrio, agua): el diferido no lo puede mezclar.
            // Lo recortado por la textura (hojas) no entra aqui: su opacidad
            // de material es 1 y el alfa va en la imagen.
            float opacity = 1.0f;
            if (material.Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS && opacity < 0.99f) {
                data.transparent = true;
            }

            out_.materials.push_back(data);
        }

        if (out_.materials.empty()) {
            MaterialData fallback{};
            fallback.name = "Default";
            fallback.base_color = Vec4{0.8f, 0.8f, 0.8f, 1.0f};
            out_.materials.push_back(fallback);
        }
    }

    // Los OBJ no tienen rugosidad: se deduce del exponente de Phong (Ns) con
    // la equivalencia habitual Blinn-Phong -> GGX, alpha = sqrt(2 / (Ns + 2)).
    // Sin brillo especular (Ks ~ 0) el material se considera mate.
    //
    // Minimo 0.3: los exponentes de estos formatos se exageran a menudo (el
    // propio San Miguel lo advierte: "some surfaces are too glossy").
    static float roughnessFromPhong(const aiMaterial& material) {
        float shininess = 0.0f;
        if (material.Get(AI_MATKEY_SHININESS, shininess) != AI_SUCCESS || shininess <= 0.0f) {
            return 0.8f;
        }
        aiColor3D specular{0.04f, 0.04f, 0.04f};
        material.Get(AI_MATKEY_COLOR_SPECULAR, specular);
        if (std::max({specular.r, specular.g, specular.b}) < 0.02f) {
            return 0.9f;
        }
        const float alpha = std::sqrt(2.0f / (shininess + 2.0f));
        return std::clamp(std::sqrt(alpha), 0.3f, 1.0f);
    }

    // En los OBJ el normal map suele ir como "map_Bump", que assimp trata como
    // mapa de alturas. Solo se usa si el archivo es un normal map de verdad:
    // en el San Miguel se distinguen por el prefijo "N_".
    std::int32_t loadNormalFromBump(const aiMaterial& material) {
        aiString path;
        if (material.GetTextureCount(aiTextureType_HEIGHT) == 0 ||
            material.GetTexture(aiTextureType_HEIGHT, 0, &path) != AI_SUCCESS) {
            return -1;
        }
        const std::string file = std::filesystem::path(path.C_Str()).filename().string();
        if (file.size() < 2 || std::toupper(static_cast<unsigned char>(file[0])) != 'N' ||
            file[1] != '_') {
            return -1;
        }
        return loadTexture(material, {aiTextureType_HEIGHT});
    }

    std::int32_t loadTexture(const aiMaterial& material,
                             std::initializer_list<aiTextureType> types) {
        aiString path;
        bool found = false;
        for (const aiTextureType type : types) {
            if (material.GetTextureCount(type) > 0 &&
                material.GetTexture(type, 0, &path) == AI_SUCCESS) {
                found = true;
                break;
            }
        }
        if (!found || path.length == 0) {
            return -1;
        }

        const std::string key = path.C_Str();
        if (const auto it = texture_index_.find(key); it != texture_index_.end()) {
            return it->second;
        }

        TextureData texture{};
        texture.name = std::filesystem::path(key).filename().string();
        bool loaded = false;

        // 1) Incrustada en el archivo ("*0" o por nombre, segun el formato).
        if (const aiTexture* embedded = scene_.GetEmbeddedTexture(path.C_Str())) {
            if (embedded->mHeight == 0) {
                // Comprimida: pcData son mWidth bytes de PNG/JPG.
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(embedded->pcData);
                texture.encoded.assign(bytes, bytes + embedded->mWidth);
                loaded = !texture.encoded.empty();
            } else {
                // Ya descomprimida, en BGRA.
                texture.width = embedded->mWidth;
                texture.height = embedded->mHeight;
                texture.rgba.resize(static_cast<std::size_t>(texture.width) * texture.height * 4);
                for (std::size_t p = 0; p < static_cast<std::size_t>(texture.width) * texture.height;
                     ++p) {
                    const aiTexel& texel = embedded->pcData[p];
                    texture.rgba[p * 4 + 0] = texel.r;
                    texture.rgba[p * 4 + 1] = texel.g;
                    texture.rgba[p * 4 + 2] = texel.b;
                    texture.rgba[p * 4 + 3] = texel.a;
                }
                loaded = true;
            }
        }

        // 2) Archivo externo: la ruta tal cual relativa al modelo, o solo el
        //    nombre junto al modelo (los FBX suelen guardar la ruta absoluta
        //    del ordenador del artista).
        if (!loaded) {
            const std::filesystem::path relative(key);
            for (const std::filesystem::path& candidate :
                 {directory_ / relative, directory_ / relative.filename(),
                  directory_ / "textures" / relative.filename()}) {
                if (readFile(candidate, texture.encoded)) {
                    loaded = true;
                    break;
                }
            }
        }

        if (!loaded) {
            std::cerr << "[Modelo] No se pudo cargar la textura: " << key << "\n";
            texture_index_.emplace(key, -1);
            return -1;
        }

        const auto index = static_cast<std::int32_t>(out_.textures.size());
        out_.textures.push_back(std::move(texture));
        texture_index_.emplace(key, index);
        return index;
    }

    // --- Animaciones -------------------------------------------------------

    void addAnimations() {
        for (unsigned int a = 0; a < scene_.mNumAnimations; ++a) {
            const aiAnimation& animation = *scene_.mAnimations[a];
            // Los tiempos vienen en "ticks"; sin tasa declarada, 25 es el
            // valor por defecto que usa el propio assimp.
            const double ticks_per_second =
                (animation.mTicksPerSecond > 0.0) ? animation.mTicksPerSecond : 25.0;
            const auto to_seconds = [ticks_per_second](double ticks) {
                return static_cast<float>(ticks / ticks_per_second);
            };

            AnimationClip clip{};
            clip.name = animation.mName.C_Str();
            clip.duration = to_seconds(animation.mDuration);

            for (unsigned int c = 0; c < animation.mNumChannels; ++c) {
                const aiNodeAnim& source = *animation.mChannels[c];

                AnimationChannel channel{};
                channel.node = findNode(source.mNodeName);
                if (channel.node < 0) {
                    continue;
                }

                for (unsigned int k = 0; k < source.mNumPositionKeys; ++k) {
                    channel.positions.push_back(
                        {to_seconds(source.mPositionKeys[k].mTime), toVec3(source.mPositionKeys[k].mValue)});
                }
                for (unsigned int k = 0; k < source.mNumRotationKeys; ++k) {
                    channel.rotations.push_back(
                        {to_seconds(source.mRotationKeys[k].mTime), toQuat(source.mRotationKeys[k].mValue)});
                }
                for (unsigned int k = 0; k < source.mNumScalingKeys; ++k) {
                    channel.scales.push_back(
                        {to_seconds(source.mScalingKeys[k].mTime), toVec3(source.mScalingKeys[k].mValue)});
                }

                clip.channels.push_back(std::move(channel));
            }

            out_.animations.push_back(std::move(clip));
        }
    }

    const aiScene& scene_;
    std::filesystem::path directory_;
    ModelData& out_;

    std::unordered_map<std::string, std::int32_t> node_index_;
    std::unordered_map<const aiNode*, std::int32_t> node_pointer_index_;
    std::unordered_map<std::string, std::uint32_t> bone_index_;
    std::unordered_map<std::int32_t, std::uint32_t> node_bone_index_;
    std::unordered_map<std::string, std::int32_t> texture_index_;
    std::unordered_set<unsigned int> skinned_meshes_done_;
};

// Decodifica todas las texturas comprimidas usando todos los nucleos: un
// escenario trae cientos de PNG grandes y, uno detras de otro, tardarian
// casi un minuto.
void decodeTextures(ModelData& model) {
    std::atomic<std::size_t> next{0};
    std::atomic<std::uint32_t> failed{0};

    const auto worker = [&]() {
        for (std::size_t i = next++; i < model.textures.size(); i = next++) {
            TextureData& texture = model.textures[i];
            if (texture.encoded.empty()) {
                continue;  // Ya venia descomprimida.
            }
            if (!decodeImage(texture.encoded.data(), texture.encoded.size(), texture)) {
                // Imagen danada: un texel blanco para que el material siga
                // siendo valido.
                texture.width = 1;
                texture.height = 1;
                texture.rgba = {255, 255, 255, 255};
                ++failed;
            }
            texture.encoded.clear();
            texture.encoded.shrink_to_fit();
        }
    };

    const unsigned int thread_count =
        std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (unsigned int t = 0; t < thread_count; ++t) {
        threads.emplace_back(worker);
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    if (failed > 0) {
        std::cerr << "[Modelo] " << failed << " texturas no se pudieron decodificar\n";
    }
}

ModelData importWithAssimp(const std::filesystem::path& path) {
    Assimp::Importer importer;

    // Sin los nodos auxiliares "$AssimpFbx$" de los pivotes: las pistas de
    // animacion apuntan directamente a los huesos.
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    importer.SetPropertyInteger(AI_CONFIG_PP_LBW_MAX_WEIGHTS,
                                static_cast<int>(kMaxBoneInfluences));
    importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE,
                                aiPrimitiveType_POINT | aiPrimitiveType_LINE);

    // FlipUVs: assimp da el origen de las UV abajo (convencion de OpenGL) y
    // las imagenes se suben empezando por la fila de arriba.
    // CalcTangentSpace: tangentes para los normal maps (glTF no siempre las
    // trae).
    const unsigned int flags = aiProcess_Triangulate | aiProcess_GenSmoothNormals |
                               aiProcess_CalcTangentSpace |
                               aiProcess_JoinIdenticalVertices | aiProcess_LimitBoneWeights |
                               aiProcess_FlipUVs | aiProcess_SortByPType |
                               aiProcess_ImproveCacheLocality | aiProcess_ValidateDataStructure;

    const aiScene* scene = importer.ReadFile(utf8(path), flags);
    if (scene == nullptr || scene->mRootNode == nullptr ||
        (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0) {
        throw std::runtime_error("No se pudo cargar el modelo " + utf8(path) + ": " +
                                 importer.GetErrorString());
    }

    ModelData model{};
    model.name = path.filename().string();
    Converter(*scene, path.parent_path(), model).run();
    computeSubmeshBounds(model);
    return model;
}

}  // namespace

void computeSubmeshBounds(ModelData& model) {
    for (SubMesh& submesh : model.submeshes) {
        Vec3 low{1e30f, 1e30f, 1e30f};
        Vec3 high{-1e30f, -1e30f, -1e30f};
        for (std::uint32_t i = 0; i < submesh.index_count; ++i) {
            const Vec3& p = model.vertices[model.indices[submesh.first_index + i]].position;
            low = Vec3{std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
            high = Vec3{std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
        }
        submesh.bounds_min = low;
        submesh.bounds_max = high;
    }
}

ModelData loadModel(const std::filesystem::path& path) {
    const auto start = std::chrono::steady_clock::now();
    const std::filesystem::path cache_path = path.string() + ".cramcache";

    ModelData model{};
    if (readModelCache(cache_path, path, model)) {
        std::cout << "[Modelo] " << model.name << " leido de la cache\n";
    } else {
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        // Los OBJ (escenarios enormes) van por el lector propio en paralelo;
        // el resto de formatos, por assimp.
        std::cout << "[Modelo] Importando " << path.filename().string()
                  << " (solo la primera vez; despues se lee de la cache)...\n";
        model = (extension == ".obj") ? loadObj(path) : importWithAssimp(path);
        try {
            writeModelCache(cache_path, path, model);
            std::cout << "[Modelo] Cache guardada en " << cache_path.string() << "\n";
        } catch (const std::exception& e) {
            std::cerr << "[Modelo] Aviso: no se pudo guardar la cache: " << e.what() << "\n";
        }
    }

    decodeTextures(model);

    if (model.indices.empty()) {
        throw std::runtime_error("El modelo " + utf8(path) + " no tiene triangulos.");
    }

    const float seconds =
        std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
    std::cout << "[Modelo] Cargado en " << seconds << " s\n";

    std::cout << "[Modelo] " << model.name << ": " << model.vertices.size() << " vertices, "
              << model.indices.size() / 3 << " triangulos, " << model.bones.size() << " huesos, "
              << model.submeshes.size() << " submallas, " << model.textures.size()
              << " texturas, " << model.animations.size() << " animaciones\n";
    for (const AnimationClip& clip : model.animations) {
        std::cout << "[Modelo]   animacion \"" << clip.name << "\": " << clip.duration << " s, "
                  << clip.channels.size() << " pistas\n";
    }

    return model;
}

}  // namespace cramion::asset
