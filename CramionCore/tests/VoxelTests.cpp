// Pruebas del mundo de bloques (consola, sin GPU): catalogo y texturas,
// mallador (caras hacia fuera, oclusion), generacion determinista, luz del
// cielo y de antorchas, rayo, colision de una caja, agua, guardar y cargar
// mundos y rendimiento. Devuelve 0 si todo va.

#include "CramionCore/ecs/World.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/voxel/Voxel.h"

#include "../src/voxel/VoxelInternal.h"
#include "../src/voxel/Structures.h"
#include "../src/voxel/FarTerrain.h"
#include "../src/voxel/Rails.h"
#include "../src/voxel/BlockLights.h"
#include <CramionFX/core/Frustum.h>
#include "CramionCore/cvar/CVar.h"

#include <CramionFX/vk/VoxelPass.h>
#include <CramionFX/asset/ImageFile.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <tuple>

using namespace cramion;
using namespace cramion::voxel;
using core::Vec3;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

void testCatalog() {
    std::printf("Bloques y texturas\n");
    check(blocks().size() == block::Count && blockId("stone") == block::Stone && blockId("Antorcha") == block::Torch,
          "catalogo y busqueda por nombre o etiqueta");
    check(textureNames().size() >= 30 && textureLayer("grass_top") != textureLayer("dirt"), "texturas por nombre");
    gfx::VoxelTextureLayer leaves;
    buildTexture("oak_leaves", 32, {}, leaves);
    int holes = 0;
    for (std::size_t i = 3; i < leaves.albedo.size(); i += 4) holes += leaves.albedo[i] < 128 ? 1 : 0;
    check(leaves.albedo.size() == 32 * 32 * 4 && holes > 20, "las hojas tienen huecos (recorte)");
    gfx::VoxelTextureLayer torch;
    buildTexture("torch", 32, {}, torch);
    int glow = 0;
    for (std::size_t i = 2; i < torch.material.size(); i += 4) glow += torch.material[i] > 200 ? 1 : 0;
    check(glow > 0, "la llama de la antorcha brilla (emision)");
}

// Generador Aventura (rios, mesetas, vegetacion densa, cuevas con setas) y el
// estilo de color liso.
// PAQUETES-TEXTURAS: un paquete en una carpeta temporal sustituye la textura (pixel art ampliado sin suavizar), el
// tramo de abajo de la hierba alta sale de su imagen, y lo que no trae sale del juego.
void testTexturePacks() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "cramion_texture_packs_test";
    std::error_code error;
    fs::remove_all(root, error);
    const fs::path pack = root / "juego" / "Mi Paquete";
    fs::create_directories(pack, error);
    {
        std::FILE* f = std::fopen((pack / "pack.json").string().c_str(), "wb");
        const char* json = R"({"name": "Paquete de prueba", "author": "Ana", "version": "1.2", "description": "rojo"})";
        if (f != nullptr) {
            std::fwrite(json, 1, std::strlen(json), f);
            std::fclose(f);
        }
    }
    asset::ImageRgba8 img;
    img.width = img.height = 16;
    img.pixels.assign(16 * 16 * 4, 255);
    for (std::size_t i = 0; i < 16 * 16; ++i) {  // tablero rojo / azul de 1 pixel
        const bool red = ((i % 16) + (i / 16)) % 2 == 0;
        img.pixels[i * 4 + 0] = red ? 255 : 0;
        img.pixels[i * 4 + 1] = 0;
        img.pixels[i * 4 + 2] = red ? 0 : 255;
    }
    asset::saveImagePng(pack / "stone.png", img);
    for (std::size_t i = 0; i < 16 * 16; ++i) img.pixels[i * 4 + 3] = (i / 16) < 6 ? 0 : 255;  // arriba (remate) transparente, abajo opaco
    asset::saveImagePng(pack / "tall_grass.png", img);
    asset::saveImagePng(pack / "no_es_un_bloque.png", img);

    setTexturePackRoots({root / "jugador", root / "juego"});
    const auto packs = listTexturePacks();
    check(packs.size() == 1 && packs[0].id == "Mi Paquete" && packs[0].name == "Paquete de prueba" && packs[0].author == "Ana" &&
              packs[0].version == "1.2" && packs[0].textures == 2,
          "paquetes de texturas: lista, pack.json y PNG que coinciden con texturas de bloque");
    check(fs::is_directory(root / "jugador"), "paquetes de texturas: se crea la carpeta del jugador");
    const std::uint64_t gen = texturePackGeneration();
    check(!setTexturePack("No existe") && texturePack().empty(), "paquete que no existe: no cambia");
    check(setTexturePack("Mi Paquete") && texturePack() == "Mi Paquete" && texturePackGeneration() != gen,
          "elegir un paquete cambia la generacion (VoxelSystem rehace las texturas)");

    gfx::VoxelTextureLayer stone, base;
    buildTexture("stone", 64, {}, texturePackFolder(), stone, false);
    buildTexture("stone", 64, {}, base);
    // 16 -> 64: bloques de 4 x 4 del mismo color (sin suavizar).
    bool sharp = stone.albedo.size() == 64 * 64 * 4;
    for (std::uint32_t y = 0; sharp && y < 64; ++y) {
        for (std::uint32_t x = 0; x < 64; ++x) {
            const bool red = ((x / 4) + (y / 4)) % 2 == 0;
            const std::size_t i = (static_cast<std::size_t>(y) * 64 + x) * 4;
            if (stone.albedo[i] != (red ? 255 : 0) || stone.albedo[i + 2] != (red ? 0 : 255)) {
                sharp = false;
                break;
            }
        }
    }
    check(sharp, "el PNG pequeno del paquete se amplia sin suavizar (pixel art)");
    check(stone.normal != base.normal && stone.normal.size() == base.normal.size(), "sin _n en el paquete: normal sacada de su color");
    gfx::VoxelTextureLayer low, dirt, dirt_base;
    buildTexture("tall_grass_low", 64, {}, texturePackFolder(), low, false);
    std::size_t transparent = 0;
    for (std::size_t i = 0; i < 64 * 64; ++i) transparent += low.albedo[i * 4 + 3] == 0 ? 1 : 0;
    check(transparent == 0, "tall_grass_low sale de la parte baja de la tall_grass del paquete");
    buildTexture("dirt", 64, {}, texturePackFolder(), dirt, false);
    buildTexture("dirt", 64, {}, dirt_base);
    check(dirt.albedo == dirt_base.albedo && dirt.normal == dirt_base.normal, "lo que no trae el paquete sale del juego");
    check(setTexturePack("") && texturePack().empty() && texturePackFolder().empty(), "volver a las texturas del juego");
    setTexturePackRoots({});
    fs::remove_all(root, error);
    // CRAMION_TEXNAMES=<archivo>: la lista de nombres de textura de bloque (para la guia de paquetes).
    if (const char* file = std::getenv("CRAMION_TEXNAMES")) {
        if (std::FILE* f = std::fopen(file, "wb")) {
            for (const std::string& n : textureNames()) std::fprintf(f, "%s\n", n.c_str());
            std::fclose(f);
        }
    }
}

void testAdventure() {
    std::printf("Generador Aventura\n");
    const Generator gen(1234, 62, VoxelGenerator::Adventure);
    check(gen.profile() == VoxelGenerator::Adventure, "perfil Aventura");

    // Biomas en una muestra grande (8 x 8 km).
    std::array<int, Generator::kAdventureBiomeCount> biomes{};
    int river_water = 0, mesas = 0;
    BlockPos forest{}, mesa{}, river{};
    bool have_forest = false, have_mesa = false, have_river = false;
    for (int z = -4096; z < 4096; z += 48) {
        for (int x = -4096; x < 4096; x += 48) {
            const Generator::AdventureColumn c = gen.adventureColumn(x, z);
            ++biomes[static_cast<std::size_t>(c.biome)];
            if (c.biome == Generator::ARiver && c.height < 62) {
                ++river_water;
                if (!have_river) river = BlockPos{x, c.height, z}, have_river = true;
            }
            if (c.mesa) {
                ++mesas;
                // La primera meseta del desierto (las de las tierras salvajes son de otros bloques).
                if (!have_mesa && c.biome == Generator::ADesert) mesa = BlockPos{x, c.height, z}, have_mesa = true;
            }
            if (c.biome == Generator::AForest && !have_forest) forest = BlockPos{x, c.height, z}, have_forest = true;
        }
    }
    bool all = true;
    for (int b = 0; b <= Generator::AMountains; ++b) all = all && biomes[static_cast<std::size_t>(b)] > 0;
    std::printf("    oceano %d playa %d rio %d pradera %d bosque %d desierto %d nieve %d montana %d (mesetas %d)\n", biomes[0],
                biomes[1], biomes[2], biomes[3], biomes[4], biomes[5], biomes[6], biomes[7], mesas);
    check(all, "estan todos los biomas (oceano, playa, rio, pradera, bosque, desierto, nieve, montana)");
    check(river_water > 20, "rios con el cauce bajo el nivel del mar (el oceano los llena)");
    check(mesas > 5, "mesetas en el desierto");

    // Determinista: el mismo chunk dos veces.
    ChunkData a, b;
    a.cx = b.cx = 3;
    a.cz = b.cz = -7;
    gen.generate(a);
    gen.generate(b);
    check(a.blocks == b.blocks, "el mismo chunk sale igual (determinista)");

    // Lo que hay en una zona: 5 x 5 chunks alrededor de cada sitio.
    std::array<int, block::Count> count{};
    const auto scan = [&](const BlockPos& at) {
        for (int dz = -2; dz <= 2; ++dz) {
            for (int dx = -2; dx <= 2; ++dx) {
                ChunkData chunk;
                chunk.cx = static_cast<int>(std::floor(static_cast<float>(at.x) / kChunkSize)) + dx;
                chunk.cz = static_cast<int>(std::floor(static_cast<float>(at.z) / kChunkSize)) + dz;
                gen.generate(chunk);
                for (BlockId id : chunk.blocks) ++count[id];
            }
        }
    };
    if (have_forest) scan(forest);
    if (have_mesa) scan(mesa);
    if (have_river) scan(river);
    check(count[block::Fern] > 0 && count[block::Bush] > 0 && count[block::TallGrass] > 50, "bosque: helechos, arbustos y hierba");
    check(count[block::OakLeaves] + count[block::BirchLeaves] > 200 && count[block::OakLog] > 0, "arboles de copa redonda");
    check(count[block::Terracotta] > 100 && count[block::RedSand] > 0, "mesetas de terracota y arena roja");
    check(count[block::Reeds] > 0, "juncos en las orillas del rio");
    check(count[block::GlowMushroom] > 0 && count[block::Moss] > 0, "cuevas con musgo y setas luminosas");
    std::printf("    flores %d helechos %d setas luminosas %d terracota %d juncos %d\n",
                count[block::RedFlower] + count[block::YellowFlower] + count[block::BlueFlower] + count[block::WhiteFlower] +
                    count[block::Lavender],
                count[block::Fern], count[block::GlowMushroom], count[block::Terracotta], count[block::Reeds]);
    check(blockDef(block::GlowMushroom).light > 0 && blockId("lavanda") == block::Lavender, "bloques nuevos con su luz y nombre");

    // Color liso: cubos de un solo color sin relieve; plantas con su silueta; hojas macizas.
    gfx::VoxelTextureLayer stone;
    buildTexture("stone", 32, {}, stone, true);
    bool uniform = true, flat_normal = true;
    for (std::size_t i = 0; i < stone.albedo.size(); i += 4) {
        uniform = uniform && stone.albedo[i] == stone.albedo[0] && stone.albedo[i + 1] == stone.albedo[1];
        flat_normal = flat_normal && stone.normal[i] == 128 && stone.normal[i + 1] == 128;
    }
    check(uniform && flat_normal, "color liso: la piedra es un solo color y sin relieve");
    gfx::VoxelTextureLayer flower, leaves;
    buildTexture("blue_flower", 32, {}, flower, true);
    buildTexture("oak_leaves", 32, {}, leaves, true);
    int flower_holes = 0, leaf_holes = 0;
    for (std::size_t i = 3; i < flower.albedo.size(); i += 4) flower_holes += flower.albedo[i] < 128 ? 1 : 0;
    for (std::size_t i = 3; i < leaves.albedo.size(); i += 4) leaf_holes += leaves.albedo[i] < 128 ? 1 : 0;
    check(flower_holes > 100 && leaf_holes == 0, "color liso: la flor conserva su silueta y las hojas son macizas");

    // Rendimiento: un chunk de Aventura.
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 16; ++i) {
        ChunkData chunk;
        chunk.cx = 40 + i;
        chunk.cz = 17;
        gen.generate(chunk);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 16.0;
    std::printf("    %.2f ms por chunk\n", ms);
    check(ms < 40.0, "un chunk de Aventura en menos de 40 ms");
}

// Posicion de un vertice empaquetado.
Vec3 vertexPosition(std::uint32_t a) {
    return Vec3{static_cast<float>(a & 31u), static_cast<float>((a >> 5) & 31u), static_cast<float>((a >> 10) & 31u)};
}

void testMesher() {
    std::printf("Mallador\n");
    SectionSnapshot s;
    s.light.fill(0xF0);
    s.grass_tint.fill(0xFFF);
    s.foliage_tint.fill(0xFFF);
    s.blocks[static_cast<std::size_t>(SectionSnapshot::index(5, 5, 5))] = block::Stone;
    std::vector<std::uint32_t> v;
    meshSection(s, v);
    check(v.size() == 6 * 4 * 2, "un bloque suelto: 6 caras");
    bool outward = true;
    for (std::size_t q = 0; q < v.size() / 8; ++q) {
        const Vec3 p0 = vertexPosition(v[q * 8]), p1 = vertexPosition(v[q * 8 + 2]), p2 = vertexPosition(v[q * 8 + 4]);
        const int face = static_cast<int>((v[q * 8] >> 15) & 7u);
        const Vec3 n = core::cross(p1 - p0, p2 - p0);
        const FaceAxes& axes = kFaces[static_cast<std::size_t>(face)];
        const float d = n.x * static_cast<float>(axes.n[0]) + n.y * static_cast<float>(axes.n[1]) + n.z * static_cast<float>(axes.n[2]);
        outward = outward && d > 0.5f;
        // Cada cara esta en el lado correcto del bloque.
        const Vec3 center = (p0 + p2) * 0.5f;
        const Vec3 expected{5.5f + 0.5f * static_cast<float>(axes.n[0]), 5.5f + 0.5f * static_cast<float>(axes.n[1]),
                            5.5f + 0.5f * static_cast<float>(axes.n[2])};
        outward = outward && core::length(center - expected) < 0.01f;
    }
    check(outward, "caras en sentido antihorario vistas desde fuera y en su sitio");

    // Dos bloques juntos: la cara de en medio no se dibuja.
    s.blocks[static_cast<std::size_t>(SectionSnapshot::index(6, 5, 5))] = block::Stone;
    meshSection(s, v);
    {
        // Losa de 16 x 16 de hierba: cada lado es un solo rectangulo.
        SectionSnapshot slab;
        slab.light.fill(0xF0);
        slab.grass_tint.fill(0x8C4);
        slab.foliage_tint.fill(0xFFF);
        for (int z = 0; z < 16; ++z)
            for (int x = 0; x < 16; ++x) slab.blocks[static_cast<std::size_t>(SectionSnapshot::index(x, 3, z))] = block::Grass;
        std::vector<std::uint32_t> sv;
        meshSection(slab, sv);
        std::printf("    losa 16x16: %zu quads\n", sv.size() / 8);
        check(sv.size() / 8 == 6, "losa de 16 x 16: 6 rectangulos");
    }
    // Las 10 caras de fuera, fusionadas (greedy meshing): 6 rectangulos que cubren lo mismo.
    check(v.size() == 6 * 4 * 2, "entre dos bloques no hay cara (y las de fuera se fusionan)");
    {
        float area = 0.0f;
        for (std::size_t q = 0; q < v.size() / 8; ++q) {
            const Vec3 p0 = vertexPosition(v[q * 8]), p1 = vertexPosition(v[q * 8 + 2]), p2 = vertexPosition(v[q * 8 + 4]);
            area += core::length(core::cross(p1 - p0, p2 - p0));
        }
        check(std::fabs(area - 10.0f) < 0.01f, "las caras fusionadas cubren las 10 caras de fuera");
    }
    // Oclusion: un bloque en el suelo junto a una pared oscurece esa esquina.
    s.blocks.fill(block::Air);
    s.blocks[static_cast<std::size_t>(SectionSnapshot::index(5, 5, 5))] = block::Stone;
    s.blocks[static_cast<std::size_t>(SectionSnapshot::index(6, 6, 5))] = block::Stone;
    meshSection(s, v);
    int darkened = 0;
    for (std::size_t q = 0; q < v.size() / 8; ++q) {
        if (((v[q * 8] >> 15) & 7u) != 2u) continue;  // cara de arriba del primero
        if (vertexPosition(v[q * 8]).y != 6.0f) continue;
        for (int k = 0; k < 4; ++k) darkened += ((v[q * 8 + static_cast<std::size_t>(k) * 2] >> 20) & 3u) < 3u ? 1 : 0;
    }
    check(darkened == 2, "oclusion en las esquinas junto a una pared");
    // Plantas: cruz por las dos caras.
    s.blocks.fill(block::Air);
    s.blocks[static_cast<std::size_t>(SectionSnapshot::index(1, 1, 1))] = block::RedFlower;
    meshSection(s, v);
    check(v.size() == 4 * 4 * 2, "una planta: dos planos por sus dos caras");
    // PASTO-HZ: la hierba es una mata de 3 laminas por sus dos caras (cara 7).
    s.blocks[static_cast<std::size_t>(SectionSnapshot::index(1, 1, 1))] = block::TallGrass;
    meshSection(s, v);
    bool blades = v.size() == 6 * 4 * 2;
    for (std::size_t q = 0; q < v.size() / 8; ++q) blades = blades && ((v[q * 8] >> 15) & 7u) == 7u;
    check(blades, "una mata de hierba: tres laminas por sus dos caras");
}

void makeWorld(ecs::World& world, int seed, int distance = 3) {
    VoxelWorld& vw = world.create("Mundo").add<VoxelWorld>();
    vw.seed = seed;
    vw.render_distance = distance;
    vw.textures.clear();
}

