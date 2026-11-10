// MALLAS-DETALLE: niveles de adornos del mallador (SectionSnapshot::mesh_tier, CVar voxel.mesh.Detail).
// Incluido desde VoxelTests.cpp (CRAMION_MESHDETAIL_ONLY=1 solo estas).
//   - meshTierFor: los cortes por distancia de Bajo / Medio / Alto;
//   - terreno real del generador Aventura (varias zonas de 6 x 6 chunks, luz del cielo por columna): cuadrados por
//     nivel (total, plantas y matas, piezas finas: hojas esponjosas, pepitas...);
//   - sin agujeros: las caras macizas (las de los bloques, caras 0-5 sin vertice fino) cubren exactamente las mismas
//     celdas en los tres niveles, asi una seccion de un nivel junto a otra de otro no deja huecos en el borde;
//   - flags: una seccion de solo piedra no tiene adornos (no se vuelve a mallar al cambiar 1 <-> 2).

namespace mesh_detail_tests {

struct Counts {
    std::size_t total = 0, plants = 0, fine = 0, solid = 0;
};

// Celdas cubiertas por las caras macizas: (cara, x, y, z) de cada cuadrado unitario.
using Coverage = std::vector<std::uint64_t>;

void countQuads(const std::vector<std::uint32_t>& v, Counts& c, Coverage* coverage) {
    for (std::size_t q = 0; q + 7 < v.size(); q += 8) {
        ++c.total;
        const std::uint32_t a = v[q], b = v[q + 1];
        const int face = static_cast<int>((a >> 15) & 7u);
        if (face >= 6) {
            ++c.plants;
            continue;
        }
        if ((b & (1u << 30)) != 0u) {
            ++c.fine;
            continue;
        }
        ++c.solid;
        if (coverage == nullptr) continue;
        int lo[3] = {99, 99, 99}, hi[3] = {-1, -1, -1};
        for (int k = 0; k < 4; ++k) {
            const std::uint32_t ak = v[q + static_cast<std::size_t>(k) * 2];
            const int p[3] = {static_cast<int>(ak & 31u), static_cast<int>((ak >> 5) & 31u), static_cast<int>((ak >> 10) & 31u)};
            for (int i = 0; i < 3; ++i) {
                lo[i] = std::min(lo[i], p[i]);
                hi[i] = std::max(hi[i], p[i]);
            }
        }
        const int d = face / 2;
        for (int y = lo[1]; y < std::max(hi[1], lo[1] + (d == 1 ? 1 : 0)); ++y)
            for (int z = lo[2]; z < std::max(hi[2], lo[2] + (d == 2 ? 1 : 0)); ++z)
                for (int x = lo[0]; x < std::max(hi[0], lo[0] + (d == 0 ? 1 : 0)); ++x)
                    coverage->push_back((static_cast<std::uint64_t>(face) << 40) | (static_cast<std::uint64_t>(x) << 20) |
                                        (static_cast<std::uint64_t>(y) << 10) | static_cast<std::uint64_t>(z));
    }
}

// Region de n x n chunks (con un borde de 1 generado para los vecinos) a partir del chunk (cx0, cz0).
struct Region {
    int cx0 = 0, cz0 = 0, n = 0;
    std::map<std::pair<int, int>, std::unique_ptr<ChunkData>> chunks;
    std::map<std::pair<int, int>, std::array<int, kChunkSize * kChunkSize>> tops;  // primer opaco desde arriba

