// -----------------------------------------------------------------------------
// CramionUpdater.exe: el actualizador de Cramion, una aplicacion aparte con su
// propia ventana (Win32 + Direct3D 11 + Dear ImGui: no necesita Vulkan, asi
// funciona aunque el motor no arranque).
//
//   CramionUpdater.exe                     busca y ensena si hay version nueva
//   CramionUpdater.exe --install           si la hay, la descarga e instala
//         --reinstall   instala aunque sea la misma version (reparar)
//         --wait-pid N  espera a que ese proceso (el editor) se cierre
//         --relaunch    al terminar abre otra vez Cramion (los proyectos de
//                       reopen.json o el Hub)
//         --feed URL    otro feed (tambien CRAMION_UPDATE_FEED)
//         --dir CARPETA instala ahi (por defecto, la carpeta del .exe)
//         --auto-close  sale al terminar (codigo 0 = instalado, 1 = error)
//
// Pasos: buscar -> descargar el zip -> descomprimir y comprobar -> pedir a
// los editores abiertos que guarden todo y se cierren -> instalar (con
// vuelta atras si algo falla) -> volver a abrir Cramion.
//
// CRAMION_UPDATER_CAPTURE=<carpeta>: guarda un PNG de cada pantalla (pruebas).
// -----------------------------------------------------------------------------

#include <CramionUpdater/NotesView.h>
#include <CramionUpdater/Update.h>

#include "Inflate.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

namespace {

using namespace cramion;
namespace fs = std::filesystem;

// --- Colores (los del editor) ------------------------------------------------------

constexpr ImU32 kAccent = IM_COL32(0, 143, 242, 255);
constexpr ImU32 kGreen = IM_COL32(46, 204, 113, 255);
constexpr ImU32 kRed = IM_COL32(235, 87, 87, 255);
constexpr ImU32 kAmber = IM_COL32(242, 170, 40, 255);
constexpr ImU32 kBg = IM_COL32(20, 21, 25, 255);
constexpr ImU32 kPanel = IM_COL32(29, 30, 35, 255);
constexpr ImU32 kBorder = IM_COL32(51, 53, 60, 255);
constexpr ImU32 kTextDim = IM_COL32(150, 156, 168, 255);

// --- Direct3D 11 -------------------------------------------------------------------

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap_chain = nullptr;
ID3D11RenderTargetView* g_target = nullptr;
UINT g_resize_width = 0;
UINT g_resize_height = 0;
float g_dpi = 1.0f;
std::atomic<bool> g_block_close{false};
bool g_close_requested = false;

void createTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back != nullptr) {
        g_device->CreateRenderTargetView(back, nullptr, &g_target);
        back->Release();
    }
}

void releaseTarget() {
    if (g_target != nullptr) {
        g_target->Release();
        g_target = nullptr;
    }
}

bool createDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                               &sd, &g_swap_chain, &g_device, &got, &g_context);
    if (FAILED(hr)) {  // sin GPU o sin controlador: WARP (por software)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                           &g_swap_chain, &g_device, &got, &g_context);
    }
    if (FAILED(hr)) return false;
    createTarget();
    return true;
}

void releaseDevice() {
    releaseTarget();
    if (g_swap_chain != nullptr) g_swap_chain->Release();
    if (g_context != nullptr) g_context->Release();
    if (g_device != nullptr) g_device->Release();
    g_swap_chain = nullptr;
    g_context = nullptr;
    g_device = nullptr;
}

// Textura desde RGBA8 (el logo).
ImTextureID createTexture(const std::vector<unsigned char>& rgba, int width, int height) {
    if (rgba.empty()) return ImTextureID{};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{rgba.data(), static_cast<UINT>(width * 4), 0};
    ID3D11Texture2D* texture = nullptr;
    if (FAILED(g_device->CreateTexture2D(&desc, &data, &texture))) return ImTextureID{};
    ID3D11ShaderResourceView* view = nullptr;
    g_device->CreateShaderResourceView(texture, nullptr, &view);
    texture->Release();
    return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(view));
}

// Copia el backbuffer a un PNG (CRAMION_UPDATER_CAPTURE).
void captureBackbuffer(const fs::path& file) {
    ID3D11Texture2D* back = nullptr;
    g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back == nullptr) return;
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (SUCCEEDED(g_device->CreateTexture2D(&desc, nullptr, &staging))) {
        g_context->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            std::vector<unsigned char> rgba(static_cast<std::size_t>(desc.Width) * desc.Height * 4u);
            for (UINT y = 0; y < desc.Height; ++y) {
                std::memcpy(&rgba[static_cast<std::size_t>(y) * desc.Width * 4u],
                            static_cast<const unsigned char*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch,
                            desc.Width * 4u);
            }
            for (std::size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
            g_context->Unmap(staging, 0);
            update::detail::writePng(update::narrow(file.wstring()).c_str(), static_cast<int>(desc.Width),
                                     static_cast<int>(desc.Height), rgba.data(), static_cast<int>(desc.Width * 4u));
        }
        staging->Release();
    }
    back->Release();
}

// --- La aplicacion -----------------------------------------------------------------

struct Options {
    bool install = false;
    bool reinstall = false;
    bool relaunch = false;
    bool auto_close = false;
    DWORD wait_pid = 0;
    std::string feed;
    fs::path dir;
};

enum class Screen { Checking, UpToDate, Available, NoPackage, Downloading, Verifying, Extracting, WaitingApps, Installing, Done, Error };

