// NOCHE-MUNDO: biomas frondosos (GeneratorLush.cpp) y bloques nuevos (198..210, 241..254, 163..165, 177..178).
// Lo incluye VoxelTests.cpp (usa check, Generator, FarTerrain...).
//   CRAMION_LUSH_ONLY=1   solo estas pruebas
//   CRAMION_LUSH_SEEDS=a,b  semillas extra para el informe de "donde esta cada bioma"

void lushReport(int seed, bool checks_on);
bool findLushRuin(const Generator& gen, int k, BlockPos& out);

void testLushBlocks() {
    std::printf("Bloques nuevos (biomas frondosos y construccion)\n");
    static const BlockId ids[] = {block::JungleGrass, block::JungleLog, block::JungleLeaves, block::JungleVines, block::BlossomGrass,
                                  block::CherryLog, block::CherryLeaves, block::BlossomPetals, block::SwampGrass, block::Mud,
                                  block::MangroveLog, block::MangroveLeaves, block::LilyPad, block::SavannaGrass, block::AcaciaLog,
                                  block::AcaciaLeaves, block::LeafLitter, block::AutumnLeavesOrange, block::AutumnLeavesRed,
                                  block::AutumnLeavesYellow, block::JunglePlanks, block::CherryPlanks, block::AcaciaPlanks,
                                  block::MangrovePlanks, block::WhiteBricks, block::SandstoneBricks, block::QuartzTiles,
                                  block::CopperBlock, block::OxidizedCopper, block::PolishedStone, block::StainedGlassBlue,
                                  block::StainedGlassRose};
    bool named = true, textures = true, placeable = true;
    for (const BlockId id : ids) {
        const BlockDef& d = blockDef(id);
        named = named && !d.name.empty() && d.name.rfind("unused_", 0) != 0 && blockId(d.name) == id && !d.label.empty();
        for (const std::string& t : {d.top, d.side, d.bottom}) {
            textures = textures && std::find(textureNames().begin(), textureNames().end(), t) != textureNames().end();
        }
        placeable = placeable && d.placeable;
    }
    check(block::Count == 512 && blocks().size() == block::Count, "512 tipos de bloque (IDS16: ids de 16 bits)");
    check(named, "los 32 bloques nuevos con nombre, etiqueta y su id por nombre");
    check(textures && placeable, "todos con sus texturas y se pueden colocar");
    // Los de antes siguen en su sitio (mundos guardados).
    check(blockId("heart_root") == block::HeartRoot && blockId("glow_vines") == block::GlowVines && blockId("elderwood_planks") == 176 &&
              blockId("standing_torch") == 190 && blockId("rail") == 211 && blockId("sea_glow") == 197,
          "los ids de antes no cambian");
    // RAMAS-HZ: 179 es la rama; 180 sigue libre.
    check(blockId("branch") == block::Branch && block::Branch == 179 && !blockDef(179).solid && blockDef(180).shape == BlockShape::Air,
          "179 la rama (sin colision) y 180 sigue libre (aire)");
    // Texturas generadas: hojas y plantas con recorte, el resto macizo.
    const auto alphaRange = [](const char* name, float& lo, float& hi) {
        gfx::VoxelTextureLayer t;
        buildTexture(name, 32, {}, t);
        lo = 1.0f;
        hi = 0.0f;
        for (std::size_t i = 3; i < t.albedo.size(); i += 4) {
            lo = std::min(lo, static_cast<float>(t.albedo[i]) / 255.0f);
            hi = std::max(hi, static_cast<float>(t.albedo[i]) / 255.0f);
        }
    };
    float lo = 0, hi = 0;
    bool ok = true;
    for (const char* n : {"cherry_leaves", "jungle_vines", "blossom_petals", "lily_pad"}) {
        alphaRange(n, lo, hi);
        ok = ok && lo < 0.5f && hi > 0.5f;
    }
    for (const char* n : {"jungle_grass_top", "white_bricks", "quartz_tiles", "copper_block", "mud", "stained_glass_blue"}) {
        alphaRange(n, lo, hi);
        ok = ok && lo > 0.9f;
    }
    check(ok, "texturas generadas: hojas, lianas, petalos y nenufar con recorte; el resto macizo");
    check(blockDef(block::LilyPad).model == BlockModel::Plate && !blockDef(block::LilyPad).solid && blockDef(block::JungleVines).shape == BlockShape::Cross,
          "nenufar = losa fina (Plate) que no choca; liana en cruz");
}

