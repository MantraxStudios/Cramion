#ifndef CRAMION_VK_TEMPORAL_UPSCALERS_H
#define CRAMION_VK_TEMPORAL_UPSCALERS_H

// Escaladores temporales de los fabricantes, sobre las mismas entradas que el
// TAA del motor (color HDR, profundidad y vectores de movimiento a la
// resolucion interna, y el jitter de la camara):
//
//   Fsr3Upscaler   AMD FidelityFX Super Resolution 3.1 (cualquier GPU). La
//                  DLL firmada (amd_fidelityfx_vk.dll) se carga al crearlo:
//                  sin ella, no esta disponible y el motor usa el TAA.
//   DlssUpscaler   NVIDIA DLSS 4 Super Resolution / DLAA (RTX): el modelo
//                  transformer. NGX pide extensiones de Vulkan que se anaden
//                  al crear la instancia y el dispositivo; nvngx_dlss.dll va
//                  junto al .exe.
//
// Las cabeceras de los SDK no salen de TemporalUpscalers.cpp.

#include "CramionFX/vk/GraphicsSettings.h"
#include "CramionFX/vk/VulkanCommon.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace cramion::gfx {

struct UpscaleImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool depth = false;
};

// Una llamada: las imagenes ya en su layout (entradas en
// SHADER_READ_ONLY_OPTIMAL, salida en GENERAL) y lo que pide cada SDK.
struct UpscaleDispatch {
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    UpscaleImage color;   // HDR lineal, resolucion interna
    UpscaleImage depth;   // [0 cerca, 1 lejos]
    UpscaleImage motion;  // UV actual - UV anterior (sin jitter), resolucion interna
    UpscaleImage output;  // pantalla (con uso STORAGE)
    std::uint32_t render_width = 0;
    std::uint32_t render_height = 0;
    float jitter_x = 0.0f;  // desplazamiento de la imagen este frame, en pixeles internos (+x derecha, +y abajo)
    float jitter_y = 0.0f;
    bool reset = false;     // la camara salto (corte, escena nueva): sin historia
    float frame_ms = 16.6f;
    float near_plane = 0.1f;
    float far_plane = 1000.0f;
    float fov_y = 1.2f;     // radianes
};

class Fsr3Upscaler {
public:
    Fsr3Upscaler();
    ~Fsr3Upscaler();
    Fsr3Upscaler(const Fsr3Upscaler&) = delete;
    Fsr3Upscaler& operator=(const Fsr3Upscaler&) = delete;

    // El motor se compilo con FSR 3 y la DLL esta junto al ejecutable.
    static bool supported();
    bool create(VkInstance instance, VkDevice device, VkPhysicalDevice physical_device, std::uint32_t max_render_width,
                std::uint32_t max_render_height, std::uint32_t output_width, std::uint32_t output_height,
                std::string& error);
    void destroy();
    bool ready() const;
    bool dispatch(const UpscaleDispatch& d);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class DlssUpscaler {
public:
    DlssUpscaler();
    ~DlssUpscaler();
    DlssUpscaler(const DlssUpscaler&) = delete;
    DlssUpscaler& operator=(const DlssUpscaler&) = delete;

    static bool compiled();
    // Lo que NGX necesita en la instancia y en el dispositivo (se anaden si existen).
    static std::vector<std::string> instanceExtensions();
    static std::vector<std::string> deviceExtensions(VkInstance instance);

    // Tras crear el dispositivo. false si la GPU o el controlador no tienen DLSS.
    bool initialize(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
                    const std::filesystem::path& logs_folder, std::string& error);
    bool available() const;
    // Resolucion interna y de salida, y el modo (Native = DLAA).
    bool createFeature(VkCommandBuffer command_buffer, std::uint32_t render_width, std::uint32_t render_height,
                       std::uint32_t output_width, std::uint32_t output_height, UpscaleQuality quality,
                       std::string& error);
    bool featureReady() const;
    void releaseFeature();
    bool evaluate(const UpscaleDispatch& d);
    // Antes de destruir el dispositivo.
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_TEMPORAL_UPSCALERS_H
