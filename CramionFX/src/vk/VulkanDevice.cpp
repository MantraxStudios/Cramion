#include "CramionFX/vk/VulkanDevice.h"

#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/VulkanInstance.h"
#include "CramionFX/vk/VulkanSurface.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string_view>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

namespace cramion::gfx {
namespace {

// Extensiones que el dispositivo logico necesita para presentar en pantalla.
constexpr std::array<const char*, 1> kRequiredDeviceExtensions = {
    vk::KHRSwapchainExtensionName,
};

// Trazado de rayos (opcionales: se activan si la GPU las tiene todas).
constexpr std::array<const char*, 3> kRayTracingExtensions = {
    vk::KHRAccelerationStructureExtensionName,
    vk::KHRRayQueryExtensionName,
    vk::KHRDeferredHostOperationsExtensionName,
};

bool hasExtension(const std::vector<vk::ExtensionProperties>& available, const char* name) {
    return std::any_of(available.begin(), available.end(), [&](const vk::ExtensionProperties& p) {
        return std::strcmp(p.extensionName.data(), name) == 0;
    });
}

// Version de la instancia (la del loader): con una instancia 1.0/1.1 no se
// pueden usar funciones de 1.3 aunque la GPU las tenga.
std::uint32_t g_instance_api = VK_API_VERSION_1_4;

// Modo compatible forzado (pruebas en PC del camino de los moviles):
// CRAMION_VK_COMPAT=1.
bool compatForced() {
    const char* value = std::getenv("CRAMION_VK_COMPAT");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

// Lo que el camino de escritorio necesita del dispositivo. Sin todo esto (la
// mayoria de los moviles, GPU de PC antiguas con drivers 1.0-1.2), modo
// compatible (VulkanCompat.h). `reason` dice que falta.
bool fullPathSupported(const vk::raii::PhysicalDevice& candidate, const char** reason) {
    const auto properties = candidate.getProperties();
    const auto& limits = properties.limits;
    const auto fail = [&](const char* why) {
        if (reason != nullptr) *reason = why;
        return false;
    };
#if defined(__ANDROID__)
    // En los moviles, siempre: descriptores dentro de los minimos, sin
    // comandos indirectos generados en la GPU (firstInstance da problemas en
    // varios drivers de Adreno) y el G-buffer ligero.
    (void)limits;
    return fail("GPU de movil");
#else
    if (compatForced()) return fail("forzado con CRAMION_VK_COMPAT");
    if (std::min(properties.apiVersion, g_instance_api) < VK_API_VERSION_1_3) return fail("Vulkan anterior a 1.3");
    const auto chain = candidate.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan12Features,
                                              vk::PhysicalDeviceVulkan13Features>();
    const auto& features = chain.template get<vk::PhysicalDeviceFeatures2>().features;
    const auto& features13 = chain.template get<vk::PhysicalDeviceVulkan13Features>();
    if (!features13.dynamicRendering || !features13.synchronization2 || !features13.shaderDemoteToHelperInvocation) {
        return fail("sin dynamic rendering, synchronization2 o demote");
    }
    if (!features.drawIndirectFirstInstance) return fail("sin drawIndirectFirstInstance");
    // Lo que usan los shaders de escritorio (texturas de los decals, la
    // iluminacion con todos sus mapas, el culling en GPU, el G-buffer).
    if (limits.maxPerStageDescriptorSamplers < 48 || limits.maxPerStageDescriptorSampledImages < 48) {
        return fail("pocas texturas por shader");
    }
    if (limits.maxPerStageDescriptorStorageBuffers < 8) return fail("pocos storage buffers por shader");
    if (limits.maxColorAttachments < 5) return fail("menos de 5 destinos de color");
    if (limits.maxComputeWorkGroupInvocations < 256) return fail("grupos de computo pequenos");
    return true;
#endif
}

}  // namespace

void VulkanDevice::initialize(const VulkanInstance& instance, const VulkanSurface& surface, VkPhysicalDevice required,
                              const std::vector<std::string>& extra_extensions,
                              const std::vector<std::string>& optional_extensions) {
    extra_extensions_ = extra_extensions;
    optional_extensions_ = optional_extensions;
    g_instance_api = instance.apiVersion();
    pickPhysicalDevice(instance, surface, required);
    queue_families_ = findQueueFamilies(physical_device_, surface.handle());
    createLogicalDevice();
    selectDepthFormat();
}

bool VulkanDevice::extensionAvailable(const std::string& name) const {
    return hasExtension(physical_device_.enumerateDeviceExtensionProperties(), name.c_str());
}

// -----------------------------------------------------------------------------
// Dispositivo fisico
// -----------------------------------------------------------------------------

void VulkanDevice::pickPhysicalDevice(const VulkanInstance& instance, const VulkanSurface& surface, VkPhysicalDevice required) {
    vk::raii::PhysicalDevices candidates(instance.handle());
    if (candidates.empty()) {
        throw std::runtime_error("No se encontro ninguna GPU compatible con Vulkan.");
    }

    // Se ordenan por puntuacion y se toma la mejor valida.
    std::multimap<std::uint32_t, const vk::raii::PhysicalDevice*> ranked;
    for (const auto& candidate : candidates) {
        std::uint32_t score = rateDevice(candidate, surface.handle());
        // OpenXR: la GPU del casco gana a todas (si vale).
        if (score > 0 && required != VK_NULL_HANDLE && static_cast<VkPhysicalDevice>(*candidate) == required) {
            score += 1000000000u;
        }
        if (score > 0) {
            ranked.emplace(score, &candidate);
        }
    }

    if (ranked.empty()) {
        // Por que no vale cada una (en el log: para saber que le falta a un movil).
        for (const auto& candidate : candidates) {
            const auto p = candidate.getProperties();
            std::cerr << "[Vulkan] " << p.deviceName.data() << ": API " << VK_API_VERSION_MAJOR(p.apiVersion) << '.'
                      << VK_API_VERSION_MINOR(p.apiVersion) << '.' << VK_API_VERSION_PATCH(p.apiVersion) << ", driver "
                      << p.driverVersion << "\n";
            const auto available = candidate.enumerateDeviceExtensionProperties();
            for (const char* required : kRequiredDeviceExtensions) {
                if (!hasExtension(available, required)) std::cerr << "[Vulkan]   falta la extension " << required << "\n";
            }
            for (const char* optional : {"VK_KHR_dynamic_rendering", "VK_KHR_synchronization2", "VK_KHR_draw_indirect_count",
                                         "VK_EXT_shader_demote_to_helper_invocation", "VK_KHR_create_renderpass2",
                                         "VK_KHR_depth_stencil_resolve", "VK_KHR_buffer_device_address",
                                         "VK_EXT_descriptor_indexing", "VK_KHR_maintenance4", "VK_KHR_format_feature_flags2"}) {
                std::cerr << "[Vulkan]   " << optional << ": " << (hasExtension(available, optional) ? "si" : "no") << "\n";
            }
            const auto chain = candidate.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan12Features,
                                                      vk::PhysicalDeviceVulkan13Features>();
            const auto& f = chain.template get<vk::PhysicalDeviceFeatures2>().features;
            const auto& f12 = chain.template get<vk::PhysicalDeviceVulkan12Features>();
            const auto& f13 = chain.template get<vk::PhysicalDeviceVulkan13Features>();
            std::cerr << "[Vulkan]   dynamicRendering " << f13.dynamicRendering << " synchronization2 " << f13.synchronization2
                      << " demote " << f13.shaderDemoteToHelperInvocation << " drawIndirectCount " << f12.drawIndirectCount
                      << " multiDrawIndirect " << f.multiDrawIndirect << " drawIndirectFirstInstance "
                      << f.drawIndirectFirstInstance << " depthClamp " << f.depthClamp << " textureCompressionBC "
                      << f.textureCompressionBC << " ETC2 " << f.textureCompressionETC2 << " ASTC " << f.textureCompressionASTC_LDR
                      << " geometryShader " << f.geometryShader << " shaderStorageImageExtendedFormats "
                      << f.shaderStorageImageExtendedFormats << " independentBlend " << f.independentBlend
                      << " samplerAnisotropy " << f.samplerAnisotropy << " imageCubeArray " << f.imageCubeArray
                      << " fragmentStoresAndAtomics " << f.fragmentStoresAndAtomics << "\n";
            std::cerr << "[Vulkan]   limites: maxImage2D " << p.limits.maxImageDimension2D << " maxPushConstants "
                      << p.limits.maxPushConstantsSize << " maxBoundDescriptorSets " << p.limits.maxBoundDescriptorSets
                      << " maxPerStageSamplers " << p.limits.maxPerStageDescriptorSamplers << " maxPerStageSampledImages "
                      << p.limits.maxPerStageDescriptorSampledImages << " maxPerStageStorageImages "
                      << p.limits.maxPerStageDescriptorStorageImages << " maxColorAttachments " << p.limits.maxColorAttachments
                      << " maxComputeSharedMemory " << p.limits.maxComputeSharedMemorySize << "\n";
        }
        throw std::runtime_error(
            "Ninguna GPU cumple los requisitos (Vulkan, VK_KHR_swapchain y una cola de graficos y computo que "
            "presente en la ventana).");
    }

    physical_device_ = *ranked.rbegin()->second;

    const auto properties = physical_device_.getProperties();
    device_name_ = properties.deviceName.data();
    // La que se puede usar de verdad: la menor entre la GPU y la instancia.
    api_version_ = std::min(properties.apiVersion, g_instance_api);

    std::cout << "[Vulkan] GPU seleccionada: " << device_name_ << " (API "
              << VK_API_VERSION_MAJOR(api_version_) << '.' << VK_API_VERSION_MINOR(api_version_)
              << '.' << VK_API_VERSION_PATCH(api_version_) << ")\n";
}

std::uint32_t VulkanDevice::rateDevice(const vk::raii::PhysicalDevice& candidate,
                                       const vk::raii::SurfaceKHR& surface) {
    const auto properties = candidate.getProperties();

    // Cualquier Vulkan sirve: sin lo del camino de escritorio (1.3, dynamic
    // rendering, limites altos) se usa el modo compatible (VulkanCompat.h).
    if (properties.apiVersion < kMinimumApiVersion) {
        return 0;
    }
    if (!supportsRequiredExtensions(candidate)) {
        return 0;
    }
    if (!findQueueFamilies(candidate, surface).isComplete()) {
        return 0;
    }
    if (candidate.getSurfaceFormatsKHR(*surface).empty() ||
        candidate.getSurfacePresentModesKHR(*surface).empty()) {
        return 0;
    }

    // Preferencia: GPU dedicada sobre integrada; a igualdad, la que admita
    // texturas mas grandes.
    std::uint32_t score = 1;
    switch (properties.deviceType) {
        case vk::PhysicalDeviceType::eDiscreteGpu:
            score += 100000;
            break;
        case vk::PhysicalDeviceType::eIntegratedGpu:
            score += 50000;
            break;
        default:
            break;
    }
    score += properties.limits.maxImageDimension2D;
    // A igualdad de tipo, la que puede ir por el camino completo.
    if (fullPathSupported(candidate, nullptr)) score += 30000;
    return score;
}

bool VulkanDevice::supportsRequiredExtensions(const vk::raii::PhysicalDevice& candidate) {
    const auto available = candidate.enumerateDeviceExtensionProperties();

    return std::all_of(
        kRequiredDeviceExtensions.begin(), kRequiredDeviceExtensions.end(),
        [&](const char* required) {
            return std::any_of(available.begin(), available.end(),
                               [&](const vk::ExtensionProperties& property) {
                                   return std::strcmp(property.extensionName.data(), required) == 0;
                               });
        });
}

bool VulkanDevice::supportsRayTracing(const vk::raii::PhysicalDevice& candidate) {
    const auto available = candidate.enumerateDeviceExtensionProperties();
    for (const char* extension : kRayTracingExtensions) {
        if (!hasExtension(available, extension)) {
            return false;
        }
    }
    const auto chain =
        candidate.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan12Features,
                               vk::PhysicalDeviceAccelerationStructureFeaturesKHR,
                               vk::PhysicalDeviceRayQueryFeaturesKHR>();
    const auto& features12 = chain.template get<vk::PhysicalDeviceVulkan12Features>();
    return chain.template get<vk::PhysicalDeviceAccelerationStructureFeaturesKHR>()
               .accelerationStructure &&
           chain.template get<vk::PhysicalDeviceRayQueryFeaturesKHR>().rayQuery &&
           features12.bufferDeviceAddress && features12.runtimeDescriptorArray &&
           features12.shaderSampledImageArrayNonUniformIndexing &&
           features12.shaderStorageBufferArrayNonUniformIndexing && features12.descriptorBindingPartiallyBound;
}

QueueFamilyIndices VulkanDevice::findQueueFamilies(const vk::raii::PhysicalDevice& candidate,
                                                   const vk::raii::SurfaceKHR& surface) {
    QueueFamilyIndices indices{};
    const auto families = candidate.getQueueFamilyProperties();

    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(families.size()); ++i) {
        // La cola grafica tambien lanza los compute shaders (auto-exposicion):
        // se exigen ambas capacidades en la misma familia.
        const vk::QueueFlags required = vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute;
        const bool graphics = (families[i].queueFlags & required) == required;
        const bool present = candidate.getSurfaceSupportKHR(i, *surface) == VK_TRUE;

        // Lo ideal es una unica familia que haga ambas cosas: menos sincronizacion.
        if (graphics && present) {
            indices.graphics = i;
            indices.present = i;
            break;
        }
        if (graphics && indices.graphics == UINT32_MAX) {
            indices.graphics = i;
        }
        if (present && indices.present == UINT32_MAX) {
            indices.present = i;
        }
    }