void testLush() {
    std::printf("Biomas frondosos (GeneratorLush.cpp)\n");
    testLushBlocks();
    Generator on(1234, 62, VoxelGenerator::Adventure);
    Generator off(1234, 62, VoxelGenerator::Adventure);
    off.setLushBiomes(false);

    // 1) Cerca del origen (< 1,35 km) el mundo es identico bloque a bloque (y los tintes).
    {
        bool same = true;
        int n = 0;
        for (int cz = -80; cz <= 80; cz += 9) {
            for (int cx = -80; cx <= 80; cx += 9) {
                if ((cx * 16) * (cx * 16) + (cz * 16) * (cz * 16) > 1330 * 1330) continue;
                ChunkData a, b;
                a.cx = b.cx = cx;
                a.cz = b.cz = cz;
                on.generate(a);
                off.generate(b);
                same = same && a.blocks == b.blocks && a.grass_tint == b.grass_tint && a.foliage_tint == b.foliage_tint;
                ++n;
            }
        }
        std::printf("    %d chunks comparados cerca del origen\n", n);
        check(same, "a menos de 1,35 km el mundo es identico al de antes (bloques y tintes)");
    }
    // 2) Lejos, las provincias de antes (cordilleras, islas, carmesi, rosa, ceniza, marchito) no cambian.
    {
        bool same = true;
        int compared = 0, changed_none = 0;
        for (int z = -9000; z <= 9000; z += 97) {
            for (int x = -9000; x <= 9000; x += 97) {
                const Generator::AdventureColumn b = off.adventureColumn(x, z);
                const Generator::AdventureColumn a = on.adventureColumn(x, z);
                if (b.region == Generator::WNone) {
                    if (a.height != b.height || a.biome != b.biome) ++changed_none;
                    continue;
                }
                // Columnas de otra provincia pero con una frondosa al lado (el borde se mezcla): fuera.
                if (Generator::isLushRegion(on.wildRegion(static_cast<float>(x) + 300.0f, static_cast<float>(z)).region) ||
                    Generator::isLushRegion(on.wildRegion(static_cast<float>(x) - 300.0f, static_cast<float>(z)).region) ||
                    Generator::isLushRegion(on.wildRegion(static_cast<float>(x), static_cast<float>(z) + 300.0f).region) ||
                    Generator::isLushRegion(on.wildRegion(static_cast<float>(x), static_cast<float>(z) - 300.0f).region)) {
                    continue;
                }
                ++compared;
                same = same && a.height == b.height && a.biome == b.biome && a.water == b.water && a.region == b.region;
            }
        }
        std::printf("    %d columnas de las provincias de antes comparadas; %d de tierra normal cambian\n", compared, changed_none);
        check(same && compared > 1000, "las provincias salvajes de antes no cambian");
        check(changed_none > 0, "la tierra normal lejana pasa a ser frondosa");
    }
    // 3) Rincones: templo, santuario, cabana, circulo de piedras y atalaya.
    {
        const char* kinds[5] = {"templo de la selva", "santuario de los cerezos", "cabana del pantano", "circulo de piedras", "atalaya del otono"};
        int found = 0;
        for (int k = 0; k < 5; ++k) {
            BlockPos at{};
            if (findLushRuin(on, k, at)) {
                ++found;
                std::printf("    %s en (%d, %d)\n", kinds[k], at.x, at.z);
            } else {
                std::printf("    %s: ninguno a menos de 11 km\n", kinds[k]);
            }
        }
        check(found >= 4, "rincones de los biomas frondosos (al menos 4 de los 5 tipos)");
    }
    // 3) Donde esta cada bioma, chunks, arboles y terreno lejano.
    lushReport(1234, true);
    if (const char* extra = std::getenv("CRAMION_LUSH_SEEDS")) {
        for (const char* p = extra; *p;) {
            lushReport(std::atoi(p), false);
            while (*p && *p != ',') ++p;
            if (*p == ',') ++p;
        }
    }
}

