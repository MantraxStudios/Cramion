#include "CramionFX/xr/XrSystem.h"

#include <CramionDM/Input.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <thread>

#if defined(CRAMION_XR)
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace cramion::xr {

namespace {
std::mutex* g_queue_mutex = nullptr;
// Llama a `f` con la cola de Vulkan para este hilo (ver setQueueMutex).
template <typename F>
auto queueLocked(F&& f) {
    std::unique_lock<std::mutex> lock;
    if (g_queue_mutex != nullptr) lock = std::unique_lock<std::mutex>(*g_queue_mutex);
    return f();
}
}  // namespace

void XrSystem::setQueueMutex(std::mutex* mutex) { g_queue_mutex = mutex; }

using core::Quat;
using core::Vec3;

const char* runtimeChoiceKey(RuntimeChoice choice) {
    switch (choice) {
        case RuntimeChoice::SteamVR: return "steamvr";
        case RuntimeChoice::Meta: return "meta";
        case RuntimeChoice::System: return "system";
        default: return "auto";
    }
}

const char* runtimeChoiceLabel(RuntimeChoice choice) {
    switch (choice) {
        case RuntimeChoice::SteamVR: return "SteamVR";
        case RuntimeChoice::Meta: return "Meta Quest Link (Oculus)";
        case RuntimeChoice::System: return "El de Windows";
        default: return "Automatico";
    }
}

RuntimeChoice runtimeChoiceFromKey(const std::string& key) {
    if (key == "steamvr") return RuntimeChoice::SteamVR;
    if (key == "meta") return RuntimeChoice::Meta;
    if (key == "system") return RuntimeChoice::System;
    return RuntimeChoice::Auto;
}

// -----------------------------------------------------------------------------
// Runtimes instalados (Windows): donde estan y cual esta abierto
// -----------------------------------------------------------------------------

namespace {

#if defined(_WIN32)

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring lower(std::wstring text) {
    for (wchar_t& c : text) c = static_cast<wchar_t>(std::towlower(c));
    return text;
}

bool containsNoCase(const std::wstring& text, const wchar_t* part) { return lower(text).find(lower(part)) != std::wstring::npos; }

std::wstring environmentVariable(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) return {};
    std::wstring value(size, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(), size);
    value.resize(length < size ? length : 0);
    return value;
}

std::wstring registryString(HKEY root, const wchar_t* key, const wchar_t* value) {
    DWORD size = 0;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size == 0) return {};
    std::wstring out(size / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, out.data(), &size) != ERROR_SUCCESS) return {};
    out.resize(wcsnlen(out.c_str(), out.size()));
    return out;
}

bool fileExists(const std::wstring& path) {
    std::error_code ec;
    return !path.empty() && std::filesystem::is_regular_file(std::filesystem::path(path), ec);
}

bool sameFile(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    std::error_code ec;
    if (std::filesystem::equivalent(std::filesystem::path(a), std::filesystem::path(b), ec)) return true;
    return lower(a) == lower(b);
}

bool processRunning(const wchar_t* exe) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry)) {
        found = _wcsicmp(entry.szExeFile, exe) == 0;
    }
    CloseHandle(snapshot);
    return found;
}

// El que usa el cargador de OpenXR si nadie dice otra cosa.
std::wstring activeRuntimeJson() {
    return registryString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime");
}

// Los que se han registrado (activados o no).
std::vector<std::wstring> availableRuntimeJsons() {
    std::vector<std::wstring> out;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes", 0, KEY_READ, &key) !=
        ERROR_SUCCESS) {
        return out;
    }
    for (DWORD i = 0; i < 256; ++i) {
        wchar_t name[2048];
        DWORD name_size = 2048;
        DWORD type = 0;
        DWORD data = 0;
        DWORD data_size = sizeof(data);
        const LONG r = RegEnumValueW(key, i, name, &name_size, nullptr, &type, reinterpret_cast<BYTE*>(&data), &data_size);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r == ERROR_SUCCESS && type == REG_DWORD && data == 0) out.emplace_back(name, name_size);
    }
    RegCloseKey(key);
    return out;
}

