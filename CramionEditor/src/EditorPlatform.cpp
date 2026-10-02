// Plataforma y flujo de trabajo en el editor:
//
//   Avisos           pushToast(): tarjetas abajo a la derecha que se apagan
//                    solas (recarga de C++, pruebas, Git, Steam...).
//   Steam            al dar Play, si la configuracion de compilacion activa
//                    usa Steam, se carga steam_api64.dll (logros, marcadores,
//                    overlay... desde Lua/C++ con Steam.*).
//   Control de versiones (Git)
//                    estado, diferencias, preparar/quitar, commit, push/pull,
//                    ramas e historial con el git del sistema. Inicializa un
//                    repositorio con un .gitignore (Library/, Builds/) y un
//                    .gitattributes con Git LFS para los binarios grandes.
//   Pruebas (Test Runner)
//                    ejecuta los *.test.lua (y Assets/Tests/*.lua): las
//                    pruebas de edicion de golpe y las de Play frame a frame
//                    (Test.wait, Test.waitSeconds, Test.waitUntil...). Tambien
//                    CramionEditor.exe --run-tests <proyecto> [--junit x.xml]
//                    para integracion continua (codigo de salida 0 = todo bien)
//                    y la herramienta MCP run_tests.

#include "EditorApp.h"

#include "Dialogs.h"

#include <CramionCore/platform/Steam.h>

#include <imgui.h>
#include <imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

namespace cramion::editor {

using nlohmann::json;

namespace {

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::wstring quoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (const wchar_t c : arg) {
        if (c == L'"') out += L"\\\"";
        else out += c;
    }
    return out + L"\"";
}

// Ejecuta un programa sin ventana y devuelve su codigo de salida (o -1).
int runProcess(const std::string& program, const std::vector<std::string>& args, const std::filesystem::path& folder,
               std::string& output) {
    std::wstring command = quoteArg(widen(program));
    for (const std::string& a : args) command += L" " + quoteArg(widen(a));
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return -1;
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    const std::wstring dir = folder.wstring();
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                        dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    CloseHandle(write_pipe);
    if (!started) {
        CloseHandle(read_pipe);
        output += "No se pudo ejecutar " + program + " (¿está instalado y en el PATH?)\n";
        return -1;
    }
    std::array<char, 4096> buffer{};
    DWORD got = 0;
    while (ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) && got > 0) {
        output.append(buffer.data(), got);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(read_pipe);
    return static_cast<int>(code);
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    std::string line;
    std::istringstream in(text);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
    }
    return out;
}

std::string xmlEscape(const std::string& text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

constexpr const char* kGitIgnore = R"(# Cramion: lo que se regenera solo
Library/
Builds/
Build/
Logs/
*.tmp
*.crdata.tmp
Thumbs.db
.DS_Store
)";

constexpr const char* kGitAttributes = R"(# Cramion: binarios grandes con Git LFS (git lfs install)
*.png filter=lfs diff=lfs merge=lfs -text
*.jpg filter=lfs diff=lfs merge=lfs -text
*.jpeg filter=lfs diff=lfs merge=lfs -text
*.tga filter=lfs diff=lfs merge=lfs -text
*.psd filter=lfs diff=lfs merge=lfs -text
*.hdr filter=lfs diff=lfs merge=lfs -text
*.exr filter=lfs diff=lfs merge=lfs -text
*.ktx2 filter=lfs diff=lfs merge=lfs -text
*.fbx filter=lfs diff=lfs merge=lfs -text
*.glb filter=lfs diff=lfs merge=lfs -text
*.gltf filter=lfs diff=lfs merge=lfs -text
*.bin filter=lfs diff=lfs merge=lfs -text
*.obj filter=lfs diff=lfs merge=lfs -text
*.wav filter=lfs diff=lfs merge=lfs -text
*.ogg filter=lfs diff=lfs merge=lfs -text
*.mp3 filter=lfs diff=lfs merge=lfs -text
*.mp4 filter=lfs diff=lfs merge=lfs -text
*.crpack filter=lfs diff=lfs merge=lfs -text
*.crdata filter=lfs diff=lfs merge=lfs -text
*.crfracture filter=lfs diff=lfs merge=lfs -text
# Texto de Cramion: siempre con saltos de linea normales
*.crscene text eol=lf
*.crprefab text eol=lf
*.crmat text eol=lf
*.lua text eol=lf
)";

}  // namespace

// --- Estado -----------------------------------------------------------------------------

struct PlatformState {
    // Avisos
    struct Toast {
        std::string title;
        std::string text;
        int kind = 0;
        double born = 0.0;
    };
    std::vector<Toast> toasts;

    // Git
    bool show_git = false;
    bool git_repo = false;
    bool git_checked = false;
    std::string git_branch;
    std::vector<std::string> git_branches;
    struct GitFile {
        std::string path;
        char index = ' ';  // estado en el indice (preparado)
        char work = ' ';   // estado en la carpeta
    };
    std::vector<GitFile> git_files;
    std::vector<std::string> git_log;
    std::string git_selected;
    std::string git_diff;
    std::string git_message;
    std::string git_new_branch;
    std::string git_output;
    // Trabajo en segundo plano (push, pull...)
    std::thread git_thread;
    std::atomic<bool> git_busy{false};
    std::atomic<bool> git_done{false};
    std::mutex git_mutex;
    std::string git_job_output;
    std::string git_job_name;
    int git_job_code = 0;

