// Scripts de C++ aislados en CramionScriptHost.exe (ver CppScripts.h).
//
// Este lado: compila la DLL, la describe, arranca el proceso dentro de un Job
// Object (tope de memoria, muere con el motor), le manda los eventos de cada
// script y contesta sus llamadas al motor (RPC) mientras espera. Cada espera
// tiene un tiempo maximo: si se pasa (bucle infinito) o el proceso muere, se
// desactiva el script culpable y el proceso se arranca de nuevo.

#include "CramionCore/scripting/CppScripts.h"

#include "CramionCore/cvar/CVar.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/scripting/CppScriptIpc.h"

#include <CramionDM/Input.h>
#include <CramionDM/KeyCode.h>
#include <cramion/CppProtocol.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <tuple>
#include <unordered_set>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>
extern char** environ;
#endif

namespace cramion::scripting {

namespace proto = cramion::cppproto;
namespace ipc = cramion::cppipc;
using core::Vec3;
using json = nlohmann::json;

namespace {

cvar::CVar<int> g_timeout_ms("script.cpp.TimeoutMs", 2000,
                             "Tiempo maximo de una llamada a un script de C++ (ms). Mas = bucle infinito: se corta",
                             cvar::Saved, 20, 600000);
cvar::CVar<int> g_memory_mb("script.cpp.MemoryLimitMB", 1024,
                            "Memoria maxima del proceso de los scripts de C++ (MB). Al pasarse, las reservas fallan",
                            cvar::Saved, 32, 65536);
cvar::CVar<bool> g_optimize("script.cpp.Optimize", true, "Compilar los scripts de C++ optimizados (-O2)", cvar::Saved);
cvar::CVar<std::string> g_compiler("script.cpp.Compiler", "",
                                   "Ruta de clang++.exe o cl.exe (vacio = el incluido con el motor, o clang/Visual Studio del sistema)",
                                   cvar::Saved);
cvar::CVar<bool> g_enabled("script.cpp.Enabled", true, "Ejecutar los scripts de C++ en Play", cvar::Saved);
cvar::CVar<bool> g_hot_keep_state("script.cpp.HotReloadKeepState", true,
                                   "Recarga en caliente (guardar un .cpp en Play): conservar propiedades y estado (onBeforeReload/"
                                   "onAfterReload) sin volver a llamar a awake/start",
                                   cvar::Saved);
cvar::CVar<int> g_max_restarts("script.cpp.MaxRestarts", 20,
                               "Veces que se rearranca el proceso de scripts en una sesion de Play antes de rendirse",
                               cvar::Saved, 0, 1000);

std::filesystem::path g_toolchain_override;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::filesystem::path fromUtf8(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

std::string generic(const std::filesystem::path& p) {
    std::string s = utf8(p);
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

std::uint64_t idOf(entt::entity h) {
    return h == entt::null ? 0 : static_cast<std::uint64_t>(entt::to_integral(h)) + 1;
}
entt::entity entityOf(std::uint64_t id) {
    return id == 0 ? entt::null : static_cast<entt::entity>(static_cast<std::underlying_type_t<entt::entity>>(id - 1));
}

// Teclas por nombre ("W", "Space", "LeftShift", "Up"...), sin mayusculas.
dm::Key keyByName(const std::string& name) {
    static const std::unordered_map<std::string, dm::Key> table = [] {
        std::unordered_map<std::string, dm::Key> t;
        for (int i = 1; i < static_cast<int>(dm::Key::Count); ++i) {
            const auto k = static_cast<dm::Key>(i);
            const std::string n = lower(dm::keyName(k));
            if (n != "unknown" && !t.contains(n)) t[n] = k;
        }
        t["shift"] = dm::Key::LeftShift;
        t["ctrl"] = t["control"] = dm::Key::LeftControl;
        t["alt"] = dm::Key::LeftAlt;
        t["esc"] = dm::Key::Escape;
        t["return"] = dm::Key::Enter;
        return t;
    }();
    const auto it = table.find(lower(name));
    return it != table.end() ? it->second : static_cast<dm::Key>(0);
}

std::filesystem::path exeFolder() {
#if defined(_WIN32)
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::filesystem::path(exe).parent_path();
#else
    std::error_code ec;
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path{} : exe.parent_path();
#endif
}

#if defined(_WIN32)
constexpr const char* kHostName = "CramionScriptHost.exe";
constexpr const char* kLibraryExt = ".dll";
constexpr const char* kExeExt = ".exe";
#else
constexpr const char* kHostName = "CramionScriptHost";
constexpr const char* kLibraryExt = ".so";
constexpr const char* kExeExt = "";
#endif

std::filesystem::path toolchainRoot() {
    return !g_toolchain_override.empty() ? g_toolchain_override : exeFolder() / "toolchain";
}

#if defined(_WIN32)
std::wstring wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<std::size_t>(std::max(n, 1)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

// Ejecuta un programa (oculto) y devuelve su salida y su codigo.
int runProcess(const std::wstring& command, std::string& output, DWORD timeout_ms = 300000) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0)) return -1;
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = write;
    si.hStdError = write;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = command;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(read);
        CloseHandle(write);
        output = "no se pudo ejecutar el compilador (error " + std::to_string(GetLastError()) + ")";
        return -1;
    }
    CloseHandle(write);
    std::thread reader([&] {
        char buffer[4096];
        DWORD n = 0;
        while (ReadFile(read, buffer, sizeof(buffer), &n, nullptr) && n > 0) output.append(buffer, n);
    });
    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    reader.join();
    CloseHandle(read);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

std::filesystem::path searchPath(const wchar_t* exe) {
    wchar_t buffer[MAX_PATH];
    if (SearchPathW(nullptr, exe, nullptr, MAX_PATH, buffer, nullptr) > 0) return buffer;
    return {};
}

// vcvars64.bat de la ultima instalacion de Visual Studio con C++.
std::filesystem::path findVcVars() {
    const std::filesystem::path vswhere = "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe";
    std::error_code ec;
    if (!std::filesystem::exists(vswhere, ec)) return {};
    std::string out;
    runProcess(L"\"" + vswhere.wstring() + L"\" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath",
               out, 20000);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    if (out.empty()) return {};
    const std::filesystem::path bat = fromUtf8(out) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
    return std::filesystem::exists(bat, ec) ? bat : std::filesystem::path{};
}
#else
// Un descriptor por encima de 10 y que no pasa a otros programas: el hijo lo
// pone en su sitio (dup2) sin pisar otro.
int highFd(int fd) {
    if (fd < 0) return fd;
    const int moved = fcntl(fd, F_DUPFD_CLOEXEC, 10);
    ::close(fd);
    return moved;
}

// Ejecuta un programa (sin shell) y devuelve su salida (stdout + stderr) y su codigo.
int runProcess(const std::vector<std::string>& args, std::string& output, int timeout_ms = 300000) {
    int pipe_fds[2] = {-1, -1};
    if (args.empty() || pipe(pipe_fds) != 0) return -1;
    const int read_end = highFd(pipe_fds[0]);
    const int write_end = highFd(pipe_fds[1]);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, write_end, 1);
    posix_spawn_file_actions_adddup2(&actions, write_end, 2);
    std::vector<std::string> copy = args;
    std::vector<char*> argv;
    for (std::string& a : copy) argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(write_end);
    if (rc != 0) {
        ::close(read_end);
        output = "no se pudo ejecutar " + args[0] + " (" + std::strerror(rc) + ")";
        return -1;
    }
    const auto start = std::chrono::steady_clock::now();
    char buffer[4096];
    while (true) {
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(timeout_ms)) {
            kill(pid, SIGKILL);
            break;
        }
        pollfd p{read_end, POLLIN, 0};
        if (poll(&p, 1, 100) == 0) continue;
        const ssize_t n = read(read_end, buffer, sizeof(buffer));
        if (n > 0) output.append(buffer, static_cast<std::size_t>(n));
        else if (n == 0 || errno != EINTR) break;
    }
    ::close(read_end);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
}

// Un programa en el PATH (vacio si no esta).
std::filesystem::path searchPath(const std::string& exe) {
    const char* path = std::getenv("PATH");
    std::string list = path != nullptr ? path : "/usr/local/bin:/usr/bin:/bin";
    std::size_t start = 0;
    while (start <= list.size()) {
        std::size_t end = list.find(':', start);
        if (end == std::string::npos) end = list.size();
        const std::string folder = list.substr(start, end - start);
        if (!folder.empty()) {
            const std::filesystem::path candidate = std::filesystem::path(folder) / exe;
            if (access(candidate.c_str(), X_OK) == 0) return candidate;
        }
        start = end + 1;
    }
    return {};
}

// Una herramienta de LLVM: en el PATH o en /usr/lib/llvm-N/bin (la version mas alta).
std::filesystem::path searchLlvmTool(const std::string& exe) {
    std::filesystem::path found = searchPath(exe);
    if (!found.empty()) return found;
    std::error_code ec;
    int best = -1;
    for (std::filesystem::directory_iterator it("/usr/lib", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = utf8(it->path().filename());
        if (name.rfind("llvm-", 0) != 0) continue;
        const int version = std::atoi(name.c_str() + 5);
        const std::filesystem::path candidate = it->path() / "bin" / exe;
        if (version > best && access(candidate.c_str(), X_OK) == 0) {
            best = version;
            found = candidate;
        }
    }
    return found;
}
#endif

std::vector<std::filesystem::path> sourcesIn(const std::filesystem::path& root, bool headers) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        const std::string name = utf8(it->path().filename());
        if (it->is_directory(ec) && !name.empty() && name[0] == '.') {
            it.disable_recursion_pending();
            continue;
        }
        const std::string ext = lower(utf8(it->path().extension()));
        if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || (headers && (ext == ".h" || ext == ".hpp"))) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// --- Registro de clases (Inspector) ---
std::vector<CppClassInfo>& classStore() {
    static std::vector<CppClassInfo> store;
    return store;
}

std::vector<CppClassInfo> parseSchema(const std::string& text, const std::filesystem::path& assets_root) {
    std::vector<CppClassInfo> out;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_array()) return out;
    for (const json& c : j) {
        CppClassInfo info;
        info.name = c.value("class", std::string());
        // El archivo, dentro de Assets (el __FILE__ de la compilacion es absoluto).
        std::string file = c.value("file", std::string());
        std::error_code ec;
        const std::filesystem::path rel = std::filesystem::relative(fromUtf8(file), assets_root, ec);
        if (!ec && !rel.empty() && utf8(rel).rfind("..", 0) != 0) file = generic(rel);
        else std::replace(file.begin(), file.end(), '\\', '/');
        info.file = file;
        for (const json& p : c.value("properties", json::array())) {
            CppPropertyInfo prop;
            prop.name = p.value("name", std::string());
            prop.label = p.value("label", std::string());
            prop.tooltip = p.value("tooltip", std::string());
            prop.header = p.value("header", std::string());
            prop.kind = p.value("kind", std::string());
            prop.element = p.value("element", std::string());
            prop.type = p.value("type", std::string());
            prop.has_range = p.value("has_range", false);
            prop.min = p.value("min", 0.0);
            prop.max = p.value("max", 0.0);
            for (const json& o : p.value("options", json::array())) prop.options.push_back(o.is_string() ? o.get<std::string>() : o.dump());
            prop.default_json = p.contains("default") ? p["default"].dump() : "null";
            if (!prop.name.empty()) info.properties.push_back(std::move(prop));
        }
        if (!info.name.empty()) out.push_back(std::move(info));
    }
    return out;
}

}  // namespace

const std::vector<CppClassInfo>& cppScriptClasses() { return classStore(); }

