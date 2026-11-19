#include "CramionFX/asset/ModelCache.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace cramion::asset {
namespace {

// Cambiar si cambia cualquier estructura de asset/Model.h que se guarda.
constexpr std::uint32_t kModelCacheVersion = 13;  // 13: color de vertice (bloque al final)
constexpr char kMagic[4] = {'C', 'R', 'M', 'C'};
// Bloques opcionales al final del contenido (desde la 13): una etiqueta y
// sus datos. Los .crdata antiguos no los tienen; quien lee sabe donde acaba
// la pieza (readModelPayload con `payload_end`) y no lee de mas.
constexpr std::uint32_t kVertexColorTag = 0x4C4F4356u;  // "VCOL"
// Formas de mezcla (morph targets) y sus pistas: etiqueta, version del
// bloque y bytes que ocupa (una version futura del bloque se salta entera).
// Va siempre detras de VCOL: un lector viejo para en la primera etiqueta
// que no conoce y asi no pierde el color de vertice.
constexpr std::uint32_t kBlendShapeTag = 0x50485342u;  // "BSHP"
constexpr std::uint32_t kBlendShapeBlockVersion = 1;

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

    // Bytes que quedan hasta `end` (posicion absoluta; -1 = sin limite: el
    // final del archivo).
    std::int64_t remaining(std::streampos end) {
        const std::streampos here = file_.tellg();
        if (here < 0) return 0;
        if (end < 0) {
            file_.seekg(0, std::ios::end);
            const std::streampos last = file_.tellg();
            file_.seekg(here);
            return last < 0 ? 0 : static_cast<std::int64_t>(last - here);
        }
        return static_cast<std::int64_t>(end - here);
    }

    // Como podVector y string, sin pasar de `end`: un tamano danado (bloque
    // BSHP) no reserva gigas antes de fallar.
    template <typename T>
    void podVectorWithin(std::vector<T>& values, std::streampos end) {
        static_assert(std::is_trivially_copyable_v<T>);
        std::uint64_t count = 0;
        pod(count);
        const std::int64_t left = remaining(end);
        if (left < 0 || count > static_cast<std::uint64_t>(left) / sizeof(T)) {
            throw std::runtime_error("datos del modelo corruptos");
        }
        values.resize(static_cast<std::size_t>(count));
        read(values.data(), values.size() * sizeof(T));
    }
    void stringWithin(std::string& text, std::streampos end) {
        std::uint64_t count = 0;
        pod(count);
        const std::int64_t left = remaining(end);
        if (left < 0 || count > static_cast<std::uint64_t>(left)) throw std::runtime_error("datos del modelo corruptos");
        text.resize(static_cast<std::size_t>(count));
        read(text.data(), text.size());
    }

    std::streampos tell() { return file_.tellg(); }
    void seek(std::streampos position) {
        file_.clear();  // (tras un bloque que fallo a medias)
        file_.seekg(position);
        if (!file_) throw std::runtime_error("datos del modelo truncados");
    }

    // Un podVector saltado sin leerlo (no se trae del disco).
    template <typename T>
    void skipPodVector() {
        static_assert(std::is_trivially_copyable_v<T>);
        std::uint64_t count = 0;
        pod(count);
        if (count == 0) {
            return;
        }
        if (count > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) / sizeof(T)) {
            throw std::runtime_error("datos del modelo corruptos");
        }
        file_.seekg(static_cast<std::streamoff>(count * sizeof(T)), std::ios::cur);
        if (!file_) {
            throw std::runtime_error("datos del modelo truncados");
        }
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
static_assert(std::is_trivially_copyable_v<BlendShapeKey>);
static_assert(std::is_trivially_copyable_v<core::Vec3>);

// Bloque BSHP: las formas y, por toma (en el orden de model.animations),
// sus pistas de peso.
void writeBlendShapes(Writer& out, const ModelData& model) {
    out.pod(static_cast<std::uint64_t>(model.blend_shapes.size()));
    for (const BlendShape& shape : model.blend_shapes) {
        out.string(shape.name);
        out.pod(shape.default_weight);
        out.podVector(shape.vertices);
        out.podVector(shape.position_deltas);
        out.podVector(shape.normal_deltas);
    }
    out.pod(static_cast<std::uint64_t>(model.animations.size()));
    for (const AnimationClip& clip : model.animations) {
        out.pod(static_cast<std::uint64_t>(clip.blend_tracks.size()));
        for (const BlendShapeTrack& track : clip.blend_tracks) {
            out.pod(track.shape);
            out.podVector(track.keys);
        }
    }
}

void readBlendShapes(Reader& in, ModelData& model, std::streampos end) {
    std::uint64_t count = 0;
    in.pod(count);
    if (count > (1u << 20)) throw std::runtime_error("formas de mezcla corruptas");
    model.blend_shapes.resize(static_cast<std::size_t>(count));
    for (BlendShape& shape : model.blend_shapes) {
        in.stringWithin(shape.name, end);
        in.pod(shape.default_weight);
        in.podVectorWithin(shape.vertices, end);
        in.podVectorWithin(shape.position_deltas, end);
        in.podVectorWithin(shape.normal_deltas, end);
        // Coherencia: indices dentro del modelo y una delta por vertice.
        if (shape.position_deltas.size() != shape.vertices.size()) shape.position_deltas.resize(shape.vertices.size());
        if (!shape.normal_deltas.empty() && shape.normal_deltas.size() != shape.vertices.size()) {
            shape.normal_deltas.clear();
        }
        for (const std::uint32_t v : shape.vertices) {
            if (v >= model.vertices.size()) throw std::runtime_error("formas de mezcla fuera del modelo");
        }
    }
    in.pod(count);
    for (std::uint64_t c = 0; c < count; ++c) {
        std::uint64_t tracks = 0;
        in.pod(tracks);
        if (tracks > (1u << 20)) throw std::runtime_error("pistas de formas corruptas");
        std::vector<BlendShapeTrack> list(static_cast<std::size_t>(tracks));
        for (BlendShapeTrack& track : list) {
            in.pod(track.shape);
            in.podVectorWithin(track.keys, end);
        }
        if (c < model.animations.size()) {
            std::erase_if(list, [&](const BlendShapeTrack& t) { return t.shape >= model.blend_shapes.size(); });
            model.animations[static_cast<std::size_t>(c)].blend_tracks = std::move(list);
        }
    }
}

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
    out.podVector(model.vertices.get());
    out.podVector(model.indices.get());
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

