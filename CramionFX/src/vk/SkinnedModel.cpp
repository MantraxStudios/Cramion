#include "CramionFX/vk/SkinnedModel.h"

#include "CramionFX/asset/Dds.h"
#include "CramionFX/vk/GpuTypes.h"
#include "CramionFX/vk/SkinnedPass.h"
#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanDeviceUpload.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace cramion::gfx {
namespace {

static_assert(sizeof(asset::SkinnedVertex) == 80, "meshlet_common.glsl lee los vertices como 20 floats");

// --- Texturas compartidas entre modelos ---
// La clave es el contenido (MurmurHash64A de los texeles, con el tamano y el
// formato) y el dispositivo: dos modelos con la misma imagen comparten la
// textura de la GPU. La cache solo guarda referencias debiles: la textura vive
// mientras algun modelo la use (los retirados esperan sus frames en vuelo).
std::uint64_t mix64(std::uint64_t a, std::uint64_t b) {
    std::uint64_t h = a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2));
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

std::uint64_t murmur64(const std::uint8_t* data, std::size_t len, std::uint64_t seed) {
    constexpr std::uint64_t m = 0xc6a4a7935bd1e995ULL;
    constexpr int r = 47;
    std::uint64_t h = seed ^ (len * m);
    const std::size_t blocks = len / 8;
    for (std::size_t i = 0; i < blocks; ++i) {
        std::uint64_t k = 0;
        std::memcpy(&k, data + i * 8, 8);
        k *= m;
        k ^= k >> r;
        k *= m;
        h ^= k;
        h *= m;
    }
    const std::uint8_t* tail = data + blocks * 8;
    std::uint64_t t = 0;
    for (std::size_t i = 0; i < (len & 7); ++i) t |= static_cast<std::uint64_t>(tail[i]) << (8 * i);
    if ((len & 7) != 0) {
        h ^= t;
        h *= m;
    }
    h ^= h >> r;
    h *= m;
    h ^= h >> r;
    return h;
}

std::uint64_t textureKey(const asset::TextureData& texture, const void* device) {
    std::uint64_t key = murmur64(texture.pixels.data(), texture.pixels.size(), 0x5eed5eedULL);
    key = mix64(key, (static_cast<std::uint64_t>(texture.width) << 32) | texture.height);
    key = mix64(key, (static_cast<std::uint64_t>(texture.format) << 32) | texture.mip_levels);
    return mix64(key, reinterpret_cast<std::uintptr_t>(device));
}

// Mitad de lado (caja 2x2) de una imagen RGBA8.
std::vector<std::uint8_t> halveRgba(const std::uint8_t* src, std::uint32_t w, std::uint32_t h, std::uint32_t& out_w,
                                    std::uint32_t& out_h) {
    out_w = std::max(w / 2, 1u);
    out_h = std::max(h / 2, 1u);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(out_w) * out_h * 4);
    for (std::uint32_t y = 0; y < out_h; ++y) {
        for (std::uint32_t x = 0; x < out_w; ++x) {
            const std::uint32_t x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
            const std::uint32_t y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
            for (int c = 0; c < 4; ++c) {
                const auto at = [&](std::uint32_t px, std::uint32_t py) {
                    return static_cast<std::uint32_t>(src[(static_cast<std::size_t>(py) * w + px) * 4 + static_cast<std::size_t>(c)]);
                };
                out[(static_cast<std::size_t>(y) * out_w + x) * 4 + static_cast<std::size_t>(c)] =
                    static_cast<std::uint8_t>((at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1) + 2) / 4);
            }
        }
    }
    return out;
}

// Crea una textura RGBA8 sin sus `lod` mips mas grandes (no baja de 32 px).
void createRgbaLod(VulkanTexture& out, const VulkanDevice& device, std::uint32_t w, std::uint32_t h,
                   const std::uint8_t* pixels, int lod, UploadBatch& batch) {
    std::vector<std::uint8_t> current;
    const std::uint8_t* src = pixels;
    for (int i = 0; i < lod && std::min(w, h) >= 64; ++i) {
        std::uint32_t nw = 0, nh = 0;
        current = halveRgba(src, w, h, nw, nh);
        src = current.data();
        w = nw;
        h = nh;
    }
    out.create(device, w, h, src, batch);
}

// Si las texturas perezosas (que no guardan pixeles) tienen alfa recortado:
// se mira al decodificarlas, una vez por textura.
std::mutex g_lazy_alpha_mutex;
std::unordered_map<std::uint64_t, bool> g_lazy_alpha;

// La ruta se normaliza para la clave (separadores y mayusculas, como el
// sistema de archivos de Windows): "Textures/Rock.png" y "textures/ROCK.png"
// son la misma imagen y se suben una vez. Y entra el uso (sRGB o lineal,
// BC1/BC7): resolveLazyTexture decodifica distinto segun el uso, y antes la
// misma imagen como albedo y como mascara compartia la del primero que llegaba.
std::string normalizedTexturePath(std::string path) {
    for (char& c : path) {
        if (c == '\\') c = '/';
#if defined(_WIN32)
        else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
#endif
    }
    return path;
}

// Recarga en caliente (invalidateSharedTextureFile): archivos cambiados en
// disco y cuantas veces. Entran en la clave de las texturas perezosas cuya
// ruta (o receta empaquetada) los contiene: la siguiente subida hace otra
// textura aunque los modelos de antes sigan vivos con la vieja. Casi siempre
// vacio (solo se mira con el atomico).
std::mutex g_file_generation_mutex;
std::vector<std::pair<std::string, std::uint64_t>> g_file_generations;
std::atomic<bool> g_any_file_generation{false};

std::uint64_t lazyTextureKey(const asset::TextureData& texture, const void* device) {
    const std::string path = normalizedTexturePath(texture.source_path);
    std::uint64_t key = murmur64(reinterpret_cast<const std::uint8_t*>(path.data()), path.size(), 0x1a2e5eedULL);
    if (g_any_file_generation.load(std::memory_order_acquire)) {
        const std::lock_guard<std::mutex> lock(g_file_generation_mutex);
        for (const auto& [file, generation] : g_file_generations) {
            if (path.find(file) != std::string::npos) key = mix64(key, 0x4E1A0000ULL + generation);
        }
    }
    key = mix64(key, (texture.height_map ? 1ULL : 0ULL) | (static_cast<std::uint64_t>(asset::maxTextureSize()) << 1));
    key = mix64(key, 0x05A6E000ULL + static_cast<std::uint64_t>(texture.usage));
    return mix64(key, reinterpret_cast<std::uintptr_t>(device));
}

// Cada entrada recuerda tambien el lote que la sube: quien la reutiliza no
// puede dibujarla hasta que ese lote termine (UploadBatch::dependOn). Si la
// fence ya no existe, ese lote termino hace tiempo.
template <typename T>
struct SharedEntry {
    std::weak_ptr<T> resource;
    std::weak_ptr<UploadFence> fence;
};

