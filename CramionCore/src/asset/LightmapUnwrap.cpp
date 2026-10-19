// Desenvuelto de las UV de lightmap con xatlas (PLAN-LIGHTMAPS.md 1.3). Solo
// en el editor (CRAMION_HAS_LIGHTMAP_UNWRAP): Android no compila xatlas ni
// este archivo (CramionCore/CMakeLists.txt).
//
//   1. Posiciones soldadas -> componentes conexas (union-find).
//   2. Componentes diminutas (diagonal < min_component_texels) y caras
//      degeneradas: fuera (centinela: las iluminan las sondas).
//   3. Trozos de <= 65536 caras (componentes enteras) -> un AddMesh cada uno.
//   4. ComputeCharts + PackCharts a texels_per_unit; si no cabe en max_size,
//      se vuelve a empaquetar mas pequeno (hasta 3 veces).
//   5. Forma compacta: xatlas conserva el orden de las caras de cada malla
//      (indexArray[mapFaceToSourceFace(f) * 3 + j]), asi que cada esquina de
//      salida es la misma esquina de la pieza; solo los vertices que una
//      costura parte en dos se duplican al final.

#include "CramionCore/asset/LightmapUv.h"

#include <xatlas.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#include <unordered_map>

namespace cramion::lighting {

using core::Vec3;

namespace {

constexpr std::uint32_t kChunkFaces = 65536;

// Id comun para los vertices que estan en la misma posicion (los que separan
// las costuras de normales o de UV de la textura): asi las componentes cruzan
// esas costuras si la superficie sigue.
std::vector<std::uint32_t> weldPositions(const std::vector<asset::SkinnedVertex>& vertices, float diagonal) {
    const std::size_t count = vertices.size();
    std::vector<std::uint32_t> canonical(count);
    if (count == 0) return canonical;
    const double cell = std::max(static_cast<double>(diagonal), 1e-6) * 1e-6;
    struct Key {
        std::int64_t x, y, z;
        std::uint32_t index;
    };
    std::vector<Key> keys(count);
    for (std::size_t i = 0; i < count; ++i) {
        const Vec3& p = vertices[i].position;
        keys[i] = Key{static_cast<std::int64_t>(std::llround(p.x / cell)), static_cast<std::int64_t>(std::llround(p.y / cell)),
                      static_cast<std::int64_t>(std::llround(p.z / cell)), static_cast<std::uint32_t>(i)};
    }
    std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        if (a.z != b.z) return a.z < b.z;
        return a.index < b.index;
    });
    std::uint32_t current = keys[0].index;
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0 && (keys[i].x != keys[i - 1].x || keys[i].y != keys[i - 1].y || keys[i].z != keys[i - 1].z)) {
            current = keys[i].index;
        }
        canonical[keys[i].index] = current;
    }
    return canonical;
}

struct UnionFind {
    std::vector<std::uint32_t> parent;
    explicit UnionFind(std::size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), 0u); }
    std::uint32_t find(std::uint32_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    }
    void unite(std::uint32_t a, std::uint32_t b) {
        a = find(a);
        b = find(b);
        if (a == b) return;
        // El menor manda: el resultado no depende del orden de las uniones.
        if (a < b) parent[b] = a;
        else parent[a] = b;
    }
};

struct ProgressState {
    const std::function<bool(float)>* callback = nullptr;
    bool cancelled = false;
};

bool xatlasProgress(xatlas::ProgressCategory category, int progress, void* user) {
    auto* state = static_cast<ProgressState*>(user);
    if (state == nullptr || state->callback == nullptr || !*state->callback) return true;
    float begin = 0.0f;
    float span = 0.1f;
    switch (category) {
        case xatlas::ProgressCategory::AddMesh: begin = 0.0f; span = 0.1f; break;
        case xatlas::ProgressCategory::ComputeCharts: begin = 0.1f; span = 0.6f; break;
        case xatlas::ProgressCategory::PackCharts: begin = 0.7f; span = 0.25f; break;
        case xatlas::ProgressCategory::BuildOutputMeshes: begin = 0.95f; span = 0.05f; break;
    }
    const float fraction = begin + span * static_cast<float>(std::clamp(progress, 0, 100)) / 100.0f;
    if (!(*state->callback)(fraction)) {
        state->cancelled = true;
        return false;
    }
    return true;
}

std::uint64_t fnvArray(std::uint64_t hash, const std::vector<std::uint32_t>& values) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(values.data());
    for (std::size_t i = 0; i < values.size() * sizeof(std::uint32_t); ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}

}  // namespace