const char* screenName(Screen s) {
    switch (s) {
        case Screen::Checking: return "checking";
        case Screen::UpToDate: return "uptodate";
        case Screen::Available: return "available";
        case Screen::NoPackage: return "nopackage";
        case Screen::Downloading: return "downloading";
        case Screen::Verifying: return "verifying";
        case Screen::Extracting: return "extracting";
        case Screen::WaitingApps: return "waiting";
        case Screen::Installing: return "installing";
        case Screen::Done: return "done";
        case Screen::Error: return "error";
    }
    return "?";
}

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool processAlive(DWORD pid) {
    if (pid == 0) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (process == nullptr) return false;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}

class Updater {
public:
    explicit Updater(Options options) : opt_(std::move(options)) {
        settings_ = update::loadSettings();
        if (opt_.feed.empty()) opt_.feed = update::feedUrl();
        if (opt_.dir.empty()) opt_.dir = update::installFolder();
        update::cleanupOldFiles();
        startCheck();
    }
    ~Updater() {
        cancel_ = true;
        if (worker_.joinable()) worker_.join();
    }

    void setFonts(ImFont* regular, ImFont* bold) {
        regular_ = regular;
        bold_ = bold;
    }
    void setLogo(ImTextureID logo) { logo_ = logo; }
    Screen screen() const { return screen_; }
    bool finished() const { return exit_; }
    int exitCode() const { return exit_code_; }
    bool busy() const {
        const Screen s = screen_;
        return s == Screen::Installing;
    }

    void frame(HWND hwnd);

private:
    // --- Pasos ---
    void startCheck() {
        screen_ = Screen::Checking;
        check_ = update::startCheck(opt_.feed);
    }

    void pollCheck() {
        if (!check_ || check_->state == update::CheckJob::State::Checking) return;
        if (check_->state == update::CheckJob::State::Failed) {
            fail("No se pudo buscar la actualización", check_->error);
            check_.reset();
            return;
        }
        release_ = check_->release;
        notes_ = update::parseNotes(release_.notes);
        check_.reset();
        const bool newer = release_.version > update::currentVersion();
        if (!newer && !opt_.reinstall) {
            screen_ = Screen::UpToDate;
            if (opt_.install && opt_.auto_close) finish(0);
            return;
        }
        if (release_.zip_url.empty()) {
            screen_ = Screen::NoPackage;
            if (opt_.auto_close) finish(1);
            return;
        }
        screen_ = Screen::Available;
        if (opt_.install) {
            if (sourceBuild()) {
                fail("No se instala sobre una compilación del código",
                     "La carpeta " + update::narrow(opt_.dir.wstring()) + " tiene build.ninja: actualízala con git pull.");
            } else {
                startDownload();
            }
        }
    }

    // Carpeta de compilacion del codigo: no se pisa con el paquete.
    bool sourceBuild() const {
        std::error_code ec;
        return fs::exists(opt_.dir / "build.ninja", ec) || fs::exists(opt_.dir / "CMakeCache.txt", ec);
    }

    void startDownload() {
        if (worker_.joinable()) worker_.join();
        cancel_ = false;
        done_bytes_ = 0;
        total_bytes_ = release_.zip_size;
        speed_bps_ = 0.0;
        speed_t0_ = now();
        speed_b0_ = 0;
        screen_ = Screen::Downloading;
        worker_ = std::thread([this] {
            const std::string version = release_.version.str();
            const fs::path zip = update::downloadsFolder() / ("Cramion-" + version + "-win64.zip");
            staged_ = update::downloadsFolder() / ("Cramion-" + version);
            std::error_code ec;
            std::string error;
            const auto progress = [this](std::uint64_t done, std::uint64_t total) {
                done_bytes_ = done;
                if (total > 0) total_bytes_ = total;
                return !cancel_.load();
            };
            // Ya descargado antes (y del mismo tamano): no se baja otra vez.
            const bool cached = release_.zip_size > 0 && fs::exists(zip, ec) && fs::file_size(zip, ec) == release_.zip_size;
            if (!cached && !update::downloadFile(release_.zip_url, zip, progress, &error)) {
                if (cancel_) {
                    screen_ = Screen::Available;
                } else {
                    fail("No se pudo descargar la actualización", error);
                }
                return;
            }
            // SHA-256: el zip tiene que ser exactamente el publicado.
            screen_ = Screen::Verifying;
            bool verified = false;
            if (!update::verifyPackage(release_, zip, &verified, &error)) {
                fs::remove(zip, ec);
                fail("El paquete descargado no es válido", error);
                return;
            }
            verified_ = verified;
            screen_ = Screen::Extracting;
            done_bytes_ = 0;
            total_bytes_ = 0;
            if (!update::extractZip(zip, staged_, progress, &error)) {
                fs::remove(zip, ec);  // roto: la proxima vez se descarga de nuevo
                if (cancel_) {
                    screen_ = Screen::Available;
                } else {
                    fail("El paquete descargado no es válido", error);
                }
                return;
            }
            if (!update::looksLikeEnginePackage(staged_)) {
                fail("El paquete descargado no es válido", "No contiene CramionEditor.exe ni la carpeta shaders/.");
                return;
            }
            waiting_since_ = 0.0;
            screen_ = Screen::WaitingApps;
        });
    }

