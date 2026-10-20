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
//
// Otra escena con el proyecto ya abierto (beginOpenScene) NO tapa el editor:
//
//   Scene    leer la escena (un frame)
//   Stream   el editor sigue funcionando y los modelos llegan solos por el
//            streaming de RenderSync (hilos de fondo y subida en hilos); en
//            la barra de estado, una barra de carga hasta que esta todo

#include "EditorApp.h"

#include "Dialogs.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <CramionFX/asset/TextureCompression.h>
#include <CramionCore/video/VideoFiles.h>
#include <unordered_set>

namespace cramion::editor {

std::vector<std::uint8_t> captureEditorWindow(HWND hwnd, const std::filesystem::path& png);

namespace {

constexpr ImU32 kAccent = theme::kAccent;  // acento del tema
constexpr ImU32 kCardBg = IM_COL32(12, 14, 17, 255);   // fondo de la tarjeta (el logo va con alfa encima)

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

void EditorApp::startTextureImport() {
    stopTextureImport();
    if (!sync_ || !asset::textureCompressionEnabled()) return;
    auto import = std::make_unique<TextureImport>();
    // Primero las de los materiales (las que piden las escenas), despues
    // cualquier otra imagen de Assets.
    import->items = sync_->materialTextureSources();
    std::unordered_set<std::string> seen;
    for (const asset::TextureData& texture : import->items) seen.insert(texture.source_path);
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             project_.assetsFolder(), std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".tga" && ext != ".bmp" && ext != ".tif" &&
            ext != ".tiff" && ext != ".psd") {
            continue;
        }
        asset::TextureData texture;
        texture.name = dialogs::utf8(it->path().filename());
        texture.source_path = dialogs::utf8(it->path());
        if (seen.insert(texture.source_path).second) import->items.push_back(std::move(texture));
    }
    if (import->items.empty()) return;
    import->started = std::chrono::steady_clock::now();
    std::cout << "[Texturas] Importando " << import->items.size() << " texturas (BC7, Library/Cache/Textures)\n";
    TextureImport* state = import.get();
    // Dos a la vez: cada compresion ya usa varios nucleos.
    for (int t = 0; t < 2; ++t) {
        import->threads.emplace_back([state] {
            for (std::size_t i = state->next++; i < state->items.size() && !state->stop; i = state->next++) {
                try {
                    if (!asset::prepareLazyTexture(state->items[i])) ++state->compressed;
                } catch (const std::exception& e) {
                    std::cerr << "[Texturas] " << state->items[i].name << ": " << e.what() << "\n";
                }
                ++state->done;
            }
        });
    }
    texture_import_ = std::move(import);
}

std::string EditorApp::textureImportStatus() {
    if (!texture_import_) return {};
    TextureImport& import = *texture_import_;
    const std::size_t total = import.items.size();
    const std::size_t done = import.done.load();
    if (done < total) {
        return "Optimizando texturas " + std::to_string(done) + "/" + std::to_string(total);
    }
    if (!import.reported) {
        import.reported = true;
        std::cout << "[Texturas] Importacion lista: " << import.compressed.load() << " comprimidas, "
                  << total - import.compressed.load() << " ya estaban, en "
                  << std::chrono::duration<float>(std::chrono::steady_clock::now() - import.started).count() << " s\n";
    }
    return {};
}

// --- Importacion de videos (como la de texturas) -----------------------------
// Al abrir el proyecto (nunca al abrir una escena): los videos de Assets sin
// cache al dia (tamano y fecha) se abren en 1-2 hilos para leer sus datos y
// una miniatura (video::probeVideo -> Library/Cache/Video/<hash>.json|.png).
// Cada hilo inicia COM/Media Foundation dentro del decodificador (MTA); el
// hilo principal del editor (STA) nunca toca Media Foundation.
struct EditorApp::VideoImport {
    std::vector<std::filesystem::path> items;
    std::filesystem::path cache;
    std::vector<std::thread> threads;
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> done{0};
    std::atomic<std::size_t> failed{0};
    std::chrono::steady_clock::time_point started;
    bool reported = false;
    ~VideoImport() {
        stop = true;
        for (std::thread& thread : threads) {
            if (thread.joinable()) thread.join();  // archivos locales: cada uno tarda poco
        }
    }
};

void EditorApp::VideoImportDeleter::operator()(VideoImport* import) const { delete import; }

std::filesystem::path EditorApp::videoCacheFolder() const {
    return project_.libraryFolder() / "Cache" / "Video";
}

void EditorApp::stopVideoImport() {
    video_import_.reset();
    video_probe_queue_.clear();
}

