#include "CramionCore/asset/RenderTextureAsset.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace cramion::assets {

bool loadRenderTexture(const std::filesystem::path& path, RenderTextureAsset& out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error != nullptr) *error = "no se puede abrir";
        return false;
    }
    const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        if (error != nullptr) *error = "JSON no valido";
        return false;
    }
    RenderTextureAsset t;
    t.uuid = Uuid::parse(json.value("uuid", std::string()));
    t.width = std::clamp(json.value("width", 512), 1, 8192);
    t.height = std::clamp(json.value("height", 512), 1, 8192);
    out = t;
    return true;
}

bool saveRenderTexture(RenderTextureAsset& texture, const std::filesystem::path& path, std::string* error) {
    if (!texture.uuid.valid()) texture.uuid = Uuid::generate();
    texture.width = std::clamp(texture.width, 1, 8192);
    texture.height = std::clamp(texture.height, 1, 8192);
    const nlohmann::json json = {{"uuid", texture.uuid.toString()}, {"width", texture.width}, {"height", texture.height}};
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        if (error != nullptr) *error = "no se puede escribir";
        return false;
    }
    out << json.dump(2) << "\n";
    return static_cast<bool>(out);
}

}  // namespace cramion::assets