// SteamVR no siempre se registra en AvailableRuntimes: se busca donde lo
// apunta OpenVR (%LOCALAPPDATA%/openvr/openvrpaths.vrpath) y en Steam.
std::wstring steamVrJson() {
    std::vector<std::filesystem::path> folders;
    if (const std::wstring local = environmentVariable(L"LOCALAPPDATA"); !local.empty()) {
        std::ifstream in(std::filesystem::path(local) / "openvr" / "openvrpaths.vrpath", std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::size_t at = text.find("\"runtime\"");
        const std::size_t open = at == std::string::npos ? std::string::npos : text.find('[', at);
        const std::size_t close = open == std::string::npos ? std::string::npos : text.find(']', open);
        for (std::size_t q = open == std::string::npos ? std::string::npos : text.find('"', open);
             q != std::string::npos && q < close;) {
            std::string value;
            std::size_t i = q + 1;
            for (; i < text.size() && text[i] != '"'; ++i) {
                if (text[i] == '\\' && i + 1 < text.size()) ++i;  // JSON: "C:\\Program Files..."
                value += text[i];
            }
            if (!value.empty()) folders.emplace_back(widen(value));
            q = text.find('"', i + 1);
        }
    }
    if (const std::wstring steam = registryString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"); !steam.empty()) {
        folders.push_back(std::filesystem::path(steam) / "steamapps" / "common" / "SteamVR");
    }
    folders.emplace_back(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR");
    for (const std::filesystem::path& folder : folders) {
        const std::wstring json = (folder / "steamxr_win64.json").wstring();
        if (fileExists(json)) return std::filesystem::path(json).make_preferred().wstring();
    }
    for (const std::wstring& json : availableRuntimeJsons()) {
        if (containsNoCase(json, L"steamxr") && fileExists(json)) return json;
    }
    return {};
}

// Meta Horizon Link (antes Oculus).
std::wstring metaJson() {
    for (const std::wstring& json : availableRuntimeJsons()) {
        if (containsNoCase(json, L"oculus_openxr") && fileExists(json)) return json;
    }
    std::vector<std::filesystem::path> folders;
    if (const std::wstring base = registryString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Oculus VR, LLC\\Oculus", L"Base");
        !base.empty()) {
        folders.emplace_back(base);
    }
    folders.emplace_back(L"C:\\Program Files\\Meta Horizon");
    folders.emplace_back(L"C:\\Program Files\\Oculus");
    for (const std::filesystem::path& folder : folders) {
        const std::wstring json = (folder / "Support" / "oculus-runtime" / "oculus_openxr_64.json").wstring();
        if (fileExists(json)) return json;
    }
    return {};
}

std::string labelForJson(const std::wstring& json) {
    if (containsNoCase(json, L"steamxr")) return "SteamVR";
    if (containsNoCase(json, L"oculus")) return "Meta";
    if (containsNoCase(json, L"mixedreality")) return "Windows Mixed Reality";
    if (containsNoCase(json, L"virtualdesktop")) return "Virtual Desktop";
    if (containsNoCase(json, L"pico")) return "Pico";
    if (containsNoCase(json, L"varjo")) return "Varjo";
    if (containsNoCase(json, L"vive")) return "VIVE";
    return narrow(std::filesystem::path(json).stem().wstring());
}

#endif  // _WIN32

}  // namespace

std::string XrSystem::systemRuntimeLabel() {
#if defined(_WIN32)
    const std::wstring user = environmentVariable(L"XR_RUNTIME_JSON");
    const std::wstring active = !user.empty() ? user : activeRuntimeJson();
    return active.empty() ? std::string() : labelForJson(active);
#else
    return {};
#endif
}

bool XrSystem::steamVrRunning() {
#if defined(_WIN32)
    return processRunning(L"vrserver.exe");
#else
    return false;
#endif
}

// -----------------------------------------------------------------------------
// Utilidades
// -----------------------------------------------------------------------------

Quat multiply(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Quat conjugate(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }

Vec3 rotate(const Quat& q, const Vec3& v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = core::cross(u, v) * 2.0f;
    return v + t * q.w + core::cross(u, t);
}

core::Mat4 poseMatrix(const Pose& pose) { return core::composeTrs(pose.position, pose.orientation, Vec3{1.0f, 1.0f, 1.0f}); }

const char* buttonName(Button button) {
    switch (button) {
        case Button::Trigger: return "trigger";
        case Button::Grip: return "grip";
        case Button::Thumbstick: return "thumbstick";
        case Button::Primary: return "primary";
        case Button::Secondary: return "secondary";
        case Button::Menu: return "menu";
        default: return "?";
    }
}

void XrSystem::applyToInput(dm::Input& input) const {
    // Sin sesion o sin foco: todo suelto (las transiciones salen igual).
    const bool on = focused();
    for (int h = 0; h < 2; ++h) {
        const Controller& c = controller(static_cast<Hand>(h));
        const bool live = on && c.active;
        const int b0 = h * 6;
        const int a0 = h * 4;
        for (int b = 0; b < 6; ++b) {
            input.setXrButton(static_cast<dm::XrButton>(b0 + b), live && c.buttons[static_cast<std::size_t>(b)]);
        }
        input.setXrAxis(static_cast<dm::XrAxis>(a0 + 0), live ? c.trigger : 0.0f);
        input.setXrAxis(static_cast<dm::XrAxis>(a0 + 1), live ? c.grip_value : 0.0f);
        input.setXrAxis(static_cast<dm::XrAxis>(a0 + 2), live ? c.thumbstick.x : 0.0f);
        input.setXrAxis(static_cast<dm::XrAxis>(a0 + 3), live ? c.thumbstick.y : 0.0f);
    }
}

#if defined(CRAMION_XR)

// -----------------------------------------------------------------------------
// OpenXR
// -----------------------------------------------------------------------------

namespace {

std::vector<std::string> splitSpaces(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string word;
    while (in >> word) out.push_back(word);
    return out;
}

Pose toPose(const XrPosef& p, bool valid) {
    Pose out;
    out.position = Vec3{p.position.x, p.position.y, p.position.z};
    out.orientation = Quat{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
    out.valid = valid;
    return out;
}

// Una camara para los dos ojos: entre los dos ojos, mirando como la cabeza, y
// con un campo de vision que cubre los dos (lo que ve cualquiera de ellos).
XrView combinedView(const std::array<XrView, 2>& views) {
    XrView c = views[0];
    const XrVector3f& a = views[0].pose.position;
    const XrVector3f& b = views[1].pose.position;
    c.pose.position = XrVector3f{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f};
    c.fov.angleLeft = std::min(views[0].fov.angleLeft, views[1].fov.angleLeft);
    c.fov.angleRight = std::max(views[0].fov.angleRight, views[1].fov.angleRight);
    c.fov.angleUp = std::max(views[0].fov.angleUp, views[1].fov.angleUp);
    c.fov.angleDown = std::min(views[0].fov.angleDown, views[1].fov.angleDown);
    return c;
}

// Un runtime a probar. El cargador de OpenXR usa XR_RUNTIME_JSON si existe
// (si no, el activo del registro) y lo lee cada vez que carga un runtime: al
// destruir la instancia lo descarga y se puede probar otro.
struct RuntimeCandidate {
    std::string label;
    std::wstring json;     // vacio: el de Windows (o el XR_RUNTIME_JSON que puso el usuario)
    int wait_ms = 0;       // sin casco: reintentos (el runtime o el casco aun arrancando)
    bool missing = false;  // no esta instalado
};

std::vector<RuntimeCandidate> runtimeCandidates(RuntimeChoice choice) {
    std::vector<RuntimeCandidate> out;
#if defined(_WIN32)
    const std::wstring user_override = environmentVariable(L"XR_RUNTIME_JSON");
    const std::wstring active = !user_override.empty() ? user_override : activeRuntimeJson();
    const std::wstring steam = steamVrJson();
    const std::wstring meta = metaJson();
    const bool steam_running = processRunning(L"vrserver.exe");
    const std::string system_label =
        (active.empty() ? std::string("Windows") : labelForJson(active)) +
        (user_override.empty() ? " (el runtime activo de Windows)" : " (XR_RUNTIME_JSON)");
    // SteamVR cerrado tarda en arrancar y en ver el casco.
    const int system_wait = sameFile(active, steam) ? (steam_running ? 2000 : 15000) : 1000;
    switch (choice) {
        case RuntimeChoice::SteamVR:
            out.push_back({"SteamVR", steam, steam_running ? 2000 : 15000, steam.empty()});
            break;
        case RuntimeChoice::Meta:
            out.push_back({"Meta", meta, 2000, meta.empty()});
            break;
        case RuntimeChoice::System:
            out.push_back({system_label, L"", system_wait, false});
            break;
        default:  // Auto
            if (!user_override.empty()) {  // quien pone XR_RUNTIME_JSON sabe lo que quiere
                out.push_back({system_label, L"", system_wait, false});
                break;
            }
            // SteamVR abierto: el casco esta ahi (Steam Link, Virtual Desktop,
            // Index, Vive... o un Quest por Link con SteamVR encima).
            if (steam_running && !steam.empty()) out.push_back({"SteamVR (abierto)", steam, 2000, false});
            if (!(steam_running && sameFile(active, steam))) out.push_back({system_label, L"", system_wait, false});
            if (!meta.empty() && !sameFile(active, meta)) out.push_back({"Meta", meta, 500, false});
            break;
    }
#else
    (void)choice;
    out.push_back({"OpenXR", L"", 1000, false});
#endif
    return out;
}

// XR_RUNTIME_JSON solo mientras se carga el runtime (los procesos hijos no
// lo heredan y el valor del usuario vuelve a su sitio).
class RuntimeOverride {
public:
    explicit RuntimeOverride(const std::wstring& json) {
#if defined(_WIN32)
        if (json.empty()) return;
        previous_ = environmentVariable(L"XR_RUNTIME_JSON");
        active_ = SetEnvironmentVariableW(L"XR_RUNTIME_JSON", json.c_str()) != 0;
#else
        (void)json;
#endif
    }
    ~RuntimeOverride() {
#if defined(_WIN32)
        if (active_) SetEnvironmentVariableW(L"XR_RUNTIME_JSON", previous_.empty() ? nullptr : previous_.c_str());
#endif
    }
    RuntimeOverride(const RuntimeOverride&) = delete;
    RuntimeOverride& operator=(const RuntimeOverride&) = delete;

private:
    std::wstring previous_;
    bool active_ = false;
};

// Copia UTF-8 que cabe en `size` bytes sin partir un caracter (el nombre del
// proyecto puede llevar tildes; OpenXR rechaza UTF-8 roto).
void copyUtf8(char* out, std::size_t size, const char* text) {
    std::size_t n = std::min(std::strlen(text), size - 1);
    if (n < std::strlen(text)) {
        while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0u) == 0x80u) --n;  // no cortar dentro de uno
    }
    std::memcpy(out, text, n);
    out[n] = '\0';
}

std::string resultText(XrInstance instance, XrResult r) {
    char text[XR_MAX_RESULT_STRING_SIZE] = {};
    if (instance != XR_NULL_HANDLE) xrResultToString(instance, r, text);
    return text[0] != '\0' ? std::string(text) : std::to_string(static_cast<int>(r));
}

}  // namespace

struct XrSystem::Impl {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace stage_space = XR_NULL_HANDLE;
    XrSpace local_space = XR_NULL_HANDLE;
    XrSpace view_space = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool session_running = false;
    bool exit_requested = false;
    TrackingOrigin origin = TrackingOrigin::Floor;
    bool stereo = true;  // una camara por ojo (false: la misma imagen a los dos)
    std::string error;
    std::string runtime_name;
    std::string system_name;
    std::string runtime_label;  // el candidato que vio el casco ("SteamVR (abierto)", "Meta"...)

    PFN_xrGetVulkanInstanceExtensionsKHR get_instance_extensions = nullptr;
    PFN_xrGetVulkanDeviceExtensionsKHR get_device_extensions = nullptr;
    PFN_xrGetVulkanGraphicsDeviceKHR get_graphics_device = nullptr;
    PFN_xrGetVulkanGraphicsRequirementsKHR get_graphics_requirements = nullptr;

    struct EyeSwapchain {
        XrSwapchain handle = XR_NULL_HANDLE;
        std::vector<XrSwapchainImageVulkanKHR> images;
        std::uint32_t acquired = 0;
        bool holding = false;
        bool drawn = false;
    };
    std::array<EyeSwapchain, 2> eyes_sc;
    VkExtent2D extent{0, 0};
    VkFormat format = VK_FORMAT_UNDEFINED;

    // Frame.
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    bool frame_begun = false;
    XrSpace frame_space = XR_NULL_HANDLE;  // el de las vistas de este frame (la capa va en el mismo)
    std::array<XrView, 2> views{};
    std::array<EyeView, 2> eye_views{};
    Pose head;

    // Mandos.
    XrActionSet action_set = XR_NULL_HANDLE;
    XrAction grip_pose = XR_NULL_HANDLE, aim_pose = XR_NULL_HANDLE, trigger = XR_NULL_HANDLE, squeeze = XR_NULL_HANDLE,
             thumbstick = XR_NULL_HANDLE, thumbstick_click = XR_NULL_HANDLE, primary = XR_NULL_HANDLE,
             secondary = XR_NULL_HANDLE, menu = XR_NULL_HANDLE, haptic = XR_NULL_HANDLE;
    std::array<XrPath, 2> hand_paths{XR_NULL_PATH, XR_NULL_PATH};
    std::array<XrSpace, 2> grip_spaces{XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::array<XrSpace, 2> aim_spaces{XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::array<Controller, 2> controllers{};

    bool ok(XrResult r, const char* what) {
        if (XR_SUCCEEDED(r)) return true;
        char text[XR_MAX_RESULT_STRING_SIZE] = {};
        if (instance != XR_NULL_HANDLE) xrResultToString(instance, r, text);
        error = std::string(what) + ": " + (text[0] != '\0' ? text : std::to_string(static_cast<int>(r)));
        std::cerr << "[VR] " << error << "\n";
        return false;
    }

    XrPath path(const char* text) const {
        XrPath p = XR_NULL_PATH;
        xrStringToPath(instance, text, &p);
        return p;
    }

    XrSpace appSpace() const {
        return origin == TrackingOrigin::Floor && stage_space != XR_NULL_HANDLE ? stage_space : local_space;
    }

    XrAction makeAction(const char* name, const char* label, XrActionType type) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
        std::strncpy(info.localizedActionName, label, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        info.actionType = type;
        info.countSubactionPaths = 2;
        info.subactionPaths = hand_paths.data();
        XrAction action = XR_NULL_HANDLE;
        ok(xrCreateAction(action_set, &info, &action), name);
        return action;
    }

    // Cada perfil por separado: si un runtime no conoce uno, los demas valen.
    void suggest(const char* profile, const std::vector<std::pair<XrAction, const char*>>& bindings) {
        std::vector<XrActionSuggestedBinding> list;
        for (const auto& [action, p] : bindings) {
            if (action != XR_NULL_HANDLE) list.push_back({action, path(p)});
        }
        XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        info.interactionProfile = path(profile);
        info.countSuggestedBindings = static_cast<std::uint32_t>(list.size());
        info.suggestedBindings = list.data();
        const XrResult r = xrSuggestInteractionProfileBindings(instance, &info);
        if (XR_FAILED(r)) std::cerr << "[VR] Perfil de mandos no aceptado: " << profile << " (" << static_cast<int>(r) << ")\n";
    }

    void createActions() {
        XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strncpy(set_info.actionSetName, "cramion", XR_MAX_ACTION_SET_NAME_SIZE - 1);
        std::strncpy(set_info.localizedActionSetName, "Cramion", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
        if (!ok(xrCreateActionSet(instance, &set_info, &action_set), "xrCreateActionSet")) return;
        hand_paths = {path("/user/hand/left"), path("/user/hand/right")};
        grip_pose = makeAction("grip_pose", "Mano", XR_ACTION_TYPE_POSE_INPUT);
        aim_pose = makeAction("aim_pose", "Apuntar", XR_ACTION_TYPE_POSE_INPUT);
        trigger = makeAction("trigger", "Gatillo", XR_ACTION_TYPE_FLOAT_INPUT);
        squeeze = makeAction("squeeze", "Agarre", XR_ACTION_TYPE_FLOAT_INPUT);
        thumbstick = makeAction("thumbstick", "Stick", XR_ACTION_TYPE_VECTOR2F_INPUT);
        thumbstick_click = makeAction("thumbstick_click", "Pulsar el stick", XR_ACTION_TYPE_BOOLEAN_INPUT);
        primary = makeAction("primary", "Boton A/X", XR_ACTION_TYPE_BOOLEAN_INPUT);
        secondary = makeAction("secondary", "Boton B/Y", XR_ACTION_TYPE_BOOLEAN_INPUT);
        menu = makeAction("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
        haptic = makeAction("haptic", "Vibracion", XR_ACTION_TYPE_VIBRATION_OUTPUT);

        const auto both = [](std::vector<std::pair<XrAction, const char*>>& list, XrAction a, const char* left,
                             const char* right) {
            list.push_back({a, left});
            list.push_back({a, right});
        };
        {  // Cualquier mando (el perfil minimo)
            std::vector<std::pair<XrAction, const char*>> b;
            both(b, grip_pose, "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose");
            both(b, aim_pose, "/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose");
            both(b, trigger, "/user/hand/left/input/select/click", "/user/hand/right/input/select/click");
            both(b, menu, "/user/hand/left/input/menu/click", "/user/hand/right/input/menu/click");
            both(b, haptic, "/user/hand/left/output/haptic", "/user/hand/right/output/haptic");
            suggest("/interaction_profiles/khr/simple_controller", b);
        }
        {  // Meta / Oculus Touch (Quest por Link o Air Link)
            std::vector<std::pair<XrAction, const char*>> b;
            both(b, grip_pose, "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose");
            both(b, aim_pose, "/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose");
            both(b, trigger, "/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value");
            both(b, squeeze, "/user/hand/left/input/squeeze/value", "/user/hand/right/input/squeeze/value");
            both(b, thumbstick, "/user/hand/left/input/thumbstick", "/user/hand/right/input/thumbstick");
            both(b, thumbstick_click, "/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click");
            both(b, primary, "/user/hand/left/input/x/click", "/user/hand/right/input/a/click");
            both(b, secondary, "/user/hand/left/input/y/click", "/user/hand/right/input/b/click");
            b.push_back({menu, "/user/hand/left/input/menu/click"});
            both(b, haptic, "/user/hand/left/output/haptic", "/user/hand/right/output/haptic");
            suggest("/interaction_profiles/oculus/touch_controller", b);
        }
        {  // Valve Index
            std::vector<std::pair<XrAction, const char*>> b;
            both(b, grip_pose, "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose");
            both(b, aim_pose, "/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose");
            both(b, trigger, "/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value");
            both(b, squeeze, "/user/hand/left/input/squeeze/value", "/user/hand/right/input/squeeze/value");
            both(b, thumbstick, "/user/hand/left/input/thumbstick", "/user/hand/right/input/thumbstick");
            both(b, thumbstick_click, "/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click");
            both(b, primary, "/user/hand/left/input/a/click", "/user/hand/right/input/a/click");
            both(b, secondary, "/user/hand/left/input/b/click", "/user/hand/right/input/b/click");
            both(b, haptic, "/user/hand/left/output/haptic", "/user/hand/right/output/haptic");
            suggest("/interaction_profiles/valve/index_controller", b);
        }
        {  // HTC Vive (el trackpad hace de stick)
            std::vector<std::pair<XrAction, const char*>> b;
            both(b, grip_pose, "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose");
            both(b, aim_pose, "/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose");
            both(b, trigger, "/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value");
            both(b, squeeze, "/user/hand/left/input/squeeze/click", "/user/hand/right/input/squeeze/click");
            both(b, thumbstick, "/user/hand/left/input/trackpad", "/user/hand/right/input/trackpad");
            both(b, thumbstick_click, "/user/hand/left/input/trackpad/click", "/user/hand/right/input/trackpad/click");
            both(b, menu, "/user/hand/left/input/menu/click", "/user/hand/right/input/menu/click");
            both(b, haptic, "/user/hand/left/output/haptic", "/user/hand/right/output/haptic");
            suggest("/interaction_profiles/htc/vive_controller", b);
        }
        {  // Windows Mixed Reality
            std::vector<std::pair<XrAction, const char*>> b;
            both(b, grip_pose, "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose");
            both(b, aim_pose, "/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose");
            both(b, trigger, "/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value");
            both(b, squeeze, "/user/hand/left/input/squeeze/click", "/user/hand/right/input/squeeze/click");
            both(b, thumbstick, "/user/hand/left/input/thumbstick", "/user/hand/right/input/thumbstick");
            both(b, thumbstick_click, "/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click");
            both(b, primary, "/user/hand/left/input/trackpad/click", "/user/hand/right/input/trackpad/click");
            both(b, menu, "/user/hand/left/input/menu/click", "/user/hand/right/input/menu/click");
            both(b, haptic, "/user/hand/left/output/haptic", "/user/hand/right/output/haptic");
            suggest("/interaction_profiles/microsoft/motion_controller", b);
        }
    }

    void createActionSpaces() {
        for (int h = 0; h < 2; ++h) {
            XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            info.subactionPath = hand_paths[static_cast<std::size_t>(h)];
            info.poseInActionSpace.orientation.w = 1.0f;
            info.action = grip_pose;
            if (grip_pose != XR_NULL_HANDLE) xrCreateActionSpace(session, &info, &grip_spaces[static_cast<std::size_t>(h)]);
            info.action = aim_pose;
            if (aim_pose != XR_NULL_HANDLE) xrCreateActionSpace(session, &info, &aim_spaces[static_cast<std::size_t>(h)]);
        }
        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach.countActionSets = 1;
        attach.actionSets = &action_set;
        ok(xrAttachSessionActionSets(session, &attach), "xrAttachSessionActionSets");
    }

    Pose locate(XrSpace space, XrTime time) const {
        if (space == XR_NULL_HANDLE) return {};
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(space, appSpace(), time, &location))) return {};
        const bool valid = (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
                           (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
        return toPose(location.pose, valid);
    }

    float getFloat(XrAction action, int hand) const {
        if (action == XR_NULL_HANDLE) return 0.0f;
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = hand_paths[static_cast<std::size_t>(hand)];
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_FAILED(xrGetActionStateFloat(session, &info, &state)) || !state.isActive) return 0.0f;
        return state.currentState;
    }

    bool getBool(XrAction action, int hand) const {
        if (action == XR_NULL_HANDLE) return false;
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = hand_paths[static_cast<std::size_t>(hand)];
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_FAILED(xrGetActionStateBoolean(session, &info, &state)) || !state.isActive) return false;
        return state.currentState == XR_TRUE;
    }

    core::Vec2 getVec2(XrAction action, int hand) const {
        if (action == XR_NULL_HANDLE) return {};
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = hand_paths[static_cast<std::size_t>(hand)];
        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_FAILED(xrGetActionStateVector2f(session, &info, &state)) || !state.isActive) return {};
        return core::Vec2{state.currentState.x, state.currentState.y};
    }

    bool poseActive(XrAction action, int hand) const {
        if (action == XR_NULL_HANDLE) return false;
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = hand_paths[static_cast<std::size_t>(hand)];
        XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
        return XR_SUCCEEDED(xrGetActionStatePose(session, &info, &state)) && state.isActive;
    }

    void updateControllers(XrTime time) {
        if (action_set == XR_NULL_HANDLE) return;
        const bool can_read = state == XR_SESSION_STATE_FOCUSED;
        if (can_read) {
            XrActiveActionSet active{action_set, XR_NULL_PATH};
            XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
            sync.countActiveActionSets = 1;
            sync.activeActionSets = &active;
            xrSyncActions(session, &sync);
        }
        for (int h = 0; h < 2; ++h) {
            Controller& c = controllers[static_cast<std::size_t>(h)];
            const Controller previous = c;
            c = Controller{};
            if (!can_read) continue;
            c.active = poseActive(grip_pose, h);
            c.grip = locate(grip_spaces[static_cast<std::size_t>(h)], time);
            c.aim = locate(aim_spaces[static_cast<std::size_t>(h)], time);
            c.trigger = std::clamp(getFloat(trigger, h), 0.0f, 1.0f);
            c.grip_value = std::clamp(getFloat(squeeze, h), 0.0f, 1.0f);
            c.thumbstick = getVec2(thumbstick, h);
            // Gatillo y agarre con histeresis (no parpadean en la mitad).
            const auto analog = [](float v, bool was) { return was ? v > 0.4f : v > 0.6f; };
            c.buttons[static_cast<std::size_t>(Button::Trigger)] =
                analog(c.trigger, previous.buttons[static_cast<std::size_t>(Button::Trigger)]);
            c.buttons[static_cast<std::size_t>(Button::Grip)] =
                analog(c.grip_value, previous.buttons[static_cast<std::size_t>(Button::Grip)]);
            c.buttons[static_cast<std::size_t>(Button::Thumbstick)] = getBool(thumbstick_click, h);
            c.buttons[static_cast<std::size_t>(Button::Primary)] = getBool(primary, h);
            c.buttons[static_cast<std::size_t>(Button::Secondary)] = getBool(secondary, h);
            c.buttons[static_cast<std::size_t>(Button::Menu)] = getBool(menu, h);
        }
    }

    void pollEvents() {
        if (instance == XR_NULL_HANDLE) return;
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance, &event) == XR_SUCCESS) {
            switch (event.type) {
                case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                    const auto& changed = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                    state = changed.state;
                    if (state == XR_SESSION_STATE_READY && session != XR_NULL_HANDLE) {
                        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        if (ok(xrBeginSession(session, &begin), "xrBeginSession")) {
                            session_running = true;
                            std::cout << "[VR] Sesion en marcha\n";
                        }
                    } else if (state == XR_SESSION_STATE_STOPPING) {
                        xrEndSession(session);
                        session_running = false;
                        frame_begun = false;
                        std::cout << "[VR] Sesion parada\n";
                    } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                        // Cerrada desde el casco, o el runtime se cayo: la
                        // sesion ya no sirve (el juego sale de VR).
                        session_running = false;
                        exit_requested = true;
                    }
                    break;
                }
                case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                    session_running = false;
                    exit_requested = true;
                    break;
                default: break;
            }
            event = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    void endFrameNow() {
        if (!frame_begun) return;
        frame_begun = false;
        for (EyeSwapchain& e : eyes_sc) {
            if (e.holding) {
                XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                queueLocked([&] { return xrReleaseSwapchainImage(e.handle, &release); });
                e.holding = false;
            }
        }
        std::array<XrCompositionLayerProjectionView, 2> projection_views{};
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const XrCompositionLayerBaseHeader* layers[1] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
        const bool drawn = frame_state.shouldRender && eyes_sc[0].drawn && eyes_sc[1].drawn;
        if (drawn) {
            // Una camara: los dos ojos llevan la misma imagen, dicha desde el
            // mismo sitio (el compositor la pone bien en cada ojo).
            const XrView center = combinedView(views);
            for (int i = 0; i < 2; ++i) {
                XrCompositionLayerProjectionView& v = projection_views[static_cast<std::size_t>(i)];
                v = XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                const XrView& view = stereo ? views[static_cast<std::size_t>(i)] : center;
                v.pose = view.pose;
                v.fov = view.fov;
                v.subImage.swapchain = eyes_sc[static_cast<std::size_t>(i)].handle;
                v.subImage.imageRect.offset = {0, 0};
                v.subImage.imageRect.extent = {static_cast<std::int32_t>(extent.width), static_cast<std::int32_t>(extent.height)};
                v.subImage.imageArrayIndex = 0;
            }
            layer.space = frame_space != XR_NULL_HANDLE ? frame_space : appSpace();
            layer.viewCount = 2;
            layer.views = projection_views.data();
        }
        eyes_sc[0].drawn = eyes_sc[1].drawn = false;
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = frame_state.predictedDisplayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = drawn ? 1 : 0;
        end.layers = drawn ? layers : nullptr;
        ok(queueLocked([&] { return xrEndFrame(session, &end); }), "xrEndFrame");
    }

    // Salida ordenada: se pide cerrar y se dan frames vacios hasta STOPPING
    // (pollEvents llama a xrEndSession). Asi el runtime (SteamVR, Meta) sabe
    // que la app ha soltado el casco y no se queda esperandola.
    void exitSessionGracefully() {
        if (XR_FAILED(xrRequestExitSession(session))) return;
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        while (session_running && std::chrono::steady_clock::now() < until) {
            pollEvents();
            if (!session_running) break;
            XrFrameState frame{XR_TYPE_FRAME_STATE};
            XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
            if (XR_FAILED(xrWaitFrame(session, &wait, &frame))) break;
            XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
            if (XR_FAILED(queueLocked([&] { return xrBeginFrame(session, &begin); }))) break;
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.displayTime = frame.predictedDisplayTime;
            end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            if (XR_FAILED(queueLocked([&] { return xrEndFrame(session, &end); }))) break;
        }
    }

    // Instancia con este runtime y su casco. false: `why` dice por que no.
    bool tryRuntime(const char* app_name, const RuntimeCandidate& candidate, std::string& why) {
        if (candidate.missing) {
            why = "no esta instalado";
            return false;
        }
        const char* extensions[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        copyUtf8(info.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE,
                 app_name != nullptr && *app_name != '\0' ? app_name : "Cramion");
        info.applicationInfo.applicationVersion = 1;
        copyUtf8(info.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "Cramion Engine");
        info.applicationInfo.engineVersion = 1;
        info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        info.enabledExtensionCount = 1;
        info.enabledExtensionNames = extensions;
        XrResult r = XR_SUCCESS;
        {
            // Sin xrEnumerateInstanceExtensionProperties antes: deja el runtime
            // cargado aunque no se cree la instancia, y ya no se podria probar
            // otro. Si no admite Vulkan, xrCreateInstance lo dice.
            const RuntimeOverride runtime(candidate.json);
            r = xrCreateInstance(&info, &instance);
        }
        if (XR_FAILED(r)) {
            instance = XR_NULL_HANDLE;
            switch (r) {
                case XR_ERROR_RUNTIME_UNAVAILABLE: why = "no se pudo cargar (no hay runtime de OpenXR instalado?)"; break;
                case XR_ERROR_RUNTIME_FAILURE: why = "el runtime fallo al arrancar (esta su servicio parado?)"; break;
                case XR_ERROR_EXTENSION_NOT_PRESENT: why = "no admite Vulkan (XR_KHR_vulkan_enable)"; break;
                default: why = "xrCreateInstance: " + resultText(XR_NULL_HANDLE, r); break;
            }
            return false;
        }
        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
        runtime_name = XR_SUCCEEDED(xrGetInstanceProperties(instance, &properties)) ? std::string(properties.runtimeName)
                                                                                    : candidate.label;

        XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
        system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            r = xrGetSystem(instance, &system_info, &system);
            if (r != XR_ERROR_FORM_FACTOR_UNAVAILABLE) break;
            if (std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(candidate.wait_ms)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (XR_FAILED(r)) {
            if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
                why = "no ve ningun casco";
                if (runtime_name.find("Oculus") != std::string::npos) why += " (Quest Link o Air Link no esta conectado)";
                else if (runtime_name.find("SteamVR") != std::string::npos) why += " (SteamVR no tiene un casco conectado)";
            } else {
                why = "xrGetSystem: " + resultText(instance, r);
            }
            destroy();
            return false;
        }
        XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
        system_name = XR_SUCCEEDED(xrGetSystemProperties(instance, system, &system_properties))
                          ? std::string(system_properties.systemName)
                          : std::string("casco");

        xrGetInstanceProcAddr(instance, "xrGetVulkanInstanceExtensionsKHR",
                              reinterpret_cast<PFN_xrVoidFunction*>(&get_instance_extensions));
        xrGetInstanceProcAddr(instance, "xrGetVulkanDeviceExtensionsKHR",
                              reinterpret_cast<PFN_xrVoidFunction*>(&get_device_extensions));
        xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsDeviceKHR",
                              reinterpret_cast<PFN_xrVoidFunction*>(&get_graphics_device));
        xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsRequirementsKHR",
                              reinterpret_cast<PFN_xrVoidFunction*>(&get_graphics_requirements));
        if (get_instance_extensions == nullptr || get_device_extensions == nullptr || get_graphics_device == nullptr ||
            get_graphics_requirements == nullptr) {
            why = "no da las funciones de Vulkan";
            destroy();
            return false;
        }
        // Obligatorio antes de crear la sesion.
        XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
        get_graphics_requirements(instance, system, &requirements);

        // Las acciones se crean con la instancia (antes de la sesion).
        createActions();
        runtime_label = candidate.label;
        return true;
    }

    void destroy() {
        if (session != XR_NULL_HANDLE && frame_begun) endFrameNow();
        if (session != XR_NULL_HANDLE && session_running) exitSessionGracefully();
        for (EyeSwapchain& e : eyes_sc) {
            if (e.handle != XR_NULL_HANDLE) xrDestroySwapchain(e.handle);
            e = EyeSwapchain{};
        }
        for (XrSpace& s : grip_spaces) {
            if (s != XR_NULL_HANDLE) xrDestroySpace(s);
            s = XR_NULL_HANDLE;
        }
        for (XrSpace& s : aim_spaces) {
            if (s != XR_NULL_HANDLE) xrDestroySpace(s);
            s = XR_NULL_HANDLE;
        }
        for (XrSpace* s : {&stage_space, &local_space, &view_space}) {
            if (*s != XR_NULL_HANDLE) xrDestroySpace(*s);
            *s = XR_NULL_HANDLE;
        }
        if (action_set != XR_NULL_HANDLE) xrDestroyActionSet(action_set);  // destruye sus acciones
        action_set = XR_NULL_HANDLE;
        if (session != XR_NULL_HANDLE) {
            if (session_running) xrEndSession(session);
            xrDestroySession(session);
        }
        session = XR_NULL_HANDLE;
        session_running = false;
        if (instance != XR_NULL_HANDLE) xrDestroyInstance(instance);  // el cargador descarga el runtime
        instance = XR_NULL_HANDLE;
        system = XR_NULL_SYSTEM_ID;
        // Listo para empezar de cero (Play en VR otra vez, otro runtime).
        state = XR_SESSION_STATE_UNKNOWN;
        exit_requested = false;
        frame_begun = false;
        frame_space = XR_NULL_HANDLE;
        controllers = {};
        eye_views = {};
        head = Pose{};
        extent = VkExtent2D{0, 0};
        format = VK_FORMAT_UNDEFINED;
    }
};

