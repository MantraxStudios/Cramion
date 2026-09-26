#include "CramionFX/asset/ModelCache.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace cramion::asset {
namespace {

// Cambiar si cambia cualquier estructura de asset/Model.h que se guarda.
constexpr std::uint32_t kModelCacheVersion = 11;  // 11: clusteres sin escala fija
constexpr char kMagic[4] = {'C', 'R', 'M', 'C'};

// Identidad del archivo original: si cambia, la cache no vale.
struct SourceStamp {
    std::uint64_t size = 0;
    std::int64_t modified = 0;
};

SourceStamp stampOf(const std::filesystem::path& source) {
    SourceStamp stamp{};
    stamp.size = static_cast<std::uint64_t>(std::filesystem::file_size(source));
    stamp.modified = static_cast<std::int64_t>(
        std::filesystem::last_write_time(source).time_since_epoch().count());
    return stamp;
}

// --- Escritura ---------------------------------------------------------------

class Writer {
public:
    explicit Writer(std::ostream& file) : file_(file) {}

    template <typename T>
    void pod(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        file_.write(reinterpret_cast<const char*>(&value), sizeof(T));
    }

    template <typename T>
    void podVector(const std::vector<T>& values) {
        static_assert(std::is_trivially_copyable_v<T>);
        pod(static_cast<std::uint64_t>(values.size()));
        file_.write(reinterpret_cast<const char*>(values.data()),
                    static_cast<std::streamsize>(values.size() * sizeof(T)));
    }

    void string(const std::string& text) {
        pod(static_cast<std::uint64_t>(text.size()));
        file_.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    bool ok() const { return static_cast<bool>(file_); }

private:
    std::ostream& file_;
};

// --- Lectura -----------------------------------------------------------------

class Reader {
public:
    explicit Reader(std::istream& file) : file_(file) {}

    template <typename T>
    void pod(T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        read(&value, sizeof(T));
    }

    template <typename T>
    void podVector(std::vector<T>& values) {
        static_assert(std::is_trivially_copyable_v<T>);
        std::uint64_t count = 0;
        pod(count);
        values.resize(static_cast<std::size_t>(count));
        read(values.data(), values.size() * sizeof(T));
    }

    void string(std::string& text) {
        std::uint64_t count = 0;
        pod(count);
        text.resize(static_cast<std::size_t>(count));
        read(text.data(), text.size());
    }

private:
    void read(void* data, std::size_t size) {
        if (size == 0) {
            return;
        }
        file_.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
        if (!file_) {
            throw std::runtime_error("datos del modelo truncados");
        }
    }

