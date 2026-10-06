#ifndef CRAMION_CORE_PROJECT_MODS_H
#define CRAMION_CORE_PROJECT_MODS_H

// Mods (como los de Skyrim / Minecraft / los Workshop de Steam): contenido y
// scripts de los jugadores que el juego carga al arrancar si la configuracion
// de compilacion lo permite ("Permitir mods": mods=1 en game.ini).
//
// Donde se buscan: <juego>/Mods/ y %LOCALAPPDATA%/Cramion/Mods/<juego>/. Un mod
// es una carpeta con:
//
//   mod.json     {"name": "Mas coches", "version": "1.2", "author": "...",
//                 "description": "...", "entry": "main.lua", "load_order": 10}
//   Assets/      modelos, texturas, prefabs, escenas, scripts... (se montan en
//                Assets/Mods/<carpeta>/ sin pisar nada del juego)
//   main.lua     se ejecuta tras empezar la escena (puede crear objetos,
//                escuchar eventos, anadir menus...)
//
// o un .datapack suelto (DataPack exportado desde el editor). Se cargan por
// load_order (y por nombre). El jugador activa o desactiva cada uno con
// Mods.setEnabled (se guarda en mods.json de sus datos).

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cramion::project {

inline constexpr const char* kModManifest = "mod.json";

struct ModInfo {
    std::string id;           // nombre de la carpeta (o del .datapack sin extension)
    std::string name;
    std::string version = "1.0";
    std::string author;
    std::string description;
    std::string entry;        // script de Lua dentro del mod (relativo a la carpeta)
    int load_order = 0;
    bool enabled = true;
    std::filesystem::path folder;    // carpeta del mod (vacia si es un .datapack suelto)
    std::filesystem::path datapack;  // .datapack (suelto o dentro de la carpeta)
    bool has_assets = false;
};

struct ModMount {
    std::string id;
    std::vector<std::filesystem::path> written;  // lo copiado (se borra al desmontar)
    std::string entry_asset;  // el script de entrada dentro de Assets ("Mods/X/main.lua"), vacio si no hay
};

class ModManager {
public:
    // Carpetas donde buscar (en orden) y archivo con los activados del jugador.
    void setSearchFolders(std::vector<std::filesystem::path> folders) { folders_ = std::move(folders); }
    void setStateFile(const std::filesystem::path& file) { state_file_ = file; }

    // Busca los mods (lee mod.json y el estado guardado).
    const std::vector<ModInfo>& scan();
    const std::vector<ModInfo>& mods() const { return mods_; }
    bool setEnabled(const std::string& id, bool enabled);
    bool saveState() const;

    // Monta los activados en assets_root (Assets/Mods/<id>/ y los DataPacks).
    // `journal` apunta lo escrito para limpiarlo si el juego se cierra de golpe.
    int mountEnabled(const std::filesystem::path& assets_root, const std::filesystem::path& journal,
                     std::vector<std::string>* errors = nullptr);
    void unmountAll(const std::filesystem::path& assets_root, const std::filesystem::path& journal);
    const std::vector<ModMount>& mounted() const { return mounts_; }

    // Crea la plantilla de un mod (mod.json, Assets/ y main.lua de ejemplo).
    static bool createTemplate(const std::filesystem::path& folder, const std::string& name, std::string* error = nullptr);

private:
    std::vector<std::filesystem::path> folders_;
    std::filesystem::path state_file_;
    std::vector<ModInfo> mods_;
    std::vector<ModMount> mounts_;
    std::vector<std::filesystem::path> datapack_written_;
};

// El gestor del juego (Lua: Mods.list...). nullptr si el juego no usa mods.
ModManager* activeMods();
void setActiveMods(ModManager* manager);

}  // namespace cramion::project

#endif  // CRAMION_CORE_PROJECT_MODS_H
