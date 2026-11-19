// Ventana de clips (Ventana > Clips de animacion): elige un clip del objeto
// animado seleccionado, lo reproduce o recorre con la linea de tiempo sobre el
// propio modelo en la Escena (vista previa del Animator: runtime.preview_clip)
// y marca sus eventos (notifies): doble clic en la linea para anadir,
// arrastrar para mover, Supr para quitar. Guardar los escribe junto al clip:
// en la cabecera de su .cranim o, si es un clip de dentro del modelo, en
// <modelo>.crdata.crclipmeta (el .crdata no se toca).
//
// La logica (ordenar, mover, linea de tiempo, guardar) esta en
// AnimatorController.h y tiene pruebas en CramionAnimationTests.

#include "EditorApp.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace cramion::editor {

struct ClipEditorState {
    entt::entity entity = entt::null;  // el objeto de la vista previa
    int clip = -1;                     // indice en ModelData::animations
    std::string clip_name;
    ecs::ClipMeta meta;
    bool dirty = false;
    ecs::ClipPreviewPlayer player;
    bool preview = true;
    float fps = 30.0f;  // al fotograma al arrastrar (0 = libre)
    int selected = -1;
    int dragging = -1;
    std::string status;
};

namespace {

// Donde se guarda la meta de un clip: un .cranim con ese nombre o el archivo
// lateral del modelo.
struct ClipTarget {
    std::filesystem::path cranim;
    std::filesystem::path sidecar;
};

ClipTarget clipTarget(const assets::AssetDatabase& database, const Uuid& model, const std::string& clip_name) {
    ClipTarget t;
    for (const assets::AssetInfo& info : database.all()) {
        if (info.type == assets::AssetType::AnimationClip && info.name == clip_name) {
            t.cranim = info.path;
            return t;
        }
    }
    if (const auto info = database.find(model)) t.sidecar = ecs::clipMetaSidecarPath(info->path);
    return t;
}

}  // namespace

