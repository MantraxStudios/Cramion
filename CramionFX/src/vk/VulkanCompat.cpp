#include "CramionFX/vk/VulkanCompat.h"

#include "CramionFX/vk/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

namespace cramion::gfx::compat {
namespace {

struct Framebuffer {
    vk::raii::Framebuffer handle{nullptr};
    std::vector<VkImageView> views;
};

struct State {
    const vk::raii::Device* device = nullptr;
    Caps caps;
    std::mutex mutex;  // las vistas se registran tambien desde hilos de carga
    std::unordered_map<VkImageView, vk::Format> views;
    // Render passes por su descripcion (formatos, operaciones y layouts).
    std::map<std::vector<std::uint64_t>, vk::raii::RenderPass> render_passes;
    std::map<std::vector<std::uint64_t>, Framebuffer> framebuffers;
    bool emulating = false;  // la pasada abierta es un render pass
};

State& state() {
    static State s;
    return s;
}

// --- Descripcion de un render pass ---
struct AttachmentDesc {
    vk::Format format = vk::Format::eUndefined;
    vk::AttachmentLoadOp load = vk::AttachmentLoadOp::eDontCare;
    vk::AttachmentStoreOp store = vk::AttachmentStoreOp::eStore;
    vk::AttachmentLoadOp stencil_load = vk::AttachmentLoadOp::eDontCare;
    vk::AttachmentStoreOp stencil_store = vk::AttachmentStoreOp::eDontCare;
    vk::ImageLayout layout = vk::ImageLayout::eUndefined;
};

struct PassDesc {
    std::vector<AttachmentDesc> colors;  // formato eUndefined = hueco sin usar
    AttachmentDesc depth;                // formato eUndefined = sin profundidad
    vk::SampleCountFlagBits samples = vk::SampleCountFlagBits::e1;

    std::vector<std::uint64_t> key() const {
        std::vector<std::uint64_t> k;
        const auto add = [&](const AttachmentDesc& a) {
            k.push_back(static_cast<std::uint64_t>(a.format));
            k.push_back((static_cast<std::uint64_t>(a.load) << 48) | (static_cast<std::uint64_t>(a.store) << 32) |
                        (static_cast<std::uint64_t>(a.stencil_load) << 16) | static_cast<std::uint64_t>(a.stencil_store));
            k.push_back(static_cast<std::uint64_t>(a.layout));
        };
        k.push_back(static_cast<std::uint64_t>(samples));
        k.push_back(colors.size());
        for (const AttachmentDesc& c : colors) add(c);
        add(depth);
        return k;
    }
};

// Los handles no despachables son punteros en 64 bits y uint64_t en 32 bits
// (Android armeabi-v7a).
template <typename T>
std::uint64_t handleBits(T handle) {
    if constexpr (std::is_pointer_v<T>) {
        return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle));
    } else {
        return static_cast<std::uint64_t>(handle);
    }
}

bool hasStencil(vk::Format format) {
    return format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint ||
           format == vk::Format::eD16UnormS8Uint || format == vk::Format::eS8Uint;
}

// Un layout de destino que un render pass acepta en sus referencias.
vk::ImageLayout subpassLayout(vk::ImageLayout layout, bool depth) {
    if (layout == vk::ImageLayout::eUndefined) {
        return depth ? vk::ImageLayout::eDepthStencilAttachmentOptimal : vk::ImageLayout::eColorAttachmentOptimal;
    }
    return layout;
}

