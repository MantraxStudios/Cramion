// Fisica en el editor (Jolt a traves de physics::PhysicsSystem):
//
//   - Play / Pausa / Paso (barra de menus): Play guarda el mundo, la fisica
//     simula y al parar se restaura, como Unity. Fuera de Play el mundo
//     fisico sigue reflejando la escena (sin simular): los raycast del
//     probador y la vista previa de particulas funcionan igual.
//   - Ventana Fisica: capas con nombre y matriz de colisiones, ajustes
//     (gravedad, paso fijo...), registro de eventos (Collision / Trigger
//     Enter-Stay-Exit y ParticleCollision), probador de raycast y
//     estadisticas.
//   - Gizmos 3D (con profundidad): colliders (verde; triggers en azul),
//     asas para editar cajas, esferas y capsulas, emisores de particulas,
//     contactos, velocidades y consultas (rayos, esferas, cajas).

#include "EditorApp.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>

namespace cramion::editor {

using core::Mat4;
using core::Quat;
using core::Vec3;
using physics::PhysicsEventType;

namespace {

constexpr ImU32 kColliderColor = IM_COL32(145, 244, 139, 255);  // el verde de Unity
constexpr ImU32 kTriggerColor = IM_COL32(110, 190, 255, 255);
constexpr ImU32 kSleepingColor = IM_COL32(95, 175, 95, 255);  // dormido: verde mas oscuro
constexpr ImU32 kEmitterColor = IM_COL32(150, 210, 255, 255);
constexpr ImU32 kContactColor = IM_COL32(255, 90, 60, 255);
constexpr ImU32 kVelocityColor = IM_COL32(255, 220, 60, 255);
constexpr ImU32 kRayMiss = IM_COL32(230, 80, 80, 255);
constexpr ImU32 kRayHit = IM_COL32(90, 235, 110, 255);
constexpr ImU32 kHitPoint = IM_COL32(255, 235, 90, 255);

// Colores de cada tipo de evento (registro).
constexpr ImVec4 kEventColors[physics::kPhysicsEventTypeCount] = {
    ImVec4(1.0f, 0.55f, 0.35f, 1.0f), ImVec4(0.85f, 0.65f, 0.5f, 1.0f), ImVec4(0.75f, 0.45f, 0.35f, 1.0f),
    ImVec4(0.45f, 0.75f, 1.0f, 1.0f), ImVec4(0.55f, 0.7f, 0.85f, 1.0f), ImVec4(0.35f, 0.55f, 0.85f, 1.0f),
    ImVec4(1.0f, 0.85f, 0.35f, 1.0f),
};

ImU32 fade(ImU32 color, float alpha) {
    const auto a = static_cast<std::uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * static_cast<float>((color >> 24) & 0xFF));
    return (color & 0x00FFFFFFu) | (a << 24);
}

Vec3 column(const Mat4& m, int c) {
    return Vec3{m.m[c][0], m.m[c][1], m.m[c][2]};
}

Vec3 safeNormalize(const Vec3& v, const Vec3& fallback) {
    const float l = core::length(v);
    return l > 1e-8f ? v * (1.0f / l) : fallback;
}

// Punto de la recta A (a + s*da) mas cercano a la recta B (b + t*db): devuelve s.
float closestOnLine(const Vec3& a, const Vec3& da, const Vec3& b, const Vec3& db) {
    const Vec3 w = a - b;
    const float aa = core::dot(da, da);
    const float ab = core::dot(da, db);
    const float bb = core::dot(db, db);
    const float d = core::dot(da, w);
    const float e = core::dot(db, w);
    const float denom = aa * bb - ab * ab;
    if (std::abs(denom) < 1e-8f) return 0.0f;
    return (ab * e - bb * d) / denom;
}

// Ejes (unitarios) y escala en el mundo de una matriz.
struct Frame {
    Vec3 origin{};
    Vec3 axes[3] = {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}};
    Vec3 scale{1.0f, 1.0f, 1.0f};
};

Frame frameOf(const Mat4& m) {
    Frame f;
    f.origin = column(m, 3);
    for (int i = 0; i < 3; ++i) {
        const Vec3 c = column(m, i);
        const float l = core::length(c);
        (&f.scale.x)[i] = l;
        if (l > 1e-8f) f.axes[i] = c * (1.0f / l);
    }
    return f;
}

const char* queryName(int query) {
    static constexpr const char* kNames[] = {"Raycast", "RaycastAll", "SphereCast", "OverlapSphere", "OverlapBox"};
    return query >= 0 && query < 5 ? kNames[query] : "?";
}

}  // namespace

// -----------------------------------------------------------------------------
// Ajustes del proyecto
// -----------------------------------------------------------------------------

void EditorApp::loadPhysicsSettings() {
    // Tags del proyecto (ProjectSettings/Tags.json; se crea la primera vez).
    std::vector<std::string> tags = ecs::defaultTags();
    const std::filesystem::path tags_file = project_.settingsFolder() / "Tags.json";
    if (!ecs::loadTags(tags_file, tags)) ecs::saveTags(tags_file, tags);
    ecs::setProjectTags(tags);

    physics_settings_ = physics::PhysicsSettings{};
    const std::filesystem::path file = project_.settingsFolder() / "Physics.json";
    if (!physics::loadPhysicsSettings(file, physics_settings_)) {
        savePhysicsSettings();  // primera vez: se crea con los valores por defecto
    }
    applyPhysicsSettings();
}

void EditorApp::savePhysicsSettings() {
    if (!has_project_) return;
    const std::filesystem::path file = project_.settingsFolder() / "Physics.json";
    if (!physics::savePhysicsSettings(file, physics_settings_)) {
        std::cerr << "[Fisica] No se pudo guardar " << file.string() << "\n";
    }
    physics_settings_dirty_ = false;
}

void EditorApp::applyPhysicsSettings() {
    physics::setProjectPhysicsSettings(physics_settings_);
    physics_.setSettings(physics_settings_);
}

// -----------------------------------------------------------------------------
// Play / Pausa / Paso
// -----------------------------------------------------------------------------

void EditorApp::enterPlay() {
    if (!has_project_ || playing()) return;
    if (collider_handle_drag_ != 0 || light_handle_drag_ != 0) commit();
    flushCommit();
    collider_handle_drag_ = 0;
    play_snapshot_ = ecs::serializeWorld(world_);
    play_dirty_before_ = dirty_;
    // Mundo fisico nuevo: velocidades iniciales, sin contactos viejos.
    physics_.stop();
    particles_.clear();
    physics_.start(world_);
    cinematics_.reset();
    cinematics_.clearPreview();
    timeline_playing_ = false;
    // Como Unity: al dar Play se ve el Juego.
    if (show_game_) {
        focus_game_ = true;
        preferred_view_ = kGameSlot;
    }
    play_state_ = PlayState::Playing;
    play_time_ = 0.0f;
    step_requests_ = 0;
    event_log_.clear();
    event_counts_.fill(0);
    std::cout << "[Fisica] Play" << std::endl;
}

