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

#include "CramionCore/ecs/MathUtil.h"

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
const char* const kBlendNames[] = {"Aditiva", "Alfa", "Premultiplicada", "Distorsión", "Opaca"};
const char* const kOutputNames[] = {"Quads", "Cinta (estela)", "Malla 3D"};
const char* const kFacingNames[] = {"De cara a la cámara", "Eje Y del emisor", "Hacia un vector", "Lado = un vector"};
const char* const kRibbonUvNames[] = {"Estirada (cabeza a cola)", "Por distancia (se repite, no resbala)",
                                      "Por edad (0 cabeza .. 1 cola)"};
const char* const kMeshOrientNames[] = {"Al azar (giran)", "Siguen la velocidad", "Fija (cuaternión + giro en Y)",
                                        "Hacia la cámara"};
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
    // Position Mesh: con las mallas que ya se dibujan (estan en la escena del
    // renderizador; en el AssetManager quedan vacias y no salia ningun punto).
    vfx_.setModelProvider([this](const Uuid& uuid) -> std::shared_ptr<const assets::ModelAsset> {
        return asset_manager_ ? fullModel(uuid) : nullptr;
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

void EditorApp::effectsAssetChanged(const Uuid& uuid) {
    vfx_.reloadAsset(uuid);
    // Abierto en el editor de efectos y sin cambios: se ve el del disco.
    EffectsEditorState& st = effectsEditor();
    if (st.vfx_uuid != uuid || st.vfx_dirty || st.vfx_path.empty()) return;
    vfx::VfxGraph graph;
    std::string error;
    if (!vfx::loadVfxGraph(st.vfx_path, graph, &error)) return;  // a medio escribir: llegara otro aviso
    if (!graph.uuid.valid()) graph.uuid = uuid;
    vfx_.clearGraphOverride(uuid);
    st.graph = std::move(graph);
}

int EditorApp::effectsTextureChanged(const std::string& relative) { return vfx_.reloadTextureFile(relative); }

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
    // La camara: distancia y vista de los efectos con tipo o caja fija.
    {
        const scene::Camera& camera = scene_.camera();
        vfx_.setCamera(camera.position(), camera.projection() * camera.view());
    }
    // Suelo bajo los emisores (bloques "Sobre el terreno" / "Suelo sin la vista").
    vfx_.setGroundQuery([this](float x, float z, float& h) { return sync_ && sync_->terrainHeight(x, z, h); });
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

void EditorApp::saveVfxEditor() {
    EffectsEditorState& st = effectsEditor();
    if (st.vfx_path.empty()) return;
    std::string error;
    if (!vfx::saveVfxGraph(st.graph, st.vfx_path, &error)) {
        st.vfx_error = error;
        return;
    }
    st.vfx_error.clear();
    st.vfx_dirty = false;
    vfx_.clearGraphOverride(st.vfx_uuid);
    vfx_.reloadGraphs();
}

EditorApp::GraphDoc EditorApp::graphDocVfx() {
    EffectsEditorState& st = effectsEditor();
    GraphDoc d;
    d.show = &st.show_vfx;
    d.focus = &st.vfx_focus;
    d.dirty = st.vfx_dirty;
    d.path = st.vfx_path;
    const auto info = database_ ? database_->find(st.vfx_uuid) : std::nullopt;
    if (info) d.name = info->name;
    return d;
}

void EditorApp::drawVfxEditor() {
    EffectsEditorState& st = effectsEditor();
    if (!st.show_vfx) {
        if (st.vfx_uuid.valid() && st.vfx_dirty) vfx_.clearGraphOverride(st.vfx_uuid);
        return;
    }
    const auto info = database_ ? database_->find(st.vfx_uuid) : std::nullopt;
    const std::string title = "VFX Graph: " + (info ? info->name : std::string("?")) + (st.vfx_dirty ? " *" : "") + "###vfx_editor";
    if (!beginGraphWorkspace(GraphKind::Vfx, title.c_str())) return;  // en su pestana de arriba
    vfx::VfxGraph& g = st.graph;
    bool changed = false;
    const auto save = [&] { saveVfxEditor(); };

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
    // Por tipo de efecto (solo los que se usan).
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("Calidad de los efectos: %d · pool: %u aparcadas · %u sueltos (playAt)", stats.quality, stats.parked,
                    stats.one_shots);
        for (int i = 0; i < vfx::kEffectTypeSlots; ++i) {
            const vfx::VfxTypeStats& t = stats.types[static_cast<std::size_t>(i)];
            if (t.instances == 0 && t.culled == 0) continue;
            ImGui::Text("%s: %u activos, %u fuera, %u partículas, %.2f ms CPU + %.2f ms GPU (est.)",
                        vfx::effectTypeLabel(static_cast<vfx::EffectType>(i)), t.instances, t.culled, t.particles,
                        static_cast<double>(t.cpu_ms), static_cast<double>(t.gpu_ms));
        }
        ImGui::EndTooltip();
    }
    if (!st.vfx_error.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "%s", st.vfx_error.c_str());
    }
    // Lo que le falta para dibujarse entero (textura o malla que no cabe o
    // no se pudo leer): antes solo salia en la consola.
    if (const std::string warning = vfx_.resourceWarning(st.vfx_uuid); !warning.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Aviso: %s", warning.c_str());
        if (ImGui::IsItemHovered()) {
            const gfx::VfxPass& pass = renderer_.vfx();
            ImGui::SetTooltip("Texturas de efectos en la GPU: %u de %u · mallas: %u de %u", pass.texturesInUse(),
                              pass.textureLimit(), pass.meshesInUse(), gfx::VfxPass::kMaxMeshes);
        }
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

    // Escalabilidad (MMO): tipo de efecto, orden de dibujo, caja fija y rafagas.
    ImGui::SeparatorText("Escalabilidad");
    {
        int type = static_cast<int>(g.effect_type);
        const char* type_names[vfx::kEffectTypeSlots] = {};
        for (int i = 0; i < vfx::kEffectTypeSlots; ++i) type_names[i] = vfx::effectTypeLabel(static_cast<vfx::EffectType>(i));
        if (ImGui::Combo("Tipo de efecto", &type, type_names, vfx::kEffectTypeSlots)) {
            g.effect_type = static_cast<vfx::EffectType>(type);
            changed = true;
        }
        ImGui::SetItemTooltip("La calidad decide cuántos de cada tipo, a qué distancia, cuántas partículas y qué salidas\n"
                              "caras se apagan (Héroe: los del jugador; Combate: los de los demás; Impacto: golpes;\n"
                              "Ambiente: antorchas, polvo...). Sin tipo: sin límites, como antes.");
        if (g.effect_type != vfx::EffectType::None) {
            const vfx::EffectTypeBudget& b = vfx_.scalability().types[static_cast<std::size_t>(g.effect_type)];
            ImGui::TextDisabled("Calidad %d: %s, %d a la vez, partículas x%.2f", vfx::qualityLevel(),
                                b.max_distance > 0.0f ? (std::to_string(static_cast<int>(b.max_distance)) + " m").c_str()
                                                      : "sin distancia",
                                b.max_instances, static_cast<double>(b.particle_scale));
        }
        changed |= ImGui::DragInt("Orden de dibujo", &g.sort_order, 0.1f, -1000, 1000);
        ImGui::SetItemTooltip("Entre efectos transparentes: mayor = encima (a igual orden, de atrás adelante)");
        changed |= ImGui::Checkbox("Caja fija", &g.fixed_bounds);
        ImGui::SetItemTooltip("Caja en el espacio del objeto: fuera de la vista no se dibuja (y con bucle se congela)");
        if (g.fixed_bounds) {
            changed |= ImGui::DragFloat3("Centro de la caja", &g.bounds_center.x, 0.05f);
            changed |= ImGui::DragFloat3("Tamaño de la caja", &g.bounds_size.x, 0.05f, 0.0f, 10000.0f);
        }
        changed |= ImGui::SliderInt("Repartir ráfagas (frames)", &g.spawn_spread_frames, 1, 30);
        ImGui::SetItemTooltip("Las ráfagas grandes nacen repartidas en estos frames (sin tirones); 1 = todas a la vez");
    }

    ImGui::SeparatorText("Parámetros expuestos");
    ImGui::TextDisabled("Salen en el Inspector del objeto y\nlos cambian los scripts:\nentity().setEffectFloat(\"Rate\", 200)");
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
                if (bi->has_text) {
                    changed |= ImGui::InputText(bi->text_label != nullptr ? bi->text_label : "Evento", &b.text);
                    // Canales de datos: el mismo nombre en quien apunta (Emit*) y en
                    // quien escucha; tambien Effects.pushEvent desde los scripts.
                    if (vfx::isChannelBlock(b.kind)) {
                        ImGui::SetItemTooltip("Canal de datos: el mismo nombre en los bloques que apuntan "
                                              "('evento al canal') y en los que escuchan ('Al recibir un evento de la "
                                              "GPU'). Los scripts escriben con Effects.pushEvent(\"%s\", tipo, posicion, "
                                              "velocidad, color, valor). Hasta %u canales y %u eventos por canal y frame.",
                                              b.text.c_str(), gfx::VfxPass::kMaxChannels, gfx::VfxPass::kMaxChannelEvents);
                        if (b.kind == vfx::BlockKind::OnGpuEvent && !g.world_space) {
                            ImGui::TextDisabled("(escuchar un canal hace que simule en el mundo)");
                        }
                    }
                }
                if (bi->has_gradient) changed |= gradientEditor(b.gradient);
                if (bi->has_curve) changed |= curveEditor(b.curve);
                if (b.kind == vfx::BlockKind::ParticleLight) {
                    // Luz por particula: el color es el de la particula y el tope
                    // global (todas las luces de particula del frame) va aparte.
                    ImGui::TextDisabled("Color: el de la partícula (Color en la vida y su alfa)");
                    ImGui::TextDisabled("Tope por frame: %u (calidad y render.vfx.MaxParticleLights)",
                                        renderer_.vfx().lightBudget());
                }
                if (bi->has_effect) {
                    // Subemisor: el .crvfx que nace donde muere o choca.
                    std::string label = "(arrastra un efecto .crvfx)";
                    if (b.effect.valid() && database_) {
                        if (const auto ei = database_->find(b.effect.uuid)) label = ei->name;
                    }
                    ImGui::Button(label.c_str(), ImVec2(-1.0f, 0.0f));
                    ImGui::SetItemTooltip("Nace donde muere o choca cada particula (en la GPU, el frame siguiente)");
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                            Uuid uuid{};
                            std::memcpy(&uuid, payload->Data, sizeof(Uuid));
                            const auto ei = database_->find(uuid);
                            if (ei && ei->type == assets::AssetType::VisualEffect && uuid != st.vfx_uuid) {
                                b.effect = assets::AssetRef{uuid, assets::AssetType::VisualEffect};
                                changed = true;
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                }
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
            int output_type = static_cast<int>(o.type);
            if (ImGui::Combo("Salida", &output_type, kOutputNames, 3)) {  // Sprite (quads) / Cinta / Malla
                o.type = static_cast<gfx::VfxOutput>(output_type);
                changed = true;
            }
            ImGui::SetItemTooltip("Quads; una cinta continua entre las vivas (estelas de espada y proyectil); o una "
                                  "malla 3D por particula (casquillos, escombros)");
            if (o.type == gfx::VfxOutput::Quads) {
                int orient = static_cast<int>(o.orient);
                if (ImGui::Combo("Orientación", &orient, kOrientNames, 4)) {
                    o.orient = static_cast<gfx::VfxOrient>(orient);
                    changed = true;
                }
            } else if (o.type == gfx::VfxOutput::Ribbon) {
                int facing = static_cast<int>(o.ribbon_facing);
                if (ImGui::Combo("Ancho hacia", &facing, kFacingNames, 4)) {
                    o.ribbon_facing = static_cast<gfx::VfxRibbonFacing>(facing);
                    changed = true;
                }
                ImGui::SetItemTooltip("Eje Y del emisor: el de cuando nacio cada punto (una espada con Y a lo largo de la hoja). "
                                      "Hacia un vector: la cinta mira hacia el (ancho = tangente x vector). Lado: el ancho va por el");
                if (o.ribbon_facing == gfx::VfxRibbonFacing::CustomFacing || o.ribbon_facing == gfx::VfxRibbonFacing::CustomSide) {
                    changed |= ImGui::DragFloat3("Vector", &o.ribbon_vector.x, 0.01f, -1.0f, 1.0f, "%.2f");
                    ImGui::SetItemTooltip("En el espacio del emisor");
                }
                int uv = static_cast<int>(o.ribbon_uv);
                if (ImGui::Combo("Textura", &uv, kRibbonUvNames, 3)) {
                    o.ribbon_uv = static_cast<gfx::VfxRibbonUv>(uv);
                    changed = true;
                }
                if (o.ribbon_uv == gfx::VfxRibbonUv::Tile) {
                    changed |= ImGui::DragFloat("Repetición (m)", &o.ribbon_tile, 0.01f, 0.01f, 100.0f, "%.2f");
                }
                changed |= ImGui::DragFloat("Corte (s)", &o.ribbon_gap, 0.005f, 0.0f, 10.0f, "%.3f");
                ImGui::SetItemTooltip("Dos puntos que nacieron mas separados no se unen (el emisor paro). 0 = nunca");
                changed |= ImGui::DragFloat2("Escala UV", &o.ribbon_uv_scale.x, 0.01f, -100.0f, 100.0f, "%.2f");
                changed |= ImGui::DragFloat2("Desplazamiento UV", &o.ribbon_uv_offset.x, 0.01f, -100.0f, 100.0f, "%.2f");
                changed |= ImGui::DragInt("Celda del atlas", &o.ribbon_atlas_cell, 0.1f, -1, 4095);
                ImGui::SetItemTooltip("Celda de la textura (Columnas x Filas de abajo); -1 = la textura entera");
                if (o.ribbon_atlas_cell >= 0) {
                    changed |= ImGui::DragInt("Columnas", &o.flip_cols, 0.1f, 1, 64);
                    changed |= ImGui::DragInt("Filas", &o.flip_rows, 0.1f, 1, 64);
                }
                changed |= ImGui::SliderInt("Subdivisiones", &o.ribbon_subdivisions, 1, 16);
                ImGui::SetItemTooltip("Tramos por segmento (curva suave entre los puntos). 1 = sin suavizar (lo mas barato)");
                if (o.ribbon_subdivisions > 1) {
                    changed |= ImGui::SliderFloat("Tensión", &o.ribbon_tension, 0.0f, 1.0f, "%.2f");
                    ImGui::SetItemTooltip("0 = Catmull-Rom (redonda), 1 = tramos rectos");
                    changed |= ImGui::Checkbox("Adaptativa (por el ángulo)", &o.ribbon_adaptive);
                    ImGui::SetItemTooltip("Solo subdivide donde la cinta gira (90 grados = todas)");
                }
                ImGui::TextDisabled("Ancho = tamaño (o Cinta: atributos); Tamaño y Color en la vida van a lo largo");
            } else {
                std::string label = "(arrastra un modelo)";
                if (o.mesh.valid() && database_) {
                    if (const auto mi = database_->find(o.mesh.uuid)) label = mi->name;
                }
                ImGui::Button(label.c_str(), ImVec2(-1.0f, 0.0f));
                ImGui::SetItemTooltip("El modelo de cada particula (todas sus piezas; hasta 65536 vertices entre todas las mallas)");
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                        Uuid uuid{};
                        std::memcpy(&uuid, payload->Data, sizeof(Uuid));
                        const auto mi = database_->find(uuid);
                        if (mi && mi->type == assets::AssetType::Model) {
                            o.mesh = assets::AssetRef{uuid, assets::AssetType::Model};
                            changed = true;
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                int mesh_orient = static_cast<int>(o.mesh_orient);
                if (ImGui::Combo("Giro", &mesh_orient, kMeshOrientNames, 4)) {
                    o.mesh_orient = static_cast<gfx::VfxMeshOrient>(mesh_orient);
                    changed = true;
                }
                ImGui::SetItemTooltip("El bloque Giro (Initialize) da el angulo y la velocidad de giro");
                changed |= ImGui::DragFloat3("Escala", &o.mesh_scale.x, 0.01f, -100.0f, 100.0f, "%.2f");
                ImGui::SetItemTooltip("Por el tamaño de la particula");
                if (o.mesh_orient == gfx::VfxMeshOrient::Fixed) {
                    // Se guarda en cuaternion; los grados solo son para editar.
                    const core::Quat q{o.mesh_rotation.x, o.mesh_rotation.y, o.mesh_rotation.z, o.mesh_rotation.w};
                    core::Vec3 euler = ecs::quatToEulerDegrees(q);
                    if (ImGui::DragFloat3("Rotación", &euler.x, 0.5f, -360.0f, 360.0f, "%.1f°")) {
                        const core::Quat r = ecs::quatFromEulerDegrees(euler);
                        o.mesh_rotation = core::Vec4{r.x, r.y, r.z, r.w};
                        changed = true;
                    }
                    ImGui::SetItemTooltip("Orientacion de todas (cuaternion); el bloque Giro gira cada una en su Y");
                }
                changed |= ImGui::SliderFloat3("Escala al azar", &o.mesh_scale_jitter.x, 0.0f, 1.0f, "%.2f");
                if (!o.material.valid()) {
                    changed |= ImGui::Checkbox("Textura de la salida en la malla", &o.mesh_output_texture);
                    ImGui::SetItemTooltip("Apagado (por defecto): color y textura base del propio modelo, con su UV. "
                                          "Encendido: la textura de arriba con la UV de la malla (una textura de "
                                          "sprite con alfa casi 0 deja la malla invisible)");
                }
                ImGui::SetItemTooltip("Variacion por eje de cada particula (0.3 = +-30%)");
                // Variantes (o niveles de LOD): hasta 3 modelos mas.
                ImGui::SeparatorText("Variantes y LOD");
                int remove_variant = -1;
                for (std::size_t vi = 0; vi < o.mesh_variants.size(); ++vi) {
                    ImGui::PushID(static_cast<int>(vi) + 9000);
                    std::string vlabel = std::to_string(vi + 1) + ": (arrastra un modelo; vacia = la principal)";
                    if (o.mesh_variants[vi].valid() && database_) {
                        if (const auto mi = database_->find(o.mesh_variants[vi].uuid)) vlabel = std::to_string(vi + 1) + ": " + mi->name;
                    }
                    ImGui::Button(vlabel.c_str(), ImVec2(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x, 0.0f));
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                            Uuid uuid{};
                            std::memcpy(&uuid, payload->Data, sizeof(Uuid));
                            const auto mi = database_->find(uuid);
                            if (mi && mi->type == assets::AssetType::Model) {
                                o.mesh_variants[vi] = assets::AssetRef{uuid, assets::AssetType::Model};
                                changed = true;
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                    if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0.0f))) remove_variant = static_cast<int>(vi);
                    ImGui::PopID();
                }
                if (remove_variant >= 0) {
                    o.mesh_variants.erase(o.mesh_variants.begin() + remove_variant);
                    changed = true;
                }
                if (o.mesh_variants.size() + 1 < gfx::kVfxMeshVariants && ImGui::Button("+ Variante", ImVec2(-1.0f, 0.0f))) {
                    o.mesh_variants.push_back(assets::AssetRef{{}, assets::AssetType::Model});
                    changed = true;
                }
                ImGui::SetItemTooltip("Otro modelo: cada particula usa el de su bloque Índice de malla (Initialize)");
                changed |= ImGui::DragFloat("LOD cada (m)", &o.mesh_lod_distance, 0.5f, 0.0f, 10000.0f, "%.1f");
                ImGui::SetItemTooltip("> 0: las variantes son niveles de detalle (la 1 a partir de esta distancia, la 2 al "
                                      "doble...). 0 = por particula (Índice de malla)");
                changed |= ImGui::DragFloat("Sprite desde (m)", &o.mesh_sprite_distance, 0.5f, 0.0f, 100000.0f, "%.1f");
                ImGui::SetItemTooltip("Mas lejos, un quad con la textura (o el disco suave) en lugar de la malla. 0 = nunca");
                // Material (.crmat): manda sobre la mezcla, "Iluminadas" y la textura.
                {
                    std::string mlabel = "(arrastra un material .crmat)";
                    if (o.material.valid() && database_) {
                        if (const auto mi = database_->find(o.material.uuid)) mlabel = "Material: " + mi->name;
                    }
                    ImGui::Button(mlabel.c_str(), ImVec2(o.material.valid() ? -ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x
                                                                            : -1.0f,
                                                         0.0f));
                    ImGui::SetItemTooltip("Color base, textura base, emision, opaco o transparente y sin luz del material. "
                                          "Sin sombras (las particulas no las proyectan)");
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                            Uuid uuid{};
                            std::memcpy(&uuid, payload->Data, sizeof(Uuid));
                            const auto mi = database_->find(uuid);
                            if (mi && mi->type == assets::AssetType::Material) {
                                o.material = assets::AssetRef{uuid, assets::AssetType::Material};
                                changed = true;
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    if (o.material.valid()) {
                        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                        if (ImGui::Button("x##material", ImVec2(ImGui::GetFrameHeight(), 0.0f))) {
                            o.material = assets::AssetRef{{}, assets::AssetType::Material};
                            changed = true;
                        }
                    }
                }
            }
            int blend = static_cast<int>(o.blend);
            if (ImGui::Combo("Mezcla", &blend, kBlendNames, 5)) {
                o.blend = static_cast<gfx::VfxBlend>(blend);
                changed = true;
            }
            ImGui::SetItemTooltip("Distorsión: no pinta, desplaza la imagen de detrás (calor, ondas de choque; la "
                                  "textura es un normal map). Opaca: sin mezcla (mallas)");
            if (o.blend == gfx::VfxBlend::Distortion) {
                changed |= ImGui::SliderFloat("Distorsión (px)", &o.distortion, 0.0f, gfx::kVfxMaxDistortionPixels, "%.1f");
                ImGui::SetItemTooltip("Desplazamiento maximo en pixeles a 1080p (por el alfa de la particula). Se apaga "
                                      "en calidad Baja y con render.vfx.Distortion = false");
                changed |= ImGui::Checkbox("Textura = mapa de alturas", &o.distortion_height);
                ImGui::SetItemTooltip("Gris: desplaza por la pendiente. Si no, la textura es un normal map (RG); sin "
                                      "textura, una lente blanda");
            }
            changed |= ImGui::DragFloat("Intensidad", &o.intensity, 0.05f, 0.0f, 100.0f, "%.2f");
            ImGui::SetItemTooltip("> 1 brilla con el bloom");
            changed |= ImGui::DragFloat("Suaves (m)", &o.soft_distance, 0.01f, 0.0f, 10.0f, "%.2f");
            ImGui::SetItemTooltip("Se funden al tocar la escena (0 = bordes duros)");
            if (o.type == gfx::VfxOutput::Quads && o.orient == gfx::VfxOrient::Stretched) {
                changed |= ImGui::DragFloat("Estela (s)", &o.stretch, 0.005f, 0.0f, 2.0f, "%.3f");
            }
            changed |= ImGui::SliderFloat("Recorte alfa", &o.alpha_clip, 0.0f, 1.0f, "%.2f");
            changed |= ImGui::Checkbox("Iluminadas (humo, polvo)", &o.lit);
            changed |= assetFilePicker("vfx_tex", project_.assetsFolder(), kImageFileExts, "imagen", o.texture);
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x));
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
            if (o.type == gfx::VfxOutput::Quads) {
                changed |= ImGui::DragInt("Columnas", &o.flip_cols, 0.1f, 1, 64);
                changed |= ImGui::DragInt("Filas", &o.flip_rows, 0.1f, 1, 64);
                changed |= ImGui::DragFloat("Cuadros/s", &o.flip_fps, 0.1f, 0.0f, 120.0f, "%.1f");
                ImGui::SetItemTooltip("Flipbook: 0 = la animación dura la vida de la partícula");
                changed |= ImGui::Checkbox("Fundir cuadros", &o.flip_blend);
                ImGui::SetItemTooltip("Mezcla cada cuadro con el siguiente (animación suave con pocos cuadros). "
                                      "Cuadro inicial al azar y variación de velocidad: bloque Cuadro del flipbook");
            }
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
                           "con cámara libre. Desde un script: Replay::start(), Replay::play(-5), Replay::save(\"gol\").");
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
