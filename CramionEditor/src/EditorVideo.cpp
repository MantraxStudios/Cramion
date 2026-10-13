// Video en el editor (VideoPlayer, como el de Unity):
//
//   Inspector    caja para soltar el clip, vista previa SIN Play (o la del
//                juego en Play) con reproducir, pausa, parar, frame a frame y
//                barra de tiempo, "Probar URL", la tabla de pistas de audio
//                (volumen, silencio, fuentes AudioSource y "+ Crear fuente
//                hija") y las estadisticas (HW/SW, frames perdidos, bufer).
//   Proyecto     doble clic en un video: ventana de vista previa.
//   Soltar       en la Escena: "Pantalla de video" (plano con la proporcion
//                del video, Material Override sobre la emision y audio Direct,
//                un solo deshacer); en la Jerarquia: el VideoPlayer del objeto;
//                en un hueco de textura de material: Render Texture de video.
//   Pegar        texto http(s) en la Escena: pantalla con Fuente = URL.
//
// La importacion (datos y miniaturas al abrir el proyecto) esta en
// EditorProjectLoad.cpp; el ciclo de Play en EditorPhysics.cpp (el video se
// para SIEMPRE antes que el audio: suelta sus voces de stream).

#include "EditorApp.h"

#include "Dialogs.h"

#include <CramionCore/video/VideoFiles.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace cramion::editor {

using core::Vec3;

namespace {

constexpr float kScreenWidth = 4.0f;    // metros de ancho de una pantalla nueva
constexpr float kPlaneSize = 10.0f;     // el plano integrado mide 10 x 10 m
constexpr std::uint32_t kMaxVideoRt = 1920;  // lado mayor de una Render Texture de video nueva

std::string trim(const std::string& text) {
    std::size_t a = 0, b = text.size();
    while (a < b && static_cast<unsigned char>(text[a]) <= ' ') ++a;
    while (b > a && static_cast<unsigned char>(text[b - 1]) <= ' ') --b;
    return text.substr(a, b - a);
}

// Una sola URL http(s) (sin espacios ni saltos de linea): lo que se pega.
bool looksLikeHttpUrl(const std::string& text) {
    if (text.size() < 10 || text.size() > 4096) return false;
    if (text.rfind("https://", 0) != 0 && text.rfind("http://", 0) != 0) return false;
    return std::none_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) <= ' '; });
}

// "1:05.3" (o "1:02:05" con horas).
std::string timeText(double seconds) {
    if (seconds < 0.0) return "--:--";
    char text[32];
    const int whole = static_cast<int>(seconds);
    if (whole >= 3600) {
        std::snprintf(text, sizeof(text), "%d:%02d:%02d", whole / 3600, (whole / 60) % 60, whole % 60);
    } else {
        std::snprintf(text, sizeof(text), "%d:%04.1f", whole / 60, seconds - static_cast<double>((whole / 60) * 60));
    }
    return text;
}

const char* stateName(video::PlayerState state) {
    switch (state) {
        case video::PlayerState::Idle: return "Parado";
        case video::PlayerState::Preparing: return "Preparando...";
        case video::PlayerState::Prepared: return "Preparado";
        case video::PlayerState::Playing: return "Reproduciendo";
        case video::PlayerState::Paused: return "En pausa";
        case video::PlayerState::Buffering: return "Cargando (búfer)...";
        case video::PlayerState::Ended: return "Terminado";
        case video::PlayerState::Error: return "Error";
    }
    return "";
}

// Los videos que ya se intentaron leer sin cache en esta sesion (si la
// lectura no deja cache, no se reintenta cada frame).
std::unordered_set<std::wstring>& probeAttempted() {
    static std::unordered_set<std::wstring> attempted;
    return attempted;
}

