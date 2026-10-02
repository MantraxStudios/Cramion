// VFX Graph, 2D y repeticiones en el editor:
//
//   - Cada frame: la simulacion de los VisualEffect (en Play todos; fuera de
//     Play los seleccionados con "Vista previa"), los sprites y tilemaps 2D
//     (animaciones, fisica 2D en Play y lo que dibuja el renderizador) y la
//     grabacion/reproduccion de repeticiones.
//   - Editor del VFX Graph: los cuatro contextos (Spawn, Initialize, Update,
//     Output) como pilas de bloques, igual que el VFX Graph de Unity. Cada
//     campo se puede atar a un parametro expuesto. Los cambios se ven al
//     momento en la escena (sin guardar) sobre el objeto seleccionado.
//   - Ventana Repeticiones: grabar, reproducir con linea de tiempo, velocidad,
//     camara libre, marcadores y guardar/cargar .crreplay.

#include "EditorApp.h"

#include "Dialogs.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace cramion::editor {

using core::Vec3;
using core::Vec4;

struct EffectsEditorState {
    // VFX
    bool show_vfx = false;
    bool vfx_focus = false;
    Uuid vfx_uuid{};
    std::filesystem::path vfx_path;
    vfx::VfxGraph graph;
    bool vfx_dirty = false;
    std::string vfx_error;
    // Repeticiones
    bool show_replay = false;
    std::string replay_name = "repeticion";
    replay::ReplayOptions replay_options;
};