void testGeneration() {
    std::printf("Generacion y luz\n");
    registerVoxelComponents();
    ecs::World world;
    makeWorld(world, 424242);
    VoxelSystem vox;
    vox.start(world);
    check(vox.active(), "arranca con el VoxelWorld de la escena");
    const auto start = std::chrono::steady_clock::now();
    const Vec3 viewer{8.0f, 100.0f, 8.0f};
    vox.waitUntilReady(viewer, 3, 60.0f);
    const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
    const VoxelStats st = vox.stats();
    std::printf("  (%d chunks, %d secciones en %.2f s; chunk %.1f ms, seccion %.2f ms)\n", st.chunks, st.sections,
                static_cast<double>(seconds), static_cast<double>(st.last_chunk_ms), static_cast<double>(st.last_mesh_ms));
    check(vox.isReady(8, 8) && vox.isReady(-20, 30), "carga y malla los chunks de alrededor");

    const int h = vox.surfaceHeight(8, 8);
    const BlockId ground = vox.getBlock(8, h, 8);
    check(blockDef(ground).solid || ground == block::Water, "en la altura del terreno hay suelo");
    check(vox.getBlock(8, 0, 8) == block::Bedrock, "roca madre abajo del todo");
    check(vox.skyLight(8, 200, 8) == 15, "luz del cielo arriba");
    int dark = 0;
    for (int y = 5; y < 30; ++y) dark += (vox.getBlock(8, y, 8) != block::Air && vox.skyLight(8, y, 8) == 0) ? 1 : 0;
    check(dark > 10, "bajo tierra no llega el cielo");

    // Determinista: otro sistema con la misma semilla da lo mismo.
    VoxelSystem again;
    again.start(world);
    again.waitUntilReady(viewer, 1, 30.0f);
    bool same = true;
    for (int y = 0; y < 120 && same; y += 3) {
        for (int x = 0; x < 16 && same; ++x) same = vox.getBlock(x, y, 5) == again.getBlock(x, y, 5);
    }
    check(same, "la misma semilla genera el mismo mundo");

    // Un hueco cerrado bajo tierra: una caja de piedra (el terreno puede
    // tener cuevas justo ahi) con el interior vacio.
    const int y = std::max(8, h - 20);
    for (int dx = -3; dx <= 3; ++dx) {
        for (int dy = -2; dy <= 3; ++dy) {
            for (int dz = -3; dz <= 3; ++dz) {
                const bool shell = std::abs(dx) == 3 || dy == -2 || dy == 3 || std::abs(dz) == 3;
                vox.setBlock(8 + dx, y + dy, 8 + dz, shell ? block::Stone : block::Air);
            }
        }
    }
    vox.update(0.0f, viewer);
    check(vox.skyLight(8, y, 8) == 0, "un hueco cerrado bajo tierra esta a oscuras");
    vox.setBlock(8, y, 8, block::Torch);
    check(vox.blockLight(8, y, 8) == 14 && vox.blockLight(9, y, 8) == 13 && vox.blockLight(10, y, 8) == 12,
          "la antorcha alumbra y la luz baja con la distancia");
    vox.setBlock(8, y, 8, block::Air);
    check(vox.blockLight(9, y, 8) == 0, "al quitarla vuelve la oscuridad");
    // Abrir un pozo hasta el cielo: la luz del cielo baja.
    for (int yy = y + 3; yy <= h + 12; ++yy) vox.setBlock(8, yy, 8, block::Air);
    check(vox.skyLight(8, y + 2, 8) == 15, "un pozo abierto deja bajar la luz del cielo");
    check(vox.stats().pending_meshes > 0, "cambiar bloques rehace sus secciones");
    vox.waitUntilReady(viewer, 3, 30.0f);
    check(vox.stats().pending_meshes == 0, "y se vuelven a mallar");
}

void testQueries() {
    std::printf("Rayo y colision\n");
    ecs::World world;
    makeWorld(world, 777);
    VoxelSystem vox;
    vox.start(world);
    const Vec3 viewer{0.5f, 120.0f, 0.5f};
    vox.waitUntilReady(viewer, 1, 60.0f);
    const int h = vox.surfaceHeight(0, 0);
    // Quitar lo que haya encima (arboles, plantas) para probar sobre el suelo.
    for (int y = h + 1; y < h + 12; ++y) vox.setBlock(0, y, 0, block::Air);
    vox.setBlock(0, h, 0, block::Stone);
    const VoxelHit hit = vox.raycast(Vec3{0.5f, static_cast<float>(h) + 8.0f, 0.5f}, Vec3{0.0f, -1.0f, 0.0f}, 20.0f);
    check(hit.hit && hit.block == BlockPos{0, h, 0} && hit.normal == BlockPos{0, 1, 0}, "el rayo da en el suelo por arriba");
    check(!vox.raycast(Vec3{0.5f, static_cast<float>(h) + 8.0f, 0.5f}, Vec3{0.0f, 1.0f, 0.0f}, 20.0f).hit, "hacia el cielo no toca nada");

    // Caja del jugador (0.6 x 1.8) cayendo.
    const Vec3 half{0.3f, 0.9f, 0.3f};
    Vec3 p{0.5f, static_cast<float>(h) + 6.0f, 0.5f};
    bool ground = false;
    for (int i = 0; i < 200 && !ground; ++i) {
        const auto r = vox.moveBox(p, half, Vec3{0.0f, -0.3f, 0.0f});
        p = r.position;
        ground = r.on_ground;
    }
    check(ground && std::abs(p.y - (static_cast<float>(h) + 1.0f + 0.9f)) < 0.02f, "cae y se apoya en el bloque");
    // Pared delante: no la atraviesa ni a gran velocidad.
    vox.setBlock(3, h + 1, 0, block::Stone);
    vox.setBlock(3, h + 2, 0, block::Stone);
    const auto r = vox.moveBox(p, half, Vec3{10.0f, 0.0f, 0.0f});
    check(r.hit_wall && r.position.x < 3.0f - 0.29f, "choca con una pared (aunque vaya muy rapido)");
    check(vox.moveBox(p, half, Vec3{0.0f, 0.5f, 0.0f}).position.y > p.y, "puede saltar");
}

// COLISION-MODELOS (2026-11-09): vallas, sillas y mesas chocan con sus piezas (no con el cubo entero),
// el rayo pasa por sus huecos y da la caja de las piezas.
void testModelCollision() {
    std::printf("Colision de los bloques con modelo (vallas, sillas, mesas)\n");
    ecs::World world;
    makeWorld(world, 777);
    VoxelSystem vox;
    vox.start(world);
    const Vec3 viewer{8.5f, 120.0f, 20.5f};
    vox.waitUntilReady(viewer, 1, 60.0f);
    const int h = vox.surfaceHeight(8, 20);
    // Plataforma lisa de piedra en y = h (x 2..14, z 18..22) y aire encima.
    for (int x = 2; x <= 14; ++x) {
        for (int z = 18; z <= 22; ++z) {
            vox.setBlock(x, h, z, block::Stone);
            for (int y = h + 1; y <= h + 8; ++y) vox.setBlock(x, y, z, block::Air);
        }
    }
    const Vec3 half{0.3f, 0.9f, 0.3f};
    const float feet = static_cast<float>(h) + 1.0f + 0.9f + 0.01f;
    // Valla suelta en (8, h+1, 20): poste de 4/16 en el centro (x 8.375..8.625).
    vox.setBlock(8, h + 1, 20, block::Fence);
    const auto r1 = vox.moveBox(Vec3{5.5f, feet, 20.5f}, half, Vec3{6.0f, 0.0f, 0.0f});
    check(r1.hit_wall && r1.position.x > 8.375f - 0.3f - 0.06f && r1.position.x < 8.375f - 0.3f + 0.001f,
          "la valla para en su poste (no en el borde de la celda)");
    // Por el lado del poste (z 20.0: la caja va de 19.7 a 20.3, el poste empieza en 20.375): pasa.
    const auto r2 = vox.moveBox(Vec3{5.5f, feet, 20.0f}, half, Vec3{6.0f, 0.0f, 0.0f});
    check(!r2.hit_wall && r2.position.x > 11.0f, "se pasa junto al poste de una valla suelta");
    check(vox.boxCollides(Vec3{8.5f, feet, 20.5f}, half), "dentro del poste choca");
    // Rayo: por el hueco de la celda de la valla llega al suelo; por el centro da en el poste.
    const VoxelHit miss = vox.raycast(Vec3{8.1f, static_cast<float>(h) + 5.0f, 20.1f}, Vec3{0.0f, -1.0f, 0.0f}, 10.0f);
    check(miss.hit && miss.block == BlockPos{8, h, 20}, "el rayo pasa por el hueco de la valla y da en el suelo");
    const VoxelHit post = vox.raycast(Vec3{8.5f, static_cast<float>(h) + 5.0f, 20.5f}, Vec3{0.0f, -1.0f, 0.0f}, 10.0f);
    check(post.hit && post.block == BlockPos{8, h + 1, 20} && std::abs(post.box_min.x - 8.375f) < 1e-3f &&
              std::abs(post.box_max.y - (static_cast<float>(h) + 2.0f)) < 1e-3f && post.normal == BlockPos{0, 1, 0},
          "el rayo da en el poste con su caja (no el cubo entero)");
    // Dos vallas seguidas en z (travesanos): ya no se pasa entre ellas.
    vox.setBlock(8, h + 1, 21, block::Fence);
    check(vox.boxCollides(Vec3{8.5f, static_cast<float>(h) + 1.5f, 21.0f}, Vec3{0.05f, 0.3f, 0.05f}), "travesano entre dos vallas");
    vox.setBlock(8, h + 1, 20, block::Air);
    vox.setBlock(8, h + 1, 21, block::Air);
    // Silla: se queda sentado en el asiento (9/16), no encima del cubo entero.
    vox.setBlock(11, h + 1, 20, block::Chair);
    Vec3 p{11.5f, static_cast<float>(h) + 4.0f, 20.5f};
    bool ground = false;
    for (int i = 0; i < 100 && !ground; ++i) {
        const auto r = vox.moveBox(p, Vec3{0.15f, 0.4f, 0.15f}, Vec3{0.0f, -0.3f, 0.0f});
        p = r.position;
        ground = r.on_ground;
    }
    check(ground && p.y < static_cast<float>(h) + 1.0f + 9.0f / 16.0f + 0.4f + 0.02f && p.y > static_cast<float>(h) + 1.0f + 9.0f / 16.0f + 0.35f,
          "una caja pequena se apoya en el asiento de la silla");
    // Mesa: el tablero esta arriba (14/16); entre las patas, por debajo, cabe algo bajo.
    vox.setBlock(5, h + 1, 20, block::Table);
    check(vox.boxCollides(Vec3{5.5f, static_cast<float>(h) + 1.95f, 20.5f}, Vec3{0.2f, 0.02f, 0.2f}), "el tablero de la mesa choca");
    check(!vox.boxCollides(Vec3{5.5f, static_cast<float>(h) + 1.3f, 20.5f}, Vec3{0.25f, 0.25f, 0.25f}), "bajo la mesa, entre las patas, hay hueco");
    // Un cubo normal sigue igual.
    vox.setBlock(5, h + 1, 20, block::Stone);
    check(vox.boxCollides(Vec3{5.5f, static_cast<float>(h) + 1.3f, 20.5f}, Vec3{0.25f, 0.25f, 0.25f}), "un cubo normal choca entero");
    const VoxelHit cube = vox.raycast(Vec3{5.5f, static_cast<float>(h) + 5.0f, 20.5f}, Vec3{0.0f, -1.0f, 0.0f}, 10.0f);
    check(cube.hit && cube.block == BlockPos{5, h + 1, 20} && std::abs(cube.box_max.x - 6.0f) < 1e-3f, "caja de un cubo normal: la celda");
}

void testPhysics() {
    std::printf("Colision con la fisica (Jolt)\n");
    ecs::World world;
    makeWorld(world, 777);
    physics::PhysicsSystem physics;
    physics.start(world);
    VoxelSystem vox;
    vox.setPhysics(&physics);
    vox.start(world);
    const Vec3 viewer{0.5f, 120.0f, 0.5f};
    vox.waitUntilReady(viewer, 2, 60.0f);
    for (int i = 0; i < 400 && vox.stats().pending_colliders > 0; ++i) vox.update(0.0f, viewer);
    const VoxelStats st = vox.stats();
    std::printf("  (%d secciones con colision, %zu mallas en Jolt)\n", st.colliders, physics.staticMeshCount());
    check(st.colliders > 0 && st.pending_colliders == 0 && physics.staticMeshCount() == static_cast<std::size_t>(st.colliders),
          "las secciones cercanas tienen malla de colision");

    // Una columna despejada y un cubo con Rigidbody que cae sobre ella.
    const int h = vox.surfaceHeight(0, 0);
    for (int y = h + 1; y < h + 14; ++y) vox.setBlock(0, y, 0, block::Air);
    vox.setBlock(0, h, 0, block::Stone);
    ecs::Entity box = world.create("Caja");
    box.setWorldPosition(Vec3{0.5f, static_cast<float>(h) + 6.0f, 0.5f});
    box.add<physics::BoxCollider>();
    box.add<physics::Rigidbody>().interpolate = false;
    for (int i = 0; i < 180; ++i) {
        vox.update(1.0f / 60.0f, viewer);
        physics.update(world, 1.0f / 60.0f);
    }
    const float rest = box.worldPosition().y;
    std::printf("  (suelo %d, la caja reposa en y = %.2f)\n", h, static_cast<double>(rest));
    check(std::abs(rest - (static_cast<float>(h) + 1.5f)) < 0.1f, "un Rigidbody cae y se apoya en los bloques");
    // Un rayo de la fisica toca los bloques (sin entidad).
    physics::RaycastHit hit;
    check(physics.raycast(Vec3{3.5f, static_cast<float>(h) + 20.0f, 3.5f}, Vec3{0.0f, -1.0f, 0.0f}, 40.0f, hit) && !hit.entity.valid(),
          "Physics.raycast toca los bloques");
    // Romper los bloques de debajo: la malla se rehace y la caja sigue cayendo.
    for (int y = h - 4; y <= h; ++y) {
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dz = -1; dz <= 1; ++dz) vox.setBlock(dx, y, dz, block::Air);
        }
    }
    for (int i = 0; i < 120; ++i) {
        vox.update(1.0f / 60.0f, viewer);
        physics.update(world, 1.0f / 60.0f);
    }
    std::printf("  (tras cavar, y = %.2f)\n", static_cast<double>(box.worldPosition().y));
    check(box.worldPosition().y < rest - 2.0f, "al cavar debajo la caja cae (la colision se rehace)");
    // Reiniciar la fisica (Play en el editor) conserva la colision de los bloques.
    physics.stop();
    physics.start(world);
    check(physics.staticMeshCount() == static_cast<std::size_t>(vox.stats().colliders) &&
              physics.raycast(Vec3{3.5f, static_cast<float>(h) + 20.0f, 3.5f}, Vec3{0.0f, -1.0f, 0.0f}, 40.0f, hit),
          "al reiniciar la fisica los bloques siguen chocando");
    vox.stop();
    check(physics.staticMeshCount() == 0, "al parar el mundo se quitan sus mallas");
}

void testWater() {
    std::printf("Agua\n");
    // Buscar una costa: una columna de mar junto a tierra.
    ecs::World world;
    makeWorld(world, 31337, 4);
    VoxelSystem vox;
    vox.start(world);
    int sx = 0, sz = 0;
    bool found = false;
    for (int r = 0; r < 4000 && !found; r += 16) {
        for (int k = 0; k < 8 && !found; ++k) {
            const int x = static_cast<int>(std::cos(k * 0.785f) * r), z = static_cast<int>(std::sin(k * 0.785f) * r);
            if (vox.surfaceHeight(x, z) < 50) {
                sx = x;
                sz = z;
                found = true;
            }
        }
    }
    check(found, "hay oceanos");
    if (!found) return;
    vox.waitUntilReady(Vec3{static_cast<float>(sx), 80.0f, static_cast<float>(sz)}, 1, 60.0f);
    check(vox.getBlock(sx, 60, sz) == block::Water && vox.inWater(Vec3{sx + 0.5f, 60.5f, sz + 0.5f}),
          "bajo el nivel del mar hay agua (para nadar)");
    const int floor_y = vox.surfaceHeight(sx, sz);
    vox.setBlock(sx, floor_y, sz, block::Air);
    // El agua corre por pasos (cada 0.25 s): entra en el siguiente.
    vox.update(0.3f, Vec3{static_cast<float>(sx), 80.0f, static_cast<float>(sz)});
    check(block::isWater(vox.getBlock(sx, floor_y, sz)), "al cavar el fondo entra el agua");
}

void testSaving() {
    std::printf("Mundos guardados\n");
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_voxel_tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ecs::World world;
    makeWorld(world, 1);
    const Vec3 viewer{0.5f, 120.0f, 0.5f};
    int h = 0;
    {
        VoxelSystem vox;
        vox.setSaveRoot(root);
        vox.start(world);
        check(vox.newWorld("Mi mundo", 9876), "crear un mundo con nombre");
        vox.waitUntilReady(viewer, 1, 60.0f);
        h = vox.surfaceHeight(0, 0);
        vox.setBlock(0, h + 2, 0, block::Air);  // por si el origen cae en un lago (agua: quita 2 de luz)
        vox.setBlock(0, h + 1, 0, block::Glowstone);
        vox.setBlock(1, h + 1, 0, block::Bricks);
        vox.setMeta("mode", "creativo");
        vox.setMeta("player", "0.5 80 0.5");
        check(vox.saveWorld(), "guardarlo");
    }
    {
        VoxelSystem vox;
        vox.setSaveRoot(root);
        vox.start(world);
        const auto worlds = vox.listWorlds();
        check(worlds.size() == 1 && worlds[0].name == "Mi mundo" && worlds[0].seed == 9876 && worlds[0].mode == "creativo",
              "aparece en la lista con su semilla y su modo");
        check(vox.loadWorld("Mi mundo") && vox.seed() == 9876 && vox.meta("player") == "0.5 80 0.5", "cargarlo con sus datos");
        vox.waitUntilReady(viewer, 1, 60.0f);
        check(vox.getBlock(0, h + 1, 0) == block::Glowstone && vox.getBlock(1, h + 1, 0) == block::Bricks,
              "los bloques puestos siguen ahi");
        check(vox.blockLight(0, h + 2, 0) == 14, "y la luz se recalcula al cargar");
        check(vox.deleteWorld("Mi mundo") && vox.listWorlds().empty(), "borrarlo");
    }
    std::filesystem::remove_all(root, ec);
}

