#include "CramionCore/terrain/Terrain.h"

#include "CramionCore/ecs/World.h"
#include "CramionCore/terrain/TerrainGrid.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>

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
    reflectTerrainLayers(v, layers);
}

void reflectTerrainLayers(ecs::PropertyVisitor& v, std::vector<TerrainLayer>& layers) {
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

void TerrainFlatten::reflect(ecs::PropertyVisitor& v) {
    v.field({"enabled", "Activo", "Aplana el terreno bajo el objeto (y lo rehace al moverlo en el editor)"}, enabled);
    v.field({"margin", "Margen", "Metros alrededor del AABB de sus mallas"}, margin, FloatRange{0.0f, 20.0f, 0.05f, "%.2f m"});
    v.field({"blend", "Talud", "Metros en que se mezcla con el terreno de alrededor"}, blend,
            FloatRange{0.0f, 50.0f, 0.1f, "%.1f m"});
    v.field({"ground_offset", "Altura del suelo", "Altura del terreno respecto al origen del objeto"}, ground_offset,
            FloatRange{-20.0f, 20.0f, 0.01f, "%.2f m"});
    v.field({"mesh_bottom", "Suelo en la base de la malla",
             "El terreno se pone a la altura de la base del AABB (objetos con el origen en el centro)"},
            mesh_bottom);
    v.field({"paint_layer", "Capa debajo", "-2 = la capa \"Tierra\" si la hay, -1 = no pintar, 0..7 = esa capa"},
            paint_layer, -2, kMaxLayers - 1);
}

void registerTerrainComponents() {
    ecs::ComponentRegistry::instance().registerComponent<Terrain>("Terrain", "Terreno", "Entorno");
    ecs::ComponentRegistry::instance().registerComponent<TerrainFlatten>("TerrainFlatten", "Aplanar terreno", "Entorno");
    registerTerrainGridComponent();
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

namespace {

// Bilineal sobre una rejilla res*res (la comparten TerrainData y las fotos).
float sampleGrid(const std::vector<float>& heights, std::uint32_t resolution, float u, float v) {
    if (heights.empty() || resolution < 2) return 0.0f;
    const float n = static_cast<float>(resolution - 1);
    const float x = std::clamp(u, 0.0f, 1.0f) * n;
    const float y = std::clamp(v, 0.0f, 1.0f) * n;
    const auto x0 = static_cast<std::uint32_t>(std::min(std::floor(x), n - 1.0f));
    const auto y0 = static_cast<std::uint32_t>(std::min(std::floor(y), n - 1.0f));
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);
    const auto at = [&](std::uint32_t px, std::uint32_t py) { return heights[static_cast<std::size_t>(py) * resolution + px]; };
    const float a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * tx;
    const float b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * tx;
    return a + (b - a) * ty;
}

}  // namespace

TerrainData::TerrainData()
    : heights_(std::make_shared<std::vector<float>>()),
      splat0_(std::make_shared<std::vector<std::uint8_t>>()),
      splat1_(std::make_shared<std::vector<std::uint8_t>>()) {}

// Copiar comparte los buffers (copia al escribir): las copias del deshacer
// del editor ya no copian 12 MB cada vez.
TerrainData::TerrainData(const TerrainData& other)
    : resolution_(other.resolution_),
      splat_resolution_(other.splat_resolution_),
      heights_(other.heights_),
      splat0_(other.splat0_),
      splat1_(other.splat1_),
      dirty_heights_(other.dirty_heights_),
      dirty_splat_(other.dirty_splat_),
      version_(other.version_),
      collision_version_(other.collision_version_),
      splat_version_(other.splat_version_),
      unsaved_(other.unsaved()),
      snapshot_(other.snapshot_) {}

TerrainData& TerrainData::operator=(const TerrainData& other) {
    if (this == &other) return *this;
    resolution_ = other.resolution_;
    splat_resolution_ = other.splat_resolution_;
    heights_ = other.heights_;
    splat0_ = other.splat0_;
    splat1_ = other.splat1_;
    dirty_heights_ = other.dirty_heights_;
    dirty_splat_ = other.dirty_splat_;
    version_ = other.version_;
    collision_version_ = other.collision_version_;
    splat_version_ = other.splat_version_;
    unsaved_.store(other.unsaved(), std::memory_order_release);
    snapshot_ = other.snapshot_;
    return *this;
}

TerrainData::~TerrainData() = default;

void TerrainData::create(std::uint32_t resolution, std::uint32_t splat_resolution, float initial_height) {
    resolution_ = std::max(resolution, 3u);
    splat_resolution_ = std::max(splat_resolution, 2u);
    // Buffers nuevos (nunca se escribe en uno que tenga una foto).
    heights_ = std::make_shared<std::vector<float>>(static_cast<std::size_t>(resolution_) * resolution_,
                                                    std::clamp(initial_height, 0.0f, 1.0f));
    splat0_ = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(splat_resolution_) * splat_resolution_ * 4,
                                                          std::uint8_t{0});
    splat1_ = std::make_shared<std::vector<std::uint8_t>>(splat0_->size(), std::uint8_t{0});
    for (std::size_t i = 0; i < splat0_->size(); i += 4) (*splat0_)[i] = 255;  // todo capa 0
    snapshot_.reset();
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
    // Buffers nuevos (una foto anterior se queda con lo suyo).
    auto heights = std::make_shared<std::vector<float>>(static_cast<std::size_t>(resolution) * resolution);
    auto splat0 = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(splat) * splat * 4);
    auto splat1 = std::make_shared<std::vector<std::uint8_t>>(splat0->size());
    in.read(reinterpret_cast<char*>(heights->data()), static_cast<std::streamsize>(heights->size() * sizeof(float)));
    in.read(reinterpret_cast<char*>(splat0->data()), static_cast<std::streamsize>(splat0->size()));
    in.read(reinterpret_cast<char*>(splat1->data()), static_cast<std::streamsize>(splat1->size()));
    if (!in) {
        std::cerr << "[Terreno] Archivo incompleto: " << file.string() << "\n";
        return false;
    }
    resolution_ = resolution;
    splat_resolution_ = splat;
    heights_ = std::move(heights);
    splat0_ = std::move(splat0);
    splat1_ = std::move(splat1);
    snapshot_.reset();
    markAll();
    commitCollision();
    unsaved_ = false;
    return true;
}

