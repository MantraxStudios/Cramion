// Pruebas del mundo de bloques (consola, sin GPU): catalogo y texturas,
// mallador (caras hacia fuera, oclusion), generacion determinista, luz del
// cielo y de antorchas, rayo, colision de una caja, agua, guardar y cargar
// mundos y rendimiento. Devuelve 0 si todo va.

#include "CramionCore/ecs/World.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/voxel/Voxel.h"

#include "../src/voxel/VoxelInternal.h"

#include <CramionFX/vk/VoxelPass.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

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
    check(v.size() == 10 * 4 * 2, "entre dos bloques no hay cara");
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
    s.blocks[static_cast<std::size_t>(SectionSnapshot::index(1, 1, 1))] = block::TallGrass;
    meshSection(s, v);
    check(v.size() == 4 * 4 * 2, "una planta: dos planos por sus dos caras");
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
    check(vox.getBlock(sx, floor_y, sz) == block::Water, "al cavar el fondo entra el agua");
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

}  // namespace

int main() {
    testCatalog();
    testMesher();
    testGeneration();
    testQueries();
    testPhysics();
    testWater();
    testSaving();
    testPerformance();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
