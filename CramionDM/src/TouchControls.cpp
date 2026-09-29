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

}  // namespace

TouchLayout defaultTouchLayout() {
    TouchLayout layout;
    layout.buttons = {
        TouchButton{"Saltar", "Space", 0.90f, 0.78f, 1.15f},
        TouchButton{"Usar", "E", 0.80f, 0.86f, 0.85f},
        TouchButton{"Correr", "LeftShift", 0.92f, 0.56f, 0.85f},
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
    switch (f.role) {
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

float TouchControls::stickRadius() const { return 64.0f * density_ * layout_.scale; }

float TouchControls::buttonRadius(const TouchButton& button) const {
    return 34.0f * density_ * layout_.scale * button.size;
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
    } else {
        // En reposo, abajo a la izquierda (donde suele ir el pulgar).
        view.base_x = std::max(width_ * 0.14f, view.radius * 1.6f);
        view.base_y = height_ - std::max(height_ * 0.24f, view.radius * 1.6f);
    }
    view.knob_x = view.base_x + stick_x_ * view.radius;
    view.knob_y = view.base_y - stick_y_ * view.radius;
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

void TouchControls::updateStick(const Finger& finger) {
    const float r = stickRadius();
    float dx = (finger.x - stick_base_x_) / r;
    float dy = (stick_base_y_ - finger.y) / r;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length > 1.0f) {
        // El stick sigue al dedo si se pasa del borde (no hay que volver).
        const float over = (length - 1.0f) * r;
        stick_base_x_ += dx / length * over;
        stick_base_y_ -= dy / length * over;
        dx /= length;
        dy /= length;
    }
    // Zona muerta pequena para que el pulgar en reposo no camine.
    const float l = std::min(length, 1.0f);
    if (l < 0.12f) {
        stick_x_ = stick_y_ = 0.0f;
    } else {
        stick_x_ = dx;
        stick_y_ = dy;
    }
}

void TouchControls::onEvent(const Event& event, double time, std::vector<Event>& out) {
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
                } else if (layout_.joystick && f.x < width_ * 0.45f) {
                    // 2) El stick: mitad izquierda, uno solo a la vez.
                    bool busy = false;
                    for (const Finger& other : fingers_) busy = busy || other.role == Role::Stick;
                    if (!busy) {
                        f.role = Role::Stick;
                        const float r = stickRadius();
                        stick_base_x_ = std::clamp(f.x, r, width_ - r);
                        stick_base_y_ = std::clamp(f.y, r, height_ - r);
                        updateStick(f);
                        assigned = true;
                    }
                }
                if (!assigned && layout_.look) {
                    f.role = Role::Look;
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
                updateStick(*f);
            } else if (f->role == Role::Look) {
                // Como el raton capturado: solo el delta (en pixeles de un
                // monitor de ~96 dpi, para que la sensibilidad se parezca).
                Event raw;
                raw.type = EventType::MouseRawMoved;
                raw.category = EventCategory::Input | EventCategory::Mouse;
                const float k = layout_.look_sensitivity / density_ * 1.6f;
                raw.deltaX = dx * k;
                raw.deltaY = dy * k;
                out.push_back(raw);
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
}

}  // namespace cramion::dm