namespace {

bool writeTerrainFile(const std::filesystem::path& file, std::uint32_t resolution, std::uint32_t splat_resolution,
                      const std::vector<float>& heights, const std::vector<std::uint8_t>& splat0,
                      const std::vector<std::uint8_t>& splat1) {
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    // Primero a un temporal: un corte a mitad no deja el terreno roto.
    const std::filesystem::path temp = file.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(kMagic, 4);
        out.write(reinterpret_cast<const char*>(&kFormatVersion), sizeof(kFormatVersion));
        out.write(reinterpret_cast<const char*>(&resolution), sizeof(resolution));
        out.write(reinterpret_cast<const char*>(&splat_resolution), sizeof(splat_resolution));
        out.write(reinterpret_cast<const char*>(heights.data()), static_cast<std::streamsize>(heights.size() * sizeof(float)));
        out.write(reinterpret_cast<const char*>(splat0.data()), static_cast<std::streamsize>(splat0.size()));
        out.write(reinterpret_cast<const char*>(splat1.data()), static_cast<std::streamsize>(splat1.size()));
        if (!out) return false;
    }
    std::filesystem::rename(temp, file, error);
    // En Windows el rename falla si otro (el antivirus, el indexador, otra
    // lectura) tiene el archivo abierto un momento: unos reintentos cortos.
    for (int attempt = 0; error && attempt < 5; ++attempt) {
        if (attempt > 0) std::this_thread::sleep_for(std::chrono::milliseconds(10 * attempt));
        std::filesystem::remove(file, error);
        std::filesystem::rename(temp, file, error);
    }
    if (error) {
        std::filesystem::remove(temp, error);
        return false;
    }
    return true;
}

}  // namespace

bool TerrainData::save(const std::filesystem::path& file) const {
    if (!writeTerrainFile(file, resolution_, splat_resolution_, *heights_, *splat0_, *splat1_)) return false;
    unsaved_ = false;
    return true;
}