void testPerformance() {
    std::printf("Rendimiento\n");
    ecs::World world;
    makeWorld(world, 20260925, 8);
    VoxelSystem vox;
    vox.start(world);
    const Vec3 viewer{0.0f, 100.0f, 0.0f};
    const auto start = std::chrono::steady_clock::now();
    vox.waitUntilReady(viewer, 8, 120.0f);
    const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
    const VoxelStats st = vox.stats();
    std::printf("  (distancia 8: %d chunks, %d secciones con caras en %.2f s)\n", st.chunks, st.sections,
                static_cast<double>(seconds));
    check(seconds < 30.0f, "genera y malla una distancia de 8 chunks a tiempo");
    // El resto del anillo de carga llega frame a frame: el peor frame.
    float worst_ms = 0.0f;
    for (int i = 0; i < 20000 && (vox.stats().pending_generation > 0 || vox.stats().pending_meshes > 0); ++i) {
        const auto t = std::chrono::steady_clock::now();
        vox.update(0.016f, viewer);
        worst_ms = std::max(worst_ms, std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t).count());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::printf("  (peor frame mientras carga %.2f ms)\n", static_cast<double>(worst_ms));
    check(worst_ms < 16.0f, "cargar no congela el frame");
    // Un frame sin nada que hacer.
    const auto t = std::chrono::steady_clock::now();
    vox.update(0.016f, viewer);
    const float frame_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t).count();
    std::printf("  (frame sin cambios %.2f ms)\n", static_cast<double>(frame_ms));
    check(frame_ms < 4.0f, "el frame sin cambios es barato");
}

#include "VoxelStructureTests.inc"

// Huella (FNV-1a) de unos chunks: detecta cualquier cambio del terreno.
std::uint64_t chunkHash(const Generator& gen, int cx0, int cz0, int n) {
    std::uint64_t h = 1469598103934665603ull;
    for (int dz = 0; dz < n; ++dz) {
        for (int dx = 0; dx < n; ++dx) {
            ChunkData chunk;
            chunk.cx = cx0 + dx;
            chunk.cz = cz0 + dz;
            gen.generate(chunk);
            for (BlockId id : chunk.blocks) h = (h ^ id) * 1099511628211ull;
        }
    }
    return h;
}

bool isTreeBlock(BlockId id) {
    return id == block::OakLog || id == block::OakLeaves || id == block::BirchLog || id == block::BirchLeaves ||
           id == block::SpruceLog || id == block::SpruceLeaves || id == block::Cactus;
}

// Bloques de arbol en n x n chunks (solo los chunks sin estructuras cerca: las
// aldeas tienen sus propios arboles plantados).
int treeBlocks(const Generator& gen, int cx0, int cz0, int n, int* chunks_counted = nullptr) {
    int count = 0, counted = 0;
    for (int dz = 0; dz < n; ++dz) {
        for (int dx = 0; dx < n; ++dx) {
            ChunkData chunk;
            chunk.cx = cx0 + dx;
            chunk.cz = cz0 + dz;
            if (gen.structures() != nullptr) {
                const float x = static_cast<float>(chunk.cx * kChunkSize + 8), z = static_cast<float>(chunk.cz * kChunkSize + 8);
                if (!gen.structures()->nearby(x, 64.0f, z, 40.0f).empty()) continue;
            }
            gen.generate(chunk);
            ++counted;
            for (BlockId id : chunk.blocks) count += isTreeBlock(id) ? 1 : 0;
        }
    }
    if (chunks_counted != nullptr) *chunks_counted = counted;
    return count;
}

// VoxelWorld::tree_density y clearings: por defecto el mundo de siempre (huella
// tomada antes de existir los campos); 0 = sin arboles; menos de 1, menos
// arboles y en manchas. Y waterLevel con el suelo aplanado por las aldeas.
void testVegetation() {
    std::printf("Densidad de arboles\n");
    Generator adv(1234, 62, VoxelGenerator::Adventure);
    Generator classic(1234, 62, VoxelGenerator::Classic);
    adv.setCaveEntrances(0);  // las huellas son de antes de las entradas de cueva (testCaveEntrances)
    adv.setImmenseCaves(false);  // y de antes de las cuevas inmensas (testImmenseCaves)
    adv.setDeepOceans(false);    // y de antes de los oceanos hondos (testOceans)
    adv.setArcaniteOre(false);   // y de antes de la arcanita (testRails)
    adv.setLushBiomes(false);    // NOCHE-MUNDO: y de antes de los biomas frondosos (testLush)
    classic.setArcaniteOre(false);
    // Un bosque de cada generador (los arboles cuentan en la huella).
    BlockPos forest_adv{}, forest_classic{};
    for (int i = 0; i < 4096; ++i) {
        const int x = (i % 64 - 32) * 64, z = (i / 64 - 32) * 64;
        if (forest_adv.y == 0 && adv.adventureColumn(x, z).biome == Generator::AForest) forest_adv = BlockPos{x, 1, z};
        if (forest_classic.y == 0 && classic.terrainHeight(x, z) > 66 && classic.terrainHeight(x, z) < 90 &&
            (i % 7) == 3) {
            forest_classic = BlockPos{x, 1, z};
        }
    }
    const auto chunkOf = [](int v) { return static_cast<int>(std::floor(static_cast<float>(v) / kChunkSize)); };
    const int fax = chunkOf(forest_adv.x), faz = chunkOf(forest_adv.z);
    const int fcx = chunkOf(forest_classic.x), fcz = chunkOf(forest_classic.z);
    const auto hashes = [&]() {
        return std::array<std::uint64_t, 4>{chunkHash(adv, -3, -3, 6), chunkHash(classic, -3, -3, 6),
                                            chunkHash(adv, fax - 2, faz - 2, 5), chunkHash(classic, fcx - 2, fcz - 2, 5)};
    };
    // Huellas de antes de tree_density / clearings (semilla 1234).
    const std::array<std::uint64_t, 4> before = {4581862800975166454ull, 8828485424106083356ull, 10133045093403751905ull,
                                                 15734742874848356318ull};
    const std::array<std::uint64_t, 4> now = hashes();
    std::printf("    huellas %llu %llu %llu %llu\n", static_cast<unsigned long long>(now[0]), static_cast<unsigned long long>(now[1]),
                static_cast<unsigned long long>(now[2]), static_cast<unsigned long long>(now[3]));
    check(now == before, "densidad 1, sin claros y sin entradas de cueva: el mundo de siempre, bloque a bloque");
    adv.setVegetation(1.0f, 0.0f);
    classic.setVegetation(1.0f, 0.0f);
    check(hashes() == before, "setVegetation(1, 0) explicito: igual");
    const VoxelWorld defaults;
    check(defaults.tree_density == 1.0f && defaults.clearings == 0.0f, "el componente por defecto deja 1 y 0");

    const int n = 10;
    int counted_adv = 0, counted_classic = 0;
    const int full_adv = treeBlocks(adv, fax - n / 2, faz - n / 2, n, &counted_adv);
    const int full_classic = treeBlocks(classic, fcx - n / 2, fcz - n / 2, n, &counted_classic);
    adv.setVegetation(0.0f, 0.0f);
    classic.setVegetation(0.0f, 0.0f);
    const int none_adv = treeBlocks(adv, fax - n / 2, faz - n / 2, n);
    const int none_classic = treeBlocks(classic, fcx - n / 2, fcz - n / 2, n);
    check(full_adv > 1000 && full_classic > 100 && counted_adv > 20, "con densidad 1 hay arboles en la muestra");
    check(none_adv == 0 && none_classic == 0, "densidad 0: ningun arbol (fuera de las estructuras)");
    adv.setVegetation(0.45f, 0.5f);
    classic.setVegetation(0.45f, 0.5f);
    const int some_adv = treeBlocks(adv, fax - n / 2, faz - n / 2, n);
    const int some_classic = treeBlocks(classic, fcx - n / 2, fcz - n / 2, n);
    adv.setVegetation(0.45f, 0.0f);
    const int patches_adv = treeBlocks(adv, fax - n / 2, faz - n / 2, n);
    adv.setVegetation(1.0f, 0.6f);
    const int glades_adv = treeBlocks(adv, fax - n / 2, faz - n / 2, n);
    adv.setVegetation(2.0f, 0.0f);
    const int dense_adv = treeBlocks(adv, fax - n / 2, faz - n / 2, n);
    std::printf("    bloques de arbol aventura: 1 -> %d, 0.45 -> %d, 0.45+claros 0.5 -> %d, 1+claros 0.6 -> %d, 2 -> %d, 0 -> %d "
                "(%d chunks); clasico: 1 -> %d, 0.45+0.5 -> %d\n",
                full_adv, patches_adv, some_adv, glades_adv, dense_adv, none_adv, counted_adv, full_classic, some_classic);
    check(some_adv > 0 && some_adv < full_adv * 6 / 10 && patches_adv < full_adv * 7 / 10 && glades_adv < full_adv,
          "densidad 0.45 y claros: bastantes menos arboles, pero quedan");
    check(dense_adv > full_adv, "densidad 2: mas arboles");
    check(some_classic < full_classic, "el Clasico tambien");
    // Que parte de los arboles queda y cuanto suelo queda abierto (muestra de 8 x 8 km).
    {
        const auto sample = [&](float d, float c) {
            adv.setVegetation(d, c);
            double sum = 0.0;
            int open = 0, total = 0;
            for (int z = -4000; z < 4000; z += 23) {
                for (int x = -4000; x < 4000; x += 23) {
                    const float k = adv.treeChance(0.5f, x, z) / 0.5f;
                    sum += k;
                    open += k < 0.1f ? 1 : 0;
                    ++total;
                }
            }
            std::printf("    densidad %.2f claros %.2f: quedan %.0f%% de los arboles, %.0f%% del bosque abierto\n", d, c,
                        100.0 * sum / total, 100.0 * open / total);
            return sum / total;
        };
        const double r07 = sample(0.7f, 0.0f), r045 = sample(0.45f, 0.0f), r045c = sample(0.45f, 0.5f);
        const double r02 = sample(0.2f, 0.0f), r1c = sample(1.0f, 0.5f), r15 = sample(1.5f, 0.0f);
        check(r07 > r045 && r045 > r02 && r045 > 0.25 && r045 < 0.6 && r045c < r045 && r1c < 0.95 && r15 > 1.2,
              "la densidad baja los arboles de forma gradual y los claros quitan mas");
    }
    ChunkData a, b;
    a.cx = b.cx = fax;
    a.cz = b.cz = faz;
    adv.setVegetation(0.45f, 0.5f);
    adv.generate(a);
    Generator adv2(1234, 62, VoxelGenerator::Adventure);
    adv2.setVegetation(0.45f, 0.5f);
    adv2.setCaveEntrances(0);
    adv2.setImmenseCaves(false);
    adv2.setDeepOceans(false);
    adv2.setArcaniteOre(false);
    adv2.setLushBiomes(false);  // NOCHE-MUNDO: como `adv`
    adv2.generate(b);
    check(a.blocks == b.blocks, "con densidad y claros sigue siendo determinista");

    // waterLevel: donde una aldea aplano el suelo por encima del mar, columna seca.
    const Generator plain(1234, 62, VoxelGenerator::Adventure);
    bool dry_ok = true;
    int flattened = 0;
    for (int z = -200; z <= 200 && dry_ok; z += 3) {
        for (int x = -200; x <= 200; x += 3) {
            const int ground = plain.terrainHeight(x, z), water = plain.waterLevel(x, z);
            if (ground != plain.adventureColumn(x, z).height) ++flattened;
            if (water != 0 && water <= ground) {
                dry_ok = false;
                std::printf("    (%d, %d) agua %d con suelo %d\n", x, z, water, ground);
                break;
            }
        }
    }
    std::printf("    origen: suelo %d agua %d (%d columnas aplanadas)\n", plain.terrainHeight(0, 0), plain.waterLevel(0, 0),
                flattened);
    check(dry_ok && flattened > 0, "waterLevel: 0 donde el suelo final (aplanado) queda por encima del agua");
}


// VoxelWorld::cave_entrances: en 1 km2 hay entradas (bocas, simas, grietas) por
// las que se baja andando de la superficie a la cueva (sin escalones de mas de
// 1 bloque), sin agua dentro, y salen igual con la misma semilla.
void testCaveEntrances() {
    std::printf("Entradas de cueva\n");
    Generator gen(1234, 62, VoxelGenerator::Adventure);
    const VoxelWorld defaults;
    check(defaults.cave_entrances == 1 && gen.caveEntrances() == 1, "por defecto 1 (componente y generador)");
    constexpr int kX0 = -512, kZ0 = -512, kSide = 1000;
    const std::vector<Generator::CaveEntranceInfo> list = gen.caveEntrancesIn(kX0, kZ0, kX0 + kSide, kZ0 + kSide);
    int kinds[3] = {0, 0, 0}, connected = 0;
    for (const auto& e : list) {
        ++kinds[e.kind];
        connected += e.connected ? 1 : 0;
    }
    std::printf("    %zu entradas en 1 km2 (bocas %d, simas %d, grietas %d; %d llegan a una cueva natural)\n", list.size(), kinds[0],
                kinds[1], kinds[2], connected);
    check(list.size() >= 25 && list.size() <= 90, "entre 25 y 90 entradas por km2 (una cada ~110-200 m)");

    // Determinista: otro generador con la misma semilla da las mismas.
    {
        Generator other(1234, 62, VoxelGenerator::Adventure);
        const auto again = other.caveEntrancesIn(kX0, kZ0, kX0 + kSide, kZ0 + kSide);
        bool same = again.size() == list.size();
        for (std::size_t i = 0; same && i < list.size(); ++i) {
            same = again[i].x == list[i].x && again[i].y == list[i].y && again[i].z == list[i].z && again[i].end_x == list[i].end_x &&
                   again[i].end_y == list[i].end_y && again[i].kind == list[i].kind;
        }
        check(same, "las entradas son deterministas (misma semilla, mismas entradas)");
        if (!list.empty()) {
            ChunkData a, b;
            a.cx = b.cx = static_cast<int>(std::floor(static_cast<float>(list[0].x) / kChunkSize));
            a.cz = b.cz = static_cast<int>(std::floor(static_cast<float>(list[0].z) / kChunkSize));
            gen.generate(a);
            other.generate(b);
            check(a.blocks == b.blocks, "el chunk de una entrada sale igual en dos generadores");
        }
    }

    // Recorrido andando: de la superficie (cielo abierto, junto al inicio) a un
    // hueco con techo a 8 o mas bloques bajo el terreno.
    int walkable = 0, wet = 0;
    for (const auto& e : list) {
        const int margin = 6;
        const int cx0 = static_cast<int>(std::floor(static_cast<float>(e.min_x - margin) / kChunkSize));
        const int cz0 = static_cast<int>(std::floor(static_cast<float>(e.min_z - margin) / kChunkSize));
        const int cx1 = static_cast<int>(std::floor(static_cast<float>(e.max_x + margin) / kChunkSize));
        const int cz1 = static_cast<int>(std::floor(static_cast<float>(e.max_z + margin) / kChunkSize));
        const int ncx = cx1 - cx0 + 1, ncz = cz1 - cz0 + 1;
        std::vector<ChunkData> chunks(static_cast<std::size_t>(ncx * ncz));
        {
            std::atomic<int> next{0};
            std::vector<std::thread> workers;
            for (int t = 0; t < 8; ++t) {
                workers.emplace_back([&]() {
                    for (int i = next++; i < ncx * ncz; i = next++) {
                        ChunkData& c = chunks[static_cast<std::size_t>(i)];
                        c.cx = cx0 + i % ncx;
                        c.cz = cz0 + i / ncx;
                        gen.generate(c);
                    }
                });
            }
            for (std::thread& w : workers) w.join();
        }
        const int bx0 = cx0 * kChunkSize, bz0 = cz0 * kChunkSize, bw = ncx * kChunkSize, bd = ncz * kChunkSize;
        const auto get = [&](int x, int y, int z) -> BlockId {
            if (y < 0) return block::Bedrock;
            if (y >= kWorldHeight) return block::Air;
            const int lx = x - bx0, lz = z - bz0;
            if (lx < 0 || lz < 0 || lx >= bw || lz >= bd) return block::Stone;
            return chunks[static_cast<std::size_t>(lx / kChunkSize + (lz / kChunkSize) * ncx)].get(lx % kChunkSize, y, lz % kChunkSize);
        };
        const auto solid = [&](int x, int y, int z) { return blockDef(get(x, y, z)).solid; };
        const auto open = [&](int x, int y, int z) {
            const BlockId id = get(x, y, z);
            return !blockDef(id).solid && !block::isWater(id);
        };
        const auto stand = [&](int x, int y, int z) { return y > 0 && open(x, y, z) && open(x, y + 1, z) && solid(x, y - 1, z); };
        // Agua dentro de lo que excava: no deberia haber.
        bool has_water = false;
        for (int z = e.min_z; z <= e.max_z && !has_water; ++z) {
            for (int x = e.min_x; x <= e.max_x && !has_water; ++x) {
                const int h = gen.adventureColumn(x, z).height;
                for (int y = std::max(1, e.end_y - 4); y <= h && !has_water; ++y) has_water = block::isWater(get(x, y, z));
            }
        }
        wet += has_water ? 1 : 0;
        // Busqueda en anchura por los sitios donde se puede estar de pie.
        std::vector<std::uint8_t> seen(static_cast<std::size_t>(bw) * bd * kWorldHeight, 0);
        const auto idx = [&](int x, int y, int z) {
            return static_cast<std::size_t>(x - bx0) + static_cast<std::size_t>(z - bz0) * bw + static_cast<std::size_t>(y) * bw * bd;
        };
        std::vector<BlockPos> queue;
        for (int dz = -3; dz <= 3; ++dz) {
            for (int dx = -3; dx <= 3; ++dx) {
                const int x = e.x + dx, z = e.z + dz;
                for (int y = kWorldHeight - 3; y > 1; --y) {
                    if (solid(x, y, z) || block::isWater(get(x, y, z))) {
                        if (stand(x, y + 1, z) && !seen[idx(x, y + 1, z)]) {
                            seen[idx(x, y + 1, z)] = 1;
                            queue.push_back({x, y + 1, z});
                        }
                        break;
                    }
                }
            }
        }
        bool reached = false;
        for (std::size_t q = 0; q < queue.size() && !reached; ++q) {
            const BlockPos p = queue[q];
            if (p.y <= gen.adventureColumn(p.x, p.z).height - 8) {
                bool roof = false;
                for (int y = p.y + 2; y < kWorldHeight && !roof; ++y) roof = solid(p.x, y, p.z);
                if (roof) {
                    reached = true;
                    break;
                }
            }
            static constexpr int kDirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto& d : kDirs) {
                const int nx = p.x + d[0], nz = p.z + d[1];
                if (nx < bx0 || nz < bz0 || nx >= bx0 + bw || nz >= bz0 + bd) continue;
                for (int dy = -1; dy <= 1; ++dy) {
                    const int ny = p.y + dy;
                    if (ny < 1 || ny + 2 >= kWorldHeight) continue;
                    if (!stand(nx, ny, nz) || seen[idx(nx, ny, nz)]) continue;
                    if (dy > 0 && !open(p.x, p.y + 2, p.z)) continue;  // subir: hueco para la cabeza
                    if (dy < 0 && !open(nx, p.y + 1, nz)) continue;    // bajar: se entra de pie
                    seen[idx(nx, ny, nz)] = 1;
                    queue.push_back({nx, ny, nz});
                }
            }
        }
        walkable += reached ? 1 : 0;
        if (!reached) std::printf("    sin recorrido: tipo %d en (%d, %d, %d)\n", e.kind, e.x, e.y, e.z);
    }
    std::printf("    %d de %zu se recorren andando hasta la cueva; %d con agua\n", walkable, list.size(), wet);
    check(walkable >= 20 && walkable * 10 >= static_cast<int>(list.size()) * 8,
          "al menos 20 entradas por km2 (y el 80%) se bajan andando, escalones de 1 bloque, hasta la cueva");
    check(wet == 0, "ninguna entrada tiene agua dentro");

    // Media en 16 km2 (con rios y aldeas incluidos; por km2 de tierra: lejos del origen
    // hay oceanos grandes, testOceans) y en las montanas, mas.
    {
        const auto wide = gen.caveEntrancesIn(-6000, 2000, -2000, 6000);
        int land = 0, land_n = 0;
        for (int z = 2000; z < 6000; z += 40) {
            for (int x = -6000; x < -2000; x += 40, ++land_n) land += gen.adventureColumn(x, z).biome != Generator::AOcean ? 1 : 0;
        }
        const double land_km2 = std::max(1.0, 16.0 * land / std::max(1, land_n));
        const double per_km2 = static_cast<double>(wide.size()) / land_km2;
        std::printf("    media en 16 km2 (%.1f km2 de tierra): %.1f entradas por km2 (una cada %.0f m)\n", land_km2, per_km2,
                    wide.empty() ? 0.0 : std::sqrt(land_km2 * 1.0e6 / static_cast<double>(wide.size())));
        check(per_km2 >= 20.0 && per_km2 <= 70.0, "media de una entrada cada ~120-220 m");
        // El km2 mas montanoso de una muestra de 8 x 8 km.
        int mountain_x = 0, mountain_z = 0, best = -1;
        for (int i = 0; i < 1024; ++i) {
            const int x = (i % 32 - 16) * 256, z = (i / 32 - 16) * 256;
            int high = 0;
            for (int k = 0; k < 25; ++k) {
                const Generator::AdventureColumn c = gen.adventureColumn(x - 400 + (k % 5) * 200, z - 400 + (k / 5) * 200);
                high += c.biome == Generator::AMountains ? 1 : 0;
            }
            if (high > best) best = high, mountain_x = x, mountain_z = z;
        }
        const auto peaks = gen.caveEntrancesIn(mountain_x - 500, mountain_z - 500, mountain_x + 500, mountain_z + 500);
        std::printf("    montana en (%d, %d) (%d/25 de montana): %zu entradas en 1 km2\n", mountain_x, mountain_z, best, peaks.size());
        check(static_cast<double>(peaks.size()) > per_km2 * 1.3, "en las montanas hay mas entradas");
    }

    // Sin las entradas nuevas (0, el mundo de antes): la superficie casi nunca se abre.
    gen.setCaveEntrances(0);
    {
        int open_columns = 0;
        for (int cz = 0; cz < 12; ++cz) {
            for (int cx = 0; cx < 12; ++cx) {
                ChunkData chunk;
                chunk.cx = kX0 / kChunkSize + cx * 5;
                chunk.cz = kZ0 / kChunkSize + cz * 5;
                gen.generate(chunk);
                for (int z = 0; z < kChunkSize; ++z) {
                    for (int x = 0; x < kChunkSize; ++x) {
                        const Generator::AdventureColumn c = gen.adventureColumn(chunk.cx * kChunkSize + x, chunk.cz * kChunkSize + z);
                        if (c.water <= c.height && chunk.get(x, c.height, z) == block::Air) ++open_columns;
                    }
                }
            }
        }
        std::printf("    con 0: %d columnas abiertas a la cueva en 144 chunks\n", open_columns);
    }
    // 0: ninguna; 2: mas.
    const std::size_t none = gen.caveEntrancesIn(kX0, kZ0, kX0 + kSide, kZ0 + kSide).size();
    gen.setCaveEntrances(2);
    const std::size_t more = gen.caveEntrancesIn(kX0, kZ0, kX0 + kSide, kZ0 + kSide).size();
    std::printf("    cave_entrances 0 -> %zu, 1 -> %zu, 2 -> %zu\n", none, list.size(), more);
    check(none == 0 && more > list.size() * 3 / 2, "cave_entrances 0 = ninguna, 2 = bastantes mas");
}

}  // namespace

