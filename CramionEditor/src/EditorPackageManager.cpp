// Package Manager (Ventana > Package Manager), como el de Unity: los paquetes
// de assets de la Cramion Store (https://cramion.mantraxtools.store/store/).
//
// - "En el proyecto": lo instalado, con su version, y si hay una mas nueva.
// - "Cramion Store": el catalogo (busqueda y categorias).
// - Cada paquete se puede instalar, actualizar, bajar a una version anterior
//   o quitar. Las dependencias del manifiesto se instalan solas.
// - Instalar desde un .crwpack del disco y exportar una carpeta como .crwpack
//   (para subirlo luego en la web).
//
// Donde va cada cosa:
//   Assets/Packages/<nombre>/                 los assets del paquete
//   ProjectSettings/Packages.json             lo instalado (nombre -> version)
//   Library/PackageCache/<nombre>@<version>   el .crwpack descargado y su
//                                             contenido (bajar o volver a una
//                                             version ya usada no descarga nada)
//
// Lo de red y disco va en hilos aparte; lo que toca el proyecto (copiar a
// Assets, refrescar la base de datos) se encola y se hace en el hilo principal.

#include "EditorApp.h"

#include "Dialogs.h"
#include "LoadingScreen.h"
#include "Theme.h"

#include "CramionCore/crwpack/CrwPack.h"
#include "CramionCore/net/Http.h"

#include <CramionUpdater/Update.h>

#include <imgui.h>
#include <imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace cramion::editor {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr const char* kDefaultRegistry = "https://cramion.mantraxtools.store/api";
constexpr const char* kStoreWeb = "https://cramion.mantraxtools.store/store/";

struct RemoteVersion {
    std::string version, sha256, changelog, engine;
    std::uint64_t size = 0, raw_size = 0, downloads = 0;
    std::int64_t created = 0;
    bool yanked = false;
    std::map<std::string, std::string> dependencies;
};

struct RemotePackage {
    std::string name, display_name, description, category, license, author, repo_url;
    std::vector<std::string> tags;
    std::uint64_t downloads = 0;
    bool featured = false;
    RemoteVersion latest;
    bool has_latest = false;
    std::vector<RemoteVersion> versions;  // solo en el detalle, de la mas nueva a la mas vieja
    bool has_versions = false;
};

struct InstalledPackage {
    std::string name, version, display_name, sha256, source, installed;
    std::map<std::string, std::string> dependencies;
};

std::string str(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::uint64_t num(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<std::uint64_t>() : 0;
}

std::map<std::string, std::string> deps(const json& j) {
    std::map<std::string, std::string> out;
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) out[it.key()] = it->is_string() ? it->get<std::string>() : "latest";
    }
    return out;
}

RemoteVersion parseVersion(const json& j) {
    RemoteVersion v;
    v.version = str(j, "version");
    v.sha256 = str(j, "sha256");
    v.changelog = str(j, "changelog");
    v.engine = str(j, "engine");
    v.size = num(j, "size");
    v.raw_size = num(j, "raw_size");
    v.downloads = num(j, "downloads");
    v.created = static_cast<std::int64_t>(num(j, "created_at"));
    v.yanked = j.value("yanked", false);
    if (j.contains("dependencies")) v.dependencies = deps(j["dependencies"]);
    return v;
}

RemotePackage parsePackage(const json& j) {
    RemotePackage p;
    p.name = str(j, "name");
    p.display_name = str(j, "display_name");
    p.description = str(j, "description");
    p.category = str(j, "category");
    p.license = str(j, "license");
    p.repo_url = str(j, "repo_url");
    p.downloads = num(j, "downloads");
    p.featured = j.value("featured", false);
    if (j.contains("author") && j["author"].is_object()) p.author = str(j["author"], "display_name");
    if (j.contains("tags") && j["tags"].is_array()) {
        for (const json& t : j["tags"]) {
            if (t.is_string()) p.tags.push_back(t.get<std::string>());
        }
    }
    if (j.contains("latest") && j["latest"].is_object()) {
        p.latest = parseVersion(j["latest"]);
        p.has_latest = true;
    }
    if (j.contains("versions") && j["versions"].is_array()) {
        for (const json& v : j["versions"]) p.versions.push_back(parseVersion(v));
        p.has_versions = true;
    }
    return p;
}

std::string isoNow() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return text;
}

std::string dateText(std::int64_t unix_seconds) {
    if (unix_seconds <= 0) return {};
    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%d/%m/%Y", &tm);
    return text;
}

// "1.2.0", ">=1.2.0", "^1.2.0", "~1.2.0" -> la version minima; "*"/"latest" -> "".
std::string minimumVersion(std::string spec) {
    while (!spec.empty() && (spec.front() == '>' || spec.front() == '=' || spec.front() == '^' || spec.front() == '~' ||
                             spec.front() == ' ')) {
        spec.erase(spec.begin());
    }
    return crwpack::validVersion(spec) ? spec : std::string();
}

// Llamada GET a la API. Los errores de la API vienen como {"error": "..."}.
bool apiGet(const std::string& registry, const std::string& path, json& out, std::string& error,
            const std::atomic<bool>* cancel) {
    net::HttpRequest request;
    request.url = registry + path;
    request.headers = {{"Accept", "application/json"}};
    request.timeout_ms = 20000;
    const net::HttpResponse response = net::httpRequest(request, cancel);
    if (!response.ok) {
        const json body = json::parse(response.body, nullptr, false);
        if (body.is_object() && body.contains("error") && body["error"].is_string()) {
            error = body["error"].get<std::string>();
        } else if (!response.error.empty()) {
            error = response.error;
        } else {
            error = "El servidor respondio " + std::to_string(response.status);
        }
        return false;
    }
    out = json::parse(response.body, nullptr, false);
    if (out.is_discarded() || !out.is_object()) {
        error = "Respuesta no valida del servidor";
        return false;
    }
    return true;
}

}  // namespace

// =============================================================================

// Un paquete listo para copiar a Assets (en el hilo principal).
struct PackageStaged {
    InstalledPackage info;
    fs::path content;  // Library/PackageCache/<nombre>@<version>/
};

struct PackageManagerState {
    std::string registry = kDefaultRegistry;

    std::mutex mutex;  // protege lo marcado con (*)
    // Catalogo (*)
    std::vector<RemotePackage> catalog;
    int catalog_total = 0;
    bool catalog_loading = false;
    bool catalog_requested = false;
    std::string catalog_error;
    std::vector<std::pair<std::string, std::string>> categories;  // id, nombre
    // Detalle con todas las versiones (*)
    std::map<std::string, RemotePackage> details;
    std::set<std::string> details_loading;
    std::map<std::string, std::string> detail_errors;
    // Tarea en curso (*)
    std::string status;
    std::vector<std::function<void(EditorApp&)>> main_actions;

    // Instalados (hilo principal)
    fs::path project_folder;
    std::map<std::string, InstalledPackage> installed;
    bool update_check_done = false;
    // Lo ultimo que se instalo o quito y el ultimo error (para el MCP: package_state).
    std::string last_result, last_error;

    // Interfaz (hilo principal)
    int tab = 0;  // 0 en el proyecto, 1 store
    std::string search, last_search = "\x01";
    int category = 0, last_category = -1;
    int sort = 0, last_sort = -1;
    std::string selected;
    std::string selected_version;
    bool settings_open = false;
    std::string registry_edit;

    // Exportar
    bool export_open = false;
    std::string ex_folder, ex_name, ex_version = "1.0.0", ex_display, ex_description, ex_author, ex_deps, ex_changelog;
    int ex_level = 1;
    std::string ex_result;

