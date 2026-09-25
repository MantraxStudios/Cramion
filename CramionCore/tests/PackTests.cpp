// Pruebas de los paquetes .crpack (escribir, leer el indice, descomprimir,
// detectar danos), en consola.
#include "CramionCore/project/Pack.h"

#include <cstdio>
#include <fstream>
#include <random>
#include <string>

using namespace cramion;

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "OK   " : "FALLO", what);
    if (!ok) ++failures;
}

std::string readAll(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int main() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_pack_test";
    std::filesystem::remove_all(root);
    const std::filesystem::path src = root / "src";
    std::filesystem::create_directories(src / "Assets" / "Modelos");

    // Texto repetitivo (comprime mucho), binario aleatorio de 3 MB (varios
    // bloques), un archivo vacio y una ruta con acentos.
    std::string text;
    for (int i = 0; i < 20000; ++i) text += "entidad " + std::to_string(i % 17) + " posicion 1 2 3\n";
    std::string noise(3u << 20, '\0');
    std::mt19937 rng(7);
    for (char& c : noise) c = static_cast<char>(rng());
    std::ofstream(src / "Assets" / "escena.crscene", std::ios::binary) << text;
    std::ofstream(src / "Assets" / "Modelos" / "ruido.bin", std::ios::binary) << noise;
    std::ofstream(src / "Assets" / "vacio.txt", std::ios::binary);
    const std::filesystem::path accented = src / "Assets" / std::filesystem::path(u8"canción.lua");
    std::ofstream(accented, std::ios::binary) << "print('hola')";

    std::vector<project::PackInput> inputs = {
        {src / "Assets" / "escena.crscene", "Assets/escena.crscene"},
        {src / "Assets" / "Modelos" / "ruido.bin", "Assets/Modelos/ruido.bin"},
        {src / "Assets" / "vacio.txt", "Assets/vacio.txt"},
        {accented, reinterpret_cast<const char*>(u8"Assets/canción.lua")},
    };
    const std::filesystem::path pack = root / "Juego.crpack";
    std::uint64_t last_progress = 0;
    std::string error;
    const bool written = project::writePack(pack, inputs, 3, [&](std::uint64_t done, const std::string&) {
        last_progress = done;
        return true;
    }, &error);
    check(written, "escribe el paquete");
    if (!written) std::printf("    %s\n", error.c_str());
    const std::uint64_t original = text.size() + noise.size() + 13;
    std::printf("  (original %llu bytes, paquete %llu bytes)\n", static_cast<unsigned long long>(original),
                static_cast<unsigned long long>(std::filesystem::file_size(pack)));
    check(std::filesystem::file_size(pack) < original, "comprime (el texto)");
    check(last_progress == original, "el progreso llega al total");

    std::vector<project::PackEntry> entries;
    check(project::readPackIndex(pack, entries, &error) && entries.size() == 4, "lee el indice");

    const std::filesystem::path out = root / "out";
    check(project::extractPack(pack, out, {}, &error), "descomprime");
    check(readAll(out / "Assets" / "escena.crscene") == text, "texto identico");
    check(readAll(out / "Assets" / "Modelos" / "ruido.bin") == noise, "binario identico (varios bloques)");
    check(std::filesystem::exists(out / "Assets" / "vacio.txt") && std::filesystem::file_size(out / "Assets" / "vacio.txt") == 0,
          "archivo vacio");
    check(readAll(out / "Assets" / std::filesystem::path(u8"canción.lua")) == "print('hola')", "ruta con acentos");

    const std::string id = project::packId(pack);
    check(!id.empty() && id == project::packId(pack), "identificador estable");

    // Cancelar a mitad: no deja el paquete a medias.
    const std::filesystem::path cancelled = root / "Cancelado.crpack";
    check(!project::writePack(cancelled, inputs, 3, [](std::uint64_t done, const std::string&) { return done < 1000; }, &error) &&
              !std::filesystem::exists(cancelled),
          "cancelar no deja archivo");

    // Rutas peligrosas.
    std::vector<project::PackInput> evil = {{src / "Assets" / "vacio.txt", "../fuera.txt"}};
    check(!project::writePack(root / "Malo.crpack", evil, 3, {}, &error), "rechaza rutas con ..");

    // Un byte cambiado dentro de los datos: el checksum lo detecta.
    {
        std::fstream f(pack, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(static_cast<std::streamoff>(entries[1].offset + entries[1].compressed / 2));
        f.put('\x5A');
        f.seekp(static_cast<std::streamoff>(entries[1].offset + entries[1].compressed / 2 + 1));
        f.put('\xA5');
    }
    check(!project::extractPack(pack, root / "out2", {}, &error), "detecta un paquete danado");
    std::printf("    (%s)\n", error.c_str());
    check(!project::readPackIndex(src / "Assets" / "escena.crscene", entries, &error), "rechaza lo que no es .crpack");

    std::filesystem::remove_all(root);
    std::printf("\n%d fallos\n", failures);
    return failures == 0 ? 0 : 1;
}