    void pollApps() {
        const double t = now();
        if (t - apps_polled_ < 0.4) return;
        apps_polled_ = t;
        apps_ = update::runningEngineApps(opt_.dir, GetCurrentProcessId());
        if (waiting_since_ == 0.0) {
            waiting_since_ = t;
            // Los editores guardan la escena, los prefabs, los scripts y el
            // Animator y se cierran; el resto se cierra.
            if (!apps_.empty()) update::askAppsToSaveAndClose(apps_);
        }
        const bool waiting_pid = processAlive(opt_.wait_pid);
        if (apps_.empty() && !waiting_pid) startInstall();
    }

    void startInstall() {
        if (worker_.joinable()) worker_.join();
        done_bytes_ = 0;
        total_bytes_ = 0;
        g_block_close = true;
        screen_ = Screen::Installing;
        worker_ = std::thread([this] {
            std::string error;
            const bool ok = update::installFiles(
                staged_, opt_.dir,
                [this](std::uint64_t done, std::uint64_t total) {
                    done_bytes_ = done;
                    total_bytes_ = total;
                    return true;
                },
                &error);
            g_block_close = false;
            if (!ok) {
                fail("No se pudo instalar (no se ha cambiado nada)", error);
                return;
            }
            std::error_code ec;
            fs::remove_all(staged_, ec);
            fs::remove(update::downloadsFolder() / ("Cramion-" + release_.version.str() + "-win64.zip"), ec);
            update::Settings s = update::loadSettings();
            if (s.skipped_version == release_.version.str()) s.skipped_version.clear();
            update::saveSettings(s);
            done_at_ = now();
            screen_ = Screen::Done;
        });
    }

    void relaunch() {
        if (relaunched_) return;
        relaunched_ = true;
        const fs::path editor = opt_.dir / update::kEditorExe;
        const update::ReopenList list = update::takeReopenList();
        for (const fs::path& project : list.projects) update::launch(editor, update::quoteArgument(project.wstring()), opt_.dir);
        if (list.projects.empty()) update::launch(editor, L"", opt_.dir);
    }

    void fail(const std::string& title, const std::string& message) {
        {
            std::lock_guard lock(error_mutex_);
            error_title_ = title;
            error_ = message;
        }
        g_block_close = false;
        screen_ = Screen::Error;
        if (opt_.auto_close) finish(1);
    }

    void finish(int code) {
        exit_code_ = code;
        exit_ = true;
    }

    void skipVersion() {
        settings_ = update::loadSettings();
        settings_.skipped_version = release_.version.str();
        update::saveSettings(settings_);
        finish(0);
    }

    // --- Dibujo ---
    void drawCaption(HWND hwnd);
    void drawHeader(ImU32 color, int icon, const std::string& title, const std::string& subtitle);
    void drawSteps(int active);
    void drawProgress(const char* label, bool bytes);
    void drawNotesPanel(const char* title, float height);
    bool primaryButton(const char* label, float width);
    bool secondaryButton(const char* label, float width);
    void footer(float buttons_width);

    Options opt_;
    update::Settings settings_;
    std::atomic<Screen> screen_{Screen::Checking};
    std::shared_ptr<update::CheckJob> check_;
    update::Release release_;
    std::vector<update::NoteLine> notes_;
    std::mutex error_mutex_;
    std::string error_title_;
    std::string error_;
    std::atomic<std::uint64_t> done_bytes_{0};
    std::atomic<std::uint64_t> total_bytes_{0};
    std::atomic<bool> cancel_{false};
    std::atomic<bool> verified_{false};  // el zip coincidia con su SHA-256 publicado
    std::thread worker_;
    fs::path staged_;
    std::vector<update::RunningApp> apps_;
    double apps_polled_ = 0.0;
    double waiting_since_ = 0.0;
    double done_at_ = 0.0;
    bool relaunched_ = false;
    double speed_t0_ = 0.0;
    std::uint64_t speed_b0_ = 0;
    double speed_bps_ = 0.0;
    bool exit_ = false;
    int exit_code_ = 0;
    ImFont* regular_ = nullptr;
    ImFont* bold_ = nullptr;
    ImTextureID logo_{};
};

// Icono dibujado dentro de un circulo: 0 espera, 1 visto, 2 descarga, 3 aviso, 4 error.
void drawIcon(ImDrawList* d, ImVec2 c, float r, ImU32 color, int icon) {
    d->AddCircleFilled(c, r, (color & 0x00FFFFFFu) | 0x30000000u, 48);
    d->AddCircle(c, r, (color & 0x00FFFFFFu) | 0x90000000u, 48, 2.0f);
    const float s = r * 0.45f;
    const float w = std::max(2.5f, r * 0.12f);
    switch (icon) {
        case 0: {  // girando
            const float t = static_cast<float>(ImGui::GetTime()) * 5.0f;
            d->PathArcTo(c, r * 0.55f, t, t + 4.2f, 32);
            d->PathStroke(color, 0, w);
            break;
        }
        case 1:
            d->AddPolyline(std::vector<ImVec2>{ImVec2(c.x - s, c.y), ImVec2(c.x - s * 0.3f, c.y + s * 0.7f),
                                               ImVec2(c.x + s, c.y - s * 0.6f)}
                               .data(),
                           3, color, 0, w);
            break;
        case 2:
            d->AddLine(ImVec2(c.x, c.y - s), ImVec2(c.x, c.y + s * 0.6f), color, w);
            d->AddPolyline(std::vector<ImVec2>{ImVec2(c.x - s * 0.6f, c.y), ImVec2(c.x, c.y + s * 0.65f),
                                               ImVec2(c.x + s * 0.6f, c.y)}
                               .data(),
                           3, color, 0, w);
            d->AddLine(ImVec2(c.x - s, c.y + s), ImVec2(c.x + s, c.y + s), color, w);
            break;
        case 3:
            d->AddLine(ImVec2(c.x, c.y - s), ImVec2(c.x, c.y + s * 0.25f), color, w);
            d->AddCircleFilled(ImVec2(c.x, c.y + s * 0.75f), w * 0.7f, color);
            break;
        default:
            d->AddLine(ImVec2(c.x - s * 0.7f, c.y - s * 0.7f), ImVec2(c.x + s * 0.7f, c.y + s * 0.7f), color, w);
            d->AddLine(ImVec2(c.x + s * 0.7f, c.y - s * 0.7f), ImVec2(c.x - s * 0.7f, c.y + s * 0.7f), color, w);
            break;
    }
}

