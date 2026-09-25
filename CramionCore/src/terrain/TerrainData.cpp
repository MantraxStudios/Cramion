#include "CramionCore/terrain/Terrain.h"

#include "CramionCore/ecs/World.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>

namespace cramion::terrain {

using ecs::FloatRange;
using ecs::Vec3Kind;

// -----------------------------------------------------------------------------
// Componente
// -----------------------------------------------------------------------------

Terrain::Terrain() {
    // Cuatro capas de partida (solo color): hierba, tierra, roca y arena.
    TerrainLayer grass;
    grass.name = "Hierba";
    grass.tint = core::Vec3{0.36f, 0.47f, 0.22f};
    TerrainLayer dirt;
    dirt.name = "Tierra";
    dirt.tint = core::Vec3{0.45f, 0.36f, 0.26f};
    TerrainLayer rock;
    rock.name = "Roca";
    rock.tint = core::Vec3{0.5f, 0.5f, 0.48f};
    rock.roughness = 0.75f;
    TerrainLayer sand;
    sand.name = "Arena";
    sand.tint = core::Vec3{0.76f, 0.69f, 0.5f};
    layers = {grass, dirt, rock, sand};
}

void Terrain::reflect(ecs::PropertyVisitor& v) {
    v.field({"data", "Datos", "Archivo .crterrain dentro de Assets (alturas y pintura)"}, data);
    v.field({"size", "Tamano", "Metros en X y en Z"}, size, FloatRange{1.0f, 16384.0f, 1.0f, "%.0f m"});
    v.field({"height", "Altura maxima"}, height, FloatRange{0.1f, 8192.0f, 0.5f, "%.1f m"});
    if (v.wantsAllFields() || v.beginGroup("Resolucion (al crear los datos)", false)) {
        v.field({"resolution", "Alturas", "Vertices por lado (129, 257, 513, 1025)"}, resolution, 33, 4097);
        v.field({"splat_resolution", "Pintura", "Texeles de los pesos de las capas por lado"}, splat_resolution, 16, 4096);
        if (!v.wantsAllFields()) v.endGroup();
    }
    v.field({"cast_shadows", "Proyecta sombras"}, cast_shadows);
    v.field({"collision", "Colision (fisica)"}, collision);
    v.field({"friction", "Friccion"}, friction, FloatRange{0.0f, 2.0f, 0.01f, "%.2f", true});
    v.field({"lod_distance", "Detalle a distancia", "Mas = mas triangulos lejos"}, lod_distance,
            FloatRange{0.5f, 8.0f, 0.05f, "%.2f", true});
    ecs::listField(v, {"layers", "Capas"}, layers, [](TerrainLayer& layer, ecs::PropertyVisitor& item) {
        item.field({"name", "Nombre"}, layer.name);
        item.field({"tint", "Color"}, layer.tint, Vec3Kind::Color);
        item.field({"albedo", "Textura", "Imagen de Assets (se arrastra desde el Proyecto)"}, layer.albedo);
        item.field({"normal", "Normal map"}, layer.normal);
        item.field({"tiling", "Repeticion", "Metros que ocupa la textura"}, layer.tiling,
                   FloatRange{0.05f, 1000.0f, 0.05f, "%.2f m"});
        item.field({"roughness", "Rugosidad"}, layer.roughness, FloatRange{0.04f, 1.0f, 0.01f, "%.2f", true});
        item.field({"metallic", "Metal"}, layer.metallic, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        item.field({"normal_strength", "Fuerza del relieve"}, layer.normal_strength,
                   FloatRange{0.0f, 4.0f, 0.01f, "%.2f"});
    });
}

void registerTerrainComponents() {
    ecs::ComponentRegistry::instance().registerComponent<Terrain>("Terrain", "Terreno", "Entorno");
}

// -----------------------------------------------------------------------------
// Datos
// -----------------------------------------------------------------------------

void DirtyRegion::add(int ax0, int ay0, int ax1, int ay1) {
    x0 = std::min(x0, ax0);
    y0 = std::min(y0, ay0);
    x1 = std::max(x1, ax1);
    y1 = std::max(y1, ay1);
}

void TerrainData::create(std::uint32_t resolution, std::uint32_t splat_resolution, float initial_height) {
    resolution_ = std::max(resolution, 3u);
    splat_resolution_ = std::max(splat_resolution, 2u);
    heights_.assign(static_cast<std::size_t>(resolution_) * resolution_, std::clamp(initial_height, 0.0f, 1.0f));
    splat0_.assign(static_cast<std::size_t>(splat_resolution_) * splat_resolution_ * 4, 0);
    splat1_.assign(splat0_.size(), 0);
    for (std::size_t i = 0; i < splat0_.size(); i += 4) splat0_[i] = 255;  // todo capa 0
    markAll();
    commitCollision();
    unsaved_ = true;
}

namespace {
constexpr char kMagic[4] = {'C', 'R', 'T', 'R'};
constexpr std::uint32_t kFormatVersion = 1;
}  // namespace

bool TerrainData::load(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    char magic[4] = {};
    std::uint32_t version = 0;
    std::uint32_t resolution = 0;
    std::uint32_t splat = 0;
    in.read(magic, 4);
    in.read(reinterpret_cast<char*>(&version), sizeof(version));
    in.read(reinterpret_cast<char*>(&resolution), sizeof(resolution));
    in.read(reinterpret_cast<char*>(&splat), sizeof(splat));
    if (!in || std::memcmp(magic, kMagic, 4) != 0 || version != kFormatVersion || resolution < 3 || resolution > 8193 ||
        splat < 2 || splat > 8192) {
        std::cerr << "[Terreno] Archivo no valido: " << file.string() << "\n";
        return false;
    }
    resolution_ = resolution;
    splat_resolution_ = splat;
    heights_.resize(static_cast<std::size_t>(resolution) * resolution);
    splat0_.resize(static_cast<std::size_t>(splat) * splat * 4);
    splat1_.resize(splat0_.size());
    in.read(reinterpret_cast<char*>(heights_.data()), static_cast<std::streamsize>(heights_.size() * sizeof(float)));
    in.read(reinterpret_cast<char*>(splat0_.data()), static_cast<std::streamsize>(splat0_.size()));
    in.read(reinterpret_cast<char*>(splat1_.data()), static_cast<std::streamsize>(splat1_.size()));
    if (!in) {
        std::cerr << "[Terreno] Archivo incompleto: " << file.string() << "\n";
        return false;
    }
    markAll();
    commitCollision();
    unsaved_ = false;
    return true;
}

bool TerrainData::save(const std::filesystem::path& file) const {
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    // Primero a un temporal: un corte a mitad no deja el terreno roto.
    const std::filesystem::path temp = file.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(kMagic, 4);
        out.write(reinterpret_cast<const char*>(&kFormatVersion), sizeof(kFormatVersion));
        out.write(reinterpret_cast<const char*>(&resolution_), sizeof(resolution_));
        out.write(reinterpret_cast<const char*>(&splat_resolution_), sizeof(splat_resolution_));
        out.write(reinterpret_cast<const char*>(heights_.data()), static_cast<std::streamsize>(heights_.size() * sizeof(float)));
        out.write(reinterpret_cast<const char*>(splat0_.data()), static_cast<std::streamsize>(splat0_.size()));
        out.write(reinterpret_cast<const char*>(splat1_.data()), static_cast<std::streamsize>(splat1_.size()));
        if (!out) return false;
    }
    std::filesystem::rename(temp, file, error);
    if (error) {
        std::filesystem::remove(file, error);
        std::filesystem::rename(temp, file, error);
    }
    if (error) return false;
    unsaved_ = false;
    return true;
}

float TerrainData::sample(float u, float v) const {
    if (heights_.empty()) return 0.0f;
    const float n = static_cast<float>(resolution_ - 1);
    const float x = std::clamp(u, 0.0f, 1.0f) * n;
    const float y = std::clamp(v, 0.0f, 1.0f) * n;
    const auto x0 = static_cast<std::uint32_t>(std::min(std::floor(x), n - 1.0f));
    const auto y0 = static_cast<std::uint32_t>(std::min(std::floor(y), n - 1.0f));
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);
    const auto at = [&](std::uint32_t px, std::uint32_t py) { return heights_[static_cast<std::size_t>(py) * resolution_ + px]; };
    const float a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * tx;
    const float b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * tx;
    return a + (b - a) * ty;
}

