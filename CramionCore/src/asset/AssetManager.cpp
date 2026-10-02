#include "CramionCore/asset/AssetManager.h"

#include "CrData.h"
#include "Primitives.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

namespace cramion::assets {

// Hilos de fondo de requestModel(): una cola de modelos por leer y lo leido.
// readModel() es independiente (no toca el AssetManager): los hilos solo leen
// archivos y devuelven el asset; lo guarda pollLoads() en el hilo principal.
struct AssetManager::Loader {
    struct Job {
        Uuid uuid{};
        std::filesystem::path path;
        std::string name;
        std::uint64_t generation = 0;
    };
    struct Done {
        Uuid uuid{};
        std::shared_ptr<ModelAsset> asset;  // nullptr = no se pudo leer
        std::uint64_t generation = 0;
    };
    std::mutex mutex;
    std::condition_variable work;      // hay trabajo (o hay que salir)
    std::condition_variable finished;  // termino un modelo (loadModel puede estar esperandolo)
    std::deque<Job> queue;
    std::unordered_set<Uuid> in_flight;  // en la cola o leyendose
    std::vector<Done> done;
    std::vector<std::thread> threads;
    std::uint64_t generation = 0;  // clear()/unload(): lo que llegue de antes se tira
    bool stop = false;

    Loader() {
        // Pocos hilos: el disco y la memoria mandan, y el juego sigue corriendo.
        const unsigned cores = std::max(2u, std::thread::hardware_concurrency());
        const unsigned count = std::clamp(cores / 3u, 1u, 4u);
        for (unsigned i = 0; i < count; ++i) threads.emplace_back([this] { run(); });
    }
    ~Loader() {
        {
            std::lock_guard lock(mutex);
            stop = true;
            queue.clear();
        }
        work.notify_all();
        for (std::thread& t : threads) t.join();
    }
    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex);
                work.wait(lock, [&] { return stop || !queue.empty(); });
                if (stop) return;
                job = std::move(queue.front());
                queue.pop_front();
            }
            std::shared_ptr<ModelAsset> asset;
            try {
                asset = AssetManager::readModel(job.uuid, job.path, job.name);
            } catch (const std::exception& e) {
                std::cerr << "[Assets] " << job.name << ": " << e.what() << "\n";
            }
            {
                std::lock_guard lock(mutex);
                in_flight.erase(job.uuid);
                done.push_back(Done{job.uuid, std::move(asset), job.generation});
            }
            finished.notify_all();
        }
    }
};

AssetManager::AssetManager(AssetDatabase& database) : database_(database) {}

AssetManager::~AssetManager() = default;

std::shared_ptr<const ModelAsset> AssetManager::requestModel(const Uuid& uuid) {
    if (const auto it = models_.find(uuid); it != models_.end()) return it->second;
    if (primitives::isBuiltin(uuid)) return loadModel(uuid);  // se generan al momento
    if (failed_loads_.contains(uuid)) return nullptr;
    if (!loader_) loader_ = std::make_unique<Loader>();
    {
        std::lock_guard lock(loader_->mutex);
        if (loader_->in_flight.contains(uuid)) return nullptr;
    }
    const std::optional<AssetInfo> info = database_.find(uuid);
    if (!info || info->type != AssetType::Model || info->path.empty()) {
        std::cerr << "[Assets] No hay ningun modelo con UUID " << uuid.toString() << "\n";
        failed_loads_.insert(uuid);
        return nullptr;
    }
    {
        std::lock_guard lock(loader_->mutex);
        loader_->queue.push_back(Loader::Job{uuid, info->path, info->name, loader_->generation});
        loader_->in_flight.insert(uuid);
    }
    loader_->work.notify_one();
    return nullptr;
}

