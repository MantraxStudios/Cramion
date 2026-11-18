// Salida de imagen del renderizador (DisplayOutput.h):
//   - Baja latencia: NVIDIA Reflex (VK_NV_low_latency2), AMD Anti-Lag 2
//     (VK_AMD_anti_lag) o, con cualquier GPU que tenga VK_KHR_present_wait,
//     esperar a que se presente el frame anterior en vez de acumular frames.
//     Y la medida de la latencia ([Rendimiento]).
//   - Salida HDR10 / scRGB: la escena y la UI se dibujan en dos imagenes SDR y
//     hdr_output.frag las compone en la swapchain con el brillo de papel; las
//     luces de la escena por encima del papel salen del mapa de ganancia.
//   - LUT 3D (.cube) de la gradacion de color (composite.frag, binding 7).
// Apagado (lo de siempre): nada de esto hace trabajo por frame mas alla de
// apuntar un instante por frame (la latencia estimada).

#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/VulkanRenderer.h"

#include "CramionFX/vk/CubeLut.h"
#include "CramionFX/vk/DisplayOutputVk.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace cramion::gfx {

namespace {

using Clock = std::chrono::steady_clock;
constexpr vk::ImageLayout kRead = vk::ImageLayout::eShaderReadOnlyOptimal;
// Tope de las esperas de baja latencia: un driver que no avisa no congela el
// juego (100 ms y se sigue).
constexpr std::uint64_t kLatencyWaitTimeoutNs = 100'000'000;
// VkLatencyMarkerNV (con sus valores: compila tambien con cabeceras sin
// VK_NV_low_latency2, donde las marcas no hacen nada).
constexpr int kMarkerSimulationStart = 0;
constexpr int kMarkerSimulationEnd = 1;
constexpr int kMarkerRenderSubmitStart = 2;
constexpr int kMarkerRenderSubmitEnd = 3;
constexpr int kMarkerPresentStart = 4;
constexpr int kMarkerInputSample = 6;

vk::ImageMemoryBarrier2 imageBarrier(vk::Image image, vk::ImageLayout old_layout, vk::ImageLayout new_layout,
                                     vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                     vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    vk::ImageMemoryBarrier2 barrier{};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.image = image;
    barrier.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    return barrier;
}

void barrierOne(const vk::raii::CommandBuffer& cmd, const vk::ImageMemoryBarrier2& barrier) {
    vk::DependencyInfo dependency{};
    dependency.setImageMemoryBarriers(barrier);
    compat::pipelineBarrier(cmd, dependency);
}

// Destino que se reescribe entero (lo anterior se leia como textura).
vk::ImageMemoryBarrier2 toAttachment(vk::Image image) {
    return imageBarrier(image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
                        vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead,
                        vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite);
}

vk::ImageMemoryBarrier2 toSampled(vk::Image image) {
    return imageBarrier(image, vk::ImageLayout::eColorAttachmentOptimal, kRead,
                        vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                        vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead);
}

void beginColor(const vk::raii::CommandBuffer& cmd, vk::ImageView view, vk::Extent2D extent, bool clear) {
    vk::RenderingAttachmentInfo attachment{};
    attachment.imageView = view;
    attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    attachment.loadOp = clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eDontCare;
    attachment.storeOp = vk::AttachmentStoreOp::eStore;
    attachment.clearValue = vk::ClearValue{vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}}};
    vk::RenderingInfo info{};
    info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
    info.layerCount = 1;
    info.setColorAttachments(attachment);
    compat::beginRendering(cmd, info);
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                                    0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, extent});
}

std::filesystem::path pathFromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

}  // namespace

// -----------------------------------------------------------------------------
// Ajustes
// -----------------------------------------------------------------------------