    return indices;
}

// -----------------------------------------------------------------------------
// Dispositivo logico
// -----------------------------------------------------------------------------

void VulkanDevice::createLogicalDevice() {
    if (!queue_families_.isComplete()) {
        throw std::runtime_error("La GPU seleccionada no expone las familias de colas necesarias.");
    }
    limits_ = physical_device_.getProperties().limits;
    {
        const vk::PhysicalDeviceFeatures base = physical_device_.getFeatures();
        texture_compression_astc_supported_ = base.textureCompressionASTC_LDR == VK_TRUE;
        texture_compression_etc2_supported_ = base.textureCompressionETC2 == VK_TRUE;
    }
    // Sin lo del camino de escritorio: modo compatible.
    const char* reason = "";
    if (!fullPathSupported(physical_device_, &reason)) {
        createCompatLogicalDevice(reason);
        return;
    }
    compat_mode_ = false;
    compat_reason_.clear();

    // Una DeviceQueueCreateInfo por familia distinta.
    const std::set<std::uint32_t> unique_families = {queue_families_.graphics,
                                                     queue_families_.present};
    const float priority = 1.0f;

    std::vector<vk::DeviceQueueCreateInfo> queue_infos;
    queue_infos.reserve(unique_families.size());
    for (const std::uint32_t family : unique_families) {
        vk::DeviceQueueCreateInfo info{};
        info.queueFamilyIndex = family;
        info.queueCount = 1;
        info.pQueuePriorities = &priority;
        queue_infos.push_back(info);
    }

    // Caracteristicas de Vulkan 1.3 encadenadas al DeviceCreateInfo.
    vk::StructureChain<vk::DeviceCreateInfo, vk::PhysicalDeviceFeatures2,
                       vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan13Features,
                       vk::PhysicalDeviceAccelerationStructureFeaturesKHR,
                       vk::PhysicalDeviceRayQueryFeaturesKHR, vk::PhysicalDeviceOpacityMicromapFeaturesEXT,
                       vk::PhysicalDeviceRayTracingPipelineFeaturesKHR,
                       vk::PhysicalDeviceRayTracingInvocationReorderFeaturesEXT, vk::PhysicalDeviceMeshShaderFeaturesEXT>
        chain{};

    ray_tracing_supported_ = supportsRayTracing(physical_device_);
    std::vector<const char*> extensions(kRequiredDeviceExtensions.begin(),
                                        kRequiredDeviceExtensions.end());
    if (ray_tracing_supported_) {
        extensions.insert(extensions.end(), kRayTracingExtensions.begin(),
                          kRayTracingExtensions.end());
        chain.get<vk::PhysicalDeviceAccelerationStructureFeaturesKHR>().accelerationStructure =
            VK_TRUE;
        chain.get<vk::PhysicalDeviceRayQueryFeaturesKHR>().rayQuery = VK_TRUE;
    } else {
        // Sin las extensiones sus estructuras no pueden ir en la cadena.
        chain.unlink<vk::PhysicalDeviceAccelerationStructureFeaturesKHR>();
        chain.unlink<vk::PhysicalDeviceRayQueryFeaturesKHR>();
    }

    // --- Extras de la generacion actual de GPUs (todos opcionales) ---
    {
        const auto available = physical_device_.enumerateDeviceExtensionProperties();
        const auto features =
            physical_device_.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceOpacityMicromapFeaturesEXT,
                                          vk::PhysicalDeviceRayTracingPipelineFeaturesKHR,
                                          vk::PhysicalDeviceRayTracingInvocationReorderFeaturesEXT,
                                          vk::PhysicalDeviceMeshShaderFeaturesEXT>();
        // Opacity micromaps: el alfa del follaje horneado en las BLAS (el
        // hardware se salta el shader en lo que es opaco o transparente seguro).
        opacity_micromap_supported_ =
            ray_tracing_supported_ && hasExtension(available, vk::EXTOpacityMicromapExtensionName) &&
            features.get<vk::PhysicalDeviceOpacityMicromapFeaturesEXT>().micromap;
        // Pipeline de ray tracing + Shader Execution Reordering (path tracing).
        ray_tracing_pipeline_supported_ =
            ray_tracing_supported_ && hasExtension(available, vk::KHRRayTracingPipelineExtensionName) &&
            features.get<vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>().rayTracingPipeline;
        invocation_reorder_supported_ =
            ray_tracing_pipeline_supported_ &&
            hasExtension(available, vk::EXTRayTracingInvocationReorderExtensionName) &&
            features.get<vk::PhysicalDeviceRayTracingInvocationReorderFeaturesEXT>().rayTracingInvocationReorder;
        // Mesh shaders (task + mesh): meshlets con culling en la GPU.
        mesh_shader_supported_ = hasExtension(available, vk::EXTMeshShaderExtensionName) &&
                                 features.get<vk::PhysicalDeviceMeshShaderFeaturesEXT>().meshShader &&
                                 features.get<vk::PhysicalDeviceMeshShaderFeaturesEXT>().taskShader;
    }
    if (opacity_micromap_supported_) {
        extensions.push_back(vk::EXTOpacityMicromapExtensionName);
        chain.get<vk::PhysicalDeviceOpacityMicromapFeaturesEXT>().micromap = VK_TRUE;
    } else {
        chain.unlink<vk::PhysicalDeviceOpacityMicromapFeaturesEXT>();
    }
    if (ray_tracing_pipeline_supported_) {
        extensions.push_back(vk::KHRRayTracingPipelineExtensionName);
        chain.get<vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>().rayTracingPipeline = VK_TRUE;
    } else {
        chain.unlink<vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>();
    }
    if (invocation_reorder_supported_) {
        extensions.push_back(vk::EXTRayTracingInvocationReorderExtensionName);
        chain.get<vk::PhysicalDeviceRayTracingInvocationReorderFeaturesEXT>().rayTracingInvocationReorder = VK_TRUE;
    } else {
        chain.unlink<vk::PhysicalDeviceRayTracingInvocationReorderFeaturesEXT>();
    }
    if (mesh_shader_supported_) {
        extensions.push_back(vk::EXTMeshShaderExtensionName);
        chain.get<vk::PhysicalDeviceMeshShaderFeaturesEXT>().meshShader = VK_TRUE;
        chain.get<vk::PhysicalDeviceMeshShaderFeaturesEXT>().taskShader = VK_TRUE;
    } else {
        chain.unlink<vk::PhysicalDeviceMeshShaderFeaturesEXT>();
    }
    std::cout << "[Vulkan] Micromaps de opacidad: " << (opacity_micromap_supported_ ? "si" : "no")
              << ", pipeline de rayos: " << (ray_tracing_pipeline_supported_ ? "si" : "no")
              << ", SER: " << (invocation_reorder_supported_ ? "si" : "no")
              << ", mesh shaders: " << (mesh_shader_supported_ ? "si" : "no") << "\n";

    // Opcional: cuanta VRAM se usa de verdad (diagnostico y presupuesto).
    for (const vk::ExtensionProperties& extension : physical_device_.enumerateDeviceExtensionProperties()) {
        if (std::string_view(extension.extensionName.data()) == VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) {
            memory_budget_supported_ = true;
            extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
            break;
        }
    }

    auto& create_info = chain.get<vk::DeviceCreateInfo>();
    create_info.setQueueCreateInfos(queue_infos);
    // Las que pide OpenXR (si la GPU las tiene).
    {
        const auto available = physical_device_.enumerateDeviceExtensionProperties();
        for (const std::string& name : extra_extensions_) {
            if (!hasExtension(available, name.c_str())) {
                std::cerr << "[Vulkan] OpenXR pide " << name << " y la GPU no la tiene\n";
                continue;
            }
            if (std::none_of(extensions.begin(), extensions.end(), [&](const char* e) { return name == e; })) {
                extensions.push_back(name.c_str());
            }
        }
        for (const std::string& name : optional_extensions_) {
            if (hasExtension(available, name.c_str()) &&
                std::none_of(extensions.begin(), extensions.end(), [&](const char* e) { return name == e; })) {
                extensions.push_back(name.c_str());
            }
        }
    }
    create_info.setPEnabledExtensionNames(extensions);
    enabled_extensions_.assign(extensions.begin(), extensions.end());

    auto& features13 = chain.get<vk::PhysicalDeviceVulkan13Features>();
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    // Con --target-env=vulkan1.3, glslc traduce `discard` a
    // OpDemoteToHelperInvocation (lo usa el recorte por alfa de los modelos).
    features13.shaderDemoteToHelperInvocation = VK_TRUE;
    // Los task/mesh shaders (glslc) declaran el tamano de grupo con
    // LocalSizeId, que pide maintenance4 (obligatorio en Vulkan 1.3).
    features13.maintenance4 = VK_TRUE;

    // Culling en GPU: vkCmdDrawIndexedIndirectCount con varios comandos, si
    // la GPU lo tiene (si no, un comando por llamada).
    {
        const auto available = physical_device_.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan12Features>();
        indirect_count_supported_ = available.get<vk::PhysicalDeviceVulkan12Features>().drawIndirectCount &&
                                    available.get<vk::PhysicalDeviceFeatures2>().features.multiDrawIndirect;
    }
    auto& features12 = chain.get<vk::PhysicalDeviceVulkan12Features>();
    features12.drawIndirectCount = indirect_count_supported_ ? VK_TRUE : VK_FALSE;
    // Layouts de solo profundidad (DEPTH_ATTACHMENT_OPTIMAL...): obligatorios
    // en Vulkan 1.3, pero hay que pedirlos.
    features12.separateDepthStencilLayouts = VK_TRUE;
    // Semaforos de linea de tiempo (obligatorios desde Vulkan 1.2): SteamVR
    // los pide (VK_KHR_timeline_semaphore) para sincronizarse con el casco.
    features12.timelineSemaphore = physical_device_.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan12Features>()
                                       .get<vk::PhysicalDeviceVulkan12Features>()
                                       .timelineSemaphore;
    if (!indirect_count_supported_) {
        std::cout << "[Vulkan] Sin drawIndirectCount/multiDrawIndirect (GPU de movil): dibujo indirecto comando a comando\n";
    }
    if (ray_tracing_supported_) {
        // Estructuras de aceleracion (direcciones de buffer) y texturas de
        // todos los materiales indexadas desde los shaders de rayos.
        features12.bufferDeviceAddress = VK_TRUE;
        features12.runtimeDescriptorArray = VK_TRUE;
        features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        features12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
        // El array de texturas de la escena tiene huecos sin escribir.
        features12.descriptorBindingPartiallyBound = VK_TRUE;
    }

    // depthClamp es opcional: si la GPU no lo tiene, la pasada de sombras
    // simplemente prescinde de el.
    depth_clamp_supported_ =
        physical_device_.getFeatures().depthClamp == VK_TRUE;

    // Texturas comprimidas por bloques (BC1-BC7, las de los DDS): todas las
    // GPU de escritorio las tienen.
    texture_compression_bc_supported_ =
        physical_device_.getFeatures().textureCompressionBC == VK_TRUE;

    auto& features = chain.get<vk::PhysicalDeviceFeatures2>();
    features.features.depthClamp = depth_clamp_supported_ ? VK_TRUE : VK_FALSE;
    features.features.textureCompressionBC =
        texture_compression_bc_supported_ ? VK_TRUE : VK_FALSE;
    features.features.multiDrawIndirect = indirect_count_supported_ ? VK_TRUE : VK_FALSE;
    // Lineas (modo Wireframe de la vista Escena del editor).
    fill_mode_non_solid_supported_ = physical_device_.getFeatures().fillModeNonSolid == VK_TRUE;
    features.features.fillModeNonSolid = fill_mode_non_solid_supported_ ? VK_TRUE : VK_FALSE;
    // Teselacion (relieve real de los materiales con mapa de alturas).
    tessellation_supported_ = physical_device_.getFeatures().tessellationShader == VK_TRUE;
    features.features.tessellationShader = tessellation_supported_ ? VK_TRUE : VK_FALSE;
    // Filtro anisotropico (texturas del terreno), si la GPU lo tiene.
    features.features.samplerAnisotropy = physical_device_.getFeatures().samplerAnisotropy;
    // Instancias en los comandos indirectos (batching por material).
    features.features.drawIndirectFirstInstance = VK_TRUE;

    device_ = vk::raii::Device(physical_device_, create_info);

    graphics_queue_ = vk::raii::Queue(device_, queue_families_.graphics, 0);
    present_queue_ = vk::raii::Queue(device_, queue_families_.present, 0);

    // Pool aparte para los comandos de usar y tirar (copias de staging).
    vk::CommandPoolCreateInfo pool_info{};
    pool_info.flags = vk::CommandPoolCreateFlagBits::eTransient;
    pool_info.queueFamilyIndex = queue_families_.graphics;
    transient_pool_ = vk::raii::CommandPool(device_, pool_info);

    std::cout << "[Vulkan] Trazado de rayos por hardware: "
              << (ray_tracing_supported_ ? "disponible" : "no disponible") << "\n";
    std::cout << "[Vulkan] Dispositivo logico creado (familia graficos = "
              << queue_families_.graphics << ", presentacion = " << queue_families_.present
              << ")\n";

    compat::Caps caps;
    caps.api_version = api_version_;
    caps.dynamic_rendering = true;
    caps.synchronization2 = true;
    caps.tessellation = tessellation_supported_;
    caps.separate_depth_stencil_layouts = true;
    caps.lite = false;
    compat::setDevice(device_, caps);
}

// Modo compatible: lo que la GPU tenga. Dynamic rendering y synchronization2
// nativos si los tiene (Vulkan 1.3 o sus extensiones KHR); si no, los emula
// VulkanCompat. Sin trazado de rayos, mesh shaders, teselacion ni comandos
// indirectos generados en la GPU.
void VulkanDevice::createCompatLogicalDevice(const char* reason) {
    compat_mode_ = true;
    compat_reason_ = reason != nullptr ? reason : "";
    const std::uint32_t api = api_version_;
    const auto available = physical_device_.enumerateDeviceExtensionProperties();
    const auto has = [&](const char* name) { return hasExtension(available, name); };

    const std::set<std::uint32_t> unique_families = {queue_families_.graphics, queue_families_.present};
    const float priority = 1.0f;
    std::vector<vk::DeviceQueueCreateInfo> queue_infos;
    for (const std::uint32_t family : unique_families) {
        vk::DeviceQueueCreateInfo info{};
        info.queueFamilyIndex = family;
        info.queueCount = 1;
        info.pQueuePriorities = &priority;
        queue_infos.push_back(info);
    }

    std::vector<const char*> extensions(kRequiredDeviceExtensions.begin(), kRequiredDeviceExtensions.end());
    const bool core13 = api >= VK_API_VERSION_1_3;
    const bool core11 = api >= VK_API_VERSION_1_1;
    // VK_KHR_dynamic_rendering pide VK_KHR_depth_stencil_resolve (y este
    // VK_KHR_create_renderpass2), del nucleo desde 1.2.
    const bool dynamic_extension = !core13 && core11 && has(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) &&
                                   (api >= VK_API_VERSION_1_2 || (has(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME) &&
                                                                  has(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME)));
    const bool sync2_extension = !core13 && core11 && has(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);

    // Que tiene de verdad (vkGetPhysicalDeviceFeatures2 desde Vulkan 1.1; las
    // basicas en la misma consulta).
    vk::PhysicalDeviceDynamicRenderingFeatures dynamic_query{};
    vk::PhysicalDeviceSynchronization2Features sync_query{};
    vk::PhysicalDeviceFeatures base = physical_device_.getFeatures();
    if (core11) {
        vk::PhysicalDeviceFeatures2 query{};
        void* next = nullptr;
        if (core13 || dynamic_extension) {
            dynamic_query.pNext = next;
            next = &dynamic_query;
        }
        if (core13 || sync2_extension) {
            sync_query.pNext = next;
            next = &sync_query;
        }
        query.pNext = next;
        physical_device_.getDispatcher()->vkGetPhysicalDeviceFeatures2(
            static_cast<VkPhysicalDevice>(*physical_device_), reinterpret_cast<VkPhysicalDeviceFeatures2*>(&query));
        base = query.features;
    }
    // CRAMION_VK_EMULATE=1: como un movil sin ellos (pruebas en PC de los
    // render passes y las barreras de Vulkan 1.0 de VulkanCompat).
    const char* emulate = std::getenv("CRAMION_VK_EMULATE");
    const bool forced_emulation = emulate != nullptr && emulate[0] != '\0' && emulate[0] != '0';
    const bool dynamic_rendering = dynamic_query.dynamicRendering == VK_TRUE && !forced_emulation;
    const bool synchronization2 = sync_query.synchronization2 == VK_TRUE && !forced_emulation;

    // --- Lo que se activa ---
    vk::PhysicalDeviceFeatures2 enabled2{};
    vk::PhysicalDeviceFeatures& enabled = enabled2.features;
    enabled.samplerAnisotropy = base.samplerAnisotropy;
    enabled.depthClamp = base.depthClamp;
    enabled.depthBiasClamp = base.depthBiasClamp;
    enabled.fillModeNonSolid = base.fillModeNonSolid;
    enabled.independentBlend = base.independentBlend;
    enabled.fragmentStoresAndAtomics = base.fragmentStoresAndAtomics;
    enabled.imageCubeArray = base.imageCubeArray;
    enabled.fullDrawIndexUint32 = base.fullDrawIndexUint32;
    // Los decals y los VFX indexan sus arrays de texturas con un valor de cada
    // instancia.
    enabled.shaderSampledImageArrayDynamicIndexing = base.shaderSampledImageArrayDynamicIndexing;
    enabled.shaderStorageBufferArrayDynamicIndexing = base.shaderStorageBufferArrayDynamicIndexing;
    enabled.drawIndirectFirstInstance = base.drawIndirectFirstInstance;
    draw_indirect_first_instance_ = base.drawIndirectFirstInstance == VK_TRUE;
    // (ETC2 y ASTC, las de movil, se piden cuando el motor las use.)
    enabled.textureCompressionBC = base.textureCompressionBC;
    depth_clamp_supported_ = base.depthClamp == VK_TRUE;
    texture_compression_bc_supported_ = base.textureCompressionBC == VK_TRUE;
    fill_mode_non_solid_supported_ = base.fillModeNonSolid == VK_TRUE;
    tessellation_supported_ = false;
    indirect_count_supported_ = false;
    ray_tracing_supported_ = false;
    opacity_micromap_supported_ = false;
    ray_tracing_pipeline_supported_ = false;
    invocation_reorder_supported_ = false;
    mesh_shader_supported_ = false;

    vk::PhysicalDeviceDynamicRenderingFeatures dynamic_enable{};
    dynamic_enable.dynamicRendering = VK_TRUE;
    vk::PhysicalDeviceSynchronization2Features sync_enable{};
    sync_enable.synchronization2 = VK_TRUE;
    void* next = nullptr;
    if (dynamic_rendering) {
        dynamic_enable.pNext = next;
        next = &dynamic_enable;
        if (dynamic_extension) {
            extensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
            if (api < VK_API_VERSION_1_2) {
                extensions.push_back(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
                extensions.push_back(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
            }
        }
    }
    if (synchronization2) {
        sync_enable.pNext = next;
        next = &sync_enable;
        if (sync2_extension) extensions.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    }
    if (has(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) && core11) {
        memory_budget_supported_ = true;
        extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    }
    // Si el dispositivo es un subconjunto portable (MoltenVK, capas de
    // emulacion de perfiles), la especificacion obliga a activarlo.
    if (has("VK_KHR_portability_subset")) extensions.push_back("VK_KHR_portability_subset");

    vk::DeviceCreateInfo create_info{};
    create_info.setQueueCreateInfos(queue_infos);
    create_info.setPEnabledExtensionNames(extensions);
    if (core11) {
        enabled2.pNext = next;
        create_info.pNext = &enabled2;
    } else {
        create_info.pEnabledFeatures = &enabled;
    }
    enabled_extensions_.assign(extensions.begin(), extensions.end());
    device_ = vk::raii::Device(physical_device_, create_info);

    graphics_queue_ = vk::raii::Queue(device_, queue_families_.graphics, 0);
    present_queue_ = vk::raii::Queue(device_, queue_families_.present, 0);
    vk::CommandPoolCreateInfo pool_info{};
    pool_info.flags = vk::CommandPoolCreateFlagBits::eTransient;
    pool_info.queueFamilyIndex = queue_families_.graphics;
    transient_pool_ = vk::raii::CommandPool(device_, pool_info);

    compat::Caps caps;
    caps.api_version = api;
    caps.dynamic_rendering = dynamic_rendering;
    caps.synchronization2 = synchronization2;
    caps.tessellation = false;
    // Los combinados de profundidad y stencil: validos desde Vulkan 1.0.
    caps.separate_depth_stencil_layouts = false;
    caps.lite = true;
    caps.reason = compat_reason_.c_str();
    compat::setDevice(device_, caps);

    std::cout << "[Vulkan] Modo compatible (" << compat_reason_ << "): Vulkan " << VK_API_VERSION_MAJOR(api) << '.'
              << VK_API_VERSION_MINOR(api) << ", dynamic rendering " << (dynamic_rendering ? "nativo" : "emulado")
              << ", synchronization2 " << (synchronization2 ? "nativo" : "emulado") << ", texturas por shader "
              << limits_.maxPerStageDescriptorSamplers << "/" << limits_.maxPerStageDescriptorSampledImages
              << ", storage buffers " << limits_.maxPerStageDescriptorStorageBuffers << ", destinos de color "
              << limits_.maxColorAttachments << "\n";
}

void VulkanDevice::selectDepthFormat() {
    // De mas a menos preciso; D32 es el habitual en GPU de escritorio.
    // Primero los que no tienen stencil (el motor no lo usa): sin layouts
    // separados de profundidad (modo compatible) las barreras de uno con
    // stencil tendrian que incluir tambien ese aspecto. D16 es obligatorio en
    // cualquier GPU con Vulkan, asi que siempre hay alguno sin stencil.
    constexpr std::array<vk::Format, 5> kCandidates = {
        vk::Format::eD32Sfloat,
        vk::Format::eX8D24UnormPack32,
        vk::Format::eD16Unorm,
        vk::Format::eD32SfloatS8Uint,
        vk::Format::eD24UnormS8Uint,
    };

    for (const vk::Format candidate : kCandidates) {
        const auto properties = physical_device_.getFormatProperties(candidate);

        // Ademas de servir de attachment, la pasada de iluminacion lo muestrea
        // para reconstruir la posicion del mundo.
        const vk::FormatFeatureFlags required =
            vk::FormatFeatureFlagBits::eDepthStencilAttachment |
            vk::FormatFeatureFlagBits::eSampledImage;

        if ((properties.optimalTilingFeatures & required) == required) {
            depth_format_ = candidate;
            std::cout << "[Vulkan] Formato de profundidad: " << vk::to_string(depth_format_)
                      << "\n";
            return;
        }
    }

    throw std::runtime_error("La GPU no admite ningun formato de profundidad conocido.");
}

std::uint32_t VulkanDevice::findMemoryType(std::uint32_t type_bits,
                                           vk::MemoryPropertyFlags properties) const {
    const auto memory_properties = physical_device_.getMemoryProperties();

    for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        const bool type_allowed = (type_bits & (1u << i)) != 0;
        const bool has_properties =
            (memory_properties.memoryTypes[i].propertyFlags & properties) == properties;

        if (type_allowed && has_properties) {
            return i;
        }
    }

    throw std::runtime_error("No hay ningun tipo de memoria compatible con lo solicitado.");
}

void VulkanDevice::submitOneTime(
    const std::function<void(const vk::raii::CommandBuffer&)>& record) const {
    vk::CommandBufferAllocateInfo allocate_info{};
    allocate_info.commandPool = *transient_pool_;
    allocate_info.level = vk::CommandBufferLevel::ePrimary;
    allocate_info.commandBufferCount = 1;

    // El pool y la cola se comparten entre hilos: el pool mientras se graba
    // (y al liberar), la cola solo al enviar. La espera, sin nada puesto.
    std::unique_lock pool_lock(transient_mutex_);
    std::optional<vk::raii::CommandBuffers> buffers;
    buffers.emplace(device_, allocate_info);
    const vk::raii::CommandBuffer& cmd = buffers->front();

    vk::CommandBufferBeginInfo begin_info{};
    begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    cmd.begin(begin_info);

    record(cmd);

    cmd.end();
    pool_lock.unlock();

    const vk::CommandBuffer raw = *cmd;
    vk::SubmitInfo submit_info{};
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &raw;

    // Una fence propia evita bloquear toda la cola con un waitIdle.
    vk::raii::Fence fence(device_, vk::FenceCreateInfo{});
    {
        const std::lock_guard queue_lock(queue_mutex_);
        graphics_queue_.submit(submit_info, *fence);
    }

    const vk::Result waited = device_.waitForFences(*fence, VK_TRUE, UINT64_MAX);
    pool_lock.lock();
    buffers.reset();
    pool_lock.unlock();
    if (waited != vk::Result::eSuccess) {
        throw std::runtime_error("Tiempo agotado esperando un comando de un solo uso.");
    }
}

void VulkanDevice::waitIdle() const {
    if (*device_ != VK_NULL_HANDLE) {
        const std::lock_guard lock(queue_mutex_);
        device_.waitIdle();
    }
}

// -----------------------------------------------------------------------------
// Cache de pipelines
// -----------------------------------------------------------------------------

void VulkanDevice::loadPipelineCache(const std::filesystem::path& file) {
    pipeline_cache_file_ = file;
    std::vector<char> data;
    {
        std::ifstream in(file, std::ios::binary | std::ios::ate);
        if (in) {
            data.resize(static_cast<std::size_t>(in.tellg()));
            in.seekg(0);
            in.read(data.data(), static_cast<std::streamsize>(data.size()));
            if (!in) data.clear();
        }
    }
    // Solo si la cabecera es de esta GPU y este driver (algunos drivers no
    // toleran datos de otra).
    const vk::PhysicalDeviceProperties properties = physical_device_.getProperties();
    struct Header {
        std::uint32_t length, version, vendor, device;
        std::uint8_t uuid[VK_UUID_SIZE];
    };
    bool valid = data.size() > sizeof(Header);
    if (valid) {
        Header h{};
        std::memcpy(&h, data.data(), sizeof(Header));
        valid = h.length >= sizeof(Header) && h.version == static_cast<std::uint32_t>(vk::PipelineCacheHeaderVersion::eOne) &&
                h.vendor == properties.vendorID && h.device == properties.deviceID &&
                std::memcmp(h.uuid, properties.pipelineCacheUUID.data(), VK_UUID_SIZE) == 0;
    }
    vk::PipelineCacheCreateInfo info{};
    if (valid) {
        info.initialDataSize = data.size();
        info.pInitialData = data.data();
    }
    try {
        pipeline_cache_ = vk::raii::PipelineCache(device_, info);
    } catch (const std::exception&) {
        pipeline_cache_ = vk::raii::PipelineCache(device_, vk::PipelineCacheCreateInfo{});
        valid = false;
    }
    std::cout << "[Vulkan] Cache de pipelines: " << (valid ? "cargada" : "nueva (se compilan los shaders)") << '\n';
}

void VulkanDevice::savePipelineCache() const {
    if (pipeline_cache_file_.empty() || !*pipeline_cache_) return;
    try {
        const std::vector<std::uint8_t> data = pipeline_cache_.getData();
        std::error_code error;
        std::filesystem::create_directories(pipeline_cache_file_.parent_path(), error);
        // Primero a un temporal: si se corta, la cache anterior sigue sana.
        std::filesystem::path temp = pipeline_cache_file_;
        temp += ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            if (!out) return;
        }
        std::filesystem::rename(temp, pipeline_cache_file_, error);
    } catch (const std::exception& e) {
        std::cerr << "[Vulkan] No se pudo guardar la cache de pipelines: " << e.what() << '\n';
    }
}

const vk::raii::PipelineCache& VulkanDevice::pipelineCache() const {
    ++pipelines_created_;
    if (pipeline_callback_) pipeline_callback_(pipelines_created_);
    return pipeline_cache_;
}

void VulkanDevice::shutdown() {
    waitIdle();
    savePipelineCache();
    pipeline_cache_ = nullptr;
    compat::shutdown();  // sus render passes y framebuffers, antes que el dispositivo

    transient_pool_ = nullptr;
    present_queue_ = nullptr;
    graphics_queue_ = nullptr;
    device_ = nullptr;
    physical_device_ = nullptr;
    queue_families_ = {};
    device_name_.clear();
    depth_format_ = vk::Format::eUndefined;
    depth_clamp_supported_ = false;
    api_version_ = 0;
}

bool VulkanDevice::videoMemory(std::uint64_t& used_bytes, std::uint64_t& budget_bytes) const {
    used_bytes = 0;
    budget_bytes = 0;
    if (!memory_budget_supported_) return false;
    const auto chain = physical_device_.getMemoryProperties2<vk::PhysicalDeviceMemoryProperties2,
                                                             vk::PhysicalDeviceMemoryBudgetPropertiesEXT>();
    const vk::PhysicalDeviceMemoryProperties& properties =
        chain.get<vk::PhysicalDeviceMemoryProperties2>().memoryProperties;
    const auto& budget = chain.get<vk::PhysicalDeviceMemoryBudgetPropertiesEXT>();
    for (std::uint32_t i = 0; i < properties.memoryHeapCount; ++i) {
        if (properties.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
            used_bytes += budget.heapUsage[i];
            budget_bytes += budget.heapBudget[i];
        }
    }
    return budget_bytes > 0;
}

}  // namespace cramion::gfx