void AssetManager::takeFinished(const Uuid* only) {
    if (!loader_) return;
    std::vector<Loader::Done> done;
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(loader_->mutex);
        generation = loader_->generation;
        if (only == nullptr) {
            done.swap(loader_->done);
        } else {
            for (auto it = loader_->done.begin(); it != loader_->done.end();) {
                if (it->uuid == *only) {
                    done.push_back(std::move(*it));
                    it = loader_->done.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
    for (Loader::Done& d : done) {
        if (d.generation != generation) continue;  // de antes de un clear()/unload()
        if (d.asset) {
            models_.emplace(d.uuid, std::move(d.asset));
        } else {
            failed_loads_.insert(d.uuid);
        }
    }
}

std::size_t AssetManager::pollLoads() {
    const std::size_t before = models_.size();
    takeFinished(nullptr);
    return models_.size() >= before ? models_.size() - before : 0;
}

std::size_t AssetManager::loadsInFlight() const {
    if (!loader_) return 0;
    std::lock_guard lock(loader_->mutex);
    return loader_->in_flight.size() + loader_->done.size();
}

std::shared_ptr<ModelAsset> AssetManager::readModel(const Uuid& uuid, const std::filesystem::path& file,
                                                   const std::string& name, bool decode_textures) {
    if (primitives::isBuiltin(uuid)) {
        return primitives::make(uuid);
    }
    const auto start = std::chrono::steady_clock::now();
    crdata::Header header{};
    crdata::ModelContent content{};
    if (!crdata::readModel(file, header, content)) {
        std::cerr << "[Assets] No se pudo leer " << crdata::utf8(file)
                  << " (danado o de otra version)\n";
        return nullptr;
    }

    const float read_seconds =
        std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();

    auto asset = std::make_shared<ModelAsset>();
    asset->uuid = uuid;
    asset->name = name;
    asset->nodes = std::move(content.nodes);
    asset->animated = content.animated;
    asset->animation_names = std::move(content.animation_names);
    asset->parts.reserve(content.parts.size());
    try {
        std::size_t reclustered = 0;
        std::size_t before = 0;
        std::size_t after = 0;
        for (asset::ModelData& part : content.parts) {
            // .crdata de versiones anteriores con clusteres de 5 unidades
            // fijas: en un FBX en centimetros eran miles de submallas (y de
            // llamadas de dibujo) por objeto. Se reagrupan en memoria.
            if (asset::isOverClustered(part)) {
                before += part.submeshes.size();
                asset::clusterSubmeshes(part);
                after += part.submeshes.size();
                ++reclustered;
            }
        }
        if (reclustered > 0) {
            std::cout << "[Assets] " << name << ": reagrupadas " << reclustered << " piezas ("
                      << before << " -> " << after
                      << " submallas). Reimporta el modelo para guardarlo asi.\n";
        }
        for (asset::ModelData& part : content.parts) {
            // LODs automaticos (solo en memoria): los estaticos pesados se
            // dibujan simplificados cuando la diferencia no se ve.
            asset::generateLods(part);
            // Texturas incrustadas: se decodifican aqui (en paralelo).
            if (decode_textures) asset::finalizeModel(part, name + "/" + part.name);
            asset->parts.push_back(std::make_shared<asset::ModelData>(std::move(part)));
        }
    } catch (const std::exception& error) {
        std::cerr << "[Assets] " << error.what() << "\n";
        return nullptr;
    }

    const float seconds =
        std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
    std::cout << "[Assets] Cargado " << name << ": " << asset->parts.size() << " piezas, "
              << asset->nodes.size() << " nodos, " << seconds << " s (lectura " << read_seconds
              << " s)\n";
    return asset;
}

std::shared_ptr<const ModelAsset> AssetManager::loadModel(const Uuid& uuid) {
    if (const auto it = models_.find(uuid); it != models_.end()) {
        return it->second;
    }
    // Se esta leyendo en segundo plano: se espera a ese hilo (no dos veces).
    if (loader_) {
        bool waited = false;
        {
            std::unique_lock lock(loader_->mutex);
            if (loader_->in_flight.contains(uuid)) {
                loader_->finished.wait(lock, [&] { return !loader_->in_flight.contains(uuid); });
                waited = true;
            }
            waited = waited || std::any_of(loader_->done.begin(), loader_->done.end(),
                                           [&](const Loader::Done& d) { return d.uuid == uuid; });
        }
        if (waited) {
            takeFinished(&uuid);
            if (const auto it = models_.find(uuid); it != models_.end()) return it->second;
            if (failed_loads_.contains(uuid)) return nullptr;
        }
    }

    if (primitives::isBuiltin(uuid)) {
        std::shared_ptr<ModelAsset> asset = primitives::make(uuid);
        models_.emplace(uuid, asset);
        return asset;
    }

    const std::optional<AssetInfo> info = database_.find(uuid);
    if (!info || info->type != AssetType::Model || info->path.empty()) {
        std::cerr << "[Assets] No hay ningun modelo con UUID " << uuid.toString() << "\n";
        return nullptr;
    }

    std::shared_ptr<ModelAsset> asset = readModel(uuid, info->path, info->name);
    if (!asset) return nullptr;
    models_.emplace(uuid, asset);
    return asset;
}

std::filesystem::path AssetManager::environmentFile(const Uuid& uuid) {
    const std::optional<AssetInfo> info = database_.find(uuid);
    if (!info || info->type != AssetType::Environment || info->path.empty()) {
        std::cerr << "[Assets] No hay ningun cielo con UUID " << uuid.toString() << "\n";
        return {};
    }

    std::filesystem::path folder = cache_folder_;
    if (folder.empty()) {
        folder = std::filesystem::temp_directory_path() / "Cramion" / "Cache";
    }
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    const std::filesystem::path extracted = folder / (uuid.toString() + ".hdr");

    // Ya extraido y mas nuevo que el .crdata: no se repite (el HDR de Bistro
    // son 24 MB).
    if (std::filesystem::exists(extracted, error) &&
        std::filesystem::last_write_time(extracted, error) >=
            std::filesystem::last_write_time(info->path, error)) {
        return extracted;
    }

    crdata::Header header{};
    std::string extension;
    std::vector<std::uint8_t> bytes;
    if (!crdata::readEnvironment(info->path, header, extension, bytes)) {
        std::cerr << "[Assets] No se pudo leer el cielo " << crdata::utf8(info->path) << "\n";
        return {};
    }
    std::filesystem::path temporary = extracted;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            std::cerr << "[Assets] No se pudo extraer el cielo a " << crdata::utf8(extracted)
                      << "\n";
            return {};
        }
    }
    std::filesystem::rename(temporary, extracted, error);
    if (error) {
        std::cerr << "[Assets] No se pudo extraer el cielo: " << error.message() << "\n";
        return {};
    }
    return extracted;
}

void AssetManager::unload(const Uuid& uuid) {
    models_.erase(uuid);
    failed_loads_.erase(uuid);
    if (loader_) {
        // Lo que se este leyendo de antes ya no vale (p. ej. se reimporto): se
        // tira al llegar y quien lo necesite lo vuelve a pedir.
        std::lock_guard lock(loader_->mutex);
        ++loader_->generation;
        loader_->queue.clear();
        loader_->in_flight.clear();
    }
}

void AssetManager::clear() {
    models_.clear();
    failed_loads_.clear();
    if (loader_) {
        std::lock_guard lock(loader_->mutex);
        ++loader_->generation;
        loader_->queue.clear();
        loader_->in_flight.clear();
        loader_->done.clear();
    }
}

}  // namespace cramion::assets
