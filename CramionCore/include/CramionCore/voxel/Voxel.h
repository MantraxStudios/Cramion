#ifndef CRAMION_CORE_VOXEL_H
#define CRAMION_CORE_VOXEL_H

// Mundos de bloques (voxeles), como Minecraft, dibujados por el VoxelPass de
// CramionFX con toda la iluminacion del motor:
//
//   - Mundo infinito por columnas (chunks) de 16 x 16 x 256 que se generan y
//     se mallan en hilos de fondo alrededor de quien mira: biomas (llanura,
//     bosque, desierto, nieve, montana, playa, oceano), cuevas, minerales,
//     arboles y plantas. El agua son bloques (como Minecraft): el mar hasta
//     el nivel del mar, lagos a cualquier altura y la que corre al abrir un
//     hueco (niveles 1..7). Se dibuja en el pase del agua (WaterPass, malla de
//     voxeles: refraccion, reflejos, espuma, causticas y bajo el agua).
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
// Malla de un bloque con forma Cube (choca y se trata como cubo; solo cambia
// el dibujo): Full el cubo entero; Fence poste y travesanos hacia las vallas y
// bloques macizos vecinos; Lantern farol (cuerpo, tapa y cadena si cuelga o pie
// si esta apoyado). Las piezas se dibujan con las uv del mundo (la textura de
// 16 x 16 se lee en el trozo que ocupa cada pieza).
enum class BlockModel : std::uint8_t { Full, Fence, Lantern };

