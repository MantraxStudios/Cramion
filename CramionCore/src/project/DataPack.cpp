#include "CramionCore/project/DataPack.h"

#include "CramionCore/Uuid.h"
#include "CramionCore/asset/AssetDatabase.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <deque>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>
#include <unordered_map>

namespace cramion::project {
namespace {

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path fromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Formatos de texto del motor en los que se buscan referencias.
bool isTextAsset(const std::string& extension) {
    static const std::set<std::string> kText = {".crscene", ".crprefab", ".crmat", ".crrt",  ".crpaint", ".crshader",
                                                ".lua",     ".json",     ".txt",   ".ini",   ".crcontroller",
                                                ".cranim",  ".crterrain", ".crui", ".csv",  ".crfsm"};
    return kText.count(extension) != 0;
}

bool isHex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

// UUIDs (8-4-4-4-12) que aparecen en un texto.
std::vector<std::string> findUuids(const std::string& text) {
    std::vector<std::string> out;
    const std::size_t n = text.size();
    for (std::size_t i = 0; i + 36 <= n; ++i) {
        if (!isHex(text[i]) || (i > 0 && (isHex(text[i - 1]) || text[i - 1] == '-'))) continue;
        bool ok = true;
        for (std::size_t k = 0; k < 36 && ok; ++k) {
            const char c = text[i + k];
            ok = (k == 8 || k == 13 || k == 18 || k == 23) ? c == '-' : isHex(c);
        }
        if (ok && (i + 36 == n || (!isHex(text[i + 36]) && text[i + 36] != '-'))) {
            out.push_back(text.substr(i, 36));
            i += 35;
        }
    }
    return out;
}

// Textos entre comillas (dobles o simples) que podrian ser rutas.
std::vector<std::string> findQuoted(const std::string& text) {
    std::vector<std::string> out;
    const std::size_t n = text.size();
    for (std::size_t i = 0; i < n; ++i) {
        const char quote = text[i];
        if (quote != '"' && quote != '\'') continue;
        std::size_t j = i + 1;
        std::string value;
        while (j < n && text[j] != quote && text[j] != '\n' && value.size() < 260) {
            if (text[j] == '\\' && j + 1 < n) {
                value += text[j + 1] == '\\' ? '/' : text[j + 1];
                j += 2;
                continue;
            }
            value += text[j++];
        }
        if (j < n && text[j] == quote && value.size() >= 2) out.push_back(value);
        i = j;
    }
    return out;
}

bool isInside(const std::filesystem::path& child, const std::filesystem::path& root) {
    const std::string c = lower(utf8(child));
    std::string r = lower(utf8(root));
    if (!r.empty() && r.back() != '/') r += '/';
    return c.rfind(r, 0) == 0;
}

std::string isoNow() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

}  // namespace

DataPackCollection collectDependencies(const std::vector<std::filesystem::path>& scenes,
                                       const std::filesystem::path& assets_root,
                                       const assets::AssetDatabase& database) {
    DataPackCollection result;
    std::error_code e;
    const std::filesystem::path root = std::filesystem::weakly_canonical(assets_root, e);

    // Nombres sin extension de escenas y prefabs (Scene.load("Nivel2"),
    // Scene.instantiate("Enemigo")): se buscan por nombre en todo Assets.
    std::unordered_map<std::string, std::vector<std::filesystem::path>> by_stem;
    for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, e);
         !e && it != std::filesystem::recursive_directory_iterator(); it.increment(e)) {
        std::error_code fe;
        if (!it->is_regular_file(fe)) continue;
        const std::string extension = lower(it->path().extension().string());
        if (extension == ".crscene" || extension == ".crprefab") by_stem[lower(utf8(it->path().stem()))].push_back(it->path());
    }

    std::set<std::string> seen;  // rutas normalizadas (minusculas)
    std::deque<std::filesystem::path> queue;
    const auto add = [&](const std::filesystem::path& file) {
        std::error_code ec;
        std::filesystem::path canonical = std::filesystem::weakly_canonical(file, ec);
        if (ec || !std::filesystem::is_regular_file(canonical, ec) || !isInside(canonical, root)) return false;
        if (!seen.insert(lower(utf8(canonical))).second) return true;
        result.files.push_back(canonical);
        result.bytes += std::filesystem::file_size(canonical, ec);
        queue.push_back(canonical);
        return true;
    };
    for (const std::filesystem::path& scene : scenes) {
        if (!add(scene)) result.missing.push_back(utf8(scene));
    }

    while (!queue.empty()) {
        const std::filesystem::path file = queue.front();
        queue.pop_front();
        if (!isTextAsset(lower(file.extension().string()))) continue;  // binarios (modelos, imagenes, sonidos): hojas
        std::ifstream in(file, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        // Referencias por UUID (modelos, materiales, prefabs, cielos...).
        for (const std::string& id : findUuids(text)) {
            const Uuid uuid = Uuid::parse(id);
            if (!uuid.valid()) continue;
            const auto info = database.find(uuid);
            if (!info) continue;  // UUID de una entidad, no de un asset
            if (info->path.empty()) continue;  // integrado (primitivas)
            if (!add(info->path)) result.missing.push_back(utf8(file.filename()) + ": " + utf8(info->path));
        }
        // Referencias por ruta (scripts, sonidos, texturas, terrenos...).
        for (std::string value : findQuoted(text)) {
            std::replace(value.begin(), value.end(), '\\', '/');
            while (value.rfind("./", 0) == 0) value.erase(0, 2);
            if (value.rfind("Assets/", 0) == 0) value.erase(0, 7);
            if (value.empty() || value.find("..") != std::string::npos || value.find(':') != std::string::npos) continue;
            const std::filesystem::path candidate = root / fromUtf8(value);
            std::error_code fe;
            if (std::filesystem::is_regular_file(candidate, fe)) {
                add(candidate);
                continue;
            }
            // Sin extension: escena o prefab por ruta o por nombre.
            if (!fromUtf8(value).has_extension() && value.size() < 128) {
                bool found = add(std::filesystem::path(candidate).concat(".crscene")) ||
                             add(std::filesystem::path(candidate).concat(".crprefab"));
                if (!found) {
                    const auto it = by_stem.find(lower(utf8(fromUtf8(value).filename())));
                    if (it != by_stem.end()) {
                        for (const std::filesystem::path& p : it->second) add(p);
                    }
                }
            }
        }
    }
    return result;
}

