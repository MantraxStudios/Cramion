#include "CramionFX/asset/ImageFile.h"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cramion::asset {

namespace {

FILE* openFile(const std::filesystem::path& file) {
    FILE* handle = nullptr;
#ifdef _WIN32
    if (_wfopen_s(&handle, file.wstring().c_str(), L"rb") != 0) handle = nullptr;
#else
    handle = std::fopen(file.string().c_str(), "rb");
#endif
    return handle;
}

// Reduccion por media de areas (cada pixel de destino promedia su caja).
void downscale(ImageRgba8& image, std::uint32_t max_size) {
    const std::uint32_t largest = std::max(image.width, image.height);
    if (max_size == 0 || largest <= max_size) return;
    const float scale = static_cast<float>(max_size) / static_cast<float>(largest);
    const std::uint32_t w = std::max(1u, static_cast<std::uint32_t>(std::lround(static_cast<float>(image.width) * scale)));
    const std::uint32_t h = std::max(1u, static_cast<std::uint32_t>(std::lround(static_cast<float>(image.height) * scale)));
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 4);
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint32_t y0 = y * image.height / h;
        const std::uint32_t y1 = std::max(y0 + 1, (y + 1) * image.height / h);
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t x0 = x * image.width / w;
            const std::uint32_t x1 = std::max(x0 + 1, (x + 1) * image.width / w);
            std::uint32_t sum[4] = {0, 0, 0, 0};
            for (std::uint32_t sy = y0; sy < y1; ++sy) {
                const std::uint8_t* row = image.pixels.data() + (static_cast<std::size_t>(sy) * image.width + x0) * 4;
                for (std::uint32_t sx = x0; sx < x1; ++sx, row += 4) {
                    sum[0] += row[0];
                    sum[1] += row[1];
                    sum[2] += row[2];
                    sum[3] += row[3];
                }
            }
            const std::uint32_t count = (y1 - y0) * (x1 - x0);
            std::uint8_t* dst = out.data() + (static_cast<std::size_t>(y) * w + x) * 4;
            for (int c = 0; c < 4; ++c) dst[c] = static_cast<std::uint8_t>(sum[c] / count);
        }
    }
    image.width = w;
    image.height = h;
    image.pixels = std::move(out);
}

}  // namespace

bool loadImageRgba8(const std::filesystem::path& file, ImageRgba8& image, std::uint32_t max_size) {
    FILE* handle = openFile(file);
    if (handle == nullptr) return false;
    int width = 0;
    int height = 0;
    int channels = 0;
    const bool hdr = stbi_is_hdr_from_file(handle) != 0;
    image = ImageRgba8{};
    if (hdr) {
        // Cielos HDR: Reinhard y gamma 2.2 para que la miniatura se parezca.
        float* data = stbi_loadf_from_file(handle, &width, &height, &channels, 4);
        std::fclose(handle);
        if (data == nullptr) return false;
        image.width = static_cast<std::uint32_t>(width);
        image.height = static_cast<std::uint32_t>(height);
        image.pixels.resize(static_cast<std::size_t>(width) * height * 4);
        for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
            for (int c = 0; c < 3; ++c) {
                const float v = data[i * 4 + c];
                const float mapped = std::pow(v / (1.0f + v), 1.0f / 2.2f);
                image.pixels[i * 4 + c] = static_cast<std::uint8_t>(std::clamp(mapped, 0.0f, 1.0f) * 255.0f);
            }
            image.pixels[i * 4 + 3] = 255;
        }
        stbi_image_free(data);
    } else {
        stbi_uc* data = stbi_load_from_file(handle, &width, &height, &channels, 4);
        std::fclose(handle);
        if (data == nullptr) return false;
        image.width = static_cast<std::uint32_t>(width);
        image.height = static_cast<std::uint32_t>(height);
        image.pixels.assign(data, data + static_cast<std::size_t>(width) * height * 4);
        stbi_image_free(data);
    }
    downscale(image, max_size);
    return true;
}

}  // namespace cramion::asset
