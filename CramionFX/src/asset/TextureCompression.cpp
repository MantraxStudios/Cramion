#include "CramionFX/asset/TextureCompression.h"

#include "CramionFX/asset/Dds.h"

#include <bc7enc.h>

#define STB_DXT_STATIC
#define STB_DXT_IMPLEMENTATION
#include <stb_dxt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <vector>

namespace cramion::asset {
namespace {

// Sube si cambia el resultado (otro codificador, otros mips): las de antes
// dejan de valer y se rehacen. 2: BC1 para color y datos opacos, mips en
// espacio lineal y normal maps renormalizados.
constexpr std::uint64_t kEncoderVersion = 2;

std::mutex g_folder_mutex;
std::filesystem::path g_folder;
std::filesystem::path g_portable_root;  // (con g_folder_mutex)

// Compresiones en marcha: los hilos de cada una se reparten los nucleos
// (decodeTextures ya lanza un hilo por textura).
std::atomic<unsigned> g_running{0};

// Claves que se estan comprimiendo ahora mismo.
std::mutex g_busy_mutex;
std::condition_variable g_busy_done;
std::unordered_set<std::uint64_t> g_busy;

std::uint64_t mix(std::uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

// Lo que dice el DX10 del canal alfa (miscFlags2): asi la cache recuerda si
// la textura tiene recortes (con BC7 no se puede saber sin descomprimir).
constexpr std::uint32_t kAlphaModeStraight = 1;
constexpr std::uint32_t kAlphaModeOpaque = 3;

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    const std::size_t at = out.size();
    out.resize(at + 4);
    std::memcpy(out.data() + at, &value, 4);
}

std::filesystem::path cachePath(std::uint64_t key) {
    std::filesystem::path folder;
    {
        const std::lock_guard<std::mutex> lock(g_folder_mutex);
        folder = g_folder;
    }
    if (folder.empty()) return {};
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.dds", static_cast<unsigned long long>(key));
    return folder / name;
}

// sRGB <-> lineal (los mips del color se promedian en lineal: en sRGB las
// medias salen mas oscuras y lo lejano se ennegrece).
const std::array<float, 256>& srgbToLinearTable() {
    static const std::array<float, 256> table = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const float c = static_cast<float>(i) / 255.0f;
            t[static_cast<std::size_t>(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    return table;
}

// Con tabla de 4096 entradas (los mips de una de 4K son millones de texeles).
std::uint8_t linearToSrgb(float linear) {
    static const std::array<std::uint8_t, 4096> table = [] {
        std::array<std::uint8_t, 4096> t{};
        for (std::size_t i = 0; i < t.size(); ++i) {
            const float l = static_cast<float>(i) / static_cast<float>(t.size() - 1);
            const float c = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
            t[i] = static_cast<std::uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
        }
        return t;
    }();
    const float index = std::clamp(linear, 0.0f, 1.0f) * static_cast<float>(table.size() - 1);
    return table[static_cast<std::size_t>(index + 0.5f)];
}

// Como se promedian los texeles al bajar de mip.
enum class MipFilter { Plain, Srgb, Normal };

// Mitad de lado (media de 2x2) de una imagen RGBA8.
std::vector<std::uint8_t> halve(const std::vector<std::uint8_t>& src, std::uint32_t w, std::uint32_t h,
                                std::uint32_t& out_w, std::uint32_t& out_h, MipFilter filter) {
    out_w = std::max(w / 2, 1u);
    out_h = std::max(h / 2, 1u);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(out_w) * out_h * 4);
    const std::array<float, 256>& to_linear = srgbToLinearTable();
    for (std::uint32_t y = 0; y < out_h; ++y) {
        const std::uint32_t y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
        for (std::uint32_t x = 0; x < out_w; ++x) {
            const std::uint32_t x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
            const std::uint8_t* t[4] = {&src[(static_cast<std::size_t>(y0) * w + x0) * 4],
                                        &src[(static_cast<std::size_t>(y0) * w + x1) * 4],
                                        &src[(static_cast<std::size_t>(y1) * w + x0) * 4],
                                        &src[(static_cast<std::size_t>(y1) * w + x1) * 4]};
            std::uint8_t* o = &out[(static_cast<std::size_t>(y) * out_w + x) * 4];
            // Alfa (y todo en los datos): media simple.
            for (std::size_t c = 0; c < 4; ++c) {
                o[c] = static_cast<std::uint8_t>((t[0][c] + t[1][c] + t[2][c] + t[3][c] + 2u) / 4u);
            }
            if (filter == MipFilter::Srgb) {
                for (std::size_t c = 0; c < 3; ++c) {
                    o[c] = linearToSrgb((to_linear[t[0][c]] + to_linear[t[1][c]] + to_linear[t[2][c]] + to_linear[t[3][c]]) *
                                        0.25f);
                }
            } else if (filter == MipFilter::Normal) {
                // La media de normales es mas corta que 1: se alarga otra vez.
                float n[3] = {0.0f, 0.0f, 0.0f};
                for (int k = 0; k < 4; ++k) {
                    for (std::size_t c = 0; c < 3; ++c) n[c] += static_cast<float>(t[k][c]) / 127.5f - 1.0f;
                }
                const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (len > 1e-4f) {
                    for (std::size_t c = 0; c < 3; ++c) {
                        o[c] = static_cast<std::uint8_t>(std::lround(std::clamp((n[c] / len + 1.0f) * 127.5f, 0.0f, 255.0f)));
                    }
                }
            }
        }
    }
    return out;
}

// Parece un normal map en espacio tangente (azulado, R y G alrededor de la
// mitad): los que no dicen su uso tampoco se comprimen a BC1.
bool looksLikeNormalMap(const std::vector<std::uint8_t>& pixels) {
    const std::size_t count = pixels.size() / 4;
    if (count == 0) return false;
    const std::size_t step = std::max<std::size_t>(count / 4096, 1);
    double r = 0.0, g = 0.0, b = 0.0;
    std::size_t blue = 0, samples = 0;
    for (std::size_t i = 0; i < count; i += step) {
        const std::uint8_t* p = &pixels[i * 4];
        r += p[0];
        g += p[1];
        b += p[2];
        blue += p[2] >= 160 && p[2] >= p[0] && p[2] >= p[1] ? 1 : 0;
        ++samples;
    }
    r /= static_cast<double>(samples);
    g /= static_cast<double>(samples);
    b /= static_cast<double>(samples);
    return b > 180.0 && std::abs(r - 128.0) < 40.0 && std::abs(g - 128.0) < 40.0 &&
           static_cast<double>(blue) > 0.85 * static_cast<double>(samples);
}

// Un nivel RGBA8 a bloques BC1 (8 bytes) o BC7 (16), repartiendo las filas
// de bloques entre hilos.
void encodeLevel(const std::uint8_t* pixels, std::uint32_t w, std::uint32_t h, std::uint8_t* out,
                 const bc7enc_compress_block_params& params, unsigned threads, bool bc1) {
    const std::size_t block_bytes = bc1 ? 8 : 16;
    const std::uint32_t bx = std::max((w + 3) / 4, 1u);
    const std::uint32_t by = std::max((h + 3) / 4, 1u);
    std::atomic<std::uint32_t> next_row{0};
    const auto worker = [&] {
        std::uint8_t block[16 * 4];
        for (std::uint32_t row = next_row++; row < by; row = next_row++) {
            for (std::uint32_t col = 0; col < bx; ++col) {
                // 4x4 texeles (los del borde se repiten si el lado no es
                // multiplo de 4).
                for (std::uint32_t j = 0; j < 4; ++j) {
                    const std::uint32_t sy = std::min(row * 4 + j, h - 1);
                    for (std::uint32_t i = 0; i < 4; ++i) {
                        const std::uint32_t sx = std::min(col * 4 + i, w - 1);
                        std::memcpy(block + (j * 4 + i) * 4, pixels + (static_cast<std::size_t>(sy) * w + sx) * 4, 4);
                    }
                }
                std::uint8_t* dest = out + (static_cast<std::size_t>(row) * bx + col) * block_bytes;
                if (bc1) {
                    stb_compress_dxt_block(dest, block, 0, STB_DXT_HIGHQUAL);
                } else {
                    bc7enc_compress_block(dest, block, &params);
                }
            }
        }
    };
    threads = std::clamp(threads, 1u, by);
    if (threads == 1) {
        worker();
        return;
    }
    std::vector<std::thread> pool;
    pool.reserve(threads - 1);
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(worker);
    worker();
    for (std::thread& thread : pool) thread.join();
}

}  // namespace

void setTextureCacheFolder(const std::filesystem::path& folder) {
    if (!folder.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
    }
    const std::lock_guard<std::mutex> lock(g_folder_mutex);
    g_folder = folder;
}

std::filesystem::path textureCacheFolder() {
    const std::lock_guard<std::mutex> lock(g_folder_mutex);
    return g_folder;
}

bool textureCompressionEnabled() {
    const std::lock_guard<std::mutex> lock(g_folder_mutex);
    return !g_folder.empty();
}

void setTexturePortableKeys(const std::filesystem::path& assets_root) {
    const std::lock_guard<std::mutex> lock(g_folder_mutex);
    g_portable_root = assets_root;
}

std::filesystem::path texturePortableRoot() {
    const std::lock_guard<std::mutex> lock(g_folder_mutex);
    return g_portable_root;
}

bool textureCacheWritable() {
    const std::lock_guard<std::mutex> lock(g_folder_mutex);
    return !g_folder.empty() && g_portable_root.empty();
}

std::uint64_t textureKeyWithUsage(std::uint64_t key, TextureUsage usage) {
    if (key == 0 || usage == TextureUsage::Unknown) return key;
    return mix(key ^ (0x55a6e0000ULL + static_cast<std::uint64_t>(usage))) | 1u;
}

std::uint64_t textureCacheKey(const std::uint8_t* data, std::size_t size, bool height_map, TextureUsage usage) {
    // 8 bytes por paso (un archivo de 20 MB en unos milisegundos).
    std::uint64_t h = mix(0x9e3779b97f4a7c15ULL ^ size ^ (kEncoderVersion << 56));
    std::size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        std::uint64_t v = 0;
        std::memcpy(&v, data + i, 8);
        h = mix(h ^ v) + 0x9e3779b97f4a7c15ULL;
    }
    std::uint64_t tail = 0;
    if (i < size) std::memcpy(&tail, data + i, size - i);
    h = mix(h ^ tail ^ (height_map ? 0xa5a5ULL : 0ULL));
    return textureKeyWithUsage(h, usage);
}

std::uint64_t texturePortableFileKey(const std::filesystem::path& file, const std::filesystem::path& assets_root,
                                     bool height_map, TextureUsage usage) {
    // La ruta dentro de Assets con '/' y en minusculas (Windows no distingue).
    std::error_code ec;
    std::filesystem::path relative = std::filesystem::relative(file, assets_root, ec);
    if (ec || relative.empty()) relative = file.lexically_relative(assets_root);
    if (relative.empty()) relative = file.filename();
    std::u8string text = u8"portable:" + relative.lexically_normal().generic_u8string();
    std::transform(text.begin(), text.end(), text.begin(),
                   [](char8_t c) { return c >= u8'A' && c <= u8'Z' ? static_cast<char8_t>(c - u8'A' + u8'a') : c; });
    const std::uint64_t h = textureCacheKey(reinterpret_cast<const std::uint8_t*>(text.data()), text.size(), height_map);
    return textureKeyWithUsage(h ^ 0x9047ULL, usage);
}

std::uint64_t textureFileKey(const std::filesystem::path& file, bool height_map, TextureUsage usage) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec) return 0;
    const auto time = std::filesystem::last_write_time(file, ec);
    if (ec) return 0;
    // La misma imagen escrita de dos formas (barras, mayusculas: Windows no
    // las distingue) es la misma clave.
    std::filesystem::path normal = std::filesystem::weakly_canonical(file, ec);
    if (ec) normal = file.lexically_normal();
    std::u8string text = normal.generic_u8string();
    std::transform(text.begin(), text.end(), text.begin(),
                   [](char8_t c) { return c >= u8'A' && c <= u8'Z' ? static_cast<char8_t>(c - u8'A' + u8'a') : c; });
    std::uint64_t h = textureCacheKey(reinterpret_cast<const std::uint8_t*>(text.data()), text.size(), height_map);
    h = mix(h ^ static_cast<std::uint64_t>(size));
    h = mix(h ^ static_cast<std::uint64_t>(time.time_since_epoch().count()));
    return textureKeyWithUsage(h ^ 0xf11eULL, usage);
}

