// Navegacion en el editor, como en Unreal:
//
//   - La malla se genera sola y en tiempo real mientras se edita (mover un
//     collider rehace solo sus baldosas, en hilos de fondo).
//   - Se ve en la vista de escena en verde, con su borde (tecla P o el boton
//     "Nav" de la barra), las zonas de Evitar en naranja, los volumenes y los
//     modificadores como cajas y, en Play, el camino de cada agente.
//   - Ventana Navegacion: los ajustes del agente y de la malla
//     (ProjectSettings/Navigation.json), estadisticas y "Reconstruir".
//   - Crear > Navegacion: volumen de NavMesh (ajustado a la escena) y
//     modificador.

#include "EditorApp.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace cramion::editor {

using core::Mat4;
using core::Vec3;

namespace {

constexpr ImU32 kNavFill = IM_COL32(46, 204, 90, 96);
constexpr ImU32 kNavAvoidFill = IM_COL32(240, 150, 40, 110);
constexpr ImU32 kNavEdge = IM_COL32(24, 120, 52, 235);
constexpr ImU32 kVolumeColor = IM_COL32(255, 170, 40, 255);
constexpr ImU32 kBlockColor = IM_COL32(235, 70, 70, 255);
constexpr ImU32 kAvoidColor = IM_COL32(240, 150, 40, 255);
constexpr ImU32 kPathColor = IM_COL32(255, 225, 60, 255);
constexpr ImU32 kAgentColor = IM_COL32(90, 200, 255, 255);

ImU32 withAlpha(ImU32 color, int alpha) {
    return (color & 0x00FFFFFFu) | (static_cast<ImU32>(std::clamp(alpha, 0, 255)) << 24);
}

// Matriz de una caja de `size` centrada en la entidad, alineada con el mundo
// (el volumen de NavMesh no gira).
Mat4 axisAlignedBox(const ecs::Entity& e, const Vec3& size) {
    const Mat4& w = e.worldMatrix();
    Vec3 lo{1e9f, 1e9f, 1e9f};
    Vec3 hi{-1e9f, -1e9f, -1e9f};
    for (int i = 0; i < 8; ++i) {
        const core::Vec4 p = w * core::Vec4{((i & 1) ? 0.5f : -0.5f) * std::abs(size.x),
                                            ((i & 2) ? 0.5f : -0.5f) * std::abs(size.y),
                                            ((i & 4) ? 0.5f : -0.5f) * std::abs(size.z), 1.0f};
        lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    return core::translate((lo + hi) * 0.5f) * core::scale(hi - lo);
}

}  // namespace

// -----------------------------------------------------------------------------
// Ajustes y ciclo
// -----------------------------------------------------------------------------

void EditorApp::loadNavigationSettings() {
    nav_settings_ = navigation::NavigationSettings{};
    const std::filesystem::path file = project_.settingsFolder() / "Navigation.json";
    if (!navigation::loadNavigationSettings(file, nav_settings_)) {
        saveNavigationSettings();  // primera vez: con los valores por defecto
    }
    nav_.setSettings(nav_settings_);
}

void EditorApp::saveNavigationSettings() {
    if (!has_project_ && project_.folder.empty()) return;
    const std::filesystem::path file = project_.settingsFolder() / "Navigation.json";
    if (!navigation::saveNavigationSettings(file, nav_settings_)) {
        std::cerr << "[Navegacion] No se pudo guardar " << dialogs::utf8(file) << "\n";
    }
}

void EditorApp::updateNavigation(float delta_seconds) {
    if (!has_project_) return;
    switch (play_state_) {
        case PlayState::Edit:
            nav_.update(world_, delta_seconds, false, true);
            break;
        case PlayState::Playing:
            nav_.update(world_, delta_seconds, true, nav_settings_.runtime_generation);
            break;
        case PlayState::Paused:
            nav_.update(world_, step_requests_ > 0 ? physics_settings_.fixed_step : 0.0f, step_requests_ > 0,
                        nav_settings_.runtime_generation);
            break;
    }
}

// -----------------------------------------------------------------------------
// Vista de escena
// -----------------------------------------------------------------------------

void EditorApp::drawNavigationGizmos() {
    if (!has_project_) return;
    ImGuiIO& io = ImGui::GetIO();
    // P: mostrar/ocultar la navegacion (la tecla de Unreal).
    if ((view_hovered_ || view_focused_) && !io.WantTextInput && !flying_ && !io.KeyCtrl && !io.KeyAlt &&
        ImGui::IsKeyPressed(ImGuiKey_P, false)) {
        show_navigation_ = !show_navigation_;
    }

    // --- Volumenes y modificadores (siempre, como las brochas de Unreal) ---
    for (const entt::entity h : world_.registry().view<navigation::NavMeshBounds>()) {
        const ecs::Entity e = world_.wrap(h);
        if (!e.activeInHierarchy()) continue;
        const bool selected = isSelected(e.uuid());
        if (!selected && !show_navigation_) continue;
        overlayBoxEdges(axisAlignedBox(e, e.get<navigation::NavMeshBounds>().size),
                        selected ? kVolumeColor : withAlpha(kVolumeColor, 150));
    }
    for (const entt::entity h : world_.registry().view<navigation::NavModifier>()) {
        const ecs::Entity e = world_.wrap(h);
        if (!e.activeInHierarchy()) continue;
        const bool selected = isSelected(e.uuid());
        if (!selected && !show_navigation_) continue;
        const navigation::NavModifier& m = e.get<navigation::NavModifier>();
        const ImU32 color = m.area == navigation::NavArea::Block ? kBlockColor : kAvoidColor;
        overlayBoxEdges(e.worldMatrix() * core::scale(Vec3{std::abs(m.size.x), std::abs(m.size.y), std::abs(m.size.z)}),
                        selected ? color : withAlpha(color, 150));
    }
    if (!show_navigation_) return;

    // --- La malla (cache: solo se rehace cuando cambia) ---
    const navigation::NavDebugMesh& mesh = nav_.debugMesh();
    // La malla vive en su propio espacio (origen flotante): se lleva al del mundo.
    const Vec3 nav_to_local = nav_.navToLocal();
    const bool moved = nav_to_local.x != nav_draw_offset_.x || nav_to_local.y != nav_draw_offset_.y ||
                       nav_to_local.z != nav_draw_offset_.z;
    if (mesh.version != nav_draw_version_ || moved) {
        nav_draw_version_ = mesh.version;
        nav_draw_offset_ = nav_to_local;
        nav_draw_triangles_.clear();
        nav_draw_edges_.clear();
        nav_draw_triangles_.reserve(mesh.triangles.size());
        for (std::size_t i = 0; i < mesh.triangles.size(); ++i) {
            const std::size_t triangle = i / 3;
            const bool avoid = triangle < mesh.triangle_area.size() && mesh.triangle_area[triangle] == 1;
            nav_draw_triangles_.push_back({mesh.triangles[i] + nav_to_local, avoid ? kNavAvoidFill : kNavFill});
        }
        nav_draw_edges_.reserve(mesh.edges.size());
        for (const Vec3& p : mesh.edges) nav_draw_edges_.push_back({p + nav_to_local, kNavEdge});
    }
    overlay_.triangles.insert(overlay_.triangles.end(), nav_draw_triangles_.begin(), nav_draw_triangles_.end());
    overlay_.lines.insert(overlay_.lines.end(), nav_draw_edges_.begin(), nav_draw_edges_.end());

    // --- Agentes: su radio y, en Play, el camino que siguen ---
    for (const entt::entity h : world_.registry().view<navigation::NavAgent>()) {
        const ecs::Entity e = world_.wrap(h);
        if (!e.activeInHierarchy()) continue;
        const navigation::NavAgent& a = e.get<navigation::NavAgent>();
        const Vec3 feet = e.worldPosition() - Vec3{0.0f, a.base_offset, 0.0f} + Vec3{0.0f, 0.05f, 0.0f};
        overlayCircle(feet, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, std::max(a.radius, 0.05f), kAgentColor,
                      false, 24);
        if (!playing()) continue;
        const std::vector<Vec3> path = nav_.agentPath(e);
        for (std::size_t i = 1; i < path.size(); ++i) {
            overlayLine(path[i - 1] + Vec3{0.0f, 0.1f, 0.0f}, path[i] + Vec3{0.0f, 0.1f, 0.0f}, kPathColor);
        }
        if (!path.empty()) overlayScreenDisc(path.back() + Vec3{0.0f, 0.1f, 0.0f}, 0.12f, kPathColor);
    }
}

// -----------------------------------------------------------------------------
// Crear
// -----------------------------------------------------------------------------

// Caja de toda la geometria estatica de la escena (lo que puede tener malla).
bool EditorApp::navigationSceneBounds(Vec3& lo, Vec3& hi) const {
    lo = Vec3{1e9f, 1e9f, 1e9f};
    hi = Vec3{-1e9f, -1e9f, -1e9f};
    bool any = false;
    const auto grow = [&](const ecs::Entity& e, const Vec3& half, const Vec3& center) {
        const Mat4& w = e.worldMatrix();
        for (int i = 0; i < 8; ++i) {
            const core::Vec4 p = w * core::Vec4{center.x + ((i & 1) ? half.x : -half.x),
                                                center.y + ((i & 2) ? half.y : -half.y),
                                                center.z + ((i & 4) ? half.z : -half.z), 1.0f};
            lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
        any = true;
    };
    const entt::registry& registry = world_.registry();
    for (const entt::entity h : registry.view<physics::BoxCollider>()) {
        const ecs::Entity e = world_.wrap(h);
        if (!e.activeInHierarchy() || e.has<navigation::NavAgent>()) continue;
        const auto& c = e.get<physics::BoxCollider>();
        if (c.material.is_trigger) continue;
        grow(e, c.size * 0.5f, c.center);
    }
    for (const entt::entity h : registry.view<physics::MeshCollider>()) {
        const ecs::Entity e = world_.wrap(h);
        if (!e.activeInHierarchy() || e.has<navigation::NavAgent>() || !sync_) continue;
        if (const asset::ModelData* data = sync_->actorModelData(e, scene_)) {
            Vec3 mlo{1e9f, 1e9f, 1e9f};
            Vec3 mhi{-1e9f, -1e9f, -1e9f};
            for (const auto& v : data->vertices) {
                mlo = Vec3{std::min(mlo.x, v.position.x), std::min(mlo.y, v.position.y), std::min(mlo.z, v.position.z)};
                mhi = Vec3{std::max(mhi.x, v.position.x), std::max(mhi.y, v.position.y), std::max(mhi.z, v.position.z)};
            }
            if (!data->vertices.empty()) grow(e, (mhi - mlo) * 0.5f, (mhi + mlo) * 0.5f);
        }
    }
    for (const entt::entity h : registry.view<terrain::Terrain>()) {
        const ecs::Entity e = world_.wrap(h);
        if (!e.activeInHierarchy()) continue;
        const auto& t = e.get<terrain::Terrain>();
        grow(e, Vec3{t.size, t.height, t.size} * 0.5f, Vec3{t.size, t.height, t.size} * 0.5f);
    }
    return any && lo.x <= hi.x;
}

void EditorApp::fitNavBoundsToScene(ecs::Entity volume) {
    navigation::NavMeshBounds* bounds = volume.tryGet<navigation::NavMeshBounds>();
    if (bounds == nullptr) return;
    Vec3 lo{};
    Vec3 hi{};
    if (!navigationSceneBounds(lo, hi)) return;
    // Un poco de margen arriba (para la altura del agente) y abajo.
    lo = lo - Vec3{0.5f, 1.0f, 0.5f};
    hi = hi + Vec3{0.5f, nav_settings_.agent_height + 1.0f, 0.5f};
    volume.setLocalScale(Vec3{1.0f, 1.0f, 1.0f});
    volume.setLocalEulerDegrees(Vec3{});
    volume.setWorldPosition((lo + hi) * 0.5f);
    bounds->size = hi - lo;
}

ecs::Entity EditorApp::createNavigationEntity(int kind) {
    ecs::Entity entity;
    if (kind == 0) {
        entity = world_.create("NavMeshBoundsVolume");
        entity.add<navigation::NavMeshBounds>();
        Vec3 lo{};
        Vec3 hi{};
        if (navigationSceneBounds(lo, hi)) {
            fitNavBoundsToScene(entity);
        } else {
            const Vec3 forward = scene_.camera().forward();
            Vec3 flat{forward.x, 0.0f, forward.z};
            flat = core::length(flat) > 1e-3f ? core::normalize(flat) : Vec3{0.0f, 0.0f, -1.0f};
            const Vec3 at = scene_.camera().position() + flat * 20.0f;
            entity.setWorldPosition(Vec3{at.x, 2.0f, at.z});
        }
    } else {
        entity = world_.create("NavModifier");
        entity.add<navigation::NavModifier>();
        const Vec3 forward = scene_.camera().forward();
        Vec3 flat{forward.x, 0.0f, forward.z};
        flat = core::length(flat) > 1e-3f ? core::normalize(flat) : Vec3{0.0f, 0.0f, -1.0f};
        const Vec3 at = scene_.camera().position() + flat * 8.0f;
        float ground = 0.0f;
        groundHeightAt(at.x, at.z, ground);
        entity.setWorldPosition(Vec3{at.x, ground + 2.0f, at.z});
    }
    show_navigation_ = true;
    selectOnly(entity.uuid());
    revealInHierarchy(entity.uuid());
    commit();
    return entity;
}

void EditorApp::drawNavigationCreateMenu() {
    if (ImGui::BeginMenu("Navegación")) {
        if (ImGui::MenuItem("Volumen de NavMesh")) createNavigationEntity(0);
        ImGui::SetItemTooltip("Dentro se genera la malla de navegación (se ajusta a la escena)");
        if (ImGui::MenuItem("Modificador de navegación")) createNavigationEntity(1);
        ImGui::SetItemTooltip("Caja que bloquea o encarece la navegación de su interior");
        ImGui::EndMenu();
    }
}

// -----------------------------------------------------------------------------
// Inspector y ventana
// -----------------------------------------------------------------------------

void EditorApp::drawNavigationStats() {
    const navigation::NavStats stats = nav_.stats();
    if (stats.pending_tiles > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Generando... %d baldosas pendientes", stats.pending_tiles);
    } else if (stats.tiles == 0) {
        ImGui::TextDisabled("Sin malla: hace falta un volumen de NavMesh con colliders dentro");
    } else {
        ImGui::TextDisabled("%d baldosas · %d polígonos · %.0f KB · %.1f ms/baldosa", stats.tiles, stats.polygons,
                            static_cast<double>(stats.memory_bytes) / 1024.0, static_cast<double>(stats.last_tile_ms));
    }
    if (stats.agents > 0) ImGui::TextDisabled("%d agentes", stats.agents);
}

void EditorApp::drawNavMeshBoundsInspector(ecs::Entity entity) {
    ImGui::Spacing();
    drawNavigationStats();
    const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Ajustar a la escena", ImVec2(w, 0.0f))) {
        fitNavBoundsToScene(entity);
        commit();
    }
    ImGui::SetItemTooltip("Cubre toda la geometría con colisión de la escena");
    ImGui::SameLine();
    if (ImGui::Button("Reconstruir", ImVec2(w, 0.0f))) nav_.rebuildAll();
    if (ImGui::Checkbox("Mostrar navegación (P)", &show_navigation_)) {}
    ImGui::SameLine();
    if (ImGui::SmallButton("Ajustes...")) show_navigation_window_ = true;
}

void EditorApp::drawNavigationWindow() {
    if (!ImGui::Begin("Navegación", &show_navigation_window_)) {
        ImGui::End();
        return;
    }
    if (!has_project_) {
        ImGui::TextDisabled("Abre un proyecto");
        ImGui::End();
        return;
    }
    drawNavigationStats();
    ImGui::Checkbox("Mostrar en la escena (P)", &show_navigation_);
    ImGui::SameLine();
    if (ImGui::Button("Reconstruir todo")) nav_.rebuildAll();

    bool changed = false;   // se aplica al soltar (no en cada paso del arrastre)
    bool finished = false;
    const auto track = [&](bool edit) {
        changed |= edit;
        finished |= ImGui::IsItemDeactivatedAfterEdit();
    };
    navigation::NavigationSettings& s = nav_settings_;
    ImGui::SeparatorText("Agente");
    track(ImGui::DragFloat("Radio", &s.agent_radius, 0.01f, 0.0f, 5.0f, "%.2f m"));
    ImGui::SetItemTooltip("Distancia mínima a las paredes");
    track(ImGui::DragFloat("Altura", &s.agent_height, 0.01f, 0.2f, 10.0f, "%.2f m"));
    track(ImGui::DragFloat("Escalón máximo", &s.max_step_height, 0.01f, 0.0f, 3.0f, "%.2f m"));
    track(ImGui::DragFloat("Pendiente máxima", &s.max_slope, 0.5f, 0.0f, 89.0f, "%.0f°"));

    ImGui::SeparatorText("Malla");
    track(ImGui::DragFloat("Tamaño de celda", &s.cell_size, 0.005f, 0.05f, 2.0f, "%.3f m"));
    ImGui::SetItemTooltip("Menos = más detalle (y más lento de generar)");
    track(ImGui::DragFloat("Altura de celda", &s.cell_height, 0.005f, 0.02f, 1.0f, "%.3f m"));
    track(ImGui::SliderInt("Celdas por baldosa", &s.tile_size, 16, 256));
    ImGui::SetItemTooltip("Baldosa de %.1f m: al mover algo se rehacen las baldosas que toca",
                          static_cast<double>(s.tileWorldSize()));
    track(ImGui::DragFloat("Área mínima", &s.min_region_area, 0.1f, 0.0f, 100.0f, "%.1f m²"));
    ImGui::SetItemTooltip("Las islas más pequeñas se quitan");
    track(ImGui::DragFloat("Detalle de altura", &s.detail_sample_distance, 0.1f, 0.0f, 32.0f, "%.1f"));
    ImGui::SetItemTooltip("Cada cuántas celdas se muestrea el relieve (0 = sin detalle)");
    track(ImGui::DragFloat("Error de altura", &s.detail_max_error, 0.05f, 0.1f, 16.0f, "%.2f"));

    ImGui::SeparatorText("Ejecución");
    track(ImGui::DragFloat("Coste de Evitar", &s.avoid_cost, 0.1f, 1.0f, 1000.0f, "%.1f"));
    ImGui::SetItemTooltip("Cuánto más caras son las zonas de Evitar que el suelo normal");
    track(ImGui::SliderInt("Agentes máximos", &s.max_agents, 1, 1024));
    const bool generation = ImGui::Checkbox("Generación en tiempo real (en Play y en el juego)", &s.runtime_generation);
    changed |= generation;
    finished |= generation;
    ImGui::SetItemTooltip("Si está desactivada, en el juego la malla se genera al cargar y ya no cambia");
    track(ImGui::SliderInt("Hilos (0 = auto)", &s.build_threads, 0, 16));

    if (changed && finished) {
        nav_.setSettings(nav_settings_);
        saveNavigationSettings();
    }
    if (ImGui::Button("Valores por defecto")) {
        nav_settings_ = navigation::NavigationSettings{};
        nav_.setSettings(nav_settings_);
        saveNavigationSettings();
    }

    ImGui::SeparatorText("Ayuda");
    ImGui::TextWrapped(
        "La malla sale de la geometría de colisión (como en Unreal): colliders de caja, esfera, cápsula, malla y "
        "plano, y terrenos con colisión. No cuentan los triggers, los Rigidbody dinámicos ni los agentes. "
        "Crea un volumen con Crear > Navegación y añade el componente Agente de navegación a lo que tenga que "
        "moverse (en Lua: self.entity:moveTo(destino)).");
    ImGui::End();
}

}  // namespace cramion::editor