const CppClassInfo* findCppScriptClass(const std::string& name) {
    for (const CppClassInfo& c : classStore()) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

std::vector<const CppClassInfo*> cppScriptClassesInFile(const std::string& relative_file) {
    std::vector<const CppClassInfo*> out;
    const std::string want = lower(relative_file);
    for (const CppClassInfo& c : classStore()) {
        if (lower(c.file) == want) out.push_back(&c);
    }
    return out;
}

// -----------------------------------------------------------------------------
// Componente
// -----------------------------------------------------------------------------

const CppScriptValue* CppScript::value(const std::string& name) const {
    for (const CppScriptValue& v : values) {
        if (v.name == name) return &v;
    }
    return nullptr;
}

void CppScript::setValue(const std::string& name, const std::string& json_text) {
    for (CppScriptValue& v : values) {
        if (v.name == name) {
            v.json = json_text;
            return;
        }
    }
    values.push_back({name, json_text});
}

namespace {

// Un valor de una propiedad en el Inspector, con el control de su tipo.
bool reflectValue(ecs::PropertyVisitor& v, const ecs::Meta& meta, const std::string& kind, const CppPropertyInfo& p,
                  json& value) {
    if (kind == "bool") {
        bool b = value.is_boolean() ? value.get<bool>() : (value.is_number() && value.get<double>() != 0.0);
        if (!v.field(meta, b)) return false;
        value = b;
        return true;
    }
    if (kind == "int") {
        int i = value.is_number() ? static_cast<int>(value.get<double>()) : 0;
        const bool changed = p.has_range ? v.field(meta, i, static_cast<int>(p.min), static_cast<int>(p.max)) : v.field(meta, i);
        if (!changed) return false;
        value = i;
        return true;
    }
    if (kind == "float") {
        float f = value.is_number() ? static_cast<float>(value.get<double>()) : 0.0f;
        ecs::FloatRange range{};
        if (p.has_range) range = ecs::FloatRange{static_cast<float>(p.min), static_cast<float>(p.max), 0.01f, "%.3f", true};
        if (!v.field(meta, f, range)) return false;
        value = f;
        return true;
    }
    if (kind == "string" || kind == "file") {
        std::string s = value.is_string() ? value.get<std::string>() : std::string();
        if (!v.field(meta, s)) return false;
        value = s;
        return true;
    }
    if (kind == "vec2") {
        core::Vec2 w{};
        if (value.is_object() && value.contains("$v")) w = core::Vec2{value["$v"][0].get<float>(), value["$v"][1].get<float>()};
        if (!v.field(meta, w)) return false;
        value = json{{"$v", {w.x, w.y, 0.0f}}};
        return true;
    }
    if (kind == "vec3" || kind == "color") {
        Vec3 w{};
        if (value.is_object() && value.contains("$v")) w = Vec3{value["$v"][0].get<float>(), value["$v"][1].get<float>(), value["$v"][2].get<float>()};
        if (!v.field(meta, w, kind == "color" ? ecs::Vec3Kind::Color : ecs::Vec3Kind::Position)) return false;
        value = json{{"$v", {w.x, w.y, w.z}}};
        return true;
    }
    if (kind == "enum") {
        int i = value.is_number() ? static_cast<int>(value.get<double>()) : 0;
        std::vector<const char*> names;
        for (const std::string& o : p.options) names.push_back(o.c_str());
        if (names.empty()) return false;
        if (!v.enumeration(meta, i, names)) return false;
        value = i;
        return true;
    }
    if (kind == "entity") {
        Uuid ref{};
        if (value.is_object() && value.contains("$uuid") && value["$uuid"].is_string()) ref = Uuid::parse(value["$uuid"].get<std::string>());
        if (!v.entity(meta, ref)) return false;
        value = ref.valid() ? json{{"$uuid", ref.toString()}} : json(nullptr);
        return true;
    }
    if (kind == "asset") {
        const auto type = static_cast<assets::AssetType>(std::atoi(p.type.c_str()));
        assets::AssetRef ref{{}, type};
        if (value.is_object() && value.contains("uuid") && value["uuid"].is_string()) ref.uuid = Uuid::parse(value["uuid"].get<std::string>());
        if (!v.asset(meta, ref, type)) return false;
        value = ref.uuid.valid() ? json{{"uuid", ref.uuid.toString()}} : json(nullptr);
        return true;
    }
    return false;
}

}  // namespace

void CppScript::reflect(ecs::PropertyVisitor& v) {
    if (v.wantsAllFields()) {
        // Guardar / leer: todo, sin depender de que la DLL este compilada.
        v.field({"script", "Script"}, script);
        v.field({"class_name", "Clase"}, class_name);
        v.field({"enabled", "Activo"}, enabled);
        ecs::listField(v, {"values", "Valores"}, values, [](CppScriptValue& p, ecs::PropertyVisitor& iv) {
            iv.field({"name", "Nombre"}, p.name);
            iv.field({"json", "Valor"}, p.json);
        });
        return;
    }
    // Inspector: las propiedades que declara la clase, con su control (el
    // archivo y la clase los pone el editor encima).
    v.field({"enabled", "Activo"}, enabled);
    const CppClassInfo* info = findCppScriptClass(class_name);
    if (info == nullptr) return;
    bool in_group = false;
    for (const CppPropertyInfo& p : info->properties) {
        if (!p.header.empty()) {
            if (in_group) v.endGroup();
            in_group = v.beginGroup(p.header.c_str(), true);
            if (!in_group) continue;
        }
        if (p.kind == "file") continue;  // el editor los dibuja (soltar imagenes, sonidos...)
        const CppScriptValue* stored = value(p.name);
        json current = json::parse(stored != nullptr ? stored->json : p.default_json, nullptr, false);
        if (current.is_discarded()) current = nullptr;
        const std::string label = p.label.empty() ? p.name : p.label;
        std::string tooltip = p.tooltip;
        if (p.kind == "entity" && !p.type.empty()) tooltip += (tooltip.empty() ? "" : "\n") + std::string("Un objeto con ") + p.type;
        const ecs::Meta meta{p.name.c_str(), label.c_str(), tooltip.empty() ? nullptr : tooltip.c_str()};
        if (p.kind == "list") {
            std::vector<json> items;
            if (current.is_array()) {
                for (const json& item : current) items.push_back(item);
            }
            CppPropertyInfo element = p;
            element.kind = p.element;
            const bool changed = ecs::listField(v, meta, items, [&](json& item, ecs::PropertyVisitor& iv) {
                const ecs::Meta item_meta{"value", "Valor", nullptr};
                reflectValue(iv, item_meta, p.element, element, item);
            });
            // Los elementos editados por dentro tambien cuentan.
            json out = json::array();
            for (const json& item : items) out.push_back(item);
            if (changed || out != current) setValue(p.name, out.dump());
            continue;
        }
        if (reflectValue(v, meta, p.kind, p, current)) setValue(p.name, current.dump());
    }
    if (in_group) v.endGroup();
}

void registerCppScriptComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("CppScript") == nullptr) registry.registerComponent<CppScript>("CppScript", "C++ Script", "Scripting");
}

std::string cppScriptHeaderTemplate(const std::string& name) {
    return "// " + name + ".h: la clase y sus propiedades (salen en el Inspector).\n"
           "#pragma once\n\n"
           "#include <cramion/Script.h>\n\n"
           "using namespace cramion;\n\n"
           "class " + name + " : public Script {\n"
           "public:\n"
           "    Property<float> velocidad{this, \"velocidad\", 45.0f, Range(0, 360), Tooltip(\"Grados por segundo\")};\n"
           "    Property<Color> color{this, \"color\", Color(1, 1, 1)};\n"
           "    Property<Entity> objetivo{this, \"objetivo\"};\n\n"
           "    void start() override;\n"
           "    void update(float dt) override;\n"
           "    void onCollisionEnter(const Collision& c) override;\n"
           "};\n";
}

std::string cppScriptTemplate(const std::string& name) {
    return "// " + name + ".cpp: corre aislado (si falla, el motor sigue y te dice donde).\n"
           "// Arrastralo (o su .h) a un objeto.\n"
           "#include \"" + name + ".h\"\n\n"
           "void " + name + "::start() {\n"
           "    Debug::log(\"Hola desde C++: \" + entity().name());\n"
           "}\n\n"
           "void " + name + "::update(float dt) {\n"
           "    entity().rotate(Vec3{0.0f, velocidad * dt, 0.0f});\n"
           "    if (objetivo.get()) entity().lookAt(objetivo->position());\n"
           "}\n\n"
           "void " + name + "::onCollisionEnter(const Collision& c) {\n"
           "    Debug::log(\"Choque con \" + c.other.name());\n"
           "}\n\n"
           "CRAMION_SCRIPT(" + name + ")\n";
}

// -----------------------------------------------------------------------------
// El proceso de los scripts
// -----------------------------------------------------------------------------

namespace {

enum class Outcome { Ok, ScriptError, Missing, HostDied, Timeout };
using RpcHandler = std::function<void(proto::Rpc, proto::Reader&, proto::Writer&)>;

struct HostProcess {
#if defined(_WIN32)
    HANDLE mapping = nullptr;
    ipc::Block* block = nullptr;
    HANDLE to_host = nullptr;
    HANDLE to_engine = nullptr;
    HANDLE job = nullptr;
    HANDLE process = nullptr;
#else
    // El bloque (memfd heredado como descriptor 4) y un socket (el 3 en el
    // proceso): un byte despierta al otro lado; cerrado = el otro se fue.
    ipc::Block* block = nullptr;
    int channel = -1;
    mutable pid_t pid = -1;
    mutable bool reaped = false;
    mutable int status = 0;
#endif
    std::uint32_t serial = 0;
    std::uint64_t rpcs = 0;

    bool running() const {
#if defined(_WIN32)
        return process != nullptr && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
#else
        if (pid <= 0 || reaped) return false;
        int st = 0;
        const pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) {
            reaped = true;
            status = st;
            return false;
        }
        return r == 0;
#endif
    }

#if !defined(_WIN32)
    void wake() const {
        const char byte = 1;
        if (channel >= 0) ::send(channel, &byte, 1, MSG_DONTWAIT | MSG_NOSIGNAL);
    }

    // Espera a que el proceso escriba (como mucho `ms`); vacia los bytes del canal.
    void waitSignal(int ms) const {
        if (channel < 0) return;
        pollfd p{channel, POLLIN, 0};
        if (poll(&p, 1, ms) <= 0) return;
        char buffer[64];
        while (recv(channel, buffer, sizeof(buffer), MSG_DONTWAIT) > 0) {
        }
    }

    static rlimit memoryLimit() {
        const rlim_t bytes = static_cast<rlim_t>(std::max(g_memory_mb.get(), 32)) << 20;
        return rlimit{bytes, bytes};
    }

    // Por que murio el proceso (el texto que dejo en el bloque y la senal).
    std::string deathMessage() const {
        std::string message = block != nullptr && block->crash[0] != 0 ? std::string(block->crash) : "el proceso de scripts se cerro";
        if (reaped && WIFSIGNALED(status)) {
            const int sig = WTERMSIG(status);
            if (sig == SIGABRT && (block == nullptr || block->crash[0] == 0)) message = "abort() o std::terminate (fallo grave del script)";
            const char* name = strsignal(sig);
            message += " [senal " + std::to_string(sig) + (name != nullptr ? std::string(": ") + name : std::string()) + "]";
        } else if (reaped && WIFEXITED(status)) {
            message += " [codigo " + std::to_string(WEXITSTATUS(status)) + "]";
        }
        return message;
    }
#endif

