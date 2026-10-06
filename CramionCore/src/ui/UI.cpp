#include "CramionCore/ui/UI.h"

#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/gameplay/Accessibility.h"
#include "CramionCore/gameplay/DialogueUi.h"
#include "CramionCore/gameplay/Localization.h"

#include <algorithm>
#include <cctype>
#include <string>
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
    v.field({"rich_text", "Texto enriquecido", "<b> <i> <u> <s> <color=#ff8800> <size=40> <br>"}, rich_text);
    v.field({"bold", "Negrita"}, bold);
    v.field({"italic", "Cursiva"}, italic);
    v.field({"font", "Fuente", "Archivo .ttf u .otf de Assets (vacio = la del motor)"}, font);
    v.field({"outline", "Contorno"}, outline, FloatRange{0.0f, 20.0f, 0.1f, "%.1f"});
    v.field({"outline_color", "Color del contorno"}, outline_color, Vec3Kind::Color);
    v.field({"line_spacing", "Interlineado"}, line_spacing, FloatRange{0.5f, 3.0f, 0.01f, "%.2f"});
}

void Dropdown::reflect(ecs::PropertyVisitor& v) {
    v.field({"interactable", "Interactivo"}, interactable);
    ecs::listField(v, {"options", "Opciones"}, options,
                   [](std::string& o, ecs::PropertyVisitor& iv) { iv.field({"text", "Texto"}, o); });
    v.field({"value", "Elegida", "Indice de la opcion (0 = la primera)"}, value, 0,
            std::max(0, static_cast<int>(options.size()) - 1));
    v.field({"font_size", "Tamano del texto"}, font_size, FloatRange{4.0f, 200.0f, 0.5f, "%.0f"});
    v.field({"background", "Fondo"}, background, Vec3Kind::Color);
    v.field({"text_color", "Color del texto"}, text_color, Vec3Kind::Color);
    v.field({"highlight", "Resaltado"}, highlight, Vec3Kind::Color);
    v.field({"corner_radius", "Esquinas"}, corner_radius, FloatRange{0.0f, 100.0f, 0.5f, "%.1f"});
    v.field({"max_visible", "Opciones visibles"}, max_visible, 1, 50);
    targetFields(v, target, on_change, "on_change", "Al cambiar", "function X:OnCalidad(indice) (desde 1; el texto en self.entity.dropdownText)");
}

void ScrollView::reflect(ecs::PropertyVisitor& v) {
    v.field({"vertical", "Vertical"}, vertical);
    v.field({"horizontal", "Horizontal"}, horizontal);
    v.field({"scroll", "Desplazamiento"}, scroll, 1.0f);
    v.field({"wheel_speed", "Velocidad de la rueda"}, wheel_speed, FloatRange{1.0f, 1000.0f, 1.0f, "%.0f"});
    v.field({"inertia", "Inercia"}, inertia);
    v.field({"deceleration", "Frenado"}, deceleration, FloatRange{0.1f, 50.0f, 0.1f, "%.1f"});
    v.field({"show_scrollbar", "Barra de desplazamiento"}, show_scrollbar);
    v.field({"background", "Fondo"}, background, Vec3Kind::Color);
    v.field({"background_alpha", "Opacidad del fondo"}, background_alpha, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
    v.field({"scrollbar_color", "Color de la barra"}, scrollbar_color, Vec3Kind::Color);
}

void LayoutGroup::reflect(ecs::PropertyVisitor& v) {
    static constexpr std::array<const char*, 3> kTypes = {"Columna (vertical)", "Fila (horizontal)", "Rejilla"};
    static constexpr std::array<const char*, 3> kH = {"Izquierda", "Centro", "Derecha"};
    static constexpr std::array<const char*, 3> kV = {"Arriba", "Medio", "Abajo"};
    ecs::enumField(v, {"type", "Tipo"}, type, kTypes);
    v.field({"spacing", "Separacion"}, spacing, 1.0f);
    v.field({"padding", "Margen", "x = izquierda y derecha, y = arriba y abajo"}, padding, 1.0f);
    ecs::enumField(v, {"child_h", "Alinear (horizontal)"}, child_h, kH);
    ecs::enumField(v, {"child_v", "Alinear (vertical)"}, child_v, kV);
    v.field({"expand_width", "Ocupar el ancho", "Columna: los hijos ocupan todo el ancho; fila: todo el alto"},
            expand_width);
    v.field({"expand_height", "Repartir el sobrante", "Los hijos se estiran para llenar el espacio a lo largo"},
            expand_height);
    v.field({"cell_size", "Celda (rejilla)"}, cell_size, 1.0f);
    v.field({"columns", "Columnas (rejilla)", "0 = las que quepan"}, columns, 0, 64);
    v.field({"fit_content", "Ajustar al contenido", "El rectangulo crece con los hijos (Content Size Fitter)"},
            fit_content);
}

void Mask::reflect(ecs::PropertyVisitor& v) { v.field({"enabled", "Recortar a los hijos"}, enabled); }

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
    open_dropdown_ = scroll_drag_ = entt::null;
    last_time_ = -1.0f;
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
        if (!visibleAt(*it, x, y) || !r.valid(it->entity)) continue;
        if (const Button* b = r.try_get<Button>(it->entity); b != nullptr && b->interactable) return true;
        if (const Slider* s = r.try_get<Slider>(it->entity); s != nullptr && s->interactable) return true;
        if (const Toggle* t = r.try_get<Toggle>(it->entity); t != nullptr && t->interactable) return true;
        if (const InputField* f = r.try_get<InputField>(it->entity); f != nullptr && f->interactable) return true;
        if (const Dropdown* d = r.try_get<Dropdown>(it->entity); d != nullptr && d->interactable) return true;
        if (r.all_of<ScrollView>(it->entity)) return true;
    }
    return open_dropdown_ != entt::null;
}

