#ifndef CRAMION_VK_VULKAN_SWAPCHAIN_H
#define CRAMION_VK_VULKAN_SWAPCHAIN_H

#include "CramionFX/vk/DisplayOutputVk.h"
#include "CramionFX/vk/VulkanCommon.h"

#include <vector>

namespace cramion::gfx {

class VulkanDevice;
class VulkanSurface;

// Paso 4 de la inicializacion: cadena de intercambio (swapchain) y sus vistas
// de imagen.
//
// La swapchain depende del tamano de la ventana, asi que se recrea cada vez que
// esta cambia de tamano o el driver la marca como obsoleta.
class VulkanSwapchain {
public:
    VulkanSwapchain() = default;
    ~VulkanSwapchain() = default;

    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

    void initialize(const VulkanDevice& device, const VulkanSurface& surface, std::uint32_t width,
                    std::uint32_t height);

    // Vuelve a crear la swapchain con el tamano indicado reutilizando la
    // anterior. Devuelve false si la ventana esta minimizada (area cero), caso
    // en el que no hay nada que crear.
    bool recreate(std::uint32_t width, std::uint32_t height);
    // Sincronia vertical (FIFO) o mailbox; se aplica al recrearla.
    void setVsync(bool vsync) { vsync_ = vsync; }
    // Salida HDR que se pide (DisplayOutput.h); se aplica al recrearla.
    // `colorspace_extension`: VK_EXT_swapchain_colorspace en la instancia.
    void setOutputRequest(HdrOutput wanted, bool colorspace_extension) {
        hdr_request_ = wanted;
        colorspace_extension_ = colorspace_extension;
    }
    HdrOutput outputRequest() const { return hdr_request_; }
    // NVIDIA Reflex: la swapchain se crea con VkSwapchainLatencyCreateInfoNV
    // (solo si el dispositivo tiene VK_NV_low_latency2).
    void setLatencyModeEnable(bool enable) { latency_mode_enable_ = enable; }
    bool latencyModeEnabled() const { return latency_created_; }
    bool latencyModeRequested() const { return latency_mode_enable_; }
    // Metadatos HDR10 de la pantalla (VK_EXT_hdr_metadata): nits maximos y
    // medios de la imagen. No hace nada en SDR o sin la extension.
    void setHdrMetadata(float max_nits, float paper_white) const;

    void shutdown();

    const vk::raii::SwapchainKHR& handle() const { return swapchain_; }
    vk::Format imageFormat() const { return image_format_; }
    // Formato SDR: el de la imagen de la UI (y el FXAA) y el de los pipelines
    // de las herramientas (ImGui). Igual a imageFormat() en SDR; en HDR la
    // UI se dibuja aparte en este formato y hdr_output.frag la compone.
    vk::Format uiFormat() const { return ui_format_; }
    vk::ColorSpaceKHR colorSpace() const { return color_space_; }
    OutputEncoding encoding() const { return encoding_; }
    bool hdrActive() const { return encoding_ != OutputEncoding::Sdr; }
    bool hdrFellBack() const { return hdr_fell_back_; }
    vk::Extent2D extent() const { return extent_; }
    std::uint32_t imageCount() const { return static_cast<std::uint32_t>(images_.size()); }

    const std::vector<vk::Image>& images() const { return images_; }
    const std::vector<vk::raii::ImageView>& imageViews() const { return image_views_; }

    // true si hay una swapchain utilizable (ventana con area > 0).
    bool isValid() const { return *swapchain_ != VK_NULL_HANDLE; }

private:
    // false si la superficie no tiene area (ventana minimizada): no se crea
    // nada, pero el formato queda elegido (las pipelines lo necesitan).
    bool create(std::uint32_t width, std::uint32_t height);
    void createImageViews();

    vk::PresentModeKHR choosePresentMode(const std::vector<vk::PresentModeKHR>& available) const;
    bool vsync_ = false;
    HdrOutput hdr_request_ = HdrOutput::Off;
    bool colorspace_extension_ = false;
    bool latency_mode_enable_ = false;
    bool latency_created_ = false;
    bool hdr_fell_back_ = false;
    bool hdr_fallback_logged_ = false;
    bool zero_area_logged_ = false;  // aviso de "minimizada": una vez por episodio
    static vk::Extent2D chooseExtent(const vk::SurfaceCapabilitiesKHR& capabilities,
                                     std::uint32_t width, std::uint32_t height);

    // Referencias a los objetos de los que depende; propiedad del renderizador.
    const VulkanDevice* device_ = nullptr;
    const VulkanSurface* surface_ = nullptr;

    // Orden de declaracion = orden de creacion; la destruccion es inversa, por
    // lo que las vistas mueren antes que la swapchain.
    vk::raii::SwapchainKHR swapchain_{nullptr};
    std::vector<vk::Image> images_;
    std::vector<vk::raii::ImageView> image_views_;

    vk::Format image_format_ = vk::Format::eUndefined;
    vk::Format ui_format_ = vk::Format::eUndefined;
    vk::ColorSpaceKHR color_space_ = vk::ColorSpaceKHR::eSrgbNonlinear;
    OutputEncoding encoding_ = OutputEncoding::Sdr;
    vk::Extent2D extent_{0, 0};
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_VULKAN_SWAPCHAIN_H
