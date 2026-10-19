#include "CramionFX/asset/TextureStats.h"

#include "CramionFX/asset/Dds.h"
#include "CramionFX/asset/ImageFile.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace cramion::asset {

namespace {

// La misma conversion que el render (lighting.frag toLinear).
float toLinear(float c) { return std::pow(std::clamp(c, 0.0f, 1.0f), 2.2f); }

}  // namespace

bool averageTextureLinear(const TextureData& t, core::Vec3& rgb, float* opaque_fraction) {
    std::vector<std::uint8_t> decoded;
    const std::uint8_t* px = nullptr;
    const std::size_t count = static_cast<std::size_t>(t.width) * t.height;
    if (count == 0) return false;
    if (t.format == TextureFormat::Rgba8) {
        if (t.pixels.size() < count * 4) return false;
        px = t.pixels.data();
    } else {
        decoded = decodeBlockCompressed(t);
        if (decoded.size() < count * 4) return false;
        px = decoded.data();
    }
    const std::size_t step = std::max<std::size_t>(count / 2048, 1);
    double r = 0.0, g = 0.0, b = 0.0, n = 0.0;     // opacos
    double ar = 0.0, ag = 0.0, ab = 0.0, an = 0.0;  // todos
    for (std::size_t i = 0; i < count; i += step) {
        const std::uint8_t* p = px + i * 4;
        const double lr = toLinear(static_cast<float>(p[0]) / 255.0f);
        const double lg = toLinear(static_cast<float>(p[1]) / 255.0f);
        const double lb = toLinear(static_cast<float>(p[2]) / 255.0f);
        ar += lr;
        ag += lg;
        ab += lb;
        an += 1.0;
        if (p[3] < 128) continue;  // recortado (hojas, rejas): no se ve
        r += lr;
        g += lg;
        b += lb;
        n += 1.0;
    }
    if (an <= 0.0) return false;
    if (opaque_fraction != nullptr) *opaque_fraction = static_cast<float>(n / an);
    if (n > 0.0) {
        rgb = core::Vec3{static_cast<float>(r / n), static_cast<float>(g / n), static_cast<float>(b / n)};
    } else {
        rgb = core::Vec3{static_cast<float>(ar / an), static_cast<float>(ag / an), static_cast<float>(ab / an)};
    }
    return true;
}

bool averageStoredTextureLinear(const TextureData& t, core::Vec3& rgb, float* opaque_fraction) {
    if (!t.pixels.empty()) return averageTextureLinear(t, rgb, opaque_fraction);
    TextureData decoded;
    if (!t.encoded.empty()) {
        if (isDds(t.encoded.data(), t.encoded.size())) {
            if (!parseDds(t.encoded.data(), t.encoded.size(), decoded)) return false;
            return averageTextureLinear(decoded, rgb, opaque_fraction);
        }
        ImageRgba8 image;
        if (!decodeImageRgba8(t.encoded.data(), t.encoded.size(), image)) return false;
        decoded.width = image.width;
        decoded.height = image.height;
        decoded.format = TextureFormat::Rgba8;
        decoded.pixels = std::move(image.pixels);
        return averageTextureLinear(decoded, rgb, opaque_fraction);
    }
    if (t.source_path.empty()) return false;
    if (isLazyTexture(t) && resolveLazyTexture(t, decoded) && averageTextureLinear(decoded, rgb, opaque_fraction)) {
        return true;
    }
    return averageImageFileLinear(std::filesystem::path(std::u8string(t.source_path.begin(), t.source_path.end())), rgb,
                                  opaque_fraction);
}

bool averageImageFileLinear(const std::filesystem::path& file, core::Vec3& rgb, float* opaque_fraction) {
    if (file.empty()) return false;
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error)) return false;
    ImageRgba8 image;
    if (!loadImageRgba8(file, image, 256)) return false;
    TextureData t;
    t.width = image.width;
    t.height = image.height;
    t.format = TextureFormat::Rgba8;
    t.pixels = std::move(image.pixels);
    return averageTextureLinear(t, rgb, opaque_fraction);
}

}  // namespace cramion::asset
