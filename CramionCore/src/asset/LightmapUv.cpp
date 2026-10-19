#include "CramionCore/asset/LightmapUv.h"

#include <zstd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>

namespace cramion::lighting {

using core::Vec2;
using core::Vec3;

namespace {

constexpr char kMagic[4] = {'C', 'R', 'U', 'V'};
constexpr std::uint32_t kFileVersion = 2;  // 1: el generador antiguo (sin comprimir); 2: compacta + zstd
// Commit de xatlas vendorizado (CramionCore/vendor/xatlas): entra en el hash
// de los ajustes, asi otro commit no reutiliza disposiciones viejas.
constexpr std::uint64_t kXatlasCommit = 0xf700c7790aaa030eull;

std::uint64_t fnv(std::uint64_t hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}

// Escritura y lectura de valores sueltos en un buffer de bytes.
struct Writer {
    std::vector<std::uint8_t> bytes;
    template <typename T>
    void put(const T& value) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
        bytes.insert(bytes.end(), p, p + sizeof(T));
    }
    void putArray(const std::vector<std::uint32_t>& values) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(values.data());
        bytes.insert(bytes.end(), p, p + values.size() * sizeof(std::uint32_t));
    }
};

struct Reader {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t at = 0;
    template <typename T>
    bool get(T& value) {
        if (at + sizeof(T) > size) return false;
        std::memcpy(&value, data + at, sizeof(T));
        at += sizeof(T);
        return true;
    }
    bool getArray(std::vector<std::uint32_t>& values, std::uint32_t count) {
        const std::size_t bytes = static_cast<std::size_t>(count) * sizeof(std::uint32_t);
        if (at + bytes > size) return false;
        values.resize(count);
        if (bytes > 0) std::memcpy(values.data(), data + at, bytes);
        at += bytes;
        return true;
    }
};

bool readBytes(const std::filesystem::path& file, std::uint64_t offset, std::uint64_t size, std::vector<std::uint8_t>& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    in.seekg(static_cast<std::streamoff>(offset));
    out.resize(static_cast<std::size_t>(size));
    if (size > 0) in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    return static_cast<bool>(in);
}

}  // namespace

std::uint64_t LightmapUvSettings::hash() const {
    std::uint64_t h = 1469598103934665603ull;
    // Densidad redondeada a 1/64: la misma escala calculada por dos caminos
    // (una suma en otro orden) reutiliza la cache.
    const auto density = static_cast<std::int64_t>(std::llround(static_cast<double>(texels_per_unit) * 64.0));
    h = fnv(h, &density, sizeof(density));
    h = fnv(h, &padding, sizeof(padding));
    const auto min_texels = static_cast<std::int64_t>(std::llround(static_cast<double>(min_component_texels) * 64.0));
    h = fnv(h, &min_texels, sizeof(min_texels));
    h = fnv(h, &max_size, sizeof(max_size));
    h = fnv(h, &chart_iterations, sizeof(chart_iterations));
    const std::uint32_t version = kUnwrapVersion;
    h = fnv(h, &version, sizeof(version));
    h = fnv(h, &kXatlasCommit, sizeof(kXatlasCommit));
    return h;
}

std::uint64_t lightmapMeshHash(const asset::ModelData& model) {
    std::uint64_t hash = 1469598103934665603ull;
    const std::vector<std::uint32_t>& indices = model.indices.get();
    const std::vector<asset::SkinnedVertex>& vertices = model.vertices.get();
    hash = fnv(hash, indices.data(), indices.size() * sizeof(std::uint32_t));
    for (const asset::SkinnedVertex& v : vertices) hash = fnv(hash, &v.position, sizeof(Vec3));
    return hash;
}

float meshSurfaceArea(const asset::ModelData& model) {
    const std::vector<std::uint32_t>& indices = model.indices.get();
    const std::vector<asset::SkinnedVertex>& vertices = model.vertices.get();
    double area = 0.0;
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const std::uint32_t a = indices[i];
        const std::uint32_t b = indices[i + 1];
        const std::uint32_t c = indices[i + 2];
        if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size()) continue;
        const Vec3& pa = vertices[a].position;
        area += 0.5 * static_cast<double>(core::length(core::cross(vertices[b].position - pa, vertices[c].position - pa)));
    }
    return static_cast<float>(area);
}

