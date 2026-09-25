// Prueba de las miniaturas de modelos (consola): dibuja las primitivas y
// guarda/lee el PNG. Devuelve 0 si todo va.

#include "../src/ModelPreviews.h"

#include <CramionCore/asset/AssetManager.h>
#include <CramionFX/asset/ImageFile.h>

#include <cstdio>
#include <filesystem>

using namespace cramion;

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        std::printf("  %s %s\n", ok ? "OK   " : "FALLO", what);
        if (!ok) ++failures;
    };
    for (const Uuid uuid : {assets::builtin::kCube, assets::builtin::kSphere}) {
        const auto model = assets::AssetManager::readModel(uuid, {}, "prueba");
        asset::ImageRgba8 image;
        const bool drawn = model && editor::ModelPreviews::render(*model, 128, image);
        int opaque = 0;
        int corner = image.pixels.empty() ? 1 : image.pixels[3];
        int darkest = 255;
        int brightest = 0;
        for (std::size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
            if (image.pixels[i + 3] == 255) {
                ++opaque;
                darkest = std::min<int>(darkest, image.pixels[i]);
                brightest = std::max<int>(brightest, image.pixels[i]);
            }
        }
        std::printf("  (%d px opacos, luz %d..%d)\n", opaque, darkest, brightest);
        check(drawn && opaque > 128 * 128 / 5 && opaque < 128 * 128 && corner == 0, "encuadrada, fondo transparente");
        check(brightest - darkest > 40, "sombreada (caras con luz distinta)");
        const auto png = std::filesystem::temp_directory_path() / "cramion_preview_test.png";
        asset::ImageRgba8 back;
        check(asset::saveImagePng(png, image) && asset::loadImageRgba8(png, back) && back.width == 128,
              "PNG guardado y leido");
        std::filesystem::remove(png);
    }
    std::printf("\n%d fallos\n", failures);
    return failures == 0 ? 0 : 1;
}
