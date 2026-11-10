#include "CramionDM/TouchControls.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace cramion::dm {

namespace {

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Toque corto: menos de este tiempo y de este recorrido (en dp).
constexpr double kTapSeconds = 0.3;
constexpr float kTapTravelDp = 14.0f;
// Stick: zona muerta (fraccion del radio) y empujar para correr (mas alla del
// aro: entra a 1.3 radios, sale por debajo de 1.15; la base sigue al dedo
// pasado 1.6, asi nunca hay que volver atras para cambiar de direccion).
constexpr float kStickDead = 0.12f;
constexpr float kSprintOn = 1.3f;
constexpr float kSprintOff = 1.15f;
constexpr float kStickFollow = 1.6f;
// Un dedo que empieza en un boton de accion y se arrastra mas de esto (dp)
// gira tambien la camara (el boton sigue pulsado: atacar y mirar a la vez).
constexpr float kButtonDragDp = 12.0f;
// Editor: la barra de arriba (-, +, Reset, Done) y los limites de tamano.
constexpr float kPanelItemW = 64.0f, kPanelItemH = 40.0f, kPanelGap = 10.0f, kPanelTop = 10.0f;
constexpr const char* kPanelLabels[] = {"-", "+", "Reset", "Done"};

}  // namespace

TouchLayout defaultTouchLayout() {
    TouchLayout layout;
    const auto button = [](const char* label, const char* action, float x, float y, float size) {
        TouchButton b;
        b.label = label;
        b.action = action;
        b.x = x;
        b.y = y;
        b.size = size;
        return b;
    };
    layout.buttons = {
        button("Saltar", "Space", 0.90f, 0.78f, 1.15f),
        button("Usar", "E", 0.80f, 0.86f, 0.85f),
        button("Correr", "LeftShift", 0.92f, 0.56f, 0.85f),
    };
    return layout;
}

std::string touchLayoutIni(const TouchLayout& layout) {
    std::ostringstream out;
    out << "touch_enabled=" << (layout.enabled ? 1 : 0) << "\n";
    out << "touch_joystick=" << (layout.joystick ? 1 : 0) << "\n";
    out << "touch_look=" << (layout.look ? 1 : 0) << "\n";
    out << "touch_sensitivity=" << layout.look_sensitivity << "\n";
    out << "touch_scale=" << layout.scale << "\n";
    out << "touch_opacity=" << layout.opacity << "\n";
    out << "touch_tap=" << (layout.tap_clicks ? 1 : 0) << "\n";
    for (const TouchButton& b : layout.buttons) {
        std::string label = b.label;
        std::replace(label.begin(), label.end(), '|', '/');
        out << "touch_button=" << label << "|" << b.action << "|" << b.x << "|" << b.y << "|" << b.size << "|"
            << (b.visible ? 1 : 0) << "\n";
    }
    return out.str();
}

bool parseTouchLayoutLine(const std::string& key, const std::string& value, TouchLayout& layout) {
    const auto number = [&](float fallback) {
        char* end = nullptr;
        const float v = std::strtof(value.c_str(), &end);
        return end != value.c_str() && std::isfinite(v) ? v : fallback;
    };
    if (key == "touch_enabled") layout.enabled = value == "1";
    else if (key == "touch_joystick") layout.joystick = value == "1";
    else if (key == "touch_look") layout.look = value == "1";
    else if (key == "touch_sensitivity") layout.look_sensitivity = std::clamp(number(1.0f), 0.05f, 10.0f);
    else if (key == "touch_scale") layout.scale = std::clamp(number(1.0f), 0.3f, 3.0f);
    else if (key == "touch_opacity") layout.opacity = std::clamp(number(0.55f), 0.0f, 1.0f);
    else if (key == "touch_tap") layout.tap_clicks = value == "1";
    else if (key == "touch_button") {
        std::vector<std::string> parts;
        std::stringstream in(value);
        std::string part;
        while (std::getline(in, part, '|')) parts.push_back(part);
        if (parts.size() < 4) return true;
        TouchButton b;
        b.label = parts[0];
        b.action = parts[1];
        b.x = std::clamp(std::strtof(parts[2].c_str(), nullptr), 0.0f, 1.0f);
        b.y = std::clamp(std::strtof(parts[3].c_str(), nullptr), 0.0f, 1.0f);
        b.size = parts.size() > 4 ? std::clamp(std::strtof(parts[4].c_str(), nullptr), 0.3f, 3.0f) : 1.0f;
        b.visible = parts.size() <= 5 || parts[5] != "0";
        layout.buttons.push_back(b);
    } else {
        return false;
    }
    return true;
}

