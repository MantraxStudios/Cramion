#include "CramionCore/project/SaveFile.h"

#include <zstd.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace cramion::project {

namespace {

constexpr char kMagic[4] = {'C', 'R', 'S', 'V'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kHeader = 4 + 4 + 8;
constexpr std::uint64_t kMaxSize = 512ull * 1024ull * 1024ull;  // una partida de mas de 512 MB es un error

void fail(std::string* error, const std::string& message) {
    if (error) *error = message;
}

}  // namespace

bool writeSaveFile(const std::filesystem::path& file, const std::string& text, bool compress, std::string* error) {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    std::filesystem::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            fail(error, "no se pudo crear el archivo de la partida");
            return false;
        }
        if (compress) {
            std::vector<char> packed(ZSTD_compressBound(text.size()));
            const std::size_t size = ZSTD_compress(packed.data(), packed.size(), text.data(), text.size(), 9);
            if (ZSTD_isError(size)) {
                fail(error, std::string("zstd: ") + ZSTD_getErrorName(size));
                return false;
            }
            const std::uint32_t version = kVersion;
            const std::uint64_t original = text.size();
            out.write(kMagic, 4);
            out.write(reinterpret_cast<const char*>(&version), sizeof(version));
            out.write(reinterpret_cast<const char*>(&original), sizeof(original));
            out.write(packed.data(), static_cast<std::streamsize>(size));
        } else {
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
        }
        out.flush();
        if (!out) {
            fail(error, "no se pudo escribir la partida (disco lleno?)");
            return false;
        }
    }
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        // Windows: si el destino esta abierto o protegido, se intenta borrar antes.
        std::error_code e2;
        std::filesystem::remove(file, e2);
        ec.clear();
        std::filesystem::rename(temp, file, ec);
    }
    if (ec) {
        fail(error, "no se pudo reemplazar la partida: " + ec.message());
        return false;
    }
    return true;
}

bool readSaveFile(const std::filesystem::path& file, std::string& text, std::string* error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        fail(error, "no existe la partida");
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string data = ss.str();
    if (data.size() >= kHeader && std::memcmp(data.data(), kMagic, 4) == 0) {
        std::uint32_t version = 0;
        std::uint64_t original = 0;
        std::memcpy(&version, data.data() + 4, sizeof(version));
        std::memcpy(&original, data.data() + 8, sizeof(original));
        if (version != kVersion || original > kMaxSize) {
            fail(error, "partida de otra version o danada");
            return false;
        }
        text.assign(static_cast<std::size_t>(original), '\0');
        const std::size_t size =
            ZSTD_decompress(text.data(), text.size(), data.data() + kHeader, data.size() - kHeader);
        if (ZSTD_isError(size) || size != original) {
            fail(error, "partida danada (no se pudo descomprimir)");
            text.clear();
            return false;
        }
        return true;
    }
    text = data;
    return true;
}

}  // namespace cramion::project
