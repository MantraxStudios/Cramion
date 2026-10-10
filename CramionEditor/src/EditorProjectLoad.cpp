// Abrir un proyecto desde el Hub sin congelar la ventana (como la ventana de
// carga de Unity): el Hub desaparece y sale un dialogo con el banner del
// motor y una barra de progreso. Lo pesado, leer los modelos de la escena y
// descomprimir sus texturas, va en otro hilo; el resto, por etapas (una por
// frame, con su texto en pantalla antes de bloquear):
//
//   Open     el proyecto: base de assets, fisica, ajustes (openProject sin escena)
//   Scene    leer la escena inicial y lanzar el hilo de los modelos
//   Models   el hilo lee cada modelo que se va a dibujar
//   Upload   subirlos a la GPU (la primera sincronizacion del render)
//   Physics  colliders y cuerpos de la escena
//
// Mientras dura, el editor no sincroniza el render ni atiende al MCP ni a
// los cambios de archivos: nadie mas toca el AssetManager.

#include "EditorApp.h"

#include "Dialogs.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <unordered_set>

namespace cramion::editor {

std::vector<std::uint8_t> captureEditorWindow(HWND hwnd, const std::filesystem::path& png);

namespace {

constexpr ImU32 kAccent = theme::kRed;  // acento del tema
constexpr ImU32 kCardBg = IM_COL32(12, 14, 17, 255);   // el fondo del banner (igual: sin recuadro)

}  // namespace

void EditorApp::beginOpenProject(const std::filesystem::path& path) {
    if (projectLoading()) return;
    ProjectLoad& load = project_load_;
    load.reset();
    load.path = path;
    load.stage = ProjectLoad::Stage::Open;
    load.started = std::chrono::steady_clock::now();
    hub_error_.clear();
    // Nombre para el dialogo antes de abrirlo (la carpeta o el .crproj).
    const std::filesystem::path name = path.extension() == ".crproj" ? path.stem() : path.filename();
    load.project_name = dialogs::utf8(name);
}

void EditorApp::beginOpenScene(const std::filesystem::path& path) {
    if (projectLoading()) return;
    ProjectLoad& load = project_load_;
    load.reset();
    load.scene_only = true;
    load.scene_path = path;
    load.project_name = dialogs::utf8(path.stem());
    load.stage = ProjectLoad::Stage::Scene;
    load.started = std::chrono::steady_clock::now();
}

void EditorApp::stepProjectLoad() {
    using Stage = ProjectLoad::Stage;
    ProjectLoad& load = project_load_;
    // Cada etapa deja un frame con su texto en pantalla antes de trabajar.
    if (load.frames++ == 0) return;
    switch (load.stage) {
        case Stage::Idle:
        case Stage::Done: break;
        case Stage::Open: {
            if (!openProject(load.path, /*open_scene=*/false)) {
                // hub_error_ ya dice por que: de vuelta al Hub.
                load.stage = Stage::Idle;
                return;
            }
            load.project_name = project_.name;
            load.stage = Stage::Scene;
            load.frames = 0;
            break;
        }
        case Stage::Scene: {
            if (load.scene_only) {
                if (!openScene(load.scene_path)) {  // (el error va a la consola)
                    load.stage = Stage::Idle;
                    return;
                }
            } else {
                openStartupScene();
            }
            // Los modelos que se van a dibujar, una vez cada uno.
            std::unordered_set<Uuid> seen;
            world_.forEachDepthFirst([&](ecs::Entity e) {
                const ecs::MeshRenderer* r = e.tryGet<ecs::MeshRenderer>();
                if (r == nullptr || r->mesh || !r->model.valid() || !e.activeInHierarchy()) return;
                if (seen.insert(r->model.uuid).second) load.models.push_back(r->model.uuid);
            });
            std::cout << "[Editor] Cargando la escena: " << load.models.size() << " modelos\n";
            assets::AssetManager* manager = asset_manager_.get();
            assets::AssetDatabase* database = database_.get();
            load.worker = std::async(std::launch::async, [&load, manager, database] {
                for (const Uuid& id : load.models) {
                    {
                        const auto info = database->find(id);
                        std::lock_guard lock(load.mutex);
                        load.current = info ? info->name : std::string{};
                    }
                    try {
                        manager->loadModel(id);
                    } catch (const std::exception& e) {
                        std::cerr << "[Editor] No se pudo cargar un modelo: " << e.what() << "\n";
                    }
                    ++load.models_done;
                }
            });
            load.stage = Stage::Models;
            load.frames = 1;  // sin frame de espera: el hilo ya trabaja
            break;
        }
        case Stage::Models:
            if (load.worker.valid() && load.worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                break;
            }
            if (load.worker.valid()) load.worker.get();
            load.stage = Stage::Upload;
            load.frames = 0;
            break;
        case Stage::Upload: {
            // La primera sincronizacion: modelos y texturas a la GPU (con
            // los modelos ya leidos, solo queda subirlos).
            ecs::RenderSync::Options options;
            options.apply_main_camera = false;
            // Con el streaming cada sincronizacion sube solo unos milisegundos:
            // en la pantalla de carga se sigue (en tandas de 250 ms, para que la
            // pantalla no se congele) hasta que todo este en la GPU.
            const auto upload_start = std::chrono::steady_clock::now();
            // (Con la subida en hilos cada sincronizacion vuelve enseguida: un
            // respiro entre una y otra deja la CPU a esos hilos.)
            do {
                sync_->sync(world_, scene_, renderer_, 0.0f, options);
                if (renderer_.uploadedModelCount() < scene_.models().size()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            } while (renderer_.uploadedModelCount() < scene_.models().size() &&
                     std::chrono::steady_clock::now() - upload_start < std::chrono::milliseconds(250));
            if (renderer_.uploadedModelCount() < scene_.models().size()) break;
            load.stage = Stage::Physics;
            load.frames = 0;
            break;
        }
        case Stage::Physics:
            // Colliders (tambien los Mesh Collider, que usan los modelos) y
            // navegacion, antes del primer frame del editor.
            updatePhysics(0.0f);
            updateNavigation(0.0f);
            load.stage = Stage::Done;
            std::cout << (load.scene_only ? "[Editor] Escena lista en " : "[Editor] Proyecto listo en ")
                      << std::chrono::duration<float>(std::chrono::steady_clock::now() - load.started).count()
                      << " s\n";
            break;
    }
}

// Fraccion 0..1 y texto de la etapa actual.
void EditorApp::projectLoadProgress(float& fraction, std::string& text) {
    using Stage = ProjectLoad::Stage;
    ProjectLoad& load = project_load_;
    switch (load.stage) {
        case Stage::Open:
            fraction = 0.04f;
            text = "Abriendo el proyecto";
            break;
        case Stage::Scene:
            fraction = load.scene_only ? 0.04f : 0.1f;
            text = "Leyendo la escena";
            break;
        case Stage::Models: {
            const std::size_t total = std::max<std::size_t>(load.models.size(), 1);
            const std::size_t done = std::min(load.models_done.load(), total);
            fraction = 0.12f + 0.68f * static_cast<float>(done) / static_cast<float>(total);
            std::lock_guard lock(load.mutex);
            text = "Cargando modelos (" + std::to_string(std::min(done + 1, total)) + "/" + std::to_string(total) + ")";
            if (!load.current.empty()) text += ": " + load.current;
            break;
        }
        case Stage::Upload: {
            const std::size_t total = std::max<std::size_t>(scene_.models().size(), 1);
            const std::size_t done = std::min<std::size_t>(renderer_.uploadedModelCount(), total);
            fraction = 0.8f + 0.12f * static_cast<float>(done) / static_cast<float>(total);
            text = "Subiendo modelos y texturas a la GPU (" + std::to_string(done) + "/" + std::to_string(total) + ")";
            break;
        }
        case Stage::Physics:
            fraction = 0.94f;
            text = "Preparando física y navegación";
            break;
        default:
            fraction = 1.0f;
            text = "Listo";
            break;
    }
}

// El dialogo: fondo oscuro en toda la ventana y una tarjeta centrada con el
// logo, CRAMION, el proyecto, la barra y la etapa.
void EditorApp::drawProjectLoading(float delta_seconds) {
    float fraction = 0.0f;
    std::string text;
    projectLoadProgress(fraction, text);
    // La barra avanza suave hacia el valor real (sin saltos).
    ProjectLoad& load = project_load_;
    load.shown = load.shown + (fraction - load.shown) * std::min(1.0f, delta_seconds * 8.0f);
    if (fraction >= 1.0f) load.shown = 1.0f;
    load.time += delta_seconds;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
                      IM_COL32(8, 9, 12, 255));

    const ImVec2 card_size(620.0f, 400.0f);
    const ImVec2 center(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(card_size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(kCardBg));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.2f, 0.22f, 0.27f, 1.0f));
    ImGui::Begin("##project_loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    const float w = card_size.x;

    // --- Banner: el logo y la palabra CRAMION, recortados del de 1920x1080
    // (sin el "GAME POWERED BY", que es para los juegos) ---
    const ImTextureID banner = imgui_.banner();
    float y = p.y + 34.0f;
    if (banner != 0) {
        // Logo: x 820..1100, y 205..490 del banner.
        const float logo_h = 120.0f;
        const float logo_w = logo_h * (280.0f / 285.0f);
        draw->AddImage(banner, ImVec2(p.x + (w - logo_w) * 0.5f, y), ImVec2(p.x + (w + logo_w) * 0.5f, y + logo_h),
                       ImVec2(820.0f / 1920.0f, 205.0f / 1080.0f), ImVec2(1100.0f / 1920.0f, 490.0f / 1080.0f));
        y += logo_h + 22.0f;
        // CRAMION: x 770..1150, y 628..712.
        const float word_h = 38.0f;
        const float word_w = word_h * (380.0f / 84.0f);
        draw->AddImage(banner, ImVec2(p.x + (w - word_w) * 0.5f, y), ImVec2(p.x + (w + word_w) * 0.5f, y + word_h),
                       ImVec2(770.0f / 1920.0f, 628.0f / 1080.0f), ImVec2(1150.0f / 1920.0f, 712.0f / 1080.0f));
        y += word_h + 30.0f;
    } else {
        const char* title = "CRAMION";
        const float size = ImGui::GetFontSize() * 2.6f;
        const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, title);
        draw->AddText(ImGui::GetFont(), size, ImVec2(p.x + (w - ts.x) * 0.5f, y + 90.0f), IM_COL32(240, 242, 246, 255),
                      title);
        y += 190.0f;
    }