void Updater::drawCaption(HWND hwnd) {
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowWidth();
    const float h = 46.0f * g_dpi;
    d->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(16, 17, 20, 255));
    d->AddLine(ImVec2(p.x, p.y + h), ImVec2(p.x + w, p.y + h), kBorder);
    const float logo = 24.0f * g_dpi;
    if (logo_ != ImTextureID{}) {
        d->AddImage(logo_, ImVec2(p.x + 16.0f * g_dpi, p.y + (h - logo) * 0.5f),
                    ImVec2(p.x + 16.0f * g_dpi + logo, p.y + (h + logo) * 0.5f));
    }
    d->AddText(bold_, 15.0f * g_dpi, ImVec2(p.x + 50.0f * g_dpi, p.y + h * 0.5f - 9.0f * g_dpi), IM_COL32(236, 238, 242, 255),
               "Cramion");
    d->AddText(regular_, 15.0f * g_dpi, ImVec2(p.x + 112.0f * g_dpi, p.y + h * 0.5f - 9.0f * g_dpi), kTextDim,
               "Actualizador");

    // Canal: estable o beta (las betas salen antes y pueden fallar).
    {
        const bool busy_now = screen_ == Screen::Downloading || screen_ == Screen::Verifying || screen_ == Screen::Extracting ||
                              screen_ == Screen::WaitingApps || screen_ == Screen::Installing;
        int channel = settings_.channel == "beta" ? 1 : 0;
        const float cw = 150.0f * g_dpi;
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - 46.0f * g_dpi * 2.0f - cw - 10.0f * g_dpi, p.y + (h - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::SetNextItemWidth(cw);
        ImGui::BeginDisabled(busy_now);
        if (ImGui::Combo("##canal", &channel, "Canal estable\0Canal beta\0")) {
            update::Settings s = update::loadSettings();
            s.channel = channel == 1 ? "beta" : "estable";
            update::saveSettings(s);
            settings_.channel = s.channel;
            opt_.feed = update::feedUrl();
            startCheck();
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Beta: recibe también las versiones previas (salen antes, pueden tener fallos)");
    }

    // Minimizar y cerrar.
    const float bw = 46.0f * g_dpi;
    for (int i = 0; i < 2; ++i) {
        const ImVec2 a(p.x + w - bw * static_cast<float>(2 - i), p.y);
        const ImVec2 b(a.x + bw, p.y + h);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        const bool pressed = ImGui::InvisibleButton("caption", ImVec2(bw, h));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hovered) d->AddRectFilled(a, b, i == 1 ? IM_COL32(196, 43, 28, 255) : IM_COL32(255, 255, 255, 20));
        const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        const float s = 5.0f * g_dpi;
        if (i == 0) {
            d->AddLine(ImVec2(c.x - s, c.y), ImVec2(c.x + s, c.y), IM_COL32(220, 222, 228, 255), 1.0f * g_dpi);
            if (pressed) ShowWindow(hwnd, SW_MINIMIZE);
        } else {
            d->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), IM_COL32(220, 222, 228, 255), 1.0f * g_dpi);
            d->AddLine(ImVec2(c.x + s, c.y - s), ImVec2(c.x - s, c.y + s), IM_COL32(220, 222, 228, 255), 1.0f * g_dpi);
            if (pressed) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
}

void Updater::drawHeader(ImU32 color, int icon, const std::string& title, const std::string& subtitle) {
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = 30.0f * g_dpi;
    drawIcon(d, ImVec2(p.x + r, p.y + r), r, color, icon);
    ImGui::SetCursorScreenPos(ImVec2(p.x + r * 2.0f + 18.0f * g_dpi, p.y + 4.0f * g_dpi));
    ImGui::BeginGroup();
    ImGui::PushFont(bold_, 25.0f * g_dpi);
    ImGui::TextUnformatted(title.c_str());
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kTextDim));
    ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x);
    ImGui::TextUnformatted(subtitle.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(p.x, std::max(ImGui::GetCursorScreenPos().y, p.y + r * 2.0f) + 16.0f * g_dpi));
}

