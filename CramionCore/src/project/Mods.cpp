#include "CramionCore/project/Mods.h"

#include "CramionCore/project/DataPack.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

namespace cramion::project {

namespace {

ModManager* g_active = nullptr;

std::string readText(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::filesystem::path path8(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

void appendJournal(const std::filesystem::path& journal, const std::filesystem::path& file) {
    if (journal.empty()) return;
    std::ofstream out(journal, std::ios::app | std::ios::binary);
    out << utf8(file) << "\n";
}

}  // namespace

ModManager* activeMods() { return g_active; }
void setActiveMods(ModManager* manager) { g_active = manager; }

const std::vector<ModInfo>& ModManager::scan() {
    mods_.clear();
    std::set<std::string> seen;
    // Estado del jugador: {"disabled": ["id", ...]}
    std::set<std::string> disabled;
    if (!state_file_.empty()) {
        const nlohmann::json j = nlohmann::json::parse(readText(state_file_), nullptr, false);
        if (j.is_object() && j.contains("disabled") && j["disabled"].is_array()) {
            for (const auto& v : j["disabled"]) {
                if (v.is_string()) disabled.insert(v.get<std::string>());
            }
        }
    }
    for (const std::filesystem::path& root : folders_) {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) continue;
        for (std::filesystem::directory_iterator it(root, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            const std::filesystem::path p = it->path();
            ModInfo m;
            if (it->is_directory(ec)) {
                m.id = utf8(p.filename());
                m.folder = p;
                m.name = m.id;
                const std::filesystem::path manifest = p / kModManifest;
                if (std::filesystem::exists(manifest, ec)) {
                    const nlohmann::json j = nlohmann::json::parse(readText(manifest), nullptr, false);
                    if (j.is_object()) {
                        m.name = j.value("name", m.name);
                        m.version = j.value("version", m.version);
                        m.author = j.value("author", std::string());
                        m.description = j.value("description", std::string());
                        m.entry = j.value("entry", std::string());
                        m.load_order = j.value("load_order", 0);
                    } else {
                        std::cerr << "[Mods] " << m.id << ": mod.json no es valido\n";
                    }
                }
                if (m.entry.empty() && std::filesystem::exists(p / "main.lua", ec)) m.entry = "main.lua";
                m.has_assets = std::filesystem::is_directory(p / "Assets", ec);
                for (std::filesystem::directory_iterator f(p, ec); !ec && f != std::filesystem::directory_iterator(); f.increment(ec)) {
                    if (f->path().extension() == kDataPackExtension) m.datapack = f->path();
                }
                if (!m.has_assets && m.datapack.empty() && m.entry.empty()) continue;  // carpeta sin nada
            } else if (p.extension() == kDataPackExtension) {
                m.id = utf8(p.stem());
                m.datapack = p;
                DataPackManifest dm;
                std::string error;
                m.name = readDataPackManifest(p, dm, &error) && !dm.name.empty() ? dm.name : m.id;
                m.version = dm.version;
            } else {
                continue;
            }
            if (!seen.insert(m.id).second) continue;  // el primero que se encuentra
            m.enabled = disabled.count(m.id) == 0;
            mods_.push_back(std::move(m));
        }
    }
    std::sort(mods_.begin(), mods_.end(), [](const ModInfo& a, const ModInfo& b) {
        return a.load_order != b.load_order ? a.load_order < b.load_order : a.name < b.name;
    });
    return mods_;
}

bool ModManager::setEnabled(const std::string& id, bool enabled) {
    for (ModInfo& m : mods_) {
        if (m.id == id) {
            m.enabled = enabled;
            return true;
        }
    }
    return false;
}

bool ModManager::saveState() const {
    if (state_file_.empty()) return false;
    nlohmann::json j;
    j["disabled"] = nlohmann::json::array();
    for (const ModInfo& m : mods_) {
        if (!m.enabled) j["disabled"].push_back(m.id);
    }
    std::error_code ec;
    std::filesystem::create_directories(state_file_.parent_path(), ec);
    std::ofstream out(state_file_, std::ios::binary);
    out << j.dump(2);
    return static_cast<bool>(out);
}

int ModManager::mountEnabled(const std::filesystem::path& assets_root, const std::filesystem::path& journal,
                             std::vector<std::string>* errors) {
    int count = 0;
    for (const ModInfo& m : mods_) {
        if (!m.enabled) continue;
        ModMount mount;
        mount.id = m.id;
        std::error_code ec;
        const std::filesystem::path target = assets_root / "Mods" / path8(m.id);
        // Assets/ del mod -> Assets/Mods/<id>/ (sin pisar nada).
        if (m.has_assets) {
            const std::filesystem::path source = m.folder / "Assets";
            for (std::filesystem::recursive_directory_iterator it(source, ec); !ec && it != std::filesystem::recursive_directory_iterator();
                 it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                const std::filesystem::path rel = it->path().lexically_relative(source);
                const std::filesystem::path dst = target / rel;
                if (std::filesystem::exists(dst, ec)) continue;
                std::filesystem::create_directories(dst.parent_path(), ec);
                if (std::filesystem::copy_file(it->path(), dst, ec)) {
                    mount.written.push_back(dst);
                    appendJournal(journal, dst);
                }
            }
        }
        // El script de entrada va con los assets (asi lo encuentra Lua).
        if (!m.folder.empty() && !m.entry.empty()) {
            const std::filesystem::path src = m.folder / path8(m.entry);
            const std::filesystem::path dst = target / path8(m.entry);
            if (std::filesystem::exists(src, ec)) {
                std::filesystem::create_directories(dst.parent_path(), ec);
                if (!std::filesystem::exists(dst, ec) && std::filesystem::copy_file(src, dst, ec)) {
                    mount.written.push_back(dst);
                    appendJournal(journal, dst);
                }
                mount.entry_asset = utf8(std::filesystem::path("Mods") / path8(m.id) / path8(m.entry));
                std::replace(mount.entry_asset.begin(), mount.entry_asset.end(), '\\', '/');
            } else if (errors != nullptr) {
                errors->push_back(m.name + ": no existe " + m.entry);
            }
        }
        if (!m.datapack.empty()) {
            DataPackMount dpm;
            std::string error;
            if (mountDataPack(m.datapack, assets_root, dpm, &error, journal)) {
                mount.written.insert(mount.written.end(), dpm.written.begin(), dpm.written.end());
            } else if (errors != nullptr) {
                errors->push_back(m.name + ": " + error);
            }
        }
        std::cout << "[Mods] " << m.name << " " << m.version << " (" << mount.written.size() << " archivos)\n";
        mounts_.push_back(std::move(mount));
        ++count;
    }
    return count;
}

void ModManager::unmountAll(const std::filesystem::path& assets_root, const std::filesystem::path& journal) {
    std::error_code ec;
    for (const ModMount& m : mounts_) {
        for (const std::filesystem::path& f : m.written) std::filesystem::remove(f, ec);
        // Las carpetas vacias que quedan.
        const std::filesystem::path dir = assets_root / "Mods" / path8(m.id);
        for (int i = 0; i < 8 && std::filesystem::exists(dir, ec); ++i) {
            bool removed = false;
            for (std::filesystem::recursive_directory_iterator it(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator();
                 it.increment(ec)) {
                if (it->is_directory(ec) && std::filesystem::is_empty(it->path(), ec)) {
                    std::filesystem::remove(it->path(), ec);
                    removed = true;
                    break;
                }
            }
            if (!removed) break;
        }
        if (std::filesystem::is_empty(dir, ec)) std::filesystem::remove(dir, ec);
    }
    mounts_.clear();
    if (!journal.empty()) std::filesystem::remove(journal, ec);
}

bool ModManager::createTemplate(const std::filesystem::path& folder, const std::string& name, std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(folder / "Assets", ec);
    if (ec) {
        if (error != nullptr) *error = ec.message();
        return false;
    }
    nlohmann::json j;
    j["name"] = name;
    j["version"] = "1.0";
    j["author"] = "";
    j["description"] = "Mi mod";
    j["entry"] = "main.lua";
    j["load_order"] = 0;
    std::ofstream(folder / kModManifest, std::ios::binary) << j.dump(2);
    std::ofstream(folder / "main.lua", std::ios::binary)
        << "-- Se ejecuta al empezar cada escena (los assets del mod estan en Assets/Mods/<carpeta>/).\n"
           "print(\"Mod cargado: " << name << "\")\n"
           "-- local coche = Scene.instantiate(\"Mods/MiMod/Coche.crprefab\", Vec3(0, 1, 0))\n";
    return true;
}

}  // namespace cramion::project