struct BlockDef {
    std::string name;   // clave estable ("stone")
    std::string label;  // para el jugador ("Piedra")
    std::string top;    // texturas por cara (nombres)
    std::string bottom;
    std::string side;
    // Lados cuando debajo hay el mismo bloque (puertas de 2 de alto: la mitad
    // de arriba). Vacio = `side`.
    std::string side_upper;
    BlockShape shape = BlockShape::Cube;
    BlockModel model = BlockModel::Full;
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
                         CraftingTable = 31, Glowstone = 32, Clay = 33, DeadBush = 34, Ice = 35,
                         // Generador Aventura: vegetacion del suelo, cuevas y desierto rojo.
                         Fern = 36, Bush = 37, BlueFlower = 38, Lavender = 39, WhiteFlower = 40, RedMushroom = 41,
                         BrownMushroom = 42, GlowMushroom = 43, Reeds = 44, DryGrass = 45, Moss = 46, RedSand = 47,
                         Terracotta = 48,
                         // Dimensiones (GeneratorDimensions.cpp). Abismo:
                         Basalt = 49, Obsidian = 50, Lava = 51, Magma = 52, Ash = 53, FireStem = 54, FireCap = 55,
                         EmberGrass = 56, AbyssBricks = 57,
                         // Cielo:
                         SkyGrass = 58, SkySoil = 59, SkyStone = 60, Cloud = 61, GoldenLog = 62, GoldenLeaves = 63,
                         SkyCrystal = 64, FallingWater = 65, SkyFlower = 66,
                         // Cristal:
                         GlowMoss = 67, DuskSoil = 68, DuskStone = 69, CrystalPink = 70, CrystalBlue = 71,
                         CrystalViolet = 72, CrystalGreen = 73, ShroomStem = 74, ShroomCapBlue = 75,
                         ShroomCapViolet = 76, GlowGrass = 77, CrystalSprout = 78,
                         // Estructuras (aldeas, ciudades, mazmorras; Blocks.cpp addStructureBlocks). Construccion:
                         Plaster = 79, Timber = 80, DarkPlanks = 81, SprucePlanks = 82, RoofRed = 83, RoofBlue = 84,
                         RoofGreen = 85, RoofOrange = 86, RoofPurple = 87, RoofSlate = 88, Thatch = 89, MudBricks = 90,
                         Marble = 91, MarblePillar = 92, Paving = 93, DirtPath = 94, Farmland = 95, Wheat = 96,
                         Carrots = 97, HayBale = 98, Fence = 99, Door = 100, Lantern = 101, Brazier = 102, Fire = 103,
                         // Muebles y decoracion:
                         Table = 104, Chair = 105, Bed = 106, Bookshelf = 107, Barrel = 108, Crate = 109, Chest = 110,
                         Counter = 111, Shelf = 112, Anvil = 113, Forge = 114, Oven = 115, RugRed = 116, RugBlue = 117,
                         Painting = 118, Bench = 119, Altar = 120, Throne = 121, BannerRed = 122, BannerBlue = 123,
                         ClothRed = 124, ClothBlue = 125, ClothYellow = 126, ClothWhite = 127, GoldBlock = 128,
                         WeaponRack = 129, PoolWater = 130,
                         // Mazmorras:
                         DungeonBricks = 131, MossyBricks = 132, CrackedBricks = 133, CarvedStone = 134, IronBars = 135,
                         Spikes = 136, Bones = 137, Cobweb = 138, Coffin = 139, RuneStone = 140, Candles = 141,
                         // Minerales extra (Horizonz: Recoleccion.cpp)
                         CopperOre = 142, CrystalOre = 143,
                         // Agua que corre (como Minecraft): nivel 1 (junto a la fuente) .. 7 (la punta, fina).
                         // La fuente es Water. VoxelSystem la hace fluir al abrir un hueco junto al agua.
                         WaterFlow1 = 144, WaterFlow7 = 150;
inline constexpr BlockId Count = 151;
// Agua de cualquier nivel (fuente o corriente).
inline constexpr bool isWater(BlockId id) { return id == Water || (id >= WaterFlow1 && id <= WaterFlow7); }
// 0 = fuente, 1..7 = corriente, -1 = no es agua.
inline constexpr int waterLevel(BlockId id) {
    return id == Water ? 0 : (id >= WaterFlow1 && id <= WaterFlow7 ? static_cast<int>(id - WaterFlow1) + 1 : -1);
}
inline constexpr BlockId flowingWater(int level) { return static_cast<BlockId>(WaterFlow1 + (level - 1)); }
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
// (se reescala) o generada por codigo. `flat_colors`: estilo de color liso
// (el color medio de la textura, sin relieve; las plantas conservan su silueta
// y las hojas se vuelven macizas).
void buildTexture(const std::string& name, std::uint32_t size, const std::filesystem::path& folder,
                  gfx::VoxelTextureLayer& out, bool flat_colors = false);
// Escribe las texturas generadas como PNG (color, _n y _m) en `folder`.
bool writeTexturePngs(const std::filesystem::path& folder, std::uint32_t size);
// Iconos para la interfaz: un cubo en perspectiva isometrica por bloque
// (<nombre>.png en `folder`), a partir de las texturas.
bool writeBlockIcons(const std::filesystem::path& folder, std::uint32_t size,
                     const std::filesystem::path& textures_folder = {});

// --- Componente -------------------------------------------------------------------

// Que generador hace el terreno.
enum class VoxelGenerator : int {
    Classic = 0,    // continentes, biomas, cuevas y arboles (el de siempre)
    Adventure = 1,  // + rios, mesetas, acantilados, vegetacion densa, cuevas con setas y entradas
    // Dimensiones (VoxelSystem::setDimension): no se eligen en el componente.
    Abyss = 2,    // cavernas de basalto, mares y rios de lava, columnas, hongos de fuego, fortalezas en ruinas
    Sky = 3,      // islas flotantes sobre un mar de nubes, cascadas, arboles dorados, cristales
    Crystal = 4,  // bosque de cristales luminosos y hongos gigantes, musgo brillante, lagos
};

struct VoxelWorld {
    int seed = 20260925;
    VoxelGenerator generator = VoxelGenerator::Classic;
    bool flat_colors = false;  // estilo de color liso (sin texturas: cada bloque un color con variacion)
    int render_distance = 10;       // chunks alrededor de la camara
    float sea_level = 62.0f;
    std::string textures = "Voxel/Textures";  // carpeta en Assets (vacia = solo generadas)
    int texture_size = 256;         // pixeles por textura (potencia de dos)
    bool preview_in_editor = true;  // generar alrededor de la camara del editor
    int collision_distance = 4;     // chunks con colision fisica alrededor de la camara
    // Arboles: 1 = lo de siempre; menos de 1, bosques solo en manchas con praderas
    // abiertas entre ellas (0 = sin arboles); hasta 2, mas arboles.
    float tree_density = 1.0f;
    // Claros (0..1): huecos redondos y praderas dentro de los bosques (0 = ninguno).
    float clearings = 0.0f;

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

// --- Dimensiones -------------------------------------------------------------------
// Cada mundo tiene varias dimensiones (otro generador y su propia carpeta de
// guardado dentro del mundo): "superficie" (la de siempre), "abismo",
// "cielo" y "cristal". Ver VoxelSystem::setDimension.
struct DimensionInfo {
    std::string name;       // clave estable ("abismo")
    std::string label;      // para el jugador, en ingles ("The Abyss")
    std::string generator;  // "superficie" (el del VoxelWorld), "abismo", "cielo", "cristal"
    std::string sky;        // ambiente: "day", "abyss", "skyrealm", "night"
};
const std::vector<DimensionInfo>& dimensions();

// --- Estructuras (aldeas, ciudades, mazmorras...) ----------------------------------
// Las planifica el generador por regiones (deterministas por semilla) y las
// estampa en cada chunk al generarlo. Ver Structures.h y ESTRUCTURAS.md.

// Un sitio con significado para el juego (aldeano, mercader, cofre, jefe...).
struct StructurePoint {
    std::string kind;  // "npc", "mercader", "cofre", "jefe", "spawner", "entrada", "portal", "plaza", "cama", "altar", "inicio", "trampa"
    core::Vec3 position{};   // de pie sobre el bloque (y = suelo)
    core::Vec3 direction{0.0f, 0.0f, 1.0f};  // hacia donde mira
    std::string tag;         // oficio, tienda, calidad, id del bestiario, dimension...
};

struct StructureInfo {
    std::uint64_t id = 0;  // estable (semilla + region + indice)
    std::string type;      // "aldea", "ciudad", "mazmorra", "ruina", "portal", "torre"
    std::string name;      // para el jugador (en ingles): "Oakvale", "Citadel of Ostra"...
    core::Vec3 center{};
    core::Vec3 min{};      // caja que la contiene (bloques)
    core::Vec3 max{};
    int level = 1;         // dificultad 1..
    std::uint32_t seed = 0;
    std::string dimension = "superficie";
    std::vector<StructurePoint> points;
    int room = -1;         // structureAt en mazmorras: sala que contiene el punto (o -1)
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
    // Origen flotante (ecs/FloatingOrigin.h): el mundo se desplazo -offset
    // (multiplo entero de metros). Los bloques y las partidas guardadas
    // siguen en coordenadas absolutas; la API (getBlock, raycast, moveBox...)
    // trabaja en las locales del mundo, las mismas que los Transform.
    void shiftOrigin(const core::Vec3& offset);
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
    // Agua segun el generador (aunque no este cargado): el primer bloque por
    // encima del agua de la columna (62 en el mar, mas arriba en los lagos de
    // montana), o 0 si la columna no tiene agua encima del terreno.
    int waterLevel(int x, int z) const;
    // Hay un bloque de agua (fuente o corriente) en ese punto.
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