    // Tareas (instalar, exportar)
    std::atomic<bool> busy{false};
    std::atomic<bool> cancel{false};
    std::atomic<bool> shutting_down{false};
    std::atomic<std::uint64_t> done{0}, total{0};

    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };
    std::vector<Worker> workers;

    ~PackageManagerState() {
        shutting_down = true;
        cancel = true;
        for (Worker& w : workers) {
            if (w.thread.joinable()) w.thread.join();
        }
    }

    template <class F>
    void run(F&& job) {
        auto finished = std::make_shared<std::atomic<bool>>(false);
        workers.push_back({std::thread([job = std::forward<F>(job), finished]() mutable {
                               job();
                               *finished = true;
                           }),
                           finished});
    }

    void reapWorkers() {
        for (auto it = workers.begin(); it != workers.end();) {
            if (*it->finished) {
                it->thread.join();
                it = workers.erase(it);
            } else {
                ++it;
            }
        }
    }

    void post(std::function<void(EditorApp&)> action) {
        std::lock_guard lock(mutex);
        main_actions.push_back(std::move(action));
    }

    void setStatus(const std::string& text) {
        std::lock_guard lock(mutex);
        status = text;
    }

    fs::path settingsFile() const { return localDataFolder("Settings") / "PackageManager.json"; }

    void loadSettings() {
        std::ifstream in(settingsFile());
        const json j = json::parse(in, nullptr, false);
        if (j.is_object() && j.contains("registry") && j["registry"].is_string() && !j["registry"].get<std::string>().empty()) {
            registry = j["registry"].get<std::string>();
        }
    }

    void saveSettings() const {
        std::ofstream out(settingsFile(), std::ios::trunc);
        out << json{{"registry", registry}}.dump(2);
    }
};

PackageManagerState& EditorApp::packageManager() {
    if (!package_manager_) {
        package_manager_ = std::make_shared<PackageManagerState>();
        package_manager_->loadSettings();
    }
    return *package_manager_;
}

namespace {

fs::path lockFile(const fs::path& project) { return project / "ProjectSettings" / "Packages.json"; }

std::map<std::string, InstalledPackage> loadInstalled(const fs::path& project) {
    std::map<std::string, InstalledPackage> out;
    std::ifstream in(lockFile(project));
    const json j = json::parse(in, nullptr, false);
    if (!j.is_object() || !j.contains("packages") || !j["packages"].is_object()) return out;
    for (auto it = j["packages"].begin(); it != j["packages"].end(); ++it) {
        if (!it->is_object()) continue;
        InstalledPackage p;
        p.name = it.key();
        p.version = str(*it, "version");
        p.display_name = str(*it, "displayName");
        p.sha256 = str(*it, "sha256");
        p.source = str(*it, "source");
        p.installed = str(*it, "installed");
        if (it->contains("dependencies")) p.dependencies = deps((*it)["dependencies"]);
        if (crwpack::validName(p.name)) out[p.name] = p;
    }
    return out;
}

void saveInstalled(const fs::path& project, const std::map<std::string, InstalledPackage>& installed,
                   const std::string& registry) {
    json packages = json::object();
    for (const auto& [name, p] : installed) {
        packages[name] = {{"version", p.version},     {"displayName", p.display_name}, {"sha256", p.sha256},
                          {"source", p.source},       {"installed", p.installed},      {"dependencies", p.dependencies}};
    }
    std::error_code ec;
    fs::create_directories(lockFile(project).parent_path(), ec);
    std::ofstream out(lockFile(project), std::ios::trunc);
    out << json{{"registry", registry}, {"packages", packages}}.dump(2);
}

fs::path cacheFolder(const fs::path& project) { return project / "Library" / "PackageCache"; }

// Lo instalado del proyecto abierto (se relee al cambiar de proyecto).
void syncProject(PackageManagerState& s, const fs::path& project) {
    if (s.project_folder == project) return;
    s.project_folder = project;
    s.installed = loadInstalled(project);
    s.update_check_done = false;
    s.selected.clear();
    s.last_result.clear();
    s.last_error.clear();
}

// Descarga (o toma de la cache), verifica y extrae un paquete y, antes, sus
// dependencias. Corre en un hilo aparte.
class Installer {
public:
    Installer(PackageManagerState& state, fs::path project, std::string registry,
              std::map<std::string, InstalledPackage> installed)
        : s_(state), project_(std::move(project)), registry_(std::move(registry)), installed_(std::move(installed)) {}

    std::vector<PackageStaged> staged;
    std::string error;

