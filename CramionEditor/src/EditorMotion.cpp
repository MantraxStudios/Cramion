// Motion Matching en el editor: el editor de la base (.crmmdb: clips,
// etiquetas, velocidad de los clips en el sitio, recortes y pesos) y el dibujo
// de depuracion en la Escena (la trayectoria pedida en azul, la del fotograma
// elegido en naranja) para los objetos con el componente Motion Matching.

#include "EditorApp.h"

#include "Dialogs.h"

#include <CramionCore/anim/MotionMatching.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <cstring>
#include <iostream>

namespace cramion::editor {

struct MotionEditorState {
    bool show = false;
    bool focus = false;
    Uuid uuid{};
    std::filesystem::path path;
    anim::MotionDatabaseAsset db;
    bool dirty = false;
    int selected = -1;
};

MotionEditorState& EditorApp::motionEditor() {
    if (!motion_editor_) motion_editor_ = std::make_shared<MotionEditorState>();
    return *motion_editor_;
}

Uuid EditorApp::createMotionDatabaseAsset(const std::filesystem::path& folder) {
    anim::MotionDatabaseAsset db;
    // Los clips (.cranim) seleccionados en el Proyecto entran ya.
    for (const std::string& key : browser_selection_) {
        const Uuid uuid = Uuid::parse(key);
        if (!uuid.valid() || !database_) continue;
        const auto info = database_->find(uuid);
        if (!info || info->type != assets::AssetType::AnimationClip) continue;
        anim::MotionClipEntry e;
        e.clip = assets::AssetRef{uuid, assets::AssetType::AnimationClip};
        db.clips.push_back(e);
    }
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    std::filesystem::path path = folder / "Locomocion.crmmdb";
    for (int i = 2; std::filesystem::exists(path); ++i) path = folder / dialogs::fromUtf8("Locomocion " + std::to_string(i) + ".crmmdb");
    std::string error;
    if (!anim::saveMotionDatabase(db, path, &error)) {
        std::cerr << "[Editor] No se pudo crear la base de Motion Matching: " << error << "\n";
        return {};
    }
    refreshDatabase();
    openMotionDatabaseEditor(db.uuid);
    return db.uuid;
}

void EditorApp::openMotionDatabaseEditor(const Uuid& uuid) {
    MotionEditorState& st = motionEditor();
    const auto info = database_ ? database_->find(uuid) : std::nullopt;
    if (!info) return;
    anim::MotionDatabaseAsset db;
    std::string error;
    if (!anim::loadMotionDatabase(info->path, db, &error)) {
        std::cerr << "[Editor] No se pudo abrir la base de Motion Matching: " << error << "\n";
        return;
    }
    st.db = std::move(db);
    st.uuid = uuid;
    st.path = info->path;
    st.dirty = false;
    st.selected = st.db.clips.empty() ? -1 : 0;
    st.show = true;
    st.focus = true;
}

void EditorApp::drawMotionDatabaseEditor() {
    MotionEditorState& st = motionEditor();
    if (!st.show) return;
    if (st.focus) {
        ImGui::SetNextWindowFocus();
        st.focus = false;
    }
    ImGui::SetNextWindowSize(ImVec2(860.0f, 560.0f), ImGuiCond_FirstUseEver);
    const std::string title = "Motion Matching: " + dialogs::utf8(st.path.stem()) + (st.dirty ? " *" : "") + "###motion_db";
    if (!ImGui::Begin(title.c_str(), &st.show)) {
        ImGui::End();
        return;
    }
    anim::MotionDatabaseAsset& db = st.db;
    bool changed = false;

    ImGui::BeginChild("mm_left", ImVec2(320.0f, 0.0f), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
    ImGui::SeparatorText("Clips");
    ImGui::TextDisabled("Arrastra clips (.cranim) aquí");
    for (std::size_t i = 0; i < db.clips.size(); ++i) {
        const anim::MotionClipEntry& c = db.clips[i];
        std::string label = c.clip_name;
        if (c.clip.valid() && database_) {
            if (const auto info = database_->find(c.clip.uuid)) label = info->name;
        }
        if (label.empty()) label = "(sin clip)";
        if (!c.tags.empty()) label += "  [" + c.tags + "]";
        if (ImGui::Selectable((label + "##" + std::to_string(i)).c_str(), st.selected == static_cast<int>(i))) st.selected = static_cast<int>(i);
    }
    // Soltar clips
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, 40.0f));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            Uuid uuid{};
            std::memcpy(&uuid, payload->Data, sizeof(Uuid));
            const auto info = database_ ? database_->find(uuid) : std::nullopt;
            if (info && info->type == assets::AssetType::AnimationClip) {
                anim::MotionClipEntry e;
                e.clip = assets::AssetRef{uuid, assets::AssetType::AnimationClip};
                db.clips.push_back(e);
                st.selected = static_cast<int>(db.clips.size()) - 1;
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::Button("+ Clip del modelo (por nombre)")) {
        db.clips.push_back(anim::MotionClipEntry{});
        db.clips.back().clip_name = "Idle";
        st.selected = static_cast<int>(db.clips.size()) - 1;
        changed = true;
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("mm_right", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    if (st.selected >= 0 && st.selected < static_cast<int>(db.clips.size())) {
        anim::MotionClipEntry& c = db.clips[static_cast<std::size_t>(st.selected)];
        ImGui::SeparatorText("Clip");
        changed |= ImGui::InputTextWithHint("Nombre en el modelo", "si no hay .cranim", &c.clip_name);
        changed |= ImGui::InputTextWithHint("Etiquetas", "agachado, combate", &c.tags);
        ImGui::SetItemTooltip("Con entity:setMotionTags(\"agachado\") solo se buscan los fotogramas con esa etiqueta");
        changed |= ImGui::Checkbox("Bucle", &c.loop);
        changed |= ImGui::DragFloat3("Velocidad (m/s)", &c.velocity.x, 0.01f);
        ImGui::SetItemTooltip("Clips en el sitio (Mixamo In Place): lo que avanzaría el personaje (+Z delante)");
        changed |= ImGui::DragFloat("Giro (°/s)", &c.turn_rate, 0.5f, -720.0f, 720.0f, "%.1f");
        changed |= ImGui::DragFloat("Desde (s)", &c.start, 0.01f, 0.0f, 600.0f, "%.2f");
        changed |= ImGui::DragFloat("Hasta (s)", &c.end, 0.01f, 0.0f, 600.0f, "%.2f");
        ImGui::SetItemTooltip("0 = hasta el final");
        changed |= ImGui::DragFloat("Sesgo del coste", &c.cost_bias, 0.01f, -5.0f, 5.0f, "%.2f");
        ImGui::SetItemTooltip("> 0 se elige menos; < 0 más (un idle preferido)");
        if (ImGui::Button("Quitar clip")) {
            db.clips.erase(db.clips.begin() + st.selected);
            st.selected = -1;
            changed = true;
        }
    }
    ImGui::SeparatorText("Base");
    changed |= ImGui::DragFloat("Fotogramas/s", &db.sample_rate, 0.5f, 5.0f, 120.0f, "%.0f");
    changed |= ImGui::DragFloat3("Trayectoria (s)", db.trajectory_times.data(), 0.01f, 0.05f, 3.0f, "%.2f");
    changed |= ImGui::Checkbox("Quitar root motion", &db.strip_root_motion);
    ImGui::SetItemTooltip("La pose se queda en el sitio: al personaje lo mueve su script o su Character Controller");
    ImGui::SeparatorText("Pesos de los rasgos");
    changed |= ImGui::SliderFloat("Posición de la trayectoria", &db.weight_trajectory_position, 0.0f, 5.0f, "%.2f");
    changed |= ImGui::SliderFloat("Dirección de la trayectoria", &db.weight_trajectory_direction, 0.0f, 5.0f, "%.2f");
    changed |= ImGui::SliderFloat("Posición de los pies", &db.weight_foot_position, 0.0f, 5.0f, "%.2f");
    changed |= ImGui::SliderFloat("Velocidad de los pies", &db.weight_foot_velocity, 0.0f, 5.0f, "%.2f");
    changed |= ImGui::SliderFloat("Velocidad de la cadera", &db.weight_hip_velocity, 0.0f, 5.0f, "%.2f");
    ImGui::SeparatorText("Huesos (vacío = humanoide automático)");
    changed |= ImGui::InputText("Cadera", &db.hips_bone);
    changed |= ImGui::InputText("Pie izquierdo", &db.left_foot_bone);
    changed |= ImGui::InputText("Pie derecho", &db.right_foot_bone);
    if (ImGui::Button("Asignar a la selección")) {
        int assigned = 0;
        for (ecs::Entity e : topLevelSelection()) {
            anim::MotionMatching& mm = e.has<anim::MotionMatching>() ? e.get<anim::MotionMatching>() : e.add<anim::MotionMatching>();
            mm.database = assets::AssetRef{st.uuid, assets::AssetType::MotionDatabase};
            ++assigned;
        }
        if (assigned > 0) commit();
    }
    ImGui::SetItemTooltip("Va en el objeto del modelo animado (el del MeshRenderer con esqueleto)");
    ImGui::EndChild();

    if (changed) st.dirty = true;
    if (st.dirty && !ImGui::IsAnyItemActive() && ImGui::GetIO().MouseDownDuration[0] < 0.0f) {
        std::string error;
        if (anim::saveMotionDatabase(db, st.path, &error)) st.dirty = false;
        else std::cerr << "[Editor] " << error << "\n";
    }
    ImGui::End();
}

void EditorApp::drawMotionMatchingDebug() {
    // Trayectorias de los seleccionados con Motion Matching (y depuracion activa).
    for (const ecs::Entity e : selectedEntities()) {
        const anim::MotionMatching* mm = e.tryGet<anim::MotionMatching>();
        if (mm == nullptr || !mm->debug_draw || !mm->runtime.debug_valid) continue;
        const anim::MotionMatchingRuntime& rt = mm->runtime;
        core::Vec3 previous = rt.debug_origin;
        core::Vec3 previous_matched = rt.debug_origin;
        for (int i = 0; i < anim::kTrajectoryPoints; ++i) {
            const core::Vec3 p = rt.debug_desired[static_cast<std::size_t>(i)];
            const core::Vec3 q = rt.debug_matched[static_cast<std::size_t>(i)];
            overlayLine(previous, p, 0xFFFFA040u);
            overlayLine(p, p + rt.debug_desired_dir[static_cast<std::size_t>(i)] * 0.35f, 0xFFFFC080u);
            overlayLine(previous_matched, q, 0xFF40A0FFu);
            overlayLine(q, q + rt.debug_matched_dir[static_cast<std::size_t>(i)] * 0.35f, 0xFF80C0FFu);
            previous = p;
            previous_matched = q;
        }
    }
}

}  // namespace cramion::editor
