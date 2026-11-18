// Pruebas de las texturas de movil (solo CramionFX, sin GPU):
//
//   - ETC2 RGBA8: codificar y decodificar un nivel (PSNR), alfa constante
//     exacto, recortes por alfa, lados que no son multiplo de 4, los modos T,
//     H y plano del decodificador (bloques hechos a mano).
//   - ASTC 4x4 (astc-encoder): ida y vuelta con buen PSNR.
//   - convertToMobileTexture: de BC7 con mips (los mismos mips) y de RGBA8.
//   - Archivo .crtx: ida y vuelta; los rotos se rechazan.
//   - limitTextureSize con ASTC (quita los mips grandes) y decodeBlockCompressed
//     (ETC2 si, ASTC no).
//   - Exportar y cargar: prepareMobileExportTexture deja el archivo (y la
//     segunda vez sale de la cache), y con la cache de movil activa la carga
//     del modelo (finalizeModel) recibe la textura en ASTC sin decodificar.

#include <CramionFX/asset/Dds.h>
#include <CramionFX/asset/ImageFile.h>
#include <CramionFX/asset/MobileTextures.h>
#include <CramionFX/asset/Model.h>
#include <CramionFX/asset/TextureCompression.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
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

std::vector<std::uint8_t> gradient(std::uint32_t w, std::uint32_t h, bool alpha_holes) {
    std::vector<std::uint8_t> p(static_cast<std::size_t>(w) * h * 4);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t* t = &p[(static_cast<std::size_t>(y) * w + x) * 4];
            t[0] = static_cast<std::uint8_t>(x * 255 / std::max(w - 1, 1u));
            t[1] = static_cast<std::uint8_t>(y * 255 / std::max(h - 1, 1u));
            t[2] = static_cast<std::uint8_t>(128 + 100 * std::sin(static_cast<float>(x + y) * 0.05f));
            t[3] = alpha_holes && ((x / 8 + y / 8) % 2 == 0) ? 0 : 255;
        }
    }
    return p;
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

void writeBe(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (56 - 8 * i));
}

}  // namespace

