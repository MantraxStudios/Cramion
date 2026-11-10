// PAQUETES-TEXTURAS: paquetes de texturas de bloques del jugador (solo visuales, del cliente).
//
// Cada paquete es una carpeta dentro de una de las raices (setTexturePackRoots; el juego pone primero la del
// jugador: %LOCALAPPDATA%/Cramion/Saves/<juego>/TexturePacks) con PNG que se llaman como la textura del bloque
// (textureNames(): grass_top.png, stone.png...) y un pack.json opcional:
//   {"name": "...", "author": "...", "version": "1.0", "description": "...", "icon": "pack.png"}
// El paquete activo es global (como los CVars): VoxelSystem::syncRenderer mira texturePackGeneration() y rehace
// el array de texturas en segundo plano (buildTexture con la carpeta del paquete encima de la del juego).

#include "CramionCore/voxel/Voxel.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

namespace cramion::voxel {
namespace {

struct PackState {
    std::mutex mutex;
    std::vector<std::filesystem::path> roots;
    std::string active;
    std::filesystem::path active_folder;
};

PackState& state() {
    static PackState s;
    return s;
}

std::atomic<std::uint64_t> g_generation{0};

std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string lowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// Lee pack.json (si lo hay) y cuenta los PNG que sustituyen a una textura de bloque.
TexturePackInfo readPack(const std::filesystem::path& folder) {
    TexturePackInfo info;
    info.id = utf8(folder.filename());
    info.name = info.id;
    info.folder = folder;
    std::error_code error;
    const std::filesystem::path json_file = folder / "pack.json";
    if (std::filesystem::exists(json_file, error)) {
        std::ifstream in(json_file, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        const nlohmann::json j = nlohmann::json::parse(text.str(), nullptr, false);
        if (j.is_object()) {
            const auto str = [&](const char* key, std::string& out) {
                if (const auto it = j.find(key); it != j.end() && it->is_string()) out = it->get<std::string>();
            };
            str("name", info.name);
            str("author", info.author);
            if (const auto it = j.find("version"); it != j.end()) {
                if (it->is_string()) info.version = it->get<std::string>();
                else if (it->is_number()) info.version = it->dump();
            }
            str("description", info.description);
            std::string icon;
            str("icon", icon);
            if (!icon.empty() && icon.find("..") == std::string::npos) {
                const std::filesystem::path p = folder / std::filesystem::path(std::u8string(icon.begin(), icon.end()));
                if (std::filesystem::exists(p, error)) info.icon = p;
            }
        }
    }
    if (info.icon.empty() && std::filesystem::exists(folder / "pack.png", error)) info.icon = folder / "pack.png";
    std::set<std::string> names;
    for (const std::string& n : textureNames()) names.insert(n);
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (!entry.is_regular_file(error)) continue;
        const std::filesystem::path& p = entry.path();
        if (lowerAscii(utf8(p.extension())) != ".png") continue;
        if (names.count(utf8(p.stem())) != 0) ++info.textures;
    }
    return info;
}

}  // namespace

void setTexturePackRoots(std::vector<std::filesystem::path> roots) {
    PackState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.roots = std::move(roots);
}

std::vector<std::filesystem::path> texturePackRoots() {
    PackState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.roots;
}

std::vector<TexturePackInfo> listTexturePacks() {
    const std::vector<std::filesystem::path> roots = texturePackRoots();
    std::vector<TexturePackInfo> out;
    std::set<std::string> seen;
    std::error_code error;
    // La del jugador se crea, para que sepa donde dejar los suyos.
    if (!roots.empty() && !roots.front().empty()) std::filesystem::create_directories(roots.front(), error);
    for (const std::filesystem::path& root : roots) {
        if (root.empty() || !std::filesystem::is_directory(root, error)) continue;
        std::vector<std::filesystem::path> folders;
        for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
            if (entry.is_directory(error)) folders.push_back(entry.path());
        }
        std::sort(folders.begin(), folders.end());
        for (const std::filesystem::path& folder : folders) {
            const std::string id = utf8(folder.filename());
            if (id.empty() || id[0] == '.' || seen.count(id) != 0) continue;  // la primera raiz manda
            seen.insert(id);
            out.push_back(readPack(folder));
        }
    }
    return out;
}

bool setTexturePack(const std::string& id) {
    std::filesystem::path folder;
    if (!id.empty()) {
        for (const TexturePackInfo& p : listTexturePacks()) {
            if (p.id == id) {
                folder = p.folder;
                break;
            }
        }
        if (folder.empty()) return false;
    }
    PackState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.active == id && s.active_folder == folder) return true;
    s.active = id;
    s.active_folder = folder;
    g_generation.fetch_add(1);
    return true;
}

std::string texturePack() {
    PackState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.active;
}

std::filesystem::path texturePackFolder() {
    PackState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.active_folder;
}

std::uint64_t texturePackGeneration() { return g_generation.load(); }

void reloadTextures() { g_generation.fetch_add(1); }

}  // namespace cramion::voxel