void EditorApp::startVideoImport(std::vector<std::filesystem::path> files) {
    if (!has_project_) return;
    const bool whole_project = files.empty();
    if (video_import_ && video_import_->done.load() < video_import_->items.size()) {
        // Ya hay una en marcha: estos van a la cola y salen al terminar.
        for (std::filesystem::path& file : files) video_probe_queue_.push_back(std::move(file));
        return;
    }
    if (video_import_) video_info_.clear();  // lo de la anterior se vuelve a leer de la cache
    video_import_.reset();
    const std::filesystem::path cache = videoCacheFolder();
    std::vector<std::filesystem::path> pending;
    const auto add_if_stale = [&](const std::filesystem::path& file) {
        video::VideoInfo cached;
        if (!video::readCachedVideoInfo(cache, file, cached) &&
            std::find(pending.begin(), pending.end(), file) == pending.end()) {
            pending.push_back(file);
        }
    };
    if (whole_project) {
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 project_.assetsFolder(), std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_regular_file(ec) && video::isVideoFile(it->path())) add_if_stale(it->path());
        }
    } else {
        // Los pedidos (sin cache, o "Volver a leer", que ya borro la suya).
        for (const std::filesystem::path& file : files) add_if_stale(file);
    }
    files = std::move(pending);
    const bool forced = !whole_project;
    if (files.empty()) return;
    std::unique_ptr<VideoImport, VideoImportDeleter> import(new VideoImport());
    import->items = std::move(files);
    import->cache = cache;
    import->started = std::chrono::steady_clock::now();
    if (!forced) std::cout << "[Video] Leyendo " << import->items.size() << " videos (miniaturas en Library/Cache/Video)\n";
    VideoImport* state = import.get();
    // Uno o dos hilos: la decodificacion ya es por hardware o usa varios nucleos.
    const int threads = import->items.size() > 1 ? 2 : 1;
    for (int t = 0; t < threads; ++t) {
        import->threads.emplace_back([state] {
            for (std::size_t i = state->next++; i < state->items.size() && !state->stop; i = state->next++) {
                try {
                    video::VideoInfo info;
                    if (!video::probeVideo(state->items[i], state->cache, info)) {
                        ++state->failed;
                        std::cerr << "[Video] " << dialogs::utf8(state->items[i].filename()) << ": " << info.error << "\n";
                    }
                } catch (const std::exception& e) {
                    ++state->failed;
                    std::cerr << "[Video] " << dialogs::utf8(state->items[i].filename()) << ": " << e.what() << "\n";
                }
                ++state->done;
            }
        });
    }
    video_import_ = std::move(import);
}

std::string EditorApp::videoImportStatus() {
    if (!video_import_) return {};
    VideoImport& import = *video_import_;
    const std::size_t total = import.items.size();
    const std::size_t done = import.done.load();
    if (done < total) return "Leyendo vídeos " + std::to_string(done) + "/" + std::to_string(total);
    if (!import.reported) {
        import.reported = true;
        if (total > 1) {
            std::cout << "[Video] Lectura lista: " << total - import.failed.load() << " videos, " << import.failed.load()
                      << " con error, en " << std::chrono::duration<float>(std::chrono::steady_clock::now() - import.started).count()
                      << " s\n";
        }
        video_info_.clear();  // la cache nueva se lee otra vez (miniaturas)
    }
    return {};
}