    void close() {
#if defined(_WIN32)
        if (process != nullptr) {
            TerminateProcess(process, 0);
            WaitForSingleObject(process, 2000);
            CloseHandle(process);
            process = nullptr;
        }
        if (job != nullptr) {
            CloseHandle(job);
            job = nullptr;
        }
        if (block != nullptr) {
            UnmapViewOfFile(block);
            block = nullptr;
        }
        for (HANDLE* h : {&mapping, &to_host, &to_engine}) {
            if (*h != nullptr) {
                CloseHandle(*h);
                *h = nullptr;
            }
        }
#else
        if (pid > 0 && !reaped) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            reaped = true;
        }
        pid = -1;
        if (block != nullptr) {
            munmap(block, sizeof(ipc::Block));
            block = nullptr;
        }
        if (channel >= 0) {
            ::close(channel);
            channel = -1;
        }
#endif
    }

    void applyMemoryLimit() {
#if defined(_WIN32)
        if (job == nullptr) return;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        limits.ProcessMemoryLimit = static_cast<SIZE_T>(std::max(g_memory_mb.get(), 32)) << 20;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
#elif defined(__linux__)
        // El tope (RLIMIT_DATA) se pone al arrancar; si cambia en marcha, tambien.
        if (!running()) return;
        const rlimit limit = memoryLimit();
        prlimit(pid, RLIMIT_DATA, &limit, nullptr);
#endif
    }

    std::uint64_t memory() const {
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS_EX counters{};
        if (running() && GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
            return counters.PrivateUsage;
        }
#elif defined(__linux__)
        // /proc/<pid>/statm: el sexto numero son las paginas de datos (+ pila).
        if (running()) {
            std::ifstream in("/proc/" + std::to_string(pid) + "/statm");
            std::uint64_t values[6] = {};
            for (std::uint64_t& v : values) in >> v;
            if (in) return values[5] * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
        }
#endif
        return 0;
    }

    // Una peticion; las RPC del script se contestan mientras se espera.
    Outcome request(ipc::Op op, const proto::Writer& w, std::string& message, std::vector<std::uint8_t>* payload,
                    int timeout_ms, const RpcHandler& handler) {
#if defined(_WIN32)
        if (block == nullptr || !running()) {
            message = "el proceso de scripts no esta en marcha";
            return Outcome::HostDied;
        }
        const std::size_t size = std::min<std::size_t>(w.bytes.size(), ipc::kDataSize);
        if (size > 0) std::memcpy(block->data, w.bytes.data(), size);
        block->op = static_cast<std::uint32_t>(op);
        block->size = static_cast<std::uint32_t>(size);
        block->turn.store(1, std::memory_order_release);
        SetEvent(to_host);
        const auto start = std::chrono::steady_clock::now();
        const int budget = timeout_ms > 0 ? timeout_ms : g_timeout_ms.get();
        const auto limit = std::chrono::milliseconds(std::max(budget, 20));
        int spin = 0;
        while (true) {
            if (block->turn.load(std::memory_order_acquire) == 2) {
                spin = 0;
                const auto reply_op = static_cast<ipc::Op>(block->op);
                if (reply_op == ipc::Op::Rpc) {
                    const std::vector<std::uint8_t> in(block->data, block->data + block->size);
                    proto::Reader r(in.data(), in.size());
                    const auto rpc_op = static_cast<proto::Rpc>(r.u32());
                    proto::Writer reply;
                    if (handler) handler(rpc_op, r, reply);
                    ++rpcs;
                    const std::size_t n = std::min<std::size_t>(reply.bytes.size(), ipc::kDataSize);
                    if (n > 0) std::memcpy(block->data, reply.bytes.data(), n);
                    block->op = static_cast<std::uint32_t>(ipc::Op::RpcReply);
                    block->size = static_cast<std::uint32_t>(n);
                    block->turn.store(1, std::memory_order_release);
                    SetEvent(to_host);
                    continue;
                }
                if (reply_op == ipc::Op::Done) {
                    proto::Reader r(block->data, block->size);
                    const auto status = static_cast<ipc::Status>(r.u32());
                    message = r.str();
                    if (payload != nullptr) {
                        const std::size_t used = block->size - r.remaining();
                        payload->assign(block->data + used, block->data + block->size);
                    }
                    block->turn.store(0, std::memory_order_release);
                    switch (status) {
                        case ipc::Status::Ok: return Outcome::Ok;
                        case ipc::Status::Missing: return Outcome::Missing;
                        default: return Outcome::ScriptError;
                    }
                }
            }
            if (++spin < 3000) {
                YieldProcessor();
            } else {
                HANDLE handles[2] = {to_engine, process};
                if (WaitForMultipleObjects(2, handles, FALSE, 1) == WAIT_OBJECT_0 + 1) {
                    DWORD code = 0;
                    GetExitCodeProcess(process, &code);
                    message = block->crash[0] != 0 ? std::string(block->crash) : "el proceso de scripts se cerro";
                    if (code == 0xC0000017 || code == 0xC000012D) message += " (sin memoria: sube script.cpp.MemoryLimitMB)";
                    if (code == 0xC0000409 && block->crash[0] == 0) message = "abort(), __fastfail o pila corrompida (fallo grave del script)";
                    char hex[32];
                    std::snprintf(hex, sizeof(hex), " [codigo 0x%08lX]", static_cast<unsigned long>(code));
                    message += hex;
                    return Outcome::HostDied;
                }
            }
            if (std::chrono::steady_clock::now() - start > limit) {
                TerminateProcess(process, 1);
                message = "tardo mas de " + std::to_string(budget) + " ms (bucle infinito?): se corto (script.cpp.TimeoutMs)";
                return Outcome::Timeout;
            }
        }
#else
        if (block == nullptr || !running()) {
            message = "el proceso de scripts no esta en marcha";
            return Outcome::HostDied;
        }
        const std::size_t size = std::min<std::size_t>(w.bytes.size(), ipc::kDataSize);
        if (size > 0) std::memcpy(block->data, w.bytes.data(), size);
        block->op = static_cast<std::uint32_t>(op);
        block->size = static_cast<std::uint32_t>(size);
        block->turn.store(1, std::memory_order_release);
        wake();
        const auto start = std::chrono::steady_clock::now();
        const int budget = timeout_ms > 0 ? timeout_ms : g_timeout_ms.get();
        const auto limit = std::chrono::milliseconds(std::max(budget, 20));
        int spin = 0;
        while (true) {
            if (block->turn.load(std::memory_order_acquire) == 2) {
                spin = 0;
                const auto reply_op = static_cast<ipc::Op>(block->op);
                if (reply_op == ipc::Op::Rpc) {
                    const std::vector<std::uint8_t> in(block->data, block->data + block->size);
                    proto::Reader r(in.data(), in.size());
                    const auto rpc_op = static_cast<proto::Rpc>(r.u32());
                    proto::Writer reply;
                    if (handler) handler(rpc_op, r, reply);
                    ++rpcs;
                    const std::size_t n = std::min<std::size_t>(reply.bytes.size(), ipc::kDataSize);
                    if (n > 0) std::memcpy(block->data, reply.bytes.data(), n);
                    block->op = static_cast<std::uint32_t>(ipc::Op::RpcReply);
                    block->size = static_cast<std::uint32_t>(n);
                    block->turn.store(1, std::memory_order_release);
                    wake();
                    continue;
                }
                if (reply_op == ipc::Op::Done) {
                    proto::Reader r(block->data, block->size);
                    const auto st = static_cast<ipc::Status>(r.u32());
                    message = r.str();
                    if (payload != nullptr) {
                        const std::size_t used = block->size - r.remaining();
                        payload->assign(block->data + used, block->data + block->size);
                    }
                    block->turn.store(0, std::memory_order_release);
                    switch (st) {
                        case ipc::Status::Ok: return Outcome::Ok;
                        case ipc::Status::Missing: return Outcome::Missing;
                        default: return Outcome::ScriptError;
                    }
                }
            }
            if (++spin < 3000) {
                std::this_thread::yield();
            } else {
                waitSignal(1);
                if (!running()) {
                    message = deathMessage();
                    return Outcome::HostDied;
                }
            }
            if (std::chrono::steady_clock::now() - start > limit) {
                kill(pid, SIGKILL);
                message = "tardo mas de " + std::to_string(budget) + " ms (bucle infinito?): se corto (script.cpp.TimeoutMs)";
                return Outcome::Timeout;
            }
        }
#endif
    }

    // Arranca el proceso y carga la DLL. Las clases, en `classes`.
    bool launch(const std::filesystem::path& host_exe, const std::filesystem::path& dll, std::uint32_t new_serial,
                std::string& error, std::vector<std::string>& classes, const RpcHandler& handler) {
#if defined(_WIN32)
        close();
        serial = new_serial;
        std::error_code ec;
        if (dll.empty() || !std::filesystem::exists(dll, ec)) {
            error = "no hay DLL de scripts de C++ compilada";
            return false;
        }
        if (!std::filesystem::exists(host_exe, ec)) {
            error = "falta " + utf8(host_exe.filename()) + " junto al ejecutable";
            return false;
        }
        const std::string name = ipc::blockName(GetCurrentProcessId(), serial);
        mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(ipc::Block), name.c_str());
        to_host = CreateEventA(nullptr, FALSE, FALSE, (name + "_h").c_str());
        to_engine = CreateEventA(nullptr, FALSE, FALSE, (name + "_e").c_str());
        if (mapping == nullptr || to_host == nullptr || to_engine == nullptr) {
            error = "no se pudo crear el canal con el proceso de scripts";
            close();
            return false;
        }
        block = static_cast<ipc::Block*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ipc::Block)));
        block->turn.store(0);
        block->host_ready = 0;
        block->crash[0] = 0;
        job = CreateJobObjectW(nullptr, nullptr);
        applyMemoryLimit();
        std::wstring cmd = L"\"" + host_exe.wstring() + L"\" " + wide(name) + L" " + std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
                            dll.parent_path().wstring().c_str(), &si, &pi)) {
            error = "no se pudo arrancar " + utf8(host_exe.filename()) + " (error " + std::to_string(GetLastError()) + ")";
            close();
            return false;
        }
        AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        process = pi.hProcess;
        const auto t0 = std::chrono::steady_clock::now();
        while (block->host_ready == 0) {
            if (!running() || std::chrono::steady_clock::now() - t0 > std::chrono::seconds(10)) {
                error = "el proceso de scripts no arranco";
                close();
                return false;
            }
            WaitForSingleObject(to_engine, 5);
        }
#else
        close();
        serial = new_serial;
        reaped = false;
        status = 0;
        std::error_code ec;
        if (dll.empty() || !std::filesystem::exists(dll, ec)) {
            error = "no hay biblioteca de scripts de C++ compilada";
            return false;
        }
        if (!std::filesystem::exists(host_exe, ec)) {
            error = "falta " + utf8(host_exe.filename()) + " junto al ejecutable";
            return false;
        }
        // El bloque: memoria anonima compartida que el proceso hereda.
        int memory = -1;
#if defined(__linux__)
        memory = memfd_create("cramion_scripts", MFD_CLOEXEC);
#else
        {
            const std::string shm = "/cramion_" + std::to_string(getpid()) + "_" + std::to_string(serial);
            memory = shm_open(shm.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
            if (memory >= 0) shm_unlink(shm.c_str());
        }
#endif
        int pair[2] = {-1, -1};
        if (memory < 0 || ftruncate(memory, sizeof(ipc::Block)) != 0 || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
            if (memory >= 0) ::close(memory);
            error = "no se pudo crear el canal con el proceso de scripts";
            return false;
        }
        void* mapped = mmap(nullptr, sizeof(ipc::Block), PROT_READ | PROT_WRITE, MAP_SHARED, memory, 0);
        if (mapped == MAP_FAILED) {
            ::close(memory);
            ::close(pair[0]);
            ::close(pair[1]);
            error = "no se pudo crear el canal con el proceso de scripts";
            return false;
        }
        block = static_cast<ipc::Block*>(mapped);
        block->turn.store(0);
        block->host_ready = 0;
        block->crash[0] = 0;
        channel = pair[0];
        // En el hijo solo hay llamadas seguras despues de fork (el motor tiene hilos).
        const int child_channel = highFd(pair[1]);
        const int child_memory = highFd(memory);
        const std::string exe = utf8(host_exe);
        const std::string folder = utf8(dll.parent_path());
        std::string arg_name = ipc::blockName(static_cast<std::uint32_t>(getpid()), serial);
        std::string arg_pid = std::to_string(getpid());
        std::string arg_exe = exe;
        char* argv[] = {arg_exe.data(), arg_name.data(), arg_pid.data(), nullptr};
        const rlimit limit = memoryLimit();
        const pid_t child = fork();
        if (child == 0) {
            const int dev_null = open("/dev/null", O_RDONLY);
            if (dev_null >= 0) dup2(dev_null, 0);
            if (dup2(child_channel, 3) < 0 || dup2(child_memory, 4) < 0) _exit(126);
            if (chdir(folder.c_str()) != 0) {
                // sigue en la carpeta del motor
            }
            setrlimit(RLIMIT_DATA, &limit);
            execv(exe.c_str(), argv);
            _exit(127);
        }
        ::close(child_channel);
        ::close(child_memory);
        if (child < 0) {
            error = "no se pudo arrancar " + utf8(host_exe.filename()) + " (" + std::strerror(errno) + ")";
            close();
            return false;
        }
        pid = child;
        const auto t0 = std::chrono::steady_clock::now();
        while (block->host_ready == 0) {
            if (!running() || std::chrono::steady_clock::now() - t0 > std::chrono::seconds(10)) {
                error = "el proceso de scripts no arranco";
                if (reaped && WIFEXITED(status) && WEXITSTATUS(status) == 127) error += " (no se pudo ejecutar " + exe + ")";
                close();
                return false;
            }
            waitSignal(5);
        }
#endif
        proto::Writer w;
        w.str(utf8(dll)).u32(serial);
        std::vector<std::uint8_t> payload;
        std::string message;
        // Cargar (constructores estaticos y simbolos): con mas margen.
        if (request(ipc::Op::Load, w, message, &payload, std::max(g_timeout_ms.get(), 30000), handler) != Outcome::Ok) {
            error = "no se pudo cargar la DLL de scripts: " + message;
            close();
            return false;
        }
        proto::Reader r(payload.data(), payload.size());
        const std::uint32_t n = r.u32();
        classes.clear();
        for (std::uint32_t i = 0; i < n && r.ok(); ++i) classes.push_back(r.str());
        return true;
    }
};

std::atomic<std::uint32_t> g_next_serial{1};

}  // namespace

// -----------------------------------------------------------------------------
// Estado
// -----------------------------------------------------------------------------

struct CppScriptSystem::Impl {
    std::filesystem::path assets_root;
    std::filesystem::path build_folder;
    std::filesystem::path sdk_folder;
    std::filesystem::path host_exe;
    std::filesystem::path dll_path;
    bool prebuilt = false;
    physics::PhysicsSystem* physics = nullptr;
    int physics_listener = -1;
    const dm::Input* input = nullptr;
    ScriptSystem* lua = nullptr;
    std::function<std::string(const Uuid&)> asset_path;
    std::vector<ScriptError> errors;
    std::vector<std::string> classes;