    // --- Estructuras --- (coordenadas locales del mundo, como el resto de la API;
    // funcionan aunque los chunks no esten cargados).
    // Las que tocan la esfera (su caja).
    std::vector<StructureInfo> structuresNear(const core::Vec3& position, float radius) const;
    // La que contiene el punto (false si ninguna); en mazmorras, out.room.
    bool structureAt(const core::Vec3& position, StructureInfo& out) const;

    // --- Dimensiones ---
    // La actual ("superficie" al empezar o crear un mundo; loadWorld vuelve a
    // la que se guardo).
    const std::string& dimension() const;
    // Cambia en caliente: guarda la actual (con nombre, en disco; sin nombre,
    // en memoria hasta que se cree o cargue otro mundo), descarga sus chunks,
    // cambia de generador y de carpeta (<mundo>/dimensions/<nombre>/chunks; la
    // superficie sigue en <mundo>/chunks) y vuelve a cargar alrededor de quien
    // mira en los hilos, como siempre. `ambience`: aplica a la escena el
    // ambiente de la dimension (hora, clima, nubes, sol, niebla y el mar del
    // motor) y al volver a la superficie deja lo que habia (VoxelDimensions.cpp).
    // false si no existe o no hay mundo.
    bool setDimension(const std::string& name, bool ambience = true);
    // Un sitio seguro en la dimension actual cerca de `around` (local): los pies
    // sobre un bloque solido con dos de aire encima, sin lava al lado (en el
    // abismo, en el suelo de la caverna; en el cielo, sobre una isla). Mira lo
    // cargado o genera los chunks que falten (no hace falta que esten cargados).
    core::Vec3 spawnPoint(const core::Vec3& around) const;
    // Cerca de quien mira (o del origen si aun no hay nadie).
    core::Vec3 spawnPoint() const;

    // Se llama al cambiar un bloque (el juego lo puede usar para sonidos).
    using BlockListener = std::function<void(const BlockPos&, BlockId before, BlockId after)>;
    void setBlockListener(BlockListener listener);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cramion::voxel

#endif  // CRAMION_CORE_VOXEL_H