// Una textura que otro hilo esta decodificando y grabando ahora mismo.
struct TextureInFlight {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::shared_ptr<VulkanTexture> result;  // nulo si fallo
    std::shared_ptr<UploadFence> fence;
};

std::shared_ptr<VulkanTexture> sharedTexture(std::uint64_t key, UploadBatch& batch,
                                             const std::function<void(VulkanTexture&)>& make) {
    static std::mutex mutex;
    static std::unordered_map<std::uint64_t, SharedEntry<VulkanTexture>> cache;
    // Las que se estan haciendo: con varios hilos de streaming y celdas que
    // traen a la vez los mismos materiales, cada hilo decodificaba (o
    // comprimia a BC7) y subia la misma imagen; solo se quedaba una. Ahora el
    // segundo espera al primero (fuera de make, sin ciclos posibles) y depende
    // de su lote.
    static std::unordered_map<std::uint64_t, std::shared_ptr<TextureInFlight>> in_flight;
    std::shared_ptr<TextureInFlight> mine;
    for (int attempt = 0; attempt < 2 && !mine; ++attempt) {
        std::shared_ptr<TextureInFlight> other;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            const auto it = cache.find(key);
            if (it != cache.end()) {
                if (std::shared_ptr<VulkanTexture> alive = it->second.resource.lock()) {
                    batch.dependOn(it->second.fence.lock());
                    return alive;
                }
            }
            const auto flight = in_flight.find(key);
            if (flight == in_flight.end()) {
                mine = std::make_shared<TextureInFlight>();
                in_flight[key] = mine;
            } else {
                other = flight->second;
            }
        }
        if (!other) break;
        // Con tope: si algo raro la retiene, se hace aqui como antes.
        std::unique_lock<std::mutex> wait_lock(other->mutex);
        if (!other->cv.wait_for(wait_lock, std::chrono::seconds(2), [&] { return other->done; })) break;
        if (other->result) {
            batch.dependOn(other->fence);
            return other->result;
        }
        // Fallo en el otro hilo: se intenta aqui (una vez).
    }
    const auto finish_flight = [&](const std::shared_ptr<VulkanTexture>& result, std::shared_ptr<UploadFence> fence) {
        if (!mine) return;
        {
            const std::lock_guard<std::mutex> lock(mine->mutex);
            mine->done = true;
            mine->result = result;
            mine->fence = std::move(fence);
        }
        mine->cv.notify_all();
    };
    const auto drop_flight = [&] {
        if (!mine) return;
        const auto it = in_flight.find(key);
        if (it != in_flight.end() && it->second == mine) in_flight.erase(it);
    };
    auto texture = std::make_shared<VulkanTexture>();
    try {
        make(*texture);
    } catch (...) {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            drop_flight();
        }
        finish_flight(nullptr, nullptr);
        throw;
    }
    // La GPU copia en ella hasta que el lote termina, aunque el modelo se tire antes.
    batch.keepAlive(texture);
    std::shared_ptr<VulkanTexture> shared_result;
    std::shared_ptr<UploadFence> shared_fence;
    struct FinishOnExit {
        const decltype(finish_flight)& finish;
        std::shared_ptr<VulkanTexture>& result;
        std::shared_ptr<UploadFence>& fence;
        ~FinishOnExit() { finish(result, fence); }
    } finish_on_exit{finish_flight, shared_result, shared_fence};
    const std::lock_guard<std::mutex> lock(mutex);
    drop_flight();
    // De vez en cuando, fuera las de modelos que ya no existen.
    if (cache.size() > 4096) {
        for (auto it = cache.begin(); it != cache.end();) {
            it = it->second.resource.expired() ? cache.erase(it) : std::next(it);
        }
    }
    SharedEntry<VulkanTexture>& slot = cache[key];
    if (std::shared_ptr<VulkanTexture> alive = slot.resource.lock()) {  // otro hilo la subio a la vez
        batch.dependOn(slot.fence.lock());
        shared_result = alive;
        shared_fence = slot.fence.lock();
        return alive;
    }
    slot.resource = texture;
    slot.fence = batch.fence();
    shared_result = texture;
    shared_fence = batch.fence();
    return texture;
}

// --- Geometria compartida entre modelos ---
// Igual que las texturas: misma clave de contenido = mismo buffer de la GPU.
// Las variantes de material de RenderSync copian la malla del original tal
// cual (salvo tiling en las UV), asi que comparten vertices e indices.
std::shared_ptr<VulkanBuffer> sharedBuffer(std::uint64_t key, UploadBatch& batch,
                                           const std::function<std::shared_ptr<VulkanBuffer>()>& make) {
    static std::mutex mutex;
    static std::unordered_map<std::uint64_t, SharedEntry<VulkanBuffer>> cache;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto it = cache.find(key);
        if (it != cache.end()) {
            if (std::shared_ptr<VulkanBuffer> alive = it->second.resource.lock()) {
                batch.dependOn(it->second.fence.lock());
                return alive;
            }
        }
    }
    std::shared_ptr<VulkanBuffer> buffer = make();  // (el lote ya lo mantiene vivo)
    const std::lock_guard<std::mutex> lock(mutex);
    if (cache.size() > 4096) {
        for (auto it = cache.begin(); it != cache.end();) {
            it = it->second.resource.expired() ? cache.erase(it) : std::next(it);
        }
    }
    SharedEntry<VulkanBuffer>& slot = cache[key];
    if (std::shared_ptr<VulkanBuffer> alive = slot.resource.lock()) {  // otro hilo lo subio a la vez
        batch.dependOn(slot.fence.lock());
        return alive;
    }
    slot.resource = buffer;
    slot.fence = batch.fence();
    return buffer;
}

template <typename T>
std::uint64_t vectorKey(const std::vector<T>& values, std::uint64_t seed) {
    return mix64(murmur64(reinterpret_cast<const std::uint8_t*>(values.data()), values.size() * sizeof(T), seed),
                 values.size());
}

// UV de lightmap empaquetadas como las lee skinned.vert (unorm16x2, u en los
// 16 bits bajos); (-1, -1) = sin lightmap (0xFFFFFFFF). Igual que
// lighting::packLightmapUv de CramionCore.
std::vector<std::uint32_t> packLightmapUvs(const std::vector<core::Vec2>& uvs) {
    std::vector<std::uint32_t> packed(uvs.size());
    const auto q = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 65535.0f + 0.5f); };
    for (std::size_t i = 0; i < uvs.size(); ++i) {
        const core::Vec2& uv = uvs[i];
        if (uv.x < 0.0f || uv.y < 0.0f) {
            packed[i] = 0xFFFFFFFFu;
            continue;
        }
        const std::uint32_t value = q(uv.x) | (q(uv.y) << 16);
        packed[i] = value == 0xFFFFFFFFu ? 0xFFFEFFFFu : value;
    }
    return packed;
}

