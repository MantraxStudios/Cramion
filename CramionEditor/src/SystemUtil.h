#ifndef CRAMION_EDITOR_SYSTEM_UTIL_H
#define CRAMION_EDITOR_SYSTEM_UTIL_H

// Lo del sistema operativo que el editor usa en muchos sitios, para no
// repetir #if en cada uno: la carpeta del ejecutable, abrir un archivo o una
// web con la aplicacion del sistema y lanzar procesos. En Windows es Win32
// (ShellExecute, CreateProcess); en Linux, POSIX (fork/exec, xdg-open).

#include <filesystem>
#include <string>
#include <vector>

namespace cramion::editor::sys {

// Ruta completa de este ejecutable y su carpeta.
std::filesystem::path executablePath();
std::filesystem::path executableDir();

// Abre un archivo, carpeta o URL con la aplicacion del sistema (Explorador,
// navegador, gestor de archivos). No espera.
bool openWithSystem(const std::string& path_or_url);
bool openWithSystem(const std::filesystem::path& path);
// Muestra el archivo en el gestor de archivos (seleccionado si se puede).
bool revealInFileManager(const std::filesystem::path& path);

// Lanza un proceso sin esperarlo (desligado del editor). `args` sin el
// ejecutable. Devuelve el pid (0 si fallo).
unsigned long spawnDetached(const std::filesystem::path& exe, const std::vector<std::string>& args,
                            const std::filesystem::path& working_dir = {});

// Ejecuta y espera: devuelve el codigo de salida (-1 si no se pudo lanzar) y
// deja en `output` lo que escribio en stdout (y stderr si `merge_stderr`).
int runCapture(const std::vector<std::string>& argv, std::string& output, bool merge_stderr = false,
               const std::filesystem::path& working_dir = {});

// Busca un programa en el PATH; vacio si no esta.
std::filesystem::path findInPath(const std::string& program);

// Variable de entorno (vacio si no esta).
std::string env(const char* name);

}  // namespace cramion::editor::sys

#endif  // CRAMION_EDITOR_SYSTEM_UTIL_H
