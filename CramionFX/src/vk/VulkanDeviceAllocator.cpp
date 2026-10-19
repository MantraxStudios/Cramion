// Implementacion de Vulkan Memory Allocator (vendor/vma, MIT). Va sola en
// este archivo (sin avisos: ver CMakeLists.txt); el resto de CramionFX solo
// incluye la cabecera.

#include "CramionFX/vk/VulkanCommon.h"

#include <cstdio>

// Las funciones de Vulkan se las da VulkanDevice::createAllocator
// (vkGetInstanceProcAddr / vkGetDeviceProcAddr): VMA carga el resto.
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
// Recursos sin liberar al destruir el asignador: aviso, no un assert (el
// dispositivo ya lo comprueba antes y entonces no lo destruye).
#define VMA_ASSERT_LEAK(expr)                                                              \
    do {                                                                                   \
        if (!(expr)) std::fprintf(stderr, "[Vulkan] VMA: memoria sin liberar (%s)\n", #expr); \
    } while (false)
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
