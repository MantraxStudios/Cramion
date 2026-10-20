#include "CramionCore/asset/AssetManager.h"

#include "CramionCore/asset/Importer.h"

#include "CramionCore/asset/LightmapUv.h"

#include "CrData.h"
#include "Primitives.h"

#include <CramionFX/asset/TextureStats.h>

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
        // Las UV de lightmap de la escena cuando se pidio (se leen en el hilo).
        std::shared_ptr<const lighting::LightmapUvSet> uvs;
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
                asset = AssetManager::readModel(job.uuid, job.path, job.name, true, job.uvs.get());
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
        loader_->queue.push_back(Loader::Job{uuid, info->path, info->name, loader_->generation, lightmap_uvs_});
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

std::vector<asset::TextureData> AssetManager::readModelTextures(const std::filesystem::path& file) {
    std::vector<asset::TextureData> textures;
    crdata::Header header{};
    crdata::ModelContent content{};
    if (!crdata::readModel(file, header, content)) return textures;
    for (asset::ModelData& part : content.parts) {
        asset::assignTextureUsage(part);
        for (asset::TextureData& texture : part.textures) textures.push_back(std::move(texture));
    }
    return textures;
}

std::vector<std::vector<MaterialColors>> AssetManager::readMaterialColors(const Uuid& uuid,
                                                                          const std::filesystem::path& file,
                                                                          const std::vector<bool>& wanted) {
    std::vector<asset::ModelData> parts;
    if (primitives::isBuiltin(uuid)) {
        const std::shared_ptr<ModelAsset> asset = primitives::make(uuid);
        if (asset) {
            for (const std::shared_ptr<asset::ModelData>& part : asset->parts) {
                parts.push_back(part ? *part : asset::ModelData{});
            }
        }
    } else {
        crdata::Header header{};
        crdata::ModelContent content{};
        if (file.empty() || !crdata::readModel(file, header, content)) {
            std::cerr << "[Assets] No se pudieron leer los materiales de " << crdata::utf8(file) << "\n";
            return {};
        }
        parts = std::move(content.parts);
    }
    // Lo mismo que skinned.frag: emision = textura x factor x kEmissiveIntensity.
    constexpr float kEmissiveIntensity = 6.0f;
    std::vector<std::vector<MaterialColors>> out(parts.size());
    for (std::size_t p = 0; p < parts.size(); ++p) {
        if (!wanted.empty() && (p >= wanted.size() || !wanted[p])) {
            parts[p] = asset::ModelData{};
            continue;
        }
        asset::ModelData& part = parts[p];
        // La media de cada textura, una vez (varios materiales la comparten).
        std::vector<std::uint8_t> state(part.textures.size(), 0);  // 0 sin mirar, 1 leida, 2 fallo
        std::vector<core::Vec3> average(part.textures.size());
        std::vector<float> opaque(part.textures.size(), 1.0f);
        const auto texture_average = [&](std::int32_t index, core::Vec3& rgb, float* opaque_fraction) {
            if (index < 0 || static_cast<std::size_t>(index) >= part.textures.size()) return false;
            const auto i = static_cast<std::size_t>(index);
            if (state[i] == 0) {
                state[i] = asset::averageStoredTextureLinear(part.textures[i], average[i], &opaque[i]) ? 1 : 2;
                // Ya no hace falta: fuera de la memoria.
                part.textures[i].pixels = {};
                part.textures[i].encoded = {};
            }
            if (state[i] != 1) return false;
            rgb = average[i];
            if (opaque_fraction != nullptr) *opaque_fraction = opaque[i];
            return true;
        };
        out[p].reserve(part.materials.size());
        for (const asset::MaterialData& md : part.materials) {
            MaterialColors c;
            c.albedo = core::Vec3{md.base_color.x, md.base_color.y, md.base_color.z};
            c.transparent = md.transparent;
            c.metallic = md.metallic;
            c.roughness = md.roughness;
            if (md.albedo_texture >= 0) {
                c.albedo_texture = true;
                core::Vec3 t{1.0f, 1.0f, 1.0f};
                float fraction = 1.0f;
                if (texture_average(md.albedo_texture, t, &fraction)) {
                    c.albedo = c.albedo * t;
                    c.opaque_fraction = fraction;
                    c.albedo_texture_read = true;
                } else {
                    c.albedo = c.albedo * 0.6f;  // sin leer: lo de antes
                }
            } else if (md.base_color.w < 0.5f) {
                c.opaque_fraction = 0.0f;  // todo recortado
            }
            core::Vec3 e{1.0f, 1.0f, 1.0f};
            if (md.emissive_texture >= 0) {
                core::Vec3 t;
                if (texture_average(md.emissive_texture, t, nullptr)) e = t;
            }
            c.emission = md.emissive * e * kEmissiveIntensity;
            out[p].push_back(c);
        }
        part = asset::ModelData{};  // suelta la pieza (y sus texturas)
    }
    return out;
}

