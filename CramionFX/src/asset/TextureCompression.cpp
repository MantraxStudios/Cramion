#include "CramionFX/asset/TextureCompression.h"

#include "CramionFX/asset/Dds.h"

#include <bc7enc.h>

#include <algorithm>
#include <atomic>
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
// dejan de valer y se rehacen.
constexpr std::uint64_t kEncoderVersion = 1;

std::mutex g_folder_mutex;
std::filesystem::path g_folder;

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

// Mitad de lado (media de 2x2) de una imagen RGBA8.
std::vector<std::uint8_t> halve(const std::vector<std::uint8_t>& src, std::uint32_t w, std::uint32_t h,
                                std::uint32_t& out_w, std::uint32_t& out_h) {
    out_w = std::max(w / 2, 1u);
    out_h = std::max(h / 2, 1u);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(out_w) * out_h * 4);
    for (std::uint32_t y = 0; y < out_h; ++y) {
        const std::uint32_t y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
        for (std::uint32_t x = 0; x < out_w; ++x) {
            const std::uint32_t x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
            for (std::size_t c = 0; c < 4; ++c) {
                const auto at = [&](std::uint32_t px, std::uint32_t py) {
                    return static_cast<std::uint32_t>(src[(static_cast<std::size_t>(py) * w + px) * 4 + c]);
                };
                out[(static_cast<std::size_t>(y) * out_w + x) * 4 + c] =
                    static_cast<std::uint8_t>((at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1) + 2) / 4);
            }
        }
    }
    return out;
}

// Un nivel RGBA8 a bloques BC7, repartiendo las filas de bloques entre hilos.
void encodeLevel(const std::uint8_t* pixels, std::uint32_t w, std::uint32_t h, std::uint8_t* out,
                 const bc7enc_compress_block_params& params, unsigned threads) {
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
                bc7enc_compress_block(out + (static_cast<std::size_t>(row) * bx + col) * 16, block, &params);
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

std::uint64_t textureCacheKey(const std::uint8_t* data, std::size_t size, bool height_map) {
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
    return h;
}

std::uint64_t textureFileKey(const std::filesystem::path& file, bool height_map) {
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
    return h ^ 0xf11eULL;
}

std::uint64_t lazyTextureKey(const TextureData& lazy) {
    if (!isLazyTexture(lazy)) return 0;
    if (const std::uint64_t key = resolveTextureKey(lazy.source_path); key != 0) return key;
    const std::string& source = lazy.source_path;
    std::filesystem::path path(std::u8string(source.begin(), source.end()));
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) path = std::filesystem::path(source);
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".dds") return 0;  // ya viene comprimida
    return textureFileKey(path, lazy.height_map);
}

bool isTextureCached(std::uint64_t key) {
    const std::filesystem::path path = cachePath(key);
    std::error_code ec;
    return !path.empty() && std::filesystem::exists(path, ec);
}

bool prepareLazyTexture(const TextureData& lazy) {
    if (!textureCompressionEnabled()) return true;
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

bool compressTextureBc7(TextureData& texture, std::uint64_t key) {
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

    // Recortes por alfa (como hasAlpha): texeles por debajo de la mitad.
    bool alpha = false;
    for (std::size_t i = 3; i < texture.pixels.size() && !alpha; i += 4) alpha = texture.pixels[i] < 128;

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
        blocks.resize(at + mipByteSize(TextureFormat::Bc7, w, h));
        encodeLevel(level.data(), w, h, blocks.data() + at, params, threads);
        ++mips;
        if (w == 1 && h == 1) break;
        std::uint32_t nw = 0, nh = 0;
        level = halve(level, w, h, nw, nh);
        w = nw;
        h = nh;
    }
    --g_running;

    texture.pixels = std::move(blocks);
    texture.format = TextureFormat::Bc7;
    texture.mip_levels = mips;
    texture.alpha = alpha ? 1 : 0;

    // A la cache (a un temporal y luego el nombre: otro hilo puede estar
    // escribiendo la misma).
    const std::filesystem::path path = cachePath(key);
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
