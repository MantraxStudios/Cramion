// Pruebas de las estructuras del mundo de bloques (aldeas, ciudades, la
// capital del origen, mazmorras...): planificacion determinista, estampado
// igual en cualquier orden de carga, piezas enteras a traves de los bordes de
// los chunks, puntos para el juego y rendimiento. Se incluye dentro del
// espacio anonimo de VoxelTests.cpp (usa check()).



std::uint64_t chunkHash(const ChunkData& c) {
    std::uint64_t h = 1469598103934665603ull;
    for (BlockId b : c.blocks) h = (h ^ b) * 1099511628211ull;
    return h;
}

// Huella del plan de una region (piezas, bloques, calles, aplanados y puntos):
// sirve para comprobar que la capital (region 0,0) no cambia ni un bloque.
std::uint64_t planFingerprint(const RegionPlan& plan) {
    std::uint64_t h = 1469598103934665603ull;
    const auto add = [&](std::int64_t v) { h = (h ^ static_cast<std::uint64_t>(v)) * 1099511628211ull; };
    const auto addf = [&](float f) { add(static_cast<std::int64_t>(std::lround(f * 64.0f))); };
    const auto adds = [&](const std::string& s) {
        for (char c : s) add(c);
        add(0x7F);
    };
    for (const Structure& s : plan.structures) {
        adds(s.info.type);
        adds(s.info.name);
        add(s.min_x), add(s.min_y), add(s.min_z), add(s.max_x), add(s.max_y), add(s.max_z);
        for (const Terraform& t : s.terraforms) add(t.cx), add(t.cz), addf(t.inner), addf(t.outer), add(t.base), add(t.surface), add(t.under);
        for (const Road& r : s.roads) addf(r.x0), addf(r.z0), addf(r.x1), addf(r.z1), addf(r.half_width), add(r.surface), add(r.alt);
        for (const Piece& p : s.pieces) {
            add(p.x), add(p.y), add(p.z), add(p.rot), add(p.plan->w), add(p.plan->h), add(p.plan->d);
            for (BlockId b : p.plan->cells) add(b == kKeep ? 255 : b);  // IDS16: kKeep era 255 (las huellas no cambian)
        }
        for (const StructurePoint& p : s.info.points) {
            adds(p.kind);
            adds(p.tag);
            addf(p.position.x), addf(p.position.y), addf(p.position.z);
        }
    }
    return h;
}

int countPoints(const StructureInfo& s, const std::string& kind) {
    return static_cast<int>(std::count_if(s.points.begin(), s.points.end(), [&](const StructurePoint& p) { return p.kind == kind; }));
}

