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
#include <iostream>
#include <unordered_map>

namespace cramion::asset {
namespace {

// Por debajo de esto no merece la pena: el modelo ya es barato.
constexpr std::size_t kMinModelTriangles = 4096;
// Un material con menos triangulos no se sigue simplificando.
constexpr std::size_t kMinGroupTriangles = 64;
constexpr std::size_t kMaxLods = 5;

}  // namespace

std::size_t generateLods(ModelData& model) {
    model.lods.clear();
    model.lod_indices.clear();
    // Solo estaticos: los animados se deforman y se dibujan por otro camino.
    if (model.bones.size() > 1 || !model.animations.empty() || model.vertices.empty() ||
        model.indices.size() / 3 < kMinModelTriangles) {
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
            std::size_t result = meshopt_simplify(scratch.data(), group.indices.data(), count, positions,
                                                  vertex_count, kStride, target, extent, kOptions, &error);
            // La topologia no deja bajar (p. ej. mallas de muchas piezas
            // abiertas): simplificacion "sloppy" (agrupa vertices por celdas).
            if (result > count * 85 / 100) {
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
        std::cout << " triangulos) en " << seconds << " s\n";
    }
    return model.lods.size();
}

}  // namespace cramion::asset