entt::entity UiSystem::pick(float x, float y) const {
    for (auto it = laid_.rbegin(); it != laid_.rend(); ++it) {
        if (visibleAt(*it, x, y)) return it->entity;
    }
    return entt::null;
}

void UiSystem::update(ecs::World& world, float width, float height, const UiInput& input, bool interactive, float time) {
    laid_.clear();
    draw_.clear();
    if (width <= 0.0f || height <= 0.0f) return;
    view_height_ = height;
    const float dt = last_time_ < 0.0f ? 0.0f : std::clamp(time - last_time_, 0.0f, 0.1f);
    last_time_ = time;

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
        // Lista abierta de un Dropdown: va por encima de todo.
        bool consumed = false;
        if (open_dropdown_ != entt::null) {
            Dropdown* d = world.registry().valid(open_dropdown_) ? world.registry().try_get<Dropdown>(open_dropdown_) : nullptr;
            UiRect box;
            if (d == nullptr || !d->open || !rectOf(open_dropdown_, box)) {
                if (d != nullptr) d->open = false;
                open_dropdown_ = entt::null;
            } else {
                const int count = static_cast<int>(d->options.size());
                const int visible = std::clamp(d->max_visible, 1, std::max(count, 1));
                const UiRect list{box.x, dropdownItemRect(box, *d, 0).y + d->list_scroll * box.h, box.w,
                                  box.h * static_cast<float>(visible)};
                const bool over_list = list.contains(input.mouse_x, input.mouse_y);
                if (over_list) {
                    capturing_mouse_ = true;
                    if (input.wheel != 0.0f) {
                        d->list_scroll = std::clamp(d->list_scroll - input.wheel, 0.0f,
                                                    static_cast<float>(std::max(count - visible, 0)));
                    }
                }
                if (input.mouse_pressed) {
                    consumed = true;
                    if (over_list) {
                        const int index = static_cast<int>(std::floor((input.mouse_y - list.y) / std::max(box.h, 1.0f) + d->list_scroll));
                        if (index >= 0 && index < count) {
                            d->value = index;
                            if (!d->on_change.empty()) {
                                ecs::Entity src = world.wrap(open_dropdown_);
                                UiEvent ev;
                                ev.source = src;
                                ev.target = targetOf(world, src, d->target);
                                ev.method = d->on_change;
                                ev.kind = UiEvent::Kind::Number;
                                ev.number = static_cast<float>(index + 1);
                                ev.text = d->options[static_cast<std::size_t>(index)];
                                events_.push_back(ev);
                            }
                        }
                    }
                    d->open = false;
                    open_dropdown_ = entt::null;
                    pressed_ = entt::null;
                }
            }
        }
        for (auto it = laid_.rbegin(); it != laid_.rend() && !consumed; ++it) {
            const ecs::Entity e = world.wrap(it->entity);
            const bool control = e.has<Button>() || e.has<Slider>() || e.has<InputField>() || e.has<Toggle>() ||
                                 e.has<Dropdown>();
            if (visibleAt(*it, input.mouse_x, input.mouse_y)) {
                capturing_mouse_ = capturing_mouse_ || control || e.has<Image>() || e.has<ScrollView>();
                if (control) {
                    hovered = it->entity;
                    break;
                }
            }
        }
        // ScrollView bajo el raton: rueda y arrastre (con inercia al soltar).
        ScrollView* wheel_target = nullptr;
        entt::entity scroll_under = entt::null;
        for (auto it = laid_.rbegin(); it != laid_.rend(); ++it) {
            if (!visibleAt(*it, input.mouse_x, input.mouse_y)) continue;
            if (ScrollView* sv = world.registry().try_get<ScrollView>(it->entity)) {
                wheel_target = sv;
                scroll_under = it->entity;
                break;
            }
        }
        if (wheel_target != nullptr && input.wheel != 0.0f && open_dropdown_ == entt::null) {
            if (wheel_target->vertical) wheel_target->scroll.y -= input.wheel * wheel_target->wheel_speed;
            else if (wheel_target->horizontal) wheel_target->scroll.x -= input.wheel * wheel_target->wheel_speed;
            wheel_target->velocity = Vec2{};
        }
        if (input.mouse_pressed && !consumed && scroll_under != entt::null &&
            (hovered == entt::null || !world.wrap(hovered).has<Slider>())) {
            scroll_drag_ = scroll_under;
            scroll_drag_last_ = Vec2{input.mouse_x, input.mouse_y};
        }
        if (scroll_drag_ != entt::null) {
            ScrollView* sv = world.registry().valid(scroll_drag_) ? world.registry().try_get<ScrollView>(scroll_drag_) : nullptr;
            const float sc = std::max(scaleOf(scroll_drag_), 1e-3f);
            if (sv == nullptr || !input.mouse_down) {
                scroll_drag_ = entt::null;
            } else {
                const Vec2 delta{(input.mouse_x - scroll_drag_last_.x) / sc, (input.mouse_y - scroll_drag_last_.y) / sc};
                scroll_drag_last_ = Vec2{input.mouse_x, input.mouse_y};
                if (sv->horizontal) sv->scroll.x -= delta.x;
                if (sv->vertical) sv->scroll.y -= delta.y;
                if (dt > 0.0f) {
                    sv->velocity = Vec2{sv->horizontal ? -delta.x / dt : 0.0f, sv->vertical ? -delta.y / dt : 0.0f};
                }
                // Si el dedo se movio, no es un clic sobre un boton de dentro.
                if (std::abs(delta.x) + std::abs(delta.y) > 2.0f && pressed_ != entt::null) pressed_ = entt::null;
            }
        }
        if (input.mouse_pressed && !consumed) {
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
        scroll_drag_ = entt::null;
    }
    // Inercia de los ScrollView que no se arrastran.
    if (dt > 0.0f) {
        for (const entt::entity h : world.registry().view<ScrollView>()) {
            if (h == scroll_drag_) continue;
            ScrollView& sv = world.registry().get<ScrollView>(h);
            if (!sv.inertia) {
                sv.velocity = Vec2{};
                continue;
            }
            if (std::abs(sv.velocity.x) + std::abs(sv.velocity.y) < 1.0f) continue;
            sv.scroll.x += sv.velocity.x * dt;
            sv.scroll.y += sv.velocity.y * dt;
            const float k = std::exp(-std::max(sv.deceleration, 0.1f) * dt);
            sv.velocity = Vec2{sv.velocity.x * k, sv.velocity.y * k};
        }
    }

    DrawState state;
    state.hovered[0] = hovered;
    state.pressed[0] = pressed_;
    state.focused = focused_;
    buildDraw(world, laid_, state, input, interactive, time, draw_);
}

