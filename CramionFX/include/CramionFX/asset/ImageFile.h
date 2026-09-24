#ifndef CRAMION_ASSET_IMAGE_FILE_H
#define CRAMION_ASSET_IMAGE_FILE_H

// Leer una imagen de disco como RGBA8 (PNG, JPG, TGA, BMP y HDR, este con un
// tonemap sencillo), reducida si hace falta: iconos y miniaturas del editor.
// Se puede llamar desde cualquier hilo.

#include <cstdint>
#include <filesystem>
#include <vector>

namespace cramion::asset {

struct ImageRgba8 {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;  // width * height * 4, fila de arriba primero
};

// `max_size` (> 0): el lado mayor se reduce a ese tamano (media de areas).
// false si no se pudo leer.
bool loadImageRgba8(const std::filesystem::path& file, ImageRgba8& image, std::uint32_t max_size = 0);

}  // namespace cramion::asset

#endif  // CRAMION_ASSET_IMAGE_FILE_H