void lushReport(int seed, bool checks_on) {
    const Generator gen(seed, 62, VoxelGenerator::Adventure);
    std::printf("  semilla %d\n", seed);
    constexpr int kKinds = 5;
    const char* names[kKinds] = {"Verdant Jungle", "Blossom Vale", "Mirefen Swamp", "Sunscorch Savanna", "Emberleaf Woods"};
    std::array<float, kKinds> nearest{};
    std::array<BlockPos, kKinds> where{};
    std::array<int, kKinds> columns{};
    nearest.fill(1e9f);
    for (int z = -12000; z <= 12000; z += 32) {
        for (int x = -12000; x <= 12000; x += 32) {
            const float d = std::sqrt(static_cast<float>(x) * static_cast<float>(x) + static_cast<float>(z) * static_cast<float>(z));
            if (d < 1300.0f) continue;
            const Generator::AdventureColumn c = gen.adventureColumn(x, z);
            if (!Generator::isLushBiome(c.biome)) continue;
            const int k = c.biome - Generator::AJungle;
            ++columns[static_cast<std::size_t>(k)];
            if (c.strength < 0.9f || c.water > c.height) continue;
            if (d < nearest[static_cast<std::size_t>(k)]) {
                nearest[static_cast<std::size_t>(k)] = d;
                where[static_cast<std::size_t>(k)] = BlockPos{x, c.height, z};
            }
        }
    }
    bool found = true;
    for (int k = 0; k < kKinds; ++k) {
        const auto i = static_cast<std::size_t>(k);
        std::printf("    %s: a %.0f m (%d, %d, %d), %.1f %% de la tierra muestreada\n", names[k], static_cast<double>(nearest[i]), where[i].x,
                    where[i].y, where[i].z, 100.0 * columns[i] / (751.0 * 751.0));
        found = found && nearest[i] < 12000.0f;
    }
    if (!checks_on) return;
    check(found, "los cinco biomas frondosos aparecen a menos de 12 km");

    // Chunks de cada uno: bloques, determinismo, tiempo y arboles.
    std::array<int, block::Count> count{};
    double worst_ms = 0.0, total_ms = 0.0;
    int chunks = 0, tallest_jungle = 0, ponds = 0;
    bool same = true;
    auto shared = std::make_shared<Generator>(seed, 62, VoxelGenerator::Adventure);
    auto palette = std::make_shared<FarPalette>();
    bool far_ok = true, canopy = false, well_formed = true;
    for (int k = 0; k < kKinds; ++k) {
        if (nearest[static_cast<std::size_t>(k)] >= 12000.0f) continue;
        const BlockPos at = where[static_cast<std::size_t>(k)];
        for (int dz = -2; dz <= 2; ++dz) {
            for (int dx = -2; dx <= 2; ++dx) {
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
                // Columna de tronco de selva mas alta (los gigantes).
                for (int lz = 0; lz < kChunkSize; ++lz) {
                    for (int lx = 0; lx < kChunkSize; ++lx) {
                        int run = 0;
                        for (int y = 1; y < kWorldHeight; ++y) {
                            run = chunk.get(lx, y, lz) == block::JungleLog ? run + 1 : 0;
                            tallest_jungle = std::max(tallest_jungle, run);
                        }
                        const Generator::AdventureColumn c = gen.adventureColumn(chunk.cx * kChunkSize + lx, chunk.cz * kChunkSize + lz);
                        if (c.pond > c.height) ++ponds;
                    }
                }
            }
        }
        // Terreno lejano: el bloque de arriba de lejos es el de la superficie del chunk y hay copas.
        Generator::FarColumn f;
        gen.farColumn(at.x, at.z, f);
        ChunkData chunk;
        chunk.cx = static_cast<int>(std::floor(static_cast<float>(at.x) / kChunkSize));
        chunk.cz = static_cast<int>(std::floor(static_cast<float>(at.z) / kChunkSize));
        gen.generate(chunk);
        const BlockId top = chunk.get(at.x - chunk.cx * kChunkSize, at.y, at.z - chunk.cz * kChunkSize);
        if (top != f.top && top != block::Moss && top != block::Grass && top != block::Mud && top != block::LeafLitter) {
            std::printf("    %s: de cerca %s, de lejos %s\n", names[k], blockDef(top).name.c_str(), blockDef(f.top).name.c_str());
            far_ok = false;
        }
        for (int z = at.z - 40; z <= at.z + 40; z += 3) {
            for (int x = at.x - 40; x <= at.x + 40; x += 3) {
                Generator::FarColumn g;
                gen.farColumn(x, z, g);
                canopy = canopy || g.canopy > g.height;
            }
        }
        for (int level : {0, 3}) {
            const int span = FarTerrain::kSamples << level;
            std::vector<Generator::FarColumn> cols;
            FarTerrain::sampleTile(*shared, level, static_cast<int>(std::floor(static_cast<float>(at.x) / static_cast<float>(span))),
                                   static_cast<int>(std::floor(static_cast<float>(at.z) / static_cast<float>(span))), cols);
            std::vector<std::uint32_t> v;
            FarTerrain::meshTile(cols, level, *palette, true, v);
            well_formed = well_formed && v.size() % 8 == 0 && v.size() / 8 < gfx::VoxelPass::kMaxQuadsPerSection;
        }
    }
    std::printf("    selva: cesped %d, troncos %d, lianas %d (tronco mas alto %d); cerezos: hojas %d, petalos %d; pantano: cesped %d, "
                "barro %d, mangle %d, nenufares %d, charcas %d col.; sabana: hierba %d, acacia %d; otono: hojarasca %d, hojas %d/%d/%d\n",
                count[block::JungleGrass], count[block::JungleLog], count[block::JungleVines], tallest_jungle, count[block::CherryLeaves],
                count[block::BlossomPetals], count[block::SwampGrass], count[block::Mud], count[block::MangroveLog], count[block::LilyPad],
                ponds, count[block::SavannaGrass], count[block::AcaciaLeaves], count[block::LeafLitter], count[block::AutumnLeavesOrange],
                count[block::AutumnLeavesRed], count[block::AutumnLeavesYellow]);
    std::printf("    %.2f ms por chunk de media (peor %.2f ms)\n", total_ms / std::max(1, chunks), worst_ms);
    check(same, "un chunk frondoso sale igual dos veces (determinista)");
    check(count[block::JungleGrass] > 0 && count[block::BlossomGrass] > 0 && count[block::SwampGrass] > 0 && count[block::SavannaGrass] > 0 &&
              count[block::LeafLitter] > 0,
          "la superficie propia de cada bioma (la que mira el juego para saber el bioma)");
    check(count[block::JungleLeaves] > 0 && count[block::JungleVines] > 0 && count[block::CherryLeaves] > 0 && count[block::MangroveLog] > 0 &&
              count[block::AcaciaLeaves] > 0 && count[block::AutumnLeavesOrange] + count[block::AutumnLeavesRed] + count[block::AutumnLeavesYellow] > 0,
          "arboles de cada bioma (selva con lianas, cerezos, manglares, acacias, otono)");
    check(tallest_jungle >= 24, "arboles gigantes en la selva (tronco de mas de 24 m)");
    check(ponds > 0 && count[block::LilyPad] > 0, "charcas con nenufares");
    check(total_ms / std::max(1, chunks) < 45.0, "un chunk frondoso en menos de 45 ms de media");
    check(far_ok && canopy && well_formed, "el terreno lejano conoce la superficie y las copas (mallas bien formadas)");

    // Sin cortes entre chunks: un arbol que cae entre dos chunks sale entero. Se genera una zona de 3 x 3
    // chunks y se compara con lo que sale generando cada chunk por separado en otro generador (el mismo
    // resultado: nada depende del orden ni de los vecinos ya generados).
    {
        const BlockPos at = where[0];
        const Generator other(seed, 62, VoxelGenerator::Adventure);
        bool equal = true;
        for (int dz = 1; dz >= -1; --dz) {
            for (int dx = 1; dx >= -1; --dx) {
                ChunkData a, b;
                a.cx = b.cx = static_cast<int>(std::floor(static_cast<float>(at.x) / kChunkSize)) + dx;
                a.cz = b.cz = static_cast<int>(std::floor(static_cast<float>(at.z) / kChunkSize)) + dz;
                gen.generate(a);
                other.generate(b);
                equal = equal && a.blocks == b.blocks;
            }
        }
        // Y en los bordes de un chunk de selva hay hojas que siguen en el de al lado.
        ChunkData left, right;
        left.cx = static_cast<int>(std::floor(static_cast<float>(at.x) / kChunkSize));
        left.cz = right.cz = static_cast<int>(std::floor(static_cast<float>(at.z) / kChunkSize));
        right.cx = left.cx + 1;
        gen.generate(left);
        gen.generate(right);
        int across = 0, alone = 0;
        for (int y = 1; y < kWorldHeight; ++y) {
            for (int z = 0; z < kChunkSize; ++z) {
                const bool l = left.get(kChunkSize - 1, y, z) == block::JungleLeaves, r = right.get(0, y, z) == block::JungleLeaves;
                if (l && r) ++across;
                else if (l != r) ++alone;
            }
        }
        std::printf("    borde entre chunks de selva: %d hojas siguen al otro lado, %d no\n", across, alone);
        check(equal && (across > 0 || alone == 0), "sin cortes ni diferencias entre chunks");
    }
}