    const ChunkData* chunk(int cx, int cz) const {
        const auto it = chunks.find({cx, cz});
        return it == chunks.end() ? nullptr : it->second.get();
    }
    void build(const Generator& gen) {
        for (int cz = cz0 - 1; cz <= cz0 + n; ++cz) {
            for (int cx = cx0 - 1; cx <= cx0 + n; ++cx) {
                auto c = std::make_unique<ChunkData>();
                c->cx = cx;
                c->cz = cz;
                gen.generate(*c);
                std::array<int, kChunkSize * kChunkSize> top{};
                for (int z = 0; z < kChunkSize; ++z) {
                    for (int x = 0; x < kChunkSize; ++x) {
                        int y = kWorldHeight - 1;
                        while (y >= 0 && !blockDef(c->get(x, y, z)).opaque) --y;
                        top[static_cast<std::size_t>(x + z * kChunkSize)] = y;
                    }
                }
                tops[{cx, cz}] = top;
                chunks[{cx, cz}] = std::move(c);
            }
        }
    }
    // Copia de la seccion con borde (como VoxelSystem::snapshot): luz del cielo 15 por encima del primer bloque
    // opaco de la columna, 0 debajo (cuevas a oscuras).
    void snapshot(int cx, int sy, int cz, SectionSnapshot& s, bool& empty) const {
        s.cx = cx;
        s.sy = sy;
        s.cz = cz;
        int solid = 0;
        for (int y = -1; y <= kChunkSize; ++y) {
            const int wy = sy * kChunkSize + y;
            for (int z = -1; z <= kChunkSize; ++z) {
                for (int x = -1; x <= kChunkSize; ++x) {
                    const int gx = cx + (x < 0 ? -1 : x >= kChunkSize ? 1 : 0), gz = cz + (z < 0 ? -1 : z >= kChunkSize ? 1 : 0);
                    const int lx = (x + kChunkSize) % kChunkSize, lz = (z + kChunkSize) % kChunkSize;
                    const ChunkData* c = chunk(gx, gz);
                    BlockId id = block::Air;
                    std::uint8_t light = 0xF0;
                    if (wy < 0) {
                        id = block::Bedrock;
                        light = 0;
                    } else if (wy < kWorldHeight && c != nullptr) {
                        id = c->get(lx, wy, lz);
                        const int top = tops.at({gx, gz})[static_cast<std::size_t>(lx + lz * kChunkSize)];
                        light = wy > top ? 0xF0 : 0x00;
                    }
                    const std::size_t si = static_cast<std::size_t>(SectionSnapshot::index(x, y, z));
                    s.blocks[si] = id;
                    s.light[si] = light;
                    if (x >= 0 && y >= 0 && z >= 0 && x < kChunkSize && y < kChunkSize && z < kChunkSize && id != block::Air) ++solid;
                }
            }
        }
        for (int z = -1; z <= kChunkSize; ++z) {
            for (int x = -1; x <= kChunkSize; ++x) {
                const int gx = cx + (x < 0 ? -1 : x >= kChunkSize ? 1 : 0), gz = cz + (z < 0 ? -1 : z >= kChunkSize ? 1 : 0);
                const int lx = (x + kChunkSize) % kChunkSize, lz = (z + kChunkSize) % kChunkSize;
                const ChunkData* c = chunk(gx, gz);
                const std::size_t si = static_cast<std::size_t>((x + 1) + (z + 1) * SectionSnapshot::kSize);
                const std::size_t column = static_cast<std::size_t>(lx + lz * kChunkSize);
                s.grass_tint[si] = c != nullptr ? c->grass_tint[column] : 0xFFF;
                s.foliage_tint[si] = c != nullptr ? c->foliage_tint[column] : 0xFFF;
            }
        }
        empty = solid == 0;
    }
};

}  // namespace mesh_detail_tests

