#ifndef CRAMION_XR_SYSTEM_H
#define CRAMION_XR_SYSTEM_H

// Realidad virtual con OpenXR (SteamVR, Meta/Oculus, Windows Mixed Reality,
// Pico, Varjo...): el casco, sus dos pantallas y los mandos.
//
// Orden (lo hace VulkanRenderer::initialize con EngineInfo::enable_xr):
//   1) createInstance()              antes de Vulkan: runtime y casco.
//   2) requiredInstanceExtensions()  -> instancia de Vulkan.
//   3) physicalDevice()              la GPU a la que esta conectado el casco.
//   4) requiredDeviceExtensions()    -> dispositivo de Vulkan.
//   5) createSession()               sesion, espacios, swapchains y mandos.
//
// Cada frame:  beginFrame() (espera al casco, poses de ojos y mandos) ...
// logica del juego ... por ojo acquireEye() / dibujar / releaseEye() ...
// endFrame(). Si no hay runtime o casco, createInstance() devuelve false y el
// motor sigue sin VR.
//
// Coordenadas: las de OpenXR (metros, Y arriba, -Z adelante), las mismas del
// motor. Las poses son relativas al origen de seguimiento (el suelo o los
// ojos al empezar); el juego las lleva al mundo con el padre de la camara (el
// "rig", como el XR Origin de Unity).

#include "CramionFX/core/Math.h"
#include "CramionFX/vk/VulkanCommon.h"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cramion::dm {
class Input;
}

namespace cramion::xr {

struct Pose {
    core::Vec3 position{};
    core::Quat orientation{};
    bool valid = false;
};

// Angulos del campo de vision de un ojo (radianes; izquierda y abajo negativos).
struct Fov {
    float left = -0.8f;
    float right = 0.8f;
    float up = 0.8f;
    float down = -0.8f;
};

struct EyeView {
    Pose pose;
    Fov fov;
};

enum class Hand : std::uint8_t { Left = 0, Right = 1 };

enum class Button : std::uint8_t { Trigger = 0, Grip, Thumbstick, Primary, Secondary, Menu, Count };

struct Controller {
    bool active = false;  // el mando esta conectado y se sigue
    Pose grip;            // donde se agarra (la mano)
    Pose aim;             // hacia donde apunta (rayos, punteros)
    float trigger = 0.0f;
    float grip_value = 0.0f;
    core::Vec2 thumbstick{};
    std::array<bool, static_cast<std::size_t>(Button::Count)> buttons{};
};

enum class TrackingOrigin : std::uint8_t {
    Floor = 0,  // de pie / room scale: el 0 es el suelo
    Eyes,       // sentado: el 0 es donde estaba la cabeza al empezar
};

// Que runtime de OpenXR se usa (como el "Play Mode OpenXR Runtime" de Unity).
// Windows tiene UNO activo (el ultimo que lo pidio: SteamVR, Meta...), y no
// tiene por que ser el que ve el casco: un Quest por Steam Link o Virtual
// Desktop esta en SteamVR aunque el activo sea el de Meta.
enum class RuntimeChoice : std::uint8_t {
    Auto = 0,  // SteamVR si esta abierto; si no, el de Windows; si no ve casco, Meta
    SteamVR,   // lo abre si hace falta
    Meta,      // Meta Quest Link / Air Link (Oculus)
    System,    // el activo de Windows (o XR_RUNTIME_JSON)
};
const char* runtimeChoiceKey(RuntimeChoice choice);    // "auto", "steamvr", "meta", "system" (ini)
const char* runtimeChoiceLabel(RuntimeChoice choice);  // para la interfaz
RuntimeChoice runtimeChoiceFromKey(const std::string& key);  // desconocido -> Auto

// Utilidades.
core::Vec3 rotate(const core::Quat& q, const core::Vec3& v);
core::Quat multiply(const core::Quat& a, const core::Quat& b);
core::Quat conjugate(const core::Quat& q);
core::Mat4 poseMatrix(const Pose& pose);
const char* buttonName(Button button);  // "trigger", "grip", "thumbstick", "primary", "secondary", "menu"

class XrSystem {
public:
    XrSystem();
    ~XrSystem();
    XrSystem(const XrSystem&) = delete;
    XrSystem& operator=(const XrSystem&) = delete;

