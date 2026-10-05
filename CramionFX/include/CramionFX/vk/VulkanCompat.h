#ifndef CRAMION_VK_VULKAN_COMPAT_H
#define CRAMION_VK_VULKAN_COMPAT_H

// Compatibilidad con las GPU que no tienen Vulkan 1.3 (la mayoria de los
// moviles Android: Mali, Adreno y PowerVR con Vulkan 1.0/1.1/1.2).
//
// El renderizador esta escrito con dynamic rendering (vkCmdBeginRendering) y
// synchronization2 (vkCmdPipelineBarrier2), del nucleo de Vulkan 1.3. Si el
// dispositivo los tiene (1.3, o 1.1/1.2 con VK_KHR_dynamic_rendering y
// VK_KHR_synchronization2: Vulkan-Hpp usa la funcion KHR sola) no cambia
// nada. Si no:
//
//   - beginRendering()/endRendering() abren un VkRenderPass + VkFramebuffer
//     equivalentes (mismos destinos, mismas operaciones de carga y guardado y
//     el layout que pide el attachment al empezar y al terminar: sin
//     transiciones implicitas). Se crean la primera vez y se guardan.
//   - makeGraphicsPipeline() cambia el VkPipelineRenderingCreateInfo de la
//     cadena por un render pass compatible (mismos formatos).
//   - pipelineBarrier() traduce las etapas y accesos de synchronization2 a los
//     de vkCmdPipelineBarrier.
//
// Para crear framebuffers hace falta saber el formato de cada vista: quien
// crea una vista que se usa como destino de render la registra (registerView).

#include "CramionFX/vk/VulkanCommon.h"

#include <cstdint>
#include <vector>

namespace cramion::gfx {

class VulkanDevice;

namespace compat {

// Lo que el dispositivo trae de serie (lo rellena VulkanDevice).
struct Caps {
    std::uint32_t api_version = 0;
    bool dynamic_rendering = true;   // nativo (1.3 o la extension KHR)
    bool synchronization2 = true;    // nativo
    bool tessellation = false;       // etapas de teselacion activadas
    // Layouts de solo profundidad (DEPTH_ATTACHMENT_OPTIMAL...), de Vulkan 1.2.
    // Sin ellos, los combinados de profundidad y stencil (validos en 1.0).
    bool separate_depth_stencil_layouts = true;
    // Modo compatible: shaders de shaders/compat (SPIR-V 1.0, sin demote),
    // descriptores dentro de los minimos de Vulkan (16 texturas por etapa, 4
    // storage buffers), G-buffer de 4 destinos, escenario dibujado con culling
    // en la CPU (sin firstInstance en comandos indirectos), sin trazado de
    // rayos ni mesh shaders. Lo eligen las GPU de movil y las que no llegan a
    // los requisitos del camino de escritorio.
    bool lite = false;
    // Por que (para el log y la interfaz).
    const char* reason = "";
};

// El dispositivo ya creado y lo que trae (VulkanDevice, al crearlo).
void setDevice(const vk::raii::Device& device, const Caps& caps);
// Destruye los render passes y framebuffers guardados (antes de destruir el
// dispositivo).
void shutdown();
const Caps& caps();
inline bool lite() { return caps().lite; }

// Layout de la profundidad como destino y como textura de solo lectura.
inline vk::ImageLayout depthAttachmentLayout() {
    return caps().separate_depth_stencil_layouts ? vk::ImageLayout::eDepthAttachmentOptimal
                                                 : vk::ImageLayout::eDepthStencilAttachmentOptimal;
}
inline vk::ImageLayout depthReadOnlyLayout() {
    return caps().separate_depth_stencil_layouts ? vk::ImageLayout::eDepthReadOnlyOptimal
                                                 : vk::ImageLayout::eDepthStencilReadOnlyOptimal;
}

// Una vista de imagen que puede ser destino de render (color o profundidad).
// Si el handle se reutiliza (la vista anterior se destruyo), los
// framebuffers que la usaban se tiran.
void registerView(VkImageView view, vk::Format format);

void beginRendering(const vk::raii::CommandBuffer& cmd, const vk::RenderingInfo& info);
void endRendering(const vk::raii::CommandBuffer& cmd);
void pipelineBarrier(const vk::raii::CommandBuffer& cmd, const vk::DependencyInfo& dependency);

// Crea un pipeline grafico (con la cache de pipelines del dispositivo). Sin
// dynamic rendering, con un render pass compatible con su
// VkPipelineRenderingCreateInfo.
vk::raii::Pipeline makeGraphicsPipeline(const VulkanDevice& device, vk::GraphicsPipelineCreateInfo info);

// Render pass compatible con estos formatos (para librerias que no saben de
// dynamic rendering, como el backend de ImGui). VK_NULL_HANDLE con dynamic
// rendering nativo.
VkRenderPass compatibleRenderPass(const std::vector<vk::Format>& colors, vk::Format depth = vk::Format::eUndefined);

}  // namespace compat
}  // namespace cramion::gfx

#endif  // CRAMION_VK_VULKAN_COMPAT_H