bool applyLightmapUvs(asset::ModelData& model, const LightmapUvLayout& layout) {
    const std::size_t vertex_count = model.vertices.size();
    const std::size_t index_count = model.indices.size();
    const std::size_t added = layout.duplicates.size();
    if (layout.source_vertices != vertex_count || layout.source_indices != index_count ||
        layout.uv.size() != vertex_count + added || layout.patch_corner.size() != layout.patch_vertex.size()) {
        return false;
    }
    for (const std::uint32_t v : layout.duplicates) {
        if (v >= vertex_count) return false;
    }
    for (std::size_t j = 0; j < layout.patch_corner.size(); ++j) {
        if (layout.patch_corner[j] >= index_count || layout.patch_vertex[j] < vertex_count ||
            layout.patch_vertex[j] >= vertex_count + added) {
            return false;
        }
    }
    // El hash al final: es lo mas caro (recorre toda la malla).
    if (layout.source_hash != lightmapMeshHash(model)) return false;

    std::vector<asset::SkinnedVertex>& vertices = model.vertices.mut();
    vertices.reserve(vertex_count + added);
    for (std::size_t i = 0; i < added; ++i) {
        const asset::SkinnedVertex copy = vertices[layout.duplicates[i]];
        vertices.push_back(copy);
    }
    if (!layout.patch_corner.empty()) {
        std::vector<std::uint32_t>& indices = model.indices.mut();
        for (std::size_t j = 0; j < layout.patch_corner.size(); ++j) indices[layout.patch_corner[j]] = layout.patch_vertex[j];
    }
    model.lightmap_uvs.resize(layout.uv.size());
    for (std::size_t i = 0; i < layout.uv.size(); ++i) model.lightmap_uvs[i] = unpackLightmapUv(layout.uv[i]);
    model.lightmap_layout_hash = layout.layout_hash;
    // Los LODs apuntaban a los vertices viejos: se rehacen sobre los nuevos
    // (cada esquina con la UV de su carta; MeshLod no usa el simplificado
    // "sloppy" con UV2).
    if (!model.lods.empty() || !model.lod_indices.empty()) {
        model.lods.clear();
        model.lod_indices.clear();
        asset::generateLods(model);
    }
    return true;
}

std::vector<std::uint8_t> encodeLightmapUvLayout(const LightmapUvLayout& l) {
    Writer w;
    w.bytes.reserve(96 + (l.duplicates.size() + l.patch_corner.size() * 2 + l.uv.size()) * 4);
    w.put(l.source_hash);
    w.put(l.source_vertices);
    w.put(l.source_indices);
    w.put(l.settings_hash);
    w.put(l.layout_hash);
    w.put(l.width);
    w.put(l.height);
    w.put(l.texels_per_unit);
    w.put(l.charts);
    w.put(l.ignored_triangles);
    w.put(l.utilization);
    w.put(l.lightmapped_area);
    w.put(static_cast<std::uint32_t>(l.duplicates.size()));
    w.put(static_cast<std::uint32_t>(l.patch_corner.size()));
    w.put(static_cast<std::uint32_t>(l.uv.size()));
    w.putArray(l.duplicates);
    w.putArray(l.patch_corner);
    w.putArray(l.patch_vertex);
    w.putArray(l.uv);
    return std::move(w.bytes);
}

bool decodeLightmapUvLayout(const std::uint8_t* data, std::size_t size, LightmapUvLayout& out) {
    Reader r{data, size};
    LightmapUvLayout l;
    std::uint32_t duplicates = 0, patches = 0, uvs = 0;
    if (!r.get(l.source_hash) || !r.get(l.source_vertices) || !r.get(l.source_indices) || !r.get(l.settings_hash) ||
        !r.get(l.layout_hash) || !r.get(l.width) || !r.get(l.height) || !r.get(l.texels_per_unit) || !r.get(l.charts) ||
        !r.get(l.ignored_triangles) || !r.get(l.utilization) || !r.get(l.lightmapped_area) || !r.get(duplicates) ||
        !r.get(patches) || !r.get(uvs)) {
        return false;
    }
    // Limites de cordura (un archivo danado no reserva gigas) y coherencia.
    if (duplicates > (1u << 28) || patches > (1u << 30) || uvs > (1u << 29) ||
        static_cast<std::uint64_t>(uvs) != static_cast<std::uint64_t>(l.source_vertices) + duplicates) {
        return false;
    }
    if (!r.getArray(l.duplicates, duplicates) || !r.getArray(l.patch_corner, patches) ||
        !r.getArray(l.patch_vertex, patches) || !r.getArray(l.uv, uvs)) {
        return false;
    }
    out = std::move(l);
    return true;
}

