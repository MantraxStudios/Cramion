#include "CramionCore/project/Pack.h"

#include <zstd.h>

#include <cstring>
#include <fstream>
#include <memory>

namespace cramion::project {

namespace {

constexpr char kMagic[4] = {'C', 'R', 'P', 'K'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kHeaderSize = 4 + 4 + 4 + 8;
constexpr std::size_t kChunk = 1u << 20;

template <typename T>
void put(std::ostream& out, T value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
bool get(std::istream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(in);
}

void fail(std::string* error, const std::string& text) {
    if (error != nullptr) *error = text;
}

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path fromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

struct CCtxDeleter {
    void operator()(ZSTD_CCtx* c) const { ZSTD_freeCCtx(c); }
};
struct DCtxDeleter {
    void operator()(ZSTD_DCtx* d) const { ZSTD_freeDCtx(d); }
};

// Ruta segura dentro del paquete: relativa y sin "..".
bool safeRelative(const std::string& path) {
    if (path.empty() || path.size() > 4096) return false;
    const std::filesystem::path p = fromUtf8(path);
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return false;
    for (const auto& part : p) {
        if (part == "..") return false;
    }
    return true;
}

}  // namespace

bool writePack(const std::filesystem::path& file, const std::vector<PackInput>& inputs, int level,
               const PackProgress& progress, std::string* error) {
    std::filesystem::path temp = file;
    temp += ".tmp";
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
        fail(error, "No se pudo crear " + utf8(file));
        return false;
    }
    out.write(kMagic, 4);
    put<std::uint32_t>(out, kVersion);
    put<std::uint32_t>(out, 0);  // archivos (al final)
    put<std::uint64_t>(out, 0);  // indice (al final)

    std::unique_ptr<ZSTD_CCtx, CCtxDeleter> cctx(ZSTD_createCCtx());
    ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_compressionLevel, level);
    ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_checksumFlag, 1);
    std::vector<char> in_buffer(kChunk);
    std::vector<char> out_buffer(ZSTD_CStreamOutSize());
    std::vector<PackEntry> entries;
    entries.reserve(inputs.size());
    std::uint64_t done = 0;
    bool ok = true;

    for (const PackInput& input : inputs) {
        if (!safeRelative(input.path)) {
            fail(error, "Ruta no valida en el paquete: " + input.path);
            ok = false;
            break;
        }
        if (progress && !progress(done, input.path)) {
            fail(error, "Cancelado");
            ok = false;
            break;
        }
        std::ifstream src(input.source, std::ios::binary);
        if (!src) {
            fail(error, "No se pudo leer " + utf8(input.source));
            ok = false;
            break;
        }
        PackEntry entry;
        entry.path = input.path;
        entry.offset = static_cast<std::uint64_t>(out.tellp());
        ZSTD_CCtx_reset(cctx.get(), ZSTD_reset_session_only);
        const std::uint64_t source_size = std::filesystem::file_size(input.source, ec);
        if (!ec) ZSTD_CCtx_setPledgedSrcSize(cctx.get(), source_size);
        bool last = false;
        while (!last) {
            src.read(in_buffer.data(), static_cast<std::streamsize>(in_buffer.size()));
            const std::size_t n = static_cast<std::size_t>(src.gcount());
            last = n < in_buffer.size();
            entry.size += n;
            ZSTD_inBuffer zin{in_buffer.data(), n, 0};
            const ZSTD_EndDirective mode = last ? ZSTD_e_end : ZSTD_e_continue;
            bool finished = false;
            while (!finished) {
                ZSTD_outBuffer zout{out_buffer.data(), out_buffer.size(), 0};
                const std::size_t remaining = ZSTD_compressStream2(cctx.get(), &zout, &zin, mode);
                if (ZSTD_isError(remaining)) {
                    fail(error, std::string("zstd: ") + ZSTD_getErrorName(remaining));
                    return false;
                }
                out.write(out_buffer.data(), static_cast<std::streamsize>(zout.pos));
                finished = last ? remaining == 0 : zin.pos == zin.size;
            }
            done += n;
            if (!last && progress && !progress(done, input.path)) {
                fail(error, "Cancelado");
                ok = false;
                break;
            }
        }
        if (!ok) break;
        entry.compressed = static_cast<std::uint64_t>(out.tellp()) - entry.offset;
        entries.push_back(std::move(entry));
        if (!out) {
            fail(error, "No se pudo escribir " + utf8(file) + " (disco lleno?)");
            ok = false;
            break;
        }
    }

    if (ok) {
        const std::uint64_t index = static_cast<std::uint64_t>(out.tellp());
        for (const PackEntry& e : entries) {
            put<std::uint16_t>(out, static_cast<std::uint16_t>(e.path.size()));
            out.write(e.path.data(), static_cast<std::streamsize>(e.path.size()));
            put<std::uint64_t>(out, e.offset);
            put<std::uint64_t>(out, e.compressed);
            put<std::uint64_t>(out, e.size);
        }
        out.seekp(8);
        put<std::uint32_t>(out, static_cast<std::uint32_t>(entries.size()));
        put<std::uint64_t>(out, index);
        out.close();
        ok = static_cast<bool>(out);
        if (!ok) fail(error, "No se pudo escribir " + utf8(file));
    }
    if (!ok) {
        out.close();
        std::filesystem::remove(temp, ec);
        return false;
    }
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        std::filesystem::remove(file, ec);
        std::filesystem::rename(temp, file, ec);
    }
    if (ec) {
        fail(error, "No se pudo reemplazar " + utf8(file) + ": " + ec.message());
        return false;
    }
    if (progress) progress(done, {});
    return true;
}

