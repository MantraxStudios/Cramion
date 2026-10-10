// VulkanRenderer: la oclusion del cielo vista desde arriba y el autoenfoque
// suave de la profundidad de campo. Aparte para no engordar VulkanRenderer.cpp.

#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/VulkanRenderer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <future>
#include <utility>
#include <vector>

#include "CramionFX/scene/Scene.h"
#include <cmath>
#include <cstring>
#include <iostream>

namespace cramion::gfx {
namespace {

void pipelineBarrier(const vk::raii::CommandBuffer& cmd, const vk::ImageMemoryBarrier2& barrier) {
    vk::DependencyInfo dependency{};
    dependency.setImageMemoryBarriers(barrier);
    compat::pipelineBarrier(cmd, dependency);
}

void memoryBarrier(const vk::raii::CommandBuffer& cmd, vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                   vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    vk::MemoryBarrier2 barrier{};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    vk::DependencyInfo dependency{};
    dependency.setMemoryBarriers(barrier);
    compat::pipelineBarrier(cmd, dependency);
}

}  // namespace

// --- Oclusion del cielo desde arriba (sin trazado de rayos) ---
//
// Lo que tapa el cielo de un punto suele estar ENCIMA de el: la copa de los
// arboles, un tejado, un voladizo. La GI de pantalla no lo ve si no sale en
// pantalla (mirando al suelo de un bosque, las copas quedan fuera) y el
// sotobosque recibia todo el cielo: un bosque iluminado como un prado.
//
// Dos mapas de profundidad vistos desde arriba alrededor de la camara: solo el
// terreno y todo (terreno, escenario y arboles). lighting.frag busca alrededor
// de cada punto lo que esta por encima de el y no es el propio relieve
// (skyMapVisibility). Se rehacen al moverse la camara o cada pocos segundos.
// Con trazado de rayos no hace falta: la GI de rayos ya lo ve.

namespace {
constexpr std::uint32_t kSkyMapSize = 512;
constexpr float kSkyMapExtent = 160.0f;  // m de lado (31 cm por texel)
constexpr float kSkyMapAbove = 120.0f;   // m sobre la camara desde donde se mira
constexpr float kSkyMapRange = 400.0f;   // m de profundidad que cubre
}  // namespace

bool VulkanRenderer::skyMapWanted() const {
    const SceneDrawMode mode = drawModeNow();
    return sky_occlusion_enabled_ && !rayTracingActive() && mode != SceneDrawMode::Unlit &&
           mode != SceneDrawMode::Wireframe && *sky_map_scene_.handle() != nullptr;
}

void VulkanRenderer::createSkyMap() {
    for (VulkanImage* image : {&sky_map_terrain_, &sky_map_scene_}) {
        image->create(device_, vk::Extent2D{kSkyMapSize, kSkyMapSize}, shadow_map_.format(),
                      vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled |
                          vk::ImageUsageFlagBits::eTransferDst,
                      vk::ImageAspectFlagBits::eDepth);
        // Hasta dibujarlo: vacio (profundidad maxima) y ya como textura.
        const vk::Image handle = *image->handle();
        device_.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
            const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
            vk::ImageMemoryBarrier2 barrier{};
            barrier.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
            barrier.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
            barrier.oldLayout = vk::ImageLayout::eUndefined;
            barrier.newLayout = vk::ImageLayout::eTransferDstOptimal;
            barrier.image = handle;
            barrier.subresourceRange = range;
            pipelineBarrier(cmd, barrier);
            cmd.clearDepthStencilImage(handle, vk::ImageLayout::eTransferDstOptimal, vk::ClearDepthStencilValue{1.0f, 0},
                                       range);
            barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
            barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
            barrier.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
            barrier.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
            barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
            barrier.newLayout = compat::depthReadOnlyLayout();
            pipelineBarrier(cmd, barrier);
        });
    }
    sky_map_drawn_ = false;
}

