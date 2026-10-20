#include "CramionCore/asset/AssetDatabase.h"

#include "CrData.h"
#include "Primitives.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>

namespace cramion::assets {

namespace {

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// El UUID de una escena esta en su JSON (campo "uuid"). Se lee el archivo
// entero: las escenas son pequenas comparadas con los modelos. Los
// .cranimator igual; de los .cranim solo la primera linea (la cabecera).
std::optional<AssetInfo> sceneInfo(const std::filesystem::path& file,
                                   AssetType type = AssetType::Scene, bool first_line_only = false) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    nlohmann::json json;
    if (first_line_only) {
        std::string line;
        std::getline(in, line);
        json = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    } else {
        json = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    }
    if (json.is_discarded() || !json.is_object() || !json.contains("uuid") ||
        !json["uuid"].is_string()) {
        return std::nullopt;
    }
    const Uuid uuid = Uuid::parse(json["uuid"].get<std::string>());
    if (!uuid.valid()) {
        return std::nullopt;
    }
    AssetInfo info{};
    info.uuid = uuid;
    info.type = type;
    info.name = crdata::utf8(file.stem());
    info.path = file;
    return info;
}

// Tipos de JSON con UUID: extension, tipo, solo la 1a linea y nombre para el aviso.
struct JsonKind {
    const char* extension;
    AssetType type;
    bool first_line_only;
    const char* label;
};
constexpr JsonKind kJsonKinds[] = {
    {".crscene", AssetType::Scene, false, "Escena sin UUID o con JSON danado"},
    {".cranimator", AssetType::AnimatorController, false, "Animacion sin UUID o danada"},
    {".cranim", AssetType::AnimationClip, true, "Animacion sin UUID o danada"},
    {".crprefab", AssetType::Prefab, false, "Prefab sin UUID o danado"},
    {".crdialog", AssetType::Dialogue, false, "Dialogo sin UUID o danado"},
    {".crfsm", AssetType::StateMachine, false, "Maquina de estados sin UUID o danada"},
    {".crmmdb", AssetType::MotionDatabase, false, "Base de Motion Matching sin UUID o danada"},
    {".crbt", AssetType::BehaviorTree, false, "Behavior Tree sin UUID o danado"},
    {".crvfx", AssetType::VisualEffect, false, "Efecto VFX sin UUID o danado"},
    // 1a linea: cabecera JSON (uuid); despues los trozos.
    {".crfracture", AssetType::Fracture, true, "Fractura sin UUID o danada"},
    {".crrt", AssetType::RenderTexture, false, "Render Texture sin UUID o danado"},
    {".crmat", AssetType::Material, false, "Material sin UUID o danado"},
};

// El asset de un archivo (sin tamano) o nada si no es un asset; avisa de los
// danados. Lo usan el recorrido completo y el incremental.
std::optional<AssetInfo> readAsset(const std::filesystem::path& file) {
    const std::string extension = lower(file.extension().string());
    if (extension == crdata::kExtension) {
        if (const auto header = crdata::readHeader(file)) {
            AssetInfo a{};
            a.uuid = header->uuid;
            a.type = header->type;
            a.name = crdata::utf8(file.stem());
            a.path = file;
            a.source = crdata::fromUtf8(header->source);
            return a;
        }
        std::cerr << "[Assets] " << crdata::utf8(file.filename())
                  << " no es un .crdata valido (o es de otra version): se ignora\n";
        return std::nullopt;
    }
    for (const JsonKind& kind : kJsonKinds) {
        if (extension != kind.extension) continue;
        std::optional<AssetInfo> info = sceneInfo(file, kind.type, kind.first_line_only);
        if (!info) std::cerr << "[Assets] " << kind.label << ": " << crdata::utf8(file.filename()) << "\n";
        return info;
    }
    return std::nullopt;
}

// Clave de una ruta para comparar (separadores y, en Windows, mayusculas: el
// sistema de archivos no las distingue).
std::string pathKey(const std::filesystem::path& path) {
    std::string text = crdata::utf8(path.lexically_normal());
    std::replace(text.begin(), text.end(), '\\', '/');
#if defined(_WIN32)
    text = lower(std::move(text));
#endif
    while (text.size() > 1 && text.back() == '/') text.pop_back();
    return text;
}

// `key` es `root` o esta dentro (las dos con pathKey).
bool insideKey(const std::string& key, const std::string& root) {
    return key == root || (key.size() > root.size() && key.compare(0, root.size(), root) == 0 && key[root.size()] == '/');
}

}  // namespace

void AssetDatabase::open(const std::filesystem::path& assets_root) {
    root_ = assets_root;
    std::error_code error;
    std::filesystem::create_directories(root_, error);
    refresh();
}