void EditorApp::drawClipEditor() {
    if (!clip_editor_) clip_editor_ = std::make_shared<ClipEditorState>();
    ClipEditorState& st = *clip_editor_;
    // Suelta la vista previa del objeto anterior (o al cerrar la ventana).
    const auto releasePreview = [&]() {
        if (st.entity != entt::null && world_.valid(st.entity)) {
            if (ecs::Animator* a = world_.wrap(st.entity).tryGet<ecs::Animator>()) a->runtime.preview_clip = -1;
        }
    };
    if (!show_clip_editor_ || !sync_) {
        releasePreview();
        st.entity = entt::null;
        return;
    }
    if (!ImGui::Begin("Clips de animación###clip_editor", &show_clip_editor_)) {
        ImGui::End();
        return;
    }

    // Objeto: lo animado de la seleccion o el del Animator abierto.
    ecs::Entity target = animatedEntityIn(world_.find(active_));
    if (!target.valid()) target = animatorPreviewEntity();
    if (!target.valid() || target.handle() != st.entity) {
        releasePreview();
        st.entity = target.valid() ? target.handle() : entt::null;
        st.clip = -1;
        st.selected = -1;
        st.dirty = false;
    }
    const asset::ModelData* data = target.valid() ? sync_->actorModelData(target, scene_) : nullptr;
    ecs::Animator* live = target.valid() ? target.tryGet<ecs::Animator>() : nullptr;
    if (data == nullptr || data->animations.empty() || live == nullptr) {
        ImGui::TextDisabled("Selecciona un objeto animado (con Animator y clips).");
        ImGui::End();
        return;
    }
    ImGui::Text("Objeto: %s", target.name().c_str());

    // --- Clip ---
    if (st.clip >= static_cast<int>(data->animations.size())) st.clip = -1;
    const char* current = st.clip >= 0 ? data->animations[static_cast<std::size_t>(st.clip)].name.c_str() : "(elige un clip)";
    if (ImGui::BeginCombo("Clip", current)) {
        for (std::size_t i = 0; i < data->animations.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(data->animations[i].name.empty() ? "(sin nombre)" : data->animations[i].name.c_str(),
                                  static_cast<int>(i) == st.clip)) {
                st.clip = static_cast<int>(i);
                st.clip_name = data->animations[i].name;
                st.meta = ecs::clipMetaOf(data->animations[i]);
                st.player = ecs::ClipPreviewPlayer{};
                st.selected = -1;
                st.dirty = false;
                st.status.clear();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (st.clip < 0) {
        live->runtime.preview_clip = -1;
        ImGui::End();
        return;
    }
    const asset::AnimationClip& clip = data->animations[static_cast<std::size_t>(st.clip)];
    const float duration = std::max(clip.duration, 1e-3f);

    // --- Reproduccion ---
    if (ImGui::Button(st.player.playing ? "Pausa" : "Reproducir")) st.player.playing = !st.player.playing;
    ImGui::SameLine();
    ImGui::Checkbox("Bucle", &st.player.loop);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::DragFloat("Velocidad", &st.player.speed, 0.01f, -4.0f, 4.0f, "%.2f");
    ImGui::SameLine();
    ImGui::Checkbox("Ver en el modelo", &st.preview);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f);
    ImGui::DragFloat("FPS", &st.fps, 1.0f, 0.0f, 240.0f, "%.0f");
    st.player.step(ImGui::GetIO().DeltaTime, duration);
    live->runtime.preview_clip = st.preview ? st.clip : -1;
    live->runtime.preview_time = st.player.time;

    // --- Linea de tiempo ---
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(ImGui::GetContentRegionAvail().x - 8.0f, 50.0f);
    const float height = 56.0f;
    ImGui::InvisibleButton("##timeline", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(32, 34, 40, 255), 4.0f);
    // Marcas de segundo (y de fotograma si caben).
    const float frames = st.fps > 0.0f ? duration * st.fps : 0.0f;
    const bool frame_ticks = frames > 0.0f && width / frames > 6.0f;
    const int ticks = frame_ticks ? static_cast<int>(frames) : static_cast<int>(std::floor(duration * 10.0f));
    for (int k = 0; k <= ticks; ++k) {
        const float t = frame_ticks ? k / st.fps : k * 0.1f;
        const float x = ecs::timelineX(t, origin.x, width, duration);
        const bool major = std::abs(t - std::round(t)) < 1e-4f;
        draw->AddLine(ImVec2(x, origin.y + height - (major ? 14.0f : 6.0f)), ImVec2(x, origin.y + height),
                      major ? IM_COL32(160, 160, 170, 255) : IM_COL32(90, 90, 100, 255));
    }
    // Secciones (montages).
    for (const asset::ClipSection& s : st.meta.sections) {
        const float x = ecs::timelineX(s.time, origin.x, width, duration);
        draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + height), IM_COL32(120, 200, 120, 200), 1.5f);
        draw->AddText(ImVec2(x + 3.0f, origin.y + 2.0f), IM_COL32(150, 220, 150, 255), s.name.c_str());
    }
    // Eventos: rombos que se arrastran.
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int hovered_event = -1;
    for (std::size_t i = 0; i < st.meta.events.size(); ++i) {
        const float x = ecs::timelineX(st.meta.events[i].time, origin.x, width, duration);
        const float y = origin.y + height * 0.5f;
        const bool selected = static_cast<int>(i) == st.selected;
        const ImU32 color = selected ? IM_COL32(255, 200, 60, 255) : IM_COL32(230, 120, 60, 255);
        draw->AddQuadFilled(ImVec2(x, y - 7.0f), ImVec2(x + 7.0f, y), ImVec2(x, y + 7.0f), ImVec2(x - 7.0f, y), color);
        if (std::abs(mouse.x - x) <= 7.0f && std::abs(mouse.y - y) <= 9.0f) hovered_event = static_cast<int>(i);
    }
    if (hovered && hovered_event >= 0 && st.dragging < 0) {
        ImGui::SetTooltip("%s  (%.3f s)", st.meta.events[static_cast<std::size_t>(hovered_event)].name.c_str(),
                          st.meta.events[static_cast<std::size_t>(hovered_event)].time);
    }
    const float mouse_time = ecs::timelineTimeAt(mouse.x, origin.x, width, duration, st.fps);
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hovered_event < 0) {
        st.selected = ecs::addClipEvent(st.meta, "Evento", mouse_time, duration);
        st.dirty = true;
    } else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hovered_event >= 0) {
            st.selected = hovered_event;
            st.dragging = hovered_event;
        } else {
            st.player.time = mouse_time;  // recorrer
            st.player.playing = false;
        }
    }
    if (st.dragging >= 0) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const float t = mouse_time;
            if (std::abs(st.meta.events[static_cast<std::size_t>(st.dragging)].time - t) > 1e-6f) {
                st.dragging = ecs::moveClipEvent(st.meta, st.dragging, t, duration);
                st.selected = st.dragging;
                st.dirty = true;
            }
        } else {
            st.dragging = -1;
        }
    } else if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && hovered_event < 0) {
        st.player.time = mouse_time;
    }
    // Cabezal.
    const float head = ecs::timelineX(st.player.time, origin.x, width, duration);
    draw->AddLine(ImVec2(head, origin.y), ImVec2(head, origin.y + height), IM_COL32(90, 170, 255, 255), 2.0f);
    ImGui::Text("%.3f / %.3f s", st.player.time, clip.duration);
    if (st.fps > 0.0f) {
        ImGui::SameLine();
        ImGui::TextDisabled("(fotograma %d)", static_cast<int>(std::round(st.player.time * st.fps)));
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Evento aquí")) {
        st.selected = ecs::addClipEvent(st.meta, "Evento", st.player.time, duration);
        st.dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Sección aquí")) {
        st.meta.sections.push_back({"Seccion" + std::to_string(st.meta.sections.size() + 1), st.player.time, ""});
        st.meta.sort();
        st.dirty = true;
    }

    // --- Evento seleccionado ---
    if (st.selected >= static_cast<int>(st.meta.events.size())) st.selected = -1;
    if (st.selected >= 0) {
        asset::ClipEvent& ev = st.meta.events[static_cast<std::size_t>(st.selected)];
        ImGui::SeparatorText("Evento");
        if (ImGui::InputText("Nombre", &ev.name)) st.dirty = true;
        if (ImGui::InputText("Parámetros", &ev.params)) st.dirty = true;
        ImGui::SetItemTooltip("Texto o JSON: llega en OnAnimationEvent (params)");
        float time = ev.time;
        if (ImGui::DragFloat("Tiempo", &time, 0.001f, 0.0f, duration, "%.3f s")) {
            st.selected = ecs::moveClipEvent(st.meta, st.selected, time, duration);
            st.dirty = true;
        }
        if (ImGui::Button("Quitar evento") || (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete))) {
            ecs::removeClipEvent(st.meta, st.selected);
            st.selected = -1;
            st.dirty = true;
        }
    }

    // --- Secciones ---
    if (!st.meta.sections.empty() && ImGui::TreeNode("Secciones (montages)")) {
        int remove = -1;
        for (std::size_t i = 0; i < st.meta.sections.size(); ++i) {
            asset::ClipSection& s = st.meta.sections[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::InputText("##n", &s.name)) st.dirty = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::DragFloat("##t", &s.time, 0.001f, 0.0f, duration, "%.3f s")) st.dirty = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::InputTextWithHint("##next", "siguiente", &s.next)) st.dirty = true;
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) remove = static_cast<int>(i);
            ImGui::PopID();
        }
        if (remove >= 0) {
            st.meta.sections.erase(st.meta.sections.begin() + remove);
            st.dirty = true;
        }
        ImGui::TreePop();
    }
    if (!st.meta.curves.empty()) {
        ImGui::TextDisabled("Curvas: %zu (se leen con entity().animatorCurve)", st.meta.curves.size());
    }

    // --- Guardar ---
    ImGui::Separator();
    const ecs::MeshRenderer* renderer = target.tryGet<ecs::MeshRenderer>();
    const ClipTarget where = clipTarget(*database_, renderer != nullptr ? renderer->model.uuid : Uuid{}, clip.name);
    ImGui::BeginDisabled(!st.dirty);
    if (ImGui::Button("Guardar")) {
        st.meta.sort();
        std::string error;
        bool ok = false;
        if (!where.cranim.empty()) {
            ok = ecs::saveClipMetaToCranim(where.cranim, st.meta, &error);  // la recarga en caliente lo relee
        } else if (!where.sidecar.empty()) {
            std::map<std::string, ecs::ClipMeta> clips;
            std::error_code ec;
            if (std::filesystem::exists(where.sidecar, ec)) ecs::loadModelClipMeta(where.sidecar, clips, &error);
            clips[clip.name] = st.meta;
            ok = ecs::saveModelClipMeta(where.sidecar, clips, &error);
            sync_->reloadClipMeta();
        } else {
            error = "no se sabe de que archivo es el clip";
        }
        st.dirty = !ok;
        st.status = ok ? "Guardado." : "No se pudo guardar: " + error;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", !where.cranim.empty() ? "en su .cranim" : "en <modelo>.crdata.crclipmeta");
    if (!st.status.empty()) ImGui::TextWrapped("%s", st.status.c_str());
    ImGui::End();
}

}  // namespace cramion::editor