XrSystem::XrSystem() : impl_(std::make_unique<Impl>()) {}
XrSystem::~XrSystem() { shutdown(); }

bool XrSystem::compiled() { return true; }

bool XrSystem::createInstance(const char* app_name, RuntimeChoice choice) {
    Impl& d = *impl_;
    if (d.instance != XR_NULL_HANDLE) return true;
    d.error.clear();

    // Cada runtime en orden hasta que uno vea el casco (el activo de Windows
    // puede ser el de Meta con el Quest en SteamVR por Steam Link...).
    std::string report;
    for (const RuntimeCandidate& candidate : runtimeCandidates(choice)) {
        std::string why;
        if (d.tryRuntime(app_name, candidate, why)) {
            std::cout << "[VR] OpenXR: " << d.runtime_name << " (" << candidate.label << "), casco: " << d.system_name
                      << std::endl;
            return true;
        }
        std::cout << "[VR] " << candidate.label << ": " << why << std::endl;
        report += "\n- " + candidate.label + ": " + why;
    }
    d.error = "No se encontro el casco de VR." + report +
              "\nAbre SteamVR (Steam Link, Virtual Desktop, Index, Vive...) o conecta Quest Link / Air Link y vuelve a probar.";
#if defined(_WIN32)
    if (choice == RuntimeChoice::Auto && !steamVrRunning() && !steamVrJson().empty()) {
        d.error += "\nSteamVR esta instalado pero cerrado: en Automatico solo se usa si esta abierto (o elige el runtime SteamVR).";
    }
#endif
    std::cerr << "[VR] " << d.error << std::endl;
    return false;
}

