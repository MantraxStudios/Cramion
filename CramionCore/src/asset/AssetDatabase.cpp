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
}

void AssetDatabase::refresh() {
    assets_.clear();
    by_uuid_.clear();

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
        const std::string extension = lower(file.extension().string());

        std::optional<AssetInfo> info;
        if (extension == crdata::kExtension) {
            if (const auto header = crdata::readHeader(file)) {
                AssetInfo a{};
                a.uuid = header->uuid;
                a.type = header->type;
                a.name = crdata::utf8(file.stem());
                a.path = file;
                a.source = crdata::fromUtf8(header->source);
                info = a;
            } else {
                std::cerr << "[Assets] " << crdata::utf8(file.filename())
                          << " no es un .crdata valido (o es de otra version): se ignora\n";
            }
        } else if (extension == ".crscene") {
            info = sceneInfo(file);
            if (!info) {
                std::cerr << "[Assets] Escena sin UUID o con JSON danado: "
                          << crdata::utf8(file.filename()) << "\n";
            }
        } else if (extension == ".cranimator" || extension == ".cranim") {
            const bool clip = extension == ".cranim";
            info = sceneInfo(file, clip ? AssetType::AnimationClip : AssetType::AnimatorController, clip);
            if (!info) {
                std::cerr << "[Assets] Animacion sin UUID o danada: " << crdata::utf8(file.filename()) << "\n";
            }
        } else if (extension == ".crprefab") {
            info = sceneInfo(file, AssetType::Prefab);
            if (!info) {
                std::cerr << "[Assets] Prefab sin UUID o danado: " << crdata::utf8(file.filename()) << "\n";
            }
        } else if (extension == ".crmat") {
            info = sceneInfo(file, AssetType::Material);
            if (!info) {
                std::cerr << "[Assets] Material sin UUID o danado: " << crdata::utf8(file.filename()) << "\n";
            }
        }
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
        by_uuid_.emplace(info->uuid, assets_.size());
        assets_.push_back(std::move(*info));
    }
    if (duplicates > 0) {
        std::cerr << "[Assets] " << duplicates << " assets con UUID repetido\n";
    }
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
