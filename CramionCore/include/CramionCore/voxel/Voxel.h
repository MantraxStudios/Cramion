#ifndef CRAMION_CORE_VOXEL_H
#define CRAMION_CORE_VOXEL_H

// Mundos de bloques (voxeles), como Minecraft, dibujados por el VoxelPass de
// CramionFX con toda la iluminacion del motor:
//
//   - Mundo infinito por columnas (chunks) de 16 x 16 x 256 que se generan y
//     se mallan en hilos de fondo alrededor de quien mira: biomas (llanura,
//     bosque, desierto, nieve, montana, playa, oceano), cuevas, minerales,
//     arboles y plantas. El agua es el oceano del motor (WaterBody) al nivel
//     del mar; los bloques de agua solo marcan donde se nada.
//   - Luz de cielo y de antorchas propagada por bloques (0..15), como
//     Minecraft: las cuevas son oscuras y las antorchas las alumbran.
//   - Mundos con nombre que se guardan en disco (solo los chunks cambiados y
//     los datos del juego: posicion, inventario...).
//   - Consultas para el juego: bloques, rayo, colision de una caja (el
//     jugador) y altura del suelo.
//   - Colisiones con la fisica (setPhysics): cada seccion cercana es una
//     malla estatica de Jolt con las caras expuestas de los bloques solidos
//     (fusionadas en rectangulos). Se rehace al romper o poner bloques.
//
// Texturas PBR por bloque (color, normal + altura + oclusion, material):
// se leen de Assets/<VoxelWorld::textures>/<nombre>.png, _n.png y _m.png y,
// si no estan, se generan por codigo. writeTexturePngs() las escribe para
// poder cambiarlas por fotos o por un pack.

#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/World.h"

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cramion::gfx {
class VulkanRenderer;
struct VoxelTextureLayer;
}
namespace cramion::physics {
class PhysicsSystem;
}

namespace cramion::voxel {

inline constexpr int kChunkSize = 16;
inline constexpr int kWorldHeight = 256;
inline constexpr int kSectionCount = kWorldHeight / kChunkSize;

using BlockId = std::uint8_t;

enum class BlockShape : std::uint8_t { Air, Cube, Cross, Liquid };
enum class BlockTint : std::uint8_t { None, Grass, Foliage };

struct BlockDef {
    std::string name;   // clave estable ("stone")
    std::string label;  // para el jugador ("Piedra")
    std::string top;    // texturas por cara (nombres)
    std::string bottom;
    std::string side;
    BlockShape shape = BlockShape::Cube;
    bool solid = true;         // choca
    bool opaque = true;        // tapa las caras vecinas y la luz
    bool cutout = false;       // textura con huecos (hojas, cristal, plantas)
    bool waves = false;        // se mueve con el viento
    bool replaceable = false;  // se puede poner un bloque encima sin romperlo (hierba, agua)
    std::uint8_t light = 0;    // luz que emite (0..15)
    std::uint8_t light_filter = 15;  // cuanta luz quita al pasar (0 aire, 15 opaco)
    float hardness = 1.0f;     // segundos para romperlo con la mano (<0 = irrompible)
    BlockId drop = 0;          // lo que suelta (0 = el mismo)
    BlockTint tint = BlockTint::None;
    bool placeable = true;     // aparece en el inventario creativo
};

// Los bloques integrados (el indice es su BlockId).
namespace block {
inline constexpr BlockId Air = 0, Stone = 1, Dirt = 2, Grass = 3, Cobblestone = 4, OakLog = 5, OakLeaves = 6,
                         OakPlanks = 7, Sand = 8, Gravel = 9, Water = 10, Bedrock = 11, CoalOre = 12, IronOre = 13,
                         GoldOre = 14, DiamondOre = 15, SnowyGrass = 16, Snow = 17, Sandstone = 18, Glass = 19,
                         Bricks = 20, StoneBricks = 21, Torch = 22, TallGrass = 23, RedFlower = 24, YellowFlower = 25,
                         BirchLog = 26, BirchLeaves = 27, SpruceLog = 28, SpruceLeaves = 29, Cactus = 30,
                         CraftingTable = 31, Glowstone = 32, Clay = 33, DeadBush = 34, Ice = 35;
inline constexpr BlockId Count = 36;
}  // namespace block

const std::vector<BlockDef>& blocks();
const BlockDef& blockDef(BlockId id);
// Por nombre (clave o etiqueta, sin distinguir mayusculas). Air si no existe.
BlockId blockId(std::string_view name);

// --- Texturas -------------------------------------------------------------------

// Nombres de las texturas (el indice es la capa del array de la GPU).
const std::vector<std::string>& textureNames();
int textureLayer(std::string_view name);
// Una textura PBR (tres RGBA8 de size x size): de `folder` si existe el PNG
// (se reescala) o generada por codigo.
void buildTexture(const std::string& name, std::uint32_t size, const std::filesystem::path& folder,
                  gfx::VoxelTextureLayer& out);
// Escribe las texturas generadas como PNG (color, _n y _m) en `folder`.
bool writeTexturePngs(const std::filesystem::path& folder, std::uint32_t size);
// Iconos para la interfaz: un cubo en perspectiva isometrica por bloque
// (<nombre>.png en `folder`), a partir de las texturas.
bool writeBlockIcons(const std::filesystem::path& folder, std::uint32_t size,
                     const std::filesystem::path& textures_folder = {});

// --- Componente -------------------------------------------------------------------

struct VoxelWorld {
    int seed = 20260925;
    int render_distance = 10;       // chunks alrededor de la camara
    float sea_level = 62.0f;
    std::string textures = "Voxel/Textures";  // carpeta en Assets (vacia = solo generadas)
    int texture_size = 256;         // pixeles por textura (potencia de dos)
    bool preview_in_editor = true;  // generar alrededor de la camara del editor
    int collision_distance = 4;     // chunks con colision fisica alrededor de la camara

