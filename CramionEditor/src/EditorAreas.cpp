// Areas al estilo de Blender: cada area del acoplado muestra UN editor, sin
// pestanas, con su cabecera arriba (la barra de menu de la ventana): el boton
// del tipo de editor y lo que ese editor ponga (menus, buscador, opciones).
//
// Por dentro siguen siendo ventanas acopladas de ImGui, pero sus nodos no
// tienen barra de pestanas (ImGuiDockNodeFlags_NoTabBar): de las ventanas de
// un nodo solo se ve la primera. Cambiar el tipo de editor de un area trae
// ese editor a su nodo y lo pone el primero (el de antes queda detras: como
// en Blender, nunca queda un area vacia). Los
// editores de Propiedades (Objeto, Render, Mundo y Fisica) viven en la misma
// area y se cambian con sus pestanas verticales.
//
// Tambien aqui: las pestanas de los espacios de trabajo en la barra superior
// y los avisos del raton de la barra de estado.

#include "EditorApp.h"

#include "BlenderUi.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace cramion::editor {

namespace {

namespace bl = blender;

struct AreaInfo {
    const char* name;   // titulo de la ventana
    const char* label;  // en el menu del tipo de editor
    bl::Glyph glyph;
    bool per_workspace;  // titulo con panelTitle (otra ventana en cada espacio de trabajo)
    int group;           // 0 general, 1 datos, 2 herramientas
};

constexpr std::size_t kAreaCount = static_cast<std::size_t>(AreaEditor::Count);
const AreaInfo kAreas[kAreaCount] = {
    {"Escena", "Vista 3D", bl::Glyph::View3D, true, 0},
    {"Juego", "Juego", bl::Glyph::Game, false, 0},
    {"Jerarquía", "Jerarquía (Outliner)", bl::Glyph::Outliner, true, 1},
    {"Inspector", "Propiedades", bl::Glyph::Properties, true, 1},
    {"Proyecto", "Proyecto (assets)", bl::Glyph::FileBrowser, true, 1},
    {"Consola", "Consola", bl::Glyph::Console, true, 2},
    {"Estadísticas", "Estadísticas", bl::Glyph::Statistics, false, 2},
    {"Ajustes de render", "Render", bl::Glyph::Render, false, 2},
    {"Física", "Física", bl::Glyph::Physics, false, 2},
    {"Cinemática", "Cinemática (línea de tiempo)", bl::Glyph::Timeline, false, 2},
    {"Ambiente", "Mundo (ambiente)", bl::Glyph::World, false, 2},
};

const AreaInfo& info(AreaEditor editor) { return kAreas[static_cast<std::size_t>(editor)]; }

// Las pestanas verticales del editor de Propiedades, en el orden de Blender.
constexpr AreaEditor kPropertiesTabs[] = {
    AreaEditor::RenderSettings, AreaEditor::Environment, AreaEditor::Inspector,
    AreaEditor::Physics};

bool isPropertiesEditor(AreaEditor editor) {
    return std::find(std::begin(kPropertiesTabs), std::end(kPropertiesTabs), editor) != std::end(kPropertiesTabs);
}

const char* propertiesTabName(AreaEditor editor) {
    switch (editor) {
        case AreaEditor::RenderSettings: return "Render: calidad, sombras, efectos y escalado";
        case AreaEditor::Environment: return "Mundo: cielo, clima y hora del dia";
        case AreaEditor::Inspector: return "Objeto: el objeto activo y sus componentes";
        case AreaEditor::Physics: return "Física: ajustes de la simulación";
        default: return "";
    }
}

// La primera de su nodo: la que se ve (los nodos de las areas no tienen
// pestanas; ImGui muestra Windows[0]).
void bringToFront(ImGuiWindow* window) {
    if (window == nullptr || window->DockNode == nullptr) return;
    ImVector<ImGuiWindow*>& list = window->DockNode->Windows;
    ImGuiWindow** it = list.find(window);
    if (it == list.end() || it == list.begin()) return;
    list.erase(it);
    list.push_front(window);
}

// La ventana acoplada de un hijo (los menus y popups no cuentan).
ImGuiWindow* dockedWindowOf(ImGuiWindow* window) {
    while (window != nullptr && !window->DockIsActive && (window->Flags & ImGuiWindowFlags_ChildWindow) &&
           !(window->Flags & ImGuiWindowFlags_Popup)) {
        window = window->ParentWindow;
    }
    return window;
}

// Acoplada detras de otra en su area (no se ve).
bool hiddenBehindInArea(ImGuiWindow* window) {
    window = dockedWindowOf(window);
    if (window == nullptr || !window->DockIsActive || window->DockNode == nullptr) return false;
    const ImVector<ImGuiWindow*>& list = window->DockNode->Windows;
    return !list.empty() && list[0] != window;
}

// Acoplada en un dockspace que se dibuja este frame (o flotante a la vista).
// Las de los dockspaces de otras pestanas (vivos con KeepAliveOnly) no:
// ponerlas delante cambiaria el area de esa pestana sin verse.
bool inActiveDockspace(ImGuiWindow* window) {
    if (window == nullptr || window->DockNode == nullptr) return false;
    return ImGui::DockNodeGetRootNode(window->DockNode)->LastFrameActive == ImGui::GetFrameCount();
}

// Intercambia dos ventanas acopladas en areas distintas (como "intercambiar
// areas" de Blender) sin sacarlas del acoplado. Con DockBuilderDockWindow no
// se puede: SetWindowDock quita la ventana de su nodo en el acto y, si se
// queda vacio, ImGui lo borra (y con el, el area). Aqui se cambian los
// punteros dentro de los dos nodos, que nunca se quedan vacios. Solo entre
// nodos del mismo anfitrion y sin barra de pestanas (las areas no tienen);
// si no, devuelve false y no toca nada. Llamar al principio del frame, antes
// de dibujar el dockspace y las ventanas.
bool swapDockedWindows(ImGuiWindow* a, ImGuiWindow* b) {
    if (a == nullptr || b == nullptr || a == b) return false;
    ImGuiDockNode* node_a = a->DockNode;
    ImGuiDockNode* node_b = b->DockNode;
    if (node_a == nullptr || node_b == nullptr || node_a == node_b) return false;
    if (node_a->HostWindow == nullptr || node_a->HostWindow != node_b->HostWindow) return false;
    if (node_a->TabBar != nullptr || node_b->TabBar != nullptr) return false;
    ImGuiWindow** slot_a = node_a->Windows.find(a);
    ImGuiWindow** slot_b = node_b->Windows.find(b);
    if (slot_a == node_a->Windows.end() || slot_b == node_b->Windows.end()) return false;
    *slot_a = b;
    *slot_b = a;
    std::swap(a->DockNode, b->DockNode);
    std::swap(a->DockId, b->DockId);
    std::swap(a->DockOrder, b->DockOrder);
    if (node_a->VisibleWindow == a) node_a->VisibleWindow = b;
    if (node_b->VisibleWindow == b) node_b->VisibleWindow = a;
    if (node_a->SelectedTabId == a->TabId) node_a->SelectedTabId = b->TabId;
    if (node_b->SelectedTabId == b->TabId) node_b->SelectedTabId = a->TabId;
    node_a->WantHiddenTabBarUpdate = true;
    node_b->WantHiddenTabBarUpdate = true;
    return true;
}

}  // namespace