    // Pruebas
    bool show_tests = false;
    enum class Phase { Idle, EnteringPlay, Loading, Edit, Play, Finish } phase = Phase::Idle;
    struct TestResult {
        std::string file;
        std::string name;
        bool play = false;
        int status = 0;  // 0 pendiente, 1 corriendo, 2 bien, 3 mal, 4 saltada
        std::string message;
        double seconds = 0.0;
        int assertions = 0;
        std::vector<std::string> log;
        float timeout = 30.0f;
    };
    std::vector<TestResult> results;
    std::vector<std::filesystem::path> test_files;
    int run_mode = 0;
    bool started_play = false;
    int wait_frames = 0;
    std::size_t current = 0;
    float current_time = 0.0f;
    std::chrono::steady_clock::time_point run_started{};
    double run_seconds = 0.0;
    bool cli = false;
    std::filesystem::path cli_junit;
    int selected_result = -1;

    ~PlatformState() {
        if (git_thread.joinable()) git_thread.join();
    }
};

PlatformState& EditorApp::platform() {
    if (!platform_) platform_ = std::make_shared<PlatformState>();
    return *platform_;
}

// --- Avisos ---------------------------------------------------------------------------

void EditorApp::pushToast(const std::string& title, const std::string& text, int kind) {
    PlatformState& p = platform();
    p.toasts.push_back(PlatformState::Toast{title, text, kind, ImGui::GetTime()});
    if (p.toasts.size() > 6) p.toasts.erase(p.toasts.begin());
    (kind == 3 ? std::cerr : std::cout) << "[Aviso] " << title << (text.empty() ? "" : ": " + text) << "\n";
}

void EditorApp::drawToasts() {
    PlatformState& p = platform();
    if (p.toasts.empty()) return;
    const double now = ImGui::GetTime();
    constexpr double kLife = 5.0;
    p.toasts.erase(std::remove_if(p.toasts.begin(), p.toasts.end(), [&](const PlatformState::Toast& t) { return now - t.born > kLife; }),
                   p.toasts.end());
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    float y = viewport->WorkPos.y + viewport->WorkSize.y - 16.0f;
    ImDrawList* draw = ImGui::GetForegroundDrawList(const_cast<ImGuiViewport*>(viewport));
    for (auto it = p.toasts.rbegin(); it != p.toasts.rend(); ++it) {
        const float age = static_cast<float>(now - it->born);
        const float alpha = std::clamp(std::min(age * 4.0f, static_cast<float>(kLife) - age), 0.0f, 1.0f);
        const float width = 360.0f;
        const ImVec2 title_size = ImGui::CalcTextSize(it->title.c_str());
        const ImVec2 text_size = it->text.empty() ? ImVec2(0.0f, 0.0f) : ImGui::CalcTextSize(it->text.c_str(), nullptr, false, width - 28.0f);
        const float height = 16.0f + title_size.y + (it->text.empty() ? 0.0f : text_size.y + 4.0f);
        const ImVec2 max(viewport->WorkPos.x + viewport->WorkSize.x - 16.0f, y);
        const ImVec2 min(max.x - width, y - height);
        static const ImU32 kColors[] = {IM_COL32(90, 150, 255, 255), IM_COL32(80, 200, 110, 255), IM_COL32(240, 180, 60, 255),
                                        IM_COL32(235, 80, 70, 255)};
        const int a = static_cast<int>(alpha * 235.0f);
        draw->AddRectFilled(min, max, IM_COL32(32, 34, 40, a), 6.0f);
        draw->AddRectFilled(min, ImVec2(min.x + 5.0f, max.y), (kColors[std::clamp(it->kind, 0, 3)] & 0x00FFFFFFu) | (static_cast<ImU32>(a) << 24),
                            6.0f, ImDrawFlags_RoundCornersLeft);
        draw->AddText(ImVec2(min.x + 16.0f, min.y + 8.0f), IM_COL32(245, 245, 245, a), it->title.c_str());
        if (!it->text.empty()) {
            draw->AddText(nullptr, 0.0f, ImVec2(min.x + 16.0f, min.y + 12.0f + title_size.y), IM_COL32(200, 200, 210, a),
                          it->text.c_str(), nullptr, width - 28.0f);
        }
        y = min.y - 8.0f;
    }
}

// --- Steam ------------------------------------------------------------------------------

void EditorApp::platformEnterPlay() {
    const BuildConfig& config = build_configs_.current();
    if (!config.steam.enabled) return;
    platform::Steam& steam = platform::Steam::instance();
    if (steam.available()) return;
    std::filesystem::path dll = config.steam.dll.empty() ? std::filesystem::path() : dialogs::fromUtf8(config.steam.dll);
    if (!dll.empty() && dll.is_relative()) dll = project_.folder / dll;
    if (steam.init(config.steam.app_id, dll)) {
        pushToast("Steam conectado", steam.userName() + " · AppID " + std::to_string(steam.appId()), 1);
    } else {
        pushToast("Steam no disponible", steam.error() + " (el juego sigue sin Steam)", 2);
    }
}