bool writeDataPack(const std::filesystem::path& file, const std::string& name,
                   const std::vector<std::filesystem::path>& scenes, const DataPackCollection& collection,
                   const std::filesystem::path& assets_root, const std::string& engine_version, int level,
                   const PackProgress& progress, std::string* error, DataPackManifest* written) {
    std::error_code e;
    const std::filesystem::path root = std::filesystem::weakly_canonical(assets_root, e);
    DataPackManifest manifest;
    manifest.name = name;
    manifest.engine = engine_version;
    manifest.created = isoNow();
    manifest.bytes = collection.bytes;
    std::vector<PackInput> inputs;
    for (const std::filesystem::path& f : collection.files) {
        const std::string relative = utf8(std::filesystem::relative(f, root, e));
        manifest.files.push_back(relative);
        inputs.push_back(PackInput{f, "Assets/" + relative});
    }
    for (const std::filesystem::path& s : scenes) {
        const std::string relative = utf8(std::filesystem::relative(std::filesystem::weakly_canonical(s, e), root, e));
        (lower(s.extension().string()) == ".crprefab" ? manifest.objects : manifest.scenes).push_back(relative);
    }

    nlohmann::json json;
    json["format"] = "cramion-datapack";
    json["format_version"] = 1;
    json["name"] = manifest.name;
    json["version"] = manifest.version;
    json["engine"] = manifest.engine;
    json["created"] = manifest.created;
    json["scenes"] = manifest.scenes;
    json["objects"] = manifest.objects;
    json["files"] = manifest.files;
    json["bytes"] = manifest.bytes;
    const std::filesystem::path manifest_file =
        std::filesystem::temp_directory_path(e) / fromUtf8("cramion_datapack_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    {
        std::ofstream out(manifest_file, std::ios::binary | std::ios::trunc);
        out << json.dump(2);
        if (!out) {
            if (error) *error = "No se pudo escribir el manifiesto";
            return false;
        }
    }
    inputs.insert(inputs.begin(), PackInput{manifest_file, kDataPackManifest});
    std::filesystem::create_directories(file.parent_path(), e);
    const bool ok = writePack(file, inputs, level, progress, error);
    std::filesystem::remove(manifest_file, e);
    if (ok && written != nullptr) *written = manifest;
    return ok;
}

bool readDataPackManifest(const std::filesystem::path& file, DataPackManifest& manifest, std::string* error) {
    std::string data;
    if (!readPackFile(file, kDataPackManifest, data, error)) return false;
    const nlohmann::json json = nlohmann::json::parse(data, nullptr, false);
    if (json.is_discarded() || json.value("format", std::string()) != "cramion-datapack") {
        if (error) *error = "No es un DataPack de Cramion";
        return false;
    }
    manifest.name = json.value("name", std::string());
    manifest.version = json.value("version", std::string("1.0"));
    manifest.engine = json.value("engine", std::string());
    manifest.created = json.value("created", std::string());
    manifest.scenes = json.value("scenes", std::vector<std::string>{});
    manifest.objects = json.value("objects", std::vector<std::string>{});
    manifest.files = json.value("files", std::vector<std::string>{});
    manifest.bytes = json.value("bytes", std::uint64_t{0});
    return true;
}