    // `version` vacia = la ultima. `local` = un .crwpack del disco.
    bool install(const std::string& name, const std::string& version, const fs::path& local, bool is_dependency) {
        if (visiting_.count(name)) return true;  // ciclo o ya hecho
        visiting_.insert(name);

        std::string pkg_name = name, pkg_version = version, sha;
        fs::path archive;
        std::error_code ec;
        if (!local.empty()) {
            json manifest;
            if (!crwpack::readManifest(local, manifest, &error)) return false;
            pkg_name = str(manifest, "name");
            pkg_version = str(manifest, "version");
            if (!crwpack::validName(pkg_name) || !crwpack::validVersion(pkg_version)) {
                error = "El .crwpack no tiene un nombre y una version validos en su manifiesto";
                return false;
            }
            visiting_.insert(pkg_name);
            archive = cacheFolder(project_) / (pkg_name + "@" + pkg_version + ".crwpack");
            fs::create_directories(archive.parent_path(), ec);
            if (fs::weakly_canonical(local, ec) != fs::weakly_canonical(archive, ec)) {
                fs::copy_file(local, archive, fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    error = "No se pudo copiar el .crwpack a la cache del proyecto";
                    return false;
                }
            }
            sha = update::sha256File(archive);
        } else {
            // Version concreta (o la ultima) segun la Store.
            s_.setStatus("Consultando " + name + "...");
            json reply;
            if (!apiGet(registry_, "/packages/" + net::urlEncode(name), reply, error, &s_.cancel)) {
                error = name + ": " + error;
                return false;
            }
            const RemotePackage remote = parsePackage(reply.value("package", json::object()));
            const RemoteVersion* chosen = nullptr;
            for (const RemoteVersion& v : remote.versions) {
                if (pkg_version.empty() ? !v.yanked : v.version == pkg_version) {
                    chosen = &v;
                    break;
                }
            }
            // Dependencia con version minima que no existe tal cual: la ultima
            // que la cumpla.
            if (chosen == nullptr && is_dependency && !pkg_version.empty()) {
                for (const RemoteVersion& v : remote.versions) {
                    if (!v.yanked && crwpack::compareVersions(v.version, pkg_version) >= 0) {
                        chosen = &v;
                        break;
                    }
                }
            }
            if (chosen == nullptr) {
                error = name + ": no hay version " + (pkg_version.empty() ? std::string("publicada") : pkg_version);
                return false;
            }
            pkg_version = chosen->version;
            sha = chosen->sha256;
            archive = cacheFolder(project_) / (name + "@" + pkg_version + ".crwpack");
            fs::create_directories(archive.parent_path(), ec);
            const bool cached = fs::exists(archive, ec) && update::sha256File(archive) == sha;
            if (!cached) {
                s_.setStatus("Descargando " + remote.display_name + " " + pkg_version + "...");
                s_.done = 0;
                s_.total = chosen->size;
                const std::string url = registry_ + "/packages/" + net::urlEncode(name) + "/versions/" +
                                        net::urlEncode(pkg_version) + "/download";
                std::string download_error;
                const bool ok = update::downloadFile(
                    url, archive,
                    [this](std::uint64_t done, std::uint64_t total) {
                        s_.done = done;
                        if (total) s_.total = total;
                        return !s_.cancel.load();
                    },
                    &download_error);
                if (!ok) {
                    error = s_.cancel ? "Cancelado" : "No se pudo descargar " + name + ": " + download_error;
                    return false;
                }
                if (update::sha256File(archive) != sha) {
                    fs::remove(archive, ec);
                    error = "La descarga de " + name + " " + pkg_version + " no coincide con la de la Store (SHA-256)";
                    return false;
                }
            }
        }

        // Manifiesto: nombre correcto y dependencias.
        json manifest;
        if (!crwpack::readManifest(archive, manifest, &error)) return false;
        if (str(manifest, "name") != pkg_name) {
            error = "El paquete descargado no es " + pkg_name;
            return false;
        }
        const std::map<std::string, std::string> dependencies =
            manifest.contains("dependencies") ? deps(manifest["dependencies"]) : std::map<std::string, std::string>{};
        for (const auto& [dep, spec] : dependencies) {
            if (!crwpack::validName(dep)) continue;
            const std::string minimum = minimumVersion(spec);
            auto have = installed_.find(dep);
            if (have != installed_.end() &&
                (minimum.empty() || crwpack::compareVersions(have->second.version, minimum) >= 0)) {
                continue;
            }
            if (!install(dep, minimum, {}, true)) return false;
        }

        // Extraer (multihilo) una vez por version; se reutiliza al volver a ella.
        const fs::path content = cacheFolder(project_) / (pkg_name + "@" + pkg_version);
        const fs::path ok_marker = content / ".crwpack-ok";
        bool extracted = false;
        {
            std::ifstream marker(ok_marker);
            std::string marker_sha;
            marker >> marker_sha;
            extracted = marker && marker_sha == sha;
        }
        if (!extracted) {
            s_.setStatus("Descomprimiendo " + pkg_name + " " + pkg_version + "...");
            fs::remove_all(content, ec);
            s_.done = 0;
            s_.total = 1;
            if (!crwpack::unpack(archive, content, 0,
                                 [this](std::uint64_t done, std::uint64_t total) {
                                     s_.done = done;
                                     s_.total = total ? total : 1;
                                     return !s_.cancel.load();
                                 },
                                 &error)) {
                fs::remove_all(content, ec);
                return false;
            }
            std::ofstream(ok_marker) << sha;
        }

        PackageStaged st;
        st.content = content;
        st.info.name = pkg_name;
        st.info.version = pkg_version;
        st.info.display_name = str(manifest, "displayName").empty() ? pkg_name : str(manifest, "displayName");
        st.info.sha256 = sha;
        st.info.source = local.empty() ? "store" : "local";
        st.info.installed = isoNow();
        st.info.dependencies = dependencies;
        installed_[pkg_name] = st.info;
        staged.push_back(std::move(st));
        return true;
    }

private:
    PackageManagerState& s_;
    fs::path project_;
    std::string registry_;
    std::map<std::string, InstalledPackage> installed_;
    std::set<std::string> visiting_;
};

}  // namespace

// --- Peticiones --------------------------------------------------------------

void EditorApp::packageFetchCatalog() {
    PackageManagerState& s = packageManager();
    {
        std::lock_guard lock(s.mutex);
        if (s.catalog_loading) return;
        s.catalog_loading = true;
        s.catalog_error.clear();
    }
    s.catalog_requested = true;
    s.last_search = s.search;
    s.last_category = s.category;
    s.last_sort = s.sort;
    std::string query = "/packages?limit=100&sort=";
    query += s.sort == 1 ? "recent" : s.sort == 2 ? "name" : "downloads";
    if (!s.search.empty()) query += "&q=" + net::urlEncode(s.search);
    std::string category;
    {
        std::lock_guard lock(s.mutex);
        if (s.category > 0 && s.category <= static_cast<int>(s.categories.size())) {
            category = s.categories[static_cast<std::size_t>(s.category - 1)].first;
        }
    }
    if (!category.empty()) query += "&category=" + net::urlEncode(category);
    const std::string registry = s.registry;
    const bool need_categories = s.categories.empty();
    PackageManagerState* state = &s;
    s.run([state, registry, query, need_categories] {
        std::string error;
        json reply, cats;
        const bool ok = apiGet(registry, query, reply, error, &state->shutting_down);
        if (need_categories && ok) apiGet(registry, "/categories", cats, error, &state->shutting_down);
        std::lock_guard lock(state->mutex);
        state->catalog_loading = false;
        if (!ok) {
            state->catalog_error = error;
            return;
        }
        state->catalog.clear();
        for (const json& p : reply.value("packages", json::array())) state->catalog.push_back(parsePackage(p));
        state->catalog_total = reply.value("total", 0);
        if (cats.is_object() && cats.contains("categories")) {
            state->categories.clear();
            for (const json& c : cats["categories"]) state->categories.emplace_back(str(c, "id"), str(c, "name"));
        }
    });
}

void EditorApp::packageFetchDetail(const std::string& name) {
    PackageManagerState& s = packageManager();
    {
        std::lock_guard lock(s.mutex);
        if (s.details_loading.count(name)) return;
        s.details_loading.insert(name);
        s.detail_errors.erase(name);
    }
    const std::string registry = s.registry;
    PackageManagerState* state = &s;
    s.run([state, registry, name] {
        std::string error;
        json reply;
        const bool ok = apiGet(registry, "/packages/" + net::urlEncode(name), reply, error, &state->shutting_down);
        std::lock_guard lock(state->mutex);
        state->details_loading.erase(name);
        if (ok) state->details[name] = parsePackage(reply.value("package", json::object()));
        else state->detail_errors[name] = error;
    });
}

void EditorApp::packageInstall(const std::string& name, const std::string& version, const fs::path& local_file) {
    PackageManagerState& s = packageManager();
    if (s.busy) return;
    s.busy = true;
    s.cancel = false;
    s.done = 0;
    s.total = 0;
    s.last_result.clear();
    s.last_error.clear();
    s.setStatus("Preparando...");
    PackageManagerState* state = &s;
    const fs::path project = project_.folder;
    const std::string registry = s.registry;
    std::map<std::string, InstalledPackage> installed = s.installed;
    s.run([state, project, registry, installed, name, version, local_file] {
        auto installer = std::make_shared<Installer>(*state, project, registry, installed);
        const bool ok = installer->install(name, version, local_file, false);
        state->post([installer, ok, state](EditorApp& app) {
            state->busy = false;
            state->setStatus({});
            if (!ok) {
                state->last_error = installer->error;
                app.pushToast("Package Manager", installer->error, 3);
                std::cerr << "[Paquetes] " << installer->error << "\n";
                return;
            }
            app.packageApplyStaged(installer->staged);
        });
    });
}