void VulkanRenderer::planSkyMap() {
    if (!skyMapWanted() || isolated()) return;
    const core::Vec3 camera = camera_position_;
    ++sky_map_age_;
    const float dx = camera.x - sky_map_center_.x;
    const float dz = camera.z - sky_map_center_.z;
    // Al moverse, al subir o bajar mucho, cada ~3 s (lo que se mueve) o si
    // aparecen o desaparecen cosas.
    const bool due = !sky_map_drawn_ || dx * dx + dz * dz > 12.0f * 12.0f ||
                     std::abs(camera.y - sky_map_center_.y) > 25.0f || sky_map_age_ > 180 || actor_set_changed_;
    if (!due) return;
    // Encajado en la rejilla de 4 texeles: al moverse no tiembla.
    const float snap = kSkyMapExtent / static_cast<float>(kSkyMapSize) * 4.0f;
    const float cx = std::round(camera.x / snap) * snap;
    const float cz = std::round(camera.z / snap) * snap;
    const float half = kSkyMapExtent * 0.5f;
    const float top = camera.y + kSkyMapAbove;
    const core::Vec3 eye{cx, top, cz};
    // Mirando hacia abajo con "arriba" = -z: x del mapa = x del mundo y v
    // crece con z (la proyeccion de Vulkan invierte la y).
    const core::Mat4 view = core::lookAt(eye, core::Vec3{cx, top - 1.0f, cz}, core::Vec3{0.0f, 0.0f, -1.0f});
    sky_map_view_projection_ = core::orthographic(-half, half, -half, half, 0.0f, kSkyMapRange) * view;
    sky_map_params_ = core::Vec4{cx - half, cz - half, kSkyMapExtent, 1.0f};
    sky_map_depth_ = core::Vec4{top, kSkyMapRange, 0.0f, 0.0f};
    sky_map_center_ = camera;
    sky_map_age_ = 0;
    sky_map_redraw_ = true;
}

void VulkanRenderer::recordSkyMap(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index) {
    sky_map_redraw_ = false;
    const vk::Extent2D extent{kSkyMapSize, kSkyMapSize};
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
    const auto draw = [&](VulkanImage& image, bool everything) {
        vk::ImageMemoryBarrier2 to_attachment{};
        to_attachment.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
        to_attachment.srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
        to_attachment.dstStageMask =
            vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
        to_attachment.dstAccessMask =
            vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
        to_attachment.oldLayout = vk::ImageLayout::eUndefined;
        to_attachment.newLayout = compat::depthAttachmentLayout();
        to_attachment.image = *image.handle();
        to_attachment.subresourceRange = range;
        pipelineBarrier(cmd, to_attachment);

        vk::RenderingAttachmentInfo depth_attachment{};
        depth_attachment.imageView = *image.view();
        depth_attachment.imageLayout = compat::depthAttachmentLayout();
        depth_attachment.loadOp = vk::AttachmentLoadOp::eClear;
        depth_attachment.storeOp = vk::AttachmentStoreOp::eStore;
        depth_attachment.clearValue = vk::ClearValue{vk::ClearDepthStencilValue{1.0f, 0}};
        vk::RenderingInfo rendering_info{};
        rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
        rendering_info.layerCount = 1;
        rendering_info.pDepthAttachment = &depth_attachment;
        compat::beginRendering(cmd, rendering_info);
        cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                                        static_cast<float>(extent.height), 0.0f, 1.0f});
        cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, extent});
        // El terreno con sus trozos dentro de la caja (como una luz local).
        terrain_pass_.recordShadow(cmd, frame_index, skin_sets_[frame_index], sky_map_view_projection_, true);
        if (everything) {
            foliage_pass_.recordShadow(cmd, frame_index, skin_sets_[frame_index], sky_map_view_projection_);
            recordActorShadows(cmd, frame_index, sky_map_view_projection_);
        }
        compat::endRendering(cmd);

        vk::ImageMemoryBarrier2 to_read = to_attachment;
        to_read.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests;
        to_read.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
        to_read.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
        to_read.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
        to_read.oldLayout = compat::depthAttachmentLayout();
        to_read.newLayout = compat::depthReadOnlyLayout();
        pipelineBarrier(cmd, to_read);
    };
    draw(sky_map_terrain_, false);
    draw(sky_map_scene_, true);
    if (!sky_map_drawn_) std::cout << "[Vulkan] Oclusion del cielo desde arriba: activa\n";
    sky_map_drawn_ = true;
}