// ¿Cada arista (con los vertices soldados por posicion) la comparten
// exactamente dos triangulos? Entonces la malla es cerrada: lo que mira hacia
// atras siempre queda detras de algo que mira hacia delante.
bool isClosedMesh(const asset::ModelData& model, const std::vector<asset::SubMesh>& submeshes,
                  const std::vector<std::uint32_t>& indices, const std::vector<bool>& transparent) {
    std::size_t triangles = 0;
    for (const asset::SubMesh& submesh : submeshes) triangles += submesh.index_count / 3;
    if (triangles == 0 || triangles > 2'000'000) return false;  // enorme: no se comprueba (tiempo de carga)

    struct PositionHash {
        std::size_t operator()(const std::array<std::uint32_t, 3>& p) const {
            return (static_cast<std::size_t>(p[0]) * 73856093u) ^ (static_cast<std::size_t>(p[1]) * 19349663u) ^
                   (static_cast<std::size_t>(p[2]) * 83492791u);
        }
    };
    // Soldado con tolerancia (1/100000 del tamano del modelo): en las costuras
    // de UV las copias de un vertice pueden diferir en el ultimo bit
    // (cos(2 pi) != 1) y la malla parecia abierta.
    core::Vec3 low = model.vertices[0].position;
    core::Vec3 high = low;
    for (const asset::SkinnedVertex& v : model.vertices) {
        low = core::Vec3{std::min(low.x, v.position.x), std::min(low.y, v.position.y), std::min(low.z, v.position.z)};
        high = core::Vec3{std::max(high.x, v.position.x), std::max(high.y, v.position.y), std::max(high.z, v.position.z)};
    }
    const float extent = std::max({high.x - low.x, high.y - low.y, high.z - low.z, 1e-6f});
    const float cell = extent * 1e-5f;
    std::unordered_map<std::array<std::uint32_t, 3>, std::uint32_t, PositionHash> welded;
    welded.reserve(triangles * 2);
    const auto weld = [&](std::uint32_t vertex) {
        const core::Vec3& p = model.vertices[vertex].position;
        const std::array<std::uint32_t, 3> key = {static_cast<std::uint32_t>(std::lround((p.x - low.x) / cell)),
                                                  static_cast<std::uint32_t>(std::lround((p.y - low.y) / cell)),
                                                  static_cast<std::uint32_t>(std::lround((p.z - low.z) / cell))};
        return welded.emplace(key, static_cast<std::uint32_t>(welded.size())).first->second;
    };
    std::unordered_map<std::uint64_t, std::uint32_t> edges;
    edges.reserve(triangles * 3);
    for (const asset::SubMesh& submesh : submeshes) {
        if (submesh.material < transparent.size() && transparent[submesh.material]) continue;
        for (std::uint32_t t = 0; t + 2 < submesh.index_count; t += 3) {
            const std::uint32_t v[3] = {weld(indices[submesh.first_index + t]), weld(indices[submesh.first_index + t + 1]),
                                        weld(indices[submesh.first_index + t + 2])};
            if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) continue;  // degenerado
            for (int e = 0; e < 3; ++e) {
                const std::uint32_t a = std::min(v[e], v[(e + 1) % 3]);
                const std::uint32_t b = std::max(v[e], v[(e + 1) % 3]);
                ++edges[(static_cast<std::uint64_t>(a) << 32) | b];
            }
        }
    }
    if (edges.empty()) return false;
    for (const auto& [edge, count] : edges) {
        if (count != 2) return false;
    }
    return true;
}

// Formato de Vulkan de una textura comprimida del DDS. Variantes UNORM: el
// shader linealiza el color base a mano.
vk::Format blockFormat(asset::TextureFormat format) {
    switch (format) {
        case asset::TextureFormat::Bc1: return vk::Format::eBc1RgbaUnormBlock;
        case asset::TextureFormat::Bc2: return vk::Format::eBc2UnormBlock;
        case asset::TextureFormat::Bc3: return vk::Format::eBc3UnormBlock;
        case asset::TextureFormat::Bc4: return vk::Format::eBc4UnormBlock;
        case asset::TextureFormat::Bc5: return vk::Format::eBc5UnormBlock;
        case asset::TextureFormat::Bc7: return vk::Format::eBc7UnormBlock;
        case asset::TextureFormat::Rgba8: break;
    }
    return vk::Format::eR8G8B8A8Unorm;
}

// El color base tiene zonas recortadas por alfa (el raster las descarta con
// alfa < 0.5). DXT1 se toma como opaco; DXT3/5 y BC7 casi siempre llevan alfa
// cuando se eligen; RGBA8 se comprueba.
bool hasAlpha(const asset::TextureData& texture) {
    if (texture.alpha >= 0) return texture.alpha == 1;  // lo midio la compresion
    switch (texture.format) {
        case asset::TextureFormat::Rgba8:
            for (std::size_t i = 3; i < texture.pixels.size(); i += 4) {
                if (texture.pixels[i] < 128) {
                    return true;
                }
            }
            return false;
        case asset::TextureFormat::Bc2:
        case asset::TextureFormat::Bc3:
        case asset::TextureFormat::Bc7:
            return true;
        default:
            return false;
    }
}

}  // namespace

void invalidateSharedTextureFile(const std::string& path) {
    const std::string file = normalizedTexturePath(path);
    if (file.empty()) return;
    const std::lock_guard<std::mutex> lock(g_file_generation_mutex);
    const auto it = std::find_if(g_file_generations.begin(), g_file_generations.end(),
                                 [&](const auto& entry) { return entry.first == file; });
    if (it != g_file_generations.end()) {
        ++it->second;
    } else {
        g_file_generations.emplace_back(file, 1);
    }
    g_any_file_generation.store(true, std::memory_order_release);
}