// El rincon de un bioma frondoso (k = 0 selva .. 4 otono) mas cercano al origen: replica la rejilla de
// GeneratorLush.cpp (placeLushTrees) y mira que el chunk tenga su bloque propio.
bool findLushRuin(const Generator& gen, int k, BlockPos& out) {
    const auto hash3 = [](int x, int y, int z, std::uint32_t seed) {
        std::uint32_t h = static_cast<std::uint32_t>(x) * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u ^
                          static_cast<std::uint32_t>(z) * 83492791u ^ seed * 2654435761u;
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        return h;
    };
    const auto r01 = [&](int x, int y, int z, std::uint32_t seed) { return static_cast<float>(hash3(x, y, z, seed) & 0xFFFFFF) / 16777215.0f; };
    const auto s = static_cast<std::uint32_t>(gen.seed()) + 7717u;
    constexpr int kCell = 224;
    static const BlockId sign[5] = {block::CarvedStone, block::Altar, block::MangrovePlanks, block::Campfire, block::Paving};
    float best = 1e9f;
    for (int gz = -50; gz <= 50; ++gz) {
        for (int gx = -50; gx <= 50; ++gx) {
            if (r01(gx, 70, gz, s) > 0.5f) continue;
            const int rx = gx * kCell + 24 + static_cast<int>(r01(gx, 71, gz, s) * (kCell - 48));
            const int rz = gz * kCell + 24 + static_cast<int>(r01(gx, 72, gz, s) * (kCell - 48));
            const float d = std::hypot(static_cast<float>(rx), static_cast<float>(rz));
            if (d >= best || gen.adventureColumn(rx, rz).biome != Generator::AJungle + k) continue;
            ChunkData chunk;
            chunk.cx = static_cast<int>(std::floor(static_cast<float>(rx) / kChunkSize));
            chunk.cz = static_cast<int>(std::floor(static_cast<float>(rz) / kChunkSize));
            gen.generate(chunk);
            bool found = false;
            for (std::size_t i = 0; i < chunk.blocks.size() && !found; ++i) found = chunk.blocks[i] == sign[k];
            if (!found) continue;
            best = d;
            out = BlockPos{rx, 0, rz};
        }
    }
    return best < 1e9f;
}