bool EditorApp::workspaceDrawsArea(AreaEditor editor) const {
    switch (activeWorkspaceKind()) {
        case WorkspaceKind::Scene: return true;
        // drawBlueprintWorkspace solo dibuja su Vista 3D con beginArea (los
        // demas paneles del Blueprint son suyos).
        case WorkspaceKind::Blueprint: return editor == AreaEditor::Scene;
        // Prefab: Escena, Jerarquia, Propiedades, Proyecto y Consola (los de
        // titulo propio en cada pestana).
        default: return info(editor).per_workspace;
    }
}

std::string EditorApp::areaTitle(AreaEditor editor) const {
    const AreaInfo& a = info(editor);
    return a.per_workspace ? panelTitle(a.name) : std::string(a.name);
}

bool EditorApp::beginArea(AreaEditor editor, bool* p_open, ImGuiWindowFlags flags, const std::function<void()>& header) {
    const std::string title = areaTitle(editor);
    // Sin pestanas en el nodo: el area es el editor.
    ImGuiWindowClass area_class;
    area_class.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
    ImGui::SetNextWindowClass(&area_class);
    // Al cambiar de pestana (espacio de trabajo) vuelven a aparecer todas
    // las areas, tambien los editores que estan detras: sin esto cada una se
    // llevaba el foco al aparecer y promoteFocusedArea ponia delante la
    // ultima (Cinematica en vez de Proyecto, Ambiente en vez de Objeto...).
    // Un SetNextWindowFocus explicito sigue funcionando.
    if (ImGui::GetFrameCount() - workspace_switch_frame_ <= 1) flags |= ImGuiWindowFlags_NoFocusOnAppearing;
    const bool open = ImGui::Begin(title.c_str(), p_open, flags | ImGuiWindowFlags_MenuBar);
    // Todo lo de dentro del area, con el ID de su editor y su pestana (el
    // titulo lleva el sufijo de la pestana). No cambia el ID de la ventana ni
    // su acoplado. Siempre (tambien si Begin devuelve false): lo quita
    // endArea, antes de End.
    ImGui::PushID(title.c_str());
    area_stack_.push_back(AreaFrame{editor, false});
    ImGuiWindow* window = ImGui::GetCurrentWindow();

    // Pedida delante (cambio de tipo de editor) y ya en su area: la primera
    // de su nodo. (Lo que se suelta en un area o se abre desde Ventana recibe
    // el foco y lo pone delante promoteFocusedArea.)
    const ImGuiID node = window->DockNode != nullptr ? window->DockNode->ID : 0;
    // Al aparecer: delante si es la que su area tenia elegida. Al arrancar,
    // SelectedTabId es aun el del .ini (Selected=): asi vuelve el editor que
    // el usuario dejo en cada area (sin pestanas ImGui no guarda el orden y
    // pondria delante la primera que se dibuja). Despues coincide con la de
    // delante y no cambia nada.
    if (window->Appearing && window->DockNode != nullptr && window->DockNode->SelectedTabId == window->TabId) {
        bringToFront(window);
    }
    if (const auto it = area_front_.find(window->ID); it != area_front_.end() && node != 0) {
        if (it->second.node == 0 || it->second.node == node) {
            bringToFront(window);
            area_front_.erase(it);
        }
    }

    if (!open) return false;
    if (ImGui::BeginMenuBar()) {
        drawAreaTypeButton(editor);
        if (header) header();
        ImGui::EndMenuBar();
    }
    if (isPropertiesEditor(editor) && activeWorkspaceKind() == WorkspaceKind::Scene) {
        drawPropertiesTabs(editor);
        area_stack_.back().child = true;
    }
    return true;
}

