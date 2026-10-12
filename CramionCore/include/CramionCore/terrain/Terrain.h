#ifndef CRAMION_CORE_TERRAIN_H
#define CRAMION_CORE_TERRAIN_H

// Terreno de mapa de alturas (como el Landscape de Unreal / Terrain de Unity):
//
//   Terrain       el componente: tamano, altura maxima, capas de textura y el
//                 archivo de datos (.crterrain en Assets). Su entidad marca la
//                 esquina (x, z minimas) y la altura 0; no gira ni escala.
//   TerrainData   las alturas (0..1) y los pesos de hasta 8 capas (dos mapas
//                 RGBA8), con las regiones cambiadas para subir solo eso.
//   TerrainStore  carga, crea y guarda los datos por ruta.
//
// Herramientas (TerrainTools): esculpir, suavizar, aplanar, rampa, ruido,
// erosion termica e hidraulica, terrazas, pintar capas y generar un relieve.

#include "CramionCore/ecs/Reflection.h"

#include <CramionFX/core/Math.h>

#include "CramionCore/jobs/JobSystem.h"

#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace cramion::ecs {
class Entity;
}

namespace cramion::terrain {

inline constexpr int kMaxLayers = 8;

struct TerrainLayer {
    std::string name = "Capa";
    std::string albedo;  // imagen en Assets (vacia = solo el color)
    std::string normal;  // normal map en Assets (opcional)
    core::Vec3 tint{1.0f, 1.0f, 1.0f};
    float tiling = 8.0f;  // metros por repeticion
    float roughness = 0.9f;
    float metallic = 0.0f;
    float normal_strength = 1.0f;
};

struct Terrain {
    std::string data;  // ruta del .crterrain dentro de Assets
    float size = 256.0f;
    float height = 80.0f;        // altura maxima (el 0..1 de los datos)
    int resolution = 257;        // vertices por lado al crear los datos (2^n + 1)
    int splat_resolution = 512;  // texeles de los pesos por lado
    std::vector<TerrainLayer> layers;
    bool cast_shadows = true;
    bool collision = true;
    float friction = 0.8f;
    float lod_distance = 2.0f;

    Terrain();
    void reflect(ecs::PropertyVisitor& v);
};

// Aplanar el terreno bajo un objeto (casas, edificios, lo que se apoya en el
// suelo): su huella (el AABB de sus mallas en el suelo, girado con el objeto,
// mas un margen) queda plana a la altura de su origen (o de la base de la
// malla) con un talud suave alrededor, y debajo se pinta tierra para que la
// hierba no atraviese el suelo. En el editor se rehace al soltarlo despues de
// moverlo o girarlo, y el sitio de antes vuelve a como estaba.
struct TerrainFlatten {
    bool enabled = true;
    float margin = 0.4f;         // m alrededor del AABB
    float blend = 4.0f;          // m de talud (mezcla con el terreno de alrededor)
    float ground_offset = 0.0f;  // altura del suelo respecto al origen (m)
    bool mesh_bottom = false;    // el suelo es la base del AABB (no el origen)
    int paint_layer = -2;        // capa pintada debajo: -2 = "Tierra" si existe, -1 = ninguna

    void reflect(ecs::PropertyVisitor& v);
};

// Rectangulo de texeles cambiados (inclusivo).
struct DirtyRegion {
    int x0 = INT_MAX, y0 = INT_MAX, x1 = -1, y1 = -1;
    bool valid() const { return x1 >= x0 && y1 >= y0; }
    void add(int ax0, int ay0, int ax1, int ay1);
    void clear() { *this = DirtyRegion{}; }
};

// Copia inmutable de los datos de un terreno para otros hilos (fisica,
// subida a GPU, follaje, guardar en disco). Nadie la cambia: se lee desde
// cualquier hilo sin cerrojos.
struct TerrainSnapshot {
    std::uint64_t version = 0, collision_version = 0;
    std::uint32_t resolution = 0, splat_resolution = 0;
    std::shared_ptr<const std::vector<float>> heights;               // res*res, normalized 0..1
    std::shared_ptr<const std::vector<std::uint8_t>> splat0, splat1;  // splat_res^2 * 4
    std::uint64_t splat_version = 0;  // sube con cada cambio de pintura