std::uint64_t lazyTextureKey(const TextureData& lazy) { return lazyTextureKey(lazy, texturePortableRoot()); }

std::uint64_t lazyTextureKey(const TextureData& lazy, const std::filesystem::path& portable_root) {
    if (!isLazyTexture(lazy)) return 0;
    if (const std::uint64_t key = resolveTextureKey(lazy.source_path, portable_root); key != 0) {
        return textureKeyWithUsage(key, lazy.usage);
    }
    const std::string& source = lazy.source_path;
    std::filesystem::path path(std::u8string(source.begin(), source.end()));
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) path = std::filesystem::path(source);
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".dds") return 0;  // ya viene comprimida
    return !portable_root.empty() ? texturePortableFileKey(path, portable_root, lazy.height_map, lazy.usage)
                                  : textureFileKey(path, lazy.height_map, lazy.usage);
}

bool isTextureCached(std::uint64_t key) {
    const std::filesystem::path path = cachePath(key);
    std::error_code ec;
    return !path.empty() && std::filesystem::exists(path, ec);
}

std::filesystem::path cachedTexturePath(std::uint64_t key) { return cachePath(key); }

bool prepareLazyTexture(const TextureData& lazy) {
    if (!textureCacheWritable()) return true;
    const std::uint64_t key = lazyTextureKey(lazy);
    if (key == 0 || isTextureCached(key)) return true;
    TextureData scratch;
    resolveLazyTexture(lazy, scratch);  // la comprime y la guarda
    return false;
}

