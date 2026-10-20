#include "CramionCore/asset/Importer.h"

#include "CrData.h"

#include <CramionFX/asset/Model.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <unordered_set>

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/scene.h>

namespace cramion::assets {

namespace {

using asset::ModelData;
using core::Mat4;
using core::Vec3;
using core::Vec4;

// Mas piezas que esto (o texturas muy repetidas entre piezas) y el modelo se
// agrupa por material en lugar de por nodo/objeto. El renderizador sube las
// texturas de cada pieza por separado: miles de piezas que comparten las
// mismas texturas (Bistro, San Miguel) multiplicarian la memoria de video, y
// cada pieza es ademas un modelo mas que subir y dibujar.
constexpr std::size_t kMaxNodeParts = 256;
// Bytes de textura de todas las piezas / bytes de las texturas distintas.
constexpr double kMaxTextureDuplication = 1.5;

// Variable de entorno (vacia si no existe). _dupenv_s: getenv esta marcada
// como insegura en el CRT de Windows.
std::string environmentVariable(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t size = 0;
    std::string result;
    if (_dupenv_s(&value, &size, name) == 0 && value != nullptr) {
        result = value;
    }
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

Vec3 transformPoint(const Mat4& m, const Vec3& p) {
    const Vec4 r = m * Vec4{p.x, p.y, p.z, 1.0f};
    return Vec3{r.x, r.y, r.z};
}

Vec3 transformDirection(const Mat4& m, const Vec3& d) {
    const Vec4 r = m * Vec4{d.x, d.y, d.z, 0.0f};
    return Vec3{r.x, r.y, r.z};
}

std::size_t textureBytes(const asset::TextureData& texture) {
    return texture.encoded.size() + texture.pixels.size();
}

// Una pieza con los triangulos de las submallas `selected` del modelo
// original: solo sus vertices, materiales y texturas (reindexados), un unico
// hueso identidad y un nodo raiz. `to_local` se aplica a los vertices (p. ej.
// para mover el pivote).
ModelData extractPart(const ModelData& source, const std::vector<std::size_t>& selected,
                      const std::string& name, const Mat4* to_local) {
    ModelData part{};
    part.name = name;
    part.nodes.push_back(asset::Node{name, -1, Mat4::identity()});
    part.bones.push_back(asset::Bone{name, 0, Mat4::identity()});

    std::unordered_map<std::uint32_t, std::uint32_t> vertex_map;
    std::unordered_map<std::uint32_t, std::uint32_t> material_map;
    std::unordered_map<std::int32_t, std::int32_t> texture_map;

    const auto texture_of = [&](std::int32_t texture) -> std::int32_t {
        if (texture < 0 || static_cast<std::size_t>(texture) >= source.textures.size()) {
            return -1;
        }
        const auto [it, inserted] =
            texture_map.try_emplace(texture, static_cast<std::int32_t>(part.textures.size()));
        if (inserted) {
            part.textures.push_back(source.textures[static_cast<std::size_t>(texture)]);
        }
        return it->second;
    };

    for (const std::size_t s : selected) {
        const asset::SubMesh& submesh = source.submeshes[s];
        const auto [material_it, new_material] = material_map.try_emplace(
            submesh.material, static_cast<std::uint32_t>(part.materials.size()));
        if (new_material) {
            asset::MaterialData material = source.materials[submesh.material];
            material.albedo_texture = texture_of(material.albedo_texture);
            material.metallic_roughness_texture = texture_of(material.metallic_roughness_texture);
            material.normal_texture = texture_of(material.normal_texture);
            material.occlusion_texture = texture_of(material.occlusion_texture);
            material.emissive_texture = texture_of(material.emissive_texture);
            part.materials.push_back(std::move(material));
        }

        asset::SubMesh out{};
        out.first_index = static_cast<std::uint32_t>(part.indices.size());
        out.material = material_it->second;
        out.node = 0;
        for (std::uint32_t i = 0; i < submesh.index_count; ++i) {
            const std::uint32_t original = source.indices[submesh.first_index + i];
            const auto [vertex_it, new_vertex] = vertex_map.try_emplace(
                original, static_cast<std::uint32_t>(part.vertices.size()));
            if (new_vertex) {
                asset::SkinnedVertex vertex = source.vertices[original];
                if (to_local != nullptr) {
                    vertex.position = transformPoint(*to_local, vertex.position);
                    vertex.normal = core::normalize(transformDirection(*to_local, vertex.normal));
                    const Vec3 t = core::normalize(transformDirection(
                        *to_local, Vec3{vertex.tangent.x, vertex.tangent.y, vertex.tangent.z}));
                    vertex.tangent = Vec4{t.x, t.y, t.z, vertex.tangent.w};
                }
                // Un solo hueso: el de la pieza.
                vertex.joints[0] = 0;
                vertex.joints[1] = vertex.joints[2] = vertex.joints[3] = 0;
                vertex.weights[0] = 1.0f;
                vertex.weights[1] = vertex.weights[2] = vertex.weights[3] = 0.0f;
                part.vertices.push_back(vertex);
                // Su color de vertice, si el modelo lo trae.
                if (source.vertex_colors.size() == source.vertices.size()) {
                    part.vertex_colors.push_back(source.vertex_colors.get()[original]);
                }
            }
            part.indices.push_back(vertex_it->second);
        }
        out.index_count = static_cast<std::uint32_t>(part.indices.size()) - out.first_index;
        part.submeshes.push_back(out);
    }

    // Clusteres de 5 m para el culling (y cajas), por pieza.
    asset::clusterSubmeshes(part);
    return part;
}

// Caja de las submallas elegidas (espacio del modelo).
void boundsOf(const ModelData& source, const std::vector<std::size_t>& selected, Vec3& low,
              Vec3& high) {
    low = Vec3{1e30f, 1e30f, 1e30f};
    high = Vec3{-1e30f, -1e30f, -1e30f};
    for (const std::size_t s : selected) {
        const asset::SubMesh& submesh = source.submeshes[s];
        for (std::uint32_t i = 0; i < submesh.index_count; ++i) {
            const Vec3& p = source.vertices[source.indices[submesh.first_index + i]].position;
            low = Vec3{std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
            high = Vec3{std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
        }
    }
}

// Transform global de cada nodo (los padres van antes que los hijos).
std::vector<Mat4> globalTransforms(const ModelData& model) {
    std::vector<Mat4> global(model.nodes.size(), Mat4::identity());
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const asset::Node& node = model.nodes[i];
        global[i] = node.parent >= 0 ? global[static_cast<std::size_t>(node.parent)] * node.local
                                     : node.local;
    }
    return global;
}

// Piezas por material, con todos los nodos horneados en el espacio del
// modelo y el pivote de cada pieza en el centro de su caja. Para modelos con
// demasiados objetos o con texturas muy compartidas.
void splitByMaterial(const ModelData& source, bool hierarchy_space, crdata::ModelContent& out) {
    // En modo jerarquia (assimp) los vertices estan en el espacio de su
    // nodo: se llevan al del modelo con el transform global del nodo.
    const std::vector<Mat4> global = globalTransforms(source);

    std::vector<std::vector<std::size_t>> by_material(source.materials.size());
    for (std::size_t s = 0; s < source.submeshes.size(); ++s) {
        by_material[source.submeshes[s].material].push_back(s);
    }

    out.nodes.clear();
    out.parts.clear();
    out.nodes.push_back(ModelNode{source.name, -1, Mat4::identity(), -1});

    for (std::size_t m = 0; m < by_material.size(); ++m) {
        if (by_material[m].empty()) {
            continue;
        }
        // Submallas agrupadas por nodo para hornear cada una con el suyo.
        ModelData baked = source;
        if (hierarchy_space) {
            std::vector<bool> done(baked.vertices.size(), false);
            for (const std::size_t s : by_material[m]) {
                const asset::SubMesh& submesh = source.submeshes[s];
                const Mat4& to_model =
                    submesh.node >= 0 && static_cast<std::size_t>(submesh.node) < global.size()
                        ? global[static_cast<std::size_t>(submesh.node)]
                        : Mat4::identity();
                for (std::uint32_t i = 0; i < submesh.index_count; ++i) {
                    const std::uint32_t v = source.indices[submesh.first_index + i];
                    if (done[v]) {
                        continue;
                    }
                    done[v] = true;
                    asset::SkinnedVertex& vertex = baked.vertices[v];
                    vertex.position = transformPoint(to_model, vertex.position);
                    vertex.normal = core::normalize(transformDirection(to_model, vertex.normal));
                    const Vec3 t = core::normalize(transformDirection(
                        to_model, Vec3{vertex.tangent.x, vertex.tangent.y, vertex.tangent.z}));
                    vertex.tangent = Vec4{t.x, t.y, t.z, vertex.tangent.w};
                }
            }
        }

        Vec3 low{};
        Vec3 high{};
        boundsOf(baked, by_material[m], low, high);
        const Vec3 center = (low + high) * 0.5f;
        const Mat4 to_local = core::translate(Vec3{-center.x, -center.y, -center.z});

        const std::string name = source.materials[m].name.empty() ? "Material " + std::to_string(m)
                                                                  : source.materials[m].name;
        const auto part = static_cast<std::int32_t>(out.parts.size());
        out.parts.push_back(extractPart(baked, by_material[m], name, &to_local));
        out.nodes.push_back(ModelNode{name, 0, core::translate(center), part});
    }
}

// Piezas por nodo (assimp) u objeto (OBJ). Devuelve false si conviene agrupar
// por material (demasiadas piezas o texturas muy repetidas).
bool splitByNode(const ModelData& source, bool is_obj, crdata::ModelContent& out,
                 std::string& reason, std::size_t combine_above) {
    std::vector<std::vector<std::size_t>> by_node(source.nodes.size());
    for (std::size_t s = 0; s < source.submeshes.size(); ++s) {
        const std::int32_t node = source.submeshes[s].node;
        if (node >= 0 && static_cast<std::size_t>(node) < source.nodes.size()) {
            by_node[static_cast<std::size_t>(node)].push_back(s);
        } else {
            by_node[0].push_back(s);  // sin dato: a la raiz
        }
    }

    std::size_t part_count = 0;
    for (const auto& list : by_node) {
        part_count += list.empty() ? 0 : 1;
    }
    // El tope se puede subir con CRAMION_IMPORT_MAX_PARTS (jerarquia completa
    // aunque cueste mas memoria y llamadas de dibujo).
    std::size_t max_parts = std::min(kMaxNodeParts, combine_above);
    const std::string env = environmentVariable("CRAMION_IMPORT_MAX_PARTS");
    if (!env.empty()) {
        max_parts = static_cast<std::size_t>(std::max(1, std::atoi(env.c_str())));
    }
    if (part_count > max_parts) {
        reason = std::to_string(part_count) + " objetos (tope " + std::to_string(max_parts) + ")";
        return false;
    }

    // Texturas repetidas entre piezas: estimacion antes de copiar nada.
    std::size_t unique_bytes = 0;
    for (const asset::TextureData& texture : source.textures) {
        unique_bytes += textureBytes(texture);
    }
    std::size_t total_bytes = 0;
    for (const auto& list : by_node) {
        std::vector<bool> used(source.textures.size(), false);
        for (const std::size_t s : list) {
            const asset::MaterialData& material = source.materials[source.submeshes[s].material];
            for (const std::int32_t t : {material.albedo_texture, material.metallic_roughness_texture,
                                         material.normal_texture, material.occlusion_texture,
                                         material.emissive_texture}) {
                if (t >= 0 && static_cast<std::size_t>(t) < used.size() && !used[t]) {
                    used[t] = true;
                    total_bytes += textureBytes(source.textures[static_cast<std::size_t>(t)]);
                }
            }
        }
    }
    // Con el tope forzado a mano tampoco se mira la repeticion de texturas:
    // quien lo pide quiere la jerarquia completa.
    const bool forced = !environmentVariable("CRAMION_IMPORT_MAX_PARTS").empty();
    if (!forced && unique_bytes > 0 &&
        static_cast<double>(total_bytes) > kMaxTextureDuplication * static_cast<double>(unique_bytes)) {
        reason = "las texturas se repetirian x" +
                 std::to_string(static_cast<double>(total_bytes) / static_cast<double>(unique_bytes))
                     .substr(0, 4) +
                 " entre piezas";
        return false;
    }

    // Nodos que conservar: los que tienen malla y sus antepasados (los
    // demas, camaras, luces o huesos vacios, no aportan nada a la escena).
    std::vector<bool> keep(source.nodes.size(), false);
    for (std::size_t n = 0; n < source.nodes.size(); ++n) {
        if (by_node[n].empty()) {
            continue;
        }
        for (std::int32_t i = static_cast<std::int32_t>(n); i >= 0 && !keep[i];
             i = source.nodes[static_cast<std::size_t>(i)].parent) {
            keep[static_cast<std::size_t>(i)] = true;
        }
    }
    keep[0] = true;

    out.nodes.clear();
    out.parts.clear();
    std::vector<std::int32_t> new_index(source.nodes.size(), -1);
    for (std::size_t n = 0; n < source.nodes.size(); ++n) {
        if (!keep[n]) {
            continue;
        }
        const asset::Node& node = source.nodes[n];
        ModelNode result{};
        result.name = node.name.empty() ? "Nodo " + std::to_string(n) : node.name;
        result.parent = node.parent >= 0 ? new_index[static_cast<std::size_t>(node.parent)] : -1;
        result.local = node.local;

        if (!by_node[n].empty()) {
            if (is_obj && n != 0) {
                // OBJ: los vertices estan en el espacio del modelo; el pivote
                // de cada objeto va al centro de su caja (se rota sobre si
                // mismo, no sobre el origen del archivo).
                Vec3 low{};
                Vec3 high{};
                boundsOf(source, by_node[n], low, high);
                const Vec3 center = (low + high) * 0.5f;
                const Mat4 to_local = core::translate(Vec3{-center.x, -center.y, -center.z});
                result.local = core::translate(center);
                result.part = static_cast<std::int32_t>(out.parts.size());
                out.parts.push_back(extractPart(source, by_node[n], result.name, &to_local));
            } else {
                result.part = static_cast<std::int32_t>(out.parts.size());
                out.parts.push_back(extractPart(source, by_node[n], result.name, nullptr));
            }
        }
        new_index[n] = static_cast<std::int32_t>(out.nodes.size());
        out.nodes.push_back(std::move(result));
    }
    return true;
}

// --- LODs hechos a mano (mallas _LOD0.._LODn del archivo) ---

// Nivel de LOD por el nombre del nodo: "Roca_LOD1", "roca-lod2", "Roca LOD0"
// o "SM_Roca.LOD3" (sin distinguir mayusculas, como Unity y Unreal). -1 si no
// lo es; `base` = el nombre sin el sufijo, en minusculas.
int lodLevelOf(const std::string& name, std::string& base) {
    const std::string l = lower(name);
    std::size_t digits = l.size();
    while (digits > 0 && std::isdigit(static_cast<unsigned char>(l[digits - 1])) != 0) --digits;
    if (digits == l.size() || l.size() - digits > 2 || digits < 5) return -1;
    if (l.compare(digits - 3, 3, "lod") != 0) return -1;
    const char separator = l[digits - 4];
    if (separator != '_' && separator != '-' && separator != ' ' && separator != '.') return -1;
    base = l.substr(0, digits - 4);
    return std::atoi(l.c_str() + digits);
}

// Los niveles 1..n de una malla con LODs a mano, ya como piezas en el
// espacio de su LOD0.
struct ManualLodGroup {
    std::size_t lod0 = 0;  // nodo de LOD0 (en el modelo de assimp / OBJ)
    std::vector<ModelData> levels;
};

// Saca de `model` las mallas _LOD1.._LODn que tienen un LOD0 hermano (mismo
// padre y mismo nombre con _LOD0, o sin sufijo): quedan fuera de la
// jerarquia (no son objetos de la escena) y, si `keep`, se devuelven como
// piezas en el espacio de LOD0 (OBJ: con el mismo pivote que le da
// splitByNode). Sin `keep` (LOD Off) solo se descartan.
std::vector<ManualLodGroup> takeManualLods(ModelData& model, bool is_obj, bool keep) {
    std::vector<ManualLodGroup> result;
    if (model.nodes.empty()) return result;
    std::vector<std::vector<std::size_t>> by_node(model.nodes.size());
    for (std::size_t s = 0; s < model.submeshes.size(); ++s) {
        const std::int32_t node = model.submeshes[s].node;
        by_node[node >= 0 && static_cast<std::size_t>(node) < model.nodes.size() ? static_cast<std::size_t>(node) : 0]
            .push_back(s);
    }
    // (padre, nombre base) -> LOD0 y los demas niveles.
    struct Found {
        std::int64_t lod0 = -1;
        std::vector<std::pair<int, std::size_t>> levels;  // (nivel >= 1, nodo)
    };
    std::unordered_map<std::string, Found> found;
    std::unordered_map<std::string, std::size_t> plain;  // (padre, nombre) de las mallas sin sufijo
    for (std::size_t n = 0; n < model.nodes.size(); ++n) {
        if (by_node[n].empty()) continue;
        const std::string parent = std::to_string(model.nodes[n].parent) + "/";
        std::string base;
        const int level = lodLevelOf(model.nodes[n].name, base);
        if (level < 0) {
            plain.try_emplace(parent + lower(model.nodes[n].name), n);
            continue;
        }
        Found& f = found[parent + base];
        if (level == 0) {
            if (f.lod0 < 0) f.lod0 = static_cast<std::int64_t>(n);
        } else {
            f.levels.emplace_back(level, n);
        }
    }
    if (found.empty()) return result;

    std::vector<Mat4> global;
    if (!is_obj) global = globalTransforms(model);
    std::vector<bool> removed(model.submeshes.size(), false);
    std::size_t taken = 0;
    for (auto& [key, f] : found) {
        if (f.levels.empty()) continue;
        if (f.lod0 < 0) {
            // Sin _LOD0: la malla hermana con el nombre base.
            const auto it = plain.find(key);
            if (it == plain.end()) continue;  // no hay LOD0: se quedan como piezas normales
            f.lod0 = static_cast<std::int64_t>(it->second);
        }
        const auto lod0 = static_cast<std::size_t>(f.lod0);
        std::sort(f.levels.begin(), f.levels.end());
        ManualLodGroup group{};
        group.lod0 = lod0;
        // Al espacio de LOD0: OBJ, su pivote (el centro de su caja, como en
        // splitByNode); assimp, la inversa de su transform global por la del
        // nivel (los LODs pueden colgar de otro nodo o estar movidos).
        Mat4 obj_to_local = Mat4::identity();
        if (is_obj && lod0 != 0) {
            Vec3 low{};
            Vec3 high{};
            boundsOf(model, by_node[lod0], low, high);
            const Vec3 center = (low + high) * 0.5f;
            obj_to_local = core::translate(Vec3{-center.x, -center.y, -center.z});
        }
        int last_level = 0;
        for (const auto& [level, node] : f.levels) {
            for (const std::size_t s : by_node[node]) removed[s] = true;
            ++taken;
            if (!keep || level == last_level) continue;  // (dos mallas del mismo nivel: la primera)
            last_level = level;
            const Mat4 to_local = is_obj ? obj_to_local : core::inverse(global[lod0]) * global[node];
            ModelData part = extractPart(model, by_node[node], model.nodes[node].name, &to_local);
            // Sus texturas no hacen falta: al cargarlo se usan los materiales
            // de LOD0 (por nombre, asset::setManualLods).
            part.textures.clear();
            for (asset::MaterialData& material : part.materials) {
                material.albedo_texture = material.metallic_roughness_texture = material.normal_texture =
                    material.occlusion_texture = material.emissive_texture = -1;
            }
            group.levels.push_back(std::move(part));
        }
        if (!group.levels.empty()) result.push_back(std::move(group));
    }
    if (taken > 0) {
        std::vector<asset::SubMesh> kept;
        kept.reserve(model.submeshes.size());
        for (std::size_t s = 0; s < model.submeshes.size(); ++s) {
            if (!removed[s]) kept.push_back(model.submeshes[s]);
        }
        model.submeshes.swap(kept);
    }
    // Mismo orden siempre (unordered_map no lo garantiza): por nodo de LOD0.
    std::sort(result.begin(), result.end(),
              [](const ManualLodGroup& a, const ManualLodGroup& b) { return a.lod0 < b.lod0; });
    return result;
}

// Tras splitByNode: cada grupo a la pieza de su LOD0 (las piezas van en el
// orden de los nodos con malla) y sus niveles como piezas sin nodo al final.
// Devuelve los grupos para la cabecera ([LOD0, LOD1, ...] por pieza).
std::vector<std::vector<std::int32_t>> attachManualLods(const ModelData& model, std::vector<ManualLodGroup>& groups,
                                                        crdata::ModelContent& content) {
    std::vector<std::vector<std::int32_t>> result;
    std::vector<bool> has_mesh(model.nodes.size(), false);
    for (const asset::SubMesh& submesh : model.submeshes) {
        const std::int32_t node = submesh.node;
        has_mesh[node >= 0 && static_cast<std::size_t>(node) < model.nodes.size() ? static_cast<std::size_t>(node) : 0] =
            true;
    }
    for (ManualLodGroup& group : groups) {
        if (group.lod0 >= has_mesh.size() || !has_mesh[group.lod0]) continue;
        std::int32_t part = 0;
        for (std::size_t n = 0; n < group.lod0; ++n) part += has_mesh[n] ? 1 : 0;
        if (static_cast<std::size_t>(part) >= content.parts.size()) continue;
        std::vector<std::int32_t> entry{part};
        for (ModelData& level : group.levels) {
            entry.push_back(static_cast<std::int32_t>(content.parts.size()));
            content.parts.push_back(std::move(level));
        }
        result.push_back(std::move(entry));
    }
    return result;
}

const char* lodModeName(ModelLodMode mode) {
    switch (mode) {
        case ModelLodMode::Off: return "off";
        case ModelLodMode::Manual: return "manual";
        default: return "auto";
    }
}

ModelLodMode lodModeFrom(const nlohmann::json& j) {
    if (!j.is_object()) return ModelLodMode::Auto;
    const auto it = j.find("lod_mode");
    if (it == j.end() || !it->is_string()) return ModelLodMode::Auto;
    const std::string mode = it->get<std::string>();
    if (mode == "off") return ModelLodMode::Off;
    if (mode == "manual") return ModelLodMode::Manual;
    return ModelLodMode::Auto;
}

std::string settingsJson(const ModelImportSettings& settings,
                         const std::vector<std::vector<std::int32_t>>& lod_groups = {}) {
    nlohmann::json j;
    j["animated"] = settings.animated;
    j["scale"] = settings.scale;
    j["convert_units"] = settings.convert_units;
    j["file_unit_scale"] = settings.file_unit_scale;
    j["directx_normals"] = settings.directx_normals;
    j["combine_meshes"] = settings.combine_meshes;
    j["combine_above_parts"] = settings.combine_above_parts;
    j["lod_mode"] = lodModeName(settings.lod_mode);
    if (!lod_groups.empty()) j["lod_groups"] = lod_groups;
    return j.dump();
}

// Avance opcional (nullptr = nadie mira).
void report(ImportProgress* progress, float fraction, std::string stage) {
    if (progress != nullptr) progress->report(fraction, std::move(stage));
}

// La lectura (assimp) ocupa esta parte de la barra; el resto es partir,
// incrustar texturas y escribir.
constexpr float kReadShare = 0.6f;

std::size_t countTriangles(const crdata::ModelContent& content) {
    std::size_t triangles = 0;
    for (const ModelData& part : content.parts) {
        triangles += part.indices.size() / 3;
    }
    return triangles;
}

}  // namespace

// Importa `source` a `target` (vacio = un nombre libre en `destination_folder`)
// con `uuid` (invalido = uno nuevo).
static ImportResult importModelTo(const std::filesystem::path& source,
                                  const std::filesystem::path& destination_folder,
                                  const std::filesystem::path& target, const Uuid& uuid,
                                  const ModelImportSettings& settings, ImportProgress* progress) {
    ImportResult result{};
    const auto start = std::chrono::steady_clock::now();
    try {
        if (!std::filesystem::is_regular_file(source)) {
            throw std::runtime_error("no existe el archivo");
        }
        std::filesystem::create_directories(destination_folder);
        const bool is_obj = lower(source.extension().string()) == ".obj";
        const std::string stem = crdata::utf8(source.stem());

        report(progress, 0.0f, "Leyendo " + crdata::utf8(source.filename()));
        asset::ImportProgressCallback on_read;
        if (progress != nullptr) {
            on_read = [progress](float f) { progress->report(f * kReadShare); };
        }

        crdata::ModelContent content{};
        std::string how;
        // Scale Factor x unidades del archivo: horneado en todo el modelo.
        ModelImportSettings stored = settings;
        stored.file_unit_scale = asset::fileUnitScale(source);
        const float factor = settings.scale * (settings.convert_units ? stored.file_unit_scale : 1.0f);
        std::vector<std::vector<std::int32_t>> lod_groups;  // LODs a mano: [LOD0, LOD1, ...] por pieza
        if (settings.animated) {
            // Personaje: una sola pieza con esqueleto y animaciones.
            ModelData model = asset::importModelSource(source, /*force_static=*/false, on_read);
            asset::scaleModel(model, factor);
            report(progress, kReadShare, "Preparando el esqueleto y las animaciones");
            content.animated = !model.animations.empty() || model.bones.size() > 1;
            for (const asset::AnimationClip& clip : model.animations) {
                content.animation_names.push_back(clip.name);
            }
            model.name = stem;
            content.nodes.push_back(ModelNode{stem, -1, Mat4::identity(), 0});
            content.parts.push_back(std::move(model));
            how = "una pieza animada";
        } else {
            ModelData model = asset::importModelHierarchy(source, on_read);
            asset::scaleModel(model, factor);
            report(progress, kReadShare, "Partiendo en piezas y agrupando");
            model.name = stem;
            // LODs hechos a mano (_LOD1.._LODn): fuera de la jerarquia antes de
            // partir (no son objetos de la escena ni cuentan como piezas).
            std::vector<ManualLodGroup> manual_lods =
                takeManualLods(model, is_obj, settings.lod_mode != ModelLodMode::Off);
            std::string reason;
            const std::size_t combine_above =
                settings.combine_meshes ? static_cast<std::size_t>(std::max(settings.combine_above_parts, 1))
                                        : kMaxNodeParts;
            if (splitByNode(model, is_obj, content, reason, combine_above)) {
                how = std::to_string(content.parts.size()) + " piezas por " +
                      (is_obj ? "objeto" : "nodo");
                lod_groups = attachManualLods(model, manual_lods, content);
                if (!lod_groups.empty()) how += ", " + std::to_string(lod_groups.size()) + " con LODs a mano";
            } else {
                splitByMaterial(model, /*hierarchy_space=*/!is_obj, content);
                how = std::to_string(content.parts.size()) + " piezas por material (" + reason +
                      ")";
                // Agrupado por material, LOD0 ya no es una pieza: sus niveles
                // se descartan (los automaticos siguen).
                if (!manual_lods.empty()) how += ", sin los LODs a mano (desactiva Combinar mallas)";
            }
            content.nodes[0].name = stem;
        }

        // (La escala ya va en las piezas: la raiz se queda sin escalar.)
        std::uint32_t missing = 0;
        std::size_t parts_done = 0;
        for (ModelData& part : content.parts) {
            const std::size_t part_index = parts_done++;
            report(progress,
                   0.7f + 0.15f * static_cast<float>(part_index) /
                              static_cast<float>(std::max<std::size_t>(content.parts.size(), 1)),
                   "Incrustando texturas (" + std::to_string(part_index + 1) + "/" +
                       std::to_string(content.parts.size()) + ")");
            for (asset::MaterialData& material : part.materials) {
                material.normal_map_directx = material.normal_map_directx || settings.directx_normals;
            }
            // Autonomo: las texturas que solo tenian ruta pasan dentro.
            missing += asset::embedTextures(part);
        }

        crdata::Header header{};
        header.type = AssetType::Model;
        header.uuid = uuid.valid() ? uuid : Uuid::generate();
        header.name = stem;
        header.source = crdata::utf8(std::filesystem::absolute(source));
        header.settings_json = settingsJson(stored, lod_groups);

        const std::filesystem::path file =
            target.empty() ? crdata::uniquePath(destination_folder, stem, crdata::kExtension) : target;
        report(progress, 0.85f, "Escribiendo " + crdata::utf8(file.filename()));
        crdata::writeModel(file, header, content);
        report(progress, 1.0f, "Terminado");

        result.ok = true;
        result.info.uuid = header.uuid;
        result.info.type = AssetType::Model;
        result.info.name = crdata::utf8(file.stem());
        result.info.path = file;
        result.info.source = source;
        result.info.size_bytes = std::filesystem::file_size(file);

        const float seconds =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
        result.message = "[Assets] Importado " + crdata::utf8(source.filename()) + " -> " +
                         crdata::utf8(file.filename()) + ": " + how + ", " +
                         std::to_string(content.nodes.size()) + " nodos, " +
                         std::to_string(countTriangles(content)) + " triangulos, " +
                         std::to_string(result.info.size_bytes / (1024 * 1024)) + " MB, " +
                         std::to_string(seconds).substr(0, 5) + " s";
        if (factor != 1.0f) result.message += " (escala " + std::to_string(factor).substr(0, 6) + ")";
        if (missing > 0) {
            result.message += " (" + std::to_string(missing) + " texturas no encontradas)";
        }
        std::cout << result.message << "\n";
    } catch (const std::exception& error) {
        result.ok = false;
        result.message = "[Assets] No se pudo importar " + crdata::utf8(source) + ": " + error.what();
        std::cerr << result.message << "\n";
    }
    return result;
}

ImportResult importModel(const std::filesystem::path& source,
                         const std::filesystem::path& destination_folder,
                         const ModelImportSettings& settings, ImportProgress* progress) {
    return importModelTo(source, destination_folder, {}, {}, settings, progress);
}

ModelImportSettings modelImportSettings(const std::filesystem::path& file) {
    ModelImportSettings settings;
    const auto header = crdata::readHeader(file);
    if (!header) return settings;
    const nlohmann::json j = nlohmann::json::parse(header->settings_json, nullptr, false);
    if (!j.is_object()) return settings;
    settings.animated = j.value("animated", settings.animated);
    settings.scale = j.value("scale", settings.scale);
    // Antes de la 0.7.2 no se convertian las unidades: igual al reimportar.
    settings.convert_units = j.value("convert_units", false);
    settings.file_unit_scale = j.value("file_unit_scale", 1.0f);
    settings.directx_normals = j.value("directx_normals", settings.directx_normals);
    // Los importados antes de existir el ajuste: se combinan al reimportar.
    settings.combine_meshes = j.value("combine_meshes", true);
    settings.combine_above_parts = j.value("combine_above_parts", settings.combine_above_parts);
    settings.lod_mode = lodModeFrom(j);
    return settings;
}

ModelLodInfo modelLodInfo(const std::string& settings_json) {
    ModelLodInfo info;
    const nlohmann::json j = nlohmann::json::parse(settings_json, nullptr, false);
    if (!j.is_object()) return info;
    info.mode = lodModeFrom(j);
    const auto groups = j.find("lod_groups");
    if (groups == j.end() || !groups->is_array()) return info;
    for (const nlohmann::json& group : *groups) {
        if (!group.is_array() || group.size() < 2) continue;
        std::vector<std::int32_t> parts;
        for (const nlohmann::json& part : group) {
            if (part.is_number_integer()) parts.push_back(part.get<std::int32_t>());
        }
        if (parts.size() >= 2) info.groups.push_back(std::move(parts));
    }
    return info;
}

std::string modelImportSource(const std::filesystem::path& file) {
    const auto header = crdata::readHeader(file);
    return header ? header->source : std::string();
}

ImportResult reimportModel(const AssetInfo& info, const ModelImportSettings& settings, ImportProgress* progress) {
    ImportResult result{};
    const auto header = crdata::readHeader(info.path);
    if (!header || header->type != AssetType::Model) {
        result.message = "[Assets] " + crdata::utf8(info.path.filename()) + " no es un modelo importado";
        return result;
    }
    const std::filesystem::path source = crdata::fromUtf8(header->source);
    if (header->source.empty() || !std::filesystem::is_regular_file(source)) {
        result.message = "[Assets] No se puede reimportar " + info.name + ": ya no esta el original (" +
                         header->source + ")";
        std::cerr << result.message << "\n";
        return result;
    }
    // Se escribe a un temporal y se reemplaza al final: si falla, el
    // modelo de antes sigue intacto.
    std::filesystem::path temporary = info.path;
    temporary += ".reimport";
    result = importModelTo(source, info.path.parent_path(), temporary, header->uuid, settings, progress);
    if (!result.ok) {
        std::error_code e;
        std::filesystem::remove(temporary, e);
        return result;
    }
    std::error_code e;
    std::filesystem::rename(temporary, info.path, e);
    if (e) {
        result.ok = false;
        result.message = "[Assets] No se pudo reemplazar " + crdata::utf8(info.path) + ": " + e.message();
        std::filesystem::remove(temporary, e);
        return result;
    }
    result.info.path = info.path;
    result.info.name = info.name;
    result.info.size_bytes = std::filesystem::file_size(info.path, e);
    return result;
}

ImportResult importEnvironment(const std::filesystem::path& source,
                               const std::filesystem::path& destination_folder,
                               ImportProgress* progress) {
    ImportResult result{};
    try {
        report(progress, 0.0f, "Leyendo " + crdata::utf8(source.filename()));
        std::ifstream in(source, std::ios::binary | std::ios::ate);
        if (!in) {
            throw std::runtime_error("no se pudo abrir");
        }
        const std::streamsize size = in.tellg();
        if (size <= 0) {
            throw std::runtime_error("archivo vacio");
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        in.seekg(0);
        in.read(reinterpret_cast<char*>(bytes.data()), size);
        if (!in) {
            throw std::runtime_error("error de lectura");
        }
        // Firma de Radiance: "#?RADIANCE" o "#?RGBE".
        if (bytes.size() < 2 || bytes[0] != '#' || bytes[1] != '?') {
            throw std::runtime_error("no es un HDR de Radiance (.hdr)");
        }

        std::filesystem::create_directories(destination_folder);
        const std::string stem = crdata::utf8(source.stem());
        crdata::Header header{};
        header.type = AssetType::Environment;
        header.uuid = Uuid::generate();
        header.name = stem;
        header.source = crdata::utf8(std::filesystem::absolute(source));
        header.settings_json = "{}";

        const std::filesystem::path file =
            crdata::uniquePath(destination_folder, stem, crdata::kExtension);
        report(progress, 0.5f, "Escribiendo " + crdata::utf8(file.filename()));
        crdata::writeEnvironment(file, header, lower(source.extension().string()), bytes);
        report(progress, 1.0f, "Terminado");

        result.ok = true;
        result.info.uuid = header.uuid;
        result.info.type = AssetType::Environment;
        result.info.name = crdata::utf8(file.stem());
        result.info.path = file;
        result.info.source = source;
        result.info.size_bytes = std::filesystem::file_size(file);
        result.message = "[Assets] Importado cielo " + crdata::utf8(source.filename()) + " -> " +
                         crdata::utf8(file.filename()) + " (" +
                         std::to_string(result.info.size_bytes / (1024 * 1024)) + " MB)";
        std::cout << result.message << "\n";
    } catch (const std::exception& error) {
        result.ok = false;
        result.message = "[Assets] No se pudo importar el cielo " + crdata::utf8(source) + ": " +
                         error.what();
        std::cerr << result.message << "\n";
    }
    return result;
}

bool writeGeneratedModel(const std::filesystem::path& file, const Uuid& uuid, const std::string& name,
                         const std::vector<ModelNode>& nodes, const std::vector<asset::ModelData>& parts,
                         std::string* error, bool animated) {
    try {
        std::filesystem::create_directories(file.parent_path());
        crdata::Header header{};
        header.type = AssetType::Model;
        header.uuid = uuid;
        header.name = name;
        header.settings_json = "{\"generated\":true}";
        crdata::ModelContent content{};
        content.nodes = nodes;
        content.parts = parts;
        content.animated = animated;
        if (animated && !parts.empty()) {
            for (const asset::AnimationClip& clip : parts.front().animations) content.animation_names.push_back(clip.name);
        }
        crdata::writeModel(file, header, content);
        return true;
    } catch (const std::exception& e) {
        if (error != nullptr) *error = "No se pudo escribir " + crdata::utf8(file) + ": " + e.what();
        return false;
    }
}

// Si el archivo trae animaciones o piel (un personaje, tambien una malla con
// esqueleto exportada sin tomas, como hacen Unity, Unreal y Godot) se importa
// como una pieza animada con su esqueleto; si no, partido en piezas por nodo.
// Basta con 2 huesos distintos: con uno solo es una pieza rigida pegada a un
// hueso (un arma) y la importacion animada la dejaria igual sin esqueleto.
// Se lee la escena de assimp sin post-proceso y, en FBX, sin materiales,
// texturas incrustadas, luces ni camaras: es lo caro de la segunda lectura
// (la de importModelSource / importModelHierarchy) que aqui no hace falta.
static bool sourceIsAnimated(const std::filesystem::path& source) {
    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_MATERIALS, false);
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_TEXTURES, false);
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_LIGHTS, false);
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_CAMERAS, false);
    const aiScene* scene = importer.ReadFile(crdata::utf8(source), 0);
    if (scene == nullptr) return false;
    if (scene->mNumAnimations > 0) return true;
    std::unordered_set<std::string> bones;
    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        if (mesh == nullptr || !mesh->HasBones()) continue;
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            if (mesh->mBones[b] != nullptr) bones.insert(mesh->mBones[b]->mName.C_Str());
        }
        if (bones.size() >= 2) return true;
    }
    return false;
}

ImportResult importAny(const std::filesystem::path& source,
                       const std::filesystem::path& destination_folder,
                       ImportProgress* progress) {
    const std::string extension = lower(source.extension().string());
    if (extension == ".hdr") {
        return importEnvironment(source, destination_folder, progress);
    }
    if (extension == ".obj" || extension == ".fbx" || extension == ".gltf" || extension == ".glb" ||
        extension == ".dae") {
        ModelImportSettings settings;
        report(progress, 0.0f, "Analizando " + crdata::utf8(source.filename()));
        settings.animated = extension != ".obj" && sourceIsAnimated(source);
        return importModel(source, destination_folder, settings, progress);
    }
    ImportResult result{};
    result.message = "[Assets] Formato no soportado: " + crdata::utf8(source.filename());
    std::cerr << result.message << "\n";
    return result;
}

}  // namespace cramion::assets
