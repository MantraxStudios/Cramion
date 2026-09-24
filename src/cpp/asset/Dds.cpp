#include "asset/Dds.h"

#include <algorithm>
#include <cstring>

namespace cramion::asset {
namespace {

// Campos de la cabecera DDS (DDS_HEADER, 124 bytes tras la firma).
constexpr std::size_t kMagicSize = 4;
constexpr std::size_t kHeaderSize = 124;
constexpr std::size_t kDx10HeaderSize = 20;

constexpr std::uint32_t kPixelFormatFourCc = 0x4;
constexpr std::uint32_t kPixelFormatRgb = 0x40;

std::uint32_t read32(const std::uint8_t* p) {
    std::uint32_t value = 0;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

constexpr std::uint32_t fourCc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 8) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d)) << 24);
}

// Formato comprimido de un FourCC clasico o de un DXGI_FORMAT (DX10).
// Las variantes sRGB se leen como UNORM: el motor linealiza a mano el color
// base y el resto de mapas son datos lineales.
TextureFormat formatFromFourCc(std::uint32_t code) {
    switch (code) {
        case fourCc('D', 'X', 'T', '1'): return TextureFormat::Bc1;
        case fourCc('D', 'X', 'T', '2'):
        case fourCc('D', 'X', 'T', '3'): return TextureFormat::Bc2;
        case fourCc('D', 'X', 'T', '4'):
        case fourCc('D', 'X', 'T', '5'): return TextureFormat::Bc3;
        case fourCc('A', 'T', 'I', '1'):
        case fourCc('B', 'C', '4', 'U'): return TextureFormat::Bc4;
        case fourCc('A', 'T', 'I', '2'):
        case fourCc('B', 'C', '5', 'U'): return TextureFormat::Bc5;
        default: return TextureFormat::Rgba8;
    }
}

bool formatFromDxgi(std::uint32_t dxgi, TextureFormat& format) {
    switch (dxgi) {
        case 70: case 71: case 72: format = TextureFormat::Bc1; return true;
        case 73: case 74: case 75: format = TextureFormat::Bc2; return true;
        case 76: case 77: case 78: format = TextureFormat::Bc3; return true;
        case 79: case 80: format = TextureFormat::Bc4; return true;
        case 82: case 83: format = TextureFormat::Bc5; return true;
        case 97: case 98: case 99: format = TextureFormat::Bc7; return true;
        case 27: case 28: case 29: format = TextureFormat::Rgba8; return true;  // R8G8B8A8
        default: return false;
    }
}

// Convierte el nivel 0 de un DDS de 32 bits por pixel con mascaras (RGBA,
// BGRA...) a RGBA8.
bool convertUncompressed32(const std::uint8_t* pixels, std::size_t available, std::uint32_t width,
                           std::uint32_t height, const std::uint32_t masks[4],
                           TextureData& out) {
    const std::size_t count = static_cast<std::size_t>(width) * height;
    if (available < count * 4) {
        return false;
    }

    const auto shift_of = [](std::uint32_t mask) {
        std::uint32_t shift = 0;
        while (mask != 0 && (mask & 1u) == 0) {
            mask >>= 1;
            ++shift;
        }
        return shift;
    };

    out.pixels.resize(count * 4);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t value = read32(pixels + i * 4);
        for (int c = 0; c < 4; ++c) {
            out.pixels[i * 4 + static_cast<std::size_t>(c)] =
                (masks[c] == 0) ? 255
                                : static_cast<std::uint8_t>((value & masks[c]) >> shift_of(masks[c]));
        }
    }
    out.format = TextureFormat::Rgba8;
    out.mip_levels = 1;
    return true;
}

}  // namespace

std::uint32_t blockBytes(TextureFormat format) {
    switch (format) {
        case TextureFormat::Bc1:
        case TextureFormat::Bc4: return 8;
        case TextureFormat::Bc2:
        case TextureFormat::Bc3:
        case TextureFormat::Bc5:
        case TextureFormat::Bc7: return 16;
        case TextureFormat::Rgba8: return 0;
    }
    return 0;
}

std::size_t mipByteSize(TextureFormat format, std::uint32_t width, std::uint32_t height) {
    if (format == TextureFormat::Rgba8) {
        return static_cast<std::size_t>(width) * height * 4;
    }
    const std::size_t blocks_x = std::max(1u, (width + 3) / 4);
    const std::size_t blocks_y = std::max(1u, (height + 3) / 4);
    return blocks_x * blocks_y * blockBytes(format);
}

bool isDds(const std::uint8_t* data, std::size_t size) {
    return size >= kMagicSize + kHeaderSize && std::memcmp(data, "DDS ", 4) == 0;
}

bool parseDds(const std::uint8_t* data, std::size_t size, TextureData& out) {
    if (!isDds(data, size)) {
        return false;
    }
    const std::uint8_t* header = data + kMagicSize;
    const std::uint32_t height = read32(header + 8);
    const std::uint32_t width = read32(header + 12);
    const std::uint32_t declared_mips = std::max(1u, read32(header + 24));
    const std::uint8_t* pixel_format = header + 72;
    const std::uint32_t pf_flags = read32(pixel_format + 4);
    const std::uint32_t pf_fourcc = read32(pixel_format + 8);
    const std::uint32_t pf_bits = read32(pixel_format + 12);
    const std::uint32_t masks[4] = {read32(pixel_format + 16), read32(pixel_format + 20),
                                    read32(pixel_format + 24), read32(pixel_format + 28)};

    if (width == 0 || height == 0) {
        return false;
    }

    std::size_t offset = kMagicSize + kHeaderSize;
    TextureFormat format = TextureFormat::Rgba8;
    bool compressed = false;

    if ((pf_flags & kPixelFormatFourCc) != 0) {
        if (pf_fourcc == fourCc('D', 'X', '1', '0')) {
            if (size < offset + kDx10HeaderSize) {
                return false;
            }
            const std::uint32_t dxgi = read32(data + offset);
            offset += kDx10HeaderSize;
            if (!formatFromDxgi(dxgi, format)) {
                return false;
            }
            compressed = format != TextureFormat::Rgba8;
            if (!compressed) {
                // R8G8B8A8 sin comprimir: el nivel 0 tal cual.
                const std::uint32_t rgba_masks[4] = {0x000000FFu, 0x0000FF00u, 0x00FF0000u,
                                                     0xFF000000u};
                out.width = width;
                out.height = height;
                return convertUncompressed32(data + offset, size - offset, width, height,
                                             rgba_masks, out);
            }
        } else {
            format = formatFromFourCc(pf_fourcc);
            compressed = format != TextureFormat::Rgba8;
            if (!compressed) {
                return false;
            }
        }
    } else if ((pf_flags & kPixelFormatRgb) != 0 && pf_bits == 32) {
        out.width = width;
        out.height = height;
        return convertUncompressed32(data + offset, size - offset, width, height, masks, out);
    } else {
        return false;
    }

    // --- Comprimido: todos los mips que haya realmente en el archivo ---
    std::size_t total = 0;
    std::uint32_t mips = 0;
    std::uint32_t w = width;
    std::uint32_t h = height;
    while (mips < declared_mips) {
        const std::size_t level = mipByteSize(format, w, h);
        if (offset + total + level > size) {
            break;
        }
        total += level;
        ++mips;
        if (w == 1 && h == 1) {
            break;
        }
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    if (mips == 0) {
        return false;
    }

    out.width = width;
    out.height = height;
    out.format = format;
    out.mip_levels = mips;
    out.pixels.assign(data + offset, data + offset + total);
    return true;
}

}  // namespace cramion::asset