std::vector<std::string> XrSystem::requiredInstanceExtensions() const {
    const Impl& d = *impl_;
    if (d.instance == XR_NULL_HANDLE) return {};
    std::uint32_t size = 0;
    d.get_instance_extensions(d.instance, d.system, 0, &size, nullptr);
    std::string text(size, '\0');
    d.get_instance_extensions(d.instance, d.system, size, &size, text.data());
    return splitSpaces(text.c_str());
}

std::vector<std::string> XrSystem::requiredDeviceExtensions() const {
    const Impl& d = *impl_;
    if (d.instance == XR_NULL_HANDLE) return {};
    std::uint32_t size = 0;
    d.get_device_extensions(d.instance, d.system, 0, &size, nullptr);
    std::string text(size, '\0');
    d.get_device_extensions(d.instance, d.system, size, &size, text.data());
    return splitSpaces(text.c_str());
}

VkPhysicalDevice XrSystem::physicalDevice(VkInstance instance) const {
    const Impl& d = *impl_;
    if (d.instance == XR_NULL_HANDLE) return VK_NULL_HANDLE;
    VkPhysicalDevice device = VK_NULL_HANDLE;
    if (XR_FAILED(d.get_graphics_device(d.instance, d.system, instance, &device))) return VK_NULL_HANDLE;
    return device;
}

