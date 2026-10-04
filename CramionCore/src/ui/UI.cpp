#include "CramionCore/ui/UI.h"

#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/gameplay/DialogueUi.h"
#include "CramionCore/gameplay/Localization.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace cramion::ui {

using core::Vec2;
using core::Vec3;
using core::Vec4;
using ecs::FloatRange;
using ecs::Vec3Kind;

namespace {

void targetFields(ecs::PropertyVisitor& v, Uuid& target, std::string& method, const char* key, const char* label,
                  const char* tip) {
    v.entity({"target", "Objeto del script", "Quien recibe el evento (vacio = este objeto)"}, target);
    v.field({key, label, tip}, method);
}

Vec4 rgba(const Vec3& c, float a) { return Vec4{c.x, c.y, c.z, a}; }

}  // namespace

void Canvas::reflect(ecs::PropertyVisitor& v) {
    static constexpr std::array<const char*, 2> kRender = {"Pantalla", "Mundo (VR)"};
    ecs::enumField(v, {"render_mode", "Modo",
                       "Pantalla: encima de la vista. Mundo: un panel en la escena (VR: se usa con el rayo de los mandos)"},
                   render_mode, kRender);
    v.field({"reference", "Resolucion de referencia"}, reference, 1.0f);
    if (v.wantsAllFields() || render_mode == RenderMode::ScreenSpace) {
        static constexpr std::array<const char*, 2> kModes = {"Pixeles constantes", "Escalar con la pantalla"};
        ecs::enumField(v, {"scale_mode", "Escalado"}, scale_mode, kModes);
        v.field({"match", "Ajustar a", "0 = ancho, 1 = alto"}, match, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
    }
    if (v.wantsAllFields() || render_mode == RenderMode::WorldSpace) {
        v.field({"pixels_per_meter", "Pixeles por metro",
                 "Tamano del panel: resolucion / pixeles por metro (1000: 1920 px = 1,92 m)"},
                pixels_per_meter, FloatRange{50.0f, 10000.0f, 10.0f, "%.0f"});
    }
    v.field({"sort_order", "Orden"}, sort_order, -100, 100);
}

void RectTransform::reflect(ecs::PropertyVisitor& v) {
    v.field({"anchor_min", "Ancla min", "0..1 del padre (0,0 = arriba izquierda)"}, anchor_min, 0.01f);
    v.field({"anchor_max", "Ancla max"}, anchor_max, 0.01f);
    v.field({"pivot", "Pivote"}, pivot, 0.01f);
    v.field({"position", "Posicion"}, position, 1.0f);
    v.field({"size", "Tamano", "Con anclas separadas se suma al hueco entre ellas"}, size, 1.0f);
}

void Image::reflect(ecs::PropertyVisitor& v) {
    v.field({"color", "Color"}, color, Vec3Kind::Color);
    v.field({"alpha", "Opacidad"}, alpha, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
    v.field({"texture", "Imagen", "Archivo en Assets (vacio = color liso)"}, texture);
    v.field({"corner_radius", "Esquinas"}, corner_radius, FloatRange{0.0f, 200.0f, 0.5f, "%.1f"});
    v.field({"preserve_aspect", "Mantener proporcion"}, preserve_aspect);
}

void Text::reflect(ecs::PropertyVisitor& v) {
    v.field({"text", "Texto"}, text);
    v.field({"localization_key", "Clave de localizacion", "Clave de la tabla de idiomas (Proyecto > Localizacion). Vacia = el Texto tal cual"},
            localization_key);
    v.field({"font_size", "Tamano"}, font_size, FloatRange{4.0f, 400.0f, 0.5f, "%.0f"});
    v.field({"color", "Color"}, color, Vec3Kind::Color);
    v.field({"alpha", "Opacidad"}, alpha, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
    static constexpr std::array<const char*, 3> kH = {"Izquierda", "Centro", "Derecha"};
    static constexpr std::array<const char*, 3> kV = {"Arriba", "Medio", "Abajo"};
    ecs::enumField(v, {"h_align", "Horizontal"}, h_align, kH);
    ecs::enumField(v, {"v_align", "Vertical"}, v_align, kV);
    v.field({"wrap", "Ajustar lineas"}, wrap);
    v.field({"shadow", "Sombra"}, shadow);
}

void Button::reflect(ecs::PropertyVisitor& v) {
    v.field({"interactable", "Interactivo"}, interactable);
    v.field({"normal", "Normal"}, normal, Vec3Kind::Color);
    v.field({"hover", "Encima"}, hover, Vec3Kind::Color);
    v.field({"pressed", "Pulsado"}, pressed, Vec3Kind::Color);
    v.field({"disabled", "Desactivado"}, disabled, Vec3Kind::Color);
    v.field({"corner_radius", "Esquinas"}, corner_radius, FloatRange{0.0f, 200.0f, 0.5f, "%.1f"});
    targetFields(v, target, on_click, "on_click", "Al hacer clic", "Metodo del script: function X:OnJugar(boton)");
}

void Slider::reflect(ecs::PropertyVisitor& v) {
    v.field({"interactable", "Interactivo"}, interactable);
    v.field({"min", "Minimo"}, min, FloatRange{-100000.0f, 100000.0f, 0.1f, "%.2f"});
    v.field({"max", "Maximo"}, max, FloatRange{-100000.0f, 100000.0f, 0.1f, "%.2f"});
    v.field({"value", "Valor"}, value, FloatRange{-100000.0f, 100000.0f, 0.01f, "%.2f"});
    v.field({"whole_numbers", "Enteros"}, whole_numbers);
    v.field({"track", "Barra"}, track, Vec3Kind::Color);
    v.field({"fill", "Relleno"}, fill, Vec3Kind::Color);
    v.field({"handle", "Asa"}, handle, Vec3Kind::Color);
    targetFields(v, target, on_change, "on_change", "Al cambiar", "function X:OnVolumen(valor)");
}

void InputField::reflect(ecs::PropertyVisitor& v) {
    v.field({"interactable", "Interactivo"}, interactable);
    v.field({"text", "Texto"}, text);
    v.field({"placeholder", "Indicacion"}, placeholder);
    v.field({"font_size", "Tamano"}, font_size, FloatRange{4.0f, 200.0f, 0.5f, "%.0f"});
    v.field({"max_length", "Longitud maxima", "0 = sin limite"}, max_length, 0, 10000);
    v.field({"password", "Contrasena"}, password);
    v.field({"background", "Fondo"}, background, Vec3Kind::Color);
    v.field({"text_color", "Color del texto"}, text_color, Vec3Kind::Color);
    v.entity({"target", "Objeto del script"}, target);
    v.field({"on_change", "Al escribir", "function X:OnEscribe(texto)"}, on_change);
    v.field({"on_submit", "Al pulsar Enter", "function X:OnEnviar(texto)"}, on_submit);
}

void Toggle::reflect(ecs::PropertyVisitor& v) {
    v.field({"interactable", "Interactivo"}, interactable);
    v.field({"on", "Activada"}, on);
    v.field({"box", "Caja"}, box, Vec3Kind::Color);
    v.field({"check", "Marca"}, check, Vec3Kind::Color);
    targetFields(v, target, on_change, "on_change", "Al cambiar", "function X:OnCasilla(activa)");
}

UiRect layoutRect(const RectTransform& rt, const UiRect& parent) {
    UiRect r;
    const float ax0 = parent.x + parent.w * rt.anchor_min.x;
    const float ax1 = parent.x + parent.w * rt.anchor_max.x;
    const float ay0 = parent.y + parent.h * rt.anchor_min.y;
    const float ay1 = parent.y + parent.h * rt.anchor_max.y;
    r.w = std::max((ax1 - ax0) + rt.size.x, 0.0f);
    r.h = std::max((ay1 - ay0) + rt.size.y, 0.0f);
    // El pivote cae en el punto de anclaje (interpolado) mas la posicion.
    const float px = ax0 + (ax1 - ax0) * rt.pivot.x + rt.position.x;
    const float py = ay0 + (ay1 - ay0) * rt.pivot.y + rt.position.y;
    r.x = px - r.w * rt.pivot.x;
    r.y = py - r.h * rt.pivot.y;
    return r;
}

void UiSystem::reset() {
    laid_.clear();
    draw_.clear();
    events_.clear();
    pressed_ = dragging_ = focused_ = entt::null;
    capturing_mouse_ = false;
}

std::vector<UiEvent> UiSystem::takeEvents() {
    std::vector<UiEvent> out;
    out.swap(events_);
    return out;
}

bool UiSystem::rectOf(entt::entity entity, UiRect& rect) const {
    for (const Laid& l : laid_) {
        if (l.entity == entity) {
            rect = l.rect;
            return true;
        }
    }
    return false;
}

float UiSystem::scaleOf(entt::entity entity) const {
    for (const Laid& l : laid_) {
        if (l.entity == entity) return l.scale;
    }
    return 1.0f;
}

bool UiSystem::interactiveAt(const ecs::World& world, float x, float y) const {
    const entt::registry& r = world.registry();
    for (auto it = laid_.rbegin(); it != laid_.rend(); ++it) {
        if (!it->rect.contains(x, y) || !r.valid(it->entity)) continue;
        if (const Button* b = r.try_get<Button>(it->entity); b != nullptr && b->interactable) return true;
        if (const Slider* s = r.try_get<Slider>(it->entity); s != nullptr && s->interactable) return true;
        if (const Toggle* t = r.try_get<Toggle>(it->entity); t != nullptr && t->interactable) return true;
        if (const InputField* f = r.try_get<InputField>(it->entity); f != nullptr && f->interactable) return true;
    }
    return false;
}

entt::entity UiSystem::pick(float x, float y) const {
    for (auto it = laid_.rbegin(); it != laid_.rend(); ++it) {
        if (it->rect.contains(x, y)) return it->entity;
    }
    return entt::null;
}

void UiSystem::update(ecs::World& world, float width, float height, const UiInput& input, bool interactive, float time) {
    laid_.clear();
    draw_.clear();
    if (width <= 0.0f || height <= 0.0f) return;

    // Canvas raiz (sin Canvas por encima) de la pantalla, por orden.
    std::vector<ecs::Entity> canvases;
    world.forEachDepthFirst([&](ecs::Entity e) {
        if (!e.has<Canvas>() || !e.activeInHierarchy()) return;
        if (e.get<Canvas>().render_mode == RenderMode::WorldSpace) return;
        for (ecs::Entity p = e.parent(); p.valid(); p = p.parent()) {
            if (p.has<Canvas>()) return;
        }
        canvases.push_back(e);
    });
    std::stable_sort(canvases.begin(), canvases.end(),
                     [](const ecs::Entity& a, const ecs::Entity& b) { return a.get<Canvas>().sort_order < b.get<Canvas>().sort_order; });

    const auto target_of = [&](ecs::Entity source, const Uuid& target) { return targetOf(world, source, target); };

    for (const ecs::Entity& canvas_entity : canvases) {
        const Canvas& canvas = canvas_entity.get<Canvas>();
        float scale = 1.0f;
        if (canvas.scale_mode == ScaleMode::ScaleWithScreen && canvas.reference.x > 0.0f && canvas.reference.y > 0.0f) {
            // Media logaritmica entre ajustar al ancho y al alto (como uGUI).
            const float lw = std::log2(width / canvas.reference.x);
            const float lh = std::log2(height / canvas.reference.y);
            scale = std::pow(2.0f, lw + (lh - lw) * std::clamp(canvas.match, 0.0f, 1.0f));
        }
        const UiRect root{0.0f, 0.0f, width / scale, height / scale};
        layoutCanvas(world, canvas_entity, root, scale, laid_);
    }

    // --- Interaccion: el control mas arriba bajo el raton ---
    capturing_mouse_ = false;
    entt::entity hovered = entt::null;
    if (interactive) {
        for (auto it = laid_.rbegin(); it != laid_.rend(); ++it) {
            const ecs::Entity e = world.wrap(it->entity);
            const bool control = e.has<Button>() || e.has<Slider>() || e.has<InputField>() || e.has<Toggle>();
            if (it->rect.contains(input.mouse_x, input.mouse_y)) {
                capturing_mouse_ = capturing_mouse_ || control || e.has<Image>();
                if (control) {
                    hovered = it->entity;
                    break;
                }
            }
        }
        if (input.mouse_pressed) {
            pressed_ = hovered;
            if (hovered == entt::null || !world.wrap(hovered).has<InputField>()) focused_ = entt::null;
            if (hovered != entt::null) {
                ecs::Entity e = world.wrap(hovered);
                if (InputField* field = e.tryGet<InputField>(); field != nullptr && field->interactable) focused_ = hovered;
                if (Slider* slider = e.tryGet<Slider>(); slider != nullptr && slider->interactable) dragging_ = hovered;
            }
        }
        // Soltar sobre el mismo control: clic.
        if (input.mouse_released) {
            if (pressed_ != entt::null && pressed_ == hovered && world.registry().valid(pressed_)) {
                activate(world, world.wrap(pressed_));
            }
            pressed_ = entt::null;
            dragging_ = entt::null;
        }
        // Arrastrar un slider.
        if (dragging_ != entt::null && world.registry().valid(dragging_) && input.mouse_down) {
            UiRect r;
            if (rectOf(dragging_, r) && r.w > 1.0f) {
                setSlider(world, world.wrap(dragging_), std::clamp((input.mouse_x - r.x) / r.w, 0.0f, 1.0f));
            }
        }
        // Escribir en el campo con el foco.
        if (focused_ != entt::null && world.registry().valid(focused_)) {
            ecs::Entity e = world.wrap(focused_);
            if (InputField* field = e.tryGet<InputField>()) {
                bool changed = false;
                for (const char c : input.typed) {
                    if (static_cast<unsigned char>(c) < 32) continue;
                    if (field->max_length > 0 && static_cast<int>(field->text.size()) >= field->max_length) break;
                    field->text += c;
                    changed = true;
                }
                if (input.backspace && !field->text.empty()) {
                    // Borra un caracter UTF-8 entero.
                    std::size_t cut = field->text.size() - 1;
                    while (cut > 0 && (static_cast<unsigned char>(field->text[cut]) & 0xC0) == 0x80) --cut;
                    field->text.erase(cut);
                    changed = true;
                }
                if (changed && !field->on_change.empty()) {
                    UiEvent ev;
                    ev.source = e;
                    ev.target = target_of(e, field->target);
                    ev.method = field->on_change;
                    ev.kind = UiEvent::Kind::Text;
                    ev.text = field->text;
                    events_.push_back(ev);
                }
                if (input.enter) {
                    if (!field->on_submit.empty()) {
                        UiEvent ev;
                        ev.source = e;
                        ev.target = target_of(e, field->target);
                        ev.method = field->on_submit;
                        ev.kind = UiEvent::Kind::Text;
                        ev.text = field->text;
                        events_.push_back(ev);
                    }
                    focused_ = entt::null;
                }
            } else {
                focused_ = entt::null;
            }
        }
    } else {
        pressed_ = dragging_ = focused_ = entt::null;
    }

    DrawState state;
    state.hovered[0] = hovered;
    state.pressed[0] = pressed_;
    state.focused = focused_;
    buildDraw(world, laid_, state, input, interactive, time, draw_);
}

// Rectangulo de cada hijo en el de su padre (recorrido en profundidad).
void UiSystem::layoutCanvas(ecs::World& world, const ecs::Entity& canvas, const UiRect& root, float scale,
                            std::vector<Laid>& out) const {
    struct Item {
        entt::entity entity;
        UiRect parent;
    };
    std::vector<Item> stack;
    const auto& root_children = canvas.children();
    for (auto it = root_children.rbegin(); it != root_children.rend(); ++it) stack.push_back(Item{*it, root});
    while (!stack.empty()) {
        const Item item = stack.back();
        stack.pop_back();
        const ecs::Entity e = world.wrap(item.entity);
        if (!e.activeSelf()) continue;
        UiRect r = item.parent;
        if (const RectTransform* rt = e.tryGet<RectTransform>()) r = layoutRect(*rt, item.parent);
        out.push_back(Laid{item.entity, UiRect{r.x * scale, r.y * scale, r.w * scale, r.h * scale}, item.parent, scale});
        const auto& children = e.children();
        for (auto it = children.rbegin(); it != children.rend(); ++it) stack.push_back(Item{*it, r});
    }
}

ecs::Entity UiSystem::targetOf(ecs::World& world, ecs::Entity source, const Uuid& target) const {
    const ecs::Entity t = target.valid() ? world.find(target) : ecs::Entity{};
    return t.valid() ? t : source;
}

void UiSystem::activate(ecs::World& world, ecs::Entity e) {
    if (Button* button = e.tryGet<Button>(); button != nullptr && button->interactable && !button->on_click.empty()) {
        UiEvent ev;
        ev.source = e;
        ev.target = targetOf(world, e, button->target);
        ev.method = button->on_click;
        events_.push_back(ev);
    }
    if (Toggle* toggle = e.tryGet<Toggle>(); toggle != nullptr && toggle->interactable) {
        toggle->on = !toggle->on;
        if (!toggle->on_change.empty()) {
            UiEvent ev;
            ev.source = e;
            ev.target = targetOf(world, e, toggle->target);
            ev.method = toggle->on_change;
            ev.kind = UiEvent::Kind::Bool;
            ev.flag = toggle->on;
            events_.push_back(ev);
        }
    }
}

void UiSystem::setSlider(ecs::World& world, ecs::Entity e, float t) {
    Slider* slider = e.tryGet<Slider>();
    if (slider == nullptr || !slider->interactable) return;
    float value = slider->min + (slider->max - slider->min) * t;
    if (slider->whole_numbers) value = std::round(value);
    if (value == slider->value) return;
    slider->value = value;
    if (!slider->on_change.empty()) {
        UiEvent ev;
        ev.source = e;
        ev.target = targetOf(world, e, slider->target);
        ev.method = slider->on_change;
        ev.kind = UiEvent::Kind::Number;
        ev.number = value;
        events_.push_back(ev);
    }
}

void UiSystem::updateWorld(ecs::World& world, const std::array<UiPointer, kMaxPointers>& pointers, bool interactive,
                           float time) {
    world_canvases_.clear();
    for (UiPointerHit& h : pointer_hits_) h = UiPointerHit{};

    // Canvas raiz en modo Mundo.
    std::vector<ecs::Entity> canvases;
    world.forEachDepthFirst([&](ecs::Entity e) {
        if (!e.has<Canvas>() || !e.activeInHierarchy()) return;
        if (e.get<Canvas>().render_mode != RenderMode::WorldSpace) return;
        for (ecs::Entity p = e.parent(); p.valid(); p = p.parent()) {
            if (p.has<Canvas>()) return;
        }
        canvases.push_back(e);
    });
    std::stable_sort(canvases.begin(), canvases.end(),
                     [](const ecs::Entity& a, const ecs::Entity& b) { return a.get<Canvas>().sort_order < b.get<Canvas>().sort_order; });

    std::vector<std::vector<Laid>> laid(canvases.size());
    std::vector<core::Quat> rotations(canvases.size());
    for (std::size_t i = 0; i < canvases.size(); ++i) {
        const Canvas& canvas = canvases[i].get<Canvas>();
        WorldCanvasDraw d;
        d.canvas = canvases[i].handle();
        d.width = std::max(canvas.reference.x, 1.0f);
        d.height = std::max(canvas.reference.y, 1.0f);
        Vec3 position, scale;
        core::Quat rotation;
        ecs::decomposeMatrix(canvases[i].worldMatrix(), position, rotation, scale);
        rotations[i] = core::normalize(rotation);
        d.transform = core::composeTrs(position, rotations[i], Vec3{1.0f, 1.0f, 1.0f});
        const float ppm = std::max(canvas.pixels_per_meter, 1.0f);
        d.size = Vec2{d.width / ppm * std::abs(scale.x), d.height / ppm * std::abs(scale.y)};
        layoutCanvas(world, canvases[i], UiRect{0.0f, 0.0f, d.width, d.height}, 1.0f, laid[i]);
        world_canvases_.push_back(std::move(d));
    }

    // Cada rayo: el panel mas cercano que corta, en pixeles de ese panel.
    DrawState state;
    std::array<int, kMaxPointers> hit_canvas{-1, -1};
    std::array<Vec2, kMaxPointers> hit_pixel{};
    for (std::size_t p = 0; p < kMaxPointers; ++p) {
        const UiPointer& pointer = pointers[p];
        if (!pointer.valid) continue;
        const Vec3 direction = core::normalize(pointer.direction);
        float best = std::numeric_limits<float>::max();
        for (std::size_t i = 0; i < world_canvases_.size(); ++i) {
            const WorldCanvasDraw& d = world_canvases_[i];
            const Vec3 center{d.transform.m[3][0], d.transform.m[3][1], d.transform.m[3][2]};
            const Vec3 normal = ecs::quatRotate(rotations[i], Vec3{0.0f, 0.0f, 1.0f});
            const float denom = core::dot(direction, normal);
            if (std::abs(denom) < 1e-5f) continue;
            const float t = core::dot(center - pointer.origin, normal) / denom;
            if (t <= 0.0f || t >= best) continue;
            const Vec3 point = pointer.origin + direction * t;
            const Vec3 local = ecs::quatRotate(ecs::quatConjugate(rotations[i]), point - center);
            const float u = local.x / std::max(d.size.x, 1e-5f) + 0.5f;
            const float v = 0.5f - local.y / std::max(d.size.y, 1e-5f);
            if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) continue;
            best = t;
            hit_canvas[p] = static_cast<int>(i);
            hit_pixel[p] = Vec2{u * d.width, v * d.height};
            pointer_hits_[p] = UiPointerHit{true, t, point, d.canvas, false};
        }
        // El control mas arriba bajo el rayo.
        if (hit_canvas[p] >= 0) {
            const std::vector<Laid>& items = laid[static_cast<std::size_t>(hit_canvas[p])];
            for (auto it = items.rbegin(); it != items.rend(); ++it) {
                const ecs::Entity e = world.wrap(it->entity);
                const bool control = e.has<Button>() || e.has<Slider>() || e.has<InputField>() || e.has<Toggle>();
                if (control && it->rect.contains(hit_pixel[p].x, hit_pixel[p].y)) {
                    state.hovered[p] = it->entity;
                    pointer_hits_[p].over_control = true;
                    break;
                }
            }
        }
    }

    // Gatillo: pulsar y soltar sobre el mismo control es un clic; un Slider
    // se arrastra mientras se mantiene.
    for (std::size_t p = 0; p < kMaxPointers; ++p) {
        PointerState& ps = pointer_states_[p];
        const bool down = interactive && pointers[p].valid && pointers[p].down;
        if (!interactive) {
            ps = PointerState{};
            continue;
        }
        if (down && !ps.was_down) {
            ps.pressed = state.hovered[p];
            if (ps.pressed != entt::null && world.wrap(ps.pressed).has<Slider>()) ps.dragging = ps.pressed;
        }
        if (!down && ps.was_down) {
            if (ps.pressed != entt::null && ps.pressed == state.hovered[p] && world.registry().valid(ps.pressed)) {
                activate(world, world.wrap(ps.pressed));
            }
            ps.pressed = ps.dragging = entt::null;
        }
        if (down && ps.dragging != entt::null && world.registry().valid(ps.dragging) && hit_canvas[p] >= 0) {
            for (const Laid& l : laid[static_cast<std::size_t>(hit_canvas[p])]) {
                if (l.entity != ps.dragging || l.rect.w <= 1.0f) continue;
                setSlider(world, world.wrap(ps.dragging), std::clamp((hit_pixel[p].x - l.rect.x) / l.rect.w, 0.0f, 1.0f));
                break;
            }
        }
        ps.was_down = down;
        state.pressed[p] = ps.pressed;
    }

    const UiInput no_mouse;
    for (std::size_t i = 0; i < world_canvases_.size(); ++i) {
        buildDraw(world, laid[i], state, no_mouse, false, time, world_canvases_[i].commands);
    }
}

// Lista de dibujo (en el orden de la jerarquia: de atras a delante).
void UiSystem::buildDraw(ecs::World& world, const std::vector<Laid>& laid, const DrawState& state, const UiInput& input,
                         bool interactive, float time, std::vector<UiDrawCommand>& out) {
    const auto hovered_by_any = [&](entt::entity e) {
        return std::find(state.hovered.begin(), state.hovered.end(), e) != state.hovered.end();
    };
    const auto pressed_by_any = [&](entt::entity e) {
        for (std::size_t p = 0; p < kMaxPointers; ++p) {
            if (state.pressed[p] == e && state.hovered[p] == e) return true;
        }
        return false;
    };
    for (const Laid& l : laid) {
        const ecs::Entity e = world.wrap(l.entity);
        const UiRect& r = l.rect;
        const float s = l.scale;
        UiDrawCommand base;
        base.rect = r;
        base.entity = l.entity;
        if (const Button* button = e.tryGet<Button>()) {
            Vec3 color = button->normal;
            if (!button->interactable) {
                color = button->disabled;
            } else if (pressed_by_any(l.entity)) {
                color = button->pressed;
            } else if (hovered_by_any(l.entity)) {
                color = button->hover;
            }
            UiDrawCommand c = base;
            c.color = rgba(color, 1.0f);
            c.radius = button->corner_radius * s;
            out.push_back(c);
        }
        if (const Image* image = e.tryGet<Image>()) {
            UiDrawCommand c = base;
            c.type = image->texture.empty() ? UiDrawCommand::Type::Rect : UiDrawCommand::Type::Image;
            c.texture = image->texture;
            c.color = rgba(image->color, image->alpha);
            c.radius = image->corner_radius * s;
            c.preserve_aspect = image->preserve_aspect;
            // Con Button, la imagen se tine con el color del estado.
            if (e.has<Button>() && !out.empty() && out.back().entity == l.entity && out.back().type == UiDrawCommand::Type::Rect) {
                c.color = Vec4{c.color.x * out.back().color.x * 1.6f, c.color.y * out.back().color.y * 1.6f,
                               c.color.z * out.back().color.z * 1.6f, c.color.w};
                out.pop_back();
            }
            out.push_back(c);
        }
        if (const Slider* slider = e.tryGet<Slider>()) {
            const float t = slider->max != slider->min ? std::clamp((slider->value - slider->min) / (slider->max - slider->min), 0.0f, 1.0f) : 0.0f;
            const float bar_h = std::max(r.h * 0.28f, 3.0f);
            UiDrawCommand track = base;
            track.rect = UiRect{r.x, r.y + (r.h - bar_h) * 0.5f, r.w, bar_h};
            track.color = rgba(slider->track, 1.0f);
            track.radius = bar_h * 0.5f;
            out.push_back(track);
            UiDrawCommand fill = track;
            fill.rect.w = r.w * t;
            fill.color = rgba(slider->fill, 1.0f);
            out.push_back(fill);
            UiDrawCommand handle = base;
            handle.type = UiDrawCommand::Type::Circle;
            const float radius = r.h * 0.42f;
            handle.rect = UiRect{r.x + r.w * t - radius, r.y + r.h * 0.5f - radius, radius * 2.0f, radius * 2.0f};
            handle.color = rgba(slider->handle, slider->interactable ? 1.0f : 0.5f);
            out.push_back(handle);
        }
        if (const InputField* field = e.tryGet<InputField>()) {
            UiDrawCommand bg = base;
            bg.color = rgba(field->background, 1.0f);
            bg.radius = 6.0f * s;
            out.push_back(bg);
            const bool focused = state.focused == l.entity;
            if (focused) {
                UiDrawCommand outline = bg;
                outline.type = UiDrawCommand::Type::Line;
                outline.color = Vec4{0.25f, 0.55f, 1.0f, 1.0f};
                out.push_back(outline);
            }
            UiDrawCommand text = base;
            text.type = UiDrawCommand::Type::Text;
            text.rect = UiRect{r.x + 10.0f * s, r.y, r.w - 20.0f * s, r.h};
            text.font_size = field->font_size * s;
            text.h_align = 0;
            const bool empty = field->text.empty();
            text.text = empty ? field->placeholder : (field->password ? std::string(field->text.size(), '*') : field->text);
            if (focused && std::fmod(time, 1.0f) < 0.55f) text.text += "|";
            text.color = empty ? Vec4{0.55f, 0.55f, 0.6f, 1.0f} : rgba(field->text_color, 1.0f);
            if (empty && focused) text.text = std::fmod(time, 1.0f) < 0.55f ? "|" : "";
            out.push_back(text);
        }
        if (const Toggle* toggle = e.tryGet<Toggle>()) {
            const float side = std::min(r.w, r.h);
            UiDrawCommand box = base;
            box.rect = UiRect{r.x, r.y + (r.h - side) * 0.5f, side, side};
            box.color = rgba(toggle->box, 1.0f);
            box.radius = side * 0.2f;
            out.push_back(box);
            if (toggle->on) {
                UiDrawCommand mark = box;
                const float inset = side * 0.22f;
                mark.rect = UiRect{box.rect.x + inset, box.rect.y + inset, side - inset * 2.0f, side - inset * 2.0f};
                mark.color = rgba(toggle->check, 1.0f);
                mark.radius = side * 0.12f;
                out.push_back(mark);
            }
        }
        if (const Text* text = e.tryGet<Text>()) {
            UiDrawCommand c = base;
            c.type = UiDrawCommand::Type::Text;
            c.text = text->localization_key.empty() ? text->text : gameplay::localization().get(text->localization_key);
            c.font_size = text->font_size * s;
            c.color = rgba(text->color, text->alpha);
            c.h_align = static_cast<int>(text->h_align);
            c.v_align = static_cast<int>(text->v_align);
            c.wrap = text->wrap;
            c.shadow = text->shadow;
            out.push_back(c);
        }
        // Caja de dialogo (gameplay/DialogueUi.cpp): el dialogo que corre.
        if (const gameplay::DialogueBox* box = e.tryGet<gameplay::DialogueBox>()) {
            if (gameplay::updateDialogueBox(*box, r, s, input, interactive, l.entity, out)) capturing_mouse_ = true;
        }
    }
}

void registerUiComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("UICanvas") != nullptr) return;
    registry.registerComponent<Canvas>("UICanvas", "Canvas", "UI");
    registry.registerComponent<RectTransform>("RectTransform", "Rect Transform", "UI");
    registry.registerComponent<Image>("UIImage", "Imagen", "UI");
    registry.registerComponent<Text>("UIText", "Texto", "UI");
    registry.registerComponent<Button>("UIButton", "Boton", "UI");
    registry.registerComponent<Slider>("UISlider", "Slider", "UI");
    registry.registerComponent<InputField>("UIInputField", "Campo de texto", "UI");
    registry.registerComponent<Toggle>("UIToggle", "Casilla", "UI");
}

}  // namespace cramion::ui
