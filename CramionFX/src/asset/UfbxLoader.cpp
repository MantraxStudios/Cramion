#include "UfbxLoader.h"

#include <ufbx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

// Cargador de FBX con ufbx (ROADMAP P1 #1, como Godot 4.3). Reproduce lo que
// sale del camino de assimp (ModelLoader.cpp) con los mismos pasos:
//   - assimp con PRESERVE_PIVOTS = false: cada nodo lleva su transformacion
//     local completa (pivotes incluidos). ufbx la da en node_to_parent.
//   - Triangulate + JoinIdenticalVertices: triangulos de cada poligono y
//     vertices repetidos fusionados (aqui, por vertice logico y atributos).
//   - FlipUVs: v = 1 - v (y la bitangente cambia de signo).
//   - CalcTangentSpace / GenSmoothNormals: solo si el archivo no las trae.
//   - LimitBoneWeights: las 4 influencias de mas peso, normalizadas.
//   - una malla por material (assimp parte cada geometria por material).
//   - PreTransformVertices (escenarios): los vertices al espacio del modelo
//     y una submalla por material.
// Diferencias buscadas: las transformaciones geometricas (pivotes de 3ds
// Max) van horneadas en los vertices (assimp las perdia), las tomas se leen
// sin ValidateDataStructure (una pista rota no tumba el archivo: ufbx no la
// valida, se queda con lo que puede leer) y las curvas no lineales se
// muestrean a 30 Hz en vez de tomar solo las claves.

