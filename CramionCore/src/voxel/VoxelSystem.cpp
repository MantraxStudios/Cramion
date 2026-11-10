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

#include "CramionCore/jobs/JobSystem.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/water/Water.h"
#include "VoxelInternal.h"
#include "FarTerrain.h"
#include "VoxelDimensions.h"
#include "Structures.h"
#include "BlockLights.h"
#include "Rails.h"

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

// Chunk guardado: "CRVX" + version + tramos. IDS16: version 2 = (cuantos u16, bloque u16), little endian; la
// version 1 (la de antes, bloque u8) se sigue leyendo: los mundos viejos cargan igual. Siempre se escribe la 2.
constexpr std::uint8_t kChunkFileVersion = 2;

// Tramos (cuantos u16, bloque u16) de todas las celdas de la columna.
void encodeRuns(const ChunkData& chunk, std::vector<std::uint8_t>& bytes) {
    // Siempre las kChunkCells celdas (lo recortado de arriba es aire).
    const std::size_t n = static_cast<std::size_t>(kChunkCells);
    std::size_t i = 0;
    while (i < n) {
        const BlockId id = chunk.at(i);
        std::size_t run = 1;
        while (i + run < n && chunk.at(i + run) == id && run < 65535) ++run;
        bytes.push_back(static_cast<std::uint8_t>(run & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>(run >> 8));
        bytes.push_back(static_cast<std::uint8_t>(id & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>(id >> 8));
        i += run;
    }
}
// Lee tramos desde `first`: `id_bytes` 1 (version 1) o 2 (version 2). Los ids sin bloque (>= Count) pasan a aire.
bool decodeRuns(const std::vector<std::uint8_t>& bytes, std::size_t first, int id_bytes, ChunkData& chunk) {
    const std::size_t step = 2 + static_cast<std::size_t>(id_bytes);
    std::size_t out = 0;
    for (std::size_t i = first; i + step <= bytes.size() && out < chunk.blocks.size(); i += step) {
        const std::size_t run = static_cast<std::size_t>(bytes[i]) | (static_cast<std::size_t>(bytes[i + 1]) << 8);
        unsigned raw = bytes[i + 2];
        if (id_bytes == 2) raw |= static_cast<unsigned>(bytes[i + 3]) << 8;
        const BlockId id = raw < block::Count ? static_cast<BlockId>(raw) : block::Air;
        for (std::size_t k = 0; k < run && out < chunk.blocks.size(); ++k) chunk.blocks[out++] = id;
    }
    return out == chunk.blocks.size();
}

bool writeChunkFile(const std::filesystem::path& file, const ChunkData& chunk) {
    std::vector<std::uint8_t> bytes = {'C', 'R', 'V', 'X', kChunkFileVersion};
    encodeRuns(chunk, bytes);
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
    if (bytes[4] == 1) return decodeRuns(bytes, 5, 1, chunk);  // 8 bits (mundos de antes de IDS16)
    if (bytes[4] == 2) return decodeRuns(bytes, 5, 2, chunk);
    return false;  // version futura: se queda lo generado
}

// Lo mismo en memoria (dimensiones de un mundo sin nombre: lo construido en
// cada una se conserva al ir y volver, aunque no se guarde en disco).
// (solo en memoria de este proceso: siempre tramos de 16 bits)
std::vector<std::uint8_t> encodeChunk(const ChunkData& chunk) {
    std::vector<std::uint8_t> bytes;
    encodeRuns(chunk, bytes);
    return bytes;
}
bool decodeChunk(const std::vector<std::uint8_t>& bytes, ChunkData& chunk) { return decodeRuns(bytes, 0, 2, chunk); }

}  // namespace

// ================================================================================

struct VoxelSystem::Impl {
    // Origen flotante: los bloques y los chunks van en coordenadas absolutas
    // (enteras, las de los archivos de guardado); la API publica y lo que se
    // manda al renderizador y a la fisica, en las locales del mundo. Los
    // desplazamientos son multiplos de 1024: la conversion es exacta.
    int ox = 0, oy = 0, oz = 0;
    // --- Estado ---
    bool active = false;
    VoxelWorld settings;
    std::array<float, 2> scene_trees{1.0f, 0.0f};  // tree_density y clearings del componente (newWorld)
    int scene_caves = 1;                           // cave_entrances del componente (newWorld)
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
    // Senales de arcanita (Rails.h): solo los circuitos que cambian, a 10 Hz. Lo que la simulacion cambia
    // (versiones encendidas) no la vuelve a despertar (signal_applying).
    rail::SignalSim signals;
    bool signal_applying = false;
    rail::SignalSim::World signal_world;
    const rail::SignalSim::World& signalWorld() {
        if (!signal_world.get) {
            signal_world.get = [this](int x, int y, int z) { return getBlock(x, y, z); };
            signal_world.set = [this](int x, int y, int z, BlockId id) {
                signal_applying = true;
                setBlockInternal(x, y, z, id, false);
                signal_applying = false;
            };
        }
        return signal_world;
    }

    // --- Dimensiones (setDimension) ---
    std::string dimension = kSurfaceDimension;
    // Mundo sin nombre: chunks cambiados de cada dimension (encodeChunk).
    std::unordered_map<std::string, std::unordered_map<std::int64_t, std::shared_ptr<const std::vector<std::uint8_t>>>> memory_chunks;
    ecs::World* scene = nullptr;  // la de start(): su ambiente cambia con la dimension
    entt::entity world_entity = entt::null;  // la del VoxelWorld (distancias en vivo, update)
    bool ambience = true;
    AmbienceSnapshot ambience_snapshot;
    VoxelGenerator profile() const { return dimensionGenerator(dimension, settings.generator); }
    void applyAmbience() {
        if (scene != nullptr && (ambience || ambience_snapshot.taken)) {
            applyDimensionAmbience(*scene, ambience ? dimension : std::string(kSurfaceDimension), ambience_snapshot);
        }
    }
    // Vuelve a la superficie (mundo nuevo o cargado).
    void resetDimensions() {
        memory_chunks.clear();
        dimension = kSurfaceDimension;
    }

    struct Chunk {
        ChunkData data;
        std::array<bool, kSectionCount> dirty{};
        std::array<std::uint32_t, kSectionCount> version{};   // sube al ensuciarse
        std::array<std::uint32_t, kSectionCount> in_flight{};  // version que se esta mallando (0 = nada)
        std::array<bool, kSectionCount> meshed{};
        // MALLAS-DETALLE: nivel de adornos con el que se mallo cada seccion y lo que le cambia (MeshTierFlags).
        std::array<std::uint8_t, kSectionCount> mesh_tier{};
        std::array<std::uint8_t, kSectionCount> tier_flags{};
        std::array<bool, kSectionCount> uploaded{};             // hay algo en el renderizador
        std::array<bool, kSectionCount> water_uploaded{};       // hay agua en el renderizador
        bool ever_meshed = false;
        int top = kWorldHeight - 1;  // el bloque no-aire mas alto (encima: aire con cielo 15)
        std::array<bool, kSectionCount> collision_built{};  // la malla de colision esta al dia
        std::array<bool, kSectionCount> has_collider{};     // hay cuerpo en la fisica
        // Luces reales de los bloques (BlockLights.h): la cache se rehace si cambian.
        std::uint64_t light_serial = 0;  // unico por carga de la columna
        std::uint32_t light_edits = 0;   // sube al poner o quitar un bloque que da luz
    };
    std::uint64_t light_serials = 0;
    BlockLightCache block_lights;
    std::unordered_map<std::int64_t, std::unique_ptr<Chunk>> chunks;
    // La luz y las consultas piden muchas celdas seguidas del mismo chunk.
    mutable std::int64_t cached_key = 0;
    mutable Chunk* cached_chunk = nullptr;
    std::unordered_set<std::int64_t> generating;
    // Celdas ocultas solo en el dibujo (setBlockHidden: puertas que el juego anima), en coordenadas
    // absolutas. La copia para mallar (snapshot) las pone como aire; los datos no cambian.
    std::unordered_map<std::int64_t, std::array<int, 3>> hidden_cells;
    // Las vuelve a mostrar todas (cambio de origen: el juego vuelve a buscar sus puertas).
    void showHiddenCells() {
        for (const auto& [key, p] : hidden_cells) markDirty(p[0], p[1], p[2]);
        hidden_cells.clear();
    }
    static std::int64_t hiddenKey(int x, int y, int z) {
        return (static_cast<std::int64_t>(x & 0x3FFFFF) << 42) | (static_cast<std::int64_t>(z & 0x3FFFFF) << 20) |
               static_cast<std::int64_t>(y & 0xFFFFF);
    }

    // --- Hilos ---
    struct Job {
        int kind = 0;  // 0 generar, 1 mallar
        int cx = 0, cz = 0, sy = 0;
        std::uint32_t version = 0;
        std::uint64_t epoch = 0;
        std::shared_ptr<const Generator> generator;
        std::filesystem::path saved;       // generar: chunk guardado (si hay)
        std::shared_ptr<const std::vector<std::uint8_t>> saved_bytes;  // o guardado en memoria (encodeChunk)
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
        std::vector<std::uint32_t> water;  // caras del agua (pase del agua)
        float milliseconds = 0.0f;
        std::uint8_t mesh_tier = 2;   // MALLAS-DETALLE
        std::uint8_t tier_flags = 0;
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
        bool water = false;                   // malla del agua (pase del agua)
    };
    std::vector<SectionUpdate> section_updates;
    bool clear_renderer = true;
    std::future<std::vector<gfx::VoxelTextureLayer>> textures_future;
    bool textures_uploaded = false;
    std::string textures_signature;
    float time = 0.0f;
    // Personajes que apartan las plantas (noteFoliageInteractor): pies, radio
    // y cuando se vieron; syncRenderer los pasa al render y quita los viejos.
    struct FoliageNote {
        Vec3 feet;
        float radius = 0.4f;
        float time = 0.0f;
        std::uint64_t frame = 0;  // syncRenderer en que se apunto
    };
    std::vector<FoliageNote> foliage_notes;
    std::uint64_t foliage_frame = 0;
    std::mutex foliage_mutex;
    float last_chunk_ms = 0.0f;
    float last_mesh_ms = 0.0f;

    // --- Terreno lejano (FarTerrain.h; CVar voxel.far.Distance) ---
    FarTerrain far_terrain;
    // La paleta (color medio de cada bloque) la calcula la tarea de las texturas.
    std::shared_ptr<std::shared_ptr<FarPalette>> far_palette_slot;
    std::vector<FarTerrain::Update> far_updates;
    float far_hole = 0.0f;  // radio (bloques) de los chunks de verdad ya dibujados
    int far_hole_frame = 0;

    // Radio (bloques) alrededor del centro del chunk de quien mira donde todos
    // los chunks estan ya mallados: ahi el VoxelPass no dibuja lo lejano.
    float readyRadius() const {
        const int r = renderRadius();
        int first_missing = (r + 1) * (r + 1);  // d^2 del chunk sin mallar mas cercano
        for (int dz = -r; dz <= r; ++dz) {
            for (int dx = -r; dx <= r; ++dx) {
                const int d2 = dx * dx + dz * dz;
                if (d2 > r * r || d2 >= first_missing) continue;
                const Chunk* c = chunkAt(viewer_cx + dx, viewer_cz + dz);
                bool ready = c != nullptr && c->ever_meshed;
                for (int s = 0; s < kSectionCount && ready; ++s) ready = c->meshed[static_cast<std::size_t>(s)];
                if (!ready) first_missing = d2;
            }
        }
        // Chunks listos hasta k (k^2 < first_missing). Un punto a menos de
        // 16 k - 12 del centro cae en un chunk cuyo centro esta a <= 16 k.
        int k = static_cast<int>(std::floor(std::sqrt(static_cast<float>(first_missing))));
        if (k * k >= first_missing) --k;
        k = std::min(k, r);
        return k <= 0 ? 0.0f : static_cast<float>(k * kChunkSize - 12);
    }

    void updateFar() {
        if (clear_renderer) return;  // syncRenderer lo reinicia con el mundo nuevo
        if (far_terrain.generator() != generator.get()) far_terrain.reset(generator);
        if (++far_hole_frame >= 4) {
            far_hole_frame = 0;
            far_hole = readyRadius();
        }
        far_terrain.update(viewer, static_cast<float>(renderRadius() * kChunkSize), FarTerrain::currentSettings());
    }

    // Agua que corre (como Minecraft): celdas por revisar en el proximo paso
    // (cada 0.25 s), sin repetir.
    std::deque<BlockPos> water_flow;
    std::unordered_set<std::uint64_t> water_flow_set;
    float water_flow_timer = 0.0f;

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
                if (job.saved_bytes && decodeChunk(*job.saved_bytes, *r.chunk)) r.chunk->modified = true;
                r.chunk->trimAir();  // IDS16: sin el aire de arriba (VoxelInternal.h ChunkData)
            } else {
                r.mesh_tier = job.snapshot->mesh_tier;  // MALLAS-DETALLE
                meshSection(*job.snapshot, r.vertices, &r.water, &r.tier_flags);
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
        return c->data.at(static_cast<std::size_t>(cellIndex(floorMod(x, kChunkSize), y, floorMod(z, kChunkSize))));
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
                if (d.at(static_cast<std::size_t>(i + y * kChunkSize * kChunkSize)) != block::Air) {
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
        std::filesystem::path folder = worldFolder();
        if (folder.empty()) return {};
        // Cada dimension en su carpeta (la superficie, donde siempre).
        if (dimension != kSurfaceDimension) folder = folder / "dimensions" / fromUtf8(dimension);
        return folder / "chunks" / (std::to_string(cx) + "." + std::to_string(cz) + ".bin");
    }

    void insertChunk(std::unique_ptr<ChunkData> data) {
        auto chunk = std::make_unique<Chunk>();
        chunk->data = std::move(*data);
        chunk->light_serial = ++light_serials;
        for (int s = 0; s < kSectionCount; ++s) {
            chunk->dirty[static_cast<std::size_t>(s)] = true;
            chunk->version[static_cast<std::size_t>(s)] = 1;
        }
        const int cx = chunk->data.cx, cz = chunk->data.cz;
        Chunk& ref = *chunk;
        chunks[chunkKey(cx, cz)] = std::move(chunk);
        lightNewChunk(ref);
        signals.chunkLoaded(cx, cz, ref.data.blocks);  // sus circuitos se revisan (lo encendido se recalcula)
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
        if (world_name.empty() && active) {
            memory_chunks[dimension][chunkKey(chunk.data.cx, chunk.data.cz)] =
                std::make_shared<const std::vector<std::uint8_t>>(encodeChunk(chunk.data));
            return;
        }
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
            if (it->second->water_uploaded[static_cast<std::size_t>(s)]) {
                section_updates.push_back({sectionKey(it->second->data.cx, s, it->second->data.cz), Vec3{}, {}, true});
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
                        id = c->data.at(i);
                        light = c->data.light[i];
                        if (!hidden_cells.empty() && id != block::Air &&
                            hidden_cells.count(hiddenKey(x0 + x, wy, z0 + z)) != 0) {
                            id = block::Air;  // oculta solo en el dibujo
                        }
                    }
                    const std::size_t si = static_cast<std::size_t>(SectionSnapshot::index(x, y, z));
                    s->blocks[si] = id;
                    s->light[si] = light;
                    if (x >= 0 && y >= 0 && z >= 0 && x < kChunkSize && y < kChunkSize && z < kChunkSize && id != block::Air) {
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
                c->mesh_tier[s] = r.mesh_tier;  // MALLAS-DETALLE
                c->tier_flags[s] = r.tier_flags;
                c->uploaded[s] = !r.vertices.empty();
                const Vec3 origin{static_cast<float>(r.cx * kChunkSize), static_cast<float>(r.sy * kChunkSize),
                                  static_cast<float>(r.cz * kChunkSize)};
                section_updates.push_back({sectionKey(r.cx, r.sy, r.cz), origin, std::move(r.vertices)});
                if (!r.water.empty() || c->water_uploaded[s]) {
                    c->water_uploaded[s] = !r.water.empty();
                    section_updates.push_back({sectionKey(r.cx, r.sy, r.cz), origin, std::move(r.water), true});
                }
            }
        }
        if (!later.empty()) {
            std::lock_guard lock(mutex);
            for (Result& r : later) results.push_back(std::move(r));
        }
    }

    // MALLAS-DETALLE: distancia (m) de quien mira al punto mas cercano de la seccion y su nivel de adornos.
    float sectionDistance(int cx, int sy, int cz) const {
        const auto axis = [](float v, float lo) { return std::max({lo - v, 0.0f, v - (lo + static_cast<float>(kChunkSize))}); };
        const float dx = axis(viewer.x, static_cast<float>(cx * kChunkSize));
        const float dy = axis(viewer.y, static_cast<float>(sy * kChunkSize));
        const float dz = axis(viewer.z, static_cast<float>(cz * kChunkSize));
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    int last_detail_level = -1;
    std::array<int, 3> last_detail_section{-1000000, -1000000, -1000000};
    // Al cambiar de seccion quien mira (o la CVar voxel.mesh.Detail): las secciones malladas con otro nivel que
    // el que les toca se vuelven a mallar (la malla vieja sigue hasta que llega la nueva). Con 8 m de margen al
    // bajar de nivel, para no rehacerlas yendo y viniendo por el borde; solo si el nivel cambia algo en ellas.
    void refreshMeshTiers() {
        const int level = meshDetailLevel();
        const std::array<int, 3> here{viewer_cx, floorDiv(static_cast<int>(std::floor(viewer.y)), kChunkSize), viewer_cz};
        if (level == last_detail_level && here == last_detail_section) return;
        const bool level_changed = level != last_detail_level;
        last_detail_level = level;
        last_detail_section = here;
        for (const auto& [key, chunk] : chunks) {
            if (!inRenderRadius(chunk->data.cx, chunk->data.cz)) continue;
            for (int sy = 0; sy < kSectionCount; ++sy) {
                const std::size_t s = static_cast<std::size_t>(sy);
                if (!chunk->meshed[s] || chunk->dirty[s] || chunk->in_flight[s] != 0) continue;
                const int have = chunk->mesh_tier[s];
                const float d = sectionDistance(chunk->data.cx, sy, chunk->data.cz);
                const int want = meshTierFor(level, d);
                if (want == have) continue;
                // Menos detalle: solo pasado el margen (si el cambio de nivel no lo pide ya).
                if (want < have && !level_changed && meshTierFor(level, std::max(d - 8.0f, 0.0f)) >= have) continue;
                // Adornos: cambian entre cualquier par de niveles; la luz plana, solo con el nivel 0.
                const std::uint8_t flags = chunk->tier_flags[s];
                const bool decor = (flags & kTierDecor) != 0;
                const bool faces = (flags & kTierFaces) != 0 && std::min(want, have) == 0;
                if (!decor && !faces) continue;
                chunk->dirty[s] = true;
                ++chunk->version[s];
            }
        }
    }

    void stream(double budget_ms) {
        if (!active || !generator || !has_viewer) return;
        refreshMeshTiers();  // MALLAS-DETALLE
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
            } else if (const auto dim = memory_chunks.find(dimension); dim != memory_chunks.end()) {
                if (const auto it = dim->second.find(chunkKey(pos.first, pos.second)); it != dim->second.end()) job.saved_bytes = it->second;
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
                if (chunk->water_uploaded[s]) {
                    section_updates.push_back({sectionKey(c.cx, c.sy, c.cz), Vec3{}, {}, true});
                    chunk->water_uploaded[s] = false;
                }
                continue;
            }
            snap->mesh_tier = static_cast<std::uint8_t>(meshTierFor(meshDetailLevel(), sectionDistance(c.cx, c.sy, c.cz)));
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

    static std::uint64_t cellKey(int x, int y, int z) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x) & 0xFFFFFFu) << 40) |
               (static_cast<std::uint64_t>(static_cast<std::uint32_t>(z) & 0xFFFFFFu) << 16) | static_cast<std::uint64_t>(y & 0xFFFF);
    }
    void queueWater(int x, int y, int z) {
        if (y < 1 || y >= kWorldHeight) return;
        if (water_flow_set.insert(cellKey(x, y, z)).second) water_flow.push_back({x, y, z});
    }
    // Tras cambiar una celda: si hay agua en ella o al lado, se revisan ella y sus vecinas.
    void wakeWater(int x, int y, int z, BlockId before) {
        bool touches = block::isWater(before) || block::isWater(getBlock(x, y, z));
        for (int d = 0; d < 6 && !touches; ++d) touches = block::isWater(getBlock(x + kDirs[d][0], y + kDirs[d][1], z + kDirs[d][2]));
        if (!touches) return;
        queueWater(x, y, z);
        for (const auto& d : kDirs) queueWater(x + d[0], y + d[1], z + d[2]);
    }
    // El agua que tendria que haber en una celda (aire, planta o agua que corre):
    // la fuente no cambia; con agua encima, corriente 1 (cae); si no, la de la
    // vecina mas fuerte que pueda extenderse (sobre suelo) mas 1, hasta 7. Dos
    // fuentes al lado sobre suelo hacen una fuente (como Minecraft).
    void updateWaterCell(const BlockPos& p) {
        if (p.y < 1 || p.y >= kWorldHeight - 1 || chunkOf(p.x, p.z) == nullptr) return;
        const BlockId id = getBlock(p.x, p.y, p.z);
        const int level = block::waterLevel(id);
        if (level == 0 || block::isAquatic(id)) return;  // fuente, o planta que ya esta dentro del agua
        const BlockDef& def = blockDef(id);
        if (level < 0 && id != block::Air && !(def.shape == BlockShape::Cross && def.replaceable)) return;
        int target = 99;
        if (block::isWater(getBlock(p.x, p.y + 1, p.z))) target = 1;
        int sources = 0;
        static constexpr int kSides[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& sd : kSides) {
            const int nx = p.x + sd[0], nz = p.z + sd[1];
            if (chunkOf(nx, nz) == nullptr) continue;
            const int nl = block::waterLevel(getBlock(nx, p.y, nz));
            if (nl < 0) continue;
            if (nl == 0) ++sources;
            // Solo se extiende a los lados lo que no puede caer.
            const BlockId below = getBlock(nx, p.y - 1, nz);
            const bool spreads = blockDef(below).solid ||
                                 (block::waterLevel(below) == 0 && !block::isWater(getBlock(nx, p.y + 1, nz)));
            if (spreads) target = std::min(target, nl + 1);
        }
        const BlockId below = getBlock(p.x, p.y - 1, p.z);
        if (sources >= 2 && (blockDef(below).solid || block::waterLevel(below) == 0)) target = 0;
        const BlockId want = target == 0 ? block::Water : (target <= 7 ? block::flowingWater(target) : block::Air);
        if (level < 0 && want == block::Air) return;  // seco y sigue seco (la planta se queda)
        if (want != id) setBlockInternal(p.x, p.y, p.z, want, false);
    }
    void flowWater(float delta_seconds) {
        water_flow_timer += delta_seconds;
        if (water_flow_timer < 0.25f || water_flow.empty()) return;
        water_flow_timer = 0.0f;
        std::deque<BlockPos> now;
        now.swap(water_flow);
        water_flow_set.clear();
        int budget = 384;
        while (!now.empty()) {
            const BlockPos p = now.front();
            now.pop_front();
            if (budget-- <= 0) {
                queueWater(p.x, p.y, p.z);  // lo que no cabe, al siguiente paso
                continue;
            }
            updateWaterCell(p);
        }
    }

    bool setBlockInternal(int x, int y, int z, BlockId id, bool by_player) {
        if (y < 0 || y >= kWorldHeight) return false;
        Chunk* c = chunkOf(x, z);
        if (c == nullptr) return false;
        const std::size_t cell = static_cast<std::size_t>(cellIndex(floorMod(x, kChunkSize), y, floorMod(z, kChunkSize)));
        const BlockId before = c->data.at(cell);
        if (before == id) return true;
        (void)by_player;
        if (blockDef(before).light > 0 || blockDef(id).light > 0) ++c->light_edits;  // BlockLights.h
        c->data.set(cell, id);  // IDS16: crece si estaba recortado
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
        if (listener) listener(BlockPos{x - ox, y - oy, z - oz}, before, id);
        if (!signal_applying) signals.blockChanged(signalWorld(), x, y, z, before, id);
        // El agua de alrededor fluye si se abrio un hueco (o se quito su fuente).
        wakeWater(x, y, z, before);
        return true;
    }

    // ------------------------------------------------------------ agua (aspecto)
    // El agua de bloques se dibuja en el pase del agua como un cuerpo mas
    // (tipo 3). Su aspecto: el de un WaterBody hijo del VoxelWorld si lo hay
    // (aunque este desactivado: asi se ajusta sin dibujar el plano), si no uno
    // azul claro como el de Minecraft. Y si la camara esta en un bloque de agua
    // (niebla, causticas y rayos bajo el agua), con la altura de su superficie.
    void syncWaterLook(gfx::VulkanRenderer& renderer) const {
        gfx::GpuWaterBody p{};
        water::WaterBody look;
        look.shallow_color = Vec3{0.10f, 0.45f, 0.52f};
        look.deep_color = Vec3{0.012f, 0.07f, 0.13f};
        look.clarity = 3.0f;
        look.foam = 0.35f;
        look.shore_foam = 0.3f;
        look.caustics = 0.7f;
        look.refraction = 0.3f;
        look.roughness = 0.05f;
        look.wave_height = 0.05f;
        look.wavelength = 4.0f;
        look.steepness = 0.3f;
        look.scattering = 1.0f;
        look.detail = 1.0f;
        if (scene != nullptr) {
            for (const entt::entity h : scene->registry().view<water::WaterBody>()) {
                const ecs::Entity e = scene->wrap(h);
                const ecs::Entity parent = e.parent();
                if (parent.valid() && parent.has<VoxelWorld>()) {
                    look = e.get<water::WaterBody>();
                    break;
                }
            }
        }
        constexpr float kRad = 3.14159265f / 180.0f;
        p.origin = core::Vec4{0.0f, 0.0f, 0.0f, 0.0f};
        p.extent = core::Vec4{0.0f, 0.0f, 3.0f, 0.0f};  // 3 = agua de bloques
        p.shallow = core::Vec4{look.shallow_color.x, look.shallow_color.y, look.shallow_color.z, look.clarity};
        p.deep = core::Vec4{look.deep_color.x, look.deep_color.y, look.deep_color.z, std::min(look.foam, 0.6f)};
        // Olas pequenas: los bloques no se mueven (solo tiembla la superficie).
        p.waves = core::Vec4{std::min(look.wave_height, 0.06f), std::clamp(look.wavelength, 2.0f, 8.0f), look.wave_speed,
                             look.steepness};
        p.wind = core::Vec4{look.wind_direction * kRad, look.wind_spread * kRad, 1.2f, look.detail};
        p.look = core::Vec4{look.roughness, look.refraction, look.caustics, std::min(look.shore_foam, 0.4f)};
        p.under = core::Vec4{look.god_rays, look.particles, 0.0f, 0.0f};
        bool inside = false;
        float surface = 0.0f, sky = 1.0f;
        if (active && has_viewer) {
            const int x = static_cast<int>(std::floor(viewer.x)), z = static_cast<int>(std::floor(viewer.z));
            int y = static_cast<int>(std::floor(viewer.y));
            // AGUA-PLANTAS: entre algas, hierba marina y corales (isAquatic) tambien se esta bajo el agua: con
            // isWater, al meter la camara en una planta se iba la niebla/tinte del agua (water::setUnderwaterOverride
            // y Voxel.inWater ya usaban isWet).
            if (chunkOf(x, z) != nullptr && block::isWet(getBlock(x, y, z))) {
                inside = true;
                while (y + 1 < kWorldHeight && block::isWet(getBlock(x, y + 1, z))) ++y;
                // La luz del cielo sobre la superficie (el agua la va quitando hacia abajo:
                // la de la camara dejaria negro el fondo del mar).
                sky = static_cast<float>(std::max(channel(x, y + 1, z, true), channel(x, y, z, true))) / 15.0f;
                const BlockId up = getBlock(x, y + 1, z);
                const BlockId top_block = getBlock(x, y, z);  // AGUA-PLANTAS: una planta arriba = agua llena
                const int top = blockDef(up).opaque ? 16
                                                    : waterHeight16(block::waterLevel(block::isAquatic(top_block) ? block::Water : top_block));
                surface = static_cast<float>(y) + static_cast<float>(top) / 16.0f - static_cast<float>(oy);
            }
        }
        // extra: olas de playa, luz en las crestas, superficie en la camara, luz del cielo en la camara.
        p.extra = core::Vec4{0.0f, look.scattering, surface, sky};
        renderer.setVoxelWater(p, active, inside);
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
        // COLISION-MODELOS: las vallas, sillas, mesas... van aparte con sus cajas (collisionTriangles).
        return def.solid && def.shape == BlockShape::Cube && !hasModelCollision(def);
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
        // COLISION-MODELOS: los bloques con modelo propio chocan con sus piezas (cajas cerradas).
        const auto at = [this](int bx, int by, int bz) { return getBlock(bx, by, bz); };
        ModelBoxes boxes{};
        for (int ly = 0; ly < kChunkSize; ++ly) {
            for (int lz = 0; lz < kChunkSize; ++lz) {
                for (int lx = 0; lx < kChunkSize; ++lx) {
                    const int wx = base[0] + lx, wy = base[1] + ly, wz = base[2] + lz;
                    const BlockId id = getBlock(wx, wy, wz);
                    if (!hasModelCollision(blockDef(id))) continue;
                    const int count = modelCollisionBoxes(id, at, wx, wy, wz, boxes);
                    for (int i = 0; i < count; ++i) {
                        const ModelBox& mb = boxes[static_cast<std::size_t>(i)];
                        const float lo[3] = {lx + mb.lo[0] / 16.0f, ly + mb.lo[1] / 16.0f, lz + mb.lo[2] / 16.0f};
                        const float hi[3] = {lx + mb.hi[0] / 16.0f, ly + mb.hi[1] / 16.0f, lz + mb.hi[2] / 16.0f};
                        for (int d = 0; d < 3; ++d) {
                            const int u = (d + 1) % 3, v = (d + 2) % 3;
                            for (const int sign : {1, -1}) {
                                const auto corner = [&](float a, float b) {
                                    float q[3];
                                    q[d] = sign > 0 ? hi[d] : lo[d];
                                    q[u] = a;
                                    q[v] = b;
                                    return Vec3{q[0], q[1], q[2]};
                                };
                                const Vec3 q0 = corner(lo[u], lo[v]), q1 = corner(hi[u], lo[v]), q2 = corner(hi[u], hi[v]),
                                           q3 = corner(lo[u], hi[v]);
                                if (sign > 0) {
                                    out.insert(out.end(), {q0, q1, q2, q0, q2, q3});
                                } else {
                                    out.insert(out.end(), {q0, q2, q1, q0, q3, q2});
                                }
                            }
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
                                       Vec3{static_cast<float>(chunk.data.cx * kChunkSize - ox),
                                            static_cast<float>(c.sy * kChunkSize - oy),
                                            static_cast<float>(chunk.data.cz * kChunkSize - oz)},
                                       triangles);
                chunk.has_collider[s] = true;
            }
            chunk.collision_built[s] = true;
        }
    }

    // ------------------------------------------------------------ mundos
    void resetWorld(int new_seed) {
        ++epoch;
        hidden_cells.clear();
        {
            std::lock_guard lock(mutex);
            jobs.clear();
        }
        if (physics != nullptr) {
            for (auto& [key, chunk] : chunks) removeColliders(*chunk);
        }
        forgetCachedChunk();
        chunks.clear();
        signals.clear();
        generating.clear();
        section_updates.clear();
        water_flow.clear();
        water_flow_set.clear();
        clear_renderer = true;
        seed = new_seed;
        auto made = std::make_shared<Generator>(seed, static_cast<int>(std::lround(settings.sea_level)),
                                                profile());  // el de la dimension actual
        made->setVegetation(settings.tree_density, settings.clearings);
        made->setCaveEntrances(settings.cave_entrances);
        if (const int layer = deepLayerOf(dimension); layer > 0) made->setDeepLayer(layer);  // capas del Reino Profundo
        generator = std::move(made);
    }

    void writeWorldInfo() const {
        const std::filesystem::path folder = worldFolder();
        if (folder.empty()) return;
        nlohmann::json json;
        json["name"] = world_name;
        json["seed"] = seed;
        json["sea_level"] = settings.sea_level;
        // El mismo terreno al volver: con que generador se creo.
        json["generator"] = static_cast<int>(settings.generator);
        json["tree_density"] = settings.tree_density;
        json["clearings"] = settings.clearings;
        json["cave_entrances"] = settings.cave_entrances;
        json["dimension"] = dimension;  // donde estaba el jugador
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
        d.world_entity = h;
        d.scene_trees = {d.settings.tree_density, d.settings.clearings};
        d.scene_caves = d.settings.cave_entrances;
        d.active = true;
        break;
    }
    if (!d.active) return;
    // La escena puede venir ya desplazada (se guardo lejos del origen).
    d.ox = static_cast<int>(std::llround(world.origin().x));
    d.oy = static_cast<int>(std::llround(world.origin().y));
    d.oz = static_cast<int>(std::llround(world.origin().z));
    d.world_name.clear();
    d.meta_values.clear();
    d.scene = &world;
    d.resetDimensions();
    d.resetWorld(d.settings.seed);
    // Bajo el agua solo si la camara esta en un bloque de agua de verdad.
    Impl* impl = &d;
    // En el bloque de la superficie el agua no llena la celda (14/16, menos si corre): se compara con la altura
    // real de la superficie, con histeresis (+-6 cm) para que el efecto no parpadee al moverse junto a ella.
    water::setUnderwaterOverride([impl, dentro = 0](const Vec3& camera) mutable {
        if (!impl->active) return -1;
        const int x = static_cast<int>(std::floor(camera.x)) + impl->ox, y = static_cast<int>(std::floor(camera.y)) + impl->oy,
                  z = static_cast<int>(std::floor(camera.z)) + impl->oz;
        if (impl->chunkOf(x, z) == nullptr) return -1;
        const BlockId b = impl->getBlock(x, y, z);
        float superficie = -1.0f;  // altura (en bloques) del agua dentro de esta celda; <0 = seca
        if (block::isWet(b)) {     // tambien entre las algas
            const BlockId arriba = impl->getBlock(x, y + 1, z);
            if (block::isWet(arriba) || blockDef(arriba).opaque) {
                superficie = 1.0f;
            } else {
                const int nivel = block::waterLevel(block::isAquatic(b) ? block::Water : b);
                superficie = static_cast<float>(waterHeight16(nivel < 0 ? 0 : nivel)) / 16.0f;
            }
        }
        const float fy = camera.y + static_cast<float>(impl->oy) - static_cast<float>(y);  // 0..1 dentro de la celda
        constexpr float kMargen = 0.06f;
        if (superficie < 0.0f) {
            dentro = 0;
        } else if (superficie >= 1.0f) {
            dentro = 1;
        } else {
            dentro = fy < superficie + (dentro ? kMargen : -kMargen) ? 1 : 0;
        }
        return dentro;
    });
}

void VoxelSystem::stop() {
    Impl& d = *impl_;
    if (d.active && !d.world_name.empty()) saveWorld();
    if (d.active) water::setUnderwaterOverride({});
    d.active = false;
    d.world_name.clear();
    // La escena (y su ambiente) es de quien la cree: no se toca al parar.
    d.scene = nullptr;
    d.world_entity = entt::null;
    d.ambience_snapshot = AmbienceSnapshot{};
    d.resetDimensions();
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
    d.viewer = viewer + Vec3{static_cast<float>(d.ox), static_cast<float>(d.oy), static_cast<float>(d.oz)};
    d.viewer_cx = floorDiv(static_cast<int>(std::floor(viewer.x)) + d.ox, kChunkSize);
    d.viewer_cz = floorDiv(static_cast<int>(std::floor(viewer.z)) + d.oz, kChunkSize);
    d.has_viewer = true;
    // Distancia de dibujado y de colision en vivo (un script la baja en el
    // movil o desde las opciones): antes solo se leia al empezar.
    if (d.scene != nullptr && d.scene->registry().valid(d.world_entity)) {
        if (const VoxelWorld* w = d.scene->registry().try_get<VoxelWorld>(d.world_entity)) {
            d.settings.render_distance = w->render_distance;
            d.settings.collision_distance = w->collision_distance;
        }
    }
    d.collect(6.0);
    d.flowWater(delta_seconds);
    d.signals.update(delta_seconds, d.signalWorld());
    d.stream(3.0);
    d.updateCollision(2.0);
    d.updateFar();
}

void VoxelSystem::syncRenderer(gfx::VulkanRenderer& renderer) {
    Impl& d = *impl_;
    if (d.clear_renderer) {
        renderer.clearVoxelSections();
        d.clear_renderer = false;
        // Lo lejano tambien se ha ido: se rehace (del disco, rapido) con el mundo de ahora.
        d.far_terrain.reset(d.active ? d.generator : nullptr);
        d.far_updates.clear();
    }
    if (!d.active) {
        d.section_updates.clear();
        renderer.setVoxelWater(gfx::GpuWaterBody{}, false, false);
        renderer.setVoxelInteractors({});
        return;
    }
    // Texturas: se generan en otro hilo la primera vez (o si cambia la carpeta).
    const std::filesystem::path folder = d.settings.textures.empty() ? std::filesystem::path{} : d.assets_root / fromUtf8(d.settings.textures);
    const std::uint32_t size = std::bit_ceil(static_cast<std::uint32_t>(std::clamp(d.settings.texture_size, 16, 1024)));
    const bool flat = d.settings.flat_colors;
    // PAQUETES-TEXTURAS: el paquete del jugador (encima de la carpeta del juego); cambiarlo o recargar rehace todo.
    const std::filesystem::path pack = texturePackFolder();
    const std::string signature = folder.string() + "|" + std::to_string(size) + (flat ? "|liso" : "") + "|" +
                                  pack.string() + "|" + std::to_string(texturePackGeneration());
    renderer.setVoxelFlatColors(flat);
    if (signature != d.textures_signature && !d.textures_future.valid()) {
        d.textures_signature = signature;
        // En el sistema de jobs (antes un std::async por textura: un hilo del
        // sistema nuevo cada una). La promesa lleva el resultado al futuro.
        auto promise = std::make_shared<std::promise<std::vector<gfx::VoxelTextureLayer>>>();
        d.textures_future = promise->get_future();
        auto palette_slot = std::make_shared<std::shared_ptr<FarPalette>>();
        d.far_palette_slot = palette_slot;
        jobs::schedule(
            [promise, palette_slot, folder, pack, size, flat]() {
                try {
                    const auto& names = textureNames();
                    std::vector<gfx::VoxelTextureLayer> layers(names.size());
                    // Una textura por trozo; quien espera ayuda (parallelFor).
                    jobs::parallelFor(names.size(), 1, [&](std::size_t begin, std::size_t end) {
                        for (std::size_t i = begin; i < end; ++i) buildTexture(names[i], size, folder, pack, layers[i], flat);
                    });
                    // Color medio de cada bloque para el terreno lejano (antes del
                    // set_value: el futuro listo ya lo deja escrito).
                    *palette_slot = buildFarPalette(layers);
                    promise->set_value(std::move(layers));
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            },
            nullptr, jobs::Priority::Background);
        d.textures_uploaded = false;
    }
    if (d.textures_future.valid() && d.textures_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        std::vector<gfx::VoxelTextureLayer> layers;
        try {
            layers = d.textures_future.get();
        } catch (const std::exception& e) {
            std::cerr << "[Voxel] No se pudieron generar las texturas: " << e.what() << "\n";
        }
        if (!layers.empty()) renderer.setVoxelTextures(size, layers);
        d.textures_uploaded = true;
        if (d.far_palette_slot && *d.far_palette_slot) d.far_terrain.setPalette(*d.far_palette_slot);
        d.far_palette_slot.reset();
    }
    for (Impl::SectionUpdate& u : d.section_updates) {
        if (u.vertices.empty()) {
            if (u.water) renderer.removeVoxelWaterSection(u.key);
            else renderer.removeVoxelSection(u.key);
        } else {
            // Del chunk (absoluto) al mundo local.
            const Vec3 origin = u.origin - Vec3{static_cast<float>(d.ox), static_cast<float>(d.oy), static_cast<float>(d.oz)};
            const auto count = static_cast<std::uint32_t>(u.vertices.size() / 2);
            if (u.water) renderer.setVoxelWaterSection(u.key, origin, u.vertices.data(), count);
            else renderer.setVoxelSection(u.key, origin, u.vertices.data(), count);
        }
    }
    d.section_updates.clear();
    // Terreno lejano: lo quitado y, con un presupuesto por frame, las mallas nuevas.
    d.far_terrain.takeUpdates(d.far_updates, 1536u * 1024u);
    const Vec3 offset{static_cast<float>(d.ox), static_cast<float>(d.oy), static_cast<float>(d.oz)};
    for (FarTerrain::Update& u : d.far_updates) {
        if (u.vertices.empty()) {
            renderer.removeVoxelFarSection(u.key);
        } else {
            renderer.setVoxelFarSection(u.key, u.origin - offset, u.scale, u.vertices.data(),
                                        static_cast<std::uint32_t>(u.vertices.size() / 2));
        }
    }
    d.far_updates.clear();
    const Vec3 hole{static_cast<float>(d.viewer_cx * kChunkSize + kChunkSize / 2), 0.0f,
                    static_cast<float>(d.viewer_cz * kChunkSize + kChunkSize / 2)};
    renderer.setVoxelFarHole(hole - offset, d.has_viewer ? d.far_hole : 0.0f, FarTerrain::currentSettings().distance);
    renderer.setVoxelTime(d.time);
    // Lo que aparta las plantas: lo apuntado hace menos de un momento (los que
    // se mueven a saltos, como las criaturas lejanas que esperan turno para
    // moverse, no parpadean). El render los junta y los suaviza (VoxelPass).
    {
        std::lock_guard<std::mutex> lock(d.foliage_mutex);
        const float now = d.time;
        std::erase_if(d.foliage_notes, [now](const Impl::FoliageNote& n) { return now - n.time > 0.25f; });
        std::vector<core::Vec4> spheres;
        spheres.reserve(d.foliage_notes.size());
        for (const Impl::FoliageNote& n : d.foliage_notes) spheres.push_back(core::Vec4{n.feet.x, n.feet.y, n.feet.z, n.radius});
        renderer.setVoxelInteractors(spheres);
        ++d.foliage_frame;
    }
    d.syncWaterLook(renderer);
}

void VoxelSystem::noteFoliageInteractor(const Vec3& feet, float radius) {
    Impl& d = *impl_;
    if (!d.active || !std::isfinite(feet.x) || !std::isfinite(feet.y) || !std::isfinite(feet.z)) return;
    radius = std::clamp(std::isfinite(radius) ? radius : 0.4f, 0.1f, 3.0f);
    std::lock_guard<std::mutex> lock(d.foliage_mutex);
    // El mismo de antes si estaba cerca y aun no se apunto en este frame
    // (si no, es otro: dos criaturas juntas no se pisan).
    for (Impl::FoliageNote& n : d.foliage_notes) {
        if (n.frame == d.foliage_frame) continue;
        const float dx = n.feet.x - feet.x, dz = n.feet.z - feet.z;
        if (dx * dx + dz * dz < 1.0f && std::abs(n.feet.y - feet.y) < 1.5f) {
            n = Impl::FoliageNote{feet, radius, d.time, d.foliage_frame};
            return;
        }
    }
    if (d.foliage_notes.size() >= 128) return;  // (de sobra: el render se queda con los 16 mas cercanos)
    d.foliage_notes.push_back(Impl::FoliageNote{feet, radius, d.time, d.foliage_frame});
}

void VoxelSystem::appendBlockLights(const Vec3& eye, std::vector<scene::PointLight>& out) {
    Impl& d = *impl_;
    if (!d.active) return;
    const Vec3 origin{static_cast<float>(d.ox), static_cast<float>(d.oy), static_cast<float>(d.oz)};
    d.block_lights.collect(eye + origin, origin,
                           [&d](int cx, int cz, BlockLightCache::Column& column) {
                               const Impl::Chunk* c = d.chunkAt(cx, cz);
                               if (c == nullptr) return false;
                               column.data = &c->data;
                               column.serial = c->light_serial;
                               column.edits = c->light_edits;
                               column.top = c->top;
                               return true;
                           },
                           d.time, out);
}

void VoxelSystem::waitUntilReady(const Vec3& center, int radius_chunks, float timeout_seconds) {
    Impl& d = *impl_;
    if (!d.active) return;
    const auto start = Clock::now();
    const int cx = floorDiv(static_cast<int>(std::floor(center.x)) + d.ox, kChunkSize);
    const int cz = floorDiv(static_cast<int>(std::floor(center.z)) + d.oz, kChunkSize);
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
    // Un mundo nuevo empieza sin nada guardado, con los arboles del componente
    // (loadWorld pudo poner los de otro mundo).
    d.settings.tree_density = d.scene_trees[0];
    d.settings.clearings = d.scene_trees[1];
    d.settings.cave_entrances = d.scene_caves;
    std::error_code error;
    std::filesystem::remove_all(d.worldFolder(), error);
    d.resetDimensions();  // empieza en la superficie
    d.applyAmbience();
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
    // Los mundos de antes no lo guardaban: el Clasico.
    d.settings.generator = static_cast<VoxelGenerator>(std::clamp(json.value("generator", 0), 0, 1));
    // Los arboles con los que se creo (los de antes no lo guardaban: los del componente).
    d.settings.tree_density = json.value("tree_density", d.settings.tree_density);
    d.settings.clearings = json.value("clearings", d.settings.clearings);
    // Entradas de cueva (los de antes no lo guardaban: las del componente, y las zonas
    // sin visitar ya las tienen).
    d.settings.cave_entrances = std::clamp(json.value("cave_entrances", d.scene_caves), 0, 2);
    // La dimension en la que estaba el jugador (los de antes: la superficie).
    d.resetDimensions();
    if (const std::string dim = json.value("dimension", std::string(kSurfaceDimension)); findDimension(dim) != nullptr) d.dimension = dim;
    d.applyAmbience();
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

// La API publica va en coordenadas locales (las del mundo desplazado).
BlockId VoxelSystem::getBlock(int x, int y, int z) const {
    const Impl& d = *impl_;
    return d.getBlock(x + d.ox, y + d.oy, z + d.oz);
}
bool VoxelSystem::setBlock(int x, int y, int z, BlockId id) {
    if (id >= block::Count) return false;
    Impl& d = *impl_;
    return d.setBlockInternal(x + d.ox, y + d.oy, z + d.oz, id, true);
}
void VoxelSystem::setBlockHidden(int x, int y, int z, bool hidden) {
    Impl& d = *impl_;
    const int ax = x + d.ox, ay = y + d.oy, az = z + d.oz;
    if (ay < 0 || ay >= kWorldHeight) return;
    const std::int64_t key = Impl::hiddenKey(ax, ay, az);
    const bool changed = hidden ? d.hidden_cells.emplace(key, std::array<int, 3>{ax, ay, az}).second
                                : d.hidden_cells.erase(key) != 0;
    if (changed) d.markDirty(ax, ay, az);  // solo se vuelve a mallar (sin luz ni colision)
}
void VoxelSystem::showHiddenBlocks() { impl_->showHiddenCells(); }
bool VoxelSystem::railAt(int x, int y, int z, int& mask, int& up) const {
    const Impl& d = *impl_;
    const auto at = [&](int bx, int by, int bz) { return d.getBlock(bx, by, bz); };
    const int ax = x + d.ox, ay = y + d.oy, az = z + d.oz;
    if (!block::isRail(at(ax, ay, az))) return false;
    const rail::RailShape r = rail::railShape(at, ax, ay, az);
    mask = r.mask;
    up = r.up;
    return true;
}
int VoxelSystem::signalPower(int x, int y, int z) const {
    const Impl& d = *impl_;
    return d.signals.power(x + d.ox, y + d.oy, z + d.oz);
}
void VoxelSystem::setSignalInput(int x, int y, int z, bool on) {
    Impl& d = *impl_;
    d.signals.setInput(x + d.ox, y + d.oy, z + d.oz, on);
}
std::vector<BlockPos> VoxelSystem::findBlocks(const Vec3& center, int radius, BlockId id, int max) const {
    const Impl& d = *impl_;
    std::vector<BlockPos> out;
    radius = std::clamp(radius, 0, 48);
    const int cx = static_cast<int>(std::floor(center.x)) + d.ox, cy = static_cast<int>(std::floor(center.y)) + d.oy;
    const int cz = static_cast<int>(std::floor(center.z)) + d.oz;
    for (int y = std::max(0, cy - radius); y <= std::min(kWorldHeight - 1, cy + radius); ++y)
        for (int z = cz - radius; z <= cz + radius; ++z)
            for (int x = cx - radius; x <= cx + radius; ++x) {
                if (d.getBlock(x, y, z) != id) continue;
                out.push_back(BlockPos{x - d.ox, y - d.oy, z - d.oz});
                if (static_cast<int>(out.size()) >= max) return out;
            }
    return out;
}
// VFX-VIVO: como findBlocks pero con varios tipos a la vez; la columna (x, z) busca su chunk una vez.
std::vector<VoxelSystem::FoundBlock> VoxelSystem::findBlocksAny(const Vec3& center, int radius, const std::vector<bool>& wanted,
                                                              int maxPerId, int max) const {
    const Impl& d = *impl_;
    std::vector<FoundBlock> out;
    radius = std::clamp(radius, 0, 48);
    const int cx = static_cast<int>(std::floor(center.x)) + d.ox, cy = static_cast<int>(std::floor(center.y)) + d.oy;
    const int cz = static_cast<int>(std::floor(center.z)) + d.oz;
    const int y0 = std::max(0, cy - radius), y1 = std::min(kWorldHeight - 1, cy + radius);
    std::vector<int> per(wanted.size(), 0);
    for (int z = cz - radius; z <= cz + radius; ++z)
        for (int x = cx - radius; x <= cx + radius; ++x) {
            const Impl::Chunk* c = d.chunkOf(x, z);
            if (c == nullptr) continue;
            const int lx = floorMod(x, kChunkSize), lz = floorMod(z, kChunkSize);
            for (int y = y0; y <= y1; ++y) {
                const BlockId id = c->data.at(static_cast<std::size_t>(cellIndex(lx, y, lz)));
                if (id == block::Air || id >= wanted.size() || !wanted[id]) continue;
                if (maxPerId > 0 && per[id] >= maxPerId) continue;
                ++per[id];
                out.push_back(FoundBlock{BlockPos{x - d.ox, y - d.oy, z - d.oz}, id});
                if (static_cast<int>(out.size()) >= max) return out;
            }
        }
    return out;
}
bool VoxelSystem::isBlockHidden(int x, int y, int z) const {
    const Impl& d = *impl_;
    return d.hidden_cells.count(Impl::hiddenKey(x + d.ox, y + d.oy, z + d.oz)) != 0;
}
int VoxelSystem::skyLight(int x, int y, int z) const {
    const Impl& d = *impl_;
    return d.getLight(x + d.ox, y + d.oy, z + d.oz) >> 4;
}
int VoxelSystem::blockLight(int x, int y, int z) const {
    const Impl& d = *impl_;
    return d.getLight(x + d.ox, y + d.oy, z + d.oz) & 15;
}
bool VoxelSystem::isLoaded(int x, int z) const { return impl_->chunkOf(x + impl_->ox, z + impl_->oz) != nullptr; }
bool VoxelSystem::isReady(int x, int z) const {
    const Impl::Chunk* c = impl_->chunkOf(x + impl_->ox, z + impl_->oz);
    return c != nullptr && c->ever_meshed;
}
int VoxelSystem::surfaceHeight(int x, int z) const {
    const Impl& d = *impl_;
    return (d.generator ? d.generator->terrainHeight(x + d.ox, z + d.oz) : 64) - d.oy;
}

// --- Estructuras (Structures.cpp): en coordenadas absolutas por dentro ---
namespace {
void structureToLocal(StructureInfo& s, const Vec3& offset) {
    s.center = s.center - offset;
    s.min = s.min - offset;
    s.max = s.max - offset;
    for (StructurePoint& p : s.points) p.position = p.position - offset;
}
}  // namespace

std::vector<StructureInfo> VoxelSystem::structuresNear(const Vec3& position, float radius) const {
    const Impl& d = *impl_;
    if (!d.generator) return {};
    const Vec3 offset{static_cast<float>(d.ox), static_cast<float>(d.oy), static_cast<float>(d.oz)};
    const Vec3 p = position + offset;
    std::vector<StructureInfo> list;
    if (d.generator->structures() != nullptr) list = d.generator->structures()->nearby(p.x, p.y, p.z, radius);
    // Simas abisales (superficie) y lo del Reino Profundo (GeneratorDeep.cpp).
    for (StructureInfo& s : d.generator->deepStructuresNear(p.x, p.y, p.z, radius)) list.push_back(std::move(s));
    for (StructureInfo& s : list) structureToLocal(s, offset);
    return list;
}

std::vector<GeneratedChest> VoxelSystem::structureChests(const Vec3& min, const Vec3& max) const {
    // CIUDADES-GRANDES: cofres de botin del generador (coordenadas absolutas en x, y, z y key).
    const Impl& d = *impl_;
    if (!d.generator || d.generator->structures() == nullptr) return {};
    const Vec3 offset{static_cast<float>(d.ox), static_cast<float>(d.oy), static_cast<float>(d.oz)};
    const Vec3 a = min + offset, b = max + offset;
    const int x0 = static_cast<int>(std::floor(std::min(a.x, b.x))), x1 = static_cast<int>(std::floor(std::max(a.x, b.x)));
    const int z0 = static_cast<int>(std::floor(std::min(a.z, b.z))), z1 = static_cast<int>(std::floor(std::max(a.z, b.z)));
    const float y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
    std::vector<GeneratedChest> out;
    for (GeneratedChest& c : d.generator->structures()->chests(x0, z0, x1, z1)) {
        if (static_cast<float>(c.y) + 0.5f < y0 || static_cast<float>(c.y) + 0.5f > y1) continue;
        c.position = Vec3{static_cast<float>(c.x) + 0.5f, static_cast<float>(c.y), static_cast<float>(c.z) + 0.5f} - offset;
        out.push_back(std::move(c));
    }
    return out;
}

bool VoxelSystem::structureAt(const Vec3& position, StructureInfo& out) const {
    const Impl& d = *impl_;
    if (!d.generator) return false;
    const Vec3 offset{static_cast<float>(d.ox), static_cast<float>(d.oy), static_cast<float>(d.oz)};
    const Vec3 p = position + offset;
    const bool found = (d.generator->structures() != nullptr && d.generator->structures()->at(p.x, p.y, p.z, out)) ||
                       d.generator->deepStructureAt(p.x, p.y, p.z, out);
    if (!found) return false;
    structureToLocal(out, offset);
    return true;
}
void VoxelSystem::shiftOrigin(const Vec3& offset) {
    Impl& d = *impl_;
    d.showHiddenCells();  // las puertas ocultas (dibujo) iban en las coordenadas de antes del juego
    d.ox += static_cast<int>(std::lround(offset.x));
    d.oy += static_cast<int>(std::lround(offset.y));
    d.oz += static_cast<int>(std::lround(offset.z));
    // Los chunks ya subidos los mueve el renderizador (shiftOrigin) y sus
    // colisiones la fisica; lo que este en cola se convierte al mandarlo.
    std::lock_guard<std::mutex> lock(d.foliage_mutex);
    for (Impl::FoliageNote& n : d.foliage_notes) n.feet = n.feet - offset;
}
bool VoxelSystem::inWater(const Vec3& p) const {
    return block::isWet(getBlock(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)), static_cast<int>(std::floor(p.z))));
}
int VoxelSystem::oceanDepth(int x, int z) const {
    const Impl& d = *impl_;
    return d.generator ? d.generator->oceanDepth(x + d.ox, z + d.oz) : 0;
}
int VoxelSystem::waterLevel(int x, int z) const {
    const Impl& d = *impl_;
    if (!d.generator) return 0;
    const int level = d.generator->waterLevel(x + d.ox, z + d.oz);
    return level > 0 ? level - d.oy : 0;
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
            const Vec3 cell{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
            // COLISION-MODELOS: valla, silla, mesa... el rayo tiene que tocar una de sus piezas (si pasa
            // por un hueco sigue hasta el bloque de detras) y la caja es la de sus piezas.
            ModelBoxes boxes{};
            const int count = hasModelCollision(def)
                                  ? modelCollisionBoxes(id, [this](int bx, int by, int bz) { return getBlock(bx, by, bz); }, x, y, z, boxes)
                                  : 0;
            if (count > 0) {
                float best = 1e30f;
                BlockPos best_normal = normal;
                Vec3 lo_all{1e30f, 1e30f, 1e30f}, hi_all{-1e30f, -1e30f, -1e30f};
                const float o[3] = {origin.x, origin.y, origin.z}, dd[3] = {dir.x, dir.y, dir.z};
                for (int i = 0; i < count; ++i) {
                    const ModelBox& mb = boxes[static_cast<std::size_t>(i)];
                    const float lo[3] = {cell.x + mb.lo[0] / 16.0f, cell.y + mb.lo[1] / 16.0f, cell.z + mb.lo[2] / 16.0f};
                    const float hi[3] = {cell.x + mb.hi[0] / 16.0f, cell.y + mb.hi[1] / 16.0f, cell.z + mb.hi[2] / 16.0f};
                    lo_all = Vec3{std::min(lo_all.x, lo[0]), std::min(lo_all.y, lo[1]), std::min(lo_all.z, lo[2])};
                    hi_all = Vec3{std::max(hi_all.x, hi[0]), std::max(hi_all.y, hi[1]), std::max(hi_all.z, hi[2])};
                    float t0 = 0.0f, t1 = 1e30f;
                    int axis = -1;
                    bool miss = false;
                    for (int k = 0; k < 3 && !miss; ++k) {
                        if (std::abs(dd[k]) < 1e-9f) {
                            miss = o[k] < lo[k] || o[k] > hi[k];
                            continue;
                        }
                        float a = (lo[k] - o[k]) / dd[k], b = (hi[k] - o[k]) / dd[k];
                        if (a > b) std::swap(a, b);
                        if (a > t0) {
                            t0 = a;
                            axis = k;
                        }
                        t1 = std::min(t1, b);
                        miss = t0 > t1;
                    }
                    if (miss || t0 >= best) continue;
                    best = t0;
                    if (axis >= 0) {
                        int nn[3] = {0, 0, 0};
                        nn[axis] = dd[axis] > 0 ? -1 : 1;
                        best_normal = BlockPos{nn[0], nn[1], nn[2]};
                    }
                }
                if (best <= max_distance && best < 1e29f) {
                    hit.hit = true;
                    hit.block = BlockPos{x, y, z};
                    hit.normal = best_normal;
                    hit.id = id;
                    hit.distance = best;
                    hit.point = origin + dir * best;
                    hit.box_min = lo_all;
                    hit.box_max = hi_all;
                    return hit;
                }
            } else {
                hit.hit = true;
                hit.block = BlockPos{x, y, z};
                hit.normal = normal;
                hit.id = id;
                hit.distance = t;
                hit.point = origin + dir * t;
                hit.box_min = cell;
                hit.box_max = cell + Vec3{1.0f, 1.0f, 1.0f};
                return hit;
            }
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
                if (!isLoaded(x, z) && y + impl_->oy >= 0 && y + impl_->oy < kWorldHeight) return true;  // sin cargar: pared
                const BlockId id = getBlock(x, y, z);
                const BlockDef& def = blockDef(id);
                if (!def.solid) continue;
                if (!hasModelCollision(def)) return true;
                // COLISION-MODELOS: valla, silla, mesa... solo sus piezas.
                ModelBoxes boxes{};
                const int count = modelCollisionBoxes(id, [this](int bx, int by, int bz) { return getBlock(bx, by, bz); }, x, y, z, boxes);
                for (int i = 0; i < count; ++i) {
                    const ModelBox& mb = boxes[static_cast<std::size_t>(i)];
                    if (c.x + h.x > x + mb.lo[0] / 16.0f && c.x - h.x < x + mb.hi[0] / 16.0f && c.y + h.y > y + mb.lo[1] / 16.0f &&
                        c.y - h.y < y + mb.hi[1] / 16.0f && c.z + h.z > z + mb.lo[2] / 16.0f && c.z - h.z < z + mb.hi[2] / 16.0f) {
                        return true;
                    }
                }
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
            const float before = coord;
            if (amount > 0) {
                coord = std::floor(coord + extent + amount) - extent - 1e-3f;
            } else {
                coord = std::floor(coord - extent + amount) + 1.0f + extent + 1e-3f;
            }
            // COLISION-MODELOS: con piezas finas (vallas, sillas...) el borde de la celda puede quedar
            // detras (volveria hacia atras) o dentro de otra pieza: se busca el tramo libre (biseccion).
            const bool backwards = amount > 0 ? coord < before : coord > before;
            if (backwards || boxCollides(r.position, half)) {
                float lo = 0.0f, hi = 1.0f;
                for (int k = 0; k < 10; ++k) {
                    const float mid = 0.5f * (lo + hi);
                    coord = before + amount * mid;
                    if (boxCollides(r.position, half)) hi = mid;
                    else lo = mid;
                }
                // Si ni el primer trozo cabe, se queda donde estaba en este eje.
                coord = before + amount * lo;
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
    const FarTerrain::Stats fs = d.far_terrain.stats();
    s.far_tiles = fs.tiles;
    s.far_ready = fs.ready;
    s.far_pending = fs.pending;
    s.far_quads = fs.quads;
    s.far_last_tile_ms = fs.last_tile_ms;
    s.far_from_cache = fs.from_cache;
    s.far_generated = fs.generated;
    s.far_hole = d.far_hole;
    return s;
}

// --- Dimensiones ----------------------------------------------------------------------

const std::string& VoxelSystem::dimension() const { return impl_->dimension; }

bool VoxelSystem::setDimension(const std::string& name, bool ambience) {
    Impl& d = *impl_;
    if (!d.active || findDimension(name) == nullptr) return false;
    d.ambience = ambience;
    if (name != d.dimension) {
        // Lo construido en la actual: con nombre, a disco; sin nombre, a memoria.
        for (const auto& [key, chunk] : d.chunks) d.saveChunk(*chunk);
        d.dimension = name;
        // Fuera chunks, colisiones, trabajos en vuelo (otra epoca) y las
        // secciones del renderizador (syncRenderer las borra todas); otro
        // generador. Los chunks nuevos cargan alrededor de quien mira.
        d.resetWorld(d.seed);
        d.writeWorldInfo();
    }
    d.applyAmbience();
    return true;
}

namespace {
// Se puede pisar: solido y cubo (ni hojas, ni nubes, ni lava).
bool goodGround(BlockId id) {
    const BlockDef& def = blockDef(id);
    return def.solid && def.shape == BlockShape::Cube && !(def.cutout && def.waves) && id != block::Lava && id != block::Cloud;
}
// Se puede estar dentro: no choca y no es liquido.
bool freeCell(BlockId id) {
    return !blockDef(id).solid && id != block::Lava && !block::isWet(id) && id != block::FallingWater;
}
}  // namespace

Vec3 VoxelSystem::spawnPoint() const {
    const Impl& d = *impl_;
    const Vec3 origin{static_cast<float>(d.ox), static_cast<float>(d.oy), static_cast<float>(d.oz)};
    return spawnPoint(d.has_viewer ? d.viewer - origin : Vec3{0.0f, 0.0f, 0.0f} - origin);
}

Vec3 VoxelSystem::spawnPoint(const Vec3& around) const {
    const Impl& d = *impl_;
    if (!d.generator) return around;
    const int ax = static_cast<int>(std::floor(around.x)) + d.ox, az = static_cast<int>(std::floor(around.z)) + d.oz;
    const int ccx = floorDiv(ax, kChunkSize), ccz = floorDiv(az, kChunkSize);
    // En el abismo y en el Reino Profundo, dentro de la caverna (de abajo arriba, no sobre el techo).
    const bool abyss = d.generator->profile() == VoxelGenerator::Abyss || d.generator->profile() == VoxelGenerator::Deep;
    const int y_cap = d.generator->profile() == VoxelGenerator::Abyss ? 140 : kWorldHeight - 3;
    const auto mem = d.memory_chunks.find(d.dimension);
    ChunkData temp;
    for (int ring = 0; ring <= 6; ++ring) {
        long long best_d2 = -1;
        int bx = 0, by = 0, bz = 0;
        for (int dz = -ring; dz <= ring; ++dz) {
            for (int dx = -ring; dx <= ring; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != ring) continue;
                const int cx = ccx + dx, cz = ccz + dz;
                const ChunkData* data = nullptr;
                if (const Impl::Chunk* c = d.chunkAt(cx, cz)) {
                    data = &c->data;
                } else {
                    // Sin cargar: lo genera aqui (y encima lo guardado).
                    temp = ChunkData{};
                    temp.cx = cx;
                    temp.cz = cz;
                    d.generator->generate(temp);
                    if (const std::filesystem::path file = d.chunkFile(cx, cz); !file.empty()) {
                        std::error_code error;
                        if (std::filesystem::exists(file, error)) readChunkFile(file, temp);
                    } else if (mem != d.memory_chunks.end()) {
                        if (const auto it = mem->second.find(chunkKey(cx, cz)); it != mem->second.end()) decodeChunk(*it->second, temp);
                    }
                    data = &temp;
                }
                const auto at = [&](int x, int y, int z) { return data->get(x, y, z); };
                const auto valid = [&](int x, int y, int z) {
                    if (!goodGround(at(x, y, z)) || !freeCell(at(x, y + 1, z)) || !freeCell(at(x, y + 2, z))) return false;
                    for (int oz = -1; oz <= 1; ++oz) {
                        for (int ox = -1; ox <= 1; ++ox) {
                            if (at(x + ox, y, z + oz) == block::Lava || at(x + ox, y + 1, z + oz) == block::Lava) return false;
                            if ((ox == 0) != (oz == 0) && !goodGround(at(x + ox, y, z + oz))) return false;  // ni borde ni hueco
                        }
                    }
                    return true;
                };
                for (int z = 1; z < kChunkSize - 1; ++z) {
                    for (int x = 1; x < kChunkSize - 1; ++x) {
                        int found = -1;
                        if (abyss) {
                            for (int y = 3; y < y_cap && found < 0; ++y) {
                                if (valid(x, y, z)) found = y;
                            }
                        } else {
                            for (int y = y_cap; y >= 1 && found < 0; --y) {
                                if (valid(x, y, z)) found = y;
                            }
                        }
                        if (found < 0) continue;
                        const long long ex = cx * kChunkSize + x - ax, ez = cz * kChunkSize + z - az;
                        const long long d2 = ex * ex + ez * ez;
                        if (best_d2 < 0 || d2 < best_d2) {
                            best_d2 = d2;
                            bx = cx * kChunkSize + x;
                            by = found;
                            bz = cz * kChunkSize + z;
                        }
                    }
                }
            }
        }
        if (best_d2 >= 0) {
            return Vec3{static_cast<float>(bx - d.ox) + 0.5f, static_cast<float>(by + 1 - d.oy), static_cast<float>(bz - d.oz) + 0.5f};
        }
    }
    return Vec3{around.x, static_cast<float>(surfaceHeight(static_cast<int>(std::floor(around.x)), static_cast<int>(std::floor(around.z))) + 1),
                around.z};
}

}  // namespace cramion::voxel
