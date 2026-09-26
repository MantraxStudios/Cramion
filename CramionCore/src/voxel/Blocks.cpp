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
    b[block::Bedrock] = cube("bedrock", "Roca madre", "bedrock", -1.0f);
    b[block::Bedrock].placeable = false;
    b[block::CoalOre] = cube("coal_ore", "Mena de carbón", "coal_ore", 2.0f);
    b[block::IronOre] = cube("iron_ore", "Mena de hierro", "iron_ore", 2.3f);
    b[block::GoldOre] = cube("gold_ore", "Mena de oro", "gold_ore", 2.3f);
    b[block::DiamondOre] = cube("diamond_ore", "Mena de diamante", "diamond_ore", 2.8f);
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
    v.field({"render_distance", "Distancia de dibujado", "Chunks (16 m) alrededor de la camara"}, render_distance, 2, 32);
    v.field({"sea_level", "Nivel del mar"}, sea_level, ecs::FloatRange{1.0f, 250.0f, 0.5f, "%.0f"});
    v.field({"textures", "Carpeta de texturas", "En Assets: <bloque>.png, _n.png (normal) y _m.png (material)"}, textures);
    v.field({"texture_size", "Resolucion de textura"}, texture_size, 16, 1024);
    v.field({"preview_in_editor", "Vista previa en el editor"}, preview_in_editor);
    v.field({"collision_distance", "Distancia de colision", "Chunks con colision fisica (Rigidbody) alrededor de la camara"},
            collision_distance, 1, 16);
}

void registerVoxelComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("VoxelWorld") == nullptr) {
        registry.registerComponent<VoxelWorld>("VoxelWorld", "Mundo de bloques", "Entorno");
    }
}

}  // namespace cramion::voxel
