// Sistema del mundo de bloques: une el generador, la luz, el mallador, los
// hilos, el guardado y las consultas (ver Voxel.h).
//
// Hilo principal: los datos de los chunks (bloques y luz) viven aqui; solo
// aqui se leen y se escriben. Los hilos de fondo reciben copias:
//   - Generar: (semilla, cx, cz) -> un ChunkData nuevo (o el guardado).
//   - Mallar: una SectionSnapshot (18^3) -> los vertices.
// Cada frame se recogen los resultados con un presupuesto de tiempo, se
// propaga la luz, se lanzan las secciones sucias y se piden las cercanas.

#include "CramionCore/voxel/Voxel.h"

#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/water/Water.h"
#include "VoxelInternal.h"

#include <CramionFX/vk/VulkanRenderer.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace cramion::voxel {

using core::Vec3;
using Clock = std::chrono::steady_clock;

namespace {

std::int64_t chunkKey(int cx, int cz) {
    return (static_cast<std::int64_t>(cx) << 32) ^ static_cast<std::int64_t>(static_cast<std::uint32_t>(cz));
}
std::uint64_t sectionKey(int cx, int sy, int cz) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx) & 0xFFFFFFu) << 32) |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cz) & 0xFFFFFFu) << 8) | static_cast<std::uint64_t>(sy & 0xFF);
}
int floorDiv(int a, int b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }
int floorMod(int a, int b) { return a - floorDiv(a, b) * b; }
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

std::string safeName(const std::string& name) {
    std::string out;
    for (const char c : name) out += (std::string("<>:\"/\\|?*").find(c) != std::string::npos || static_cast<unsigned char>(c) < 32) ? '_' : c;
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out;
}
std::filesystem::path fromUtf8(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

// Chunk guardado: "CRVX" + version + pares (cuantos u16, bloque u8).
bool writeChunkFile(const std::filesystem::path& file, const ChunkData& chunk) {
    std::vector<std::uint8_t> bytes = {'C', 'R', 'V', 'X', 1};
    const std::vector<BlockId>& b = chunk.blocks;
    std::size_t i = 0;
    while (i < b.size()) {
        std::size_t run = 1;
        while (i + run < b.size() && b[i + run] == b[i] && run < 65535) ++run;
        bytes.push_back(static_cast<std::uint8_t>(run & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>(run >> 8));
        bytes.push_back(b[i]);
        i += run;
    }
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    const std::filesystem::path temp = std::filesystem::path(file).concat(".tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) return false;
    }
    std::filesystem::rename(temp, file, error);  // atomico: nunca queda a medias
    return !error;
}

bool readChunkFile(const std::filesystem::path& file, ChunkData& chunk) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 5 || bytes[0] != 'C' || bytes[1] != 'R' || bytes[2] != 'V' || bytes[3] != 'X') return false;
    std::size_t out = 0;
    for (std::size_t i = 5; i + 2 < bytes.size() + 0 && out < chunk.blocks.size(); i += 3) {
        const std::size_t run = static_cast<std::size_t>(bytes[i]) | (static_cast<std::size_t>(bytes[i + 1]) << 8);
        const BlockId id = bytes[i + 2] < block::Count ? bytes[i + 2] : block::Air;
        for (std::size_t k = 0; k < run && out < chunk.blocks.size(); ++k) chunk.blocks[out++] = id;
    }
    return out == chunk.blocks.size();
}

}  // namespace

// ================================================================================

struct VoxelSystem::Impl {
    // --- Estado ---
    bool active = false;
    VoxelWorld settings;
    std::filesystem::path assets_root;
    std::filesystem::path save_root;
    std::string world_name;
    int seed = 0;
    std::map<std::string, std::string> meta_values;
    std::shared_ptr<const Generator> generator;
    Vec3 viewer{};
    int viewer_cx = 0, viewer_cz = 0;
    bool has_viewer = false;
    BlockListener listener;
    physics::PhysicsSystem* physics = nullptr;

    struct Chunk {
        ChunkData data;
        std::array<bool, kSectionCount> dirty{};
        std::array<std::uint32_t, kSectionCount> version{};   // sube al ensuciarse
        std::array<std::uint32_t, kSectionCount> in_flight{};  // version que se esta mallando (0 = nada)
        std::array<bool, kSectionCount> meshed{};
        std::array<bool, kSectionCount> uploaded{};             // hay algo en el renderizador
        bool ever_meshed = false;
        int top = kWorldHeight - 1;  // el bloque no-aire mas alto (encima: aire con cielo 15)
        std::array<bool, kSectionCount> collision_built{};  // la malla de colision esta al dia
        std::array<bool, kSectionCount> has_collider{};     // hay cuerpo en la fisica
    };
    std::unordered_map<std::int64_t, std::unique_ptr<Chunk>> chunks;
    // La luz y las consultas piden muchas celdas seguidas del mismo chunk.
    mutable std::int64_t cached_key = 0;
    mutable Chunk* cached_chunk = nullptr;
    std::unordered_set<std::int64_t> generating;

    // --- Hilos ---
    struct Job {
        int kind = 0;  // 0 generar, 1 mallar
        int cx = 0, cz = 0, sy = 0;
        std::uint32_t version = 0;
        std::uint64_t epoch = 0;
        std::shared_ptr<const Generator> generator;
        std::filesystem::path saved;       // generar: chunk guardado (si hay)
        std::unique_ptr<SectionSnapshot> snapshot;
        float priority = 0.0f;
    };
    struct Result {
        int kind = 0;
        int cx = 0, cz = 0, sy = 0;
        std::uint32_t version = 0;
        std::uint64_t epoch = 0;
        std::unique_ptr<ChunkData> chunk;
        std::vector<std::uint32_t> vertices;
        float milliseconds = 0.0f;
    };
    std::vector<std::thread> workers;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> jobs;
    std::vector<Result> results;
    bool quit = false;
    std::uint64_t epoch = 1;  // sube al cambiar de mundo: resultados viejos se tiran

    // --- Renderizador ---
    struct SectionUpdate {
        std::uint64_t key;
        Vec3 origin;
        std::vector<std::uint32_t> vertices;  // vacio = quitar
    };
    std::vector<SectionUpdate> section_updates;
    bool clear_renderer = true;
    std::future<std::vector<gfx::VoxelTextureLayer>> textures_future;
    bool textures_uploaded = false;
    std::string textures_signature;
    float time = 0.0f;
    float last_chunk_ms = 0.0f;
    float last_mesh_ms = 0.0f;

    // Agua que entra al cavar junto al mar.
    std::deque<BlockPos> water_flow;

    Impl() {
        const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
        const int count = static_cast<int>(std::clamp(hw - 2u, 2u, 8u));
        for (int i = 0; i < count; ++i) workers.emplace_back([this]() { workerLoop(); });
    }
    ~Impl() {
        {
            std::lock_guard lock(mutex);
            quit = true;
            jobs.clear();
        }
        wake.notify_all();
        for (std::thread& t : workers) t.join();
    }