bool readPackIndex(const std::filesystem::path& file, std::vector<PackEntry>& entries, std::string* error) {
    entries.clear();
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        fail(error, "No se pudo abrir " + utf8(file));
        return false;
    }
    char magic[4] = {};
    std::uint32_t version = 0;
    std::uint32_t count = 0;
    std::uint64_t index = 0;
    in.read(magic, 4);
    if (!in || std::memcmp(magic, kMagic, 4) != 0 || !get(in, version) || !get(in, count) || !get(in, index)) {
        fail(error, utf8(file) + " no es un paquete .crpack");
        return false;
    }
    if (version != kVersion) {
        fail(error, "Version de .crpack no soportada");
        return false;
    }
    std::error_code ec;
    const std::uint64_t file_size = std::filesystem::file_size(file, ec);
    if (ec || index < kHeaderSize || index > file_size) {
        fail(error, "Paquete danado (indice)");
        return false;
    }
    in.seekg(static_cast<std::streamoff>(index));
    entries.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint16_t length = 0;
        PackEntry e;
        if (!get(in, length)) break;
        e.path.resize(length);
        in.read(e.path.data(), length);
        if (!get(in, e.offset) || !get(in, e.compressed) || !get(in, e.size)) break;
        if (e.offset < kHeaderSize || e.offset + e.compressed > index || !safeRelative(e.path)) {
            fail(error, "Paquete danado (" + e.path + ")");
            return false;
        }
        entries.push_back(std::move(e));
    }
    if (entries.size() != count) {
        fail(error, "Paquete danado (indice incompleto)");
        return false;
    }
    return true;
}

bool extractPack(const std::filesystem::path& file, const std::filesystem::path& folder,
                 const PackProgress& progress, std::string* error) {
    std::vector<PackEntry> entries;
    if (!readPackIndex(file, entries, error)) return false;
    std::ifstream in(file, std::ios::binary);
    std::unique_ptr<ZSTD_DCtx, DCtxDeleter> dctx(ZSTD_createDCtx());
    std::vector<char> in_buffer(ZSTD_DStreamInSize());
    std::vector<char> out_buffer(ZSTD_DStreamOutSize());
    std::uint64_t done = 0;
    std::error_code ec;
    for (const PackEntry& e : entries) {
        if (progress && !progress(done, e.path)) {
            fail(error, "Cancelado");
            return false;
        }
        const std::filesystem::path target = folder / fromUtf8(e.path);
        std::filesystem::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            fail(error, "No se pudo escribir " + utf8(target));
            return false;
        }
        ZSTD_DCtx_reset(dctx.get(), ZSTD_reset_session_only);
        in.seekg(static_cast<std::streamoff>(e.offset));
        std::uint64_t left = e.compressed;
        std::uint64_t written = 0;
        std::size_t last_result = 1;
        while (left > 0) {
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(left, in_buffer.size()));
            in.read(in_buffer.data(), static_cast<std::streamsize>(n));
            if (!in) {
                fail(error, "Paquete danado (" + e.path + ")");
                return false;
            }
            left -= n;
            ZSTD_inBuffer zin{in_buffer.data(), n, 0};
            while (zin.pos < zin.size) {
                ZSTD_outBuffer zout{out_buffer.data(), out_buffer.size(), 0};
                last_result = ZSTD_decompressStream(dctx.get(), &zout, &zin);
                if (ZSTD_isError(last_result)) {
                    fail(error, "Paquete danado (" + e.path + "): " + ZSTD_getErrorName(last_result));
                    return false;
                }
                out.write(out_buffer.data(), static_cast<std::streamsize>(zout.pos));
                written += zout.pos;
            }
        }
        // Lo que quede en el descompresor (sin mas entrada).
        while (last_result != 0) {
            ZSTD_inBuffer zin{nullptr, 0, 0};
            ZSTD_outBuffer zout{out_buffer.data(), out_buffer.size(), 0};
            last_result = ZSTD_decompressStream(dctx.get(), &zout, &zin);
            if (ZSTD_isError(last_result) || zout.pos == 0) {
                fail(error, "Paquete danado (" + e.path + ", incompleto)");
                return false;
            }
            out.write(out_buffer.data(), static_cast<std::streamsize>(zout.pos));
            written += zout.pos;
        }
        if (written != e.size || !out) {
            fail(error, "Paquete danado (" + e.path + ", tamano)");
            return false;
        }
        done += e.size;
    }
    if (progress) progress(done, {});
    return true;
}

std::string packId(const std::filesystem::path& file) {
    std::vector<PackEntry> entries;
    if (!readPackIndex(file, entries, nullptr)) return {};
    // FNV-1a de las rutas, tamanos y posiciones, mas el tamano del archivo.
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&](const void* data, std::size_t n) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < n; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
    };
    std::error_code ec;
    const std::uint64_t size = std::filesystem::file_size(file, ec);
    mix(&size, sizeof(size));
    for (const PackEntry& e : entries) {
        mix(e.path.data(), e.path.size());
        mix(&e.offset, sizeof(e.offset));
        mix(&e.compressed, sizeof(e.compressed));
        mix(&e.size, sizeof(e.size));
    }
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

}  // namespace cramion::project