bool XrSystem::createSession(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
                             std::uint32_t queue_family, std::uint32_t queue_index) {
    Impl& d = *impl_;
    if (d.instance == XR_NULL_HANDLE) return false;
    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    binding.instance = instance;
    binding.physicalDevice = physical_device;
    binding.device = device;
    binding.queueFamilyIndex = queue_family;
    binding.queueIndex = queue_index;
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    info.next = &binding;
    info.systemId = d.system;
    if (!d.ok(xrCreateSession(d.instance, &info, &d.session), "xrCreateSession")) {
        d.session = XR_NULL_HANDLE;
        return false;
    }

    // Espacios: suelo (si el casco lo tiene), sentado y la cabeza.
    std::uint32_t space_count = 0;
    xrEnumerateReferenceSpaces(d.session, 0, &space_count, nullptr);
    std::vector<XrReferenceSpaceType> spaces(space_count);
    xrEnumerateReferenceSpaces(d.session, space_count, &space_count, spaces.data());
    const auto make_space = [&](XrReferenceSpaceType type, XrSpace& out) {
        XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_info.referenceSpaceType = type;
        space_info.poseInReferenceSpace.orientation.w = 1.0f;
        xrCreateReferenceSpace(d.session, &space_info, &out);
    };
    if (std::find(spaces.begin(), spaces.end(), XR_REFERENCE_SPACE_TYPE_STAGE) != spaces.end()) {
        make_space(XR_REFERENCE_SPACE_TYPE_STAGE, d.stage_space);
    }
    make_space(XR_REFERENCE_SPACE_TYPE_LOCAL, d.local_space);
    make_space(XR_REFERENCE_SPACE_TYPE_VIEW, d.view_space);
    // Sin suelo (STAGE), "de pie" usa LOCAL, cuyo 0 es la cabeza: el rig sube
    // la cabeza a la altura del XR Origin (si no, los ojos quedaban en el
    // suelo y todo se veia gigante).
    std::cout << "[VR] Suelo del casco: "
              << (d.stage_space != XR_NULL_HANDLE ? "STAGE" : "no hay (de pie: cabeza a la altura del XR Origin)")
              << std::endl;

    // Tamano de cada ojo (el recomendado por el runtime).
    std::uint32_t view_count = 0;
    xrEnumerateViewConfigurationViews(d.instance, d.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr);
    std::vector<XrViewConfigurationView> config(view_count, XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(d.instance, d.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count,
                                      &view_count, config.data());
    if (view_count < 2) {
        d.error = "El casco no tiene vista estereo";
        return false;
    }
    d.extent = VkExtent2D{config[0].recommendedImageRectWidth, config[0].recommendedImageRectHeight};

    // Formato: sRGB de 8 bits (el motor ya escribe con gamma; se copia tal cual).
    std::uint32_t format_count = 0;
    xrEnumerateSwapchainFormats(d.session, 0, &format_count, nullptr);
    std::vector<std::int64_t> formats(format_count);
    xrEnumerateSwapchainFormats(d.session, format_count, &format_count, formats.data());
    for (const VkFormat wanted : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB}) {
        if (std::find(formats.begin(), formats.end(), static_cast<std::int64_t>(wanted)) != formats.end()) {
            d.format = wanted;
            break;
        }
    }
    if (d.format == VK_FORMAT_UNDEFINED) {
        d.error = "El runtime no ofrece swapchains sRGB de 8 bits";
        std::cerr << "[VR] " << d.error << "\n";
        return false;
    }
    for (Impl::EyeSwapchain& eye : d.eyes_sc) {
        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sc.format = d.format;
        sc.sampleCount = 1;
        sc.width = d.extent.width;
        sc.height = d.extent.height;
        sc.faceCount = 1;
        sc.arraySize = 1;
        sc.mipCount = 1;
        if (!d.ok(xrCreateSwapchain(d.session, &sc, &eye.handle), "xrCreateSwapchain")) return false;
        std::uint32_t image_count = 0;
        xrEnumerateSwapchainImages(eye.handle, 0, &image_count, nullptr);
        eye.images.assign(image_count, XrSwapchainImageVulkanKHR{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xrEnumerateSwapchainImages(eye.handle, image_count, &image_count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(eye.images.data()));
    }
    d.createActionSpaces();
    for (XrView& v : d.views) v = XrView{XR_TYPE_VIEW};
    std::cout << "[VR] Sesion creada: " << d.extent.width << " x " << d.extent.height << " por ojo\n";
    return true;
}

void XrSystem::shutdown() { impl_->destroy(); }

const std::string& XrSystem::error() const { return impl_->error; }
bool XrSystem::available() const { return impl_->session != XR_NULL_HANDLE; }
bool XrSystem::running() const { return impl_->session_running; }
bool XrSystem::focused() const { return impl_->session_running && impl_->state == XR_SESSION_STATE_FOCUSED; }
bool XrSystem::exitRequested() const { return impl_->exit_requested; }
const std::string& XrSystem::runtimeName() const { return impl_->runtime_name; }
const std::string& XrSystem::systemName() const { return impl_->system_name; }
VkExtent2D XrSystem::eyeExtent() const { return impl_->extent; }
VkFormat XrSystem::swapchainFormat() const { return impl_->format; }
bool XrSystem::frameBegun() const { return impl_->frame_begun; }
bool XrSystem::shouldRender() const { return impl_->frame_begun && impl_->frame_state.shouldRender == XR_TRUE; }
const EyeView& XrSystem::eye(int index) const { return impl_->eye_views[static_cast<std::size_t>(index != 0 ? 1 : 0)]; }
EyeView XrSystem::centerView() const {
    const XrView c = combinedView(impl_->views);
    EyeView out;
    out.pose = toPose(c.pose, impl_->eye_views[0].pose.valid && impl_->eye_views[1].pose.valid);
    out.fov = Fov{c.fov.angleLeft, c.fov.angleRight, c.fov.angleUp, c.fov.angleDown};
    return out;
}
void XrSystem::setStereo(bool stereo) { impl_->stereo = stereo; }
bool XrSystem::stereo() const { return impl_->stereo; }
bool XrSystem::floorAvailable() const { return impl_->stage_space != XR_NULL_HANDLE; }
float XrSystem::displayHz() const {
    const XrDuration period = impl_->frame_state.predictedDisplayPeriod;
    return period > 0 ? std::clamp(1e9f / static_cast<float>(period), 30.0f, 240.0f) : 90.0f;
}
const Pose& XrSystem::head() const { return impl_->head; }
const Controller& XrSystem::controller(Hand hand) const { return impl_->controllers[static_cast<std::size_t>(hand)]; }
TrackingOrigin XrSystem::trackingOrigin() const { return impl_->origin; }
void XrSystem::setTrackingOrigin(TrackingOrigin origin) { impl_->origin = origin; }

bool XrSystem::beginFrame() {
    Impl& d = *impl_;
    if (d.session == XR_NULL_HANDLE) return false;
    d.endFrameNow();  // uno sin terminar (un frame en que no se dibujo)
    d.pollEvents();
    if (!d.session_running) {
        for (Controller& c : d.controllers) c = Controller{};
        return false;
    }
    d.frame_state = XrFrameState{XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
    if (!d.ok(xrWaitFrame(d.session, &wait, &d.frame_state), "xrWaitFrame")) return false;
    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
    if (!d.ok(queueLocked([&] { return xrBeginFrame(d.session, &begin); }), "xrBeginFrame")) return false;
    d.frame_begun = true;
    d.frame_space = d.appSpace();

    const XrTime time = d.frame_state.predictedDisplayTime;
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = time;
    locate.space = d.frame_space;
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    std::uint32_t count = 0;
    for (XrView& v : d.views) v = XrView{XR_TYPE_VIEW};
    const bool located = XR_SUCCEEDED(xrLocateViews(d.session, &locate, &view_state, 2, &count, d.views.data())) && count == 2;
    const bool valid = located && (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
    if (located) {
        for (int i = 0; i < 2; ++i) {
            const XrView& v = d.views[static_cast<std::size_t>(i)];
            d.eye_views[static_cast<std::size_t>(i)].pose = toPose(v.pose, valid);
            d.eye_views[static_cast<std::size_t>(i)].fov = Fov{v.fov.angleLeft, v.fov.angleRight, v.fov.angleUp, v.fov.angleDown};
        }
    }
    d.head = d.locate(d.view_space, time);
    d.updateControllers(time);
    return d.frame_state.shouldRender == XR_TRUE && located;
}

VkImage XrSystem::acquireEye(int eye) {
    Impl& d = *impl_;
    if (!shouldRender() || eye < 0 || eye > 1) return VK_NULL_HANDLE;
    Impl::EyeSwapchain& sc = d.eyes_sc[static_cast<std::size_t>(eye)];
    if (sc.handle == XR_NULL_HANDLE || sc.holding) return VK_NULL_HANDLE;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!d.ok(queueLocked([&] { return xrAcquireSwapchainImage(sc.handle, &acquire, &sc.acquired); }), "xrAcquireSwapchainImage")) return VK_NULL_HANDLE;
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (!d.ok(xrWaitSwapchainImage(sc.handle, &wait), "xrWaitSwapchainImage")) {
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        queueLocked([&] { return xrReleaseSwapchainImage(sc.handle, &release); });
        return VK_NULL_HANDLE;
    }
    sc.holding = true;
    return sc.images[sc.acquired].image;
}

void XrSystem::releaseEye(int eye) {
    Impl& d = *impl_;
    if (eye < 0 || eye > 1) return;
    Impl::EyeSwapchain& sc = d.eyes_sc[static_cast<std::size_t>(eye)];
    if (!sc.holding) return;
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    d.ok(queueLocked([&] { return xrReleaseSwapchainImage(sc.handle, &release); }), "xrReleaseSwapchainImage");
    sc.holding = false;
    sc.drawn = true;
}

void XrSystem::endFrame() { impl_->endFrameNow(); }

void XrSystem::vibrate(Hand hand, float amplitude, float seconds, float frequency) {
    Impl& d = *impl_;
    if (d.haptic == XR_NULL_HANDLE || !focused()) return;
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = std::clamp(amplitude, 0.0f, 1.0f);
    vibration.duration = seconds <= 0.0f ? XR_MIN_HAPTIC_DURATION : static_cast<XrDuration>(seconds * 1e9);
    vibration.frequency = frequency > 0.0f ? frequency : XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action = d.haptic;
    info.subactionPath = d.hand_paths[static_cast<std::size_t>(hand)];
    xrApplyHapticFeedback(d.session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
}

#else  // sin OpenXR (Android o CRAMION_XR=OFF)

struct XrSystem::Impl {
    std::string error = "El motor se compilo sin OpenXR";
    std::string empty;
    std::array<EyeView, 2> eyes{};
    Pose head;
    std::array<Controller, 2> controllers{};
    TrackingOrigin origin = TrackingOrigin::Floor;
    bool stereo = true;
};

XrSystem::XrSystem() : impl_(std::make_unique<Impl>()) {}
XrSystem::~XrSystem() = default;
bool XrSystem::compiled() { return false; }
bool XrSystem::createInstance(const char*, RuntimeChoice) { return false; }
std::vector<std::string> XrSystem::requiredInstanceExtensions() const { return {}; }
std::vector<std::string> XrSystem::requiredDeviceExtensions() const { return {}; }
VkPhysicalDevice XrSystem::physicalDevice(VkInstance) const { return VK_NULL_HANDLE; }
bool XrSystem::createSession(VkInstance, VkPhysicalDevice, VkDevice, std::uint32_t, std::uint32_t) { return false; }
void XrSystem::shutdown() {}
const std::string& XrSystem::error() const { return impl_->error; }
bool XrSystem::available() const { return false; }
bool XrSystem::running() const { return false; }
bool XrSystem::focused() const { return false; }
bool XrSystem::exitRequested() const { return false; }
const std::string& XrSystem::runtimeName() const { return impl_->empty; }
const std::string& XrSystem::systemName() const { return impl_->empty; }
VkExtent2D XrSystem::eyeExtent() const { return {0, 0}; }
VkFormat XrSystem::swapchainFormat() const { return VK_FORMAT_UNDEFINED; }
bool XrSystem::beginFrame() { return false; }
bool XrSystem::frameBegun() const { return false; }
bool XrSystem::shouldRender() const { return false; }
const EyeView& XrSystem::eye(int index) const { return impl_->eyes[index != 0 ? 1 : 0]; }
EyeView XrSystem::centerView() const { return impl_->eyes[0]; }
float XrSystem::displayHz() const { return 90.0f; }
bool XrSystem::floorAvailable() const { return false; }
void XrSystem::setStereo(bool stereo) { impl_->stereo = stereo; }
bool XrSystem::stereo() const { return impl_->stereo; }
const Pose& XrSystem::head() const { return impl_->head; }
const Controller& XrSystem::controller(Hand hand) const { return impl_->controllers[static_cast<std::size_t>(hand)]; }
VkImage XrSystem::acquireEye(int) { return VK_NULL_HANDLE; }
void XrSystem::releaseEye(int) {}
void XrSystem::endFrame() {}
void XrSystem::vibrate(Hand, float, float, float) {}
void XrSystem::setTrackingOrigin(TrackingOrigin origin) { impl_->origin = origin; }
TrackingOrigin XrSystem::trackingOrigin() const { return impl_->origin; }

#endif

}  // namespace cramion::xr