bool mountDataPack(const std::filesystem::path& file, const std::filesystem::path& assets_root, DataPackMount& mount,
                   std::string* error, const std::filesystem::path& journal) {
    DataPackManifest manifest;
    if (!readDataPackManifest(file, manifest, error)) return false;
    std::error_code e;
    // Se extrae a una carpeta temporal y se mueve a Assets lo que no existe.
    const std::filesystem::path staging =
        std::filesystem::temp_directory_path(e) /
        fromUtf8("cramion_datapack_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!extractPack(file, staging, {}, error)) {
        std::filesystem::remove_all(staging, e);
        return false;
    }
    mount = DataPackMount{};
    mount.name = manifest.name;
    mount.file = file;
    mount.scenes = manifest.scenes;
    mount.objects = manifest.objects;
    std::ofstream log;
    if (!journal.empty()) {
        std::filesystem::create_directories(journal.parent_path(), e);
        log.open(journal, std::ios::binary | std::ios::app);
    }
    for (const std::string& relative : manifest.files) {
        if (relative.find("..") != std::string::npos) continue;
        const std::filesystem::path from = staging / "Assets" / fromUtf8(relative);
        const std::filesystem::path to = assets_root / fromUtf8(relative);
        std::error_code fe;
        if (std::filesystem::exists(to, fe)) {
            mount.kept.push_back(relative);  // del juego (o de otro paquete): no se toca
            continue;
        }
        std::filesystem::create_directories(to.parent_path(), fe);
        std::filesystem::rename(from, to, fe);
        if (fe) {  // otra unidad: copia
            fe.clear();
            std::filesystem::copy_file(from, to, fe);
        }
        if (fe) {
            if (error) *error = "No se pudo escribir " + utf8(to);
            unmountDataPack(mount, assets_root, journal);
            std::filesystem::remove_all(staging, e);
            return false;
        }
        mount.written.push_back(to);
        if (log) log << relative << '\n';
    }
    std::filesystem::remove_all(staging, e);
    return true;
}

void unmountDataPack(const DataPackMount& mount, const std::filesystem::path& assets_root,
                     const std::filesystem::path& journal) {
    std::error_code e;
    std::set<std::filesystem::path> folders;
    for (const std::filesystem::path& f : mount.written) {
        std::filesystem::remove(f, e);
        folders.insert(f.parent_path());
    }
    // Carpetas que quedaron vacias (de dentro hacia fuera, sin salir de Assets).
    for (auto it = folders.rbegin(); it != folders.rend(); ++it) {
        for (std::filesystem::path p = *it; isInside(p, assets_root) && p != assets_root; p = p.parent_path()) {
            if (!std::filesystem::is_empty(p, e) || e) break;
            std::filesystem::remove(p, e);
        }
    }
    // El diario ya no tiene que borrar estos.
    if (!journal.empty() && std::filesystem::exists(journal, e)) {
        std::set<std::string> removed;
        std::error_code re;
        const std::filesystem::path root = std::filesystem::weakly_canonical(assets_root, re);
        for (const std::filesystem::path& f : mount.written) {
            removed.insert(utf8(std::filesystem::relative(std::filesystem::weakly_canonical(f, re), root, re)));
        }
        std::ifstream in(journal, std::ios::binary);
        std::vector<std::string> keep;
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && removed.count(line) == 0) keep.push_back(line);
        }
        in.close();
        std::ofstream out(journal, std::ios::binary | std::ios::trunc);
        for (const std::string& line : keep) out << line << '\n';
    }
}

void cleanupDataPackJournal(const std::filesystem::path& journal, const std::filesystem::path& assets_root) {
    std::error_code e;
    if (journal.empty() || !std::filesystem::exists(journal, e)) return;
    DataPackMount leftover;
    std::ifstream in(journal, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.find("..") != std::string::npos) continue;
        leftover.written.push_back(assets_root / fromUtf8(line));
    }
    in.close();
    if (!leftover.written.empty()) {
        std::cout << "[DataPack] Se desmontan " << leftover.written.size()
                  << " archivos de paquetes que quedaron montados\n";
    }
    unmountDataPack(leftover, assets_root);
    std::filesystem::remove(journal, e);
}

}  // namespace cramion::project