void EditorApp::endArea() {
    if (area_stack_.empty()) {
        ImGui::End();
        return;
    }
    const AreaFrame frame = area_stack_.back();
    area_stack_.pop_back();
    if (frame.child) ImGui::EndChild();
    ImGui::PopID();  // el de beginArea (despues del hijo, antes de End)
    // Esquinas redondeadas del area (encima de su contenido), como Blender.
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->DockIsActive && window->DockTabIsVisible) {
        bl::areaCorners(window->DrawList, window->Pos, ImVec2(window->Pos.x + window->Size.x, window->Pos.y + window->Size.y),
                        6.0f, theme::kGap);
    }
    ImGui::End();
}

void EditorApp::promoteFocusedArea() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    // Recien cambiada la pestana: el foco era de una ventana de la otra
    // pestana, que ya no se dibuja (las areas aparecen sin llevarselo, ver
    // beginArea). Vuelve a la ventana que lo tenia en esta pestana; si no se
    // puede, a ninguna (nunca a un editor oculto detras en su area). Apuntada
    // como ya vista: no es un foco nuevo y no se trae delante.
    // Tambien si se lo llevo al aparecer una ventana que no es un area
    // (Navegacion, Pintar...) acoplada detras de otra: no es una eleccion.
    ImGuiWindow* nav_docked = dockedWindowOf(g.NavWindow);
    const bool stray_focus = g.NavWindow == nullptr || !g.NavWindow->Active ||
                             (nav_docked != nullptr && nav_docked->Appearing && hiddenBehindInArea(nav_docked));
    if (g.FrameCount - workspace_switch_frame_ <= 1 && stray_focus) {
        ImGuiWindow* target = workspace_restore_focus_ != 0 ? ImGui::FindWindowByID(workspace_restore_focus_) : nullptr;
        if (target != nullptr && (!target->Active || hiddenBehindInArea(target) ||
                                  (target->DockIsActive && !inActiveDockspace(target)))) {
            target = nullptr;
        }
        if (target != nullptr || g.NavWindow != nullptr) ImGui::FocusWindow(target);
        last_focused_window_ = g.NavWindow != nullptr ? g.NavWindow->ID : 0;
        last_focused_was_popup_ = false;
    }
    ImGuiWindow* window = g.NavWindow;
    const ImGuiID id = window != nullptr ? window->ID : 0;
    if (id == last_focused_window_) return;  // solo cuando cambia el foco
    const bool is_popup = window != nullptr && (window->Flags & ImGuiWindowFlags_Popup) != 0;
    const bool from_popup = last_focused_was_popup_;
    if (is_popup && !from_popup) focus_before_popup_ = last_focused_window_;
    last_focused_window_ = id;
    last_focused_was_popup_ = is_popup;
    // Al cerrarse un menu o popup ImGui devuelve el foco a la ventana que lo
    // tenia: no es un foco nuevo y no se trae delante (desharia el cambio de
    // tipo de editor elegido en ese menu). Un clic en otra area, o una
    // ventana enfocada desde el menu, si cuentan.
    if (from_popup && id == focus_before_popup_) return;
    // De un hijo a su ventana acoplada (los menus y popups no cuentan).
    window = dockedWindowOf(window);
    if (window == nullptr || !window->DockIsActive || window->DockNode == nullptr) return;
    // Oculta en el diseno de otra pestana (p. ej. Terminal acoplada en la
    // Escena y abierta desde el Prefab): no se toca esa area.
    if (!inActiveDockspace(window)) return;
    // La que tiene el foco en esta pestana: vuelve a tenerlo al volver a ella.
    workspace_focus_[active_workspace_] = window->ID;
    if (!window->DockNode->IsNoTabBar()) return;
    bringToFront(window);
}