namespace {

const char* const kContextTitles[] = {"Spawn", "Initialize", "Update", "Output"};
const char* const kContextHelp[] = {"Cuántas nacen", "Cómo nacen", "Qué les pasa cada frame", "Cómo se ven"};
const ImVec4 kContextColors[] = {ImVec4(0.30f, 0.55f, 0.30f, 1.0f), ImVec4(0.25f, 0.45f, 0.70f, 1.0f),
                                 ImVec4(0.65f, 0.45f, 0.20f, 1.0f), ImVec4(0.55f, 0.30f, 0.60f, 1.0f)};
const char* const kOrientNames[] = {"De cara a la cámara", "Estirada por la velocidad", "Horizontal", "Vertical"};
const char* const kBlendNames[] = {"Aditiva", "Alfa", "Premultiplicada"};
const char* const kParamTypeNames[] = {"Float", "Vector3", "Color", "Bool"};

bool paramFits(vfx::ParamType param, vfx::FieldType field) {
    switch (field) {
        case vfx::FieldType::Vec3: return param == vfx::ParamType::Vector3;
        case vfx::FieldType::Color: return param == vfx::ParamType::Color;
        case vfx::FieldType::Bool: return param == vfx::ParamType::Bool;
        default: return param == vfx::ParamType::Float;
    }
}

// Un campo de un bloque. true si cambio.
bool fieldEditor(vfx::Block& block, const vfx::FieldInfo& f, const vfx::VfxGraph& graph) {
    bool changed = false;
    ImGui::PushID(f.key);
    std::string& binding = block.bindings[static_cast<std::size_t>(f.slot)];
    // Boton de atar a un parametro.
    const bool bound = !binding.empty();
    if (bound) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.55f, 0.85f, 1.0f));
    if (ImGui::SmallButton(bound ? "P" : "·")) ImGui::OpenPopup("bind");
    if (bound) ImGui::PopStyleColor();
    ImGui::SetItemTooltip(bound ? "Atado al parámetro %s (clic para cambiar)" : "Atar a un parámetro expuesto%s", bound ? binding.c_str() : "");
    if (ImGui::BeginPopup("bind")) {
        if (ImGui::MenuItem("(ninguno: valor fijo)", nullptr, binding.empty())) {
            binding.clear();
            changed = true;
        }
        for (const vfx::ExposedParam& p : graph.params) {
            if (!paramFits(p.type, f.type)) continue;
            if (ImGui::MenuItem(p.name.c_str(), nullptr, binding == p.name)) {
                binding = p.name;
                changed = true;
            }
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    float* v = &block.values[static_cast<std::size_t>(f.slot)];
    const float speed = f.max > f.min ? (f.max - f.min) * 0.002f : 0.01f;
    ImGui::SetNextItemWidth(-90.0f);
    if (bound) {
        ImGui::TextDisabled("= %s", binding.c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted(f.label);
    } else {
        switch (f.type) {
            case vfx::FieldType::Float:
            case vfx::FieldType::Angle:
                changed |= ImGui::DragFloat(f.label, v, speed, f.min, f.max, f.type == vfx::FieldType::Angle ? "%.1f°" : "%.3f");
                break;
            case vfx::FieldType::Int: {
                int i = static_cast<int>(std::lround(*v));
                if (ImGui::DragInt(f.label, &i, 1.0f, static_cast<int>(f.min), static_cast<int>(f.max))) {
                    *v = static_cast<float>(i);
                    changed = true;
                }
                break;
            }
            case vfx::FieldType::Bool: {
                bool b = *v > 0.5f;
                if (ImGui::Checkbox(f.label, &b)) {
                    *v = b ? 1.0f : 0.0f;
                    changed = true;
                }
                break;
            }
            case vfx::FieldType::Vec3: changed |= ImGui::DragFloat3(f.label, v, 0.01f); break;
            case vfx::FieldType::Color:
                changed |= ImGui::ColorEdit4(f.label, v, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_AlphaBar);
                break;
        }
    }
    if (f.tooltip != nullptr) ImGui::SetItemTooltip("%s", f.tooltip);
    ImGui::PopID();
    return changed;
}

bool gradientEditor(std::vector<vfx::GradientKey>& keys) {
    bool changed = false;
    // Vista previa de la rampa.
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = std::max(ImGui::GetContentRegionAvail().x, 40.0f);
    const float h = 14.0f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (int i = 0; i < 48; ++i) {
        const float t0 = static_cast<float>(i) / 48.0f;
        const float t1 = static_cast<float>(i + 1) / 48.0f;
        const Vec4 c = vfx::sampleGradient(keys, (t0 + t1) * 0.5f);
        // Sobre cuadros (el alfa se ve).
        draw->AddRectFilled(ImVec2(p.x + w * t0, p.y), ImVec2(p.x + w * t1, p.y + h),
                            (i / 2) % 2 == 0 ? IM_COL32(90, 90, 90, 255) : IM_COL32(60, 60, 60, 255));
        draw->AddRectFilled(ImVec2(p.x + w * t0, p.y), ImVec2(p.x + w * t1, p.y + h),
                            ImGui::ColorConvertFloat4ToU32(ImVec4(std::min(c.x, 1.0f), std::min(c.y, 1.0f), std::min(c.z, 1.0f), c.w)));
    }
    ImGui::Dummy(ImVec2(w, h + 2.0f));
    int remove = -1;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(70.0f);
        changed |= ImGui::SliderFloat("##t", &keys[i].time, 0.0f, 1.0f, "%.2f");
        ImGui::SameLine();
        changed |= ImGui::ColorEdit4("##c", &keys[i].color.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_HDR);
        ImGui::SameLine();
        if (keys.size() > 1 && ImGui::SmallButton("x")) remove = static_cast<int>(i);
        ImGui::PopID();
    }
    if (remove >= 0) {
        keys.erase(keys.begin() + remove);
        changed = true;
    }
    if (ImGui::SmallButton("+ Clave")) {
        keys.push_back(vfx::GradientKey{0.5f, vfx::sampleGradient(keys, 0.5f)});
        changed = true;
    }
    if (changed) std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    return changed;
}

bool curveEditor(std::vector<vfx::CurveKey>& keys) {
    bool changed = false;
    float values[48];
    for (int i = 0; i < 48; ++i) values[i] = vfx::sampleCurve(keys, static_cast<float>(i) / 47.0f);
    ImGui::PlotLines("##curve", values, 48, 0, nullptr, 0.0f, 3.0f, ImVec2(ImGui::GetContentRegionAvail().x, 40.0f));
    int remove = -1;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(70.0f);
        changed |= ImGui::SliderFloat("##t", &keys[i].time, 0.0f, 1.0f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        changed |= ImGui::DragFloat("##v", &keys[i].value, 0.01f, 0.0f, 100.0f, "x%.2f");
        ImGui::SameLine();
        if (keys.size() > 1 && ImGui::SmallButton("x")) remove = static_cast<int>(i);
        ImGui::PopID();
    }
    if (remove >= 0) {
        keys.erase(keys.begin() + remove);
        changed = true;
    }
    if (ImGui::SmallButton("+ Clave")) {
        keys.push_back(vfx::CurveKey{0.5f, vfx::sampleCurve(keys, 0.5f)});
        changed = true;
    }
    if (changed) std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    return changed;
}

}  // namespace

