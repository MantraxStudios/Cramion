#include "CramionCore/terrain/HeightmapImport.h"

#include "CramionCore/jobs/JobSystem.h"
#include "CramionCore/terrain/TerrainTools.h"

#include <CramionFX/asset/ImageFile.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>

namespace cramion::terrain {

namespace {

std::string lowerExtension(const std::filesystem::path& file) {
    const std::u8string ext8 = file.extension().u8string();
    std::string ext(ext8.begin(), ext8.end());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

bool readAll(const std::filesystem::path& file, std::vector<char>& bytes) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamsize size = in.tellg();
    if (size <= 0) return false;
    bytes.resize(static_cast<std::size_t>(size));
    in.seekg(0);
    return static_cast<bool>(in.read(bytes.data(), size));
}

std::uint32_t squareSide(std::size_t count) {
    const auto side = static_cast<std::uint32_t>(std::llround(std::sqrt(static_cast<double>(count))));
    return static_cast<std::size_t>(side) * side == count ? side : 0;
}

float cubic(float p0, float p1, float p2, float p3, float t) {
    // Catmull-Rom.
    const float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
    const float b = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
    const float c = -0.5f * p0 + 0.5f * p2;
    return ((a * t + b) * t + c) * t + p1;
}

}  // namespace

bool loadHeightmap(const std::filesystem::path& file, Heightmap& out, std::string* error) {
    out = Heightmap{};
    const std::string ext = lowerExtension(file);
    const auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (ext == ".raw" || ext == ".r16" || ext == ".r32") {
        std::vector<char> bytes;
        if (!readAll(file, bytes)) return fail("no se pudo leer el archivo");
        const bool is_float = ext == ".r32";
        const std::size_t sample = is_float ? 4 : 2;
        if (bytes.size() % sample != 0) return fail("el tamano no es de valores de " + std::to_string(sample * 8) + " bits");
        const std::uint32_t side = squareSide(bytes.size() / sample);
        if (side < 2) return fail("el RAW no es cuadrado (el lado se saca del tamano del archivo)");
        out.width = out.height = side;
        out.values.resize(static_cast<std::size_t>(side) * side);
        if (is_float) {
            float lo = 1e30f, hi = -1e30f;
            for (std::size_t i = 0; i < out.values.size(); ++i) {
                float f = 0.0f;
                std::memcpy(&f, bytes.data() + i * 4, 4);
                if (!std::isfinite(f)) f = 0.0f;
                out.values[i] = f;
                lo = std::min(lo, f);
                hi = std::max(hi, f);
            }
            if (lo < 0.0f || hi > 1.0f) {
                const float range = std::max(hi - lo, 1e-6f);
                for (float& v : out.values) v = (v - lo) / range;
                out.source_min = lo;
                out.source_max = hi;
            }
        } else {
            for (std::size_t i = 0; i < out.values.size(); ++i) {
                const auto lo = static_cast<std::uint8_t>(bytes[i * 2]);
                const auto hi = static_cast<std::uint8_t>(bytes[i * 2 + 1]);
                out.values[i] = static_cast<float>(static_cast<std::uint16_t>(lo | (hi << 8))) / 65535.0f;
            }
            out.source_max = 65535.0f;
        }
        return true;
    }
    asset::ImageGray16 image;
    if (!asset::loadImageGray16(file, image) || image.width < 2 || image.height < 2) {
        return fail("formato no reconocido (RAW/R16 de 16 bits, R32 o PNG de 16 bits)");
    }
    out.width = image.width;
    out.height = image.height;
    out.values.resize(image.pixels.size());
    for (std::size_t i = 0; i < image.pixels.size(); ++i) out.values[i] = static_cast<float>(image.pixels[i]) / 65535.0f;
    out.source_max = 65535.0f;
    return true;
}

Heightmap heightmapFromTerrain(const TerrainData& data) {
    Heightmap map;
    map.width = map.height = data.resolution();
    map.values = data.heights();
    return map;
}

float sampleHeightmap(const Heightmap& map, float u, float v) {
    if (!map.valid()) return 0.0f;
    const float x = std::clamp(u, 0.0f, 1.0f) * static_cast<float>(map.width - 1);
    const float y = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(map.height - 1);
    const int ix = std::min(static_cast<int>(x), static_cast<int>(map.width) - 2);
    const int iy = std::min(static_cast<int>(y), static_cast<int>(map.height) - 2);
    const float tx = x - static_cast<float>(ix);
    const float ty = y - static_cast<float>(iy);
    const int w = static_cast<int>(map.width), h = static_cast<int>(map.height);
    const auto at = [&](int px, int py) {
        px = std::clamp(px, 0, w - 1);
        py = std::clamp(py, 0, h - 1);
        return map.values[static_cast<std::size_t>(py) * map.width + px];
    };
    float rows[4];
    for (int r = 0; r < 4; ++r) {
        const int py = iy - 1 + r;
        rows[r] = cubic(at(ix - 1, py), at(ix, py), at(ix + 1, py), at(ix + 2, py), tx);
    }
    return std::clamp(cubic(rows[0], rows[1], rows[2], rows[3], ty), 0.0f, 1.0f);
}

void flipHeightmapZ(Heightmap& map) {
    for (std::uint32_t y = 0; y < map.height / 2; ++y) {
        float* a = map.values.data() + static_cast<std::size_t>(y) * map.width;
        float* b = map.values.data() + static_cast<std::size_t>(map.height - 1 - y) * map.width;
        std::swap_ranges(a, a + map.width, b);
    }
}

void importIntoTerrain(const Heightmap& map, TerrainData& data) {
    const std::uint32_t n = data.resolution();
    if (n < 2 || !map.valid()) return;
    std::vector<float>& h = data.heights();
    const float inv = 1.0f / static_cast<float>(n - 1);
    jobs::parallelFor(n, 16, [&](std::size_t y0, std::size_t y1) {
        for (std::size_t y = y0; y < y1; ++y) {
            for (std::uint32_t x = 0; x < n; ++x) {
                h[y * n + x] = sampleHeightmap(map, static_cast<float>(x) * inv, static_cast<float>(y) * inv);
            }
        }
    });
    data.markHeights(0, 0, static_cast<int>(n) - 1, static_cast<int>(n) - 1);  // y queda sin guardar
    data.commitCollision();
}

bool writeTerrainGrid(const Heightmap& map, const TerrainGrid& grid, const std::filesystem::path& assets_root,
                      const GridImportOptions& options, GridImportProgress* progress, std::string* error) {
    if (!map.valid()) {
        if (error != nullptr) *error = "el mapa de alturas esta vacio";
        return false;
    }
    const int tx = std::max(grid.tiles_x, 1), tz = std::max(grid.tiles_z, 1);
    const std::uint32_t n = grid.tileResolution();
    const auto splat = static_cast<std::uint32_t>(std::clamp(grid.splat_resolution, 16, 4096));
    // Vertices del mundo entero (los bordes compartidos cuentan una vez).
    const float world_x = static_cast<float>(grid.worldResolutionX() - 1);
    const float world_z = static_cast<float>(grid.worldResolutionZ() - 1);
    Terrain shape;
    applyGridSettings(grid, shape);
    if (progress != nullptr) {
        progress->total = tx * tz;
        progress->done = 0;
    }
    int written = 0;
    for (int z = 0; z < tz; ++z) {
        for (int x = 0; x < tx; ++x) {
            if (progress != nullptr && progress->cancel) {
                if (error != nullptr) *error = "cancelado";
                return false;
            }
            const std::filesystem::path file = assets_root / std::filesystem::path(tilePath(grid, x, z));
            std::error_code ec;
            if (!options.overwrite && std::filesystem::exists(file, ec)) {
                if (progress != nullptr) ++progress->done;
                continue;
            }
            TerrainData data;
            data.create(n, splat);
            std::vector<float>& h = data.heights();
            const std::size_t gx0 = static_cast<std::size_t>(x) * (n - 1);
            const std::size_t gz0 = static_cast<std::size_t>(z) * (n - 1);
            jobs::parallelFor(n, 16, [&](std::size_t r0, std::size_t r1) {
                for (std::size_t r = r0; r < r1; ++r) {
                    const float v = static_cast<float>(gz0 + r) / world_z;
                    for (std::uint32_t c = 0; c < n; ++c) {
                        h[r * n + c] = sampleHeightmap(map, static_cast<float>(gx0 + c) / world_x, v);
                    }
                }
            });
            data.markAll();
            if (options.auto_paint && options.rock_layer >= 0 && options.rock_layer < kMaxLayers) {
                paintByRules(data, shape, options.rock_layer, options.rock_slope, 90.0f, 0.0f, 1.0f);
            }
            if (!data.save(file)) {
                if (error != nullptr) *error = "no se pudo escribir " + tilePath(grid, x, z);
                return false;
            }
            ++written;
            if (progress != nullptr) ++progress->done;
        }
    }
    std::cout << "[Terreno] Rejilla " << grid.folder << ": " << written << " teselas de " << n << "x" << n << " ("
              << grid.worldSizeX() << " x " << grid.worldSizeZ() << " m)\n";
    return true;
}

}  // namespace cramion::terrain
