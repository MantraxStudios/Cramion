#include "CramionCore/asset/AssetManager.h"

#include "CrData.h"
#include "Primitives.h"

#include <chrono>
#include <fstream>
#include <iostream>

namespace cramion::assets {

std::shared_ptr<ModelAsset> AssetManager::readModel(const Uuid& uuid, const std::filesystem::path& file,
                                                   const std::string& name) {
    if (primitives::isBuiltin(uuid)) {
        return primitives::make(uuid);
    }
    const auto start = std::chrono::steady_clock::now();
    crdata::Header header{};
    crdata::ModelContent content{};
    if (!crdata::readModel(file, header, content)) {
        std::cerr << "[Assets] No se pudo leer " << crdata::utf8(file)
                  << " (danado o de otra version)\n";
        return nullptr;
    }

    const float read_seconds =
        std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();

    auto asset = std::make_shared<ModelAsset>();
    asset->uuid = uuid;
    asset->name = name;
    asset->nodes = std::move(content.nodes);
    asset->animated = content.animated;
    asset->animation_names = std::move(content.animation_names);
    asset->parts.reserve(content.parts.size());
    try {
        std::size_t reclustered = 0;
        std::size_t before = 0;
        std::size_t after = 0;
        for (asset::ModelData& part : content.parts) {
            // .crdata de versiones anteriores con clusteres de 5 unidades
            // fijas: en un FBX en centimetros eran miles de submallas (y de
            // llamadas de dibujo) por objeto. Se reagrupan en memoria.
            if (asset::isOverClustered(part)) {
                before += part.submeshes.size();
                asset::clusterSubmeshes(part);
                after += part.submeshes.size();
                ++reclustered;
            }
        }
        if (reclustered > 0) {
            std::cout << "[Assets] " << name << ": reagrupadas " << reclustered << " piezas ("
                      << before << " -> " << after
                      << " submallas). Reimporta el modelo para guardarlo asi.\n";
        }
        for (asset::ModelData& part : content.parts) {
            // Texturas incrustadas: se decodifican aqui (en paralelo).
            asset::finalizeModel(part, name + "/" + part.name);
            asset->parts.push_back(std::make_shared<asset::ModelData>(std::move(part)));
        }
    } catch (const std::exception& error) {
        std::cerr << "[Assets] " << error.what() << "\n";
        return nullptr;
    }

    const float seconds =
        std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
    std::cout << "[Assets] Cargado " << name << ": " << asset->parts.size() << " piezas, "
              << asset->nodes.size() << " nodos, " << seconds << " s (lectura " << read_seconds
              << " s)\n";
    return asset;
}

std::shared_ptr<const ModelAsset> AssetManager::loadModel(const Uuid& uuid) {
    if (const auto it = models_.find(uuid); it != models_.end()) {
        return it->second;
    }

    if (primitives::isBuiltin(uuid)) {
        std::shared_ptr<ModelAsset> asset = primitives::make(uuid);
        models_.emplace(uuid, asset);
        return asset;
    }

    const std::optional<AssetInfo> info = database_.find(uuid);
    if (!info || info->type != AssetType::Model || info->path.empty()) {
        std::cerr << "[Assets] No hay ningun modelo con UUID " << uuid.toString() << "\n";
        return nullptr;
    }

    std::shared_ptr<ModelAsset> asset = readModel(uuid, info->path, info->name);
    if (!asset) return nullptr;
    models_.emplace(uuid, asset);
    return asset;
}

std::filesystem::path AssetManager::environmentFile(const Uuid& uuid) {
    const std::optional<AssetInfo> info = database_.find(uuid);
    if (!info || info->type != AssetType::Environment || info->path.empty()) {
        std::cerr << "[Assets] No hay ningun cielo con UUID " << uuid.toString() << "\n";
        return {};
    }

    std::filesystem::path folder = cache_folder_;
    if (folder.empty()) {
        folder = std::filesystem::temp_directory_path() / "Cramion" / "Cache";
    }
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    const std::filesystem::path extracted = folder / (uuid.toString() + ".hdr");

    // Ya extraido y mas nuevo que el .crdata: no se repite (el HDR de Bistro
    // son 24 MB).
    if (std::filesystem::exists(extracted, error) &&
        std::filesystem::last_write_time(extracted, error) >=
            std::filesystem::last_write_time(info->path, error)) {
        return extracted;
    }

    crdata::Header header{};
    std::string extension;
    std::vector<std::uint8_t> bytes;
    if (!crdata::readEnvironment(info->path, header, extension, bytes)) {
        std::cerr << "[Assets] No se pudo leer el cielo " << crdata::utf8(info->path) << "\n";
        return {};
    }
    std::filesystem::path temporary = extracted;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            std::cerr << "[Assets] No se pudo extraer el cielo a " << crdata::utf8(extracted)
                      << "\n";
            return {};
        }
    }
    std::filesystem::rename(temporary, extracted, error);
    if (error) {
        std::cerr << "[Assets] No se pudo extraer el cielo: " << error.message() << "\n";
        return {};
    }
    return extracted;
}

void AssetManager::unload(const Uuid& uuid) {
    models_.erase(uuid);
}

void AssetManager::clear() {
    models_.clear();
}

}  // namespace cramion::assets