// 0 descargar, 1 cerrar Cramion, 2 instalar, 3 listo.
void Updater::drawSteps(int active) {
    static constexpr const char* kSteps[] = {"Descargar", "Guardar y cerrar Cramion", "Instalar", "Listo"};
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float step = w / 4.0f;
    const float r = 11.0f * g_dpi;
    for (int i = 0; i < 4; ++i) {
        const ImVec2 c(p.x + step * (static_cast<float>(i) + 0.5f), p.y + r);
        if (i < 3) {
            d->AddLine(ImVec2(c.x + r + 4.0f, c.y), ImVec2(c.x + step - r - 4.0f, c.y), i < active ? kAccent : kBorder,
                       2.0f * g_dpi);
        }
        const bool done = i < active;
        const bool now_ = i == active;
        d->AddCircleFilled(c, r, done ? kAccent : now_ ? IM_COL32(0, 143, 242, 60) : kPanel, 24);
        d->AddCircle(c, r, done || now_ ? kAccent : kBorder, 24, 1.5f * g_dpi);
        char number[4];
        std::snprintf(number, sizeof(number), "%d", i + 1);
        const ImVec2 ts = ImGui::CalcTextSize(number);
        if (done) {
            const float s = r * 0.45f;
            d->AddPolyline(std::vector<ImVec2>{ImVec2(c.x - s, c.y), ImVec2(c.x - s * 0.25f, c.y + s * 0.7f),
                                               ImVec2(c.x + s, c.y - s * 0.6f)}
                               .data(),
                           3, IM_COL32(255, 255, 255, 255), 0, 2.0f * g_dpi);
        } else {
            d->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), now_ ? IM_COL32(255, 255, 255, 255) : kTextDim, number);
        }
        const ImVec2 ls = ImGui::CalcTextSize(kSteps[i]);
        d->AddText(ImVec2(c.x - ls.x * 0.5f, c.y + r + 6.0f * g_dpi), now_ || done ? IM_COL32(230, 232, 238, 255) : kTextDim,
                   kSteps[i]);
    }
    ImGui::Dummy(ImVec2(w, r * 2.0f + 30.0f * g_dpi));
}

void Updater::drawProgress(const char* label, bool bytes) {
    const std::uint64_t done = done_bytes_;
    const std::uint64_t total = total_bytes_;
    const float fraction = total > 0 ? static_cast<float>(static_cast<double>(done) / static_cast<double>(total)) : 0.0f;
    // Velocidad media del ultimo segundo.
    const double t = now();
    if (t - speed_t0_ >= 1.0) {
        speed_bps_ = static_cast<double>(done - std::min(done, speed_b0_)) / (t - speed_t0_);
        speed_t0_ = t;
        speed_b0_ = done;
    }
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    char right[160] = {};
    if (bytes && total > 0) {
        const std::string eta = speed_bps_ > 1.0 ? std::to_string(static_cast<int>(static_cast<double>(total - std::min(total, done)) / speed_bps_) + 1) + " s" : "...";
        std::snprintf(right, sizeof(right), "%s de %s  ·  %s/s  ·  quedan %s", update::formatBytes(done).c_str(),
                      update::formatBytes(total).c_str(), update::formatBytes(static_cast<std::uint64_t>(speed_bps_)).c_str(),
                      eta.c_str());
    } else if (total > 0) {
        std::snprintf(right, sizeof(right), "%d %%", static_cast<int>(fraction * 100.0f));
    }
    const float rw = ImGui::CalcTextSize(right).x;
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - rw);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kTextDim), "%s", right);

    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 10.0f * g_dpi;
    d->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kPanel, h);
    if (total > 0) {
        d->AddRectFilled(p, ImVec2(p.x + std::max(h, w * fraction), p.y + h), kAccent, h);
    } else {  // sin tamano: barra que va y viene
        const float x = (std::sin(static_cast<float>(ImGui::GetTime()) * 2.5f) * 0.5f + 0.5f) * w * 0.75f;
        d->AddRectFilled(ImVec2(p.x + x, p.y), ImVec2(p.x + x + w * 0.25f, p.y + h), kAccent, h);
    }
    ImGui::Dummy(ImVec2(w, h + 8.0f * g_dpi));
}

void Updater::drawNotesPanel(const char* title, float height) {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kTextDim), "%s", title);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(kPanel));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * g_dpi);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * g_dpi, 12.0f * g_dpi));
    ImGui::BeginChild("notes", ImVec2(0.0f, height), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    if (notes_.empty()) {
        ImGui::TextDisabled("Esta versión no tiene notas.");
    } else {
        update::drawReleaseNotes(notes_, kAccent, bold_);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

bool Updater::primaryButton(const char* label, float width) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(kAccent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.65f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.45f, 0.8f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    const bool pressed = ImGui::Button(label, ImVec2(width * g_dpi, 36.0f * g_dpi));
    ImGui::PopStyleColor(4);
    return pressed;
}

bool Updater::secondaryButton(const char* label, float width) {
    return ImGui::Button(label, ImVec2(width * g_dpi, 36.0f * g_dpi));
}

// Pie: la opcion de buscar al abrir a la izquierda y los botones a la derecha
// (se dibujan justo despues, `buttons_width` en px sin escalar).
void Updater::footer(float buttons_width) {
    const float h = 64.0f * g_dpi;
    const float y = ImGui::GetWindowHeight() - h;
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    d->AddRectFilled(ImVec2(wp.x, wp.y + y), ImVec2(wp.x + ImGui::GetWindowWidth(), wp.y + ImGui::GetWindowHeight()),
                     IM_COL32(16, 17, 20, 255));
    d->AddLine(ImVec2(wp.x, wp.y + y), ImVec2(wp.x + ImGui::GetWindowWidth(), wp.y + y), kBorder);
    ImGui::SetCursorPos(ImVec2(28.0f * g_dpi, y + (h - ImGui::GetFrameHeight()) * 0.5f));
    if (ImGui::Checkbox("Buscar actualizaciones al abrir Cramion", &settings_.check_on_startup)) {
        update::Settings s = update::loadSettings();
        s.check_on_startup = settings_.check_on_startup;
        update::saveSettings(s);
    }
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - (buttons_width + 28.0f) * g_dpi, y + (h - 36.0f * g_dpi) * 0.5f));
}