void EditorApp::drawPlatformBuildSettings(BuildConfig& c, bool& changed) {
    const float label_w = 150.0f;
    if (c.platform == BuildPlatform::Linux) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "Linux (x64)");
        ImGui::TextWrapped("El reproductor nativo de Linux aun no esta disponible: estos ajustes se guardan para cuando llegue. "
                           "Mientras tanto, el juego exportado para Windows funciona en Linux y Steam Deck con Proton. "
                           "Ver Manual > Linux.");
    }
    if (c.platform == BuildPlatform::Android) return;
    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Steam");
    ImGui::SameLine(label_w);
    changed |= ImGui::Checkbox("Usar Steamworks", &c.steam.enabled);
    ImGui::SetItemTooltip("Logros, estadísticas, marcadores, Rich Presence, overlay, nube, Workshop y salas.\n"
                          "Sin Steam abierto (o sin la DLL) el juego funciona igual: Steam.isAvailable() = false.");
    if (!c.steam.enabled) return;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("AppID");
    ImGui::SameLine(label_w);
    int app = static_cast<int>(c.steam.app_id);
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::InputInt("##steam_app", &app)) {
        c.steam.app_id = static_cast<std::uint32_t>(std::max(app, 0));
        changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("480 = Spacewar (pruebas)");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("steam_api64.dll");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(-90.0f);
    changed |= ImGui::InputTextWithHint("##steam_dll", "Steamworks SDK/redistributable_bin/win64/steam_api64.dll", &c.steam.dll);
    ImGui::SameLine();
    if (ImGui::Button("Examinar##steam")) {
        const std::filesystem::path file =
            dialogs::openFile(window_.handle(), L"steam_api64.dll\0steam_api64.dll\0DLL (*.dll)\0*.dll\0", project_.folder);
        if (!file.empty()) {
            c.steam.dll = dialogs::utf8(file);
            changed = true;
        }
    }
    ImGui::Dummy(ImVec2(label_w - ImGui::GetStyle().ItemSpacing.x, 0.0f));
    ImGui::SameLine(label_w);
    changed |= ImGui::Checkbox("Copiar steam_appid.txt (probar fuera de Steam)", &c.steam.ship_appid_file);
    ImGui::Dummy(ImVec2(label_w - ImGui::GetStyle().ItemSpacing.x, 0.0f));
    ImGui::SameLine(label_w);
    changed |= ImGui::Checkbox("Relanzar desde Steam si se abre a mano", &c.steam.restart_if_necessary);
    ImGui::SetItemTooltip("SteamAPI_RestartAppIfNecessary: para la versión que se sube a Steam (sin steam_appid.txt).");
    const platform::Steam& steam = platform::Steam::instance();
    ImGui::Dummy(ImVec2(label_w - ImGui::GetStyle().ItemSpacing.x, 0.0f));
    ImGui::SameLine(label_w);
    if (steam.available()) ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.5f, 1.0f), "Conectado como %s", steam.userName().c_str());
    else ImGui::TextDisabled("En Play se conecta con esta configuración (Steam tiene que estar abierto).");
}

std::vector<std::pair<std::filesystem::path, std::filesystem::path>> EditorApp::platformExportFiles(const BuildConfig& config,
                                                                                                     const std::filesystem::path& target) {
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> files;
    if (!config.steam.enabled || config.platform == BuildPlatform::Android) return files;
    std::filesystem::path dll = config.steam.dll.empty() ? std::filesystem::path() : dialogs::fromUtf8(config.steam.dll);
    if (!dll.empty() && dll.is_relative()) dll = project_.folder / dll;
    if (!dll.empty() && std::filesystem::is_directory(dll)) dll /= "steam_api64.dll";
    if (!dll.empty() && std::filesystem::exists(dll)) {
        files.emplace_back(dll, target / "steam_api64.dll");
    } else {
        std::cerr << "[Exportar] Steam: no se encontro steam_api64.dll (" << config.steam.dll
                  << "); el juego funcionara sin Steam hasta que la copies junto al .exe\n";
    }
    if (config.steam.ship_appid_file) {
        const std::filesystem::path cache = project_.libraryFolder() / "ExportCache" / "steam_appid.txt";
        std::error_code e;
        std::filesystem::create_directories(cache.parent_path(), e);
        std::ofstream(cache, std::ios::binary | std::ios::trunc) << config.steam.app_id;
        files.emplace_back(cache, target / "steam_appid.txt");
    }
    return files;
}

// --- Git --------------------------------------------------------------------------------

void EditorApp::gitRefresh() {
    PlatformState& p = platform();
    p.git_checked = true;
    if (!has_project_) return;
    std::string out;
    p.git_repo = runProcess("git", {"rev-parse", "--is-inside-work-tree"}, project_.folder, out) == 0 &&
                 out.find("true") != std::string::npos;
    if (!p.git_repo) {
        p.git_files.clear();
        p.git_branches.clear();
        p.git_log.clear();
        return;
    }
    out.clear();
    runProcess("git", {"branch", "--show-current"}, project_.folder, out);
    p.git_branch = splitLines(out).empty() ? std::string() : splitLines(out).front();
    out.clear();
    runProcess("git", {"branch", "--format=%(refname:short)"}, project_.folder, out);
    p.git_branches = splitLines(out);
    p.git_branches.erase(std::remove(p.git_branches.begin(), p.git_branches.end(), std::string()), p.git_branches.end());
    out.clear();
    runProcess("git", {"-c", "core.quotepath=off", "status", "--porcelain=v1", "--untracked-files=all"}, project_.folder, out);
    p.git_files.clear();
    for (const std::string& line : splitLines(out)) {
        if (line.size() < 4) continue;
        PlatformState::GitFile f;
        f.index = line[0];
        f.work = line[1];
        f.path = line.substr(3);
        const std::size_t arrow = f.path.find(" -> ");
        if (arrow != std::string::npos) f.path = f.path.substr(arrow + 4);
        if (f.path.size() >= 2 && f.path.front() == '"' && f.path.back() == '"') f.path = f.path.substr(1, f.path.size() - 2);
        p.git_files.push_back(std::move(f));
    }
    out.clear();
    runProcess("git", {"log", "-n", "60", "--date=relative", "--format=%h  %ad  %an  ·  %s"}, project_.folder, out);
    p.git_log = splitLines(out);
}

