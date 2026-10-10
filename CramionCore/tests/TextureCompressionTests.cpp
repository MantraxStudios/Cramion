// Pruebas de la compresion de texturas (solo CramionFX, sin GPU):
//
//   - Color opaco -> BC1 (la mitad que BC7) casi igual a la original.
//   - Normal maps, texturas con alfa y las de uso desconocido -> BC7.
//   - Los mips del color se promedian en espacio lineal.
//   - Claves portables (el juego exportado): la misma con otra carpeta del
//     proyecto, y la cache de solo lectura no escribe nada nuevo.
//   - prepareExportTexture deja la textura en la cache del editor y devuelve
//     la clave que buscara el juego.

#include <CramionFX/asset/Dds.h>
#include <CramionFX/asset/ImageFile.h>
#include <CramionFX/asset/Model.h>
#include <CramionFX/asset/TextureCompression.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  OK    " : "  FALLO ") << what << "\n";
    if (!ok) ++g_failures;
}

using namespace cramion;

asset::ImageRgba8 gradient(std::uint32_t size, std::uint8_t alpha) {
    asset::ImageRgba8 image{};
    image.width = image.height = size;
    image.pixels.assign(static_cast<std::size_t>(size) * size * 4u, alpha);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            std::uint8_t* p = &image.pixels[(static_cast<std::size_t>(y) * size + x) * 4];
            p[0] = static_cast<std::uint8_t>(x * 255 / (size - 1));
            p[1] = static_cast<std::uint8_t>(y * 255 / (size - 1));
            p[2] = static_cast<std::uint8_t>((x + y) * 255 / (2 * (size - 1)));
        }
    }
    return image;
}

// Un normal map de bultos suaves (azulado, R y G alrededor de 128).
asset::ImageRgba8 bumps(std::uint32_t size) {
    asset::ImageRgba8 image{};
    image.width = image.height = size;
    image.pixels.assign(static_cast<std::size_t>(size) * size * 4u, 255);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const float nx = 0.4f * std::sin(static_cast<float>(x) * 0.2f);
            const float ny = 0.4f * std::cos(static_cast<float>(y) * 0.15f);
            const float nz = std::sqrt(std::max(0.0f, 1.0f - nx * nx - ny * ny));
            std::uint8_t* p = &image.pixels[(static_cast<std::size_t>(y) * size + x) * 4];
            p[0] = static_cast<std::uint8_t>(std::lround((nx * 0.5f + 0.5f) * 255.0f));
            p[1] = static_cast<std::uint8_t>(std::lround((ny * 0.5f + 0.5f) * 255.0f));
            p[2] = static_cast<std::uint8_t>(std::lround((nz * 0.5f + 0.5f) * 255.0f));
        }
    }
    return image;
}

double psnr(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b, bool rgb_only) {
    if (a.size() != b.size() || a.empty()) return 0.0;
    double error = 0.0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (rgb_only && i % 4 == 3) continue;
        const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        error += d * d;
        ++count;
    }
    const double mse = error / static_cast<double>(count);
    return mse > 0.0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0;
}

// Decodifica (y comprime con la cache activa) una textura de un archivo como
// la usaria un material con ese uso.
asset::TextureData load(const std::filesystem::path& file, asset::TextureUsage usage) {
    asset::ModelData model{};
    model.indices = {0, 1, 2};
    model.vertices.resize(3);
    asset::TextureData texture{};
    texture.name = file.filename().string();
    texture.source_path = file.string();
    model.textures.push_back(texture);
    asset::MaterialData material{};
    if (usage == asset::TextureUsage::Color) material.albedo_texture = 0;
    if (usage == asset::TextureUsage::Normal) material.normal_texture = 0;
    if (usage == asset::TextureUsage::Data) material.metallic_roughness_texture = 0;
    model.materials.push_back(material);
    asset::finalizeModel(model, "prueba");
    return model.textures[0];
}

std::size_t ddsCount(const std::filesystem::path& folder) {
    std::size_t n = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(folder, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        n += it->path().extension() == ".dds" ? 1 : 0;
    }
    return n;
}

}  // namespace