std::uint32_t VulkanRenderer::evictedModelCount() const {
    return static_cast<std::uint32_t>(std::count(model_evicted_.begin(), model_evicted_.end(), true));
}

void VulkanRenderer::evictModel(std::uint32_t index) {
    if (!modelResident(index)) return;
    if (model_evicted_.size() < skinned_models_.size()) model_evicted_.resize(skinned_models_.size(), false);
    // Los frames en vuelo (y la escena de rayos) aun pueden usarlo.
    if (index < ray_traced_models_ && index < ray_pinned_.size() && !ray_pinned_[index]) {
        ray_pinned_[index] = true;
        ray_pinned_models_.push_back(std::move(skinned_models_[index]));
    } else {
        retired_models_.push_back(RetiredModel{std::move(skinned_models_[index]), kMaxFramesInFlight + 1});
    }
    skinned_models_[index] = SkinnedModel{};
    model_evicted_[index] = true;
    // (La escena de rayos no se rehace: conserva su copia del modelo. Las
    // sombras cacheadas tampoco: no se veia, y rehacerlas todas era un tiron.)
}

bool VulkanRenderer::modelJobPending(std::uint32_t index) const {
    return std::any_of(model_jobs_.begin(), model_jobs_.end(), [&](const ModelJob& job) { return job.index == index; });
}

void VulkanRenderer::launchModelJob(const scene::Scene& scene, std::uint32_t index, int texture_lod, bool restore,
                                    bool append) {
    if (index >= scene.models().size()) return;
    // Antes de que la escena cambie o quite un modelo, el hilo que lo lee
    // termina (y lo suyo se tira). weak_ptr: la escena puede durar mas que el
    // renderizador.
    scene.setModelWriteHook([this, alive = std::weak_ptr<bool>(model_jobs_alive_)](std::uint32_t first,
                                                                                   std::uint32_t last) {
        if (alive.lock()) finishModelJobs(first, last);
    });
    const asset::ModelData* data = scene.models()[index].get();
    ModelJob job;
    job.index = index;
    job.texture_lod = texture_lod;
    job.restore = restore;
    job.append = append;
    // Lo lento (leer y convertir texturas, copiarlas, esperar a la GPU) va en
    // este hilo; VulkanDevice::submitOneTime se puede usar desde cualquiera.
    job.result = std::async(std::launch::async, [this, data, texture_lod]() -> std::unique_ptr<SkinnedModel> {
        try {
            auto model = std::make_unique<SkinnedModel>();
            model->create(device_, *data, skinned_pass_, texture_lod);
            return model;
        } catch (const std::exception& e) {
            std::cerr << "[Streaming] No se pudo subir un modelo: " << e.what() << "\n";
            return nullptr;
        }
    });
    model_jobs_.push_back(std::move(job));
}

void VulkanRenderer::finishModelJobs(std::uint32_t first, std::uint32_t last) {
    for (auto it = model_jobs_.begin(); it != model_jobs_.end();) {
        if (it->index < first || it->index >= last) {
            ++it;
            continue;
        }
        if (it->result.valid()) it->result.get();  // se libera aqui (la GPU nunca lo uso)
        it = model_jobs_.erase(it);
    }
}