    void reflect(ecs::PropertyVisitor& v);
};

void registerVoxelComponents();

// --- Sistema -------------------------------------------------------------------

struct BlockPos {
    int x = 0, y = 0, z = 0;
    friend bool operator==(const BlockPos&, const BlockPos&) = default;
};

struct VoxelHit {
    bool hit = false;
    BlockPos block;   // el bloque tocado
    BlockPos normal;  // cara tocada (donde iria un bloque nuevo: block + normal)
    BlockId id = block::Air;
    float distance = 0.0f;
    core::Vec3 point{};
};

struct WorldInfo {
    std::string name;
    int seed = 0;
    std::int64_t last_played = 0;  // segundos desde 1970
    std::string mode;              // lo que guardo el juego (meta "mode")
};

struct VoxelStats {
    int chunks = 0;
    int pending_generation = 0;
    int pending_meshes = 0;
    int sections = 0;
    float last_chunk_ms = 0.0f;
    float last_mesh_ms = 0.0f;
    int colliders = 0;        // secciones con malla de colision en la fisica
    int pending_colliders = 0;
};

class VoxelSystem {
public:
    VoxelSystem();
    ~VoxelSystem();
    VoxelSystem(const VoxelSystem&) = delete;
    VoxelSystem& operator=(const VoxelSystem&) = delete;

    // Texturas (Assets) y mundos guardados (una carpeta por mundo).
    void setAssetsRoot(const std::filesystem::path& root);
    void setSaveRoot(const std::filesystem::path& root);
    // Colisiones de los bloques con los Rigidbody (nulo = sin colision fisica;
    // moveBox y raycast funcionan igual).
    void setPhysics(physics::PhysicsSystem* physics);
    const std::filesystem::path& saveRoot() const;

    // Busca el VoxelWorld de la escena y empieza un mundo sin nombre (no se
    // guarda) con su semilla. Sin VoxelWorld queda apagado.
    void start(ecs::World& world);
    void stop();
    bool active() const;
    const VoxelWorld& settings() const;

    // Cada frame: carga/descarga chunks alrededor de `viewer`, recoge lo que
    // terminaron los hilos y relanza lo que cambio.
    void update(float delta_seconds, const core::Vec3& viewer);
    // Texturas y secciones al renderizador (despues de update).
    void syncRenderer(gfx::VulkanRenderer& renderer);
    // Espera a que esten listos los chunks a `radius` de `center` (pruebas,
    // aparecer en el mundo).
    void waitUntilReady(const core::Vec3& center, int radius_chunks, float timeout_seconds = 30.0f);

    // --- Mundos con nombre ---
    bool newWorld(const std::string& name, int seed);
    bool loadWorld(const std::string& name);
    bool saveWorld();  // chunks cambiados y datos del juego
    std::vector<WorldInfo> listWorlds() const;
    bool deleteWorld(const std::string& name);
    const std::string& worldName() const;  // vacio = sin nombre (no se guarda)
    int seed() const;
    // Datos del juego que se guardan con el mundo (texto).
    void setMeta(const std::string& key, const std::string& value);
    std::string meta(const std::string& key, const std::string& fallback = {}) const;

    // --- Bloques ---
    BlockId getBlock(int x, int y, int z) const;
    // false si el chunk no esta cargado o y fuera del mundo.
    bool setBlock(int x, int y, int z, BlockId id);
    int skyLight(int x, int y, int z) const;
    int blockLight(int x, int y, int z) const;
    bool isLoaded(int x, int z) const;
    // El chunk esta cargado y mallado (se puede estar en el).
    bool isReady(int x, int z) const;
    // Altura del suelo segun el generador (aunque no este cargado): el
    // primer bloque solido desde arriba sin contar arboles.
    int surfaceHeight(int x, int z) const;
    bool inWater(const core::Vec3& position) const;

    VoxelHit raycast(const core::Vec3& origin, const core::Vec3& direction, float max_distance) const;
    // Mueve una caja (centro, semiejes) `delta` contra los bloques solidos
    // (eje por eje, como Minecraft). Los chunks sin cargar cuentan como
    // solidos (no se cae del mundo mientras carga).
    struct MoveResult {
        core::Vec3 position{};
        bool on_ground = false;
        bool hit_ceiling = false;
        bool hit_wall = false;
    };
    MoveResult moveBox(const core::Vec3& center, const core::Vec3& half_extents, const core::Vec3& delta) const;
    // Hay algun bloque solido dentro de la caja.
    bool boxCollides(const core::Vec3& center, const core::Vec3& half_extents) const;

    VoxelStats stats() const;
    // Color medio (sRGB 0..1) de la textura lateral de un bloque: particulas,
    // objetos que caen, iconos sencillos.
    core::Vec3 blockColor(BlockId id) const;

    // Se llama al cambiar un bloque (el juego lo puede usar para sonidos).
    using BlockListener = std::function<void(const BlockPos&, BlockId before, BlockId after)>;
    void setBlockListener(BlockListener listener);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cramion::voxel

#endif  // CRAMION_CORE_VOXEL_H
