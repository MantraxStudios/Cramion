#include "Inflate.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include <stb_image.h>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <climits>
#include <cstring>

namespace cramion::update::detail {

bool inflateRaw(const unsigned char* data, std::size_t size, unsigned char* out, std::size_t out_size) {
    if (size > INT_MAX || out_size > INT_MAX) return false;
    if (out_size == 0) return true;
    const int written = stbi_zlib_decode_noheader_buffer(reinterpret_cast<char*>(out), static_cast<int>(out_size),
                                                         reinterpret_cast<const char*>(data), static_cast<int>(size));
    return written == static_cast<int>(out_size);
}

std::vector<unsigned char> decodeImage(const unsigned char* data, std::size_t size, int& width, int& height) {
    width = height = 0;
    if (size > INT_MAX) return {};
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(data, static_cast<int>(size), &width, &height, &channels, 4);
    if (pixels == nullptr) return {};
    std::vector<unsigned char> rgba(pixels, pixels + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
    stbi_image_free(pixels);
    return rgba;
}

bool writePng(const char* path, int width, int height, const unsigned char* rgba, int stride) {
    return stbi_write_png(path, width, height, 4, rgba, stride) != 0;
}

}  // namespace cramion::update::detail