// --- Streaming de la memoria de video (residencia de los modelos) ---
//
// Cada frame updateActors mide lo grande que se ve cada modelo (la mayor de
// sus instancias, en pixeles). Si lleva `streaming_idle_seconds_` por debajo de
// `streaming_min_pixels_` (no se distingue: el recorte por tamano ya ni lo
// dibujaba), sus mallas y texturas salen de la GPU; en cuanto una instancia
// se acerca, vuelve a subirse desde la copia de la CPU en otro hilo y entra
// cuando esta lista. El detalle de las texturas cambia igual: el modelo se
// rehace en otro hilo y mientras tanto se ve el de antes.

void VulkanRenderer::streamModels(const scene::Scene& scene) {
    const auto now = std::chrono::steady_clock::now();
    // Diagnostico (cada 2 s, solo si hubo algo): que sube el streaming y
    // cuanta VRAM queda. La VRAM se mira cada medio segundo (no es gratis).
    static auto report_at = now;
    static auto vram_at = now - std::chrono::seconds(1);
    static std::uint64_t vram_used = 0, vram_budget = 0;
    static int restores = 0, texture_uploads = 0, evictions = 0;
    if (now - vram_at > std::chrono::milliseconds(500)) {
        vram_at = now;
        if (!device_.videoMemory(vram_used, vram_budget)) vram_used = vram_budget = 0;
    }
    // Casi sin VRAM: el streaming de texturas no sube detalle (solo baja).
    const bool vram_full = vram_budget > 0 && vram_used > vram_budget / 100 * 85;
    if (now - report_at > std::chrono::seconds(2)) {
        if (restores + texture_uploads + evictions > 0) {
            std::cout << "[Streaming] 2 s: " << restores << " modelos vueltos, " << texture_uploads
                      << " cambios de detalle de texturas, " << evictions << " fuera; en hilos " << model_jobs_.size()
                      << ", retirados " << retired_models_.size() << ", fijados por RT " << ray_pinned_models_.size()
                      << "; VRAM " << vram_used / (1024 * 1024) << " de " << vram_budget / (1024 * 1024) << " MB"
                      << (vram_full ? " (LLENA: no se sube detalle)" : "")
                      << " [texturas " << gpuMemoryBytes(GpuMemoryKind::Texture) / (1024 * 1024) << " MB, imagenes "
                      << gpuMemoryBytes(GpuMemoryKind::Image) / (1024 * 1024) << " MB, buffers "
                      << gpuMemoryBytes(GpuMemoryKind::Buffer) / (1024 * 1024) << " MB, CPU-visibles "
                      << gpuMemoryBytes(GpuMemoryKind::Staging) / (1024 * 1024) << " MB; " << skinned_models_.size()
                      << " modelos]\n";
        }
        report_at = now;
        restores = texture_uploads = evictions = 0;
    }

    const std::uint32_t count = std::min(static_cast<std::uint32_t>(scene.models().size()), uploadedModelCount());
    if (model_last_seen_.size() < count) model_last_seen_.resize(count, now);
    if (model_evicted_.size() < count) model_evicted_.resize(count, false);
    if (model_lod_since_.size() < count) model_lod_since_.resize(count, now);

    // Lo que terminaron los hilos entra ya (solo mover punteros: sin esperas).
    for (auto it = model_jobs_.begin(); it != model_jobs_.end();) {
        // (Los nuevos los mete uploadNewModels, en orden.)
        if (it->append || it->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        std::unique_ptr<SkinnedModel> fresh = it->result.get();
        const std::uint32_t index = it->index;
        const bool restore = it->restore;
        it = model_jobs_.erase(it);
        if (!fresh || index >= count) continue;
        installModel(scene, index, std::move(*fresh), true);
        model_lod_since_[index] = now;
        ++(restore ? restores : texture_uploads);
    }

    const auto wanted_lod = [&](float pixels) {
        const int lod = pixels >= 400.0f ? 0 : pixels >= 150.0f ? 1 : pixels >= 50.0f ? 2 : 3;
        return std::clamp(lod + texture_lod_bias_, 0, 3);
    };
    // Dos hilos a la vez como mucho: el resto espera su turno (los mas
    // grandes en pantalla antes).
    constexpr std::size_t kMaxJobs = 2;

    // Vuelven los que se ven, con el detalle de texturas que ya piden.
    std::vector<std::pair<float, std::uint32_t>> wanted;
    for (std::uint32_t i = 0; i < count; ++i) {
        const float pixels = i < model_pixels_.size() ? model_pixels_[i] : 0.0f;
        if (pixels >= streaming_min_pixels_) model_last_seen_[i] = now;
        if (model_evicted_[i] && (pixels >= streaming_min_pixels_ || !model_streaming_) && !modelJobPending(i)) {
            wanted.emplace_back(pixels, i);
        }
    }
    std::sort(wanted.begin(), wanted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [pixels, index] : wanted) {
        if (model_jobs_.size() >= kMaxJobs) break;
        launchModelJob(scene, index, texture_streaming_ ? wanted_lod(pixels) : 0, true);
    }

    // Streaming de texturas por mips: el detalle que pide cada modelo por lo
    // que mide en pantalla. Mas detalle: ya (los mas grandes antes); menos:
    // tras 3 s pidiendolo.
    if (texture_streaming_ && model_jobs_.size() < kMaxJobs) {
        int best = -1;
        float best_pixels = -1.0f;
        int best_lod = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            if (model_evicted_[i] || skinned_models_[i].submeshes().empty() || modelJobPending(i)) continue;
            const float pixels = i < model_pixels_.size() ? model_pixels_[i] : 0.0f;
            const int current = skinned_models_[i].textureLod();
            const int want = wanted_lod(pixels);
            if (want >= current) {
                if (want == current) model_lod_since_[i] = now;
                if (want > current && std::chrono::duration<float>(now - model_lod_since_[i]).count() > 3.0f &&
                    best < 0) {
                    best = static_cast<int>(i);
                    best_lod = want;
                }
                continue;
            }
            model_lod_since_[i] = now;
            if (vram_full) continue;
            if (pixels > best_pixels) {  // le falta detalle: el mas grande primero
                best_pixels = pixels;
                best = static_cast<int>(i);
                best_lod = want;
            }
        }
        if (best >= 0) {
            launchModelJob(scene, static_cast<std::uint32_t>(best), best_lod, false);
            model_lod_since_[static_cast<std::size_t>(best)] = now;
        }
    }
    if (!model_streaming_) return;
    // Salen los que llevan un rato sin verse (como mucho 8 por frame).
    std::uint32_t evicted = 0;
    for (std::uint32_t i = 0; i < count && evicted < 8; ++i) {
        if (model_evicted_[i] || skinned_models_[i].submeshes().empty() || modelJobPending(i)) continue;
        if (std::chrono::duration<float>(now - model_last_seen_[i]).count() < streaming_idle_seconds_) continue;
        evictModel(i);
        ++evicted;
        ++evictions;
    }
}

