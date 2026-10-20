#include "CramionCore/asset/PathReferences.h"

#include "CrData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace cramion::assets {

namespace {

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Los JSON del proyecto que pueden guardar rutas. Los .crdata, .cranim,
// .crfracture y .crterrain son binarios (o casi): no se tocan.
bool isJsonAsset(const std::string& extension) {
    static const std::unordered_set<std::string> kJson = {
        ".crscene", ".crprefab", ".crmat",   ".cranimator", ".crvfx",        ".crdialog", ".crfsm",
        ".crbt",    ".crmmdb",   ".crrt",    ".crgraph",    ".crshadergraph", ".crtileset", ".crui",
        ".crsprite", ".crpreset", ".crcontroller", ".crcell", ".json",
    };
    return kJson.contains(extension);
}

std::string pathKey(const std::filesystem::path& path) {
    std::string text = crdata::utf8(path.lexically_normal());
    std::replace(text.begin(), text.end(), '\\', '/');
#if defined(_WIN32)
    text = lower(std::move(text));
#endif
    return text;
}

bool readFile(const std::filesystem::path& file, std::string& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// Temporal al lado + rename: un corte a medias no deja el archivo roto.
bool writeFileAtomic(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::path temp = file;
    temp += ".tmp-rename";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) return false;
    }
    std::error_code error;
    std::filesystem::rename(temp, file, error);
    if (error) {
        std::filesystem::remove(temp, error);
        return false;
    }
    return true;
}

}  // namespace

bool rewritePath(std::string& value, std::span<const PathRename> renames) {
    if (value.empty()) return false;
    for (const PathRename& r : renames) {
        if (r.from.empty() || r.from == r.to) continue;
        for (const std::string_view prefix : {std::string_view{}, std::string_view{"Assets/"}}) {
            if (value.size() < prefix.size() + r.from.size() || value.compare(0, prefix.size(), prefix) != 0 ||
                value.compare(prefix.size(), r.from.size(), r.from) != 0) {
                continue;
            }
            const std::size_t end = prefix.size() + r.from.size();
            // Exacto: un archivo (o una carpeta con '/', que no se confunde con
            // un nombre suelto como "Audio"). Si es carpeta, tambien lo de dentro.
            const bool exact = end == value.size() && (!r.folder || r.from.find('/') != std::string::npos);
            const bool inside = r.folder && end < value.size() && value[end] == '/';
            if (!exact && !inside) continue;
            value = std::string(prefix) + r.to + value.substr(end);
            return true;
        }
    }
    return false;
}

int rewritePathReferences(std::string& text, std::span<const PathRename> renames) {
    // Rapido: si ninguna ruta aparece tal cual, no hay nada que hacer.
    const bool mentioned = std::any_of(renames.begin(), renames.end(), [&](const PathRename& r) {
        return !r.from.empty() && text.find(r.from) != std::string::npos;
    });
    if (!mentioned) return 0;
    int count = 0;
    std::string out;
    std::size_t copied = 0;  // lo de text[copied, ...) aun no esta en `out`
    std::string value;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '"') continue;
        // Un literal de texto: hasta la comilla que no va escapada.
        const std::size_t start = i;
        bool escaped = false;
        std::size_t j = i + 1;
        for (; j < text.size(); ++j) {
            const char c = text[j];
            if (c == '\\') {
                escaped = true;
                ++j;  // salta el caracter escapado
                continue;
            }
            if (c == '"') break;
        }
        if (j >= text.size()) break;  // sin cerrar: JSON roto, no se toca el resto
        i = j;
        if (escaped) {
            const nlohmann::json parsed =
                nlohmann::json::parse(text.begin() + static_cast<std::ptrdiff_t>(start),
                                      text.begin() + static_cast<std::ptrdiff_t>(j + 1), nullptr, false);
            if (!parsed.is_string()) continue;
            value = parsed.get<std::string>();
        } else {
            value.assign(text, start + 1, j - start - 1);
        }
        if (!rewritePath(value, renames)) continue;
        if (out.empty()) out.reserve(text.size() + 256);
        out.append(text, copied, start - copied);
        out += nlohmann::json(value).dump();  // con los escapes de nlohmann (UTF-8 tal cual)
        copied = j + 1;
        ++count;
    }
    if (count > 0) {
        out.append(text, copied, std::string::npos);
        text = std::move(out);
    }
    return count;
}

PathRewriteReport rewriteProjectReferences(const std::filesystem::path& assets_root,
                                           std::span<const PathRename> renames,
                                           std::span<const std::filesystem::path> skip) {
    PathRewriteReport report;
    if (assets_root.empty() || renames.empty()) return report;
    std::unordered_set<std::string> skipped;
    for (const std::filesystem::path& path : skip) skipped.insert(pathKey(path));

    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(
             assets_root, std::filesystem::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error)) continue;
        if (!isJsonAsset(lower(crdata::utf8(it->path().extension())))) continue;
        if (!skipped.empty() && skipped.contains(pathKey(it->path()))) continue;
        files.push_back(it->path());
    }
    report.scanned = files.size();

    // Leer y buscar en paralelo (miles de prefabs: el disco y el find mandan).
    std::mutex mutex;
    std::atomic<std::size_t> next{0};
    const auto work = [&]() {
        std::string text;
        for (std::size_t i = next.fetch_add(1); i < files.size(); i = next.fetch_add(1)) {
            const std::filesystem::path& file = files[i];
            if (!readFile(file, text)) continue;
            // Solo JSON de verdad (un .json de otra herramienta podria no serlo).
            const auto first = text.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
            if (first == std::string::npos || (text[first] != '{' && text[first] != '[')) continue;
            const int changed = rewritePathReferences(text, renames);
            if (changed == 0) continue;
            const bool ok = writeFileAtomic(file, text);
            const std::lock_guard lock(mutex);
            if (ok) {
                report.references += changed;
                report.updated.push_back(file);
            } else {
                report.failed.push_back(file);
            }
        }
    };
    const std::size_t workers =
        std::min<std::size_t>(std::max(1u, std::thread::hardware_concurrency()), std::min<std::size_t>(8, files.size()));
    std::vector<std::thread> threads;
    try {
        for (std::size_t t = 1; t < workers; ++t) threads.emplace_back(work);
    } catch (const std::exception&) {
        // Sin hilos: lo que falte lo hace este.
    }
    work();
    for (std::thread& t : threads) t.join();
    for (const std::filesystem::path& file : report.failed) {
        std::cerr << "[Assets] No se pudieron actualizar las rutas de " << crdata::utf8(file) << "\n";
    }
    std::sort(report.updated.begin(), report.updated.end());
    return report;
}

}  // namespace cramion::assets