VkRenderPass renderPass(const PassDesc& desc) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    const std::vector<std::uint64_t> key = desc.key();
    // (static_cast: en 32 bits los handles son uint64_t y Vulkan-Hpp no los
    // convierte implicitamente.)
    if (const auto it = s.render_passes.find(key); it != s.render_passes.end()) {
        return static_cast<VkRenderPass>(*it->second);
    }

    std::vector<vk::AttachmentDescription> attachments;
    std::vector<vk::AttachmentReference> color_refs;
    for (const AttachmentDesc& c : desc.colors) {
        if (c.format == vk::Format::eUndefined) {
            color_refs.push_back(vk::AttachmentReference{VK_ATTACHMENT_UNUSED, vk::ImageLayout::eUndefined});
            continue;
        }
        const vk::ImageLayout layout = subpassLayout(c.layout, false);
        vk::AttachmentDescription a{};
        a.format = c.format;
        a.samples = desc.samples;
        a.loadOp = c.load;
        a.storeOp = c.store;
        a.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
        a.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
        // Igual que con dynamic rendering: el attachment ya esta (y se queda)
        // en su layout; las transiciones las hacen las barreras de fuera.
        a.initialLayout = c.load == vk::AttachmentLoadOp::eLoad ? layout : vk::ImageLayout::eUndefined;
        a.finalLayout = layout;
        color_refs.push_back(vk::AttachmentReference{static_cast<std::uint32_t>(attachments.size()), layout});
        attachments.push_back(a);
    }
    vk::AttachmentReference depth_ref{VK_ATTACHMENT_UNUSED, vk::ImageLayout::eUndefined};
    const bool has_depth = desc.depth.format != vk::Format::eUndefined;
    if (has_depth) {
        const vk::ImageLayout layout = subpassLayout(desc.depth.layout, true);
        vk::AttachmentDescription a{};
        a.format = desc.depth.format;
        a.samples = desc.samples;
        a.loadOp = desc.depth.load;
        a.storeOp = desc.depth.store;
        a.stencilLoadOp = desc.depth.stencil_load;
        a.stencilStoreOp = desc.depth.stencil_store;
        const bool keeps = desc.depth.load == vk::AttachmentLoadOp::eLoad ||
                           desc.depth.stencil_load == vk::AttachmentLoadOp::eLoad;
        a.initialLayout = keeps ? layout : vk::ImageLayout::eUndefined;
        a.finalLayout = layout;
        depth_ref = vk::AttachmentReference{static_cast<std::uint32_t>(attachments.size()), layout};
        attachments.push_back(a);
    }

    vk::SubpassDescription subpass{};
    subpass.pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
    subpass.colorAttachmentCount = static_cast<std::uint32_t>(color_refs.size());
    subpass.pColorAttachments = color_refs.empty() ? nullptr : color_refs.data();
    subpass.pDepthStencilAttachment = has_depth ? &depth_ref : nullptr;

    // Las dependencias con lo de fuera las ponen las barreras del renderizador
    // (como con dynamic rendering); estas solo ordenan los destinos del pase.
    std::array<vk::SubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput |
                                   vk::PipelineStageFlagBits::eLateFragmentTests;
    dependencies[0].dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput |
                                   vk::PipelineStageFlagBits::eEarlyFragmentTests;
    dependencies[0].srcAccessMask =
        vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    dependencies[0].dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite |
                                    vk::AccessFlagBits::eDepthStencilAttachmentRead |
                                    vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput |
                                   vk::PipelineStageFlagBits::eLateFragmentTests;
    dependencies[1].dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput |
                                   vk::PipelineStageFlagBits::eEarlyFragmentTests;
    dependencies[1].srcAccessMask =
        vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    dependencies[1].dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite |
                                    vk::AccessFlagBits::eDepthStencilAttachmentRead |
                                    vk::AccessFlagBits::eDepthStencilAttachmentWrite;

    vk::RenderPassCreateInfo info{};
    info.setAttachments(attachments);
    info.setSubpasses(subpass);
    info.setDependencies(dependencies);
    auto [it, inserted] = s.render_passes.emplace(key, vk::raii::RenderPass(*s.device, info));
    (void)inserted;
    return static_cast<VkRenderPass>(*it->second);
}

vk::Format viewFormat(VkImageView view) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    const auto it = s.views.find(view);
    if (it == s.views.end()) {
        throw std::runtime_error("[Vulkan] Vista sin registrar como destino de render (compat::registerView)");
    }
    return it->second;
}