// --- Autoenfoque suave ---
//
// Antes el shader de la profundidad de campo enfocaba en cada frame lo que
// hubiera en el centro: al pasar algo por delante o mover la camara, el
// enfoque saltaba de golpe. Ahora un compute (dof_focus.comp) mide la
// distancia y la CPU la sigue como el motor de una lente: exponencial en
// escala logaritmica (de 1 a 10 m tarda lo mismo que de 10 a 100 m), con la
// velocidad del ajuste "Velocidad del autoenfoque".

void VulkanRenderer::createDofFocus() {
    const std::array<vk::DescriptorType, 2> bindings = {vk::DescriptorType::eCombinedImageSampler,
                                                        vk::DescriptorType::eStorageBuffer};
    ComputePassDesc desc{};
    desc.shader = "dof_focus.comp.spv";
    desc.bindings = bindings;
    desc.push_constant_size = sizeof(core::Vec4);
    dof_focus_pass_.create(device_, desc);

    const std::array<vk::DescriptorPoolSize, 2> sizes = {
        vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler, kMaxFramesInFlight},
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, kMaxFramesInFlight}};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = kMaxFramesInFlight;
    pool_info.setPoolSizes(sizes);
    dof_focus_pool_ = vk::raii::DescriptorPool(device_.handle(), pool_info);
    const std::vector<vk::DescriptorSetLayout> layouts(kMaxFramesInFlight, *dof_focus_pass_.descriptorSetLayout());
    vk::DescriptorSetAllocateInfo alloc{};
    alloc.descriptorPool = *dof_focus_pool_;
    alloc.setSetLayouts(layouts);
    dof_focus_sets_ = vk::raii::DescriptorSets(device_.handle(), alloc);

    dof_focus_buffers_.clear();
    dof_focus_buffers_.resize(kMaxFramesInFlight);
    for (VulkanBuffer& buffer : dof_focus_buffers_) {
        buffer.create(device_, sizeof(float), vk::BufferUsageFlagBits::eStorageBuffer,
                      vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    }
    dof_focus_pending_.assign(kMaxFramesInFlight, false);
}

