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

}  // namespace

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
    const bool open = ImGui::Begin(title.c_str(), p_open, flags | ImGuiWindowFlags_MenuBar);
    area_stack_.push_back(AreaFrame{editor, false});
    ImGuiWindow* window = ImGui::GetCurrentWindow();

    // Pedida delante (cambio de tipo de editor) y ya en su area: la primera
    // de su nodo. (Lo que se suelta en un area o se abre desde Ventana recibe
    // el foco y lo pone delante promoteFocusedArea.)
    const ImGuiID node = window->DockNode != nullptr ? window->DockNode->ID : 0;
    if (const auto it = area_front_.find(window->ID); it != area_front_.end() && node != 0) {
        if (it->second == 0 || it->second == node) {
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
    // Esquinas redondeadas del area (encima de su contenido), como Blender.
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->DockIsActive && window->DockTabIsVisible) {
        bl::areaCorners(window->DrawList, window->Pos, ImVec2(window->Pos.x + window->Size.x, window->Pos.y + window->Size.y),
                        6.0f, theme::kGap);
    }
    ImGui::End();
}

void EditorApp::promoteFocusedArea() {
    ImGuiWindow* window = ImGui::GetCurrentContext()->NavWindow;
    const ImGuiID id = window != nullptr ? window->ID : 0;
    if (id == last_focused_window_) return;  // solo cuando cambia el foco
    last_focused_window_ = id;
    // De un hijo a su ventana acoplada (los menus y popups no cuentan).
    while (window != nullptr && !window->DockIsActive && (window->Flags & ImGuiWindowFlags_ChildWindow) &&
           !(window->Flags & ImGuiWindowFlags_Popup)) {
        window = window->ParentWindow;
    }
    if (window == nullptr || !window->DockIsActive || window->DockNode == nullptr) return;
    if (!window->DockNode->IsNoTabBar()) return;
    bringToFront(window);
}

void EditorApp::showAreaEditor(AreaEditor editor) {
    area_front_[ImHashStr(areaTitle(editor).c_str())] = 0;
}

void EditorApp::switchArea(AreaEditor from, AreaEditor to) {
    if (from == to) return;
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
    if (from_window == nullptr || from_window->DockNode == nullptr) {
        // Area flotante: el otro editor, al frente (donde este).
        area_front_[to_id] = 0;
        if (to_window != nullptr) ImGui::FocusWindow(to_window);
        return;
    }
    ImGuiDockNode* node = from_window->DockNode;
    if (to_window != nullptr && to_window->DockNode == node) {
        bringToFront(to_window);
        return;
    }
    // Esta solo en otra area: no se saca de alli (ImGui borraria esa area y
    // el editor que quedase sin sitio saldria flotando); se le da el foco
    // para que se vea donde esta.
    if (to_window != nullptr && to_window->DockNode != nullptr && to_window->DockNode->Windows.Size <= 1) {
        ImGui::FocusWindow(to_window);
        return;
    }
    // Si no, viene a esta area y se pone delante (el de antes queda detras:
    // nunca se queda un area vacia).
    ImGui::DockBuilderDockWindow(to_title.c_str(), node->ID);
    area_front_[to_id] = node->ID;
    if (to_window != nullptr) ImGui::FocusWindow(to_window);
}

void EditorApp::drawAreaTypeButton(AreaEditor editor) {
    const AreaInfo& current = info(editor);
    if (bl::editorTypeButton("##area_type", current.glyph, current.label)) ImGui::OpenPopup("##area_type_menu");
    if (ImGui::BeginPopup("##area_type_menu")) {
        const WorkspaceKind kind = activeWorkspaceKind();
        static const char* const kGroups[] = {"General", "Datos", "Herramientas"};
        for (int group = 0; group < 3; ++group) {
            ImGui::SeparatorText(kGroups[group]);
            for (std::size_t i = 0; i < kAreaCount; ++i) {
                const auto e = static_cast<AreaEditor>(i);
                const AreaInfo& a = kAreas[i];
                if (a.group != group) continue;
                // En un prefab solo estan los suyos.
                if (kind != WorkspaceKind::Scene && !a.per_workspace) continue;
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
                if (ImGui::MenuItem(ws.name.c_str(), nullptr, ws.id == active_workspace_)) clicked = ws.id;
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
    }
    ImGui::PopStyleVar(2);
    if (clicked >= 0) requestWorkspace(clicked);
    if (close >= 0) {
        const Workspace* ws = findWorkspace(close);
        const bool dirty = ws != nullptr && ws->kind == WorkspaceKind::Prefab &&
                           (close == world_workspace_ ? dirty_ : ws->dirty);
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
