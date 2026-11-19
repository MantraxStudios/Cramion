// Fuzzing de los .datapack (project/DataPack.h) y del contenedor .crpack que
// usan (project/Pack.h): indice, entradas zstd, manifiesto JSON y el montaje
// en una carpeta Assets propia (nada sale de la carpeta temporal del proceso).

#include "FuzzCommon.h"

#include "CramionCore/project/DataPack.h"
#include "CramionCore/project/Pack.h"

#include <nlohmann/json.hpp>

#include <vector>

using namespace cramion;

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {
    const std::filesystem::path seeds = fuzz::seedsFolder(argc, argv);
    if (seeds.empty()) return 0;
    std::error_code e;
    std::filesystem::create_directories(seeds, e);
    const std::filesystem::path src = fuzz::scratch() / "semilla";
    const nlohmann::json manifest = {{"format", "cramion-datapack"},
                                     {"name", "Semilla"},
                                     {"version", "1.0"},
                                     {"scenes", {"Escenas/Nivel.crscene"}},
                                     {"files", {"Escenas/Nivel.crscene", "Sonidos/a.wav"}}};
    fuzz::writeFile(src / "datapack.json", manifest.dump());
    fuzz::writeFile(src / "Nivel.crscene", "{\"entities\":[]}");
    fuzz::writeFile(src / "a.wav", "RIFF....WAVE");
    const std::vector<project::PackInput> inputs = {{src / "datapack.json", "datapack.json"},
                                                    {src / "Nivel.crscene", "Assets/Escenas/Nivel.crscene"},
                                                    {src / "a.wav", "Assets/Sonidos/a.wav"}};
    project::writePack(seeds / "nivel.datapack", inputs, 1, {}, nullptr);
    project::writePack(seeds / "solo_manifiesto.datapack", {inputs[0]}, 1, {}, nullptr);
    std::filesystem::remove_all(src, e);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::filesystem::path file = fuzz::writeTemp("entrada.datapack", data, size);
    std::string error;
    std::vector<project::PackEntry> entries;
    if (!project::readPackIndex(file, entries, &error)) return 0;
    project::packId(file);
    std::string text;
    if (!entries.empty()) project::readPackFile(file, entries.front().path, text, &error);
    project::DataPackManifest manifest;
    if (!project::readDataPackManifest(file, manifest, &error)) return 0;
    // Montar en un Assets propio y desmontar: el montaje no debe escribir
    // fuera de esa carpeta (las rutas del manifiesto vienen de fuera).
    const std::filesystem::path assets = fuzz::scratch() / "Assets";
    std::error_code e;
    std::filesystem::create_directories(assets, e);
    project::DataPackMount mount;
    if (project::mountDataPack(file, assets, mount, &error)) project::unmountDataPack(mount, assets);
    std::filesystem::remove_all(assets, e);
    return 0;
}