std::uint8_t TerrainData::weight(int layer, std::uint32_t x, std::uint32_t y) const {
    if (layer < 0 || layer >= kMaxLayers || x >= splat_resolution_ || y >= splat_resolution_) return 0;
    const std::size_t i = (static_cast<std::size_t>(y) * splat_resolution_ + x) * 4 + static_cast<std::size_t>(layer % 4);
    return layer < 4 ? splat0_[i] : splat1_[i];
}

std::uint8_t* TerrainData::weightPtr(int layer, std::uint32_t x, std::uint32_t y) {
    if (layer < 0 || layer >= kMaxLayers || x >= splat_resolution_ || y >= splat_resolution_) return nullptr;
    const std::size_t i = (static_cast<std::size_t>(y) * splat_resolution_ + x) * 4 + static_cast<std::size_t>(layer % 4);
    return layer < 4 ? &splat0_[i] : &splat1_[i];
}

void TerrainData::markHeights(int x0, int y0, int x1, int y1) {
    dirty_heights_.add(x0, y0, x1, y1);
    ++version_;
    unsaved_ = true;
}

void TerrainData::markSplat(int x0, int y0, int x1, int y1) {
    dirty_splat_.add(x0, y0, x1, y1);
    unsaved_ = true;
}

void TerrainData::markAll() {
    dirty_heights_.add(0, 0, static_cast<int>(resolution_) - 1, static_cast<int>(resolution_) - 1);
    dirty_splat_.add(0, 0, static_cast<int>(splat_resolution_) - 1, static_cast<int>(splat_resolution_) - 1);
    ++version_;
}