bool VulkanRenderer::prepareSwapchainOutput() {
    HdrOutput hdr = hdr_output_override_ >= 0 ? static_cast<HdrOutput>(std::clamp(hdr_output_override_, 0, 3))
                                              : user_graphics_.hdr_output;
    LowLatencyMode mode = low_latency_override_ >= 0
                              ? static_cast<LowLatencyMode>(std::clamp(low_latency_override_, 0, 2))
                              : user_graphics_.low_latency;
    // Modo compatible (moviles, GPU antiguas): ni HDR ni baja latencia.
    if (device_.compatMode()) {
        hdr = HdrOutput::Off;
        mode = LowLatencyMode::Off;
    }
    const auto& instance_ext = instance_.enabledExtensions();
    const bool colorspace = std::find(instance_ext.begin(), instance_ext.end(),
                                      std::string(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) != instance_ext.end();
    const LatencyBackend backend = chooseLatencyBackend(mode, device_.outputExtensions());
    const bool reflex = backend == LatencyBackend::Reflex;
    const bool changed = hdr != swapchain_.outputRequest() || reflex != swapchain_.latencyModeRequested();
    swapchain_.setOutputRequest(hdr, colorspace);
    swapchain_.setLatencyModeEnable(reflex);

    if (mode != latency_mode_ || backend != latency_backend_) {
#if defined(VK_AMD_anti_lag)
        // Anti-Lag 2 se apaga explicitamente (si no, el driver sigue con el).
        if (latency_backend_ == LatencyBackend::AntiLag && backend != LatencyBackend::AntiLag && initialized_) {
            vk::AntiLagDataAMD data{};
            data.mode = vk::AntiLagModeAMD::eOff;
            device_.handle().antiLagUpdateAMD(data);
        }
#endif
        if (mode != LowLatencyMode::Off || latency_mode_ != LowLatencyMode::Off) {
            std::cout << "[Vulkan] Baja latencia: " << lowLatencyModeName(mode)
                      << (mode != LowLatencyMode::Off ? std::string(" (") + latencyBackendName(backend) + ")" : std::string())
                      << "\n";
        }
        latency_mode_ = mode;
        latency_backend_ = backend;
        latency_meter_.reset();
        reflex_applied_mode_ = -1;
    }
    if (initialized_ && swapchain_.isValid() && !changed) {
        applyLatencyMode();
        // Otro brillo de papel o maximo: solo los metadatos.
        swapchain_.setHdrMetadata(user_graphics_.hdr_max_nits, user_graphics_.hdr_paper_white);
    }
    return changed;
}

void VulkanRenderer::setOutputOverrides(int low_latency, int hdr_output) {
    low_latency = std::clamp(low_latency, -1, 2);
    hdr_output = std::clamp(hdr_output, -1, 3);
    if (low_latency == low_latency_override_ && hdr_output == hdr_output_override_) return;
    low_latency_override_ = low_latency;
    hdr_output_override_ = hdr_output;
    if (prepareSwapchainOutput()) settings_dirty_ = true;
}

VulkanRenderer::OutputStatus VulkanRenderer::outputStatus() const {
    OutputStatus s{};
    s.low_latency = latency_mode_;
    s.backend = latency_backend_;
    s.encoding = swapchain_.encoding();
    s.hdr_fell_back = swapchain_.hdrFellBack();
    s.latency_ms = latency_meter_.averageMs();
    s.latency_last_ms = latency_meter_.lastMs();
    s.latency_samples = latency_meter_.samples();
    s.latency_measured = latency_backend_ == LatencyBackend::Reflex || latency_backend_ == LatencyBackend::PresentWait;
    s.color_lut_loaded = !color_lut_path_.empty() && color_lut_size_ >= 2;
    return s;
}

// -----------------------------------------------------------------------------
// Baja latencia
// -----------------------------------------------------------------------------

void VulkanRenderer::applyLatencyMode() {
#if defined(VK_NV_low_latency2)
    if (latency_backend_ != LatencyBackend::Reflex || !swapchain_.isValid() || !swapchain_.latencyModeEnabled()) return;
    if (!*latency_semaphore_) {
        vk::SemaphoreTypeCreateInfo type{};
        type.semaphoreType = vk::SemaphoreType::eTimeline;
        type.initialValue = 0;
        vk::SemaphoreCreateInfo info{};
        info.pNext = &type;
        latency_semaphore_ = vk::raii::Semaphore(device_.handle(), info);
        latency_sleep_value_ = 0;
    }
    const int want = static_cast<int>(latency_mode_);
    if (want == reflex_applied_mode_) return;
    vk::LatencySleepModeInfoNV info{};
    info.lowLatencyMode = latency_mode_ != LowLatencyMode::Off ? VK_TRUE : VK_FALSE;
    info.lowLatencyBoost = latency_mode_ == LowLatencyMode::Boost ? VK_TRUE : VK_FALSE;
    info.minimumIntervalUs = 0;
    try {
        swapchain_.handle().setLatencySleepModeNV(info);
        reflex_applied_mode_ = want;
    } catch (const vk::SystemError& e) {
        std::cerr << "[Vulkan] Reflex: setLatencySleepModeNV fallo (" << e.what() << "); se sigue sin el\n";
        latency_backend_ = LatencyBackend::None;
    }
#endif
}

void VulkanRenderer::latencyMarker(int marker) const {
#if defined(VK_NV_low_latency2)
    if (latency_backend_ != LatencyBackend::Reflex || reflex_applied_mode_ < 0 || !swapchain_.isValid()) return;
    vk::SetLatencyMarkerInfoNV info{};
    info.presentID = present_id_ + 1;
    info.marker = static_cast<vk::LatencyMarkerNV>(marker);
    swapchain_.handle().setLatencyMarkerNV(info);
#else
    (void)marker;
#endif
}

void VulkanRenderer::beginLatencyFrame() {
    if (!initialized_ || !swapchain_.isValid()) return;
    latency_frame_begun_ = false;
    latencySleep();
    latency_frame_begun_ = true;
}

void VulkanRenderer::latencySleep() {
    if (latency_frame_begun_) {
        // Ya se espero al empezar el bucle (beginLatencyFrame): aqui empieza el render.
        latency_frame_begun_ = false;
        latencyMarker(kMarkerSimulationEnd);
        latencyMarker(kMarkerRenderSubmitStart);
        return;
    }
    const std::uint64_t next_id = present_id_ + 1;
    switch (latency_backend_) {
#if defined(VK_NV_low_latency2)
        case LatencyBackend::Reflex: {
            if (reflex_applied_mode_ < 0 || !*latency_semaphore_) break;
            vk::LatencySleepInfoNV sleep{};
            sleep.signalSemaphore = *latency_semaphore_;
            sleep.value = ++latency_sleep_value_;
            try {
                swapchain_.handle().latencySleepNV(sleep);
                vk::SemaphoreWaitInfo wait{};
                const vk::Semaphore semaphore = *latency_semaphore_;
                wait.setSemaphores(semaphore);
                wait.setValues(latency_sleep_value_);
                (void)device_.handle().waitSemaphores(wait, kLatencyWaitTimeoutNs);
            } catch (const vk::SystemError& e) {
                std::cerr << "[Vulkan] Reflex: latencySleepNV fallo (" << e.what() << "); se sigue sin el\n";
                latency_backend_ = LatencyBackend::None;
                break;
            }
            latencyMarker(kMarkerSimulationStart);
            latencyMarker(kMarkerInputSample);
            break;
        }
#endif
#if defined(VK_AMD_anti_lag)
        case LatencyBackend::AntiLag: {
            // El driver duerme lo justo aqui (antes de leer la entrada).
            vk::AntiLagPresentationInfoAMD presentation{};
            presentation.stage = vk::AntiLagStageAMD::eInput;
            presentation.frameIndex = next_id;
            vk::AntiLagDataAMD data{};
            data.mode = vk::AntiLagModeAMD::eOn;
            data.maxFPS = 0;
            data.pPresentationInfo = &presentation;
            device_.handle().antiLagUpdateAMD(data);
            break;
        }
#endif
        case LatencyBackend::PresentWait: {
            const std::uint64_t target = presentWaitTarget(latency_mode_, next_id);
            if (target == 0 || target <= latency_waited_id_ || target > present_id_) break;
            try {
                const vk::Result r = swapchain_.handle().waitForPresent(target, kLatencyWaitTimeoutNs);
                latency_waited_id_ = target;
                if (r == vk::Result::eSuccess || r == vk::Result::eSuboptimalKHR) {
                    latency_meter_.frameFinished(target, Clock::now());
                }
            } catch (const vk::SystemError& e) {
                std::cerr << "[Vulkan] present_wait fallo (" << e.what() << "); se sigue sin esperar\n";
                latency_backend_ = LatencyBackend::None;
            }
            break;
        }
        default: break;
    }
    latency_meter_.frameStarted(next_id, Clock::now());
}

void VulkanRenderer::latencyFenceDone(std::uint32_t slot) {
    if (slot >= slot_present_id_.size()) return;
    const std::uint64_t id = slot_present_id_[slot];
    slot_present_id_[slot] = 0;
    // Con Reflex o present_wait la medida es la buena (no esta, que es una cota).
    if (id == 0 || latency_backend_ == LatencyBackend::Reflex || latency_backend_ == LatencyBackend::PresentWait) return;
    latency_meter_.frameFinished(id, Clock::now());
}

void VulkanRenderer::latencyBeforeSubmit(vk::SubmitInfo& submit) {
#if defined(VK_NV_low_latency2)
    if (latency_backend_ == LatencyBackend::Reflex && reflex_applied_mode_ >= 0) {
        latency_submit_info_ = vk::LatencySubmissionPresentIdNV{};
        latency_submit_info_.presentID = present_id_ + 1;
        latency_submit_info_.pNext = submit.pNext;
        submit.pNext = &latency_submit_info_;
    }
#else
    (void)submit;
#endif
}

void VulkanRenderer::latencyAfterSubmit() { latencyMarker(kMarkerRenderSubmitEnd); }

void VulkanRenderer::latencyBeforePresent(vk::PresentInfoKHR& present) {
    latencyMarker(kMarkerPresentStart);
    ++present_id_;
    if (current_frame_ < slot_present_id_.size()) slot_present_id_[current_frame_] = present_id_;
    // Solo con baja latencia: apagada, la presentacion va como siempre.
    if (latency_backend_ == LatencyBackend::None || !device_.outputExtensions().present_id) return;
    present_id_value_ = present_id_;
    present_id_info_ = vk::PresentIdKHR{};
    present_id_info_.swapchainCount = 1;
    present_id_info_.pPresentIds = &present_id_value_;
    present_id_info_.pNext = present.pNext;
    present.pNext = &present_id_info_;
#if defined(VK_AMD_anti_lag)
    if (latency_backend_ == LatencyBackend::AntiLag) {
        vk::AntiLagPresentationInfoAMD presentation{};
        presentation.stage = vk::AntiLagStageAMD::ePresent;
        presentation.frameIndex = present_id_;
        vk::AntiLagDataAMD data{};
        data.mode = vk::AntiLagModeAMD::eOn;
        data.pPresentationInfo = &presentation;
        device_.handle().antiLagUpdateAMD(data);
    }
#endif
}

void VulkanRenderer::latencyAfterPresent() {
#if defined(VK_NV_low_latency2)
    if (latency_backend_ != LatencyBackend::Reflex || reflex_applied_mode_ < 0 || !swapchain_.isValid()) return;
    {
        // (La marca va con el id del frame que se acaba de presentar.)
        vk::SetLatencyMarkerInfoNV info{};
        info.presentID = present_id_;
        info.marker = vk::LatencyMarkerNV::ePresentEnd;
        swapchain_.handle().setLatencyMarkerNV(info);
    }
    // Tiempos de Reflex cada 16 frames: de la entrada (inicio de la
    // simulacion) al final del render en la GPU de cada frame.
    if (present_id_ % 16 != 0) return;
    std::array<vk::LatencyTimingsFrameReportNV, 64> reports{};
    vk::GetLatencyMarkerInfoNV info{};
    info.timingCount = static_cast<std::uint32_t>(reports.size());
    info.pTimings = reports.data();
    swapchain_.handle().getLatencyTimingsNV(&info);
    for (std::uint32_t i = 0; i < info.timingCount && i < reports.size(); ++i) {
        const vk::LatencyTimingsFrameReportNV& r = reports[i];
        if (r.presentID <= latency_reported_id_) continue;
        const std::uint64_t start = r.inputSampleTimeUs != 0 ? r.inputSampleTimeUs : r.simStartTimeUs;
        if (start == 0 || r.gpuRenderEndTimeUs <= start) continue;
        latency_meter_.addSample(static_cast<float>(r.gpuRenderEndTimeUs - start) / 1000.0f);
        latency_reported_id_ = r.presentID;
    }
#endif
}

// -----------------------------------------------------------------------------
// Salida HDR
// -----------------------------------------------------------------------------

bool VulkanRenderer::hdrOutputReady() const {
    return swapchain_.hdrActive() && hdr_ui_scene_.isValid() && hdr_ui_layer_.isValid() &&
           hdr_output_format_ == swapchain_.imageFormat() && !hdr_output_sets_.empty();
}

void VulkanRenderer::createOutputTargets() {
    // Baja latencia: la swapchain nueva empieza sin ids presentados (no se
    // espera a los de la anterior) y sin el modo de Reflex aplicado.
    latency_waited_id_ = present_id_;
    reflex_applied_mode_ = -1;
    applyLatencyMode();

    if (!swapchain_.isValid() || !swapchain_.hdrActive()) {
        hdr_ui_scene_.destroy();
        hdr_ui_layer_.destroy();
        return;
    }
    const vk::Extent2D extent = swapchain_.extent();
    const vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled;
    hdr_ui_scene_.create(device_, extent, swapchain_.uiFormat(), usage, vk::ImageAspectFlagBits::eColor);
    hdr_ui_layer_.create(device_, extent, swapchain_.uiFormat(), usage, vk::ImageAspectFlagBits::eColor);
    hdr_ui_scene_.setName(device_, "HdrUiScene");
    hdr_ui_layer_.setName(device_, "HdrUiLayer");
    // Con un layout de lectura desde el principio (el set las referencia).
    device_.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        for (const VulkanImage* image : {&hdr_ui_scene_, &hdr_ui_layer_}) {
            barrierOne(cmd, imageBarrier(*image->handle(), vk::ImageLayout::eUndefined, kRead,
                                         vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                                         vk::PipelineStageFlagBits2::eFragmentShader,
                                         vk::AccessFlagBits2::eShaderSampledRead));
        }
    });

    if (hdr_output_format_ != swapchain_.imageFormat() || hdr_output_sets_.empty()) {
        hdr_output_sets_.clear();
        hdr_output_pool_ = nullptr;
        using Type = vk::DescriptorType;
        const std::array<Type, 4> bindings = {Type::eCombinedImageSampler, Type::eCombinedImageSampler,
                                              Type::eCombinedImageSampler, Type::eStorageBuffer};
        FullscreenPassDesc desc{};
        desc.fragment_shader = "hdr_output.frag.spv";
        desc.bindings = bindings;
        desc.push_constant_size = sizeof(GpuHdrOutputPush);
        desc.color_format = swapchain_.imageFormat();
        hdr_output_pass_.create(device_, desc);
        hdr_output_format_ = swapchain_.imageFormat();

        const std::array<vk::DescriptorPoolSize, 2> sizes = {
            vk::DescriptorPoolSize{Type::eCombinedImageSampler, kMaxFramesInFlight * 3},
            vk::DescriptorPoolSize{Type::eStorageBuffer, kMaxFramesInFlight}};
        vk::DescriptorPoolCreateInfo pool_info{};
        pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
        pool_info.maxSets = kMaxFramesInFlight;
        pool_info.setPoolSizes(sizes);
        hdr_output_pool_ = vk::raii::DescriptorPool(device_.handle(), pool_info);
        const std::vector<vk::DescriptorSetLayout> layouts(kMaxFramesInFlight, *hdr_output_pass_.descriptorSetLayout());
        vk::DescriptorSetAllocateInfo alloc{};
        alloc.descriptorPool = *hdr_output_pool_;
        alloc.setSetLayouts(layouts);
        vk::raii::DescriptorSets sets(device_.handle(), alloc);
        for (vk::raii::DescriptorSet& set : sets) hdr_output_sets_.push_back(std::move(set));
    }
    swapchain_.setHdrMetadata(user_graphics_.hdr_max_nits, user_graphics_.hdr_paper_white);
    std::cout << "[Vulkan] Salida HDR: " << outputEncodingName(swapchain_.encoding()) << ", papel "
              << clampPaperWhite(user_graphics_.hdr_paper_white) << " nits, maximo "
              << clampMaxNits(user_graphics_.hdr_max_nits, user_graphics_.hdr_paper_white) << " nits"
              << (device_.outputExtensions().hdr_metadata ? "" : " (sin VK_EXT_hdr_metadata)") << "\n";
}