    // Altura 0..1 en uv (0..1), bilineal (igual que TerrainData::sample).
    float sample(float u, float v) const;
};

// Datos de un terreno. Los buffers se comparten con las TerrainSnapshot
// (copia al escribir): snapshot() no copia nada; la primera escritura despues
// de una foto copia ese buffer (una vez) y la foto se queda con lo de antes.
//
// Hilos: TerrainData es del hilo principal (o del hilo que la creo, mientras
// nadie mas la tenga). Los otros hilos solo leen TerrainSnapshot. Ojo: no
// guardar la referencia de heights() / splat0() / splat1() (no const) a traves
// de una llamada a snapshot(): la escritura iria a la foto.
class TerrainData {
public:
    TerrainData();
    TerrainData(const TerrainData& other);
    TerrainData& operator=(const TerrainData& other);
    ~TerrainData();

    void create(std::uint32_t resolution, std::uint32_t splat_resolution, float initial_height = 0.0f);
    bool load(const std::filesystem::path& file);
    bool save(const std::filesystem::path& file) const;

    std::uint32_t resolution() const { return resolution_; }
    std::uint32_t splatResolution() const { return splat_resolution_; }
    // No const: si una foto comparte el buffer, se copia antes (copia al escribir).
    std::vector<float>& heights() { return own(heights_); }
    const std::vector<float>& heights() const { return *heights_; }
    std::vector<std::uint8_t>& splat0() { return own(splat0_); }
    std::vector<std::uint8_t>& splat1() { return own(splat1_); }
    const std::vector<std::uint8_t>& splat0() const { return *splat0_; }
    const std::vector<std::uint8_t>& splat1() const { return *splat1_; }

    // SOLO HILO PRINCIPAL. Guardada por version: O(1) si no cambio nada y,
    // con la copia al escribir, O(1) tambien si cambio (no copia los datos).
    std::shared_ptr<const TerrainSnapshot> snapshot() const;
    // En el hilo que la esta creando, antes de compartirla (antes de
    // store.put): sin copias.
    std::shared_ptr<const TerrainSnapshot> adoptSnapshotUnshared();

    // Altura 0..1 en uv (0..1), bilineal.
    float sample(float u, float v) const;
    // Peso (0..255) de una capa en un texel de los pesos.
    std::uint8_t weight(int layer, std::uint32_t x, std::uint32_t y) const;
    std::uint8_t* weightPtr(int layer, std::uint32_t x, std::uint32_t y);

    // Marcar cambios: regiones para el render, versiones para el resto.
    void markHeights(int x0, int y0, int x1, int y1);
    void markSplat(int x0, int y0, int x1, int y1);
    void markAll();
    DirtyRegion takeDirtyHeights();
    DirtyRegion takeDirtySplat();
    std::uint64_t version() const { return version_; }
    // La colision (Jolt) se rehace cuando sube esta version: el editor la
    // sube al terminar cada trazo (rehacerla en cada toque seria lento).
    std::uint64_t collisionVersion() const { return collision_version_; }
    void commitCollision() { collision_version_ = version_; }
    // Sube con cada cambio de la pintura (version() solo con las alturas).
    std::uint64_t splatVersion() const { return splat_version_; }
    bool unsaved() const { return unsaved_.load(std::memory_order_acquire); }
    void setSaved() const { unsaved_.store(false, std::memory_order_release); }

private:
    template <typename T>
    std::vector<T>& own(std::shared_ptr<std::vector<T>>& buffer) {
        if (buffer.use_count() != 1) {
            buffer = std::make_shared<std::vector<T>>(*buffer);
        } else {
            // Lo que leyo otro hilo de este buffer antes de soltarlo, visto.
            std::atomic_thread_fence(std::memory_order_acquire);
        }
        snapshot_.reset();
        return *buffer;
    }

    std::uint32_t resolution_ = 0;
    std::uint32_t splat_resolution_ = 0;
    std::shared_ptr<std::vector<float>> heights_;
    std::shared_ptr<std::vector<std::uint8_t>> splat0_;
    std::shared_ptr<std::vector<std::uint8_t>> splat1_;
    DirtyRegion dirty_heights_;
    DirtyRegion dirty_splat_;
    std::uint64_t version_ = 1;
    std::uint64_t collision_version_ = 1;
    std::uint64_t splat_version_ = 1;
    mutable std::atomic<bool> unsaved_{false};
    // La ultima foto (debil: no retiene memoria si nadie la usa).
    mutable std::weak_ptr<const TerrainSnapshot> snapshot_;
};

// Escribe una foto en un .crterrain (temporal + rename, con reintentos).
// Se puede llamar desde cualquier hilo.
bool saveTerrainSnapshot(const TerrainSnapshot& snapshot, const std::filesystem::path& file);

// Las alturas para la colision (Jolt): `samples` x `samples` (2^n) cubriendo
// todo el terreno, en metros (0..1 * height_scale). Pura: cualquier hilo.
void resampleForCollision(const TerrainSnapshot& snapshot, float height_scale, std::uint32_t samples,
                          std::vector<float>& out);

class TerrainStore {
public:
    void setRoot(const std::filesystem::path& assets_root) { root_ = assets_root; }
    const std::filesystem::path& root() const { return root_; }
    std::filesystem::path absolute(const std::string& relative) const { return root_ / std::filesystem::path(relative); }