    // Compilacion en segundo plano.
    std::thread compile_thread;
    std::atomic<bool> compiling{false};
    std::mutex result_mutex;
    std::optional<CppCompileResult> pending_result;

    // Play.
    struct Instance {
        std::uint32_t id = 0;
        std::string cls;
        bool created = false;
        bool started = false;
    };
    HostProcess host;
    ecs::World* world = nullptr;
    bool playing = false;
    std::unordered_map<entt::entity, Instance> instances;
    std::vector<std::tuple<entt::entity, std::string, std::string>> net_vars;  // OnNetVar pendientes
    std::vector<std::tuple<entt::entity, std::string, std::string>> bt_messages;  // de los Behavior Trees
    std::unordered_set<entt::entity> faulted;
    std::vector<entt::entity> pending_destroy;
    std::vector<physics::PhysicsEvent> events;
    std::vector<std::pair<std::uint64_t, std::string>> callbacks;
    std::uint32_t next_id = 1;
    int restarts = 0;
    // Recarga en caliente: el estado de cada instancia hasta que se cree con la DLL nueva.
    struct ReloadState {
        std::string json;
        bool started = false;
    };
    std::unordered_map<entt::entity, ReloadState> reload_state;
    int reloads = 0;
    bool load_failed = false;
    std::uint64_t calls = 0;
    double frame_ms = 0.0;
    float time = 0.0f;
    std::uint64_t frame = 0;
    float fixed_step = 1.0f / 60.0f;
    int memory_listener = -1;
    // Vivo mientras exista este sistema: el sumidero de callbacks que se da a
    // Lua lo mira (Lua puede durar mas o menos que nosotros).
    std::shared_ptr<int> alive = std::make_shared<int>(0);

    void error(const std::string& file, int line, const std::string& message) {
        errors.push_back(ScriptError{file, line, message});
        std::cerr << "[C++] " << (file.empty() ? "" : file + (line > 0 ? ":" + std::to_string(line) : "") + ": ") << message
                  << std::endl;
    }

    std::string entityName(entt::entity h) const {
        return world != nullptr && world->valid(h) ? world->wrap(h).name() : std::string("?");
    }

    void closeHost() {
        host.close();
        for (auto& [h, inst] : instances) inst.created = inst.started = false;
        callbacks.clear();
    }

    RpcHandler handler() {
        return [this](proto::Rpc op, proto::Reader& r, proto::Writer& out) { handleRpc(op, r, out); };
    }

    bool launch() {
        std::string message;
        if (!host.launch(host_exe, dll_path, g_next_serial++, message, classes, handler())) {
            error({}, 0, message);
            load_failed = true;
            return false;
        }
        return true;
    }

    Outcome request(ipc::Op op, const proto::Writer& w, std::string& message, std::vector<std::uint8_t>* payload = nullptr) {
        return host.request(op, w, message, payload, 0, handler());
    }

    bool callEvent(entt::entity h, Instance& inst, proto::Event event, const proto::EventArgs& args) {
        proto::Writer w;
        w.u32(inst.id).u32(static_cast<std::uint32_t>(event)).raw(&args, sizeof(args));
        std::string message;
        const Outcome o = request(ipc::Op::Call, w, message);
        if (o == Outcome::Ok) return true;
        fault(h, inst, eventName(event), o, message);
        return false;
    }

    static const char* eventName(proto::Event e) {
        switch (e) {
            case proto::Event::Awake: return "awake()";
            case proto::Event::Start: return "start()";
            case proto::Event::Update: return "update()";
            case proto::Event::FixedUpdate: return "fixedUpdate()";
            case proto::Event::LateUpdate: return "lateUpdate()";
            case proto::Event::Destroy: return "onDestroy()";
            case proto::Event::CollisionEnter: return "onCollisionEnter()";
            case proto::Event::CollisionStay: return "onCollisionStay()";
            case proto::Event::CollisionExit: return "onCollisionExit()";
            case proto::Event::TriggerEnter: return "onTriggerEnter()";
            case proto::Event::TriggerStay: return "onTriggerStay()";
            case proto::Event::TriggerExit: return "onTriggerExit()";
            default: return "?";
        }
    }

    // El archivo y la linea que da el proceso ("... (Jugador.cpp:12)").
    void reportFault(const std::string& what, const std::string& message) {
        static const std::regex where(R"(\(([^():]+\.(?:cpp|h|hpp|cc|cxx)):(\d+)\))");
        std::smatch m;
        std::string file;
        int line = 0;
        if (std::regex_search(message, m, where)) {
            const std::string name = m[1];
            line = std::stoi(m[2]);
            std::error_code ec;
            for (std::filesystem::recursive_directory_iterator it(assets_root, ec), end; !ec && it != end; it.increment(ec)) {
                if (it->is_regular_file(ec) && utf8(it->path().filename()) == name) {
                    file = generic(std::filesystem::relative(it->path(), assets_root, ec));
                    break;
                }
            }
            if (file.empty()) file = name;
        }
        error(file, line, what + ": " + message);
    }

    void fault(entt::entity h, Instance& inst, const char* during, Outcome o, const std::string& message) {
        faulted.insert(h);
        const std::string who = inst.cls + " (" + entityName(h) + ") en " + during;
        if (o == Outcome::ScriptError || o == Outcome::Missing) {
            reportFault(who, message + "  -> script desactivado hasta el siguiente Play");
            proto::Writer w;
            w.u32(inst.id);
            std::string ignored;
            if (host.running() && inst.created) request(ipc::Op::Destroy, w, ignored);
            inst.created = false;
            return;
        }
        reportFault(who, (o == Outcome::Timeout ? "" : "el proceso de scripts cayo: ") + message +
                             "  -> script desactivado; los demas scripts se reinician");
        closeHost();
        ++restarts;
    }

    // --- RPC: lo que piden los scripts ---
    ecs::Entity entity(std::uint64_t id) const {
        const entt::entity h = entityOf(id);
        return world != nullptr && h != entt::null && world->registry().valid(h) ? world->wrap(h) : ecs::Entity{};
    }
    static void writeVec(proto::Writer& w, const Vec3& v) {
        const float f[3] = {v.x, v.y, v.z};
        w.vec3(f);
    }
    static Vec3 readVec(proto::Reader& r) {
        float f[3] = {0, 0, 0};
        r.vec3(f);
        return Vec3{f[0], f[1], f[2]};
    }

    float axis(const std::string& name_in) const {
        if (input == nullptr) return 0.0f;
        const std::string name = lower(name_in);
        const auto down = [&](dm::Key a, dm::Key b) { return input->isKeyDown(a) || input->isKeyDown(b); };
        if (name == "horizontal") {
            const float v = (down(dm::Key::D, dm::Key::Right) ? 1.0f : 0.0f) - (down(dm::Key::A, dm::Key::Left) ? 1.0f : 0.0f) +
                            input->virtualStickX() + input->gamepadAxis(dm::GamepadAxis::LeftX);
            return std::clamp(v, -1.0f, 1.0f);
        }
        if (name == "vertical") {
            const float v = (down(dm::Key::W, dm::Key::Up) ? 1.0f : 0.0f) - (down(dm::Key::S, dm::Key::Down) ? 1.0f : 0.0f) +
                            input->virtualStickY() + input->gamepadAxis(dm::GamepadAxis::LeftY);
            return std::clamp(v, -1.0f, 1.0f);
        }
        if (name == "mouse x") return input->mouseDeltaX();
        if (name == "mouse y") return -input->mouseDeltaY();
        if (name == "mouse scrollwheel") return input->scrollY();
        return 0.0f;
    }

    // Navega "a.b[2].c" dentro de un JSON.
    static json* walk(json& root, const std::string& path, bool create) {
        json* node = &root;
        std::size_t i = 0;
        while (i < path.size()) {
            if (path[i] == '.') {
                ++i;
                continue;
            }
            if (path[i] == '[') {
                const std::size_t close = path.find(']', i);
                if (close == std::string::npos) return nullptr;
                const int index = std::atoi(path.substr(i + 1, close - i - 1).c_str());
                if (!node->is_array() || index < 0 || static_cast<std::size_t>(index) >= node->size()) return nullptr;
                node = &(*node)[static_cast<std::size_t>(index)];
                i = close + 1;
                continue;
            }
            std::size_t end = path.find_first_of(".[", i);
            if (end == std::string::npos) end = path.size();
            const std::string key = path.substr(i, end - i);
            if (!node->is_object()) return nullptr;
            if (!node->contains(key)) {
                if (!create) return nullptr;
                (*node)[key] = nullptr;
            }
            node = &(*node)[key];
            i = end;
        }
        return node;
    }

    // Las referencias del Inspector a lo que entiende el script: entidades
    // ({"$uuid"} -> {"$e": id}) y assets (con su ruta).
    json resolveValue(const json& v) const {
        if (v.is_array()) {
            json out = json::array();
            for (const json& item : v) out.push_back(resolveValue(item));
            return out;
        }
        if (v.is_object()) {
            if (v.contains("$uuid") && v["$uuid"].is_string()) {
                const ecs::Entity e = world != nullptr ? world->find(Uuid::parse(v["$uuid"].get<std::string>())) : ecs::Entity{};
                return json{{"$e", e.valid() ? idOf(e.handle()) : 0}};
            }
            if (v.contains("uuid") && v["uuid"].is_string() && v.size() <= 2) {
                const std::string uuid = v["uuid"].get<std::string>();
                const std::string path = asset_path ? asset_path(Uuid::parse(uuid)) : std::string();
                return json{{"uuid", uuid}, {"path", path}};
            }
        }
        return v;
    }