namespace {

UiRect intersect(const UiRect& a, const UiRect& b) {
    const float x0 = std::max(a.x, b.x);
    const float y0 = std::max(a.y, b.y);
    const float x1 = std::min(a.x + a.w, b.x + b.w);
    const float y1 = std::min(a.y + a.h, b.y + b.h);
    return UiRect{x0, y0, std::max(x1 - x0, 0.0f), std::max(y1 - y0, 0.0f)};
}

UiRect scaled(const UiRect& r, float s) { return UiRect{r.x * s, r.y * s, r.w * s, r.h * s}; }

float alignFactor(int a) { return a == 0 ? 0.0f : (a == 1 ? 0.5f : 1.0f); }

std::vector<ecs::Entity> activeChildren(ecs::World& world, const ecs::Entity& e) {
    std::vector<ecs::Entity> out;
    for (const entt::entity c : e.children()) {
        const ecs::Entity child = world.wrap(c);
        if (child.activeSelf()) out.push_back(child);
    }
    return out;
}

// Coloca hijos de tamano `sizes` en `area` con el LayoutGroup; devuelve el
// tamano del contenido (con margenes).
Vec2 placeChildren(const LayoutGroup& lg, const UiRect& area, const std::vector<Vec2>& sizes, std::vector<UiRect>& out) {
    out.assign(sizes.size(), UiRect{});
    const UiRect inner{area.x + lg.padding.x, area.y + lg.padding.y, std::max(area.w - lg.padding.x * 2.0f, 0.0f),
                       std::max(area.h - lg.padding.y * 2.0f, 0.0f)};
    const std::size_t n = sizes.size();
    if (n == 0) return Vec2{lg.padding.x * 2.0f, lg.padding.y * 2.0f};
    const float ah = alignFactor(static_cast<int>(lg.child_h));
    const float av = alignFactor(static_cast<int>(lg.child_v));
    if (lg.type == LayoutType::Grid) {
        const float cw = std::max(lg.cell_size.x, 1.0f);
        const float ch = std::max(lg.cell_size.y, 1.0f);
        int cols = lg.columns > 0 ? lg.columns
                                  : std::max(1, static_cast<int>(std::floor((inner.w + lg.spacing.x) / (cw + lg.spacing.x))));
        cols = std::min(cols, static_cast<int>(n));
        const int rows = static_cast<int>((n + static_cast<std::size_t>(cols) - 1) / static_cast<std::size_t>(cols));
        const float gw = static_cast<float>(cols) * cw + static_cast<float>(cols - 1) * lg.spacing.x;
        const float gh = static_cast<float>(rows) * ch + static_cast<float>(rows - 1) * lg.spacing.y;
        const float x0 = inner.x + std::max(inner.w - gw, 0.0f) * ah;
        const float y0 = inner.y + std::max(inner.h - gh, 0.0f) * av;
        for (std::size_t i = 0; i < n; ++i) {
            const int c = static_cast<int>(i) % cols;
            const int r = static_cast<int>(i) / cols;
            out[i] = UiRect{x0 + static_cast<float>(c) * (cw + lg.spacing.x), y0 + static_cast<float>(r) * (ch + lg.spacing.y),
                            cw, ch};
        }
        return Vec2{gw + lg.padding.x * 2.0f, gh + lg.padding.y * 2.0f};
    }
    const bool vertical = lg.type == LayoutType::Vertical;
    const float spacing = vertical ? lg.spacing.y : lg.spacing.x;
    float total = spacing * static_cast<float>(n - 1);
    float cross_max = 0.0f;
    for (const Vec2& s : sizes) {
        total += vertical ? s.y : s.x;
        cross_max = std::max(cross_max, vertical ? s.x : s.y);
    }
    const float along_space = vertical ? inner.h : inner.w;
    const float cross_space = vertical ? inner.w : inner.h;
    const float extra = lg.expand_height && total < along_space ? (along_space - total) / static_cast<float>(n) : 0.0f;
    float cursor = (vertical ? inner.y : inner.x);
    if (extra == 0.0f && total < along_space) cursor += (along_space - total) * (vertical ? av : ah);
    for (std::size_t i = 0; i < n; ++i) {
        const float along = (vertical ? sizes[i].y : sizes[i].x) + extra;
        const float cross = lg.expand_width ? cross_space : (vertical ? sizes[i].x : sizes[i].y);
        const float cross_pos = (vertical ? inner.x : inner.y) + (cross_space - cross) * (vertical ? ah : av);
        out[i] = vertical ? UiRect{cross_pos, cursor, cross, along} : UiRect{cursor, cross_pos, along, cross};
        cursor += along + spacing;
    }
    const float cross_content = lg.expand_width ? cross_space : cross_max;
    return vertical ? Vec2{cross_content + lg.padding.x * 2.0f, total + lg.padding.y * 2.0f}
                    : Vec2{total + lg.padding.x * 2.0f, cross_content + lg.padding.y * 2.0f};
}

Vec2 preferredSize(ecs::World& world, const ecs::Entity& e, float width_hint, int depth = 0);

std::vector<Vec2> childSizes(ecs::World& world, const LayoutGroup& lg, const std::vector<ecs::Entity>& kids, float area_w,
                             int depth) {
    std::vector<Vec2> sizes;
    sizes.reserve(kids.size());
    const float inner_w = std::max(area_w - lg.padding.x * 2.0f, 0.0f);
    for (const ecs::Entity& k : kids) {
        const float hint = lg.type == LayoutType::Vertical && lg.expand_width ? inner_w : -1.0f;
        sizes.push_back(preferredSize(world, k, hint, depth + 1));
    }
    return sizes;
}

// Tamano que quiere un elemento: el de su RectTransform o, con un LayoutGroup
// que se ajusta al contenido, el de sus hijos.
Vec2 preferredSize(ecs::World& world, const ecs::Entity& e, float width_hint, int depth) {
    const RectTransform* rt = e.tryGet<RectTransform>();
    Vec2 base = rt != nullptr ? Vec2{std::max(rt->size.x, 0.0f), std::max(rt->size.y, 0.0f)} : Vec2{100.0f, 30.0f};
    if (width_hint > 0.0f) base.x = width_hint;
    const LayoutGroup* lg = e.tryGet<LayoutGroup>();
    if (lg == nullptr || !lg->fit_content || depth > 32) return base;
    const std::vector<ecs::Entity> kids = activeChildren(world, e);
    std::vector<UiRect> rects;
    const Vec2 content = placeChildren(*lg, UiRect{0.0f, 0.0f, base.x, base.y}, childSizes(world, *lg, kids, base.x, depth), rects);
    if (lg->type == LayoutType::Horizontal) return Vec2{content.x, base.y};
    return Vec2{base.x, content.y};
}

}  // namespace