int main() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_mobile_texture_tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    std::cout << "ETC2 RGBA8\n";
    {
        const std::vector<std::uint8_t> image = gradient(128, 128, false);
        const std::vector<std::uint8_t> blocks = asset::encodeEtc2Rgba8Level(image.data(), 128, 128, 4);
        check(blocks.size() == 32u * 32u * 16u, "16 bytes por bloque de 4x4");
        const std::vector<std::uint8_t> back = asset::decodeEtc2Rgba8Level(blocks.data(), blocks.size(), 128, 128);
        const double p = psnr(back, image, true);
        check(p > 30.0, "color con buen PSNR (" + std::to_string(p) + " dB)");
        bool alpha_exact = true;
        for (std::size_t i = 3; i < back.size(); i += 4) alpha_exact = alpha_exact && back[i] == 255;
        check(alpha_exact, "alfa constante exacto (EAC)");
        // Lo mismo por decodeBlockCompressed (el camino de las GPU sin ETC2).
        asset::TextureData t;
        t.width = t.height = 128;
        t.format = asset::TextureFormat::Etc2Rgba8;
        t.pixels = blocks;
        check(asset::decodeBlockCompressed(t) == back, "decodeBlockCompressed entiende ETC2");
    }
    {
        // Recortes por alfa (0 / 255) y un lado que no es multiplo de 4.
        const std::vector<std::uint8_t> image = gradient(37, 21, true);
        const std::vector<std::uint8_t> blocks = asset::encodeEtc2Rgba8Level(image.data(), 37, 21, 2);
        check(blocks.size() == 10u * 6u * 16u, "lados de 37x21: 10x6 bloques");
        const std::vector<std::uint8_t> back = asset::decodeEtc2Rgba8Level(blocks.data(), blocks.size(), 37, 21);
        bool cut = back.size() == image.size();
        for (std::size_t i = 3; cut && i < back.size(); i += 4) cut = (back[i] < 128) == (image[i] < 128);
        check(cut, "el recorte por alfa se conserva texel a texel");
    }
    {
        // Bloques a mano de los modos de ETC2 que el codificador no usa.
        std::uint8_t block[16] = {};
        std::uint8_t rgba[64];
        // Alfa: base 200, multiplicador 1, tabla 13, indice 4 (= +0) en todos.
        block[0] = 200;
        block[1] = 0x1D;
        std::uint64_t alpha_bits = 0;
        for (int i = 0; i < 16; ++i) alpha_bits |= 4ull << (45 - 3 * i);
        for (int i = 0; i < 6; ++i) block[2 + i] = static_cast<std::uint8_t>(alpha_bits >> (40 - 8 * i));
        // Plano: R+dR y G+dG validos, B+dB fuera de rango. O = (32,64,32)
        // (6/7/6 bits: RO=8, GO=32, BO=8), H y V iguales a O: color plano.
        // Campos: RO 62-57, GO 56 y 54-49, BO 48, 44-43 y 41-39, RH 38-34 y 32,
        // GH 31-25, BH 24-19, RV 18-13, GV 12-6, BV 5-0; diff (33) = 1.
        std::uint64_t bits = 0;
        const auto set = [&](int high, int count, std::uint64_t value) {
            bits |= (value & ((1ull << count) - 1)) << (high - count + 1);
        };
        set(62, 6, 8);                   // RO
        set(56, 1, 32 >> 6);             // GO alto
        set(54, 6, 32 & 63);             // GO bajo
        set(48, 1, (8 >> 5) & 1);        // BO
        set(44, 2, (8 >> 3) & 3);
        set(41, 3, 8 & 7);
        set(38, 5, 8 >> 1);              // RH
        set(32, 1, 8 & 1);
        set(31, 7, 32);                  // GH
        set(24, 6, 8);                   // BH
        set(18, 6, 8);                   // RV
        set(12, 7, 32);                  // GV
        set(5, 6, 8);                    // BV
        set(33, 1, 1);                   // diferencial
        // B (bits 47-43) + dB (42-40) fuera de rango: 31 + 3. Pisa los bits
        // de BO (solo se miran R y G, que salen planos).
        bits |= (31ull << 43) | (3ull << 40);
        writeBe(block + 8, bits);
        asset::decodeEtc2Rgba8Block(block, rgba);
        const int ro = 8 << 2 | 8 >> 4;
        const int go = 32 << 1 | 32 >> 6;
        bool flat_rg = true;
        for (int i = 0; i < 16; ++i) flat_rg = flat_rg && rgba[i * 4] == ro && rgba[i * 4 + 1] == go && rgba[i * 4 + 3] == 200;
        check(flat_rg, "modo plano: R y G planos y alfa EAC 200");
        // T: R+dR fuera de rango (R=31, dR=+3).
        // c1 = (R1a R1b, G1, B1): R1a = bits 60-59 (11), R1b = 57-56 (11) -> 15.
        const std::uint64_t t_bits = (31ull << 59) | (3ull << 56) | (1ull << 33);
        writeBe(block + 8, t_bits);
        asset::decodeEtc2Rgba8Block(block, rgba);
        // Indice 0 en todos (pixel bits a 0): el color 1 tal cual (R=255, G=0, B=0).
        bool t_ok = true;
        for (int i = 0; i < 16; ++i) t_ok = t_ok && rgba[i * 4] == 255 && rgba[i * 4 + 1] == 0 && rgba[i * 4 + 2] == 0;
        check(t_ok, "modo T: indice 0 = el primer color");
    }

    std::cout << "ASTC 4x4\n";
    if (asset::astcEncoderAvailable()) {
        const std::vector<std::uint8_t> image = gradient(64, 48, true);
        const auto start = std::chrono::steady_clock::now();
        const std::vector<std::uint8_t> blocks = asset::encodeAstc4x4Level(image.data(), 64, 48, false, 4);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        check(blocks.size() == 16u * 12u * 16u, "16 bytes por bloque de 4x4 (" + std::to_string(ms) + " ms)");
        const std::vector<std::uint8_t> back = asset::decodeAstc4x4Level(blocks.data(), blocks.size(), 64, 48);
        const double p = psnr(back, image, false);
        check(p > 35.0, "ida y vuelta con buen PSNR (" + std::to_string(p) + " dB)");
        asset::TextureData t;
        t.width = 64;
        t.height = 48;
        t.format = asset::TextureFormat::Astc4x4;
        t.pixels = blocks;
        check(asset::decodeBlockCompressed(t).empty(), "decodeBlockCompressed no decodifica ASTC (vacio)");
    } else {
        check(false, "sin compresor ASTC en este binario");
    }

    std::cout << "Conversion con mips\n";
    asset::TextureData bc7;
    {
        // Un BC7 con todos sus mips (como los de la cache del editor).
        std::vector<std::uint8_t> level = gradient(64, 64, false);
        bc7.width = bc7.height = 64;
        bc7.format = asset::TextureFormat::Bc7;
        bc7.usage = asset::TextureUsage::Color;
        bc7.mip_levels = 0;
        std::uint32_t size = 64;
        while (true) {
            const std::vector<std::uint8_t> blocks = asset::encodeBc7Level(level.data(), size, size, 2);
            bc7.pixels.insert(bc7.pixels.end(), blocks.begin(), blocks.end());
            ++bc7.mip_levels;
            if (size == 1) break;
            const std::uint32_t half = size / 2;
            std::vector<std::uint8_t> next(static_cast<std::size_t>(half) * half * 4);
            for (std::uint32_t y = 0; y < half; ++y) {
                for (std::uint32_t x = 0; x < half; ++x) {
                    for (int c = 0; c < 4; ++c) {
                        next[(y * half + x) * 4 + c] = level[((y * 2) * size + x * 2) * 4 + c];
                    }
                }
            }
            level = std::move(next);
            size = half;
        }
        asset::TextureData astc;
        const bool ok = asset::convertToMobileTexture(bc7, asset::MobileFormat::Astc4x4, astc);
        check(ok && astc.format == asset::TextureFormat::Astc4x4 && astc.mip_levels == 7,
              "BC7 64x64 con 7 mips -> ASTC con 7 mips");
        std::size_t expected = 0;
        for (std::uint32_t w = 64;; w /= 2) {
            expected += asset::mipByteSize(asset::TextureFormat::Astc4x4, w, w);
            if (w == 1) break;
        }
        check(astc.pixels.size() == expected, "bytes justos para todos los mips");
        check(astc.alpha == 0, "opaca (alfa medida al convertir)");
        asset::TextureData etc;
        check(asset::convertToMobileTexture(bc7, asset::MobileFormat::Etc2Rgba8, etc) && etc.mip_levels == 7 &&
                  etc.pixels.size() == expected,
              "BC7 -> ETC2 con los mismos mips");
        // RGBA8 sin mips: se hacen.
        asset::TextureData rgba;
        rgba.width = 32;
        rgba.height = 16;
        rgba.pixels = gradient(32, 16, true);
        asset::TextureData out;
        check(asset::convertToMobileTexture(rgba, asset::MobileFormat::Etc2Rgba8, out) && out.mip_levels == 6 &&
                  out.alpha == 1,
              "RGBA8 32x16 -> ETC2 con 6 mips y recortes");

        std::cout << "Archivo .crtx\n";
        const std::vector<std::uint8_t> file = asset::writeMobileTextureFile(astc);
        asset::TextureData read;
        check(asset::readMobileTextureFile(file.data(), file.size(), read) && read.pixels == astc.pixels &&
                  read.width == 64 && read.mip_levels == 7 && read.format == asset::TextureFormat::Astc4x4,
              "ida y vuelta");
        std::vector<std::uint8_t> broken = file;
        broken.resize(broken.size() - 16);
        check(!asset::readMobileTextureFile(broken.data(), broken.size(), read), "cortado: se rechaza");
        broken = file;
        broken[20] = 9;  // dice 9 mips
        check(!asset::readMobileTextureFile(broken.data(), broken.size(), read), "mips que no cuadran: se rechaza");
        broken = file;
        broken[0] = 'X';
        check(!asset::readMobileTextureFile(broken.data(), broken.size(), read), "sin firma: se rechaza");
    }

    std::cout << "Exportar y cargar\n";
    {
        const std::filesystem::path assets = root / "Proyecto" / "Assets";
        const std::filesystem::path editor_cache = root / "Proyecto" / "Library" / "Cache" / "Textures";
        const std::filesystem::path mobile_cache = root / "Proyecto" / "Library" / "Cache" / "MobileTextures";
        const std::filesystem::path package = root / "Paquete" / "TextureCache";
        std::filesystem::create_directories(assets);
        std::filesystem::create_directories(package);
        asset::ImageRgba8 image{};
        image.width = image.height = 128;
        image.pixels = gradient(128, 128, false);
        check(asset::saveImagePng(assets / "Ladrillo.png", image), "escribe la imagen");
        asset::setTextureCacheFolder(editor_cache);

        // Imagen incrustada (bytes en el .crdata) y textura de su archivo (perezosa).
        asset::TextureData embedded;
        embedded.name = "Ladrillo";
        embedded.usage = asset::TextureUsage::Color;
        {
            std::ifstream in(assets / "Ladrillo.png", std::ios::binary);
            embedded.encoded.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        asset::TextureData lazy;
        lazy.name = "Ladrillo";
        lazy.usage = asset::TextureUsage::Color;
        lazy.source_path = (assets / "Ladrillo.png").string();

        for (const asset::TextureData* texture : {&embedded, &lazy}) {
            const char* what = texture == &embedded ? "incrustada" : "de archivo";
            std::filesystem::path file;
            const auto start = std::chrono::steady_clock::now();
            const std::uint64_t key =
                asset::prepareMobileExportTexture(*texture, assets, asset::MobileFormat::Astc4x4, mobile_cache, file);
            const double first = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            check(key != 0 && std::filesystem::exists(file), std::string(what) + ": convertida (" + std::to_string(first) + " ms)");
            check(key == asset::mobileTextureKey(*texture, assets), std::string(what) + ": la clave es la que buscara el juego");
            std::filesystem::path again;
            const std::uint64_t key2 =
                asset::prepareMobileExportTexture(*texture, assets, asset::MobileFormat::Astc4x4, mobile_cache, again);
            check(key2 == key && again == file, std::string(what) + ": la segunda vez sale de la cache");
            char name[48];
            std::snprintf(name, sizeof(name), "%016llx.astc", static_cast<unsigned long long>(key));
            std::filesystem::copy_file(file, package / name, std::filesystem::copy_options::overwrite_existing);
        }
        // Pixeles BC que ya trae el .crdata.
        {
            std::filesystem::path file;
            const std::uint64_t key =
                asset::prepareMobileExportTexture(bc7, assets, asset::MobileFormat::Etc2Rgba8, mobile_cache, file);
            check(key != 0 && key == asset::mobileTextureKey(bc7, assets) && std::filesystem::exists(file),
                  "BC7 del .crdata -> ETC2 con su clave");
            char name[48];
            std::snprintf(name, sizeof(name), "%016llx.etc2", static_cast<unsigned long long>(key));
            std::filesystem::copy_file(file, package / name, std::filesystem::copy_options::overwrite_existing);
        }

        // El juego en el movil: sin cache de BC (decodifica a RGBA8) y con la de movil.
        asset::setTextureCacheFolder({});
        asset::setDecodeBlockCompressed(true);
        {
            asset::ModelData model{};
            model.indices = {0, 1, 2};
            model.vertices.resize(3);
            model.textures.push_back(embedded);
            model.textures.push_back(bc7);
            asset::MaterialData material{};
            material.albedo_texture = 0;
            model.materials.push_back(material);
            asset::finalizeModel(model, "sin cache de movil");
            check(model.textures[0].format == asset::TextureFormat::Rgba8, "sin la cache de movil: RGBA8 como antes");
        }
        asset::setMobileTextureCache(package, assets, true, false);
        {
            asset::ModelData model{};
            model.indices = {0, 1, 2};
            model.vertices.resize(3);
            model.textures.push_back(embedded);
            model.textures.push_back(bc7);
            asset::MaterialData material{};
            material.albedo_texture = 0;
            model.materials.push_back(material);
            asset::finalizeModel(model, "con cache de movil (solo ASTC)");
            check(model.textures[0].format == asset::TextureFormat::Astc4x4 && model.textures[0].mip_levels == 8,
                  "incrustada: llega en ASTC con sus 8 mips (sin decodificar)");
            check(model.textures[1].format != asset::TextureFormat::Etc2Rgba8,
                  "la de ETC2 no se usa si la GPU no la lee");
            asset::TextureData resolved;
            check(asset::resolveLazyTexture(lazy, resolved) && resolved.format == asset::TextureFormat::Astc4x4,
                  "de archivo (perezosa): llega en ASTC");
        }
        asset::setMobileTextureCache(package, assets, false, true);
        asset::setMaxTextureSize(16);
        {
            asset::ModelData model{};
            model.indices = {0, 1, 2};
            model.vertices.resize(3);
            model.textures.push_back(bc7);
            model.materials.push_back(asset::MaterialData{});
            asset::finalizeModel(model, "con cache de movil (ETC2)");
            const asset::TextureData& t = model.textures[0];
            check(t.format == asset::TextureFormat::Etc2Rgba8 && t.width == 16 && t.mip_levels == 5 &&
                      t.pixels.size() == asset::mipByteSize(t.format, 16, 16) + asset::mipByteSize(t.format, 8, 8) +
                                             asset::mipByteSize(t.format, 4, 4) + asset::mipByteSize(t.format, 2, 2) +
                                             asset::mipByteSize(t.format, 1, 1),
                  "BC7 del .crdata en ETC2 y con el tamano maximo (16): quita los mips grandes");
        }
        std::uint32_t loaded = 0;
        std::uint32_t looked = 0;
        asset::mobileTextureCounts(loaded, looked);
        check(loaded >= 3 && looked >= loaded, "contadores (" + std::to_string(loaded) + " de " + std::to_string(looked) + ")");
        asset::setMaxTextureSize(0);
        asset::setMobileTextureCache({}, {}, false, false);
        asset::setDecodeBlockCompressed(false);
    }

    std::filesystem::remove_all(root);
    std::cout << (g_failures == 0 ? "TODO OK\n" : "HAY FALLOS\n");
    return g_failures == 0 ? 0 : 1;
}