void VulkanRenderer::writeDofFocusDescriptors() {
    if (dof_focus_sets_.empty()) return;
    for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        const vk::DescriptorImageInfo depth{*dof_focus_pass_.sampler(), *gbuffer_.depth().view(),
                                            compat::depthReadOnlyLayout()};
        const vk::DescriptorBufferInfo focus{*dof_focus_buffers_[i].handle(), 0, sizeof(float)};
        std::array<vk::WriteDescriptorSet, 2> writes{};
        writes[0].dstSet = *dof_focus_sets_[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[0].setImageInfo(depth);
        writes[1].dstSet = *dof_focus_sets_[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[1].setBufferInfo(focus);
        device_.handle().updateDescriptorSets(writes, nullptr);
    }
}

void VulkanRenderer::readDofFocus(std::uint32_t frame_index) {
    if (frame_index >= dof_focus_pending_.size() || !dof_focus_pending_[frame_index]) return;
    dof_focus_pending_[frame_index] = false;
    float measured = 0.0f;
    std::memcpy(&measured, dof_focus_buffers_[frame_index].mapped(), sizeof(float));
    if (std::isfinite(measured) && measured > 0.0f) dof_focus_target_ = measured;
}

void VulkanRenderer::recordDofFocusMeasure(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index) {
    if (dof_focus_sets_.empty() || frame_index >= dof_focus_sets_.size()) return;
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *dof_focus_pass_.pipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *dof_focus_pass_.layout(), 0, *dof_focus_sets_[frame_index],
                           nullptr);
    const core::Vec4 camera{camera_projection_.m[3][2], camera_projection_.m[2][2],
                            static_cast<float>(render_extent_.width) /
                                static_cast<float>(std::max(render_extent_.height, 1u)),
                            0.0f};
    cmd.pushConstants<core::Vec4>(*dof_focus_pass_.layout(), vk::ShaderStageFlagBits::eCompute, 0, camera);
    cmd.dispatch(1, 1, 1);
    // La lee la CPU cuando vuelva a tocar este hueco (despues de su fence).
    memoryBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
                  vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead);
    dof_focus_pending_[frame_index] = true;
}

float VulkanRenderer::smoothedDofFocus() {
    const float target = std::clamp(dof_focus_target_, 0.05f, 2000.0f);
    if (dof_focus_current_ <= 0.0f) {
        dof_focus_current_ = target;
        return dof_focus_current_;
    }
    const float speed = std::max(post_.dof_focus_speed, 0.05f);
    const float k = 1.0f - std::exp(-speed * std::clamp(frame_delta_seconds_, 0.0f, 0.25f));
    dof_focus_current_ = std::exp(std::log(dof_focus_current_) + (std::log(target) - std::log(dof_focus_current_)) * k);
    return dof_focus_current_;
}

}  // namespace cramion::gfx