EffectsEditorState& EditorApp::effectsEditor() {
    if (!effects_editor_) effects_editor_ = std::make_shared<EffectsEditorState>();
    return *effects_editor_;
}

// --- Enganches ---------------------------------------------------------------------

void EditorApp::setupEffects() {
    vfx::setActiveSystem(&vfx_);
    replay::setActiveSystem(&replay_);
    vfx_.clear();
    vfx_.setAssetsRoot(project_.assetsFolder());
    vfx_.setGraphResolver([this](const Uuid& uuid) -> std::filesystem::path {
        if (!database_) return {};
        const auto info = database_->find(uuid);
        return info ? info->path : std::filesystem::path{};
    });
    vfx_.setModelProvider([this](const Uuid& uuid) -> std::shared_ptr<const assets::ModelAsset> {
        return asset_manager_ ? asset_manager_->loadModel(uuid) : nullptr;
    });
    vfx_.reloadGraphs();
    twod_.stop();
    twod_.setAssetsRoot(project_.assetsFolder());
    twod::loadSortingLayers(project_.settingsFolder() / "SortingLayers.json");
    replay_.clear();
    replay_.setFolder(project_.libraryFolder() / "Replays");
    replay_.setAudio(&audio_);
    replay_.setInput(input_);
    // Los one-shots (Audio.playOneShot) entran en la grabacion.
    audio::setOneShotListener([this](const std::string& clip, const core::Vec3& position, float volume, bool spatial) {
        if (replay_.recording()) replay_.notifyOneShot(clip, position, volume, spatial);
    });
}

void EditorApp::effectsEnterPlay() {
    vfx_.clear();
    twod_.start(world_);
    replay_.setInput(input_);
}

void EditorApp::effectsExitPlay() {
    if (replay_.playing()) replay_.stop(world_);
    if (replay_.recording()) replay_.stopRecording();
    twod_.stop();
    vfx_.clear();
}

void EditorApp::updateEffects(float delta_seconds) {
    if (!has_project_) return;
    // VFX Graph
    std::vector<entt::entity> preview;
    if (!playing()) {
        for (const ecs::Entity e : selectedEntities()) {
            if (e.has<vfx::VisualEffect>()) preview.push_back(e.handle());
        }
    }
    const float vfx_dt = play_state_ == PlayState::Paused ? 0.0f : delta_seconds;
    vfx_.update(world_, vfx_dt, playing(), preview, renderer_);

    // 2D
    const twod::System2D::Mode mode = play_state_ == PlayState::Playing  ? twod::System2D::Mode::Play
                                      : play_state_ == PlayState::Paused ? twod::System2D::Mode::Paused
                                                                         : twod::System2D::Mode::Edit;
    twod_.update(world_, delta_seconds, mode);
    renderer_.setSprites(twod_.drawList(world_, scene_.camera().position(), scene_.camera().forward()));

    // Repeticiones (despues de los scripts)
    if (play_state_ == PlayState::Playing) replay_.update(world_, delta_seconds);
}

// --- Assets VFX -------------------------------------------------------------------------