void EditorApp::beginOpenScene(const std::filesystem::path& path) {
    if (projectLoading()) return;
    // Si ya se estaba abriendo otra escena, se cambia a esta (no hay hilos
    // que esperar: los modelos los trae el streaming).
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
                    load.scene_only = false;
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
            if (load.scene_only) {
                // El editor sigue a la vista: nada de hilos que usen el
                // AssetManager por su cuenta. RenderSync pide los modelos
                // (requestModel) y los sube en hilos; aqui solo se mira.
                load.stage = Stage::Stream;
                load.frames = 0;
                break;
            }
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
            // Falta algo: modelos sin subir o variantes de material esperando
            // a sus texturas (se leen en hilos).
            const auto busy = [&] {
                return renderer_.uploadedModelCount() < scene_.models().size() || sync_->materialVariantsPending();
            };
            // (Con la subida en hilos cada sincronizacion vuelve enseguida: un
            // respiro entre una y otra deja la CPU a esos hilos.)
            do {
                sync_->sync(world_, scene_, renderer_, 0.0f, options);
                if (busy()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            } while (busy() && std::chrono::steady_clock::now() - upload_start < std::chrono::milliseconds(250));
            if (busy()) break;
            load.stage = Stage::Physics;
            load.frames = 0;
            break;
        }
        case Stage::Stream: {
            // Primero RenderSync tiene que haber pedido los modelos (lo hace
            // en syncWorld, despues de este paso): unos frames de margen.
            if (load.frames < 3) break;
            const bool pending = asset_manager_->loadsInFlight() > 0 ||
                                 renderer_.uploadedModelCount() < scene_.models().size() ||
                                 sync_->materialVariantsPending();
            const float elapsed =
                std::chrono::duration<float>(std::chrono::steady_clock::now() - load.started).count();
            if (pending && elapsed < 600.0f) break;  // (por si algo no llega nunca)
            load.stage = Stage::Done;
            load.time = 0.0f;
            std::cout << "[Editor] Escena lista en " << elapsed << " s\n";
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
            fraction = load.scene_only ? 0.03f : 0.1f;
            text = load.scene_only ? "Abriendo " + load.project_name : std::string("Leyendo la escena");
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
        case Stage::Stream: {
            // Modelos que ya estan en memoria y piezas ya en la GPU.
            const std::size_t total = std::max<std::size_t>(load.models.size(), 1);
            std::size_t arrived = 0;
            for (const Uuid& id : load.models) {
                if (asset_manager_->isLoaded(id) || asset_manager_->loadFailed(id)) ++arrived;
            }
            if (load.models.empty()) arrived = 1;
            const float a = static_cast<float>(arrived) / static_cast<float>(total);
            const std::size_t parts = scene_.models().size();
            const float u = parts == 0 ? 1.0f
                                       : static_cast<float>(std::min<std::size_t>(renderer_.uploadedModelCount(), parts)) /
                                             static_cast<float>(parts);
            fraction = 0.06f + 0.62f * a + 0.3f * a * u;
            text = "Abriendo " + load.project_name;
            if (!load.models.empty()) {
                text += " (" + std::to_string(std::min(arrived, total)) + "/" + std::to_string(total) + " modelos)";
            }
            break;
        }
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

    // --- Logo del motor (editor_icons/logo.png, con alfa) y CRAMION en texto ---
    // Antes se recortaba el banner de 1920x1080 con coordenadas fijas del
    // banner viejo: con el de la 2.5.1 (solo texto, sin logo) salian trozos de
    // las letras de "GAME" y "CRAMION" sobre recuadros negros.
    const ImTextureID logo = imgui_.logo();
    float y = p.y + 34.0f;
    if (logo != 0) {
        const float logo_size = 120.0f;
        draw->AddImage(logo, ImVec2(p.x + (w - logo_size) * 0.5f, y), ImVec2(p.x + (w + logo_size) * 0.5f, y + logo_size));
        y += logo_size + 18.0f;
    } else {
        y += 90.0f;
    }
    {
        // CRAMION con espaciado entre letras, centrado.
        const char* title = "CRAMION";
        const float size = ImGui::GetFontSize() * 2.3f;
        const float spacing = size * 0.18f;
        ImFont* font = ImGui::GetFont();
        float title_w = -spacing;
        for (const char* c = title; *c != '\0'; ++c) {
            title_w += font->CalcTextSizeA(size, FLT_MAX, 0.0f, c, c + 1).x + spacing;
        }
        float x = p.x + (w - title_w) * 0.5f;
        for (const char* c = title; *c != '\0'; ++c) {
            draw->AddText(font, size, ImVec2(x, y), IM_COL32(240, 242, 246, 255), c, c + 1);
            x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, c, c + 1).x + spacing;
        }
        y += size + 30.0f;
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

// Otra escena del proyecto: la barra avanza suave hacia el progreso real
// (sin retroceder) y, ya completa, se queda un momento y desaparece.
void EditorApp::updateSceneLoading(float delta_seconds) {
    ProjectLoad& load = project_load_;
    float fraction = 0.0f;
    std::string text;
    projectLoadProgress(fraction, text);
    load.target = std::max(load.target, fraction);
    load.shown += (load.target - load.shown) * std::min(1.0f, delta_seconds * 8.0f);
    if (load.stage == ProjectLoad::Stage::Done) {
        load.time += delta_seconds;
        load.shown = std::min(1.0f, load.shown + delta_seconds * 3.0f);
        if (load.time > 0.45f) load.reset();
    }
}

void EditorApp::drawSceneLoadingBar(float width) {
    if (!sceneLoading()) return;
    ProjectLoad& load = project_load_;
    float fraction = 0.0f;
    std::string text;
    projectLoadProgress(fraction, text);
    const float shown = std::clamp(load.shown, 0.0f, 1.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::SameLine();
    const float h = std::max(6.0f, std::floor(ImGui::GetFontSize() * 0.55f));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float line = ImGui::GetTextLineHeight();
    const ImVec2 a(p.x, p.y + std::floor((line - h) * 0.5f) + 1.0f);
    const ImVec2 b(a.x + width, a.y + h);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(a, b, theme::kBg3, h * 0.5f);
    if (shown > 0.0f) draw->AddRectFilled(a, ImVec2(a.x + std::max(width * shown, h), b.y), kAccent, h * 0.5f);
    ImGui::Dummy(ImVec2(width, line));
    ImGui::SameLine();
    ImGui::Text("%d %%", static_cast<int>(shown * 100.0f + 0.5f));
}

}  // namespace cramion::editor