bool saveTerrainSnapshot(const TerrainSnapshot& snapshot, const std::filesystem::path& file) {
    if (!snapshot.heights || !snapshot.splat0 || !snapshot.splat1) return false;
    return writeTerrainFile(file, snapshot.resolution, snapshot.splat_resolution, *snapshot.heights, *snapshot.splat0,
                            *snapshot.splat1);
}

float TerrainData::sample(float u, float v) const { return sampleGrid(*heights_, resolution_, u, v); }

float TerrainSnapshot::sample(float u, float v) const {
    return heights ? sampleGrid(*heights, resolution, u, v) : 0.0f;
}

std::shared_ptr<const TerrainSnapshot> TerrainData::snapshot() const {
    if (std::shared_ptr<const TerrainSnapshot> cached = snapshot_.lock();
        cached && cached->version == version_ && cached->splat_version == splat_version_ &&
        cached->collision_version == collision_version_ && cached->heights == heights_ && cached->splat0 == splat0_ &&
        cached->splat1 == splat1_) {
        return cached;
    }
    auto shot = std::make_shared<TerrainSnapshot>();
    shot->version = version_;
    shot->collision_version = collision_version_;
    shot->splat_version = splat_version_;
    shot->resolution = resolution_;
    shot->splat_resolution = splat_resolution_;
    // Sin copiar: la foto comparte los buffers; el proximo heights() (no
    // const) los copia antes de escribir.
    shot->heights = heights_;
    shot->splat0 = splat0_;
    shot->splat1 = splat1_;
    std::shared_ptr<const TerrainSnapshot> out = std::move(shot);
    snapshot_ = out;
    return out;
}

std::shared_ptr<const TerrainSnapshot> TerrainData::adoptSnapshotUnshared() { return snapshot(); }

void resampleForCollision(const TerrainSnapshot& snapshot, float height_scale, std::uint32_t samples,
                          std::vector<float>& out) {
    samples = std::max<std::uint32_t>(samples, 2);
    out.resize(static_cast<std::size_t>(samples) * samples);
    if (!snapshot.heights || snapshot.resolution < 2) {
        std::fill(out.begin(), out.end(), 0.0f);
        return;
    }
    const std::vector<float>& h = *snapshot.heights;
    const float inv = 1.0f / static_cast<float>(samples - 1);
    for (std::uint32_t y = 0; y < samples; ++y) {
        for (std::uint32_t x = 0; x < samples; ++x) {
            out[static_cast<std::size_t>(y) * samples + x] =
                sampleGrid(h, snapshot.resolution, static_cast<float>(x) * inv, static_cast<float>(y) * inv) * height_scale;
        }
    }
}

std::uint8_t TerrainData::weight(int layer, std::uint32_t x, std::uint32_t y) const {
    if (layer < 0 || layer >= kMaxLayers || x >= splat_resolution_ || y >= splat_resolution_) return 0;
    const std::size_t i = (static_cast<std::size_t>(y) * splat_resolution_ + x) * 4 + static_cast<std::size_t>(layer % 4);
    return layer < 4 ? (*splat0_)[i] : (*splat1_)[i];
}

std::uint8_t* TerrainData::weightPtr(int layer, std::uint32_t x, std::uint32_t y) {
    if (layer < 0 || layer >= kMaxLayers || x >= splat_resolution_ || y >= splat_resolution_) return nullptr;
    const std::size_t i = (static_cast<std::size_t>(y) * splat_resolution_ + x) * 4 + static_cast<std::size_t>(layer % 4);
    return layer < 4 ? &own(splat0_)[i] : &own(splat1_)[i];
}

void TerrainData::markHeights(int x0, int y0, int x1, int y1) {
    dirty_heights_.add(x0, y0, x1, y1);
    ++version_;
    unsaved_ = true;
}

void TerrainData::markSplat(int x0, int y0, int x1, int y1) {
    dirty_splat_.add(x0, y0, x1, y1);
    ++splat_version_;
    unsaved_ = true;
}