// --- synchronization2 -> vkCmdPipelineBarrier ---
constexpr std::uint64_t kLegacyStageBits = 0xFFFFFFFFull;

vk::PipelineStageFlags legacyStages(vk::PipelineStageFlags2 stages2, bool source) {
    const auto bits = static_cast<std::uint64_t>(static_cast<VkPipelineStageFlags2>(stages2));
    std::uint64_t legacy = bits & kLegacyStageBits;
    if (bits & (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT |
                VK_PIPELINE_STAGE_2_CLEAR_BIT)) {
        legacy |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    if (bits & (VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT)) {
        legacy |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    }
    if (bits & VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT) {
        legacy |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
        if (state().caps.tessellation) {
            legacy |= VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
        }
    }
    // Etapas que el dispositivo no tiene activadas no pueden aparecer.
    if (!state().caps.tessellation) {
        legacy &= ~static_cast<std::uint64_t>(VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
                                              VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT);
    }
    legacy &= ~static_cast<std::uint64_t>(VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT);
    // Las de extensiones (rayos, mesh shaders...) no existen en este modo.
    legacy &= 0x1FFFFull;
    if (legacy == 0) legacy = source ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    return vk::PipelineStageFlags(static_cast<VkPipelineStageFlags>(legacy));
}

vk::AccessFlags legacyAccess(vk::AccessFlags2 access2) {
    const auto bits = static_cast<std::uint64_t>(static_cast<VkAccessFlags2>(access2));
    std::uint64_t legacy = bits & 0x1FFFFull;
    if (bits & (VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT)) {
        legacy |= VK_ACCESS_SHADER_READ_BIT;
    }
    if (bits & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT) legacy |= VK_ACCESS_SHADER_WRITE_BIT;
    return vk::AccessFlags(static_cast<VkAccessFlags>(legacy));
}

}  // namespace

void setDevice(const vk::raii::Device& device, const Caps& caps) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.framebuffers.clear();
    s.render_passes.clear();
    s.device = &device;
    s.caps = caps;
}

void shutdown() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.framebuffers.clear();
    s.render_passes.clear();
    s.views.clear();
    s.device = nullptr;
    s.emulating = false;
}

const Caps& caps() {
    return state().caps;
}

void registerView(VkImageView view, vk::Format format) {
    if (view == VK_NULL_HANDLE) return;
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.views[view] = format;
    if (s.caps.dynamic_rendering || s.framebuffers.empty()) return;
    // El handle es de una vista nueva: los framebuffers de la anterior (ya
    // destruida) no valen.
    for (auto it = s.framebuffers.begin(); it != s.framebuffers.end();) {
        if (std::find(it->second.views.begin(), it->second.views.end(), view) != it->second.views.end()) {
            it = s.framebuffers.erase(it);
        } else {
            ++it;
        }
    }
}

