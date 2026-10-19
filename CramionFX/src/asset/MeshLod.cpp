// LODs automaticos de los modelos estaticos (como el "auto LOD" de Unreal o un
// LOD Group de Unity generado solo): cada nivel con ~la mitad de triangulos
// que el anterior, con meshoptimizer. El renderizador elige el nivel de cada
// objeto por el error en pixeles que tendria en pantalla (ver
// VulkanRenderer::chooseLod), asi que no hay distancias que ajustar.

#include "CramionFX/asset/Model.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <unordered_map>

namespace cramion::asset {
namespace {

// Por debajo de esto no merece la pena: el modelo ya es barato.
constexpr std::size_t kMinModelTriangles = 4096;
// Un material con menos triangulos no se sigue simplificando.
constexpr std::size_t kMinGroupTriangles = 64;
constexpr std::size_t kMaxLods = 5;
// Modelos con esqueleto (personajes): el error que puede tener cada nivel,
// en fraccion del tamano del modelo (meshopt_simplifyScale). Para uno de 2 m:
// 5 mm, 1,2 cm, 2,8 cm, 6 cm y 12 cm. Sin tope (como los estaticos), el
// nivel 1 ya llegaba a la mitad de triangulos a cualquier precio y con Prune
// desaparecian los cuernos, el arco, la cara o las botas a 10-15 m; el
// "sloppy" (agrupa vertices por celdas) juntaba piezas distintas que el
// esqueleto mueve por separado. CRAMION_SKINNED_LOD=old: como antes (A/B).
constexpr float kSkinnedLevelError[kMaxLods] = {0.0025f, 0.006f, 0.014f, 0.03f, 0.06f};

bool oldSkinnedLods() {
    static const bool old = [] {
        const char* v = std::getenv("CRAMION_SKINNED_LOD");
        return v != nullptr && std::strcmp(v, "old") == 0;
    }();
    return old;
}

}  // namespace

VertexCacheStats optimizeVertexCache(std::vector<std::uint32_t>& indices, const std::vector<SubMesh>& submeshes,
                                     const std::vector<MaterialData>& materials, std::size_t vertex_count) {
    VertexCacheStats stats;
    if (indices.empty() || vertex_count == 0) return stats;
    // Cada submalla con sus vertices renumerados (0..n): el coste va con el
    // tamano de la submalla, no con el de todo el modelo (un lote estatico
    // tiene millones de vertices y miles de clusteres).
    std::vector<std::uint32_t> local_of(vertex_count, 0);
    std::vector<std::uint32_t> stamp(vertex_count, 0);
    std::vector<std::uint32_t> global_of;
    std::vector<std::uint32_t> local;
    std::vector<std::uint32_t> optimized;
    double before = 0.0;
    double after = 0.0;
    std::size_t triangles = 0;
    std::uint32_t generation = 0;
    for (const SubMesh& submesh : submeshes) {
        if (submesh.index_count < 3 || submesh.first_index + submesh.index_count > indices.size()) continue;
        if (submesh.material < materials.size() && materials[submesh.material].transparent) continue;
        ++generation;
        global_of.clear();
        local.resize(submesh.index_count);
        bool valid = true;
        for (std::uint32_t i = 0; i < submesh.index_count; ++i) {
            const std::uint32_t v = indices[submesh.first_index + i];
            if (v >= vertex_count) {
                valid = false;
                break;
            }
            if (stamp[v] != generation) {
                stamp[v] = generation;
                local_of[v] = static_cast<std::uint32_t>(global_of.size());
                global_of.push_back(v);
            }
            local[i] = local_of[v];
        }
        if (!valid) continue;
        const meshopt_VertexCacheStatistics old_stats =
            meshopt_analyzeVertexCache(local.data(), local.size(), global_of.size(), 16, 0, 0);
        optimized.resize(local.size());
        meshopt_optimizeVertexCache(optimized.data(), local.data(), local.size(), global_of.size());
        const meshopt_VertexCacheStatistics new_stats =
            meshopt_analyzeVertexCache(optimized.data(), optimized.size(), global_of.size(), 16, 0, 0);
        const std::size_t count = submesh.index_count / 3;
        before += static_cast<double>(old_stats.acmr) * static_cast<double>(count);
        after += static_cast<double>(new_stats.acmr) * static_cast<double>(count);
        triangles += count;
        for (std::uint32_t i = 0; i < submesh.index_count; ++i) {
            indices[submesh.first_index + i] = global_of[optimized[i]];
        }
    }
    if (triangles > 0) {
        stats.acmr_before = static_cast<float>(before / static_cast<double>(triangles));
        stats.acmr_after = static_cast<float>(after / static_cast<double>(triangles));
    }
    return stats;
}

VertexCacheStats optimizeVertexCache(ModelData& model) {
    return optimizeVertexCache(model.indices, model.submeshes, model.materials, model.vertices.size());
}

std::size_t generateLods(ModelData& model) {
    model.lods.clear();
    model.lod_indices.clear();
    // Tambien los de esqueleto: un LOD solo cambia los triangulos y comparte
    // los vertices, asi que los pesos de los huesos siguen valiendo (la
    // simplificacion se mide en la pose de reposo).
    if (model.vertices.empty() || model.indices.size() / 3 < kMinModelTriangles) {
        return 0;
    }
    const auto start = std::chrono::steady_clock::now();

    // Se simplifica cada material entero (no cluster a cluster): asi no se
    // abren grietas entre clusteres vecinos. Luego se vuelve a partir.
    struct Group {
        std::uint32_t material = 0;
        std::vector<std::uint32_t> indices;
        float error = 0.0f;  // acumulado desde LOD0, en unidades del modelo
        bool done = false;
    };
    std::vector<Group> groups;
    {
        std::unordered_map<std::uint32_t, std::size_t> group_of;
        for (const SubMesh& submesh : model.submeshes) {
            const auto [it, inserted] = group_of.try_emplace(submesh.material, groups.size());
            if (inserted) groups.push_back(Group{submesh.material, {}, 0.0f, false});
            std::vector<std::uint32_t>& out = groups[it->second].indices;
            out.insert(out.end(), model.indices.begin() + submesh.first_index,
                       model.indices.begin() + submesh.first_index + submesh.index_count);
        }
    }

    const float* positions = &model.vertices[0].position.x;
    const std::size_t vertex_count = model.vertices.size();
    constexpr std::size_t kStride = sizeof(SkinnedVertex);
    const float extent = meshopt_simplifyScale(positions, vertex_count, kStride);
    // Error absoluto (unidades del modelo); Prune quita las piezas sueltas
    // (hojas, briznas, tornillos) cuando ya no aportan; Sparse porque cada
    // material usa solo parte de los vertices.
    constexpr unsigned int kOptions =
        meshopt_SimplifyErrorAbsolute | meshopt_SimplifyPrune | meshopt_SimplifySparse;

    // Con esqueleto: cada nivel con su tope de error y sin "sloppy" (ver
    // kSkinnedLevelError). Los estaticos, como siempre.
    const bool skinned = model.bones.size() > 1 && !oldSkinnedLods() && extent > 0.0f;
    // Y con atributos (meshopt_simplifyWithAttributes): el error de un
    // personaje no es solo la forma en reposo. Cuentan tambien
    //   - la normal (sombreado): 30 grados ~ 0,5 % del tamano;
    //   - la UV (la textura resbala: la cara o la barba se desdibujan):
    //     0,005 de UV (10 texeles de 2048) ~ 0,25 % del tamano;
    //   - a que huesos va (la media de las posiciones de sus huesos en la
    //     pose de union, por los pesos): juntar un vertice del brazo con uno
    //     del pecho los estira al animar; se mide en unidades del modelo.
    // meshoptimizer lleva las posiciones a un cubo de lado 1 (divide por
    // `extent`) pero los atributos solo los multiplica por su peso: los pesos
    // van en fraccion del tamano, y los de los huesos (unidades del modelo)
    // ademas divididos por `extent`.
    constexpr std::size_t kAttributes = 8;
    std::vector<float> attributes;
    float attribute_weights[kAttributes] = {};
    if (skinned) {
        std::vector<core::Vec3> joint(model.bones.size());
        for (std::size_t b = 0; b < model.bones.size(); ++b) {
            const core::Mat4 bind = core::inverse(model.bones[b].offset);
            joint[b] = core::Vec3{bind.m[3][0], bind.m[3][1], bind.m[3][2]};
        }
        attributes.resize(vertex_count * kAttributes);
        for (std::size_t v = 0; v < vertex_count; ++v) {
            const SkinnedVertex& x = model.vertices[v];
            float* a = &attributes[v * kAttributes];
            a[0] = x.normal.x;
            a[1] = x.normal.y;
            a[2] = x.normal.z;
            a[3] = x.uv.x;
            a[4] = x.uv.y;
            core::Vec3 j{};
            float total = 0.0f;
            for (std::size_t k = 0; k < kMaxBoneInfluences; ++k) {
                if (x.weights[k] <= 0.0f || x.joints[k] >= joint.size()) continue;
                j = j + joint[x.joints[k]] * x.weights[k];
                total += x.weights[k];
            }
            if (total > 0.0f) j = j * (1.0f / total);
            a[5] = j.x;
            a[6] = j.y;
            a[7] = j.z;
        }
        const float wn = 0.01f, wu = 0.5f, wb = 0.5f / extent;
        const float w[kAttributes] = {wn, wn, wn, wu, wu, wb, wb, wb};
        std::copy(std::begin(w), std::end(w), attribute_weights);
    }

    std::vector<std::uint32_t> scratch;
    for (std::size_t level = 1; level <= kMaxLods; ++level) {
        bool reduced = false;
        for (Group& group : groups) {
            const std::size_t count = group.indices.size();
            if (group.done || count / 3 < kMinGroupTriangles) {
                group.done = true;
                continue;
            }
            const std::size_t target = (count / 2) / 3 * 3;
            scratch.resize(count);
            float error = 0.0f;
            // Con esqueleto el tope es el del nivel menos lo que ya se perdio
            // en los anteriores (se simplifica desde el nivel anterior).
            const float budget = skinned ? extent * kSkinnedLevelError[level - 1] - group.error : extent;
            if (budget <= 0.0f) {
                group.done = true;
                continue;
            }
            std::size_t result =
                skinned ? meshopt_simplifyWithAttributes(scratch.data(), group.indices.data(), count, positions,
                                                         vertex_count, kStride, attributes.data(),
                                                         kAttributes * sizeof(float), attribute_weights, kAttributes,
                                                         nullptr, target, budget, kOptions, &error)
                        : meshopt_simplify(scratch.data(), group.indices.data(), count, positions, vertex_count,
                                           kStride, target, budget, kOptions, &error);
            // Con esqueleto: lo que no baje dentro del tope se queda para el
            // nivel siguiente (con mas tope), sin "sloppy".
            if (skinned && result > count * 90 / 100) continue;
            // La topologia no deja bajar (p. ej. mallas de muchas piezas
            // abiertas): simplificacion "sloppy" (agrupa vertices por celdas).
            // Con UV de lightmap no: juntaria vertices de cartas distintas y
            // el lightmap se veria cruzado (las costuras de las cartas se
            // respetan como costuras con la normal, PLAN-LIGHTMAPS 1.5).
            // Con esqueleto tampoco (ni con CRAMION_SKINNED_LOD=old): soldaria
            // vertices de piezas que los huesos mueven por separado (brazo y
            // torso).
            if (!skinned && model.bones.size() <= 1 && result > count * 85 / 100 &&
                model.lightmap_uvs.empty()) {
                std::vector<std::uint32_t> sloppy(count);
                float sloppy_error = 0.0f;
                const std::size_t sloppy_result =
                    meshopt_simplifySloppy(sloppy.data(), group.indices.data(), count, positions, vertex_count,
                                           kStride, nullptr, target, 1.0f, &sloppy_error);
                if (sloppy_result < result * 85 / 100) {
                    scratch.swap(sloppy);
                    result = sloppy_result;
                    error = sloppy_error * extent;  // sloppy da el error relativo
                }
            }
            if (result > count * 90 / 100) {
                group.done = true;  // ya no baja: se queda como esta
                continue;
            }
            scratch.resize(result);
            group.indices.swap(scratch);
            // Se simplifica desde el nivel anterior: los errores se suman.
            group.error += error;
            reduced = true;
        }
        // Con esqueleto, un nivel que no baja con su tope puede bajar con el
        // del siguiente.
        if (!reduced && skinned) continue;
        if (!reduced) break;

        MeshLod lod{};
        std::vector<std::uint32_t> indices;
        for (const Group& group : groups) {
            lod.error = std::max(lod.error, group.error);
            if (group.indices.empty()) continue;  // podado entero
            SubMesh submesh{};
            submesh.first_index = static_cast<std::uint32_t>(indices.size());
            submesh.index_count = static_cast<std::uint32_t>(group.indices.size());
            submesh.material = group.material;
            lod.submeshes.push_back(submesh);
            indices.insert(indices.end(), group.indices.begin(), group.indices.end());
        }
        const std::size_t triangles = indices.size() / 3;
        clusterIndexRanges(model.vertices, indices, lod.submeshes);
        // La simplificacion y los clusteres desordenan los triangulos.
        optimizeVertexCache(indices, lod.submeshes, model.materials, model.vertices.size());
        const auto offset = static_cast<std::uint32_t>(model.lod_indices.size());
        for (SubMesh& submesh : lod.submeshes) submesh.first_index += offset;
        model.lod_indices.insert(model.lod_indices.end(), indices.begin(), indices.end());
        model.lods.push_back(std::move(lod));
        if (triangles < 256) break;
    }

    if (!model.lods.empty()) {
        const float seconds =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
        std::cout << "[Modelo] " << model.name << ": " << model.lods.size() << " LODs ("
                  << model.indices.size() / 3;
        for (const MeshLod& lod : model.lods) {
            std::size_t triangles = 0;
            for (const SubMesh& submesh : lod.submeshes) triangles += submesh.index_count / 3;
            std::cout << " -> " << triangles;
        }
        std::cout << " triangulos";
        if (skinned) {
            // Error de cada nivel en % del tamano (forma, normal, UV y huesos).
            std::cout << ", error";
            for (const MeshLod& lod : model.lods) {
                std::cout << " " << 100.0f * lod.error / std::max(extent, 1e-6f) << " %";
            }
        }
        std::cout << ") en " << seconds << " s\n";
    }
    return model.lods.size();
}

}  // namespace cramion::asset