namespace {

// Un comando largo (push, pull, checkout) en otro hilo.
void startGitJob(PlatformState& p, const std::filesystem::path& folder, std::string name, std::vector<std::vector<std::string>> commands) {
    if (p.git_busy) return;
    if (p.git_thread.joinable()) p.git_thread.join();
    p.git_busy = true;
    p.git_done = false;
    p.git_job_name = std::move(name);
    p.git_thread = std::thread([&p, folder, commands = std::move(commands)] {
        std::string output;
        int code = 0;
        for (const std::vector<std::string>& args : commands) {
            output += "> git";
            for (const std::string& a : args) output += " " + a;
            output += "\n";
            code = runProcess("git", args, folder, output);
            if (code != 0) break;
        }
        {
            std::lock_guard lock(p.git_mutex);
            p.git_job_output = output;
            p.git_job_code = code;
        }
        p.git_busy = false;
        p.git_done = true;
    });
}

const char* gitStatusName(char c) {
    switch (c) {
        case 'M': return "modificado";
        case 'A': return "nuevo";
        case 'D': return "borrado";
        case 'R': return "renombrado";
        case 'C': return "copiado";
        case 'U': return "en conflicto";
        case '?': return "sin seguimiento";
        default: return "";
    }
}

}  // namespace

void EditorApp::drawGitWindow() {
    PlatformState& p = platform();
    if (!p.show_git) return;
    ImGui::SetNextWindowSize(ImVec2(1000.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Control de versiones (Git)###git_window", &p.show_git)) {
        ImGui::End();
        return;
    }
    if (!p.git_checked) gitRefresh();
    if (p.git_done.exchange(false)) {
        if (p.git_thread.joinable()) p.git_thread.join();
        std::string output;
        int code = 0;
        {
            std::lock_guard lock(p.git_mutex);
            output = p.git_job_output;
            code = p.git_job_code;
        }
        p.git_output = output;
        pushToast(code == 0 ? p.git_job_name + " hecho" : p.git_job_name + " falló", code == 0 ? "" : "Mira la salida de Git", code == 0 ? 1 : 3);
        gitRefresh();
    }
    const std::filesystem::path folder = project_.folder;
    std::string out;
    if (!p.git_repo) {
        ImGui::TextWrapped("Este proyecto no está en un repositorio de Git.");
        ImGui::TextDisabled("Hace falta Git instalado (git-scm.com). Git LFS es opcional para imágenes, modelos y audio.");
        if (ImGui::Button("Inicializar repositorio")) {
            std::error_code e;
            if (!std::filesystem::exists(folder / ".gitignore")) std::ofstream(folder / ".gitignore", std::ios::binary) << kGitIgnore;
            if (!std::filesystem::exists(folder / ".gitattributes")) std::ofstream(folder / ".gitattributes", std::ios::binary) << kGitAttributes;
            startGitJob(p, folder, "Inicializar Git", {{"init"}, {"lfs", "install", "--local"}, {"add", "-A"}});
        }
        ImGui::SameLine();
        if (ImGui::Button("Comprobar otra vez")) gitRefresh();
        if (!p.git_output.empty()) {
            ImGui::SeparatorText("Salida");
            ImGui::TextUnformatted(p.git_output.c_str());
        }
        ImGui::End();
        return;
    }

    // --- Barra: rama, actualizar, push/pull ---
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Rama:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::BeginCombo("##branch", p.git_branch.c_str())) {
        for (const std::string& b : p.git_branches) {
            if (ImGui::Selectable(b.c_str(), b == p.git_branch) && b != p.git_branch) {
                startGitJob(p, folder, "Cambiar a " + b, {{"checkout", b}});
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputTextWithHint("##newbranch", "rama nueva", &p.git_new_branch);
    ImGui::SameLine();
    ImGui::BeginDisabled(p.git_new_branch.empty() || p.git_busy);
    if (ImGui::Button("Crear rama")) {
        startGitJob(p, folder, "Crear rama " + p.git_new_branch, {{"checkout", "-b", p.git_new_branch}});
        p.git_new_branch.clear();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(p.git_busy);
    if (ImGui::Button("Actualizar")) gitRefresh();
    ImGui::SameLine();
    if (ImGui::Button("Pull")) startGitJob(p, folder, "Pull", {{"pull", "--ff-only"}});
    ImGui::SameLine();
    if (ImGui::Button("Push")) startGitJob(p, folder, "Push", {{"push", "-u", "origin", p.git_branch}});
    ImGui::EndDisabled();
    if (p.git_busy) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "%s...", p.git_job_name.c_str());
    }

    // --- Izquierda: cambios y commit ---
    ImGui::BeginChild("git_left", ImVec2(420.0f, 0.0f), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
    ImGui::SeparatorText(("Cambios (" + std::to_string(p.git_files.size()) + ")").c_str());
    if (ImGui::SmallButton("Preparar todo")) {
        runProcess("git", {"add", "-A"}, folder, out);
        gitRefresh();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Quitar todo")) {
        runProcess("git", {"reset"}, folder, out);
        gitRefresh();
    }
    ImGui::BeginChild("git_files", ImVec2(0.0f, -150.0f));
    for (const PlatformState::GitFile& f : p.git_files) {
        ImGui::PushID(f.path.c_str());
        bool staged = f.index != ' ' && f.index != '?';
        if (ImGui::Checkbox("##stage", &staged)) {
            if (staged) runProcess("git", {"add", "--", f.path}, folder, out);
            else runProcess("git", {"restore", "--staged", "--", f.path}, folder, out);
            gitRefresh();
            ImGui::PopID();
            break;
        }
        ImGui::SetItemTooltip(staged ? "Preparado (entra en el commit)" : "Sin preparar");
        ImGui::SameLine();
        const char code = f.index != ' ' && f.index != '?' ? f.index : f.work;
        ImVec4 color(0.85f, 0.85f, 0.9f, 1.0f);
        if (code == 'A' || code == '?') color = ImVec4(0.45f, 0.85f, 0.5f, 1.0f);
        else if (code == 'D') color = ImVec4(0.95f, 0.45f, 0.4f, 1.0f);
        else if (code == 'M') color = ImVec4(0.95f, 0.75f, 0.35f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        if (ImGui::Selectable(f.path.c_str(), p.git_selected == f.path)) {
            p.git_selected = f.path;
            p.git_diff.clear();
            if (f.index == '?') {
                std::ifstream in(folder / dialogs::fromUtf8(f.path), std::ios::binary);
                std::stringstream ss;
                ss << in.rdbuf();
                p.git_diff = "(archivo nuevo)\n" + ss.str().substr(0, 20000);
            } else {
                runProcess("git", {"-c", "core.quotepath=off", "diff", staged ? "--cached" : "--no-ext-diff", "--", f.path}, folder,
                           p.git_diff);
                if (p.git_diff.empty() && !staged) runProcess("git", {"diff", "--cached", "--", f.path}, folder, p.git_diff);
            }
        }
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", gitStatusName(code));
        if (ImGui::BeginPopupContextItem("file_menu")) {
            if (ImGui::MenuItem("Descartar los cambios")) {
                if (f.index == '?') {
                    std::error_code e;
                    std::filesystem::remove(folder / dialogs::fromUtf8(f.path), e);
                } else {
                    runProcess("git", {"checkout", "--", f.path}, folder, out);
                }
                gitRefresh();
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (p.git_files.empty()) ImGui::TextDisabled("Sin cambios.");
    ImGui::EndChild();
    ImGui::InputTextMultiline("##message", &p.git_message, ImVec2(-1.0f, 70.0f));
    const bool any_staged = std::any_of(p.git_files.begin(), p.git_files.end(),
                                        [](const PlatformState::GitFile& f) { return f.index != ' ' && f.index != '?'; });
    ImGui::BeginDisabled(p.git_message.empty() || !any_staged || p.git_busy);
    if (ImGui::Button("Commit", ImVec2(-1.0f, 0.0f))) {
        startGitJob(p, folder, "Commit", {{"commit", "-m", p.git_message}});
        p.git_message.clear();
    }
    ImGui::EndDisabled();
    if (!any_staged) ImGui::TextDisabled("Marca los archivos que entran en el commit.");
    ImGui::EndChild();
    ImGui::SameLine();

    // --- Derecha: diferencias / historial ---
    ImGui::BeginChild("git_right", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    if (ImGui::BeginTabBar("git_tabs")) {
        if (ImGui::BeginTabItem("Diferencias")) {
            if (p.git_selected.empty()) ImGui::TextDisabled("Elige un archivo de la lista.");
            ImGui::BeginChild("diff", ImVec2(0.0f, 0.0f), 0, ImGuiWindowFlags_HorizontalScrollbar);
            for (const std::string& line : splitLines(p.git_diff)) {
                ImVec4 color(0.8f, 0.8f, 0.85f, 1.0f);
                if (!line.empty() && line[0] == '+' && line.rfind("+++", 0) != 0) color = ImVec4(0.45f, 0.85f, 0.5f, 1.0f);
                else if (!line.empty() && line[0] == '-' && line.rfind("---", 0) != 0) color = ImVec4(0.95f, 0.45f, 0.4f, 1.0f);
                else if (line.rfind("@@", 0) == 0) color = ImVec4(0.45f, 0.7f, 1.0f, 1.0f);
                ImGui::TextColored(color, "%s", line.c_str());
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Historial")) {
            for (const std::string& line : p.git_log) ImGui::TextUnformatted(line.c_str());
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Salida")) {
            ImGui::TextUnformatted(p.git_output.empty() ? "(nada aún)" : p.git_output.c_str());
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::End();
}

// --- Pruebas automaticas -----------------------------------------------------------

namespace {

std::vector<std::filesystem::path> findTestFiles(const std::filesystem::path& assets) {
    std::vector<std::filesystem::path> files;
    std::error_code e;
    for (std::filesystem::recursive_directory_iterator it(assets, std::filesystem::directory_options::skip_permission_denied, e);
         !e && it != std::filesystem::recursive_directory_iterator(); it.increment(e)) {
        std::error_code fe;
        if (!it->is_regular_file(fe)) continue;
        const std::string name = dialogs::utf8(it->path().filename());
        const bool test_lua = name.size() > 9 && name.substr(name.size() - 9) == ".test.lua";
        bool in_tests = false;
        for (const auto& part : std::filesystem::relative(it->path(), assets, fe)) {
            if (dialogs::utf8(part) == "Tests") in_tests = true;
        }
        if (test_lua || (in_tests && it->path().extension() == ".lua")) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

}  // namespace

bool EditorApp::testRunActive() const { return platform_ && platform_->phase != PlatformState::Phase::Idle; }

void EditorApp::startTestRun(int mode) {
    PlatformState& p = platform();
    if (p.phase != PlatformState::Phase::Idle || !has_project_) return;
    p.test_files = findTestFiles(project_.assetsFolder());
    if (p.test_files.empty()) {
        pushToast("No hay pruebas", "Crea un archivo *.test.lua (Proyecto > Crear > Prueba) o pon scripts en Assets/Tests", 2);
        if (p.cli) {
            exit_code_ = 2;
            requestQuit();
        }
        return;
    }
    // Las que fallaron: se recuerdan para filtrar.
    std::vector<std::string> failed;
    if (mode == 3) {
        for (const PlatformState::TestResult& r : p.results) {
            if (r.status == 3) failed.push_back(r.name);
        }
    }
    p.run_mode = mode;
    p.results.clear();
    p.current = 0;
    p.selected_result = -1;
    p.run_started = std::chrono::steady_clock::now();
    p.started_play = !playing();
    if (p.started_play) enterPlay();
    p.phase = PlatformState::Phase::EnteringPlay;
    p.wait_frames = 2;
    (void)failed;
}

void EditorApp::stopTestRun() {
    PlatformState& p = platform();
    if (p.phase == PlatformState::Phase::Idle) return;
    scripts_.testCall("__cramion_test_abort");
    for (PlatformState::TestResult& r : p.results) {
        if (r.status <= 1) {
            r.status = 4;
            r.message = "cancelada";
        }
    }
    p.phase = PlatformState::Phase::Finish;
}

std::string EditorApp::testResultsJson() const {
    json out = json::array();
    if (!platform_) return out.dump();
    for (const PlatformState::TestResult& r : platform_->results) {
        static const char* kStatus[] = {"pending", "running", "passed", "failed", "skipped"};
        out.push_back({{"file", r.file},
                       {"name", r.name},
                       {"mode", r.play ? "play" : "edit"},
                       {"status", kStatus[std::clamp(r.status, 0, 4)]},
                       {"message", r.message},
                       {"seconds", r.seconds},
                       {"assertions", r.assertions}});
    }
    return out.dump();
}

void EditorApp::updateTestRunner(float delta_seconds) {
    PlatformState& p = platform();
    using Phase = PlatformState::Phase;
    switch (p.phase) {
        case Phase::Idle: return;
        case Phase::EnteringPlay: {
            if (!scripts_.running()) {
                if (--p.wait_frames < -600) {  // 10 s sin arrancar
                    pushToast("Pruebas canceladas", "No se pudo entrar en Play", 3);
                    p.phase = Phase::Finish;
                }
                return;
            }
            if (--p.wait_frames > 0) return;
            p.phase = Phase::Loading;
            [[fallthrough]];
        }
        case Phase::Loading: {
            scripts_.testCall("__cramion_test_reset_cases");
            std::size_t listed = 0;  // casos de la lista ya repartidos (la lista es acumulada)
            for (const std::filesystem::path& file : p.test_files) {
                std::string error;
                const std::string rel = assetRelative(file);
                const std::size_t before = p.results.size();
                if (!scripts_.loadTestFile(dialogs::utf8(file), &error)) {
                    PlatformState::TestResult r;
                    r.file = rel;
                    r.name = dialogs::utf8(file.filename());
                    r.status = 3;
                    r.message = "No carga: " + error;
                    p.results.push_back(r);
                    continue;
                }
                // Las que registro este archivo: las nuevas de la lista.
                const json list = json::parse(scripts_.testCall("__cramion_test_list"), nullptr, false);
                if (!list.is_array()) continue;
                for (std::size_t i = listed; i < list.size(); ++i) {
                    PlatformState::TestResult r;
                    r.file = rel;
                    r.name = list[i].value("name", std::string("?"));
                    r.play = list[i].value("play", false);
                    if (list[i].contains("timeout") && list[i]["timeout"].is_number()) r.timeout = list[i]["timeout"].get<float>();
                    if ((p.run_mode == 1 && r.play) || (p.run_mode == 2 && !r.play)) r.status = 4;
                    p.results.push_back(r);
                }
                listed = list.size();
                (void)before;
            }
            p.phase = Phase::Edit;
            return;
        }
        case Phase::Edit: {
            // Todas las de edicion de golpe.
            const bool any_edit = std::any_of(p.results.begin(), p.results.end(),
                                              [](const PlatformState::TestResult& r) { return !r.play && r.status == 0; });
            if (any_edit) {
                const json results = json::parse(scripts_.testCall("__cramion_test_run_edit", ""), nullptr, false);
                if (results.is_array()) {
                    for (const json& j : results) {
                        const std::string name = j.value("name", std::string());
                        for (PlatformState::TestResult& r : p.results) {
                            if (r.play || r.name != name || r.status != 0) continue;
                            r.status = j.value("ok", false) ? 2 : 3;
                            r.message = j.value("message", std::string());
                            r.seconds = j.value("seconds", 0.0);
                            r.assertions = j.value("assertions", 0);
                            if (j.contains("log") && j["log"].is_array()) {
                                for (const json& l : j["log"]) r.log.push_back(l.is_string() ? l.get<std::string>() : l.dump());
                            }
                            break;
                        }
                    }
                }
            }
            p.phase = Phase::Play;
            p.current = 0;
            p.current_time = -1.0f;
            return;
        }
        case Phase::Play: {
            while (p.current < p.results.size() && (!p.results[p.current].play || p.results[p.current].status != 0)) ++p.current;
            if (p.current >= p.results.size()) {
                p.phase = Phase::Finish;
                return;
            }
            PlatformState::TestResult& r = p.results[p.current];
            if (p.current_time < 0.0f) {
                if (scripts_.testCall("__cramion_test_begin", r.name) != "ok") {
                    r.status = 3;
                    r.message = "no se encontro la prueba";
                    ++p.current;
                    return;
                }
                r.status = 1;
                p.current_time = 0.0f;
                return;
            }
            p.current_time += delta_seconds;
            const json status = json::parse(scripts_.testStep(delta_seconds), nullptr, false);
            const std::string state = status.is_object() ? status.value("state", std::string("failed")) : std::string("failed");
            if (state == "running" && p.current_time < r.timeout) return;
            if (state == "running") {
                scripts_.testCall("__cramion_test_abort");
                r.status = 3;
                r.message = "tiempo agotado (" + std::to_string(static_cast<int>(r.timeout)) + " s)";
            } else {
                r.status = state == "passed" ? 2 : 3;
                r.message = status.value("message", std::string());
            }
            r.seconds = p.current_time;
            r.assertions = status.is_object() ? status.value("assertions", 0) : 0;
            if (status.is_object() && status.contains("log") && status["log"].is_array()) {
                for (const json& l : status["log"]) r.log.push_back(l.is_string() ? l.get<std::string>() : l.dump());
            }
            ++p.current;
            p.current_time = -1.0f;
            return;
        }
        case Phase::Finish: {
            p.run_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - p.run_started).count();
            if (p.started_play && playing()) exitPlay();
            int passed = 0, failed = 0, skipped = 0;
            for (const PlatformState::TestResult& r : p.results) {
                passed += r.status == 2;
                failed += r.status == 3;
                skipped += r.status == 4;
            }
            pushToast(failed == 0 ? "Pruebas: todo bien" : "Pruebas: " + std::to_string(failed) + " fallaron",
                      std::to_string(passed) + " bien, " + std::to_string(failed) + " mal, " + std::to_string(skipped) + " saltadas", failed == 0 ? 1 : 3);
            if (p.cli) {
                if (!p.cli_junit.empty()) {
                    std::ofstream x(p.cli_junit, std::ios::binary | std::ios::trunc);
                    x << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuites name=\"Cramion\" tests=\"" << p.results.size()
                      << "\" failures=\"" << failed << "\" skipped=\"" << skipped << "\" time=\"" << p.run_seconds << "\">\n";
                    x << "  <testsuite name=\"" << xmlEscape(project_.name) << "\" tests=\"" << p.results.size() << "\" failures=\""
                      << failed << "\">\n";
                    for (const PlatformState::TestResult& r : p.results) {
                        x << "    <testcase classname=\"" << xmlEscape(r.file) << "\" name=\"" << xmlEscape(r.name) << "\" time=\""
                          << r.seconds << "\">";
                        if (r.status == 3) x << "<failure message=\"" << xmlEscape(r.message) << "\"/>";
                        if (r.status == 4) x << "<skipped/>";
                        x << "</testcase>\n";
                    }
                    x << "  </testsuite>\n</testsuites>\n";
                }
                for (const PlatformState::TestResult& r : p.results) {
                    static const char* kMark[] = {"?", "...", "OK", "FALLO", "SALTADA"};
                    std::cout << "[Pruebas] " << kMark[std::clamp(r.status, 0, 4)] << "  " << r.file << " :: " << r.name
                              << (r.message.empty() ? "" : "  -- " + r.message) << "\n";
                }
                std::cout << "[Pruebas] " << passed << " bien, " << failed << " mal, " << skipped << " saltadas" << std::endl;
                exit_code_ = failed == 0 ? 0 : 1;
                requestQuit();
            }
            p.phase = Phase::Idle;
            return;
        }
    }
}

void EditorApp::startTestRunFromCli(const std::filesystem::path& project, const std::filesystem::path& junit) {
    PlatformState& p = platform();
    p.cli = true;
    p.cli_junit = junit;
    if (!openProject(project)) {
        std::cerr << "[Pruebas] No se pudo abrir el proyecto " << dialogs::utf8(project) << std::endl;
        exit_code_ = 3;
        requestQuit();
        return;
    }
    p.show_tests = true;
    startTestRun(0);
}

void EditorApp::drawTestRunnerWindow() {
    PlatformState& p = platform();
    if (!p.show_tests) return;
    ImGui::SetNextWindowSize(ImVec2(820.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Pruebas (Test Runner)###test_runner", &p.show_tests)) {
        ImGui::End();
        return;
    }
    const bool running = p.phase != PlatformState::Phase::Idle;
    ImGui::BeginDisabled(running);
    if (ImGui::Button("Ejecutar todas")) startTestRun(0);
    ImGui::SameLine();
    if (ImGui::Button("Solo edición")) startTestRun(1);
    ImGui::SameLine();
    if (ImGui::Button("Solo Play")) startTestRun(2);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (ImGui::Button("Parar")) stopTestRun();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Nueva prueba")) {
        const std::filesystem::path folder = project_.assetsFolder() / "Tests";
        std::error_code e;
        std::filesystem::create_directories(folder, e);
        std::filesystem::path file = folder / "Ejemplo.test.lua";
        for (int i = 2; std::filesystem::exists(file); ++i) file = folder / ("Ejemplo" + std::to_string(i) + ".test.lua");
        std::ofstream(file, std::ios::binary) << R"lua(-- Pruebas automaticas (Ventana > Pruebas, o CramionEditor.exe --run-tests <proyecto>)
-- Test.case(nombre, funcion)               se ejecuta de golpe
-- Test.case(nombre, {play = true}, funcion) en Play, frame a frame:
--     Test.wait(frames), Test.waitSeconds(s), Test.waitUntil(funcion, limite)

Test.case("las matematicas funcionan", function()
    Assert.equal(2 + 2, 4)
    Assert.approx(math.sqrt(2), 1.41421, 0.0001)
end)

Test.case("la escena tiene una camara", {play = true, timeout = 10}, function()
    Test.wait(2)
    local camara = Scene.findWithTag("MainCamera") or Scene.find("Main Camera")
    Assert.notNil(camara, "no hay camara principal")
end)
)lua";
        refreshDatabase();
        pushToast("Prueba creada", assetRelative(file), 1);
    }
    int passed = 0, failed = 0, skipped = 0;
    for (const PlatformState::TestResult& r : p.results) {
        passed += r.status == 2;
        failed += r.status == 3;
        skipped += r.status == 4;
    }
    ImGui::SameLine();
    if (running) ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "Ejecutando...");
    else if (!p.results.empty()) ImGui::Text("%d bien · %d mal · %d saltadas  (%.1f s)", passed, failed, skipped, p.run_seconds);
    ImGui::TextDisabled("Archivos *.test.lua o scripts en Assets/Tests. Línea de comandos: CramionEditor.exe --run-tests <proyecto> [--junit resultados.xml]");

    ImGui::BeginChild("tests_list", ImVec2(ImGui::GetContentRegionAvail().x * 0.55f, 0.0f), ImGuiChildFlags_Borders);
    std::string last_file;
    for (std::size_t i = 0; i < p.results.size(); ++i) {
        const PlatformState::TestResult& r = p.results[i];
        if (r.file != last_file) {
            ImGui::SeparatorText(r.file.c_str());
            last_file = r.file;
        }
        static const ImVec4 kColors[] = {ImVec4(0.6f, 0.6f, 0.65f, 1.0f), ImVec4(0.45f, 0.75f, 1.0f, 1.0f), ImVec4(0.4f, 0.85f, 0.5f, 1.0f),
                                         ImVec4(0.95f, 0.4f, 0.35f, 1.0f), ImVec4(0.6f, 0.6f, 0.6f, 1.0f)};
        static const char* kIcons[] = {"○", "…", "✔", "✖", "–"};
        ImGui::PushID(static_cast<int>(i));
        ImGui::TextColored(kColors[std::clamp(r.status, 0, 4)], "%s", kIcons[std::clamp(r.status, 0, 4)]);
        ImGui::SameLine();
        if (ImGui::Selectable((r.name + (r.play ? "  [Play]" : "")).c_str(), p.selected_result == static_cast<int>(i))) {
            p.selected_result = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (p.results.empty()) ImGui::TextDisabled("Pulsa Ejecutar.");
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("tests_detail", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    if (p.selected_result >= 0 && p.selected_result < static_cast<int>(p.results.size())) {
        const PlatformState::TestResult& r = p.results[static_cast<std::size_t>(p.selected_result)];
        ImGui::TextUnformatted(r.name.c_str());
        ImGui::TextDisabled("%s · %s · %.3f s · %d comprobaciones", r.file.c_str(), r.play ? "Play" : "edición", r.seconds, r.assertions);
        if (!r.message.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", r.message.c_str());
        }
        for (const std::string& line : r.log) ImGui::TextDisabled("%s", line.c_str());
    } else {
        ImGui::TextDisabled("Elige una prueba para ver el detalle.");
    }
    ImGui::EndChild();
    ImGui::End();
}

// --- Enganches ---------------------------------------------------------------------

void EditorApp::updatePlatform(float delta_seconds) {
    platform::Steam& steam = platform::Steam::instance();
    if (steam.available()) steam.update();
    if (has_project_) updateTestRunner(delta_seconds);
}

void EditorApp::drawPlatformWindows() {
    if (has_project_) {
        drawGitWindow();
        drawTestRunnerWindow();
    }
    drawToasts();
}

void EditorApp::drawPlatformWindowMenu() {
    PlatformState& p = platform();
    if (ImGui::MenuItem("Control de versiones (Git)", nullptr, &p.show_git) && p.show_git) gitRefresh();
    ImGui::MenuItem("Pruebas (Test Runner)", nullptr, &p.show_tests);
}

}  // namespace cramion::editor