    std::istream& file_;
};

// Los tipos que se guardan como bloques de bytes.
static_assert(std::is_trivially_copyable_v<SkinnedVertex>);
static_assert(std::is_trivially_copyable_v<SubMesh>);
static_assert(std::is_trivially_copyable_v<VectorKey>);
static_assert(std::is_trivially_copyable_v<QuatKey>);
static_assert(std::is_trivially_copyable_v<core::Mat4>);

void writeMaterial(Writer& out, const MaterialData& m) {
    out.string(m.name);
    out.pod(m.base_color);
    out.pod(m.emissive);
    out.pod(m.metallic);
    out.pod(m.roughness);
    out.pod(m.occlusion_strength);
    out.pod(m.normal_scale);
    out.pod(m.reflectance);
    out.pod(static_cast<std::uint8_t>(m.transparent ? 1 : 0));
    out.pod(static_cast<std::uint8_t>(m.normal_map_directx ? 1 : 0));
    out.pod(m.albedo_texture);
    out.pod(m.metallic_roughness_texture);
    out.pod(m.normal_texture);
    out.pod(m.occlusion_texture);
    out.pod(m.emissive_texture);
}

void readMaterial(Reader& in, MaterialData& m) {
    in.string(m.name);
    in.pod(m.base_color);
    in.pod(m.emissive);
    in.pod(m.metallic);
    in.pod(m.roughness);
    in.pod(m.occlusion_strength);
    in.pod(m.normal_scale);
    in.pod(m.reflectance);
    std::uint8_t transparent = 0;
    in.pod(transparent);
    m.transparent = transparent != 0;
    std::uint8_t directx = 0;
    in.pod(directx);
    m.normal_map_directx = directx != 0;
    in.pod(m.albedo_texture);
    in.pod(m.metallic_roughness_texture);
    in.pod(m.normal_texture);
    in.pod(m.occlusion_texture);
    in.pod(m.emissive_texture);
}

void writePayload(Writer& out, const ModelData& model) {
    out.string(model.name);
    out.podVector(model.vertices);
    out.podVector(model.indices);
    out.podVector(model.submeshes);

    out.pod(static_cast<std::uint64_t>(model.materials.size()));
    for (const MaterialData& material : model.materials) {
        writeMaterial(out, material);
    }

    out.pod(static_cast<std::uint64_t>(model.textures.size()));
    for (const TextureData& texture : model.textures) {
        out.string(texture.name);
        out.pod(texture.width);
        out.pod(texture.height);
        out.pod(texture.format);
        out.pod(texture.mip_levels);
        out.podVector(texture.pixels);
        // Las que tienen archivo en disco se releen de el: no se duplican.
        if (texture.source_path.empty()) {
            out.podVector(texture.encoded);
        } else {
            out.podVector(std::vector<std::uint8_t>{});
        }
        out.string(texture.source_path);
        out.pod(static_cast<std::uint8_t>(texture.height_map ? 1 : 0));
    }

    out.pod(static_cast<std::uint64_t>(model.nodes.size()));
    for (const Node& node : model.nodes) {
        out.string(node.name);
        out.pod(node.parent);
        out.pod(node.local);
    }

    out.pod(static_cast<std::uint64_t>(model.bones.size()));
    for (const Bone& bone : model.bones) {
        out.string(bone.name);
        out.pod(bone.node);
        out.pod(bone.offset);
    }

    out.pod(static_cast<std::uint64_t>(model.animations.size()));
    for (const AnimationClip& clip : model.animations) {
        out.string(clip.name);
        out.pod(clip.duration);
        out.pod(static_cast<std::uint64_t>(clip.channels.size()));
        for (const AnimationChannel& channel : clip.channels) {
            out.pod(channel.node);
            out.podVector(channel.positions);
            out.podVector(channel.rotations);
            out.podVector(channel.scales);
        }
    }
}

void readPayload(Reader& in, ModelData& model) {
    in.string(model.name);
    in.podVector(model.vertices);
    in.podVector(model.indices);
    in.podVector(model.submeshes);

    std::uint64_t count = 0;
    in.pod(count);
    model.materials.resize(static_cast<std::size_t>(count));
    for (MaterialData& material : model.materials) {
        readMaterial(in, material);
    }

    in.pod(count);
    model.textures.resize(static_cast<std::size_t>(count));
    for (TextureData& texture : model.textures) {
        in.string(texture.name);
        in.pod(texture.width);
        in.pod(texture.height);
        in.pod(texture.format);
        in.pod(texture.mip_levels);
        in.podVector(texture.pixels);
        in.podVector(texture.encoded);
        in.string(texture.source_path);
        std::uint8_t height_map = 0;
        in.pod(height_map);
        texture.height_map = height_map != 0;
    }

    in.pod(count);
    model.nodes.resize(static_cast<std::size_t>(count));
    for (Node& node : model.nodes) {
        in.string(node.name);
        in.pod(node.parent);
        in.pod(node.local);
    }

    in.pod(count);
    model.bones.resize(static_cast<std::size_t>(count));
    for (Bone& bone : model.bones) {
        in.string(bone.name);
        in.pod(bone.node);
        in.pod(bone.offset);
    }

    in.pod(count);
    model.animations.resize(static_cast<std::size_t>(count));
    for (AnimationClip& clip : model.animations) {
        in.string(clip.name);
        in.pod(clip.duration);
        std::uint64_t channels = 0;
        in.pod(channels);
        clip.channels.resize(static_cast<std::size_t>(channels));
        for (AnimationChannel& channel : clip.channels) {
            in.pod(channel.node);
            in.podVector(channel.positions);
            in.podVector(channel.rotations);
            in.podVector(channel.scales);
        }
    }
}

}  // namespace

void writeModelCache(const std::filesystem::path& cache, const std::filesystem::path& source,
                     const ModelData& model) {
    // Se escribe a un temporal y se renombra al final: una escritura
    // interrumpida nunca deja una cache a medias con apariencia de valida.
    const std::filesystem::path temporary = cache.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary);
        if (!file) {
            throw std::runtime_error("No se pudo crear la cache " + temporary.string());
        }
        Writer out(file);
        out.pod(kMagic);
        out.pod(kModelCacheVersion);
        out.pod(stampOf(source));

        writePayload(out, model);

        if (!out.ok()) {
            throw std::runtime_error("Error escribiendo la cache " + temporary.string());
        }
    }
    std::filesystem::rename(temporary, cache);
}

bool readModelCache(const std::filesystem::path& cache, const std::filesystem::path& source,
                    ModelData& out) {
    std::error_code error;
    if (!std::filesystem::exists(cache, error)) {
        return false;
    }

    std::ifstream file(cache, std::ios::binary);
    if (!file) {
        return false;
    }
    Reader in(file);

    try {
        char magic[4] = {};
        std::uint32_t version = 0;
        SourceStamp stamp{};
        in.pod(magic);
        in.pod(version);
        in.pod(stamp);

        const SourceStamp current = stampOf(source);
        if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0 || version != kModelCacheVersion ||
            stamp.size != current.size || stamp.modified != current.modified) {
            return false;  // De otra version del motor o de otro archivo.
        }

        ModelData model{};
        readPayload(in, model);
        out = std::move(model);
        return true;
    } catch (const std::exception&) {
        return false;  // Cache corrupta o truncada: se reimporta.
    }
}

void writeModelPayload(std::ostream& stream, const ModelData& model) {
    Writer out(stream);
    writePayload(out, model);
    if (!out.ok()) {
        throw std::runtime_error("Error escribiendo los datos del modelo");
    }
}

bool readModelPayload(std::istream& stream, ModelData& out) {
    try {
        Reader in(stream);
        ModelData model{};
        readPayload(in, model);
        out = std::move(model);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace cramion::asset