void AssetDatabase::close() {
    root_.clear();
    assets_.clear();
    by_uuid_.clear();
    stamps_.clear();
}

void AssetDatabase::reindex() {
    by_uuid_.clear();
    by_uuid_.reserve(assets_.size());
    for (std::size_t i = 0; i < assets_.size(); ++i) by_uuid_.emplace(assets_[i].uuid, i);
}

void AssetDatabase::refresh() {
    assets_.clear();
    by_uuid_.clear();
    stamps_.clear();

    // Integrados primero (sin archivo).
    for (const AssetInfo& builtin : primitives::builtinInfos()) {
        by_uuid_.emplace(builtin.uuid, assets_.size());
        assets_.push_back(builtin);
    }
    if (root_.empty()) {
        return;
    }

    std::error_code error;
    std::filesystem::recursive_directory_iterator it(
        root_, std::filesystem::directory_options::skip_permission_denied, error);
    std::size_t duplicates = 0;
    for (; !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (!it->is_regular_file(error)) {
            continue;
        }
        const std::filesystem::path& file = it->path();
        std::optional<AssetInfo> info = readAsset(file);
        if (!info) {
            continue;
        }
        info->size_bytes = it->file_size(error);

        // Dos archivos con el mismo UUID (una copia hecha a mano en el
        // explorador): el segundo no entra, las referencias seguirian yendo
        // al primero sin avisar.
        if (by_uuid_.contains(info->uuid)) {
            ++duplicates;
            std::cerr << "[Assets] AVISO: " << crdata::utf8(file) << " tiene el mismo UUID que "
                      << crdata::utf8(assets_[by_uuid_[info->uuid]].path)
                      << " (copia manual?). Se ignora; reimportalo para darle uno nuevo.\n";
            continue;
        }
        stamps_[info->uuid] = it->last_write_time(error);
        by_uuid_.emplace(info->uuid, assets_.size());
        assets_.push_back(std::move(*info));
    }
    if (duplicates > 0) {
        std::cerr << "[Assets] " << duplicates << " assets con UUID repetido\n";
    }
}

AssetScan AssetDatabase::scanPaths(const std::filesystem::path& root, std::vector<std::filesystem::path> paths) {
    AssetScan scan;
    // Sin repetidos y sin las que ya estan dentro de otra pedida (una carpeta
    // copiada avisa de ella y de cada archivo).
    std::vector<std::pair<std::string, std::filesystem::path>> keyed;
    keyed.reserve(paths.size());
    for (std::filesystem::path& path : paths) {
        if (path.empty()) continue;
        if (path.is_relative()) path = root / path;
        keyed.emplace_back(pathKey(path), std::move(path));
    }
    std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string last;
    for (auto& [key, path] : keyed) {
        if (!last.empty() && insideKey(key, last)) continue;
        last = key;
        scan.roots.push_back(std::move(path));
    }

    const auto add = [&](const std::filesystem::path& file, std::uintmax_t size, std::filesystem::file_time_type stamp) {
        std::optional<AssetInfo> info = readAsset(file);
        if (!info) return;
        info->size_bytes = size;
        scan.found.push_back(std::move(*info));
        scan.stamps.push_back(stamp);
    };
    for (const std::filesystem::path& path : scan.roots) {
        std::error_code error;
        const std::filesystem::file_status status = std::filesystem::status(path, error);
        if (std::filesystem::is_regular_file(status)) {
            add(path, std::filesystem::file_size(path, error), std::filesystem::last_write_time(path, error));
        } else if (std::filesystem::is_directory(status)) {
            for (std::filesystem::recursive_directory_iterator it(
                     path, std::filesystem::directory_options::skip_permission_denied, error), end;
                 !error && it != end; it.increment(error)) {
                if (it->is_regular_file(error)) add(it->path(), it->file_size(error), it->last_write_time(error));
            }
        }
        // (No existe: lo que habia dentro sale en applyScan.)
    }
    return scan;
}