bool loadCachedTexture(std::uint64_t key, TextureData& out) {
    const std::filesystem::path path = cachePath(key);
    if (path.empty()) return false;
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamsize size = in.tellg();
    if (size <= 148) return false;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) return false;
    TextureData texture;
    texture.name = out.name;
    texture.source_path = out.source_path;
    texture.height_map = out.height_map;
    if (!parseDds(bytes.data(), bytes.size(), texture) || texture.format == TextureFormat::Rgba8) return false;
    // miscFlags2 del DX10 (4 + 124 + 16): opaca o con alfa.
    std::uint32_t alpha_mode = 0;
    std::memcpy(&alpha_mode, bytes.data() + 4 + 124 + 16, 4);
    texture.alpha = alpha_mode == kAlphaModeOpaque ? 0 : 1;
    out = std::move(texture);
    return true;
}

bool compressTexture(TextureData& texture, std::uint64_t key) {
    if (texture.format != TextureFormat::Rgba8 || texture.width == 0 || texture.height == 0 ||
        texture.pixels.size() < static_cast<std::size_t>(texture.width) * texture.height * 4) {
        return false;
    }
    static std::once_flag init;
    std::call_once(init, [] { bc7enc_compress_block_init(); });

    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    // Pesos lineales: tambien sirven a los normal maps y a los mapas de datos
    // (rugosidad, metal, oclusion), que no son color.
    bc7enc_compress_block_params_init_linear_weights(&params);
    // 16 de las 64 particiones del modo 1: un tercio menos de tiempo (una de
    // 4K, ~0.5 s en 16 nucleos) por una diferencia que no se ve.
    params.m_max_partitions = 16;

    // Recortes por alfa (como hasAlpha): texeles por debajo de la mitad. Y si
    // es opaca del todo (BC1 no guarda alfa).
    bool alpha = false;
    bool opaque = true;
    for (std::size_t i = 3; i < texture.pixels.size(); i += 4) {
        alpha = alpha || texture.pixels[i] < 128;
        opaque = opaque && texture.pixels[i] >= 250;
        if (alpha && !opaque) break;
    }
    // El formato por su uso: color y datos opacos a BC1; el resto, BC7.
    const bool normal_map = texture.usage == TextureUsage::Normal || texture.height_map ||
                            (texture.usage == TextureUsage::Unknown && looksLikeNormalMap(texture.pixels));
    const bool bc1 = opaque && !normal_map &&
                     (texture.usage == TextureUsage::Color || texture.usage == TextureUsage::Data);
    const TextureFormat format = bc1 ? TextureFormat::Bc1 : TextureFormat::Bc7;
    const MipFilter filter = normal_map ? MipFilter::Normal
                             : texture.usage == TextureUsage::Color ? MipFilter::Srgb
                                                                    : MipFilter::Plain;

    const unsigned running = ++g_running;
    const unsigned cores = std::max(std::thread::hardware_concurrency(), 1u);
    const unsigned threads = std::max(cores / std::max(running, 1u), 1u);

    std::uint32_t w = texture.width;
    std::uint32_t h = texture.height;
    std::vector<std::uint8_t> level = std::move(texture.pixels);
    std::vector<std::uint8_t> blocks;
    std::uint32_t mips = 0;
    for (;;) {
        const std::size_t at = blocks.size();
        blocks.resize(at + mipByteSize(format, w, h));
        encodeLevel(level.data(), w, h, blocks.data() + at, params, threads, bc1);
        ++mips;
        if (w == 1 && h == 1) break;
        std::uint32_t nw = 0, nh = 0;
        level = halve(level, w, h, nw, nh, filter);
        w = nw;
        h = nh;
    }
    --g_running;

    texture.pixels = std::move(blocks);
    texture.format = format;
    texture.mip_levels = mips;
    texture.alpha = alpha ? 1 : 0;

    // A la cache (a un temporal y luego el nombre: otro hilo puede estar
    // escribiendo la misma).
    const std::filesystem::path path = textureCacheWritable() ? cachePath(key) : std::filesystem::path();
    if (!path.empty()) {
        const std::vector<std::uint8_t> dds = writeDds(texture);
        std::ostringstream tmp_name;
        tmp_name << path.filename().string() << "." << std::this_thread::get_id() << ".tmp";
        const std::filesystem::path tmp = path.parent_path() / tmp_name.str();
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(dds.data()), static_cast<std::streamsize>(dds.size()));
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec) std::filesystem::remove(tmp, ec);
    }
    return true;
}