// La imagen de un video encajada en `width` sin deformarla (alto maximo
// `max_height`); un recuadro oscuro con `empty_text` si no hay textura.
void drawVideoImage(ImTextureID texture, std::uint32_t video_w, std::uint32_t video_h, float width, float max_height,
                    const char* empty_text) {
    const float aspect = video_w > 0 && video_h > 0 ? static_cast<float>(video_w) / static_cast<float>(video_h) : 16.0f / 9.0f;
    float w = width;
    float h = w / aspect;
    if (h > max_height) {
        h = max_height;
        w = h * aspect;
    }
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 box_min(start.x, start.y);
    const ImVec2 box_max(start.x + width, start.y + h);
    draw->AddRectFilled(box_min, box_max, IM_COL32(10, 10, 12, 255), 4.0f);
    if (texture != 0) {
        const float x = start.x + (width - w) * 0.5f;
        draw->AddImage(texture, ImVec2(x, start.y), ImVec2(x + w, start.y + h));
    } else if (empty_text != nullptr) {
        const ImVec2 t = ImGui::CalcTextSize(empty_text);
        draw->AddText(ImVec2(start.x + (width - t.x) * 0.5f, start.y + (h - t.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                      empty_text);
    }
    draw->AddRect(box_min, box_max, IM_COL32(0, 0, 0, 160), 4.0f);
    ImGui::Dummy(ImVec2(width, h));
}

void drawStatusLines(const video::PlayerStatus& st) {
    if (st.state == video::PlayerState::Error) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
        ImGui::TextWrapped("Error: %s", st.error.empty() ? "no se pudo abrir el vídeo" : st.error.c_str());
        ImGui::PopStyleColor();
        return;
    }
    ImGui::TextDisabled("%s  ·  %s / %s", stateName(st.state), timeText(st.time).c_str(),
                        st.is_live ? "directo" : timeText(st.length).c_str());
    if (st.width > 0) {
        ImGui::TextDisabled("%ux%u  ·  %.3g fps  ·  %s%s%s", st.width, st.height, st.fps,
                            st.hardware ? "Hardware" : "Software", st.codec.empty() ? "" : "  ·  ", st.codec.c_str());
        ImGui::TextDisabled("Frame %lld  ·  perdidos %llu  ·  búfer %.1f s%s%s", static_cast<long long>(st.frame),
                            static_cast<unsigned long long>(st.dropped), st.buffered_seconds,
                            st.backend.empty() ? "" : "  ·  ", st.backend.c_str());
    }
}

}  // namespace

// =============================================================================
// "Probar URL": se abre en otro hilo (la red puede tardar). Si el editor se
// cierra con la prueba en marcha, se espera un poco y se suelta el hilo (el
// estado es compartido: no queda nada colgando de EditorApp).
// =============================================================================

struct EditorApp::VideoUrlTest {
    struct State {
        std::string url;
        std::atomic<bool> done{false};
        std::mutex mutex;
        bool ok = false;
        std::string text;
    };
    std::shared_ptr<State> state = std::make_shared<State>();
    std::thread thread;