// Copia a Assets/Packages/<nombre> lo que el instalador dejo listo (hilo
// principal: toca la base de datos de assets).
void EditorApp::packageApplyStaged(const std::vector<PackageStaged>& staged) {
    PackageManagerState& s = packageManager();
    // Los modelos sin importar de un paquete (.fbx, .obj...) se importan en
    // otro hilo y escriben su .crdata al terminar: con uno en marcha, cambiar
    // ya la carpeta dejaria el modelo de la version anterior en la nueva. Se
    // aplica cuando terminen (sigue ocupado hasta entonces).
    if (!imports_.empty()) {
        s.busy = true;
        s.setStatus("Esperando a que terminen las importaciones...");
        s.post([staged](EditorApp& app) { app.packageApplyStaged(staged); });
        return;
    }
    s.busy = false;
    s.setStatus({});
    std::string summary;
    for (const PackageStaged& st : staged) {
        const fs::path target = project_.assetsFolder() / "Packages" / dialogs::fromUtf8(st.info.name);
        std::error_code ec;
        // La version anterior se quita entera (como Unity): lo del paquete no se edita a mano.
        fs::remove_all(target, ec);
        if (ec) {
            s.last_error = "No se pudo quitar la version anterior de " + st.info.name + " (algun archivo abierto?)";
            pushToast("Package Manager", s.last_error, 3);
            return;
        }
        fs::create_directories(target, ec);
        importFolder(st.content, target);
        fs::remove(target / ".crwpack-ok", ec);
        const auto previous = s.installed.find(st.info.name);
        const std::string before = previous != s.installed.end() ? previous->second.version : std::string();
        s.installed[st.info.name] = st.info;
        std::cout << "[Paquetes] " << st.info.name << " " << (before.empty() ? "" : before + " -> ") << st.info.version
                  << " instalado en Assets/Packages/" << st.info.name << "\n";
        if (!summary.empty()) summary += ", ";
        summary += st.info.display_name + " " + st.info.version;
    }
    saveInstalled(project_.folder, s.installed, s.registry);
    s.last_result = summary.empty() ? std::string() : "Instalado: " + summary;
    if (!summary.empty()) pushToast("Paquete instalado", summary, 1);
}

void EditorApp::packageRemove(const std::string& name) {
    PackageManagerState& s = packageManager();
    if (!imports_.empty()) {
        // Un modelo aun importandose escribiria su .crdata en la carpeta borrada.
        s.last_error = "Espera a que terminen las importaciones para quitar " + name;
        pushToast("Package Manager", s.last_error, 2);
        return;
    }
    const fs::path target = project_.assetsFolder() / "Packages" / dialogs::fromUtf8(name);
    std::error_code ec;
    fs::remove_all(target, ec);
    if (ec) {
        s.last_error = "No se pudo borrar " + dialogs::utf8(target) + ": " + ec.message();
        pushToast("Package Manager", s.last_error, 3);
        return;
    }
    // Packages/ vacia sobra.
    if (fs::is_empty(project_.assetsFolder() / "Packages", ec)) fs::remove(project_.assetsFolder() / "Packages", ec);
    s.installed.erase(name);
    saveInstalled(project_.folder, s.installed, s.registry);
    refreshDatabase();
    s.last_result = "Quitado: " + name;
    s.last_error.clear();
    std::cout << "[Paquetes] " << name << " quitado del proyecto\n";
    pushToast("Paquete quitado", name, 0);
}

void EditorApp::packageOpenExport() {
    PackageManagerState& s = packageManager();
    s.export_open = true;
    s.ex_result.clear();
    // La carpeta abierta en el navegador del proyecto.
    s.ex_folder = dialogs::utf8(current_folder_.empty() ? project_.assetsFolder() : current_folder_);
}

void EditorApp::packageExport(const fs::path& output) {
    PackageManagerState& s = packageManager();
    if (s.busy) return;
    const fs::path folder = dialogs::fromUtf8(s.ex_folder);
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        s.ex_result = "Esa carpeta no existe";
        return;
    }
    if (!crwpack::validName(s.ex_name)) {
        s.ex_result = "Identificador no valido: minusculas, numeros, '.', '_' o '-' (ej. com.tunombre.rocas)";
        return;
    }
    if (!crwpack::validVersion(s.ex_version)) {
        s.ex_result = "Version no valida: MAYOR.MENOR.PARCHE (ej. 1.0.0)";
        return;
    }
    json manifest = {{"name", s.ex_name},
                     {"version", s.ex_version},
                     {"displayName", s.ex_display.empty() ? s.ex_name : s.ex_display},
                     {"description", s.ex_description},
                     {"author", s.ex_author},
                     {"engine", std::string(">=") + CRAMION_VERSION_STRING},
                     {"changelog", s.ex_changelog},
                     {"createdWith", std::string("CramionEditor ") + CRAMION_VERSION_STRING},
                     {"dependencies", json::object()}};
    // Dependencias: "com.a.b=1.0.0" separadas por comas o espacios.
    std::string token;
    for (std::size_t i = 0; i <= s.ex_deps.size(); ++i) {
        const char c = i < s.ex_deps.size() ? s.ex_deps[i] : ',';
        if (c == ',' || c == ' ' || c == ';' || c == '\n') {
            if (!token.empty()) {
                const auto eq = token.find('=');
                const std::string dep = token.substr(0, eq);
                if (!crwpack::validName(dep)) {
                    s.ex_result = "Dependencia no valida: " + token;
                    return;
                }
                manifest["dependencies"][dep] = eq == std::string::npos ? "latest" : token.substr(eq + 1);
            }
            token.clear();
        } else {
            token += c;
        }
    }
    const fs::path file = !output.empty()
                              ? output
                              : dialogs::saveFile(window_.handle(), L"Paquete de Cramion (*.crwpack)\0*.crwpack\0", L"crwpack",
                                                  project_.folder.parent_path(),
                                                  dialogs::fromUtf8(s.ex_name + "-" + s.ex_version + ".crwpack").wstring());
    if (file.empty()) return;
    crwpack::PackOptions options;
    options.level = s.ex_level == 0 ? static_cast<int>(crwpack::Level::Fast)
                    : s.ex_level == 2 ? static_cast<int>(crwpack::Level::Max)
                                      : static_cast<int>(crwpack::Level::Normal);
    s.busy = true;
    s.cancel = false;
    s.done = 0;
    s.total = 0;
    s.ex_result.clear();
    s.setStatus("Comprimiendo " + s.ex_name + "...");
    PackageManagerState* state = &s;
    s.run([state, folder, file, manifest, options] {
        std::string error;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = crwpack::pack(folder, file, manifest, options,
                                      [state](std::uint64_t done, std::uint64_t total) {
                                          state->done = done;
                                          state->total = total;
                                          return !state->cancel.load();
                                      },
                                      &error);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        crwpack::Info info;
        if (ok) crwpack::readInfo(file, info, nullptr);
        state->post([state, ok, error, file, info, seconds](EditorApp& app) {
            state->busy = false;
            state->setStatus({});
            if (!ok) {
                state->ex_result = "Error: " + error;
                return;
            }
            char text[256];
            std::snprintf(text, sizeof(text), "%zu archivos, %s -> %s en %.1f s", info.files.size(),
                          update::formatBytes(info.raw_size).c_str(), update::formatBytes(info.packed_size).c_str(), seconds);
            state->ex_result = std::string("Listo: ") + dialogs::utf8(file.filename()) + " (" + text + ")";
            app.pushToast("Paquete exportado", dialogs::utf8(file.filename()), 1);
        });
    });
}

// --- Interfaz ----------------------------------------------------------------