    // --- Bloques opcionales (al final: los lectores viejos no los ven) ---
    if (!model.vertex_colors.empty() && model.vertex_colors.size() == model.vertices.size()) {
        out.pod(kVertexColorTag);
        out.podVector(model.vertex_colors.get());
    }
    if (!model.blend_shapes.empty()) {
        std::ostringstream block(std::ios::binary);
        Writer block_out(block);
        writeBlendShapes(block_out, model);
        const std::string bytes = block.str();
        out.pod(kBlendShapeTag);
        out.pod(kBlendShapeBlockVersion);
        out.string(bytes);  // tamano (u64) + bytes
    }
}

void readPayload(Reader& in, ModelData& model, bool skip_texture_bytes = false, std::streampos end = -1) {
    in.string(model.name);
    in.podVector(model.vertices.mut());
    in.podVector(model.indices.mut());
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
        if (skip_texture_bytes) {
            in.skipPodVector<std::uint8_t>();
            in.skipPodVector<std::uint8_t>();
        } else {
            in.podVector(texture.pixels);
            in.podVector(texture.encoded);
        }
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

    // --- Bloques opcionales hasta el final de la pieza ---
    while (in.remaining(end) >= static_cast<std::int64_t>(sizeof(std::uint32_t) + sizeof(std::uint64_t))) {
        std::uint32_t tag = 0;
        in.pod(tag);
        if (tag == kVertexColorTag) {
            in.podVector(model.vertex_colors.mut());
            if (model.vertex_colors.size() != model.vertices.size()) model.vertex_colors = std::vector<std::uint32_t>{};
        } else if (tag == kBlendShapeTag) {
            std::uint32_t version = 0;
            std::uint64_t bytes = 0;
            in.pod(version);
            in.pod(bytes);
            // Tamano imposible (bloque danado o cortado): sin formas, y lo que
            // viniera detras se ignora (no se puede saber donde empieza).
            const std::int64_t left = in.remaining(end);
            if (left < 0 || bytes > static_cast<std::uint64_t>(left)) break;
            const std::streampos block_end = in.tell() + static_cast<std::streamoff>(bytes);
            if (version == kBlendShapeBlockVersion) {
                try {
                    readBlendShapes(in, model, block_end);
                } catch (const std::exception&) {
                    // Bloque danado: el modelo vale igual, sin formas.
                    model.blend_shapes.clear();
                    for (AnimationClip& clip : model.animations) clip.blend_tracks.clear();
                }
            }
            in.seek(block_end);  // (una version futura del bloque se salta)
        } else {
            break;  // de una version futura: lo demas se ignora
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
        readPayload(in, model);  // (los bloques opcionales, hasta el final del archivo)
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

bool readModelPayload(std::istream& stream, ModelData& out, bool skip_texture_bytes, std::streampos payload_end) {
    try {
        Reader in(stream);
        ModelData model{};
        readPayload(in, model, skip_texture_bytes, payload_end);
        out = std::move(model);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace cramion::asset