    void workerLoop() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [this]() { return quit || !jobs.empty(); });
                if (quit) return;
                // La mas prioritaria (mas cerca de quien mira).
                auto best = std::min_element(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.priority < b.priority; });
                job = std::move(*best);
                jobs.erase(best);
            }
            const auto start = Clock::now();
            Result r;
            r.kind = job.kind;
            r.cx = job.cx;
            r.cz = job.cz;
            r.sy = job.sy;
            r.version = job.version;
            r.epoch = job.epoch;
            if (job.kind == 0) {
                r.chunk = std::make_unique<ChunkData>();
                r.chunk->cx = job.cx;
                r.chunk->cz = job.cz;
                job.generator->generate(*r.chunk);  // tambien da los tintes
                if (!job.saved.empty() && readChunkFile(job.saved, *r.chunk)) r.chunk->modified = true;
            } else {
                meshSection(*job.snapshot, r.vertices);
            }
            r.milliseconds = static_cast<float>(msSince(start));
            std::lock_guard lock(mutex);
            results.push_back(std::move(r));
        }
    }

    void submit(Job job) {
        {
            std::lock_guard lock(mutex);
            jobs.push_back(std::move(job));
        }
        wake.notify_one();
    }

    // ------------------------------------------------------------ acceso
    Chunk* chunkAt(int cx, int cz) const {
        const std::int64_t key = chunkKey(cx, cz);
        if (cached_chunk != nullptr && cached_key == key) return cached_chunk;
        const auto it = chunks.find(key);
        if (it == chunks.end()) return nullptr;
        cached_key = key;
        cached_chunk = it->second.get();
        return cached_chunk;
    }
    void forgetCachedChunk() { cached_chunk = nullptr; }
    // Por encima de esto, en el chunk y sus 4 vecinos, todo es aire con cielo 15.
    int topAround(int cx, int cz) const {
        int top = 0;
        const int around[5][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& o : around) {
            if (const Chunk* c = chunkAt(cx + o[0], cz + o[1])) top = std::max(top, c->top);
        }
        return std::min(top + 1, kWorldHeight - 1);
    }
    Chunk* chunkOf(int x, int z) const { return chunkAt(floorDiv(x, kChunkSize), floorDiv(z, kChunkSize)); }

    BlockId getBlock(int x, int y, int z) const {
        if (y < 0) return block::Bedrock;
        if (y >= kWorldHeight) return block::Air;
        const Chunk* c = chunkOf(x, z);
        if (c == nullptr) return block::Air;
        return c->data.blocks[static_cast<std::size_t>(cellIndex(floorMod(x, kChunkSize), y, floorMod(z, kChunkSize)))];
    }
    std::uint8_t getLight(int x, int y, int z) const {
        if (y >= kWorldHeight) return 0xF0;
        if (y < 0) return 0;
        const Chunk* c = chunkOf(x, z);
        if (c == nullptr) return 0xF0;
        return c->data.light[static_cast<std::size_t>(cellIndex(floorMod(x, kChunkSize), y, floorMod(z, kChunkSize)))];
    }

    // Marca la seccion de la celda (y las vecinas si esta en su borde).
    void markDirty(int x, int y, int z) {
        const auto mark = [&](int wx, int wy, int wz) {
            if (wy < 0 || wy >= kWorldHeight) return;
            Chunk* c = chunkOf(wx, wz);
            if (c == nullptr) return;
            const int s = wy / kChunkSize;
            if (!c->dirty[static_cast<std::size_t>(s)]) {
                c->dirty[static_cast<std::size_t>(s)] = true;
                ++c->version[static_cast<std::size_t>(s)];
            }
        };
        mark(x, y, z);
        const int lx = floorMod(x, kChunkSize), ly = y % kChunkSize, lz = floorMod(z, kChunkSize);
        if (lx == 0) mark(x - 1, y, z);
        if (lx == kChunkSize - 1) mark(x + 1, y, z);
        if (lz == 0) mark(x, y, z - 1);
        if (lz == kChunkSize - 1) mark(x, y, z + 1);
        if (ly == 0) mark(x, y - 1, z);
        if (ly == kChunkSize - 1) mark(x, y + 1, z);
        // Esquinas (la oclusion de las esquinas mira en diagonal).
        if ((lx == 0 || lx == kChunkSize - 1) && (lz == 0 || lz == kChunkSize - 1)) {
            mark(x + (lx == 0 ? -1 : 1), y, z + (lz == 0 ? -1 : 1));
        }
    }

    // ------------------------------------------------------------ luz
    struct LightNode {
        int x, y, z;
        std::uint8_t level;
    };

    static int filterOf(BlockId id) { return blockDef(id).light_filter; }

    void setLightChannel(int x, int y, int z, bool sky, int level) {
        Chunk* c = chunkOf(x, z);
        if (c == nullptr || y < 0 || y >= kWorldHeight) return;
        std::uint8_t& l = c->data.light[static_cast<std::size_t>(cellIndex(floorMod(x, kChunkSize), y, floorMod(z, kChunkSize)))];
        const std::uint8_t updated = sky ? static_cast<std::uint8_t>((l & 0x0F) | (level << 4)) : static_cast<std::uint8_t>((l & 0xF0) | level);
        if (updated != l) {
            l = updated;
            markDirty(x, y, z);
        }
    }
    int channel(int x, int y, int z, bool sky) const {
        const std::uint8_t l = getLight(x, y, z);
        return sky ? (l >> 4) : (l & 15);
    }

    static constexpr int kDirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

    // Nivel que llega a una vecina desde una celda con `level`.
    static int spread(int level, BlockId neighbor, bool sky, bool downward) {
        const int filter = filterOf(neighbor);
        if (filter >= 15) return 0;
        if (sky && downward && level == 15 && filter == 0) return 15;  // la luz del cielo baja sin perderse
        return level - std::max(1, filter);
    }

    void propagate(std::deque<LightNode>& queue, bool sky) {
        while (!queue.empty()) {
            const LightNode n = queue.front();
            queue.pop_front();
            const int level = channel(n.x, n.y, n.z, sky);
            if (level <= 1) continue;
            for (int d = 0; d < 6; ++d) {
                const int x = n.x + kDirs[d][0], y = n.y + kDirs[d][1], z = n.z + kDirs[d][2];
                if (y < 0 || y >= kWorldHeight) continue;
                if (chunkOf(x, z) == nullptr) continue;
                const BlockId id = getBlock(x, y, z);
                const int next = spread(level, id, sky, d == 3);
                if (next > channel(x, y, z, sky)) {
                    setLightChannel(x, y, z, sky, next);
                    queue.push_back({x, y, z, static_cast<std::uint8_t>(next)});
                }
            }
        }
    }

    // Quita la luz que dependia de (x, y, z) y la rellena desde lo que queda.
    void removeLight(int x, int y, int z, bool sky) {
        std::deque<LightNode> removal;
        std::deque<LightNode> refill;
        const int start = channel(x, y, z, sky);
        setLightChannel(x, y, z, sky, 0);
        removal.push_back({x, y, z, static_cast<std::uint8_t>(start)});
        while (!removal.empty()) {
            const LightNode n = removal.front();
            removal.pop_front();
            for (int d = 0; d < 6; ++d) {
                const int nx = n.x + kDirs[d][0], ny = n.y + kDirs[d][1], nz = n.z + kDirs[d][2];
                if (ny < 0 || ny >= kWorldHeight || chunkOf(nx, nz) == nullptr) continue;
                const int nl = channel(nx, ny, nz, sky);
                if (nl == 0) continue;
                const bool vertical = sky && d == 3 && n.level == 15 && nl == 15;
                if (nl < n.level || vertical) {
                    setLightChannel(nx, ny, nz, sky, 0);
                    removal.push_back({nx, ny, nz, static_cast<std::uint8_t>(nl)});
                } else {
                    refill.push_back({nx, ny, nz, static_cast<std::uint8_t>(nl)});
                }
            }
        }
        propagate(refill, sky);
    }

    // Luz inicial de un chunk recien llegado y del borde con sus vecinos.
    void lightNewChunk(Chunk& chunk) {
        ChunkData& d = chunk.data;
        const int x0 = d.cx * kChunkSize, z0 = d.cz * kChunkSize;
        chunk.top = 0;
        for (int y = kWorldHeight - 1; y > 0 && chunk.top == 0; --y) {
            for (int i = 0; i < kChunkSize * kChunkSize; ++i) {
                if (d.blocks[static_cast<std::size_t>(i + y * kChunkSize * kChunkSize)] != block::Air) {
                    chunk.top = y;
                    break;
                }
            }
        }
        // Cielo: de arriba abajo por columna.
        for (int z = 0; z < kChunkSize; ++z) {
            for (int x = 0; x < kChunkSize; ++x) {
                int level = 15;
                for (int y = kWorldHeight - 1; y >= 0; --y) {
                    const int filter = filterOf(d.get(x, y, z));
                    if (level > 0 && filter > 0) level = std::max(0, level - std::max(filter, 1));
                    if (filter >= 15) level = 0;
                    std::uint8_t& l = d.light[static_cast<std::size_t>(cellIndex(x, y, z))];
                    l = static_cast<std::uint8_t>((level << 4) | blockDef(d.get(x, y, z)).light);
                }
            }
        }
        // Semillas: celdas con luz junto a una vecina transparente mas oscura
        // (dentro del chunk y en el borde de los vecinos que ya estan).
        std::deque<LightNode> sky_queue;
        std::deque<LightNode> torch_queue;
        const auto consider = [&](int wx, int y, int wz) {
            const std::uint8_t l = getLight(wx, y, wz);
            const int s = l >> 4, t = l & 15;
            if (s <= 1 && t <= 1) return;
            for (int dd = 0; dd < 6; ++dd) {
                const int nx = wx + kDirs[dd][0], ny = y + kDirs[dd][1], nz = wz + kDirs[dd][2];
                if (ny < 0 || ny >= kWorldHeight || chunkOf(nx, nz) == nullptr) continue;
                const BlockId id = getBlock(nx, ny, nz);
                if (filterOf(id) >= 15) continue;
                const std::uint8_t nl = getLight(nx, ny, nz);
                if (s > 1 && spread(s, id, true, dd == 3) > (nl >> 4)) {
                    sky_queue.push_back({wx, y, wz, static_cast<std::uint8_t>(s)});
                    break;
                }
            }
            if (t > 1) torch_queue.push_back({wx, y, wz, static_cast<std::uint8_t>(t)});
        };
        // Mas arriba que todo lo de alrededor no hay nada que propagar.
        const int y_max = topAround(d.cx, d.cz);
        for (int y = 0; y <= y_max; ++y) {
            for (int z = 0; z < kChunkSize; ++z) {
                for (int x = 0; x < kChunkSize; ++x) {
                    const bool border = x == 0 || z == 0 || x == kChunkSize - 1 || z == kChunkSize - 1;
                    const std::uint8_t l = d.light[static_cast<std::size_t>(cellIndex(x, y, z))];
                    // Dentro: solo las de luz del cielo que tocan sombra, y las antorchas.
                    if (!border && (l & 15) <= 1) {
                        const int s = l >> 4;
                        if (s <= 1) continue;
                        // Basta con mirar a los lados (arriba/abajo ya es la columna).
                        bool darker = false;
                        for (int dd = 0; dd < 4 && !darker; ++dd) {
                            const int nx = x + (dd == 0 ? 1 : dd == 1 ? -1 : 0), nz = z + (dd == 2 ? 1 : dd == 3 ? -1 : 0);
                            const std::uint8_t nl = d.light[static_cast<std::size_t>(cellIndex(nx, y, nz))];
                            darker = (nl >> 4) + 1 < s && filterOf(d.get(nx, y, nz)) < 15;
                        }
                        if (darker) sky_queue.push_back({x0 + x, y, z0 + z, static_cast<std::uint8_t>(s)});
                        continue;
                    }
                    consider(x0 + x, y, z0 + z);
                }
            }
        }
        // Borde de los vecinos cargados (su luz entra en este chunk).
        const bool west = chunkAt(d.cx - 1, d.cz) != nullptr, east = chunkAt(d.cx + 1, d.cz) != nullptr;
        const bool north = chunkAt(d.cx, d.cz - 1) != nullptr, south = chunkAt(d.cx, d.cz + 1) != nullptr;
        for (int i = 0; i < kChunkSize; ++i) {
            for (int y = 0; y <= y_max; ++y) {
                if (west) consider(x0 - 1, y, z0 + i);
                if (east) consider(x0 + kChunkSize, y, z0 + i);
                if (north) consider(x0 + i, y, z0 - 1);
                if (south) consider(x0 + i, y, z0 + kChunkSize);
            }
        }
        propagate(sky_queue, true);
        propagate(torch_queue, false);
    }

    // ------------------------------------------------------------ chunks
    std::filesystem::path worldFolder() const {
        if (world_name.empty() || save_root.empty()) return {};
        return save_root / fromUtf8(safeName(world_name));
    }
    std::filesystem::path chunkFile(int cx, int cz) const {
        const std::filesystem::path folder = worldFolder();
        if (folder.empty()) return {};
        return folder / "chunks" / (std::to_string(cx) + "." + std::to_string(cz) + ".bin");
    }

    void insertChunk(std::unique_ptr<ChunkData> data) {
        auto chunk = std::make_unique<Chunk>();
        chunk->data = std::move(*data);
        for (int s = 0; s < kSectionCount; ++s) {
            chunk->dirty[static_cast<std::size_t>(s)] = true;
            chunk->version[static_cast<std::size_t>(s)] = 1;
        }
        const int cx = chunk->data.cx, cz = chunk->data.cz;
        Chunk& ref = *chunk;
        chunks[chunkKey(cx, cz)] = std::move(chunk);
        lightNewChunk(ref);
        for (const auto& o : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
            if (Chunk* n = chunkAt(cx + o.first, cz + o.second)) n->collision_built.fill(false);
        }
        // Los vecinos rehacen sus caras del borde (ahora tapadas o no).
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dz == 0) continue;
                if (Chunk* n = chunkAt(cx + dx, cz + dz)) {
                    for (int s = 0; s < kSectionCount; ++s) {
                        if (n->meshed[static_cast<std::size_t>(s)] || n->uploaded[static_cast<std::size_t>(s)]) {
                            n->dirty[static_cast<std::size_t>(s)] = true;
                            ++n->version[static_cast<std::size_t>(s)];
                        }
                    }
                }
            }
        }
    }

    void saveChunk(const Chunk& chunk) {
        if (!chunk.data.modified) return;
        const std::filesystem::path file = chunkFile(chunk.data.cx, chunk.data.cz);
        if (!file.empty() && !writeChunkFile(file, chunk.data)) {
            std::cerr << "[Voxel] No se pudo guardar " << file.string() << "\n";
        }
    }

    void unloadChunk(std::int64_t key) {
        const auto it = chunks.find(key);
        if (it == chunks.end()) return;
        saveChunk(*it->second);
        for (int s = 0; s < kSectionCount; ++s) {
            if (it->second->uploaded[static_cast<std::size_t>(s)]) {
                section_updates.push_back({sectionKey(it->second->data.cx, s, it->second->data.cz), Vec3{}, {}});
            }
        }
        removeColliders(*it->second);
        forgetCachedChunk();
        chunks.erase(it);
    }

    bool neighborsReady(int cx, int cz) const {
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (!chunkAt(cx + dx, cz + dz)) return false;
            }
        }
        return true;
    }

    int renderRadius() const { return std::clamp(settings.render_distance, 2, 32); }
    bool inRenderRadius(int cx, int cz) const {
        const int dx = cx - viewer_cx, dz = cz - viewer_cz, r = renderRadius();
        return dx * dx + dz * dz <= r * r;
    }

    // Copia de la seccion con su borde (18^3) para mallarla en otro hilo.
    std::unique_ptr<SectionSnapshot> snapshot(int cx, int sy, int cz, bool& empty) const {
        auto s = std::make_unique<SectionSnapshot>();
        s->cx = cx;
        s->sy = sy;
        s->cz = cz;
        const int x0 = cx * kChunkSize, y0 = sy * kChunkSize, z0 = cz * kChunkSize;
        // Los 3 x 3 chunks de alrededor, para no buscarlos por celda.
        const Chunk* around[3][3];
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) around[dz + 1][dx + 1] = chunkAt(cx + dx, cz + dz);
        }
        int solid = 0;
        for (int y = -1; y <= kChunkSize; ++y) {
            const int wy = y0 + y;
            for (int z = -1; z <= kChunkSize; ++z) {
                const int gz = z < 0 ? 0 : (z >= kChunkSize ? 2 : 1);
                const int lz = floorMod(z, kChunkSize);
                for (int x = -1; x <= kChunkSize; ++x) {
                    const int gx = x < 0 ? 0 : (x >= kChunkSize ? 2 : 1);
                    const int lx = floorMod(x, kChunkSize);
                    const Chunk* c = around[gz][gx];
                    BlockId id = block::Air;
                    std::uint8_t light = 0xF0;
                    if (wy < 0) {
                        id = block::Bedrock;
                        light = 0;
                    } else if (wy < kWorldHeight && c != nullptr) {
                        const std::size_t i = static_cast<std::size_t>(cellIndex(lx, wy, lz));
                        id = c->data.blocks[i];
                        light = c->data.light[i];
                    }
                    const std::size_t si = static_cast<std::size_t>(SectionSnapshot::index(x, y, z));
                    s->blocks[si] = id;
                    s->light[si] = light;
                    if (x >= 0 && y >= 0 && z >= 0 && x < kChunkSize && y < kChunkSize && z < kChunkSize && id != block::Air &&
                        id != block::Water) {
                        ++solid;
                    }
                }
            }
        }
        for (int z = -1; z <= kChunkSize; ++z) {
            const int gz = z < 0 ? 0 : (z >= kChunkSize ? 2 : 1);
            for (int x = -1; x <= kChunkSize; ++x) {
                const int gx = x < 0 ? 0 : (x >= kChunkSize ? 2 : 1);
                const Chunk* c = around[gz][gx];
                const std::size_t column = static_cast<std::size_t>(floorMod(x, kChunkSize) + floorMod(z, kChunkSize) * kChunkSize);
                const std::size_t si = static_cast<std::size_t>((x + 1) + (z + 1) * SectionSnapshot::kSize);
                s->grass_tint[si] = c != nullptr ? c->data.grass_tint[column] : 0xFFF;
                s->foliage_tint[si] = c != nullptr ? c->data.foliage_tint[column] : 0xFFF;
            }
        }
        (void)x0;
        (void)z0;
        empty = solid == 0;
        return s;
    }

    // ------------------------------------------------------------ cada frame
    void collect(double budget_ms) {
        std::vector<Result> done;
        {
            std::lock_guard lock(mutex);
            done.swap(results);
        }
        const auto start = Clock::now();
        std::vector<Result> later;
        for (Result& r : done) {
            if (r.epoch != epoch) {
                if (r.kind == 0) generating.erase(chunkKey(r.cx, r.cz));
                continue;  // de otro mundo
            }
            if (r.kind == 0) {
                // Integrar un chunk cuesta (la luz): con presupuesto.
                if (msSince(start) > budget_ms) {
                    later.push_back(std::move(r));
                    continue;
                }
                generating.erase(chunkKey(r.cx, r.cz));
                if (chunkAt(r.cx, r.cz) != nullptr) continue;
                last_chunk_ms = r.milliseconds;
                insertChunk(std::move(r.chunk));
            } else {
                Chunk* c = chunkAt(r.cx, r.cz);
                if (c == nullptr) continue;
                const std::size_t s = static_cast<std::size_t>(r.sy);
                if (c->in_flight[s] == r.version) c->in_flight[s] = 0;
                if (r.version != c->version[s]) continue;  // ya cambio otra vez
                last_mesh_ms = r.milliseconds;
                c->meshed[s] = true;
                c->ever_meshed = true;
                c->uploaded[s] = !r.vertices.empty();
                section_updates.push_back({sectionKey(r.cx, r.sy, r.cz),
                                           Vec3{static_cast<float>(r.cx * kChunkSize), static_cast<float>(r.sy * kChunkSize),
                                                static_cast<float>(r.cz * kChunkSize)},
                                           std::move(r.vertices)});
            }
        }
        if (!later.empty()) {
            std::lock_guard lock(mutex);
            for (Result& r : later) results.push_back(std::move(r));
        }
    }

    void stream(double budget_ms) {
        if (!active || !generator || !has_viewer) return;
        const int radius = renderRadius();
        // +2: un chunk del borde (tambien en diagonal, a radius + 1.41) tiene
        // cargados sus 8 vecinos y se puede mallar.
        const int load_radius = radius + 2;
        // Descargar lo lejano (con margen para no cargar y descargar al borde).
        std::vector<std::int64_t> distant;  // ("far" es una macro de Windows)
        for (const auto& [key, chunk] : chunks) {
            const int dx = chunk->data.cx - viewer_cx, dz = chunk->data.cz - viewer_cz;
            if (dx * dx + dz * dz > (load_radius + 2) * (load_radius + 2)) distant.push_back(key);
        }
        for (const std::int64_t key : distant) unloadChunk(key);

        // Pedir los que faltan, de cerca a lejos.
        std::vector<std::pair<int, std::pair<int, int>>> wanted;
        for (int dz = -load_radius; dz <= load_radius; ++dz) {
            for (int dx = -load_radius; dx <= load_radius; ++dx) {
                const int d2 = dx * dx + dz * dz;
                if (d2 > load_radius * load_radius) continue;
                const int cx = viewer_cx + dx, cz = viewer_cz + dz;
                const std::int64_t key = chunkKey(cx, cz);
                if (chunks.count(key) || generating.count(key)) continue;
                wanted.push_back({d2, {cx, cz}});
            }
        }
        std::sort(wanted.begin(), wanted.end());
        const std::size_t max_in_flight = workers.size() * 3;
        for (const auto& [d2, pos] : wanted) {
            if (generating.size() >= max_in_flight) break;
            Job job;
            job.kind = 0;
            job.cx = pos.first;
            job.cz = pos.second;
            job.epoch = epoch;
            job.generator = generator;
            job.priority = static_cast<float>(d2);
            if (const std::filesystem::path file = chunkFile(pos.first, pos.second); !file.empty()) {
                std::error_code error;
                if (std::filesystem::exists(file, error)) job.saved = file;
            }
            generating.insert(chunkKey(pos.first, pos.second));
            submit(std::move(job));
        }

        // Mallar las secciones sucias con los 8 vecinos cargados, de cerca a lejos.
        struct Candidate {
            float distance;
            int cx, cz, sy;
        };
        std::vector<Candidate> candidates;
        for (const auto& [key, chunk] : chunks) {
            if (!inRenderRadius(chunk->data.cx, chunk->data.cz)) continue;
            bool any = false;
            for (int s = 0; s < kSectionCount && !any; ++s) any = chunk->dirty[static_cast<std::size_t>(s)] && chunk->in_flight[static_cast<std::size_t>(s)] == 0;
            if (!any || !neighborsReady(chunk->data.cx, chunk->data.cz)) continue;
            for (int s = 0; s < kSectionCount; ++s) {
                if (!chunk->dirty[static_cast<std::size_t>(s)] || chunk->in_flight[static_cast<std::size_t>(s)] != 0) continue;
                const float sx = static_cast<float>(chunk->data.cx * kChunkSize + 8) - viewer.x;
                const float sy = static_cast<float>(s * kChunkSize + 8) - viewer.y;
                const float sz = static_cast<float>(chunk->data.cz * kChunkSize + 8) - viewer.z;
                candidates.push_back({sx * sx + sy * sy * 0.5f + sz * sz, chunk->data.cx, chunk->data.cz, s});
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
        const auto start = Clock::now();
        int in_flight = 0;
        {
            std::lock_guard lock(mutex);
            in_flight = static_cast<int>(jobs.size());
        }
        for (const Candidate& c : candidates) {
            if (msSince(start) > budget_ms || in_flight > static_cast<int>(workers.size()) * 6) break;
            Chunk* chunk = chunkAt(c.cx, c.cz);
            const std::size_t s = static_cast<std::size_t>(c.sy);
            bool empty = false;
            std::unique_ptr<SectionSnapshot> snap = snapshot(c.cx, c.sy, c.cz, empty);
            chunk->dirty[s] = false;
            if (empty) {
                chunk->meshed[s] = true;
                if (chunk->uploaded[s]) {
                    section_updates.push_back({sectionKey(c.cx, c.sy, c.cz), Vec3{}, {}});
                    chunk->uploaded[s] = false;
                }
                continue;
            }
            Job job;
            job.kind = 1;
            job.cx = c.cx;
            job.cz = c.cz;
            job.sy = c.sy;
            job.version = chunk->version[s];
            job.epoch = epoch;
            job.snapshot = std::move(snap);
            job.priority = c.distance * 0.01f - 1e6f;  // mallar va antes que generar lejos
            chunk->in_flight[s] = job.version;
            submit(std::move(job));
            ++in_flight;
        }
    }

    void flowWater(int budget) {
        const int sea = generator ? generator->seaLevel() : 62;
        while (!water_flow.empty() && budget-- > 0) {
            const BlockPos p = water_flow.front();
            water_flow.pop_front();
            if (getBlock(p.x, p.y, p.z) != block::Water) continue;
            for (int d = 0; d < 6; ++d) {
                if (d == 2) continue;  // no sube
                const int x = p.x + kDirs[d][0], y = p.y + kDirs[d][1], z = p.z + kDirs[d][2];
                if (y < 1 || y >= sea || chunkOf(x, z) == nullptr) continue;
                const BlockId id = getBlock(x, y, z);
                if (id == block::Air || (blockDef(id).replaceable && id != block::Water)) {
                    setBlockInternal(x, y, z, block::Water, false);
                    water_flow.push_back({x, y, z});
                }
            }
        }
    }

    bool setBlockInternal(int x, int y, int z, BlockId id, bool by_player) {
        if (y < 0 || y >= kWorldHeight) return false;
        Chunk* c = chunkOf(x, z);
        if (c == nullptr) return false;
        BlockId& cell = c->data.blocks[static_cast<std::size_t>(cellIndex(floorMod(x, kChunkSize), y, floorMod(z, kChunkSize)))];
        const BlockId before = cell;
        if (before == id) return true;
        // Al quitar un bloque bajo el nivel del mar junto al agua, entra el agua.
        const int sea = generator ? generator->seaLevel() : 62;
        if (by_player && id == block::Air && y < sea) {
            for (int d = 0; d < 6; ++d) {
                if (d == 3) continue;  // el agua de abajo no sube
                if (getBlock(x + kDirs[d][0], y + kDirs[d][1], z + kDirs[d][2]) == block::Water) {
                    id = block::Water;
                    water_flow.push_back({x, y, z});
                    break;
                }
            }
        }
        cell = id;
        c->data.modified = true;
        if (id != block::Air) c->top = std::max(c->top, y);
        dirtyCollision(x, y, z);
        for (const auto& dir : kDirs) dirtyCollision(x + dir[0], y + dir[1], z + dir[2]);
        markDirty(x, y, z);
        // Luz: se quita lo que dependia de esta celda y se rellena.
        removeLight(x, y, z, true);
        removeLight(x, y, z, false);
        std::deque<LightNode> sky_queue, torch_queue;
        for (int d = 0; d < 6; ++d) {
            const int nx = x + kDirs[d][0], ny = y + kDirs[d][1], nz = z + kDirs[d][2];
            if (ny < 0 || ny >= kWorldHeight || chunkOf(nx, nz) == nullptr) continue;
            sky_queue.push_back({nx, ny, nz, 0});
            torch_queue.push_back({nx, ny, nz, 0});
        }
        if (blockDef(id).light > 0) {
            setLightChannel(x, y, z, false, blockDef(id).light);
            torch_queue.push_back({x, y, z, blockDef(id).light});
        }
        // Cielo directo: si arriba no hay nada, esta celda (si es transparente) vuelve a 15.
        if (filterOf(id) == 0 && (y + 1 >= kWorldHeight || channel(x, y + 1, z, true) == 15)) {
            setLightChannel(x, y, z, true, 15);
            sky_queue.push_back({x, y, z, 15});
        }
        propagate(sky_queue, true);
        propagate(torch_queue, false);
        // Una planta sin suelo se cae; el agua de alrededor fluye si se abrio un hueco.
        if (y + 1 < kWorldHeight) {
            const BlockDef& above = blockDef(getBlock(x, y + 1, z));
            if (above.shape == BlockShape::Cross && !blockDef(id).solid) setBlockInternal(x, y + 1, z, block::Air, false);
        }
        if (listener) listener(BlockPos{x, y, z}, before, id);
        return true;
    }

    // ------------------------------------------------------------ colision
    static std::uint64_t colliderKey(int cx, int sy, int cz) { return sectionKey(cx, sy, cz) | (1ull << 63); }

    void dirtyCollision(int x, int y, int z) {
        if (y < 0 || y >= kWorldHeight) return;
        if (Chunk* c = chunkOf(x, z)) c->collision_built[static_cast<std::size_t>(y / kChunkSize)] = false;
    }

    void removeColliders(Chunk& chunk) {
        for (int s = 0; s < kSectionCount; ++s) {
            if (chunk.has_collider[static_cast<std::size_t>(s)] && physics != nullptr) {
                physics->removeStaticMesh(colliderKey(chunk.data.cx, s, chunk.data.cz));
            }
            chunk.has_collider[static_cast<std::size_t>(s)] = false;
            chunk.collision_built[static_cast<std::size_t>(s)] = false;
        }
    }

    bool solidAt(int x, int y, int z) const {
        const BlockDef& def = blockDef(getBlock(x, y, z));
        return def.solid && def.shape == BlockShape::Cube;
    }

    // Caras expuestas de los bloques solidos de la seccion, fusionadas en
    // rectangulos (greedy meshing) por direccion y capa. Triangulos
    // antihorarios vistos desde fuera, relativos a la esquina de la seccion.
    void collisionTriangles(int cx, int sy, int cz, std::vector<Vec3>& out) const {
        out.clear();
        const int base[3] = {cx * kChunkSize, sy * kChunkSize, cz * kChunkSize};
        std::array<bool, kChunkSize * kChunkSize> mask{};
        for (int d = 0; d < 3; ++d) {
            const int u = (d + 1) % 3, v = (d + 2) % 3;
            for (const int sign : {1, -1}) {
                for (int k = 0; k < kChunkSize; ++k) {
                    bool any = false;
                    for (int b = 0; b < kChunkSize; ++b) {
                        for (int a = 0; a < kChunkSize; ++a) {
                            int c[3];
                            c[d] = base[d] + k;
                            c[u] = base[u] + a;
                            c[v] = base[v] + b;
                            bool face = solidAt(c[0], c[1], c[2]);
                            if (face) {
                                c[d] += sign;
                                face = !solidAt(c[0], c[1], c[2]);
                            }
                            mask[static_cast<std::size_t>(a + b * kChunkSize)] = face;
                            any = any || face;
                        }
                    }
                    if (!any) continue;
                    const float plane = static_cast<float>(sign > 0 ? k + 1 : k);
                    const auto corner = [&](int a, int b) {
                        float p[3];
                        p[d] = plane;
                        p[u] = static_cast<float>(a);
                        p[v] = static_cast<float>(b);
                        return Vec3{p[0], p[1], p[2]};
                    };
                    for (int b = 0; b < kChunkSize; ++b) {
                        for (int a = 0; a < kChunkSize;) {
                            if (!mask[static_cast<std::size_t>(a + b * kChunkSize)]) {
                                ++a;
                                continue;
                            }
                            int w = 1;
                            while (a + w < kChunkSize && mask[static_cast<std::size_t>(a + w + b * kChunkSize)]) ++w;
                            int h = 1;
                            for (bool full = true; b + h < kChunkSize && full; h += full ? 1 : 0) {
                                for (int i = 0; i < w && full; ++i) full = mask[static_cast<std::size_t>(a + i + (b + h) * kChunkSize)];
                            }
                            for (int j = 0; j < h; ++j) {
                                for (int i = 0; i < w; ++i) mask[static_cast<std::size_t>(a + i + (b + j) * kChunkSize)] = false;
                            }
                            const Vec3 q0 = corner(a, b), q1 = corner(a + w, b), q2 = corner(a + w, b + h), q3 = corner(a, b + h);
                            if (sign > 0) {
                                out.insert(out.end(), {q0, q1, q2, q0, q2, q3});
                            } else {
                                out.insert(out.end(), {q0, q2, q1, q0, q3, q2});
                            }
                            a += w;
                        }
                    }
                }
            }
        }
    }

    int collisionRadius() const { return std::clamp(settings.collision_distance, 1, renderRadius()); }

    // Mallas de colision de las secciones cercanas (de cerca a lejos, con
    // presupuesto) y fuera las lejanas.
    void updateCollision(double budget_ms) {
        if (physics == nullptr || !has_viewer) return;
        const int r = collisionRadius();
        struct Candidate {
            float distance;
            Chunk* chunk;
            int sy;
        };
        std::vector<Candidate> candidates;
        for (auto& [key, chunk] : chunks) {
            const int dx = chunk->data.cx - viewer_cx, dz = chunk->data.cz - viewer_cz;
            const int d2 = dx * dx + dz * dz;
            if (d2 > (r + 1) * (r + 1)) {
                removeColliders(*chunk);
                continue;
            }
            if (d2 > r * r || !neighborsReady(chunk->data.cx, chunk->data.cz)) continue;
            const int top_section = std::min(chunk->top / kChunkSize, kSectionCount - 1);
            for (int s = 0; s <= top_section; ++s) {
                if (chunk->collision_built[static_cast<std::size_t>(s)]) continue;
                const float sx = static_cast<float>(chunk->data.cx * kChunkSize + 8) - viewer.x;
                const float sy = static_cast<float>(s * kChunkSize + 8) - viewer.y;
                const float sz = static_cast<float>(chunk->data.cz * kChunkSize + 8) - viewer.z;
                candidates.push_back({sx * sx + sy * sy + sz * sz, chunk.get(), s});
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
        const auto start = Clock::now();
        std::vector<Vec3> triangles;
        for (const Candidate& c : candidates) {
            if (msSince(start) > budget_ms) break;
            Chunk& chunk = *c.chunk;
            const std::size_t s = static_cast<std::size_t>(c.sy);
            collisionTriangles(chunk.data.cx, c.sy, chunk.data.cz, triangles);
            const std::uint64_t key = colliderKey(chunk.data.cx, c.sy, chunk.data.cz);
            if (triangles.empty()) {
                if (chunk.has_collider[s]) physics->removeStaticMesh(key);
                chunk.has_collider[s] = false;
            } else {
                physics->setStaticMesh(key,
                                       Vec3{static_cast<float>(chunk.data.cx * kChunkSize), static_cast<float>(c.sy * kChunkSize),
                                            static_cast<float>(chunk.data.cz * kChunkSize)},
                                       triangles);
                chunk.has_collider[s] = true;
            }
            chunk.collision_built[s] = true;
        }
    }

    // ------------------------------------------------------------ mundos
    void resetWorld(int new_seed) {
        ++epoch;
        {
            std::lock_guard lock(mutex);
            jobs.clear();
        }
        if (physics != nullptr) {
            for (auto& [key, chunk] : chunks) removeColliders(*chunk);
        }
        forgetCachedChunk();
        chunks.clear();
        generating.clear();
        section_updates.clear();
        water_flow.clear();
        clear_renderer = true;
        seed = new_seed;
        generator = std::make_shared<Generator>(seed, static_cast<int>(std::lround(settings.sea_level)));
    }

    void writeWorldInfo() const {
        const std::filesystem::path folder = worldFolder();
        if (folder.empty()) return;
        nlohmann::json json;
        json["name"] = world_name;
        json["seed"] = seed;
        json["sea_level"] = settings.sea_level;
        json["last_played"] = static_cast<std::int64_t>(std::time(nullptr));
        json["meta"] = meta_values;
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        std::ofstream out(folder / "world.json", std::ios::binary | std::ios::trunc);
        out << json.dump(2);
    }
};

// ================================================================================

VoxelSystem::VoxelSystem() : impl_(std::make_unique<Impl>()) {}
VoxelSystem::~VoxelSystem() { stop(); }

void VoxelSystem::setAssetsRoot(const std::filesystem::path& root) { impl_->assets_root = root; }
void VoxelSystem::setSaveRoot(const std::filesystem::path& root) { impl_->save_root = root; }
const std::filesystem::path& VoxelSystem::saveRoot() const { return impl_->save_root; }
void VoxelSystem::setBlockListener(BlockListener listener) { impl_->listener = std::move(listener); }
void VoxelSystem::setPhysics(physics::PhysicsSystem* physics) {
    Impl& d = *impl_;
    if (d.physics == physics) return;
    for (auto& [key, chunk] : d.chunks) d.removeColliders(*chunk);
    d.physics = physics;
}

void VoxelSystem::start(ecs::World& world) {
    Impl& d = *impl_;
    stop();
    for (const entt::entity h : world.registry().view<VoxelWorld>()) {
        const ecs::Entity e = world.wrap(h);
        if (!e.activeInHierarchy()) continue;
        d.settings = e.get<VoxelWorld>();
        d.active = true;
        break;
    }
    if (!d.active) return;
    d.world_name.clear();
    d.meta_values.clear();
    d.resetWorld(d.settings.seed);
    // Bajo el agua solo si la camara esta en un bloque de agua de verdad.
    Impl* impl = &d;
    water::setUnderwaterOverride([impl](const Vec3& camera) {
        if (!impl->active) return -1;
        const int x = static_cast<int>(std::floor(camera.x)), y = static_cast<int>(std::floor(camera.y)),
                  z = static_cast<int>(std::floor(camera.z));
        if (impl->chunkOf(x, z) == nullptr) return -1;
        return impl->getBlock(x, y, z) == block::Water ? 1 : 0;
    });
}

void VoxelSystem::stop() {
    Impl& d = *impl_;
    if (d.active && !d.world_name.empty()) saveWorld();
    if (d.active) water::setUnderwaterOverride({});
    d.active = false;
    d.world_name.clear();
    d.resetWorld(d.seed);
    d.generator.reset();
    d.has_viewer = false;
}

bool VoxelSystem::active() const { return impl_->active; }
const VoxelWorld& VoxelSystem::settings() const { return impl_->settings; }

void VoxelSystem::update(float delta_seconds, const Vec3& viewer) {
    Impl& d = *impl_;
    if (!d.active) return;
    d.time += delta_seconds;
    d.viewer = viewer;
    d.viewer_cx = floorDiv(static_cast<int>(std::floor(viewer.x)), kChunkSize);
    d.viewer_cz = floorDiv(static_cast<int>(std::floor(viewer.z)), kChunkSize);
    d.has_viewer = true;
    d.collect(6.0);
    d.flowWater(64);
    d.stream(3.0);
    d.updateCollision(2.0);
}

void VoxelSystem::syncRenderer(gfx::VulkanRenderer& renderer) {
    Impl& d = *impl_;
    if (d.clear_renderer) {
        renderer.clearVoxelSections();
        d.clear_renderer = false;
    }
    if (!d.active) {
        d.section_updates.clear();
        return;
    }
    // Texturas: se generan en otro hilo la primera vez (o si cambia la carpeta).
    const std::filesystem::path folder = d.settings.textures.empty() ? std::filesystem::path{} : d.assets_root / fromUtf8(d.settings.textures);
    const std::uint32_t size = std::bit_ceil(static_cast<std::uint32_t>(std::clamp(d.settings.texture_size, 16, 1024)));
    const std::string signature = folder.string() + "|" + std::to_string(size);
    if (signature != d.textures_signature && !d.textures_future.valid()) {
        d.textures_signature = signature;
        d.textures_future = std::async(std::launch::async, [folder, size]() {
            const auto& names = textureNames();
            std::vector<gfx::VoxelTextureLayer> layers(names.size());
            std::vector<std::future<void>> parts;
            for (std::size_t i = 0; i < names.size(); ++i) {
                parts.push_back(std::async(std::launch::async, [&, i]() { buildTexture(names[i], size, folder, layers[i]); }));
            }
            for (auto& p : parts) p.wait();
            return layers;
        });
        d.textures_uploaded = false;
    }
    if (d.textures_future.valid() && d.textures_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const std::vector<gfx::VoxelTextureLayer> layers = d.textures_future.get();
        renderer.setVoxelTextures(size, layers);
        d.textures_uploaded = true;
    }
    for (Impl::SectionUpdate& u : d.section_updates) {
        if (u.vertices.empty()) {
            renderer.removeVoxelSection(u.key);
        } else {
            renderer.setVoxelSection(u.key, u.origin, u.vertices.data(), static_cast<std::uint32_t>(u.vertices.size() / 2));
        }
    }
    d.section_updates.clear();
    renderer.setVoxelTime(d.time);
}

void VoxelSystem::waitUntilReady(const Vec3& center, int radius_chunks, float timeout_seconds) {
    Impl& d = *impl_;
    if (!d.active) return;
    const auto start = Clock::now();
    const int cx = floorDiv(static_cast<int>(std::floor(center.x)), kChunkSize);
    const int cz = floorDiv(static_cast<int>(std::floor(center.z)), kChunkSize);
    // Un circulo, como el que se carga (y nunca mas alla de lo que se malla).
    const int radius = std::min(radius_chunks, d.renderRadius());
    for (;;) {
        update(0.0f, center);
        bool ready = true;
        for (int dz = -radius; dz <= radius && ready; ++dz) {
            for (int dx = -radius; dx <= radius && ready; ++dx) {
                if (dx * dx + dz * dz > radius * radius) continue;
                const Impl::Chunk* c = d.chunkAt(cx + dx, cz + dz);
                if (c == nullptr) {
                    ready = false;
                    break;
                }
                for (int s = 0; s < kSectionCount; ++s) {
                    if (c->dirty[static_cast<std::size_t>(s)] || c->in_flight[static_cast<std::size_t>(s)] != 0) ready = false;
                }
            }
        }
        if (ready) return;
        if (std::chrono::duration<float>(Clock::now() - start).count() > timeout_seconds) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// --- Mundos --------------------------------------------------------------------------

bool VoxelSystem::newWorld(const std::string& name, int seed) {
    Impl& d = *impl_;
    if (!d.active || safeName(name).empty()) return false;
    if (!d.world_name.empty()) saveWorld();
    d.world_name = name;
    d.meta_values.clear();
    // Un mundo nuevo empieza sin nada guardado.
    std::error_code error;
    std::filesystem::remove_all(d.worldFolder(), error);
    d.resetWorld(seed);
    d.writeWorldInfo();
    return true;
}

bool VoxelSystem::loadWorld(const std::string& name) {
    Impl& d = *impl_;
    if (!d.active) return false;
    const std::filesystem::path folder = d.save_root / fromUtf8(safeName(name));
    std::ifstream in(folder / "world.json", std::ios::binary);
    if (!in) return false;
    const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
    if (json.is_discarded()) return false;
    if (!d.world_name.empty()) saveWorld();
    d.world_name = name;
    d.meta_values.clear();
    if (json.contains("meta") && json["meta"].is_object()) {
        for (const auto& [key, value] : json["meta"].items()) {
            if (value.is_string()) d.meta_values[key] = value.get<std::string>();
        }
    }
    d.resetWorld(json.value("seed", d.settings.seed));
    return true;
}

bool VoxelSystem::saveWorld() {
    Impl& d = *impl_;
    if (!d.active || d.world_name.empty()) return false;
    for (const auto& [key, chunk] : d.chunks) {
        if (chunk->data.modified) d.saveChunk(*chunk);
    }
    d.writeWorldInfo();
    return true;
}

std::vector<WorldInfo> VoxelSystem::listWorlds() const {
    std::vector<WorldInfo> out;
    std::error_code error;
    for (std::filesystem::directory_iterator it(impl_->save_root, error); !error && it != std::filesystem::directory_iterator();
         it.increment(error)) {
        std::ifstream in(it->path() / "world.json", std::ios::binary);
        if (!in) continue;
        const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
        if (json.is_discarded()) continue;
        WorldInfo info;
        info.name = json.value("name", std::string());
        info.seed = json.value("seed", 0);
        info.last_played = json.value("last_played", std::int64_t{0});
        if (json.contains("meta") && json["meta"].is_object()) info.mode = json["meta"].value("mode", std::string());
        if (!info.name.empty()) out.push_back(info);
    }
    std::sort(out.begin(), out.end(), [](const WorldInfo& a, const WorldInfo& b) { return a.last_played > b.last_played; });
    return out;
}

bool VoxelSystem::deleteWorld(const std::string& name) {
    Impl& d = *impl_;
    if (safeName(name).empty()) return false;
    if (d.world_name == name) d.world_name.clear();
    std::error_code error;
    return std::filesystem::remove_all(d.save_root / fromUtf8(safeName(name)), error) > 0 && !error;
}

const std::string& VoxelSystem::worldName() const { return impl_->world_name; }
int VoxelSystem::seed() const { return impl_->seed; }
void VoxelSystem::setMeta(const std::string& key, const std::string& value) { impl_->meta_values[key] = value; }
std::string VoxelSystem::meta(const std::string& key, const std::string& fallback) const {
    const auto it = impl_->meta_values.find(key);
    return it == impl_->meta_values.end() ? fallback : it->second;
}

// --- Bloques -------------------------------------------------------------------------

BlockId VoxelSystem::getBlock(int x, int y, int z) const { return impl_->getBlock(x, y, z); }
bool VoxelSystem::setBlock(int x, int y, int z, BlockId id) {
    if (id >= block::Count) return false;
    return impl_->setBlockInternal(x, y, z, id, true);
}
int VoxelSystem::skyLight(int x, int y, int z) const { return impl_->getLight(x, y, z) >> 4; }
int VoxelSystem::blockLight(int x, int y, int z) const { return impl_->getLight(x, y, z) & 15; }
bool VoxelSystem::isLoaded(int x, int z) const { return impl_->chunkOf(x, z) != nullptr; }
bool VoxelSystem::isReady(int x, int z) const {
    const Impl::Chunk* c = impl_->chunkOf(x, z);
    return c != nullptr && c->ever_meshed;
}
int VoxelSystem::surfaceHeight(int x, int z) const {
    return impl_->generator ? impl_->generator->terrainHeight(x, z) : 64;
}
bool VoxelSystem::inWater(const Vec3& p) const {
    return getBlock(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)), static_cast<int>(std::floor(p.z))) ==
           block::Water;
}

VoxelHit VoxelSystem::raycast(const Vec3& origin, const Vec3& direction, float max_distance) const {
    VoxelHit hit;
    const float len = core::length(direction);
    if (len < 1e-6f) return hit;
    const Vec3 dir = direction * (1.0f / len);
    int x = static_cast<int>(std::floor(origin.x)), y = static_cast<int>(std::floor(origin.y)), z = static_cast<int>(std::floor(origin.z));
    const int step_x = dir.x > 0 ? 1 : -1, step_y = dir.y > 0 ? 1 : -1, step_z = dir.z > 0 ? 1 : -1;
    const auto first = [](float o, float d, int cell) {
        if (std::abs(d) < 1e-9f) return 1e30f;
        const float boundary = d > 0 ? static_cast<float>(cell + 1) : static_cast<float>(cell);
        return (boundary - o) / d;
    };
    float t_x = first(origin.x, dir.x, x), t_y = first(origin.y, dir.y, y), t_z = first(origin.z, dir.z, z);
    const float d_x = std::abs(dir.x) < 1e-9f ? 1e30f : 1.0f / std::abs(dir.x);
    const float d_y = std::abs(dir.y) < 1e-9f ? 1e30f : 1.0f / std::abs(dir.y);
    const float d_z = std::abs(dir.z) < 1e-9f ? 1e30f : 1.0f / std::abs(dir.z);
    float t = 0.0f;
    BlockPos normal{};
    for (int i = 0; i < 512 && t <= max_distance; ++i) {
        const BlockId id = getBlock(x, y, z);
        const BlockDef& def = blockDef(id);
        if (def.shape != BlockShape::Air && def.shape != BlockShape::Liquid) {
            hit.hit = true;
            hit.block = BlockPos{x, y, z};
            hit.normal = normal;
            hit.id = id;
            hit.distance = t;
            hit.point = origin + dir * t;
            return hit;
        }
        if (!isLoaded(x, z)) break;
        if (t_x < t_y && t_x < t_z) {
            x += step_x;
            t = t_x;
            t_x += d_x;
            normal = BlockPos{-step_x, 0, 0};
        } else if (t_y < t_z) {
            y += step_y;
            t = t_y;
            t_y += d_y;
            normal = BlockPos{0, -step_y, 0};
        } else {
            z += step_z;
            t = t_z;
            t_z += d_z;
            normal = BlockPos{0, 0, -step_z};
        }
    }
    return hit;
}

bool VoxelSystem::boxCollides(const Vec3& c, const Vec3& h) const {
    const int x0 = static_cast<int>(std::floor(c.x - h.x)), x1 = static_cast<int>(std::floor(c.x + h.x - 1e-4f));
    const int y0 = static_cast<int>(std::floor(c.y - h.y)), y1 = static_cast<int>(std::floor(c.y + h.y - 1e-4f));
    const int z0 = static_cast<int>(std::floor(c.z - h.z)), z1 = static_cast<int>(std::floor(c.z + h.z - 1e-4f));
    for (int y = y0; y <= y1; ++y) {
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                if (!isLoaded(x, z) && y >= 0 && y < kWorldHeight) return true;  // sin cargar: pared
                if (blockDef(getBlock(x, y, z)).solid) return true;
            }
        }
    }
    return false;
}

VoxelSystem::MoveResult VoxelSystem::moveBox(const Vec3& center, const Vec3& half, const Vec3& delta) const {
    MoveResult r;
    r.position = center;
    // Pasos pequenos: nunca atraviesa un bloque aunque vaya muy rapido.
    const float longest = std::max({std::abs(delta.x), std::abs(delta.y), std::abs(delta.z)});
    const int steps = std::max(1, static_cast<int>(std::ceil(longest / 0.45f)));
    const Vec3 step = delta * (1.0f / static_cast<float>(steps));
    for (int i = 0; i < steps; ++i) {
        for (int axis = 0; axis < 3; ++axis) {
            const float amount = axis == 0 ? step.x : (axis == 1 ? step.y : step.z);
            if (amount == 0.0f) continue;
            Vec3 moved = r.position;
            (axis == 0 ? moved.x : (axis == 1 ? moved.y : moved.z)) += amount;
            if (!boxCollides(moved, half)) {
                r.position = moved;
                continue;
            }
            // Pegarse a la cara del bloque.
            float& coord = axis == 0 ? r.position.x : (axis == 1 ? r.position.y : r.position.z);
            const float extent = axis == 0 ? half.x : (axis == 1 ? half.y : half.z);
            if (amount > 0) {
                coord = std::floor(coord + extent + amount) - extent - 1e-3f;
            } else {
                coord = std::floor(coord - extent + amount) + 1.0f + extent + 1e-3f;
            }
            if (boxCollides(r.position, half)) {
                // No cabe pegado: se queda donde estaba en este eje.
                coord = axis == 0 ? (moved.x - amount) : (axis == 1 ? (moved.y - amount) : (moved.z - amount));
            }
            if (axis == 1) {
                if (amount < 0) r.on_ground = true;
                else r.hit_ceiling = true;
            } else {
                r.hit_wall = true;
            }
        }
    }
    // Tambien en el suelo si esta justo encima (sin moverse en y).
    if (!r.on_ground) {
        Vec3 below = r.position;
        below.y -= 0.02f;
        r.on_ground = boxCollides(below, half);
    }
    return r;
}

core::Vec3 VoxelSystem::blockColor(BlockId id) const {
    static std::array<std::optional<Vec3>, block::Count> cache;
    if (id >= block::Count) return Vec3{1.0f, 1.0f, 1.0f};
    if (cache[id]) return *cache[id];
    const BlockDef& def = blockDef(id);
    const std::string& name = def.side.empty() ? def.top : def.side;
    gfx::VoxelTextureLayer layer;
    const Impl& d = *impl_;
    const std::filesystem::path folder = d.settings.textures.empty() ? std::filesystem::path{} : d.assets_root / fromUtf8(d.settings.textures);
    buildTexture(name, 16, folder, layer);
    double r = 0.0, g = 0.0, b = 0.0, n = 0.0;
    for (std::size_t i = 0; i + 3 < layer.albedo.size(); i += 4) {
        if (layer.albedo[i + 3] < 128) continue;  // huecos (hojas, cristal)
        r += layer.albedo[i];
        g += layer.albedo[i + 1];
        b += layer.albedo[i + 2];
        n += 1.0;
    }
    Vec3 c = n > 0.0 ? Vec3{static_cast<float>(r / n / 255.0), static_cast<float>(g / n / 255.0), static_cast<float>(b / n / 255.0)}
                     : Vec3{0.8f, 0.8f, 0.8f};
    // Hierba y hojas: el tinte verde que les da el bioma.
    if (def.tint != BlockTint::None) c = Vec3{c.x * 0.55f, c.y * 0.85f, c.z * 0.45f};
    cache[id] = c;
    return c;
}

VoxelStats VoxelSystem::stats() const {
    const Impl& d = *impl_;
    VoxelStats s;
    s.chunks = static_cast<int>(d.chunks.size());
    s.pending_generation = static_cast<int>(d.generating.size());
    for (const auto& [key, c] : d.chunks) {
        // Fuera del radio de dibujo solo hay datos (no se mallan).
        const bool drawn = d.inRenderRadius(c->data.cx, c->data.cz);
        for (int i = 0; i < kSectionCount; ++i) {
            if (drawn && (c->dirty[static_cast<std::size_t>(i)] || c->in_flight[static_cast<std::size_t>(i)] != 0)) ++s.pending_meshes;
            if (c->uploaded[static_cast<std::size_t>(i)]) ++s.sections;
        }
    }
    const int r = d.collisionRadius();
    for (const auto& [key, c] : d.chunks) {
        const int dx = c->data.cx - d.viewer_cx, dz = c->data.cz - d.viewer_cz;
        const bool close = d.physics != nullptr && dx * dx + dz * dz <= r * r;
        const int top_section = std::min(c->top / kChunkSize, kSectionCount - 1);
        for (int i = 0; i < kSectionCount; ++i) {
            if (c->has_collider[static_cast<std::size_t>(i)]) ++s.colliders;
            if (close && i <= top_section && !c->collision_built[static_cast<std::size_t>(i)]) ++s.pending_colliders;
        }
    }
    s.last_chunk_ms = d.last_chunk_ms;
    s.last_mesh_ms = d.last_mesh_ms;
    return s;
}

}  // namespace cramion::voxel