void EditorApp::exitPlay() {
    if (!playing()) return;
    const std::uint64_t steps = physics_.stats().steps;
    physics_.stop();
    particles_.clear();
    renderer_.setParticles({});
    play_state_ = PlayState::Edit;
    collider_handle_drag_ = 0;
    // El mundo vuelve a como estaba al darle a Play (la seleccion va por UUID).
    ecs::deserializeWorld(world_, play_snapshot_);
    play_snapshot_.clear();
    dirty_ = play_dirty_before_;
    physics_.start(world_);
    cinematics_.reset();
    updateTitle();
    std::cout << "[Fisica] Stop (" << steps << " pasos, "
              << event_counts_[static_cast<int>(PhysicsEventType::CollisionEnter)] << " colisiones, "
              << event_counts_[static_cast<int>(PhysicsEventType::TriggerEnter)] << " triggers)" << std::endl;
}

void EditorApp::togglePause() {
    if (play_state_ == PlayState::Playing) {
        play_state_ = PlayState::Paused;
    } else if (play_state_ == PlayState::Paused) {
        play_state_ = PlayState::Playing;
    } else {
        enterPlay();
        play_state_ = PlayState::Paused;
    }
}

void EditorApp::updatePhysics(float delta_seconds) {
    if (!physics_.running()) return;
    console_events_this_frame_ = 0;
    console_events_skipped_ = 0;
    physics_.setRecordQueries(gizmo_queries_);
    const float fixed = physics_settings_.fixed_step;
    switch (play_state_) {
        case PlayState::Playing:
            play_time_ += delta_seconds;
            physics_.update(world_, delta_seconds, true);
            particles_.update(world_, delta_seconds, &physics_);
            break;
        case PlayState::Paused:
            if (step_requests_ > 0) {
                --step_requests_;
                play_time_ += fixed;
                physics_.singleStep(world_);
                particles_.update(world_, fixed, &physics_);
            } else {
                physics_.update(world_, 0.0f, false);
            }
            break;
        case PlayState::Edit: {
            physics_.update(world_, delta_seconds, false);
            // Vista previa de los emisores seleccionados.
            std::vector<entt::entity> preview;
            for (const ecs::Entity e : selectedEntities()) {
                const physics::ParticleSystem* ps = e.tryGet<physics::ParticleSystem>();
                if (ps != nullptr && ps->preview_in_editor) preview.push_back(e.handle());
            }
            if (preview.empty()) {
                particles_.clear();
            } else {
                particles_.update(world_, delta_seconds, &physics_, preview);
            }
            break;
        }
    }
    renderer_.setParticles(particles_.drawList(scene_.camera().position()));
    if (console_events_skipped_ > 0) {
        std::cout << "[Fisica] ... y " << console_events_skipped_ << " eventos mas en este frame (ventana Fisica > Eventos)\n";
    }
}

void EditorApp::onPhysicsEvent(const physics::PhysicsEvent& event) {
    if (!playing()) return;  // la vista previa de particulas no cuenta
    const int type = static_cast<int>(event.type);
    ++event_counts_[static_cast<std::size_t>(type)];
    if (event_log_paused_ || !event_filter_[static_cast<std::size_t>(type)]) return;

    LoggedEvent logged;
    logged.type = event.type;
    logged.a = event.a.valid() ? event.a.name() : std::string("(borrado)");
    logged.b = event.b.valid() ? event.b.name() : std::string("(borrado)");
    logged.a_uuid = event.a.valid() ? event.a.uuid() : Uuid{};
    logged.b_uuid = event.b.valid() ? event.b.uuid() : Uuid{};
    logged.point = event.point;
    logged.step = event.step;
    logged.time = play_time_;
    event_log_.push_back(std::move(logged));
    while (event_log_.size() > 2000) event_log_.pop_front();

    // A la Consola: los Enter/Exit (los Stay y las particulas saturarian).
    const bool noisy = event.type == PhysicsEventType::CollisionStay || event.type == PhysicsEventType::TriggerStay ||
                       event.type == PhysicsEventType::ParticleCollision;
    // Con un tope por frame: cientos de objetos chocando a la vez no deben
    // llenar la Consola (y frenar el frame) linea a linea.
    if (event_log_console_ && !noisy) {
        if (console_events_this_frame_ < kConsoleEventsPerFrame) {
            const LoggedEvent& e = event_log_.back();
            std::cout << "[Fisica] " << physics::eventTypeName(event.type) << ": " << e.a << " / " << e.b << '\n';
        } else {
            ++console_events_skipped_;
        }
        ++console_events_this_frame_;
    }
}

void EditorApp::drawPlayControls() {
    if (!has_project_) return;
    const float button = 64.0f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - (button * 3.0f + spacing * 2.0f)) * 0.5f);
    const auto toggle = [&](const char* label, bool active, ImVec4 color) {
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, color);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x * 1.15f, color.y * 1.15f, color.z * 1.15f, 1.0f));
        }
        const bool pressed = ImGui::Button(label, ImVec2(button, 0.0f));
        if (active) ImGui::PopStyleColor(2);
        return pressed;
    };
    if (toggle(playing() ? "Stop" : "Play", playing(), ImVec4(0.2f, 0.45f, 0.8f, 1.0f))) {
        playing() ? exitPlay() : enterPlay();
    }
    ImGui::SetItemTooltip(playing() ? "Parar y restaurar la escena (Ctrl+P)" : "Simular la fisica (Ctrl+P)");
    if (toggle("Pausa", play_state_ == PlayState::Paused, ImVec4(0.75f, 0.5f, 0.15f, 1.0f))) togglePause();
    ImGui::SetItemTooltip("Pausar / seguir (Ctrl+Mayus+P)");
    ImGui::BeginDisabled(play_state_ != PlayState::Paused);
    if (ImGui::Button("Paso", ImVec2(button, 0.0f))) ++step_requests_;
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Un paso fijo de fisica (en pausa)");
}

// -----------------------------------------------------------------------------
// Capas
// -----------------------------------------------------------------------------

