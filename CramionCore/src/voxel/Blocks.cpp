// Catalogo de bloques y el componente VoxelWorld.

#include "CramionCore/voxel/Voxel.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace cramion::voxel {

namespace {

BlockDef cube(std::string name, std::string label, std::string all, float hardness) {
    BlockDef d;
    d.name = std::move(name);
    d.label = std::move(label);
    d.top = d.bottom = d.side = std::move(all);
    d.hardness = hardness;
    return d;
}

BlockDef cube3(std::string name, std::string label, std::string top, std::string bottom, std::string side,
               float hardness) {
    BlockDef d = cube(std::move(name), std::move(label), "", hardness);
    d.top = std::move(top);
    d.bottom = std::move(bottom);
    d.side = std::move(side);
    return d;
}

BlockDef plant(std::string name, std::string label, std::string texture, float hardness = 0.05f) {
    BlockDef d = cube(std::move(name), std::move(label), std::move(texture), hardness);
    d.shape = BlockShape::Cross;
    d.solid = false;
    d.opaque = false;
    d.cutout = true;
    d.waves = true;
    d.replaceable = true;
    d.light_filter = 0;
    return d;
}

BlockDef leaves(std::string name, std::string label, std::string texture, BlockTint tint) {
    BlockDef d = cube(std::move(name), std::move(label), std::move(texture), 0.25f);
    d.opaque = false;
    d.cutout = true;
    d.waves = true;
    d.light_filter = 1;
    d.tint = tint;
    return d;
}

// Bloques de las dimensiones (abismo, cielo, cristal). Etiquetas en ingles
// (las ve el jugador).
void addDimensionBlocks(std::vector<BlockDef>& b) {
    const auto glowing = [](BlockDef d, std::uint8_t light) {
        d.light = light;
        return d;
    };
    // --- Abismo ---
    b[block::Basalt] = cube3("basalt", "Basalt", "basalt_top", "basalt_top", "basalt", 1.8f);
    b[block::Obsidian] = cube("obsidian", "Obsidian", "obsidian", 6.0f);
    BlockDef lava = cube("lava", "Lava", "lava", -1.0f);
    lava.solid = false;  // se hunde (el juego hace el dano)
    lava.replaceable = true;
    lava.light = 15;
    lava.placeable = false;
    b[block::Lava] = lava;
    b[block::Magma] = glowing(cube("magma", "Magma Rock", "magma", 1.4f), 9);
    b[block::Ash] = cube("ash", "Ash", "ash", 0.5f);
    b[block::FireStem] = cube("fire_stem", "Fire Mushroom Stem", "fire_stem", 0.8f);
    b[block::FireCap] = glowing(cube("fire_cap", "Fire Mushroom Cap", "fire_cap", 0.6f), 12);
    b[block::EmberGrass] = glowing(plant("ember_grass", "Ember Grass", "ember_grass", 0.02f), 6);
    b[block::EmberGrass].waves = false;
    b[block::AbyssBricks] = cube("abyss_bricks", "Abyss Bricks", "abyss_bricks", 2.5f);
    // --- Cielo ---
    b[block::SkyGrass] = cube3("sky_grass", "Sky Grass", "sky_grass_top", "sky_soil", "sky_grass_side", 0.7f);
    b[block::SkyGrass].drop = block::SkySoil;
    b[block::SkySoil] = cube("sky_soil", "Sky Soil", "sky_soil", 0.6f);
    b[block::SkyStone] = cube("sky_stone", "Skystone", "sky_stone", 1.6f);
    BlockDef cloud = cube("cloud", "Cloud", "cloud", 0.2f);
    cloud.solid = false;  // el mar de nubes no sostiene: debajo esta el vacio
    b[block::Cloud] = cloud;
    b[block::GoldenLog] = cube3("golden_log", "Golden Log", "golden_log_top", "golden_log_top", "golden_log", 1.3f);
    b[block::GoldenLeaves] = leaves("golden_leaves", "Golden Leaves", "golden_leaves", BlockTint::None);
    b[block::SkyCrystal] = glowing(cube("sky_crystal", "Sky Crystal", "sky_crystal", 1.0f), 10);
    BlockDef falling = cube("falling_water", "Waterfall", "falling_water", -1.0f);
    falling.solid = false;
    falling.opaque = false;
    falling.cutout = true;  // sin caras entre bloques de cascada
    falling.replaceable = true;
    falling.light_filter = 1;
    falling.placeable = false;
    b[block::FallingWater] = falling;
    b[block::SkyFlower] = plant("sky_flower", "Sky Bloom", "sky_flower", 0.02f);
    // --- Cristal ---
    b[block::GlowMoss] = cube3("glow_moss", "Glowing Moss", "glow_moss", "dusk_soil", "glow_moss_side", 0.6f);
    b[block::GlowMoss].drop = block::DuskSoil;
    b[block::DuskSoil] = cube("dusk_soil", "Dusk Soil", "dusk_soil", 0.6f);
    b[block::DuskStone] = cube("dusk_stone", "Duskstone", "dusk_stone", 1.6f);
    b[block::CrystalPink] = glowing(cube("crystal_pink", "Rose Crystal", "crystal_pink", 1.0f), 11);
    b[block::CrystalBlue] = glowing(cube("crystal_blue", "Azure Crystal", "crystal_blue", 1.0f), 11);
    b[block::CrystalViolet] = glowing(cube("crystal_violet", "Violet Crystal", "crystal_violet", 1.0f), 11);
    b[block::CrystalGreen] = glowing(cube("crystal_green", "Jade Crystal", "crystal_green", 1.0f), 11);
    b[block::ShroomStem] = cube("shroom_stem", "Giant Mushroom Stem", "shroom_stem", 0.8f);
    b[block::ShroomCapBlue] = glowing(cube("shroom_cap_blue", "Blue Glowcap", "shroom_cap_blue", 0.6f), 9);
    b[block::ShroomCapViolet] = glowing(cube("shroom_cap_violet", "Violet Glowcap", "shroom_cap_violet", 0.6f), 9);
    b[block::GlowGrass] = plant("glow_grass", "Glowgrass", "glow_grass", 0.02f);
    b[block::CrystalSprout] = glowing(plant("crystal_sprout", "Crystal Sprout", "crystal_sprout", 0.3f), 7);
    b[block::CrystalSprout].waves = false;
}

// Bloques de las estructuras (aldeas, ciudades, torres, mazmorras): construccion,
// muebles y decoracion. Etiquetas en ingles (las ve el jugador). Los muebles
// son cubos con recorte (la silueta de la mesa, la silla...) y algunos dan luz.
void addStructureBlocks(std::vector<BlockDef>& b) {
    const auto see_through = [](BlockDef d) {
        d.opaque = false;
        d.cutout = true;
        d.light_filter = 0;
        return d;
    };
    const auto glowing = [](BlockDef d, std::uint8_t light) {
        d.light = light;
        return d;
    };
    // --- Construccion ---
    b[block::Plaster] = cube("plaster", "Plaster", "plaster", 1.0f);
    b[block::Timber] = cube3("timber", "Timber Beam", "timber_top", "timber_top", "timber", 1.3f);
    b[block::DarkPlanks] = cube("dark_planks", "Dark Planks", "dark_planks", 1.0f);
    b[block::SprucePlanks] = cube("spruce_planks", "Spruce Planks", "spruce_planks", 1.0f);
    b[block::RoofRed] = cube("roof_red", "Red Roof Tiles", "roof_red", 1.2f);
    b[block::RoofBlue] = cube("roof_blue", "Blue Roof Tiles", "roof_blue", 1.2f);
    b[block::RoofGreen] = cube("roof_green", "Green Roof Tiles", "roof_green", 1.2f);
    b[block::RoofOrange] = cube("roof_orange", "Orange Roof Tiles", "roof_orange", 1.2f);
    b[block::RoofPurple] = cube("roof_purple", "Purple Roof Tiles", "roof_purple", 1.2f);
    b[block::RoofSlate] = cube("roof_slate", "Slate Roof Tiles", "roof_slate", 1.2f);
    b[block::Thatch] = cube("thatch", "Thatch", "thatch", 0.5f);
    b[block::MudBricks] = cube("mud_bricks", "Mud Bricks", "mud_bricks", 1.2f);
    b[block::Marble] = cube("marble", "Marble", "marble", 1.8f);
    b[block::MarblePillar] = cube3("marble_pillar", "Marble Pillar", "marble", "marble", "marble_pillar", 1.8f);
    b[block::Paving] = cube("paving", "Paving Stones", "paving", 1.6f);
    b[block::DirtPath] = cube3("dirt_path", "Dirt Path", "dirt_path", "dirt", "dirt", 0.6f);
    b[block::DirtPath].drop = block::Dirt;
    b[block::Farmland] = cube3("farmland", "Farmland", "farmland", "dirt", "dirt", 0.6f);
    b[block::Farmland].drop = block::Dirt;
    b[block::Wheat] = plant("wheat", "Wheat", "wheat", 0.02f);
    b[block::Carrots] = plant("carrots", "Carrots", "carrots", 0.02f);
    b[block::HayBale] = cube3("hay_bale", "Hay Bale", "hay_top", "hay_top", "hay_side", 0.5f);
    b[block::Fence] = see_through(cube3("fence", "Wooden Fence", "fence_top", "fence_top", "fence", 1.0f));
    b[block::Fence].model = BlockModel::Fence;  // poste y travesanos (Mesher.cpp)
    BlockDef door = cube("door", "Wooden Door", "door", 1.0f);
    door.solid = false;  // se pasa (la puerta esta abierta)
    door.side_upper = "door_top";  // 2 de alto: la mitad de arriba con su dibujo (no la puerta repetida)
    b[block::Door] = door;
    b[block::Lantern] = glowing(see_through(cube3("lantern", "Lantern", "lantern_top", "lantern_top", "lantern", 0.3f)), 15);
    b[block::Lantern].solid = false;  // cuelga a la altura de la cabeza
    b[block::Lantern].model = BlockModel::Lantern;  // farol pequeno con cadena o pie (Mesher.cpp)
    b[block::Brazier] = glowing(see_through(cube3("brazier", "Brazier", "brazier_top", "brazier_top", "brazier", 1.2f)), 15);
    BlockDef fire = plant("fire", "Fire", "fire", 0.05f);
    fire.waves = false;
    fire.replaceable = false;
    fire.light = 15;
    b[block::Fire] = fire;
    // --- Muebles y decoracion ---
    b[block::Table] = see_through(cube3("table", "Table", "table_top", "table_top", "table_side", 0.8f));
    b[block::Chair] = see_through(cube3("chair", "Chair", "chair_top", "chair_top", "chair", 0.6f));
    b[block::Bed] = cube3("bed", "Bed", "bed_top", "dark_planks", "bed_side", 0.6f);
    b[block::Bookshelf] = cube3("bookshelf", "Bookshelf", "dark_planks", "dark_planks", "bookshelf", 0.9f);
    b[block::Barrel] = cube3("barrel", "Barrel", "barrel_top", "barrel_top", "barrel", 0.8f);
    b[block::Crate] = cube("crate", "Crate", "crate", 0.8f);
    b[block::Chest] = cube3("chest", "Chest", "chest_top", "chest_top", "chest", 1.0f);
    b[block::Counter] = cube3("counter", "Counter", "counter_top", "counter_top", "counter", 0.9f);
    b[block::Shelf] = cube3("shelf", "Display Shelf", "dark_planks", "dark_planks", "shelf", 0.9f);
    b[block::Anvil] = see_through(cube3("anvil", "Anvil", "anvil_top", "anvil_top", "anvil", 3.0f));
    b[block::Forge] = glowing(cube3("forge", "Forge", "forge_top", "stone_bricks", "forge", 2.0f), 13);
    b[block::Oven] = glowing(cube3("oven", "Oven", "stone_bricks", "stone_bricks", "oven", 1.8f), 10);
    b[block::RugRed] = cube("rug_red", "Red Rug", "rug_red", 0.4f);
    b[block::RugBlue] = cube("rug_blue", "Blue Rug", "rug_blue", 0.4f);
    b[block::Painting] = cube3("painting", "Painting", "dark_planks", "dark_planks", "painting", 0.5f);
    b[block::Bench] = see_through(cube3("bench", "Bench", "bench_top", "bench_top", "bench", 0.6f));
    b[block::Altar] = cube3("altar", "Altar", "altar_top", "marble", "altar", 2.5f);
    b[block::Throne] = cube3("throne", "Throne", "throne_top", "throne_top", "throne", 2.5f);
    b[block::BannerRed] = cube("banner_red", "Red Banner", "banner_red", 0.4f);
    b[block::BannerBlue] = cube("banner_blue", "Blue Banner", "banner_blue", 0.4f);
    b[block::ClothRed] = cube("cloth_red", "Red Cloth", "cloth_red", 0.4f);
    b[block::ClothBlue] = cube("cloth_blue", "Blue Cloth", "cloth_blue", 0.4f);
    b[block::ClothYellow] = cube("cloth_yellow", "Yellow Cloth", "cloth_yellow", 0.4f);
    b[block::ClothWhite] = cube("cloth_white", "White Cloth", "cloth_white", 0.4f);
    b[block::GoldBlock] = cube("gold_block", "Gold Block", "gold_block", 3.0f);
    b[block::WeaponRack] = see_through(cube3("weapon_rack", "Weapon Rack", "dark_planks", "dark_planks", "weapon_rack", 1.0f));
    BlockDef pool = cube("pool_water", "Fountain Water", "pool_water", -1.0f);
    pool.solid = false;  // el agua de las fuentes y pozos (sobre el nivel del mar)
    pool.opaque = false;
    pool.cutout = true;  // sin caras entre bloques de agua
    pool.replaceable = true;
    pool.light_filter = 2;
    pool.placeable = false;
    b[block::PoolWater] = pool;
    // --- Mazmorras ---
    b[block::DungeonBricks] = cube("dungeon_bricks", "Dungeon Bricks", "dungeon_bricks", 2.5f);
    b[block::MossyBricks] = cube("mossy_bricks", "Mossy Bricks", "mossy_bricks", 2.5f);
    b[block::CrackedBricks] = cube("cracked_bricks", "Cracked Bricks", "cracked_bricks", 2.0f);
    b[block::CarvedStone] = cube3("carved_stone", "Carved Stone", "carved_stone_top", "carved_stone_top", "carved_stone", 2.5f);
    b[block::IronBars] = see_through(cube("iron_bars", "Iron Bars", "iron_bars", 3.0f));
    BlockDef spikes = plant("spikes", "Spikes", "spikes", 1.5f);
    spikes.waves = false;
    spikes.replaceable = false;  // trampa (el juego hace el dano)
    b[block::Spikes] = spikes;
    b[block::Bones] = plant("bones", "Bones", "bones", 0.1f);
    b[block::Bones].waves = false;
    b[block::Cobweb] = plant("cobweb", "Cobweb", "cobweb", 0.2f);
    b[block::Cobweb].waves = false;
    b[block::Coffin] = cube3("coffin", "Sarcophagus", "coffin_top", "carved_stone_top", "coffin", 3.0f);
    b[block::RuneStone] = glowing(cube("rune_stone", "Rune Stone", "rune_stone", 3.0f), 7);
    BlockDef candles = plant("candles", "Candles", "candles", 0.05f);
    candles.waves = false;
    candles.replaceable = false;
    candles.light = 11;
    b[block::Candles] = candles;
}

std::vector<BlockDef> makeBlocks() {
    std::vector<BlockDef> b(block::Count);
    BlockDef air;
    air.name = "air";
    air.label = "Aire";
    air.shape = BlockShape::Air;
    air.solid = false;
    air.opaque = false;
    air.light_filter = 0;
    air.hardness = -1.0f;
    air.placeable = false;
    b[block::Air] = air;
    b[block::Stone] = cube("stone", "Piedra", "stone", 1.6f);
    b[block::Stone].drop = block::Cobblestone;
    b[block::Dirt] = cube("dirt", "Tierra", "dirt", 0.6f);
    b[block::Grass] = cube3("grass", "Césped", "grass_top", "dirt", "grass_side", 0.7f);
    b[block::Grass].tint = BlockTint::Grass;
    b[block::Grass].drop = block::Dirt;
    b[block::Cobblestone] = cube("cobblestone", "Roca", "cobblestone", 1.8f);
    b[block::OakLog] = cube3("oak_log", "Tronco de roble", "oak_log_top", "oak_log_top", "oak_log", 1.3f);
    b[block::OakLeaves] = leaves("oak_leaves", "Hojas de roble", "oak_leaves", BlockTint::Foliage);
    b[block::OakPlanks] = cube("oak_planks", "Tablones de roble", "oak_planks", 1.0f);
    b[block::Sand] = cube("sand", "Arena", "sand", 0.55f);
    b[block::Gravel] = cube("gravel", "Grava", "gravel", 0.65f);
    BlockDef water = cube("water", "Agua", "water", -1.0f);
    water.shape = BlockShape::Liquid;
    water.solid = false;
    water.opaque = false;
    water.replaceable = true;
    water.light_filter = 2;
    water.placeable = false;
    b[block::Water] = water;
    // Agua que corre (niveles 1..7): la misma agua, mas baja (VoxelSystem: flowWater).
    for (int level = 1; level <= 7; ++level) {
        BlockDef flow = water;
        flow.name = "water_flow_" + std::to_string(level);
        flow.label = "Agua (corriente)";
        b[block::flowingWater(level)] = flow;
    }
    b[block::Bedrock] = cube("bedrock", "Roca madre", "bedrock", -1.0f);
    b[block::Bedrock].placeable = false;
    b[block::CoalOre] = cube("coal_ore", "Mena de carbón", "coal_ore", 2.0f);
    b[block::IronOre] = cube("iron_ore", "Mena de hierro", "iron_ore", 2.3f);
    b[block::GoldOre] = cube("gold_ore", "Mena de oro", "gold_ore", 2.3f);
    b[block::DiamondOre] = cube("diamond_ore", "Mena de diamante", "diamond_ore", 2.8f);
    b[block::CopperOre] = cube("copper_ore", "Mena de cobre", "copper_ore", 2.1f);
    b[block::CrystalOre] = cube("crystal_ore", "Mena de cristal", "crystal_ore", 2.6f);
    b[block::CrystalOre].light = 6;
    b[block::SnowyGrass] = cube3("snowy_grass", "Césped nevado", "snow", "dirt", "grass_side_snow", 0.7f);
    b[block::SnowyGrass].drop = block::Dirt;
    b[block::Snow] = cube("snow", "Nieve", "snow", 0.35f);
    b[block::Sandstone] = cube3("sandstone", "Arenisca", "sandstone_top", "sandstone_top", "sandstone", 1.2f);
    b[block::Glass] = cube("glass", "Cristal", "glass", 0.4f);
    b[block::Glass].opaque = false;
    b[block::Glass].cutout = true;
    b[block::Glass].light_filter = 0;
    b[block::Bricks] = cube("bricks", "Ladrillos", "bricks", 1.8f);
    b[block::StoneBricks] = cube("stone_bricks", "Ladrillos de piedra", "stone_bricks", 1.8f);
    BlockDef torch = plant("torch", "Antorcha", "torch", 0.05f);
    torch.waves = false;
    torch.replaceable = false;
    torch.light = 14;
    b[block::Torch] = torch;
    b[block::TallGrass] = plant("tall_grass", "Hierba alta", "tall_grass", 0.02f);
    b[block::TallGrass].tint = BlockTint::Grass;
    b[block::RedFlower] = plant("red_flower", "Amapola", "red_flower", 0.02f);
    b[block::YellowFlower] = plant("yellow_flower", "Diente de león", "yellow_flower", 0.02f);
    b[block::BirchLog] = cube3("birch_log", "Tronco de abedul", "birch_log_top", "birch_log_top", "birch_log", 1.3f);
    b[block::BirchLeaves] = leaves("birch_leaves", "Hojas de abedul", "birch_leaves", BlockTint::None);
    b[block::SpruceLog] = cube3("spruce_log", "Tronco de abeto", "spruce_log_top", "spruce_log_top", "spruce_log", 1.3f);
    b[block::SpruceLeaves] = leaves("spruce_leaves", "Hojas de abeto", "spruce_leaves", BlockTint::None);
    b[block::Cactus] = cube3("cactus", "Cactus", "cactus_top", "cactus_top", "cactus_side", 0.4f);
    b[block::CraftingTable] =
        cube3("crafting_table", "Mesa de trabajo", "crafting_table_top", "oak_planks", "crafting_table_side", 1.2f);
    b[block::Glowstone] = cube("glowstone", "Piedra luminosa", "glowstone", 0.3f);
    b[block::Glowstone].light = 15;
    b[block::Clay] = cube("clay", "Arcilla", "clay", 0.6f);
    b[block::DeadBush] = plant("dead_bush", "Arbusto seco", "dead_bush", 0.02f);
    b[block::Ice] = cube("ice", "Hielo", "ice", 0.5f);
    // --- Generador Aventura ---
    b[block::Fern] = plant("fern", "Helecho", "fern", 0.02f);
    b[block::Fern].tint = BlockTint::Foliage;
    b[block::Bush] = plant("bush", "Arbusto", "bush", 0.1f);
    b[block::Bush].tint = BlockTint::Foliage;
    b[block::BlueFlower] = plant("blue_flower", "Aciano", "blue_flower", 0.02f);
    b[block::Lavender] = plant("lavender", "Lavanda", "lavender", 0.02f);
    b[block::WhiteFlower] = plant("white_flower", "Margarita", "white_flower", 0.02f);
    b[block::RedMushroom] = plant("red_mushroom", "Seta roja", "red_mushroom", 0.02f);
    b[block::RedMushroom].waves = false;
    b[block::BrownMushroom] = plant("brown_mushroom", "Seta parda", "brown_mushroom", 0.02f);
    b[block::BrownMushroom].waves = false;
    b[block::GlowMushroom] = plant("glow_mushroom", "Seta luminosa", "glow_mushroom", 0.02f);
    b[block::GlowMushroom].waves = false;
    b[block::GlowMushroom].light = 10;
    b[block::Reeds] = plant("reeds", "Juncos", "reeds", 0.05f);
    b[block::DryGrass] = plant("dry_grass", "Hierba seca", "dry_grass", 0.02f);
    b[block::Moss] = cube("moss", "Musgo", "moss", 0.5f);
    b[block::Moss].tint = BlockTint::Grass;
    b[block::RedSand] = cube("red_sand", "Arena roja", "red_sand", 0.55f);
    b[block::Terracotta] = cube("terracotta", "Terracota", "terracotta", 1.25f);
    addDimensionBlocks(b);
    addStructureBlocks(b);
    return b;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

const std::vector<BlockDef>& blocks() {
    static const std::vector<BlockDef> list = makeBlocks();
    return list;
}

const BlockDef& blockDef(BlockId id) {
    const auto& list = blocks();
    return id < list.size() ? list[id] : list[block::Air];
}

BlockId blockId(std::string_view name) {
    const std::string wanted = lower(name);
    const auto& list = blocks();
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].name == wanted || lower(list[i].label) == wanted) return static_cast<BlockId>(i);
    }
    return block::Air;
}

