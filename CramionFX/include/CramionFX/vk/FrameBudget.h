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
#include "CramionFX/vk/GraphicsSettings.h"
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
// Filtrado anisotropico de las texturas para cada nivel: 4x / 8x / 16x / 16x.
std::uint32_t anisotropyFor(HardwareTier tier);
// Objetos diminutos: radio en pixeles (de la imagen interna) por debajo del
// cual la camara no dibuja un objeto, para cada nivel: 1.5 / 1 / 0.75 / 0.5.
// El presupuesto puede subirlo (cullPixels); se usa el mayor de los dos.
float tinyObjectPixelsFor(HardwareTier tier);
// Alcance de las sombras del sol (m) para cada nivel: 60 / 80 / 100 / 100. Las
// cuatro cascadas se reparten en menos metros: menos objetos por cascada y
// mas nitidez por texel.
float shadowDistanceFor(HardwareTier tier);
// Sesgo de LOD de las mallas para cada nivel (multiplica el error en pixeles
// permitido): 2 / 1.5 / 1 / 1. La palanca Lod va encima (antes Bajo y Medio
// arrancaban en Lod 1; ahora en 0, para no contarlo dos veces).
float lodScaleFor(HardwareTier tier);
// Con histeresis: un objeto se oculta por debajo de `threshold` pixeles y no
// vuelve hasta pasar de 1.25 x `threshold` (en el borde no parpadea).
inline bool tinyObject(float pixels, float threshold, bool was_tiny) {
    return pixels < (was_tiny ? threshold * 1.25f : threshold);
}

// --- Juego exportado ---
// La configuracion grafica del proyecto (ProjectSettings/Graphics.ini: la del
// visor del editor en el PC del autor, con su presupuesto apagado, 240 FPS o
// sombras de 6144) adaptada a ESTE PC:
//   - presupuesto adaptativo siempre encendido;
//   - objetivo entre 30 y los Hz de la pantalla, 144 como mucho (60 si no se
//     saben; con 240 el presupuesto lo bajaba todo en cualquier PC);
//   - sombras, texturas y anisotropico del proyecto como tope de los de la
//     gama (0 = segun el PC, se queda): 6144 y texturas de 16K no caben en una
//     GPU de 2-4 GB;
//   - en Bajo y Medio, sin sesgo negativo de mips (mas ancho de banda y
//     parpadeo de lejos).
// `locked` (graphics_lock=1 en game.ini, casilla "Respetar los ajustes
// graficos del proyecto" de la configuracion de compilacion): tal cual.
// `display_hz` <= 1: desconocidos.
GraphicsSettings shippedGraphics(const GraphicsSettings& project, HardwareTier tier, float display_hz, bool locked);

// Render Textures (camaras con Target Texture): cada una es otro frame entero
// (dos portales eran 3 frames por frame). Cada cuantos frames se dibuja cada
// camara segun el nivel: 1 en Alto y Ultra, 2 en Medio, 3 en Bajo.
int renderTextureIntervalFor(HardwareTier tier);
// Si a la camara `index` le toca dibujarse en el frame `frame`: escalonadas
// (con dos camaras cada 3 frames, una en el 0, 3, 6... y la otra en el 2, 5,
// 8...), no todas en el mismo frame.
inline bool renderTextureDue(std::uint64_t frame, std::size_t index, int interval) {
    if (interval <= 1) return true;
    return (frame + index) % static_cast<std::uint64_t>(interval) == 0;
}

// Palancas, de la que menos se nota a la que mas. Las nuevas van al final:
// el log ([Rendimiento] palancas), el editor y el MCP las leen por su indice.
enum class Lever : std::uint8_t {
    Lod = 0,         // error de los LODs y objetos de menos de N pixeles
    ShadowDetail,    // LOD de las sombras por texel, sombras de objetos diminutos, cascadas lejanas
    Volumetric,      // luz volumetrica: 16 tramos sin rayos de luz, 8 tramos, sin luces locales (solo el sol)
    ContactShadows,  // sombras de contacto
    Lighting,        // iluminacion ligera: sin sombras suaves de contacto del sol (PCSS) ni oclusion del cielo desde arriba
    Ssao,            // oclusion ambiental
    Reflections,     // reflejos en pantalla / por rayos
    Gi,              // luz rebotada
    RenderScale,     // resolucion interna (con AMD FSR 1)
    // Profundidad de campo y motion blur: la mitad de muestras y despues
    // apagados. Van despues del escalado (a la resolucion de salida): bajar
    // la resolucion interna no los abarata.
    CameraFx,
    // Nubes volumetricas: 64 pasos (4 hacia el sol) y despues 32 (2), con la
    // sombra de las nubes cada 4 frames.
    Clouds,
    Count,
};
inline constexpr std::size_t kLeverCount = static_cast<std::size_t>(Lever::Count);

