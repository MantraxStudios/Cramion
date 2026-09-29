#include "CramionFX/vk/TemporalUpscalers.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>

#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(CRAMION_FSR3)
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_api_loader.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>
#endif

#if defined(CRAMION_DLSS)
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_vk.h>
#endif

namespace cramion::gfx {

namespace {

#if defined(_WIN32)
std::filesystem::path exeFolder() {
    wchar_t buffer[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path();
}
#endif

// Signo del jitter para cada SDK (+1 = el desplazamiento de la imagen tal
// cual). Se puede cambiar para depurar: CRAMION_UPSCALER_JITTER=-1.
float jitterSign() {
    const char* value = std::getenv("CRAMION_UPSCALER_JITTER");
    return value != nullptr && std::atof(value) < 0.0 ? -1.0f : 1.0f;
}

}  // namespace

// =============================================================================
// AMD FSR 3.1 (FidelityFX API)
// =============================================================================

#if defined(CRAMION_FSR3)

struct Fsr3Upscaler::Impl {
    HMODULE module = nullptr;
    ffxFunctions ffx{};
    ffxContext context = nullptr;
    bool ready = false;

    static void message(uint32_t type, const wchar_t* text) {
        const std::wstring w(text != nullptr ? text : L"");
        std::string s(w.begin(), w.end());
        std::cerr << "[FSR 3] " << (type == FFX_API_MESSAGE_TYPE_ERROR ? "Error: " : "Aviso: ") << s << "\n";
    }
};

namespace {

// FidelityFX pide todo a vkGetDeviceProcAddr, tambien nombres de extension
// que el nucleo de Vulkan ya trae con otro nombre (vkGetBufferMemoryRequirements2KHR)
// y funciones de depuracion opcionales: si llegaban nulas, se caia al
// llamarlas. Aqui: el nombre, el del nucleo sin el sufijo, el de la
// instancia, o una funcion vacia para lo opcional.
VkInstance g_ffx_instance = VK_NULL_HANDLE;

VKAPI_ATTR void VKAPI_CALL noOp() {}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ffxDeviceProcAddr(VkDevice device, const char* name) {
    if (PFN_vkVoidFunction f = vkGetDeviceProcAddr(device, name)) return f;
    const std::string n = name != nullptr ? name : "";
    for (const char* suffix : {"KHR", "EXT"}) {
        const std::size_t len = std::strlen(suffix);
        if (n.size() > len && n.compare(n.size() - len, len, suffix) == 0) {
            const std::string core = n.substr(0, n.size() - len);
            if (PFN_vkVoidFunction f = vkGetDeviceProcAddr(device, core.c_str())) return f;
        }
    }
    if (g_ffx_instance != VK_NULL_HANDLE) {
        if (PFN_vkVoidFunction f = vkGetInstanceProcAddr(g_ffx_instance, name)) return f;
    }
    static constexpr const char* kOptional[] = {"vkCmdBeginDebugUtilsLabelEXT", "vkCmdEndDebugUtilsLabelEXT",
                                                "vkSetDebugUtilsObjectNameEXT", "vkCmdWriteBufferMarkerAMD",
                                                "vkCmdWriteBufferMarker2AMD", "vkSetHdrMetadataEXT"};
    for (const char* optional : kOptional) {
        if (n == optional) return reinterpret_cast<PFN_vkVoidFunction>(&noOp);
    }
    std::cerr << "[FSR 3] Vulkan no tiene " << n << "\n";
    return nullptr;
}

}  // namespace

Fsr3Upscaler::Fsr3Upscaler() : impl_(std::make_unique<Impl>()) {}
Fsr3Upscaler::~Fsr3Upscaler() {
    destroy();
    if (impl_->module != nullptr) FreeLibrary(impl_->module);
}

bool Fsr3Upscaler::supported() {
    std::error_code e;
    return std::filesystem::exists(exeFolder() / "amd_fidelityfx_vk.dll", e);
}

bool Fsr3Upscaler::create(VkInstance instance, VkDevice device, VkPhysicalDevice physical_device, std::uint32_t max_render_width,
                          std::uint32_t max_render_height, std::uint32_t output_width, std::uint32_t output_height,
                          std::string& error) {
    Impl& d = *impl_;
    destroy();
    if (d.module == nullptr) {
        d.module = LoadLibraryW((exeFolder() / "amd_fidelityfx_vk.dll").c_str());
        if (d.module == nullptr) {
            error = "falta amd_fidelityfx_vk.dll junto al ejecutable";
            return false;
        }
        ffxLoadFunctions(&d.ffx, d.module);
        if (d.ffx.CreateContext == nullptr || d.ffx.Dispatch == nullptr || d.ffx.DestroyContext == nullptr) {
            error = "amd_fidelityfx_vk.dll no tiene la API de FidelityFX";
            return false;
        }
    }
    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.vkDevice = device;
    backend.vkPhysicalDevice = physical_device;
    g_ffx_instance = instance;
    backend.vkDeviceProcAddr = ffxDeviceProcAddr;

    ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.header.pNext = &backend.header;
    // Color HDR lineal sin exponer (la auto-exposicion del motor va despues):
    // FSR calcula su propia exposicion para decidir la historia.
    desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
                 FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
    desc.maxRenderSize = FfxApiDimensions2D{max_render_width, max_render_height};
    desc.maxUpscaleSize = FfxApiDimensions2D{output_width, output_height};
    desc.fpMessage = &Impl::message;
    const ffxReturnCode_t result = d.ffx.CreateContext(&d.context, &desc.header, nullptr);
    if (result != FFX_API_RETURN_OK) {
        d.context = nullptr;
        error = "ffxCreateContext devolvio " + std::to_string(result);
        return false;
    }
    d.ready = true;
    std::cout << "[FSR 3] Listo: " << max_render_width << "x" << max_render_height << " -> " << output_width << "x"
              << output_height << "\n";
    return true;
}

void Fsr3Upscaler::destroy() {
    Impl& d = *impl_;
    if (d.context != nullptr && d.ffx.DestroyContext != nullptr) d.ffx.DestroyContext(&d.context, nullptr);
    d.context = nullptr;
    d.ready = false;
}

bool Fsr3Upscaler::ready() const { return impl_->ready; }

namespace {

FfxApiResource ffxImage(const UpscaleImage& image, uint32_t state, bool storage) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = image.format;
    info.extent = VkExtent3D{image.width, image.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    return ffxApiGetResourceVK(reinterpret_cast<void*>(image.image), ffxApiGetImageResourceDescriptionVK(image.image, info, 0),
                               state);
}

}  // namespace

bool Fsr3Upscaler::dispatch(const UpscaleDispatch& in) {
    Impl& d = *impl_;
    if (!d.ready) return false;
    ffxDispatchDescUpscale desc{};
    desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    desc.commandList = in.command_buffer;
    desc.color = ffxImage(in.color, FFX_API_RESOURCE_STATE_COMPUTE_READ, false);
    desc.depth = ffxImage(in.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ, false);
    desc.motionVectors = ffxImage(in.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ, false);
    desc.output = ffxImage(in.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, true);
    const float sign = jitterSign();
    desc.jitterOffset = FfxApiFloatCoords2D{in.jitter_x * sign, in.jitter_y * sign};
    // Del pixel actual al del frame anterior, en pixeles internos.
    desc.motionVectorScale = FfxApiFloatCoords2D{-static_cast<float>(in.render_width), -static_cast<float>(in.render_height)};
    desc.renderSize = FfxApiDimensions2D{in.render_width, in.render_height};
    desc.upscaleSize = FfxApiDimensions2D{in.output.width, in.output.height};
    desc.enableSharpening = false;  // la nitidez es la del motor (RCAS)
    desc.frameTimeDelta = std::clamp(in.frame_ms, 0.1f, 100.0f);
    desc.preExposure = 1.0f;
    desc.reset = in.reset;
    desc.cameraNear = in.near_plane;
    desc.cameraFar = in.far_plane;
    desc.cameraFovAngleVertical = in.fov_y;
    desc.viewSpaceToMetersFactor = 1.0f;
    const ffxReturnCode_t result = d.ffx.Dispatch(&d.context, &desc.header);
    if (result != FFX_API_RETURN_OK) {
        std::cerr << "[FSR 3] ffxDispatch devolvio " << result << "\n";
        return false;
    }
    return true;
}

#else  // sin FSR 3

struct Fsr3Upscaler::Impl {};
Fsr3Upscaler::Fsr3Upscaler() : impl_(std::make_unique<Impl>()) {}
Fsr3Upscaler::~Fsr3Upscaler() = default;
bool Fsr3Upscaler::supported() { return false; }
bool Fsr3Upscaler::create(VkInstance, VkDevice, VkPhysicalDevice, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                          std::string& error) {
    error = "el motor se compilo sin FSR 3";
    return false;
}
void Fsr3Upscaler::destroy() {}
bool Fsr3Upscaler::ready() const { return false; }
bool Fsr3Upscaler::dispatch(const UpscaleDispatch&) { return false; }

#endif

// =============================================================================
// NVIDIA DLSS 4 (NGX)
// =============================================================================

#if defined(CRAMION_DLSS)

namespace {

// Identificador del proyecto para NGX (motor propio: ENGINE_TYPE_CUSTOM).
constexpr const char* kProjectId = "6c1b8f2e-3d4a-4e8b-9a61-c7a3910e0001";

NVSDK_NGX_FeatureCommonInfo& featureInfo() {
    static std::wstring folder;
    static const wchar_t* paths[1] = {nullptr};
    static NVSDK_NGX_FeatureCommonInfo info{};
#if defined(_WIN32)
    folder = exeFolder().wstring();
#endif
    paths[0] = folder.c_str();
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    return info;
}

NVSDK_NGX_FeatureDiscoveryInfo discoveryInfo() {
    NVSDK_NGX_FeatureDiscoveryInfo info{};
    info.SDKVersion = NVSDK_NGX_Version_API;
    info.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    info.Identifier.v.ProjectDesc.ProjectId = kProjectId;
    info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    info.Identifier.v.ProjectDesc.EngineVersion = "1.2";
    static std::wstring data_path = std::filesystem::temp_directory_path().wstring();
    info.ApplicationDataPath = data_path.c_str();
    info.FeatureInfo = &featureInfo();
    return info;
}

std::string ngxError(NVSDK_NGX_Result result) {
    char text[32];
    std::snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned>(result));
    return text;
}

}  // namespace

struct DlssUpscaler::Impl {
    VkDevice device = VK_NULL_HANDLE;
    bool initialized = false;
    bool available = false;
    NVSDK_NGX_Parameter* parameters = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    std::wstring logs;
};

DlssUpscaler::DlssUpscaler() : impl_(std::make_unique<Impl>()) {}
DlssUpscaler::~DlssUpscaler() { shutdown(); }

bool DlssUpscaler::compiled() { return true; }

std::vector<std::string> DlssUpscaler::instanceExtensions() {
    std::vector<std::string> out;
    NVSDK_NGX_FeatureDiscoveryInfo info = discoveryInfo();
    std::uint32_t count = 0;
    VkExtensionProperties* properties = nullptr;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&info, &count, &properties))) {
        for (std::uint32_t i = 0; i < count; ++i) out.emplace_back(properties[i].extensionName);
    }
    return out;
}