const std::vector<std::string>& textureNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        const auto add = [&](const std::string& n) {
            if (!n.empty() && std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
        };
        for (const BlockDef& d : blocks()) {
            if (d.shape == BlockShape::Air || d.shape == BlockShape::Liquid) continue;
            add(d.top);
            add(d.side);
            add(d.bottom);
            add(d.side_upper);
        }
        return out;
    }();
    return names;
}

int textureLayer(std::string_view name) {
    const auto& names = textureNames();
    const auto it = std::find(names.begin(), names.end(), name);
    return it == names.end() ? 0 : static_cast<int>(it - names.begin());
}

// --- Componente -------------------------------------------------------------------

void VoxelWorld::reflect(ecs::PropertyVisitor& v) {
    v.field({"seed", "Semilla", "La misma semilla da el mismo mundo"}, seed);
    static constexpr std::array<const char*, 2> kGenerators = {"Clasico", "Aventura"};
    ecs::enumField(v, {"generator", "Generador",
                       "Clasico: continentes, biomas, cuevas y arboles. Aventura: ademas rios, mesetas, acantilados, "
                       "vegetacion densa y cuevas con setas"},
                   generator, kGenerators);
    v.field({"flat_colors", "Color liso", "Cada bloque de un color con variacion suave, sin texturas ni relieve"},
            flat_colors);
    v.field({"render_distance", "Distancia de dibujado", "Chunks (16 m) alrededor de la camara"}, render_distance, 2, 32);
    v.field({"sea_level", "Nivel del mar"}, sea_level, ecs::FloatRange{1.0f, 250.0f, 0.5f, "%.0f"});
    v.field({"textures", "Carpeta de texturas", "En Assets: <bloque>.png, _n.png (normal) y _m.png (material)"}, textures);
    v.field({"texture_size", "Resolucion de textura"}, texture_size, 16, 1024);
    v.field({"preview_in_editor", "Vista previa en el editor"}, preview_in_editor);
    v.field({"collision_distance", "Distancia de colision", "Chunks con colision fisica (Rigidbody) alrededor de la camara"},
            collision_distance, 1, 16);
    v.field({"tree_density", "Densidad de arboles",
             "1 = como siempre. Menos de 1: bosques solo en manchas con praderas abiertas entre ellas (0 = sin "
             "arboles); hasta 2: mas arboles"},
            tree_density, ecs::FloatRange{0.0f, 2.0f, 0.01f, "%.2f"});
    v.field({"clearings", "Claros", "Huecos y praderas dentro de los bosques (0 = ninguno, 1 = muchos)"}, clearings,
            ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f"});
    v.field({"cave_entrances", "Entradas de cueva",
             "Generador Aventura: bocas en laderas, simas con rampa y grietas que bajan andando a las cuevas. "
             "0 = solo las aberturas naturales, 1 = una cada ~160 m (mas en las montanas), 2 = el doble"},
            cave_entrances, 0, 2);
}

void registerVoxelComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("VoxelWorld") == nullptr) {
        registry.registerComponent<VoxelWorld>("VoxelWorld", "Mundo de bloques", "Entorno");
    }
}

}  // namespace cramion::voxel
