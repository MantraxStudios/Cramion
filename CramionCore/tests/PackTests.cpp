// Pruebas de los paquetes .crpack (escribir, leer el indice, descomprimir,
// detectar danos), en consola.
#include "CramionCore/asset/AssetDatabase.h"
#include "CramionCore/project/DataPack.h"
#include "CramionCore/project/Pack.h"
#include "asset/CrData.h"  // interna de CramionAssets (src/asset)

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <iterator>
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
    const std::filesystem::path accented = src / "Assets" / std::filesystem::path(u8"canción.txt");
    std::ofstream(accented, std::ios::binary) << "print('hola')";

    std::vector<project::PackInput> inputs = {
        {src / "Assets" / "escena.crscene", "Assets/escena.crscene"},
        {src / "Assets" / "Modelos" / "ruido.bin", "Assets/Modelos/ruido.bin"},
        {src / "Assets" / "vacio.txt", "Assets/vacio.txt"},
        {accented, reinterpret_cast<const char*>(u8"Assets/canción.txt")},
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
    check(readAll(out / "Assets" / std::filesystem::path(u8"canción.txt")) == "print('hola')", "ruta con acentos");

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

    // --- DataPacks: una escena con todo lo que usa ---
    {
        const std::filesystem::path game = root / "dp_game" / "Assets";
        const auto write = [](const std::filesystem::path& f, const std::string& text) {
            std::filesystem::create_directories(f.parent_path());
            std::ofstream(f, std::ios::binary) << text;
        };
        write(game / "Escenas" / "Nivel1.crscene",
              "{\"entities\":[{\"CppScript\":{\"script\":\"Scripts/Jugador.cpp\"}},{\"AudioSource\":{\"clip\":\"Sonidos/viento.wav\"}}]}");
        write(game / "Scripts" / "Jugador.cpp", "// jugador\nAudio::playOneShot(\"Sonidos/disparo.wav\");\nScene::load(\"Nivel2\");\n");
        write(game / "Sonidos" / "viento.wav", "RIFFviento");
        write(game / "Sonidos" / "disparo.wav", "RIFFdisparo");
        write(game / "Escenas" / "Nivel2.crscene", "{\"entities\":[]}");
        write(game / "NoUsado" / "grande.png", std::string(5000, 'x'));

        assets::AssetDatabase database;
        database.open(game);
        const project::DataPackCollection deps =
            project::collectDependencies({game / "Escenas" / "Nivel1.crscene"}, game, database);
        const auto has = [&](const char* name) {
            for (const auto& f : deps.files) {
                if (f.filename() == name) return true;
            }
            return false;
        };
        check(deps.files.size() == 5 && has("Nivel1.crscene") && has("Jugador.cpp") && has("viento.wav") &&
                  has("disparo.wav") && has("Nivel2.crscene") && !has("grande.png"),
              "DataPack: dependencias de la escena (script, sonidos, escena por nombre) y nada mas");

        const std::filesystem::path pack_file = root / "Nivel1.datapack";
        project::DataPackManifest written;
        check(project::writeDataPack(pack_file, "Nivel1", {game / "Escenas" / "Nivel1.crscene"}, deps, game, "test", 3, {},
                                     &error, &written),
              "DataPack: se escribe");
        project::DataPackManifest manifest;
        check(project::readDataPackManifest(pack_file, manifest, &error) && manifest.name == "Nivel1" &&
                  manifest.scenes.size() == 1 && manifest.scenes[0] == "Escenas/Nivel1.crscene" &&
                  manifest.files.size() == 5,
              "DataPack: manifiesto sin extraer");

        // Montar en otro juego que ya tiene un archivo con el mismo nombre.
        const std::filesystem::path other = root / "dp_other" / "Assets";
        write(other / "Sonidos" / "viento.wav", "DEL JUEGO");
        project::DataPackMount mount;
        const std::filesystem::path journal = root / "dp_other" / "journal.txt";
        check(project::mountDataPack(pack_file, other, mount, &error, journal) && mount.written.size() == 4 &&
                  mount.kept.size() == 1,
              "DataPack: se monta sin sobrescribir lo que existe");
        std::ifstream kept(other / "Sonidos" / "viento.wav", std::ios::binary);
        std::string kept_text((std::istreambuf_iterator<char>(kept)), std::istreambuf_iterator<char>());
        kept.close();
        check(kept_text == "DEL JUEGO" && std::filesystem::exists(other / "Scripts" / "Jugador.cpp"),
              "DataPack: el archivo del juego queda intacto");
        project::unmountDataPack(mount, other, journal);
        check(!std::filesystem::exists(other / "Scripts") && std::filesystem::exists(other / "Sonidos" / "viento.wav"),
              "DataPack: desmontar borra lo suyo y sus carpetas vacias");
        // Diario: un montaje que no se desmonto se limpia al abrir.
        project::DataPackMount again;
        project::mountDataPack(pack_file, other, again, &error, journal);
        project::cleanupDataPackJournal(journal, other);
        check(!std::filesystem::exists(other / "Escenas" / "Nivel2.crscene") && !std::filesystem::exists(journal),
              "DataPack: el diario limpia montajes olvidados");

        // Objetos: un prefab (con su script) en vez de una escena.
        write(game / "Prefabs" / "Coche.crprefab", "{\"entities\":[{\"CppScript\":{\"script\":\"Scripts/Jugador.cpp\"}}]}");
        database.refresh();
        const project::DataPackCollection object_deps =
            project::collectDependencies({game / "Prefabs" / "Coche.crprefab"}, game, database);
        const std::filesystem::path object_pack = root / "Coche.datapack";
        project::DataPackManifest objects;
        check(object_deps.files.size() == 4 &&
                  project::writeDataPack(object_pack, "Coche", {game / "Prefabs" / "Coche.crprefab"}, object_deps, game,
                                         "test", 3, {}, &error) &&
                  project::readDataPackManifest(object_pack, objects, &error) && objects.scenes.empty() &&
                  objects.objects.size() == 1 && objects.objects[0] == "Prefabs/Coche.crprefab",
              "DataPack: objetos (prefab) con sus dependencias en \"objects\"");
    }

    // --- Tamanos enormes en el indice de un .crpack (fuzzing): readPackFile
    // y extractPack fallan sin reservar memoria de mas ---
    {
        const std::filesystem::path dir = root / "tam";
        std::filesystem::create_directories(dir);
        std::ofstream(dir / "a.txt", std::ios::binary) << "contenido corto";
        const std::filesystem::path big = dir / "Grande.crpack";
        check(project::writePack(big, {{dir / "a.txt", "a.txt"}}, 3, {}, &error), "paquete de un archivo");
        // El indice va al final: su ultimo campo es el tamano original.
        {
            std::fstream f(big, std::ios::binary | std::ios::in | std::ios::out);
            f.seekp(-8, std::ios::end);
            const std::uint64_t huge = 1ull << 40;
            f.write(reinterpret_cast<const char*>(&huge), sizeof(huge));
        }
        std::string data;
        check(!project::readPackFile(big, "a.txt", data, &error), "tamano enorme: readPackFile falla (sin reservar 1 TB)");
        check(!project::extractPack(big, dir / "out", {}, &error), "tamano enorme: extractPack falla");

        // Ruta del indice con UTF-8 no valido: antes std::filesystem::path lanzaba.
        const std::filesystem::path bad = dir / "Utf8.crpack";
        check(project::writePack(bad, {{dir / "a.txt", "a.txt"}}, 3, {}, &error), "paquete para romper la ruta");
        {
            std::fstream f(bad, std::ios::binary | std::ios::in | std::ios::out);
            std::uint64_t index = 0;
            f.seekg(12);
            f.read(reinterpret_cast<char*>(&index), sizeof(index));
            f.seekp(static_cast<std::streamoff>(index + 2));  // tras el u16 del largo
            const char invalid[2] = {static_cast<char>(0xC3), static_cast<char>(0x28)};
            f.write(invalid, 2);
        }
        std::vector<project::PackEntry> bad_entries;
        check(!project::readPackIndex(bad, bad_entries, &error), "ruta con UTF-8 no valido: paquete danado (sin excepcion)");

        // Manifiestos de DataPack con otros tipos: no son DataPacks (antes type_error).
        for (const char* text : {"[1, 2]", "{\"format\": 5}", "{\"format\": \"cramion-datapack\", \"files\": 5}"}) {
            std::ofstream(dir / "datapack.json", std::ios::binary) << text;
            const std::filesystem::path odd = dir / "Raro.datapack";
            project::DataPackManifest m;
            check(project::writePack(odd, {{dir / "datapack.json", "datapack.json"}}, 3, {}, &error) &&
                      !project::readDataPackManifest(odd, m, &error),
                  "manifiesto con otro tipo: no es un DataPack (sin excepcion)");
        }
    }

    // --- .crdata con tamanos enormes (fuzzing): se rechazan sin reservar ---
    {
        const std::filesystem::path dir = root / "crdata";
        std::filesystem::create_directories(dir);
        assets::crdata::Header header;
        header.type = assets::AssetType::Environment;
        header.name = "Cielo";
        header.source = "cielo.hdr";
        header.settings_json = "{}";
        // Cabecera: firma, version, tipo, UUID y tres cadenas (u64 + texto).
        const std::streamoff header_size = 8 + 4 + 4 + 16 + (8 + 5) + (8 + 9) + (8 + 2);
        const auto patch = [](const std::filesystem::path& f, std::streamoff at, const void* value, std::size_t n) {
            std::fstream s(f, std::ios::binary | std::ios::in | std::ios::out);
            s.seekp(at);
            s.write(static_cast<const char*>(value), static_cast<std::streamsize>(n));
        };
        const std::filesystem::path sky = dir / "cielo.crdata";
        assets::crdata::writeEnvironment(sky, header, ".hdr", std::vector<std::uint8_t>(10, 7));
        assets::crdata::Header h;
        std::string extension;
        std::vector<std::uint8_t> bytes;
        check(assets::crdata::readEnvironment(sky, h, extension, bytes) && bytes.size() == 10 && extension == ".hdr",
              ".crdata: cielo de ida y vuelta");
        const std::uint64_t huge = 1ull << 50;
        patch(sky, header_size, &huge, 8);                 // payload_bytes
        patch(sky, header_size + 8 + 8 + 4, &huge, 8);     // bytes del .hdr
        check(!assets::crdata::readEnvironment(sky, h, extension, bytes), ".crdata: cielo con tamano enorme se rechaza");

        header.type = assets::AssetType::Model;
        assets::crdata::ModelContent model;
        model.nodes.resize(1);
        model.nodes[0].name = "Raiz";
        const std::filesystem::path mesh = dir / "modelo.crdata";
        assets::crdata::writeModel(mesh, header, model);
        assets::crdata::ModelContent read;
        check(assets::crdata::readModel(mesh, h, read) && read.nodes.size() == 1 && read.nodes[0].name == "Raiz",
              ".crdata: modelo de ida y vuelta");
        const std::uint32_t many = (1u << 24) - 1;
        patch(mesh, header_size + 8, &many, 4);            // numero de nodos
        check(!assets::crdata::readModel(mesh, h, read), ".crdata: 16 M de nodos en un archivo pequeno se rechaza");
    }

    // --- DataPack con rutas absolutas en el manifiesto (fuzzing): se saltan
    // esas entradas y el resto se monta; nada se escribe fuera de Assets ---
    {
        const std::filesystem::path dir = root / "dp_abs";
        std::filesystem::create_directories(dir);
        const std::filesystem::path outside = dir / "fuera.txt";
        std::string outside_text;
        for (char8_t c : outside.generic_u8string()) outside_text += static_cast<char>(c);
        const nlohmann::json manifest = {{"format", "cramion-datapack"},
                                         {"name", "Abs"},
                                         {"files", {"bueno.txt", outside_text, "/raiz.txt", "C:relativa.txt"}}};
        std::ofstream(dir / "datapack.json", std::ios::binary) << manifest.dump();
        std::ofstream(dir / "bueno.txt", std::ios::binary) << "bien";
        const std::vector<project::PackInput> dp_inputs = {{dir / "datapack.json", "datapack.json"},
                                                           {dir / "bueno.txt", "Assets/bueno.txt"}};
        const std::filesystem::path dp_file = dir / "Abs.datapack";
        const std::filesystem::path assets = dir / "Assets";
        std::filesystem::create_directories(assets);
        project::DataPackMount mount;
        check(project::writePack(dp_file, dp_inputs, 3, {}, &error) &&
                  project::mountDataPack(dp_file, assets, mount, &error) && mount.written.size() == 1 &&
                  readAll(assets / "bueno.txt") == "bien",
              "DataPack: rutas absolutas del manifiesto se saltan (el resto se monta)");
        check(!std::filesystem::exists(outside), "DataPack: nada fuera de Assets");
        project::unmountDataPack(mount, assets);
    }

    std::filesystem::remove_all(root);
    std::printf("\n%d fallos\n", failures);
    return failures == 0 ? 0 : 1;
}