    TerrainStore() = default;
    TerrainStore(const TerrainStore&) = delete;
    TerrainStore& operator=(const TerrainStore&) = delete;
    ~TerrainStore();

    // Los datos de un Terrain (se leen, o se crean con su resolucion si el
    // archivo no existe). nullptr si la ruta esta vacia. Lee del disco en el
    // hilo que llama: para las teselas (TerrainTile) usar findOnly.
    std::shared_ptr<TerrainData> get(const Terrain& terrain);
    // Solo lo que ya esta en memoria (nunca lee ni crea). Las teselas las
    // carga el streaming en otros hilos.
    std::shared_ptr<TerrainData> findOnly(const Terrain& terrain) const { return findPath(terrain.data); }
    std::shared_ptr<TerrainData> findOnly(const std::string& relative) const { return findPath(relative); }
    // Solo si ya estan cargados (sin leer ni crear nada).
    std::shared_ptr<TerrainData> find(const Terrain& terrain) const { return findPath(terrain.data); }
    std::shared_ptr<TerrainData> findPath(const std::string& relative) const {
        const auto it = data_.find(relative);
        return it != data_.end() ? it->second : nullptr;
    }
    // Unos datos ya leidos (el streaming de teselas los lee en otro hilo).
    void put(const std::string& relative, std::shared_ptr<TerrainData> data) { data_[relative] = std::move(data); }
    // Los suelta de la memoria (quien aun los use se queda su referencia).
    void release(const std::string& relative) { data_.erase(relative); }
    std::size_t loadedCount() const { return data_.size(); }
    // Guarda los que tienen cambios. Devuelve cuantos.
    int saveAll();
    // Las escrituras en marcha siguen (ver pendingSave).
    void clear() { data_.clear(); }

    // --- Guardar en otro hilo (al soltar una tesela con cambios) ---
    // Saca los datos del almacen y los escribe en el hilo de IO (con una
    // foto: nadie los toca mientras). Hasta que termine, pendingSave(ruta) y
    // takeSaving(ruta) devuelve los datos (volver a cargarla no lee el disco
    // a medio escribir). Solo hilo principal.
    void saveAsync(const std::string& relative, std::shared_ptr<TerrainData> data);
    bool pendingSave(const std::string& relative) const { return saving_.count(relative) != 0; }
    // Los datos que se estan guardando (vuelven a la memoria; el guardado
    // sigue con su foto). nullptr si no hay.
    std::shared_ptr<TerrainData> takeSaving(const std::string& relative);
    // Mira las escrituras terminadas (cada frame). Devuelve cuantas acabaron.
    int pollSaves();
    // Espera a todas (cerrar la escena, salir).
    void flushSaves();
    std::size_t savingCount() const { return saving_.size(); }

private:
    struct SaveState {
        std::atomic<bool> ok{false};
    };
    struct Saving {
        std::shared_ptr<TerrainData> data;  // nullptr si ya volvio a la memoria
        std::shared_ptr<SaveState> state;
        jobs::JobHandle job;
        std::uint64_t version = 0, splat_version = 0;
        // Otro guardado de la misma ruta mientras este escribia: va despues.
        std::shared_ptr<const TerrainSnapshot> next;
    };
    void startSave(const std::string& relative, Saving& saving, std::shared_ptr<const TerrainSnapshot> snapshot);

    std::filesystem::path root_;
    std::unordered_map<std::string, std::shared_ptr<TerrainData>> data_;
    std::unordered_map<std::string, Saving> saving_;
};

// Registra el componente Terrain (idempotente).
void registerTerrainComponents();

// La lista de capas en el Inspector (la usan Terrain y TerrainGrid).
void reflectTerrainLayers(ecs::PropertyVisitor& v, std::vector<TerrainLayer>& layers);

}  // namespace cramion::terrain

#endif  // CRAMION_CORE_TERRAIN_H