    void handleRpc(proto::Rpc op, proto::Reader& r, proto::Writer& out) {
        // CRAMION_CPP_TRACE=1: cada llamada de los scripts en la consola (depurar).
        static const bool trace = std::getenv("CRAMION_CPP_TRACE") != nullptr;
        if (trace) std::cerr << "[C++ rpc] " << static_cast<int>(op) << " (" << r.remaining() << " bytes)" << std::endl;
        if (world == nullptr) return;
        ecs::World& w = *world;
        ++calls;
        switch (op) {
            case proto::Rpc::Log: {
                const std::uint32_t level = r.u32();
                const std::string text = r.str();
                (level >= 1 ? std::cerr : std::cout) << "[C++] " << (level == 1 ? "Aviso: " : (level >= 2 ? "Error: " : ""))
                                                      << text << std::endl;
                break;
            }
            case proto::Rpc::Batch: {
                const std::uint32_t n = r.u32();
                proto::Writer ignored;
                for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
                    const auto sub_op = static_cast<proto::Rpc>(r.u32());
                    const std::uint32_t size = r.u32();
                    const std::uint8_t* data = r.bytes(size);
                    if (data == nullptr) break;
                    proto::Reader sub(data, size);
                    if (sub_op != proto::Rpc::Batch) handleRpc(sub_op, sub, ignored);
                }
                --calls;
                break;
            }
            case proto::Rpc::Api:
            case proto::Rpc::ApiSend: {
                const std::string request = r.str();
                const std::string reply = lua != nullptr ? lua->bridgeCall(request)
                                                         : std::string(R"j({"ok":false,"error":"sin API: el juego no esta en marcha"})j");
                if (op == proto::Rpc::Api) {
                    out.str(reply);
                } else if (reply.find("\"ok\":false") != std::string::npos) {
                    std::cerr << "[C++] Api::send: " << reply << std::endl;
                }
                break;
            }
            case proto::Rpc::GetPositions: {
                const std::uint32_t n = r.u32();
                for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
                    const ecs::Entity e = entity(r.u64());
                    writeVec(out, e.valid() ? e.worldPosition() : Vec3{});
                }
                break;
            }
            case proto::Rpc::SetPositions: {
                const std::uint32_t n = r.u32();
                for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
                    ecs::Entity e = entity(r.u64());
                    const Vec3 p = readVec(r);
                    if (e.valid() && std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) e.setWorldPosition(p);
                }
                break;
            }
            case proto::Rpc::FindEntity: out.u64(idOf(w.findByName(r.str()).handle())); break;
            case proto::Rpc::FindWithTag: {
                const std::string tag = r.str();
                std::uint64_t found = 0;
                w.forEachDepthFirst([&](ecs::Entity e) {
                    if (found == 0 && e.tag() == tag) found = idOf(e.handle());
                });
                out.u64(found);
                break;
            }
            case proto::Rpc::CreateEntity: out.u64(idOf(w.create(r.str()).handle())); break;
            case proto::Rpc::Destroy: {
                const ecs::Entity e = entity(r.u64());
                if (e.valid()) pending_destroy.push_back(e.handle());
                break;
            }
            case proto::Rpc::Valid: out.u32(entity(r.u64()).valid() ? 1 : 0); break;
            case proto::Rpc::GetName: {
                const ecs::Entity e = entity(r.u64());
                out.str(e.valid() ? e.name() : std::string());
                break;
            }
            case proto::Rpc::SetName: {
                ecs::Entity e = entity(r.u64());
                const std::string n = r.str();
                if (e.valid()) e.setName(n);
                break;
            }
            case proto::Rpc::GetTag: {
                const ecs::Entity e = entity(r.u64());
                out.str(e.valid() ? e.tag() : std::string());
                break;
            }
            case proto::Rpc::GetVec: {
                const ecs::Entity e = entity(r.u64());
                const auto which = static_cast<proto::VecGet>(r.u32());
                Vec3 v{};
                if (e.valid()) {
                    switch (which) {
                        case proto::VecGet::WorldPosition: v = e.worldPosition(); break;
                        case proto::VecGet::LocalPosition: v = e.localPosition(); break;
                        case proto::VecGet::Euler: v = e.localEulerDegrees(); break;
                        case proto::VecGet::Scale: v = e.localScale(); break;
                        case proto::VecGet::Forward: v = e.forward(); break;
                        case proto::VecGet::Right: v = e.right(); break;
                        case proto::VecGet::Up: v = e.up(); break;
                        case proto::VecGet::Velocity: v = physics != nullptr ? physics->linearVelocity(e) : Vec3{}; break;
                        case proto::VecGet::AngularVelocity: v = physics != nullptr ? physics->angularVelocity(e) : Vec3{}; break;
                    }
                }
                writeVec(out, v);
                break;
            }
            case proto::Rpc::SetVec: {
                ecs::Entity e = entity(r.u64());
                const auto which = static_cast<proto::VecSet>(r.u32());
                const Vec3 v = readVec(r);
                if (!e.valid() || !std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) break;
                switch (which) {
                    case proto::VecSet::WorldPosition: e.setWorldPosition(v); break;
                    case proto::VecSet::LocalPosition: e.setLocalPosition(v); break;
                    case proto::VecSet::Euler: e.setLocalEulerDegrees(v); break;
                    case proto::VecSet::Scale: e.setLocalScale(v); break;
                    case proto::VecSet::Velocity:
                        if (physics != nullptr) physics->setLinearVelocity(e, v);
                        break;
                    case proto::VecSet::AngularVelocity:
                        if (physics != nullptr) physics->setAngularVelocity(e, v);
                        break;
                    case proto::VecSet::Translate: e.setWorldPosition(e.worldPosition() + v); break;
                    case proto::VecSet::Rotate: e.setLocalEulerDegrees(e.localEulerDegrees() + v); break;
                    case proto::VecSet::LookAt: {
                        const Vec3 d = v - e.worldPosition();
                        const float len = core::length(d);
                        if (len < 1e-5f) break;
                        const Vec3 dir = d * (1.0f / len);
                        constexpr float kDeg = 57.2957795f;
                        e.setLocalEulerDegrees(Vec3{std::asin(std::clamp(dir.y, -1.0f, 1.0f)) * kDeg, std::atan2(-dir.x, -dir.z) * kDeg, 0.0f});
                        break;
                    }
                }
                break;
            }
            case proto::Rpc::GetActive: {
                const ecs::Entity e = entity(r.u64());
                out.u32(e.valid() && e.activeSelf() ? 1 : 0);
                break;
            }
            case proto::Rpc::SetActive: {
                ecs::Entity e = entity(r.u64());
                const bool on = r.u32() != 0;
                if (e.valid()) e.setActive(on);
                break;
            }
            case proto::Rpc::Parent: {
                const ecs::Entity e = entity(r.u64());
                out.u64(e.valid() ? idOf(e.parent().handle()) : 0);
                break;
            }
            case proto::Rpc::HasComponent:
            case proto::Rpc::AddComponent: {
                ecs::Entity e = entity(r.u64());
                const std::string type = r.str();
                const ecs::ComponentType* t = ecs::ComponentRegistry::instance().find(type);
                bool ok = false;
                if (e.valid() && t != nullptr) {
                    if (op == proto::Rpc::AddComponent && !t->has(w, e.handle()) && t->addable) t->add(w, e.handle());
                    ok = t->has(w, e.handle());
                }
                out.u32(ok ? 1 : 0);
                break;
            }
            case proto::Rpc::GetComponent: {
                ecs::Entity e = entity(r.u64());
                const std::string type = r.str();
                const ecs::ComponentType* t = ecs::ComponentRegistry::instance().find(type);
                out.str(e.valid() && t != nullptr && t->has(w, e.handle()) ? ecs::componentToJson(w, e, type) : std::string());
                break;
            }
            case proto::Rpc::SetComponent: {
                ecs::Entity e = entity(r.u64());
                const std::string type = r.str();
                const std::string fields = r.str();
                std::string err;
                const bool ok = e.valid() && ecs::componentFromJson(w, e, type, fields, &err);
                if (!ok && !err.empty()) std::cerr << "[C++] setComponent(" << type << "): " << err << std::endl;
                out.u32(ok ? 1 : 0);
                break;
            }
            case proto::Rpc::GetField:
            case proto::Rpc::SetField: {
                ecs::Entity e = entity(r.u64());
                const std::string type = r.str();
                const std::string path = r.str();
                const std::string value = op == proto::Rpc::SetField ? r.str() : std::string();
                const ecs::ComponentType* t = ecs::ComponentRegistry::instance().find(type);
                if (!e.valid() || t == nullptr || !t->has(w, e.handle())) {
                    out.u32(0);
                    if (op == proto::Rpc::GetField) out.str({});
                    break;
                }
                json doc = json::parse(ecs::componentToJson(w, e, type), nullptr, false);
                json* node = doc.is_discarded() ? nullptr : walk(doc, path, op == proto::Rpc::SetField);
                if (op == proto::Rpc::GetField) {
                    out.u32(node != nullptr ? 1 : 0);
                    out.str(node != nullptr ? node->dump() : std::string());
                    break;
                }
                bool ok = false;
                if (node != nullptr) {
                    json parsed = json::parse(value, nullptr, false);
                    if (!parsed.is_discarded() && parsed.is_object() && parsed.contains("$v") && node->is_object()) {
                        // Vec3 del script -> {x, y, z} del componente.
                        (*node)["x"] = parsed["$v"][0];
                        (*node)["y"] = parsed["$v"][1];
                        (*node)["z"] = parsed["$v"][2];
                    } else {
                        *node = parsed.is_discarded() ? json(value) : parsed;
                    }
                    ok = ecs::componentFromJson(w, e, type, doc.dump());
                }
                out.u32(ok ? 1 : 0);
                break;
            }
            case proto::Rpc::Key: {
                const dm::Key key = keyByName(r.str());
                const std::uint32_t mode = r.u32();
                bool v = false;
                if (input != nullptr && static_cast<int>(key) != 0) {
                    v = mode == 0 ? input->isKeyDown(key) : (mode == 1 ? input->isKeyPressed(key) : input->isKeyReleased(key));
                }
                out.u32(v ? 1 : 0);
                break;
            }
            case proto::Rpc::MouseButton: {
                const auto button = static_cast<dm::MouseButton>(std::min<std::uint32_t>(r.u32(), 4));
                const std::uint32_t mode = r.u32();
                bool v = false;
                if (input != nullptr) {
                    v = mode == 0 ? input->isMouseButtonDown(button)
                                  : (mode == 1 ? input->isMouseButtonPressed(button) : input->isMouseButtonReleased(button));
                }
                out.u32(v ? 1 : 0);
                break;
            }
            case proto::Rpc::Axis: out.f32(axis(r.str())); break;
            case proto::Rpc::Mouse:
                out.f32(input != nullptr ? input->mouseX() : 0.0f)
                    .f32(input != nullptr ? input->mouseY() : 0.0f)
                    .f32(input != nullptr ? input->mouseDeltaX() : 0.0f)
                    .f32(input != nullptr ? input->mouseDeltaY() : 0.0f)
                    .f32(input != nullptr ? input->scrollY() : 0.0f);
                break;
            case proto::Rpc::Raycast: {
                const Vec3 origin = readVec(r);
                const Vec3 dir = readVec(r);
                const float max = r.f32();
                const ecs::Entity ignore = entity(r.u64());
                physics::RaycastHit hit;
                physics::QueryFilter filter;
                filter.ignore = ignore;
                const bool touched = physics != nullptr && physics->raycast(origin, dir, max, hit, filter);
                out.u32(touched ? 1 : 0);
                if (touched) {
                    out.u64(idOf(hit.entity.handle()));
                    writeVec(out, hit.point);
                    writeVec(out, hit.normal);
                    out.f32(hit.distance);
                }
                break;
            }
            case proto::Rpc::CharMove: {
                ecs::Entity e = entity(r.u64());
                const Vec3 d = readVec(r);
                out.u32(e.valid() && physics != nullptr ? physics->moveCharacter(w, e, d) : 0);
                break;
            }
            case proto::Rpc::CharGrounded: {
                const ecs::Entity e = entity(r.u64());
                out.u32(e.valid() && physics != nullptr && physics->characterState(e).grounded ? 1 : 0);
                break;
            }
            case proto::Rpc::CharInput: {
                const ecs::Entity e = entity(r.u64());
                const Vec3 d = readVec(r);
                const bool run = r.u32() != 0;
                if (e.valid() && physics != nullptr) physics->setCharacterInput(e, d, run);
                break;
            }
            case proto::Rpc::CharJump: {
                const ecs::Entity e = entity(r.u64());
                const float height = r.f32();
                out.u32(e.valid() && physics != nullptr && physics->characterJump(e, height) ? 1 : 0);
                break;
            }
            case proto::Rpc::CharCrouch: {
                const ecs::Entity e = entity(r.u64());
                const bool on = r.u32() != 0;
                if (e.valid() && physics != nullptr) physics->setCharacterCrouch(e, on);
                break;
            }
            case proto::Rpc::AddForce: {
                const ecs::Entity e = entity(r.u64());
                const Vec3 f = readVec(r);
                const std::uint32_t mode = r.u32();
                if (!e.valid() || physics == nullptr) break;
                if (physics->isCharacter(e)) {
                    physics->addCharacterVelocity(e, f);
                } else {
                    physics->addForce(e, f, static_cast<physics::ForceMode>(std::min<std::uint32_t>(mode, 3)));
                }
                break;
            }
            case proto::Rpc::CVarGet: {
                const cvar::CVarBase* c = cvar::Registry::instance().find(r.str());
                out.u32(c != nullptr ? 1 : 0).str(c != nullptr ? c->toString() : std::string());
                break;
            }
            case proto::Rpc::CVarSet: {
                const std::string name = r.str();
                const std::string value = r.str();
                std::string err;
                const bool ok = cvar::Registry::instance().set(name, value, &err);
                out.u32(ok ? 1 : 0).str(err);
                break;
            }
            case proto::Rpc::CVarRegister: {
                const std::string name = r.str();
                const std::uint32_t type = r.u32();
                const std::string def = r.str();
                const std::string desc = r.str();
                const std::uint32_t flags = r.u32();
                const cvar::CVarBase* c = cvar::Registry::instance().createDynamic(
                    name, static_cast<cvar::Type>(std::min<std::uint32_t>(type, 3)), def, desc,
                    (flags & (cvar::ReadOnly | cvar::Saved | cvar::Cheat)) | cvar::Script);
                out.u32(c != nullptr ? 1 : 0);
                break;
            }
            default: break;
        }
    }

    // --- Ciclo de vida ---
    proto::EventArgs args(float dt) const {
        proto::EventArgs a{};
        a.delta_time = dt;
        a.time = time;
        a.fixed_delta = fixed_step;
        a.frame = frame;
        return a;
    }

    // Crea las instancias que falten y destruye las que sobren. false si no hay proceso.
    bool sync(ecs::World& w) {
        auto& registry = w.registry();
        for (auto it = instances.begin(); it != instances.end();) {
            const entt::entity h = it->first;
            const CppScript* s = registry.valid(h) ? registry.try_get<CppScript>(h) : nullptr;
            const bool keep = s != nullptr && s->enabled && w.wrap(h).activeInHierarchy() && s->class_name == it->second.cls &&
                              !faulted.contains(h);
            if (keep) {
                ++it;
                continue;
            }
            if (it->second.created && host.running() && !faulted.contains(h)) {
                callEvent(h, it->second, proto::Event::Destroy, args(0.0f));
                proto::Writer msg;
                msg.u32(it->second.id);
                std::string ignored;
                request(ipc::Op::Destroy, msg, ignored);
            }
            it = instances.erase(it);
        }
        bool any = false;
        for (const entt::entity h : registry.view<CppScript>()) {
            const CppScript& s = registry.get<CppScript>(h);
            if (!s.enabled || s.class_name.empty() || faulted.contains(h) || !w.wrap(h).activeInHierarchy()) continue;
            any = true;
            if (!instances.contains(h)) instances[h] = Instance{next_id++, s.class_name, false, false};
        }
        if (!any) return false;
        if (!host.running()) {
            if (load_failed || restarts > g_max_restarts.get()) return false;
            if (!launch()) return false;
        }
        std::vector<entt::entity> order;
        for (auto& [h, inst] : instances) {
            if (!inst.created) order.push_back(h);
        }
        for (const entt::entity h : order) {
            auto it = instances.find(h);
            if (it == instances.end() || !host.running()) break;
            Instance& inst = it->second;
            const CppScript& s = registry.get<CppScript>(h);
            json values = json::object();
            for (const CppScriptValue& v : s.values) {
                const json parsed = json::parse(v.json, nullptr, false);
                values[v.name] = parsed.is_discarded() ? json(v.json) : resolveValue(parsed);
            }
            proto::Writer msg;
            msg.u32(inst.id).str(inst.cls).u64(idOf(h)).str(values.dump());
            std::string message;
            const Outcome o = request(ipc::Op::Create, msg, message);
            if (o != Outcome::Ok) {
                if (o == Outcome::Missing) {
                    std::string known;
                    for (const std::string& c : classes) known += (known.empty() ? "" : ", ") + c;
                    message += known.empty() ? " (la DLL no tiene clases)" : " (hay: " + known + ")";
                }
                fault(h, inst, "crear", o, message);
                continue;
            }
            inst.created = true;
            // Recarga en caliente: el estado de antes en vez de awake()/start().
            const auto saved = reload_state.find(h);
            if (saved != reload_state.end()) {
                const bool was_started = saved->second.started;
                proto::Writer restore;
                restore.u32(inst.id).str(saved->second.json);
                reload_state.erase(saved);
                std::string restore_message;
                const Outcome ro = request(ipc::Op::Restore, restore, restore_message);
                if (ro != Outcome::Ok) {
                    fault(h, inst, "onAfterReload()", ro, restore_message);
                    continue;
                }
                inst.started = was_started;
                continue;
            }
            callEvent(h, inst, proto::Event::Awake, args(0.0f));
        }
        return host.running();
    }

    void dispatchPhysics() {
        std::vector<physics::PhysicsEvent> list;
        list.swap(events);
        for (const physics::PhysicsEvent& ev : list) {
            proto::Event kind = proto::Event::CollisionEnter;
            switch (ev.type) {
                case physics::PhysicsEventType::CollisionEnter: kind = proto::Event::CollisionEnter; break;
                case physics::PhysicsEventType::CollisionStay: kind = proto::Event::CollisionStay; break;
                case physics::PhysicsEventType::CollisionExit: kind = proto::Event::CollisionExit; break;
                case physics::PhysicsEventType::TriggerEnter: kind = proto::Event::TriggerEnter; break;
                case physics::PhysicsEventType::TriggerStay: kind = proto::Event::TriggerStay; break;
                case physics::PhysicsEventType::TriggerExit: kind = proto::Event::TriggerExit; break;
                default: continue;
            }
            for (int side = 0; side < 2; ++side) {
                const physics::PhysicsEvent e = side == 0 ? ev : ev.flipped();
                auto it = instances.find(e.a.handle());
                if (it == instances.end() || !it->second.created || !it->second.started || !host.running()) continue;
                proto::EventArgs a = args(0.0f);
                a.other = idOf(e.b.handle());
                const float p[3] = {e.point.x, e.point.y, e.point.z};
                const float n[3] = {e.normal.x, e.normal.y, e.normal.z};
                const float v[3] = {e.relative_velocity.x, e.relative_velocity.y, e.relative_velocity.z};
                std::memcpy(a.point, p, sizeof(p));
                std::memcpy(a.normal, n, sizeof(n));
                std::memcpy(a.relative_velocity, v, sizeof(v));
                callEvent(e.a.handle(), it->second, kind, a);
            }
        }
    }

    // Los callbacks que la API de Lua llamo (botones, red, acciones...).
    void dispatchCallbacks() {
        std::vector<std::pair<std::uint64_t, std::string>> list;
        list.swap(callbacks);
        for (const auto& [id, args_json] : list) {
            if ((id >> 32) != host.serial || !host.running()) continue;  // de un proceso anterior
            proto::Writer w;
            w.u64(id).str(args_json);
            std::string message;
            const Outcome o = request(ipc::Op::Callback, w, message);
            if (o == Outcome::ScriptError) {
                error({}, 0, "callback: " + message);
            } else if (o == Outcome::HostDied || o == Outcome::Timeout) {
                reportFault("un callback", message + "  -> los scripts se reinician");
                closeHost();
                ++restarts;
                return;
            }
        }
    }

    template <typename Fn>
    void forEachLive(Fn&& fn) {
        std::vector<entt::entity> handles;
        for (const auto& [h, inst] : instances) handles.push_back(h);
        for (const entt::entity h : handles) {
            if (!host.running()) return;
            auto it = instances.find(h);
            if (it == instances.end() || !it->second.created || faulted.contains(h)) continue;
            fn(h, it->second);
        }
    }

    void flushDestroys(ecs::World& w) {
        for (const entt::entity h : pending_destroy) {
            if (w.registry().valid(h)) w.destroy(w.wrap(h));
        }
        pending_destroy.clear();
    }

    // Las clases y propiedades de una DLL, en un proceso aparte (los
    // constructores son codigo del usuario: si fallan, no pasa nada aqui).
    bool describe(const std::filesystem::path& dll, std::vector<CppClassInfo>& out, std::string& message) const {
        HostProcess temp;
        std::vector<std::string> names;
        // Solo los mensajes; lo demas se contesta vacio (no hay juego).
        const RpcHandler quiet = [](proto::Rpc op, proto::Reader& r, proto::Writer& reply) {
            if (op == proto::Rpc::Log) {
                r.u32();
                std::cout << "[C++] " << r.str() << std::endl;
            } else if (op == proto::Rpc::Api) {
                reply.str(R"({"ok":false,"error":"el juego no esta en marcha"})");
            }
        };
        if (!temp.launch(host_exe, dll, g_next_serial++, message, names, quiet)) return false;
        std::vector<std::uint8_t> payload;
        const Outcome o = temp.request(ipc::Op::Describe, proto::Writer{}, message, &payload, std::max(g_timeout_ms.get(), 10000), quiet);
        std::string text;
        if (!payload.empty()) {
            proto::Reader r(payload.data(), payload.size());
            text = r.str();
        }
        proto::Writer quit;
        std::string ignored;
        if (temp.running()) temp.request(ipc::Op::Quit, quit, ignored, nullptr, 2000, quiet);
        temp.close();
        out = parseSchema(text, assets_root);
        return o == Outcome::Ok || !out.empty();
    }

    void saveSchema(const std::vector<CppClassInfo>& list) const {
        json j = json::array();
        for (const CppClassInfo& c : list) {
            json props = json::array();
            for (const CppPropertyInfo& p : c.properties) {
                json opts = json::array();
                for (const std::string& o : p.options) opts.push_back(o);
                props.push_back(json{{"name", p.name}, {"label", p.label}, {"tooltip", p.tooltip}, {"header", p.header},
                                     {"kind", p.kind}, {"element", p.element}, {"type", p.type}, {"has_range", p.has_range},
                                     {"min", p.min}, {"max", p.max}, {"options", opts},
                                     {"default", json::parse(p.default_json, nullptr, false)}});
            }
            j.push_back(json{{"class", c.name}, {"file", c.file}, {"properties", props}});
        }
        std::error_code ec;
        std::filesystem::create_directories(build_folder, ec);
        std::ofstream(build_folder / "schema.json", std::ios::binary) << j.dump(1);
    }
};

