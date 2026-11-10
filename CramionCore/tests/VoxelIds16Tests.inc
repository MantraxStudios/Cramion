// IDS16: pruebas de los ids de bloque de 16 bits (incluido desde VoxelTests.cpp; CRAMION_IDS16_ONLY=1 solo estas).
//   - catalogo: block::Count, los bloques de prueba de ids altos (256, 300, 511) y el limite de capas de textura
//     (10 bits en el vertice: menos de 1024);
//   - el mallador dibuja un id alto con su textura;
//   - guardar y cargar un mundo con ids altos (formato de chunk version 2);
//   - un chunk guardado con el formato de antes (version 1, ids de 8 bits) carga igual;
//   - kKeep de los planos de estructuras fuera de los ids;
//   - memoria de los bloques por chunk (y cuanto ocuparia recortando el aire de arriba).

void testIds16() {
    std::printf("IDS16: ids de bloque de 16 bits\n");
    static_assert(sizeof(BlockId) == 2, "BlockId de 16 bits");
    check(blocks().size() == block::Count && block::Count > 256, "hay ids por encima de 255");
    const BlockId high[] = {block::IdTest256, block::IdTest300, block::IdTest511};
    bool named = true;
    for (const BlockId id : high) {
        const BlockDef& d = blockDef(id);
        named = named && d.name == "id_test_" + std::to_string(id) && blockId(d.name) == id && !d.placeable &&
                d.shape == BlockShape::Cube && d.solid;
    }
    check(named, "bloques de prueba 256, 300 y 511 (por nombre, no se colocan)");
    check(blockDef(block::IdTest300).side == "gold_block" && textureLayer("gold_block") > 0, "el 300 usa una textura que ya existia");
    // CIUDADES-GRANDES: 257 y 258 ya tienen bloque (estanteria y muro secretos); el primer libre de arriba es el 259.
    check(blockDef(259).shape == BlockShape::Air && blockDef(259).name == "unused_259" && blockDef(255).name == "unused_255",
          "los ids libres son aire");
    check(blockId("secret_bookshelf") == block::SecretBookshelf && blockId("secret_wall") == block::SecretWall &&
              !blockDef(block::SecretBookshelf).solid && !blockDef(block::SecretWall).solid && blockDef(block::SecretWall).opaque &&
              !blockDef(block::SecretWall).placeable,
          "CIUDADES-GRANDES: estanteria y muro secretos (257, 258): se ven macizos pero se cruzan");
    check(blockDef(static_cast<BlockId>(block::Count + 5)).shape == BlockShape::Air, "fuera del catalogo: aire");
    std::printf("    %zu texturas (capas) de %d posibles en el vertice\n", textureNames().size(), 1024);
    check(textureNames().size() < 1024, "las capas caben en los 10 bits del vertice");
    check(kKeep >= block::Count && Blueprint(2, 2, 2).get(0, 0, 0) == kKeep, "kKeep de los planos fuera de los ids");

    // Mallador: un bloque de id 300 se dibuja con la capa de su textura.
    {
        SectionSnapshot s;
        s.light.fill(0xF0);
        s.grass_tint.fill(0xFFF);
        s.foliage_tint.fill(0xFFF);
        s.blocks[static_cast<std::size_t>(SectionSnapshot::index(5, 5, 5))] = block::IdTest300;
        std::vector<std::uint32_t> v;
        meshSection(s, v);
        bool layer = !v.empty();
        const std::uint32_t want = static_cast<std::uint32_t>(textureLayer("gold_block"));
        for (std::size_t i = 1; i < v.size(); i += 2) layer = layer && (v[i] & 1023u) == want;
        check(v.size() == 6 * 4 * 2 && layer, "el mallador dibuja el id 300 (6 caras con la capa de gold_block)");
    }

    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "cramion_voxel_ids16";
    std::error_code ec;
    fs::remove_all(root, ec);
    ecs::World world;
    makeWorld(world, 1);
    const Vec3 viewer{0.5f, 120.0f, 0.5f};
    int h = 0;
    {
        VoxelSystem vox;
        vox.setSaveRoot(root);
        vox.start(world);
        check(vox.newWorld("Ids16", 777), "mundo nuevo");
        vox.waitUntilReady(viewer, 1, 60.0f);
        h = vox.surfaceHeight(0, 0);
        const bool set = vox.setBlock(0, h + 1, 0, block::IdTest300) && vox.setBlock(1, h + 1, 0, block::IdTest256) &&
                         vox.setBlock(2, h + 1, 0, block::IdTest511);
        check(set && vox.getBlock(0, h + 1, 0) == block::IdTest300 && vox.getBlock(2, h + 1, 0) == block::IdTest511,
              "setBlock / getBlock con ids altos");
        check(!vox.setBlock(3, h + 1, 0, block::Count), "un id fuera del catalogo no se pone");
        const VoxelHit hit = vox.raycast(Vec3{0.5f, static_cast<float>(h) + 4.5f, 0.5f}, Vec3{0.0f, -1.0f, 0.0f}, 10.0f);
        check(hit.hit && hit.id == block::IdTest300, "el rayo devuelve el id 300");
        check(vox.setBlock(5, 250, 5, block::IdTest511) && vox.getBlock(5, 250, 5) == block::IdTest511 &&
                  vox.getBlock(5, 249, 5) == block::Air,
              "poner un bloque en el aire recortado de arriba (y = 250)");
        check(vox.saveWorld(), "guardar");
    }
    // El archivo del chunk (0, 0) es de la version 2.
    {
        std::ifstream in(root / "Ids16" / "chunks" / "0.0.bin", std::ios::binary);
        char head[5]{};
        in.read(head, 5);
        check(in && std::memcmp(head, "CRVX", 4) == 0 && head[4] == 2, "el chunk se guarda con la version 2 (16 bits)");
    }
    // Un chunk del formato de antes (version 1: tramos de 3 bytes, ids de 8 bits): el (5, 5) de ladrillos hasta
    // y = 99 y aire encima.
    {
        std::vector<std::uint8_t> bytes = {'C', 'R', 'V', 'X', 1};
        const auto run = [&](int n, int id) {
            while (n > 0) {
                const int k = std::min(n, 65535);
                bytes.push_back(static_cast<std::uint8_t>(k & 0xFF));
                bytes.push_back(static_cast<std::uint8_t>(k >> 8));
                bytes.push_back(static_cast<std::uint8_t>(id));
                n -= k;
            }
        };
        run(256 * 100, block::Bricks);
        run(256 * 156, block::Air);
        fs::create_directories(root / "Ids16" / "chunks", ec);
        std::ofstream out(root / "Ids16" / "chunks" / "5.5.bin", std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    {
        VoxelSystem vox;
        vox.setSaveRoot(root);
        vox.start(world);
        check(vox.loadWorld("Ids16"), "cargar");
        vox.waitUntilReady(viewer, 1, 60.0f);
        check(vox.getBlock(0, h + 1, 0) == block::IdTest300 && vox.getBlock(1, h + 1, 0) == block::IdTest256 &&
                  vox.getBlock(2, h + 1, 0) == block::IdTest511,
              "los ids 300, 256 y 511 siguen tras guardar y cargar");
        check(vox.getBlock(5, 250, 5) == block::IdTest511, "el bloque de y = 250 (en lo recortado) se guarda y vuelve");
        const Vec3 old_chunk{5.0f * 16.0f + 8.0f, 120.0f, 5.0f * 16.0f + 8.0f};
        vox.waitUntilReady(old_chunk, 0, 60.0f);
        check(vox.isLoaded(88, 88) && vox.getBlock(88, 99, 88) == block::Bricks && vox.getBlock(88, 0, 88) == block::Bricks &&
                  vox.getBlock(88, 100, 88) == block::Air,
              "un chunk guardado con 8 bits (version 1) carga igual");
    }
    fs::remove_all(root, ec);

    // ChunkData sin el aire de arriba (trimAir / at / set).
    {
        constexpr std::size_t kSec = static_cast<std::size_t>(kChunkSize) * kChunkSize * kChunkSize;
        ChunkData c;
        c.blocks[static_cast<std::size_t>(cellIndex(3, 40, 3))] = block::IdTest300;
        c.trimAir();
        const bool trimmed = c.blocks.size() == 3 * kSec;
        const bool above = c.get(3, 200, 3) == block::Air && c.get(3, 40, 3) == block::IdTest300;
        c.set(static_cast<std::size_t>(cellIndex(1, 200, 1)), block::Air);  // aire encima: no crece
        const bool no_grow = c.blocks.size() == 3 * kSec;
        c.set(static_cast<std::size_t>(cellIndex(1, 200, 1)), block::IdTest256);
        check(trimmed && above && no_grow && c.get(1, 200, 1) == block::IdTest256 && c.blocks.size() == 13 * kSec,
              "recortar el aire de arriba, leer encima (aire) y crecer al poner");
    }

    // Memoria real de los bloques por chunk (Aventura, la semilla del juego; cerca y lejos del origen: montanas).
    {
        Generator g(20260925, 62, VoxelGenerator::Adventure);
        double bytes = 0;
        long long chunks = 0;
        int top_max = 0;
        for (int cz = -60; cz <= 60; cz += 12) {
            for (int cx = -60; cx <= 60; cx += 12) {
                ChunkData c;
                c.cx = cx;
                c.cz = cz;
                g.generate(c);
                c.trimAir();
                bytes += static_cast<double>(c.blocks.capacity() * sizeof(BlockId));
                top_max = std::max(top_max, static_cast<int>(c.blocks.size() / (kChunkSize * kChunkSize)));
                ++chunks;
            }
        }
        const double per = bytes / static_cast<double>(chunks) / 1024.0;
        std::printf("    bloques por chunk (%lld chunks de Aventura): %.1f KB con 16 bits recortados; antes 64 KB (8 bits); "
                    "16 bits sin recortar 128 KB (altura recortada max %d)\n",
                    chunks, per, top_max);
        check(per < 128.0, "recortar el aire de arriba ahorra memoria");
    }
}