namespace cramion::asset::detail {
namespace {

using core::Mat4;
using core::Quat;
using core::Vec2;
using core::Vec3;
using core::Vec4;

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::string str(const ufbx_string& s) { return std::string(s.data != nullptr ? s.data : "", s.length); }

Mat4 toMat4(const ufbx_matrix& a) {
    Mat4 m = Mat4::identity();
    for (int c = 0; c < 4; ++c) {
        m.m[c][0] = static_cast<float>(a.cols[c].x);
        m.m[c][1] = static_cast<float>(a.cols[c].y);
        m.m[c][2] = static_cast<float>(a.cols[c].z);
        m.m[c][3] = c == 3 ? 1.0f : 0.0f;
    }
    return m;
}

Vec3 toVec3(const ufbx_vec3& v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

ufbx_vec3 toUfbx(const Vec3& v) {
    ufbx_vec3 out{};
    out.x = v.x;
    out.y = v.y;
    out.z = v.z;
    return out;
}

std::filesystem::path fromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

Quat toQuat(const ufbx_quat& q) {
    return {static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z), static_cast<float>(q.w)};
}

bool readFile(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const std::streamsize size = file.tellg();
    if (size <= 0) return false;
    bytes.resize(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    return static_cast<bool>(file);
}

bool containsNoCase(const std::string& text, const std::string& word) {
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find(word) != std::string::npos;
}

// --- Lectura -----------------------------------------------------------------

struct SceneDeleter {
    void operator()(ufbx_scene* scene) const { ufbx_free_scene(scene); }
};
using ScenePtr = std::unique_ptr<ufbx_scene, SceneDeleter>;

struct BakeDeleter {
    void operator()(ufbx_baked_anim* bake) const { ufbx_free_baked_anim(bake); }
};
using BakePtr = std::unique_ptr<ufbx_baked_anim, BakeDeleter>;

ufbx_progress_result forwardProgress(void* user, const ufbx_progress* progress) {
    const auto& callback = *static_cast<const ImportProgressCallback*>(user);
    if (progress->bytes_total > 0) {
        // La lectura es casi todo el trabajo; el 10 % que queda, la conversion.
        callback(0.9f * std::min(1.0f, static_cast<float>(static_cast<double>(progress->bytes_read) /
                                                          static_cast<double>(progress->bytes_total))));
    }
    return UFBX_PROGRESS_CONTINUE;
}

// `geometry`: con mallas, pieles y formas (sin ellas, solo nodos y tomas).
ScenePtr loadScene(const std::filesystem::path& path, bool geometry, bool embedded,
                   const ImportProgressCallback* on_progress, std::string& error) {
    // El archivo se lee aqui (rutas con acentos en Windows) y ufbx lo
    // interpreta en memoria.
    std::vector<std::uint8_t> bytes;
    if (!readFile(path, bytes)) {
        error = "no se pudo leer el archivo";
        return nullptr;
    }
    ufbx_load_opts opts{};
    opts.generate_missing_normals = true;
    // Pivotes geometricos (3ds Max) horneados en los vertices; con una
    // malla instanciada en nodos con pivotes distintos, nodo auxiliar.
    opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
    // Escala sin heredar (Maya "segment scale compensate"): compensada en
    // los nodos, que el Animator compone siempre con herencia normal.
    opts.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_COMPENSATE;
    opts.clean_skin_weights = true;
    opts.ignore_missing_external_files = true;
    opts.connect_broken_elements = true;
    opts.ignore_geometry = !geometry;
    opts.ignore_embedded = !embedded;
    const std::string name = utf8(path);
    opts.filename.data = name.c_str();
    opts.filename.length = name.size();
    if (on_progress != nullptr && *on_progress) {
        opts.progress_cb.fn = &forwardProgress;
        opts.progress_cb.user = const_cast<ImportProgressCallback*>(on_progress);
        opts.progress_interval_hint = 1u << 20;
    }
    ufbx_error load_error{};
    ufbx_scene* scene = ufbx_load_memory(bytes.data(), bytes.size(), &opts, &load_error);
    if (scene == nullptr) {
        char text[1024] = {};
        ufbx_format_error(text, sizeof(text), &load_error);
        error = text;
    }
    return ScenePtr(scene);
}

// Metros por unidad (UnitScaleFactor esta en centimetros), como
// sceneUnitScale de assimp: 1 si el archivo no lo dice.
float unitScale(const ufbx_scene& scene) {
    const ufbx_prop* prop = ufbx_find_prop(&scene.settings.props, "UnitScaleFactor");
    if (prop == nullptr) return 1.0f;
    const double factor = prop->value_real;
    return factor > 1e-6 && factor < 1e6 ? static_cast<float>(factor * 0.01) : 1.0f;
}

const ufbx_prop* ownProp(const ufbx_props& props, const char* name);

// La transformacion que assimp pone en su nodo raiz (correctRootTransform):
// filas = ejes derecha, arriba y delante del archivo (con su signo). Solo si
// el archivo trae mallas (assimp no la aplica a los FBX solo de animacion).
// Sin ella, un FBX con otros ejes saldria girado o en espejo respecto a
// assimp. assimp quiere multiplicarla por UnitScaleFactor, pero lo lee como
// double de unos metadatos que guarda como float y se queda en 1: aqui
// tambien 1 (la unidad la aplica el importador con Convert Units).
Mat4 assimpRootTransform(const ufbx_scene& scene) {
    bool meshes = false;
    for (std::size_t m = 0; m < scene.meshes.count && !meshes; ++m) {
        meshes = scene.meshes.data[m]->num_faces > 0 && scene.meshes.data[m]->instances.count > 0;
    }
    if (!meshes) return Mat4::identity();
    const auto integer = [&](const char* name, std::int64_t fallback) {
        const ufbx_prop* prop = ownProp(scene.settings.props, name);
        return prop != nullptr ? prop->value_int : fallback;
    };
    const float scale = 1.0f;
    const std::int64_t up = integer("UpAxis", 1), up_sign = integer("UpAxisSign", 1);
    const std::int64_t front = integer("FrontAxis", 2), front_sign = integer("FrontAxisSign", 1);
    const std::int64_t coord = integer("CoordAxis", 0), coord_sign = integer("CoordAxisSign", 1);
    if (up < 0 || up > 2 || front < 0 || front > 2 || coord < 0 || coord > 2) return Mat4::identity();
    float right_vec[3] = {0, 0, 0}, up_vec[3] = {0, 0, 0}, forward_vec[3] = {0, 0, 0};
    up_vec[up] = static_cast<float>(up_sign) * scale;
    forward_vec[front] = static_cast<float>(front_sign) * scale;
    right_vec[coord] = static_cast<float>(coord_sign) * scale;
    // Filas de assimp -> Mat4 por columnas: m[columna][fila].
    Mat4 m = Mat4::identity();
    for (int c = 0; c < 3; ++c) {
        m.m[c][0] = right_vec[c];
        m.m[c][1] = up_vec[c];
        m.m[c][2] = forward_vec[c];
    }
    return m;
}

// Propiedad escrita en el propio elemento (sin la plantilla del archivo): lo
// que assimp lee con useTemplate = false.
const ufbx_prop* ownProp(const ufbx_props& props, const char* name) {
    const std::size_t length = std::strlen(name);
    for (std::size_t i = 0; i < props.props.count; ++i) {
        const ufbx_prop& prop = props.props.data[i];
        if (prop.name.length == length && std::memcmp(prop.name.data, name, length) == 0) return &prop;
    }
    return nullptr;
}

// Con la plantilla del archivo (useTemplate = true en assimp).
const ufbx_prop* templateProp(const ufbx_props& props, const char* name) {
    return ufbx_find_prop(&props, name);
}

// El nodo o alguno de sus descendientes lleva una malla con caras.
bool subtreeHasMeshes(const ufbx_node* node) {
    if (node->mesh != nullptr && node->mesh->num_faces > 0) return true;
    for (std::size_t i = 0; i < node->children.count; ++i) {
        if (subtreeHasMeshes(node->children.data[i])) return true;
    }
    return false;
}

// La toma tiene alguna curva (lo que assimp convierte en una animacion).
bool stackHasCurves(const ufbx_anim_stack& stack) {
    for (std::size_t l = 0; l < stack.layers.count; ++l) {
        if (stack.layers.data[l]->anim_props.count > 0) return true;
    }
    return false;
}

// Tiempo de la toma: su inicio y fin (LocalStart/LocalStop) o, sin ellos,
// el de sus claves (igual que assimp: las claves se desplazan al inicio).
void clipRange(const ufbx_anim_stack& stack, const ufbx_baked_anim& bake, double& begin, double& end) {
    begin = stack.time_begin;
    end = stack.time_end;
    if (!(end > begin)) {
        begin = bake.key_time_min;
        end = bake.key_time_max;
    }
    if (!std::isfinite(begin) || !std::isfinite(end) || end < begin) {
        begin = 0.0;
        end = 0.0;
    }
}

BakePtr bake(const ufbx_scene& scene, const ufbx_anim_stack& stack) {
    ufbx_bake_opts opts{};
    // Las curvas lineales y las ya muestreadas (>= 19.5 Hz) se quedan con
    // sus claves; las demas se muestrean a 30 Hz. Las claves que sobran en
    // tramos rectos se quitan.
    opts.resample_rate = 30.0;
    opts.key_reduction_enabled = true;
    opts.key_reduction_rotation = true;
    ufbx_error error{};
    ufbx_baked_anim* baked = ufbx_bake_anim(&scene, stack.anim, &opts, &error);
    if (baked == nullptr) {
        char text[512] = {};
        ufbx_format_error(text, sizeof(text), &error);
        std::cerr << "[Modelo] Aviso: la toma \"" << str(stack.name) << "\" no se pudo leer (" << text
                  << "): se descarta.\n";
    }
    return BakePtr(baked);
}

bool finite(const ufbx_vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool finite(const ufbx_quat& q) {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}

// Pistas de nodo de una toma ya horneada. `node_of` lleva el typed_id de
// cada nodo de ufbx a un nodo del motor (-1 = se descarta). Como
// repairAnimations con assimp: una pista con valores no finitos (una toma
// rota) se quita con aviso y el resto de la toma sigue.
template <typename NodeOf>
void addNodeChannels(const ufbx_baked_anim& baked, double begin, float position_scale, NodeOf&& node_of,
                     AnimationClip& clip, const std::string& file) {
    std::size_t dropped = 0;
    for (std::size_t n = 0; n < baked.nodes.count; ++n) {
        const ufbx_baked_node& source = baked.nodes.data[n];
        AnimationChannel channel{};
        channel.node = node_of(source.typed_id);
        if (channel.node < 0) continue;
        const auto seconds = [begin](double t) { return static_cast<float>(t - begin); };
        bool valid = true;
        for (std::size_t k = 0; k < source.translation_keys.count && valid; ++k) {
            const ufbx_baked_vec3& key = source.translation_keys.data[k];
            valid = std::isfinite(key.time) && finite(key.value);
            channel.positions.push_back({seconds(key.time), toVec3(key.value) * position_scale});
        }
        for (std::size_t k = 0; k < source.rotation_keys.count && valid; ++k) {
            const ufbx_baked_quat& key = source.rotation_keys.data[k];
            valid = std::isfinite(key.time) && finite(key.value);
            channel.rotations.push_back({seconds(key.time), toQuat(key.value)});
        }
        for (std::size_t k = 0; k < source.scale_keys.count && valid; ++k) {
            const ufbx_baked_vec3& key = source.scale_keys.data[k];
            valid = std::isfinite(key.time) && finite(key.value);
            channel.scales.push_back({seconds(key.time), toVec3(key.value)});
        }
        if (!valid || (channel.positions.empty() && channel.rotations.empty() && channel.scales.empty())) {
            dropped += valid ? 0 : 1;
            continue;
        }
        clip.channels.push_back(std::move(channel));
    }
    if (dropped > 0) {
        std::cerr << "[Modelo] Aviso: " << file << ", toma \"" << clip.name << "\": " << dropped
                  << " pistas con valores no finitos quitadas\n";
    }
}

// --- Conversion --------------------------------------------------------------

class Converter {
public:
    enum class Mode { Animated, Static, Hierarchy };

    Converter(const ufbx_scene& scene, std::filesystem::path directory, ModelData& out, Mode mode)
        : scene_(scene), directory_(std::move(directory)), out_(out), mode_(mode),
          root_(assimpRootTransform(scene)) {}

    void run() {
        addNodes();
        node_bone_.assign(out_.nodes.size(), -1);
        if (mode_ == Mode::Static) {
            static_nodes_.clear();
        }
        // Las mallas en el orden de los nodos (el de addMeshes con assimp).
        for (std::size_t i = 0; i < order_.size(); ++i) {
            const ufbx_node* node = order_[i];
            if (node->mesh != nullptr) {
                addMesh(*node, *node->mesh, static_cast<std::int32_t>(i));
            }
        }
        if (out_.materials.empty()) {
            MaterialData fallback{};
            fallback.name = "Default";
            fallback.base_color = Vec4{0.8f, 0.8f, 0.8f, 1.0f};
            out_.materials.push_back(fallback);
        }
        if (mode_ == Mode::Animated) {
            addAnimations();
        }
    }

private:
    // --- Jerarquia -----------------------------------------------------------

    void addNodes() {
        node_index_of_typed_.assign(scene_.nodes.count, -1);
        if (mode_ == Mode::Static) {
            // PreTransformVertices: una raiz y un hijo por material (se anaden
            // al convertir las mallas).
            out_.nodes.push_back(Node{"RootNode", -1, Mat4::identity()});  // (la raiz ya va en los vertices)
            order_.clear();
            collect(scene_.root_node);
            return;
        }
        addNode(scene_.root_node, -1);
    }

    // Orden de los nodos sin crearlos (escenario horneado).
    void collect(const ufbx_node* node) {
        order_.push_back(node);
        for (std::size_t i = 0; i < node->children.count; ++i) collect(node->children.data[i]);
    }

    void addNode(const ufbx_node* node, std::int32_t parent) {
        const auto index = static_cast<std::int32_t>(out_.nodes.size());
        // La raiz de assimp se llama "RootNode": los prefabs y las escenas
        // guardan nombres de nodo.
        std::string name = node->is_root ? std::string("RootNode") : str(node->name);
        out_.nodes.push_back(Node{name, parent, node->is_root ? root_ : toMat4(node->node_to_parent)});
        node_index_of_typed_[node->typed_id] = index;
        order_.push_back(node);
        for (std::size_t i = 0; i < node->children.count; ++i) addNode(node->children.data[i], index);
    }

    std::int32_t nodeIndex(const ufbx_node* node) const {
        if (node == nullptr || node->typed_id >= node_index_of_typed_.size()) return -1;
        return node_index_of_typed_[node->typed_id];
    }

    // --- Huesos --------------------------------------------------------------

    // Mismo criterio que el camino de assimp: un hueso por nombre y matriz de
    // enlace (offset); el mismo nodo con offsets distintos (3ds Max) son dos
    // entradas.
    std::uint32_t boneFor(const ufbx_skin_cluster& cluster) {
        const std::string name = str(cluster.bone_node->name);
        const Mat4 offset = toMat4(cluster.geometry_to_bone);
        const auto [first, last] = bone_index_.equal_range(name);
        for (auto it = first; it != last; ++it) {
            if (sameMatrix(out_.bones[it->second].offset, offset)) return it->second;
        }
        const std::int32_t node = nodeIndex(cluster.bone_node);
        const auto index = static_cast<std::uint32_t>(out_.bones.size());
        out_.bones.push_back(Bone{name, node, offset});
        bone_index_.emplace(name, index);
        return index;
    }

    static bool sameMatrix(const Mat4& a, const Mat4& b) {
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                const float x = a.m[c][r];
                const float y = b.m[c][r];
                if (std::abs(x - y) > 1e-4f * std::max(1.0f, std::max(std::abs(x), std::abs(y)))) return false;
            }
        }
        return true;
    }

    // Malla rigida: un hueso que es su nodo, con offset identidad.
    std::uint32_t nodeBone(std::int32_t node) {
        if (node_bone_[static_cast<std::size_t>(node)] >= 0) {
            return static_cast<std::uint32_t>(node_bone_[static_cast<std::size_t>(node)]);
        }
        const auto index = static_cast<std::uint32_t>(out_.bones.size());
        out_.bones.push_back(Bone{out_.nodes[static_cast<std::size_t>(node)].name, node, Mat4::identity()});
        node_bone_[static_cast<std::size_t>(node)] = static_cast<std::int32_t>(index);
        return index;
    }

    // --- Mallas --------------------------------------------------------------

    // Vertice de salida antes de fusionar: atributos tal cual (para comparar)
    // y el vertice logico de ufbx (pesos y formas de mezcla).
    struct Corner {
        std::uint32_t logical = 0;
        float position[3] = {};
        float normal[3] = {};
        float uv[2] = {};
        float tangent[3] = {};
        float bitangent[3] = {};
        float color[4] = {};
        bool operator==(const Corner& o) const { return std::memcmp(this, &o, sizeof(Corner)) == 0; }
    };
    struct CornerHash {
        std::size_t operator()(const Corner& c) const {
            const auto* bytes = reinterpret_cast<const unsigned char*>(&c);
            std::uint64_t h = 1469598103934665603ull;
            for (std::size_t i = 0; i < sizeof(Corner); ++i) h = (h ^ bytes[i]) * 1099511628211ull;
            return static_cast<std::size_t>(h);
        }
    };
    static_assert(sizeof(Corner) == 4 * (1 + 3 + 3 + 2 + 3 + 3 + 4), "Corner sin relleno (se compara por bytes)");

    static void store(float* dst, const ufbx_vec3& v) {
        dst[0] = static_cast<float>(v.x);
        dst[1] = static_cast<float>(v.y);
        dst[2] = static_cast<float>(v.z);
    }

    const ufbx_material* partMaterial(const ufbx_node& node, const ufbx_mesh& mesh, std::uint32_t index) const {
        if (index < node.materials.count && node.materials.data[index] != nullptr) return node.materials.data[index];
        if (index < mesh.materials.count) return mesh.materials.data[index];
        return nullptr;
    }

    void addMesh(const ufbx_node& node, const ufbx_mesh& mesh, std::int32_t node_index) {
        if (mesh.num_faces == 0 || mesh.num_triangles == 0) return;
        const ufbx_skin_deformer* skin = nullptr;
        if (mode_ == Mode::Animated && mesh.skin_deformers.count > 0 && mesh.skin_deformers.data[0]->clusters.count > 0) {
            skin = mesh.skin_deformers.data[0];
        }
        // Una malla con piel no depende del nodo que la referencia: si esta
        // en varios, se dibujaria dos veces en el mismo sitio.
        if (skin != nullptr && !skinned_meshes_done_.insert(mesh.typed_id).second) return;

        std::vector<std::uint32_t> triangle(mesh.max_face_triangles * 3);
        for (std::size_t p = 0; p < mesh.material_parts.count; ++p) {
            const ufbx_mesh_part& part = mesh.material_parts.data[p];
            if (part.num_triangles == 0) continue;
            addPart(node, mesh, part, skin, node_index, triangle);
        }
    }

    void addPart(const ufbx_node& node, const ufbx_mesh& mesh, const ufbx_mesh_part& part,
                 const ufbx_skin_deformer* skin, std::int32_t node_index, std::vector<std::uint32_t>& triangle) {
        const bool has_uv = mesh.vertex_uv.exists;
        const bool has_tangents = mesh.vertex_tangent.exists && mesh.vertex_bitangent.exists && has_uv;
        const bool has_color = mesh.vertex_color.exists;
        const bool has_normals = mesh.vertex_normal.exists;

        // --- Esquinas fusionadas (Triangulate + JoinIdenticalVertices) ---
        std::vector<Corner> corners;
        std::vector<std::uint32_t> local_indices;
        local_indices.reserve(part.num_triangles * 3);
        std::unordered_map<Corner, std::uint32_t, CornerHash> unique;
        unique.reserve(part.num_triangles * 2);
        for (std::size_t f = 0; f < part.face_indices.count; ++f) {
            const ufbx_face face = mesh.faces.data[part.face_indices.data[f]];
            if (face.num_indices < 3) continue;  // puntos y lineas sueltas
            const std::uint32_t count =
                ufbx_triangulate_face(triangle.data(), triangle.size(), &mesh, face);
            for (std::uint32_t k = 0; k < count * 3; ++k) {
                const std::uint32_t ix = triangle[k];
                Corner c{};
                c.logical = mesh.vertex_indices.data[ix];
                store(c.position, ufbx_get_vertex_vec3(&mesh.vertex_position, ix));
                if (has_normals) store(c.normal, ufbx_get_vertex_vec3(&mesh.vertex_normal, ix));
                if (has_uv) {
                    const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh.vertex_uv, ix);
                    c.uv[0] = static_cast<float>(uv.x);
                    c.uv[1] = 1.0f - static_cast<float>(uv.y);  // FlipUVs
                }
                if (has_tangents) {
                    store(c.tangent, ufbx_get_vertex_vec3(&mesh.vertex_tangent, ix));
                    store(c.bitangent, ufbx_get_vertex_vec3(&mesh.vertex_bitangent, ix));
                }
                if (has_color) {
                    const ufbx_vec4 color = ufbx_get_vertex_vec4(&mesh.vertex_color, ix);
                    c.color[0] = static_cast<float>(color.x);
                    c.color[1] = static_cast<float>(color.y);
                    c.color[2] = static_cast<float>(color.z);
                    c.color[3] = static_cast<float>(color.w);
                }
                const auto [it, inserted] = unique.try_emplace(c, static_cast<std::uint32_t>(corners.size()));
                if (inserted) corners.push_back(c);
                local_indices.push_back(it->second);
            }
        }
        if (local_indices.empty()) return;

        // --- Vertices ---
        std::vector<SkinnedVertex> vertices(corners.size());
        for (std::size_t v = 0; v < corners.size(); ++v) {
            const Corner& c = corners[v];
            SkinnedVertex& vertex = vertices[v];
            vertex.position = Vec3{c.position[0], c.position[1], c.position[2]};
            vertex.normal = has_normals ? Vec3{c.normal[0], c.normal[1], c.normal[2]} : Vec3{0.0f, 1.0f, 0.0f};
            vertex.uv = Vec2{c.uv[0], c.uv[1]};
            if (has_tangents) {
                // Con FlipUVs la bitangente cambia de signo (assimp la niega).
                const Vec3 tangent{c.tangent[0], c.tangent[1], c.tangent[2]};
                const Vec3 bitangent = Vec3{c.bitangent[0], c.bitangent[1], c.bitangent[2]} * -1.0f;
                const float handedness =
                    core::dot(core::cross(vertex.normal, tangent), bitangent) < 0.0f ? -1.0f : 1.0f;
                vertex.tangent = Vec4{tangent.x, tangent.y, tangent.z, handedness};
            }
        }
        if (!has_tangents && has_uv) computeTangents(vertices, local_indices);

        // Escenario: al espacio del modelo (PreTransformVertices).
        std::int32_t submesh_node = node_index;
        if (mode_ == Mode::Static) {
            // La raiz de assimp (ejes y UnitScaleFactor) por encima del mundo de ufbx.
            ufbx_matrix root{};
            for (int c = 0; c < 4; ++c) {
                root.cols[c].x = root_.m[c][0];
                root.cols[c].y = root_.m[c][1];
                root.cols[c].z = root_.m[c][2];
            }
            const ufbx_matrix m = ufbx_matrix_mul(&root, &node.geometry_to_world);
            const ufbx_matrix normal_matrix = ufbx_matrix_for_normals(&m);
            for (SkinnedVertex& vertex : vertices) {
                const ufbx_vec3 p = ufbx_transform_position(&m, toUfbx(vertex.position));
                vertex.position = toVec3(p);
                vertex.normal = normalized(toVec3(ufbx_transform_direction(
                    &normal_matrix, toUfbx(vertex.normal))));
                const Vec3 t = normalized(toVec3(ufbx_transform_direction(
                    &m, toUfbx(Vec3{vertex.tangent.x, vertex.tangent.y, vertex.tangent.z}))));
                vertex.tangent = Vec4{t.x, t.y, t.z, vertex.tangent.w};
            }
        }

        // --- Material ---
        const std::uint32_t material = materialIndex(partMaterial(node, mesh, part.index));
        if (mode_ == Mode::Static) submesh_node = staticNode(material);

        const auto base_vertex = static_cast<std::uint32_t>(out_.vertices.size());
        // Escenario y piezas: todo al hueso de la raiz (uno solo).
        const std::uint32_t fallback_bone = nodeBone(mode_ == Mode::Animated ? node_index : 0);

        // --- Influencias (las 4 de mas peso, normalizadas) ---
        if (skin != nullptr) {
            for (std::size_t v = 0; v < corners.size(); ++v) {
                const std::uint32_t logical = corners[v].logical;
                if (logical >= skin->vertices.count) continue;
                const ufbx_skin_vertex& sv = skin->vertices.data[logical];
                for (std::uint32_t w = 0; w < sv.num_weights; ++w) {
                    const ufbx_skin_weight& weight = skin->weights.data[sv.weight_begin + w];
                    if (!(weight.weight > 0.0) || weight.cluster_index >= skin->clusters.count) continue;
                    const ufbx_skin_cluster& cluster = *skin->clusters.data[weight.cluster_index];
                    if (cluster.bone_node == nullptr) continue;
                    addInfluence(vertices[v], boneFor(cluster), static_cast<float>(weight.weight));
                }
            }
        }
        for (SkinnedVertex& vertex : vertices) {
            float sum = 0.0f;
            for (float weight : vertex.weights) sum += weight;
            if (sum <= 0.0f) {
                vertex.joints[0] = fallback_bone;
                vertex.weights[0] = 1.0f;
                for (std::size_t i = 1; i < kMaxBoneInfluences; ++i) {
                    vertex.joints[i] = 0;
                    vertex.weights[i] = 0.0f;
                }
                continue;
            }
            for (float& weight : vertex.weights) weight /= sum;
        }

        // --- Color de vertice (como el camino de assimp) ---
        if (has_color || !out_.vertex_colors.empty()) {
            std::vector<std::uint32_t>& colors = out_.vertex_colors.mut();
            colors.resize(base_vertex, 0xFFFFFFFFu);
            const auto byte = [](float c) {
                return static_cast<std::uint32_t>(std::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            for (const Corner& c : corners) {
                colors.push_back(has_color ? (byte(c.color[0]) | (byte(c.color[1]) << 8) | (byte(c.color[2]) << 16) |
                                              (byte(c.color[3]) << 24))
                                           : 0xFFFFFFFFu);
            }
        }

        // --- Formas de mezcla ---
        if (mode_ == Mode::Animated && importBlendShapes()) addBlendShapes(mesh, corners, base_vertex);

        std::vector<SkinnedVertex>& all = out_.vertices.mut();
        all.insert(all.end(), vertices.begin(), vertices.end());

        SubMesh submesh{};
        submesh.first_index = static_cast<std::uint32_t>(out_.indices.size());
        submesh.material = material;
        submesh.node = submesh_node;
        std::vector<std::uint32_t>& indices = out_.indices.mut();
        for (const std::uint32_t i : local_indices) indices.push_back(base_vertex + i);
        submesh.index_count = static_cast<std::uint32_t>(local_indices.size());
        out_.submeshes.push_back(submesh);
    }

    static Vec3 normalized(const Vec3& v) {
        const float length = core::length(v);
        return length > 1e-12f ? v * (1.0f / length) : Vec3{0.0f, 1.0f, 0.0f};
    }

    // Tangentes de las derivadas de la posicion respecto a las UV (ya
    // giradas), acumuladas y ortonormalizadas: el convenio de
    // aiProcess_CalcTangentSpace despues de FlipUVs (como el lector de OBJ).
    static void computeTangents(std::vector<SkinnedVertex>& vertices, const std::vector<std::uint32_t>& indices) {
        std::vector<Vec3> tangents(vertices.size(), Vec3{0.0f, 0.0f, 0.0f});
        std::vector<Vec3> bitangents(vertices.size(), Vec3{0.0f, 0.0f, 0.0f});
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
            const std::uint32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
            const Vec3 e1 = vertices[b].position - vertices[a].position;
            const Vec3 e2 = vertices[c].position - vertices[a].position;
            const float du1 = vertices[b].uv.x - vertices[a].uv.x;
            const float dv1 = vertices[b].uv.y - vertices[a].uv.y;
            const float du2 = vertices[c].uv.x - vertices[a].uv.x;
            const float dv2 = vertices[c].uv.y - vertices[a].uv.y;
            const float det = du1 * dv2 - du2 * dv1;
            if (std::abs(det) < 1e-12f) continue;
            const float r = 1.0f / det;
            const Vec3 t = (e1 * dv2 - e2 * dv1) * r;
            const Vec3 bt = (e2 * du1 - e1 * du2) * r;
            for (const std::uint32_t v : {a, b, c}) {
                tangents[v] += t;
                bitangents[v] += bt;
            }
        }
        for (std::size_t v = 0; v < vertices.size(); ++v) {
            SkinnedVertex& vertex = vertices[v];
            Vec3 t = tangents[v] - vertex.normal * core::dot(vertex.normal, tangents[v]);
            const float length = core::length(t);
            if (length < 1e-12f) {
                vertex.tangent = Vec4{1.0f, 0.0f, 0.0f, 1.0f};
                continue;
            }
            t = t * (1.0f / length);
            const float handedness = core::dot(core::cross(vertex.normal, t), bitangents[v]) < 0.0f ? -1.0f : 1.0f;
            vertex.tangent = Vec4{t.x, t.y, t.z, handedness};
        }
    }

    static void addInfluence(SkinnedVertex& vertex, std::uint32_t bone, float weight) {
        // Si el hueso ya esta (dos entradas con el mismo offset), se suma.
        for (std::size_t i = 0; i < kMaxBoneInfluences; ++i) {
            if (vertex.weights[i] > 0.0f && vertex.joints[i] == bone) {
                vertex.weights[i] += weight;
                return;
            }
        }
        std::size_t slot = 0;
        for (std::size_t i = 1; i < kMaxBoneInfluences; ++i) {
            if (vertex.weights[i] < vertex.weights[slot]) slot = i;
        }
        if (weight > vertex.weights[slot]) {
            vertex.joints[slot] = bone;
            vertex.weights[slot] = weight;
        }
    }

    // Escenario: un nodo hijo de la raiz por material (los de
    // PreTransformVertices).
    std::int32_t staticNode(std::uint32_t material) {
        const auto it = static_nodes_.find(material);
        if (it != static_nodes_.end()) return it->second;
        const auto index = static_cast<std::int32_t>(out_.nodes.size());
        out_.nodes.push_back(Node{out_.materials[material].name, 0, Mat4::identity()});
        node_bone_.push_back(-1);
        static_nodes_.emplace(material, index);
        return index;
    }

    // --- Formas de mezcla ----------------------------------------------------

    void addBlendShapes(const ufbx_mesh& mesh, const std::vector<Corner>& corners, std::uint32_t base_vertex) {
        for (std::size_t d = 0; d < mesh.blend_deformers.count; ++d) {
            const ufbx_blend_deformer& deformer = *mesh.blend_deformers.data[d];
            for (std::size_t c = 0; c < deformer.channels.count; ++c) {
                const ufbx_blend_channel& channel = *deformer.channels.data[c];
                const ufbx_blend_shape* shape = channel.target_shape;
                if (shape == nullptr || shape->num_offsets == 0) continue;
                const std::uint32_t index = shapeFor(channel);
                BlendShape& out = out_.blend_shapes[index];
                const bool normals = shape->normal_offsets.count == shape->num_offsets;
                for (std::size_t v = 0; v < corners.size(); ++v) {
                    const std::uint32_t offset = ufbx_get_blend_shape_offset_index(shape, corners[v].logical);
                    if (offset == UFBX_NO_INDEX || offset >= shape->position_offsets.count) continue;
                    const Vec3 delta = toVec3(shape->position_offsets.data[offset]);
                    const Vec3 normal = normals ? toVec3(shape->normal_offsets.data[offset]) : Vec3{0.0f, 0.0f, 0.0f};
                    if (core::dot(delta, delta) == 0.0f && core::dot(normal, normal) == 0.0f) continue;
                    // Las de otras mallas ya guardadas no tienen normales: se
                    // rellenan con ceros para que vayan a la par.
                    if (normals && out.normal_deltas.size() < out.position_deltas.size()) {
                        out.normal_deltas.resize(out.position_deltas.size(), Vec3{0.0f, 0.0f, 0.0f});
                    }
                    out.vertices.push_back(base_vertex + static_cast<std::uint32_t>(v));
                    out.position_deltas.push_back(delta);
                    if (normals || !out.normal_deltas.empty()) out.normal_deltas.push_back(normal);
                }
            }
        }
    }

    // La forma del canal: una por nombre en todo el modelo.
    std::uint32_t shapeFor(const ufbx_blend_channel& channel) {
        std::string name = str(channel.name);
        if (name.empty() && channel.target_shape != nullptr) name = str(channel.target_shape->name);
        const auto [it, inserted] = shape_index_.try_emplace(name, static_cast<std::uint32_t>(out_.blend_shapes.size()));
        if (inserted) {
            BlendShape shape{};
            shape.name = name;
            shape.default_weight = static_cast<float>(channel.weight);
            out_.blend_shapes.push_back(std::move(shape));
        }
        channel_shape_[channel.element_id] = it->second;
        return it->second;
    }

    // --- Materiales y texturas -----------------------------------------------

    std::uint32_t materialIndex(const ufbx_material* material) {
        if (material == nullptr) {
            // El "DefaultMaterial" de assimp (difuso 0.8).
            if (default_material_ < 0) {
                MaterialData data{};
                data.name = "DefaultMaterial";
                data.base_color = Vec4{0.8f, 0.8f, 0.8f, 1.0f};
                default_material_ = static_cast<std::int32_t>(out_.materials.size());
                out_.materials.push_back(data);
            }
            return static_cast<std::uint32_t>(default_material_);
        }
        const auto it = material_index_.find(material->typed_id);
        if (it != material_index_.end()) return it->second;
        const auto index = static_cast<std::uint32_t>(out_.materials.size());
        out_.materials.push_back(convertMaterial(*material));
        material_index_.emplace(material->typed_id, index);
        return index;
    }

    // La textura conectada a la propiedad: como assimp, si hay varias del
    // mismo tipo gana la ultima de la lista.
    static const ufbx_texture* findTexture(const ufbx_material& material, std::initializer_list<const char*> props) {
        const ufbx_texture* found = nullptr;
        for (const char* prop : props) {
            const std::size_t length = std::strlen(prop);
            for (std::size_t i = 0; i < material.textures.count; ++i) {
                const ufbx_material_texture& entry = material.textures.data[i];
                if (entry.texture != nullptr && entry.material_prop.length == length &&
                    std::memcmp(entry.material_prop.data, prop, length) == 0) {
                    found = entry.texture;
                    break;
                }
            }
        }
        return found;
    }

    static const ufbx_texture* fileTexture(const ufbx_texture* texture) {
        if (texture == nullptr) return nullptr;
        if (texture->type != UFBX_TEXTURE_FILE && texture->file_textures.count > 0) {
            return texture->file_textures.data[0];
        }
        return texture;
    }

    static std::string texturePath(const ufbx_texture* texture) {
        texture = fileTexture(texture);
        if (texture == nullptr) return {};
        std::string path = str(texture->relative_filename);
        if (path.empty()) path = str(texture->filename);
        if (path.empty()) path = str(texture->absolute_filename);
        return path;
    }

    MaterialData convertMaterial(const ufbx_material& material) {
        MaterialData data{};
        data.name = str(material.name);
        const ufbx_props& props = material.props;

        // Los tipos de textura que mira el camino de assimp, con las
        // propiedades de FBX que assimp asigna a cada uno.
        data.albedo_texture = loadTexture(findTexture(material, {"Maya|baseColor", "Maya|TEX_color_map",
                                                                 "3dsMax|Parameters|base_color_map",
                                                                 "3dsMax|main|base_color_map"}));
        if (data.albedo_texture < 0) {
            data.albedo_texture = loadTexture(findTexture(material, {"DiffuseColor", "Maya|DiffuseTexture"}));
        }
        data.metallic_roughness_texture = loadTexture(findTexture(
            material, {"ReflectionFactor", "Maya|metalness", "Maya|TEX_metallic_map", "3dsMax|Parameters|metalness_map",
                       "3dsMax|main|metalness_map"}));
        if (data.metallic_roughness_texture < 0) {
            data.metallic_roughness_texture = loadTexture(findTexture(
                material, {"Maya|diffuseRoughness", "Maya|TEX_roughness_map", "3dsMax|Parameters|roughness_map",
                           "3dsMax|main|roughness_map"}));
        }
        bool orca_specular = false;
        if (data.metallic_roughness_texture < 0) {
            // Convenio ORCA: "_Specular" = G rugosidad, B metal.
            const ufbx_texture* specular = findTexture(
                material, {"SpecularColor", "SpecularFactor", "Maya|SpecularTexture", "3dsMax|main|specular_map"});
            const std::string file = std::filesystem::path(texturePath(specular)).filename().string();
            if (specular != nullptr && containsNoCase(file, "_specular")) {
                data.metallic_roughness_texture = loadTexture(specular);
                orca_specular = data.metallic_roughness_texture >= 0;
            }
        }
        data.normal_texture = loadTexture(findTexture(material, {"NormalMap", "Maya|NormalTexture"}));
        if (data.normal_texture < 0) {
            // "Bump" solo si es un normal map de verdad (prefijo N_ o "normal").
            const ufbx_texture* bump = findTexture(material, {"Bump"});
            const std::string file = std::filesystem::path(texturePath(bump)).filename().string();
            const bool prefixed = file.size() >= 2 && std::toupper(static_cast<unsigned char>(file[0])) == 'N' &&
                                  file[1] == '_';
            if (bump != nullptr && (prefixed || containsNoCase(file, "normal"))) data.normal_texture = loadTexture(bump);
        }
        data.occlusion_texture = loadTexture(findTexture(material, {"Maya|TEX_ao_map", "3dsMax|main|ao_map"}));
        data.emissive_texture = loadTexture(findTexture(material, {"EmissiveColor", "EmissiveFactor"}));

        // Factores: los de SetShadingPropertiesCommon de assimp.
        bool has_metallic = false;
        if (const ufbx_prop* p = ownProp(props, "Maya|metallic")) {
            data.metallic = static_cast<float>(p->value_real);
            has_metallic = true;
        }
        bool has_roughness = false;
        if (const ufbx_prop* p = ownProp(props, "ShininessExponent")) {
            data.roughness = 1.0f - std::sqrt(static_cast<float>(p->value_real)) / 10.0f;
            has_roughness = true;
        }
        if (const ufbx_prop* p = ownProp(props, "Maya|roughness")) {
            data.roughness = static_cast<float>(p->value_real);
            has_roughness = true;
        }
        if (!has_roughness && orca_specular) {
            data.roughness = 1.0f;
            data.metallic = 1.0f;
        } else if (!has_roughness) {
            data.roughness = 0.8f;  // roughnessFromPhong sin brillo
        }
        (void)has_metallic;

        Vec3 emissive{0.0f, 0.0f, 0.0f};
        if (const ufbx_prop* color = templateProp(props, "EmissiveColor")) {
            emissive = toVec3(color->value_vec3);
            if (const ufbx_prop* factor = templateProp(props, "EmissiveFactor")) {
                emissive = emissive * static_cast<float>(factor->value_real);
            }
        } else if (const ufbx_prop* maya = ownProp(props, "Maya|emissive")) {
            emissive = toVec3(maya->value_vec3);
        }
        data.emissive = emissive;
        if (data.emissive_texture >= 0 && data.emissive.x <= 0.0f && data.emissive.y <= 0.0f &&
            data.emissive.z <= 0.0f) {
            data.emissive = Vec3{1.0f, 1.0f, 1.0f};
        }

        if (data.albedo_texture < 0) {
            if (const ufbx_prop* base = ownProp(props, "Maya|base_color")) {
                const Vec3 c = toVec3(base->value_vec3);
                data.base_color = Vec4{c.x, c.y, c.z, 1.0f};
            } else if (const ufbx_prop* diffuse = templateProp(props, "DiffuseColor")) {
                Vec3 c = toVec3(diffuse->value_vec3);
                if (const ufbx_prop* factor = templateProp(props, "DiffuseFactor")) {
                    c = c * static_cast<float>(factor->value_real);
                }
                data.base_color = Vec4{c.x, c.y, c.z, 1.0f};
            }
        }

        // Opacidad: "Opacity" o 1 - media(TransparentColor * TransparencyFactor).
        float opacity = 1.0f;
        if (const ufbx_prop* p = ownProp(props, "Opacity")) {
            opacity = static_cast<float>(p->value_real);
        } else if (const ufbx_prop* t = templateProp(props, "TransparentColor")) {
            Vec3 c = toVec3(t->value_vec3);
            if (const ufbx_prop* factor = templateProp(props, "TransparencyFactor")) {
                c = c * static_cast<float>(factor->value_real);
            }
            opacity = 1.0f - (c.x + c.y + c.z) / 3.0f;
        }
        if (opacity < 0.99f) data.transparent = true;
        return data;
    }

    std::int32_t loadTexture(const ufbx_texture* source) {
        const ufbx_texture* texture = fileTexture(source);
        if (texture == nullptr) return -1;
        const std::string relative = str(texture->relative_filename);
        const std::string absolute = str(texture->filename);
        std::string key = !relative.empty() ? relative : absolute;
        const bool embedded = texture->content.size > 0 ||
                              (texture->video != nullptr && texture->video->content.size > 0);
        if (key.empty()) {
            if (!embedded) return -1;
            key = "*" + std::to_string(texture->typed_id);
        }
        if (const auto it = texture_index_.find(key); it != texture_index_.end()) return it->second;

        TextureData data{};
        data.name = std::filesystem::path(key).filename().string();
        bool loaded = false;
        // 1) Incrustada en el archivo.
        if (embedded) {
            const ufbx_blob& blob = texture->content.size > 0 ? texture->content : texture->video->content;
            const auto* bytes = static_cast<const std::uint8_t*>(blob.data);
            data.encoded.assign(bytes, bytes + blob.size);
            loaded = !data.encoded.empty();
        }
        // 2) Archivo externo: la ruta relativa al modelo, la absoluta del
        //    artista o solo el nombre junto al modelo (o en textures/).
        if (!loaded) {
            std::vector<std::filesystem::path> candidates;
            const std::filesystem::path rel = fromUtf8(relative);
            const std::filesystem::path abs = fromUtf8(absolute);
            if (!relative.empty()) candidates.push_back(directory_ / rel);
            if (!absolute.empty()) candidates.push_back(abs);
            const std::filesystem::path name = fromUtf8(key).filename();
            candidates.push_back(directory_ / name);
            candidates.push_back(directory_ / "textures" / name);
            for (const std::filesystem::path& candidate : candidates) {
                std::error_code error;
                if (std::filesystem::is_regular_file(candidate, error)) {
                    data.source_path = candidate.string();
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
        out_.textures.push_back(std::move(data));
        texture_index_.emplace(key, index);
        return index;
    }

    // --- Animaciones ---------------------------------------------------------

    void addAnimations() {
        for (std::size_t s = 0; s < scene_.anim_stacks.count; ++s) {
            const ufbx_anim_stack& stack = *scene_.anim_stacks.data[s];
            if (!stackHasCurves(stack)) continue;
            const BakePtr baked = bake(scene_, stack);
            if (!baked) continue;
            double begin = 0.0, end = 0.0;
            clipRange(stack, *baked, begin, end);

            AnimationClip clip{};
            clip.name = str(stack.name);
            clip.duration = static_cast<float>(end - begin);
            addNodeChannels(*baked, begin, 1.0f,
                            [this](std::uint32_t typed_id) {
                                return typed_id < node_index_of_typed_.size() ? node_index_of_typed_[typed_id] : -1;
                            },
                            clip, out_.name);
            // Pesos de las formas: la propiedad DeformPercent (0..100) de cada canal.
            for (std::size_t e = 0; e < baked->elements.count; ++e) {
                const ufbx_baked_element& element = baked->elements.data[e];
                const auto shape = channel_shape_.find(element.element_id);
                if (shape == channel_shape_.end()) continue;
                for (std::size_t p = 0; p < element.props.count; ++p) {
                    const ufbx_baked_prop& prop = element.props.data[p];
                    if (str(prop.name) != "DeformPercent" || prop.keys.count == 0) continue;
                    BlendShapeTrack track{};
                    track.shape = shape->second;
                    for (std::size_t k = 0; k < prop.keys.count; ++k) {
                        const ufbx_baked_vec3& key = prop.keys.data[k];
                        if (!std::isfinite(key.time) || !std::isfinite(key.value.x)) continue;
                        track.keys.push_back({static_cast<float>(key.time - begin), static_cast<float>(key.value.x / 100.0)});
                    }
                    if (!track.keys.empty()) clip.blend_tracks.push_back(std::move(track));
                }
            }
            if (clip.channels.empty() && clip.blend_tracks.empty()) continue;
            // Robustez (lo que hacia repairAnimations): la duracion cubre las claves.
            float last = 0.0f;
            for (const AnimationChannel& c : clip.channels) {
                if (!c.positions.empty()) last = std::max(last, c.positions.back().time);
                if (!c.rotations.empty()) last = std::max(last, c.rotations.back().time);
                if (!c.scales.empty()) last = std::max(last, c.scales.back().time);
            }
            if (!(clip.duration > 0.0f)) clip.duration = last;
            out_.animations.push_back(std::move(clip));
        }
    }

    const ufbx_scene& scene_;
    std::filesystem::path directory_;
    ModelData& out_;
    Mode mode_;
    Mat4 root_;  // la del nodo raiz de assimp

    std::vector<const ufbx_node*> order_;            // nodos en el orden del motor
    std::vector<std::int32_t> node_index_of_typed_;  // typed_id de ufbx -> nodo del motor
    std::vector<std::int32_t> node_bone_;            // nodo -> su hueso rigido (-1 = aun no)
    std::unordered_multimap<std::string, std::uint32_t> bone_index_;
    std::unordered_map<std::uint32_t, std::uint32_t> material_index_;
    std::unordered_map<std::uint32_t, std::int32_t> static_nodes_;
    std::unordered_map<std::string, std::int32_t> texture_index_;
    std::unordered_map<std::string, std::uint32_t> shape_index_;
    std::unordered_map<std::uint32_t, std::uint32_t> channel_shape_;  // element_id del canal -> forma
    std::unordered_set<std::uint32_t> skinned_meshes_done_;
    std::int32_t default_material_ = -1;
};

// Si el archivo se importa animado (sin hornear): lo mismo que mira
// importWithAssimp.
bool isAnimatedScene(const ufbx_scene& scene) {
    bool meshes = false;
    for (std::size_t m = 0; m < scene.meshes.count; ++m) {
        const ufbx_mesh& mesh = *scene.meshes.data[m];
        if (mesh.num_triangles == 0 || mesh.instances.count == 0) continue;
        meshes = true;
        if (mesh.skin_deformers.count > 0 && mesh.skin_deformers.data[0]->clusters.count > 0) return true;
    }
    bool any_stack = false;
    for (std::size_t s = 0; s < scene.anim_stacks.count; ++s) {
        const ufbx_anim_stack& stack = *scene.anim_stacks.data[s];
        if (!stackHasCurves(stack)) continue;
        any_stack = true;
        for (std::size_t l = 0; l < stack.layers.count; ++l) {
            const ufbx_anim_layer& layer = *stack.layers.data[l];
            for (std::size_t p = 0; p < layer.anim_props.count; ++p) {
                const ufbx_element* element = layer.anim_props.data[p].element;
                if (element == nullptr || element->type != UFBX_ELEMENT_NODE) continue;
                if (subtreeHasMeshes(reinterpret_cast<const ufbx_node*>(element))) return true;
            }
        }
    }
    return !meshes && any_stack;
}

}  // namespace

ModelData importFbxWithUfbx(const std::filesystem::path& path, bool force_static, bool keep_hierarchy,
                            const ImportProgressCallback& on_progress) {
    const auto start = std::chrono::steady_clock::now();
    std::string error;
    const ScenePtr scene = loadScene(path, /*geometry=*/true, /*embedded=*/true, &on_progress, error);
    if (!scene) {
        throw std::runtime_error("No se pudo cargar el modelo " + utf8(path) + " (ufbx): " + error);
    }
    std::cout << "[Modelo]   lectura de ufbx: "
              << std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count() << " s\n";

    ModelData model{};
    model.name = path.filename().string();
    Converter::Mode mode = Converter::Mode::Hierarchy;
    if (!keep_hierarchy) {
        const bool animated = !force_static && isAnimatedScene(*scene);
        mode = animated ? Converter::Mode::Animated : Converter::Mode::Static;
    }
    Converter(*scene, path.parent_path(), model, mode).run();
    if (mode == Converter::Mode::Static) {
        clusterSubmeshes(model);  // (tambien las cajas)
    } else {
        computeSubmeshBounds(model);
    }
    if (on_progress) on_progress(1.0f);
    return model;
}

float ufbxFileUnitScale(const std::filesystem::path& path) {
    std::string error;
    const ScenePtr scene = loadScene(path, /*geometry=*/false, /*embedded=*/false, nullptr, error);
    return scene ? unitScale(*scene) : 1.0f;
}

std::vector<AnimationClip> ufbxLoadAnimationClips(const std::filesystem::path& path, const std::vector<Node>& target,
                                                  std::string* error, float scale, bool convert_units) {
    std::vector<AnimationClip> clips;
    std::string load_error;
    const ScenePtr scene = loadScene(path, /*geometry=*/false, /*embedded=*/false, nullptr, load_error);
    bool any = false;
    if (scene) {
        for (std::size_t s = 0; s < scene->anim_stacks.count && !any; ++s) {
            any = stackHasCurves(*scene->anim_stacks.data[s]);
        }
    }
    if (!scene || !any) {
        if (error != nullptr) *error = !scene ? load_error : std::string("el archivo no tiene animaciones");
        return clips;
    }
    const float position_scale = scale * (convert_units ? unitScale(*scene) : 1.0f);
    std::unordered_map<std::string, std::int32_t> by_name;
    for (std::size_t i = 0; i < target.size(); ++i) by_name.emplace(target[i].name, static_cast<std::int32_t>(i));
    for (std::size_t s = 0; s < scene->anim_stacks.count; ++s) {
        const ufbx_anim_stack& stack = *scene->anim_stacks.data[s];
        if (!stackHasCurves(stack)) continue;
        const BakePtr baked = bake(*scene, stack);
        if (!baked) continue;
        double begin = 0.0, end = 0.0;
        clipRange(stack, *baked, begin, end);
        AnimationClip clip{};
        clip.name = str(stack.name);
        clip.duration = static_cast<float>(end - begin);
        const ufbx_scene& sc = *scene;
        addNodeChannels(*baked, begin, position_scale,
                        [&](std::uint32_t typed_id) -> std::int32_t {
                            if (typed_id >= sc.nodes.count) return -1;
                            const ufbx_node* node = sc.nodes.data[typed_id];
                            if (node->is_root) return -1;
                            const auto it = by_name.find(str(node->name));
                            return it != by_name.end() ? it->second : -1;
                        },
                        clip, path.filename().string());
        if (!clip.channels.empty()) clips.push_back(std::move(clip));
    }
    if (clips.empty() && error != nullptr) *error = "ninguna pista coincide con los huesos del personaje";
    return clips;
}

}  // namespace cramion::asset::detail

namespace cramion::asset {

int ufbxSourceIsAnimated(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".fbx" || fbxLoader() != FbxLoader::Ufbx) return -1;
    std::string error;
    const detail::ScenePtr scene = detail::loadScene(path, /*geometry=*/true, /*embedded=*/false, nullptr, error);
    if (!scene) return 0;
    for (std::size_t s = 0; s < scene->anim_stacks.count; ++s) {
        if (detail::stackHasCurves(*scene->anim_stacks.data[s])) return 1;
    }
    // Piel con al menos 2 huesos distintos que pesen.
    std::unordered_set<std::string> bones;
    for (std::size_t c = 0; c < scene->skin_clusters.count; ++c) {
        const ufbx_skin_cluster& cluster = *scene->skin_clusters.data[c];
        if (cluster.bone_node == nullptr || cluster.num_weights == 0) continue;
        bones.insert(detail::str(cluster.bone_node->name));
        if (bones.size() >= 2) return 1;
    }
    return 0;
}

}  // namespace cramion::asset