std::uint32_t SkinnedModel::shadingBits(const asset::MaterialData& material) {
    const auto byte = [](float value) {
        return static_cast<std::uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    int model = std::clamp(material.shading_model, 0, 6);
    // La transmision es del vidrio (pasada forward); en lo opaco, estandar.
    if (model == 5 && !material.transparent) model = 0;
    float p0 = material.specular_tint;
    float p1 = 0.0f;
    float p2 = 0.0f;
    switch (model) {
        case 1: p0 = material.clearcoat; p1 = material.clearcoat_roughness; p2 = material.specular_tint; break;
        case 2: p0 = material.sheen; p1 = material.sheen_tint; p2 = material.specular_tint; break;
        case 3:
            p0 = material.subsurface;
            p1 = material.translucency;
            p2 = (material.subsurface_thickness - 0.01f) / 0.29f;
            break;
        case 4: p0 = material.anisotropy; p1 = material.anisotropy_rotation / 180.0f; p2 = material.specular_tint; break;
        case 5: p0 = (material.ior - 1.0f) / 1.5f; p1 = material.transmission_thickness / 0.2f; break;
        case 6: p0 = material.hair_shift; p1 = material.anisotropy_rotation / 180.0f; p2 = material.hair_secondary; break;
        default: break;
    }
    // Sin luz y opaco: el modelo 8 del G-buffer (lighting.frag deja albedo +
    // emision). Transparente: el bit de unlit de glass.frag.
    if (material.unlit && !material.transparent) model = kShadingUnlit;
    std::uint32_t bits = static_cast<std::uint32_t>(model) | (byte(p0) << 8) | (byte(p1) << 16) | (byte(p2) << 24);
    if (material.transparent) {
        bits |= (static_cast<std::uint32_t>(std::min<std::uint8_t>(material.transparent_blend, 2)) << kBlendShift);
        if (material.unlit) bits |= kShadingUnlitBit;
    }
    if (material.vertex_color_albedo) bits |= kShadingVertexColorBit;
    return bits;
}

std::uint32_t SkinnedModel::reliefFlags(std::uint32_t flags, const asset::MaterialData& material, bool has_height,
                                        bool tessellation) {
    flags &= ~(GpuSkinnedPush::kFlagHeightMap | GpuSkinnedPush::kFlagTessellation |
               GpuSkinnedPush::kFlagParallaxShadow | GpuSkinnedPush::kTessFactorMask);
    if (!has_height) return flags;
    if (material.tessellation && tessellation) {
        const auto factor = static_cast<std::uint32_t>(std::clamp(material.tessellation_density, 1.0f, 64.0f) + 0.5f);
        return flags | GpuSkinnedPush::kFlagTessellation | (factor << GpuSkinnedPush::kTessFactorShift);
    }
    // Parallax (tambien si se pidio teselacion y la GPU no la tiene).
    flags |= GpuSkinnedPush::kFlagHeightMap;
    if (material.parallax_shadows) flags |= GpuSkinnedPush::kFlagParallaxShadow;
    return flags;
}

void SkinnedModel::create(const VulkanDevice& device, const asset::ModelData& model,
                          const SkinnedPass& pass, int texture_lod, bool wait_for_gpu) {
    destroy();
    texture_lod_ = std::clamp(texture_lod, 0, 4);

    // Un lote para todo el modelo: antes cada buffer y cada textura esperaba
    // a la GPU por separado (un modelo con 30 texturas, ~32 esperas).
    const auto batch = std::make_shared<UploadBatch>(device);

    // (Tambien storage: los mesh shaders leen los vertices de los meshlets.)
    const std::uint64_t device_key = reinterpret_cast<std::uintptr_t>(&device);
    vertices_ = sharedBuffer(mix64(vectorKey(model.vertices.get(), 0x7e47e4ULL), device_key), *batch, [&] {
        return VulkanBuffer::createDeviceLocal(
            device, model.vertices.data(), sizeof(asset::SkinnedVertex) * model.vertices.size(),
            vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eStorageBuffer, *batch);
    });
    // Indices de LOD0 y detras los de los LODs (sus submallas se desplazan).
    const std::uint64_t index_key =
        mix64(mix64(vectorKey(model.indices.get(), 0x1d1ce5ULL), vectorKey(model.lod_indices.get(), 0x10d1ULL)), device_key);
    indices_ = sharedBuffer(index_key, *batch, [&] {
        if (model.lod_indices.empty()) {
            return VulkanBuffer::createDeviceLocal(device, model.indices.data(),
                                                   sizeof(std::uint32_t) * model.indices.size(),
                                                   vk::BufferUsageFlagBits::eIndexBuffer, *batch);
        }
        std::vector<std::uint32_t> all;
        all.reserve(model.indices.size() + model.lod_indices.size());
        all.insert(all.end(), model.indices.begin(), model.indices.end());
        all.insert(all.end(), model.lod_indices.begin(), model.lod_indices.end());
        return VulkanBuffer::createDeviceLocal(device, all.data(), sizeof(std::uint32_t) * all.size(),
                                               vk::BufferUsageFlagBits::eIndexBuffer, *batch);
    });
    index_count_ = static_cast<std::uint32_t>(model.indices.size());
    // UV de lightmap (PLAN-LIGHTMAPS 1.6): una por vertice, compartidas como
    // los vertices (las variantes de material las copian del original).
    lightmap_uvs_.reset();
    if (!model.lightmap_uvs.empty() && model.lightmap_uvs.size() == model.vertices.size()) {
        lightmap_uvs_ = sharedBuffer(mix64(vectorKey(model.lightmap_uvs, 0x11e3a9ULL), device_key), *batch, [&] {
            const std::vector<std::uint32_t> packed = packLightmapUvs(model.lightmap_uvs);
            return VulkanBuffer::createDeviceLocal(device, packed.data(), sizeof(std::uint32_t) * packed.size(),
                                                   vk::BufferUsageFlagBits::eStorageBuffer, *batch);
        });
    }
    // Color de vertice (COLOR_0): uno por vertice, compartido igual.
    vertex_colors_.reset();
    if (!model.vertex_colors.empty() && model.vertex_colors.size() == model.vertices.size()) {
        vertex_colors_ = sharedBuffer(mix64(vectorKey(model.vertex_colors.get(), 0xc0102ULL), device_key), *batch, [&] {
            return VulkanBuffer::createDeviceLocal(device, model.vertex_colors.data(),
                                                   sizeof(std::uint32_t) * model.vertex_colors.size(),
                                                   vk::BufferUsageFlagBits::eStorageBuffer, *batch);
        });
    }
    submeshes_ = model.submeshes;
    lods_.clear();
    for (const asset::MeshLod& lod : model.lods) {
        Lod gpu{};
        gpu.submeshes = lod.submeshes;
        for (asset::SubMesh& submesh : gpu.submeshes) submesh.first_index += index_count_;
        gpu.error = lod.error;
        lods_.push_back(std::move(gpu));
    }
    rigid_ = model.animations.empty();

    // --- Texturas, con los texeles por defecto al final ---
    // La misma imagen en varios modelos (materiales compartidos) se sube una
    // vez: antes cada casa de un pueblo llevaba su copia de todas y la VRAM
    // se llenaba (decenas de modelos x cientos de MB).
    textures_.reserve(model.textures.size() + 3);
    const int lod = texture_lod_;
    std::vector<bool> lazy_alpha(model.textures.size(), false);
    for (std::size_t t = 0; t < model.textures.size(); ++t) {
        const asset::TextureData& stored = model.textures[t];
        const bool lazy = asset::isLazyTexture(stored);
        const std::uint64_t key = mix64(lazy ? lazyTextureKey(stored, &device) : textureKey(stored, &device),
                                        0x10D0000ull + static_cast<std::uint64_t>(lod));
        textures_.push_back(sharedTexture(key, *batch, [&](VulkanTexture& out) {
            // Perezosa: se decodifica aqui (este hilo) y se tira al subirla.
            asset::TextureData decoded;
            if (lazy) {
                if (!asset::resolveLazyTexture(stored, decoded)) {
                    std::cerr << "[Vulkan] No se pudo leer la textura " << stored.name << "\n";
                    decoded.width = decoded.height = 1;
                    decoded.format = asset::TextureFormat::Rgba8;
                    decoded.pixels = {255, 255, 255, 255};
                }
                const bool alpha = hasAlpha(decoded);
                const std::lock_guard<std::mutex> lock(g_lazy_alpha_mutex);
                g_lazy_alpha[key] = alpha;
            }
            const asset::TextureData& texture = lazy ? decoded : stored;
            if (texture.format == asset::TextureFormat::Rgba8) {
                createRgbaLod(out, device, texture.width, texture.height, texture.pixels.data(), lod, *batch);
            } else if (!device.textureCompressionBcSupported()) {
                // GPU sin BC (casi todas las de movil): se descomprime aqui (el
                // nivel 0; los mips los hace la GPU). Antes el modelo entero
                // fallaba y el juego no arrancaba en esos moviles.
                const std::vector<std::uint8_t> rgba = asset::decodeBlockCompressed(texture);
                if (rgba.empty()) throw std::runtime_error("Textura comprimida con datos incompletos: " + texture.name);
                createRgbaLod(out, device, texture.width, texture.height, rgba.data(), lod, *batch);
            } else {
                // Comprimida con mips: se salta los `lod` mas grandes (si
                // tiene; sin bajar de 16 px).
                std::uint32_t w = texture.width;
                std::uint32_t h = texture.height;
                std::uint32_t mips = texture.mip_levels;
                std::size_t offset = 0;
                const std::uint32_t block = asset::blockBytes(texture.format);
                for (int i = 0; i < lod && mips > 1 && std::min(w, h) >= 32; ++i) {
                    offset += static_cast<std::size_t>(std::max((w + 3) / 4, 1u)) * std::max((h + 3) / 4, 1u) * block;
                    w = std::max(w / 2, 1u);
                    h = std::max(h / 2, 1u);
                    --mips;
                }
                if (offset >= texture.pixels.size()) {
                    offset = 0;
                    w = texture.width;
                    h = texture.height;
                    mips = texture.mip_levels;
                }
                out.createCompressed(device, w, h, blockFormat(texture.format), block, mips,
                                     texture.pixels.data() + offset, texture.pixels.size() - offset, *batch);
            }
        }));
        if (lazy) {
            const std::lock_guard<std::mutex> lock(g_lazy_alpha_mutex);
            const auto it = g_lazy_alpha.find(key);
            lazy_alpha[t] = it != g_lazy_alpha.end() && it->second;
        }
    }
    const auto add_texel = [&](std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        const std::array<std::uint8_t, 4> texel = {r, g, b, 255};
        const std::uint64_t key = mix64(0x7e7e1u ^ (static_cast<std::uint64_t>(r) << 16 | static_cast<std::uint64_t>(g) << 8 | b),
                                        reinterpret_cast<std::uintptr_t>(&device));
        textures_.push_back(
            sharedTexture(key, *batch, [&](VulkanTexture& out) { out.create(device, 1, 1, texel.data(), *batch); }));
        return textures_.size() - 1;
    };
    const std::size_t white_index = add_texel(255, 255, 255);
    // (0.5, 0.5, 1) en espacio tangente = la normal de la malla sin cambios.
    const std::size_t flat_normal_index = add_texel(128, 128, 255);
    const std::size_t black_index = add_texel(0, 0, 0);
    white_index_ = white_index;
    black_index_ = black_index;
    render_texture_refs_.clear();

    // --- Un descriptor set por material ---
    const auto material_count = static_cast<std::uint32_t>(model.materials.size());

    // (+ por material: un uniform buffer, el tiling de las UV, binding 9, y
    // dos storage buffers, las UV de lightmap y el color de vertice, 10 y 11.)
    const std::array<vk::DescriptorPoolSize, 3> pool_sizes = {
        vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler,
                               material_count * SkinnedPass::kMaterialBindingCount},
        vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, std::max(material_count, 1u)},
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, std::max(material_count, 1u) * 2}};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = material_count;
    pool_info.setPoolSizes(pool_sizes);
    pool_ = vk::raii::DescriptorPool(device.handle(), pool_info);

    const std::vector<vk::DescriptorSetLayout> layouts(material_count, *pass.materialSetLayout());
    vk::DescriptorSetAllocateInfo alloc_info{};
    alloc_info.descriptorPool = *pool_;
    alloc_info.setSetLayouts(layouts);
    material_sets_ = vk::raii::DescriptorSets(device.handle(), alloc_info);

    const auto pick = [](std::int32_t texture, std::size_t fallback) {
        return (texture >= 0) ? static_cast<std::size_t>(texture) : fallback;
    };

    // --- Tiling de las UV (binding kMaterialUvBinding de cada set) ---
    // Un vec4 por material (xy repeticion, zw desplazamiento), cada uno en su
    // offset alineado. Compartido por contenido como la geometria: los modelos
    // sin tiling (casi todos) usan el mismo buffer de identidad, y las
    // variantes con el mismo tiling, el mismo.
    const auto uv_transform = [](const asset::MaterialData& material) {
        return core::Vec4{material.uv_scale.x, material.uv_scale.y, material.uv_offset.x, material.uv_offset.y};
    };
    const vk::DeviceSize uv_stride =
        std::max<vk::DeviceSize>(device.limits().minUniformBufferOffsetAlignment, sizeof(core::Vec4));
    bool tiled = false;
    for (const asset::MaterialData& material : model.materials) {
        tiled = tiled || material.uv_scale.x != 1.0f || material.uv_scale.y != 1.0f || material.uv_offset.x != 0.0f ||
                material.uv_offset.y != 0.0f;
    }
    std::vector<std::uint8_t> uv_data(static_cast<std::size_t>(uv_stride) * (tiled ? material_count : 1u), 0);
    for (std::uint32_t m = 0; m < (tiled ? material_count : 1u); ++m) {
        const core::Vec4 value = tiled ? uv_transform(model.materials[m]) : core::Vec4{1.0f, 1.0f, 0.0f, 0.0f};
        std::memcpy(uv_data.data() + static_cast<std::size_t>(uv_stride) * m, &value, sizeof(value));
    }
    uv_buffer_ = sharedBuffer(mix64(vectorKey(uv_data, 0x71e5ULL), device_key), *batch, [&] {
        return VulkanBuffer::createDeviceLocal(device, uv_data.data(), uv_data.size(),
                                               vk::BufferUsageFlagBits::eUniformBuffer, *batch);
    });

    materials_.reserve(material_count);
    material_views_.assign(material_count, std::vector<vk::ImageView>(SkinnedPass::kMaterialBindingCount));
    material_sampler_revision_ = pass.samplerRevision();
    for (std::uint32_t m = 0; m < material_count; ++m) {
        const asset::MaterialData& material = model.materials[m];

        Material gpu{};
        gpu.base_color = material.base_color;
        const bool parallax = material.height_scale > 0.0f && material.occlusion_texture >= 0;
        gpu.emissive = core::Vec4{material.emissive.x, material.emissive.y, material.emissive.z,
                                  parallax ? material.height_scale : 0.0f};
        gpu.shader_flags = material.specular_map && material.metallic_roughness_texture >= 0
                               ? GpuSkinnedPush::kFlagSpecularMap
                               : 0u;
        gpu.shader_flags = reliefFlags(gpu.shader_flags, material, parallax, pass.tessellationEnabled());
        if (lightmap_uvs_) gpu.shader_flags |= GpuSkinnedPush::kFlagLightmap;
        if (vertex_colors_) gpu.shader_flags |= GpuSkinnedPush::kFlagVertexColor;
        gpu.shading = shadingBits(material);
        // El signo de la escala del normal map dice su convenio (ver
        // skinned.frag): positivo = OpenGL, negativo = DirectX.
        gpu.params = core::Vec4{material.metallic, material.roughness, material.occlusion_strength,
                                material.normal_map_directx ? -material.normal_scale
                                                            : material.normal_scale};
        gpu.reflectance = material.reflectance;
        gpu.transparent = material.transparent;
        gpu.surface_shader = material.surface_shader;
        gpu.surface_params = material.surface_params;
        gpu.uv_transform = uv_transform(material);

        // Sin textura emisiva, el factor multiplica al blanco: un material
        // emisivo liso (glTF sin mapa) sigue brillando.
        const bool emits =
            material.emissive.x > 0.0f || material.emissive.y > 0.0f || material.emissive.z > 0.0f;

        // Mismo orden que los bindings del set 1 (ver SkinnedPass).
        // (+ las 4 del shader de superficie del usuario, blancas si no hay.)
        const std::array<std::size_t, SkinnedPass::kMaterialBindingCount> textures = {
            pick(material.albedo_texture, white_index),
            pick(material.metallic_roughness_texture, white_index),
            pick(material.normal_texture, flat_normal_index),
            pick(material.occlusion_texture, white_index),
            pick(material.emissive_texture, emits ? white_index : black_index),
            pick(material.surface_textures[0], white_index),
            pick(material.surface_textures[1], white_index),
            pick(material.surface_textures[2], white_index),
            pick(material.surface_textures[3], white_index),
        };
        gpu.albedo_texture = static_cast<std::uint32_t>(textures[0]);
        gpu.metallic_roughness_texture = static_cast<std::uint32_t>(textures[1]);
        gpu.emissive_texture = static_cast<std::uint32_t>(textures[4]);
        if (material.albedo_texture >= 0) {
            const auto albedo = static_cast<std::size_t>(material.albedo_texture);
            gpu.alpha_masked = albedo < lazy_alpha.size() && asset::isLazyTexture(model.textures[albedo])
                                   ? lazy_alpha[albedo]
                                   : hasAlpha(model.textures[albedo]);
        }
        // Un factor de alfa bajo recorta aunque la textura sea opaca (como en
        // skinned.frag): la sombra y los rayos tienen que recortarlo igual.
        gpu.texture_alpha = gpu.alpha_masked;
        gpu.alpha_masked = gpu.texture_alpha || gpu.base_color.w < 0.5f;
        // BC1 que no viene de la cache de texturas (un DDS del artista): puede
        // llevar el alfa de 1 bit, asi que se trata como recortada.
        bool unknown_alpha = false;
        if (material.albedo_texture >= 0 && static_cast<std::size_t>(material.albedo_texture) < model.textures.size()) {
            const asset::TextureData& albedo = model.textures[static_cast<std::size_t>(material.albedo_texture)];
            unknown_alpha = !asset::isLazyTexture(albedo) && albedo.alpha < 0 && albedo.format == asset::TextureFormat::Bc1;
        }
        // Con texture_alpha (no alpha_masked): el factor base_color.w cambia en
        // vivo y bindMaterialPipeline lo mira aparte.
        gpu.opaque_albedo = !gpu.texture_alpha && !unknown_alpha && material.albedo_render_texture < 0;
        materials_.push_back(gpu);
        if (material.albedo_render_texture >= 0) render_texture_refs_.push_back({m, 0, material.albedo_render_texture});
        if (material.emissive_render_texture >= 0) render_texture_refs_.push_back({m, 4, material.emissive_render_texture});

        std::array<vk::DescriptorImageInfo, SkinnedPass::kMaterialBindingCount> infos{};
        for (std::uint32_t t = 0; t < SkinnedPass::kMaterialBindingCount; ++t) {
            infos[t].sampler = *pass.sampler();
            infos[t].imageView = *textures_[textures[t]]->view();
            infos[t].imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
            material_views_[m][t] = infos[t].imageView;
        }

        std::array<vk::WriteDescriptorSet, SkinnedPass::kMaterialBindingCount + 1> writes{};
        for (std::uint32_t t = 0; t < SkinnedPass::kMaterialBindingCount; ++t) {
            writes[t].dstSet = *material_sets_[m];
            writes[t].dstBinding = t;
            writes[t].descriptorType = vk::DescriptorType::eCombinedImageSampler;
            writes[t].setImageInfo(infos[t]);
        }
        // Modo compatible: el set solo tiene las texturas PBR (y el tiling,
        // que va justo detras de las que se escriben).
        const std::uint32_t texture_writes = SkinnedPass::materialBindingCount();
        const vk::DescriptorBufferInfo uv_info{*uv_buffer_->handle(), tiled ? uv_stride * m : 0,
                                               sizeof(core::Vec4)};
        vk::WriteDescriptorSet& uv_write = writes[texture_writes];
        uv_write = vk::WriteDescriptorSet{};
        uv_write.dstSet = *material_sets_[m];
        uv_write.dstBinding = SkinnedPass::kMaterialUvBinding;
        uv_write.descriptorType = vk::DescriptorType::eUniformBuffer;
        uv_write.setBufferInfo(uv_info);
        device.handle().updateDescriptorSets(
            vk::ArrayProxy<const vk::WriteDescriptorSet>(texture_writes + 1, writes.data()), nullptr);
        // Binding 10: sus UV de lightmap (o el buffer ficticio de la pasada).
        vk::DescriptorBufferInfo lightmap_info{};
        lightmap_info.buffer = lightmap_uvs_ ? *lightmap_uvs_->handle() : *pass.dummyLightmapUvs().handle();
        lightmap_info.range = VK_WHOLE_SIZE;
        vk::WriteDescriptorSet lightmap_write{};
        lightmap_write.dstSet = *material_sets_[m];
        lightmap_write.dstBinding = SkinnedPass::kLightmapUvBinding;
        lightmap_write.descriptorType = vk::DescriptorType::eStorageBuffer;
        lightmap_write.setBufferInfo(lightmap_info);
        // Binding 11: su color de vertice (o el mismo ficticio).
        vk::DescriptorBufferInfo color_info{};
        color_info.buffer = vertex_colors_ ? *vertex_colors_->handle() : *pass.dummyLightmapUvs().handle();
        color_info.range = VK_WHOLE_SIZE;
        vk::WriteDescriptorSet color_write = lightmap_write;
        color_write.dstBinding = SkinnedPass::kVertexColorBinding;
        color_write.setBufferInfo(color_info);
        const std::array<vk::WriteDescriptorSet, 2> buffer_writes = {lightmap_write, color_write};
        device.handle().updateDescriptorSets(buffer_writes, nullptr);
    }

    // --- Grupos de dibujo (un grupo por material con submallas opacas) ---
    draw_groups_.clear();
    submesh_groups_.assign(submeshes_.size(), kNoGroup);
    std::vector<std::uint32_t> material_group(materials_.size(), kNoGroup);
    for (std::uint32_t i = 0; i < submeshes_.size(); ++i) {
        const std::uint32_t material = submeshes_[i].material;
        if (materials_[material].transparent) {
            continue;
        }
        if (material_group[material] == kNoGroup) {
            material_group[material] = static_cast<std::uint32_t>(draw_groups_.size());
            draw_groups_.push_back(DrawGroup{material, 0, 0});
        }
        submesh_groups_[i] = material_group[material];
        ++draw_groups_[material_group[material]].capacity;
    }
    // Las submallas de los LODs, al grupo de su material (un material que
    // solo aparece en un LOD no pasa: generateLods no crea materiales).
    for (Lod& lod : lods_) {
        lod.groups.resize(lod.submeshes.size());
        for (std::size_t i = 0; i < lod.submeshes.size(); ++i) {
            const std::uint32_t material = lod.submeshes[i].material;
            lod.groups[i] = material < material_group.size() ? material_group[material] : kNoGroup;
        }
    }
    slot_count_ = 0;
    for (DrawGroup& group : draw_groups_) {
        group.first_slot = slot_count_;
        slot_count_ += group.capacity;
    }

    buildMeshlets(device, model, pass, *batch);

    // Todo grabado: un envio (sin esperar a la GPU salvo que se pida).
    upload_ = batch->submit();
    if (wait_for_gpu) waitUpload();

    if (VulkanDevice::debugLog()) {
        std::cout << "[Vulkan] Modelo " << model.name << " subido: " << model.textures.size()
                  << " texturas con mipmaps, " << material_count << " materiales\n";
    }
}

