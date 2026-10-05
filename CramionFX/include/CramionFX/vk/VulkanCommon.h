#ifndef CRAMION_VK_VULKAN_COMMON_H
#define CRAMION_VK_VULKAN_COMMON_H

// -----------------------------------------------------------------------------
// Configuracion global de Vulkan-Hpp.
//
// Este cabecero debe incluirse SIEMPRE antes que cualquier otro cabecero de
// Vulkan, porque define las macros que configuran la biblioteca:
//
//   VK_USE_PLATFORM_WIN32_KHR                      -> superficie Win32 (HWND).
//   VK_USE_PLATFORM_ANDROID_KHR (Android)          -> ANativeWindow.
//   VULKAN_HPP_HANDLE_ERROR_OUT_OF_DATE_AS_SUCCESS -> eErrorOutOfDateKHR se
//       devuelve como resultado normal en vez de lanzar excepcion, lo que
//       permite recrear el swapchain al redimensionar sin usar try/catch.
// -----------------------------------------------------------------------------

#if defined(__ANDROID__)
#if !defined(VK_USE_PLATFORM_ANDROID_KHR)
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <android/native_window.h>
#else
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#if !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#endif
#if !defined(VULKAN_HPP_HANDLE_ERROR_OUT_OF_DATE_AS_SUCCESS)
#define VULKAN_HPP_HANDLE_ERROR_OUT_OF_DATE_AS_SUCCESS
#endif

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>

namespace cramion::gfx {

// La ventana donde se presenta: HWND en Windows, ANativeWindow en Android.
#if defined(__ANDROID__)
using NativeWindow = ANativeWindow*;
#else
using NativeWindow = HWND;
#endif

// Numero de frames que la CPU puede preparar por delante de la GPU.
inline constexpr std::uint32_t kMaxFramesInFlight = 2;

// Version minima de Vulkan del dispositivo fisico. El camino completo (el de
// escritorio) usa Vulkan 1.3 (dynamic rendering, synchronization2); con menos
// (la mayoria de los moviles Android, GPU de PC antiguas) el renderizador va
// por el modo compatible (VulkanCompat.h), que vale desde Vulkan 1.0.
inline constexpr std::uint32_t kMinimumApiVersion = VK_API_VERSION_1_0;

// Datos identificativos de la aplicacion, usados al crear la instancia.
struct EngineInfo {
    const char* app_name = "Cramion";
    const char* engine_name = "Cramion Engine";
    std::uint32_t app_version = VK_MAKE_API_VERSION(0, 0, 1, 0);
    std::uint32_t engine_version = VK_MAKE_API_VERSION(0, 0, 1, 0);

    // Activa las capas de validacion y el mensajero de depuracion. Si las capas
    // no estan instaladas en el sistema, se desactiva de forma silenciosa.
    bool enable_validation = true;

    // Realidad virtual (OpenXR): si hay runtime y casco, Vulkan se crea en la
    // GPU del casco con las extensiones que pide. Sin casco, sigue sin VR.
    bool enable_xr = false;
    // Que runtime de OpenXR probar (xr::RuntimeChoice: 0 automatico, 1 SteamVR,
    // 2 Meta, 3 el activo de Windows).
    int xr_runtime = 0;
    // Con enable_xr: true, la sesion de VR empieza al crear el render (el
    // juego exportado). false: solo se prepara Vulkan para el casco (su GPU y
    // sus extensiones) y la sesion se abre despues con connectXrSession (el
    // editor, al dar Play on VR): abrir el editor no le quita el casco a nadie.
    bool xr_session = true;
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
