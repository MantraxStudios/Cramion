// crwpack: el compresor/descompresor de paquetes .crwpack de la Cramion Store.
//
//   crwpack pack <carpeta> <salida.crwpack> --name com.autor.rocas --version 1.0.0
//                [--display "Rocas"] [--description "..."] [--author yo] [--engine ">=2.4.0"]
//                [--dep com.otro.base=1.2.0]... [--changelog "..."]
//                [--level fast|normal|max|1..22] [--block-mb 4] [-j hilos]
//   crwpack unpack <paquete.crwpack> <carpeta> [-j hilos]
//   crwpack list <paquete.crwpack>
//   crwpack info <paquete.crwpack>          (manifiesto en JSON)
//   crwpack verify <paquete.crwpack> [-j hilos]

#include "CramionCore/crwpack/CrwPack.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace cramion;

namespace {

fs::path pathArg(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

std::string bytesText(std::uint64_t bytes) {
    char text[32];
    if (bytes >= (1ull << 30)) std::snprintf(text, sizeof(text), "%.2f GB", bytes / double(1ull << 30));
    else if (bytes >= (1ull << 20)) std::snprintf(text, sizeof(text), "%.1f MB", bytes / double(1ull << 20));
    else std::snprintf(text, sizeof(text), "%.1f KB", bytes / 1024.0);
    return text;
}

crwpack::Progress bar(const char* label) {
    auto last = std::make_shared<int>(-1);
    return [label, last](std::uint64_t done, std::uint64_t total) {
        const int pct = total ? static_cast<int>(done * 100 / total) : 100;
        if (pct != *last) {
            *last = pct;
            std::fprintf(stderr, "\r%s %3d%%", label, pct);
            if (pct == 100) std::fprintf(stderr, "\n");
        }
        return true;
    };
}

int usage() {
    std::cerr <<
        "crwpack: paquetes de assets de Cramion (.crwpack, multihilo)\n\n"
        "  crwpack pack <carpeta> <salida.crwpack> --name com.autor.paquete --version 1.0.0\n"
        "               [--display NOMBRE] [--description TEXTO] [--author AUTOR] [--engine \">=2.4.0\"]\n"
        "               [--dep nombre=version]... [--changelog TEXTO]\n"
        "               [--level fast|normal|max|1..22] [--block-mb 4] [-j HILOS]\n"
        "  crwpack unpack <paquete.crwpack> <carpeta> [-j HILOS]\n"
        "  crwpack list   <paquete.crwpack>\n"
        "  crwpack info   <paquete.crwpack>\n"
        "  crwpack verify <paquete.crwpack> [-j HILOS]\n";
    return 2;
}

int run(std::vector<std::string> args) {
    if (args.size() < 2) return usage();
    const std::string cmd = args[0];
    int threads = 0;
    std::vector<std::string> positional;
    nlohmann::json manifest = nlohmann::json::object();
    crwpack::PackOptions options;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= args.size()) throw std::runtime_error("falta el valor de " + a);
            return args[++i];
        };
        if (a == "-j" || a == "--threads") threads = std::stoi(value());
        else if (a == "--name") manifest["name"] = value();
        else if (a == "--version") manifest["version"] = value();
        else if (a == "--display") manifest["displayName"] = value();
        else if (a == "--description") manifest["description"] = value();
        else if (a == "--author") manifest["author"] = value();
        else if (a == "--engine") manifest["engine"] = value();
        else if (a == "--changelog") manifest["changelog"] = value();
        else if (a == "--dep") {
            const std::string d = value();
            const auto eq = d.find('=');
            if (eq == std::string::npos) throw std::runtime_error("--dep espera nombre=version");
            manifest["dependencies"][d.substr(0, eq)] = d.substr(eq + 1);
        } else if (a == "--level") {
            const std::string l = value();
            options.level = l == "fast" ? 3 : l == "normal" ? 9 : l == "max" ? 19 : std::stoi(l);
        } else if (a == "--block-mb") options.block_size = static_cast<std::uint32_t>(std::stoi(value())) << 20;
        else if (!a.empty() && a[0] == '-') throw std::runtime_error("opcion desconocida: " + a);
        else positional.push_back(a);
    }
    options.threads = threads;
    std::string error;
    const auto t0 = std::chrono::steady_clock::now();
    auto seconds = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };

    if (cmd == "pack") {
        if (positional.size() != 2) return usage();
        const std::string name = manifest.value("name", "");
        const std::string version = manifest.value("version", "");
        if (!crwpack::validName(name)) throw std::runtime_error("--name no valido (minusculas, numeros, '.', '_', '-'; ej. com.autor.rocas)");
        if (!crwpack::validVersion(version)) throw std::runtime_error("--version no valida (semver: 1.0.0)");
        if (!manifest.contains("displayName")) manifest["displayName"] = name;
        manifest["createdWith"] = "crwpack";
        if (!manifest.contains("dependencies")) manifest["dependencies"] = nlohmann::json::object();
        if (!crwpack::pack(pathArg(positional[0]), pathArg(positional[1]), manifest, options, bar("Comprimiendo"), &error))
            throw std::runtime_error(error);
        crwpack::Info info;
        crwpack::readInfo(pathArg(positional[1]), info, nullptr);
        std::printf("%s: %zu archivos, %s -> %s (%.0f%%) en %.2f s\n", positional[1].c_str(), info.files.size(),
                    bytesText(info.raw_size).c_str(), bytesText(info.packed_size).c_str(),
                    info.raw_size ? 100.0 * double(info.packed_size) / double(info.raw_size) : 100.0, seconds());
        return 0;
    }
    if (cmd == "unpack") {
        if (positional.size() != 2) return usage();
        if (!crwpack::unpack(pathArg(positional[0]), pathArg(positional[1]), threads, bar("Extrayendo"), &error))
            throw std::runtime_error(error);
        std::printf("Extraido en %s (%.2f s)\n", positional[1].c_str(), seconds());
        return 0;
    }
    if (positional.size() != 1) return usage();
    if (cmd == "verify") {
        if (!crwpack::verify(pathArg(positional[0]), threads, bar("Verificando"), &error)) throw std::runtime_error(error);
        std::printf("OK (%.2f s)\n", seconds());
        return 0;
    }
    crwpack::Info info;
    if (!crwpack::readInfo(pathArg(positional[0]), info, &error)) throw std::runtime_error(error);
    if (cmd == "info") {
        std::printf("%s\n", info.manifest.dump(2).c_str());
        return 0;
    }
    if (cmd == "list") {
        for (const crwpack::FileEntry& f : info.files) std::printf("%12s  %s\n", bytesText(f.size).c_str(), f.path.c_str());
        std::printf("%zu archivos, %s en %zu bloques de %s; %s comprimido\n", info.files.size(), bytesText(info.raw_size).c_str(),
                    info.blocks.size(), bytesText(info.block_size).c_str(), bytesText(info.packed_size).c_str());
        return 0;
    }
    return usage();
}

}  // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, s.data(), n, nullptr, nullptr);
        args.push_back(std::move(s));
    }
#else
int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
#endif
    try {
        return run(args);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "crwpack: %s\n", e.what());
        return 1;
    }
}
