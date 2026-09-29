// Interfaz tactil del proyecto (Archivo > Controles tactiles), como el Touch
// Interface de Unreal: se disena aqui una vez (joystick, zona de mirar y
// botones que pulsan teclas), se guarda en ProjectSettings/TouchInterface.json
// y el juego en el movil la usa sola. En Play, el boton "Tactil" de la vista
// Juego la simula con el raton como dedo.

#include "EditorApp.h"

#include "Dialogs.h"

#include <CramionCore/project/TouchInterface.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <iostream>

namespace cramion::editor {

namespace {

// Acciones que se ofrecen en la lista (se puede escribir cualquier tecla).
constexpr const char* kActions[] = {"Space", "E", "F", "Q", "R", "LeftShift", "LeftControl", "Tab", "Escape", "Enter",
                                    "1", "2", "3", "Mouse0", "Mouse1"};

// Dibuja stick y botones como en el juego. `dp`: pixeles por dp.
void drawTouchOverlay(ImDrawList* draw, const ImVec2& origin, const ImVec2& size, const dm::TouchLayout& t, float dp,
                      const dm::TouchControls* live, int highlighted) {
    const auto alpha = [&](float k) { return static_cast<int>(std::clamp(t.opacity * k, 0.0f, 1.0f) * 255.0f); };
    if (!t.enabled) return;
    if (t.joystick) {
        float r = 64.0f * dp * t.scale;
        ImVec2 base(origin.x + std::max(size.x * 0.14f, r * 1.6f), origin.y + size.y - std::max(size.y * 0.24f, r * 1.6f));
        ImVec2 knob = base;
        bool active = false;
        if (live != nullptr) {
            const dm::TouchControls::StickView s = live->stick();
            r = s.radius;
            base = ImVec2(origin.x + s.base_x, origin.y + s.base_y);
            knob = ImVec2(origin.x + s.knob_x, origin.y + s.knob_y);
            active = s.active;
        }
        draw->AddCircleFilled(base, r, IM_COL32(15, 17, 22, alpha(0.45f)), 40);
        draw->AddCircle(base, r, IM_COL32(255, 255, 255, alpha(0.55f)), 40, 1.5f);
        draw->AddCircleFilled(knob, r * 0.42f, IM_COL32(255, 255, 255, alpha(active ? 0.95f : 0.6f)), 24);
    }
    for (int i = 0; i < static_cast<int>(t.buttons.size()); ++i) {
        const dm::TouchButton& b = t.buttons[static_cast<std::size_t>(i)];
        if (!b.visible && live != nullptr) continue;  // oculto en el juego
        const ImVec2 center(origin.x + b.x * size.x, origin.y + b.y * size.y);
        const float r = 34.0f * dp * t.scale * b.size;
        const bool down = i == highlighted || (live != nullptr && live->buttonDown(static_cast<std::size_t>(i)));
        const float fade = b.visible ? 1.0f : 0.4f;  // en el disenador: los ocultos al empezar, tenues
        draw->AddCircleFilled(center, r, down ? IM_COL32(90, 150, 255, alpha(0.9f)) : IM_COL32(15, 17, 22, alpha(0.5f * fade)), 32);
        draw->AddCircle(center, r, IM_COL32(255, 255, 255, alpha(0.7f * fade)), 32, 1.5f);
        const ImVec2 text = ImGui::CalcTextSize(b.label.c_str());
        const float fit = std::min(1.0f, r * 1.8f / std::max(text.x, 1.0f));
        draw->AddText(nullptr, ImGui::GetFontSize() * fit, ImVec2(center.x - text.x * fit * 0.5f, center.y - text.y * fit * 0.5f),
                      IM_COL32(255, 255, 255, alpha(1.4f * fade)), b.label.c_str());
    }
}

}  // namespace

void EditorApp::loadTouchInterface() {
    if (!has_project_) return;
    const std::filesystem::path file = project::touchInterfaceFile(project_.settingsFolder());
    if (touch_layout_file_ == file) return;
    touch_layout_file_ = file;
    touch_layout_ = project::loadTouchInterface(file);
}

void EditorApp::drawTouchInterfaceWindow() {
    if (!show_touch_interface_ || !has_project_) return;
    loadTouchInterface();
    ImGui::SetNextWindowSize(ImVec2(620.0f, 640.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Controles táctiles", &show_touch_interface_, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }
    dm::TouchLayout& t = touch_layout_;
    bool changed = false;
    ImGui::TextWrapped("Los controles en pantalla del juego en moviles. Se guardan en el proyecto "
                       "(ProjectSettings/TouchInterface.json) y el juego exportado los usa solos. En Play, activa "
                       "\"Tactil\" en la vista Juego para probarlos con el raton.");
    ImGui::Separator();
    changed |= ImGui::Checkbox("Controles en pantalla", &t.enabled);
    ImGui::SetItemTooltip("Apagado: el dedo hace de raton (toque = clic). Lua: Input.setTouchControls(true/false)");
    ImGui::BeginDisabled(!t.enabled);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Joystick (mover)", &t.joystick);
    ImGui::SetItemTooltip("Mitad izquierda: aparece donde se apoya el pulgar. Mueve Horizontal/Vertical (como WASD).");
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Arrastrar para mirar", &t.look);
    ImGui::SetItemTooltip("El resto de la pantalla: arrastrar = mouseDelta / \"Mouse X\" (como mirar con el raton).");
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Toque corto = clic", &t.tap_clicks);
    ImGui::SetNextItemWidth(160.0f);
    changed |= ImGui::SliderFloat("Sensibilidad", &t.look_sensitivity, 0.1f, 4.0f, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    changed |= ImGui::SliderFloat("Tamano", &t.scale, 0.5f, 2.0f, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    changed |= ImGui::SliderFloat("Opacidad", &t.opacity, 0.1f, 1.0f, "%.2f");

    // Vista previa de un movil: arrastrar los botones para colocarlos.
    ImGui::Checkbox("Vista vertical", &touch_preview_portrait_);
    const bool portrait = touch_preview_portrait_;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float pw = portrait ? std::min(avail, 230.0f) : std::min(avail, 560.0f);
    const float ph = portrait ? pw * 2.1f : pw * 9.0f / 19.5f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size(pw, ph);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p0, ImVec2(p0.x + pw, p0.y + ph), IM_COL32(24, 30, 40, 255), 12.0f);
    draw->AddRect(p0, ImVec2(p0.x + pw, p0.y + ph), IM_COL32(90, 100, 120, 255), 12.0f, 0, 2.0f);
    // 1 dp de un movil de ~400 dp de lado corto.
    const float dp = (portrait ? pw : ph) / 400.0f;
    ImGui::InvisibleButton("##touch_preview", size);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) touch_button_dragged_ = -1;
    for (int i = 0; i < static_cast<int>(t.buttons.size()); ++i) {
        dm::TouchButton& b = t.buttons[static_cast<std::size_t>(i)];
        const float r = 34.0f * dp * t.scale * b.size;
        const float dx = mouse.x - (p0.x + b.x * pw);
        const float dy = mouse.y - (p0.y + b.y * ph);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && dx * dx + dy * dy <= r * r * 1.4f) touch_button_dragged_ = i;
        if (touch_button_dragged_ == i) {
            b.x = std::clamp((mouse.x - p0.x) / pw, 0.02f, 0.98f);
            b.y = std::clamp((mouse.y - p0.y) / ph, 0.02f, 0.98f);
            changed = true;
        }
    }
    drawTouchOverlay(draw, p0, size, t, dp, nullptr, touch_button_dragged_);
    ImGui::TextDisabled("Arrastra los botones en la vista previa. La posicion es relativa a la pantalla (vale en cualquier movil).");

    // Botones.
    int remove = -1;
    if (ImGui::BeginTable("##touch_buttons", 5, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Texto (nombre en Lua)", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Accion", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Tamano", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Visible", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < static_cast<int>(t.buttons.size()); ++i) {
            dm::TouchButton& b = t.buttons[static_cast<std::size_t>(i)];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            changed |= ImGui::InputText("##label", &b.label);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##action", b.action.c_str())) {
                for (const char* action : kActions) {
                    if (ImGui::Selectable(action, b.action == action)) {
                        b.action = action;
                        changed = true;
                    }
                }
                ImGui::Separator();
                ImGui::SetNextItemWidth(120.0f);
                changed |= ImGui::InputTextWithHint("##custom", "otra tecla", &b.action);
                ImGui::EndCombo();
            }
            if (dm::keyFromName(b.action) == dm::Key::Unknown && b.action.rfind("Mouse", 0) != 0) {
                ImGui::SetItemTooltip("\"%s\" no es una tecla conocida", b.action.c_str());
            } else {
                ImGui::SetItemTooltip("Mientras se mantiene, pulsa esta tecla (Input.getKey) o Mouse0/Mouse1.");
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            changed |= ImGui::SliderFloat("##size", &b.size, 0.5f, 2.0f, "%.2f");
            ImGui::TableNextColumn();
            changed |= ImGui::Checkbox("##visible", &b.visible);
            ImGui::SetItemTooltip("Visible al empezar. Lua: Input.setTouchButton(\"%s\", true/false)", b.label.c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x")) remove = i;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remove >= 0) {
        t.buttons.erase(t.buttons.begin() + remove);
        changed = true;
    }
    if (ImGui::Button("Anadir boton")) {
        dm::TouchButton b;
        b.label = "Boton " + std::to_string(t.buttons.size() + 1);
        b.action = "F";
        b.x = 0.75f;
        b.y = 0.6f;
        t.buttons.push_back(b);
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Restablecer")) {
        t = dm::defaultTouchLayout();
        changed = true;
    }
    ImGui::Separator();
    ImGui::TextDisabled("Lua: Input.setTouchControls(on)  Input.setTouchButton(\"Saltar\", on)  Input.setTouchJoystick(on)\n"
                        "     Input.setTouchLook(on)  Screen.setOrientation(\"auto\" | \"landscape\" | \"portrait\" | ...)");
    if (changed) {
        if (!project::saveTouchInterface(touch_layout_file_, t)) {
            std::cerr << "[Tactil] No se pudo guardar " << dialogs::utf8(touch_layout_file_) << "\n";
        }
        if (playing()) touch_game_.setLayout(t);  // se ve en Play al momento
    }
    ImGui::End();
}

// --- Simulacion en la vista Juego (Play): el raton hace de dedo ---

void EditorApp::drawTouchSimulation(const ImVec2& origin, const ImVec2& size) {
    touch_game_.setScreen(size.x, size.y, 1.0f);
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 local(io.MousePos.x - origin.x, io.MousePos.y - origin.y);
    const bool inside = local.x >= 0.0f && local.y >= 0.0f && local.x < size.x && local.y < size.y;
    const double time = ImGui::GetTime();
    const auto send = [&](dm::EventType type) {
        dm::Event e;
        e.type = type;
        e.touchId = 0;
        e.mouseX = std::clamp(local.x, 0.0f, size.x);
        e.mouseY = std::clamp(local.y, 0.0f, size.y);
        touch_pending_.push_back(e);  // Input.getTouch en Lua
        touch_game_.onEvent(e, time, touch_pending_);
    };
    if (!touch_sim_down_ && inside && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        touch_sim_down_ = true;
        send(dm::EventType::TouchBegan);
    } else if (touch_sim_down_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        touch_sim_down_ = false;
        send(dm::EventType::TouchEnded);
    } else if (touch_sim_down_ && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
        send(dm::EventType::TouchMoved);
    }
    drawTouchOverlay(ImGui::GetWindowDrawList(), origin, size, touch_game_.layout(), 1.0f, &touch_game_, -1);
}

void EditorApp::onRawInputEvent(const dm::Event& e) {
    // Con la simulacion, el raton es el dedo: el juego no lo recibe como raton.
    switch (e.type) {
        case dm::EventType::MouseButtonPressed:
        case dm::EventType::MouseButtonReleased:
        case dm::EventType::MouseMoved:
        case dm::EventType::MouseRawMoved:
        case dm::EventType::MouseScrolled: return;
        default: touch_sim_input_.onEvent(e); break;
    }
}

void EditorApp::endInputFrame() { touch_sim_input_.newFrame(); }

const dm::Input* EditorApp::touchSimulatedInput() {
    if (!touch_simulate_) return input_;
    for (const dm::Event& e : touch_pending_) touch_sim_input_.onEvent(e);
    touch_pending_.clear();
    std::vector<dm::Event> released;
    touch_game_.update(released);
    for (const dm::Event& e : released) touch_sim_input_.onEvent(e);
    touch_game_.apply(touch_sim_input_);
    return &touch_sim_input_;
}

}  // namespace cramion::editor
