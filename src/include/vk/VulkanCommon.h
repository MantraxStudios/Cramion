#ifndef CRAMION_VK_VULKAN_COMMON_H
#define CRAMION_VK_VULKAN_COMMON_H

// -----------------------------------------------------------------------------
// Configuracion global de Vulkan-Hpp.
//
// Este cabecero debe incluirse SIEMPRE antes que cualquier otro cabecero de
// Vulkan, porque define las macros que configuran la biblioteca:
//
//   VK_USE_PLATFORM_WIN32_KHR                      -> superficie Win32 (HWND).
//   VULKAN_HPP_HANDLE_ERROR_OUT_OF_DATE_AS_SUCCESS -> eErrorOutOfDateKHR se
//       devuelve como resultado normal en vez de lanzar excepcion, lo que
//       permite recrear el swapchain al redimensionar sin usar try/catch.
// -----------------------------------------------------------------------------

#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#if !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#if !defined(VULKAN_HPP_HANDLE_ERROR_OUT_OF_DATE_AS_SUCCESS)
#define VULKAN_HPP_HANDLE_ERROR_OUT_OF_DATE_AS_SUCCESS
#endif

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>

namespace cramion::gfx {

// Numero de frames que la CPU puede preparar por delante de la GPU.
inline constexpr std::uint32_t kMaxFramesInFlight = 2;

// Version minima de Vulkan exigida al dispositivo fisico. Se pide 1.3 porque
// el renderizador usa dynamic rendering y synchronization2 como parte del nucleo
// (sin extensiones).
inline constexpr std::uint32_t kMinimumApiVersion = VK_API_VERSION_1_3;

// Datos identificativos de la aplicacion, usados al crear la instancia.
struct EngineInfo {
    const char* app_name = "Cramion";
    const char* engine_name = "Cramion Engine";
    std::uint32_t app_version = VK_MAKE_API_VERSION(0, 0, 1, 0);
    std::uint32_t engine_version = VK_MAKE_API_VERSION(0, 0, 1, 0);

    // Activa las capas de validacion y el mensajero de depuracion. Si las capas
    // no estan instaladas en el sistema, se desactiva de forma silenciosa.
    bool enable_validation = true;
};

// Indices de las familias de colas que necesita el motor.
struct QueueFamilyIndices {
    std::uint32_t graphics = UINT32_MAX;
    std::uint32_t present = UINT32_MAX;

    bool isComplete() const {
        return graphics != UINT32_MAX && present != UINT32_MAX;
    }
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_VULKAN_COMMON_H