void VulkanRenderer::destroyOutputResources() {
    hdr_output_sets_.clear();
    hdr_output_pool_ = nullptr;
    hdr_output_pass_.destroy();
    hdr_output_format_ = vk::Format::eUndefined;
    hdr_ui_scene_.destroy();
    hdr_ui_layer_.destroy();
    latency_semaphore_ = nullptr;
    color_lut_view_ = nullptr;
    color_lut_image_ = nullptr;
    color_lut_memory_ = nullptr;
    color_lut_path_.clear();
    color_lut_failed_.clear();
    color_lut_size_ = 0;
}

void VulkanRenderer::writeOutputDescriptors() {
    if (hdr_output_sets_.empty() || !hdr_ui_scene_.isValid() || !hdr_ui_layer_.isValid()) return;
    const VulkanImage& hdr_scene = postSource();
    if (!hdr_scene.isValid()) return;
    for (const vk::raii::DescriptorSet& set : hdr_output_sets_) {
        std::array<vk::DescriptorImageInfo, 3> images{};
        const std::array<const VulkanImage*, 3> sources = {&hdr_ui_scene_, &hdr_ui_layer_, &hdr_scene};
        for (std::size_t i = 0; i < images.size(); ++i) {
            images[i].sampler = *hdr_output_pass_.sampler();
            images[i].imageView = *sources[i]->view();
            images[i].imageLayout = kRead;
        }
        vk::DescriptorBufferInfo exposure{};
        exposure.buffer = *exposure_buffer_.handle();
        exposure.range = VK_WHOLE_SIZE;
        std::array<vk::WriteDescriptorSet, 4> writes{};
        for (std::uint32_t i = 0; i < 3; ++i) {
            writes[i].dstSet = *set;
            writes[i].dstBinding = i;
            writes[i].descriptorType = vk::DescriptorType::eCombinedImageSampler;
            writes[i].setImageInfo(images[i]);
        }
        writes[3].dstSet = *set;
        writes[3].dstBinding = 3;
        writes[3].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[3].setBufferInfo(exposure);
        device_.handle().updateDescriptorSets(writes, nullptr);
    }
}

