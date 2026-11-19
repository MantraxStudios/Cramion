#ifndef CRAMION_VK_DISPLAY_OUTPUT_VK_H
#define CRAMION_VK_DISPLAY_OUTPUT_VK_H

// Eleccion del formato y el espacio de color de la swapchain (DisplayOutput.h).

#include "CramionFX/vk/DisplayOutput.h"
#include "CramionFX/vk/VulkanCommon.h"

#include <vector>

namespace cramion::gfx {

struct SurfaceFormatChoice {
    vk::SurfaceFormatKHR surface{};                    // lo que se pide a la swapchain
    vk::Format ui_format = vk::Format::eUndefined;     // formato SDR (pipelines de la UI y del FXAA)
    OutputEncoding encoding = OutputEncoding::Sdr;
    bool hdr_fell_back = false;  // se pidio HDR y no se pudo
};

// Elige el formato de la swapchain. El SDR es el de siempre (UNORM de 8 bits:
// la composicion aplica la curva sRGB a mano; uno _SRGB la aplicaria otra
// vez). Lanza si la lista esta vacia (como antes). `colorspace_extension`:
// VK_EXT_swapchain_colorspace activada en la instancia (sin ella el driver no
// puede ofrecer otros espacios de color y no se miran).
SurfaceFormatChoice chooseOutputFormat(const std::vector<vk::SurfaceFormatKHR>& available, HdrOutput wanted,
                                       bool colorspace_extension);

}  // namespace cramion::gfx

#endif  // CRAMION_VK_DISPLAY_OUTPUT_VK_H
