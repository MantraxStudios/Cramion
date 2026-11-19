#ifndef CRAMION_FUZZ_COMMON_H
#define CRAMION_FUZZ_COMMON_H

// Comun de los objetivos de fuzzing (libFuzzer, CRAMION_FUZZ=ON).
//
// Los lectores de formatos del motor leen de un archivo: cada entrada se
// escribe en un temporal propio del proceso (cramion_fuzz_<pid>/) y se lee de
// ahi. Las semillas se escriben con CRAMION_FUZZ_SEEDS=<carpeta> (o con
// --seeds=<carpeta> en la linea de ordenes) antes de empezar: asi el corpus
// no vive en el repositorio y sigue al formato si cambia.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#define CRAMION_FUZZ_PID _getpid()
#else
#include <unistd.h>
#define CRAMION_FUZZ_PID getpid()
#endif

namespace cramion::fuzz {

// Carpeta temporal de este proceso (se crea una vez).
inline const std::filesystem::path& scratch() {
    static const std::filesystem::path dir = [] {
        std::error_code e;
        std::filesystem::path d =
            std::filesystem::temp_directory_path(e) / ("cramion_fuzz_" + std::to_string(CRAMION_FUZZ_PID));
        std::filesystem::create_directories(d, e);
        return d;
    }();
    // Al salir sin fallo se borra (con un fallo se queda para mirarla).
    static const bool cleanup = [] {
        std::atexit([] {
            std::error_code e;
            std::filesystem::remove_all(scratch(), e);
        });
        return true;
    }();
    (void)cleanup;
    return dir;
}

inline std::filesystem::path writeTemp(const std::string& name, const std::uint8_t* data, std::size_t size) {
    const std::filesystem::path file = scratch() / name;
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (size > 0) out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return file;
}

inline void writeFile(const std::filesystem::path& file, const std::string& bytes) {
    std::error_code e;
    std::filesystem::create_directories(file.parent_path(), e);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

inline std::string readFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Carpeta de semillas pedida (vacia si no se pidio).
inline std::filesystem::path seedsFolder(int* argc, char*** argv) {
    if (argc != nullptr && argv != nullptr) {
        for (int i = 1; i < *argc; ++i) {
            const char* a = (*argv)[i];
            if (std::strncmp(a, "--seeds=", 8) == 0) return std::filesystem::path(a + 8);
        }
    }
    if (const char* env = std::getenv("CRAMION_FUZZ_SEEDS")) return std::filesystem::path(env);
    return {};
}

}  // namespace cramion::fuzz

#endif  // CRAMION_FUZZ_COMMON_H