// Niveles de salida de cada palanca segun el hardware (setStartLevels): un PC
// flojo no empieza a tope y tarda segundos en bajar.
std::array<std::uint8_t, kLeverCount> startLevelsFor(HardwareTier tier);

// --- Resolucion interna (el renderizador lo aplica al presupuesto) ---
// Nivel de salida de la palanca RenderScale: si el escalado del preset ya
// dibuja por debajo del 90 %, 0 (el presupuesto no la baja ademas de salida;
// con los dos, FSR 1 Calidad en un PC Bajo arrancaba al 45 %).
std::uint8_t renderScaleStartLevel(HardwareTier tier, float preset_scale);
// Suelo de la escala del presupuesto (factor sobre la del preset) para que el
// alto interno no baje de max(540, la mitad del de salida). 1 = no puede bajar
// la resolucion; el preset del usuario por debajo del suelo se respeta.
float renderScaleFloor(std::uint32_t output_height, float preset_scale);

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
    // Cambia el nivel de salida de una palanca (el movil: su resolucion ya la
    // limitan la pantalla del juego y el escalado del preset, asi que la
    // resolucion interna empieza a tope y solo baja si hace falta).
    void setStartLevel(Lever lever, std::uint8_t level);
    std::uint8_t startLevel(Lever lever) const { return start_levels_[static_cast<std::size_t>(lever)]; }
    // Tope de una palanca (por debajo de su maximo): el gobernador no la baja
    // mas alla y el punto de partida tampoco. VR: la resolucion interna se
    // queda en 0.87 como poco (en el casco se nota mucho) y baja antes lo
    // demas. 255 = sin tope (su maximo).
    void setLevelCap(Lever lever, std::uint8_t cap);

    // El usuario cambio la calidad (ajustes graficos, un efecto prendido):
    // se vuelve al punto de partida y se mide de nuevo, sin los bloqueos de
    // subida ni lo aprendido (con otros ajustes cada cosa cuesta distinto).
    void reset();
    // Solo las palancas de la mascara (bit = 1 << indice de la palanca): lo
    // que el gobernador bajo de su punto de partida vuelve a el (lo que subio
    // por encima se queda) y olvidan lo aprendido; las demas siguen igual.
    void restoreLevers(std::uint32_t mask);
    // La escena prendio algo que las palancas pueden apagar (mascara de
    // postLeversRaised): reinicio entero como mucho cada 10 s (la mezcla de
    // volumenes de post-proceso llama en cada frame); entre medias solo
    // vuelven esas palancas, asi lo que se prende se ve ya.
    void qualityRaised(std::uint32_t levers);

    // Con cada muestra nueva de la GPU (total y por pasada, sin medias: las
    // del perfilador tardaban segundos en enterarse de un cambio). `dt`:
    // segundos desde la muestra anterior. Sin medida (0, NaN) no cuenta.
    void update(float dt, float gpu_ms, const std::vector<GpuTiming>& passes);

    std::uint8_t level(Lever lever) const { return levels_[static_cast<std::size_t>(lever)]; }
    std::uint8_t maxLevel(Lever lever) const;
    static const char* leverName(Lever lever);

    // --- Lo que el renderizador aplica ---
    PostProcessSettings apply(const PostProcessSettings& user) const;
    float renderScale() const;         // 1 = nativa (con el suelo de setRenderScaleFloor)
    float cullPixels() const;          // radio en pixeles por debajo del cual no se dibuja
    float shadowTexelFactor() const;   // error de LOD permitido en sombras, en texeles
    float shadowMinTexels() const;     // radio minimo (texeles) para proyectar sombra
    std::uint32_t farCascadeInterval() const;  // multiplica el turno de las cascadas 2 y 3

    // Suelo de renderScale() (renderScaleFloor; 0 = sin suelo). Solo se baja
    // de el si el frame lleva mas de 3 s por encima de 1.5 veces el objetivo
    // (una GPU que no llega ni de lejos), hasta que vuelva a subirla.
    void setRenderScaleFloor(float factor);
    bool renderScaleFloorLifted() const { return floor_lifted_; }

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
    void seed(float gpu_ms);
    // Escala de un nivel de RenderScale con el suelo (si no esta levantado).
    float scaleAt(std::uint8_t level) const;
    // Bajar RenderScale desde `level` cambia algo (no lo tapa el suelo).
    bool renderScaleStepUseful(std::uint8_t level) const;
    // Un paso de RenderScale cada 3 s como mucho (cada uno rehace los destinos).
    bool renderScaleRested() const;

    bool enabled_ = true;
    float target_fps_ = 60.0f;
    std::array<std::uint8_t, kLeverCount> levels_{};
    std::array<std::uint8_t, kLeverCount> start_levels_{};
    std::array<std::uint8_t, kLeverCount> caps_{};  // = maxLevel salvo setLevelCap
    void clampToCaps();

    float ema_ms_ = 0.0f;
    bool has_sample_ = false;
    float over_seconds_ = 0.0f;
    float under_seconds_ = 0.0f;
    float clock_ = 0.0f;
    // Tiron en curso: muestras seguidas muy por encima de la media.
    float spike_seconds_ = 0.0f;
    std::uint32_t spike_samples_ = 0;
    // Calentamiento: desde que se sembro la media.
    float seeded_at_ = 0.0f;
    std::uint32_t samples_since_seed_ = 0;
    // Suelo de la resolucion interna y si esta levantado.
    float render_scale_floor_ = 0.0f;
    float far_over_seconds_ = 0.0f;
    bool floor_lifted_ = false;
    float render_scale_changed_at_ = -1000.0f;
    float reset_at_ = -1000.0f;  // clock_ del ultimo reinicio entero

    // Ultimo tiempo medido de cada pasada (por nombre, solo las que importan).
    // `output`: lo que va despues del escalado (a la resolucion de salida: no
    // baja con la resolucion interna). `fixed`: lo que no depende de la
    // resolucion (compute del mundo, mapas de tamano fijo, subidas).
    // `sky_map`: la oclusion del cielo desde arriba (la quita la iluminacion
    // ligera).
    struct PassCosts {
        float geometry = 0, shadows = 0, local_shadows = 0, volumetric = 0, shafts = 0, lighting = 0,
              ssao = 0, reflections = 0, gi = 0, total = 0, camera_fx = 0, clouds = 0, output = 0, fixed = 0,
              sky_map = 0;
    } passes_;

    // Cambio en observacion: se mide lo que de verdad ahorro (o costo).
    struct Pending {
        bool active = false;
        Lever lever = Lever::Lod;
        std::uint8_t from = 0;
        std::uint8_t to = 0;
        float before_ms = 0.0f;
        float timer = 0.0f;
        std::uint32_t samples = 0;
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

// Palancas (mascara: bit = 1 << indice) de lo que `after` prende y el
// presupuesto puede apagar, o del LOD si pide bastante mas detalle (el error
// baja mas de un 25 %; antes bastaba un 1 % y la mezcla de volumenes
// reiniciaba el presupuesto en cada frame).
inline std::uint32_t postLeversRaised(const PostProcessSettings& before, const PostProcessSettings& after) {
    const auto on = [](bool was, bool now) { return now && !was; };
    const auto bit = [](Lever lever) { return 1u << static_cast<std::uint32_t>(lever); };
    std::uint32_t mask = 0;
    if (on(before.light_shafts, after.light_shafts) || on(before.volumetric_light, after.volumetric_light)) {
        mask |= bit(Lever::Volumetric);
    }
    if (on(before.contact_shadows, after.contact_shadows)) mask |= bit(Lever::ContactShadows);
    if (on(before.ambient_occlusion, after.ambient_occlusion)) mask |= bit(Lever::Ssao);
    if (on(before.reflections, after.reflections)) mask |= bit(Lever::Reflections);
    if (on(before.global_illumination, after.global_illumination)) mask |= bit(Lever::Gi);
    if (on(before.motion_blur, after.motion_blur) || on(before.depth_of_field, after.depth_of_field)) {
        mask |= bit(Lever::CameraFx);
    }
    if (on(before.lods, after.lods) || after.lod_pixel_error < before.lod_pixel_error * 0.75f) mask |= bit(Lever::Lod);
    return mask;
}

// `after` prende algo que las palancas del presupuesto pueden apagar o pide
// bastante mas detalle de LOD que `before`.
inline bool postQualityRaised(const PostProcessSettings& before, const PostProcessSettings& after) {
    return postLeversRaised(before, after) != 0;
}

}  // namespace cramion::gfx

#endif  // CRAMION_VK_FRAME_BUDGET_H