void UiSystem::layoutCanvas(ecs::World& world, const ecs::Entity& canvas, const UiRect& root, float scale,
                            std::vector<Laid>& out) const {
    for (const ecs::Entity& child : activeChildren(world, canvas)) {
        UiRect r = root;
        if (const RectTransform* rt = child.tryGet<RectTransform>()) r = layoutRect(*rt, root);
        if (const LayoutGroup* lg = child.tryGet<LayoutGroup>(); lg != nullptr && lg->fit_content) {
            const Vec2 pref = preferredSize(world, child, r.w);
            if (lg->type == LayoutType::Horizontal) r.w = pref.x;
            else r.h = pref.y;
        }
        layoutNode(world, child, r, scale, false, UiRect{}, out);
    }
}

// El elemento (con su rectangulo ya calculado) y, debajo, sus hijos: con su
// LayoutGroup, dentro de su ScrollView (desplazados y recortados) o con sus
// RectTransform.
void UiSystem::layoutNode(ecs::World& world, const ecs::Entity& e, const UiRect& rect_in, float scale, bool clipped,
                          const UiRect& clip, std::vector<Laid>& out) const {
    UiRect rect = rect_in;
    const std::vector<ecs::Entity> kids = activeChildren(world, e);
    const LayoutGroup* lg = e.tryGet<LayoutGroup>();
    std::vector<UiRect> rects;
    Vec2 content{};
    if (lg != nullptr) {
        content = placeChildren(*lg, rect, childSizes(world, *lg, kids, rect.w, 0), rects);
        if (lg->fit_content) {
            if (lg->type == LayoutType::Horizontal) rect.w = content.x;
            else rect.h = content.y;
            content = placeChildren(*lg, rect, childSizes(world, *lg, kids, rect.w, 0), rects);
        }
    } else {
        rects.resize(kids.size());
        for (std::size_t i = 0; i < kids.size(); ++i) {
            UiRect r = rect;
            if (const RectTransform* rt = kids[i].tryGet<RectTransform>()) r = layoutRect(*rt, rect);
            if (const LayoutGroup* klg = kids[i].tryGet<LayoutGroup>(); klg != nullptr && klg->fit_content) {
                const Vec2 pref = preferredSize(world, kids[i], r.w);
                if (klg->type == LayoutType::Horizontal) r.w = pref.x;
                else r.h = pref.y;
            }
            rects[i] = r;
            content.x = std::max(content.x, r.x + r.w - rect.x);
            content.y = std::max(content.y, r.y + r.h - rect.y);
        }
    }
    out.push_back(Laid{e.handle(), scaled(rect, scale), rect_in, scale, clipped, clip});

    bool child_clipped = clipped;
    UiRect child_clip = clip;
    const Mask* mask = e.tryGet<Mask>();
    ScrollView* sv = e.tryGet<ScrollView>();
    if (sv != nullptr || (mask != nullptr && mask->enabled)) {
        const UiRect px = scaled(rect, scale);
        child_clip = clipped ? intersect(clip, px) : px;
        child_clipped = true;
    }
    if (sv != nullptr) {
        sv->content = content;
        const float max_x = sv->horizontal ? std::max(content.x - rect.w, 0.0f) : 0.0f;
        const float max_y = sv->vertical ? std::max(content.y - rect.h, 0.0f) : 0.0f;
        sv->scroll.x = std::clamp(sv->scroll.x, 0.0f, max_x);
        sv->scroll.y = std::clamp(sv->scroll.y, 0.0f, max_y);
        for (UiRect& r : rects) {
            r.x -= sv->scroll.x;
            r.y -= sv->scroll.y;
        }
    }
    for (std::size_t i = 0; i < kids.size(); ++i) layoutNode(world, kids[i], rects[i], scale, child_clipped, child_clip, out);
}

