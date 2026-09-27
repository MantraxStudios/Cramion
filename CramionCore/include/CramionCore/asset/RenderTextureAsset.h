#ifndef CRAMION_CORE_ASSET_RENDER_TEXTURE_ASSET_H
#define CRAMION_CORE_ASSET_RENDER_TEXTURE_ASSET_H

// Render Texture (.crrt, JSON), como el RenderTexture de Unity: una textura
// que se rellena cada frame con lo que ve una camara que la tiene como Target
// Texture. Se usa en un material (color o emision) como cualquier imagen:
// pantallas, camaras de seguridad, espejos, minimapas, retratos.

#include "CramionCore/Uuid.h"

#include <filesystem>
#include <string>

namespace cramion::assets {

inline constexpr const char* kRenderTextureExtension = ".crrt";

struct RenderTextureAsset {
    Uuid uuid;
    int width = 512;
    int height = 512;
};

bool loadRenderTexture(const std::filesystem::path& path, RenderTextureAsset& out, std::string* error = nullptr);
// Sin UUID se le da uno nuevo (y se escribe en `texture`).
bool saveRenderTexture(RenderTextureAsset& texture, const std::filesystem::path& path, std::string* error = nullptr);

}  // namespace cramion::assets

#endif  // CRAMION_CORE_ASSET_RENDER_TEXTURE_ASSET_H