void TerrainData::markAll() {
    dirty_heights_.add(0, 0, static_cast<int>(resolution_) - 1, static_cast<int>(resolution_) - 1);
    dirty_splat_.add(0, 0, static_cast<int>(splat_resolution_) - 1, static_cast<int>(splat_resolution_) - 1);
    ++version_;
    ++splat_version_;
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
        // Una escritura en otro hilo de la misma ruta: que acabe antes.
        if (const auto it = saving_.find(path); it != saving_.end()) jobs::wait(it->second.job);
        if (data->save(absolute(path))) {
            ++saved;
        } else {
            std::cerr << "[Terreno] No se pudo guardar " << path << "\n";
        }
    }
    return saved;
}

TerrainStore::~TerrainStore() {
    // Las escrituras solo usan su foto y su estado (no el almacen), pero se
    // espera a que acaben: cerrar no deja teselas a medio escribir.
    flushSaves();
}

void TerrainStore::startSave(const std::string& relative, Saving& saving, std::shared_ptr<const TerrainSnapshot> snapshot) {
    saving.state = std::make_shared<SaveState>();
    saving.version = snapshot->version;
    saving.splat_version = snapshot->splat_version;
    // Sin `this`: el job solo tiene la foto, la ruta y su estado.
    saving.job = jobs::scheduleIo(
        [snapshot = std::move(snapshot), file = absolute(relative), state = saving.state]() {
            state->ok.store(saveTerrainSnapshot(*snapshot, file), std::memory_order_release);
        },
        jobs::Priority::Background);
}

void TerrainStore::saveAsync(const std::string& relative, std::shared_ptr<TerrainData> data) {
    if (!data) return;
    data_.erase(relative);
    std::shared_ptr<const TerrainSnapshot> shot = data->snapshot();
    auto it = saving_.find(relative);
    if (it != saving_.end()) {
        // Ya se escribia esta ruta: esta foto va despues (dos a la vez
        // romperian el rename en Windows).
        it->second.data = std::move(data);
        it->second.next = std::move(shot);
        return;
    }
    Saving& saving = saving_[relative];
    saving.data = std::move(data);
    startSave(relative, saving, std::move(shot));
}

std::shared_ptr<TerrainData> TerrainStore::takeSaving(const std::string& relative) {
    const auto it = saving_.find(relative);
    if (it == saving_.end()) return nullptr;
    // El guardado sigue con su foto; los datos vuelven a quien los pide (si
    // los cambia, la copia al escribir los separa de la foto).
    return it->second.data;
}

int TerrainStore::pollSaves() {
    int finished = 0;
    for (auto it = saving_.begin(); it != saving_.end();) {
        Saving& s = it->second;
        if (s.job && !s.job->done()) {
            ++it;
            continue;
        }
        const bool ok = s.state && s.state->ok.load(std::memory_order_acquire);
        if (!ok) std::cerr << "[Terreno] No se pudo guardar la tesela " << it->first << "\n";
        ++finished;
        if (s.next) {
            std::shared_ptr<const TerrainSnapshot> next = std::move(s.next);
            s.next.reset();
            startSave(it->first, s, std::move(next));
            ++it;
            continue;
        }
        // Guardada sin cambios despues de la foto: ya no hay nada sin guardar.
        if (ok && s.data && s.data->version() == s.version && s.data->splatVersion() == s.splat_version) {
            s.data->setSaved();
        } else if (!ok && s.data && data_.count(it->first) == 0) {
            // No se pudo escribir y nadie la volvio a cargar: vuelve al
            // almacen con sus cambios (se guardara con la escena).
            data_[it->first] = s.data;
        }
        // Lo que nadie usa ya se libera en el hilo de IO (12 MB por tesela).
        if (s.data && s.data.use_count() == 1) {
            jobs::scheduleIo([dead = std::move(s.data)]() mutable { dead.reset(); }, jobs::Priority::Background);
        }
        it = saving_.erase(it);
    }
    return finished;
}

void TerrainStore::flushSaves() {
    // Puede haber guardados encadenados (next): hasta que no quede ninguno.
    for (int guard = 0; !saving_.empty() && guard < 8; ++guard) {
        for (auto& [path, s] : saving_) jobs::wait(s.job);
        pollSaves();
    }
}

}  // namespace cramion::terrain