bool unwrapLightmapUvs(const asset::ModelData& part, const LightmapUvSettings& s, LightmapUvLayout& out,
                       const std::function<bool(float)>& progress, std::string* error) {
    out = LightmapUvLayout{};
    const std::vector<asset::SkinnedVertex>& vertices = part.vertices.get();
    const std::vector<std::uint32_t>& indices = part.indices.get();
    const auto vertex_count = static_cast<std::uint32_t>(vertices.size());
    const std::size_t face_count = indices.size() / 3;
    out.source_vertices = vertex_count;
    out.source_indices = static_cast<std::uint32_t>(indices.size());
    out.source_hash = lightmapMeshHash(part);
    out.settings_hash = s.hash();
    out.texels_per_unit = s.texels_per_unit;
    for (const std::uint32_t index : indices) {
        if (index >= vertex_count) {
            if (error) *error = "la malla tiene indices fuera de rango";
            return false;
        }
    }

    // --- 1. Componentes conexas por posicion soldada ---
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const asset::SkinnedVertex& v : vertices) {
        lo = Vec3{std::min(lo.x, v.position.x), std::min(lo.y, v.position.y), std::min(lo.z, v.position.z)};
        hi = Vec3{std::max(hi.x, v.position.x), std::max(hi.y, v.position.y), std::max(hi.z, v.position.z)};
    }
    const float diagonal = vertices.empty() ? 0.0f : core::length(hi - lo);
    const std::vector<std::uint32_t> canonical = weldPositions(vertices, diagonal);
    UnionFind sets(vertices.size());
    for (std::size_t f = 0; f < face_count; ++f) {
        const std::uint32_t a = canonical[indices[f * 3]];
        sets.unite(a, canonical[indices[f * 3 + 1]]);
        sets.unite(a, canonical[indices[f * 3 + 2]]);
    }

    // --- 2. Caras que entran: ni degeneradas ni de una pieza diminuta ---
    const double min_area = 1e-12 * static_cast<double>(diagonal) * static_cast<double>(diagonal);
    struct Component {
        Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        std::vector<std::uint32_t> faces;
    };
    std::unordered_map<std::uint32_t, std::uint32_t> component_of_root;
    std::vector<Component> components;
    std::vector<std::uint8_t> degenerate(face_count, 0);
    for (std::size_t f = 0; f < face_count; ++f) {
        const Vec3& a = vertices[indices[f * 3]].position;
        const Vec3& b = vertices[indices[f * 3 + 1]].position;
        const Vec3& c = vertices[indices[f * 3 + 2]].position;
        const double area = 0.5 * static_cast<double>(core::length(core::cross(b - a, c - a)));
        if (!(area > min_area)) degenerate[f] = 1;
        const std::uint32_t root = sets.find(canonical[indices[f * 3]]);
        const auto [it, inserted] = component_of_root.try_emplace(root, static_cast<std::uint32_t>(components.size()));
        if (inserted) components.emplace_back();
        Component& comp = components[it->second];
        for (const Vec3* p : {&a, &b, &c}) {
            comp.lo = Vec3{std::min(comp.lo.x, p->x), std::min(comp.lo.y, p->y), std::min(comp.lo.z, p->z)};
            comp.hi = Vec3{std::max(comp.hi.x, p->x), std::max(comp.hi.y, p->y), std::max(comp.hi.z, p->z)};
        }
        comp.faces.push_back(static_cast<std::uint32_t>(f));
    }
    std::size_t ignored = 0;
    // --- 3. Trozos de componentes enteras (en el orden de su primera cara) ---
    std::vector<std::vector<std::uint32_t>> chunks;
    for (const Component& comp : components) {
        const float comp_diagonal = core::length(comp.hi - comp.lo);
        std::vector<std::uint32_t> kept;
        kept.reserve(comp.faces.size());
        if (comp_diagonal * s.texels_per_unit < s.min_component_texels) {
            ignored += comp.faces.size();
            continue;
        }
        for (const std::uint32_t f : comp.faces) {
            if (degenerate[f]) {
                ++ignored;
            } else {
                kept.push_back(f);
            }
        }
        if (kept.empty()) continue;
        if (chunks.empty() || (!chunks.back().empty() && chunks.back().size() + kept.size() > kChunkFaces)) {
            chunks.emplace_back();
        }
        chunks.back().insert(chunks.back().end(), kept.begin(), kept.end());
    }
    out.ignored_triangles = static_cast<std::uint32_t>(ignored);

    // Sin nada que desenvolver: todo con centinela (lo iluminan las sondas).
    std::vector<std::uint32_t> slot_uv(vertex_count, kLightmapUvNone);
    if (chunks.empty()) {
        out.uv = std::move(slot_uv);
        std::uint64_t h = 1469598103934665603ull;
        h = fnvArray(h, out.uv);
        out.layout_hash = h;
        return true;
    }

    // --- 4. xatlas ---
    ProgressState state{&progress, false};
    xatlas::Atlas* atlas = xatlas::Create();
    xatlas::SetProgressCallback(atlas, xatlasProgress, &state);
    struct LocalMesh {
        std::vector<std::uint32_t> to_part;  // vertice local -> vertice de la pieza
        std::vector<Vec3> positions;
        std::vector<Vec3> normals;
        std::vector<std::uint32_t> indices;
    };
    std::vector<LocalMesh> locals(chunks.size());
    {
        std::vector<std::uint32_t> local_of(vertex_count, 0xFFFFFFFFu);
        for (std::size_t c = 0; c < chunks.size(); ++c) {
            LocalMesh& mesh = locals[c];
            mesh.indices.reserve(chunks[c].size() * 3);
            for (const std::uint32_t f : chunks[c]) {
                for (int k = 0; k < 3; ++k) {
                    const std::uint32_t v = indices[static_cast<std::size_t>(f) * 3 + static_cast<std::size_t>(k)];
                    if (local_of[v] == 0xFFFFFFFFu) {
                        local_of[v] = static_cast<std::uint32_t>(mesh.to_part.size());
                        mesh.to_part.push_back(v);
                        mesh.positions.push_back(vertices[v].position);
                        mesh.normals.push_back(vertices[v].normal);
                    }
                    mesh.indices.push_back(local_of[v]);
                }
            }
            for (const std::uint32_t v : mesh.to_part) local_of[v] = 0xFFFFFFFFu;
            xatlas::MeshDecl decl;
            decl.vertexPositionData = mesh.positions.data();
            decl.vertexPositionStride = sizeof(Vec3);
            decl.vertexNormalData = mesh.normals.data();
            decl.vertexNormalStride = sizeof(Vec3);
            decl.vertexCount = static_cast<std::uint32_t>(mesh.positions.size());
            decl.indexData = mesh.indices.data();
            decl.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
            decl.indexFormat = xatlas::IndexFormat::UInt32;
            const xatlas::AddMeshError result = xatlas::AddMesh(atlas, decl, static_cast<std::uint32_t>(chunks.size()));
            if (result != xatlas::AddMeshError::Success) {
                if (error) *error = std::string("xatlas: ") + xatlas::StringForEnum(result);
                xatlas::Destroy(atlas);
                return false;
            }
            // xatlas copia la malla: las copias locales de posiciones ya no hacen falta.
            std::vector<Vec3>().swap(mesh.positions);
            std::vector<Vec3>().swap(mesh.normals);
            std::vector<std::uint32_t>().swap(mesh.indices);
        }
    }
    if (state.cancelled) {
        xatlas::Destroy(atlas);
        if (error) *error = "cancelado";
        return false;
    }
    xatlas::ChartOptions chart_options;
    chart_options.maxIterations = static_cast<std::uint32_t>(std::clamp(s.chart_iterations, 1, 8));
    chart_options.fixWinding = true;
    xatlas::ComputeCharts(atlas, chart_options);
    if (state.cancelled) {
        xatlas::Destroy(atlas);
        if (error) *error = "cancelado";
        return false;
    }
    float texels_per_unit = std::max(s.texels_per_unit, 1e-4f);
    const std::uint32_t max_size = std::max(s.max_size, 64u);
    for (int attempt = 0;; ++attempt) {
        xatlas::PackOptions pack;
        pack.padding = s.padding;
        pack.bilinear = true;
        pack.texelsPerUnit = texels_per_unit;
        pack.resolution = 0;
        pack.rotateCharts = true;
        pack.rotateChartsToAxis = true;
        pack.blockAlign = false;
        pack.bruteForce = false;
        xatlas::PackCharts(atlas, pack);
        if (state.cancelled) {
            xatlas::Destroy(atlas);
            if (error) *error = "cancelado";
            return false;
        }
        if ((atlas->width <= max_size && atlas->height <= max_size) || attempt >= 3) break;
        const float shrink = 0.98f * static_cast<float>(max_size) / static_cast<float>(std::max(atlas->width, atlas->height));
        texels_per_unit *= shrink;
        std::cerr << "[Lightmap] " << part.name << ": no cabe en " << max_size << " texeles, se baja a "
                  << texels_per_unit << " texeles por unidad\n";
    }
    if (atlas->meshCount != chunks.size() || atlas->width == 0 || atlas->height == 0) {
        if (error) *error = "xatlas no devolvio la malla";
        xatlas::Destroy(atlas);
        return false;
    }
    out.width = atlas->width;
    out.height = atlas->height;
    out.texels_per_unit = texels_per_unit;
    out.charts = atlas->chartCount;
    if (atlas->atlasCount > 0 && atlas->utilization != nullptr) out.utilization = atlas->utilization[0];

    // --- 5. Forma compacta ---
    const float inv_w = 1.0f / static_cast<float>(out.width);
    const float inv_h = 1.0f / static_cast<float>(out.height);
    // Quien asigno la UV de cada vertice de la pieza (trozo, vertice de salida).
    std::vector<std::uint64_t> owner(vertex_count, ~0ull);
    std::unordered_map<std::uint64_t, std::uint32_t> duplicate_of;  // (trozo, vertice de salida) -> anadido
    std::vector<std::uint32_t> duplicate_uv;
    double uv_area = 0.0;
    for (std::size_t c = 0; c < chunks.size(); ++c) {
        const xatlas::Mesh& mesh = atlas->meshes[c];
        const LocalMesh& local = locals[c];
        if (mesh.indexCount != chunks[c].size() * 3) {
            if (error) *error = "xatlas cambio el numero de caras";
            xatlas::Destroy(atlas);
            return false;
        }
        for (std::size_t f = 0; f < chunks[c].size(); ++f) {
            const std::uint32_t face = chunks[c][f];
            float corner_uv[3][2] = {};
            bool all_inside = true;
            for (std::uint32_t k = 0; k < 3; ++k) {
                const std::uint32_t corner = face * 3 + k;
                const std::uint32_t vs = mesh.indexArray[f * 3 + k];
                const xatlas::Vertex& xv = mesh.vertexArray[vs];
                const std::uint32_t vp = local.to_part[xv.xref];
                const std::uint32_t uv =
                    xv.atlasIndex < 0 ? kLightmapUvNone
                                      : packLightmapUv(core::Vec2{xv.uv[0] * inv_w, xv.uv[1] * inv_h});
                corner_uv[k][0] = xv.uv[0];
                corner_uv[k][1] = xv.uv[1];
                all_inside = all_inside && xv.atlasIndex >= 0;
                const std::uint64_t key = (static_cast<std::uint64_t>(c) << 32) | vs;
                if (owner[vp] == ~0ull) {
                    owner[vp] = key;
                    slot_uv[vp] = uv;
                } else if (owner[vp] != key) {
                    const auto [it, inserted] =
                        duplicate_of.try_emplace(key, vertex_count + static_cast<std::uint32_t>(out.duplicates.size()));
                    if (inserted) {
                        out.duplicates.push_back(vp);
                        duplicate_uv.push_back(uv);
                    }
                    out.patch_corner.push_back(corner);
                    out.patch_vertex.push_back(it->second);
                }
            }
            if (all_inside) {
                const double ux = corner_uv[1][0] - corner_uv[0][0], uy = corner_uv[1][1] - corner_uv[0][1];
                const double vx = corner_uv[2][0] - corner_uv[0][0], vy = corner_uv[2][1] - corner_uv[0][1];
                uv_area += 0.5 * std::fabs(ux * vy - uy * vx);
            }
            const Vec3& a = vertices[indices[static_cast<std::size_t>(face) * 3]].position;
            const Vec3& b = vertices[indices[static_cast<std::size_t>(face) * 3 + 1]].position;
            const Vec3& cc = vertices[indices[static_cast<std::size_t>(face) * 3 + 2]].position;
            out.lightmapped_area += 0.5f * core::length(core::cross(b - a, cc - a));
        }
    }
    xatlas::Destroy(atlas);
    if (out.utilization <= 0.0f) {
        out.utilization = static_cast<float>(uv_area / (static_cast<double>(out.width) * static_cast<double>(out.height)));
    }
    out.uv = std::move(slot_uv);
    out.uv.insert(out.uv.end(), duplicate_uv.begin(), duplicate_uv.end());
    std::uint64_t h = 1469598103934665603ull;
    h = fnvArray(h, out.duplicates);
    h = fnvArray(h, out.patch_corner);
    h = fnvArray(h, out.patch_vertex);
    h = fnvArray(h, out.uv);
    const std::vector<std::uint32_t> size{out.width, out.height};
    h = fnvArray(h, size);
    out.layout_hash = h == 0 ? 1 : h;
    if (progress) progress(1.0f);
    return true;
}

}  // namespace cramion::lighting
