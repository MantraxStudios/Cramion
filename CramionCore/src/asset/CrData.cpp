#include "CrData.h"

#include <CramionFX/asset/ModelCache.h>

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <type_traits>

namespace cramion::assets::crdata {

namespace {

// --- Primitivas de lectura/escritura binaria ---------------------------------

template <typename T>
void put(std::ostream& out, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

void putString(std::ostream& out, const std::string& text) {
    put(out, static_cast<std::uint64_t>(text.size()));
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

template <typename T>
bool get(std::istream& in, T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(in);
}

// Tope para cadenas: una cabecera danada no debe pedir gigas de memoria.
constexpr std::uint64_t kMaxString = 1ull << 26;

bool getString(std::istream& in, std::string& text) {
    std::uint64_t size = 0;
    if (!get(in, size) || size > kMaxString) {
        return false;
    }
    text.resize(static_cast<std::size_t>(size));
    in.read(text.data(), static_cast<std::streamsize>(size));
    return static_cast<bool>(in);
}

void writeHeader(std::ostream& out, const Header& header) {
    out.write(kMagic, sizeof(kMagic));
    put(out, kVersion);
    put(out, static_cast<std::uint32_t>(header.type));
    put(out, header.uuid.high);
    put(out, header.uuid.low);
    putString(out, header.name);
    putString(out, header.source);
    putString(out, header.settings_json);
}

bool readHeaderFrom(std::istream& in, Header& header) {
    char magic[sizeof(kMagic)] = {};
    in.read(magic, sizeof(magic));
    if (!in || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
        return false;
    }
    std::uint32_t version = 0;
    std::uint32_t type = 0;
    if (!get(in, version) || version != kVersion || !get(in, type)) {
        return false;
    }
    header.type = static_cast<AssetType>(type);
    return get(in, header.uuid.high) && get(in, header.uuid.low) && getString(in, header.name) &&
           getString(in, header.source) && getString(in, header.settings_json) &&
           get(in, header.payload_bytes);
}

// Escribe la cabecera con el tamano del contenido a 0 y devuelve donde esta
// ese campo, para corregirlo al terminar (sin copiar el contenido a memoria).
std::streampos beginPayload(std::ostream& out, const Header& header) {
    writeHeader(out, header);
    const std::streampos size_field = out.tellp();
    put(out, std::uint64_t{0});
    return size_field;
}

void endPayload(std::ostream& out, std::streampos size_field) {
    const std::streampos end = out.tellp();
    const auto bytes = static_cast<std::uint64_t>(end - size_field) - sizeof(std::uint64_t);
    out.seekp(size_field);
    put(out, bytes);
    out.seekp(end);
}

// Escritura atomica: si algo falla a medias no queda un .crdata roto con
// apariencia de valido.
template <typename Write>
void writeAtomically(const std::filesystem::path& file, Write&& write) {
    std::filesystem::path temporary = file;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("No se pudo crear " + utf8(temporary));
        }
        write(out);
        out.flush();
        if (!out) {
            throw std::runtime_error("Error escribiendo " + utf8(temporary));
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, file, error);
    if (error) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("No se pudo guardar " + utf8(file) + ": " + error.message());
    }
}

}  // namespace

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::optional<Header> readHeader(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    Header header{};
    if (!readHeaderFrom(in, header)) {
        return std::nullopt;
    }
    return header;
}

void writeModel(const std::filesystem::path& file, const Header& header,
                const ModelContent& content) {
    writeAtomically(file, [&](std::ostream& out) {
        Header h = header;
        h.type = AssetType::Model;
        const std::streampos size_field = beginPayload(out, h);

        put(out, static_cast<std::uint32_t>(content.nodes.size()));
        for (const ModelNode& node : content.nodes) {
            putString(out, node.name);
            put(out, node.parent);
            put(out, node.local);
            put(out, node.part);
        }
        put(out, static_cast<std::uint32_t>(content.animated ? 1 : 0));
        put(out, static_cast<std::uint32_t>(content.animation_names.size()));
        for (const std::string& name : content.animation_names) {
            putString(out, name);
        }

        put(out, static_cast<std::uint32_t>(content.parts.size()));
        for (const asset::ModelData& part : content.parts) {
            // Tamano de la pieza delante: permite saltarla sin leerla.
            const std::streampos part_size = out.tellp();
            put(out, std::uint64_t{0});
            asset::writeModelPayload(out, part);
            const std::streampos end = out.tellp();
            out.seekp(part_size);
            put(out, static_cast<std::uint64_t>(end - part_size) - sizeof(std::uint64_t));
            out.seekp(end);
        }
        endPayload(out, size_field);
    });
}

bool readModel(const std::filesystem::path& file, Header& header, ModelContent& content) {
    std::ifstream in(file, std::ios::binary);
    if (!in || !readHeaderFrom(in, header) || header.type != AssetType::Model) {
        return false;
    }

    ModelContent result{};
    std::uint32_t count = 0;
    if (!get(in, count) || count > (1u << 24)) {
        return false;
    }
    result.nodes.resize(count);
    for (ModelNode& node : result.nodes) {
        if (!getString(in, node.name) || !get(in, node.parent) || !get(in, node.local) ||
            !get(in, node.part)) {
            return false;
        }
    }
    std::uint32_t animated = 0;
    if (!get(in, animated) || !get(in, count) || count > (1u << 20)) {
        return false;
    }
    result.animated = animated != 0;
    result.animation_names.resize(count);
    for (std::string& name : result.animation_names) {
        if (!getString(in, name)) {
            return false;
        }
    }

    if (!get(in, count) || count > (1u << 20)) {
        return false;
    }
    result.parts.resize(count);
    for (asset::ModelData& part : result.parts) {
        std::uint64_t bytes = 0;
        if (!get(in, bytes)) {
            return false;
        }
        const std::streampos start = in.tellg();
        if (!asset::readModelPayload(in, part)) {
            return false;
        }
        // Robustez: seguir desde donde dice el tamano, no desde donde acabo
        // de leer (versiones futuras pueden anadir datos al final).
        in.seekg(start + static_cast<std::streamoff>(bytes));
    }
    content = std::move(result);
    return true;
}

void writeEnvironment(const std::filesystem::path& file, const Header& header,
                      const std::string& extension, const std::vector<std::uint8_t>& bytes) {
    writeAtomically(file, [&](std::ostream& out) {
        Header h = header;
        h.type = AssetType::Environment;
        const std::streampos size_field = beginPayload(out, h);
        putString(out, extension);
        put(out, static_cast<std::uint64_t>(bytes.size()));
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        endPayload(out, size_field);
    });
}

bool readEnvironment(const std::filesystem::path& file, Header& header, std::string& extension,
                     std::vector<std::uint8_t>& bytes) {
    std::ifstream in(file, std::ios::binary);
    if (!in || !readHeaderFrom(in, header) || header.type != AssetType::Environment) {
        return false;
    }
    std::uint64_t size = 0;
    if (!getString(in, extension) || !get(in, size) || size > header.payload_bytes) {
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    return static_cast<bool>(in);
}

std::filesystem::path fromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::filesystem::path uniquePath(const std::filesystem::path& folder, const std::string& stem,
                                 const std::string& extension) {
    std::filesystem::path candidate = folder / fromUtf8(stem + extension);
    for (int i = 1; std::filesystem::exists(candidate); ++i) {
        candidate = folder / fromUtf8(stem + " " + std::to_string(i) + extension);
    }
    return candidate;
}

}  // namespace cramion::assets::crdata
