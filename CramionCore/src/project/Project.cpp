#include "CramionCore/project/Project.h"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace cramion::project {

namespace {

constexpr const char* kProjectExtension = ".crproj";
constexpr std::size_t kMaxRecent = 20;

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path fromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

// JSON o nullopt si el archivo no existe o esta danado (nunca lanza).
std::optional<nlohmann::json> readJson(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    nlohmann::json json = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded()) {
        return std::nullopt;
    }
    return json;
}

// Escritura atomica: un corte a medias no deja el .crproj (ni la lista de
// recientes) vacio.
void writeJson(const std::filesystem::path& file, const nlohmann::json& json) {
    std::filesystem::path temporary = file;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("No se pudo escribir " + utf8(file));
        }
        out << json.dump(2) << "\n";
    }
    std::filesystem::rename(temporary, file);
}

std::filesystem::path hubFile() {
    PWSTR folder = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &folder))) {
        result = std::filesystem::path(folder) / "Cramion" / "hub.json";
    } else {
        result = std::filesystem::temp_directory_path() / "Cramion" / "hub.json";
    }
    CoTaskMemFree(folder);
    return result;
}

std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::vector<RecentProject> readRecent() {
    std::vector<RecentProject> recent;
    const auto json = readJson(hubFile());
    if (!json || !json->is_object() || !json->contains("recent") || !(*json)["recent"].is_array()) {
        return recent;
    }
    for (const nlohmann::json& entry : (*json)["recent"]) {
        if (!entry.is_object() || !entry.contains("file") || !entry["file"].is_string()) {
            continue;
        }
        RecentProject project{};
        project.file = fromUtf8(entry["file"].get<std::string>());
        project.name = entry.value("name", utf8(project.file.stem()));
        project.last_opened = entry.value("last_opened", std::int64_t{0});
        recent.push_back(std::move(project));
    }
    return recent;
}

void writeRecent(const std::vector<RecentProject>& recent) {
    nlohmann::json list = nlohmann::json::array();
    for (const RecentProject& project : recent) {
        list.push_back({{"name", project.name},
                        {"file", utf8(project.file)},
                        {"last_opened", project.last_opened}});
    }
    const std::filesystem::path file = hubFile();
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    try {
        writeJson(file, nlohmann::json{{"recent", list}});
    } catch (const std::exception& e) {
        std::cerr << "[Hub] " << e.what() << "\n";
    }
}

bool samePath(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code error;
    return std::filesystem::weakly_canonical(a, error) == std::filesystem::weakly_canonical(b, error);
}

}  // namespace

ProjectInfo createProject(const std::filesystem::path& parent, const std::string& name) {
    if (name.empty()) {
        throw std::runtime_error("El proyecto necesita un nombre.");
    }
    const std::filesystem::path folder = parent / fromUtf8(name);
    std::error_code error;
    if (std::filesystem::exists(folder, error) && !std::filesystem::is_empty(folder, error)) {
        throw std::runtime_error("La carpeta " + utf8(folder) + " ya existe y no esta vacia.");
    }

    ProjectInfo project{};
    project.name = name;
    project.folder = folder;
    project.file = folder / fromUtf8(name + kProjectExtension);

    for (const std::filesystem::path& sub :
         {project.assetsFolder() / "Scenes", project.libraryFolder() / "Cache",
          project.settingsFolder()}) {
        std::filesystem::create_directories(sub, error);
        if (error) {
            throw std::runtime_error("No se pudo crear " + utf8(sub) + ": " + error.message());
        }
    }
    saveProject(project);
    std::cout << "[Proyecto] Creado " << name << " en " << utf8(folder) << "\n";
    return project;
}

std::optional<ProjectInfo> openProject(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path file = path;
    // Se admite la carpeta: se busca su .crproj.
    if (std::filesystem::is_directory(path, error)) {
        file.clear();
        for (const auto& entry : std::filesystem::directory_iterator(path, error)) {
            if (entry.path().extension() == kProjectExtension) {
                file = entry.path();
                break;
            }
        }
        if (file.empty()) {
            return std::nullopt;
        }
    }

    const auto json = readJson(file);
    if (!json || !json->is_object()) {
        std::cerr << "[Proyecto] " << utf8(file) << " no es un proyecto valido\n";
        return std::nullopt;
    }

    ProjectInfo project{};
    project.file = std::filesystem::absolute(file, error);
    project.folder = project.file.parent_path();
    project.name = json->value("name", utf8(file.stem()));
    project.engine_version = json->value("engine_version", std::string("0.1"));
    project.startup_scene = Uuid::parse(json->value("startup_scene", std::string()));

    // Carpetas que falten (p. ej. Library borrada a mano): se recrean.
    for (const std::filesystem::path& sub :
         {project.assetsFolder(), project.libraryFolder() / "Cache", project.settingsFolder()}) {
        std::filesystem::create_directories(sub, error);
    }
    return project;
}

void saveProject(const ProjectInfo& project) {
    nlohmann::json json{
        {"name", project.name},
        {"engine_version", project.engine_version},
        {"startup_scene", project.startup_scene.valid() ? project.startup_scene.toString() : ""},
        {"format", 1},
    };
    writeJson(project.file, json);
}

std::vector<RecentProject> recentProjects() {
    std::vector<RecentProject> recent = readRecent();
    std::sort(recent.begin(), recent.end(), [](const RecentProject& a, const RecentProject& b) {
        return a.last_opened > b.last_opened;
    });
    return recent;
}

void addRecentProject(const ProjectInfo& project) {
    std::vector<RecentProject> recent = readRecent();
    std::erase_if(recent, [&](const RecentProject& r) { return samePath(r.file, project.file); });
    recent.insert(recent.begin(), RecentProject{project.name, project.file, now()});
    if (recent.size() > kMaxRecent) {
        recent.resize(kMaxRecent);
    }
    writeRecent(recent);
}

void removeRecentProject(const std::filesystem::path& file) {
    std::vector<RecentProject> recent = readRecent();
    std::erase_if(recent, [&](const RecentProject& r) { return samePath(r.file, file); });
    writeRecent(recent);
}

}  // namespace cramion::project
