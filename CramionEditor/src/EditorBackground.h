#ifndef CRAMION_EDITOR_EDITOR_BACKGROUND_H
#define CRAMION_EDITOR_EDITOR_BACKGROUND_H

// Ritmo del editor en segundo plano (Editar > Segundo plano). Antes era fijo
// en main.cpp: 10 fps con Sleep (que en Windows redondea a ~15 ms y daba unos
// 5-8 fps reales) y la escena cada 2 s. Ahora:
//   - Ahorro (por defecto): la interfaz a `fps` (10) y la escena cada 2 s.
//   - Vista en vivo: interfaz y escena a `fps` (para mirar la Escena o el
//     Juego en otro monitor mientras se trabaja en otra aplicacion).
//   - Sin limite: como en primer plano.
// Oculta (minimizada, en otro escritorio virtual o tapada entera por una
// ventana de otro programa, como el juego exportado a pantalla completa) no
// dibuja la escena en ningun modo salvo Sin limite: no se ve y gastaria la GPU
// del juego. La espera es con un temporizador de alta resolucion (como el
// limitador del player), asi que los fps pedidos son los que salen.
// Preferencias del PC en %LOCALAPPDATA%/Cramion/Settings/EditorBackground.ini.

#include <chrono>

namespace cramion::editor {

enum class BackgroundMode : int { Saver = 0, Live = 1, Unlimited = 2 };

struct BackgroundPreferences {
    BackgroundMode mode = BackgroundMode::Saver;
    int fps = 10;  // 1..60
};

// Las de este PC (se leen la primera vez). saveBackgroundPreferences las guarda.
BackgroundPreferences& backgroundPreferences();
void saveBackgroundPreferences();

// Estado de la ventana principal respecto al usuario.
enum class WindowPresence { Foreground, Background, Hidden };
// `main_window` es el HWND de la ventana principal (void* para no meter
// windows.h aqui).
WindowPresence windowPresence(void* main_window);

// Lo que hace este frame con el modo y la presencia: si puede dibujar la
// escena y cuanto debe durar el frame (0 = sin esperar).
struct BackgroundDecision {
    bool throttle = false;    // frame limitado (no esta en primer plano)
    bool draw_scene = true;   // dibujar la escena (si no, solo interfaz y mensajes)
    float frame_ms = 0.0f;    // duracion objetivo del frame
};
// `allowed`: el editor permite limitar (no en Play, casco, cine, pruebas ni
// cargando). `busy`: hay que dibujar la escena aunque este detras (una IA
// por MCP: sus capturas necesitan frames).
// `ms_since_scene`: tiempo desde la ultima vez que se dibujo la escena.
BackgroundDecision decideBackground(WindowPresence presence, bool allowed, bool busy, float ms_since_scene);

// Espera hasta `deadline` con un temporizador de alta resolucion (Windows 10
// 1803+; si no, sleep_for) y los ultimos ~0,5 ms cediendo el hilo.
void waitUntil(std::chrono::steady_clock::time_point deadline);

// Submenu Editar > Segundo plano (ImGui). Guarda al cambiar.
void drawBackgroundMenu();

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_EDITOR_BACKGROUND_H