// CRAMION_LUSH_VIEW=<carpeta> [CRAMION_LUSH_TEXTURES=<Assets/Textures/Bloques>] [CRAMION_LUSH_SEED=1234]: una vista
// isometrica (PNG) de 8 x 8 chunks de cada bioma frondoso, con el color medio de cada textura, para mirarla a ojo.
void lushView(const char* folder) {
    const int seed = std::getenv("CRAMION_LUSH_SEED") ? std::atoi(std::getenv("CRAMION_LUSH_SEED")) : 1234;
    const char* textures = std::getenv("CRAMION_LUSH_TEXTURES");
    const Generator gen(seed, 62, VoxelGenerator::Adventure);
    using C3 = std::array<float, 3>;
    std::map<std::string, C3> cache;
    const auto color = [&](const std::string& name) {  // color medio (lo opaco) de una textura
        if (const auto it = cache.find(name); it != cache.end()) return it->second;
        gfx::VoxelTextureLayer t;
        buildTexture(name, 16, textures != nullptr ? textures : "", t);
        double r = 0, g = 0, b = 0, n = 0;
        for (std::size_t i = 0; i + 3 < t.albedo.size(); i += 4) {
            if (t.albedo[i + 3] < 128) continue;
            r += t.albedo[i] / 255.0;
            g += t.albedo[i + 1] / 255.0;
            b += t.albedo[i + 2] / 255.0;
            n += 1;
        }
        const C3 c = n > 0 ? C3{float(r / n), float(g / n), float(b / n)} : C3{1, 0, 1};
        cache[name] = c;
        return c;
    };
    const char* names[5] = {"jungle", "blossom", "swamp", "savanna", "autumn"};
    for (int k = 0; k < 5; ++k) {
        float best = 1e9f;  // el sitio mas cercano bien dentro del bioma
        int bx = 0, bz = 0;
        for (int z = -12000; z <= 12000; z += 48) {
            for (int x = -12000; x <= 12000; x += 48) {
                const float d = std::hypot(static_cast<float>(x), static_cast<float>(z));
                if (d < 1300.0f || d >= best) continue;
                const Generator::AdventureColumn c = gen.adventureColumn(x, z);
                if (c.biome != Generator::AJungle + k || c.strength < 0.99f || c.river > 0.02f || c.water > c.height) continue;
                bool clear = true;  // lejos de aldeas y caminos (que talan los arboles)
                for (int oz = -64; oz <= 64 && clear; oz += 32)
                    for (int ox = -64; ox <= 64 && clear; ox += 32)
                        clear = !(gen.structures() && gen.structures()->blocksTrees(x + ox, z + oz, 0.0f)) &&
                                gen.adventureColumn(x + ox, z + oz).biome == Generator::AJungle + k;
                if (clear) {
                    best = d;
                    bx = x;
                    bz = z;
                }
            }
        }
        if (best >= 1e9f) continue;
        if (std::getenv("CRAMION_LUSH_RUINS")) {  // en vez del bioma, el rincon (templo, santuario...) mas cercano
            BlockPos ruin{};
            if (!findLushRuin(gen, k, ruin)) continue;
            bx = ruin.x;
            bz = ruin.z;
        }
        const int kN = std::getenv("CRAMION_LUSH_RUINS") ? 3 : 8, S = std::getenv("CRAMION_LUSH_RUINS") ? 8 : 4;
        const int c0x = static_cast<int>(std::floor(static_cast<float>(bx) / kChunkSize)) - kN / 2;
        const int c0z = static_cast<int>(std::floor(static_cast<float>(bz) / kChunkSize)) - kN / 2;
        std::vector<ChunkData> chunks(kN * kN);
        for (int i = 0; i < kN * kN; ++i) {
            chunks[static_cast<std::size_t>(i)].cx = c0x + i % kN;
            chunks[static_cast<std::size_t>(i)].cz = c0z + i / kN;
            gen.generate(chunks[static_cast<std::size_t>(i)]);
        }
        const int W = kN * kChunkSize;
        const auto at = [&](int x, int y, int z) -> BlockId {
            if (x < 0 || z < 0 || x >= W || z >= W || y < 0 || y >= kWorldHeight) return block::Air;
            return chunks[static_cast<std::size_t>((x / kChunkSize) + (z / kChunkSize) * kN)].get(x % kChunkSize, y, z % kChunkSize);
        };
        int ymin = kWorldHeight, ymax = 0;
        for (int z = 0; z < W; ++z) {
            for (int x = 0; x < W; ++x) {
                for (int y = kWorldHeight - 1; y > 0; --y) {
                    if (at(x, y, z) == block::Air) continue;
                    ymin = std::min(ymin, y);
                    ymax = std::max(ymax, y);
                    break;
                }
            }
        }
        ymin = std::max(1, ymin - 6);
        const int img_w = 2 * W * S + 16, img_h = W * S + (ymax - ymin + 3) * S + 16;
        asset::ImageRgba8 img;
        img.width = static_cast<std::uint32_t>(img_w);
        img.height = static_cast<std::uint32_t>(img_h);
        img.pixels.assign(static_cast<std::size_t>(img_w) * static_cast<std::size_t>(img_h) * 4, 255);
        for (std::size_t i = 0; i < img.pixels.size(); i += 4) {
            img.pixels[i] = 40;
            img.pixels[i + 1] = 44;
            img.pixels[i + 2] = 52;
        }
        const auto plot = [&](int px, int py, const C3& c, float shade) {
            if (px < 0 || py < 0 || px >= img_w || py >= img_h) return;
            const std::size_t i = (static_cast<std::size_t>(py) * static_cast<std::size_t>(img_w) + static_cast<std::size_t>(px)) * 4;
            for (int ch = 0; ch < 3; ++ch) img.pixels[i + static_cast<std::size_t>(ch)] = static_cast<std::uint8_t>(std::clamp(c[static_cast<std::size_t>(ch)] * shade * 255.0f, 0.0f, 255.0f));
        };
        const auto tintOf = [&](int x, int z, bool foliage) {
            const ChunkData& ch = chunks[static_cast<std::size_t>((x / kChunkSize) + (z / kChunkSize) * kN)];
            const std::uint16_t t = (foliage ? ch.foliage_tint : ch.grass_tint)[static_cast<std::size_t>((x % kChunkSize) + (z % kChunkSize) * kChunkSize)];
            return C3{float(t & 15) / 15.0f, float((t >> 4) & 15) / 15.0f, float((t >> 8) & 15) / 15.0f};
        };
        // De atras (x + z pequeno) a delante; en cada diagonal, de abajo arriba.
        for (int sum = 0; sum <= 2 * (W - 1); ++sum) {
            for (int x = std::max(0, sum - (W - 1)); x <= std::min(W - 1, sum); ++x) {
                const int z = sum - x;
                for (int y = ymin; y <= ymax; ++y) {
                    const BlockId id = at(x, y, z);
                    if (id == block::Air) continue;
                    const BlockDef& d = blockDef(id);
                    const bool water = block::isWater(id);
                    const auto covered = [&](BlockId n) { return n != block::Air && blockDef(n).opaque; };
                    if (covered(at(x, y + 1, z)) && covered(at(x + 1, y, z)) && covered(at(x, y, z + 1))) continue;
                    if (water && block::isWater(at(x, y + 1, z))) continue;
                    C3 top = water ? C3{0.22f, 0.45f, 0.60f} : color(d.top);
                    C3 side = water ? top : color(d.side);
                    if (d.tint != BlockTint::None) {
                        const C3 t = tintOf(x, z, d.tint == BlockTint::Foliage);
                        for (std::size_t ch = 0; ch < 3; ++ch) {
                            top[ch] *= t[ch];
                            if (d.tint == BlockTint::Foliage) side[ch] *= t[ch];
                        }
                    }
                    // Vertice de arriba del rombo: x hacia abajo a la derecha, z hacia abajo a la izquierda.
                    const int sx = (x - z) * S + W * S + 8;
                    const int sy = (x + z) * S / 2 - (y - ymin) * S + (ymax - ymin + 2) * S + 8;
                    const bool cross = d.shape == BlockShape::Cross;
                    for (int py = 0; py <= S; ++py) {
                        const int half = py <= S / 2 ? py * 2 : (S - py) * 2;
                        for (int px = -half; px <= half; ++px) {
                            if (cross && ((px + py) & 1)) continue;
                            plot(sx + px, sy + py, top, 1.0f);
                        }
                    }
                    if (cross || water) continue;
                    for (int px = -S; px <= S; ++px) {
                        const int edge = S / 2 + (S - std::abs(px)) / 2;  // borde de abajo del rombo
                        for (int py = 0; py < S; ++py) plot(sx + px, sy + edge + py, side, px < 0 ? 0.78f : 0.60f);
                    }
                }
            }
        }
        const std::string file = std::string(folder) + (std::getenv("CRAMION_LUSH_RUINS") ? "/ruina_" : "/lush_") + names[k] + ".png";
        asset::saveImagePng(file, img);
        std::printf("  %s: (%d, %d) -> %s\n", names[k], bx, bz, file.c_str());
    }
}