// -----------------------------------------------------------------------------
// CppScriptSystem
// -----------------------------------------------------------------------------

CppScriptSystem::CppScriptSystem() : impl_(std::make_unique<Impl>()) {
    registerCppScriptComponents();
    impl_->host_exe = exeFolder() / kHostName;
    impl_->sdk_folder = exeFolder() / "sdk";
    impl_->memory_listener = g_memory_mb.onChanged([this](cvar::CVarBase&) { impl_->host.applyMemoryLimit(); });
}

CppScriptSystem::~CppScriptSystem() {
    stop();
    if (impl_->compile_thread.joinable()) impl_->compile_thread.join();
    g_memory_mb.removeCallback(impl_->memory_listener);
    setPhysics(nullptr);
    impl_->lua = nullptr;  // sin tocarlo: puede que ya no exista (el sumidero mira `alive`)
}

void CppScriptSystem::setAssetsRoot(const std::filesystem::path& folder) { impl_->assets_root = folder; }

void CppScriptSystem::setBuildFolder(const std::filesystem::path& folder) {
    impl_->build_folder = folder;
    impl_->prebuilt = false;
    std::error_code ec;
    std::filesystem::file_time_type newest{};
    impl_->dll_path.clear();
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != kLibraryExt) continue;
        const auto t = it->last_write_time(ec);
        if (impl_->dll_path.empty() || t > newest) {
            newest = t;
            impl_->dll_path = it->path();
        }
    }
    // Las clases de la ultima compilacion (sin tener que describir otra vez).
    std::ifstream in(folder / "schema.json", std::ios::binary);
    if (in) {
        std::stringstream s;
        s << in.rdbuf();
        classStore() = parseSchema(s.str(), impl_->assets_root);
        impl_->classes.clear();
        for (const CppClassInfo& c : classStore()) impl_->classes.push_back(c.name);
    } else {
        classStore().clear();
    }
}

void CppScriptSystem::setSdkFolder(const std::filesystem::path& folder) { impl_->sdk_folder = folder; }
void CppScriptSystem::setHostExecutable(const std::filesystem::path& exe) { impl_->host_exe = exe; }
void CppScriptSystem::setInput(const dm::Input* input) { impl_->input = input; }
void CppScriptSystem::setAssetPathResolver(std::function<std::string(const Uuid&)> resolver) { impl_->asset_path = std::move(resolver); }

void CppScriptSystem::setPhysics(physics::PhysicsSystem* physics) {
    if (impl_->physics != nullptr && impl_->physics_listener >= 0) impl_->physics->removeListener(impl_->physics_listener);
    impl_->physics = physics;
    impl_->physics_listener = -1;
    if (physics != nullptr) {
        impl_->physics_listener = physics->addListener([this](const physics::PhysicsEvent& e) {
            if (impl_->playing) impl_->events.push_back(e);
        });
    }
}

void CppScriptSystem::setScriptSystem(ScriptSystem* lua) {
    impl_->lua = lua;
    if (lua != nullptr) {
        std::weak_ptr<int> alive = impl_->alive;
        Impl* d = impl_.get();
        lua->setBridgeCallbackSink([alive, d](std::uint64_t id, const std::string& args) {
            if (!alive.expired()) d->callbacks.emplace_back(id, args);
        });
        // Variables de red (OnNetVar): se guardan y se entregan en el siguiente update.
        lua->setNetVarListener([alive, d](ecs::Entity e, const std::string& key, const std::string& value) {
            if (!alive.expired()) d->net_vars.emplace_back(e.handle(), key, value);
        });
        // Mensajes de los Behavior Trees (Send Message / Run Script): onMessage.
        lua->setMessageListener([alive, d](ecs::Entity e, const std::string& method, const std::string& value) {
            if (!alive.expired()) d->bt_messages.emplace_back(e.handle(), method, value);
        });
    }
}

void CppScriptSystem::setToolchainRoot(const std::filesystem::path& folder) { g_toolchain_override = folder; }