void beginRendering(const vk::raii::CommandBuffer& cmd, const vk::RenderingInfo& info) {
    State& s = state();
    if (s.caps.dynamic_rendering) {
        cmd.beginRendering(info);
        return;
    }

    // --- El render pass equivalente ---
    PassDesc desc;
    std::vector<VkImageView> views;
    std::vector<vk::ClearValue> clears;
    for (std::uint32_t i = 0; i < info.colorAttachmentCount; ++i) {
        const vk::RenderingAttachmentInfo& a = info.pColorAttachments[i];
        AttachmentDesc c;
        if (a.imageView) {
            c.format = viewFormat(static_cast<VkImageView>(a.imageView));
            c.load = a.loadOp;
            c.store = a.storeOp;
            c.layout = a.imageLayout;
            views.push_back(static_cast<VkImageView>(a.imageView));
            clears.push_back(a.clearValue);
        }
        desc.colors.push_back(c);
    }
    const vk::RenderingAttachmentInfo* depth = info.pDepthAttachment;
    const vk::RenderingAttachmentInfo* stencil = info.pStencilAttachment;
    const vk::RenderingAttachmentInfo* ds = depth != nullptr && depth->imageView ? depth
                                            : stencil != nullptr && stencil->imageView ? stencil
                                                                                       : nullptr;
    if (ds != nullptr) {
        desc.depth.format = viewFormat(static_cast<VkImageView>(ds->imageView));
        desc.depth.layout = ds->imageLayout;
        if (depth != nullptr && depth->imageView) {
            desc.depth.load = depth->loadOp;
            desc.depth.store = depth->storeOp;
        } else {
            desc.depth.store = vk::AttachmentStoreOp::eDontCare;
        }
        if (stencil != nullptr && stencil->imageView && hasStencil(desc.depth.format)) {
            desc.depth.stencil_load = stencil->loadOp;
            desc.depth.stencil_store = stencil->storeOp;
        }
        views.push_back(static_cast<VkImageView>(ds->imageView));
        vk::ClearValue clear = ds->clearValue;
        if (depth != nullptr && depth->imageView && stencil != nullptr && stencil->imageView) {
            clear.depthStencil.stencil = stencil->clearValue.depthStencil.stencil;
        }
        clears.push_back(clear);
    }
    const VkRenderPass pass = renderPass(desc);

    // --- Su framebuffer (los destinos y el tamano de esta pasada) ---
    const vk::Extent2D extent{info.renderArea.offset.x + info.renderArea.extent.width,
                              info.renderArea.offset.y + info.renderArea.extent.height};
    std::vector<std::uint64_t> key = {handleBits(pass), extent.width, extent.height, std::max(info.layerCount, 1u)};
    for (const VkImageView v : views) key.push_back(handleBits(v));
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        auto it = s.framebuffers.find(key);
        if (it == s.framebuffers.end()) {
            vk::FramebufferCreateInfo fb{};
            fb.renderPass = vk::RenderPass(pass);
            std::vector<vk::ImageView> attachments(views.begin(), views.end());
            fb.setAttachments(attachments);
            fb.width = extent.width;
            fb.height = extent.height;
            fb.layers = std::max(info.layerCount, 1u);
            Framebuffer entry;
            entry.handle = vk::raii::Framebuffer(*s.device, fb);
            entry.views = views;
            it = s.framebuffers.emplace(key, std::move(entry)).first;
        }
        framebuffer = static_cast<VkFramebuffer>(*it->second.handle);
    }

    vk::RenderPassBeginInfo begin{};
    begin.renderPass = vk::RenderPass(pass);
    begin.framebuffer = vk::Framebuffer(framebuffer);
    begin.renderArea = info.renderArea;
    begin.setClearValues(clears);
    cmd.beginRenderPass(begin, vk::SubpassContents::eInline);
    s.emulating = true;
}

void endRendering(const vk::raii::CommandBuffer& cmd) {
    State& s = state();
    if (s.emulating) {
        cmd.endRenderPass();
        s.emulating = false;
        return;
    }
    cmd.endRendering();
}

void pipelineBarrier(const vk::raii::CommandBuffer& cmd, const vk::DependencyInfo& dependency) {
    if (state().caps.synchronization2) {
        cmd.pipelineBarrier2(dependency);
        return;
    }
    vk::PipelineStageFlags2 src2{};
    vk::PipelineStageFlags2 dst2{};
    std::vector<vk::MemoryBarrier> memory;
    std::vector<vk::BufferMemoryBarrier> buffers;
    std::vector<vk::ImageMemoryBarrier> images;
    for (std::uint32_t i = 0; i < dependency.memoryBarrierCount; ++i) {
        const vk::MemoryBarrier2& b = dependency.pMemoryBarriers[i];
        src2 |= b.srcStageMask;
        dst2 |= b.dstStageMask;
        memory.push_back(vk::MemoryBarrier{legacyAccess(b.srcAccessMask), legacyAccess(b.dstAccessMask)});
    }
    for (std::uint32_t i = 0; i < dependency.bufferMemoryBarrierCount; ++i) {
        const vk::BufferMemoryBarrier2& b = dependency.pBufferMemoryBarriers[i];
        src2 |= b.srcStageMask;
        dst2 |= b.dstStageMask;
        buffers.push_back(vk::BufferMemoryBarrier{legacyAccess(b.srcAccessMask), legacyAccess(b.dstAccessMask),
                                                  b.srcQueueFamilyIndex, b.dstQueueFamilyIndex, b.buffer, b.offset,
                                                  b.size});
    }
    for (std::uint32_t i = 0; i < dependency.imageMemoryBarrierCount; ++i) {
        const vk::ImageMemoryBarrier2& b = dependency.pImageMemoryBarriers[i];
        src2 |= b.srcStageMask;
        dst2 |= b.dstStageMask;
        images.push_back(vk::ImageMemoryBarrier{legacyAccess(b.srcAccessMask), legacyAccess(b.dstAccessMask),
                                                b.oldLayout, b.newLayout, b.srcQueueFamilyIndex, b.dstQueueFamilyIndex,
                                                b.image, b.subresourceRange});
    }
    cmd.pipelineBarrier(legacyStages(src2, true), legacyStages(dst2, false), dependency.dependencyFlags, memory, buffers,
                        images);
}