void VulkanRenderer::recordHdrPostProcess(const vk::raii::CommandBuffer& cmd, std::uint32_t image_index) {
    const vk::Extent2D extent = swapchain_.extent();

    // 1) La escena (FXAA y nitidez) a su imagen SDR, como iria a la pantalla.
    barrierOne(cmd, toAttachment(*hdr_ui_scene_.handle()));
    beginColor(cmd, *hdr_ui_scene_.view(), extent, false);
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *post_process_pass_.pipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *post_process_pass_.layout(), 0,
                           *post_process_sets_[current_frame_], nullptr);
    const GpuPostProcessPush fxaa = postProcessPush();
    cmd.pushConstants<GpuPostProcessPush>(*post_process_pass_.layout(), vk::ShaderStageFlagBits::eFragment, 0, fxaa);
    cmd.draw(3, 1, 0, 0);
    compat::endRendering(cmd);
    barrierOne(cmd, toSampled(*hdr_ui_scene_.handle()));

    // 2) La UI de la herramienta (ImGui, la UI del juego) sobre negro
    // transparente: queda con alfa premultiplicado.
    barrierOne(cmd, toAttachment(*hdr_ui_layer_.handle()));
    beginColor(cmd, *hdr_ui_layer_.view(), extent, true);
    if (overlay_) overlay_(*cmd);
    compat::endRendering(cmd);
    barrierOne(cmd, toSampled(*hdr_ui_layer_.handle()));

    // 3) Composicion y codificacion (PQ o scRGB) en la swapchain.
    const vk::Image swap_image = swapchain_.images()[image_index];
    barrierOne(cmd, imageBarrier(swap_image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
                                 vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eNone,
                                 vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                 vk::AccessFlagBits2::eColorAttachmentWrite));
    beginColor(cmd, *swapchain_.imageViews()[image_index], extent, false);
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *hdr_output_pass_.pipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *hdr_output_pass_.layout(), 0,
                           *hdr_output_sets_[current_frame_], nullptr);
    GpuHdrOutputPush push{};
    push.encoding = swapchain_.encoding() == OutputEncoding::ScRgbLinear ? 2.0f : 1.0f;
    push.paper_white = clampPaperWhite(graphics_.hdr_paper_white);
    push.max_nits = clampMaxNits(graphics_.hdr_max_nits, graphics_.hdr_paper_white);
    push.manual_exposure = exposure_ * post_.manual_exposure;
    push.auto_exposure = post_.auto_exposure ? 1.0f : 0.0f;
    push.exposure_compensation = post_.exposure_compensation;
    // Sin escena HDR detras (modos de depuracion del editor): sin ganancia.
    push.gain_strength = mirror_only_ ? 0.0f : 1.0f;
    push.tonemapper = static_cast<float>(static_cast<std::int32_t>(post_.tonemapper));
    cmd.pushConstants<GpuHdrOutputPush>(*hdr_output_pass_.layout(), vk::ShaderStageFlagBits::eFragment, 0, push);
    cmd.draw(3, 1, 0, 0);
    compat::endRendering(cmd);

    barrierOne(cmd, imageBarrier(swap_image, vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::ePresentSrcKHR,
                                 vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                 vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eBottomOfPipe,
                                 vk::AccessFlagBits2::eNone));
}