UiRect UiSystem::dropdownItemRect(const UiRect& box, const Dropdown& d, int i) const {
    const int count = static_cast<int>(d.options.size());
    const int visible = std::clamp(d.max_visible, 1, std::max(count, 1));
    const float h = box.h;
    const float list_h = h * static_cast<float>(visible);
    // Hacia abajo si cabe; si no, hacia arriba.
    const bool up = view_height_ > 0.0f && box.y + box.h + list_h > view_height_ && box.y - list_h >= 0.0f;
    const float top = up ? box.y - list_h : box.y + box.h;
    return UiRect{box.x, top + (static_cast<float>(i) - d.list_scroll) * h, box.w, h};
}

ecs::Entity UiSystem::targetOf(ecs::World& world, ecs::Entity source, const Uuid& target) const {
    const ecs::Entity t = target.valid() ? world.find(target) : ecs::Entity{};
    return t.valid() ? t : source;
}

void UiSystem::activate(ecs::World& world, ecs::Entity e) {
    if (Dropdown* d = e.tryGet<Dropdown>(); d != nullptr && d->interactable) {
        d->open = !d->open;
        if (d->open) {
            if (open_dropdown_ != entt::null && open_dropdown_ != e.handle() && world.registry().valid(open_dropdown_)) {
                if (Dropdown* other = world.registry().try_get<Dropdown>(open_dropdown_)) other->open = false;
            }
            open_dropdown_ = e.handle();
            const int count = static_cast<int>(d->options.size());
            const int visible = std::clamp(d->max_visible, 1, std::max(count, 1));
            d->list_scroll = static_cast<float>(std::clamp(d->value - visible / 2, 0, std::max(count - visible, 0)));
        } else if (open_dropdown_ == e.handle()) {
            open_dropdown_ = entt::null;
        }
    }
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
    std::vector<UiDrawCommand> late;     // barras de desplazamiento (encima de su contenido)
    std::vector<UiDrawCommand> overlay;  // listas abiertas de los Dropdown (encima de todo)
    for (const Laid& l : laid) {
        const ecs::Entity e = world.wrap(l.entity);
        const UiRect& r = l.rect;
        const float s = l.scale;
        UiDrawCommand base;
        base.rect = r;
        base.entity = l.entity;
        base.clipped = l.clipped;
        base.clip = l.clip;
        base.scale = s;
        if (const ScrollView* sv = e.tryGet<ScrollView>()) {
            if (sv->background_alpha > 0.0f) {
                UiDrawCommand bg = base;
                bg.color = rgba(sv->background, sv->background_alpha);
                out.push_back(bg);
            }
            if (sv->show_scrollbar) {
                const float view_h = r.h / std::max(s, 1e-3f);
                const float view_w = r.w / std::max(s, 1e-3f);
                const float thick = 6.0f * s;
                if (sv->vertical && sv->content.y > view_h + 0.5f) {
                    const float frac = view_h / sv->content.y;
                    const float pos = sv->scroll.y / std::max(sv->content.y - view_h, 1.0f);
                    UiDrawCommand bar = base;
                    bar.rect = UiRect{r.x + r.w - thick - 2.0f * s, r.y + (r.h - r.h * frac) * pos, thick, r.h * frac};
                    bar.color = rgba(sv->scrollbar_color, 0.75f);
                    bar.radius = thick * 0.5f;
                    late.push_back(bar);
                }
                if (sv->horizontal && sv->content.x > view_w + 0.5f) {
                    const float frac = view_w / sv->content.x;
                    const float pos = sv->scroll.x / std::max(sv->content.x - view_w, 1.0f);
                    UiDrawCommand bar = base;
                    bar.rect = UiRect{r.x + (r.w - r.w * frac) * pos, r.y + r.h - thick - 2.0f * s, r.w * frac, thick};
                    bar.color = rgba(sv->scrollbar_color, 0.75f);
                    bar.radius = thick * 0.5f;
                    late.push_back(bar);
                }
            }
        }
        if (const Dropdown* d = e.tryGet<Dropdown>()) {
            UiDrawCommand box = base;
            Vec3 bg = d->background;
            if (hovered_by_any(l.entity) && d->interactable) bg = bg * 1.25f;
            box.color = rgba(bg, d->interactable ? 1.0f : 0.5f);
            box.radius = d->corner_radius * s;
            out.push_back(box);
            UiDrawCommand label = base;
            label.type = UiDrawCommand::Type::Text;
            label.rect = UiRect{r.x + 10.0f * s, r.y, r.w - 10.0f * s - r.h, r.h};
            label.font_size = d->font_size * s * gameplay::accessibility().text_scale;
            label.h_align = 0;
            label.text = d->value >= 0 && d->value < static_cast<int>(d->options.size())
                             ? d->options[static_cast<std::size_t>(d->value)]
                             : std::string();
            label.color = rgba(d->text_color, d->interactable ? 1.0f : 0.5f);
            out.push_back(label);
            UiDrawCommand arrow = base;
            arrow.type = UiDrawCommand::Type::Triangle;
            const float a = r.h * 0.28f;
            arrow.rect = UiRect{r.x + r.w - r.h * 0.5f - a * 0.5f, r.y + (r.h - a * 0.6f) * 0.5f, a, a * 0.6f};
            arrow.radius = d->open ? 1.0f : 0.0f;  // 1 = hacia arriba
            arrow.color = rgba(d->text_color, 0.9f);
            out.push_back(arrow);
            if (d->open && open_dropdown_ == l.entity) {
                const int count = static_cast<int>(d->options.size());
                const int visible = std::clamp(d->max_visible, 1, std::max(count, 1));
                const UiRect first = dropdownItemRect(r, *d, 0);
                const UiRect list{r.x, first.y + d->list_scroll * r.h, r.w, r.h * static_cast<float>(visible)};
                UiDrawCommand panel;
                panel.rect = UiRect{list.x - 1.0f, list.y - 1.0f, list.w + 2.0f, list.h + 2.0f};
                panel.color = Vec4{0.0f, 0.0f, 0.0f, 0.45f};
                panel.radius = d->corner_radius * s;
                panel.entity = l.entity;
                overlay.push_back(panel);
                UiDrawCommand fill = panel;
                fill.rect = list;
                fill.color = rgba(d->background * 0.85f, 0.98f);
                overlay.push_back(fill);
                for (int i = 0; i < count; ++i) {
                    const UiRect ir = dropdownItemRect(r, *d, i);
                    if (ir.y + ir.h <= list.y + 0.5f || ir.y >= list.y + list.h - 0.5f) continue;
                    const bool over = ir.contains(input.mouse_x, input.mouse_y);
                    if (over || i == d->value) {
                        UiDrawCommand hl;
                        hl.rect = ir;
                        hl.color = rgba(d->highlight, over ? 0.9f : 0.45f);
                        hl.entity = l.entity;
                        hl.clipped = true;
                        hl.clip = list;
                        overlay.push_back(hl);
                    }
                    UiDrawCommand t;
                    t.type = UiDrawCommand::Type::Text;
                    t.rect = UiRect{ir.x + 10.0f * s, ir.y, ir.w - 20.0f * s, ir.h};
                    t.text = d->options[static_cast<std::size_t>(i)];
                    t.font_size = d->font_size * s * gameplay::accessibility().text_scale;
                    t.h_align = 0;
                    t.color = rgba(d->text_color, 1.0f);
                    t.entity = l.entity;
                    t.clipped = true;
                    t.clip = list;
                    overlay.push_back(t);
                }
            }
        }
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
            text.font_size = field->font_size * s * gameplay::accessibility().text_scale;
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
            c.font_size = text->font_size * s * gameplay::accessibility().text_scale;
            c.color = rgba(text->color, text->alpha);
            c.h_align = static_cast<int>(text->h_align);
            c.v_align = static_cast<int>(text->v_align);
            c.wrap = text->wrap;
            c.shadow = text->shadow;
            c.rich = text->rich_text && c.text.find('<') != std::string::npos;
            c.bold = text->bold;
            c.italic = text->italic;
            c.font = text->font;
            c.outline = text->outline * s;
            if (gameplay::accessibility().high_contrast_ui) {
                // Alto contraste: contorno oscuro en todo el texto.
                c.outline = std::max(c.outline, 1.5f * s);
                c.outline_color = Vec4{0.0f, 0.0f, 0.0f, text->alpha};
            }
            c.outline_color = rgba(text->outline_color, text->alpha);
            c.line_spacing = text->line_spacing;
            out.push_back(c);
        }
        // Caja de dialogo (gameplay/DialogueUi.cpp): el dialogo que corre.
        if (const gameplay::DialogueBox* box = e.tryGet<gameplay::DialogueBox>()) {
            if (gameplay::updateDialogueBox(*box, r, s, input, interactive, l.entity, out)) capturing_mouse_ = true;
        }
    }
    out.insert(out.end(), late.begin(), late.end());
    out.insert(out.end(), overlay.begin(), overlay.end());
}