bool EditorApp::layerCombo(const char* label, int& layer) {
    const std::string preview = std::to_string(layer) + ": " + physics_settings_.layerLabel(layer);
    bool changed = false;
    if (ImGui::BeginCombo(label, ("Capa  " + preview).c_str(), ImGuiComboFlags_HeightLarge)) {
        for (int i = 0; i < physics::kLayerCount; ++i) {
            if (physics_settings_.layer_names[static_cast<std::size_t>(i)].empty() && i != layer) continue;
            const std::string item = std::to_string(i) + ": " + physics_settings_.layerLabel(i);
            if (ImGui::Selectable(item.c_str(), i == layer)) {
                layer = i;
                changed = true;
            }
        }
        ImGui::Separator();
        ImGui::TextDisabled("Nombres y matriz: ventana Física > Capas");
        ImGui::EndCombo();
    }
    return changed;
}

bool EditorApp::layerMaskCombo(const char* label, std::uint32_t& mask) {
    std::string preview = mask == 0 ? "Nada" : (mask == physics::kAllLayers ? "Todo" : "");
    if (preview.empty()) {
        int count = 0;
        for (int i = 0; i < physics::kLayerCount; ++i) {
            if ((mask & physics::layerBit(i)) == 0) continue;
            if (++count <= 3) preview += (preview.empty() ? "" : ", ") + physics_settings_.layerLabel(i);
        }
        if (count > 3) preview = "Mixta (" + std::to_string(count) + " capas)";
    }
    bool changed = false;
    if (ImGui::BeginCombo(label, preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable("Nada", mask == 0)) {
            mask = 0;
            changed = true;
        }
        if (ImGui::Selectable("Todo", mask == physics::kAllLayers)) {
            mask = physics::kAllLayers;
            changed = true;
        }
        ImGui::Separator();
        for (int i = 0; i < physics::kLayerCount; ++i) {
            if (physics_settings_.layer_names[static_cast<std::size_t>(i)].empty() && (mask & physics::layerBit(i)) == 0) {
                continue;
            }
            bool on = (mask & physics::layerBit(i)) != 0;
            const std::string item = std::to_string(i) + ": " + physics_settings_.layerLabel(i);
            if (ImGui::Checkbox(item.c_str(), &on)) {
                mask = on ? (mask | physics::layerBit(i)) : (mask & ~physics::layerBit(i));
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

// -----------------------------------------------------------------------------
// Ventana Fisica
// -----------------------------------------------------------------------------

void EditorApp::drawPhysicsWindow() {
    // La primera vez, junto a la Consola (si no, flotaria encima de la Escena).
    if (console_dock_id_ != 0) ImGui::SetNextWindowDockID(console_dock_id_, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Física", &show_physics_)) {
        ImGui::End();
        return;
    }
    bool settings_changed = false;
    bool settings_finished = false;
    const auto track = [&](bool changed) {
        settings_changed |= changed;
        settings_finished |= ImGui::IsItemDeactivatedAfterEdit();
    };

    if (ImGui::BeginTabBar("physics_tabs")) {
        // --- Eventos ---
        if (ImGui::BeginTabItem("Eventos")) {
            for (int i = 0; i < physics::kPhysicsEventTypeCount; ++i) {
                if (i > 0) ImGui::SameLine();
                bool on = event_filter_[static_cast<std::size_t>(i)];
                ImGui::PushStyleColor(ImGuiCol_Text, kEventColors[i]);
                char label[64];
                std::snprintf(label, sizeof(label), "%s %llu##f%d",
                              physics::eventTypeName(static_cast<PhysicsEventType>(i)),
                              static_cast<unsigned long long>(event_counts_[static_cast<std::size_t>(i)]), i);
                if (ImGui::Checkbox(label, &on)) event_filter_[static_cast<std::size_t>(i)] = on;
                ImGui::PopStyleColor();
            }
            ImGui::SetItemTooltip("Marca los tipos que se registran en la lista (el contador cuenta todos)");
            if (ImGui::Button("Limpiar")) {
                event_log_.clear();
                event_counts_.fill(0);
            }
            ImGui::SameLine();
            ImGui::Checkbox("Pausar registro", &event_log_paused_);
            ImGui::SameLine();
            ImGui::Checkbox("Enter/Exit en la Consola", &event_log_console_);
            if (!playing()) {
                ImGui::SameLine();
                ImGui::TextDisabled("(los eventos llegan en Play)");
            }
            if (ImGui::BeginTable("events", 6,
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Tiempo", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGui::TableSetupColumn("Paso", ImGuiTableColumnFlags_WidthFixed, 50.0f);
                ImGui::TableSetupColumn("Evento", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                ImGui::TableSetupColumn("A (trigger / emisor)");
                ImGui::TableSetupColumn("B (el otro)");
                ImGui::TableSetupColumn("Punto", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                ImGui::TableHeadersRow();
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(event_log_.size()));
                while (clipper.Step()) {
                    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                        // Lo mas reciente arriba.
                        const LoggedEvent& e = event_log_[event_log_.size() - 1 - static_cast<std::size_t>(row)];
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("%.2f s", e.time);
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%llu", static_cast<unsigned long long>(e.step));
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextColored(kEventColors[static_cast<int>(e.type)], "%s", physics::eventTypeName(e.type));
                        ImGui::TableSetColumnIndex(3);
                        ImGui::PushID(row);
                        if (ImGui::Selectable(e.a.c_str(), isSelected(e.a_uuid)) && e.a_uuid.valid()) {
                            selectOnly(e.a_uuid);
                            revealInHierarchy(e.a_uuid);
                        }
                        ImGui::TableSetColumnIndex(4);
                        if (ImGui::Selectable((e.b + "##b").c_str(), isSelected(e.b_uuid)) && e.b_uuid.valid()) {
                            selectOnly(e.b_uuid);
                            revealInHierarchy(e.b_uuid);
                        }
                        ImGui::PopID();
                        ImGui::TableSetColumnIndex(5);
                        ImGui::TextDisabled("%.2f %.2f %.2f", e.point.x, e.point.y, e.point.z);
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // --- Raycast ---
        if (ImGui::BeginTabItem("Raycast")) {
            drawRaycastTester();
            ImGui::EndTabItem();
        }

        // --- Tags ---
        if (ImGui::BeginTabItem("Tags")) {
            ImGui::TextDisabled("El tag de cada objeto se elige en la cabecera del Inspector (junto a la capa).");
            static std::string new_tag;
            ImGui::SetNextItemWidth(200.0f);
            const bool enter = ImGui::InputTextWithHint("##new_tag", "Tag nuevo...", &new_tag,
                                                        ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if ((ImGui::Button("Añadir") || enter) && ecs::addProjectTag(new_tag)) {
                ecs::saveTags(project_.settingsFolder() / "Tags.json", ecs::projectTags());
                new_tag.clear();
            }
            std::string remove;
            for (const std::string& tag : ecs::projectTags()) {
                ImGui::PushID(tag.c_str());
                const bool builtin = ecs::isBuiltinTag(tag);
                ImGui::BeginDisabled(builtin);
                if (ImGui::SmallButton("x")) remove = tag;
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextUnformatted(tag.c_str());
                if (builtin) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(integrado)");
                }
                ImGui::PopID();
            }
            if (!remove.empty()) {
                std::vector<std::string> tags = ecs::projectTags();
                tags.erase(std::remove(tags.begin(), tags.end(), remove), tags.end());
                ecs::setProjectTags(tags);
                ecs::saveTags(project_.settingsFolder() / "Tags.json", ecs::projectTags());
            }
            ImGui::EndTabItem();
        }

        // --- Capas ---
        if (ImGui::BeginTabItem("Capas")) {
            ImGui::TextDisabled("La capa de cada objeto se elige en la cabecera del Inspector.");
            if (ImGui::BeginTable("layers", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                                  ImVec2(0.0f, 220.0f))) {
                ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30.0f);
                ImGui::TableSetupColumn("Nombre");
                for (int i = 0; i < physics::kLayerCount; ++i) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%d", i);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::PushID(i);
                    ImGui::SetNextItemWidth(-1.0f);
                    const bool builtin = i == physics::kLayerDefault || i == physics::kLayerTransparentFX ||
                                         i == physics::kLayerIgnoreRaycast || i == physics::kLayerWater ||
                                         i == physics::kLayerUI;
                    ImGui::BeginDisabled(builtin);
                    track(ImGui::InputTextWithHint("##name", "(sin usar)",
                                                   &physics_settings_.layer_names[static_cast<std::size_t>(i)]));
                    ImGui::EndDisabled();
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }

            // Matriz de colisiones (triangular, como la de Unity) de las capas con nombre.
            ImGui::SeparatorText("Matriz de colisiones");
            std::vector<int> named;
            for (int i = 0; i < physics::kLayerCount; ++i) {
                if (!physics_settings_.layer_names[static_cast<std::size_t>(i)].empty()) named.push_back(i);
            }
            const int n = static_cast<int>(named.size());
            if (ImGui::BeginTable("matrix", n + 1,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX)) {
                ImGui::TableSetupColumn("");
                for (int c = n - 1; c >= 0; --c) {
                    ImGui::TableSetupColumn(std::to_string(named[static_cast<std::size_t>(c)]).c_str());
                }
                ImGui::TableHeadersRow();
                for (int r = 0; r < n; ++r) {
                    const int a = named[static_cast<std::size_t>(r)];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%d: %s", a, physics_settings_.layerLabel(a).c_str());
                    for (int c = n - 1; c >= r; --c) {
                        const int b = named[static_cast<std::size_t>(c)];
                        ImGui::TableSetColumnIndex(n - c);
                        ImGui::PushID(a * 64 + b);
                        bool on = physics_settings_.layersCollide(a, b);
                        if (ImGui::Checkbox("##c", &on)) {
                            physics_settings_.setLayersCollide(a, b, on);
                            settings_changed = true;
                            settings_finished = true;
                        }
                        ImGui::SetItemTooltip("%s <-> %s", physics_settings_.layerLabel(a).c_str(),
                                              physics_settings_.layerLabel(b).c_str());
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // --- Ajustes ---
        if (ImGui::BeginTabItem("Ajustes")) {
            track(ImGui::DragFloat3("Gravedad", &physics_settings_.gravity.x, 0.05f, -100.0f, 100.0f, "%.2f"));
            int rate = static_cast<int>(std::lround(1.0f / physics_settings_.fixed_step));
            if (ImGui::SliderInt("Pasos por segundo", &rate, 10, 240)) {
                physics_settings_.fixed_step = 1.0f / static_cast<float>(std::max(rate, 10));
                settings_changed = true;
            }
            settings_finished |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SetItemTooltip("Frecuencia fija de la simulacion (Unity: Fixed Timestep)");
            track(ImGui::SliderInt("Pasos maximos por frame", &physics_settings_.max_substeps, 1, 16));
            track(ImGui::SliderInt("Subpasos de colision", &physics_settings_.collision_steps, 1, 8));
            ImGui::SetItemTooltip("Mas = mas estable con objetos rapidos o pilas altas (mas CPU)");
            track(ImGui::Checkbox("Los raycast tocan triggers", &physics_settings_.queries_hit_triggers));
            track(ImGui::DragFloat("Umbral para dormir", &physics_settings_.sleep_threshold, 0.005f, 0.0f, 2.0f,
                                   "%.3f m/s"));
            ImGui::SeparatorText("Gizmos");
            ImGui::Checkbox("Todos los colliders (no solo la selección)", &gizmo_all_colliders_);
            ImGui::Checkbox("Puntos de contacto (Play)", &gizmo_contacts_);
            ImGui::Checkbox("Velocidades (Play)", &gizmo_velocity_);
            ImGui::Checkbox("Consultas (rayos, esferas, cajas)", &gizmo_queries_);
            ImGui::EndTabItem();
        }

        // --- Estadisticas ---
        if (ImGui::BeginTabItem("Estadísticas")) {
            const physics::PhysicsStats stats = physics_.stats();
            ImGui::Text("Estado: %s", play_state_ == PlayState::Playing ? "Play"
                                      : play_state_ == PlayState::Paused ? "Pausa"
                                                                          : "Edicion (sin simular)");
            ImGui::Text("Tiempo de juego: %.2f s", play_time_);
            ImGui::Text("Cuerpos: %u (activos %u)", stats.bodies, stats.active_bodies);
            ImGui::Text("Contactos: %u parejas, %u en triggers", stats.contact_pairs, stats.trigger_pairs);
            ImGui::Text("Pasos simulados: %llu", static_cast<unsigned long long>(stats.steps));
            ImGui::Text("CPU de la fisica: %.2f ms", stats.step_milliseconds);
            ImGui::Text("Partículas: %zu (choques %llu)", particles_.particleCount(),
                        static_cast<unsigned long long>(particles_.collisionCount()));
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (settings_changed) {
        applyPhysicsSettings();
        physics_settings_dirty_ = true;
    }
    if (settings_finished && physics_settings_dirty_) {
        savePhysicsSettings();
    }
    ImGui::End();
}

// -----------------------------------------------------------------------------
// Probador de raycast
// -----------------------------------------------------------------------------

void EditorApp::drawRaycastTester() {
    ImGui::Checkbox("Mostrar y relanzar cada frame", &raycast_.enabled);
    ImGui::SameLine();
    if (ImGui::Button("Lanzar")) {
        raycast_.enabled = true;
        runRaycastTester();
    }
    ImGui::Combo("Consulta", &raycast_.query, "Raycast\0RaycastAll\0SphereCast\0OverlapSphere\0OverlapBox\0");
    ImGui::Combo("Origen", &raycast_.origin,
                 "Cámara (centro de la vista)\0Selección (hacia delante)\0Manual\0Ratón (Alt + clic en la escena)\0");
    if (raycast_.origin == 2) {
        ImGui::DragFloat3("Desde", &raycast_.manual_origin.x, 0.05f);
        ImGui::DragFloat3("Dirección", &raycast_.manual_direction.x, 0.01f, -1.0f, 1.0f);
    }
    ImGui::Checkbox("Alt + clic en la escena lanza desde el ratón", &raycast_.click_from_mouse);
    ImGui::DragFloat("Distancia", &raycast_.distance, 0.5f, 0.1f, 10000.0f, "%.1f m");
    if (raycast_.query >= 2) {
        ImGui::DragFloat(raycast_.query == 4 ? "Semilado" : "Radio", &raycast_.radius, 0.01f, 0.01f, 100.0f, "%.2f m");
    }
    layerMaskCombo("Capas (máscara)", raycast_.mask);
    ImGui::Combo("Triggers", &raycast_.triggers, "Según los ajustes\0Tocarlos\0Ignorarlos\0");

    ImGui::SeparatorText("Resultado");
    if (raycast_.query <= 2) {
        if (raycast_.hits.empty()) {
            ImGui::TextDisabled("Nada (%s de %.1f m)", queryName(raycast_.query), raycast_.distance);
        }
        for (std::size_t i = 0; i < raycast_.hits.size(); ++i) {
            const physics::RaycastHit& h = raycast_.hits[i];
            ImGui::PushID(static_cast<int>(i));
            const std::string name = h.entity.valid() ? h.entity.name() : std::string("?");
            if (ImGui::Selectable(name.c_str(), h.entity.valid() && isSelected(h.entity.uuid())) && h.entity.valid()) {
                selectOnly(h.entity.uuid());
                revealInHierarchy(h.entity.uuid());
            }
            ImGui::PopID();
            ImGui::SameLine(180.0f);
            ImGui::TextDisabled("%.2f m  capa %s%s  normal %.2f %.2f %.2f", h.distance,
                                physics_settings_.layerLabel(h.layer).c_str(), h.trigger ? "  (trigger)" : "",
                                h.normal.x, h.normal.y, h.normal.z);
        }
    } else {
        if (raycast_.overlaps.empty()) ImGui::TextDisabled("Nada solapa");
        for (std::size_t i = 0; i < raycast_.overlaps.size(); ++i) {
            const ecs::Entity e = raycast_.overlaps[i];
            if (!e.valid()) continue;
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(e.name().c_str(), isSelected(e.uuid()))) {
                selectOnly(e.uuid());
                revealInHierarchy(e.uuid());
            }
            ImGui::PopID();
        }
    }
}

void EditorApp::runRaycastTester() {
    RaycastTester& t = raycast_;
    physics::QueryFilter filter;
    filter.layer_mask = t.mask;
    filter.triggers = static_cast<physics::QueryTriggers>(t.triggers);
    Vec3 origin{};
    Vec3 direction{0.0f, 0.0f, -1.0f};
    switch (t.origin) {
        case 0:
            origin = scene_.camera().position();
            direction = scene_.camera().forward();
            break;
        case 1: {
            const ecs::Entity e = world_.find(active_);
            if (!e.valid()) return;
            origin = e.worldPosition();
            direction = e.forward();
            filter.ignore = e;  // sin tocarse a si misma
            break;
        }
        case 2:
            origin = t.manual_origin;
            direction = t.manual_direction;
            break;
        default:
            if (!t.has_mouse_ray) return;
            origin = t.mouse_origin;
            direction = t.mouse_direction;
            break;
    }
    direction = safeNormalize(direction, Vec3{0.0f, -1.0f, 0.0f});
    t.from = origin;
    t.to = origin + direction * t.distance;
    t.hits.clear();
    t.overlaps.clear();
    physics::RaycastHit hit;
    switch (t.query) {
        case 0:
            if (physics_.raycast(origin, direction, t.distance, hit, filter)) t.hits.push_back(hit);
            break;
        case 1: t.hits = physics_.raycastAll(origin, direction, t.distance, filter); break;
        case 2:
            if (physics_.sphereCast(origin, t.radius, direction, t.distance, hit, filter)) t.hits.push_back(hit);
            break;
        case 3: {
            // Donde acaba el rayo (o donde toca).
            physics::RaycastHit end;
            const Vec3 center = physics_.raycast(origin, direction, t.distance, end, filter) ? end.point : t.to;
            t.to = center;
            t.overlaps = physics_.overlapSphere(center, t.radius, filter);
            break;
        }
        default: {
            physics::RaycastHit end;
            const Vec3 center = physics_.raycast(origin, direction, t.distance, end, filter) ? end.point : t.to;
            t.to = center;
            t.overlaps = physics_.overlapBox(center, Vec3{t.radius, t.radius, t.radius}, Quat{}, filter);
            break;
        }
    }
    if (!t.hits.empty() && t.query != 1) t.to = origin + direction * t.hits.front().distance;
}

// -----------------------------------------------------------------------------
// Gizmos
// -----------------------------------------------------------------------------

void EditorApp::drawColliderGizmo(ecs::Entity e, bool selected) {
    const Mat4& m = e.worldMatrix();
    const Frame f = frameOf(m);
    const bool sleeping = playing() && physics_.hasBody(e) && e.has<physics::Rigidbody>() &&
                          e.get<physics::Rigidbody>().type == physics::BodyType::Dynamic && physics_.isSleeping(e);
    const float alpha = selected ? 1.0f : 0.7f;
    const auto colorFor = [&](const physics::ColliderMaterial& material) {
        return fade(material.is_trigger ? kTriggerColor : (sleeping ? kSleepingColor : kColliderColor), alpha);
    };
    const Mat4 view = scene_.camera().view();
    const Vec3 cam_right = core::normalize(Vec3{view.m[0][0], view.m[1][0], view.m[2][0]});
    const Vec3 cam_up = core::normalize(Vec3{view.m[0][1], view.m[1][1], view.m[2][1]});
    const auto arc = [&](const Vec3& c, const Vec3& u, const Vec3& v, float r, float a0, float a1, ImU32 color) {
        constexpr int kSegments = 24;
        Vec3 previous = c + u * (std::cos(a0) * r) + v * (std::sin(a0) * r);
        for (int i = 1; i <= kSegments; ++i) {
            const float a = a0 + (a1 - a0) * static_cast<float>(i) / kSegments;
            const Vec3 p = c + u * (std::cos(a) * r) + v * (std::sin(a) * r);
            overlayLine(previous, p, color);
            previous = p;
        }
    };

    if (const physics::BoxCollider* box = e.tryGet<physics::BoxCollider>()) {
        overlayBoxEdges(m * core::translate(box->center) * core::scale(box->size), colorFor(box->material));
    }
    if (const physics::SphereCollider* sphere = e.tryGet<physics::SphereCollider>()) {
        const ImU32 color = colorFor(sphere->material);
        const Vec3 c = ecs::transformPoint(m, sphere->center);
        const float r = sphere->radius * std::max({f.scale.x, f.scale.y, f.scale.z});
        overlayCircle(c, f.axes[0], f.axes[1], r, color);
        overlayCircle(c, f.axes[1], f.axes[2], r, color);
        overlayCircle(c, f.axes[0], f.axes[2], r, color);
        overlayCircle(c, cam_right, cam_up, r, fade(color, 0.5f));  // contorno
    }
    if (const physics::CapsuleCollider* capsule = e.tryGet<physics::CapsuleCollider>()) {
        const ImU32 color = colorFor(capsule->material);
        const int a = static_cast<int>(capsule->axis);
        const Vec3 dir = f.axes[a];
        const Vec3 u = f.axes[(a + 1) % 3];
        const Vec3 v = f.axes[(a + 2) % 3];
        const float s_axis = (&f.scale.x)[a];
        const float s_radius = std::max((&f.scale.x)[(a + 1) % 3], (&f.scale.x)[(a + 2) % 3]);
        const float r = capsule->radius * s_radius;
        const float half = std::max(capsule->height * s_axis * 0.5f - r, 0.0f);
        const Vec3 c = ecs::transformPoint(m, capsule->center);
        const Vec3 top = c + dir * half;
        const Vec3 bottom = c - dir * half;
        overlayCircle(top, u, v, r, color);
        overlayCircle(bottom, u, v, r, color);
        for (const Vec3& side : {u, u * -1.0f, v, v * -1.0f}) {
            overlayLine(top + side * r, bottom + side * r, color);
        }
        arc(top, u, dir, r, 0.0f, core::kPi, color);
        arc(top, v, dir, r, 0.0f, core::kPi, color);
        arc(bottom, u, dir * -1.0f, r, 0.0f, core::kPi, color);
        arc(bottom, v, dir * -1.0f, r, 0.0f, core::kPi, color);
    }
    if (const physics::MeshCollider* mesh = e.tryGet<physics::MeshCollider>()) {
        // La forma que usa Jolt de verdad (malla o envolvente convexa).
        std::vector<Vec3> triangles;
        if (physics_.bodyTriangles(e, triangles, selected ? 12000 : 3000)) {
            const ImU32 color = fade(colorFor(mesh->material), 0.8f);
            for (std::size_t i = 0; i + 2 < triangles.size(); i += 3) {
                overlayLine(triangles[i], triangles[i + 1], color);
                overlayLine(triangles[i + 1], triangles[i + 2], color);
                overlayLine(triangles[i + 2], triangles[i], color);
            }
        }
    }
    if (const physics::PlaneCollider* plane = e.tryGet<physics::PlaneCollider>()) {
        // Plano infinito: una rejilla de 10 x 10 m y su normal.
        const ImU32 color = colorFor(plane->material);
        const Vec3 x = f.axes[0];
        const Vec3 z = f.axes[2];
        for (int i = -5; i <= 5; ++i) {
            const float k = static_cast<float>(i);
            overlayLine(f.origin + x * k - z * 5.0f, f.origin + x * k + z * 5.0f, fade(color, i == 0 ? 1.0f : 0.5f));
            overlayLine(f.origin + z * k - x * 5.0f, f.origin + z * k + x * 5.0f, fade(color, i == 0 ? 1.0f : 0.5f));
        }
        overlayLine(f.origin, f.origin + f.axes[1] * 1.0f, color);
        overlayCone(f.origin + f.axes[1] * 1.0f, f.axes[1], 0.2f, 0.06f, color);
    }
}

void EditorApp::drawParticleEmitterGizmo(ecs::Entity e) {
    const physics::ParticleSystem* ps = e.tryGet<physics::ParticleSystem>();
    if (ps == nullptr) return;
    const Mat4& m = e.worldMatrix();
    const Frame f = frameOf(m);
    const Vec3 up = f.axes[1];
    const float radius = ps->shape_radius * std::max(f.scale.x, f.scale.z);
    switch (ps->shape) {
        case physics::EmitterShape::Cone: {
            const float length = 1.0f;
            const float top_radius = radius + length * std::tan(core::radians(std::clamp(ps->cone_angle, 0.0f, 89.0f)));
            const Vec3 top = f.origin + up * length;
            overlayCircle(f.origin, f.axes[0], f.axes[2], std::max(radius, 0.01f), kEmitterColor);
            overlayCircle(top, f.axes[0], f.axes[2], top_radius, kEmitterColor);
            for (const Vec3& side : {f.axes[0], f.axes[0] * -1.0f, f.axes[2], f.axes[2] * -1.0f}) {
                overlayLine(f.origin + side * radius, top + side * top_radius, kEmitterColor);
            }
            break;
        }
        case physics::EmitterShape::Sphere:
            overlayCircle(f.origin, f.axes[0], f.axes[1], std::max(radius, 0.01f), kEmitterColor);
            overlayCircle(f.origin, f.axes[1], f.axes[2], std::max(radius, 0.01f), kEmitterColor);
            overlayCircle(f.origin, f.axes[0], f.axes[2], std::max(radius, 0.01f), kEmitterColor);
            break;
        case physics::EmitterShape::Box:
            overlayBoxEdges(m * core::scale(ps->box_size), kEmitterColor);
            break;
        case physics::EmitterShape::Point:
            for (int i = 0; i < 3; ++i) overlayLine(f.origin - f.axes[i] * 0.15f, f.origin + f.axes[i] * 0.15f, kEmitterColor);
            break;
    }
    // Direccion de salida.
    overlayLine(f.origin, f.origin + up * 0.6f, fade(kEmitterColor, 0.6f));
}

void EditorApp::drawPhysicsGizmos() {
    if (!has_project_ || !physics_.running()) return;

    // --- Colliders ---
    std::vector<ecs::Entity> selected = selectedEntities();
    if (gizmo_all_colliders_) {
        std::vector<entt::entity> all;
        const auto gather = [&](auto view) {
            for (const entt::entity h : view) all.push_back(h);
        };
        entt::registry& registry = world_.registry();
        gather(registry.view<physics::BoxCollider>());
        gather(registry.view<physics::SphereCollider>());
        gather(registry.view<physics::CapsuleCollider>());
        gather(registry.view<physics::MeshCollider>());
        gather(registry.view<physics::PlaneCollider>());
        std::sort(all.begin(), all.end());
        all.erase(std::unique(all.begin(), all.end()), all.end());
        for (const entt::entity h : all) {
            const ecs::Entity e = world_.wrap(h);
            if (e.activeInHierarchy()) drawColliderGizmo(e, isSelected(e.uuid()));
        }
    } else {
        for (const ecs::Entity e : selected) {
            if (e.activeInHierarchy() && physics::hasCollider(e)) drawColliderGizmo(e, true);
        }
    }
    for (const ecs::Entity e : selected) {
        if (e.activeInHierarchy()) drawParticleEmitterGizmo(e);
    }

    // --- Contactos y velocidades (Play) ---
    if (playing() && gizmo_contacts_) {
        for (const physics::ContactDebug& c : physics_.contactPoints()) {
            const ImU32 color = c.trigger ? kTriggerColor : kContactColor;
            const float s = 0.04f;
            overlayLine(c.point - Vec3{s, 0, 0}, c.point + Vec3{s, 0, 0}, color);
            overlayLine(c.point - Vec3{0, s, 0}, c.point + Vec3{0, s, 0}, color);
            overlayLine(c.point - Vec3{0, 0, s}, c.point + Vec3{0, 0, s}, color);
            if (!c.trigger) overlayLine(c.point, c.point + c.normal * 0.3f, fade(color, 0.8f));
        }
    }
    if (playing() && gizmo_velocity_) {
        for (const ecs::Entity e : selected) {
            if (!physics_.hasBody(e) || !e.has<physics::Rigidbody>()) continue;
            const Vec3 v = physics_.linearVelocity(e);
            const float speed = core::length(v);
            if (speed < 0.01f) continue;
            const Vec3 from = physics_.centerOfMass(e);
            const Vec3 to = from + v * 0.25f;
            overlayLine(from, to, kVelocityColor);
            overlayCone(to, v, std::min(0.25f, speed * 0.05f + 0.05f), 0.04f, kVelocityColor);
        }
    }

    // --- Consultas (las del juego y el probador) ---
    const auto drawRay = [&](const Vec3& from, const Vec3& to, bool hit, const Vec3& point, const Vec3& normal) {
        overlayLine(from, to, hit ? kRayHit : kRayMiss);
        if (hit) {
            overlayScreenDisc(point, gizmoWorldSize(point) * 0.04f, kHitPoint);
            overlayLine(point, point + normal * 0.5f, kHitPoint);
            overlayCone(point + normal * 0.5f, normal, 0.1f, 0.035f, kHitPoint);
        }
    };
    const auto drawSphere = [&](const Vec3& c, float r, ImU32 color) {
        overlayCircle(c, Vec3{1, 0, 0}, Vec3{0, 1, 0}, r, color);
        overlayCircle(c, Vec3{0, 1, 0}, Vec3{0, 0, 1}, r, color);
        overlayCircle(c, Vec3{1, 0, 0}, Vec3{0, 0, 1}, r, color);
    };
    if (gizmo_queries_) {
        for (const physics::QueryDebug& q : physics_.recordedQueries()) {
            switch (q.kind) {
                case physics::QueryDebug::Kind::Ray: drawRay(q.origin, q.end, q.hit, q.hit_point, q.hit_normal); break;
                case physics::QueryDebug::Kind::SphereCast:
                    drawRay(q.origin, q.end, q.hit, q.hit_point, q.hit_normal);
                    drawSphere(q.end, q.extents.x, q.hit ? kRayHit : kRayMiss);
                    break;
                case physics::QueryDebug::Kind::OverlapSphere:
                    drawSphere(q.origin, q.extents.x, q.hit ? kRayHit : kRayMiss);
                    break;
                case physics::QueryDebug::Kind::OverlapBox:
                    overlayBoxEdges(core::composeTrs(q.origin, q.rotation, q.extents * 2.0f), q.hit ? kRayHit : kRayMiss);
                    break;
            }
        }
        physics_.clearRecordedQueries();
    }

    // Probador: se relanza cada frame (salvo el del raton, que va por clic).
    if (raycast_.enabled) {
        const bool record = gizmo_queries_;
        physics_.setRecordQueries(false);  // se dibuja aqui mismo
        if (raycast_.origin != 3) runRaycastTester();
        physics_.setRecordQueries(record);
        const RaycastTester& t = raycast_;
        if (t.query <= 2) {
            const bool hit = !t.hits.empty();
            drawRay(t.from, t.query == 1 ? t.from + safeNormalize(t.to - t.from, Vec3{}) * t.distance : t.to, hit,
                    hit ? t.hits.front().point : Vec3{}, hit ? t.hits.front().normal : Vec3{});
            for (std::size_t i = 1; i < t.hits.size(); ++i) {
                overlayScreenDisc(t.hits[i].point, gizmoWorldSize(t.hits[i].point) * 0.03f, kHitPoint);
            }
            if (t.query == 2) {
                drawSphere(t.from, t.radius, fade(kRayHit, 0.5f));
                drawSphere(t.to, t.radius, hit ? kRayHit : kRayMiss);
            }
        } else {
            overlayLine(t.from, t.to, fade(kRayHit, 0.5f));
            const ImU32 color = t.overlaps.empty() ? kRayMiss : kRayHit;
            if (t.query == 3) {
                drawSphere(t.to, t.radius, color);
            } else {
                overlayBoxEdges(core::translate(t.to) * core::scale(Vec3{t.radius, t.radius, t.radius} * 2.0f), color);
            }
            for (const ecs::Entity e : t.overlaps) {
                if (e.valid() && physics::hasCollider(e)) drawColliderGizmo(e, true);
            }
        }
    }
}

// Asas para editar la forma del collider seleccionado (caja: sus 6 caras;
// esfera: radio; capsula: radio y altura). true si el raton esta sobre un asa.
bool EditorApp::drawColliderHandles() {
    ecs::Entity e = world_.find(active_);
    if (!edit_collider_ || !e.valid() || flying_) {
        collider_handle_drag_ = 0;
        return false;
    }
    physics::BoxCollider* box = e.tryGet<physics::BoxCollider>();
    physics::SphereCollider* sphere = e.tryGet<physics::SphereCollider>();
    physics::CapsuleCollider* capsule = e.tryGet<physics::CapsuleCollider>();
    if (box == nullptr && sphere == nullptr && capsule == nullptr) {
        collider_handle_drag_ = 0;
        return false;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(ImVec2(view_x_, view_y_), ImVec2(view_x_ + view_w_, view_y_ + view_h_), true);
    ImGuiIO& io = ImGui::GetIO();
    bool hovered_any = false;
    const auto handle = [&](const Vec3& p, int id) {
        float x = 0.0f, y = 0.0f;
        if (!worldToScreen(p, x, y)) return;
        const bool over = std::abs(io.MousePos.x - x) < 7.0f && std::abs(io.MousePos.y - y) < 7.0f;
        const bool active = collider_handle_drag_ == id;
        hovered_any = hovered_any || over || active;
        draw->AddRectFilled(ImVec2(x - 4.5f, y - 4.5f), ImVec2(x + 4.5f, y + 4.5f),
                            active || over ? IM_COL32(255, 255, 255, 255) : IM_COL32(145, 244, 139, 255));
        draw->AddRect(ImVec2(x - 4.5f, y - 4.5f), ImVec2(x + 4.5f, y + 4.5f), IM_COL32(0, 0, 0, 200));
        if (over && view_hovered_ && collider_handle_drag_ == 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !io.KeyAlt) {
            collider_handle_drag_ = id;
        }
    };

    const Mat4& m = e.worldMatrix();
    const Frame f = frameOf(m);
    // Caja: el centro de cada cara (1..6 = +X -X +Y -Y +Z -Z).
    if (box != nullptr) {
        const Vec3 c = ecs::transformPoint(m, box->center);
        for (int id = 1; id <= 6; ++id) {
            const int axis = (id - 1) / 2;
            const float sign = (id - 1) % 2 == 0 ? 1.0f : -1.0f;
            const float half = std::abs((&box->size.x)[axis]) * 0.5f * (&f.scale.x)[axis];
            const Vec3 face = c + f.axes[axis] * (sign * half);
            if (collider_handle_drag_ == 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                collider_drag_anchor_ = c - f.axes[axis] * (sign * half);  // la cara de enfrente queda fija
            }
            handle(face, id);
        }
    } else if (sphere != nullptr) {
        const Vec3 c = ecs::transformPoint(m, sphere->center);
        const float r = sphere->radius * std::max({f.scale.x, f.scale.y, f.scale.z});
        const Mat4 view = scene_.camera().view();
        const Vec3 cam_right = core::normalize(Vec3{view.m[0][0], view.m[1][0], view.m[2][0]});
        const Vec3 cam_up = core::normalize(Vec3{view.m[0][1], view.m[1][1], view.m[2][1]});
        for (const Vec3& d : {cam_right, cam_right * -1.0f, cam_up, cam_up * -1.0f}) handle(c + d * r, 7);
    } else if (capsule != nullptr) {
        const int a = static_cast<int>(capsule->axis);
        const Vec3 c = ecs::transformPoint(m, capsule->center);
        const float s_axis = (&f.scale.x)[a];
        const float s_radius = std::max((&f.scale.x)[(a + 1) % 3], (&f.scale.x)[(a + 2) % 3]);
        const float half_total = std::max(capsule->height * 0.5f * s_axis, capsule->radius * s_radius);
        handle(c + f.axes[(a + 1) % 3] * (capsule->radius * s_radius), 7);
        handle(c - f.axes[(a + 1) % 3] * (capsule->radius * s_radius), 7);
        handle(c + f.axes[a] * half_total, 8);
        handle(c - f.axes[a] * half_total, 9);
    }

    // Arrastre.
    if (collider_handle_drag_ != 0) {
        Vec3 origin{};
        Vec3 direction{};
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && mouseRay(io.MousePos.x, io.MousePos.y, origin, direction)) {
            const int id = collider_handle_drag_;
            if (box != nullptr && id >= 1 && id <= 6) {
                const int axis = (id - 1) / 2;
                const float sign = (id - 1) % 2 == 0 ? 1.0f : -1.0f;
                const Vec3 dir = f.axes[axis] * sign;
                const float scale = std::max((&f.scale.x)[axis], 1e-5f);
                const float world_size = std::max(closestOnLine(collider_drag_anchor_, dir, origin, direction), 0.01f);
                (&box->size.x)[axis] = world_size / scale;
                // El centro nuevo (en el espacio local de la entidad).
                const Vec3 center_world = collider_drag_anchor_ + dir * (world_size * 0.5f);
                box->center = ecs::transformPoint(core::inverse(m), center_world);
            } else if (id == 7 && sphere != nullptr) {
                const Vec3 c = ecs::transformPoint(m, sphere->center);
                const float t = core::dot(c - origin, direction) / std::max(core::dot(direction, direction), 1e-8f);
                const float scale = std::max({f.scale.x, f.scale.y, f.scale.z, 1e-5f});
                sphere->radius = std::max(core::length(origin + direction * t - c) / scale, 0.005f);
            } else if (capsule != nullptr) {
                const int a = static_cast<int>(capsule->axis);
                const Vec3 c = ecs::transformPoint(m, capsule->center);
                if (id == 7) {
                    const float s_radius =
                        std::max({(&f.scale.x)[(a + 1) % 3], (&f.scale.x)[(a + 2) % 3], 1e-5f});
                    capsule->radius =
                        std::max(std::abs(closestOnLine(c, f.axes[(a + 1) % 3], origin, direction)) / s_radius, 0.005f);
                } else {
                    const float s_axis = std::max((&f.scale.x)[a], 1e-5f);
                    const float half = std::abs(closestOnLine(c, f.axes[a], origin, direction));
                    capsule->height = std::max(2.0f * half / s_axis, capsule->radius * 2.0f);
                }
            }
            dirty_ = true;
        } else {
            collider_handle_drag_ = 0;
            commit();  // un paso de deshacer por arrastre
        }
    }
    draw->PopClipRect();
    return hovered_any;
}

}  // namespace cramion::editor