Uuid EditorApp::createVfxAsset(const std::filesystem::path& folder, int preset) {
    vfx::VfxGraph graph = vfx::vfxPreset(preset);
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    std::string stem = vfx::vfxPresetName(preset);
    std::filesystem::path path = folder / dialogs::fromUtf8(stem + vfx::kVfxExtension);
    for (int i = 2; std::filesystem::exists(path); ++i) {
        path = folder / dialogs::fromUtf8(stem + " " + std::to_string(i) + vfx::kVfxExtension);
    }
    std::string error;
    if (!vfx::saveVfxGraph(graph, path, &error)) {
        std::cerr << "[Editor] No se pudo crear el efecto: " << error << "\n";
        return {};
    }
    refreshDatabase();
    vfx_.reloadGraphs();
    std::cout << "[Editor] Efecto visual creado: " << dialogs::utf8(path.filename()) << "\n";
    openVfxEditor(graph.uuid);
    return graph.uuid;
}

void EditorApp::openVfxEditor(const Uuid& uuid) {
    EffectsEditorState& st = effectsEditor();
    const auto info = database_ ? database_->find(uuid) : std::nullopt;
    if (!info) return;
    vfx::VfxGraph graph;
    std::string error;
    if (!vfx::loadVfxGraph(info->path, graph, &error)) {
        std::cerr << "[Editor] No se pudo abrir el efecto: " << error << "\n";
        return;
    }
    if (!graph.uuid.valid()) graph.uuid = uuid;
    if (st.vfx_uuid.valid() && st.vfx_uuid != uuid) vfx_.clearGraphOverride(st.vfx_uuid);
    st.graph = std::move(graph);
    st.vfx_uuid = uuid;
    st.vfx_path = info->path;
    st.vfx_dirty = false;
    st.show_vfx = true;
    st.vfx_focus = true;
}

void EditorApp::drawEffectsCreateMenu(const std::filesystem::path& folder) {
    if (ImGui::BeginMenu("Efecto visual (VFX Graph)")) {
        for (int i = 0; i < vfx::vfxPresetCount(); ++i) {
            if (ImGui::MenuItem(vfx::vfxPresetName(i))) createVfxAsset(folder, i);
        }
        ImGui::EndMenu();
    }
}

// --- Editor del VFX Graph ---------------------------------------------------------