std::string CppScriptSystem::findCompiler(std::string* kind) {
#if defined(_WIN32)
    const std::string chosen = g_compiler.get();
    if (!chosen.empty()) {
        const std::string l = lower(chosen);
        if (kind != nullptr) *kind = l.find("cl.exe") != std::string::npos && l.find("clang") == std::string::npos ? "MSVC" : "clang++";
        return chosen;
    }
    std::error_code ec;
    const std::filesystem::path bundled = toolchainRoot() / "bin" / "clang++.exe";
    if (std::filesystem::exists(bundled, ec)) {
        if (kind != nullptr) *kind = "clang (incluido)";
        return utf8(bundled);
    }
    wchar_t env[MAX_PATH];
    if (GetEnvironmentVariableW(L"CRAMION_CXX", env, MAX_PATH) > 0) {
        if (kind != nullptr) *kind = "clang++";
        return utf8(std::filesystem::path(env));
    }
    std::filesystem::path clang = searchPath(L"clang++.exe");
    if (clang.empty() && std::filesystem::exists("C:/Program Files/LLVM/bin/clang++.exe", ec)) clang = "C:/Program Files/LLVM/bin/clang++.exe";
    if (!clang.empty()) {
        if (kind != nullptr) *kind = "clang++";
        return utf8(clang);
    }
    const std::filesystem::path vcvars = findVcVars();
    if (!vcvars.empty()) {
        if (kind != nullptr) *kind = "MSVC";
        return utf8(vcvars);
    }
#else
    const std::string chosen = g_compiler.get();
    if (!chosen.empty()) {
        if (kind != nullptr) *kind = lower(chosen).find("g++") != std::string::npos ? "g++" : "clang++";
        return chosen;
    }
    std::error_code ec;
    const std::filesystem::path bundled = toolchainRoot() / "bin" / "clang++";
    if (std::filesystem::exists(bundled, ec)) {
        if (kind != nullptr) *kind = "clang (incluido)";
        return utf8(bundled);
    }
    if (const char* env = std::getenv("CRAMION_CXX"); env != nullptr && *env != '\0') {
        if (kind != nullptr) *kind = std::string(env).find("g++") != std::string::npos ? "g++" : "clang++";
        return env;
    }
    const std::filesystem::path clang = searchLlvmTool("clang++");
    if (!clang.empty()) {
        if (kind != nullptr) *kind = "clang++";
        return utf8(clang);
    }
    const std::filesystem::path gcc = searchPath("g++");
    if (!gcc.empty()) {
        if (kind != nullptr) *kind = "g++";
        return utf8(gcc);
    }
#endif
    return {};
}

std::filesystem::path CppScriptSystem::findClangd() {
#if defined(_WIN32)
    std::error_code ec;
    const std::filesystem::path bundled = toolchainRoot() / "bin" / "clangd.exe";
    if (std::filesystem::exists(bundled, ec)) return bundled;
    std::filesystem::path found = searchPath(L"clangd.exe");
    if (found.empty() && std::filesystem::exists("C:/Program Files/LLVM/bin/clangd.exe", ec)) found = "C:/Program Files/LLVM/bin/clangd.exe";
    return found;
#else
    std::error_code ec;
    const std::filesystem::path bundled = toolchainRoot() / "bin" / "clangd";
    if (std::filesystem::exists(bundled, ec)) return bundled;
    return searchLlvmTool("clangd");
#endif
}

std::filesystem::path CppScriptSystem::findClangFormat() {
#if defined(_WIN32)
    std::error_code ec;
    const std::filesystem::path bundled = toolchainRoot() / "bin" / "clang-format.exe";
    if (std::filesystem::exists(bundled, ec)) return bundled;
    std::filesystem::path found = searchPath(L"clang-format.exe");
    if (found.empty() && std::filesystem::exists("C:/Program Files/LLVM/bin/clang-format.exe", ec)) found = "C:/Program Files/LLVM/bin/clang-format.exe";
    return found;
#else
    std::error_code ec;
    const std::filesystem::path bundled = toolchainRoot() / "bin" / "clang-format";
    if (std::filesystem::exists(bundled, ec)) return bundled;
    return searchLlvmTool("clang-format");
#endif
}

bool CppScriptSystem::formatSource(const std::string& text, const std::filesystem::path& file, const std::string& style,
                                   std::string& out, int* cursor, std::string* error) {
    const std::filesystem::path exe = findClangFormat();
    if (exe.empty()) {
        if (error != nullptr) *error = std::string("no hay clang-format (falta toolchain/bin/clang-format") + kExeExt + ")";
        return false;
    }
    // El texto va por un archivo temporal (la salida es el texto formateado).
    std::error_code ec;
    static std::atomic<int> counter{0};
#if defined(_WIN32)
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    const std::filesystem::path temp = std::filesystem::temp_directory_path(ec) /
                                       ("cramion_format_" + std::to_string(pid) + "_" + std::to_string(counter++) + ".cpp");
    {
        std::ofstream o(temp, std::ios::binary);
        o << text;
    }
    std::string output;
#if defined(_WIN32)
    std::wstring style_arg;
    for (const char c : style) style_arg += (c == '"') ? L'\'' : static_cast<wchar_t>(static_cast<unsigned char>(c));
    std::wstring cmd = L"\"" + exe.wstring() + L"\" --style=\"" + style_arg + L"\" --fallback-style=LLVM --assume-filename=\"" +
                       file.wstring() + L"\"";
    if (cursor != nullptr) cmd += L" --cursor=" + std::to_wstring(std::max(*cursor, 0));
    cmd += L" \"" + temp.wstring() + L"\"";
    const int code = runProcess(cmd, output, 20000);
#else
    std::vector<std::string> cmd{utf8(exe), "--style=" + style, "--fallback-style=LLVM", "--assume-filename=" + utf8(file)};
    if (cursor != nullptr) cmd.push_back("--cursor=" + std::to_string(std::max(*cursor, 0)));
    cmd.push_back(utf8(temp));
    const int code = runProcess(cmd, output, 20000);
#endif
    std::filesystem::remove(temp, ec);
    if (code != 0) {
        if (error != nullptr) *error = output.substr(0, 2000);
        return false;
    }
    // Con --cursor la primera linea es JSON: { "Cursor": N, ... }
    if (cursor != nullptr) {
        const std::size_t nl = output.find('\n');
        if (nl == std::string::npos) {
            if (error != nullptr) *error = "salida de clang-format sin cursor";
            return false;
        }
        const json head = json::parse(output.substr(0, nl), nullptr, false);
        if (!head.is_discarded() && head.contains("Cursor")) *cursor = head["Cursor"].get<int>();
        output.erase(0, nl + 1);
    }
    out = std::move(output);
    return true;
}

std::vector<std::string> CppScriptSystem::compileArguments(const std::filesystem::path& source) const {
    std::string kind;
    std::string compiler = findCompiler(&kind);
    if (compiler.empty() || kind == "MSVC") {
        // clangd con el clang incluido si lo hay; si no, sus banderas basicas.
        compiler = "clang++";
    }
    std::vector<std::string> args{generic(fromUtf8(compiler))};
#if defined(_WIN32)
    if (kind == "clang (incluido)") args.push_back("--target=x86_64-w64-mingw32");
#endif
    args.insert(args.end(), {"-std=c++20", "-DCRAMION_CPP_SCRIPT", "-I" + generic(impl_->sdk_folder), "-I" + generic(impl_->assets_root)});
    // Las cabeceras (.h) son de C++ (clang las tomaria por C).
    const std::string ext = lower(utf8(source.extension()));
    if (ext == ".h" || ext == ".hpp") args.insert(args.end(), {"-x", "c++-header"});
    args.insert(args.end(), {"-c", generic(source)});
    return args;
}

void CppScriptSystem::writeCompileCommands() const {
    if (impl_->assets_root.empty()) return;
    json list = json::array();
    std::vector<std::filesystem::path> files = sourcesIn(impl_->assets_root, true);
    for (const auto& f : files) {
        list.push_back(json{{"directory", generic(impl_->assets_root)}, {"file", generic(f)}, {"arguments", compileArguments(f)}});
    }
    // Uno de ejemplo (un .cpp nuevo aun sin guardar tambien tiene las rutas).
    if (files.empty()) {
        const auto f = impl_->assets_root / "Scripts" / "Nuevo.cpp";
        list.push_back(json{{"directory", generic(impl_->assets_root)}, {"file", generic(f)}, {"arguments", compileArguments(f)}});
    }
    const std::string text = list.dump(1);
    const std::filesystem::path out = impl_->assets_root.parent_path() / "compile_commands.json";
    std::ifstream in(out, std::ios::binary);
    std::stringstream old;
    old << in.rdbuf();
    if (in && old.str() == text) return;
    in.close();
    std::ofstream(out, std::ios::binary) << text;
}

bool CppScriptSystem::hasSources() const { return !impl_->assets_root.empty() && !sourcesIn(impl_->assets_root, false).empty(); }

std::filesystem::file_time_type CppScriptSystem::newestSourceTime() const {
    std::filesystem::file_time_type newest{};
    if (impl_->assets_root.empty()) return newest;
    std::error_code ec;
    for (const auto& p : sourcesIn(impl_->assets_root, true)) newest = std::max(newest, std::filesystem::last_write_time(p, ec));
    return newest;
}

bool CppScriptSystem::upToDate() const {
    if (impl_->prebuilt) return true;
    std::error_code ec;
    if (impl_->dll_path.empty() || !std::filesystem::exists(impl_->dll_path, ec)) return !hasSources();
    const auto built = std::filesystem::last_write_time(impl_->dll_path, ec);
    for (const auto& p : sourcesIn(impl_->assets_root, true)) {
        if (std::filesystem::last_write_time(p, ec) > built) return false;
    }
    for (const char* f : {"cramion/Script.h", "cramion/Value.h", "cramion/Types.h", "cramion/Api.gen.h", "cramion/CppProtocol.h",
                          "cramion/ScriptMain.cpp"}) {
        const auto t = std::filesystem::last_write_time(impl_->sdk_folder / f, ec);
        if (!ec && t > built) return false;
    }
    return true;
}