AssetChanges AssetDatabase::applyScan(const AssetScan& scan) {
    AssetChanges changes;
    if (root_.empty() || scan.roots.empty()) return changes;
    std::vector<std::string> roots;
    roots.reserve(scan.roots.size());
    for (const std::filesystem::path& path : scan.roots) roots.push_back(pathKey(path));

    // Lo que habia dentro de las rutas pedidas: sale salvo que vuelva a aparecer.
    std::vector<std::uint8_t> stale(assets_.size(), 0);
    for (std::size_t i = 0; i < assets_.size(); ++i) {
        if (assets_[i].path.empty()) continue;  // integrado
        const std::string key = pathKey(assets_[i].path);
        for (const std::string& root : roots) {
            if (insideKey(key, root)) {
                stale[i] = 1;
                break;
            }
        }
    }

    for (std::size_t f = 0; f < scan.found.size(); ++f) {
        const AssetInfo& info = scan.found[f];
        const std::filesystem::file_time_type stamp = f < scan.stamps.size() ? scan.stamps[f] : std::filesystem::file_time_type{};
        const auto it = by_uuid_.find(info.uuid);
        if (it == by_uuid_.end()) {
            stamps_[info.uuid] = stamp;
            by_uuid_.emplace(info.uuid, assets_.size());
            assets_.push_back(info);
            stale.push_back(0);
            changes.added.push_back(info.uuid);
            continue;
        }
        const std::size_t index = it->second;
        AssetInfo& known = assets_[index];
        if (known.path.empty()) continue;  // el UUID de un integrado: no se toca
        const bool same_path = pathKey(known.path) == pathKey(info.path);
        if (!same_path) {
            // Otro archivo con su UUID: es el mismo movido si en la ruta vieja ya
            // no esta (o se pidio y no aparecio); si sigue alli, es una copia.
            std::error_code error;
            if (!stale[index] && std::filesystem::exists(known.path, error)) {
                std::cerr << "[Assets] AVISO: " << crdata::utf8(info.path) << " tiene el mismo UUID que "
                          << crdata::utf8(known.path) << " (copia manual?). Se ignora; reimportalo para darle uno nuevo.\n";
                continue;
            }
            changes.moved.push_back(info.uuid);
        } else {
            const auto old_stamp = stamps_.find(info.uuid);
            if (old_stamp == stamps_.end() || old_stamp->second != stamp || known.type != info.type) {
                changes.modified.push_back(info.uuid);
            }
        }
        known = info;
        stamps_[info.uuid] = stamp;
        stale[index] = 0;
    }

    // Fuera lo que ya no esta (en el orden de siempre, sin huecos).
    bool any_removed = false;
    for (std::size_t i = 0; i < assets_.size(); ++i) {
        if (!stale[i]) continue;
        changes.removed.push_back(assets_[i].uuid);
        stamps_.erase(assets_[i].uuid);
        any_removed = true;
    }
    if (any_removed) {
        std::size_t write = 0;
        for (std::size_t i = 0; i < assets_.size(); ++i) {
            if (stale[i]) continue;
            if (write != i) assets_[write] = std::move(assets_[i]);
            ++write;
        }
        assets_.resize(write);
        reindex();
    }
    return changes;
}

std::optional<AssetInfo> AssetDatabase::find(const Uuid& uuid) const {
    const auto it = by_uuid_.find(uuid);
    if (it == by_uuid_.end()) {
        return std::nullopt;
    }
    return assets_[it->second];
}

std::vector<AssetInfo> AssetDatabase::inFolder(const std::filesystem::path& folder) const {
    std::vector<AssetInfo> result;
    std::error_code error;
    const std::filesystem::path wanted = std::filesystem::weakly_canonical(folder, error);
    for (const AssetInfo& info : assets_) {
        if (info.path.empty()) {
            continue;
        }
        if (std::filesystem::weakly_canonical(info.path.parent_path(), error) == wanted) {
            result.push_back(info);
        }
    }
    std::sort(result.begin(), result.end(),
              [](const AssetInfo& a, const AssetInfo& b) { return lower(a.name) < lower(b.name); });
    return result;
}

bool AssetDatabase::move(const Uuid& uuid, const std::filesystem::path& new_path) {
    const auto it = by_uuid_.find(uuid);
    if (it == by_uuid_.end() || assets_[it->second].path.empty()) {
        return false;  // no existe o es integrado
    }
    AssetInfo& info = assets_[it->second];
    std::error_code error;
    if (std::filesystem::exists(new_path, error)) {
        std::cerr << "[Assets] Ya existe " << crdata::utf8(new_path) << "\n";
        return false;
    }
    std::filesystem::create_directories(new_path.parent_path(), error);
    std::filesystem::rename(info.path, new_path, error);
    if (error) {
        std::cerr << "[Assets] No se pudo mover " << crdata::utf8(info.path) << ": "
                  << error.message() << "\n";
        return false;
    }
    info.path = new_path;
    info.name = crdata::utf8(new_path.stem());
    return true;
}

bool AssetDatabase::remove(const Uuid& uuid) {
    const auto it = by_uuid_.find(uuid);
    if (it == by_uuid_.end() || assets_[it->second].path.empty()) {
        return false;
    }
    std::error_code error;
    std::filesystem::remove(assets_[it->second].path, error);
    if (error) {
        std::cerr << "[Assets] No se pudo borrar " << crdata::utf8(assets_[it->second].path)
                  << ": " << error.message() << "\n";
        return false;
    }
    refresh();
    return true;
}

}  // namespace cramion::assets