    // --- Proyecto ---
    const std::string opening = "Abriendo " + load.project_name;
    const ImVec2 os = ImGui::CalcTextSize(opening.c_str());
    draw->AddText(ImVec2(p.x + (w - os.x) * 0.5f, y), IM_COL32(215, 220, 230, 255), opening.c_str());
    y += os.y + 16.0f;

    // --- Barra de progreso ---
    const float bar_x0 = p.x + 60.0f;
    const float bar_x1 = p.x + w - 60.0f;
    const float bar_h = 6.0f;
    draw->AddRectFilled(ImVec2(bar_x0, y), ImVec2(bar_x1, y + bar_h), IM_COL32(32, 36, 45, 255), 3.0f);
    const float fill_x = bar_x0 + (bar_x1 - bar_x0) * std::clamp(load.shown, 0.0f, 1.0f);
    if (fill_x > bar_x0 + 1.0f) {
        // Brillo detras de la barra y la barra.
        draw->AddRectFilled(ImVec2(bar_x0, y - 3.0f), ImVec2(fill_x, y + bar_h + 3.0f), theme::withAlpha(kAccent, 40), 6.0f);
        draw->AddRectFilled(ImVec2(bar_x0, y), ImVec2(fill_x, y + bar_h), kAccent, 3.0f);
        // Reflejo que recorre lo cargado (se ve que no esta colgado).
        const float sweep = std::fmod(load.time * 0.9f, 1.4f) - 0.2f;
        const float sx = bar_x0 + (fill_x - bar_x0) * sweep;
        const float half = 40.0f;
        draw->PushClipRect(ImVec2(bar_x0, y), ImVec2(fill_x, y + bar_h), true);
        draw->AddRectFilledMultiColor(ImVec2(sx - half, y), ImVec2(sx, y + bar_h), IM_COL32(255, 255, 255, 0),
                                      IM_COL32(255, 255, 255, 110), IM_COL32(255, 255, 255, 110),
                                      IM_COL32(255, 255, 255, 0));
        draw->AddRectFilledMultiColor(ImVec2(sx, y), ImVec2(sx + half, y + bar_h), IM_COL32(255, 255, 255, 110),
                                      IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0),
                                      IM_COL32(255, 255, 255, 110));
        draw->PopClipRect();
    }
    y += bar_h + 12.0f;

    // --- Etapa (izquierda) y porcentaje (derecha) ---
    const std::string percent = std::to_string(static_cast<int>(std::round(load.shown * 100.0f))) + " %";
    const ImVec2 ps = ImGui::CalcTextSize(percent.c_str());
    draw->PushClipRect(ImVec2(bar_x0, y), ImVec2(bar_x1 - ps.x - 12.0f, y + ps.y + 2.0f), true);
    draw->AddText(ImVec2(bar_x0, y), IM_COL32(140, 148, 162, 255), text.c_str());
    draw->PopClipRect();
    draw->AddText(ImVec2(bar_x1 - ps.x, y), IM_COL32(140, 148, 162, 255), percent.c_str());

    // --- Pie: version ---
    const std::string footer = std::string("Cramion ") + CRAMION_VERSION_STRING;
    const ImVec2 fs = ImGui::CalcTextSize(footer.c_str());
    draw->AddText(ImVec2(p.x + (w - fs.x) * 0.5f, p.y + card_size.y - fs.y - 14.0f), IM_COL32(80, 86, 98, 255),
                  footer.c_str());

    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);

    // Pruebas: CRAMION_CAPTURE_LOADING guarda una captura por etapa
    // (%TEMP%/cramion_loading_<n>.png; la del frame anterior, ya presentado).
    if (std::getenv("CRAMION_CAPTURE_LOADING") != nullptr && load.frames >= 1 &&
        static_cast<int>(load.stage) != load.captured_stage) {
        load.captured_stage = static_cast<int>(load.stage);
        captureEditorWindow(window_.handle(), std::filesystem::temp_directory_path() /
                                                  ("cramion_loading_" + std::to_string(load.captured_stage) + ".png"));
    }
}

}  // namespace cramion::editor