CppCompileResult CppScriptSystem::compile() {
    CppCompileResult result;
    Impl& d = *impl_;
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<std::filesystem::path> sources = sourcesIn(d.assets_root, false);
    writeCompileCommands();
    if (sources.empty()) {
        result.ok = true;
        result.nothing_to_compile = true;
        return result;
    }
#if defined(_WIN32)
    std::string kind;
    const std::string compiler = findCompiler(&kind);
    result.compiler = kind;
    if (compiler.empty()) {
        result.errors.push_back({{}, 0,
                                 "no hay compilador de C++: falta toolchain/ junto al editor (o instala LLVM o Visual Studio "
                                 "con C++, o pon la ruta en la CVar script.cpp.Compiler)"});
        return result;
    }
    std::error_code ec;
    std::filesystem::create_directories(d.build_folder, ec);
    for (std::filesystem::directory_iterator it(d.build_folder, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string ext = utf8(it->path().extension());
        if ((ext == ".dll" || ext == ".pdb" || ext == ".lib" || ext == ".exp" || ext == ".ilk" || ext == ".a") && it->path() != d.dll_path) {
            std::error_code rm;
            std::filesystem::remove(it->path(), rm);
        }
    }
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const std::filesystem::path out = d.build_folder / ("scripts_" + std::to_string(stamp) + ".dll");
    const std::filesystem::path pdb = out.parent_path() / (out.stem().wstring() + L".pdb");
    const std::filesystem::path rsp = d.build_folder / "build.rsp";
    {
        std::ofstream f(rsp, std::ios::binary);
        const auto q = [](const std::filesystem::path& p) { return "\"" + generic(p) + "\""; };
        const bool opt = g_optimize.get();
        if (kind == "clang (incluido)") {
            // llvm-mingw: libc++ y el runtime dentro de la DLL (-static); solo
            // depende del UCRT de Windows.
            f << "--target=x86_64-w64-mingw32 -std=c++20 " << (opt ? "-O2 " : "-O0 ") << "-g -gcodeview -shared -static -Wl,--pdb="
              << q(pdb) << " -DCRAMION_CPP_SCRIPT -Wall -Wno-unused-parameter -Wno-unused-private-field\n";
        } else if (kind == "clang++") {
            f << "-std=c++20 " << (opt ? "-O2 " : "-O0 ") << "-fms-runtime-lib=dll -g -gcodeview -shared -fuse-ld=lld -Wl,/debug -Wl,/pdb:"
              << q(pdb) << " -D_CRT_SECURE_NO_WARNINGS -DCRAMION_CPP_SCRIPT -Wall -Wno-unused-parameter -Wno-unused-private-field\n";
        } else {
            f << "/nologo /std:c++20 /EHsc /MD /Zi /utf-8 /W3 /Zc:__cplusplus " << (opt ? "/O2 " : "/Od ")
              << "/D_CRT_SECURE_NO_WARNINGS /DCRAMION_CPP_SCRIPT\n";
        }
        const char* inc = kind == "MSVC" ? "/I" : "-I";
        f << inc << q(d.sdk_folder) << " " << inc << q(d.assets_root) << "\n";
        for (const auto& s : sources) f << q(s) << "\n";
        f << q(d.sdk_folder / "cramion" / "ScriptMain.cpp") << "\n";
        if (kind == "MSVC") {
            f << "/LD /Fe:" << q(out) << " /Fo:" << q(d.build_folder / "") << " /Fd:" << q(d.build_folder / "vc.pdb") << "\n/link /DEBUG /PDB:"
              << q(pdb) << "\n";
        } else {
            f << "-o " << q(out) << "\n";
        }
    }
    std::wstring command;
    if (kind == "MSVC") {
        command = L"cmd.exe /d /s /c \"\"" + fromUtf8(compiler).wstring() + L"\" >nul && cl.exe @\"" + rsp.wstring() + L"\"\"";
    } else {
        command = L"\"" + fromUtf8(compiler).wstring() + L"\" @\"" + rsp.wstring() + L"\"";
    }
    const int code = runProcess(command, result.log);
    static const std::regex clang_error(R"(^(.*?):(\d+):\d+: (?:fatal )?error: (.*)$)");
    static const std::regex msvc_error(R"(^(.*?)\((\d+)(?:,\d+)?\)\s*: (?:fatal )?error \w+: (.*)$)");
    std::istringstream lines(result.log);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch m;
        if (std::regex_match(line, m, clang_error) || std::regex_match(line, m, msvc_error)) {
            std::filesystem::path file = fromUtf8(m[1]);
            std::error_code rel;
            const std::filesystem::path relative = std::filesystem::relative(file, d.assets_root, rel);
            const bool inside = !rel && !relative.empty() && utf8(relative).rfind("..", 0) != 0;
            result.errors.push_back({generic(inside ? relative : file.filename()), std::stoi(m[2]), m[3]});
        } else if (line.find("lld-link: error") != std::string::npos || line.find("ld.lld: error") != std::string::npos ||
                   line.find("LINK : fatal error") != std::string::npos || line.find("error LNK") != std::string::npos) {
            result.errors.push_back({{}, 0, line});
        }
    }
    result.ok = code == 0 && std::filesystem::exists(out, ec);
    if (!result.ok && result.errors.empty()) {
        result.errors.push_back({{}, 0, "el compilador fallo (codigo " + std::to_string(code) + "): " + result.log.substr(0, 600)});
    }
    if (result.ok) {
        result.dll = out;
        // Las clases y sus propiedades (para el Inspector).
        std::string message;
        if (!d.describe(out, result.classes, message) && !message.empty()) {
            result.errors.push_back({{}, 0, "describir los scripts: " + message});
        }
        d.saveSchema(result.classes);
    }
#else
    std::string kind;
    const std::string compiler = findCompiler(&kind);
    result.compiler = kind;
    if (compiler.empty()) {
        result.errors.push_back({{}, 0,
                                 "no hay compilador de C++: falta toolchain/ junto al editor (o instala clang++ o g++, "
                                 "o pon la ruta en la CVar script.cpp.Compiler)"});
        return result;
    }
    std::error_code ec;
    std::filesystem::create_directories(d.build_folder, ec);
    for (std::filesystem::directory_iterator it(d.build_folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == kLibraryExt && it->path() != d.dll_path) {
            std::error_code rm;
            std::filesystem::remove(it->path(), rm);
        }
    }
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const std::filesystem::path out = d.build_folder / ("scripts_" + std::to_string(stamp) + kLibraryExt);
    std::vector<std::string> command{compiler, "-std=c++20", g_optimize.get() ? "-O2" : "-O0", "-g", "-fPIC", "-shared", "-pthread",
                                     "-DCRAMION_CPP_SCRIPT", "-Wall", "-Wno-unused-parameter"};
    if (kind != "g++") command.push_back("-Wno-unused-private-field");
    command.push_back("-I" + utf8(d.sdk_folder));
    command.push_back("-I" + utf8(d.assets_root));
    for (const auto& s : sources) command.push_back(utf8(s));
    command.push_back(utf8(d.sdk_folder / "cramion" / "ScriptMain.cpp"));
    command.insert(command.end(), {"-o", utf8(out)});
    const int code = runProcess(command, result.log);
    static const std::regex clang_error(R"(^(.*?):(\d+):\d+: (?:fatal )?error: (.*)$)");
    std::istringstream lines(result.log);
    std::string line;
    while (std::getline(lines, line)) {
        std::smatch m;
        if (std::regex_match(line, m, clang_error)) {
            std::filesystem::path file = fromUtf8(m[1]);
            std::error_code rel;
            const std::filesystem::path relative = std::filesystem::relative(file, d.assets_root, rel);
            const bool inside = !rel && !relative.empty() && utf8(relative).rfind("..", 0) != 0;
            result.errors.push_back({generic(inside ? relative : file.filename()), std::stoi(m[2]), m[3]});
        } else if (line.find("ld: error") != std::string::npos || line.find("ld.lld: error") != std::string::npos ||
                   line.find("undefined reference") != std::string::npos || line.find("collect2: error") != std::string::npos) {
            result.errors.push_back({{}, 0, line});
        }
    }
    result.ok = code == 0 && std::filesystem::exists(out, ec);
    if (!result.ok && result.errors.empty()) {
        result.errors.push_back({{}, 0, "el compilador fallo (codigo " + std::to_string(code) + "): " + result.log.substr(0, 600)});
    }
    if (result.ok) {
        result.dll = out;
        // Las clases y sus propiedades (para el Inspector).
        std::string message;
        if (!d.describe(out, result.classes, message) && !message.empty()) {
            result.errors.push_back({{}, 0, "describir los scripts: " + message});
        }
        d.saveSchema(result.classes);
    }
#endif
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return result;
}

void CppScriptSystem::compileAsync() {
    if (impl_->compiling.exchange(true)) return;
    if (impl_->compile_thread.joinable()) impl_->compile_thread.join();
    impl_->compile_thread = std::thread([this] {
        CppCompileResult r = compile();
        std::lock_guard lock(impl_->result_mutex);
        impl_->pending_result = std::move(r);
        impl_->compiling = false;
    });
}

bool CppScriptSystem::compiling() const { return impl_->compiling; }

std::optional<CppCompileResult> CppScriptSystem::takeCompileResult() {
    std::optional<CppCompileResult> out;
    {
        std::lock_guard lock(impl_->result_mutex);
        out.swap(impl_->pending_result);
    }
    if (out && out->ok && !out->dll.empty()) {
        impl_->dll_path = out->dll;
        impl_->load_failed = false;
        classStore() = out->classes;
        impl_->classes.clear();
        for (const CppClassInfo& c : out->classes) impl_->classes.push_back(c.name);
    }
    return out;
}

void CppScriptSystem::usePrebuilt(const std::filesystem::path& dll) {
    impl_->dll_path = dll;
    impl_->prebuilt = true;
}

const std::filesystem::path& CppScriptSystem::dll() const { return impl_->dll_path; }
std::vector<std::string> CppScriptSystem::classes() const { return impl_->classes; }

void CppScriptSystem::start(ecs::World& world) {
    Impl& d = *impl_;
    stop();
    d.world = &world;
    d.playing = g_enabled.get();
    d.faulted.clear();
    d.instances.clear();
    d.events.clear();
    d.callbacks.clear();
    d.pending_destroy.clear();
    d.restarts = 0;
    d.reload_state.clear();
    d.load_failed = false;
    d.time = 0.0f;
    d.frame = 0;
}

void CppScriptSystem::stop() {
    Impl& d = *impl_;
    if (d.playing && d.world != nullptr && d.host.running()) {
        d.forEachLive([&](entt::entity h, Impl::Instance& inst) {
            if (inst.started) d.callEvent(h, inst, cppproto::Event::Destroy, d.args(0.0f));
        });
        proto::Writer w;
        std::string ignored;
        if (d.host.running()) d.request(ipc::Op::Quit, w, ignored);
    }
    d.closeHost();
    d.instances.clear();
    d.events.clear();
    d.playing = false;
}

bool CppScriptSystem::running() const { return impl_->host.running(); }

void CppScriptSystem::reload() {
    Impl& d = *impl_;
    if (!d.playing) return;
    // Recarga en caliente: se guarda el estado de cada script vivo (propiedades
    // + onBeforeReload) y se recrean con la DLL nueva sin parar el juego.
    d.reload_state.clear();
    int kept = 0;
    if (g_hot_keep_state.get() && d.host.running()) {
        d.forEachLive([&](entt::entity h, Impl::Instance& inst) {
            proto::Writer w;
            w.u32(inst.id);
            std::string message;
            std::vector<std::uint8_t> payload;
            const Outcome o = d.request(ipc::Op::Snapshot, w, message, &payload);
            std::string text = "{}";
            if (!payload.empty()) {
                proto::Reader r(payload.data(), payload.size());
                text = r.str();
            }
            if (o != Outcome::Ok) d.error({}, 0, inst.cls + " (" + d.entityName(h) + "): recarga sin su estado: " + message);
            d.reload_state[h] = Impl::ReloadState{text, inst.started};
            ++kept;
        });
        proto::Writer quit;
        std::string ignored;
        if (d.host.running()) d.request(ipc::Op::Quit, quit, ignored);
    }
    d.closeHost();
    d.load_failed = false;
    d.faulted.clear();
    ++d.reloads;
    std::cout << "[C++] Scripts recargados en caliente (" << kept << " con su estado)" << std::endl;
}

void CppScriptSystem::fixedUpdate(ecs::World& world, float step, int steps) {
    Impl& d = *impl_;
    if (!d.playing || steps <= 0) return;
    d.world = &world;
    d.fixed_step = step;
    if (!d.sync(world)) return;
    for (int i = 0; i < steps; ++i) {
        d.forEachLive([&](entt::entity h, Impl::Instance& inst) {
            if (inst.started) d.callEvent(h, inst, proto::Event::FixedUpdate, d.args(step));
        });
    }
}

void CppScriptSystem::update(ecs::World& world, float delta_seconds) {
    Impl& d = *impl_;
    if (!d.playing) return;
    const auto t0 = std::chrono::steady_clock::now();
    d.world = &world;
    d.host.rpcs = 0;
    d.calls = 0;
    d.time += delta_seconds;
    ++d.frame;
    if (d.sync(world)) {
        d.dispatchPhysics();
        d.dispatchCallbacks();
        // Variables de red que llegaron (onNetVar).
        auto net_vars = std::move(d.net_vars);
        d.net_vars.clear();
        for (const auto& [h, key, value] : net_vars) {
            if (!world.valid(h)) continue;
            json msg{{"key", key}, {"value", json::parse(value, nullptr, false)}};
            sendMessage(world.wrap(h), "OnNetVar", msg.dump());
        }
        // Mensajes de los Behavior Trees (tareas Run Script / Send Message).
        auto bt_messages = std::move(d.bt_messages);
        d.bt_messages.clear();
        for (const auto& [h, method, value] : bt_messages) {
            if (world.valid(h)) sendMessage(world.wrap(h), method, value);
        }
        d.forEachLive([&](entt::entity h, Impl::Instance& inst) {
            if (!inst.started) {
                inst.started = true;
                d.callEvent(h, inst, proto::Event::Start, d.args(delta_seconds));
            }
        });
        d.forEachLive([&](entt::entity h, Impl::Instance& inst) {
            if (inst.started) d.callEvent(h, inst, proto::Event::Update, d.args(delta_seconds));
        });
        d.forEachLive([&](entt::entity h, Impl::Instance& inst) {
            if (inst.started) d.callEvent(h, inst, proto::Event::LateUpdate, d.args(delta_seconds));
        });
    } else {
        d.events.clear();
        d.callbacks.clear();
    }
    d.flushDestroys(world);
    d.frame_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

const std::vector<ScriptError>& CppScriptSystem::errors() const { return impl_->errors; }
void CppScriptSystem::sendMessage(ecs::Entity target, const std::string& method, const std::string& value_json) {
    Impl& d = *impl_;
    if (!d.playing || !target.valid() || method.empty()) return;
    const auto it = d.instances.find(target.handle());
    if (it == d.instances.end() || !it->second.created || !d.host.running()) return;
    proto::Writer w;
    w.u32(it->second.id).str(method).str(value_json);
    std::string message;
    const Outcome o = d.request(ipc::Op::Message, w, message);
    if (o != Outcome::Ok) d.fault(target.handle(), it->second, (method + "()").c_str(), o, message);
}

void CppScriptSystem::shiftOrigin(const core::Vec3& offset) {
    Impl& d = *impl_;
    if (!d.playing || d.world == nullptr) return;
    const std::string value = json{{"$v", {offset.x, offset.y, offset.z}}}.dump();
    std::vector<entt::entity> handles;
    for (const auto& [h, inst] : d.instances) handles.push_back(h);
    for (const entt::entity h : handles) sendMessage(d.world->wrap(h), "OnOriginShift", value);
}

std::string CppScriptSystem::entityJson(ecs::Entity e) const {
    return json{{"$e", e.valid() ? idOf(e.handle()) : 0}}.dump();
}

void CppScriptSystem::clearErrors() { impl_->errors.clear(); }

CppScriptSystem::Stats CppScriptSystem::stats() const {
    const Impl& d = *impl_;
    Stats s;
    s.host_running = d.host.running();
    s.instances = static_cast<int>(d.instances.size());
    s.faulted = static_cast<int>(d.faulted.size());
    s.restarts = d.restarts;
    s.reloads = d.reloads;
    s.rpcs = d.host.rpcs;
    s.calls = d.calls;
    s.frame_ms = d.frame_ms;
    s.host_memory = d.host.memory();
    return s;
}

}  // namespace cramion::scripting
