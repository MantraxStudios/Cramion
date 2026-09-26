#ifndef CRAMION_VK_FRAME_BUDGET_H
#define CRAMION_VK_FRAME_BUDGET_H

// Presupuesto adaptativo de Cramion: hace que el juego vaya a los FPS pedidos
// en cualquier PC, bajando solo lo necesario y lo que menos se nota.
//
//   1. Perfil de hardware (HardwareProfile): al arrancar se mira la GPU
//      (VRAM, integrada o dedicada) y se elige un nivel: Bajo, Medio, Alto o
//      Ultra. Decide la resolucion del mapa de sombras y desde donde empieza
//      el gobernador.
//   2. Gobernador (FrameBudget): cada frame lee el tiempo de GPU de cada
//      pasada (timestamps) y, si se pasa del presupuesto, baja un paso la
//      "palanca" que mas milisegundos ahorra por cada punto de calidad que se
//      pierde. No adivina: el ahorro sale de lo que cuesta cada pasada en
//      ESTA escena y en ESTE PC, y despues de cada cambio mide lo que ahorro
//      de verdad y lo recuerda (modelo de costes aprendido). Con eso sabe si
//      puede volver a subir sin pasarse, y no oscila.
//
// Las palancas solo bajan la calidad que el usuario eligio: nunca activan
// algo que el apago.

#include "CramionFX/vk/GpuProfiler.h"
#include "CramionFX/vk/PostProcessSettings.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace cramion::gfx {

enum class HardwareTier : std::int32_t { Low = 0, Medium = 1, High = 2, Ultra = 3 };

struct HardwareProfile {
    HardwareTier tier = HardwareTier::High;
    std::string gpu_name;
    std::uint64_t vram_mb = 0;
    bool integrated = false;
    bool ray_tracing = false;
};

// Nivel segun la GPU: integrada o < 3 GB = Bajo, < 6 GB = Medio, < 11 GB =
// Alto, el resto Ultra.
HardwareTier tierFor(std::uint64_t vram_mb, bool integrated);
const char* tierName(HardwareTier tier);
// Resolucion por cascada del mapa de sombras del sol para cada nivel
// (4 cascadas x res^2 x 4 bytes: 36 MB en Bajo, 576 MB en Ultra).
std::uint32_t shadowResolutionFor(HardwareTier tier);
// Lado maximo de las texturas de los modelos para cada nivel (0 = sin
// limite): 2048 / 4096 / 8192 / sin limite. Una textura de 16K sin comprimir
// son 1.3 GB de VRAM; con 8K, 340 MB.
std::uint32_t textureSizeFor(HardwareTier tier);

// Palancas, de la que menos se nota a la que mas.
enum class Lever : std::uint8_t {
    Lod = 0,         // error de los LODs y objetos de menos de N pixeles
    ShadowDetail,    // LOD de las sombras por texel, sombras de objetos diminutos, cascadas lejanas
    Volumetric,      // luz volumetrica y rayos de luz
    ContactShadows,  // sombras de contacto
    Ssao,            // oclusion ambiental
    Reflections,     // reflejos en pantalla / por rayos
    Gi,              // luz rebotada
    RenderScale,     // resolucion interna (con AMD FSR 1)
    Count,
};
inline constexpr std::size_t kLeverCount = static_cast<std::size_t>(Lever::Count);

class FrameBudget {
public:
    FrameBudget();

    void setEnabled(bool enabled);
    bool enabled() const { return enabled_; }
    void setTargetFps(float fps);
    float targetFps() const { return target_fps_; }
    float targetMilliseconds() const { return 1000.0f / target_fps_; }

    // Punto de partida segun el hardware (un PC flojo no empieza a tope y
    // tarda segundos en bajar).
    void setStartLevels(HardwareTier tier);

    // Una vez por frame con el tiempo de GPU ya medido (total y por pasada).
    // `dt`: segundos desde el anterior.
    void update(float dt, float gpu_ms, const std::vector<GpuTiming>& passes);

    std::uint8_t level(Lever lever) const { return levels_[static_cast<std::size_t>(lever)]; }
    std::uint8_t maxLevel(Lever lever) const;
    static const char* leverName(Lever lever);

    // --- Lo que el renderizador aplica ---
    PostProcessSettings apply(const PostProcessSettings& user) const;
    float renderScale() const;         // 1 = nativa
    float cullPixels() const;          // radio en pixeles por debajo del cual no se dibuja
    float shadowTexelFactor() const;   // error de LOD permitido en sombras, en texeles
    float shadowMinTexels() const;     // radio minimo (texeles) para proyectar sombra
    std::uint32_t farCascadeInterval() const;  // multiplica el turno de las cascadas 2 y 3

    // --- Estado (interfaz, MCP) ---
    float smoothedGpuMs() const { return ema_ms_; }
    const std::string& lastAction() const { return last_action_; }
    // Ahorro aprendido de bajar `lever` de `level` a `level` + 1 (ms); < 0 si
    // aun no se ha medido.
    float learnedSaving(Lever lever, std::uint8_t level) const;

private:
    static constexpr std::size_t kMaxLevels = 6;

    float estimateSaving(Lever lever, std::uint8_t level) const;
    float penalty(Lever lever, std::uint8_t level) const;
    float settleSeconds(Lever lever) const;
    bool lowerOne();
    bool raiseOne();
    void change(Lever lever, std::uint8_t to, const char* verb, float predicted);

    bool enabled_ = true;
    float target_fps_ = 60.0f;
    std::array<std::uint8_t, kLeverCount> levels_{};
    std::array<std::uint8_t, kLeverCount> start_levels_{};

    float ema_ms_ = 0.0f;
    bool has_sample_ = false;
    float over_seconds_ = 0.0f;
    float under_seconds_ = 0.0f;
    float clock_ = 0.0f;

    // Ultimo tiempo medido de cada pasada (por nombre, solo las que importan).
    struct PassCosts {
        float geometry = 0, shadows = 0, local_shadows = 0, volumetric = 0, shafts = 0, lighting = 0,
              ssao = 0, reflections = 0, gi = 0, total = 0;
    } passes_;

    // Cambio en observacion: se mide lo que de verdad ahorro (o costo).
    struct Pending {
        bool active = false;
        Lever lever = Lever::Lod;
        std::uint8_t from = 0;
        std::uint8_t to = 0;
        float before_ms = 0.0f;
        float timer = 0.0f;
    } pending_;
    // learned_[palanca][nivel]: ms que ahorra pasar de nivel a nivel+1.
    std::array<std::array<float, kMaxLevels>, kLeverCount> learned_{};
    // Tras subir una palanca y tener que bajarla enseguida, no se vuelve a
    // intentar en un rato (cada vez el doble).
    std::array<float, kLeverCount> raise_blocked_until_{};
    std::array<float, kLeverCount> raise_backoff_{};
    std::array<float, kLeverCount> raised_at_{};

    std::string last_action_;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_FRAME_BUDGET_H