void Updater::frame(HWND hwnd) {
    pollCheck();
    if (screen_ == Screen::WaitingApps) pollApps();
    if (screen_ == Screen::Done && (opt_.relaunch || opt_.auto_close) && now() - done_at_ > 1.2) {
        if (opt_.relaunch) relaunch();
        finish(0);
    }
    if (g_close_requested) {
        g_close_requested = false;
        if (!busy()) {
            cancel_ = true;
            finish(screen_ == Screen::Done ? 0 : 1);
        }
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("updater", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                         ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    drawCaption(hwnd);

    const float pad = 28.0f * g_dpi;
    const float footer_h = 64.0f * g_dpi;
    ImGui::SetCursorPos(ImVec2(pad, ImGui::GetCursorPosY() + 24.0f * g_dpi));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("body", ImVec2(ImGui::GetWindowWidth() - pad * 2.0f, ImGui::GetWindowHeight() - ImGui::GetCursorPosY() - footer_h - 16.0f * g_dpi),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    const std::string current = update::currentVersion().str();
    const std::string latest = release_.version.str();
    const Screen screen = screen_;
    const float notes_h = [&] { return std::max(80.0f * g_dpi, ImGui::GetContentRegionAvail().y - 118.0f * g_dpi); }();
    switch (screen) {
        case Screen::Checking: {
            drawHeader(kAccent, 0, "Buscando actualizaciones...",
                       "Consultando la última versión publicada de Cramion. Tienes la " + current + ".");
            break;
        }
        case Screen::UpToDate: {
            drawHeader(kGreen, 1, "Cramion está al día",
                       "Tienes la " + current + ", la última versión publicada" +
                           (release_.published_at.empty() ? std::string{} : " (" + update::formatDate(release_.published_at) + ")") + ".");
            drawNotesPanel(("Novedades de la " + latest).c_str(), notes_h + 58.0f * g_dpi);
            break;
        }
        case Screen::Available: {
            std::string sub = "Tienes la " + current + ". ";
            if (release_.zip_size > 0) sub += "Descarga de " + update::formatBytes(release_.zip_size);
            if (!release_.published_at.empty()) sub += ", publicada el " + update::formatDate(release_.published_at);
            sub += ". Cramion guardará la escena y todo lo abierto antes de cerrarse.";
            drawHeader(kAccent, 2, "Cramion " + latest + (release_.prerelease ? " (beta)" : "") + " está disponible", sub);
            if (release_.prerelease) {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAmber),
                                   "Versión beta: trae lo último pero puede tener fallos. Haz copia de tus proyectos.");
            }
            if (sourceBuild()) {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAmber),
                                   "Esta carpeta es una compilación del código (build.ninja): actualízala con git pull.");
            }
            drawNotesPanel("Novedades", notes_h + 40.0f * g_dpi);
            break;
        }
        case Screen::NoPackage: {
            drawHeader(kAmber, 3, "Cramion " + latest + " aún no tiene descarga",
                       "La versión está publicada pero todavía no incluye el paquete " + std::string(update::kPackageAsset) +
                           ". Prueba dentro de un rato.");
            drawNotesPanel("Novedades", notes_h + 58.0f * g_dpi);
            break;
        }
        case Screen::Downloading:
        case Screen::Verifying:
        case Screen::Extracting: {
            drawHeader(kAccent, 2, "Descargando Cramion " + latest,
                       "Puedes seguir trabajando: Cramion solo se cierra (guardando todo) cuando la descarga esté lista.");
            drawSteps(0);
            drawProgress(screen == Screen::Downloading ? "Descargando el paquete"
                         : screen == Screen::Verifying ? "Comprobando el SHA-256 del paquete"
                                                       : "Descomprimiendo y comprobando cada archivo",
                         screen == Screen::Downloading);
            break;
        }
        case Screen::WaitingApps: {
            drawHeader(kAccent, 0, "Guardando y cerrando Cramion",
                       "Cada editor abierto guarda la escena, los prefabs, los scripts y el Animator y se cierra. "
                       "Al terminar se vuelven a abrir tus proyectos.");
            drawSteps(1);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(kPanel));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * g_dpi);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * g_dpi, 12.0f * g_dpi));
            ImGui::BeginChild("apps", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
            if (apps_.empty() && processAlive(opt_.wait_pid)) {
                ImGui::Text("Esperando a que el editor termine de guardar (proceso %lu)...", opt_.wait_pid);
            }
            for (const update::RunningApp& app : apps_) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                drawIcon(ImGui::GetWindowDrawList(), ImVec2(p.x + 9.0f * g_dpi, p.y + 9.0f * g_dpi), 8.0f * g_dpi, kAccent, 0);
                ImGui::SetCursorScreenPos(ImVec2(p.x + 26.0f * g_dpi, p.y));
                ImGui::Text("%s  (proceso %lu)  -  %s", update::narrow(app.exe).c_str(), app.pid,
                            app.editor ? "guardando todo y cerrándose..." : "cerrándose...");
            }
            if (waiting_since_ > 0.0 && now() - waiting_since_ > 12.0) {
                ImGui::Spacing();
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAmber),
                                   "¿Tarda mucho? Mira si el editor te pregunta algo (por ejemplo, dónde guardar una escena "
                                   "nueva) o ciérralo tú: el actualizador sigue en cuanto se cierre.");
                ImGui::PopTextWrapPos();
            }
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
            break;
        }
        case Screen::Installing: {
            drawHeader(kAccent, 0, "Instalando Cramion " + latest, "No cierres esta ventana. Si algo falla, todo vuelve a como estaba.");
            drawSteps(2);
            drawProgress("Copiando archivos", false);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kTextDim), "Carpeta: %s", update::narrow(opt_.dir.wstring()).c_str());
            break;
        }
        case Screen::Done: {
            drawHeader(kGreen, 1, "Cramion " + latest + " instalado",
                       std::string(opt_.relaunch ? "Abriendo Cramion con tus proyectos..." : "Todo listo. Tus proyectos no se han tocado.") +
                           (verified_ ? " Paquete verificado con SHA-256." : " (La versión no publica SHA-256: no se pudo verificar.)"));
            drawSteps(4);
            drawNotesPanel("Novedades", std::max(60.0f * g_dpi, ImGui::GetContentRegionAvail().y));
            break;
        }
        case Screen::Error: {
            std::string title, message;
            {
                std::lock_guard lock(error_mutex_);
                title = error_title_;
                message = error_;
            }
            drawHeader(kRed, 4, title, message);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kTextDim), "También puedes descargarlo a mano desde GitHub.");
            break;
        }
    }
    ImGui::EndChild();

    // --- Botones ---
    switch (screen) {
        case Screen::Checking:
            footer(120.0f);
            if (secondaryButton("Cerrar", 120.0f)) finish(0);
            break;
        case Screen::UpToDate:
            footer(120.0f * 3.0f + 16.0f);
            if (secondaryButton("Reinstalar", 120.0f) && !release_.zip_url.empty() && !sourceBuild()) {
                opt_.reinstall = true;
                startDownload();
            }
            ImGui::SetItemTooltip("Descarga e instala otra vez la %s (repara archivos que falten o esten danados)", latest.c_str());
            ImGui::SameLine();
            if (secondaryButton("Buscar de nuevo", 120.0f)) startCheck();
            ImGui::SameLine();
            if (primaryButton("Cerrar", 120.0f)) finish(0);
            break;
        case Screen::Available:
            footer(140.0f + 110.0f + 200.0f + 16.0f);
            if (secondaryButton("Omitir esta versión", 140.0f)) skipVersion();
            ImGui::SameLine();
            if (secondaryButton("Más tarde", 110.0f)) finish(0);
            ImGui::SameLine();
            ImGui::BeginDisabled(sourceBuild());
            if (primaryButton("Actualizar ahora", 200.0f)) startDownload();
            ImGui::EndDisabled();
            break;
        case Screen::NoPackage:
            footer(130.0f * 2.0f + 8.0f);
            if (secondaryButton("Ver novedades", 130.0f)) update::openUrl(release_.page_url);
            ImGui::SameLine();
            if (primaryButton("Buscar de nuevo", 130.0f)) startCheck();
            break;
        case Screen::Downloading:
        case Screen::Verifying:
        case Screen::Extracting:
            footer(120.0f);
            if (secondaryButton("Cancelar", 120.0f)) cancel_ = true;
            break;
        case Screen::WaitingApps:
            footer(150.0f + 120.0f + 8.0f);
            if (secondaryButton("Volver a pedirlo", 150.0f)) {
                update::askAppsToSaveAndClose(apps_);
                waiting_since_ = now();
            }
            ImGui::SameLine();
            if (secondaryButton("Cancelar", 120.0f)) screen_ = Screen::Available;
            break;
        case Screen::Installing:
            footer(0.0f);
            break;
        case Screen::Done:
            footer(140.0f + 120.0f + 8.0f);
            if (secondaryButton("Cerrar", 120.0f)) finish(0);
            ImGui::SameLine();
            if (primaryButton("Abrir Cramion", 140.0f)) {
                relaunch();
                finish(0);
            }
            break;
        case Screen::Error:
            footer(130.0f + 120.0f + 120.0f + 16.0f);
            if (secondaryButton("Ver novedades", 130.0f)) update::openUrl(release_.page_url.empty() ? update::kReleasesPage : release_.page_url);
            ImGui::SameLine();
            if (secondaryButton("Cerrar", 120.0f)) finish(1);
            ImGui::SameLine();
            if (primaryButton("Reintentar", 120.0f)) {
                if (release_.version.valid && !release_.zip_url.empty()) {
                    startDownload();
                } else {
                    startCheck();
                }
            }
            break;
    }
    ImGui::End();
}