void EditorApp::showAreaEditor(AreaEditor editor) {
    area_front_[ImHashStr(areaTitle(editor).c_str())] = AreaFrontRequest{0, ImGui::GetFrameCount()};
}

void EditorApp::switchArea(AreaEditor from, AreaEditor to) {
    if (from == to || !workspaceDrawsArea(to)) return;
    // Se aplica al principio del frame siguiente (applyPendingAreaSwitch):
    // a mitad de frame el popup que se cierra devuelve el foco al area de
    // antes y las ventanas de este frame ya estan dibujadas.
    pending_area_switch_.from = from;
    pending_area_switch_.to = to;
    pending_area_switch_.workspace = active_workspace_;
    pending_area_switch_.valid = true;
}

// Como en Blender, el area donde se hizo clic pasa a mostrar el editor
// elegido:
// - ya esta en esta area (detras): delante y con el foco;
// - esta solo en otra area: se intercambian las dos areas;
// - esta en otra area con mas editores: viene a esta (la otra sigue con los
//   suyos);
// - no existe o esta cerrado: se abre ya acoplado en esta area, delante.
// Un area flotante no cambia: el editor elegido se trae delante donde este.
void EditorApp::applyPendingAreaSwitch() {
    // Peticiones de "ponla delante" que nadie recogio en unos frames (la
    // ventana no se dibujo: otra pestana, o un editor que no se dibuja): fuera.
    const int frame = ImGui::GetFrameCount();
    for (auto it = area_front_.begin(); it != area_front_.end();) {
        if (frame - it->second.frame > 10) {
            it = area_front_.erase(it);
        } else {
            ++it;
        }
    }
    if (!pending_area_switch_.valid) return;
    const PendingAreaSwitch request = pending_area_switch_;
    pending_area_switch_ = PendingAreaSwitch{};
    // Otra pestana desde que se pidio: los titulos ya no son los de entonces.
    if (request.workspace != active_workspace_) return;
    const AreaEditor from = request.from;
    const AreaEditor to = request.to;
    if (from == to || !workspaceDrawsArea(to)) return;
    // Que se dibuje (los que tienen casilla en Ventana).
    switch (to) {
        case AreaEditor::Game: show_game_ = true; break;
        case AreaEditor::Hierarchy: show_hierarchy_ = true; break;
        case AreaEditor::Inspector: show_inspector_ = true; break;
        case AreaEditor::Project: show_project_ = true; break;
        case AreaEditor::Console: show_console_ = true; break;
        case AreaEditor::Statistics: show_statistics_ = true; break;
        case AreaEditor::RenderSettings: show_render_settings_ = true; break;
        case AreaEditor::Physics: show_physics_ = true; break;
        case AreaEditor::Cinematic: show_cinematic_ = true; break;
        case AreaEditor::Environment: show_environment_window_ = true; break;
        default: break;
    }
    if (to == AreaEditor::Scene) focus_scene_ = true;
    if (to == AreaEditor::Game) {
        focus_game_ = true;
        preferred_view_ = kGameSlot;
    }
    const std::string from_title = areaTitle(from);
    const std::string to_title = areaTitle(to);
    ImGuiWindow* from_window = ImGui::FindWindowByName(from_title.c_str());
    ImGuiWindow* to_window = ImGui::FindWindowByName(to_title.c_str());
    const ImGuiID to_id = ImHashStr(to_title.c_str());
    const auto front = [&](ImGuiID window_id, ImGuiID node_id) {
        area_front_[window_id] = AreaFrontRequest{node_id, frame};
    };
    // El foco al editor elegido, y apuntado como ya visto: promoteFocusedArea
    // no debe tomarlo por un foco nuevo (ni volver a poner delante el de antes).
    const auto focus = [&](ImGuiWindow* window) {
        if (window == nullptr) return;
        ImGui::FocusWindow(window);
        last_focused_window_ = window->ID;
        last_focused_was_popup_ = false;
        workspace_focus_[active_workspace_] = window->ID;
    };
    if (from_window == nullptr || from_window->DockNode == nullptr) {
        // Area flotante: el otro editor, al frente (donde este).
        front(to_id, 0);
        focus(to_window);
        return;
    }
    ImGuiDockNode* node = from_window->DockNode;
    if (to_window != nullptr && to_window->DockNode == node) {
        bringToFront(to_window);
        focus(to_window);
        return;
    }
    if (to_window != nullptr && to_window->DockNode != nullptr && to_window->DockNode->Windows.Size <= 1) {
        // Esta solo en otra area: se intercambian las dos (sacarlo de alli con
        // DockBuilderDockWindow dejaria ese nodo vacio e ImGui lo borraria).
        ImGuiDockNode* other = to_window->DockNode;
        if (swapDockedWindows(from_window, to_window)) {
            bringToFront(to_window);
            bringToFront(from_window);
            front(to_id, node->ID);
            front(from_window->ID, other->ID);
        }
        // Si no se pudo (otro anfitrion, nodo con pestanas): como antes, el
        // foco donde esta.
        focus(to_window);
        return;
    }
    // Si no (en otra area con mas editores, cerrado o sin crear), viene a
    // esta area y se pone delante (el de antes queda detras: nunca se queda
    // un area vacia).
    ImGui::DockBuilderDockWindow(to_title.c_str(), node->ID);
    front(to_id, node->ID);
    focus(to_window);
}