    ~VideoUrlTest() {
        if (!thread.joinable()) return;
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        while (!state->done.load() && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (state->done.load()) thread.join();
        else thread.detach();  // red colgada: termina sola (solo toca su State)
    }
};

void EditorApp::VideoUrlTestDeleter::operator()(VideoUrlTest* test) const { delete test; }

// =============================================================================
// Datos de la cache (miniatura y metadatos) para el navegador y el Inspector
// =============================================================================

const EditorApp::VideoProbeInfo* EditorApp::videoInfo(const std::filesystem::path& file) {
    if (video_info_version_ != database_version_) {
        video_info_.clear();
        video_info_version_ = database_version_;
    }
    const std::wstring key = file.wstring();
    auto it = video_info_.find(key);
    if (it == video_info_.end()) {
        VideoProbeInfo entry;
        video::VideoInfo info;
        if (video::readCachedVideoInfo(videoCacheFolder(), file, info)) {
            entry.ready = info.ok;
            entry.failed = !info.ok;
            entry.error = info.error;
            entry.width = info.stream.width;
            entry.height = info.stream.height;
            entry.fps = info.stream.fps;
            entry.duration = info.stream.duration;
            entry.codec = info.stream.codec;
            entry.backend = info.stream.backend;
            entry.hardware = info.stream.hardware;
            entry.audio_tracks = static_cast<int>(info.stream.audio.size());
            for (const video::AudioTrackInfo& track : info.stream.audio) {
                std::string name = track.name;
                if (!track.language.empty()) name += (name.empty() ? "" : " ") + std::string("(") + track.language + ")";
                entry.track_names.push_back(std::move(name));
            }
            if (info.has_thumbnail) entry.thumbnail = info.thumbnail;
        } else if (probeAttempted().insert(key).second) {
            // Sin cache (video nuevo, copiado o movido): se lee en cuanto se pueda.
            video_probe_queue_.push_back(file);
        }
        it = video_info_.emplace(key, std::move(entry)).first;
    }
    return it->second.ready || it->second.failed ? &it->second : nullptr;
}

// =============================================================================
// Cada frame: lecturas pendientes y la vista previa sin Play
// =============================================================================

void EditorApp::updateVideoEditor(float delta_seconds) {
    if (!video_probe_queue_.empty()) {
        std::vector<std::filesystem::path> queue = std::move(video_probe_queue_);
        video_probe_queue_.clear();
        startVideoImport(std::move(queue));  // si hay una en marcha, vuelven a la cola
    }
    if (playing() || !video_.previewing()) return;
    if (video_preview_entity_.valid()) {
        // La del Inspector: si el objeto se borro o ya no tiene VideoPlayer, fuera.
        // Deshacer recrea las entidades (mismo UUID, otro handle): la vista
        // previa apunta al handle viejo y sus controles ya no la encontrarian.
        const ecs::Entity e = world_.find(video_preview_entity_);
        if (!e.valid() || !e.has<video::VideoPlayer>() || e.handle() != video_preview_handle_) {
            video_.stopPreview();
            video_preview_entity_ = {};
            return;
        }
    } else if (!show_video_preview_) {
        video_.stopPreview();  // se cerro la ventana de vista previa
        return;
    }
    video_.updatePreview(delta_seconds);
}

// =============================================================================
// Pantalla de video, asignar y mover
// =============================================================================

ecs::Entity EditorApp::createVideoScreen(const std::string& clip_or_url, bool is_url, const std::optional<Vec3>& point) {
    // Proporcion del video (de la importacion) o 16:9.
    float aspect = 16.0f / 9.0f;
    if (!is_url) {
        if (const VideoProbeInfo* info = videoInfo(project_.assetsFolder() / dialogs::fromUtf8(clip_or_url));
            info != nullptr && info->width > 0 && info->height > 0) {
            aspect = static_cast<float>(info->width) / static_cast<float>(info->height);
        }
    }
    aspect = std::clamp(aspect, 0.2f, 5.0f);
    const float width = kScreenWidth;
    const float height = width / aspect;
    const scene::Camera& camera = scene_.camera();
    Vec3 position = camera.position() + camera.forward() * 6.0f;
    if (point) position = *point + Vec3{0.0f, height * 0.5f + 0.05f, 0.0f};  // de pie sobre el suelo

    std::string name = "Pantalla de vídeo";
    if (!is_url) name += " (" + dialogs::utf8(dialogs::fromUtf8(clip_or_url).stem()) + ")";
    ecs::Entity e = ecs::createPrimitive(world_, assets::builtin::kPlane, name, prefabStageRoot());
    if (!e.valid()) return {};
    // El plano mira hacia +Y: 90 grados en X lo ponen de pie mirando a +Z (la
    // fila de arriba del video arriba) y el giro en Y lo vuelve hacia la camara.
    const Vec3 to_camera = camera.position() - position;
    const float yaw = std::atan2(to_camera.x, to_camera.z) * 57.2957795f;
    e.setLocalEulerDegrees(Vec3{90.0f, yaw, 0.0f});
    e.setLocalScale(Vec3{width / kPlaneSize, 1.0f, height / kPlaneSize});
    e.setWorldPosition(position);

    video::VideoPlayer& player = e.add<video::VideoPlayer>();
    player.source = is_url ? video::VideoSource::Url : video::VideoSource::Clip;
    if (is_url) player.url = clip_or_url;
    else player.clip = clip_or_url;
    player.render_mode = video::RenderMode::MaterialOverride;
    player.material_slot = -1;
    player.material_property = video::MaterialProperty::Emissive;
    player.emissive_intensity = 1.0f;
    player.audio_output = video::AudioOutput::Direct;  // suena en 3D desde la pantalla
    player.direct_spatial = true;

    selectOnly(e.uuid());
    revealInHierarchy(e.uuid());
    commit();  // un solo paso de deshacer
    std::cout << "[Video] Pantalla creada: " << clip_or_url << "\n";
    return e;
}

void EditorApp::assignVideoClip(ecs::Entity entity, const std::filesystem::path& file) {
    if (!entity.valid()) return;
    video::VideoPlayer& player = entity.has<video::VideoPlayer>() ? entity.get<video::VideoPlayer>()
                                                                  : entity.add<video::VideoPlayer>();
    player.source = video::VideoSource::Clip;
    player.clip = assetRelative(file);
}

void EditorApp::retargetVideoClips(const std::string& from_relative, const std::string& to_relative) {
    int changed = 0;
    for (const entt::entity handle : world_.registry().view<video::VideoPlayer>()) {
        video::VideoPlayer& player = world_.wrap(handle).get<video::VideoPlayer>();
        if (player.clip == from_relative) {
            player.clip = to_relative;
            ++changed;
        }
    }
    video_.invalidateFile(from_relative);
    if (changed > 0) {
        std::cout << "[Video] " << changed << " VideoPlayer de la escena apuntan ahora a " << to_relative
                  << " (las otras escenas guardan la ruta antigua)\n";
        commit();
    }
}

bool EditorApp::pasteVideoUrl() {
    const char* clipboard = ImGui::GetClipboardText();
    if (clipboard == nullptr) return false;
    const std::string text = trim(clipboard);
    if (!looksLikeHttpUrl(text) || text == trim(clipboard_text_at_copy_)) return false;
    std::string error;
    if (!video::checkVideoUrl(text, &error)) {
        std::cerr << "[Video] No se puede usar esa URL: " << error << "\n";
        return true;  // era una URL: no se pegan los objetos copiados en su lugar
    }
    std::optional<Vec3> place;
    Vec3 point{};
    Vec3 normal{};
    if (view_hovered_ && surfaceHit(ImGui::GetMousePos().x, ImGui::GetMousePos().y, point, normal)) place = point;
    createVideoScreen(text, true, place);
    return true;
}

// =============================================================================
// Hueco de textura de un material: "Crear Render Texture de video?"
// =============================================================================

std::string EditorApp::materialVideoDropPopup(const char* popup_id) {
    std::string result;
    if (!ImGui::BeginPopup(popup_id)) return result;
    const std::filesystem::path file = material_video_drop_;
    ImGui::TextUnformatted("¿Crear Render Texture de vídeo?");
    ImGui::TextDisabled("%s", dialogs::utf8(file.filename()).c_str());
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 340.0f);
    ImGui::TextDisabled("Se crea una Render Texture (.crrt) junto al vídeo y un objeto con un VideoPlayer en modo "
                        "Render Texture que la rellena. Este material (y cualquier otro o una UI Image que use la "
                        ".crrt) mostrará el vídeo.");
    ImGui::PopTextWrapPos();
    if (ImGui::Button("Crear", ImVec2(120.0f, 0.0f)) && !file.empty()) {
        assets::RenderTextureAsset texture;
        std::uint32_t w = 1280, h = 720;
        if (const VideoProbeInfo* info = videoInfo(file); info != nullptr && info->width > 0 && info->height > 0) {
            w = info->width;
            h = info->height;
            const std::uint32_t longest = std::max(w, h);
            if (longest > kMaxVideoRt) {
                w = std::max<std::uint32_t>(1, w * kMaxVideoRt / longest);
                h = std::max<std::uint32_t>(1, h * kMaxVideoRt / longest);
            }
        }
        texture.width = static_cast<int>(w);
        texture.height = static_cast<int>(h);
        const std::wstring stem = file.stem().wstring();
        std::filesystem::path target = file.parent_path() / (stem + L" (vídeo)" + dialogs::fromUtf8(assets::kRenderTextureExtension).wstring());
        for (int i = 2; std::filesystem::exists(target); ++i) {
            target = file.parent_path() /
                     (stem + L" (vídeo) " + std::to_wstring(i) + dialogs::fromUtf8(assets::kRenderTextureExtension).wstring());
        }
        std::string error;
        if (assets::saveRenderTexture(texture, target, &error)) {
            refreshDatabase();
            ecs::Entity e = world_.create("Vídeo " + dialogs::utf8(file.stem()), prefabStageRoot());
            video::VideoPlayer& player = e.add<video::VideoPlayer>();
            player.source = video::VideoSource::Clip;
            player.clip = assetRelative(file);
            player.render_mode = video::RenderMode::RenderTexture;
            player.target_texture = assets::AssetRef{texture.uuid, assets::AssetType::RenderTexture};
            player.audio_output = video::AudioOutput::Direct;
            player.direct_spatial = false;  // sin pantalla en la escena: se oye en 2D
            commit();
            result = assetRelative(target);
            std::cout << "[Video] Render Texture de vídeo creada: " << result << "\n";
        } else {
            std::cerr << "[Video] No se pudo crear la Render Texture: " << error << "\n";
        }
        material_video_drop_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancelar", ImVec2(120.0f, 0.0f))) {
        material_video_drop_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return result;
}

// =============================================================================
// Vista previa de un archivo (doble clic en el Proyecto)
// =============================================================================

void EditorApp::previewVideoFile(const std::filesystem::path& file) {
    show_video_preview_ = true;
    video_preview_file_ = file;
    video_preview_entity_ = {};
    if (!playing()) {  // en Play no (no se decodifica dos veces)
        video_.previewFile(file);  // prepara en pausa (VideoSystem.h)
        video_.play(ecs::Entity{});  // Entity{} = la vista previa del archivo
    }
}

void EditorApp::drawVideoPreviewWindow() {
    if (!show_video_preview_) return;
    ImGui::SetNextWindowSize(ImVec2(560.0f, 440.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Vista previa de vídeo###VideoPreview", &show_video_preview_)) {
        ImGui::End();
        return;
    }
    const std::filesystem::path& file = video_preview_file_;
    ImGui::TextUnformatted(dialogs::utf8(file.filename()).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", assetRelative(file).c_str());
    const bool mine = video_.previewing() && !video_preview_entity_.valid();
    const video::PlayerStatus st = mine ? video_.previewStatus() : video::PlayerStatus{};
    const VideoProbeInfo* info = videoInfo(file);
    const ImTextureID texture = mine && video_.previewTexture() >= 0 ? imgui_.renderTexture(video_.previewTexture()) : 0;
    std::uint32_t w = st.width, h = st.height;
    if ((w == 0 || h == 0) && info != nullptr) {
        w = info->width;
        h = info->height;
    }
    const float avail = ImGui::GetContentRegionAvail().x;
    const float max_h = std::max(80.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 4.0f);
    drawVideoImage(texture, w, h, avail, max_h, playing() ? "En Play no hay vista previa" : "Parado");
    if (playing()) {
        ImGui::TextDisabled("La vista previa se para en Play (los reproductores del juego van primero).");
    } else {
        if (ImGui::Button(mine ? "Desde el principio" : "Reproducir")) {
            video_.previewFile(file);
            video_.play(ecs::Entity{});
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!mine);
        if (ImGui::Button("Parar")) video_.stopPreview();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(playing());
        if (ImGui::Button("Crear pantalla en la escena")) createVideoScreen(assetRelative(file), false, std::nullopt);
        ImGui::EndDisabled();
    }
    if (mine) {
        drawStatusLines(st);
    } else if (info != nullptr && info->failed) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "No se pudo leer: %s", info->error.c_str());
    } else if (info != nullptr) {
        ImGui::TextDisabled("%ux%u  ·  %.3g fps  ·  %s  ·  %d pista(s) de audio%s%s", info->width, info->height, info->fps,
                            timeText(info->duration).c_str(), info->audio_tracks, info->codec.empty() ? "" : "  ·  ",
                            info->codec.c_str());
    }
    ImGui::End();
}

// =============================================================================
// Inspector del VideoPlayer (debajo de sus campos reflejados)
// =============================================================================

void EditorApp::drawVideoInspector(ecs::Entity entity) {
    video::VideoPlayer* player = entity.tryGet<video::VideoPlayer>();
    if (player == nullptr) return;
    ImGui::PushID("video_inspector");
    const float full = ImGui::GetContentRegionAvail().x;

    // --- Fuente: caja para soltar el clip o "Probar URL" ---
    if (player->source == video::VideoSource::Clip) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        ImGui::Button(player->clip.empty() ? "Suelta aquí un vídeo (MP4, MOV, WEBM, MKV...)" : player->clip.c_str(), ImVec2(full, 0.0f));
        ImGui::PopStyleColor();
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kVideoPayload)) {
                player->clip = assetRelative(dialogs::fromUtf8(static_cast<const char*>(payload->Data)));
                commit();
            }
            ImGui::EndDragDropTarget();
        }
        if (!player->clip.empty()) {
            const std::filesystem::path file = project_.assetsFolder() / dialogs::fromUtf8(player->clip);
            if (const VideoProbeInfo* info = videoInfo(file); info == nullptr) {
                ImGui::TextDisabled("Leyendo datos del vídeo...");
            } else if (info->failed) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
                ImGui::TextWrapped("No se pudo leer: %s", info->error.c_str());
                ImGui::PopStyleColor();
            } else {
                ImGui::TextDisabled("%ux%u  ·  %.3g fps  ·  %s  ·  %s%s", info->width, info->height, info->fps,
                                    timeText(info->duration).c_str(), info->codec.c_str(),
                                    info->hardware ? " (hardware)" : "");
            }
        }
    } else {
        const bool busy = video_url_test_ && !video_url_test_->state->done.load();
        ImGui::BeginDisabled(busy || player->url.empty());
        if (ImGui::Button(busy ? "Probando..." : "Probar URL", ImVec2(full, 0.0f))) {
            video_url_test_.reset();
            std::unique_ptr<VideoUrlTest, VideoUrlTestDeleter> test(new VideoUrlTest());
            test->state->url = trim(player->url);
            std::shared_ptr<VideoUrlTest::State> state = test->state;
            test->thread = std::thread([state] {
                video::VideoInfo info;
                const bool ok = video::probeVideoUri(state->url, info, nullptr);
                std::string text;
                if (ok) {
                    char line[200];
                    std::snprintf(line, sizeof(line), "%ux%u  ·  %.3g fps  ·  %s  ·  %zu pista(s) de audio", info.stream.width,
                                  info.stream.height, info.stream.fps,
                                  info.stream.is_live || info.stream.duration < 0.0 ? "directo" : timeText(info.stream.duration).c_str(),
                                  info.stream.audio.size());
                    text = line;
                    if (!info.stream.codec.empty() || !info.stream.backend.empty()) {
                        text += "\n" + info.stream.codec + (info.stream.backend.empty() ? "" : " (" + info.stream.backend + ")") +
                                (info.stream.hardware ? ", hardware" : "");
                    }
                } else {
                    text = info.error.empty() ? std::string("No se pudo abrir la URL") : info.error;
                }
                {
                    std::lock_guard lock(state->mutex);
                    state->ok = ok;
                    state->text = std::move(text);
                }
                state->done = true;
            });
            video_url_test_ = std::move(test);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Abre la URL (en otro hilo) y muestra sus datos o por qué no se puede.\n"
                              "Solo https (o http a este PC). Recomendado: H.264 + AAC.");
        if (video_url_test_ && video_url_test_->state->done.load() && video_url_test_->state->url == trim(player->url)) {
            std::lock_guard lock(video_url_test_->state->mutex);
            if (video_url_test_->state->ok) {
                ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.55f, 1.0f), "Se puede abrir");
                ImGui::TextDisabled("%s", video_url_test_->state->text.c_str());
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
                ImGui::TextWrapped("%s", video_url_test_->state->text.c_str());
                ImGui::PopStyleColor();
            }
        }
    }

    // --- Vista previa (sin Play) o la imagen del juego (en Play) ---
    ImGui::Spacing();
    const bool in_play = playing();
    const bool previewing_this = !in_play && video_.previewing() && video_preview_entity_ == entity.uuid();
    video::PlayerStatus st;
    std::int32_t texture_id = -1;
    if (in_play) {
        st = video_.status(entity);
        texture_id = video_.textureOf(entity);
    } else if (previewing_this) {
        st = video_.previewStatus();
        texture_id = video_.previewTexture();
    }
    const ImTextureID texture = texture_id >= 0 ? imgui_.renderTexture(texture_id) : 0;
    drawVideoImage(texture, st.width, st.height, full, full * 0.75f, in_play ? "Sin imagen" : "Vista previa (pulsa Reproducir)");

    // --- Transporte ---
    const float button_w = (full - ImGui::GetStyle().ItemSpacing.x * 3.0f) / 4.0f;
    const bool active = in_play || previewing_this;
    const bool running = st.state == video::PlayerState::Playing || st.state == video::PlayerState::Buffering;
    if (ImGui::Button(running ? "Pausa" : "Reproducir", ImVec2(button_w, 0.0f))) {
        if (running) {
            video_.pause(entity);
        } else if (!in_play && !previewing_this) {
            // Sin Play: la vista previa de este objeto (audio en 2D).
            video_.previewEntity(world_, entity);  // prepara en pausa (VideoSystem.h)
            video_.play(entity);
            video_preview_entity_ = entity.uuid();
            video_preview_handle_ = entity.handle();
        } else {
            video_.play(entity);
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!active);
    if (ImGui::Button("Parar", ImVec2(button_w, 0.0f))) {
        if (in_play) {
            video_.stop(entity);
        } else {
            video_.stopPreview();
            video_preview_entity_ = {};
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Frame +1", ImVec2(button_w, 0.0f))) video_.stepForward(entity, 1);
    ImGui::SetItemTooltip("Pausa y avanza un frame");
    ImGui::SameLine();
    if (ImGui::Button("Inicio", ImVec2(button_w, 0.0f))) video_.seek(entity, 0.0);
    ImGui::EndDisabled();
    // Barra de tiempo: mientras se arrastra no se busca (solo al soltar).
    if (active && st.length > 0.0 && !st.is_live) {
        static float dragging = -1.0f;
        float t = dragging >= 0.0f ? dragging : static_cast<float>(st.time);
        ImGui::SetNextItemWidth(full);
        const std::string label = timeText(t) + " / " + timeText(st.length);
        if (ImGui::SliderFloat("##time", &t, 0.0f, static_cast<float>(st.length), label.c_str())) dragging = t;
        if (ImGui::IsItemDeactivatedAfterEdit() || (dragging >= 0.0f && !ImGui::IsItemActive())) {
            video_.seek(entity, static_cast<double>(dragging >= 0.0f ? dragging : t));
            dragging = -1.0f;
        }
    }
    if (active) drawStatusLines(st);

    // --- Pistas de audio ---
    ImGui::Spacing();
    ImGui::SeparatorText("Pistas de audio");
    int known_tracks = st.audio_tracks;
    std::vector<std::string> track_names;
    if (player->source == video::VideoSource::Clip && !player->clip.empty()) {
        if (const VideoProbeInfo* info = videoInfo(project_.assetsFolder() / dialogs::fromUtf8(player->clip)); info != nullptr) {
            known_tracks = std::max(known_tracks, info->audio_tracks);
            track_names = info->track_names;
        }
    }
    if (static_cast<int>(player->tracks.size()) < known_tracks) {
        const std::string label = "Crear las " + std::to_string(known_tracks) + " pistas del vídeo";
        if (ImGui::Button(label.c_str(), ImVec2(full, 0.0f))) {
            player->tracks.resize(static_cast<std::size_t>(known_tracks));
            commit();
        }
    }
    if (player->tracks.empty()) {
        ImGui::TextDisabled(known_tracks == 0 ? "Sin pistas de audio (o aún sin leer): sale la primera con el volumen general."
                                              : "Sin configurar: suenan con el volumen general.");
    }
    if (!player->tracks.empty() && player->audio_output != video::AudioOutput::AudioSource) {
        bool any_target = false;
        for (const video::VideoAudioTrack& t : player->tracks) any_target = any_target || !t.targets.empty();
        if (any_target) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Las fuentes solo se usan con Salida de audio = AudioSource.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Usar AudioSource")) {
                player->audio_output = video::AudioOutput::AudioSource;
                commit();
            }
        }
    }
    int remove_track = -1;
    for (std::size_t i = 0; i < player->tracks.size(); ++i) {
        video::VideoAudioTrack& track = player->tracks[i];
        ImGui::PushID(static_cast<int>(i));
        const std::string title = "Pista " + std::to_string(i) +
                                  (i < track_names.size() && !track_names[i].empty() ? "  ·  " + track_names[i] : std::string());
        ImGui::TextUnformatted(title.c_str());
        ImGui::SameLine(full - ImGui::GetFrameHeight());
        if (ImGui::SmallButton("x")) remove_track = static_cast<int>(i);
        ImGui::SetItemTooltip("Quitar la pista de la lista");
        if (ImGui::Checkbox("Activa", &track.enabled)) commit();
        ImGui::SameLine();
        if (ImGui::Checkbox("Silenciar", &track.mute)) {
            if (in_play) video_.setTrackVolume(entity, static_cast<int>(i), track.mute ? 0.0f : track.volume);
            commit();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x));
        if (ImGui::SliderFloat("##volume", &track.volume, 0.0f, 1.0f, "Volumen %.2f")) {
            if (in_play && !track.mute) video_.setTrackVolume(entity, static_cast<int>(i), track.volume);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) commit();

        // Fuentes de audio (AudioSource): el mismo sonido en N altavoces.
        int remove_target = -1;
        for (std::size_t k = 0; k < track.targets.size(); ++k) {
            ImGui::PushID(static_cast<int>(k));
            const ecs::Entity target = world_.find(track.targets[k]);
            const bool has_source = target.valid() && target.has<audio::AudioSource>();
            ImGui::Bullet();
            if (!target.valid()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "(objeto borrado)");
            else if (!has_source) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s (sin AudioSource)", target.name().c_str());
            else ImGui::TextUnformatted(target.name().c_str());
            if (target.valid() && ImGui::IsItemClicked()) {
                selectOnly(target.uuid());
                revealInHierarchy(target.uuid());
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) remove_target = static_cast<int>(k);
            ImGui::PopID();
        }
        if (remove_target >= 0) {
            track.targets.erase(track.targets.begin() + remove_target);
            commit();
        }
        // Anadir: arrastrar objetos de la Jerarquia, elegir uno con AudioSource o crear un hijo.
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("+ Fuente...", ImVec2(half, 0.0f))) ImGui::OpenPopup("pick_source");
        ImGui::SetItemTooltip("Elige un objeto con AudioSource (o arrastra objetos de la Jerarquía aquí).\n"
                              "Varios = el mismo sonido en varios altavoces, con sus efectos y su 3D.");
        if (ImGui::BeginDragDropTarget()) {
            if (ImGui::AcceptDragDropPayload(kEntityPayload)) {
                for (ecs::Entity e : draggedEntities()) {
                    if (!e.has<audio::AudioSource>()) {
                        audio::AudioSource& source = e.add<audio::AudioSource>();
                        source.play_on_awake = false;  // solo suena el video
                    }
                    if (findUuid(track.targets, e.uuid()) == track.targets.end()) track.targets.push_back(e.uuid());
                }
                commit();
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopup("pick_source")) {
            int shown = 0;
            for (const entt::entity handle : world_.registry().view<audio::AudioSource>()) {
                const ecs::Entity candidate = world_.wrap(handle);
                if (findUuid(track.targets, candidate.uuid()) != track.targets.end()) continue;
                if (ImGui::Selectable(candidate.name().c_str())) {
                    track.targets.push_back(candidate.uuid());
                    commit();
                }
                if (++shown >= 200) break;
            }
            if (shown == 0) ImGui::TextDisabled("No hay más objetos con AudioSource.");
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("+ Crear fuente hija", ImVec2(half, 0.0f))) {
            // Un altavoz hijo con su AudioSource (sin clip propio), a los lados.
            const int n = static_cast<int>(track.targets.size());
            ecs::Entity child = world_.create("Altavoz " + std::to_string(i) + "." + std::to_string(n + 1), entity);
            const float side = (n % 2 == 0 ? -1.0f : 1.0f) * (1.0f + static_cast<float>(n / 2));
            child.setLocalPosition(Vec3{side, 0.0f, 0.0f});
            audio::AudioSource& source = child.add<audio::AudioSource>();
            source.play_on_awake = false;
            source.spatial = true;
            track.targets.push_back(child.uuid());
            player->audio_output = video::AudioOutput::AudioSource;
            revealInHierarchy(child.uuid());
            commit();
        }
        ImGui::SetItemTooltip("Un objeto hijo con AudioSource que reproduce esta pista (muévelo donde quieras\n"
                              "el altavoz). La Salida de audio pasa a AudioSource.");
        ImGui::Separator();
        ImGui::PopID();
        // El puntero de la pista no vale tras borrar: se sale del bucle.
        if (remove_track >= 0) break;
    }
    if (remove_track >= 0) {
        player->tracks.erase(player->tracks.begin() + remove_track);
        commit();
    }
    if (ImGui::SmallButton("+ Pista")) {
        player->tracks.emplace_back();
        commit();
    }
    ImGui::PopID();
}

}  // namespace cramion::editor