// -----------------------------------------------------------------------------
// Texto enriquecido
// -----------------------------------------------------------------------------

namespace {

bool parseHexColor(const std::string& v, Vec4& out) {
    std::string h = v;
    if (!h.empty() && h[0] == '#') h = h.substr(1);
    if (h.size() != 6 && h.size() != 8 && h.size() != 3) return false;
    if (h.size() == 3) h = std::string{h[0], h[0], h[1], h[1], h[2], h[2]};
    unsigned int value = 0;
    for (const char c : h) {
        value <<= 4;
        if (c >= '0' && c <= '9') value |= static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned>(c - 'A' + 10);
        else return false;
    }
    if (h.size() == 6) {
        out = Vec4{((value >> 16) & 255u) / 255.0f, ((value >> 8) & 255u) / 255.0f, (value & 255u) / 255.0f, out.w};
    } else {
        out = Vec4{((value >> 24) & 255u) / 255.0f, ((value >> 16) & 255u) / 255.0f, ((value >> 8) & 255u) / 255.0f,
                   (value & 255u) / 255.0f};
    }
    return true;
}

bool namedColor(std::string n, Vec4& out) {
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    struct N {
        const char* name;
        float r, g, b;
    };
    static constexpr N kNames[] = {{"red", 1.0f, 0.2f, 0.2f},    {"rojo", 1.0f, 0.2f, 0.2f},    {"green", 0.3f, 0.9f, 0.3f},
                                   {"verde", 0.3f, 0.9f, 0.3f},  {"blue", 0.3f, 0.5f, 1.0f},    {"azul", 0.3f, 0.5f, 1.0f},
                                   {"yellow", 1.0f, 0.9f, 0.2f}, {"amarillo", 1.0f, 0.9f, 0.2f}, {"orange", 1.0f, 0.6f, 0.1f},
                                   {"naranja", 1.0f, 0.6f, 0.1f}, {"white", 1.0f, 1.0f, 1.0f},  {"blanco", 1.0f, 1.0f, 1.0f},
                                   {"black", 0.0f, 0.0f, 0.0f},  {"negro", 0.0f, 0.0f, 0.0f},   {"gray", 0.55f, 0.55f, 0.55f},
                                   {"grey", 0.55f, 0.55f, 0.55f}, {"gris", 0.55f, 0.55f, 0.55f}, {"cyan", 0.2f, 0.9f, 1.0f},
                                   {"magenta", 1.0f, 0.3f, 1.0f}, {"purple", 0.65f, 0.35f, 1.0f}, {"morado", 0.65f, 0.35f, 1.0f}};
    for (const N& k : kNames) {
        if (n == k.name) {
            out = Vec4{k.r, k.g, k.b, out.w};
            return true;
        }
    }
    return false;
}

}  // namespace