void EditorApp::drawPackageManagerWindow() {
    if (!package_manager_ && !show_package_manager_) return;
    PackageManagerState& s = packageManager();

    // Lo que dejaron los hilos para el hilo principal (aunque la ventana este cerrada).
    std::vector<std::function<void(EditorApp&)>> actions;
    {
        std::lock_guard lock(s.mutex);
        actions.swap(s.main_actions);
    }
    for (auto& action : actions) action(*this);
    s.reapWorkers();

    if (!has_project_) return;
    syncProject(s, project_.folder);
    if (!show_package_manager_) return;

    // Al abrir: catalogo y buscar actualizaciones de lo instalado.
    if (!s.catalog_requested) packageFetchCatalog();
    if (!s.update_check_done) {
        s.update_check_done = true;
        for (const auto& [name, p] : s.installed) {
            if (p.source != "local") packageFetchDetail(name);
        }
    }

    ImGui::SetNextWindowSize(ImVec2(940.0f, 580.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Package Manager", &show_package_manager_)) {
        ImGui::End();
        return;
    }

    std::unique_lock lock(s.mutex);
    const std::vector<RemotePackage> catalog = s.catalog;
    const std::map<std::string, RemotePackage> details = s.details;
    const std::vector<std::pair<std::string, std::string>> categories = s.categories;
    const bool catalog_loading = s.catalog_loading;
    const std::string catalog_error = s.catalog_error;
    const int catalog_total = s.catalog_total;
    const std::string status = s.status;
    const std::set<std::string> details_loading = s.details_loading;
    const std::map<std::string, std::string> detail_errors = s.detail_errors;
    lock.unlock();

    auto latestOf = [&](const std::string& name) -> const RemoteVersion* {
        auto d = details.find(name);
        if (d != details.end() && d->second.has_latest) return &d->second.latest;
        for (const RemotePackage& p : catalog) {
            if (p.name == name && p.has_latest) return &p.latest;
        }
        return nullptr;
    };
    auto hasUpdate = [&](const InstalledPackage& p) {
        const RemoteVersion* latest = latestOf(p.name);
        return latest != nullptr && crwpack::compareVersions(latest->version, p.version) > 0;
    };

    // Instalar y quitar esperan tambien a los modelos que se estan importando
    // (los de un paquete escriben su .crdata en Assets/Packages al terminar).
    const bool busy = s.busy || !imports_.empty();

    // --- Barra superior ---
    ImGui::SetNextItemWidth(170.0f);
    const char* tabs[] = {"En el proyecto", "Cramion Store"};
    if (ImGui::BeginCombo("##pm_tab", tabs[s.tab])) {
        for (int i = 0; i < 2; ++i) {
            if (ImGui::Selectable(tabs[i], s.tab == i)) s.tab = i;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    // El orden, del ancho de su texto mas largo (con 120 px se cortaba "Más descargados").
    const float sort_width = ImGui::CalcTextSize("Más descargados").x + ImGui::GetFrameHeight() +
                             ImGui::GetStyle().FramePadding.x * 2.0f + 4.0f;
    ImGui::SetNextItemWidth(std::max(140.0f, ImGui::GetContentRegionAvail().x - 400.0f - sort_width));
    const bool enter = ImGui::InputTextWithHint("##pm_search", "Buscar paquetes...", &s.search,
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    if (s.tab == 1) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        const std::string current = s.category == 0 || s.category > static_cast<int>(categories.size())
                                        ? std::string("Todas las categorías")
                                        : categories[static_cast<std::size_t>(s.category - 1)].second;
        if (ImGui::BeginCombo("##pm_cat", current.c_str())) {
            if (ImGui::Selectable("Todas las categorías", s.category == 0)) s.category = 0;
            for (std::size_t i = 0; i < categories.size(); ++i) {
                if (ImGui::Selectable(categories[i].second.c_str(), s.category == static_cast<int>(i) + 1)) {
                    s.category = static_cast<int>(i) + 1;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(sort_width);
        const char* sorts[] = {"Más descargados", "Recientes", "Nombre"};
        ImGui::Combo("##pm_sort", &s.sort, sorts, 3);
        if ((enter || s.category != s.last_category || s.sort != s.last_sort) && !catalog_loading) packageFetchCatalog();
    }
    ImGui::SameLine();
    if (ImGui::Button("Actualizar lista")) {
        packageFetchCatalog();
        for (const auto& [name, p] : s.installed) {
            if (p.source != "local") packageFetchDetail(name);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("+ ...")) ImGui::OpenPopup("pm_more");
    if (ImGui::BeginPopup("pm_more")) {
        if (ImGui::MenuItem("Instalar desde archivo .crwpack...", nullptr, false, !busy)) {
            const fs::path file = dialogs::openFile(window_.handle(), L"Paquete de Cramion (*.crwpack)\0*.crwpack\0",
                                                    project_.folder.parent_path());
            if (!file.empty()) packageInstall({}, {}, file);
        }
        if (ImGui::MenuItem("Exportar carpeta como .crwpack...")) packageOpenExport();
        ImGui::Separator();
        if (ImGui::MenuItem("Abrir la Cramion Store en el navegador")) update::openUrl(kStoreWeb);
        if (ImGui::MenuItem("Publicar un paquete (web)...")) update::openUrl(std::string(kStoreWeb) + "panel.html");
        ImGui::Separator();
        if (ImGui::MenuItem("Servidor de paquetes...")) {
            s.settings_open = true;
            s.registry_edit = s.registry;
        }
        ImGui::EndPopup();
    }

    // --- Lista ---
    const float footer = ImGui::GetFrameHeightWithSpacing() * (s.busy ? 2.2f : 1.2f);
    const float list_width = std::clamp(ImGui::GetContentRegionAvail().x * 0.36f, 240.0f, 380.0f);
    ImGui::BeginChild("pm_list", ImVec2(list_width, -footer), ImGuiChildFlags_Borders);
    auto matches = [&](const std::string& a, const std::string& b) {
        if (s.search.empty()) return true;
        std::string hay = a + " " + b, needle = s.search;
        std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return hay.find(needle) != std::string::npos;
    };
    auto row = [&](const std::string& name, const std::string& title, const std::string& right, ImU32 right_color) {
        ImGui::PushID(name.c_str());
        const bool selected = s.selected == name;
        const float line = ImGui::GetTextLineHeight();
        if (ImGui::Selectable("##row", selected, 0, ImVec2(0.0f, line * 2.3f))) {
            s.selected = name;
            s.selected_version.clear();
            packageFetchDetail(name);
        }
        // Los textos encima de la fila, con la lista de dibujo: mover el
        // cursor para escribirlos (SetCursorPos) sin otro elemento despues
        // dispara el aviso de Dear ImGui en la ultima fila de la lista.
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(min, max, true);
        const float w = ImGui::CalcTextSize(right.c_str()).x;
        draw->AddText(ImVec2(max.x - w - 4.0f, min.y + 2.0f), right_color, right.c_str());
        draw->PushClipRect(min, ImVec2(std::max(min.x, max.x - w - 12.0f), max.y), true);
        draw->AddText(ImVec2(min.x + 6.0f, min.y + 2.0f), ImGui::GetColorU32(ImGuiCol_Text), title.c_str());
        draw->AddText(ImVec2(min.x + 6.0f, min.y + 2.0f + line), theme::kTextFaint, name.c_str());
        draw->PopClipRect();
        draw->PopClipRect();
        ImGui::PopID();
    };
    if (s.tab == 0) {
        if (s.installed.empty()) {
            ImGui::TextWrapped("Este proyecto no tiene paquetes. Busca en la Cramion Store o instala un .crwpack desde el botón \"+ ...\".");
        }
        for (const auto& [name, p] : s.installed) {
            if (!matches(name, p.display_name)) continue;
            const bool update = hasUpdate(p);
            row(name, p.display_name.empty() ? name : p.display_name,
                update ? p.version + "  ->  " + latestOf(name)->version : p.version, update ? theme::kYellow : theme::kTextDim);
        }
    } else {
        if (catalog_loading) ImGui::TextDisabled("Cargando la Cramion Store...");
        if (!catalog_error.empty()) {
            ImGui::TextColored(theme::vec(theme::kRedText), "No se pudo conectar:");
            ImGui::TextWrapped("%s", catalog_error.c_str());
            ImGui::TextDisabled("%s", s.registry.c_str());
        } else if (!catalog_loading && catalog.empty()) {
            ImGui::TextDisabled("No hay paquetes que coincidan.");
        }
        for (const RemotePackage& p : catalog) {
            auto inst = s.installed.find(p.name);
            std::string right = p.has_latest ? p.latest.version : std::string();
            ImU32 color = theme::kTextDim;
            if (inst != s.installed.end()) {
                const bool update = p.has_latest && crwpack::compareVersions(p.latest.version, inst->second.version) > 0;
                right = update ? "Actualizar" : "Instalado";
                color = update ? theme::kYellow : theme::kOk;
            }
            row(p.name, (p.featured ? "* " : "") + p.display_name, right, color);
        }
        if (catalog_total > static_cast<int>(catalog.size())) {
            ImGui::TextDisabled("%d de %d. Afina la búsqueda para ver más.", static_cast<int>(catalog.size()), catalog_total);
        }
    }
    ImGui::EndChild();

    // --- Detalle ---
    ImGui::SameLine();
    ImGui::BeginChild("pm_detail", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders);
    if (s.selected.empty()) {
        ImGui::TextDisabled("Elige un paquete de la lista.");
    } else {
        const std::string& name = s.selected;
        auto det = details.find(name);
        const RemotePackage* remote = det != details.end() ? &det->second : nullptr;
        if (remote == nullptr) {
            for (const RemotePackage& p : catalog) {
                if (p.name == name) remote = &p;
            }
        }
        auto inst = s.installed.find(name);
        const InstalledPackage* installed = inst != s.installed.end() ? &inst->second : nullptr;

        const std::string title = remote ? remote->display_name : installed ? installed->display_name : name;
        ImGui::PushFont(theme::boldFont(), ImGui::GetStyle().FontSizeBase * 1.35f);
        ImGui::TextUnformatted(title.c_str());
        ImGui::PopFont();
        ImGui::TextColored(theme::vec(theme::kTextFaint), "%s", name.c_str());
        if (remote) {
            ImGui::Text("Por %s  ·  %s  ·  %llu %s", remote->author.c_str(), remote->license.c_str(),
                        static_cast<unsigned long long>(remote->downloads), remote->downloads == 1 ? "descarga" : "descargas");
        }
        if (installed) {
            ImGui::TextColored(theme::vec(theme::kOk), "Instalada: %s", installed->version.c_str());
            if (installed->source == "local") {
                ImGui::SameLine();
                ImGui::TextDisabled("(desde archivo)");
            }
        }
        ImGui::Separator();

        // Versiones.
        std::vector<RemoteVersion> versions = remote && remote->has_versions ? remote->versions : std::vector<RemoteVersion>{};
        if (details_loading.count(name)) ImGui::TextDisabled("Cargando versiones...");
        auto err = detail_errors.find(name);
        if (err != detail_errors.end() && !(installed && installed->source == "local")) {
            ImGui::TextColored(theme::vec(theme::kRedText), "%s", err->second.c_str());
        }
        if (s.selected_version.empty()) {
            for (const RemoteVersion& v : versions) {
                if (!v.yanked) {
                    s.selected_version = v.version;
                    break;
                }
            }
        }
        const RemoteVersion* chosen = nullptr;
        for (const RemoteVersion& v : versions) {
            if (v.version == s.selected_version) chosen = &v;
        }

        if (!versions.empty()) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Versión");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(220.0f);
            std::string preview = s.selected_version;
            if (chosen && chosen == &versions.front()) preview += " (última)";
            if (ImGui::BeginCombo("##pm_ver", preview.c_str())) {
                for (std::size_t i = 0; i < versions.size(); ++i) {
                    const RemoteVersion& v = versions[i];
                    std::string label = v.version;
                    if (i == 0) label += "  (última)";
                    if (v.yanked) label += "  (retirada)";
                    if (installed && installed->version == v.version) label += "  - instalada";
                    if (ImGui::Selectable(label.c_str(), v.version == s.selected_version)) s.selected_version = v.version;
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(busy || chosen == nullptr);
            if (!installed) {
                if (ImGui::Button(("Instalar " + s.selected_version).c_str())) packageInstall(name, s.selected_version, {});
            } else if (chosen && installed->version != chosen->version) {
                const int cmp = crwpack::compareVersions(chosen->version, installed->version);
                if (ImGui::Button(((cmp > 0 ? "Actualizar a " : "Bajar a ") + chosen->version).c_str())) {
                    packageInstall(name, chosen->version, {});
                }
            } else if (chosen) {
                if (ImGui::Button("Reinstalar")) packageInstall(name, chosen->version, {});
            }
            ImGui::EndDisabled();
        }
        if (installed) {
            ImGui::SameLine();
            ImGui::BeginDisabled(busy);
            ImGui::PushStyleColor(ImGuiCol_Button, theme::vec(theme::kRed));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::vec(theme::kRedHover));
            if (ImGui::Button("Quitar")) ImGui::OpenPopup("pm_remove");
            ImGui::PopStyleColor(2);
            ImGui::EndDisabled();
            if (ImGui::BeginPopup("pm_remove")) {
                ImGui::Text("¿Quitar %s?", title.c_str());
                ImGui::TextDisabled("Se borra Assets/Packages/%s.", name.c_str());
                if (ImGui::Button("Quitar")) {
                    packageRemove(name);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancelar")) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }
        if (remote) {
            ImGui::SameLine();
            if (ImGui::Button("Ver en la web")) update::openUrl(std::string(kStoreWeb) + "paquete.html?n=" + net::urlEncode(name));
        }

        if (chosen) {
            ImGui::TextDisabled("%s descargado  ·  %s descomprimido  ·  publicada el %s%s",
                                update::formatBytes(chosen->size).c_str(), update::formatBytes(chosen->raw_size).c_str(),
                                dateText(chosen->created).c_str(),
                                chosen->engine.empty() ? "" : ("  ·  motor " + chosen->engine).c_str());
            if (chosen->yanked) {
                ImGui::TextColored(theme::vec(theme::kYellow), "El autor retiró esta versión: solo instálala si la necesitas.");
            }
        }
        ImGui::Spacing();
        if (remote && !remote->description.empty()) {
            ImGui::SeparatorText("Descripción");
            ImGui::TextWrapped("%s", remote->description.c_str());
        }
        if (chosen && !chosen->changelog.empty()) {
            ImGui::SeparatorText(("Cambios en " + chosen->version).c_str());
            ImGui::TextWrapped("%s", chosen->changelog.c_str());
        }
        const std::map<std::string, std::string>& dependencies =
            chosen ? chosen->dependencies : installed ? installed->dependencies : std::map<std::string, std::string>{};
        if (!dependencies.empty()) {
            ImGui::SeparatorText("Dependencias");
            for (const auto& [dep, spec] : dependencies) {
                const bool have = s.installed.count(dep) != 0;
                ImGui::TextColored(theme::vec(have ? theme::kOk : theme::kTextDim), "%s %s%s", dep.c_str(), spec.c_str(),
                                   have ? "  (instalada)" : "  (se instalará)");
            }
        }
        if (remote && !remote->tags.empty()) {
            std::string tags;
            for (const std::string& t : remote->tags) tags += (tags.empty() ? "#" : "  #") + t;
            ImGui::Spacing();
            ImGui::TextColored(theme::vec(theme::kAccentHover), "%s", tags.c_str());
        }
        if (installed) {
            ImGui::Spacing();
            ImGui::TextDisabled("Carpeta: Assets/Packages/%s", name.c_str());
        }
    }
    ImGui::EndChild();

    // --- Pie: progreso ---
    if (s.busy) {
        const std::uint64_t total = s.total, done = s.done;
        const float fraction = total ? static_cast<float>(static_cast<double>(done) / static_cast<double>(total)) : 0.0f;
        std::string overlay = total ? update::formatBytes(done) + " / " + update::formatBytes(total) : std::string();
        ImGui::TextUnformatted(status.c_str());
        ImGui::ProgressBar(fraction, ImVec2(ImGui::GetContentRegionAvail().x - 90.0f, 0.0f), overlay.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Cancelar", ImVec2(-1.0f, 0.0f))) s.cancel = true;
    } else if (!imports_.empty()) {
        ImGui::TextDisabled("Importando modelos (%zu): instalar y quitar esperan a que terminen", imports_.size());
    } else {
        ImGui::TextDisabled("%zu paquete(s) en el proyecto  ·  %s", s.installed.size(), s.registry.c_str());
    }
    ImGui::End();

    // --- Exportar ---
    if (s.export_open) {
        ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
        if (ImGui::Begin("Exportar paquete .crwpack", &s.export_open, ImGuiWindowFlags_NoCollapse)) {
            ImGui::TextWrapped("Comprime una carpeta en un .crwpack (multihilo) para subirlo a la Cramion Store o compartirlo.");
            ImGui::Spacing();
            ImGui::SetNextItemWidth(-90.0f);
            ImGui::InputText("##ex_folder", &s.ex_folder);
            ImGui::SameLine();
            if (ImGui::Button("Carpeta...", ImVec2(-1.0f, 0.0f))) {
                const fs::path picked = dialogs::pickFolder(window_.handle(), dialogs::fromUtf8(s.ex_folder));
                if (!picked.empty()) s.ex_folder = dialogs::utf8(picked);
            }
            ImGui::InputTextWithHint("Identificador", "com.tunombre.paquete", &s.ex_name);
            ImGui::InputTextWithHint("Versión", "1.0.0", &s.ex_version);
            ImGui::InputTextWithHint("Nombre visible", "Rocas low poly", &s.ex_display);
            ImGui::InputTextWithHint("Autor", "Tu nombre", &s.ex_author);
            ImGui::InputTextMultiline("Descripción", &s.ex_description, ImVec2(0.0f, ImGui::GetTextLineHeight() * 3.5f));
            ImGui::InputTextWithHint("Dependencias", "com.otro.base=1.0.0, ...", &s.ex_deps);
            ImGui::InputTextWithHint("Cambios", "Qué trae esta versión", &s.ex_changelog);
            const char* levels[] = {"Rápida (zstd 3)", "Normal (zstd 9)", "Máxima (zstd 19, lenta)"};
            ImGui::Combo("Compresión", &s.ex_level, levels, 3);
            ImGui::Spacing();
            ImGui::BeginDisabled(s.busy);
            if (ImGui::Button("Exportar...", ImVec2(140.0f, 0.0f))) packageExport();
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Publicar en la web", ImVec2(160.0f, 0.0f))) update::openUrl(std::string(kStoreWeb) + "panel.html");
            if (!s.ex_result.empty()) {
                const bool bad = s.ex_result.rfind("Listo", 0) != 0;
                ImGui::TextColored(theme::vec(bad ? theme::kRedText : theme::kOk), "%s", s.ex_result.c_str());
            }
        }
        ImGui::End();
    }

    // --- Servidor ---
    if (s.settings_open) {
        ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);
        if (ImGui::Begin("Servidor de paquetes", &s.settings_open, ImGuiWindowFlags_NoCollapse)) {
            ImGui::TextWrapped("Dirección de la API de la Cramion Store. Para probar un servidor propio: http://localhost:8000/api");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##registry", &s.registry_edit);
            if (ImGui::Button("Guardar")) {
                while (!s.registry_edit.empty() && s.registry_edit.back() == '/') s.registry_edit.pop_back();
                std::string error;
                if (!net::checkUrl(s.registry_edit, &error)) {
                    pushToast("Package Manager", error, 3);
                } else {
                    s.registry = s.registry_edit;
                    s.saveSettings();
                    s.settings_open = false;
                    {
                        std::lock_guard l(s.mutex);
                        s.details.clear();
                        s.categories.clear();
                    }
                    s.update_check_done = false;
                    packageFetchCatalog();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Por defecto")) s.registry_edit = kDefaultRegistry;
        }
        ImGui::End();
    }
}

// --- MCP -----------------------------------------------------------------------
// Las herramientas de paquetes (las declara EditorMcp.cpp): lo mismo que los
// botones de la ventana. Instalar y exportar siguen en hilos aparte: su
// resultado, en package_state.

std::string EditorApp::packageMcpTool(const std::string& name, const std::string& args_json, std::string& error) {
    PackageManagerState& s = packageManager();
    syncProject(s, project_.folder);
    json args = json::parse(args_json, nullptr, false);
    if (!args.is_object()) args = json::object();
    const auto text = [&](const char* key) { return str(args, key); };
    const auto flag = [&](const char* key, bool fallback) {
        const auto it = args.find(key);
        return it != args.end() && it->is_boolean() ? it->get<bool>() : fallback;
    };
    const auto installedJson = [&] {
        json out = json::array();
        for (const auto& [id, p] : s.installed) {
            out.push_back({{"name", id},
                           {"version", p.version},
                           {"display_name", p.display_name},
                           {"source", p.source},
                           {"sha256", p.sha256},
                           {"installed", p.installed},
                           {"dependencies", p.dependencies},
                           {"folder", "Assets/Packages/" + id}});
        }
        return out;
    };
    const auto installedVersion = [&](const std::string& id) {
        const auto it = s.installed.find(id);
        return it != s.installed.end() ? it->second.version : std::string();
    };
    const auto busy = [&] {
        if (!s.busy) return false;
        error = "El Package Manager ya esta instalando o exportando: espera a que package_state diga busy=false";
        return true;
    };

    if (name == "packages") {
        json out = {{"registry", s.registry}, {"installed", installedJson()}};
        const std::string search = text("query");
        const std::string category = text("category");
        // Abrir la ventana (en la pestana que toca y con un paquete elegido)
        // para ensenarselo al usuario o capturarla con screenshot.
        if (flag("show", false)) {
            show_package_manager_ = true;
            s.tab = flag("store", false) || !search.empty() || !category.empty() ? 1 : 0;
            if (!search.empty()) s.search = search;
            if (const std::string pick = text("select"); !pick.empty()) {
                s.selected = pick;
                s.selected_version.clear();
                packageFetchDetail(pick);
            }
            out["shown"] = true;
        }
        if (!flag("store", false) && search.empty() && category.empty()) return out.dump();
        int limit = 50;
        if (const auto it = args.find("limit"); it != args.end() && it->is_number_integer()) limit = it->get<int>();
        const std::string sort = text("sort");
        std::string query = "/packages?limit=" + std::to_string(std::clamp(limit, 1, 100)) + "&sort=" +
                            (sort == "recent" || sort == "name" || sort == "featured" ? sort : std::string("downloads"));
        if (!search.empty()) query += "&q=" + net::urlEncode(search);
        if (!category.empty()) query += "&category=" + net::urlEncode(category);
        json reply;
        if (!apiGet(s.registry, query, reply, error, &s.shutting_down)) {
            error = "No se pudo consultar la Cramion Store (" + s.registry + "): " + error;
            return {};
        }
        json list = json::array();
        const json packages = reply.value("packages", json::array());
        for (const json& item : packages) {
            if (!item.is_object()) continue;
            const RemotePackage p = parsePackage(item);
            const std::string have = installedVersion(p.name);
            list.push_back({{"name", p.name},
                            {"display_name", p.display_name},
                            {"description", p.description},
                            {"category", p.category},
                            {"author", p.author},
                            {"license", p.license},
                            {"downloads", p.downloads},
                            {"featured", p.featured},
                            {"tags", p.tags},
                            {"latest", p.has_latest ? p.latest.version : std::string()},
                            {"installed_version", have},
                            {"update_available",
                             !have.empty() && p.has_latest && crwpack::compareVersions(p.latest.version, have) > 0}});
        }
        out["store"] = {{"total", reply.contains("total") && reply["total"].is_number() ? reply["total"].get<int>() : 0},
                        {"packages", list}};
        return out.dump();
    }

    if (name == "package_versions") {
        const std::string id = text("name");
        if (!crwpack::validName(id)) {
            error = "Falta 'name': el identificador del paquete (com.autor.paquete)";
            return {};
        }
        json reply;
        if (!apiGet(s.registry, "/packages/" + net::urlEncode(id), reply, error, &s.shutting_down)) {
            error = id + ": " + error;
            return {};
        }
        const RemotePackage p = parsePackage(reply.value("package", json::object()));
        {
            std::lock_guard lock(s.mutex);
            s.details[id] = p;  // la ventana tambien lo ve
        }
        json versions = json::array();
        for (const RemoteVersion& v : p.versions) {
            versions.push_back({{"version", v.version},
                                {"yanked", v.yanked},
                                {"size", v.size},
                                {"raw_size", v.raw_size},
                                {"downloads", v.downloads},
                                {"sha256", v.sha256},
                                {"engine", v.engine},
                                {"changelog", v.changelog},
                                {"dependencies", v.dependencies},
                                {"published", dateText(v.created)}});
        }
        return json{{"name", p.name},
                    {"display_name", p.display_name},
                    {"description", p.description},
                    {"author", p.author},
                    {"license", p.license},
                    {"downloads", p.downloads},
                    {"latest", p.has_latest ? p.latest.version : std::string()},
                    {"installed_version", installedVersion(id)},
                    {"versions", versions}}
            .dump();
    }

    if (name == "package_install") {
        if (busy()) return {};
        const std::string file = text("file");
        const std::string id = text("name");
        std::string version = text("version");
        if (!file.empty()) {
            fs::path path = dialogs::fromUtf8(file);
            if (path.is_relative()) path = project_.folder / path;
            std::error_code ec;
            if (!fs::is_regular_file(path, ec)) {
                error = "No existe el archivo " + dialogs::utf8(path);
                return {};
            }
            packageInstall({}, {}, path);
        } else {
            if (!crwpack::validName(id)) {
                error = "Falta 'name' (el identificador del paquete) o 'file' (un .crwpack del disco)";
                return {};
            }
            if (version == "latest") version.clear();
            if (!version.empty() && !crwpack::validVersion(version)) {
                error = "Version no valida: " + version + " (MAYOR.MENOR.PARCHE, p. ej. 1.2.0, o latest)";
                return {};
            }
            packageInstall(id, version, {});
        }
        return json{{"started", true},
                    {"previous_version", installedVersion(id)},
                    {"note", "Descarga (o toma de Library/PackageCache), comprueba el SHA-256, instala sus dependencias y "
                             "copia a Assets/Packages en segundo plano: mira package_state hasta que busy sea false."}}
            .dump();
    }

    if (name == "package_remove") {
        const std::string id = text("name");
        if (installedVersion(id).empty()) {
            error = (id.empty() ? std::string("Falta 'name'") : id + " no esta instalado en este proyecto");
            return {};
        }
        if (busy()) return {};
        packageRemove(id);
        if (!installedVersion(id).empty()) {
            error = s.last_error.empty() ? "No se pudo quitar " + id : s.last_error;
            return {};
        }
        return json{{"removed", id}, {"installed", installedJson()}}.dump();
    }

    if (name == "package_state") {
        std::string status;
        {
            std::lock_guard lock(s.mutex);
            status = s.status;
        }
        return json{{"busy", s.busy.load()},
                    {"importing", imports_.size()},  // modelos importandose (quitar espera a 0)
                    {"status", status},
                    {"done", s.done.load()},
                    {"total", s.total.load()},
                    {"last_result", s.last_result},
                    {"last_error", s.last_error},
                    {"export_result", s.ex_result},
                    {"registry", s.registry},
                    {"installed", installedJson()}}
            .dump();
    }

    if (name == "package_registry") {
        std::string url = flag("reset", false) ? std::string(kDefaultRegistry) : text("url");
        if (!url.empty()) {
            while (!url.empty() && url.back() == '/') url.pop_back();
            if (!net::checkUrl(url, &error)) return {};
            s.registry = url;
            if (flag("save", true)) s.saveSettings();
            {
                std::lock_guard lock(s.mutex);
                s.details.clear();
                s.categories.clear();
                s.catalog.clear();
            }
            s.catalog_requested = false;
            s.update_check_done = false;
        }
        return json{{"registry", s.registry}, {"default", kDefaultRegistry}}.dump();
    }

    if (name == "package_export") {
        if (busy()) return {};
        fs::path folder = dialogs::fromUtf8(text("folder"));
        fs::path output = dialogs::fromUtf8(text("output"));
        if (folder.empty() || output.empty()) {
            error = "Faltan 'folder' (carpeta dentro de Assets o ruta absoluta) y 'output' (el .crwpack que se crea)";
            return {};
        }
        if (folder.is_relative()) folder = project_.assetsFolder() / folder;
        if (output.is_relative()) output = project_.folder / output;
        s.ex_folder = dialogs::utf8(folder);
        s.ex_name = text("name");
        s.ex_version = text("version").empty() ? std::string("1.0.0") : text("version");
        s.ex_display = text("display_name");
        s.ex_description = text("description");
        s.ex_author = text("author");
        s.ex_changelog = text("changelog");
        s.ex_deps.clear();
        if (const auto it = args.find("dependencies"); it != args.end() && it->is_object()) {
            for (auto dep = it->begin(); dep != it->end(); ++dep) {
                s.ex_deps += dep.key() + "=" + (dep->is_string() ? dep->get<std::string>() : std::string("latest")) + ",";
            }
        } else {
            s.ex_deps = text("dependencies");
        }
        const std::string level = text("level");
        s.ex_level = level == "fast" ? 0 : level == "max" ? 2 : 1;
        s.ex_result.clear();
        packageExport(output);
        if (!s.busy) {
            error = s.ex_result.empty() ? std::string("No se pudo exportar") : s.ex_result;
            return {};
        }
        return json{{"started", true},
                    {"output", dialogs::utf8(output)},
                    {"note", "Comprime en segundo plano: package_state.export_result dice el resultado."}}
            .dump();
    }

    error = "herramienta desconocida: " + name;
    return {};
}

}  // namespace cramion::editor