bool saveLightmapUvLayout(const std::filesystem::path& file, const LightmapUvLayout& layout) {
    const std::vector<std::uint8_t> raw = encodeLightmapUvLayout(layout);
    std::vector<std::uint8_t> packed(ZSTD_compressBound(raw.size()));
    const std::size_t stored = ZSTD_compress(packed.data(), packed.size(), raw.data(), raw.size(), 6);
    if (ZSTD_isError(stored)) return false;
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    // A un temporal y renombrar: dos hilos (o el editor y una herramienta)
    // escribiendo la misma pieza no dejan un archivo a medias.
    std::filesystem::path temporary = file;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        const std::uint64_t raw_size = raw.size();
        out.write(kMagic, sizeof(kMagic));
        out.write(reinterpret_cast<const char*>(&kFileVersion), sizeof(kFileVersion));
        out.write(reinterpret_cast<const char*>(&raw_size), sizeof(raw_size));
        out.write(reinterpret_cast<const char*>(packed.data()), static_cast<std::streamsize>(stored));
        if (!out) return false;
    }
    std::filesystem::rename(temporary, file, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

bool loadLightmapUvLayout(const std::filesystem::path& file, LightmapUvLayout& out) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(file, error);
    if (error || size < 16) return false;
    std::vector<std::uint8_t> bytes;
    if (!readBytes(file, 0, size, bytes)) return false;
    std::uint32_t version = 0;
    std::uint64_t raw_size = 0;
    std::memcpy(&version, bytes.data() + 4, sizeof(version));
    std::memcpy(&raw_size, bytes.data() + 8, sizeof(raw_size));
    if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0 || version != kFileVersion || raw_size > (1ull << 32)) {
        return false;
    }
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(raw_size));
    const std::size_t got = ZSTD_decompress(raw.data(), raw.size(), bytes.data() + 16, bytes.size() - 16);
    if (ZSTD_isError(got) || got != raw.size()) return false;
    return decodeLightmapUvLayout(raw.data(), raw.size(), out);
}

std::shared_ptr<const LightmapUvLayout> LightmapUvSet::load(const Uuid& model, int part) const {
    const auto it = parts.find(LightmapUvPartKey{model, part});
    if (it == parts.end()) return nullptr;
    const Source& source = it->second;
    if (source.layout) return source.layout;
    auto layout = std::make_shared<LightmapUvLayout>();
    if (source.stored == 0) {
        // Un .cruv de Library/Cache/LightmapUV.
        if (!loadLightmapUvLayout(source.file, *layout)) return nullptr;
        return layout;
    }
    // Un trozo UVLY de un .crbake: [offset, offset + stored).
    std::vector<std::uint8_t> stored;
    if (!readBytes(source.file, source.offset, source.stored, stored)) return nullptr;
    if (!source.zstd) {
        if (!decodeLightmapUvLayout(stored.data(), stored.size(), *layout)) return nullptr;
        return layout;
    }
    if (source.raw > (1ull << 32)) return nullptr;
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(source.raw));
    const std::size_t got = ZSTD_decompress(raw.data(), raw.size(), stored.data(), stored.size());
    if (ZSTD_isError(got) || got != raw.size()) return nullptr;
    if (!decodeLightmapUvLayout(raw.data(), raw.size(), *layout)) return nullptr;
    return layout;
}

std::filesystem::path lightmapUvCacheFile(const std::filesystem::path& cache_folder, std::uint64_t source_hash,
                                          std::uint64_t settings_hash) {
    char name[64];
    std::snprintf(name, sizeof(name), "%016llx_%016llx.cruv", static_cast<unsigned long long>(source_hash),
                  static_cast<unsigned long long>(settings_hash));
    return cache_folder / name;
}

}  // namespace cramion::lighting