// -----------------------------------------------------------------------------
// LUT 3D (.cube)
// -----------------------------------------------------------------------------

bool VulkanRenderer::uploadColorLut(const CubeLut& lut) {
    if (!lut.valid()) return false;
    const std::vector<std::uint16_t> texels = cubeLutToHalfRgba(lut);
    const vk::Format format = vk::Format::eR16G16B16A16Sfloat;
    const vk::DeviceSize bytes = texels.size() * sizeof(std::uint16_t);

    VulkanBuffer staging;
    staging.create(device_, bytes, vk::BufferUsageFlagBits::eTransferSrc,
                   vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    staging.write(texels.data(), bytes);

    // Los sets de la composicion apuntan a la anterior: nada en vuelo.
    device_.waitIdle();
    color_lut_view_ = nullptr;
    color_lut_image_ = nullptr;
    color_lut_memory_ = nullptr;

    vk::ImageCreateInfo image_info{};
    image_info.imageType = vk::ImageType::e3D;
    image_info.format = format;
    image_info.extent = vk::Extent3D{lut.size, lut.size, lut.size};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = vk::SampleCountFlagBits::e1;
    image_info.tiling = vk::ImageTiling::eOptimal;
    image_info.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
    image_info.initialLayout = vk::ImageLayout::eUndefined;
    color_lut_image_ = vk::raii::Image(device_.handle(), image_info);
    const vk::MemoryRequirements requirements = color_lut_image_.getMemoryRequirements();
    vk::MemoryAllocateInfo allocate{};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = device_.findMemoryType(requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    color_lut_memory_ = vk::raii::DeviceMemory(device_.handle(), allocate);
    color_lut_image_.bindMemory(*color_lut_memory_, 0);

    vk::ImageViewCreateInfo view_info{};
    view_info.image = *color_lut_image_;
    view_info.viewType = vk::ImageViewType::e3D;
    view_info.format = format;
    view_info.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    color_lut_view_ = vk::raii::ImageView(device_.handle(), view_info);

    const vk::Image image = *color_lut_image_;
    device_.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        barrierOne(cmd, imageBarrier(image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                                     vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                                     vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite));
        vk::BufferImageCopy region{};
        region.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        region.imageExtent = vk::Extent3D{lut.size, lut.size, lut.size};
        cmd.copyBufferToImage(*staging.handle(), image, vk::ImageLayout::eTransferDstOptimal, region);
        barrierOne(cmd, imageBarrier(image, vk::ImageLayout::eTransferDstOptimal, kRead,
                                     vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                                     vk::PipelineStageFlagBits2::eFragmentShader,
                                     vk::AccessFlagBits2::eShaderSampledRead));
    });
    color_lut_size_ = lut.size;
    color_lut_min_ = lut.domain_min;
    color_lut_max_ = lut.domain_max;
    return true;
}

