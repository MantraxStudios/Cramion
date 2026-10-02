#ifndef CRAMION_CORE_TWOD_TILEMAP2D_H
#define CRAMION_CORE_TWOD_TILEMAP2D_H

// Tilemaps 2D (como los de Unity):
//
//   Tileset (.crtileset, JSON)  una imagen cortada en celdas de N x M
//                               pixeles (margen y separacion) + las Rule
//                               Tiles: tiles que eligen su dibujo segun sus
//                               8 vecinos (autotiling de bordes, esquinas...).
//   Tilemap (componente)        rejilla infinita de celdas en el plano XY
//                               de la entidad, con varias capas (fondo,
//                               suelo, detalles...). Se guarda por trozos
//                               de 16 x 16 celdas.
//   TilemapCollider2D           las celdas solidas como colliders 2D (filas
//                               unidas en rectangulos, sin costuras).
//
// Celdas: 0 = vacia; n > 0 = la celda n-1 de la imagen del tileset (de
// izquierda a derecha y de arriba abajo); -k = la Rule Tile k-1.

#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/twod/Sprite2D.h"

#include <CramionFX/core/Math.h>
#include <CramionFX/vk/SpriteGeometry.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace cramion::ecs {
class World;
class Entity;
}  // namespace cramion::ecs

namespace cramion::twod {

inline constexpr const char* kTilesetExtension = ".crtileset";
inline constexpr int kTileChunk = 16;

// Vecino de una regla: no importa, es esta misma tile, no es esta tile.
enum class TileNeighbor : int { Any = 0, This = 1, NotThis = 2 };

// Orden de los vecinos: arriba-izq, arriba, arriba-der, izq, der,
// abajo-izq, abajo, abajo-der.
struct TileRule {
    std::array<TileNeighbor, 8> neighbors{};
    std::vector<int> frames;  // celdas del tileset (varias = una al azar, estable por posicion)
};

struct RuleTile {
    std::string name = "Rule Tile";
    int default_frame = 0;  // si no se cumple ninguna regla
    bool collider = true;
    std::vector<TileRule> rules;  // en orden: gana la primera que se cumple
};

struct Tileset {
    std::string image;            // relativa a Assets/
    int tile_width = 16;          // pixeles
    int tile_height = 16;
    int margin = 0;               // pixeles alrededor de la imagen
    int spacing = 0;              // pixeles entre celdas
    SpriteFilter filter = SpriteFilter::Point;
    std::vector<int> no_collider;  // celdas que no son solidas (decoracion)
    std::vector<RuleTile> rule_tiles;

    // De la imagen (no se guardan).
    int image_width = 0;
    int image_height = 0;

    int columns() const;
    int rows() const;
    int tileCount() const { return columns() * rows(); }
    // UV (0..1) de una celda: min (arriba-izq) y max (abajo-der).
    void frameUv(int frame, core::Vec2& uv_min, core::Vec2& uv_max) const;
    bool frameSolid(int frame) const;
};

bool loadTileset(const std::filesystem::path& file, Tileset& tileset);
bool saveTileset(const std::filesystem::path& file, const Tileset& tileset, std::string* error = nullptr);
std::string tilesetToJson(const Tileset& tileset);
bool tilesetFromJson(const std::string& text, Tileset& tileset);
// Las reglas tipicas de un terreno con bordes a partir de una plantilla de
// 3x3 celdas (arriba-izq en `first_frame`, la siguiente fila a `columns`
// celdas): 4 esquinas, 4 bordes y el centro. Para empezar rapido.
RuleTile makeTerrainRuleTile(const std::string& name, int first_frame, int columns);

// Tilesets en uso, por ruta relativa a Assets (lee la imagen para su tamano).
class TilesetLibrary {
public:
    void setRoot(const std::filesystem::path& assets_root) { root_ = assets_root; }
    const std::filesystem::path& root() const { return root_; }
    const Tileset* get(const std::string& relative);
    void invalidate(const std::string& relative);
    void clear() { sets_.clear(); }

private:
    std::filesystem::path root_;
    std::map<std::string, std::unique_ptr<Tileset>> sets_;
};

// --- Datos de una capa: trozos de 16 x 16 celdas ---
struct TileChunk {
    std::array<std::int32_t, kTileChunk * kTileChunk> cells{};
    int count = 0;  // celdas no vacias
};

class TileLayerData {
public:
    std::int32_t get(int x, int y) const;
    // Devuelve si cambio.
    bool set(int x, int y, std::int32_t id);
    void clear() { chunks_.clear(); }
    bool empty() const { return chunks_.empty(); }
    std::size_t cellCount() const;
    // Limites de las celdas ocupadas (false si esta vacia).
    bool bounds(int& min_x, int& min_y, int& max_x, int& max_y) const;
    void forEach(const std::function<void(int x, int y, std::int32_t id)>& fn) const;
    const std::unordered_map<std::int64_t, TileChunk>& chunks() const { return chunks_; }