// --- Ventana -----------------------------------------------------------------------

LRESULT WINAPI windowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED) {
                g_resize_width = LOWORD(lparam);
                g_resize_height = HIWORD(lparam);
            }
            return 0;
        case WM_NCHITTEST: {
            // La barra de titulo propia arrastra la ventana (menos sus botones).
            POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd, &pt);
            RECT rc;
            GetClientRect(hwnd, &rc);
            // (menos los botones y el selector de canal de la derecha)
            if (pt.y >= 0 && pt.y < static_cast<LONG>(46.0f * g_dpi) && pt.x < rc.right - static_cast<LONG>(262.0f * g_dpi)) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }
        case WM_CLOSE:
            g_close_requested = true;  // el actualizador decide (no se cierra instalando)
            return 0;
        case WM_SYSCOMMAND:
            if ((wparam & 0xFFF0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

Options parseOptions() {
    Options o;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--install") o.install = true;
        else if (a == L"--reinstall") o.install = o.reinstall = true;
        else if (a == L"--relaunch") o.relaunch = true;
        else if (a == L"--auto-close") o.auto_close = true;
        else if (a == L"--wait-pid" && i + 1 < argc) o.wait_pid = static_cast<DWORD>(std::wcstoul(argv[++i], nullptr, 10));
        else if (a == L"--feed" && i + 1 < argc) o.feed = update::narrow(argv[++i]);
        else if (a == L"--dir" && i + 1 < argc) o.dir = argv[++i];
    }
    LocalFree(argv);
    return o;
}

void applyStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 0.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.PopupRounding = 8.0f;
    style.ScrollbarRounding = 8.0f;
    style.FrameBorderSize = 1.0f;
    style.FramePadding = ImVec2(10.0f, 6.0f);
    style.ItemSpacing = ImVec2(8.0f, 7.0f);
    style.ScrollbarSize = 10.0f;
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = ImGui::ColorConvertU32ToFloat4(kBg);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.125f, 0.130f, 0.149f, 0.98f);
    c[ImGuiCol_Border] = ImGui::ColorConvertU32ToFloat4(kBorder);
    c[ImGuiCol_Text] = ImVec4(0.902f, 0.910f, 0.929f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.529f, 0.549f, 0.600f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.157f, 0.163f, 0.184f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.200f, 0.208f, 0.235f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.227f, 0.239f, 0.278f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.165f, 0.172f, 0.196f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.216f, 0.226f, 0.259f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.000f, 0.560f, 0.950f, 0.55f);
    c[ImGuiCol_CheckMark] = ImGui::ColorConvertU32ToFloat4(kAccent);
    c[ImGuiCol_Separator] = ImGui::ColorConvertU32ToFloat4(kBorder);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    style.ScaleAllSizes(g_dpi);
}