void EditorApp::drawAreaTypeButton(AreaEditor editor) {
    const AreaInfo& current = info(editor);
    if (bl::editorTypeButton("##area_type", current.glyph, current.label)) ImGui::OpenPopup("##area_type_menu");
    if (ImGui::BeginPopup("##area_type_menu")) {
        static const char* const kGroups[] = {"General", "Datos", "Herramientas"};
        for (int group = 0; group < 3; ++group) {
            // Solo los editores que dibuja esta pestana (en un prefab, los
            // suyos; en un Blueprint, su Vista 3D); sin grupos vacios.
            bool any = false;
            for (std::size_t i = 0; i < kAreaCount && !any; ++i) {
                any = kAreas[i].group == group && workspaceDrawsArea(static_cast<AreaEditor>(i));
            }
            if (!any) continue;
            ImGui::SeparatorText(kGroups[group]);
            for (std::size_t i = 0; i < kAreaCount; ++i) {
                const auto e = static_cast<AreaEditor>(i);
                const AreaInfo& a = kAreas[i];
                if (a.group != group) continue;
                if (!workspaceDrawsArea(e)) continue;
                ImGui::PushID(static_cast<int>(i));
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float h = ImGui::GetTextLineHeight();
                char label[96];
                std::snprintf(label, sizeof(label), "      %s", a.label);
                if (ImGui::Selectable(label, e == editor) && e != editor) switchArea(editor, e);
                bl::drawGlyph(ImGui::GetWindowDrawList(), a.glyph, ImVec2(p.x + h * 0.6f, p.y + h * 0.5f), h, theme::kText);
                ImGui::PopID();
            }
        }
        ImGui::Separator();
        ImGuiWindow* window = ImGui::FindWindowByName(areaTitle(editor).c_str());
        const bool docked = window != nullptr && window->DockNode != nullptr;
        if (ImGui::MenuItem("Desacoplar (ventana flotante)", nullptr, false, docked)) {
            ImGui::DockContextQueueUndockWindow(ImGui::GetCurrentContext(), window);
        }
        ImGui::SetItemTooltip("Para volver a acoplarla, arrastra su barra de titulo sobre otra area.");
        ImGui::EndPopup();
    }
}