bool beginTextureCompression(std::uint64_t key) {
    std::unique_lock<std::mutex> lock(g_busy_mutex);
    if (g_busy.insert(key).second) return true;
    g_busy_done.wait(lock, [&] { return !g_busy.contains(key); });
    return false;
}

void endTextureCompression(std::uint64_t key) {
    {
        const std::lock_guard<std::mutex> lock(g_busy_mutex);
        g_busy.erase(key);
    }
    g_busy_done.notify_all();
}

std::vector<std::uint8_t> writeDds(const TextureData& texture) {
    std::vector<std::uint8_t> out;
    out.reserve(4 + 124 + 20 + texture.pixels.size());
    out.insert(out.end(), {'D', 'D', 'S', ' '});
    // DDS_HEADER
    put32(out, 124);
    put32(out, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000);  // CAPS HEIGHT WIDTH PIXELFORMAT MIPMAPCOUNT LINEARSIZE
    put32(out, texture.height);
    put32(out, texture.width);
    put32(out, static_cast<std::uint32_t>(mipByteSize(texture.format, texture.width, texture.height)));
    put32(out, 0);  // depth
    put32(out, texture.mip_levels);
    for (int i = 0; i < 11; ++i) put32(out, 0);  // reserved1
    // DDS_PIXELFORMAT: FourCC "DX10"
    put32(out, 32);
    put32(out, 0x4);
    put32(out, 0x30315844);  // 'D','X','1','0'
    for (int i = 0; i < 5; ++i) put32(out, 0);
    put32(out, 0x1000 | 0x400000 | 0x8);  // TEXTURE MIPMAP COMPLEX
    for (int i = 0; i < 4; ++i) put32(out, 0);  // caps2..4, reserved2
    // DDS_HEADER_DXT10
    std::uint32_t dxgi = 98;  // BC7_UNORM
    switch (texture.format) {
        case TextureFormat::Bc1: dxgi = 71; break;
        case TextureFormat::Bc2: dxgi = 74; break;
        case TextureFormat::Bc3: dxgi = 77; break;
        case TextureFormat::Bc4: dxgi = 80; break;
        case TextureFormat::Bc5: dxgi = 83; break;
        default: break;
    }
    put32(out, dxgi);
    put32(out, 3);  // TEXTURE2D
    put32(out, 0);
    put32(out, 1);  // arraySize
    put32(out, texture.alpha == 0 ? kAlphaModeOpaque : kAlphaModeStraight);
    out.insert(out.end(), texture.pixels.begin(), texture.pixels.end());
    return out;
}

}  // namespace cramion::asset