int main() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_texture_tests";
    std::filesystem::remove_all(root);
    const std::filesystem::path assets = root / "Proyecto" / "Assets";
    const std::filesystem::path cache = root / "Proyecto" / "Library" / "Cache" / "Textures";
    std::filesystem::create_directories(assets / "Texturas");

    const asset::ImageRgba8 color = gradient(256, 255);
    const asset::ImageRgba8 cutout = [&] {
        asset::ImageRgba8 image = gradient(256, 255);
        for (std::size_t i = 3; i < image.pixels.size(); i += 16) image.pixels[i] = 0;  // recortes
        return image;
    }();
    const asset::ImageRgba8 normal = bumps(256);
    check(asset::saveImagePng(assets / "Texturas" / "Piedra_Albedo.png", color), "escribe el albedo");
    check(asset::saveImagePng(assets / "Texturas" / "Hojas.png", cutout), "escribe la textura con recortes");
    check(asset::saveImagePng(assets / "Texturas" / "Piedra_Normal.png", normal), "escribe el normal map");

    asset::setTextureCacheFolder(cache);

    std::cout << "Formatos por uso\n";
    const asset::TextureData albedo = load(assets / "Texturas" / "Piedra_Albedo.png", asset::TextureUsage::Color);
    check(albedo.format == asset::TextureFormat::Bc1 && albedo.mip_levels == 9, "color opaco: BC1 con sus 9 mips");
    {
        std::size_t bc7 = 0;
        for (std::uint32_t w = 256;; w /= 2) {
            bc7 += asset::mipByteSize(asset::TextureFormat::Bc7, w, w);
            if (w == 1) break;
        }
        check(albedo.pixels.size() * 2 == bc7, "BC1 ocupa la mitad que BC7 (1/8 de RGBA8)");
    }
    {
        const double p = psnr(asset::decodeBlockCompressed(albedo), color.pixels, true);
        std::cout << "  BC1: PSNR " << p << " dB\n";
        check(p > 35.0, "BC1 casi igual a la original (PSNR > 35 dB)");
    }
    check(albedo.alpha == 0, "BC1 opaca: sin alfa");

    const asset::TextureData leaves = load(assets / "Texturas" / "Hojas.png", asset::TextureUsage::Color);
    check(leaves.format == asset::TextureFormat::Bc7 && leaves.alpha == 1, "color con recortes: BC7 y marcada con alfa");

    const asset::TextureData nrm = load(assets / "Texturas" / "Piedra_Normal.png", asset::TextureUsage::Normal);
    check(nrm.format == asset::TextureFormat::Bc7, "normal map: BC7");
    const asset::TextureData guessed = load(assets / "Texturas" / "Piedra_Normal.png", asset::TextureUsage::Unknown);
    check(guessed.format == asset::TextureFormat::Bc7, "uso desconocido: BC7");
    const asset::TextureData data = load(assets / "Texturas" / "Piedra_Albedo.png", asset::TextureUsage::Data);
    check(data.format == asset::TextureFormat::Bc1, "datos opacos: BC1");

    // El ultimo mip del color sale en lineal: un tablero blanco y negro da un
    // gris de ~188 (en sRGB), no 128.
    {
        asset::ImageRgba8 checker{};
        checker.width = checker.height = 64;
        checker.pixels.assign(64u * 64u * 4u, 255);
        for (std::uint32_t i = 0; i < 64u * 64u; ++i) {
            const std::uint8_t v = ((i % 64) + (i / 64)) % 2 == 0 ? 255 : 0;
            checker.pixels[i * 4] = checker.pixels[i * 4 + 1] = checker.pixels[i * 4 + 2] = v;
        }
        check(asset::saveImagePng(assets / "Texturas" / "Tablero.png", checker), "escribe el tablero");
        asset::TextureData t = load(assets / "Texturas" / "Tablero.png", asset::TextureUsage::Color);
        asset::TextureData last = t;
        // Mip de 4x4 (el bloque 3): offset de los niveles anteriores.
        std::size_t offset = 0;
        std::uint32_t w = t.width;
        for (int level = 0; level < 4; ++level) {
            offset += asset::mipByteSize(t.format, w, w);
            w /= 2;
        }
        last.width = last.height = w;
        last.mip_levels = 1;
        last.pixels.assign(t.pixels.begin() + static_cast<std::ptrdiff_t>(offset),
                           t.pixels.begin() + static_cast<std::ptrdiff_t>(offset + asset::mipByteSize(t.format, w, w)));
        const std::vector<std::uint8_t> rgba = asset::decodeBlockCompressed(last);
        const int grey = rgba.empty() ? 0 : rgba[0];
        std::cout << "  gris del mip de " << w << "x" << w << ": " << grey << "\n";
        check(grey > 170 && grey < 205, "los mips del color se promedian en lineal");
    }

    std::cout << "Exportar y claves portables\n";
    asset::TextureData lazy{};
    lazy.name = "Piedra_Albedo.png";
    lazy.source_path = (assets / "Texturas" / "Piedra_Albedo.png").string();
    lazy.usage = asset::TextureUsage::Color;
    std::filesystem::path dds;
    const std::uint64_t key = asset::prepareExportTexture(lazy, assets, dds);
    check(key != 0 && std::filesystem::exists(dds), "prepareExportTexture: clave y .dds de la cache");

    // El "juego": el proyecto copiado a otra carpeta (otras fechas) con la
    // cache del paquete en TextureCache/<clave>.dds.
    const std::filesystem::path game = root / "Juego";
    std::filesystem::create_directories(game / "Assets" / "Texturas");
    std::filesystem::create_directories(game / "TextureCache");
    std::filesystem::copy_file(assets / "Texturas" / "Piedra_Albedo.png", game / "Assets" / "Texturas" / "Piedra_Albedo.png");
    std::filesystem::copy_file(assets / "Texturas" / "Hojas.png", game / "Assets" / "Texturas" / "Hojas.png");
    char name[40];
    std::snprintf(name, sizeof(name), "%016llx.dds", static_cast<unsigned long long>(key));
    std::filesystem::copy_file(dds, game / "TextureCache" / name);

    asset::setTextureCacheFolder(game / "TextureCache");
    asset::setTexturePortableKeys(game / "Assets");
    check(!asset::textureCacheWritable(), "la cache del juego es de solo lectura");
    const asset::TextureData in_game = load(game / "Assets" / "Texturas" / "Piedra_Albedo.png", asset::TextureUsage::Color);
    check(in_game.format == asset::TextureFormat::Bc1 && in_game.pixels == albedo.pixels,
          "el juego encuentra la textura comprimida con la clave portable");
    const asset::TextureData missing = load(game / "Assets" / "Texturas" / "Hojas.png", asset::TextureUsage::Color);
    check(missing.format == asset::TextureFormat::Rgba8 && ddsCount(game / "TextureCache") == 1,
          "lo que no esta en la cache del juego se queda en RGBA8 y no se escribe nada");
    asset::setTexturePortableKeys({});
    asset::setTextureCacheFolder({});

    std::filesystem::remove_all(root);
    std::cout << (g_failures == 0 ? "TODO OK" : std::to_string(g_failures) + " FALLOS") << "\n";
    return g_failures == 0 ? 0 : 1;
}