int runDimensionTests();  // VoxelDimensionTests.cpp (devuelve los fallos)
int runDeepRealmTests();  // VoxelDimensionTests.cpp: solo el Reino Profundo (CRAMION_DEEP_ONLY)

// Terreno lejano (FarTerrain, estilo Distant Horizons): las columnas del
// generador coinciden con los chunks de verdad, las mallas estan bien
// formadas y todo un radio de varios km se construye en los hilos.
void testFarTerrain() {
    std::printf("Terreno lejano\n");
    auto gen = std::make_shared<Generator>(1234, 62, VoxelGenerator::Adventure);
    gen->setVegetation(0.45f, 0.5f);
    // 1) Columnas: la altura del suelo como la del chunk generado.
    int same = 0, total = 0, water_same = 0;
    for (int c = 0; c < 6; ++c) {
        ChunkData chunk;
        chunk.cx = c * 37 - 90;
        chunk.cz = c * -23 + 40;
        gen->generate(chunk);
        for (int z = 0; z < kChunkSize; z += 3) {
            for (int x = 0; x < kChunkSize; x += 3) {
                const int wx = chunk.cx * kChunkSize + x, wz = chunk.cz * kChunkSize + z;
                Generator::FarColumn col;
                gen->farColumn(wx, wz, col);
                int top = -1;  // ultimo bloque solido opaco que no es arbol (debajo de la isla flotante, si hay)
                for (int y = col.island_top > 0 ? col.island_bottom - 1 : kWorldHeight - 1; y >= 0 && top < 0; --y) {
                    const BlockId id = chunk.get(x, y, z);
                    const BlockDef& def = blockDef(id);
                    if (!def.solid || !def.opaque || id == block::OakLog || id == block::OakLeaves || id == block::SpruceLeaves ||
                        id == block::BirchLog || id == block::BirchLeaves || id == block::SpruceLog || id == block::Cactus) continue;
                    top = y;
                }
                ++total;
                if (std::abs(top - col.height) <= 1) ++same;
                const bool real_water = block::isWater(chunk.get(x, col.height + 1, z));
                if (real_water == (col.water > col.height)) ++water_same;
            }
        }
    }
    std::printf("  (altura igual en %d de %d columnas, agua en %d)\n", same, total, water_same);
    check(same * 10 >= total * 9, "la altura lejana coincide con el chunk generado (>= 90%)");
    check(water_same * 10 >= total * 9, "el agua lejana coincide con la del chunk (>= 90%)");

    // 2) Una tesela por nivel: tiempo y forma de la malla.
    auto palette = std::make_shared<FarPalette>();
    bool well_formed = true;
    for (int level : {0, 3, 6, 9}) {
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<Generator::FarColumn> cols;
        FarTerrain::sampleTile(*gen, level, 3, -2, cols);
        const auto t1 = std::chrono::steady_clock::now();
        std::vector<std::uint32_t> v;
        FarTerrain::meshTile(cols, level, *palette, true, v);
        const auto t2 = std::chrono::steady_clock::now();
        std::printf("  (nivel %d: columnas %.2f ms, malla %.2f ms, %zu caras)\n", level,
                    std::chrono::duration<double, std::milli>(t1 - t0).count(), std::chrono::duration<double, std::milli>(t2 - t1).count(),
                    v.size() / 8);
        well_formed = well_formed && v.size() % 8 == 0 && v.size() / 8 >= 256 && v.size() / 8 < gfx::VoxelPass::kMaxQuadsPerSection;
        for (std::size_t i = 0; i + 1 < v.size(); i += 2) {
            const std::uint32_t a = v[i], b = v[i + 1];
            const std::uint32_t x = a & 31u, z = (a >> 10u) & 31u, y = ((a >> 5u) & 31u) | (((a >> 23u) & 15u) << 5u);
            if (x > 16 || z > 16 || y > 300 || ((a >> 15u) & 7u) > 5 || (b & (1u << 30)) != 0) well_formed = false;
        }
    }
    check(well_formed, "mallas lejanas bien formadas (posiciones, caras, sin bit fino)");

    // 3) Todo un radio de 4 km en los hilos, y otra vez desde la cache del disco.
    FarTerrain::Settings settings;
    settings.distance = 4096.0f;
    if (const char* e = std::getenv("CRAMION_FAR_DISTANCE")) settings.distance = static_cast<float>(std::atof(e));
    if (const char* e = std::getenv("CRAMION_FAR_DETAIL")) settings.detail = static_cast<float>(std::atof(e));
    for (int pass = 0; pass < 2; ++pass) {
        FarTerrain far_terrain;
        far_terrain.reset(gen);
        far_terrain.setPalette(palette);
        const auto start = std::chrono::steady_clock::now();
        double worst_update = 0.0;
        std::size_t bytes = 0;
        std::vector<FarTerrain::Update> updates;
        for (int frame = 0; frame < 20000; ++frame) {
            const auto u0 = std::chrono::steady_clock::now();
            far_terrain.update(Vec3{100.0f, 80.0f, -300.0f}, 512.0f, settings);
            far_terrain.takeUpdates(updates, 1536u * 1024u);
            worst_update = std::max(worst_update, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - u0).count());
            for (const auto& u : updates) bytes += u.vertices.size() * 4;
            updates.clear();
            const FarTerrain::Stats st = far_terrain.stats();
            if (st.tiles > 0 && st.pending == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const FarTerrain::Stats st = far_terrain.stats();
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("  (%s: %d teselas en %.2f s, %llu caras, %.1f MB; peor frame del hilo principal %.2f ms; cache %d, generadas %d)\n",
                    pass == 0 ? "primera vez" : "desde el disco", st.tiles, seconds, static_cast<unsigned long long>(st.quads),
                    static_cast<double>(bytes) / (1024.0 * 1024.0), worst_update, st.from_cache, st.generated);
        check(st.tiles > 100 && st.pending == 0, pass == 0 ? "4 km de terreno lejano construidos" : "y otra vez desde la cache");
        check(worst_update < 4.0, "el hilo principal no se atasca (< 4 ms por frame)");
        if (pass == 1) check(st.from_cache > 0, "la segunda vez lee la cache del disco");
    }
}

// Tierras salvajes (GeneratorWild.cpp): nada cerca del origen; lejos, cordilleras
// gigantes, islas flotantes y los biomas peligrosos, tambien en el terreno lejano.
void testWild() {
    std::printf("Tierras salvajes\n");
    const Generator gen(1234, 62, VoxelGenerator::Adventure);
    // 1) Cerca del origen, nada salvaje.
    bool calm = true;
    for (int z = -1300; z <= 1300; z += 37) {
        for (int x = -1300; x <= 1300; x += 37) {
            if (x * x + z * z > 1350 * 1350) continue;
            const Generator::AdventureColumn c = gen.adventureColumn(x, z);
            calm = calm && c.region == Generator::WNone && !c.giant && c.lava == 0 && c.feature == 0 && c.biome < Generator::ABadlands;
        }
    }
    check(calm, "a menos de 1,3 km del origen el mundo es el de siempre");

    // 2) Lo mas cercano de cada cosa (rejilla de 32 m hasta 9 km).
    constexpr int kKinds = 6;  // carmesi, rosa, ceniza, marchito, gigante, isla
    const char* names[kKinds] = {"Tierras Carmesi", "Desierto Rosa", "Paramo de Ceniza", "Tierras Marchitas", "cordillera gigante",
                                 "isla flotante"};
    std::array<float, kKinds> nearest{};
    std::array<BlockPos, kKinds> where{};
    nearest.fill(1e9f);
    int max_height = 0, lava_columns = 0;
    BlockPos peak{};
    for (int z = -9000; z <= 9000; z += 32) {
        for (int x = -9000; x <= 9000; x += 32) {
            const float d = std::sqrt(static_cast<float>(x) * static_cast<float>(x) + static_cast<float>(z) * static_cast<float>(z));
            if (d < 1300.0f) continue;
            const Generator::AdventureColumn c = gen.adventureColumn(x, z);
            int kind = -1;
            if (c.strength < 0.9f) kind = -1;  // dentro de la provincia, no en el borde
            else if (c.biome == Generator::ABadlands) kind = 0;
            else if (c.biome == Generator::ARoseDesert) kind = 1;
            else if (c.biome == Generator::AAshlands) kind = 2;
            else if (c.biome == Generator::ABlight) kind = 3;
            else if (c.giant && c.height > 62 + 120) kind = 4;
            if (kind >= 0 && d < nearest[static_cast<std::size_t>(kind)]) {
                nearest[static_cast<std::size_t>(kind)] = d;
                where[static_cast<std::size_t>(kind)] = BlockPos{x, c.height, z};
            }
            if (c.lava > c.height) ++lava_columns;
            if (c.height > max_height) {
                max_height = c.height;
                peak = BlockPos{x, c.height, z};
            }
            if (((x + 9000) % 128) == 0 && ((z + 9000) % 128) == 0) {
                Generator::FarColumn f;
                gen.farColumn(x, z, f, false);
                if (f.island_top > 0 && d < nearest[5]) {
                    nearest[5] = d;
                    where[5] = BlockPos{x, f.island_top, z};
                }
            }
        }
    }
    bool found = true;
    for (int k = 0; k < kKinds; ++k) {
        const auto i = static_cast<std::size_t>(k);
        std::printf("    %s: a %.0f m (%d, %d, %d)\n", names[k], static_cast<double>(nearest[i]), where[i].x, where[i].y, where[i].z);
        found = found && nearest[i] < 9000.0f;
    }
    std::printf("    pico mas alto y=%d en (%d, %d); columnas con lava %d\n", max_height, peak.x, peak.z, lava_columns);
    check(found, "hay de todo a menos de 9 km (cuatro biomas peligrosos, cordilleras e islas)");
    check(max_height >= 62 + 150 && max_height < kWorldHeight - 5, "cordilleras gigantes de mas de 150 m sobre el mar, dentro del mundo");
    check(lava_columns > 0, "charcas y crateres de lava");

    // 3) Chunks de cada sitio: los bloques nuevos, determinismo y tiempo.
    std::array<int, block::Count> count{};
    double worst_ms = 0.0, total_ms = 0.0;
    int chunks = 0;
    bool same = true;
    for (int k = 0; k < kKinds; ++k) {
        if (nearest[static_cast<std::size_t>(k)] >= 9000.0f) continue;
        const BlockPos at = where[static_cast<std::size_t>(k)];
        for (int dz = -3; dz <= 3; ++dz) {
            for (int dx = -3; dx <= 3; ++dx) {
                ChunkData chunk;
                chunk.cx = static_cast<int>(std::floor(static_cast<float>(at.x) / kChunkSize)) + dx;
                chunk.cz = static_cast<int>(std::floor(static_cast<float>(at.z) / kChunkSize)) + dz;
                const auto t0 = std::chrono::steady_clock::now();
                gen.generate(chunk);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                worst_ms = std::max(worst_ms, ms);
                total_ms += ms;
                ++chunks;
                for (BlockId id : chunk.blocks) ++count[id];
                if (dx == 0 && dz == 0) {
                    ChunkData again;
                    again.cx = chunk.cx;
                    again.cz = chunk.cz;
                    gen.generate(again);
                    same = same && again.blocks == chunk.blocks;
                }
            }
        }
    }
    std::printf("    arenisca roja %d, arena rosa %d, cuarzo rosa %d, ceniza %d, lava %d, hierba marchita %d, espinas %d, "
                "obsidiana %d, roca del cielo %d, cascada %d\n",
                count[block::RedSandstone], count[block::PinkSand], count[block::RoseQuartz], count[block::Ash], count[block::Lava],
                count[block::BlightGrass], count[block::BlightThorns], count[block::Obsidian], count[block::SkyStone],
                count[block::FallingWater]);
    std::printf("    %.2f ms por chunk de media en las tierras salvajes (peor %.2f ms)\n", total_ms / std::max(1, chunks), worst_ms);
    check(same, "un chunk salvaje sale igual dos veces (determinista)");
    check(count[block::RedSandstone] > 0 && count[block::PinkSand] > 0 && count[block::RoseQuartz] > 0 && count[block::Ash] > 0 &&
              count[block::BlightGrass] > 0 && count[block::SkyStone] > 0,
          "bloques de cada bioma e islas en los chunks");
    check(total_ms / std::max(1, chunks) < 40.0, "un chunk salvaje en menos de 40 ms");

    // 4) Una isla: hueco de aire entre el suelo y la panza.
    if (nearest[5] < 9000.0f) {
        const BlockPos at = where[5];
        Generator::FarColumn f;
        gen.farColumn(at.x, at.z, f);
        ChunkData chunk;
        chunk.cx = static_cast<int>(std::floor(static_cast<float>(at.x) / kChunkSize));
        chunk.cz = static_cast<int>(std::floor(static_cast<float>(at.z) / kChunkSize));
        gen.generate(chunk);
        const int lx = at.x - chunk.cx * kChunkSize, lz = at.z - chunk.cz * kChunkSize;
        const bool solid_top = blockDef(chunk.get(lx, f.island_top, lz)).solid;
        const bool gap = f.island_bottom > f.height + 2 && !blockDef(chunk.get(lx, f.island_bottom - 1, lz)).solid;
        std::printf("    isla en (%d, %d): suelo y=%d, panza y=%d, cima y=%d\n", at.x, at.z, f.height, f.island_bottom, f.island_top);
        check(solid_top && gap, "la isla flota (cima maciza, aire debajo) y el terreno lejano la conoce");
    }
    // 4b) Pocas islas y grandes (celdas de 560 m en 16 x 16 km), ninguna cerca del origen.
    {
        int parents = 0, total = 0, with_caves = 0;
        float smallest = 1e9f, nearest_shore = 1e9f;
        Generator::WildIsland buf[4];
        for (int gz = -14; gz <= 14; ++gz) {
            for (int gx = -14; gx <= 14; ++gx) {
                const int n = gen.wildIslandCell(gx, gz, buf);
                for (int i = 0; i < n; ++i) {
                    ++total;
                    if (!buf[i].satellite) {
                        ++parents;
                        smallest = std::min(smallest, buf[i].r);
                    }
                    with_caves += buf[i].caves > 0 ? 1 : 0;
                    nearest_shore = std::min(nearest_shore, std::hypot(buf[i].cx, buf[i].cz) - buf[i].r * 1.55f);
                }
            }
        }
        std::printf("    islas en 16 x 16 km: %d (%d grandes, radio minimo %.0f m), %d con cuevas, orilla mas cerca del origen a %.0f m\n",
                    total, parents, static_cast<double>(smallest), with_caves, static_cast<double>(nearest_shore));
        check(total > 10 && total < 250 && smallest >= 40.0f && with_caves * 2 > total && nearest_shore > 1400.0f,
              "islas flotantes pocas y grandes (80-250 m), con cuevas, ninguna a menos de 1,4 km del origen");
    }

    // 5) Terreno lejano en las tierras salvajes: mallas bien formadas y a tiempo.
    auto shared = std::make_shared<Generator>(1234, 62, VoxelGenerator::Adventure);
    auto palette = std::make_shared<FarPalette>();
    bool well_formed = true;
    double worst_tile = 0.0;
    for (int k = 0; k < kKinds; ++k) {
        if (nearest[static_cast<std::size_t>(k)] >= 9000.0f) continue;
        for (int level : {0, 3, 6}) {
            const int span = FarTerrain::kSamples << level;
            const int tx = static_cast<int>(std::floor(static_cast<float>(where[static_cast<std::size_t>(k)].x) / static_cast<float>(span)));
            const int tz = static_cast<int>(std::floor(static_cast<float>(where[static_cast<std::size_t>(k)].z) / static_cast<float>(span)));
            const auto t0 = std::chrono::steady_clock::now();
            std::vector<Generator::FarColumn> cols;
            FarTerrain::sampleTile(*shared, level, tx, tz, cols);
            std::vector<std::uint32_t> v;
            FarTerrain::meshTile(cols, level, *palette, true, v);
            worst_tile = std::max(worst_tile, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            well_formed = well_formed && v.size() % 8 == 0 && v.size() / 8 < gfx::VoxelPass::kMaxQuadsPerSection;
            for (std::size_t i = 0; i + 1 < v.size(); i += 2) {
                const std::uint32_t a = v[i];
                const std::uint32_t y = ((a >> 5u) & 31u) | (((a >> 23u) & 15u) << 5u);
                if ((a & 31u) > 16 || ((a >> 10u) & 31u) > 16 || y > 300 || ((a >> 15u) & 7u) > 5) well_formed = false;
            }
        }
    }
    std::printf("    peor tesela lejana salvaje %.2f ms\n", worst_tile);
    check(well_formed, "mallas lejanas de las tierras salvajes bien formadas");
}


// CRAMION_FAR_DUMP=<salida.ppm> [CRAMION_FAR_TEXTURES=<carpeta>]: vista
// cenital del terreno lejano (4 km, una columna cada 8 m) con los colores de
// la paleta y sombreado por la pendiente, para mirarlo a ojo.
void dumpFarTerrain(const char* path) {
    auto gen = std::make_shared<Generator>(1234, 62, VoxelGenerator::Adventure);
    gen->setVegetation(0.45f, 0.5f);
    const char* folder = std::getenv("CRAMION_FAR_TEXTURES");
    const auto& names = textureNames();
    std::vector<gfx::VoxelTextureLayer> layers(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) buildTexture(names[i], 16, folder != nullptr ? folder : "", layers[i]);
    const auto palette = buildFarPalette(layers);
    constexpr int kSize = 512;
    const int kStep = std::getenv("CRAMION_FAR_STEP") ? std::atoi(std::getenv("CRAMION_FAR_STEP")) : 8;
    const int ox = std::getenv("CRAMION_FAR_X") ? std::atoi(std::getenv("CRAMION_FAR_X")) : 0;
    const int oz = std::getenv("CRAMION_FAR_Z") ? std::atoi(std::getenv("CRAMION_FAR_Z")) : 0;
    std::vector<int> heights(kSize * kSize);
    std::vector<std::array<float, 3>> colors(kSize * kSize);
    for (int pz = 0; pz < kSize; ++pz) {
        for (int px = 0; px < kSize; ++px) {
            Generator::FarColumn c;
            gen->farColumn(ox + (px - kSize / 2) * kStep, oz + (pz - kSize / 2) * kStep, c, false);
            FarPalette::Entry e = palette->top[c.top];
            int h = c.height + 1;
            std::uint16_t tint = blockDef(c.top).tint != BlockTint::None ? c.grass_tint : 0xFFF;
            if (c.island_top > 0) {
                e = palette->top[c.island_block];
                h = c.island_top + 1;
                tint = c.grass_tint;
            } else if (c.water > c.height) {
                e = palette->top[block::Water];
                h = c.water;
                tint = 0xFFF;
            } else if (c.canopy > c.height) {
                e = palette->top[c.leaves];
                h = c.canopy + 1;
                tint = c.foliage_tint;
            }
            const float t[3] = {static_cast<float>(tint & 15) / 15.0f, static_cast<float>((tint >> 4) & 15) / 15.0f,
                                static_cast<float>((tint >> 8) & 15) / 15.0f};
            colors[static_cast<std::size_t>(px + pz * kSize)] = {e.r * t[0], e.g * t[1], e.b * t[2]};
            heights[static_cast<std::size_t>(px + pz * kSize)] = h;
        }
    }
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return;
    std::fprintf(f, "P6\n%d %d\n255\n", kSize, kSize);
    for (int pz = 0; pz < kSize; ++pz) {
        for (int px = 0; px < kSize; ++px) {
            const int i = px + pz * kSize;
            const int w = px > 0 ? heights[static_cast<std::size_t>(i - 1)] : heights[static_cast<std::size_t>(i)];
            const int n = pz > 0 ? heights[static_cast<std::size_t>(i - kSize)] : heights[static_cast<std::size_t>(i)];
            const float shade = std::clamp(1.0f + static_cast<float>((heights[static_cast<std::size_t>(i)] - w) + (heights[static_cast<std::size_t>(i)] - n)) * 0.04f, 0.5f, 1.4f);
            for (int k = 0; k < 3; ++k) std::fputc(std::clamp(static_cast<int>(colors[static_cast<std::size_t>(i)][static_cast<std::size_t>(k)] * shade), 0, 255), f);
        }
    }
    std::fclose(f);
    std::printf("vista cenital: %s\n", path);
}

// CRAMION_FAR_VIEW=<salida.ppm>: las mallas lejanas de verdad (FarTerrain,
// 4 km) rasterizadas en la CPU en perspectiva (z-buffer, sol y niebla), para
// ver de un vistazo el relieve, las juntas entre niveles y los colores.
void viewFarTerrain(const char* path) {
    auto gen = std::make_shared<Generator>(1234, 62, VoxelGenerator::Adventure);
    gen->setVegetation(0.45f, 0.5f);
    const char* folder = std::getenv("CRAMION_FAR_TEXTURES");
    const auto& names = textureNames();
    std::vector<gfx::VoxelTextureLayer> layers(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) buildTexture(names[i], 16, folder != nullptr ? folder : "", layers[i]);
    FarTerrain far_terrain;
    far_terrain.reset(gen);
    far_terrain.setPalette(buildFarPalette(layers));
    FarTerrain::Settings settings;
    settings.distance = 4096.0f;
    if (const char* e = std::getenv("CRAMION_FAR_DISTANCE")) settings.distance = static_cast<float>(std::atof(e));
    const float cx = static_cast<float>(std::atof(std::getenv("CRAMION_FAR_X") ? std::getenv("CRAMION_FAR_X") : "0"));
    const float cz = static_cast<float>(std::atof(std::getenv("CRAMION_FAR_Z") ? std::getenv("CRAMION_FAR_Z") : "0"));
    const float hole = static_cast<float>(std::atof(std::getenv("CRAMION_FAR_HOLE") ? std::getenv("CRAMION_FAR_HOLE") : "0"));
    std::vector<FarTerrain::Update> all;
    for (int frame = 0; frame < 20000; ++frame) {
        far_terrain.update(Vec3{cx, 100.0f, cz}, 512.0f, settings);
        far_terrain.takeUpdates(all, 1u << 30);
        const FarTerrain::Stats st = far_terrain.stats();
        if (st.tiles > 0 && st.pending == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Generator::FarColumn here;
    gen->farColumn(static_cast<int>(cx), static_cast<int>(cz), here);
    const Vec3 eye{cx, static_cast<float>(std::max(here.height, here.water)) + 45.0f, cz};
    const float yaw = static_cast<float>(std::atof(std::getenv("CRAMION_FAR_YAW") ? std::getenv("CRAMION_FAR_YAW") : "0"));
    const Vec3 fwd = core::normalize(Vec3{std::cos(yaw), -0.12f, std::sin(yaw)});
    const Vec3 right = core::normalize(core::cross(fwd, Vec3{0.0f, 1.0f, 0.0f}));
    const Vec3 up = core::cross(right, fwd);
    constexpr int W = 1280, H = 640;
    const float focal = static_cast<float>(W) * 0.5f / std::tan(0.5f * 1.4f);
    std::vector<float> depth(W * H, 1e30f);
    std::vector<std::array<float, 3>> image(W * H);
    for (int y = 0; y < H; ++y) {
        const float t = static_cast<float>(y) / H;
        for (int x = 0; x < W; ++x) image[static_cast<std::size_t>(x + y * W)] = {120.0f + 80.0f * t, 170.0f + 50.0f * t, 235.0f};
    }
    const Vec3 sun = core::normalize(Vec3{0.4f, 0.8f, 0.3f});
    static const Vec3 kNormals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    std::size_t quads = 0;
    for (const FarTerrain::Update& u : all) {
        for (std::size_t q = 0; q + 7 < u.vertices.size(); q += 8) {
            ++quads;
            Vec3 p[4];
            float sx[4], sy[4], sz[4];
            bool behind = false;
            const std::uint32_t a0 = u.vertices[q], b0 = u.vertices[q + 1];
            for (int k = 0; k < 4; ++k) {
                const std::uint32_t a = u.vertices[q + static_cast<std::size_t>(k) * 2];
                const float vy = static_cast<float>(((a >> 5u) & 31u) | (((a >> 23u) & 15u) << 5u));
                p[k] = u.origin + Vec3{static_cast<float>(a & 31u) * u.scale, vy, static_cast<float>((a >> 10u) & 31u) * u.scale};
                const Vec3 d = p[k] - eye;
                sz[k] = core::dot(d, fwd);
                if (sz[k] < 1.0f) behind = true;
                sx[k] = static_cast<float>(W) * 0.5f + core::dot(d, right) * focal / sz[k];
                sy[k] = static_cast<float>(H) * 0.5f - core::dot(d, up) * focal / sz[k];
            }
            if (behind) continue;
            const int face = static_cast<int>((a0 >> 15u) & 7u);
            const float light = 0.45f + 0.65f * std::max(0.0f, core::dot(kNormals[face], sun));
            const float ao = 0.6f + 0.4f * static_cast<float>((a0 >> 20u) & 3u) / 3.0f;
            const float col[3] = {static_cast<float>(b0 & 255u), static_cast<float>((b0 >> 8u) & 255u), static_cast<float>((b0 >> 16u) & 255u)};
            for (int tri = 0; tri < 2; ++tri) {
                const int i0 = 0, i1 = tri == 0 ? 1 : 2, i2 = tri == 0 ? 2 : 3;
                const float x0 = sx[i0], y0 = sy[i0], x1 = sx[i1], y1 = sy[i1], x2 = sx[i2], y2 = sy[i2];
                const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
                if (std::abs(area) < 1e-6f) continue;
                const int minx = std::max(0, static_cast<int>(std::floor(std::min({x0, x1, x2}))));
                const int maxx = std::min(W - 1, static_cast<int>(std::ceil(std::max({x0, x1, x2}))));
                const int miny = std::max(0, static_cast<int>(std::floor(std::min({y0, y1, y2}))));
                const int maxy = std::min(H - 1, static_cast<int>(std::ceil(std::max({y0, y1, y2}))));
                for (int py = miny; py <= maxy; ++py) {
                    for (int px = minx; px <= maxx; ++px) {
                        const float fx = static_cast<float>(px) + 0.5f, fy = static_cast<float>(py) + 0.5f;
                        const float w0 = ((x1 - fx) * (y2 - fy) - (x2 - fx) * (y1 - fy)) / area;
                        const float w1 = ((x2 - fx) * (y0 - fy) - (x0 - fx) * (y2 - fy)) / area;
                        const float w2 = 1.0f - w0 - w1;
                        if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                        const float z = w0 * sz[i0] + w1 * sz[i1] + w2 * sz[i2];
                        const Vec3 wp = p[i0] * w0 + p[i1] * w1 + p[i2] * w2;
                        if (hole > 0.0f && (wp.x - cx) * (wp.x - cx) + (wp.z - cz) * (wp.z - cz) < hole * hole) continue;
                        float& dz = depth[static_cast<std::size_t>(px + py * W)];
                        if (z >= dz) continue;
                        dz = z;
                        const float fog = std::clamp(z / (settings.distance * 1.3f), 0.0f, 0.75f);
                        auto& o = image[static_cast<std::size_t>(px + py * W)];
                        for (int k = 0; k < 3; ++k) o[static_cast<std::size_t>(k)] = col[k] * light * ao * (1.0f - fog) + (k == 2 ? 235.0f : 190.0f) * fog;
                    }
                }
            }
        }
    }
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return;
    std::fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (const auto& c : image) {
        for (int k = 0; k < 3; ++k) std::fputc(std::clamp(static_cast<int>(c[static_cast<std::size_t>(k)]), 0, 255), f);
    }
    std::fclose(f);
    std::printf("vista: %s (%zu teselas, %zu caras)\n", path, all.size(), quads);
}

// Luces (ids 181..190) y la antorcha fina: catalogo, texturas por franjas y
// las piezas del mallador (antorcha en el suelo o contra la pared, hoguera).
Vec3 finePosition(std::uint32_t a, std::uint32_t b) {
    Vec3 p{static_cast<float>(a & 31u), static_cast<float>((a >> 5u) & 31u), static_cast<float>((a >> 10u) & 31u)};
    if (((b >> 30u) & 1u) != 0u) {
        p.x += static_cast<float>((a >> 23u) & 15u) / 16.0f;
        p.y += static_cast<float>((a >> 27u) & 15u) / 16.0f;
        p.z += static_cast<float>((a >> 18u) & 15u) / 16.0f;
    }
    return p;
}

// Fugas de luz a traves de los voxeles.
//  1) Las luces de los bloques (BlockLights) no iluminan los voxeles: ya llevan
//     esa luz propagada por celdas, y la puntual (sin sombra) cruzaba paredes.
//  2) Sombras del sol con el sol rasante (atardecer): un monte a 150 m hacia el
//     sol tiene que entrar en la cascada cercana (sin plano cercano, con depth
//     clamp, y con el barrido hasta la misma altura que con el sol alto). Antes
//     se descartaba y, sin las secciones cerradas, el sol entraba en la cueva.
void testLightLeaks() {
    std::printf("Fugas de luz\n");
    {
        ChunkData chunk;
        chunk.blocks[static_cast<std::size_t>(cellIndex(8, 40, 8))] = block::Torch;
        BlockLightCache cache;
        std::vector<scene::PointLight> lights;
        const auto lookup = [&chunk](int cx, int cz, BlockLightCache::Column& column) {
            if (cx != 0 || cz != 0) return false;
            column.data = &chunk;
            column.serial = 1;
            return true;
        };
        cache.collect(Vec3{8.0f, 41.0f, 12.0f}, Vec3{0.0f, 0.0f, 0.0f}, lookup, 0.0f, lights);
        bool all_skip = !lights.empty();
        for (const scene::PointLight& l : lights) all_skip = all_skip && l.skip_voxels && !l.cast_shadows;
        check(all_skip, "las luces de los bloques no iluminan los voxeles (skip_voxels)");
    }
    {
        // Cascada cercana como ShadowCascades: esfera de radio r en el centro,
        // camara de la luz a r + 60 m hacia el sol, profundidad 2r + 60.
        const float r = 8.0f, margin = 60.0f;
        const Vec3 center{0.0f, 40.0f, 0.0f};
        const float elevation = core::radians(4.0f);  // sol de atardecer
        const Vec3 to_sun = core::normalize(Vec3{std::cos(elevation), std::sin(elevation), 0.0f});
        const Vec3 light_dir = to_sun * -1.0f;
        const core::Mat4 view = core::lookAt(center - light_dir * (r + margin), center, Vec3{0.0f, 1.0f, 0.0f});
        const core::Mat4 lvp = core::orthographic(-r, r, -r, r, 0.0f, 2.0f * r + margin) * view;
        // Seccion de la corteza del monte, a 150 m hacia el sol (y 10 m mas alta).
        const Vec3 occ = center + to_sun * 150.0f;
        const core::Aabb box{occ - Vec3{8.0f, 8.0f, 8.0f}, occ + Vec3{8.0f, 8.0f, 8.0f}};
        check(!core::Frustum(lvp).intersects(box) && core::Frustum(lvp, true).intersects(box),
              "sol rasante: la corteza lejana entra en la cascada (sin plano cercano)");
        // Trozo de vista de la cascada: la camara mira hacia el sol.
        const core::Mat4 cam = core::perspective(1.2f, 1.7f, 0.1f, 12.0f) *
                               core::lookAt(center, center + Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
        const core::Frustum slice(cam);
        const float reach = 2.0f * r + margin;
        const float rise = std::max(-light_dir.y, 0.04f);
        const Vec3 c = (box.min + box.max) * 0.5f;
        const float radius = core::length(box.max - box.min) * 0.5f;
        check(!slice.intersectsSwept(c, c + light_dir * reach, radius) &&
                  slice.intersectsSwept(c, c + light_dir * (reach / std::min(rise, 1.0f)), radius),
              "sol rasante: el barrido llega a lo que se ve desde 150 m");
    }
}

void testLights() {
    std::printf("Luces\n");
    check(block::Count >= 191 && blocks().size() == block::Count, "los ids de las luces caben (181..190)");
    const char* const names[] = {"lamp_post", "street_lamp", "campfire", "crystal_lamp", "paper_lantern",
                                 "lamp_red", "lamp_blue", "lamp_green", "chandelier", "standing_torch"};
    bool named = true, lit = true;
    for (int i = 0; i < 10; ++i) {
        const BlockId id = static_cast<BlockId>(block::LampPost + i);
        named = named && blockDef(id).name == names[i] && blockId(names[i]) == id && blockDef(id).placeable;
        if (id != block::LampPost) lit = lit && blockDef(id).light >= 13;
    }
    check(named, "las diez luces con su nombre estable");
    check(lit && blockDef(block::Torch).light == 14, "y todas alumbran (menos el poste)");
    check(blockDef(block::Torch).shape == BlockShape::Cube && blockDef(block::Torch).model == BlockModel::Torch &&
              !blockDef(block::Torch).solid && !blockDef(block::Torch).opaque,
          "la antorcha es una pieza fina (no una cruz) que no choca");
    bool textures = true;
    for (const char* t : {"torch_stick", "torch_flame_top", "street_lamp", "campfire_fire", "chandelier_candle",
                          "paper_lantern", "lamp_red", "crystal_lamp"}) {
        textures = textures && std::find(textureNames().begin(), textureNames().end(), t) != textureNames().end();
    }
    check(textures, "sus texturas estan en el array (tambien las segundas: llamas y velas)");
    // Franjas: la llama arriba brilla, el palo abajo no; encima de la llama, hueco.
    gfx::VoxelTextureLayer stick;
    buildTexture("torch_stick", 32, {}, stick);
    // c == 3: alfa del color; c == 2: emision del material.
    const auto at = [&](int px, int py, int c) {
        const std::size_t i = static_cast<std::size_t>((py * 32 + px) * 4);
        return c == 3 ? stick.albedo[i + 3] : stick.material[i + 2];
    };
    check(at(3, 1, 3) < 128 && at(20, 8, 2) > 200 && at(9, 28, 2) < 50 && at(9, 28, 3) > 200,
          "la textura del palo: hueco, llama que brilla y madera, por filas");

    SectionSnapshot s;
    s.light.fill(0xF0);
    s.grass_tint.fill(0xFFF);
    s.foliage_tint.fill(0xFFF);
    std::vector<std::uint32_t> v;
    const auto bounds = [&](Vec3& lo, Vec3& hi) {
        lo = Vec3{99, 99, 99};
        hi = Vec3{-99, -99, -99};
        for (std::size_t i = 0; i + 1 < v.size(); i += 2) {
            if (((v[i + 1] >> 30u) & 1u) == 0u) continue;  // solo piezas finas
            const Vec3 p = finePosition(v[i], v[i + 1]);
            lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
    };
    // Antorcha en el suelo: palo en el centro, desde el suelo.
    s.blocks[static_cast<std::size_t>(SectionSnapshot::index(5, 4, 5))] = block::Stone;
    s.blocks[static_cast<std::size_t>(SectionSnapshot::index(5, 5, 5))] = block::Torch;
    s.blocks[static_cast<std::size_t>(SectionSnapshot::index(6, 5, 5))] = block::Stone;
    meshSection(s, v);
    Vec3 lo, hi;
    bounds(lo, hi);  // solo las piezas finas (la piedra no cuenta)
    check(lo.y == 5.0f && lo.x >= 5.37f && hi.x <= 5.63f && hi.y <= 5.0f + 14.0f / 16.0f + 0.001f,
          "antorcha en el suelo: palo y llama finos en el centro");
    // Sin suelo y con una pared en +X: pegada a la pared, con la escuadra hasta ella.
    s.blocks[static_cast<std::size_t>(SectionSnapshot::index(5, 4, 5))] = block::Air;
    meshSection(s, v);
    bounds(lo, hi);
    check(hi.x == 6.0f && lo.x >= 5.55f && lo.y >= 5.18f, "antorcha de pared: contra la pared y sin llegar al suelo");
    // Hoguera: troncos y las llamas (segunda textura) en cruz.
    s.blocks.fill(block::Air);
    s.blocks[static_cast<std::size_t>(SectionSnapshot::index(5, 5, 5))] = block::Campfire;
    meshSection(s, v);
    int flames = 0;
    const int fire_layer = textureLayer("campfire_fire");
    for (std::size_t i = 0; i + 1 < v.size(); i += 2) flames += static_cast<int>(v[i + 1] & 1023u) == fire_layer ? 1 : 0;
    check(flames == 4 * 4, "hoguera: cuatro caras de llama en cruz");
    // Farola: poste de dos y cabeza; el poste de arriba sin remate (sigue la cabeza).
    s.blocks.fill(block::Air);
    s.blocks[static_cast<std::size_t>(SectionSnapshot::index(5, 5, 5))] = block::LampPost;
    s.blocks[static_cast<std::size_t>(SectionSnapshot::index(5, 6, 5))] = block::LampPost;
    s.blocks[static_cast<std::size_t>(SectionSnapshot::index(5, 7, 5))] = block::StreetLamp;
    meshSection(s, v);
    bounds(lo, hi);
    check(lo.y == 5.0f && hi.y == 8.0f && lo.x >= 5.18f && hi.x <= 5.82f, "farola: poste y cabeza dentro de su columna");
    for (const BlockId id : {block::Chandelier, block::StandingTorch, block::PaperLantern}) {
        s.blocks.fill(block::Air);
        s.blocks[static_cast<std::size_t>(SectionSnapshot::index(5, 5, 5))] = id;
        meshSection(s, v);
        bounds(lo, hi);
        check(!v.empty() && lo.x >= 5.0f && hi.x <= 6.0f && lo.y >= 5.0f && hi.y <= 6.0f, blockDef(id).name.c_str());
    }
}

// Puertas (BlockModel::Door): hoja fina a lo largo del eje con paredes; setBlockHidden solo toca el dibujo.
void testDoors() {
    std::printf("Puertas\n");
    const BlockDef& door = blockDef(block::Door);
    check(door.model == BlockModel::Door && !door.solid && !door.opaque && door.light_filter < 15,
          "la puerta es una hoja fina que no choca ni tapa la luz");
    SectionSnapshot s;
    s.light.fill(0xF0);
    s.grass_tint.fill(0xFFF);
    s.foliage_tint.fill(0xFFF);
    std::vector<std::uint32_t> v;
    const auto bounds = [&](Vec3& lo, Vec3& hi) {
        lo = Vec3{99, 99, 99};
        hi = Vec3{-99, -99, -99};
        for (std::size_t i = 0; i + 1 < v.size(); i += 2) {
            if (((v[i + 1] >> 30u) & 1u) == 0u) continue;
            const Vec3 p = finePosition(v[i], v[i + 1]);
            lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
    };
    const auto put = [&](int x, int y, int z, BlockId id) { s.blocks[static_cast<std::size_t>(SectionSnapshot::index(x, y, z))] = id; };
    // Pared a lo largo de Z (vecinos en +-Z) con una puerta de 2: la hoja va a lo largo de Z, fina en X.
    for (int y = 5; y <= 6; ++y) {
        put(5, y, 4, block::Stone);
        put(5, y, 6, block::Stone);
        put(5, y, 5, block::Door);
    }
    meshSection(s, v);
    Vec3 lo, hi;
    bounds(lo, hi);
    check(lo.x == 5.0f + 6.0f / 16.0f && hi.x == 5.0f + 10.0f / 16.0f && lo.z == 5.0f && hi.z == 6.0f && lo.y == 5.0f && hi.y == 7.0f,
          "puerta en pared a lo largo de Z: hoja de 4/16 en el centro, 2 de alto");
    const int top = textureLayer("door_top");
    int upper = 0;
    for (std::size_t i = 0; i + 1 < v.size(); i += 2) upper += static_cast<int>(v[i + 1] & 1023u) == top ? 1 : 0;
    check(upper > 0, "la mitad de arriba con door_top");
    // La misma con la pared a lo largo de X.
    s.blocks.fill(block::Air);
    put(4, 5, 5, block::Stone);
    put(6, 5, 5, block::Stone);
    put(5, 5, 5, block::Door);
    meshSection(s, v);
    bounds(lo, hi);
    check(lo.z == 5.0f + 6.0f / 16.0f && hi.z == 5.0f + 10.0f / 16.0f && lo.x == 5.0f && hi.x == 6.0f,
          "puerta en pared a lo largo de X: fina en Z");

    // Oculta solo en el dibujo: el bloque sigue ahi.
    ecs::World world;
    makeWorld(world, 4242, 2);
    VoxelSystem vox;
    vox.start(world);
    vox.waitUntilReady(Vec3{0.5f, 120.0f, 0.5f}, 1, 60.0f);
    const int h = vox.surfaceHeight(0, 0) + 1;
    vox.setBlock(0, h, 0, block::Door);
    vox.setBlockHidden(0, h, 0, true);
    check(vox.isBlockHidden(0, h, 0) && vox.getBlock(0, h, 0) == block::Door, "setBlockHidden no cambia el bloque");
    vox.setBlockHidden(0, h, 0, false);
    check(!vox.isBlockHidden(0, h, 0), "y se vuelve a mostrar");
    vox.setBlockHidden(0, h, 0, true);
    vox.showHiddenBlocks();
    check(!vox.isBlockHidden(0, h, 0) && vox.getBlock(0, h, 0) == block::Door, "showHiddenBlocks las muestra todas");
}

#include "VoxelRailTests.inc"  // rieles y senales (testRails)
#include "VoxelLushTests.inc"  // NOCHE-MUNDO: biomas frondosos y bloques nuevos (testLush)
#include "VoxelIds16Tests.inc"  // IDS16: ids de bloque de 16 bits (testIds16)
#include "VoxelMeshDetailTests.inc"  // MALLAS-DETALLE: niveles de adornos del mallador (testMeshDetail)

// Cuevas inmensas (GeneratorCaves.cpp): lejos del pueblo de inicio, salas de
// mas de 100 m, barrancos hasta la roca madre, pozos, lagos y lava sin paredes
// de liquido, grutas con sus bloques, minas con antorchas; cerca del origen,
// nada de eso. Determinista y con tiempo por chunk razonable.
void testImmenseCaves() {
    std::printf("Cuevas inmensas\n");
    const Generator gen(1234, 62, VoxelGenerator::Adventure);
    // 1) Cerca del origen (pueblo de inicio), nada nuevo.
    {
        bool calm = true;
        for (int cz = -22; cz <= 22 && calm; cz += 3) {
            for (int cx = -22; cx <= 22 && calm; cx += 3) {
                if ((cx * cx + cz * cz) * 256 > 360 * 360) continue;
                ChunkData chunk;
                chunk.cx = cx;
                chunk.cz = cz;
                gen.generate(chunk);
                for (BlockId id : chunk.blocks) {
                    if (id == block::Deepstone || id == block::Dripstone || id == block::Stalactite || id == block::Stalagmite ||
                        id == block::GlowVines) {
                        calm = false;
                        std::printf("    bloque %d en el chunk (%d, %d)\n", id, cx, cz);
                        break;
                    }
                }
            }
        }
        check(calm, "a menos de ~360 m del origen las cuevas son las de siempre");
        // Bloque a bloque igual que sin las cuevas inmensas (pueblo de inicio y alrededores).
        Generator old(1234, 62, VoxelGenerator::Adventure);
        old.setImmenseCaves(false);
        bool same = true;
        for (int i = 0; i < 24 && same; ++i) {
            ChunkData a, b;
            a.cx = b.cx = (i % 6) * 4 - 12;
            a.cz = b.cz = (i / 6) * 5 - 10;
            gen.generate(a);
            old.generate(b);
            same = a.blocks == b.blocks;
        }
        check(same, "cerca del origen, bloque a bloque el mundo de antes");
    }
    check(blockId("deepstone") == block::Deepstone && blockId("glow_vines") == block::GlowVines && blockDef(block::GlowVines).light > 0 &&
              !blockDef(block::Stalactite).solid && blockDef(block::Dripstone).solid && block::Deepstone == 158,
          "bloques de las cuevas (158..162) con su nombre, luz y forma");
    // Con CRAMION_CAVE_TEX=<carpeta>: las texturas nuevas en PNG (para mirarlas).
    if (const char* dir = std::getenv("CRAMION_CAVE_TEX")) {
        for (const char* name : {"deepstone", "dripstone", "stalactite", "stalagmite", "glow_vines"}) {
            gfx::VoxelTextureLayer layer;
            buildTexture(name, 64, {}, layer);
            asset::ImageRgba8 image;
            image.width = image.height = 64;
            image.pixels = layer.albedo;
            asset::saveImagePng(std::string(dir) + "/" + name + ".png", image);
        }
    }
    // 2) Una zona de 32 x 32 chunks lejos del origen.
    const char* env = std::getenv("CRAMION_CAVES_AT");
    int ox = -900, oz = 900;
    if (env != nullptr) std::sscanf(env, "%d,%d", &ox, &oz);
    constexpr int kN = 32, kSide = kN * kChunkSize;
    const int cx0 = ox / kChunkSize, cz0 = oz / kChunkSize;
    std::vector<std::uint8_t> slice(static_cast<std::size_t>(kSide * kSide), 0);  // aire a y = 28
    std::vector<std::uint8_t> sky_depth(slice.size(), 0);  // lo abierto al cielo bajo la superficie
    constexpr int kSliceY = 28;
    std::array<long long, 16> air_by_band{}, rock_by_band{};
    std::map<int, long long> count;
    long long leaks = 0, liquid = 0, open_deep = 0, open_30 = 0, mine_planks = 0, columns = 0;
    double worst = 0.0, total = 0.0;
    // CIUDADES-GRANDES: la zona cae en una metropolis; su plan (unos 30 ms, una vez por celda de 3 x 3 regiones) se
    // hace antes de medir: aqui se mide el chunk, no la planificacion (Structures.h metropolis).
    if (gen.structures() != nullptr)
        for (int rz = Structures::regionOf(oz); rz <= Structures::regionOf(oz + kSide); ++rz)
            for (int rx = Structures::regionOf(ox); rx <= Structures::regionOf(ox + kSide); ++rx) (void)gen.structures()->region(rx, rz);
    for (int dz = 0; dz < kN; ++dz) {
        for (int dx = 0; dx < kN; ++dx) {
            ChunkData chunk;
            chunk.cx = cx0 + dx;
            chunk.cz = cz0 + dz;
            const auto t0 = std::chrono::steady_clock::now();
            gen.generate(chunk);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            worst = std::max(worst, ms);
            total += ms;
            for (int z = 0; z < kChunkSize; ++z) {
                for (int x = 0; x < kChunkSize; ++x) {
                    const int h = gen.terrainHeight(chunk.cx * kChunkSize + x, chunk.cz * kChunkSize + z);
                    ++columns;
                    // Abierto al cielo hasta y <= 12 (barranco o pozo).
                    int y = std::min(h, kWorldHeight - 1);
                    while (y > 0 && !blockDef(chunk.get(x, y, z)).solid) --y;
                    if (y <= 12 && h > 70) ++open_deep;
                    if (y < h - 30) ++open_30;
                    sky_depth[static_cast<std::size_t>((dx * kChunkSize + x) + (dz * kChunkSize + z) * kSide)] = static_cast<std::uint8_t>(std::clamp(h - y, 0, 255));
                    for (int yy = 29; yy <= 46; ++yy) mine_planks += chunk.get(x, yy, z) == block::Fence ? 1 : 0;
                    for (int yy = 1; yy < std::min(h - 6, 128) && y >= h - 6; ++yy) {  // sin barrancos ni pozos
                        const BlockId id = chunk.get(x, yy, z);
                        const bool hollow = !blockDef(id).solid;
                        (hollow ? air_by_band : rock_by_band)[static_cast<std::size_t>(yy / 8)]++;
                    }
                    slice[static_cast<std::size_t>((dx * kChunkSize + x) + (dz * kChunkSize + z) * kSide)] =
                        chunk.get(x, kSliceY, z) == block::Air && kSliceY < h - 8 ? 1 : 0;
                }
            }
            for (int y = 1; y < kWorldHeight - 1; ++y) {
                for (int z = 0; z < kChunkSize; ++z) {
                    for (int x = 0; x < kChunkSize; ++x) {
                        const BlockId id = chunk.get(x, y, z);
                        ++count[id];
                        if (id != block::Water && id != block::Lava) continue;
                        if (y > 40) continue;  // el agua y la lava de la superficie
                        ++liquid;
                        // Liquido junto a aire de lado (dentro del chunk): una pared de liquido.
                        const int n[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                        for (const auto& d : n) {
                            const int nx = x + d[0], nz = z + d[1];
                            if (nx < 0 || nz < 0 || nx >= kChunkSize || nz >= kChunkSize) continue;
                            if (chunk.get(nx, y, nz) == block::Air) {
                                ++leaks;
                                if (leaks <= 12 && std::getenv("CRAMION_LEAKS") != nullptr) {
                                    const int lx = chunk.cx * kChunkSize + x, lz = chunk.cz * kChunkSize + z;
                                    std::printf("      fuga %s en %d %d %d (suelo %d)\n", id == block::Water ? "agua" : "lava", lx, y, lz,
                                                gen.terrainHeight(lx, lz));
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
    // Con CRAMION_CAVES_MAP=<png>: a la izquierda lo abierto al cielo (mas claro = mas hondo), a la derecha el corte.
    if (const char* map = std::getenv("CRAMION_CAVES_MAP")) {
        asset::ImageRgba8 image;
        image.width = kSide * 2;
        image.height = kSide;
        image.pixels.assign(static_cast<std::size_t>(kSide * 2 * kSide * 4), 255);
        for (int z = 0; z < kSide; ++z) {
            for (int x = 0; x < kSide; ++x) {
                const std::size_t i = static_cast<std::size_t>(x + z * kSide);
                const std::size_t a = (static_cast<std::size_t>(z) * kSide * 2 + static_cast<std::size_t>(x)) * 4;
                const std::size_t b = a + static_cast<std::size_t>(kSide) * 4;
                const auto v = static_cast<std::uint8_t>(std::min(255, sky_depth[i] * 2));
                image.pixels[a] = v;
                image.pixels[a + 1] = sky_depth[i] > 0 ? v : 40;
                image.pixels[a + 2] = sky_depth[i] > 0 ? 40 : 40;
                image.pixels[b] = slice[i] ? 30 : 120;
                image.pixels[b + 1] = slice[i] ? 60 : 110;
                image.pixels[b + 2] = slice[i] ? 200 : 100;
            }
        }
        asset::saveImagePng(map, image);
    }
    // La sala mas grande a y = 28 (componente conexa de aire en el corte).
    int biggest = 0, wide_x = 0, wide_z = 0;
    {
        std::vector<int> stack;
        std::vector<std::uint8_t> seen(slice.size(), 0);
        for (int start = 0; start < kSide * kSide; ++start) {
            if (!slice[static_cast<std::size_t>(start)] || seen[static_cast<std::size_t>(start)]) continue;
            int area = 0, minx = kSide, maxx = 0, minz = kSide, maxz = 0;
            stack.push_back(start);
            seen[static_cast<std::size_t>(start)] = 1;
            while (!stack.empty()) {
                const int i = stack.back();
                stack.pop_back();
                ++area;
                const int x = i % kSide, z = i / kSide;
                minx = std::min(minx, x);
                maxx = std::max(maxx, x);
                minz = std::min(minz, z);
                maxz = std::max(maxz, z);
                const int n[4] = {x > 0 ? i - 1 : -1, x < kSide - 1 ? i + 1 : -1, z > 0 ? i - kSide : -1, z < kSide - 1 ? i + kSide : -1};
                for (int j : n) {
                    if (j >= 0 && slice[static_cast<std::size_t>(j)] && !seen[static_cast<std::size_t>(j)]) {
                        seen[static_cast<std::size_t>(j)] = 1;
                        stack.push_back(j);
                    }
                }
            }
            if (area > biggest) {
                biggest = area;
                wide_x = maxx - minx + 1;
                wide_z = maxz - minz + 1;
            }
        }
    }
    // Ancho de la sala mas ancha: el mayor cuadrado de aire en el corte.
    int square = 0;
    {
        std::vector<int> dp(slice.size(), 0);
        for (int z = 0; z < kSide; ++z) {
            for (int x = 0; x < kSide; ++x) {
                const std::size_t i = static_cast<std::size_t>(x + z * kSide);
                if (!slice[i]) continue;
                dp[i] = (x == 0 || z == 0) ? 1 : 1 + std::min({dp[i - 1], dp[i - kSide], dp[i - 1 - kSide]});
                square = std::max(square, dp[i]);
            }
        }
    }
    // Lo mismo sin las cuevas inmensas (el mundo de antes), para comparar.
    double old_worst = 0.0, old_total = 0.0;
    long long old_diamond = 0, old_gold = 0;
    std::array<long long, 16> old_air{}, old_rock{};
    {
        Generator old(1234, 62, VoxelGenerator::Adventure);
        old.setImmenseCaves(false);
        for (int dz = 0; dz < kN; ++dz) {
            for (int dx = 0; dx < kN; ++dx) {
                ChunkData chunk;
                chunk.cx = cx0 + dx;
                chunk.cz = cz0 + dz;
                const auto t0 = std::chrono::steady_clock::now();
                old.generate(chunk);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                old_worst = std::max(old_worst, ms);
                old_total += ms;
                for (BlockId id : chunk.blocks) {
                    old_diamond += id == block::DiamondOre ? 1 : 0;
                    old_gold += id == block::GoldOre ? 1 : 0;
                }
                for (int z = 0; z < kChunkSize; ++z) {
                    for (int x = 0; x < kChunkSize; ++x) {
                        const int h = old.terrainHeight(chunk.cx * kChunkSize + x, chunk.cz * kChunkSize + z);
                        for (int yy = 1; yy < std::min(h - 6, 128); ++yy) {
                            (blockDef(chunk.get(x, yy, z)).solid ? old_rock : old_air)[static_cast<std::size_t>(yy / 8)]++;
                        }
                    }
                }
            }
        }
    }
    std::printf("    hueco por altura sin cuevas inmensas:");
    for (std::size_t b = 0; b < 16; ++b) {
        const long long t = old_air[b] + old_rock[b];
        if (t > 0) std::printf(" %zu:%.0f%%", b * 8, 100.0 * static_cast<double>(old_air[b]) / static_cast<double>(t));
    }
    std::printf("\n");
    std::printf("    zona %d,%d (%d x %d m): %.2f ms por chunk de media, el peor %.2f ms (sin cuevas inmensas: %.2f / %.2f ms)\n", ox,
                oz, kSide, kSide, total / (kN * kN), worst, old_total / (kN * kN), old_worst);
    std::printf("    hueco por altura (y/8):");
    for (std::size_t b = 0; b < 16; ++b) {
        const long long t = air_by_band[b] + rock_by_band[b];
        if (t > 0) std::printf(" %zu:%.0f%%", b * 8, 100.0 * static_cast<double>(air_by_band[b]) / static_cast<double>(t));
    }
    std::printf("\n    corte a y=%d: sala mayor %d m2 (%d x %d m de extension), cuadrado libre de %d m\n", kSliceY, biggest, wide_x,
                wide_z, square);
    std::printf("    columnas abiertas al cielo hasta y<=12: %lld de %lld; liquido de cueva %lld (agua y lava; junto a aire de lado %lld)\n",
                open_deep, columns, liquid, leaks);
    std::printf("    columnas con el suelo a mas de 30 m bajo la superficie (barrancos y pozos): %lld; postes de mina %lld\n", open_30,
                mine_planks);
    std::printf("    deepstone %lld dripstone %lld estalactitas %lld estalagmitas %lld enredaderas %lld setas gigantes %lld cristales %lld\n",
                count[block::Deepstone], count[block::Dripstone], count[block::Stalactite], count[block::Stalagmite],
                count[block::GlowVines], count[block::ShroomStem],
                count[block::CrystalBlue] + count[block::CrystalPink] + count[block::CrystalViolet] + count[block::CrystalGreen]);
    std::printf("    lava %lld agua %lld antorchas %lld vallas %lld telaranas %lld diamante %lld oro %lld\n", count[block::Lava],
                count[block::Water], count[block::Torch], count[block::Fence], count[block::Cobweb], count[block::DiamondOre],
                count[block::GoldOre]);
    std::printf("    sin cuevas inmensas: diamante %lld oro %lld\n", old_diamond, old_gold);
    check(wide_x >= 100 || wide_z >= 100, "hay salas de mas de 100 m");
    check(open_deep > 0, "barrancos o pozos abiertos al cielo que bajan hasta y <= 12");
    check(count[block::Deepstone] > 0 && count[block::Dripstone] > 0 && count[block::Stalactite] > 0 && count[block::GlowVines] > 0,
          "roca honda, estalactitas y enredaderas luminosas");
    check(count[block::Torch] > 0 && count[block::Fence] > 0, "galerias de mina con puntales y antorchas");
    std::printf("    vias %lld topes %lld arcanita %lld\n", count[block::Rail], count[block::BufferStop], count[block::ArcaniteOre]);
    check(count[block::Rail] > 50 && count[block::BufferStop] > 0, "las minas tienen vias viejas y algun tope");
    check(count[block::ArcaniteOre] > 0, "mena de arcanita en lo hondo");
    check(liquid > 0 && leaks * 100 < liquid, "lagos y lava sin paredes de liquido (menos del 1% junto a aire)");
    check(worst < 25.0 && total / (kN * kN) < 12.0, "tiempo por chunk razonable");
    // Cargar con el sistema (generar, luz de la lava y las setas, mallar) junto al
    // origen (cuevas de siempre) y lejos (cuevas inmensas): tiempo y peor frame.
    if (std::getenv("CRAMION_IMMENSE_LOAD") != nullptr) {
        for (const Vec3 viewer : {Vec3{8.0f, 60.0f, 8.0f}, Vec3{-650.0f, 40.0f, 1050.0f}}) {
            ecs::World world;
            VoxelWorld& vw = world.create("Mundo").add<VoxelWorld>();
            vw.seed = 1234;
            vw.generator = VoxelGenerator::Adventure;
            vw.render_distance = 8;
            vw.textures.clear();
            VoxelSystem vox;
            vox.start(world);
            const auto start = std::chrono::steady_clock::now();
            vox.waitUntilReady(viewer, 8, 120.0f);
            const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
            float worst_ms = 0.0f;
            for (int i = 0; i < 20000 && (vox.stats().pending_generation > 0 || vox.stats().pending_meshes > 0); ++i) {
                const auto t = std::chrono::steady_clock::now();
                vox.update(0.016f, viewer);
                worst_ms = std::max(worst_ms, std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t).count());
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            std::printf("    cargar en (%.0f, %.0f): distancia 8 en %.2f s, peor frame %.2f ms (%d secciones con caras)\n",
                        static_cast<double>(viewer.x), static_cast<double>(viewer.z), static_cast<double>(seconds),
                        static_cast<double>(worst_ms), vox.stats().sections);
            check(worst_ms < 16.0f, "cargar las cuevas no congela el frame");
        }
    }
    // Determinista: otro generador con la misma semilla.
    {
        const Generator other(1234, 62, VoxelGenerator::Adventure);
        bool same = true;
        for (int i = 0; i < 6 && same; ++i) {
            ChunkData a, b;
            a.cx = b.cx = cx0 + i * 5;
            a.cz = b.cz = cz0 + i * 3;
            gen.generate(a);
            other.generate(b);
            same = a.blocks == b.blocks;
        }
        check(same, "determinista: la misma semilla da las mismas cuevas");
    }
}

// Oceanos hondos (GeneratorAdventure.cpp: oceanFloor): mas mar lejos del origen, fondo
// hondo con plataforma, talud, llanura abisal, dorsales, montes y fosas; kelp, corales,
// praderas y anemonas. Cerca del origen (< ~420 m), el mundo de siempre bloque a bloque.
void testOceans() {
    std::printf("Oceanos hondos\n");
    const Generator gen(1234, 62, VoxelGenerator::Adventure);
    Generator old(1234, 62, VoxelGenerator::Adventure);
    old.setDeepOceans(false);
    check(gen.deepOceans() && !old.deepOceans(), "oceanos hondos por defecto (setDeepOceans(false): los de antes)");

    // 1) Cerca del origen nada cambia: columnas a menos de 410 m y los chunks del centro.
    bool same_near = true;
    for (int z = -410; z <= 410; z += 3) {
        for (int x = -410; x <= 410; x += 3) {
            if (x * x + z * z > 410 * 410) continue;
            const Generator::AdventureColumn a = gen.adventureColumn(x, z), b = old.adventureColumn(x, z);
            same_near = same_near && a.height == b.height && a.water == b.water && a.biome == b.biome && a.ocean == 0.0f;
        }
    }
    check(same_near, "a menos de 410 m del origen el terreno es el de siempre");
    check(chunkHash(gen, -4, -4, 8) == chunkHash(old, -4, -4, 8), "los chunks del pueblo de inicio, iguales bloque a bloque");

    // 2) Cuanto mar y que hondo (rejilla de 64 m en 16 x 16 km).
    int samples = 0, sea_old = 0, sea_new = 0, deep30 = 0, abyss = 0, trench = 0, islands = 0, max_depth = 0;
    BlockPos deep_at{}, trench_at{};
    bool have_deep = false;
    std::array<int, 8> hist{};  // agua sobre el suelo por tramos de 8
    for (int z = -8192; z < 8192; z += 64) {
        for (int x = -8192; x < 8192; x += 64) {
            ++samples;
            const Generator::AdventureColumn a = gen.adventureColumn(x, z), b = old.adventureColumn(x, z);
            sea_old += b.biome == Generator::AOcean ? 1 : 0;
            sea_new += a.biome == Generator::AOcean ? 1 : 0;
            if (a.ocean > 0.0f && a.height >= 62) ++islands;
            const int depth = gen.oceanDepth(x, z);
            if (depth <= 0) continue;
            ++hist[static_cast<std::size_t>(std::min(depth / 8, 7))];
            max_depth = std::max(max_depth, depth);
            if (depth >= 30) ++deep30;
            if (a.height <= 20) {
                ++abyss;
                if (!have_deep && a.seabed == 0 && depth >= 42) deep_at = BlockPos{x, a.height, z}, have_deep = true;
            }
            if (a.height <= 9) {
                if (trench == 0) trench_at = BlockPos{x, a.height, z};
                ++trench;
            }
        }
    }
    std::printf("    mar: antes %.1f%% -> ahora %.1f%% de %d muestras; hondo (>=30) %d, abisal (y<=20) %d, fosas (y<=9) %d, islas %d, max %d\n",
                100.0 * sea_old / samples, 100.0 * sea_new / samples, samples, deep30, abyss, trench, islands, max_depth);
    std::printf("    agua sobre el suelo (tramos de 8):");
    for (int h : hist) std::printf(" %d", h);
    std::printf("\n    abisal en (%d, %d, %d), fosa en (%d, %d, %d)\n", deep_at.x, deep_at.y, deep_at.z, trench_at.x, trench_at.y, trench_at.z);
    check(sea_new > sea_old * 3 && sea_new * 4 > samples, "mucho mas mar lejos del origen (mas de un cuarto del mundo)");
    check(deep30 * 2 > sea_new && abyss > 0 && max_depth >= 50, "el oceano es hondo: la mitad a mas de 30 m, llanura abisal a y <= 20, mas de 50 m");
    check(trench > 0, "fosas que bajan casi a la roca madre (y <= 9)");

    // 3) Los chunks del fondo: bloques, vida, sin cuevas abiertas al agua.
    std::array<int, block::Count> count{};
    int leaks = 0, chunks = 0, deep_columns = 0, depth_mismatch = 0;
    double total_ms = 0.0, worst_ms = 0.0;
    const auto scan = [&](const BlockPos& at, int n) {
        const int cx0 = static_cast<int>(std::floor(static_cast<float>(at.x) / kChunkSize)) - n / 2;
        const int cz0 = static_cast<int>(std::floor(static_cast<float>(at.z) / kChunkSize)) - n / 2;
        for (int dz = 0; dz < n; ++dz) {
            for (int dx = 0; dx < n; ++dx) {
                ChunkData chunk;
                chunk.cx = cx0 + dx;
                chunk.cz = cz0 + dz;
                const auto t0 = std::chrono::steady_clock::now();
                gen.generate(chunk);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                total_ms += ms;
                worst_ms = std::max(worst_ms, ms);
                ++chunks;
                for (BlockId id : chunk.blocks) ++count[id];
                for (int z = 0; z < kChunkSize; ++z) {
                    for (int x = 0; x < kChunkSize; ++x) {
                        const int wx = chunk.cx * kChunkSize + x, wz = chunk.cz * kChunkSize + z;
                        // Agua junto a aire por debajo de la superficie: una cueva abierta al mar.
                        for (int y = 1; y < 61; ++y) {
                            if (!block::isWater(chunk.get(x, y, z))) continue;
                            if (chunk.get(x, y - 1, z) == block::Air) ++leaks;
                            if (x > 0 && chunk.get(x - 1, y, z) == block::Air) ++leaks;
                            if (x < kChunkSize - 1 && chunk.get(x + 1, y, z) == block::Air) ++leaks;
                            if (z > 0 && chunk.get(x, y, z - 1) == block::Air) ++leaks;
                            if (z < kChunkSize - 1 && chunk.get(x, y, z + 1) == block::Air) ++leaks;
                        }
                        // oceanDepth = las celdas de agua (o plantas) entre el suelo y la superficie.
                        const int depth = gen.oceanDepth(wx, wz);
                        if (depth <= 0) continue;
                        ++deep_columns;
                        int wet = 0;
                        for (int y = 61; y > 0 && (block::isWet(chunk.get(x, y, z)) || chunk.get(x, y, z) == block::Ice); --y) ++wet;
                        const BlockId stop = chunk.get(x, 61 - wet, z);  // los cabezos de coral suben el suelo
                        if (wet != depth && stop != block::CoralBlock && stop != block::CoralTube) ++depth_mismatch;
                    }
                }
            }
        }
    };
    if (have_deep) scan(deep_at, 6);
    if (trench > 0) scan(trench_at, 4);
    // Mar calido y poco hondo (arrecifes) y templado (kelp): los primeros sitios lejos del origen.
    BlockPos reef_at{}, kelp_at{};
    bool have_reef = false, have_kelp = false;
    for (int i = 0; i < 160 * 160 && !(have_reef && have_kelp); ++i) {
        const int x = (i % 160 - 80) * 48, z = (i / 160 - 80) * 48;
        const Generator::AdventureColumn a = gen.adventureColumn(x, z);
        if (a.ocean < 0.9f || a.biome != Generator::AOcean) continue;
        const int wd = 61 - a.height;
        if (!have_reef && a.temperature > 0.4f && wd >= 4 && wd <= 12) reef_at = BlockPos{x, a.height, z}, have_reef = true;
        if (!have_kelp && a.temperature < 0.2f && wd >= 10 && wd <= 30) kelp_at = BlockPos{x, a.height, z}, have_kelp = true;
    }
    if (have_reef) scan(reef_at, 6);
    if (have_kelp) scan(kelp_at, 6);
    std::printf("    %d chunks: kelp %d pradera %d coral %d+%d abanicos %d limo %d anemonas %d basalto %d magma %d; %.2f ms por chunk (peor %.2f)\n",
                chunks, count[block::Kelp], count[block::SeaGrass], count[block::CoralBlock], count[block::CoralTube], count[block::CoralFan],
                count[block::SeaSilt], count[block::SeaGlow], count[block::Basalt], count[block::Magma], total_ms / std::max(1, chunks), worst_ms);
    check(have_deep && have_reef && have_kelp, "hay llanura abisal, mar calido poco hondo y mar templado");
    check(count[block::Kelp] > 100 && count[block::SeaGrass] > 20, "bosques de kelp y praderas marinas");
    check(count[block::CoralBlock] + count[block::CoralTube] > 30 && count[block::CoralFan] > 0, "arrecifes de coral con abanicos");
    check(count[block::SeaSilt] > 100 && count[block::SeaGlow] > 0, "limo oscuro y anemonas luminosas en lo hondo");
    check(leaks == 0, "ninguna cueva se abre al agua del mar (ni hacia abajo ni de lado)");
    std::printf("    %d columnas de mar, %d con oceanDepth distinto del agua de verdad\n", deep_columns, depth_mismatch);
    check(deep_columns > 0 && depth_mismatch == 0, "oceanDepth coincide con el agua del chunk");
    check(total_ms / std::max(1, chunks) < 40.0, "un chunk del oceano en menos de 40 ms");

    // 4) Determinista y el terreno lejano conoce el fondo.
    if (have_deep) {
        ChunkData a, b;
        a.cx = b.cx = static_cast<int>(std::floor(static_cast<float>(deep_at.x) / kChunkSize));
        a.cz = b.cz = static_cast<int>(std::floor(static_cast<float>(deep_at.z) / kChunkSize));
        gen.generate(a);
        const Generator again(1234, 62, VoxelGenerator::Adventure);
        again.generate(b);
        check(a.blocks == b.blocks, "el mismo chunk del oceano sale igual (determinista)");
        Generator::FarColumn fc;
        gen.farColumn(deep_at.x, deep_at.z, fc);
        check(fc.height == deep_at.y && fc.water == 62 && fc.sea && fc.top == block::SeaSilt,
              "el terreno lejano ve el fondo abisal (limo a y <= 20)");
        // Tiempo de las columnas lejanas en mar abierto.
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 4096; ++i) gen.farColumn(deep_at.x + (i % 64) * 8, deep_at.z + (i / 64) * 8, fc, false);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 4096.0;
        std::printf("    columna lejana en el oceano: %.2f us\n", us);
    }
    // Las plantas del agua cuentan como agua (malla del agua, nadar).
    check(block::isAquatic(block::Kelp) && block::isWet(block::SeaGrass) && !block::isWet(block::CoralBlock) &&
              blockDef(block::Kelp).light_filter == blockDef(block::Water).light_filter && blockDef(block::SeaGlow).light > 0,
          "kelp y praderas dentro del agua; el coral es un bloque");
}

// CRAMION_ISLANDS=<carpeta>: censo de las islas flotantes (cuantas, de que
// tamano, por provincia de archipielago), tiempo por chunk y, en la carpeta,
// volcados de bloques de unas cuantas (islaN.bin) y la paleta (paleta.bin)
// para mirarlas con un script de Python. CRAMION_ISLANDS_N: cuantas volcar.
void islandReport(const char* folder) {
    const Generator gen(1234, 62, VoxelGenerator::Adventure);
    constexpr int kArea = 12000, kCell = 560;
    std::vector<Generator::WildIsland> all;
    Generator::WildIsland buf[4];
    const auto census_t0 = std::chrono::steady_clock::now();
    for (int gz = -kArea / kCell; gz <= kArea / kCell; ++gz) {
        for (int gx = -kArea / kCell; gx <= kArea / kCell; ++gx) {
            const int n = gen.wildIslandCell(gx, gz, buf);
            for (int i = 0; i < n; ++i) all.push_back(buf[i]);
        }
    }
    std::printf("Censo: %.2f ms por celda de 560 m (sin cache)\n",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - census_t0).count() / ((2 * kArea / kCell + 1) * (2 * kArea / kCell + 1)));
    int parents = 0, sats = 0, lakes = 0, falls = 0, caves = 0, kinds[4] = {0, 0, 0, 0};
    float min_r = 1e9f, max_r = 0.0f, nearest = 1e9f;
    std::map<std::pair<int, int>, int> per_region;  // centro de la provincia -> islas grandes
    std::map<int, int> parents_by_region;
    for (const auto& isl : all) {
        (isl.satellite ? sats : parents)++;
        kinds[isl.kind & 3]++;
        lakes += isl.lake_r > 0.0f ? 1 : 0;
        falls += isl.falls;
        caves += isl.caves;
        if (!isl.satellite) {
            min_r = std::min(min_r, isl.r);
            max_r = std::max(max_r, isl.r);
            const Generator::WildCell w = gen.wildRegion(isl.cx, isl.cz);
            parents_by_region[w.region]++;
            if (w.region == Generator::WSky) per_region[{static_cast<int>(w.cx), static_cast<int>(w.cz)}]++;
        }
        nearest = std::min(nearest, std::hypot(isl.cx, isl.cz) - isl.r * 1.55f);
    }
    // Provincias de archipielago en el area (por sus centros).
    std::map<std::pair<int, int>, int> sky_regions;
    for (int z = -kArea; z <= kArea; z += 100) {
        for (int x = -kArea; x <= kArea; x += 100) {
            const Generator::WildCell w = gen.wildRegion(static_cast<float>(x), static_cast<float>(z));
            if (w.region == Generator::WSky && w.strength > 0.6f) sky_regions[{static_cast<int>(w.cx), static_cast<int>(w.cz)}]++;
        }
    }
    int sky_parents = 0;
    for (const auto& [k, v] : per_region) {
        if (sky_regions.count(k) != 0) sky_parents += v;
    }
    std::printf("Islas en %d x %d km: %d (%d grandes, %d satelites); radio grande %.0f-%.0f m\n", 2 * kArea / 1000, 2 * kArea / 1000,
                static_cast<int>(all.size()), parents, sats, static_cast<double>(min_r), static_cast<double>(max_r));
    std::printf("  bosque %d, pradera %d, picos %d, ruinas %d; lagos %d, cascadas %d, camaras %d\n", kinds[0], kinds[1], kinds[2], kinds[3],
                lakes, falls, caves);
    std::printf("  provincias de archipielago %d con %d islas grandes (%.1f por provincia)\n", static_cast<int>(sky_regions.size()),
                sky_parents, sky_regions.empty() ? 0.0 : static_cast<double>(sky_parents) / static_cast<double>(sky_regions.size()));
    for (const auto& [region, n] : parents_by_region) std::printf("  grandes en provincias de tipo %d: %d\n", region, n);
    std::printf("  la orilla de isla mas cerca del origen: %.0f m\n", static_cast<double>(nearest));

    // Volcados: las grandes mas cerca de (1600, 1600) y unas cuantas de cada tipo.
    std::vector<const Generator::WildIsland*> pick;
    {
        std::vector<const Generator::WildIsland*> order;
        for (const auto& isl : all) order.push_back(&isl);
        std::sort(order.begin(), order.end(), [](auto* a, auto* b) {
            return std::hypot(a->cx - 1600.0f, a->cz - 1600.0f) < std::hypot(b->cx - 1600.0f, b->cz - 1600.0f);
        });
        const int want = std::getenv("CRAMION_ISLANDS_N") ? std::atoi(std::getenv("CRAMION_ISLANDS_N")) : 4;
        bool kind_seen[4] = {false, false, false, false};
        for (auto* isl : order) {
            if (static_cast<int>(pick.size()) >= want) break;
            if (isl->satellite || isl->r < 60.0f) continue;
            if (kind_seen[isl->kind & 3] && static_cast<int>(pick.size()) < 4) continue;
            kind_seen[isl->kind & 3] = true;
            pick.push_back(isl);
        }
    }
    // Paleta (colores de las texturas).
    {
        const auto& names = textureNames();
        std::vector<gfx::VoxelTextureLayer> layers(names.size());
        for (std::size_t i = 0; i < names.size(); ++i) buildTexture(names[i], 16, "", layers[i]);
        const auto palette = buildFarPalette(layers);
        std::FILE* f = std::fopen((std::string(folder) + "/paleta.bin").c_str(), "wb");
        if (f != nullptr) {
            for (int i = 0; i < 256; ++i) {
                const auto t = i < block::Count ? palette->top[static_cast<std::size_t>(i)] : FarPalette::Entry{};
                const auto s = i < block::Count ? palette->side[static_cast<std::size_t>(i)] : FarPalette::Entry{};
                const std::uint8_t b[6] = {t.r, t.g, t.b, s.r, s.g, s.b};
                std::fwrite(b, 1, 6, f);
            }
            std::fclose(f);
        }
    }
    double total_ms = 0.0, worst_ms = 0.0, warm_total = 0.0, warm_worst = 0.0;
    int chunks = 0, mismatches = 0;
    for (std::size_t k = 0; k < pick.size(); ++k) {
        const Generator::WildIsland& isl = *pick[k];
        const int m = static_cast<int>(isl.r * 1.55f) + 24;
        const int cx0 = static_cast<int>(std::floor((isl.cx - static_cast<float>(m)) / kChunkSize));
        const int cz0 = static_cast<int>(std::floor((isl.cz - static_cast<float>(m)) / kChunkSize));
        const int cx1 = static_cast<int>(std::floor((isl.cx + static_cast<float>(m)) / kChunkSize));
        const int cz1 = static_cast<int>(std::floor((isl.cz + static_cast<float>(m)) / kChunkSize));
        const int w = (cx1 - cx0 + 1) * kChunkSize, d = (cz1 - cz0 + 1) * kChunkSize;
        std::vector<std::uint8_t> vol(static_cast<std::size_t>(w) * d * kWorldHeight);
        for (int cz = cz0; cz <= cz1; ++cz) {
            for (int cx = cx0; cx <= cx1; ++cx) {
                ChunkData chunk;
                chunk.cx = cx;
                chunk.cz = cz;
                const auto t0 = std::chrono::steady_clock::now();
                gen.generate(chunk);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                total_ms += ms;
                worst_ms = std::max(worst_ms, ms);
                ++chunks;
                {  // otra vez, con las caches de las islas ya llenas
                    ChunkData again;
                    again.cx = cx;
                    again.cz = cz;
                    const auto t1 = std::chrono::steady_clock::now();
                    gen.generate(again);
                    const double ms2 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
                    warm_total += ms2;
                    warm_worst = std::max(warm_worst, ms2);
                    if (again.blocks != chunk.blocks) ++mismatches;
                }
                for (int y = 0; y < kWorldHeight; ++y) {
                    for (int lz = 0; lz < kChunkSize; ++lz) {
                        for (int lx = 0; lx < kChunkSize; ++lx) {
                            const int vx = (cx - cx0) * kChunkSize + lx, vz = (cz - cz0) * kChunkSize + lz;
                            vol[(static_cast<std::size_t>(y) * d + vz) * w + vx] = static_cast<std::uint8_t>(chunk.get(lx, y, lz));
                        }
                    }
                }
            }
        }
        char name[64];
        std::snprintf(name, sizeof(name), "/isla%d.bin", static_cast<int>(k));
        std::FILE* f = std::fopen((std::string(folder) + name).c_str(), "wb");
        if (f == nullptr) continue;
        const int header[8] = {w, d, kWorldHeight, cx0 * kChunkSize, cz0 * kChunkSize, static_cast<int>(isl.r), isl.kind, isl.falls};
        std::fwrite(header, sizeof(int), 8, f);
        std::fwrite(vol.data(), 1, vol.size(), f);
        std::fclose(f);
        std::printf("  isla %d: (%.0f, %.0f) r %.0f tipo %d cima %.0f borde %.0f panza %.0f lago %.0f cascadas %d camaras %d tuneles %d -> %s\n",
                    static_cast<int>(k), static_cast<double>(isl.cx), static_cast<double>(isl.cz), static_cast<double>(isl.r), isl.kind,
                    static_cast<double>(isl.top), static_cast<double>(isl.slab), static_cast<double>(isl.depth), static_cast<double>(isl.lake_r),
                    isl.falls, isl.caves, isl.tunnels, name);
        for (int c = 0; c < isl.caves; ++c) {
            std::printf("    camara %d tipo %d en volcado (%d, %d, %d) radios %.0f/%.0f\n", c, isl.cave[c].kind,
                        static_cast<int>(isl.cave[c].x) - cx0 * kChunkSize, static_cast<int>(isl.cave[c].y),
                        static_cast<int>(isl.cave[c].z) - cz0 * kChunkSize, static_cast<double>(isl.cave[c].rx), static_cast<double>(isl.cave[c].ry));
        }
    }
    std::printf("  %d chunks con isla: %.2f ms de media, peor %.2f ms (con las caches llenas: %.2f / %.2f ms); distintos al repetir: %d\n",
                chunks, total_ms / std::max(1, chunks), worst_ms, warm_total / std::max(1, chunks), warm_worst, mismatches);
}

// Rios nuevos (GeneratorAdventure.cpp, lejos del origen): el agua por encima del mar, que
// baja por escalones con cascadas (bloques de cascada quietos) y pozas; sin fugas.
void testRivers() {
    std::printf("Rios y cascadas\n");
    const Generator gen(1234, 62, VoxelGenerator::Adventure);
    // Columnas de rio con el agua por encima del mar y los saltos entre vecinas.
    int river_cols = 0, high_cols = 0, steps = 0, max_level = 0;
    std::vector<BlockPos> falls;
    for (int z = -6000; z < 6000; z += 8) {
        for (int x = -6000; x < 6000; x += 8) {
            const Generator::AdventureColumn a = gen.adventureColumn(x, z);
            if (a.biome != Generator::ARiver || a.water <= a.height) continue;
            ++river_cols;
            if (a.river_level <= 62) continue;
            ++high_cols;
            max_level = std::max(max_level, a.river_level);
            for (int d = 0; d < 2; ++d) {
                const Generator::AdventureColumn b = gen.adventureColumn(x + (d == 0 ? 4 : 0), z + (d == 1 ? 4 : 0));
                if (b.river_level > 0 && b.water > b.height && b.water != a.water) {
                    ++steps;
                    if (falls.size() < 6 && (falls.empty() || std::abs(falls.back().x - x) + std::abs(falls.back().z - z) > 800)) {
                        falls.push_back(BlockPos{x, a.water, z});
                    }
                }
            }
        }
    }
    std::printf("    %d columnas de rio con agua (rejilla de 8 m en 12 x 12 km), %d por encima del mar (hasta y=%d), %d saltos\n",
                river_cols, high_cols, max_level, steps);
    check(high_cols > 100 && max_level >= 62 + 8 && steps > 10, "rios por encima del nivel del mar que bajan por escalones");

    // Los chunks de unas cascadas: bloques de cascada y ninguna fuga del agua al aire.
    int falling = 0, leaks = 0, chunks = 0;
    double total_ms = 0.0;
    for (const BlockPos& at : falls) {
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                ChunkData chunk;
                chunk.cx = static_cast<int>(std::floor(static_cast<float>(at.x) / kChunkSize)) + dx;
                chunk.cz = static_cast<int>(std::floor(static_cast<float>(at.z) / kChunkSize)) + dz;
                const auto t0 = std::chrono::steady_clock::now();
                gen.generate(chunk);
                total_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                ++chunks;
                for (int y = 1; y < kWorldHeight - 1; ++y) {
                    for (int z = 0; z < kChunkSize; ++z) {
                        for (int x = 0; x < kChunkSize; ++x) {
                            const BlockId id = chunk.get(x, y, z);
                            falling += id == block::FallingWater ? 1 : 0;
                            if (!block::isWater(id)) continue;
                            if (chunk.get(x, y - 1, z) == block::Air) ++leaks;
                            if (x > 0 && chunk.get(x - 1, y, z) == block::Air) ++leaks;
                            if (x < kChunkSize - 1 && chunk.get(x + 1, y, z) == block::Air) ++leaks;
                            if (z > 0 && chunk.get(x, y, z - 1) == block::Air) ++leaks;
                            if (z < kChunkSize - 1 && chunk.get(x, y, z + 1) == block::Air) ++leaks;
                        }
                    }
                }
            }
        }
    }
    std::printf("    %zu cascadas: %d chunks, %d bloques de cascada, %d caras de agua contra aire; %.2f ms por chunk\n", falls.size(),
                chunks, falling, leaks, total_ms / std::max(1, chunks));
    check(!falls.empty() && falling > 0, "cascadas donde el rio salta de un escalon al de abajo");
    check(leaks == 0, "el agua de los rios no queda colgando junto al aire (cascada o ribazo)");
    // El terreno lejano ve el rio a su altura.
    if (!falls.empty()) {
        Generator::FarColumn fc;
        gen.farColumn(falls[0].x, falls[0].z, fc);
        check(fc.water == falls[0].y && !fc.sea, "el terreno lejano ve el rio por encima del mar");
    }
}

int main() {
    if (std::getenv("CRAMION_STRUCT_WATER") != nullptr) {  // censo de agua dentro de las piezas de las estructuras
        const int n = structureWaterReport(true);
        std::printf("\n%d celdas de agua en piezas\n", n);
        return n == 0 ? 0 : 1;
    }
    if (const char* folder = std::getenv("CRAMION_ISLANDS")) {  // censo y volcados de las islas flotantes
        islandReport(folder);
        return 0;
    }
    if (const char* dump = std::getenv("CRAMION_FAR_DUMP")) {
        dumpFarTerrain(dump);
        return 0;
    }
    if (const char* view = std::getenv("CRAMION_FAR_VIEW")) {
        viewFarTerrain(view);
        return 0;
    }
    if (std::getenv("CRAMION_COLLISION_ONLY") != nullptr) {  // COLISION-MODELOS: solo vallas, sillas y mesas (rapido)
        testModelCollision();
        std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("CRAMION_FAR_ONLY") != nullptr) {  // solo el terreno lejano (rapido)
        testFarTerrain();
        std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
    if (const char* view = std::getenv("CRAMION_LUSH_VIEW")) {  // NOCHE-MUNDO: vistas de los biomas frondosos
        lushView(view);
        return 0;
    }
    if (std::getenv("CRAMION_LUSH_ONLY") != nullptr) {  // NOCHE-MUNDO: solo los biomas frondosos (rapido)
        testLush();
        std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("CRAMION_WILD_ONLY") != nullptr) {  // solo las tierras salvajes (rapido)
        testWild();
        std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("CRAMION_PACKS_ONLY") != nullptr) {  // solo los paquetes de texturas (rapido)
        testTexturePacks();
        std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("CRAMION_CAVES_ONLY") != nullptr) {  // solo las entradas de cueva (rapido)
        testCaveEntrances();
        std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("CRAMION_IMMENSE_ONLY") != nullptr) {  // solo las cuevas inmensas (CRAMION_CAVES_AT=x,z)
        testImmenseCaves();
        std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("CRAMION_LIGHTS_ONLY") != nullptr) {  // solo las luces (rapido)
        testCatalog();
        testMesher();
        testLights();
        testLightLeaks();
        testDoors();
        std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("CRAMION_DEEP_ONLY") != nullptr) {  // solo el Reino Profundo y las simas (CRAMION_DEEP_DUMP=carpeta: PNG)
        failures += runDeepRealmTests();
        std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("CRAMION_RAILS_ONLY") != nullptr) {  // solo los rieles y las senales (rapido)
        testRails();
        std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("CRAMION_OCEAN_ONLY") != nullptr) {  // solo los oceanos hondos (rapido)
        testOceans();
        testRivers();
        std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("CRAMION_IDS16_ONLY") != nullptr) {  // IDS16: solo los ids de 16 bits (rapido)
        testIds16();
        std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("CRAMION_MESHDETAIL_ONLY") != nullptr) {  // MALLAS-DETALLE: solo los niveles de adornos
        testMeshDetail();
        std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("CRAMION_VEG_ONLY") != nullptr) {  // solo la densidad de arboles (rapido)
        testVegetation();
        std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
    testCatalog();
    testTexturePacks();
    testAdventure();
    testMesher();
    testGeneration();
    testQueries();
    testModelCollision();
    testPhysics();
    testWater();
    testSaving();
    testIds16();
    testMeshDetail();  // MALLAS-DETALLE
    testPerformance();
    testStructures();
    // La capital de 1234 y la de 20260925 caen junto a un lago mas alto que su suelo.
    check(structureWaterReport(false, {1234, 20260925}, 0) == 0, "sin agua de lagos ni rios dentro de las casas de la capital");
    testVegetation();
    testCaveEntrances();
    testImmenseCaves();
    testOceans();
    testRivers();
    testFarTerrain();
    testWild();
    testLush();  // NOCHE-MUNDO
    testLights();
    testLightLeaks();
    testDoors();
    testRails();
    dumpStructuresForRender();  // solo con CRAMION_STRUCT_DUMP
    failures += runDimensionTests();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