    // El motor se compilo con OpenXR (CRAMION_XR).
    static bool compiled();
    // El mutex de la cola de Vulkan (VulkanDevice::queueMutex): xrBeginFrame,
    // xrEndFrame y las imagenes de los ojos usan la cola, y las subidas del
    // streaming la usan desde otros hilos.
    static void setQueueMutex(std::mutex* mutex);

    // Paso 1. Prueba los runtimes de `choice` en orden hasta que uno vea un
    // casco. false: ninguno (error() dice que paso con cada uno). Puede
    // tardar (SteamVR arrancando): se puede llamar desde otro hilo si nadie
    // mas usa este XrSystem mientras tanto.
    bool createInstance(const char* app_name, RuntimeChoice choice = RuntimeChoice::Auto);
    // El runtime activo de Windows ("Meta", "SteamVR"...; "" si no hay) y si
    // SteamVR esta abierto: para explicar en la interfaz que se va a usar.
    static std::string systemRuntimeLabel();
    static bool steamVrRunning();
    std::vector<std::string> requiredInstanceExtensions() const;
    std::vector<std::string> requiredDeviceExtensions() const;
    VkPhysicalDevice physicalDevice(VkInstance instance) const;
    bool createSession(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device, std::uint32_t queue_family,
                       std::uint32_t queue_index);
    // Suelta el casco: pide al runtime cerrar la sesion (SteamVR vuelve a su
    // casa sin quedarse esperando frames) y destruye todo. Con la GPU parada:
    // antes de destruir el dispositivo de Vulkan o para dejar el casco.
    void shutdown();

    const std::string& error() const;
    bool available() const;  // hay sesion
    bool running() const;    // la sesion esta en marcha (el runtime nos deja dibujar)
    bool focused() const;    // el juego tiene los mandos (no esta el menu del sistema encima)
    bool exitRequested() const;  // el runtime pidio cerrar (se quito la app en el casco)
    const std::string& runtimeName() const;  // el que dio el runtime ("SteamVR/OpenXR", "Oculus"...)
    const std::string& systemName() const;
    VkExtent2D eyeExtent() const;
    VkFormat swapchainFormat() const;

    // Espera al casco y empieza un frame (si queda uno empezado, lo termina
    // sin capas). Tambien lee los eventos, las poses y los mandos.
    // true si este frame hay que dibujar los ojos.
    bool beginFrame();
    bool frameBegun() const;
    bool shouldRender() const;
    const EyeView& eye(int index) const;
    // La vista entre los dos ojos, con el campo de vision de los dos juntos:
    // la camara de las sombras (cubre los dos ojos) y, sin estereo, la unica.
    EyeView centerView() const;
    // Frecuencia de las pantallas del casco (72/90/120 Hz...; 90 si aun no se sabe).
    float displayHz() const;
    // Estereo (por defecto): una camara por ojo, como Unreal (cada ojo con su
    // propia historia temporal). false: una sola imagen para los dos ojos
    // (mitad de coste, pero sin profundidad: todo parece lejos y grande).
    void setStereo(bool stereo);
    bool stereo() const;
    // El runtime da el suelo de la habitacion (espacio STAGE). Sin el, "de
    // pie" no sabe la altura de la cabeza: el rig la pone a la del XR Origin.
    bool floorAvailable() const;
    const Pose& head() const;
    const Controller& controller(Hand hand) const;

    // Imagen de la swapchain del ojo (0 izquierdo, 1 derecho) para este frame,
    // en COLOR_ATTACHMENT_OPTIMAL (y asi hay que devolverla). VK_NULL_HANDLE si no.
    VkImage acquireEye(int eye);
    // Despues de enviar a la cola los comandos que la escriben.
    void releaseEye(int eye);
    // Envia los ojos dibujados este frame al casco.
    void endFrame();

    void vibrate(Hand hand, float amplitude, float seconds, float frequency = 0.0f);
    void setTrackingOrigin(TrackingOrigin origin);
    TrackingOrigin trackingOrigin() const;

    // Botones y ejes de los mandos a la entrada del juego (Input Actions:
    // "XR Right Trigger"...).
    void applyToInput(dm::Input& input) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cramion::xr

#endif  // CRAMION_XR_SYSTEM_H