std::vector<RichRun> parseRichText(const std::string& text, const Vec4& color, float size, float scale, bool bold,
                                   bool italic) {
    std::vector<RichRun> runs;
    std::vector<Vec4> colors{color};
    std::vector<float> sizes{size};
    int b = bold ? 1 : 0, i = italic ? 1 : 0, u = 0, st = 0;
    RichRun cur;
    const auto style = [&]() {
        cur.color = colors.back();
        cur.size = sizes.back();
        cur.bold = b > 0;
        cur.italic = i > 0;
        cur.underline = u > 0;
        cur.strike = st > 0;
    };
    const auto flush = [&]() {
        if (!cur.text.empty()) runs.push_back(cur);
        cur.text.clear();
        style();
    };
    style();
    std::size_t pos = 0;
    while (pos < text.size()) {
        const char c = text[pos];
        if (c == '<') {
            const std::size_t close = text.find('>', pos);
            if (close != std::string::npos && close - pos < 64) {
                std::string tag = text.substr(pos + 1, close - pos - 1);
                std::string value;
                const std::size_t eq = tag.find('=');
                if (eq != std::string::npos) {
                    value = tag.substr(eq + 1);
                    tag = tag.substr(0, eq);
                    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'')) value = value.substr(1, value.size() - 2);
                }
                for (char& ch : tag) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                bool known = true;
                if (tag == "b") { flush(); ++b; }
                else if (tag == "/b") { flush(); b = std::max(b - 1, 0); }
                else if (tag == "i") { flush(); ++i; }
                else if (tag == "/i") { flush(); i = std::max(i - 1, 0); }
                else if (tag == "u") { flush(); ++u; }
                else if (tag == "/u") { flush(); u = std::max(u - 1, 0); }
                else if (tag == "s") { flush(); ++st; }
                else if (tag == "/s") { flush(); st = std::max(st - 1, 0); }
                else if (tag == "br" || tag == "br/") { cur.text += '\n'; }
                else if (tag == "color") {
                    Vec4 col = colors.back();
                    if (parseHexColor(value, col) || namedColor(value, col)) {
                        flush();
                        colors.push_back(col);
                        style();
                    } else {
                        known = false;
                    }
                } else if (tag == "/color") {
                    flush();
                    if (colors.size() > 1) colors.pop_back();
                    style();
                } else if (tag == "size") {
                    float v = 0.0f;
                    const bool percent = !value.empty() && value.back() == '%';
                    try {
                        v = std::stof(percent ? value.substr(0, value.size() - 1) : value);
                    } catch (...) {
                        v = 0.0f;
                    }
                    if (v > 0.0f) {
                        flush();
                        sizes.push_back(percent ? sizes.back() * v / 100.0f : v * scale);
                        style();
                    } else {
                        known = false;
                    }
                } else if (tag == "/size") {
                    flush();
                    if (sizes.size() > 1) sizes.pop_back();
                    style();
                } else {
                    known = false;
                }
                if (known) {
                    pos = close + 1;
                    continue;
                }
            }
        }
        cur.text += c;
        ++pos;
    }
    flush();
    return runs;
}

std::string stripRichText(const std::string& text) {
    std::string out;
    for (const RichRun& r : parseRichText(text, Vec4{1.0f, 1.0f, 1.0f, 1.0f}, 16.0f, 1.0f, false, false)) out += r.text;
    return out;
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
    registry.registerComponent<Dropdown>("UIDropdown", "Desplegable (Dropdown)", "UI");
    registry.registerComponent<ScrollView>("UIScrollView", "Scroll View", "UI");
    registry.registerComponent<LayoutGroup>("UILayoutGroup", "Layout Group", "UI");
    registry.registerComponent<Mask>("UIMask", "Mascara (recorte)", "UI");
}

}  // namespace cramion::ui