vk::raii::Pipeline makeGraphicsPipeline(const VulkanDevice& device, vk::GraphicsPipelineCreateInfo info) {
    State& s = state();
    if (!s.caps.dynamic_rendering && info.renderPass == VK_NULL_HANDLE) {
        // Busca el VkPipelineRenderingCreateInfo en la cadena y lo quita.
        const vk::PipelineRenderingCreateInfo* rendering = nullptr;
        const vk::BaseOutStructure* previous = nullptr;
        for (auto* node = static_cast<const vk::BaseOutStructure*>(info.pNext); node != nullptr; node = node->pNext) {
            if (node->sType == vk::StructureType::ePipelineRenderingCreateInfo) {
                rendering = reinterpret_cast<const vk::PipelineRenderingCreateInfo*>(node);
                break;
            }
            previous = node;
        }
        if (rendering != nullptr) {
            PassDesc desc;
            if (info.pMultisampleState != nullptr) desc.samples = info.pMultisampleState->rasterizationSamples;
            for (std::uint32_t i = 0; i < rendering->colorAttachmentCount; ++i) {
                AttachmentDesc c;
                c.format = rendering->pColorAttachmentFormats[i];
                desc.colors.push_back(c);
            }
            const vk::Format depth = rendering->depthAttachmentFormat != vk::Format::eUndefined
                                         ? rendering->depthAttachmentFormat
                                         : rendering->stencilAttachmentFormat;
            desc.depth.format = depth;
            info.renderPass = vk::RenderPass(renderPass(desc));
            info.subpass = 0;
            if (previous == nullptr) {
                info.pNext = rendering->pNext;
            } else {
                // Se quita de la cadena del llamador solo mientras se crea (la
                // misma descripcion se reutiliza, p. ej. para la variante en
                // lineas).
                auto* patched = const_cast<vk::BaseOutStructure*>(previous);
                vk::BaseOutStructure* const original = patched->pNext;
                patched->pNext = static_cast<vk::BaseOutStructure*>(const_cast<void*>(rendering->pNext));
                struct Restore {
                    vk::BaseOutStructure* node;
                    vk::BaseOutStructure* next;
                    ~Restore() { node->pNext = next; }
                } restore{patched, original};
                return vk::raii::Pipeline(device.handle(), device.pipelineCache(), info);
            }
        }
    }
    return vk::raii::Pipeline(device.handle(), device.pipelineCache(), info);
}

VkRenderPass compatibleRenderPass(const std::vector<vk::Format>& colors, vk::Format depth) {
    State& s = state();
    if (s.caps.dynamic_rendering || s.device == nullptr) return VK_NULL_HANDLE;
    PassDesc desc;
    for (const vk::Format f : colors) {
        AttachmentDesc c;
        c.format = f;
        desc.colors.push_back(c);
    }
    desc.depth.format = depth;
    return renderPass(desc);
}

}  // namespace cramion::gfx::compat