DirtyRegion TerrainData::takeDirtyHeights() {
    const DirtyRegion region = dirty_heights_;
    dirty_heights_.clear();
    return region;
}

DirtyRegion TerrainData::takeDirtySplat() {
    const DirtyRegion region = dirty_splat_;
    dirty_splat_.clear();
    return region;
}

// -----------------------------------------------------------------------------
// Almacen
// -----------------------------------------------------------------------------

std::shared_ptr<TerrainData> TerrainStore::get(const Terrain& terrain) {
    if (terrain.data.empty()) return nullptr;
    auto& slot = data_[terrain.data];
    if (slot) return slot;
    slot = std::make_shared<TerrainData>();
    if (!slot->load(absolute(terrain.data))) {
        // Nuevo: con la resolucion del componente (2^n + 1).
        std::uint32_t res = static_cast<std::uint32_t>(std::clamp(terrain.resolution, 33, 4097));
        std::uint32_t pow = 32;
        while (pow + 1 < res) pow *= 2;
        slot->create(pow + 1, static_cast<std::uint32_t>(std::clamp(terrain.splat_resolution, 16, 4096)));
    }
    return slot;
}

int TerrainStore::saveAll() {
    int saved = 0;
    for (const auto& [path, data] : data_) {
        if (!data || !data->unsaved()) continue;
        if (data->save(absolute(path))) {
            ++saved;
        } else {
            std::cerr << "[Terreno] No se pudo guardar " << path << "\n";
        }
    }
    return saved;
}

}  // namespace cramion::terrain