Key keyFromName(const std::string& name) {
    const std::string n = lower(name);
    if (n.empty()) return Key::Unknown;
    if (n == "shift") return Key::LeftShift;
    if (n == "ctrl" || n == "control") return Key::LeftControl;
    if (n == "alt") return Key::LeftAlt;
    if (n == "esc") return Key::Escape;
    if (n == "return") return Key::Enter;
    for (int i = 1; i < static_cast<int>(Key::Count); ++i) {
        const Key k = static_cast<Key>(i);
        const char* label = keyName(k);
        if (std::string(label) != "Unknown" && lower(label) == n) return k;
    }
    return Key::Unknown;
}

void TouchControls::setLayout(const TouchLayout& layout) { layout_ = layout; }

void TouchControls::placeButton(const TouchButton& button) {
    for (TouchButton& b : layout_.buttons) {
        if (lower(b.label) != lower(button.label)) continue;
        const bool visible = b.visible;
        const std::string text = b.text;
        const std::string old_action = b.action;
        const std::string icon = b.icon;
        b = button;
        b.visible = visible;  // lo decide setButtonVisible
        if (b.text.empty()) b.text = text;
        if (b.icon.empty()) b.icon = icon;
        if (b.action.empty()) b.action = old_action;
        return;
    }
    layout_.buttons.push_back(button);
}

bool TouchControls::setButtonText(const std::string& label, const std::string& text) {
    bool found = false;
    for (TouchButton& b : layout_.buttons) {
        if (lower(b.label) == lower(label)) {
            b.text = text;
            found = true;
        }
    }
    return found;
}

bool TouchControls::setButtonIcon(const std::string& label, const std::string& icon) {
    bool found = false;
    for (TouchButton& b : layout_.buttons) {
        if (lower(b.label) == lower(label)) {
            b.icon = icon;
            found = true;
        }
    }
    return found;
}

bool TouchControls::setButtonState(const std::string& label, float cooldown, const std::string& badge, bool dim,
                                   std::uint32_t ring) {
    bool found = false;
    for (TouchButton& b : layout_.buttons) {
        if (lower(b.label) == lower(label)) {
            b.cooldown = std::clamp(cooldown, 0.0f, 1.0f);
            b.badge = badge;
            b.dim = dim;
            b.ring = ring;
            found = true;
        }
    }
    return found;
}

void TouchControls::setStick(float radius_dp, const std::string& sprint_action) {
    layout_.stick_radius_dp = std::clamp(radius_dp, 0.0f, 200.0f);
    layout_.stick_sprint_action = sprint_action;
}

void TouchControls::setLookSensitivity(float sensitivity) {
    layout_.look_sensitivity = std::clamp(sensitivity, 0.05f, 10.0f);
}

void TouchControls::setEnabled(bool enabled) { layout_.enabled = enabled; }
void TouchControls::setJoystick(bool enabled) { layout_.joystick = enabled; }
void TouchControls::setLook(bool enabled) { layout_.look = enabled; }

bool TouchControls::setButtonVisible(const std::string& label, bool visible) {
    bool found = false;
    for (TouchButton& b : layout_.buttons) {
        if (lower(b.label) == lower(label)) {
            b.visible = visible;
            found = true;
        }
    }
    return found;
}

bool TouchControls::fingerStillValid(const Finger& f) const {
    // Al abrir o cerrar el editor se sueltan los dedos del otro modo.
    const bool edit_role = f.role == Role::Edit || f.role == Role::EditPinch;
    if (edit_role != edit_) return false;
    switch (f.role) {
        case Role::Edit:
        case Role::EditPinch: return true;
        case Role::Stick: return layout_.enabled && layout_.joystick;
        case Role::Look: return layout_.enabled && layout_.look;
        case Role::Button: return layout_.enabled && f.button < layout_.buttons.size() && layout_.buttons[f.button].visible;
        case Role::Mouse: return true;  // el raton (o un dedo sobre la interfaz) sigue hasta que se levante
    }
    return true;
}

