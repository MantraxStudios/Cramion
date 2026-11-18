#ifndef CRAMION_VK_DISPLAY_OUTPUT_H
#define CRAMION_VK_DISPLAY_OUTPUT_H

// Salida de imagen: que formato y espacio de color lleva la swapchain (SDR,
// HDR10 o scRGB), con que se baja la latencia (present_wait, NVIDIA Reflex o
// AMD Anti-Lag 2) y la medida de esa latencia. Todo lo de aqui es logica sin
// GPU (se prueba en CramionOutputTests); VulkanSwapchain y VulkanRenderer la
// usan con lo que dice el driver.
//
//   HDR10:  A2B10G10R10 / A2R10G10B10 con VK_COLOR_SPACE_HDR10_ST2084_EXT
//           (curva PQ, primarios BT.2020). Lo que ve el usuario va en nits.
//   scRGB:  R16G16B16A16_SFLOAT con VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT
//           (lineal, primarios sRGB, 1.0 = 80 nits, puede pasar de 1).
//   SDR:    lo de siempre (UNORM de 8 bits en sRGB no lineal).
// Sin la extension VK_EXT_swapchain_colorspace, o si la pantalla no ofrece
// ninguno de los dos, se vuelve a SDR sin avisar mas que en el log.
// (Sin Vulkan: lo incluye GraphicsSettings.h. La eleccion del formato de
// la swapchain esta en DisplayOutputVk.h.)

#include <array>
#include <chrono>
#include <cstdint>

namespace cramion::gfx {

// Lo que pide el usuario (GraphicsSettings::hdr_output). El indice se guarda
// en Graphics.ini: no reordenar.
enum class HdrOutput : std::int32_t {
    Off = 0,    // SDR (por defecto)
    Auto = 1,   // HDR10 si la pantalla lo ofrece; si no, scRGB; si no, SDR
    Hdr10 = 2,  // solo HDR10 (PQ); si no, SDR
    ScRgb = 3,  // solo scRGB; si no, SDR
};

// Como se codifica lo que se escribe en la swapchain.
enum class OutputEncoding : std::int32_t {
    Sdr = 0,
    Hdr10Pq = 1,
    ScRgbLinear = 2,
};

const char* outputEncodingName(OutputEncoding encoding);

// --- Curvas de la salida HDR (las mismas que hdr_output.frag) ---
// PQ (SMPTE ST 2084): nits absolutos (0..10000) -> 0..1.
float pqEncode(float nits);
// 0..1 -> nits.
float pqDecode(float value);
// Brillo de papel y nits maximos validos (paper 80..500, max >= paper, <= 10000).
float clampPaperWhite(float nits);
float clampMaxNits(float nits, float paper_white);

// --- Baja latencia ---
// Lo que pide el usuario (GraphicsSettings::low_latency). Indices en Graphics.ini.
enum class LowLatencyMode : std::int32_t {
    Off = 0,    // como siempre
    On = 1,     // la CPU no se adelanta a la GPU mas de un frame
    Boost = 2,  // ademas la GPU va a reloj alto (Reflex) / el ritmo mas corto
};

// Con que se consigue.
enum class LatencyBackend : std::int32_t {
    None = 0,
    PresentWait = 1,  // VK_KHR_present_id + VK_KHR_present_wait (cualquier fabricante)
    Reflex = 2,       // VK_NV_low_latency2 (NVIDIA Reflex)
    AntiLag = 3,      // VK_AMD_anti_lag (AMD Anti-Lag 2)
};

const char* latencyBackendName(LatencyBackend backend);
const char* lowLatencyModeName(LowLatencyMode mode);

// Extensiones de salida que se activaron en el dispositivo.
struct OutputExtensions {
    bool present_id = false;
    bool present_wait = false;
    bool nv_low_latency2 = false;
    bool amd_anti_lag = false;
    bool hdr_metadata = false;
};

// Reflex si la GPU lo tiene (necesita present_id para sus marcas), si no
// Anti-Lag 2, si no present_wait; sin nada, None (igual que antes).
LatencyBackend chooseLatencyBackend(LowLatencyMode mode, const OutputExtensions& ext);

// Con present_wait: hasta que present_id se espera antes de empezar el frame
// `next_id` (0 = no esperar). Activado deja uno en cola (la GPU no se queda
// sin trabajo); Boost espera al anterior (sin cola: menos latencia, menos FPS
// si la CPU y la GPU van justas).
std::uint64_t presentWaitTarget(LowLatencyMode mode, std::uint64_t next_id);

// Medida de la latencia: desde que empieza el frame (antes de leer la
// entrada) hasta que se presenta (o, sin present_wait, hasta que la GPU lo
// termina). Media de las ultimas kWindow muestras y la ultima.
class LatencyMeter {
public:
    static constexpr std::size_t kWindow = 64;
    using Clock = std::chrono::steady_clock;

    // Empieza el frame con este id de presentacion.
    void frameStarted(std::uint64_t id, Clock::time_point when);
    // El frame `id` ya se presento (o termino) en `when`. Devuelve false si
    // no se conocia su comienzo (ids viejos, saltos).
    bool frameFinished(std::uint64_t id, Clock::time_point when);
    // Muestra externa (los tiempos que da Reflex), en ms.
    void addSample(float ms);
    float averageMs() const;
    float lastMs() const { return last_ms_; }
    std::size_t samples() const { return count_; }
    void reset();

private:
    struct Start {
        std::uint64_t id = 0;
        Clock::time_point when{};
    };
    std::array<Start, 16> starts_{};
    std::array<float, kWindow> window_{};
    std::size_t next_ = 0;
    std::size_t count_ = 0;
    float last_ms_ = 0.0f;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_DISPLAY_OUTPUT_H