void EditorApp::drawVfxEditor() {
    EffectsEditorState& st = effectsEditor();
    if (!st.show_vfx) {
        if (st.vfx_uuid.valid() && st.vfx_dirty) vfx_.clearGraphOverride(st.vfx_uuid);
        return;
    }
    if (st.vfx_focus) {
        ImGui::SetNextWindowFocus();
        st.vfx_focus = false;
    }
    ImGui::SetNextWindowSize(ImVec2(1280.0f, 720.0f), ImGuiCond_FirstUseEver);
    const auto info = database_ ? database_->find(st.vfx_uuid) : std::nullopt;
    const std::string title = "VFX Graph: " + (info ? info->name : std::string("?")) + (st.vfx_dirty ? " *" : "") + "###vfx_editor";
    if (!ImGui::Begin(title.c_str(), &st.show_vfx)) {
        ImGui::End();
        return;
    }
    vfx::VfxGraph& g = st.graph;
    bool changed = false;
    const auto save = [&] {
        std::string error;
        if (!vfx::saveVfxGraph(g, st.vfx_path, &error)) {
            st.vfx_error = error;
            return;
        }
        st.vfx_error.clear();
        st.vfx_dirty = false;
        vfx_.clearGraphOverride(st.vfx_uuid);
        vfx_.reloadGraphs();
    };

    // --- Barra ---
    if (ImGui::Button("Guardar")) save();
    ImGui::SameLine();
    if (ImGui::Button("Crear objeto con este efecto")) {
        ecs::Entity e = world_.create(info ? info->name : std::string("Efecto"));
        const scene::Camera& camera = scene_.camera();
        e.setLocalPosition(camera.position() + camera.forward() * 5.0f);
        vfx::VisualEffect& c = e.add<vfx::VisualEffect>();
        c.graph = assets::AssetRef{st.vfx_uuid, assets::AssetType::VisualEffect};
        selection_ = {e.uuid()};
        active_ = e.uuid();
        commit();
    }
    ImGui::SetItemTooltip("Lo pone delante de la cámara y lo selecciona: la vista previa corre en la escena.");
    ImGui::SameLine();
    const vfx::VfxSystemStats stats = vfx_.stats();
    ImGui::TextDisabled("%u efectos · %u partículas vivas · %.1f MB en la GPU", stats.effects, stats.alive,
                        static_cast<double>(stats.memory_bytes) / (1024.0 * 1024.0));
    if (!st.vfx_error.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "%s", st.vfx_error.c_str());
    }

    // --- Izquierda: ajustes y parametros ---
    ImGui::BeginChild("vfx_left", ImVec2(280.0f, 0.0f), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
    ImGui::SeparatorText("Sistema");
    changed |= ImGui::DragInt("Capacidad", &g.capacity, 16.0f, 1, static_cast<int>(gfx::VfxPass::kMaxCapacity));
    ImGui::SetItemTooltip("Partículas vivas como mucho (memoria de la GPU: 64 bytes cada una)");
    changed |= ImGui::Checkbox("Espacio del mundo", &g.world_space);
    ImGui::SetItemTooltip("Las partículas no siguen al objeto al moverse (humo, estelas)");
    changed |= ImGui::DragFloat("Duración (s)", &g.duration, 0.05f, 0.05f, 3600.0f, "%.2f");
    changed |= ImGui::Checkbox("Bucle", &g.loop);
    changed |= ImGui::DragFloat("Retraso (s)", &g.start_delay, 0.05f, 0.0f, 3600.0f, "%.2f");
    changed |= ImGui::DragFloat("Precalentar (s)", &g.prewarm, 0.05f, 0.0f, 60.0f, "%.2f");
    ImGui::SetItemTooltip("Al empezar ya hay partículas (lluvia, nieve, luciérnagas)");

    ImGui::SeparatorText("Parámetros expuestos");
    ImGui::TextDisabled("Salen en el Inspector del objeto y\nlos cambian los scripts:\nentity:setEffectFloat(\"Rate\", 200)");
    int remove_param = -1;
    for (std::size_t i = 0; i < g.params.size(); ++i) {
        vfx::ExposedParam& p = g.params[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(100.0f);
        std::string name = p.name;
        if (ImGui::InputText("##name", &name, ImGuiInputTextFlags_EnterReturnsTrue) && !name.empty() && g.findParam(name) == nullptr) {
            for (std::vector<vfx::Block>* list : {&g.spawn, &g.initialize, &g.update, &g.output_blocks}) {
                for (vfx::Block& b : *list) {
                    for (std::string& bind : b.bindings) {
                        if (bind == p.name) bind = name;
                    }
                }
            }
            p.name = name;
            changed = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        int type = static_cast<int>(p.type);
        if (ImGui::Combo("##type", &type, kParamTypeNames, 4)) {
            p.type = static_cast<vfx::ParamType>(type);
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove_param = static_cast<int>(i);
        ImGui::SetNextItemWidth(-1.0f);
        switch (p.type) {
            case vfx::ParamType::Float: changed |= ImGui::DragFloat("##v", &p.value.x, 0.05f); break;
            case vfx::ParamType::Vector3: changed |= ImGui::DragFloat3("##v", &p.value.x, 0.05f); break;
            case vfx::ParamType::Color:
                changed |= ImGui::ColorEdit4("##v", &p.value.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_AlphaBar);
                break;
            case vfx::ParamType::Bool: {
                bool b = p.value.x > 0.5f;
                if (ImGui::Checkbox("##v", &b)) {
                    p.value.x = b ? 1.0f : 0.0f;
                    changed = true;
                }
                break;
            }
        }
        ImGui::PopID();
    }
    if (remove_param >= 0) {
        const std::string gone = g.params[static_cast<std::size_t>(remove_param)].name;
        g.params.erase(g.params.begin() + remove_param);
        for (std::vector<vfx::Block>* list : {&g.spawn, &g.initialize, &g.update, &g.output_blocks}) {
            for (vfx::Block& b : *list) {
                for (std::string& bind : b.bindings) {
                    if (bind == gone) bind.clear();
                }
            }
        }
        changed = true;
    }
    if (ImGui::Button("+ Parámetro")) {
        std::string name = "Param";
        for (int i = 2; g.findParam(name) != nullptr; ++i) name = "Param" + std::to_string(i);
        g.params.push_back(vfx::ExposedParam{name, vfx::ParamType::Float, Vec4{1.0f, 0.0f, 0.0f, 0.0f}, {}});
        changed = true;
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // --- Contextos ---
    ImGui::BeginChild("vfx_contexts", ImVec2(0.0f, 0.0f), 0, ImGuiWindowFlags_HorizontalScrollbar);
    const float column = std::max((ImGui::GetContentRegionAvail().x - 3.0f * ImGui::GetStyle().ItemSpacing.x) / 4.0f, 250.0f);
    for (int c = 0; c < 4; ++c) {
        if (c > 0) ImGui::SameLine();
        const vfx::Context context = static_cast<vfx::Context>(c);
        ImGui::PushID(c);
        ImGui::BeginChild("ctx", ImVec2(column, 0.0f), ImGuiChildFlags_Borders);
        ImGui::PushStyleColor(ImGuiCol_Header, kContextColors[c]);
        ImGui::Selectable(kContextTitles[c], true, ImGuiSelectableFlags_Disabled);
        ImGui::PopStyleColor();
        ImGui::TextDisabled("%s", kContextHelp[c]);
        std::vector<vfx::Block>& blocks = g.blocks(context);
        int remove = -1;
        int move_up = -1;
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            vfx::Block& b = blocks[i];
            const vfx::BlockInfo* bi = vfx::blockInfo(b.kind);
            if (bi == nullptr) continue;
            ImGui::PushID(static_cast<int>(i));
            ImGui::Separator();
            changed |= ImGui::Checkbox("##on", &b.enabled);
            ImGui::SameLine();
            const bool open = ImGui::TreeNodeEx(bi->label, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
            ImGui::SetItemTooltip("%s", bi->description);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 46.0f);
            if (i > 0 && ImGui::SmallButton("^")) move_up = static_cast<int>(i);
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) remove = static_cast<int>(i);
            if (open) {
                ImGui::BeginDisabled(!b.enabled);
                for (const vfx::FieldInfo& f : bi->fields) changed |= fieldEditor(b, f, g);
                if (bi->has_text) changed |= ImGui::InputText("Evento", &b.text);
                if (bi->has_gradient) changed |= gradientEditor(b.gradient);
                if (bi->has_curve) changed |= curveEditor(b.curve);
                if (bi->has_mesh) {
                    std::string label = "(arrastra un modelo)";
                    if (b.mesh.valid() && database_) {
                        if (const auto mi = database_->find(b.mesh.uuid)) label = mi->name;
                    }
                    ImGui::Button(label.c_str(), ImVec2(-1.0f, 0.0f));
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                            Uuid uuid{};
                            std::memcpy(&uuid, payload->Data, sizeof(Uuid));
                            const auto mi = database_->find(uuid);
                            if (mi && mi->type == assets::AssetType::Model) {
                                b.mesh = assets::AssetRef{uuid, assets::AssetType::Model};
                                changed = true;
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                }
                ImGui::EndDisabled();
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (remove >= 0) {
            blocks.erase(blocks.begin() + remove);
            changed = true;
        }
        if (move_up > 0) {
            std::swap(blocks[static_cast<std::size_t>(move_up)], blocks[static_cast<std::size_t>(move_up - 1)]);
            changed = true;
        }
        ImGui::Separator();
        if (ImGui::Button("+ Bloque", ImVec2(-1.0f, 0.0f))) ImGui::OpenPopup("add_block");
        if (ImGui::BeginPopup("add_block")) {
            for (const vfx::BlockInfo& bi : vfx::blockInfos()) {
                if (bi.context != context) continue;
                if (ImGui::MenuItem(bi.label)) {
                    blocks.push_back(vfx::makeBlock(bi.kind));
                    changed = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", bi.description);
            }
            ImGui::EndPopup();
        }
        // Salida: como se dibujan.
        if (context == vfx::Context::Output) {
            vfx::OutputSettings& o = g.output;
            ImGui::SeparatorText("Dibujo");
            int orient = static_cast<int>(o.orient);
            if (ImGui::Combo("Orientación", &orient, kOrientNames, 4)) {
                o.orient = static_cast<gfx::VfxOrient>(orient);
                changed = true;
            }
            int blend = static_cast<int>(o.blend);
            if (ImGui::Combo("Mezcla", &blend, kBlendNames, 3)) {
                o.blend = static_cast<gfx::VfxBlend>(blend);
                changed = true;
            }
            changed |= ImGui::DragFloat("Intensidad", &o.intensity, 0.05f, 0.0f, 100.0f, "%.2f");
            ImGui::SetItemTooltip("> 1 brilla con el bloom");
            changed |= ImGui::DragFloat("Suaves (m)", &o.soft_distance, 0.01f, 0.0f, 10.0f, "%.2f");
            ImGui::SetItemTooltip("Se funden al tocar la escena (0 = bordes duros)");
            if (o.orient == gfx::VfxOrient::Stretched) changed |= ImGui::DragFloat("Estela (s)", &o.stretch, 0.005f, 0.0f, 2.0f, "%.3f");
            changed |= ImGui::SliderFloat("Recorte alfa", &o.alpha_clip, 0.0f, 1.0f, "%.2f");
            changed |= ImGui::Checkbox("Iluminadas (humo, polvo)", &o.lit);
            changed |= ImGui::InputTextWithHint("Textura", "(disco suave)", &o.texture);
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kImagePayload)) {
                    const std::filesystem::path file = dialogs::fromUtf8(static_cast<const char*>(payload->Data));
                    o.texture = assetRelative(file);
                    changed = true;
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::SetItemTooltip("Arrastra una imagen del Proyecto (PNG con alfa)");
            changed |= ImGui::DragInt("Columnas", &o.flip_cols, 0.1f, 1, 64);
            changed |= ImGui::DragInt("Filas", &o.flip_rows, 0.1f, 1, 64);
            changed |= ImGui::DragFloat("Cuadros/s", &o.flip_fps, 0.1f, 0.0f, 120.0f, "%.1f");
            ImGui::SetItemTooltip("Flipbook: 0 = la animación dura la vida de la partícula");
        }
        ImGui::EndChild();
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (changed) {
        st.vfx_dirty = true;
        vfx_.setGraphOverride(st.vfx_uuid, g);  // se ve al momento en la escena
    }
    // Se guarda al soltar el control.
    if (st.vfx_dirty && !ImGui::IsAnyItemActive() && ImGui::GetIO().MouseDownDuration[0] < 0.0f) save();
    ImGui::End();
}

// --- Repeticiones -------------------------------------------------------------------

void EditorApp::drawReplayWindow() {
    EffectsEditorState& st = effectsEditor();
    if (!st.show_replay) return;
    ImGui::SetNextWindowSize(ImVec2(720.0f, 300.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Repeticiones###replay_window", &st.show_replay)) {
        ImGui::End();
        return;
    }
    replay::ReplaySystem& r = replay_;
    if (!playing()) {
        ImGui::TextWrapped("Da Play para grabar. Se graban los objetos que se mueven (o los de una etiqueta), sus "
                           "animaciones y los sonidos; luego se reproducen con la línea de tiempo, a cámara lenta o "
                           "con cámara libre. En Lua: Replay.start(), Replay.play(-5), Replay.save(\"gol\").");
    }
    ImGui::BeginDisabled(!playing());
    if (!r.recording()) {
        if (ImGui::Button("● Grabar")) r.startRecording(world_, st.replay_options);
    } else {
        if (ImGui::Button("■ Parar grabación")) r.stopRecording();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.3f, 1.0f), "Grabando %.1f s", static_cast<double>(r.duration()));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(r.empty() || r.recording());
    if (!r.playing()) {
        if (ImGui::Button("▶ Reproducir")) r.play(world_, 0.0f, r.speed());
        ImGui::SameLine();
        if (ImGui::Button("Últimos 5 s")) r.play(world_, -5.0f, r.speed());
    } else {
        if (ImGui::Button(r.paused() ? "▶ Seguir" : "❚❚ Pausa")) r.setPaused(!r.paused());
        ImGui::SameLine();
        if (ImGui::Button("■ Volver al juego")) r.stop(world_);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if (r.playing()) {
        float t = r.time();
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##time", &t, 0.0f, std::max(r.duration(), 0.01f), "%.2f s")) r.seek(t);
        // Marcadores sobre la linea de tiempo.
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        for (const replay::ReplayEvent& e : r.events()) {
            if (e.kind != replay::ReplayEvent::Kind::Marker || r.duration() <= 0.0f) continue;
            const float x = min.x + (max.x - min.x) * std::clamp(e.time / r.duration(), 0.0f, 1.0f);
            ImGui::GetWindowDrawList()->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), IM_COL32(255, 200, 60, 255), 2.0f);
        }
        float speed = r.speed();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::SliderFloat("Velocidad", &speed, 0.05f, 4.0f, "%.2fx")) r.setSpeed(speed);
        ImGui::SameLine();
        bool loop = r.loop();
        if (ImGui::Checkbox("Bucle", &loop)) r.setLoop(loop);
        ImGui::SameLine();
        bool free_camera = r.freeCamera();
        if (ImGui::Checkbox("Cámara libre", &free_camera)) r.setFreeCamera(free_camera);
        ImGui::SetItemTooltip("WASD/QE y botón derecho del ratón (Shift = rápido) en la vista Juego");
    }

    ImGui::SeparatorText("Opciones de grabación");
    ImGui::BeginDisabled(r.recording());
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat("Muestras/s", &st.replay_options.rate, 0.5f, 5.0f, 120.0f, "%.0f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat("Últimos (s)", &st.replay_options.max_seconds, 1.0f, 0.0f, 3600.0f, "%.0f");
    ImGui::SetItemTooltip("0 = todo; > 0 = solo los últimos N segundos (killcam, repetición instantánea)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputTextWithHint("Etiqueta", "(los que se mueven)", &st.replay_options.tag);
    ImGui::Checkbox("Sonidos", &st.replay_options.record_audio);
    ImGui::SameLine();
    ImGui::Checkbox("Animaciones", &st.replay_options.record_animation);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Archivo");
    const replay::ReplayInfo ri = r.info();
    ImGui::TextDisabled("%.1f s · %d fotogramas · %d objetos · %d eventos · %.1f KB (sin comprimir %.1f KB)",
                        static_cast<double>(ri.duration), ri.frames, ri.tracks, ri.events, static_cast<double>(ri.bytes) / 1024.0,
                        static_cast<double>(ri.raw_bytes) / 1024.0);
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputText("##name", &st.replay_name);
    ImGui::SameLine();
    ImGui::BeginDisabled(r.empty() || st.replay_name.empty());
    if (ImGui::Button("Guardar")) {
        std::string error;
        if (r.save(r.fileFor(st.replay_name), &error)) pushToast("Repetición guardada", st.replay_name, 1);
        else pushToast("No se pudo guardar", error, 3);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::BeginCombo("##load", "Cargar...")) {
        for (const std::string& name : r.list()) {
            if (ImGui::Selectable(name.c_str())) {
                std::string error;
                if (!r.load(r.fileFor(name), &error)) pushToast("No se pudo cargar", error, 3);
                else st.replay_name = name;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::End();
}

void EditorApp::drawEffectsWindows() {
    if (!has_project_) return;
    drawVfxEditor();
    drawReplayWindow();
}

void EditorApp::drawEffectsWindowMenu() {
    EffectsEditorState& st = effectsEditor();
    ImGui::MenuItem("Repeticiones", nullptr, &st.show_replay);
    if (ImGui::MenuItem("VFX Graph (último efecto)", nullptr, st.show_vfx, st.vfx_uuid.valid())) st.show_vfx = !st.show_vfx;
}

}  // namespace cramion::editor