void TouchControls::update(std::vector<Event>& out) {
    for (std::size_t i = 0; i < fingers_.size();) {
        if (fingerStillValid(fingers_[i])) {
            ++i;
            continue;
        }
        Event end;
        end.type = EventType::TouchEnded;
        end.touchId = fingers_[i].id;
        end.mouseX = fingers_[i].x;
        end.mouseY = fingers_[i].y;
        const std::size_t before = fingers_.size();
        onEvent(end, 1e9, out);  // (tiempo enorme: no cuenta como toque corto)
        if (fingers_.size() == before) fingers_.erase(fingers_.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

void TouchControls::setScreen(float width, float height, float density) {
    width_ = std::max(width, 1.0f);
    height_ = std::max(height, 1.0f);
    density_ = std::max(density, 0.25f);
}

float TouchControls::unit() const {
    // Alto en dp: ~360-430 en un movil apaisado, ~530-800 en una tableta. Los
    // controles colocados en dp se disenan para ~400 dp; un poco mas grandes en
    // tableta (hasta +15 %) y un poco mas pequenos en moviles muy bajos.
    const float height_dp = height_ / density_;
    const float fit = std::clamp(height_dp / 400.0f, 0.85f, 1.15f);
    return density_ * layout_.scale * fit;
}

float TouchControls::stickRadius() const {
    if (layout_.stick_radius_dp > 0.0f) return layout_.stick_radius_dp * unit();
    return 64.0f * density_ * layout_.scale;
}

float TouchControls::buttonRadius(const TouchButton& button) const {
    if (button.anchored && button.radius_dp > 0.0f) return button.radius_dp * unit();
    return 34.0f * density_ * layout_.scale * button.size;
}

float TouchControls::buttonCenterX(const TouchButton& button) const {
    if (!button.anchored) return button.x * width_;
    const float r = buttonRadius(button);
    const float x = button.anchor_x * width_ + button.offset_x * unit();
    return std::clamp(x, std::min(r, width_ * 0.5f), std::max(width_ - r, width_ * 0.5f));
}

float TouchControls::buttonCenterY(const TouchButton& button) const {
    if (!button.anchored) return button.y * height_;
    const float r = buttonRadius(button);
    const float y = button.anchor_y * height_ + button.offset_y * unit();
    return std::clamp(y, std::min(r, height_ * 0.5f), std::max(height_ - r, height_ * 0.5f));
}

TouchControls::StickView TouchControls::stick() const {
    StickView view;
    view.radius = stickRadius();
    view.active = false;
    for (const Finger& f : fingers_) {
        if (f.role == Role::Stick) view.active = true;
    }
    if (view.active) {
        view.base_x = stick_base_x_;
        view.base_y = stick_base_y_;
    } else if (layout_.stick_radius_dp > 0.0f && layout_.stick_rest_x > 0.0f) {
        // Stick flotante colocado en el editor.
        view.base_x = layout_.stick_rest_x * unit();
        view.base_y = height_ - layout_.stick_rest_y * unit();
    } else if (layout_.stick_radius_dp > 0.0f) {
        // Stick flotante: en reposo solo una pista abajo a la izquierda.
        view.base_x = 26.0f * unit() + view.radius * 1.35f;
        view.base_y = height_ - 34.0f * unit() - view.radius * 1.5f;
    } else {
        // En reposo, abajo a la izquierda (donde suele ir el pulgar).
        view.base_x = std::max(width_ * 0.14f, view.radius * 1.6f);
        view.base_y = height_ - std::max(height_ * 0.24f, view.radius * 1.6f);
    }
    view.knob_x = view.base_x + stick_x_ * view.radius;
    view.knob_y = view.base_y - stick_y_ * view.radius;
    view.sprint = sprint_;
    view.can_sprint = !layout_.stick_sprint_action.empty();
    view.sprint_radius = view.radius * kSprintOn;
    return view;
}

bool TouchControls::buttonDown(std::size_t index) const {
    for (const Finger& f : fingers_) {
        if (f.role == Role::Button && f.button == index) return true;
    }
    return false;
}

TouchControls::Finger* TouchControls::find(std::int32_t id) {
    for (Finger& f : fingers_) {
        if (f.id == id) return &f;
    }
    return nullptr;
}

void TouchControls::pressAction(const std::string& action, bool down, std::vector<Event>& out) const {
    const std::string n = lower(action);
    Event e;
    e.touchId = kTouchButtonEventId;
    if (n.rfind("mouse", 0) == 0) {
        const int index = std::clamp(std::atoi(n.c_str() + 5), 0, 4);
        e.type = down ? EventType::MouseButtonPressed : EventType::MouseButtonReleased;
        e.category = EventCategory::Input | EventCategory::Mouse;
        e.button = static_cast<MouseButton>(index);
        out.push_back(e);
        return;
    }
    const Key key = keyFromName(action);
    if (key == Key::Unknown) return;
    e.type = down ? EventType::KeyPressed : EventType::KeyReleased;
    e.category = EventCategory::Input | EventCategory::Keyboard;
    e.key = key;
    out.push_back(e);
}

void TouchControls::setSprint(bool on, std::vector<Event>& out) {
    if (sprint_ == on) return;
    sprint_ = on;
    if (!layout_.stick_sprint_action.empty()) pressAction(layout_.stick_sprint_action, on, out);
}

void TouchControls::updateStick(const Finger& finger, std::vector<Event>& out) {
    const float r = stickRadius();
    float dx = (finger.x - stick_base_x_) / r;
    float dy = (stick_base_y_ - finger.y) / r;
    float length = std::sqrt(dx * dx + dy * dy);
    if (length > kStickFollow) {
        // El stick sigue al dedo si se pasa mucho (no hay que volver).
        const float over = (length - kStickFollow) * r;
        stick_base_x_ += dx / length * over;
        stick_base_y_ -= dy / length * over;
        dx = dx / length * kStickFollow;
        dy = dy / length * kStickFollow;
        length = kStickFollow;
    }
    // Zona muerta pequena para que el pulgar en reposo no camine; despues
    // crece desde 0 (sin salto al salir de la zona muerta).
    if (length < kStickDead) {
        stick_x_ = stick_y_ = 0.0f;
    } else {
        const float mag = std::min((length - kStickDead) / (1.0f - kStickDead), 1.0f);
        stick_x_ = dx / length * mag;
        stick_y_ = dy / length * mag;
    }
    if (!layout_.stick_sprint_action.empty()) setSprint(sprint_ ? length > kSprintOff : length > kSprintOn, out);
}

bool TouchControls::lookBusy(const Finger* except) const {
    for (const Finger& o : fingers_) {
        if (&o != except && o.looking) return true;
    }
    return false;
}

void TouchControls::emitLook(float dx, float dy, std::vector<Event>& out) const {
    // Como el raton capturado: solo el delta (en pixeles de un monitor de
    // ~96 dpi, para que la sensibilidad se parezca).
    Event raw;
    raw.type = EventType::MouseRawMoved;
    raw.category = EventCategory::Input | EventCategory::Mouse;
    const float k = layout_.look_sensitivity / density_ * 1.6f;
    raw.deltaX = dx * k;
    raw.deltaY = dy * k;
    raw.touchId = kTouchButtonEventId;
    out.push_back(raw);
}

// ---------------------------------------------------------------- editor

void TouchControls::setEditMode(bool on) {
    if (edit_ == on) return;
    edit_ = on;
    edit_selected_ = -1;
}

std::string TouchControls::takeEditEvent() {
    std::string e;
    e.swap(edit_event_);
    return e;
}

std::vector<TouchControls::EditItem> TouchControls::editPanel() const {
    std::vector<EditItem> items;
    const float u = unit();
    const float total = (4.0f * kPanelItemW + 3.0f * kPanelGap) * u;
    float x = width_ * 0.5f - total * 0.5f;
    for (const char* label : kPanelLabels) {
        EditItem it;
        it.label = label;
        it.x0 = x;
        it.y0 = kPanelTop * u;
        it.x1 = x + kPanelItemW * u;
        it.y1 = (kPanelTop + kPanelItemH) * u;
        items.push_back(it);
        x += (kPanelItemW + kPanelGap) * u;
    }
    return items;
}

int TouchControls::editHit(float x, float y) const {
    int best_item = -1;
    float best = 1e30f;
    for (std::size_t i = 0; i < layout_.buttons.size(); ++i) {
        const TouchButton& b = layout_.buttons[i];
        if (!b.visible || !b.anchored) continue;
        const float d = std::hypot(x - buttonCenterX(b), y - buttonCenterY(b));
        if (d <= buttonRadius(b) * 1.25f && d < best) {
            best = d;
            best_item = static_cast<int>(i);
        }
    }
    if (best_item < 0 && layout_.joystick && layout_.stick_radius_dp > 0.0f) {
        const StickView s = stick();
        if (std::hypot(x - s.base_x, y - s.base_y) <= s.radius) best_item = kEditStick;
    }
    return best_item;
}

void TouchControls::editMoveTo(int item, float cx, float cy) {
    const float u = unit();
    if (item == kEditStick) {
        // El stick sigue apareciendo donde se apoya el pulgar (mitad izquierda);
        // esto es la pista en reposo y su tamano.
        const float r = stickRadius();
        cx = std::clamp(cx, r * 0.5f, std::max(width_ * 0.45f, r));
        cy = std::clamp(cy, std::max(height_ * 0.22f, r * 0.5f), height_ - r * 0.5f);
        layout_.stick_rest_x = std::max(cx / u, 1.0f);
        layout_.stick_rest_y = (height_ - cy) / u;
        return;
    }
    if (item < 0 || static_cast<std::size_t>(item) >= layout_.buttons.size()) return;
    TouchButton& b = layout_.buttons[static_cast<std::size_t>(item)];
    const float r = buttonRadius(b);
    cx = std::clamp(cx, r, std::max(width_ - r, r));
    cy = std::clamp(cy, r, std::max(height_ - r, r));
    // Anclado a la esquina mas cercana: en otra pantalla queda a la misma
    // distancia (en dp) de ese borde.
    b.anchor_x = cx < width_ * 0.5f ? 0.0f : 1.0f;
    b.anchor_y = cy < height_ * 0.5f ? 0.0f : 1.0f;
    b.offset_x = (cx - b.anchor_x * width_) / u;
    b.offset_y = (cy - b.anchor_y * height_) / u;
}

float TouchControls::editRadiusDp(int item) const {
    if (item == kEditStick) return layout_.stick_radius_dp;
    if (item < 0 || static_cast<std::size_t>(item) >= layout_.buttons.size()) return 0.0f;
    return layout_.buttons[static_cast<std::size_t>(item)].radius_dp;
}

void TouchControls::editSetRadiusDp(int item, float r) {
    if (item == kEditStick) {
        layout_.stick_radius_dp = std::clamp(r, 35.0f, 110.0f);
        return;
    }
    if (item < 0 || static_cast<std::size_t>(item) >= layout_.buttons.size()) return;
    TouchButton& b = layout_.buttons[static_cast<std::size_t>(item)];
    b.radius_dp = std::clamp(r, 14.0f, 90.0f);
    editMoveTo(item, buttonCenterX(b), buttonCenterY(b));  // que no se salga de la pantalla al crecer
}

void TouchControls::onEditEvent(const Event& event) {
    // Finger::button guarda lo que arrastra el dedo: el indice + 2 (0 = el
    // stick, 1 = nada).
    const auto item_of = [](const Finger& f) { return static_cast<int>(f.button) - 2; };
    switch (event.type) {
        case EventType::TouchBegan: {
            Finger f;
            f.id = event.touchId;
            f.x = f.start_x = event.mouseX;
            f.y = f.start_y = event.mouseY;
            f.role = Role::Edit;
            f.button = 1;
            // La barra de arriba.
            for (const EditItem& it : editPanel()) {
                if (f.x < it.x0 || f.x > it.x1 || f.y < it.y0 || f.y > it.y1) continue;
                if (it.label == "Done") {
                    edit_event_ = "done";
                    setEditMode(false);
                    return;
                }
                if (it.label == "Reset") {
                    edit_event_ = "reset";
                    layout_.stick_rest_x = layout_.stick_rest_y = 0.0f;
                    edit_selected_ = -1;
                } else if (edit_selected_ != -1) {
                    const float k = it.label == "+" ? 1.12f : 1.0f / 1.12f;
                    editSetRadiusDp(edit_selected_, editRadiusDp(edit_selected_) * k);
                }
                fingers_.push_back(f);
                return;
            }
            // Segundo dedo con uno arrastrando lo elegido: pellizcar.
            for (const Finger& o : fingers_) {
                if (o.role == Role::Edit && edit_selected_ != -1 && item_of(o) == edit_selected_) {
                    f.role = Role::EditPinch;
                    pinch_start_dist_ = std::max(std::hypot(f.x - o.x, f.y - o.y), 1.0f);
                    pinch_start_radius_ = editRadiusDp(edit_selected_);
                    fingers_.push_back(f);
                    return;
                }
            }
            const int hit = editHit(f.x, f.y);
            edit_selected_ = hit;
            if (hit != -1) {
                float cx = 0.0f, cy = 0.0f;
                if (hit == kEditStick) {
                    const StickView s = stick();
                    cx = s.base_x;
                    cy = s.base_y;
                } else {
                    const TouchButton& b = layout_.buttons[static_cast<std::size_t>(hit)];
                    cx = buttonCenterX(b);
                    cy = buttonCenterY(b);
                }
                f.grab_x = cx - f.x;
                f.grab_y = cy - f.y;
                f.button = static_cast<std::size_t>(hit + 2);
            }
            fingers_.push_back(f);
            break;
        }
        case EventType::TouchMoved: {
            Finger* f = find(event.touchId);
            if (f == nullptr) return;
            f->x = event.mouseX;
            f->y = event.mouseY;
            if (f->role == Role::Edit && item_of(*f) != -1) {
                editMoveTo(item_of(*f), f->x + f->grab_x, f->y + f->grab_y);
            } else if (f->role == Role::EditPinch && edit_selected_ != -1) {
                for (const Finger& o : fingers_) {
                    if (o.role != Role::Edit || item_of(o) != edit_selected_) continue;
                    const float d = std::hypot(f->x - o.x, f->y - o.y);
                    editSetRadiusDp(edit_selected_, pinch_start_radius_ * d / pinch_start_dist_);
                }
            }
            break;
        }
        case EventType::TouchEnded: {
            Finger* f = find(event.touchId);
            if (f != nullptr) fingers_.erase(fingers_.begin() + (f - fingers_.data()));
            break;
        }
        default: break;
    }
}

std::string TouchControls::layoutString() const {
    std::ostringstream out;
    out << "v1;stick=" << layout_.stick_rest_x << "," << layout_.stick_rest_y << "," << layout_.stick_radius_dp << ";";
    for (const TouchButton& b : layout_.buttons) {
        if (!b.anchored) continue;
        out << "b=" << b.label << "," << b.anchor_x << "," << b.anchor_y << "," << b.offset_x << "," << b.offset_y << ","
            << b.radius_dp << ";";
    }
    return out.str();
}

void TouchControls::setLayoutString(const std::string& text) {
    std::stringstream in(text);
    std::string entry;
    while (std::getline(in, entry, ';')) {
        const std::size_t eq = entry.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = entry.substr(0, eq);
        std::vector<std::string> parts;
        std::stringstream values(entry.substr(eq + 1));
        std::string part;
        while (std::getline(values, part, ',')) parts.push_back(part);
        const auto num = [&](std::size_t i) {
            const float v = i < parts.size() ? std::strtof(parts[i].c_str(), nullptr) : 0.0f;
            return std::isfinite(v) ? v : 0.0f;
        };
        if (key == "stick" && parts.size() >= 3) {
            layout_.stick_rest_x = std::max(num(0), 0.0f);
            layout_.stick_rest_y = std::max(num(1), 0.0f);
            if (num(2) > 0.0f) layout_.stick_radius_dp = std::clamp(num(2), 35.0f, 110.0f);
        } else if (key == "b" && parts.size() >= 6) {
            for (TouchButton& b : layout_.buttons) {
                if (lower(b.label) != lower(parts[0]) || !b.anchored) continue;
                b.anchor_x = std::clamp(num(1), 0.0f, 1.0f);
                b.anchor_y = std::clamp(num(2), 0.0f, 1.0f);
                b.offset_x = num(3);
                b.offset_y = num(4);
                b.radius_dp = std::clamp(num(5), 14.0f, 90.0f);
            }
        }
    }
}

void TouchControls::onEvent(const Event& event, double time, std::vector<Event>& out) {
    // Editor: los dedos nuevos mueven botones; los de antes terminan como eran.
    if (event.type == EventType::TouchBegan && edit_) {
        onEditEvent(event);
        return;
    }
    if (event.type == EventType::TouchMoved || event.type == EventType::TouchEnded) {
        const Finger* f = find(event.touchId);
        if (f != nullptr && (f->role == Role::Edit || f->role == Role::EditPinch)) {
            onEditEvent(event);
            return;
        }
    }
    // Sin controles (layout_.enabled = false), el primer dedo es el raton.
    switch (event.type) {
        case EventType::TouchBegan: {
            Finger f;
            f.id = event.touchId;
            f.x = f.start_x = event.mouseX;
            f.y = f.start_y = event.mouseY;
            f.start_time = time;
            bool assigned = false;
            const bool on_ui = ui_hit_ && ui_hit_(f.x, f.y);
            if (layout_.enabled && !on_ui) {
                // 1) Un boton (el mas cercano que contenga el dedo, con margen).
                float best = 1e30f;
                for (std::size_t i = 0; i < layout_.buttons.size(); ++i) {
                    const TouchButton& b = layout_.buttons[i];
                    if (!b.visible) continue;
                    const float dx = f.x - buttonCenterX(b);
                    const float dy = f.y - buttonCenterY(b);
                    const float d = std::sqrt(dx * dx + dy * dy);
                    if (d <= buttonRadius(b) * 1.25f && d < best) {
                        best = d;
                        f.role = Role::Button;
                        f.button = i;
                        assigned = true;
                    }
                }
                if (assigned) {
                    pressAction(layout_.buttons[f.button].action, true, out);
                } else if (layout_.joystick && f.x < width_ * 0.45f &&
                           (layout_.stick_radius_dp <= 0.0f || f.y > height_ * 0.22f)) {
                    // 2) El stick: mitad izquierda (sin la franja de arriba si
                    // es flotante), uno solo a la vez. Aparece donde se apoya
                    // el dedo: sin recolocarlo, el primer toque no camina.
                    bool busy = false;
                    for (const Finger& other : fingers_) busy = busy || other.role == Role::Stick;
                    if (!busy) {
                        f.role = Role::Stick;
                        stick_base_x_ = f.x;
                        stick_base_y_ = f.y;
                        updateStick(f, out);
                        assigned = true;
                    }
                }
                if (!assigned && layout_.look) {
                    // 3) Mirar: un solo dedo gira la camara (otro mas no la
                    // acelera ni la hace saltar); si se levanta, el siguiente
                    // que se mueva toma el relevo.
                    f.role = Role::Look;
                    f.looking = !lookBusy(nullptr);
                    assigned = true;
                }
            }
            if (!assigned) {
                // 4) Raton: el primer dedo libre mueve el cursor y hace clic.
                bool busy = false;
                for (const Finger& other : fingers_) busy = busy || other.role == Role::Mouse;
                if (busy) return;
                f.role = Role::Mouse;
                Event move;
                move.type = EventType::MouseMoved;
                move.category = EventCategory::Input | EventCategory::Mouse;
                move.mouseX = f.x;
                move.mouseY = f.y;
                out.push_back(move);
                Event press;
                press.type = EventType::MouseButtonPressed;
                press.category = EventCategory::Input | EventCategory::Mouse;
                press.button = MouseButton::Left;
                press.mouseX = f.x;
                press.mouseY = f.y;
                out.push_back(press);
            }
            fingers_.push_back(f);
            break;
        }
        case EventType::TouchMoved: {
            Finger* f = find(event.touchId);
            if (f == nullptr) return;
            const float dx = event.mouseX - f->x;
            const float dy = event.mouseY - f->y;
            f->travel += std::sqrt(dx * dx + dy * dy);
            f->x = event.mouseX;
            f->y = event.mouseY;
            if (f->role == Role::Stick) {
                updateStick(*f, out);
            } else if (f->role == Role::Button && !f->dragged) {
                // Arrastrar desde un boton de accion (no los de menu) gira la
                // camara sin soltarlo: atacar o lanzar una habilidad mirando.
                const bool can = layout_.look && f->button < layout_.buttons.size() && layout_.buttons[f->button].style != 2;
                if (can && f->travel > kButtonDragDp * density_) {
                    f->dragged = true;
                    f->looking = !lookBusy(f);  // desde aqui, sin el recorrido de antes (sin salto)
                }
            } else if (f->role == Role::Look || f->role == Role::Button) {
                if (!f->looking) {
                    // El otro dedo se levanto: este sigue desde aqui (sin salto).
                    if (!lookBusy(f)) f->looking = true;
                    break;
                }
                emitLook(dx, dy, out);
            } else if (f->role == Role::Mouse) {
                Event move;
                move.type = EventType::MouseMoved;
                move.category = EventCategory::Input | EventCategory::Mouse;
                move.mouseX = f->x;
                move.mouseY = f->y;
                move.deltaX = dx;
                move.deltaY = dy;
                out.push_back(move);
            }
            break;
        }
        case EventType::TouchEnded: {
            Finger* f = find(event.touchId);
            if (f == nullptr) return;
            {
                const float dx = event.mouseX - f->x;
                const float dy = event.mouseY - f->y;
                f->travel += std::sqrt(dx * dx + dy * dy);
                f->x = event.mouseX;
                f->y = event.mouseY;
            }
            if (f->role == Role::Button) {
                // Otro dedo en el mismo boton lo mantiene pulsado.
                bool other = false;
                for (const Finger& o : fingers_) other = other || (&o != f && o.role == Role::Button && o.button == f->button);
                if (!other && f->button < layout_.buttons.size()) pressAction(layout_.buttons[f->button].action, false, out);
            } else if (f->role == Role::Stick) {
                stick_x_ = stick_y_ = 0.0f;
                setSprint(false, out);
            } else if (f->role == Role::Look) {
                const bool tap = time - f->start_time <= kTapSeconds && f->travel <= kTapTravelDp * density_;
                if (tap && layout_.tap_clicks) {
                    Event move;
                    move.type = EventType::MouseMoved;
                    move.category = EventCategory::Input | EventCategory::Mouse;
                    move.mouseX = f->x;
                    move.mouseY = f->y;
                    out.push_back(move);
                    Event click;
                    click.category = EventCategory::Input | EventCategory::Mouse;
                    click.button = MouseButton::Left;
                    click.mouseX = f->x;
                    click.mouseY = f->y;
                    click.type = EventType::MouseButtonPressed;
                    out.push_back(click);
                    click.type = EventType::MouseButtonReleased;
                    out.push_back(click);
                }
            } else if (f->role == Role::Mouse) {
                Event release;
                release.type = EventType::MouseButtonReleased;
                release.category = EventCategory::Input | EventCategory::Mouse;
                release.button = MouseButton::Left;
                release.mouseX = f->x;
                release.mouseY = f->y;
                out.push_back(release);
            }
            fingers_.erase(fingers_.begin() + (f - fingers_.data()));
            break;
        }
        default: break;
    }
}

void TouchControls::releaseAll(std::vector<Event>& out) {
    while (!fingers_.empty()) {
        Event end;
        end.type = EventType::TouchEnded;
        end.touchId = fingers_.back().id;
        end.mouseX = fingers_.back().x;
        end.mouseY = fingers_.back().y;
        const std::size_t before = fingers_.size();
        onEvent(end, 1e9, out);
        if (fingers_.size() == before) fingers_.pop_back();
    }
    stick_x_ = stick_y_ = 0.0f;
    setSprint(false, out);
}

}  // namespace cramion::dm