void EditorApp::drawPropertiesTabs(AreaEditor editor) {
    const float width = std::floor(ImGui::GetFrameHeight() * 1.45f);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    // La columna va pegada al borde del area (sin el margen de la ventana).
    const ImVec2 pad = ImGui::GetStyle().WindowPadding;
    const ImVec2 column(start.x - pad.x, start.y - pad.y + 1.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    bl::verticalTabsBackground(draw, column, ImVec2(column.x + width, start.y + avail.y + pad.y));
    float y = column.y + 4.0f;
    for (const AreaEditor tab : kPropertiesTabs) {
        ImGui::SetCursorScreenPos(ImVec2(column.x, y));
        ImGui::PushID(static_cast<int>(tab));
        if (bl::verticalTab("##prop_tab", info(tab).glyph, tab == editor, propertiesTabName(tab), width) && tab != editor) {
            switchArea(editor, tab);
        }
        ImGui::PopID();
        y += width + 2.0f;
    }
    // El contenido a la derecha de la columna, con su margen.
    ImGui::SetCursorScreenPos(ImVec2(column.x + width, column.y - 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad);
    ImGui::BeginChild("##properties_content", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
}

// -----------------------------------------------------------------------------
// Barra superior: espacios de trabajo
// -----------------------------------------------------------------------------

void EditorApp::drawWorkspaceTabs(float right_limit) {
    if (workspaces_.empty()) resetWorkspaces();
    syncScriptWorkspaces();
    syncGraphWorkspaces();
    static constexpr ImU32 kPrefabColor = IM_COL32(110, 170, 255, 255);
    static constexpr ImU32 kScriptColor = IM_COL32(120, 200, 130, 255);
    static constexpr ImU32 kGraphColor = IM_COL32(200, 140, 255, 255);
    static constexpr ImU32 kMachineColor = IM_COL32(255, 175, 80, 255);
    static constexpr ImU32 kBlueprintColor = IM_COL32(80, 150, 255, 255);
    int clicked = -1;
    int close = -1;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 0.0f));
    std::size_t hidden = 0;
    for (const Workspace& ws : workspaces_) {
        const bool loaded = ws.id == world_workspace_;
        bool dirty = false;
        std::string name = ws.name;
        ImU32 accent = 0;
        const char* prefix = "";
        if (ws.kind == WorkspaceKind::Scene) {
            dirty = loaded ? dirty_ : ws.dirty;
            if (loaded) name = world_.sceneName();
        } else if (ws.kind == WorkspaceKind::Prefab) {
            dirty = loaded ? dirty_ : ws.dirty;
            prefix = "Prefab: ";
            accent = kPrefabColor;
        } else if (ws.kind == WorkspaceKind::Blueprint) {
            dirty = blueprintDirty(ws.id);
            prefix = "Blueprint: ";
            accent = kBlueprintColor;
        } else if (ws.kind == WorkspaceKind::StateMachine) {
            prefix = "Máquina: ";
            accent = kMachineColor;
            dirty = fsm_dirty_;
        } else if (ws.kind == WorkspaceKind::Graph) {
            static const char* const kPrefixes[] = {"VFX: ", "Shader: ", "Visual Script: ", "Behavior Tree: ", "Diálogo: ",
                                                    "Animator: "};
            prefix = kPrefixes[static_cast<int>(ws.graph)];
            accent = kGraphColor;
            dirty = graphDoc(ws.graph).dirty;
        } else {
            prefix = "Script: ";
            accent = kScriptColor;
            for (const ScriptTab& tab : script_tabs_) {
                if (tab.path == ws.path) dirty = tab.text != tab.saved;
            }
        }
        const std::string label = std::string(prefix) + name;
        // Las que no caben, en el desplegable del final.
        const float width = ImGui::CalcTextSize(label.c_str()).x + ImGui::GetFrameHeight() * 1.6f;
        if (ImGui::GetCursorScreenPos().x + width > right_limit - ImGui::GetFrameHeight() * 2.0f && ws.id != active_workspace_) {
            ++hidden;
            continue;
        }
        bool closing = false;
        const std::string id = "ws" + std::to_string(ws.id);
        if (bl::workspaceTab(id.c_str(), label.c_str(), ws.id == active_workspace_, dirty, accent,
                             ws.kind == WorkspaceKind::Scene ? nullptr : &closing) &&
            ws.id != active_workspace_) {
            clicked = ws.id;
        }
        if (closing) close = ws.id;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            if (ws.kind == WorkspaceKind::Scene) {
                ImGui::SetTooltip("La escena abierta (todo el editor)");
            } else {
                ImGui::SetTooltip("%s", dialogs::utf8(ws.path).c_str());
            }
        }
        ImGui::SameLine();
    }
    if (hidden > 0) {
        const std::string more = "+" + std::to_string(hidden);
        if (ImGui::SmallButton(more.c_str())) ImGui::OpenPopup("##more_workspaces");
        ImGui::SetItemTooltip("Más espacios de trabajo abiertos");
        if (ImGui::BeginPopup("##more_workspaces")) {
            for (const Workspace& ws : workspaces_) {
                // Dos pestanas pueden llamarse igual (Player.cpp de dos
                // carpetas, un prefab y un Blueprint "Enemy"): ID por pestana.
                ImGui::PushID(ws.id);
                if (ImGui::MenuItem(ws.name.c_str(), nullptr, ws.id == active_workspace_)) clicked = ws.id;
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
    }
    ImGui::PopStyleVar(2);
    if (clicked >= 0) requestWorkspace(clicked);
    if (close >= 0) {
        const Workspace* ws = findWorkspace(close);
        const bool dirty = ws != nullptr && prefabLikeDirty(close);
        if (dirty) {
            workspace_close_ask_ = close;
            workspace_close_popup_ = true;
        } else {
            closeWorkspace(close, false);
        }
    }
}

// -----------------------------------------------------------------------------
// Barra de estado: los botones del raton (como Blender)
// -----------------------------------------------------------------------------

void EditorApp::drawStatusHints() {
    struct Hint {
        bl::Glyph glyph;
        const char* text;
    };
    Hint hints[4]{};
    int count = 0;
    if (flying_) {
        hints[count++] = {bl::Glyph::Move, "WASD  Volar"};
        hints[count++] = {bl::Glyph::Zoom, "Rueda  Velocidad"};
        hints[count++] = {bl::Glyph::Select, "Soltar  Dejar de volar"};
    } else if (view_hovered_) {
        hints[count++] = {bl::Glyph::Select, "Seleccionar"};
        hints[count++] = {bl::Glyph::Rotate, "Clic der.  Volar"};
        hints[count++] = {bl::Glyph::Pan, "Clic central  Desplazar"};
        hints[count++] = {bl::Glyph::Zoom, "Rueda  Acercar"};
    } else {
        hints[count++] = {bl::Glyph::Select, "Seleccionar"};
        hints[count++] = {bl::Glyph::Search, "F  Enfocar"};
        hints[count++] = {bl::Glyph::Play, "Ctrl+P  Play"};
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float size = ImGui::GetFontSize();
    for (int i = 0; i < count; ++i) {
        if (i > 0) ImGui::SameLine(0.0f, 16.0f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        bl::drawGlyph(draw, hints[i].glyph, ImVec2(p.x + size * 0.5f, p.y + size * 0.5f), size * 0.9f, theme::kTextDim);
        ImGui::Dummy(ImVec2(size, size));
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::TextDisabled("%s", hints[i].text);
    }
}

}  // namespace cramion::editor
