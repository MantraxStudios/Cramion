#ifndef CRAMION_CORE_GAMEPLAY_ACCESSIBILITY_H
#define CRAMION_CORE_GAMEPLAY_ACCESSIBILITY_H

// Opciones de accesibilidad del jugador (como las de The Last of Us / las
// Xbox Accessibility Guidelines). Son globales, las cambia el menu de
// opciones del juego (Lua: Accessibility.set{...}) y se guardan por jugador:
//
//   - Daltonismo: corrige (o simula) protanopia, deuteranopia, tritanopia y
//     acromatopsia en la imagen final.
//   - Tamano del texto de la interfaz y de los subtitulos (dialogos), con
//     fondo opaco opcional detras de los subtitulos.
//   - Reducir movimiento: sin motion blur, distorsion de lente ni aberracion,
//     y el temblor de camara escalado.
//   - Controles: la reasignacion la guardan las Input Actions (Lua:
//     Input.rebind / Input.saveBindings).

#include <CramionFX/core/Math.h>

#include <filesystem>
#include <string>

namespace cramion::gameplay {

struct AccessibilitySettings {
    int colorblind_mode = 0;          // 0 ninguno, 1 protanopia, 2 deuteranopia, 3 tritanopia, 4 acromatopsia
    float colorblind_strength = 1.0f;
    bool colorblind_correct = true;   // false = simular (para probar el juego)
    float text_scale = 1.0f;          // texto de la interfaz (0.5..3)
    float subtitle_scale = 1.0f;      // dialogos y subtitulos
    bool subtitle_background = false; // fondo opaco detras de los subtitulos
    bool reduce_motion = false;
    float camera_shake = 1.0f;        // 0 = sin temblores de camara
    bool high_contrast_ui = false;    // bordes y fondos de la UI mas marcados
};

AccessibilitySettings& accessibility();
const char* colorblindModeName(int mode);

// Archivo donde se guardan (lo elige la aplicacion: los datos del jugador).
void setAccessibilityFile(const std::filesystem::path& file);
const std::filesystem::path& accessibilityFile();
bool loadAccessibility();
bool saveAccessibility();

// --- Temblor de camara (como el Impulse de Cinemachine) ---
// Suma "trauma" (0..1): la camara principal tiembla con ruido suave y se
// calma en `duration` segundos. Lo escala camera_shake (0 = nada) y
// reduce_motion (a la mitad). Lua: Camera.shake(0.5, 0.4).
void addCameraShake(float intensity, float duration = 0.5f, float frequency = 18.0f);
void clearCameraShake();
// Avanza el tiempo y da el desplazamiento de este frame: posicion (m, en
// los ejes de la camara: x derecha, y arriba) y giro (grados: x cabeceo,
// y guinada, z alabeo). false si no tiembla.
bool cameraShakeOffset(float dt, core::Vec3& position, core::Vec3& rotation_degrees);

std::string accessibilityToJson(const AccessibilitySettings& s);
bool accessibilityFromJson(const std::string& text, AccessibilitySettings& s);

}  // namespace cramion::gameplay

#endif  // CRAMION_CORE_GAMEPLAY_ACCESSIBILITY_H
