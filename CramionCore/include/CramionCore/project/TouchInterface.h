#ifndef CRAMION_PROJECT_TOUCH_INTERFACE_H
#define CRAMION_PROJECT_TOUCH_INTERFACE_H

// Interfaz tactil del proyecto (como el "Default Touch Interface" de
// Unreal): joystick, zona para mirar y botones en pantalla que el juego usa
// en moviles. Se disena una vez en el editor (Archivo > Controles tactiles),
// se guarda en ProjectSettings/TouchInterface.json y el juego la carga sola;
// en Lua se muestra, oculta o cambia (Input.setTouchControls,
// Input.setTouchButton...).

#include <CramionDM/TouchControls.h>

#include <filesystem>
#include <string>

namespace cramion::project {

// Sin archivo (o danado): dm::defaultTouchLayout().
dm::TouchLayout loadTouchInterface(const std::filesystem::path& file);
bool saveTouchInterface(const std::filesystem::path& file, const dm::TouchLayout& layout);

inline std::filesystem::path touchInterfaceFile(const std::filesystem::path& settings_folder) {
    return settings_folder / "TouchInterface.json";
}

}  // namespace cramion::project

#endif  // CRAMION_PROJECT_TOUCH_INTERFACE_H
