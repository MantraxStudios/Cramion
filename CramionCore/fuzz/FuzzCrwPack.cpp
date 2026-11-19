// Fuzzing de los paquetes .crwpack de la Cramion Store (crwpack/CrwPack.h):
// cabecera, manifiesto, indice, bloques zstd y CRC. Es lo que llega de
// Internet al instalar un paquete, asi que no puede caerse con nada.

#include "FuzzCommon.h"

#include "CramionCore/crwpack/CrwPack.h"

#include <nlohmann/json.hpp>

using namespace cramion;

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {
    const std::filesystem::path seeds = fuzz::seedsFolder(argc, argv);
    if (seeds.empty()) return 0;
    std::error_code e;
    std::filesystem::create_directories(seeds, e);
    const std::filesystem::path src = fuzz::scratch() / "semilla";
    fuzz::writeFile(src / "a.txt", "hola paquete");
    fuzz::writeFile(src / "sub/b.bin", std::string(300, '\x7f'));
    nlohmann::json manifest = {{"name", "com.prueba.fuzz"}, {"version", "1.0.0"}};
    crwpack::PackOptions options;
    options.level = 1;
    options.block_size = 64 * 1024;
    options.threads = 1;
    crwpack::pack(src, seeds / "basico.crwpack", manifest, options, {}, nullptr);
    crwpack::packFiles({}, seeds / "vacio.crwpack", manifest, options, {}, nullptr);
    std::filesystem::remove_all(src, e);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::filesystem::path file = fuzz::writeTemp("entrada.crwpack", data, size);
    std::string error;
    crwpack::Info info;
    if (!crwpack::readInfo(file, info, &error)) return 0;
    nlohmann::json manifest;
    crwpack::readManifest(file, manifest, &error);
    if (!crwpack::verify(file, 1, {}, &error)) return 0;
    // Solo si el paquete es valido: extraer (tambien comprueba las rutas).
    const std::filesystem::path out = fuzz::scratch() / "extraido";
    crwpack::unpack(file, out, 1, {}, &error);
    std::error_code e;
    std::filesystem::remove_all(out, e);
    return 0;
}