void testMeshDetail() {
    using namespace mesh_detail_tests;
    std::printf("MALLAS-DETALLE: niveles de adornos del mallador\n");
    check(meshTierFor(2, 0.0f) == 2 && meshTierFor(2, 5000.0f) == 2, "Alto: todo a cualquier distancia");
    check(meshTierFor(1, 47.0f) == 2 && meshTierFor(1, 50.0f) == 1 && meshTierFor(1, 95.0f) == 1 && meshTierFor(1, 97.0f) == 0,
          "Medio: 2 hasta 48 m, 1 hasta 96 m, 0 despues");
    check(meshTierFor(0, 15.0f) == 2 && meshTierFor(0, 17.0f) == 1 && meshTierFor(0, 39.0f) == 1 && meshTierFor(0, 41.0f) == 0,
          "Bajo: 2 hasta 16 m, 1 hasta 40 m, 0 despues");

    // Flags: piedra sola (sin adornos) y una mata de hierba.
    {
        SectionSnapshot s;
        s.light.fill(0xF0);
        s.grass_tint.fill(0xFFF);
        s.foliage_tint.fill(0xFFF);
        s.blocks[static_cast<std::size_t>(SectionSnapshot::index(5, 5, 5))] = block::Stone;
        std::vector<std::uint32_t> v;
        std::uint8_t flags = 0xFF;
        meshSection(s, v, nullptr, &flags);
        check(flags == kTierFaces, "piedra sola: solo caras (sin adornos)");
        s.blocks[static_cast<std::size_t>(SectionSnapshot::index(5, 6, 5))] = block::TallGrass;
        meshSection(s, v, nullptr, &flags);
        check((flags & kTierDecor) != 0, "con una mata: adornos");
        Counts c[3];
        for (int t = 0; t < 3; ++t) {
            s.mesh_tier = static_cast<std::uint8_t>(t);
            meshSection(s, v);
            countQuads(v, c[t], nullptr);
        }
        check(c[2].plants == 6 && c[1].plants == 4 && c[0].plants == 2, "mata: 3 / 2 / 1 laminas por sus dos caras");
    }

    const Generator gen(1234, 62, VoxelGenerator::Adventure);
    const int n = std::getenv("CRAMION_MESHDETAIL_N") != nullptr ? std::max(2, std::atoi(std::getenv("CRAMION_MESHDETAIL_N"))) : 6;
    // Zonas repartidas (llanura, bosque, montana, costa... segun caiga la semilla 1234).
    const int zones[][2] = {{0, 0}, {120, -90}, {-190, 50}, {30, 220}, {-60, -160}};
    Counts sum[3];
    bool same_cover = true;
    double ms[3] = {0, 0, 0};
    int sections = 0;
    for (const auto& zone : zones) {
        Region r;
        r.cx0 = zone[0];
        r.cz0 = zone[1];
        r.n = n;
        r.build(gen);
        Counts zc[3];
        for (int cz = r.cz0; cz < r.cz0 + n; ++cz) {
            for (int cx = r.cx0; cx < r.cx0 + n; ++cx) {
                for (int sy = 0; sy < kSectionCount; ++sy) {
                    SectionSnapshot s;
                    bool empty = false;
                    r.snapshot(cx, sy, cz, s, empty);
                    if (empty) continue;
                    ++sections;
                    Coverage cover[3];
                    for (int t = 0; t < 3; ++t) {
                        s.mesh_tier = static_cast<std::uint8_t>(t);
                        std::vector<std::uint32_t> v, w;
                        const auto t0 = std::chrono::steady_clock::now();
                        meshSection(s, v, &w);
                        ms[t] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                        countQuads(v, zc[t], &cover[t]);
                        std::sort(cover[t].begin(), cover[t].end());
                    }
                    if (cover[0] != cover[2] || cover[1] != cover[2]) same_cover = false;
                }
            }
        }
        std::printf("    zona (%d, %d) %dx%d chunks: cuadrados Alto %zu | Medio-lejos (1) %zu (%.0f%%) | Bajo-lejos (0) %zu (%.0f%%)\n",
                    zone[0], zone[1], n, n, zc[2].total, zc[1].total, 100.0 * static_cast<double>(zc[1].total) / std::max<double>(1.0, static_cast<double>(zc[2].total)),
                    zc[0].total, 100.0 * static_cast<double>(zc[0].total) / std::max<double>(1.0, static_cast<double>(zc[2].total)));
        for (int t = 0; t < 3; ++t) {
            sum[t].total += zc[t].total;
            sum[t].plants += zc[t].plants;
            sum[t].fine += zc[t].fine;
            sum[t].solid += zc[t].solid;
        }
    }
    for (int t = 2; t >= 0; --t) {
        std::printf("    nivel %d: %zu cuadrados (%zu triangulos): macizas %zu, plantas/matas %zu, piezas finas %zu; %.2f ms/seccion\n", t,
                    sum[t].total, sum[t].total * 2, sum[t].solid, sum[t].plants, sum[t].fine, ms[t] / std::max(1, sections));
    }
    std::printf("    %d secciones no vacias\n", sections);
    check(same_cover, "las caras macizas cubren lo mismo en los 3 niveles (sin agujeros entre secciones)");
    check(sum[1].total < sum[2].total && sum[0].total < sum[1].total, "menos cuadrados en cada nivel");
}
