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
namespace cramion::scene {
struct PointLight;
}

namespace cramion::voxel {

inline constexpr int kChunkSize = 16;
inline constexpr int kWorldHeight = 256;
inline constexpr int kSectionCount = kWorldHeight / kChunkSize;

// IDS16: 16 bits (antes 8: no cabian mas de 255 bloques). Los mundos guardados
// con 8 bits se leen igual (VoxelSystem.cpp: formato de chunk version 1 y 2).
// Los chunks guardan 2 bytes por celda (VoxelInternal.h ChunkData).
using BlockId = std::uint16_t;

enum class BlockShape : std::uint8_t { Air, Cube, Cross, Liquid };
enum class BlockTint : std::uint8_t { None, Grass, Foliage };
// Malla de un bloque con forma Cube (choca y se trata como cubo; solo cambia
// el dibujo): Full el cubo entero; Fence poste y travesanos hacia las vallas y
// bloques macizos vecinos; Lantern farol (cuerpo, tapa y cadena si cuelga o pie
// si esta apoyado). Las piezas se dibujan con las uv del mundo (la textura de
// 16 x 16 se lee en el trozo que ocupa cada pieza).
// Luces (Mesher.cpp meshLightModel): Torch antorcha fina (en el suelo, o
// contra la pared si no tiene suelo y si una pared al lado); Post poste de
// farola (base abajo y remate arriba si no sigue el poste); StreetLamp cabeza
// de farola (cuello, cristal, tejadillo); Campfire troncos cruzados y llamas
// en cruz (las llamas con BlockDef::side_upper); Chandelier arana colgante
// (las velas con side_upper); StandingTorch antorcha alta con cesta.
// Door: hoja de puerta fina (4/16) en el centro de la celda, a lo largo del
// eje que tiene paredes a los lados; la mitad de arriba con side_upper. El
// juego la anima (se abre) ocultandola con VoxelSystem::setBlockHidden.
// Rieles y senales (Mesher.cpp meshRailModel; Rails.h): Rail via de hierro que
// se une sola a las vecinas (recta, curva, rampa, cruce en T o en cruz);
// Trace polvo de arcanita en el suelo (une con las vecinas y sube escalones);
// Lever palanca y Button boton (en el suelo o contra una pared, como Torch);
// Plate placa de presion; Rune runa inversora (obelisco); Relay repetidor
// (losa con dos cristales); Buffer tope de via.
enum class BlockModel : std::uint8_t { Full, Fence, Lantern, Torch, Post, StreetLamp, Campfire, Chandelier, StandingTorch, Door,
                                       Rail, Trace, Lever, Button, Plate, Rune, Relay, Buffer,
                                       // Muebles (Mesher.cpp meshFurnitureModel).
                                       Table, Chair, Bench, Brazier,
                                       // RAMAS-HZ: rama fina que se une a los troncos y ramas de alrededor (Mesher.cpp).
                                       Branch };

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
                         WaterFlow1 = 144, WaterFlow7 = 150,
                         // Tierras salvajes del Aventura (GeneratorWild.cpp): biomas peligrosos.
                         // Siempre al final: los ids de antes no cambian (los mundos guardados siguen valiendo).
                         RedSandstone = 151, PinkSand = 152, PinkSandstone = 153, RoseQuartz = 154, BlightGrass = 155,
                         BlightThorns = 156, Brimstone = 157;
// Cuevas inmensas del Aventura (GeneratorCaves.cpp; Blocks.cpp addCaveBlocks): ids 158..165 (163..165 libres).
inline constexpr BlockId Deepstone = 158,   // roca honda del fondo de las cuevas
                         Dripstone = 159,   // roca de goteo de las grutas de espeleotemas
                         Stalactite = 160,  // estalactita: cuelga del techo (planta en cruz)
                         Stalagmite = 161,  // estalagmita: sale del suelo (planta en cruz)
                         GlowVines = 162;   // enredadera luminosa: cuelga del techo
// Asentamientos y castillos (Blocks.cpp addSettlementBlocks): ids 166..180 reservados.
inline constexpr BlockId BlackstoneBricks = 166,   // castillos no muertos y torres del culto
                         CrackedBlackstone = 167,
                         BannerSkull = 168,        // estandarte negro con calavera (bandidos, orcos, no muertos)
                         BannerPurple = 169,       // estandarte del culto
                         BannerGreen = 170,        // estandarte elfico
                         HideTent = 171,           // pieles cosidas (tiendas de orcos y bandidos)
                         Palisade = 172,           // tronco afilado de empalizada
                         SoulBrazier = 173,        // brasero de fuego de alma (verde)
                         Gravestone = 174,         // lapida
                         SkullPile = 175,          // monton de calaveras (planta en cruz)
                         ElderwoodPlanks = 176;    // tablones claros de las aldeas elficas
// Luces y construccion (Blocks.cpp addLightBlocks): ids 181..190 reservados.
// (158..165 cuevas, 166..180 asentamientos/castillos: los anaden otros.)
inline constexpr BlockId LampPost = 181,     // poste de hierro de farola (fino, se apila)
                         StreetLamp = 182,   // cabeza de farola (va encima de un LampPost)
                         Campfire = 183,     // hoguera: troncos cruzados y llamas
                         CrystalLamp = 184,  // lampara de cristal (bloque entero, como Glowstone)
                         PaperLantern = 185, // farolillo de papel rojo (cuelga o se apoya, como Lantern)
                         LampRed = 186, LampBlue = 187, LampGreen = 188,  // lamparas de colores (bloque entero)
                         Chandelier = 189,   // arana de velas (cuelga del techo)
                         StandingTorch = 190;  // antorcha alta de pie (palo largo)
// Oceanos hondos (Blocks.cpp addOceanBlocks; GeneratorAdventure.cpp): ids 191..199.
// Kelp, SeaGrass, CoralFan y SeaGlow son plantas "dentro del agua" (isAquatic):
// la celda cuenta como agua para la malla del agua, nadar y la vista submarina.
inline constexpr BlockId Kelp = 191,        // alga gigante (se apila hasta casi la superficie)
                         SeaGrass = 192,    // pradera marina
                         CoralBlock = 193,  // coral rosa (bloque entero)
                         CoralTube = 194,   // coral naranja de tubos (bloque entero)
                         CoralFan = 195,    // abanico de coral (planta en cruz)
                         SeaSilt = 196,     // limo oscuro del fondo abisal
                         SeaGlow = 197;     // anemona luminosa del fondo (da luz)
// Rieles, vagonetas y senales de arcanita (Blocks.cpp addRailBlocks; Rails.h; Mesher.cpp): ids 211..230.
// Los "Lit"/"On"/"Dim" son el mismo bloque encendido o apagado: los cambia la simulacion de senales
// (VoxelSystem, solo en local: cada cliente calcula lo mismo) salvo la palanca y el boton (los cambia el
// jugador). No se colocan (placeable = false) y sueltan su version normal.
inline constexpr BlockId Rail = 211,              // via de hierro (se une sola: recta, curva, rampa, cruces)
                         SurgeRail = 212,         // via de impulso: con senal acelera las vagonetas
                         DragRail = 213,          // via de freno: frena; con senal deja pasar
                         SwitchRail = 214,        // via de cambio: en una T la senal cambia el desvio
                         SensorRail = 215,        // via detectora: da senal con una vagoneta encima
                         BufferStop = 216,        // tope de final de via
                         ArcaniteOre = 217,       // mena de arcanita (honda, brilla un poco)
                         ArcaniteTrace = 218,     // polvo de arcanita en el suelo: el "cable" de las senales
                         ArcaniteTraceLit = 219,  //   con senal (brilla)
                         RuneLever = 220,         // palanca runica (fuente; el jugador la cambia)
                         RuneLeverOn = 221,
                         RuneButton = 222,        // boton runico (pulso de 1 s)
                         RuneButtonOn = 223,
                         TriggerPlate = 224,      // placa de presion (el juego dice si hay algo encima)
                         SignalLamp = 225,        // lampara de senal (bloque entero)
                         SignalLampLit = 226,
                         NullRune = 227,          // runa nula (inversor): encendida sin entrada
                         NullRuneDim = 228,       //   apagada (con entrada)
                         EchoRelay = 229,         // repetidor de eco (retardo y senal a tope)
                         EchoRelayLit = 230;
// Reino Profundo, "The Sunless Deep" (GeneratorDeep.cpp; Blocks.cpp addDeepBlocks): ids 231..245 (238..245 libres).
inline constexpr BlockId UmbralStone = 231,        // roca oscura del Reino Profundo
                         DelverBricks = 232,       // ladrillos de los Delvers (salas antiguas; el jugador no los rompe)
                         GlyphStone = 233,         // piedra con glifos que brillan (anillos de los pozos, columnas)
                         LoreTablet = 234,         // tablilla con inscripciones (el juego la lee)
                         CrackedDelverBricks = 235,  // ladrillos agrietados: muro falso de las salas secretas (se rompen)
                         MithrilOre = 236,         // mena de mitril (solo en lo hondo del Reino Profundo)
                         Duskroot = 237;           // raiz luminosa que cuelga del techo (planta en cruz)
// Capas hondas del Reino Profundo (profundo2..profundo8; GeneratorDeep.cpp): ids 238..240.
inline constexpr BlockId AdamantOre = 238,   // mena de adamantita (capas 5..8: la mas rara)
                         HollowStone = 239,  // roca palida del Hueco (capa 7)
                         HeartRoot = 240;    // raiz viva del Corazon de Raiz (capa 8; brilla)
// NOCHE-MUNDO: biomas frondosos de las tierras salvajes (GeneratorLush.cpp; Blocks.cpp addLushBlocks) y
// bloques de construccion. Van en los huecos libres: 198..210 y 241..254 (biomas y tablones) y 163..165,
// 177..178 (construccion). 179..180 siguen libres. Los ids de antes no cambian.
inline constexpr BlockId JungleGrass = 198,    // Verdant Jungle: cesped de la selva (superficie)
                         JungleLog = 199,      //   tronco de la selva (los gigantes, 2 x 2)
                         JungleLeaves = 200,
                         JungleVines = 201,    //   liana que cuelga de las copas (planta en cruz)
                         BlossomGrass = 202,   // Blossom Vale: cesped con petalos (superficie)
                         CherryLog = 203,
                         CherryLeaves = 204,   //   hojas rosas del cerezo en flor
                         BlossomPetals = 205,  //   petalos caidos (planta en cruz)
                         SwampGrass = 206,     // Mirefen Swamp: cesped del pantano (superficie)
                         Mud = 207,            //   barro (orillas y fondo de las charcas)
                         MangroveLog = 208,
                         MangroveLeaves = 209,
                         LilyPad = 210,        //   nenufar: losa fina sobre el agua (BlockModel::Plate)
                         SavannaGrass = 241,   // Sunscorch Savanna: hierba seca (superficie)
                         AcaciaLog = 242,
                         AcaciaLeaves = 243,
                         LeafLitter = 244,     // Emberleaf Woods: hojarasca (superficie)
                         AutumnLeavesOrange = 245, AutumnLeavesRed = 246, AutumnLeavesYellow = 247,
                         // Construccion (el jugador los coloca):
                         JunglePlanks = 248, CherryPlanks = 249, AcaciaPlanks = 250, MangrovePlanks = 251,
                         WhiteBricks = 252, SandstoneBricks = 253, QuartzTiles = 254,
                         CopperBlock = 163, OxidizedCopper = 164, PolishedStone = 165,
                         StainedGlassBlue = 177, StainedGlassRose = 178;
// RAMAS-HZ: rama (y raiz) fina de los arboles nuevos (GeneratorAdventure.cpp, solo fuera de las regiones del origen).
// Un solo bloque para todas las especies: el Mesher la dibuja como palos de 4-8/16 hacia los troncos y ramas vecinos
// (en diagonal, escalonados) con la corteza del tronco al que llega; si esta sobre el suelo, como raiz tendida. No choca.
inline constexpr BlockId Branch = 179;
// CIUDADES-GRANDES: misterios de ciudades y castillos (Blocks.cpp addSecretBlocks). Parecen una estanteria y
// un muro de ladrillo de piedra, pero NO chocan: se cruzan andando (pasadizos secretos). No se colocan.
inline constexpr BlockId SecretBookshelf = 257, SecretWall = 258;
// IDS16: bloques de prueba de ids altos (solo para comprobar guardado, red y dibujo de ids > 255): no se colocan
// (placeable = false), no los genera nada y usan texturas que ya existen (no anaden capas). No los uses en el juego.
inline constexpr BlockId IdTest256 = 256, IdTest300 = 300, IdTest511 = 511;
// Uno mas que el id mas alto. IDS16: BlockId es de 16 bits; los ids sin bloque son aire ("unused_N", no se
// colocan). Libres: 180, 255 y 259..510 (menos 300). Se puede subir (hasta 65535) sin tocar el formato de guardado;
// la red del juego (MundoRed.cpp) lleva hasta 16383 (4095 reservado). NO bajarlo.
inline constexpr BlockId Count = 512;
// Via (cualquier tipo, el tope tambien).
inline constexpr bool isRail(BlockId id) { return id >= Rail && id <= BufferStop; }
// Bloque de la red de senales (cable, fuentes, logica, lamparas y vias que la usan).
inline constexpr bool isSignal(BlockId id) {
    return (id >= SurgeRail && id <= SensorRail) || (id >= ArcaniteTrace && id <= EchoRelayLit);
}
// Agua de cualquier nivel (fuente o corriente).
inline constexpr bool isWater(BlockId id) { return id == Water || (id >= WaterFlow1 && id <= WaterFlow7); }
// Planta que vive dentro del agua (la celda esta llena de agua).
inline constexpr bool isAquatic(BlockId id) { return id == Kelp || id == SeaGrass || id == CoralFan || id == SeaGlow; }
// Agua o planta dentro del agua.
inline constexpr bool isWet(BlockId id) { return isWater(id) || isAquatic(id); }
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
// HOJAS: bloques de hojas que llevan la capa "esponjosa" (cubo con huecos que se mece): Mesher.cpp pone una lamina
// algo mas grande por fuera de cada cara al aire con la textura <side>_fluff (hojas recortadas: rompe la silueta de cubo).
// PLANTAS-ALTAS: plantas en cruz que el mundo apila (2+ bloques). Con otra igual encima, el bloque de abajo usa la
// textura "<side>_low" (solo tallo, llega al borde de arriba) y la de arriba la normal (con el remate): una planta
// alta continua en vez de dos plantas cortadas una encima de otra.
inline bool isStackPlant(const BlockDef& d) {
    return d.shape == BlockShape::Cross &&
           (d.side == "tall_grass" || d.side == "reeds" || d.side == "fern" || d.side == "dry_grass" || d.side == "sea_grass");
}
// LIANAS: plantas en cruz que cuelgan del techo apiladas (2+ bloques). Con otra igual DEBAJO, el bloque usa
// "<side>_low" (hebras de borde a borde, sin puntas): una liana continua; solo el ultimo tramo lleva las puntas.
inline bool isHangPlant(const BlockDef& d) { return d.shape == BlockShape::Cross && d.side == "glow_vines"; }
inline bool isFluffyLeaves(const BlockDef& d) {
    return d.shape == BlockShape::Cube && d.model == BlockModel::Full && d.cutout && d.waves && !d.opaque;
}
// MENAS-HZ: menas (bloques "<algo>_ore") que el Mesher dibuja con pepitas de mineral que sobresalen de las caras al
// aire (solo dibujo). Llevan la textura "<side>_gem": el mineral sacado de la textura de la mena (TextureGen.cpp).
inline bool isOreBlock(const BlockDef& d) {
    return d.shape == BlockShape::Cube && d.model == BlockModel::Full && d.opaque && d.name.size() > 4 &&
           d.name.compare(d.name.size() - 4, 4, "_ore") == 0;
}
int textureLayer(std::string_view name);
// Una textura PBR (tres RGBA8 de size x size): de `folder` si existe el PNG
// (se reescala) o generada por codigo. `flat_colors`: estilo de color liso
// (el color medio de la textura, sin relieve; las plantas conservan su silueta
// y las hojas se vuelven macizas).
void buildTexture(const std::string& name, std::uint32_t size, const std::filesystem::path& folder,
                  gfx::VoxelTextureLayer& out, bool flat_colors = false);
// PAQUETES-TEXTURAS: igual, y encima lo que haya en `pack` (carpeta de un paquete de texturas del jugador). Del
// paquete: los PNG pequenos (16, 32, 64...) se amplian sin suavizar (pixel art); si trae el color sin _n/_m, la
// normal se saca del brillo del color y el material es el medio del original; si trae "<planta>" sin
// "<planta>_low", el tramo de abajo sale de la parte baja de esa imagen.
void buildTexture(const std::string& name, std::uint32_t size, const std::filesystem::path& folder,
                  const std::filesystem::path& pack, gfx::VoxelTextureLayer& out, bool flat_colors);

// --- Paquetes de texturas (solo visual, del cliente) ------------------------------
// Un paquete = una carpeta dentro de una de las raices, con <textura>.png (mismo nombre que la del bloque,
// textureNames()), opcionalmente _n.png/_m.png, y un pack.json {name, author, version, description, icon}.
// Lo que falte sale de las texturas del juego. VoxelSystem rehace el array de texturas al cambiar de paquete.
struct TexturePackInfo {
    std::string id;           // nombre de la carpeta (lo que se pasa a setTexturePack)
    std::string name, author, version, description;
    std::filesystem::path folder;
    std::filesystem::path icon;  // vacio si no tiene
    int textures = 0;            // PNG de color que coinciden con una textura de bloque
};
// Carpetas donde buscar paquetes (la primera es la del jugador: se crea al listar).
void setTexturePackRoots(std::vector<std::filesystem::path> roots);
std::vector<std::filesystem::path> texturePackRoots();
std::vector<TexturePackInfo> listTexturePacks();
// "" = ninguno (texturas del juego). false si no existe ese paquete (y no cambia nada).
bool setTexturePack(const std::string& id);
std::string texturePack();
std::filesystem::path texturePackFolder();
// Cambia con cada setTexturePack / reloadTextures: VoxelSystem lo mira para rehacer las texturas.
std::uint64_t texturePackGeneration();
// Vuelve a leer las texturas del disco (p. ej. tras editar los PNG de un paquete).
void reloadTextures();
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
    Deep = 5,     // Reino Profundo: dos capas de cavernas inmensas bajo el mundo, mar sin sol, aldeas ocultas
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
    // Entradas de cueva del generador Aventura (bocas en laderas, simas con
    // rampa, grietas): 0 = solo las aberturas naturales (el mundo de antes),
    // 1 = una cada ~160 m (mas en las montanas), 2 = el doble.
    int cave_entrances = 1;

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
    // COLISION-MODELOS: caja del bloque tocado (coordenadas locales del mundo): la celda entera, o la
    // que envuelve las piezas con las que choca un bloque con modelo (valla, silla, mesa...).
    core::Vec3 box_min{};
    core::Vec3 box_max{};
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
// "cielo", "cristal" y "profundo" (el Reino Profundo, bajo la superficie: se
// baja por las simas abisales) con sus capas hondas "profundo2".."profundo8"
// (se baja de una a otra por los pozos del suelo). Ver VoxelSystem::setDimension.
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
    std::string type;      // "aldea", "ciudad", "mazmorra", "ruina", "portal", "torre"; hostiles: "fuerte",
                           // "campamento", "castillo_maldito", "ciudad_maldita", "torre_culto"
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

// CIUDADES-GRANDES: cofre de botin que puso el generador (bloque Chest en el mundo). Lo identifica
// su celda ABSOLUTA + la dimension (key = "<dimension>:<x>:<y>:<z>"): el servidor reparte el botin
// con `table` y `tier` (deterministas por semilla). Los puntos "cofre_botin" de StructureInfo son
// los mismos (tag "<tier>:<table>").
struct GeneratedChest {
    int x = 0, y = 0, z = 0;       // celda del cofre (absoluta)
    core::Vec3 position{};         // centro de la celda en coordenadas locales (origen flotante)
    int tier = 1;                  // 1 comun .. 4 legendario
    std::string table;             // "catacumbas", "gremio", "noble", "banco", "ladrones", "santuario", "castillo", "tesoro_real", "cripta", "bodega", "puzle"
    std::uint64_t structure = 0;   // StructureInfo::id
    std::string dimension = "superficie";
    std::string key;
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
    // Terreno lejano (CVar voxel.far.Distance; FarTerrain.h): teselas que hacen
    // falta, las ya dibujadas, las que faltan, sus caras y el radio (m) de los
    // chunks de verdad ya dibujados (ahi no se dibuja lo lejano).
    int far_tiles = 0;
    int far_ready = 0;
    int far_pending = 0;
    std::uint64_t far_quads = 0;
    float far_last_tile_ms = 0.0f;
    int far_from_cache = 0;
    int far_generated = 0;
    float far_hole = 0.0f;
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
    // Luces reales de los bloques que dan luz (antorchas, faroles, farolas,
    // hogueras, lamparas...) cerca de `eye` (local): se anaden a `out` (las
    // puntuales de la escena, despues de RenderSync) como luces puntuales sin
    // sombra, para que iluminen tambien a personajes y objetos (la luz de los
    // bloques solo llega a los voxeles). Las mas cercanas y brillantes, con
    // fundido al entrar y salir. CVars voxel.lights.* (BlockLights.cpp).
    void appendBlockLights(const core::Vec3& eye, std::vector<scene::PointLight>& out);
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
    // Solo para el DIBUJO: la celda se malla como aire (puertas que el juego
    // anima con una entidad). No cambia el bloque, ni la luz, ni la fisica, ni
    // se guarda o se manda por red; se vacia al cambiar de mundo o dimension.
    void setBlockHidden(int x, int y, int z, bool hidden);
    bool isBlockHidden(int x, int y, int z) const;
    void showHiddenBlocks();  // todas otra vez (el juego recarga sus scripts)
    // --- Rieles y senales (Rails.h) ---
    // Via en la celda: mascara de conexiones (bit 0 +X, 1 -X, 2 +Z, 3 -Z) y hacia donde sube la rampa
    // (0..3, -1 llana). false si no hay via. La misma regla que dibuja el Mesher.
    bool railAt(int x, int y, int z, int& mask, int& up) const;
    // Senal de arcanita de la celda (0..15). Las vias de impulso, freno y cambio y las lamparas: >0 = con senal.
    int signalPower(int x, int y, int z) const;
    // Presencia sobre una placa de presion o una via detectora: el juego la refresca (cada 0.1-0.2 s)
    // mientras hay algo encima; caduca sola a los 0.35 s.
    void setSignalInput(int x, int y, int z, bool on);
    // Bloques de un tipo en un cubo de lado 2 * radius + 1 alrededor de center (como mucho `max`).
    std::vector<BlockPos> findBlocks(const core::Vec3& center, int radius, BlockId id, int max) const;
    // VFX-VIVO: celdas de CUALQUIERA de los bloques marcados en `wanted` (tabla de block::Count) en una sola pasada
    // (una busqueda de chunk por columna, no por celda). maxPerId: tope por tipo (0 = sin tope).
    struct FoundBlock { BlockPos pos; BlockId id; };
    std::vector<FoundBlock> findBlocksAny(const core::Vec3& center, int radius, const std::vector<bool>& wanted, int maxPerId,
                                          int max) const;
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
    // Bloques de mar sobre el suelo de la columna segun el generador (aunque no este
    // cargada; 0 = tierra, lago o rio). En el oceano hondo (lejos del origen) 30..55,
    // en las fosas hasta ~57; cerca del origen el mar es de menos de ~30.
    int oceanDepth(int x, int z) const;
    // Hay un bloque de agua (fuente o corriente, o una planta dentro del agua: kelp,
    // pradera marina...) en ese punto.
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
    // Algo que aparta las plantas y las hojas a ras de suelo (pies, radio en m)
    // este frame. Voxel.moveBox de los scripts lo apunta solo (jugador,
    // criaturas, aldeanos, monturas); Voxel.pushFoliage, lo demas. Dura un
    // momento (0.25 s) si no se repite. Seguro desde cualquier hilo.
    void noteFoliageInteractor(const core::Vec3& feet, float radius);
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
    // CIUDADES-GRANDES: cofres de botin generados cuyo centro cae en la caja [min, max] (coordenadas
    // locales; funciona sin chunks cargados). Ver GeneratedChest.
    std::vector<GeneratedChest> structureChests(const core::Vec3& min, const core::Vec3& max) const;

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