bool SkinnedModel::uploadReady() const {
    if (!upload_) return true;
    if (!upload_->done()) return false;
    upload_.reset();
    return true;
}

void SkinnedModel::waitUpload() const {
    if (!upload_) return;
    upload_->wait();
    upload_.reset();
}

void SkinnedModel::buildMeshlets(const VulkanDevice& device, const asset::ModelData& model, const SkinnedPass& pass,
                                 UploadBatch& batch) {
    meshlet_ranges_.clear();
    closed_.clear();
    max_cluster_meshlets_ = 0;
    // Solo escenarios (rigidos): los animados se deforman y sus esferas y conos no valdrian.
    if (!pass.meshShadersEnabled() || !rigid_ || model.vertices.empty()) return;

    constexpr std::size_t kMaxVertices = 64;
    constexpr std::size_t kMaxTriangles = 124;
    constexpr float kConeWeight = 0.25f;
    const float* positions = &model.vertices[0].position.x;
    const std::size_t vertex_count = model.vertices.size();
    constexpr std::size_t kStride = sizeof(asset::SkinnedVertex);

    std::vector<bool> transparent(materials_.size());
    for (std::size_t m = 0; m < materials_.size(); ++m) transparent[m] = materials_[m].transparent;

    std::vector<GpuMeshlet> gpu;
    std::vector<std::uint32_t> all_vertices;
    std::vector<std::uint8_t> all_triangles;
    std::vector<meshopt_Meshlet> built;
    std::vector<unsigned int> built_vertices;
    std::vector<unsigned char> built_triangles;
    for (std::uint32_t lod = 0; lod < lodCount(); ++lod) {
        const std::vector<asset::SubMesh>& submeshes = lod == 0 ? model.submeshes : model.lods[lod - 1].submeshes;
        const std::vector<std::uint32_t>& indices = lod == 0 ? model.indices : model.lod_indices;
        std::vector<MeshletRange> ranges(submeshes.size());
        for (std::size_t s = 0; s < submeshes.size(); ++s) {
            const asset::SubMesh& submesh = submeshes[s];
            if (submesh.index_count < 3 || (submesh.material < transparent.size() && transparent[submesh.material])) {
                continue;
            }
            const std::size_t bound = meshopt_buildMeshletsBound(submesh.index_count, kMaxVertices, kMaxTriangles);
            built.resize(bound);
            built_vertices.resize(bound * kMaxVertices);
            built_triangles.resize(bound * kMaxTriangles * 3);
            const std::size_t count = meshopt_buildMeshlets(
                built.data(), built_vertices.data(), built_triangles.data(), indices.data() + submesh.first_index,
                submesh.index_count, positions, vertex_count, kStride, kMaxVertices, kMaxTriangles, kConeWeight);
            ranges[s] = MeshletRange{static_cast<std::uint32_t>(gpu.size()), static_cast<std::uint32_t>(count)};
            max_cluster_meshlets_ = std::max(max_cluster_meshlets_, static_cast<std::uint32_t>(count));
            for (std::size_t i = 0; i < count; ++i) {
                const meshopt_Meshlet& m = built[i];
                const meshopt_Bounds bounds =
                    meshopt_computeMeshletBounds(&built_vertices[m.vertex_offset], &built_triangles[m.triangle_offset],
                                                 m.triangle_count, positions, vertex_count, kStride);
                GpuMeshlet out{};
                out.center_radius = core::Vec4{bounds.center[0], bounds.center[1], bounds.center[2], bounds.radius};
                out.cone_axis_cutoff =
                    core::Vec4{bounds.cone_axis[0], bounds.cone_axis[1], bounds.cone_axis[2], bounds.cone_cutoff};
                out.cone_apex = core::Vec4{bounds.cone_apex[0], bounds.cone_apex[1], bounds.cone_apex[2], 0.0f};
                out.vertex_offset = static_cast<std::uint32_t>(all_vertices.size());
                out.triangle_offset = static_cast<std::uint32_t>(all_triangles.size());
                out.vertex_count = m.vertex_count;
                out.triangle_count = m.triangle_count;
                all_vertices.insert(all_vertices.end(), built_vertices.begin() + m.vertex_offset,
                                    built_vertices.begin() + m.vertex_offset + m.vertex_count);
                all_triangles.insert(all_triangles.end(), built_triangles.begin() + m.triangle_offset,
                                     built_triangles.begin() + m.triangle_offset + m.triangle_count * 3);
                gpu.push_back(out);
            }
        }
        meshlet_ranges_.push_back(std::move(ranges));
        // LOD0 se comprueba; los simplificados heredan (el simplificador no abre mallas cerradas).
        closed_.push_back(lod == 0 ? isClosedMesh(model, submeshes, indices, transparent) : closed_[0]);
    }
    if (gpu.empty()) {
        meshlet_ranges_.clear();
        closed_.clear();
        return;
    }
    while (all_triangles.size() % 4 != 0) all_triangles.push_back(0);

    const auto storage = vk::BufferUsageFlagBits::eStorageBuffer;
    meshlets_ = VulkanBuffer::createDeviceLocal(device, gpu.data(), sizeof(GpuMeshlet) * gpu.size(), storage, batch);
    meshlet_vertices_ = VulkanBuffer::createDeviceLocal(device, all_vertices.data(),
                                                        sizeof(std::uint32_t) * all_vertices.size(), storage, batch);
    meshlet_triangles_ =
        VulkanBuffer::createDeviceLocal(device, all_triangles.data(), all_triangles.size(), storage, batch);

    const vk::DescriptorPoolSize pool_size{vk::DescriptorType::eStorageBuffer, 4};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = 1;
    pool_info.setPoolSizes(pool_size);
    meshlet_pool_ = vk::raii::DescriptorPool(device.handle(), pool_info);
    const vk::DescriptorSetLayout layout = *pass.meshletSetLayout();
    vk::DescriptorSetAllocateInfo alloc{};
    alloc.descriptorPool = *meshlet_pool_;
    alloc.setSetLayouts(layout);
    meshlet_sets_ = vk::raii::DescriptorSets(device.handle(), alloc);
    const std::array<vk::DescriptorBufferInfo, 4> infos = {
        vk::DescriptorBufferInfo{*vertices().handle(), 0, VK_WHOLE_SIZE},
        vk::DescriptorBufferInfo{*meshlets_->handle(), 0, VK_WHOLE_SIZE},
        vk::DescriptorBufferInfo{*meshlet_vertices_->handle(), 0, VK_WHOLE_SIZE},
        vk::DescriptorBufferInfo{*meshlet_triangles_->handle(), 0, VK_WHOLE_SIZE}};
    std::array<vk::WriteDescriptorSet, 4> writes{};
    for (std::uint32_t i = 0; i < 4; ++i) {
        writes[i].dstSet = *meshlet_sets_[0];
        writes[i].dstBinding = i;
        writes[i].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[i].setBufferInfo(infos[i]);
    }
    device.handle().updateDescriptorSets(writes, nullptr);
    if (VulkanDevice::debugLog()) {
        std::cout << "[Vulkan] " << model.name << ": " << gpu.size() << " meshlets en " << lodCount() << " LODs"
                  << (closed_[0] ? " (malla cerrada)" : "") << "\n";
    }
}

