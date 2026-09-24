#ifndef CRAMION_EDITOR_DIALOGS_H
#define CRAMION_EDITOR_DIALOGS_H

// Dialogos nativos de Windows (abrir, guardar, elegir carpeta). Devuelven una
// ruta vacia (o una lista vacia) si se cancela.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace cramion::editor::dialogs {

// `filter`: pares "descripcion\0patron\0" terminados en "\0" (como Win32).
std::filesystem::path openFile(HWND owner, const wchar_t* filter,
                               const std::filesystem::path& initial_folder = {});
std::vector<std::filesystem::path> openFiles(HWND owner, const wchar_t* filter,
                                             const std::filesystem::path& initial_folder = {});
std::filesystem::path saveFile(HWND owner, const wchar_t* filter, const wchar_t* default_extension,
                               const std::filesystem::path& initial_folder = {},
                               const std::wstring& default_name = {});
// Selector de carpetas moderno (IFileDialog con FOS_PICKFOLDERS).
std::filesystem::path pickFolder(HWND owner, const std::filesystem::path& initial_folder = {});

// Ruta a texto UTF-8 (para ImGui) y al reves.
std::string utf8(const std::filesystem::path& path);
std::filesystem::path fromUtf8(const std::string& text);

// Carpeta de documentos del usuario (proyectos por defecto).
std::filesystem::path documentsFolder();

}  // namespace cramion::editor::dialogs

#endif  // CRAMION_EDITOR_DIALOGS_H