void VulkanRenderer::updateColorLut() {
    const std::string& wanted = user_post_.color_lut;
    if (wanted == color_lut_path_ && *color_lut_view_) return;
    if (!wanted.empty() && wanted == color_lut_failed_) return;
    if (wanted.empty()) {
        if (color_lut_size_ == 2 && *color_lut_view_) {
            color_lut_path_.clear();
            return;
        }
        if (uploadColorLut(makeIdentityCubeLut(2))) {
            color_lut_path_.clear();
            writeColorLutDescriptors();
        }
        return;
    }
    CubeLut lut;
    std::string error;
    if (!loadCubeLut(pathFromUtf8(wanted), lut, error)) {
        std::cerr << "[Vulkan] " << error << "\n";
        color_lut_failed_ = wanted;
        return;
    }
    if (uploadColorLut(lut)) {
        color_lut_path_ = wanted;
        color_lut_failed_.clear();
        writeColorLutDescriptors();
        std::cout << "[Vulkan] LUT de color: " << wanted << " (" << lut.size << "^3"
                  << (lut.title.empty() ? std::string() : ", " + lut.title) << ")\n";
    }
}

void VulkanRenderer::writeColorLutDescriptors() {
    // La primera vez, la identidad (el binding 7 siempre tiene algo).
    if (!*color_lut_view_ && !uploadColorLut(makeIdentityCubeLut(2))) return;
    vk::DescriptorImageInfo info{};
    info.sampler = *composite_pass_.sampler();
    info.imageView = *color_lut_view_;
    info.imageLayout = kRead;
    const auto write = [&](const vk::raii::DescriptorSet& set) {
        vk::WriteDescriptorSet w{};
        w.dstSet = *set;
        w.dstBinding = 7;
        w.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        w.setImageInfo(info);
        device_.handle().updateDescriptorSets(w, nullptr);
    };
    for (const vk::raii::DescriptorSet& set : composite_sets_) write(set);
    for (const vk::raii::DescriptorSet& set : early_composite_sets_) write(set);
}

void VulkanRenderer::fillColorLutSettings(GpuCompositeSettings& settings) const {
    const bool active = !color_lut_path_.empty() && color_lut_size_ >= 2 && !post_.color_lut.empty();
    const float intensity = active ? std::clamp(post_.color_lut_intensity, 0.0f, 1.0f) : 0.0f;
    settings.lut_min = core::Vec4{color_lut_min_.x, color_lut_min_.y, color_lut_min_.z, intensity};
    settings.lut_max = core::Vec4{color_lut_max_.x, color_lut_max_.y, color_lut_max_.z,
                                  static_cast<float>(std::max(color_lut_size_, 2u))};
}

}  // namespace cramion::gfx