void SkinnedModel::setMaterialImage(const VulkanDevice& device, const SkinnedPass& pass, std::uint32_t material,
                                    std::uint32_t binding, vk::ImageView view) {
    if (material >= material_sets_.size()) return;
    if (!view) view = *textures_[binding == 4 ? black_index_ : white_index_]->view();
    vk::DescriptorImageInfo info{};
    info.sampler = *pass.sampler();
    info.imageView = view;
    info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    vk::WriteDescriptorSet write{};
    write.dstSet = *material_sets_[material];
    write.dstBinding = binding;
    write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    write.setImageInfo(info);
    device.handle().updateDescriptorSets(write, nullptr);
    if (material < material_views_.size() && binding < material_views_[material].size()) {
        material_views_[material][binding] = view;
    }
}

void SkinnedModel::refreshSampler(const VulkanDevice& device, const SkinnedPass& pass) {
    const std::uint32_t count = SkinnedPass::materialBindingCount();
    std::vector<vk::DescriptorImageInfo> infos;
    std::vector<vk::WriteDescriptorSet> writes;
    infos.reserve(static_cast<std::size_t>(material_sets_.size()) * count);
    writes.reserve(infos.capacity());
    for (std::size_t m = 0; m < material_sets_.size() && m < material_views_.size(); ++m) {
        for (std::uint32_t t = 0; t < count && t < material_views_[m].size(); ++t) {
            if (!material_views_[m][t]) continue;
            infos.push_back(vk::DescriptorImageInfo{*pass.sampler(), material_views_[m][t],
                                                    vk::ImageLayout::eShaderReadOnlyOptimal});
            vk::WriteDescriptorSet write{};
            write.dstSet = *material_sets_[m];
            write.dstBinding = t;
            write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
            write.setImageInfo(infos.back());  // reservado: la direccion no cambia
            writes.push_back(write);
        }
    }
    if (!writes.empty()) device.handle().updateDescriptorSets(writes, nullptr);
    material_sampler_revision_ = pass.samplerRevision();
}

void SkinnedModel::destroy() {
    meshlet_sets_.clear();
    meshlet_pool_ = nullptr;
    // (Si la GPU aun los copia, el lote de subida los mantiene vivos.)
    meshlets_.reset();
    meshlet_vertices_.reset();
    meshlet_triangles_.reset();
    upload_.reset();
    meshlet_ranges_.clear();
    closed_.clear();
    material_sets_.clear();
    material_views_.clear();
    material_sampler_revision_ = 0;
    pool_ = nullptr;
    uv_buffer_.reset();
    textures_.clear();
    materials_.clear();
    submeshes_.clear();
    lods_.clear();
    indices_.reset();
    vertices_.reset();
    lightmap_uvs_.reset();
    vertex_colors_.reset();
    index_count_ = 0;
}

const VulkanBuffer& SkinnedModel::emptyBuffer() {
    static const VulkanBuffer empty;
    return empty;
}

}  // namespace cramion::gfx