namespace {

// Las UV de lightmap de cada pieza (si el conjunto tiene su disposicion), sobre
// la malla tal como sale del .crdata: antes de reagrupar y de los LODs (el
// hash es de esa malla, PLAN-LIGHTMAPS 1.4).
void applySceneLightmapUvs(const Uuid& uuid, std::vector<asset::ModelData*> parts, const lighting::LightmapUvSet* uvs) {
    if (uvs == nullptr || uvs->empty()) return;
    for (std::size_t p = 0; p < parts.size(); ++p) {
        if (parts[p] == nullptr) continue;
        const std::shared_ptr<const lighting::LightmapUvLayout> layout = uvs->load(uuid, static_cast<int>(p));
        if (!layout) continue;
        if (!lighting::applyLightmapUvs(*parts[p], *layout)) {
            std::cerr << "[Lightmap] Las UV de lightmap de " << parts[p]->name
                      << " son de otra version de la malla: vuelve a hornear\n";
        }
    }
}

}  // namespace

void AssetManager::setLightmapUvs(std::shared_ptr<const lighting::LightmapUvSet> uvs) {
    lightmap_uvs_ = std::move(uvs);
}

std::shared_ptr<ModelAsset> AssetManager::readModel(const Uuid& uuid, const std::filesystem::path& file,
                                                   const std::string& name, bool decode_textures,
                                                   const lighting::LightmapUvSet* uvs) {
    if (primitives::isBuiltin(uuid)) {
        std::shared_ptr<ModelAsset> asset = primitives::make(uuid);
        if (asset && uvs != nullptr) {
            std::vector<asset::ModelData*> parts;
            for (const std::shared_ptr<asset::ModelData>& part : asset->parts) parts.push_back(part.get());
            applySceneLightmapUvs(uuid, parts, uvs);
        }
        return asset;
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

    if (uvs != nullptr) {
        std::vector<asset::ModelData*> parts;
        for (asset::ModelData& part : content.parts) parts.push_back(&part);
        applySceneLightmapUvs(uuid, parts, uvs);
    }

    auto asset = std::make_shared<ModelAsset>();
    asset->uuid = uuid;
    asset->name = name;
    asset->nodes = std::move(content.nodes);
    asset->animated = content.animated;
    asset->animation_names = std::move(content.animation_names);
    asset->parts.reserve(content.parts.size());
    double acmr_before = 0.0;
    double acmr_after = 0.0;
    std::size_t acmr_triangles = 0;
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
        // LODs del modelo (ajuste de importacion): los hechos a mano
        // (_LOD1.._LODn) pasan a su LOD0 y sus piezas se vacian (no tienen
        // nodo: nadie las dibuja); el resto, automaticos salvo Off/Manual.
        const ModelLodInfo lod_info = modelLodInfo(header.settings_json);
        std::vector<bool> manual_lods(content.parts.size(), false);
        if (lod_info.mode != ModelLodMode::Off) {
            const auto valid = [&](std::int32_t p) {
                return p >= 0 && static_cast<std::size_t>(p) < content.parts.size();
            };
            for (const std::vector<std::int32_t>& group : lod_info.groups) {
                if (!valid(group.front()) || manual_lods[static_cast<std::size_t>(group.front())]) continue;
                std::vector<const asset::ModelData*> levels;
                for (std::size_t k = 1; k < group.size(); ++k) {
                    if (valid(group[k]) && group[k] != group.front()) {
                        levels.push_back(&content.parts[static_cast<std::size_t>(group[k])]);
                    }
                }
                asset::ModelData& lod0 = content.parts[static_cast<std::size_t>(group.front())];
                // Lo que el renderizador pide antes de los LODs (el orden de
                // los triangulos de LOD0) se hace igual en el bucle de abajo.
                manual_lods[static_cast<std::size_t>(group.front())] = asset::setManualLods(lod0, levels) > 0;
            }
            for (const std::vector<std::int32_t>& group : lod_info.groups) {
                for (std::size_t k = 1; k < group.size(); ++k) {
                    if (!valid(group[k]) || group[k] == group.front()) continue;
                    asset::ModelData& level = content.parts[static_cast<std::size_t>(group[k])];
                    asset::ModelData empty{};
                    empty.name = level.name;
                    level = std::move(empty);
                }
            }
        }
        for (asset::ModelData& part : content.parts) {
            const auto part_index = static_cast<std::size_t>(&part - content.parts.data());
            // Los triangulos de cada submalla en el orden que reutiliza los
            // vertices ya transformados (solo en memoria, como los LODs): los
            // clusteres del importador y del static batching los desordenan.
            const asset::VertexCacheStats cache = asset::optimizeVertexCache(part);
            acmr_before += static_cast<double>(cache.acmr_before) * static_cast<double>(part.indices.size() / 3);
            acmr_after += static_cast<double>(cache.acmr_after) * static_cast<double>(part.indices.size() / 3);
            acmr_triangles += part.indices.size() / 3;
            // LODs automaticos (solo en memoria): los estaticos pesados se
            // dibujan simplificados cuando la diferencia no se ve. No con
            // LODs a mano ni con el ajuste Off/Manual del modelo.
            if (!manual_lods[part_index] && lod_info.mode == ModelLodMode::Auto) asset::generateLods(part);
            // Texturas incrustadas: se decodifican aqui (en paralelo).
            // Solo esqueleto y animaciones (paquetes de clips): se leen para extraerlos.
            const bool clips_only = part.indices.empty() && !part.animations.empty();
            if (decode_textures && !clips_only) asset::finalizeModel(part, name + "/" + part.name);
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
              << " s)";
    if (acmr_triangles >= 4096) {
        std::cout << ", vertices por triangulo " << acmr_before / static_cast<double>(acmr_triangles) << " -> "
                  << acmr_after / static_cast<double>(acmr_triangles);
    }
    std::cout << "\n";
    return asset;
}

std::shared_ptr<ModelAsset> AssetManager::readModelGeometry(const Uuid& uuid, const std::filesystem::path& file,
                                                           const std::string& name, const std::vector<bool>& wanted) {
    if (primitives::isBuiltin(uuid)) {
        return primitives::make(uuid);
    }
    if (file.empty()) {
        return nullptr;
    }
    crdata::Header header{};
    crdata::ModelContent content{};
    if (!crdata::readModelGeometry(file, header, content, wanted)) {
        std::cerr << "[Assets] No se pudo leer la geometria de " << crdata::utf8(file) << "\n";
        return nullptr;
    }
    auto asset = std::make_shared<ModelAsset>();
    asset->uuid = uuid;
    asset->name = name;
    asset->nodes = std::move(content.nodes);
    asset->animated = content.animated;
    asset->animation_names = std::move(content.animation_names);
    asset->parts.reserve(content.parts.size());
    for (asset::ModelData& part : content.parts) {
        asset->parts.push_back(std::make_shared<asset::ModelData>(std::move(part)));
    }
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
        std::shared_ptr<ModelAsset> asset = readModel(uuid, {}, {}, true, lightmap_uvs_.get());
        models_.emplace(uuid, asset);
        return asset;
    }

    const std::optional<AssetInfo> info = database_.find(uuid);
    if (!info || info->type != AssetType::Model || info->path.empty()) {
        std::cerr << "[Assets] No hay ningun modelo con UUID " << uuid.toString() << "\n";
        return nullptr;
    }

    std::shared_ptr<ModelAsset> asset = readModel(uuid, info->path, info->name, true, lightmap_uvs_.get());
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