// Busca una estructura de un tipo en las regiones alrededor del origen.
bool findStructure(const Generator& gen, const std::string& type, StructureInfo& out, int max_regions = 9) {
    const Structures* st = gen.structures();
    for (int r = 1; r <= max_regions; ++r) {
        for (int rz = -r; rz <= r; ++rz) {
            for (int rx = -r; rx <= r; ++rx) {
                if (std::max(std::abs(rx), std::abs(rz)) != r) continue;
                for (const Structure& s : st->region(rx, rz)->structures) {
                    if (s.info.type == type) {
                        out = s.info;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

void testStructures() {
    std::printf("Estructuras (aldeas, ciudades, mazmorras)\n");
    check(blockId("lantern") == block::Lantern && blockDef(block::Lantern).light == 15 && blockId("Wooden Door") == block::Door &&
              !blockDef(block::Door).solid && blockId("roof_red") == block::RoofRed,
          "bloques nuevos con nombre (farol con luz, puerta que se pasa, tejas)");
    gfx::VoxelTextureLayer table;
    buildTexture("table_side", 32, {}, table, true);
    int holes = 0;
    for (std::size_t i = 3; i < table.albedo.size(); i += 4) holes += table.albedo[i] < 128 ? 1 : 0;
    check(holes > 100, "la mesa tiene patas (recorte) tambien en color liso");

    // --- La capital en el origen, con cualquier semilla ---
    for (int seed : {1234, 777, 20260925}) {
        const Generator gen(seed, 62, VoxelGenerator::Adventure);
        const auto t0 = std::chrono::steady_clock::now();
        const auto list = gen.structures()->nearby(0.0f, 70.0f, 0.0f, 40.0f);
        const double plan_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const StructureInfo* capital = nullptr;
        for (const StructureInfo& s : list)
            if (s.type == "ciudad" && s.name == "Kettlecrown") capital = &s;
        char what[160];
        std::snprintf(what, sizeof what, "semilla %d: la capital Kettlecrown en el origen", seed);
        check(capital != nullptr, what);
        if (capital == nullptr) continue;
        // Kettlecrown no cambia ni un bloque con los asentamientos nuevos (huellas de
        // antes de anadirlos; si se cambia la capital a proposito, actualizarlas).
        const std::uint64_t print = planFingerprint(*gen.structures()->region(0, 0));
        // 09/11 (1.0.28): huellas renovadas. Las de antes ya no valian desde antes de CIUDADES-GRANDES (fallaba igual); los
        // BLOQUES de la capital son los de la base de CIUDADES-GRANDES (huella sin puntos igual: 8d6ca8b36fb606ea,
        // 1f5581451193e578, e18ba8cceabc3225); solo cambian los puntos (punto "chimenea" de las herrerias, para el humo).
        const std::uint64_t expected = seed == 1234 ? 0x7fb46d921d9c5e87ull : (seed == 777 ? 0x30718e6213f36fbcull : 0x8fbb0825af31b432ull);
        std::printf("    semilla %d: huella del plan de la capital %016llx\n", seed, static_cast<unsigned long long>(print));
        check(print == expected, "la capital Kettlecrown sigue igual bloque a bloque (huella del plan)");
        std::printf("    semilla %d: %d npc, %d mercaderes, %d camas, %d cofres, plan %.1f ms, caja %.0f x %.0f\n", seed,
                    countPoints(*capital, "npc"), countPoints(*capital, "mercader"), countPoints(*capital, "cama"),
                    countPoints(*capital, "cofre"), plan_ms, static_cast<double>(capital->max.x - capital->min.x),
                    static_cast<double>(capital->max.z - capital->min.z));
        check(countPoints(*capital, "inicio") == 1 && countPoints(*capital, "npc") >= 80 && countPoints(*capital, "mercader") >= 10,
              "la capital: punto de inicio, muchos aldeanos y mercaderes");
        bool king = false, smith = false, keeper = false;
        for (const StructurePoint& p : capital->points) {
            king = king || (p.kind == "npc" && p.tag == "rey");
            smith = smith || (p.kind == "npc" && p.tag == "herrero");
            keeper = keeper || (p.kind == "npc" && p.tag == "tabernero");
        }
        check(king && smith && keeper, "rey en el trono, herrero en el yunque y tabernero en la barra");
        // El punto de inicio esta sobre suelo firme con aire encima.
        for (const StructurePoint& p : capital->points) {
            if (p.kind != "inicio") continue;
            const int x = static_cast<int>(std::floor(p.position.x)), y = static_cast<int>(std::floor(p.position.y)),
                      z = static_cast<int>(std::floor(p.position.z));
            ChunkData c;
            c.cx = static_cast<int>(std::floor(static_cast<float>(x) / kChunkSize));
            c.cz = static_cast<int>(std::floor(static_cast<float>(z) / kChunkSize));
            gen.generate(c);
            const int lx = x - c.cx * kChunkSize, lz = z - c.cz * kChunkSize;
            check(blockDef(c.get(lx, y - 1, lz)).solid && !blockDef(c.get(lx, y, lz)).solid && !blockDef(c.get(lx, y + 1, lz)).solid,
                  "el inicio esta en el suelo de la plaza (solido debajo, aire encima)");
            check(gen.terrainHeight(x, z) == y - 1, "surfaceHeight sigue el terreno aplanado");
        }
    }

    const Generator gen(1234, 62, VoxelGenerator::Adventure);
    // --- Determinismo entre ejecuciones y ordenes de carga ---
    {
        const Generator other(1234, 62, VoxelGenerator::Adventure);
        std::map<std::pair<int, int>, std::uint64_t> forward, backward;
        for (int cz = -4; cz <= 4; ++cz)
            for (int cx = -4; cx <= 4; ++cx) {
                ChunkData c;
                c.cx = cx;
                c.cz = cz;
                gen.generate(c);
                forward[{cx, cz}] = chunkHash(c);
            }
        // Otro generador (otra cache), al reves y en varios hilos.
        std::vector<std::pair<int, int>> order;
        for (int cz = 4; cz >= -4; --cz)
            for (int cx = 4; cx >= -4; --cx) order.push_back({cx, cz});
        std::vector<std::uint64_t> hashes(order.size());
        std::atomic<std::size_t> next{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&]() {
                for (std::size_t i = next++; i < order.size(); i = next++) {
                    ChunkData c;
                    c.cx = order[i].first;
                    c.cz = order[i].second;
                    other.generate(c);
                    hashes[i] = chunkHash(c);
                }
            });
        }
        for (std::thread& t : threads) t.join();
        for (std::size_t i = 0; i < order.size(); ++i) backward[order[i]] = hashes[i];
        check(forward == backward, "los mismos chunks en otro orden, en 4 hilos y con otra cache (determinista)");
    }

    // --- Piezas enteras a traves de los bordes de los chunks ---
    {
        const auto plan = gen.structures()->region(0, 0);
        int checked_pieces = 0, cells = 0, matched = 0;
        for (const Structure& s : plan->structures) {
            for (const Piece& p : s.pieces) {
                if (checked_pieces >= 12) break;
                if (p.min_x / kChunkSize == p.max_x / kChunkSize && p.min_z / kChunkSize == p.max_z / kChunkSize) continue;
                if (p.plan->w < 9 || p.plan->h < 8 || p.plan->w > 30) continue;  // casas y edificios (no las murallas: las torres las cruzan)
                ++checked_pieces;
                std::map<std::pair<int, int>, std::unique_ptr<ChunkData>> chunks;
                const auto blockAt = [&](int x, int y, int z) {
                    const int cx = static_cast<int>(std::floor(static_cast<float>(x) / kChunkSize));
                    const int cz = static_cast<int>(std::floor(static_cast<float>(z) / kChunkSize));
                    auto& c = chunks[{cx, cz}];
                    if (!c) {
                        c = std::make_unique<ChunkData>();
                        c->cx = cx;
                        c->cz = cz;
                        gen.generate(*c);
                    }
                    return c->get(x - cx * kChunkSize, y, z - cz * kChunkSize);
                };
                for (int lz = 0; lz < p.plan->d; ++lz)
                    for (int lx = 0; lx < p.plan->w; ++lx) {
                        // Mundo de la celda local (la misma cuenta que el estampado).
                        int wx = 0, wz = 0;
                        const int w = p.plan->w, d = p.plan->d;
                        switch (p.rot & 3) {
                            case 0: wx = p.x + lx; wz = p.z + lz; break;
                            case 1: wx = p.x + (d - 1 - lz); wz = p.z + lx; break;
                            case 2: wx = p.x + (w - 1 - lx); wz = p.z + (d - 1 - lz); break;
                            default: wx = p.x + lz; wz = p.z + (w - 1 - lx); break;
                        }
                        for (int ly = 0; ly < p.plan->h; ++ly) {
                            const BlockId id = p.plan->get(lx, ly, lz);
                            if (id == kKeep || id == block::Air) continue;
                            ++cells;
                            const BlockId got = blockAt(wx, p.y + ly, wz);
                            matched += got == id ? 1 : 0;
                            if (got != id && std::getenv("CRAMION_STRUCT_DEBUG")) std::printf("      pieza %dx%dx%d rot %d en (%d,%d): (%d,%d,%d) quiere %s y hay %s\n", p.plan->w, p.plan->h, p.plan->d, p.rot, p.x, p.z, wx, p.y + ly, wz, blockDef(id).name.c_str(), blockDef(got).name.c_str());
                        }
                    }
            }
        }
        std::printf("    %d edificios partidos entre chunks: %d de %d bloques en su sitio\n", checked_pieces, matched, cells);
        check(checked_pieces >= 5 && matched >= cells * 98 / 100, "edificios enteros a traves de los bordes de los chunks");
    }

    // --- Aldeas, mazmorras, torres, ruinas ---
    StructureInfo village, dungeon, tower, ruin;
    const bool has_village = findStructure(gen, "aldea", village);
    const bool has_dungeon = findStructure(gen, "mazmorra", dungeon);
    const bool has_tower = findStructure(gen, "torre", tower, 12);
    const bool has_ruin = findStructure(gen, "ruina", ruin);
    check(has_village && countPoints(village, "npc") >= 6 && countPoints(village, "plaza") >= 1, "aldeas con aldeanos y plaza");
    if (has_village) std::printf("    aldea \"%s\" en (%.0f, %.0f): %d npc\n", village.name.c_str(), static_cast<double>(village.center.x),
                                 static_cast<double>(village.center.z), countPoints(village, "npc"));
    check(has_dungeon && countPoints(dungeon, "entrada") == 1 && countPoints(dungeon, "jefe") >= 1 && countPoints(dungeon, "spawner") >= 4 &&
              countPoints(dungeon, "cofre") >= 1,
          "mazmorras con entrada, jefe, spawners y cofres");
    if (has_dungeon) {
        std::printf("    mazmorra \"%s\" nivel %d: %d spawners, jefe %s\n", dungeon.name.c_str(), dungeon.level, countPoints(dungeon, "spawner"),
                    [&]() {
                        for (const auto& p : dungeon.points)
                            if (p.kind == "jefe") return p.tag.c_str();
                        return "";
                    }());
        StructureInfo inside;
        bool room_found = false;
        for (const StructurePoint& p : dungeon.points) {
            if (p.kind != "spawner") continue;
            if (gen.structures()->at(p.position.x, p.position.y + 0.5f, p.position.z, inside) && inside.type == "mazmorra" && inside.room >= 0) {
                room_found = true;
                break;
            }
        }
        check(room_found, "structureAt dentro de una sala de la mazmorra da su indice");
    }
    check(has_tower && countPoints(tower, "npc") >= 1, "torres de mago con su mago");
    check(has_ruin && countPoints(ruin, "cofre") >= 1, "ruinas con cofre");

    // --- Asentamientos nuevos: castillos reales, estilos de aldea, plazas hostiles y caminos ---
    {
        const Structures* st = gen.structures();
        std::map<std::string, int> types;
        std::map<std::string, int> styles;  // aldeas por estilo (segun el bloque de las paredes)
        int castles = 0, region_roads = 0, regions = 0, joined = 0, boundary_ends = 0;
        double plan_ms = 0.0;
        const int kR = 12;
        for (int rz = -kR; rz <= kR; ++rz)
            for (int rx = -kR; rx <= kR; ++rx) {
                const auto t0 = std::chrono::steady_clock::now();
                const auto plan = st->region(rx, rz);
                plan_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                ++regions;
                region_roads += static_cast<int>(plan->roads.size());
                for (const Structure& s : plan->structures) {
                    ++types[s.info.type];
                    if (s.info.type == "ciudad" && s.info.name != "Kettlecrown") {
                        bool throne = false;
                        for (const StructurePoint& p : s.info.points) throne = throne || (p.kind == "npc" && p.tag == "rey");
                        castles += throne ? 1 : 0;
                    }
                    if (s.info.type != "aldea") continue;
                    int tree = 0, pond = 0, pier = 0;
                    for (const Piece& p : s.pieces) {
                        if (p.plan->w == 15 && p.plan->d == 15 && p.plan->h == 22) ++tree;  // arbol mayor
                        if (p.plan->w == 7 && p.plan->d == 7 && p.plan->h == 3) ++pond;     // charcas del pantano
                        if (p.plan->w == 5 && p.plan->h == 13) ++pier;                        // muelle
                    }
                    ++styles[tree ? "elfica" : (pond ? "pantano" : "otras")];
                    if (pier) ++styles["con muelle"];
                }
                // Caminos que tocan un borde: la region de al lado tiene uno en el mismo punto.
                for (const Road& r : plan->roads) {
                    const float ex[2] = {r.x0, r.x1}, ez[2] = {r.z0, r.z1};
                    for (int k = 0; k < 2; ++k) {
                        const int bx = static_cast<int>(std::lround(ex[k])), bz = static_cast<int>(std::lround(ez[k]));
                        const bool on_x = (bx + kRegionSize / 2) % kRegionSize == 0 && std::abs(ex[k] - static_cast<float>(bx)) < 0.01f;
                        const bool on_z = (bz + kRegionSize / 2) % kRegionSize == 0 && std::abs(ez[k] - static_cast<float>(bz)) < 0.01f;
                        if (!on_x && !on_z) continue;
                        const int nrx = on_x ? (bx < rx * kRegionSize ? rx - 1 : rx + 1) : rx;
                        const int nrz = on_z ? (bz < rz * kRegionSize ? rz - 1 : rz + 1) : rz;
                        if (std::abs(nrx) > kR || std::abs(nrz) > kR) continue;
                        ++boundary_ends;
                        bool found = false;
                        for (const Road& o : st->region(nrx, nrz)->roads)
                            found = found || (std::abs(o.x0 - ex[k]) < 0.01f && std::abs(o.z0 - ez[k]) < 0.01f);
                        joined += found ? 1 : 0;
                    }
                }
            }
        std::printf("    %d regiones (%.1f ms de plan de media):", regions, plan_ms / regions);
        for (const auto& [k, v] : types) std::printf(" %s %d,", k.c_str(), v);
        std::printf("\n    castillos con rey %d; aldeas:", castles);
        for (const auto& [k, v] : styles) std::printf(" %s %d,", k.c_str(), v);
        std::printf("\n    caminos entre regiones: %d tramos, %d de %d bordes siguen en la region de al lado\n", region_roads, joined,
                    boundary_ends);
        check(types["fuerte"] >= 1 && types["campamento"] >= 1 && types["castillo_maldito"] >= 1 && types["ciudad_maldita"] >= 1 &&
                  types["torre_culto"] >= 1,
              "plazas hostiles de todos los tipos (fuerte, campamento, castillo maldito, ciudad maldita, torre del culto)");
        check(castles >= 3, "castillos y ciudades con salon del trono y rey");
        check(styles["elfica"] >= 1, "aldeas elficas con su arbol mayor");
        check(styles["pantano"] >= 1, "aldeas del pantano (casas sobre pilotes y charcas)");
        check(styles["con muelle"] >= 1, "aldeas de la costa con muelle");
        check(region_roads > 20 && boundary_ends > 0 && joined * 100 >= boundary_ends * 70, "caminos que siguen de una region a la de al lado");
        // Las plazas hostiles tienen guarnicion, jefe y cofres, y nada amigo (sin aldeanos).
        for (const char* type : {"fuerte", "campamento", "castillo_maldito", "ciudad_maldita", "torre_culto"}) {
            StructureInfo h;
            if (!findStructure(gen, type, h, kR)) continue;
            std::string boss;
            for (const auto& p : h.points)
                if (p.kind == "jefe") boss = p.tag;
            std::printf("    %s \"%s\" nivel %d: %d spawners, %d cofres, jefe %s, caja %.0f x %.0f x %.0f\n", type, h.name.c_str(), h.level,
                        countPoints(h, "spawner"), countPoints(h, "cofre"), boss.c_str(), static_cast<double>(h.max.x - h.min.x),
                        static_cast<double>(h.max.y - h.min.y), static_cast<double>(h.max.z - h.min.z));
            char what[160];
            std::snprintf(what, sizeof what, "%s: guarnicion (>= 4 spawners), un jefe, cofres y sin aldeanos", type);
            check(countPoints(h, "spawner") >= 4 && countPoints(h, "jefe") == 1 && countPoints(h, "cofre") >= 1 && countPoints(h, "npc") == 0 &&
                      !boss.empty(),
                  what);
            // Estampado: tiempo de los chunks de la plaza y bloques del plano en su sitio.
            double total = 0.0, worst = 0.0;
            int n = 0;
            for (int cz = static_cast<int>(std::floor(h.min.z / kChunkSize)); cz <= static_cast<int>(std::floor(h.max.z / kChunkSize)); ++cz)
                for (int cx = static_cast<int>(std::floor(h.min.x / kChunkSize)); cx <= static_cast<int>(std::floor(h.max.x / kChunkSize)); ++cx) {
                    ChunkData c;
                    c.cx = cx;
                    c.cz = cz;
                    const auto t = std::chrono::steady_clock::now();
                    gen.generate(c);
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
                    total += ms;
                    worst = std::max(worst, ms);
                    ++n;
                }
            std::printf("      %d chunks: %.2f ms de media (peor %.2f)\n", n, total / std::max(1, n), worst);
            check(n > 0 && total / n < 4.0, "los chunks de una plaza hostil se generan rapido");
        }
        // Las aldeas y ciudades amigas: las plazas hostiles quedan lejos. CIUDADES-GRANDES: tambien las metropolis
        // (RegionPlan::shared), los castillos en colinas y los santuarios; y estos, lejos de todo lo demas.
        int too_close = 0, overlap = 0;
        const auto hostile = [](const std::string& t) {
            return t == "fuerte" || t == "campamento" || t == "castillo_maldito" || t == "ciudad_maldita" || t == "torre_culto";
        };
        for (int rz = -kR; rz <= kR; ++rz)
            for (int rx = -kR; rx <= kR; ++rx) {
                const auto plan = st->region(rx, rz);
                std::vector<const Structure*> all;
                plan->forEach([&](const Structure& s) { all.push_back(&s); });
                for (const Structure* a : all) {
                    const bool friendly = a->info.type == "aldea" || a->info.type == "ciudad" || a->info.type == "castillo" ||
                                          a->info.type == "santuario";
                    for (const Structure* b : all) {
                        if (a == b) continue;
                        const float d = std::hypot(a->clear_x - b->clear_x, a->clear_z - b->clear_z);
                        if (friendly && hostile(b->info.type) && d < a->clear_radius + b->clear_radius + 30.0f) ++too_close;
                        const bool fresh = a->info.type == "castillo" || a->info.type == "santuario" || a->info.type == "ciudad";
                        if (fresh && a < b && d < a->clear_radius + b->clear_radius && (a->info.type != "ciudad" || b->info.type == "castillo" ||
                                                                                        b->info.type == "santuario")) ++overlap;
                    }
                }
            }
        check(too_close == 0, "ninguna plaza hostil pegada a una aldea o ciudad");
        check(overlap == 0, "CIUDADES-GRANDES: castillos y santuarios sin pisar otra estructura");
    }

    // --- La API del sistema (coordenadas locales) ---
    {
        ecs::World world;
        VoxelWorld& vw = world.create("Mundo").add<VoxelWorld>();
        vw.seed = 1234;
        vw.generator = VoxelGenerator::Adventure;
        vw.render_distance = 2;
        vw.textures.clear();
        VoxelSystem vox;
        vox.start(world);
        const auto list_near = vox.structuresNear(Vec3{0.0f, 70.0f, 0.0f}, 30.0f);
        StructureInfo at;
        const bool inside = vox.structureAt(Vec3{0.5f, 66.0f, 0.5f}, at);
        check(!list_near.empty() && list_near[0].name == "Kettlecrown" && inside && at.type == "ciudad", "VoxelSystem::structuresNear y structureAt");
        vox.stop();
    }

    // --- Rendimiento: chunks normales y dentro de la capital (el peor caso) ---
    {
        const Generator fresh(4321, 62, VoxelGenerator::Adventure);
        const auto t0 = std::chrono::steady_clock::now();
        fresh.structures()->region(0, 0);
        const double plan_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        double worst = 0.0, total = 0.0;
        int n = 0;
        for (int cz = -6; cz < 6; ++cz)
            for (int cx = -6; cx < 6; ++cx) {
                ChunkData c;
                c.cx = cx;
                c.cz = cz;
                const auto t = std::chrono::steady_clock::now();
                fresh.generate(c);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
                worst = std::max(worst, ms);
                total += ms;
                ++n;
            }
        // Chunks sin estructuras (lejos), para comparar.
        double plain = 0.0;
        int m = 0;
        for (int i = 0; i < 32; ++i) {
            ChunkData c;
            c.cx = 40 + i;
            c.cz = 17;
            const auto t = std::chrono::steady_clock::now();
            fresh.generate(c);
            plain += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
            ++m;
        }
        std::printf("    plan de la capital %.1f ms; chunk en la capital %.2f ms de media (peor %.2f); chunk normal %.2f ms\n", plan_ms,
                    total / n, worst, plain / m);
        check(total / n < 3.0, "un chunk dentro de la capital en menos de 3 ms de media");
    }
}

// --- Agua dentro de las piezas (CRAMION_STRUCT_WATER=1): censo -------------------------
// Agua de mundo (no la de pozos/fuentes, PoolWater) dentro de las cajas de las
// piezas de aldeas, ciudades y castillos, y en lo aplanado por encima del suelo.
int structureWaterReport(bool verbose, const std::vector<int>& seeds = {1234, 777, 20260925, 42, 9001}, int radius = 4) {
    int total = 0;
    for (int seed : seeds) {
        const Generator gen(seed, 62, VoxelGenerator::Adventure);
        std::map<std::pair<int, int>, std::shared_ptr<ChunkData>> cache;
        const auto chunkAt = [&](int x, int z) -> const ChunkData& {
            const int cx = static_cast<int>(std::floor(static_cast<float>(x) / kChunkSize)), cz = static_cast<int>(std::floor(static_cast<float>(z) / kChunkSize));
            auto& slot = cache[{cx, cz}];
            if (!slot) {
                slot = std::make_shared<ChunkData>();
                slot->cx = cx;
                slot->cz = cz;
                gen.generate(*slot);
            }
            return *slot;
        };
        const auto blockAt = [&](int x, int y, int z) {
            const ChunkData& c = chunkAt(x, z);
            return c.get(x - c.cx * kChunkSize, y, z - c.cz * kChunkSize);
        };
        int seed_bad = 0, structures_seen = 0;
        for (int rz = -radius; rz <= radius; ++rz)
            for (int rx = -radius; rx <= radius; ++rx) {
                for (const Structure& s : gen.structures()->region(rx, rz)->structures) {
                    const std::string& t = s.info.type;
                    if (t.find("mazmorra") != std::string::npos || t == "ruina" || t == "portal") continue;
                    ++structures_seen;
                    int bad = 0, keep = 0, falling = 0;
                    int ex = 0, ey = 0, ez = 0;
                    for (const Piece& p : s.pieces) {
                        const Blueprint& bp = *p.plan;
                        for (int wz = p.min_z; wz <= p.max_z; ++wz)
                            for (int wx = p.min_x; wx <= p.max_x; ++wx) {
                                const int dx = wx - p.x, dz = wz - p.z;
                                int lx = 0, lz = 0;
                                switch (p.rot & 3) {
                                    case 0: lx = dx; lz = dz; break;
                                    case 1: lz = bp.d - 1 - dx; lx = dz; break;
                                    case 2: lx = bp.w - 1 - dx; lz = bp.d - 1 - dz; break;
                                    default: lz = dx; lx = bp.w - 1 - dz; break;
                                }
                                for (int ly = 0; ly < bp.h; ++ly) {
                                    const int y = p.y + ly;
                                    if (y < 1 || y >= kWorldHeight) continue;
                                    const BlockId b = blockAt(wx, y, wz);
                                    if (!block::isWater(b) && b != block::FallingWater) continue;
                                    const BlockId cell = bp.cells[bp.index(lx, ly, lz)];
                                    if (cell == kKeep && y <= gen.seaLevel()) continue;  // el mar fuera del plano (bajo los muelles)
                                    ++bad;
                                    keep += cell == kKeep ? 1 : 0;
                                    falling += b == block::FallingWater ? 1 : 0;
                                    ex = wx, ey = y, ez = wz;
                                    if (std::getenv("CRAMION_STRUCT_WATER_CELLS"))
                                        std::printf("      %d %d %d id %d pieza %dx%dx%d en (%d,%d,%d) ly %d abajo %d natural %d\n", wx, y, wz, b, bp.w, bp.h,
                                                    bp.d, p.x, p.y, p.z, ly, ly > 0 ? bp.cells[bp.index(lx, ly - 1, lz)] : -1,
                                                    gen.adventureColumn(wx, wz).height);
                                }
                            }
                    }
                    if (bad > 0) {
                        ++seed_bad;
                        total += bad;
                        if (verbose)
                            std::printf("    semilla %d region (%d,%d) %s '%s' base %.0f: %d celdas de agua en piezas (%d en kKeep, %d cascada), p.ej. %d %d %d\n",
                                        seed, rx, rz, t.c_str(), s.info.name.c_str(), static_cast<double>(s.info.center.y), bad, keep, falling, ex, ey, ez);
                    }
                }
            }
        if (verbose) std::printf("  semilla %d: %d estructuras, %d con agua\n", seed, structures_seen, seed_bad);
    }
    return total;
}

// --- Volcado para ver el aspecto (CRAMION_STRUCT_DUMP=<carpeta>) -------------------------
// Escribe los bloques de una caja (blocks_<nombre>.bin: cabecera de 6 int32
// x0 y0 z0 nx ny nz y luego nx*ny*nz bytes en orden x, z, y) y la paleta
// (palette.txt: id nombre forma opaco r g b de lado y de arriba), para
// renderizarlos con Blender (tools/voxel_render.py fuera del motor).
void dumpBox(const Generator& gen, const std::string& folder, const std::string& name, int x0, int z0, int x1, int z1, int y0, int y1) {
    const int nx = x1 - x0 + 1, ny = y1 - y0 + 1, nz = z1 - z0 + 1;
    std::vector<BlockId> data(static_cast<std::size_t>(nx) * ny * nz, block::Air);
    const int cx0 = static_cast<int>(std::floor(static_cast<float>(x0) / kChunkSize)), cx1 = static_cast<int>(std::floor(static_cast<float>(x1) / kChunkSize));
    const int cz0 = static_cast<int>(std::floor(static_cast<float>(z0) / kChunkSize)), cz1 = static_cast<int>(std::floor(static_cast<float>(z1) / kChunkSize));
    for (int cz = cz0; cz <= cz1; ++cz)
        for (int cx = cx0; cx <= cx1; ++cx) {
            ChunkData c;
            c.cx = cx;
            c.cz = cz;
            gen.generate(c);
            for (int lz = 0; lz < kChunkSize; ++lz)
                for (int lx = 0; lx < kChunkSize; ++lx) {
                    const int x = cx * kChunkSize + lx, z = cz * kChunkSize + lz;
                    if (x < x0 || x > x1 || z < z0 || z > z1) continue;
                    for (int y = y0; y <= y1; ++y) {
                        data[static_cast<std::size_t>((x - x0) + (z - z0) * nx + static_cast<std::size_t>(y - y0) * nx * nz)] = c.get(lx, y, lz);
                    }
                }
        }
    const std::string path = folder + "/blocks_" + name + ".bin";
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        const std::int32_t header[6] = {x0, y0, z0, nx, ny, nz};
        std::fwrite(header, sizeof header, 1, f);
        std::fwrite(data.data(), 1, data.size(), f);
        std::fclose(f);
        std::printf("    volcado %s (%d x %d x %d)\n", path.c_str(), nx, ny, nz);
    }
}

void dumpPalette(const std::string& folder) {
    const std::string path = folder + "/palette.txt";
    FILE* f = std::fopen(path.c_str(), "w");
    if (f == nullptr) return;
    const auto average = [](const std::string& tex, float out[3]) {
        gfx::VoxelTextureLayer layer;
        buildTexture(tex, 16, {}, layer, false);
        double sum[3] = {0, 0, 0}, w = 0;
        for (std::size_t i = 0; i + 3 < layer.albedo.size(); i += 4) {
            const double a = layer.albedo[i + 3] / 255.0;
            for (int k = 0; k < 3; ++k) sum[k] += layer.albedo[i + static_cast<std::size_t>(k)] / 255.0 * a;
            w += a;
        }
        for (int k = 0; k < 3; ++k) out[k] = w > 0 ? static_cast<float>(sum[k] / w) : 0.5f;
    };
    for (std::size_t id = 0; id < blocks().size(); ++id) {
        const BlockDef& d = blocks()[id];
        float side[3] = {0, 0, 0}, top[3] = {0, 0, 0};
        if (d.shape != BlockShape::Air) {
            average(d.side.empty() ? std::string("stone") : d.side, side);
            average(d.top.empty() ? std::string("stone") : d.top, top);
        }
        const int shape = d.shape == BlockShape::Air ? 0 : (d.shape == BlockShape::Cube ? 1 : (d.shape == BlockShape::Cross ? 2 : 3));
        std::fprintf(f, "%d %s %d %d %d %d %.3f %.3f %.3f %.3f %.3f %.3f\n", static_cast<int>(id), d.name.c_str(), shape,
                     d.opaque ? 1 : 0, static_cast<int>(d.light), static_cast<int>(d.tint), side[0], side[1], side[2], top[0], top[1],
                     top[2]);
    }
    std::fclose(f);
}

void dumpStructuresForRender() {
    const char* folder = std::getenv("CRAMION_STRUCT_DUMP");
    if (folder == nullptr || *folder == 0) return;
    std::printf("Volcado de estructuras para renderizar en %s\n", folder);
    dumpPalette(folder);
    const Generator gen(1234, 62, VoxelGenerator::Adventure);
    // La capital entera.
    for (const StructureInfo& s : gen.structures()->nearby(0.0f, 70.0f, 0.0f, 10.0f)) {
        if (s.name != "Kettlecrown") continue;
        const int y0 = static_cast<int>(s.center.y) - 14;
        dumpBox(gen, folder, "capital", static_cast<int>(s.min.x) - 24, static_cast<int>(s.min.z) - 24, static_cast<int>(s.max.x) + 24,
                static_cast<int>(s.max.z) + 24, y0, y0 + 70);
    }
    StructureInfo v;
    if (findStructure(gen, "aldea", v)) {
        const int y0 = static_cast<int>(v.center.y) - 12;
        dumpBox(gen, folder, "village", static_cast<int>(v.min.x) - 24, static_cast<int>(v.min.z) - 24, static_cast<int>(v.max.x) + 24,
                static_cast<int>(v.max.z) + 24, y0, y0 + 50);
    }
    if (findStructure(gen, "mazmorra", v)) {
        dumpBox(gen, folder, "dungeon", static_cast<int>(v.min.x) - 4, static_cast<int>(v.min.z) - 4, static_cast<int>(v.max.x) + 4,
                static_cast<int>(v.max.z) + 4, static_cast<int>(v.min.y) - 2, static_cast<int>(v.max.y) + 10);
    }
    if (findStructure(gen, "torre", v, 12)) {
        const int y0 = static_cast<int>(v.center.y) - 8;
        dumpBox(gen, folder, "tower", static_cast<int>(v.min.x) - 20, static_cast<int>(v.min.z) - 20, static_cast<int>(v.max.x) + 20,
                static_cast<int>(v.max.z) + 20, y0, y0 + 64);
    }
    // Asentamientos nuevos: una de cada (castillo real, plazas hostiles).
    for (const char* type : {"fuerte", "campamento", "castillo_maldito", "ciudad_maldita", "torre_culto"}) {
        if (!findStructure(gen, type, v, 12)) continue;
        const int y0 = static_cast<int>(v.center.y) - 6;
        dumpBox(gen, folder, type, static_cast<int>(v.min.x) - 8, static_cast<int>(v.min.z) - 8, static_cast<int>(v.max.x) + 8,
                static_cast<int>(v.max.z) + 8, y0, y0 + 50);
    }
    {
        bool castle = false, elf = false, swamp = false;
        for (int r = 1; r <= 12 && !(castle && elf && swamp); ++r)
            for (int rz = -r; rz <= r; ++rz)
                for (int rx = -r; rx <= r; ++rx) {
                    if (std::max(std::abs(rx), std::abs(rz)) != r) continue;
                    for (const Structure& s : gen.structures()->region(rx, rz)->structures) {
                        bool is_castle = false, is_elf = false, is_swamp = false;
                        for (const Piece& p : s.pieces) {
                            is_castle = is_castle || (s.info.type == "ciudad" && p.plan->w == 51);
                            is_elf = is_elf || (p.plan->w == 15 && p.plan->d == 15 && p.plan->h == 22);
                            is_swamp = is_swamp || (s.info.type == "aldea" && p.plan->w == 7 && p.plan->d == 7 && p.plan->h == 3);
                        }
                        const char* name = nullptr;
                        if (is_castle && !castle) castle = true, name = "castle";
                        else if (is_elf && !elf) elf = true, name = "village_elven";
                        else if (is_swamp && !swamp) swamp = true, name = "village_swamp";
                        if (name == nullptr) continue;
                        const int y0 = static_cast<int>(s.info.center.y) - 10;
                        dumpBox(gen, folder, name, static_cast<int>(s.info.min.x) - 12, static_cast<int>(s.info.min.z) - 12,
                                static_cast<int>(s.info.max.x) + 12, static_cast<int>(s.info.max.z) + 12, y0, y0 + 60);
                    }
                }
    }
    // Otras aldeas de otros estilos (nieve, desierto, playa) si las hay cerca.
    bool done[8] = {};
    for (int r = 1; r <= 16; ++r)
        for (int rz = -r; rz <= r; ++rz)
            for (int rx = -r; rx <= r; ++rx) {
                if (std::max(std::abs(rx), std::abs(rz)) != r) continue;
                for (const Structure& s : gen.structures()->region(rx, rz)->structures) {
                    if (s.info.type != "aldea") continue;
                    const int b = gen.adventureColumn(static_cast<int>(s.info.center.x), static_cast<int>(s.info.center.z)).biome;
                    if ((b != Generator::ASnow && b != Generator::ADesert && b != Generator::ABeach) || done[b]) continue;
                    done[b] = true;
                    const char* style = b == Generator::ASnow ? "snow" : (b == Generator::ADesert ? "desert" : "beach");
                    const int y0 = static_cast<int>(s.info.center.y) - 12;
                    dumpBox(gen, folder, std::string("village_") + style, static_cast<int>(s.info.min.x) - 20, static_cast<int>(s.info.min.z) - 20,
                            static_cast<int>(s.info.max.x) + 20, static_cast<int>(s.info.max.z) + 20, y0, y0 + 50);
                }
            }
}