std::vector<std::string> DlssUpscaler::deviceExtensions(VkInstance instance) {
    std::vector<std::string> out;
    std::uint32_t gpu_count = 0;
    vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
    std::vector<VkPhysicalDevice> gpus(gpu_count);
    vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.data());
    NVSDK_NGX_FeatureDiscoveryInfo info = discoveryInfo();
    for (VkPhysicalDevice gpu : gpus) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(gpu, &props);
        if (props.vendorID != 0x10DE) continue;  // solo NVIDIA
        std::uint32_t count = 0;
        VkExtensionProperties* properties = nullptr;
        if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, gpu, &info, &count, &properties))) {
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::string name = properties[i].extensionName;
                if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
            }
        }
    }
    return out;
}

bool DlssUpscaler::initialize(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
                              const std::filesystem::path& logs_folder, std::string& error) {
    Impl& d = *impl_;
    shutdown();
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical_device, &props);
    if (props.vendorID != 0x10DE) {
        error = "DLSS necesita una GPU NVIDIA RTX";
        return false;
    }
    std::error_code e;
    std::filesystem::create_directories(logs_folder, e);
    d.logs = logs_folder.wstring();
    const NVSDK_NGX_Result init = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.2", d.logs.c_str(), instance, physical_device, device,
        vkGetInstanceProcAddr, vkGetDeviceProcAddr, &featureInfo(), NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(init)) {
        error = "NGX no se pudo iniciar (" + ngxError(init) + "): controlador sin DLSS o GPU no RTX";
        return false;
    }
    d.initialized = true;
    d.device = device;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&d.parameters)) || d.parameters == nullptr) {
        error = "NGX no dio sus parametros";
        shutdown();
        return false;
    }
    int available = 0;
    int needs_driver = 0;
    NVSDK_NGX_Parameter_GetI(d.parameters, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(d.parameters, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    if (available == 0) {
        error = needs_driver != 0 ? "DLSS necesita un controlador de NVIDIA mas nuevo" : "esta GPU no tiene DLSS (hace falta una RTX)";
        shutdown();
        return false;
    }
    d.available = true;
    std::cout << "[DLSS] Disponible en " << props.deviceName << "\n";
    return true;
}

bool DlssUpscaler::available() const { return impl_->available; }

bool DlssUpscaler::createFeature(VkCommandBuffer command_buffer, std::uint32_t render_width, std::uint32_t render_height,
                                 std::uint32_t output_width, std::uint32_t output_height, UpscaleQuality quality,
                                 std::string& error) {
    Impl& d = *impl_;
    releaseFeature();
    if (!d.available) {
        error = "DLSS no esta disponible";
        return false;
    }
    NVSDK_NGX_PerfQuality_Value mode = NVSDK_NGX_PerfQuality_Value_MaxQuality;
    switch (quality) {
        case UpscaleQuality::Native: mode = NVSDK_NGX_PerfQuality_Value_DLAA; break;
        case UpscaleQuality::Quality: mode = NVSDK_NGX_PerfQuality_Value_MaxQuality; break;
        case UpscaleQuality::Balanced: mode = NVSDK_NGX_PerfQuality_Value_Balanced; break;
        case UpscaleQuality::Performance: mode = NVSDK_NGX_PerfQuality_Value_MaxPerf; break;
        case UpscaleQuality::UltraPerformance: mode = NVSDK_NGX_PerfQuality_Value_UltraPerformance; break;
        case UpscaleQuality::Custom: {
            const float ratio = static_cast<float>(render_width) / static_cast<float>(std::max(output_width, 1u));
            mode = ratio >= 0.99f   ? NVSDK_NGX_PerfQuality_Value_DLAA
                   : ratio >= 0.62f ? NVSDK_NGX_PerfQuality_Value_MaxQuality
                   : ratio >= 0.55f ? NVSDK_NGX_PerfQuality_Value_Balanced
                   : ratio >= 0.45f ? NVSDK_NGX_PerfQuality_Value_MaxPerf
                                    : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
            break;
        }
    }
    // DLSS 4: el modelo transformer K en todos los modos. M y L (DLSS 4.5)
    // cuestan bastante mas en las RTX 20/30: con ellos DLSS daba menos FPS.
    for (const char* preset :
         {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance}) {
        NVSDK_NGX_Parameter_SetUI(d.parameters, preset, NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    }
    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InWidth = render_width;
    create.Feature.InHeight = render_height;
    create.Feature.InTargetWidth = output_width;
    create.Feature.InTargetHeight = output_height;
    create.Feature.InPerfQualityValue = mode;
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                  NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    const NVSDK_NGX_Result result =
        NGX_VULKAN_CREATE_DLSS_EXT1(d.device, command_buffer, 1, 1, &d.feature, d.parameters, &create);
    if (NVSDK_NGX_FAILED(result)) {
        d.feature = nullptr;
        error = "no se pudo crear DLSS (" + ngxError(result) + ")";
        return false;
    }
    std::cout << "[DLSS] " << (mode == NVSDK_NGX_PerfQuality_Value_DLAA ? "DLAA" : "Super Resolution") << " " << render_width
              << "x" << render_height << " -> " << output_width << "x" << output_height << " (modelo transformer)\n";
    return true;
}

bool DlssUpscaler::featureReady() const { return impl_->feature != nullptr; }

void DlssUpscaler::releaseFeature() {
    Impl& d = *impl_;
    if (d.feature != nullptr) NVSDK_NGX_VULKAN_ReleaseFeature(d.feature);
    d.feature = nullptr;
}

bool DlssUpscaler::evaluate(const UpscaleDispatch& in) {
    Impl& d = *impl_;
    if (d.feature == nullptr) return false;
    const auto resource = [](const UpscaleImage& image, bool read_write) {
        VkImageSubresourceRange range{};
        range.aspectMask = image.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = 1;
        return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, range, image.format, image.width,
                                                       image.height, read_write);
    };
    NVSDK_NGX_Resource_VK color = resource(in.color, false);
    NVSDK_NGX_Resource_VK depth = resource(in.depth, false);
    NVSDK_NGX_Resource_VK motion = resource(in.motion, false);
    NVSDK_NGX_Resource_VK output = resource(in.output, true);
    NVSDK_NGX_VK_DLSS_Eval_Params eval{};
    eval.Feature.pInColor = &color;
    eval.Feature.pInOutput = &output;
    eval.pInDepth = &depth;
    eval.pInMotionVectors = &motion;
    const float sign = jitterSign();
    eval.InJitterOffsetX = in.jitter_x * sign;
    eval.InJitterOffsetY = in.jitter_y * sign;
    eval.InRenderSubrectDimensions = NVSDK_NGX_Dimensions{in.render_width, in.render_height};
    eval.InReset = in.reset ? 1 : 0;
    // Del pixel actual al del frame anterior, en pixeles internos.
    eval.InMVScaleX = -static_cast<float>(in.render_width);
    eval.InMVScaleY = -static_cast<float>(in.render_height);
    eval.InPreExposure = 1.0f;
    eval.InExposureScale = 1.0f;
    eval.InFrameTimeDeltaInMsec = in.frame_ms;
    const NVSDK_NGX_Result result = NGX_VULKAN_EVALUATE_DLSS_EXT(in.command_buffer, d.feature, d.parameters, &eval);
    if (NVSDK_NGX_FAILED(result)) {
        std::cerr << "[DLSS] Fallo al evaluar (" << ngxError(result) << ")\n";
        return false;
    }
    return true;
}

void DlssUpscaler::shutdown() {
    Impl& d = *impl_;
    releaseFeature();
    if (d.parameters != nullptr) NVSDK_NGX_VULKAN_DestroyParameters(d.parameters);
    d.parameters = nullptr;
    if (d.initialized) NVSDK_NGX_VULKAN_Shutdown1(d.device);
    d.initialized = false;
    d.available = false;
    d.device = VK_NULL_HANDLE;
}

#else  // sin DLSS

struct DlssUpscaler::Impl {};
DlssUpscaler::DlssUpscaler() : impl_(std::make_unique<Impl>()) {}
DlssUpscaler::~DlssUpscaler() = default;
bool DlssUpscaler::compiled() { return false; }
std::vector<std::string> DlssUpscaler::instanceExtensions() { return {}; }
std::vector<std::string> DlssUpscaler::deviceExtensions(VkInstance) { return {}; }
bool DlssUpscaler::initialize(VkInstance, VkPhysicalDevice, VkDevice, const std::filesystem::path&, std::string& error) {
    error = "el motor se compilo sin DLSS";
    return false;
}
bool DlssUpscaler::available() const { return false; }
bool DlssUpscaler::createFeature(VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, UpscaleQuality,
                                 std::string& error) {
    error = "el motor se compilo sin DLSS";
    return false;
}
bool DlssUpscaler::featureReady() const { return false; }
void DlssUpscaler::releaseFeature() {}
bool DlssUpscaler::evaluate(const UpscaleDispatch&) { return false; }
void DlssUpscaler::shutdown() {}

#endif

}  // namespace cramion::gfx
