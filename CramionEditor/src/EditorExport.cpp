// Exportar el juego (Archivo > Exportar juego): una carpeta que se puede
// copiar a otro PC y jugar, como el Build de Unity:
//
//   <Carpeta>/<Proyecto>/
//     <Proyecto>.exe   CramionPlayer (banner del motor, escena inicial)
//     shaders/         del motor
//     *.dll            las que haya junto al editor
//     Game/            el proyecto sin Library (caches), banner.png y game.ini

#include "EditorApp.h"

#include "Dialogs.h"

#include <fstream>
#include <iostream>

#include <shellapi.h>

namespace cramion::editor {

namespace {

std::filesystem::path editorFolder() {
    wchar_t buffer[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path();
}

std::string safeFolderName(std::string name) {
    for (char& c : name) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') c = '_';
    }
    return name.empty() ? std::string("Juego") : name;
}

}  // namespace

std::filesystem::path EditorApp::exportGame(bool run_after) {
    if (!has_project_) return {};
    if (playing()) {
        std::cerr << "[Exportar] Sal del modo Play antes de exportar\n";
        return {};
    }
    // Lo que no esta guardado no llegaria al juego.
    if (dirty_ || scene_path_.empty()) {
        if (!saveScene()) return {};
    }
    const std::filesystem::path parent = dialogs::pickFolder(window_.handle(), project_.folder.parent_path());
    if (parent.empty()) return {};
    const std::filesystem::path target = parent / dialogs::fromUtf8(safeFolderName(project_.name));
    const std::filesystem::path source = editorFolder();
    std::error_code error;
    std::filesystem::create_directories(target, error);

    const auto copy_file = [&](const std::filesystem::path& from, const std::filesystem::path& to) {
        std::error_code e;
        std::filesystem::create_directories(to.parent_path(), e);
        std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, e);
        if (e) std::cerr << "[Exportar] No se pudo copiar " << dialogs::utf8(from) << ": " << e.message() << "\n";
        return !e;
    };
    const auto copy_folder = [&](const std::filesystem::path& from, const std::filesystem::path& to) {
        std::error_code e;
        std::filesystem::create_directories(to, e);
        std::filesystem::copy(from, to, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, e);
        if (e) std::cerr << "[Exportar] No se pudo copiar " << dialogs::utf8(from) << ": " << e.message() << "\n";
        return !e;
    };

    // --- El programa ---
    const std::filesystem::path exe = target / dialogs::fromUtf8(safeFolderName(project_.name) + ".exe");
    if (!copy_file(source / "CramionPlayer.exe", exe)) {
        std::cerr << "[Exportar] Falta CramionPlayer.exe junto al editor (compila el proyecto)\n";
        return {};
    }
    copy_folder(source / "shaders", target / "shaders");
    for (const auto& entry : std::filesystem::directory_iterator(source, error)) {
        if (entry.is_regular_file(error) && entry.path().extension() == ".dll") copy_file(entry.path(), target / entry.path().filename());
    }

    // --- El proyecto (sin caches), limpio ---
    const std::filesystem::path game = target / "Game";
    std::filesystem::remove_all(game, error);
    std::filesystem::create_directories(game, error);
    copy_file(project_.file, game / project_.file.filename());
    copy_folder(project_.assetsFolder(), game / "Assets");
    copy_folder(project_.settingsFolder(), game / "ProjectSettings");
    copy_file(source / "player_banner.png", game / "banner.png");

    // Escena inicial: la del proyecto o la que esta abierta.
    std::filesystem::path first_scene = scene_path_;
    if (project_.startup_scene.valid()) {
        if (const auto info = database_->find(project_.startup_scene)) first_scene = info->path;
    }
    std::ofstream(game / "game.ini") << "scene=" << assetRelative(first_scene) << "\n";

    std::cout << "[Exportar] Juego exportado en " << dialogs::utf8(target) << " (escena inicial: "
              << dialogs::utf8(first_scene.filename()) << ")\n";
    if (run_after) {
        ShellExecuteW(nullptr, L"open", exe.wstring().c_str(), nullptr, target.wstring().c_str(), SW_SHOWNORMAL);
    } else {
        ShellExecuteW(nullptr, L"open", target.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    return target;
}

}  // namespace cramion::editor