    // Texto compacto para la escena: "cx,cy=id*n,id,...;" por trozo.
    std::string encode() const;
    void decode(const std::string& text);

    static std::int64_t key(int cx, int cy) {
        return (static_cast<std::int64_t>(cx) << 32) ^ static_cast<std::int64_t>(static_cast<std::uint32_t>(cy));
    }
    static int chunkCoord(int v) { return v >= 0 ? v / kTileChunk : -((-v - 1) / kTileChunk) - 1; }

private:
    std::unordered_map<std::int64_t, TileChunk> chunks_;
};

struct TilemapLayer {
    std::string name = "Capa";
    bool visible = true;
    bool collision = true;     // sus celdas cuentan para el TilemapCollider2D
    int order = 0;             // se suma al orden en la capa del Tilemap
    core::Vec3 tint{1.0f, 1.0f, 1.0f};
    float opacity = 1.0f;
    TileLayerData data;
};

// Cache de dibujo (no se guarda): cuadrados por trozo en el espacio del
// tilemap, rehechos solo si el trozo cambio.
struct TilemapRuntime {
    struct ChunkMesh {
        std::vector<gfx::SpriteQuad> quads;  // corners en espacio local
        bool dirty = true;
    };
    std::vector<std::unordered_map<std::int64_t, ChunkMesh>> layers;
    std::uint64_t version = 0;      // sube con cada cambio de celdas
    std::uint64_t built_tileset = 0;  // huella del tileset con que se hizo
};
struct TilemapRuntimeRef {
    std::shared_ptr<TilemapRuntime> ptr;
    TilemapRuntimeRef() = default;
    TilemapRuntimeRef(const TilemapRuntimeRef&) {}
    TilemapRuntimeRef& operator=(const TilemapRuntimeRef&) { return *this; }
    TilemapRuntimeRef(TilemapRuntimeRef&&) noexcept = default;
    TilemapRuntimeRef& operator=(TilemapRuntimeRef&&) noexcept = default;
};

struct Tilemap {
    std::string tileset;               // .crtileset relativo a Assets/
    core::Vec2 cell_size{1.0f, 1.0f};  // unidades por celda
    std::string sorting_layer = "Default";
    int order_in_layer = 0;
    bool lit = false;
    std::vector<TilemapLayer> layers{TilemapLayer{}};
    TilemapRuntimeRef runtime;
    std::uint64_t version = 1;  // sube con cada cambio de celdas (colliders, dibujo)

    std::int32_t getTile(int x, int y, int layer = 0) const;
    // Pone una celda (marca los trozos afectados para redibujar, tambien los
    // vecinos por las Rule Tiles). Devuelve si cambio.
    bool setTile(int x, int y, std::int32_t id, int layer = 0);
    void markAllDirty();
    void clearLayer(int layer);
    // Celda que contiene un punto local (espacio de la entidad) y su centro.
    void cellAt(const core::Vec3& local, int& x, int& y) const;
    core::Vec3 cellCenter(int x, int y) const;

    void reflect(ecs::PropertyVisitor& v);
};

// Relleno por inundacion desde (x, y) de las celdas iguales a la de partida,
// dentro del rectangulo [min, max] (el vacio no tiene borde). Devuelve las
// celdas cambiadas (como mucho `limit`).
int floodFill(Tilemap& map, int layer, int x, int y, std::int32_t id, int min_x, int min_y, int max_x, int max_y,
              int limit = 65536);

// El dibujo de la Rule Tile en (x, y) segun sus vecinos (celda del tileset).
int resolveRuleTile(const Tileset& tileset, const TileLayerData& data, int x, int y, int rule_index);

// Rectangulos solidos (espacio local, en celdas: x, y, ancho, alto) de las
// capas con colision: filas unidas y despues filas iguales apiladas.
struct TileRect {
    int x = 0, y = 0, w = 0, h = 0;
};
std::vector<TileRect> solidRects(const Tilemap& map, const Tileset* tileset, bool merge_vertical = true);

// Rehace (si hace falta) los cuadrados de los trozos sucios. Devuelve los
// cuadrados de cada capa visible (espacio local, con su orden).
void buildTilemapMeshes(Tilemap& map, const Tileset& tileset);

struct TilemapCollider2D {
    bool is_trigger = false;
    float friction = 0.4f;
    float bounciness = 0.0f;
    bool one_way = false;        // plataformas que se atraviesan desde abajo
    bool used_by_composite = false;

    void reflect(ecs::PropertyVisitor& v);
};

}  // namespace cramion::twod

#endif  // CRAMION_CORE_TWOD_TILEMAP2D_H