std::vector<unsigned char> readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    ImGui_ImplWin32_EnableDpiAwareness();
    // Una sola ventana del actualizador a la vez.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"CramionUpdaterSingleInstance");
    if (single != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"CramionUpdaterWindow", nullptr); other != nullptr) {
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }

    const Options options = parseOptions();
    g_dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC | CS_DROPSHADOW;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.lpszClassName = L"CramionUpdaterWindow";
    RegisterClassExW(&wc);
    const int width = static_cast<int>(860.0f * g_dpi);
    const int height = static_cast<int>(600.0f * g_dpi);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Actualizador de Cramion", WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
                                work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2,
                                width, height, nullptr, nullptr, instance, nullptr);
    // Esquinas redondeadas de Windows 11 (DWMWA_WINDOW_CORNER_PREFERENCE = 33).
    const int round = 2;
    DwmSetWindowAttribute(hwnd, 33, &round, sizeof(round));
    if (!createDevice(hwnd)) {
        MessageBoxW(hwnd, L"No se pudo iniciar Direct3D 11.", L"Actualizador de Cramion", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    applyStyle();
    ImFont* regular = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 16.0f * g_dpi);
    if (regular == nullptr) regular = io.Fonts->AddFontDefault();
    ImFont* bold = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeuib.ttf", 16.0f * g_dpi);
    if (bold == nullptr) bold = regular;
    io.FontDefault = regular;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    Updater app(options);
    app.setFonts(regular, bold);
    {
        const std::vector<unsigned char> png = readFile(update::installFolder() / "editor_icons" / "logo.png");
        int w = 0, h = 0;
        const std::vector<unsigned char> rgba = update::detail::decodeImage(png.data(), png.size(), w, h);
        app.setLogo(createTexture(rgba, w, h));
    }

    // Capturas para las pruebas.
    fs::path capture_dir;
    {
        wchar_t env[1024] = {};
        if (GetEnvironmentVariableW(L"CRAMION_UPDATER_CAPTURE", env, 1024) > 0) capture_dir = env;
    }
    Screen captured_screen = Screen::Error;
    bool captured_any = false;
    int frames_on_screen = 0;

    bool running = true;
    while (running && !app.finished()) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;
        if (IsIconic(hwnd)) {
            Sleep(30);
            // Minimizada sigue trabajando (la descarga va en otro hilo; la
            // espera de los editores y el final, aqui).
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            app.frame(hwnd);
            ImGui::EndFrame();
            continue;
        }
        if (g_resize_width != 0 && g_resize_height != 0) {
            releaseTarget();
            g_swap_chain->ResizeBuffers(0, g_resize_width, g_resize_height, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_width = g_resize_height = 0;
            createTarget();
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        app.frame(hwnd);
        ImGui::Render();
        const float clear[4] = {20.0f / 255.0f, 21.0f / 255.0f, 25.0f / 255.0f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_target, nullptr);
        g_context->ClearRenderTargetView(g_target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (!capture_dir.empty()) {
            const Screen s = app.screen();
            if (!captured_any || s != captured_screen) {
                if (++frames_on_screen == 12) {
                    std::error_code ec;
                    fs::create_directories(capture_dir, ec);
                    captureBackbuffer(capture_dir / (std::string("updater_") + screenName(s) + ".png"));
                    captured_screen = s;
                    captured_any = true;
                    frames_on_screen = 0;
                }
            } else {
                frames_on_screen = 0;
            }
        }
        g_swap_chain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    releaseDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, instance);
    const int code = app.exitCode();
    if (single != nullptr) {
        ReleaseMutex(single);
        CloseHandle(single);
    }
    return code;
}
