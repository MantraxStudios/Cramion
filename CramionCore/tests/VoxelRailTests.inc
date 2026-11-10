// Rieles y senales de arcanita (bloques 211..230, src/voxel/Rails.h). Lo incluye
// VoxelTests.cpp despues de testDoors (usa check, makeWorld y finePosition).
// Solo estas: CRAMION_RAILS_ONLY=1.

// Mundo de prueba en un mapa (lo que no esta es aire; y < 0 piedra).
struct MapWorld {
    std::map<std::tuple<int, int, int>, BlockId> cells;
    BlockId get(int x, int y, int z) const {
        if (y < 0) return block::Stone;
        const auto it = cells.find({x, y, z});
        return it == cells.end() ? block::Air : it->second;
    }
    void put(int x, int y, int z, BlockId id) {
        if (id == block::Air) cells.erase({x, y, z});
        else cells[{x, y, z}] = id;
    }
    rail::SignalSim::World fns() {
        rail::SignalSim::World w;
        w.get = [this](int x, int y, int z) { return get(x, y, z); };
        w.set = [this](int x, int y, int z, BlockId id) { put(x, y, z, id); };
        return w;
    }
};

void testRails() {
    std::printf("Rieles y senales\n");
    check(block::Count >= 231 && blocks().size() == block::Count && block::Rail == 211 && block::EchoRelayLit == 230,
          "ids 211..230 y Count >= 231 (231.. Reino Profundo)");
    bool names = true;
    for (BlockId id = block::Rail; id <= block::EchoRelayLit; ++id) {
        const BlockDef& d = blockDef(id);
        names = names && !d.name.empty() && d.name.rfind("unused_", 0) != 0 && blockId(d.name) == id;
    }
    check(names, "cada bloque nuevo con nombre propio y se encuentra por nombre");
    check(blockId("rail") == block::Rail && blockId("arcanite_trace") == block::ArcaniteTrace && blockId("Signal Lamp") == block::SignalLamp,
          "busqueda por clave y por etiqueta");
    check(!blockDef(block::Rail).solid && !blockDef(block::ArcaniteTrace).solid && blockDef(block::SignalLamp).solid &&
              blockDef(block::SignalLampLit).light == 15 && blockDef(block::ArcaniteTraceLit).light > 0 &&
              !blockDef(block::ArcaniteTraceLit).placeable && blockDef(block::ArcaniteTraceLit).drop == block::ArcaniteTrace,
          "vias y polvo no chocan; la lampara encendida da luz 15; las versiones encendidas no se colocan");
    bool textures = true;
    for (const char* t : {"rail_tie", "rail_iron", "rail_surge", "rail_drag", "rail_switch", "rail_sensor", "buffer_stop",
                          "arcanite_ore", "arcanite_trace", "arcanite_trace_lit", "signal_lamp_lit", "null_rune_crystal_lit",
                          "echo_relay_top", "rune_lever_handle_on"}) {
        const auto& names_list = textureNames();
        textures = textures && std::find(names_list.begin(), names_list.end(), t) != names_list.end();
    }
    gfx::VoxelTextureLayer lit;
    buildTexture("arcanite_trace_lit", 32, {}, lit);
    int glow = 0;
    for (std::size_t i = 2; i < lit.material.size(); i += 4) glow += lit.material[i] > 100 ? 1 : 0;
    check(textures && glow > 100, "texturas de las vias y la arcanita (el polvo encendido brilla)");

    // --- Forma de las vias (sin datos por celda: de las vecinas) ---
    MapWorld m;
    const auto at = [&](int x, int y, int z) { return m.get(x, y, z); };
    m.put(0, 0, 0, block::Rail);
    check(rail::railShape(at, 0, 0, 0).mask == 3 && rail::railShape(at, 0, 0, 0).up == -1, "via suelta: recta en X");
    m.put(0, 0, 1, block::Rail);
    check(rail::railShape(at, 0, 0, 0).mask == (4 | 8), "con una vecina en +Z: recta en Z");
    m.put(1, 0, 0, block::Rail);
    check(rail::railShape(at, 0, 0, 0).mask == (1 | 4), "con vecinas en +X y +Z: curva");
    m.put(-1, 0, 0, block::Rail);
    check(rail::railShape(at, 0, 0, 0).mask == (1 | 2 | 4), "tres vecinas: cruce en T");
    m.put(0, 0, -1, block::Rail);
    check(rail::railShape(at, 0, 0, 0).mask == 15, "cuatro: cruce en cruz");
    // T: recto por la recta; desde el ramal, giro horario; la via de cambio con senal desvia.
    const int t_mask = 1 | 2 | 4;  // recta en X, ramal +Z
    check(rail::railExit(block::Rail, t_mask, 1, false) == 0 && rail::railExit(block::Rail, t_mask, 0, false) == 1,
          "T: por la recta sigue recto");
    check(rail::railExit(block::SwitchRail, t_mask, 1, true) == 2, "via de cambio con senal: de la recta al ramal");
    const int from_stem = rail::railExit(block::Rail, t_mask, 2, false), from_stem_sw = rail::railExit(block::SwitchRail, t_mask, 2, true);
    check((from_stem == 0 || from_stem == 1) && (from_stem_sw == 0 || from_stem_sw == 1) && from_stem != from_stem_sw,
          "desde el ramal gira a un lado y con senal al otro");
    check(rail::railExit(block::Rail, 1 | 4, 0, false) == 2 && rail::railExit(block::Rail, 3, 1, false) == 0, "curva y recta");
    // Rampa: una via un bloque mas arriba al lado.
    MapWorld r;
    const auto rat = [&](int x, int y, int z) { return r.get(x, y, z); };
    r.put(0, 5, 0, block::Rail);
    r.put(1, 5, 0, block::Rail);
    r.put(2, 6, 0, block::Rail);
    r.put(3, 6, 0, block::Rail);
    const rail::RailShape slope = rail::railShape(rat, 1, 5, 0);
    check(slope.up == 0 && slope.mask == 3 && rail::railShape(rat, 0, 5, 0).up == -1 && rail::railShape(rat, 2, 6, 0).up == -1 &&
              rail::railShape(rat, 2, 6, 0).mask == 3,
          "rampa: sube hacia la via de arriba; las de los extremos llanas");
    r.put(4, 7, 0, block::Rail);
    check(rail::railShape(rat, 3, 6, 0).up == 0 && rail::railShape(rat, 2, 6, 0).up == -1, "rampas seguidas");
    r.put(9, 5, 0, block::BufferStop);
    r.put(8, 5, 0, block::Rail);
    check(rail::railShape(rat, 9, 5, 0).mask == 2, "tope: solo hacia su via");

    // --- Dibujo ---
    {
        SectionSnapshot s;
        s.light.fill(0xF0);
        s.grass_tint.fill(0xFFF);
        s.foliage_tint.fill(0xFFF);
        const auto put = [&](int x, int y, int z, BlockId id) { s.blocks[static_cast<std::size_t>(SectionSnapshot::index(x, y, z))] = id; };
        std::vector<std::uint32_t> v;
        const auto bounds = [&](Vec3& lo, Vec3& hi) {
            lo = Vec3{99, 99, 99};
            hi = Vec3{-99, -99, -99};
            for (std::size_t i = 0; i + 1 < v.size(); i += 2) {
                if (((v[i + 1] >> 30u) & 1u) == 0u) continue;  // solo las piezas finas (no la piedra)
                const Vec3 p = finePosition(v[i], v[i + 1]);
                lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
        };
        for (int y = 0; y < 4; ++y)
            for (int z = 0; z < 16; ++z)
                for (int x = 0; x < 16; ++x) put(x, y, z, block::Stone);
        put(5, 4, 5, block::Rail);
        meshSection(s, v);
        Vec3 lo, hi;
        bounds(lo, hi);
        check(!v.empty() && lo.y == 4.0f && hi.y <= 4.0f + 2.0f / 16.0f + 0.001f && lo.x == 5.0f && hi.x == 6.0f,
              "via llana: piezas finas de 2/16 de alto a lo largo de la celda");
        put(6, 5, 5, block::Rail);  // rampa hacia +X
        put(5, 4, 5, block::Rail);
        s.blocks[static_cast<std::size_t>(SectionSnapshot::index(6, 4, 5))] = block::Stone;
        meshSection(s, v);
        float top = 0;
        for (std::size_t i = 0; i + 1 < v.size(); i += 2) {
            const Vec3 p = finePosition(v[i], v[i + 1]);
            if (p.x >= 5.0f && p.x <= 6.0f && p.y < 5.5f) top = std::max(top, p.y);
        }
        check(top >= 4.9f, "la rampa sube hasta arriba de la celda");
        // Polvo de arcanita: fino (1/16) y sube por la pared hacia la traza de arriba.
        s.blocks.fill(block::Air);
        for (int z = 0; z < 16; ++z)
            for (int x = 0; x < 16; ++x) put(x, 3, z, block::Stone);
        put(5, 4, 5, block::ArcaniteTrace);
        put(6, 4, 5, block::Stone);
        put(6, 5, 5, block::ArcaniteTrace);
        meshSection(s, v);
        float climb = 0;
        for (std::size_t i = 0; i + 1 < v.size(); i += 2) {
            const Vec3 p = finePosition(v[i], v[i + 1]);
            if (p.x >= 5.9f && p.x <= 6.0f && p.y <= 5.0f) climb = std::max(climb, p.y);
        }
        check(climb >= 4.99f, "el polvo sube por la pared hasta la traza de arriba");
    }

    // --- Senales (mundo de prueba) ---
    {
        MapWorld w;
        rail::SignalSim sim;
        auto fns = w.fns();
        const auto place = [&](int x, int y, int z, BlockId id) {
            const BlockId before = w.get(x, y, z);
            w.put(x, y, z, id);
            sim.blockChanged(fns, x, y, z, before, id);
        };
        // Palanca - 10 de polvo - lampara.
        place(0, 0, 0, block::RuneLever);
        for (int x = 1; x <= 10; ++x) place(x, 0, 0, block::ArcaniteTrace);
        place(11, 0, 0, block::SignalLamp);
        sim.step(fns, 2);
        check(w.get(11, 0, 0) == block::SignalLamp && w.get(5, 0, 0) == block::ArcaniteTrace, "palanca apagada: nada");
        place(0, 0, 0, block::RuneLeverOn);
        sim.step(fns, 1);
        check(w.get(11, 0, 0) == block::SignalLampLit && w.get(5, 0, 0) == block::ArcaniteTraceLit && sim.power(1, 0, 0) == 15 &&
                  sim.power(10, 0, 0) == 6,
              "palanca encendida: el polvo brilla (15 junto a la palanca, -1 por bloque) y la lampara se enciende");
        place(0, 0, 0, block::RuneLever);
        sim.step(fns, 1);
        check(w.get(11, 0, 0) == block::SignalLamp && w.get(5, 0, 0) == block::ArcaniteTrace && sim.power(5, 0, 0) == 0,
              "y se apaga");
        // Mas de 15 bloques: no llega.
        MapWorld w2;
        rail::SignalSim sim2;
        auto f2 = w2.fns();
        w2.put(0, 0, 0, block::RuneLeverOn);
        for (int x = 1; x <= 16; ++x) w2.put(x, 0, 0, block::ArcaniteTrace);
        w2.put(17, 0, 0, block::SignalLamp);
        sim2.blockChanged(f2, 0, 0, 0, block::Air, block::RuneLeverOn);
        sim2.step(f2, 1);
        check(w2.get(15, 0, 0) == block::ArcaniteTraceLit && w2.get(16, 0, 0) == block::ArcaniteTrace && w2.get(17, 0, 0) == block::SignalLamp,
              "la senal se acaba a los 15 bloques");
        // Runa nula (inversor): palanca - polvo - runa - polvo - lampara.
        MapWorld w3;
        rail::SignalSim sim3;
        auto f3 = w3.fns();
        const auto put3 = [&](int x, int y, int z, BlockId id) {
            const BlockId before = w3.get(x, y, z);
            w3.put(x, y, z, id);
            sim3.blockChanged(f3, x, y, z, before, id);
        };
        put3(0, 0, 0, block::RuneLever);
        put3(1, 0, 0, block::ArcaniteTrace);
        put3(2, 0, 0, block::NullRune);
        put3(3, 0, 0, block::ArcaniteTrace);
        put3(4, 0, 0, block::SignalLamp);
        sim3.step(f3, 3);
        check(w3.get(4, 0, 0) == block::SignalLampLit && w3.get(1, 0, 0) == block::ArcaniteTrace, "runa nula sin entrada: enciende la salida (y no la entrada)");
        put3(0, 0, 0, block::RuneLeverOn);
        sim3.step(f3, 3);
        check(w3.get(4, 0, 0) == block::SignalLamp && w3.get(2, 0, 0) == block::NullRuneDim && w3.get(1, 0, 0) == block::ArcaniteTraceLit,
              "con la palanca: la runa se apaga y la lampara tambien");
        // Repetidor de eco: retardo.
        MapWorld w4;
        rail::SignalSim sim4;
        auto f4 = w4.fns();
        const auto put4 = [&](int x, int y, int z, BlockId id) {
            const BlockId before = w4.get(x, y, z);
            w4.put(x, y, z, id);
            sim4.blockChanged(f4, x, y, z, before, id);
        };
        put4(0, 0, 0, block::RuneLever);
        put4(1, 0, 0, block::ArcaniteTrace);
        put4(2, 0, 0, block::EchoRelay);
        put4(3, 0, 0, block::ArcaniteTrace);
        put4(4, 0, 0, block::SignalLamp);
        sim4.step(f4, 2);
        put4(0, 0, 0, block::RuneLeverOn);
        sim4.step(f4, 1);
        const bool late = w4.get(4, 0, 0) == block::SignalLamp;
        sim4.step(f4, 4);
        check(late && w4.get(4, 0, 0) == block::SignalLampLit && w4.get(2, 0, 0) == block::EchoRelayLit,
              "repetidor de eco: la senal llega con retardo");
        // Placa de presion: entrada que caduca.
        MapWorld w5;
        rail::SignalSim sim5;
        auto f5 = w5.fns();
        w5.put(0, 0, 0, block::TriggerPlate);
        w5.put(1, 0, 0, block::SignalLamp);
        sim5.blockChanged(f5, 1, 0, 0, block::Air, block::SignalLamp);
        sim5.setInput(0, 0, 0, true);
        sim5.step(f5, 1);
        const bool pressed = w5.get(1, 0, 0) == block::SignalLampLit;
        sim5.step(f5, 6);  // nadie la refresca: caduca
        check(pressed && w5.get(1, 0, 0) == block::SignalLamp, "placa de presion: enciende mientras alguien la pisa (caduca sola)");
        // Vias de impulso seguidas: una palanca al lado de la primera alimenta hasta 8.
        MapWorld w6;
        rail::SignalSim sim6;
        auto f6 = w6.fns();
        for (int x = 0; x < 12; ++x) w6.put(x, 0, 0, block::SurgeRail);
        w6.put(0, 0, 1, block::RuneLeverOn);
        sim6.blockChanged(f6, 0, 0, 1, block::Air, block::RuneLeverOn);
        sim6.step(f6, 1);
        check(sim6.power(0, 0, 0) > 0 && sim6.power(8, 0, 0) > 0 && sim6.power(10, 0, 0) == 0,
              "vias de impulso: la senal sigue por las seguidas (hasta 8)");
        // Determinista: otro simulador con los mismos bloques y entradas da lo mismo.
        MapWorld w7 = w;
        rail::SignalSim sim7;
        auto f7 = w7.fns();
        w7.put(0, 0, 0, block::RuneLeverOn);
        sim7.blockChanged(f7, 0, 0, 0, block::RuneLever, block::RuneLeverOn);
        sim7.step(f7, 1);
        place(0, 0, 0, block::RuneLeverOn);
        sim.step(fns, 1);
        check(w7.cells == w.cells, "determinista: mismo mundo y mismas entradas, mismo resultado");
    }

    // --- En el VoxelSystem (eventos de setBlock, 10 Hz, API de vias) ---
    {
        ecs::World world;
        makeWorld(world, 777, 2);
        VoxelSystem vox;
        vox.start(world);
        vox.waitUntilReady(Vec3{0.5f, 120.0f, 0.5f}, 1, 60.0f);
        const int h = vox.surfaceHeight(0, 0) + 1;
        for (int x = -1; x <= 6; ++x)
            for (int z = -1; z <= 1; ++z) {
                vox.setBlock(x, h - 1, z, block::Stone);
                for (int y = h; y <= h + 2; ++y) vox.setBlock(x, y, z, block::Air);
            }
        vox.setBlock(0, h, 0, block::RuneLever);
        for (int x = 1; x <= 4; ++x) vox.setBlock(x, h, 0, block::ArcaniteTrace);
        vox.setBlock(5, h, 0, block::SignalLamp);
        for (int i = 0; i < 4; ++i) vox.update(0.1f, Vec3{0.5f, static_cast<float>(h), 0.5f});
        const bool off = vox.getBlock(5, h, 0) == block::SignalLamp;
        vox.setBlock(0, h, 0, block::RuneLeverOn);
        for (int i = 0; i < 4; ++i) vox.update(0.1f, Vec3{0.5f, static_cast<float>(h), 0.5f});
        check(off && vox.getBlock(5, h, 0) == block::SignalLampLit && vox.signalPower(5, h, 0) > 0 && vox.blockLight(5, h + 1, 0) > 10,
              "VoxelSystem: la palanca enciende la lampara (y da luz) en el siguiente tick");
        vox.setBlock(0, h, 1, block::Rail);
        vox.setBlock(1, h, 1, block::Rail);
        int mask = 0, up = -1;
        check(vox.railAt(0, h, 1, mask, up) && mask == 3 && up == -1 && !vox.railAt(0, h + 1, 1, mask, up), "railAt: la forma de la via");
        const auto found = vox.findBlocks(Vec3{0.5f, static_cast<float>(h), 0.5f}, 4, block::Rail, 10);
        check(found.size() == 2, "findBlocks encuentra las vias");
        vox.stop();
    }
}
