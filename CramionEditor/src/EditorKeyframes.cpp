// Animacion por claves en el editor, como la de Blender:
//
//   - Ventana Animacion con dos modos: Dope Sheet (rombos por canal, para
//     mover claves en el tiempo) y Graficas (las curvas con sus asas Bezier).
//     A la izquierda los canales (animacion > propiedad > eje); arriba la
//     regla en fotogramas con el cabezal y el rango de la animacion.
//   - Insertar claves: I con el raton sobre una propiedad del Inspector (o
//     clic derecho > Insertar fotograma clave), I en la Escena (posicion,
//     rotacion y escala de lo seleccionado) y la grabacion automatica (el
//     boton rojo): todo lo que se edite se graba en el fotograma actual.
//   - Fuera de Play las animaciones se aplican en el fotograma del cabezal
//     cada vez que cambia (o cambian las claves), como al mover el cabezal en
//     Blender. En Play suenan solas (anim::KeyframeSystem).
//
// Atajos de la ventana: Espacio reproducir, Izquierda/Derecha un fotograma,
// Arriba/Abajo clave siguiente/anterior, A todo, Alt+A nada, X/Supr borrar,
// Ctrl+C/Ctrl+V copiar y pegar en el cabezal, T interpolacion, V tipo de asa,
// Shift+E extrapolacion, Inicio encuadrar, rueda zoom, boton central mover.

#include "EditorApp.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace cramion::editor {

using anim::AnimationCurve;
using anim::HandleType;
using anim::Interpolation;
using anim::Keyframe;
using anim::KeyframeAnimation;
using core::Vec2;

namespace {

constexpr float kChannelWidth = 240.0f;
constexpr float kRulerHeight = 24.0f;
constexpr float kRowHeight = 20.0f;
constexpr float kPickRadius = 6.0f;

constexpr ImU32 kCanvasBg = IM_COL32(40, 40, 45, 255);
constexpr ImU32 kOutsideRange = IM_COL32(0, 0, 0, 60);
constexpr ImU32 kGridMajor = IM_COL32(62, 62, 70, 255);
constexpr ImU32 kGridMinor = IM_COL32(50, 50, 56, 255);
constexpr ImU32 kPlayhead = IM_COL32(71, 114, 179, 255);  // azul del cabezal de Blender
constexpr ImU32 kKey = IM_COL32(225, 225, 225, 255);
constexpr ImU32 kKeySelected = IM_COL32(255, 175, 41, 255);
constexpr ImU32 kKeyOutline = IM_COL32(10, 10, 12, 255);
constexpr ImU32 kHandleLine = IM_COL32(150, 150, 155, 200);
constexpr ImU32 kBoxFill = IM_COL32(255, 255, 255, 24);
constexpr ImU32 kBoxLine = IM_COL32(255, 255, 255, 140);
constexpr ImU32 kRecord = IM_COL32(220, 50, 50, 255);

enum class RowKind { Summary, Owner, Group, Channel };

}  // namespace

// Fila de la lista de canales (y del Dope Sheet).
struct AnimationRow {
    RowKind kind = RowKind::Channel;
    int depth = 0;
    std::string label;
    std::string key;  // plegar y elegir
    struct Ref {
        Uuid owner;
        std::size_t curve = 0;
    };
    std::vector<Ref> curves;
    ImU32 color = IM_COL32(200, 200, 200, 255);
    bool collapsible = false;
};

struct KeyframeEditorState {
    float time = 0.0f;  // cabezal, en segundos
    float applied_time = -1e30f;
    std::uint64_t applied_generation = ~0ull;
    bool dirty = true;
    bool playing = false;
    bool autokey = false;
    bool graph = false;
    bool only_selected = true;
    Interpolation default_interpolation = Interpolation::Bezier;
    Uuid active{};  // animacion de la que se usan el rango y los fps

    // Vista.
    float view_start = -0.5f;  // segundos en el borde izquierdo
    float px_per_sec = 140.0f;
    float value_center = 0.0f;  // Graficas: valor en el centro
    float px_per_unit = 40.0f;
    float scroll_rows = 0.0f;
    bool fit_request = true;

    std::vector<AnimationRow> rows;
    std::set<std::string> collapsed;
    std::string active_row;

    // Arrastre.
    enum class Drag { None, Pending, Scrub, Keys, Box, Handle, Pan };
    Drag drag = Drag::None;
    ImVec2 press{};
    ImVec2 last_mouse{};
    bool press_on_key = false;
    // Claves elegidas al empezar a moverlas, por curva y en orden.
    struct Moving {
        Uuid owner;
        std::size_t curve = 0;
        std::vector<Keyframe> original;
    };
    std::vector<Moving> moving;
    // Asa que se arrastra.
    Uuid handle_owner{};
    std::size_t handle_curve = 0;
    std::size_t handle_key = 0;
    bool handle_left = false;

    // Portapapeles.
    struct Clip {
        Uuid owner;
        Uuid target;
        std::string component;
        std::string property;
        int channel = 0;
        Keyframe key;  // tiempo relativo a la primera copiada
    };
    std::vector<Clip> clipboard;

    KeyframeHook hook;
};

KeyframeEditorState& EditorApp::keyframeEditor() {
    if (!keyframe_editor_) keyframe_editor_ = std::make_shared<KeyframeEditorState>();
    return *keyframe_editor_;
}

namespace {

float snapTime(float time, float fps) { return fps > 0.0f ? std::round(time * fps) / fps : time; }

const char* axisName(int channel, int channels) {
    if (channels <= 1) return "";
    static constexpr const char* kAxes[] = {"X", "Y", "Z", "W"};
    return kAxes[std::clamp(channel, 0, 3)];
}

ImU32 channelColor(const AnimationCurve& c, int channels) {
    if (channels >= 2 && c.channel <= 2) {
        static constexpr ImU32 kAxes[] = {theme::kAxisX, theme::kAxisY, theme::kAxisZ};
        return kAxes[c.channel];
    }
    const std::size_t h = std::hash<std::string>{}(c.component + "/" + c.property);
    ImVec4 col;
    ImGui::ColorConvertHSVtoRGB(static_cast<float>(h % 360) / 360.0f, 0.55f, 0.95f, col.x, col.y, col.z);
    return IM_COL32(static_cast<int>(col.x * 255), static_cast<int>(col.y * 255), static_cast<int>(col.z * 255), 255);
}

void drawDiamond(ImDrawList* draw, ImVec2 c, float r, ImU32 fill) {
    const ImVec2 pts[4] = {{c.x, c.y - r}, {c.x + r, c.y}, {c.x, c.y + r}, {c.x - r, c.y}};
    draw->AddConvexPolyFilled(pts, 4, fill);
    draw->AddPolyline(pts, 4, kKeyOutline, ImDrawFlags_Closed, 1.0f);
}

// Paso de la cuadricula: 1, 2 o 5 por una potencia de 10, con `min_px` entre lineas.
float niceStep(float units_per_px, float min_px) {
    const float raw = units_per_px * min_px;
    const float p = std::pow(10.0f, std::floor(std::log10(std::max(raw, 1e-6f))));
    for (const float m : {1.0f, 2.0f, 5.0f, 10.0f}) {
        if (p * m >= raw) return p * m;
    }
    return p * 10.0f;
}

AnimationCurve* resolveCurve(ecs::World& world, const Uuid& owner, std::size_t index) {
    const ecs::Entity e = world.find(owner);
    if (!e.valid()) return nullptr;
    KeyframeAnimation* a = e.tryGet<KeyframeAnimation>();
    return a != nullptr && index < a->curves.size() ? &a->curves[index] : nullptr;
}

bool interpolationIs(const AnimationCurve& c, std::size_t key, bool left, Interpolation mode) {
    if (left) return key > 0 && c.keys[key - 1].interpolation == mode;
    return key + 1 < c.keys.size() && c.keys[key].interpolation == mode;
}

}  // namespace

// -----------------------------------------------------------------------------
// Animaciones y claves
// -----------------------------------------------------------------------------

// La animacion que guarda las curvas de `target`: la suya, o la de otro objeto
// que ya lo anima. Con `create`, se le anade una si no hay.
ecs::Entity EditorApp::animationOwnerFor(ecs::Entity target, bool create) {
    if (!target.valid()) return {};
    if (target.has<KeyframeAnimation>()) return target;
    for (const entt::entity handle : world_.registry().view<KeyframeAnimation>()) {
        const KeyframeAnimation& a = world_.registry().get<KeyframeAnimation>(handle);
        for (const AnimationCurve& c : a.curves) {
            if (c.target == target.uuid()) return world_.wrap(handle);
        }
    }
    if (!create) return {};
    KeyframeAnimation& a = target.add<KeyframeAnimation>();
    a.fps = 24.0f;
    // El rango, como el de la animacion activa (si la hay).
    if (const ecs::Entity active = world_.find(keyframeEditor().active); active.valid() && active.has<KeyframeAnimation>()) {
        const KeyframeAnimation& o = active.get<KeyframeAnimation>();
        a.start = o.start;
        a.end = o.end;
        a.fps = o.fps;
    }
    keyframeEditor().active = target.uuid();
    return target;
}

float EditorApp::animationFps() {
    const ecs::Entity active = world_.find(keyframeEditor().active);
    const KeyframeAnimation* a = active.valid() ? active.tryGet<KeyframeAnimation>() : nullptr;
    return a != nullptr && a->fps > 0.0f ? a->fps : 24.0f;
}

bool EditorApp::insertKeyframes(ecs::Entity target, const std::string& component, const std::string& path) {
    const int channels = anim::propertyChannels(world_, target, component, path);
    if (channels <= 0) return false;
    const ecs::Entity owner = animationOwnerFor(target, true);
    KeyframeAnimation& a = owner.get<KeyframeAnimation>();
    KeyframeEditorState& st = keyframeEditor();
    const float time = snapTime(st.time, a.fps);
    const Uuid ref = owner == target ? Uuid{} : target.uuid();
    for (int ch = 0; ch < channels; ++ch) {
        float value = 0.0f;
        if (!anim::readProperty(world_, target, component, path, ch, value)) continue;
        a.curve(ref, component, path, ch).insert(time, value, st.default_interpolation);
    }
    st.dirty = true;
    dirty_ = true;
    return true;
}

int EditorApp::removeKeyframes(ecs::Entity target, const std::string& component, const std::string& path, bool all) {
    const ecs::Entity owner = animationOwnerFor(target, false);
    if (!owner.valid()) return 0;
    KeyframeAnimation& a = owner.get<KeyframeAnimation>();
    const Uuid ref = owner == target ? Uuid{} : target.uuid();
    const float time = snapTime(keyframeEditor().time, a.fps);
    int removed = 0;
    for (std::size_t i = a.curves.size(); i-- > 0;) {
        AnimationCurve& c = a.curves[i];
        if (!(c.target == ref) || c.component != component || c.property != path) continue;
        if (all) {
            removed += static_cast<int>(c.keys.size());
            c.keys.clear();
        } else if (const int k = c.find(time, 0.5f / a.fps); k >= 0) {
            c.keys.erase(c.keys.begin() + k);
            c.recalcHandles();
            ++removed;
        }
        if (c.keys.empty()) {
            a.curves.erase(a.curves.begin() + static_cast<std::ptrdiff_t>(i));
            a.touchCurves();  // la cache del KeyframeSystem se rehace
        }
    }
    if (removed > 0) {
        keyframeEditor().dirty = true;
        dirty_ = true;
    }
    return removed;
}

// I en la Escena: posicion, rotacion y escala de lo seleccionado (Alt+I: quitar
// las claves de este fotograma).
void EditorApp::insertSelectionKeys(bool remove) {
    if (playing()) return;
    int count = 0;
    for (ecs::Entity e : selectedEntities()) {
        for (const char* path : {"position", "rotation", "scale"}) {
            if (remove) count += removeKeyframes(e, "Transform", path, false);
            else count += insertKeyframes(e, "Transform", path) ? 1 : 0;
        }
    }
    if (count > 0) commit();
}

// Grabacion automatica tras soltar el gizmo: la propiedad que se movio.
void EditorApp::autoKeyTransforms(const char* path) {
    if (playing() || !keyframeEditor().autokey || path == nullptr) return;
    for (ecs::Entity e : selectedEntities()) insertKeyframes(e, "Transform", path);
}

KeyframeHook* EditorApp::inspectorKeyframeHook(ecs::Entity entity, const std::string& component) {
    // Las propiedades de la propia animacion no se animan.
    if (playing() || !entity.valid() || component == "KeyframeAnimation") return nullptr;
    KeyframeEditorState& st = keyframeEditor();
    st.hook.action = KeyframeHook::Action::None;
    const ecs::Entity owner = animationOwnerFor(entity, false);
    // Sin animacion: no hay nada que colorear, pero I y el menu funcionan.
    const KeyframeAnimation* a = owner.valid() ? owner.tryGet<KeyframeAnimation>() : nullptr;
    const Uuid ref = owner == entity ? Uuid{} : entity.uuid();
    const float time = a != nullptr ? snapTime(st.time, a->fps) : 0.0f;
    const float tolerance = a != nullptr ? 0.5f / a->fps : 0.0f;
    st.hook.state = [a, ref, component, time, tolerance](const std::string& path) {
        if (a == nullptr) return 0;
        int state = 0;
        for (const AnimationCurve& c : a->curves) {
            if (!(c.target == ref) || c.component != component || c.property != path) continue;
            state = std::max(state, 1);
            if (c.find(time, tolerance) >= 0) return 2;
        }
        return state;
    };
    return &st.hook;
}

void EditorApp::inspectorKeyframes(ecs::Entity entity, const std::vector<ecs::Entity>& selected,
                                   const std::string& component, const std::string* changed_path) {
    if (playing()) return;
    KeyframeEditorState& st = keyframeEditor();
    const std::vector<ecs::Entity> targets = selected.empty() ? std::vector<ecs::Entity>{entity} : selected;
    bool done = false;
    switch (st.hook.action) {
        case KeyframeHook::Action::Insert:
            for (ecs::Entity e : targets) done |= insertKeyframes(e, component, st.hook.path);
            break;
        case KeyframeHook::Action::Delete:
            for (ecs::Entity e : targets) done |= removeKeyframes(e, component, st.hook.path, false) > 0;
            break;
        case KeyframeHook::Action::Clear:
            for (ecs::Entity e : targets) done |= removeKeyframes(e, component, st.hook.path, true) > 0;
            break;
        case KeyframeHook::Action::None: break;
    }
    st.hook.action = KeyframeHook::Action::None;
    if (done) commit();
    // Grabacion automatica: lo editado en el Inspector va a una clave (el
    // punto de deshacer lo pone el Inspector al soltar el control).
    if (st.autokey && changed_path != nullptr && component != "KeyframeAnimation") {
        for (ecs::Entity e : targets) insertKeyframes(e, component, *changed_path);
    }
}

// -----------------------------------------------------------------------------
// Cada frame
// -----------------------------------------------------------------------------

void EditorApp::updateKeyframes(float delta_seconds) {
    KeyframeEditorState& st = keyframeEditor();
    if (playing()) {
        st.playing = false;
        if (play_state_ == PlayState::Playing) keyframes_.update(world_, delta_seconds);
        st.dirty = true;  // al parar se vuelve a la pose del cabezal
        return;
    }
    if (st.playing) {
        float start = 0.0f;
        float end = 4.0f;
        if (const ecs::Entity a = world_.find(st.active); a.valid() && a.has<KeyframeAnimation>()) {
            start = a.get<KeyframeAnimation>().start;
            end = a.get<KeyframeAnimation>().end;
        }
        st.time += delta_seconds;
        if (end > start && st.time > end) st.time = start + std::fmod(st.time - start, end - start);
        if (st.time < start) st.time = start;
    }
    if (st.dirty || st.time != st.applied_time || st.applied_generation != world_.sceneGeneration()) {
        anim::KeyframeSystem::applyAllAt(world_, st.time);
        st.applied_time = st.time;
        st.applied_generation = world_.sceneGeneration();
        st.dirty = false;
    }
}

void EditorApp::resetKeyframesForPlay() {
    keyframes_.reset();
    keyframeEditor().playing = false;
    keyframeEditor().dirty = true;
}

// -----------------------------------------------------------------------------
// Ventana
// -----------------------------------------------------------------------------

void EditorApp::buildAnimationRows() {
    KeyframeEditorState& st = keyframeEditor();
    st.rows.clear();
    std::vector<ecs::Entity> owners;
    for (const entt::entity handle : world_.registry().view<KeyframeAnimation>()) owners.push_back(world_.wrap(handle));
    std::sort(owners.begin(), owners.end(), [](const ecs::Entity& a, const ecs::Entity& b) { return a.name() < b.name(); });

    AnimationRow summary;
    summary.kind = RowKind::Summary;
    summary.label = "Resumen";
    summary.key = "#summary";
    st.rows.push_back(summary);
    const ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    for (const ecs::Entity owner : owners) {
        const KeyframeAnimation& a = owner.get<KeyframeAnimation>();
        const bool owner_selected = selection_.contains(owner.uuid());
        AnimationRow owner_row;
        owner_row.kind = RowKind::Owner;
        owner_row.depth = 0;
        owner_row.label = owner.name();
        owner_row.key = owner.uuid().toString();
        owner_row.collapsible = true;
        owner_row.color = theme::kYellow;
        std::vector<AnimationRow> children;
        std::vector<AnimationRow::Ref> owner_curves;
        for (std::size_t i = 0; i < a.curves.size();) {
            const AnimationCurve& first = a.curves[i];
            // Los canales seguidos de la misma propiedad, juntos.
            std::size_t j = i + 1;
            while (j < a.curves.size() && a.curves[j].target == first.target && a.curves[j].component == first.component &&
                   a.curves[j].property == first.property) {
                ++j;
            }
            const ecs::Entity target = first.target.valid() ? world_.find(first.target) : owner;
            const bool target_selected = target.valid() && selection_.contains(target.uuid());
            if (st.only_selected && !owner_selected && !target_selected) {
                i = j;
                continue;
            }
            // Etiqueta: objeto (si es otro) · componente · propiedad.
            std::string property_label = first.property;
            int channels = 1;
            for (const anim::AnimatableProperty& p : anim::animatableProperties(world_, target, first.component)) {
                if (p.path == first.property) {
                    property_label = p.label;
                    channels = p.channels;
                    break;
                }
            }
            const ecs::ComponentType* type = registry.find(first.component);
            std::string label;
            if (!(target == owner)) label += (target.valid() ? target.name() : std::string("(falta)")) + " · ";
            if (first.component != "Transform") label += (type != nullptr ? type->label : first.component) + " · ";
            label += property_label;
            const std::string group_key = owner_row.key + "|" + first.target.toString() + "|" + first.component + "|" +
                                          first.property;
            const bool grouped = channels > 1;
            if (grouped) {
                AnimationRow group;
                group.kind = RowKind::Group;
                group.depth = 1;
                group.label = label;
                group.key = group_key;
                group.collapsible = true;
                for (std::size_t k = i; k < j; ++k) group.curves.push_back({owner.uuid(), k});
                children.push_back(group);
            }
            const bool group_open = !grouped || !st.collapsed.contains(group_key);
            for (std::size_t k = i; k < j; ++k) {
                owner_curves.push_back({owner.uuid(), k});
                if (!group_open) continue;
                AnimationRow ch;
                ch.kind = RowKind::Channel;
                ch.depth = grouped ? 2 : 1;
                ch.label = grouped ? std::string(axisName(a.curves[k].channel, channels)) + " " + property_label : label;
                ch.key = group_key + "|" + std::to_string(a.curves[k].channel);
                ch.curves.push_back({owner.uuid(), k});
                ch.color = channelColor(a.curves[k], channels);
                children.push_back(ch);
            }
            i = j;
        }
        if (owner_curves.empty() && st.only_selected && !owner_selected) continue;
        owner_row.curves = owner_curves;
        st.rows[0].curves.insert(st.rows[0].curves.end(), owner_curves.begin(), owner_curves.end());
        st.rows.push_back(owner_row);
        if (!st.collapsed.contains(owner_row.key)) {
            st.rows.insert(st.rows.end(), children.begin(), children.end());
        }
    }
}

void EditorApp::drawAnimationHeader() {
    KeyframeEditorState& st = keyframeEditor();
    const ecs::Entity active = world_.find(st.active);
    KeyframeAnimation* a = active.valid() ? active.tryGet<KeyframeAnimation>() : nullptr;
    const float fps = animationFps();

    ImGui::SetNextItemWidth(118.0f);
    int mode = st.graph ? 1 : 0;
    static constexpr const char* kModes[] = {"Dope Sheet", "Gráficas"};
    if (ImGui::Combo("##modo", &mode, kModes, 2)) {
        st.graph = mode == 1;
        st.fit_request = true;
    }
    ImGui::SetItemTooltip("Dope Sheet: las claves en el tiempo. Gráficas: las curvas y sus asas");
    ImGui::Checkbox("Solo selección", &st.only_selected);
    ImGui::SetItemTooltip("Solo las animaciones de los objetos seleccionados (como en Blender)");

    if (ImGui::BeginMenu("Vista")) {
        if (ImGui::MenuItem("Encuadrar todo", "Inicio")) st.fit_request = true;
        if (ImGui::MenuItem("Ir al cabezal")) st.view_start = st.time - 1.0f;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Clave")) {
        if (ImGui::MenuItem("Insertar en lo seleccionado", "I (Escena)")) insertSelectionKeys(false);
        if (ImGui::MenuItem("Borrar elegidas", "X")) animationDeleteSelected();
        if (ImGui::MenuItem("Copiar", "Ctrl+C")) animationCopy();
        if (ImGui::MenuItem("Pegar en el cabezal", "Ctrl+V", false, !st.clipboard.empty())) animationPaste();
        ImGui::Separator();
        if (ImGui::BeginMenu("Interpolación (T)")) {
            if (ImGui::MenuItem("Constante")) animationSetInterpolation(Interpolation::Constant);
            if (ImGui::MenuItem("Lineal")) animationSetInterpolation(Interpolation::Linear);
            if (ImGui::MenuItem("Bézier")) animationSetInterpolation(Interpolation::Bezier);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tipo de asa (V)")) {
            if (ImGui::MenuItem("Automática limitada")) animationSetHandles(HandleType::AutoClamped);
            if (ImGui::MenuItem("Automática")) animationSetHandles(HandleType::Auto);
            if (ImGui::MenuItem("Vector")) animationSetHandles(HandleType::Vector);
            if (ImGui::MenuItem("Alineada")) animationSetHandles(HandleType::Aligned);
            if (ImGui::MenuItem("Libre")) animationSetHandles(HandleType::Free);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Extrapolación (Shift+E)")) {
            if (ImGui::MenuItem("Constante")) animationSetExtrapolation(anim::Extrapolation::Constant);
            if (ImGui::MenuItem("Lineal")) animationSetExtrapolation(anim::Extrapolation::Linear);
            if (ImGui::MenuItem("En ciclo")) animationSetExtrapolation(anim::Extrapolation::Cycle);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        static constexpr const char* kInterp[] = {"Constante", "Lineal", "Bézier"};
        int def = static_cast<int>(st.default_interpolation);
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::Combo("Nuevas claves", &def, kInterp, 3)) st.default_interpolation = static_cast<Interpolation>(def);
        ImGui::EndMenu();
    }

    // Grabacion automatica (el circulo rojo de Blender).
    {
        const float h = ImGui::GetFrameHeight();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##autokey", ImVec2(h, h))) st.autokey = !st.autokey;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 c(p.x + h * 0.5f, p.y + h * 0.5f);
        if (st.autokey || ImGui::IsItemHovered()) draw->AddRectFilled(p, ImVec2(p.x + h, p.y + h), theme::kBg4, 4.0f);
        draw->AddCircleFilled(c, h * 0.26f, st.autokey ? kRecord : theme::kTextFaint);
        ImGui::SetItemTooltip(st.autokey ? "Grabación automática: ACTIVADA (lo que edites se graba como clave)"
                                         : "Grabación automática de claves");
    }

    // Transporte: inicio, clave anterior, reproducir, clave siguiente, final.
    const float start = a != nullptr ? a->start : 0.0f;
    const float end = a != nullptr ? a->end : 4.0f;
    const auto transport = [&](const char* id, int icon, const char* tip) {
        const float h = ImGui::GetFrameHeight();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton(id, ImVec2(h, h));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (ImGui::IsItemHovered()) draw->AddRectFilled(p, ImVec2(p.x + h, p.y + h), theme::kBg4, 4.0f);
        const ImU32 col = theme::kText;
        const float s = h * 0.22f;
        const ImVec2 c(p.x + h * 0.5f, p.y + h * 0.5f);
        switch (icon) {
            case 0:  // |<
                draw->AddRectFilled(ImVec2(c.x - s - 2, c.y - s), ImVec2(c.x - s, c.y + s), col);
                draw->AddTriangleFilled(ImVec2(c.x + s, c.y - s), ImVec2(c.x + s, c.y + s), ImVec2(c.x - s + 1, c.y), col);
                break;
            case 1:  // <◆
                draw->AddTriangleFilled(ImVec2(c.x - 1, c.y - s), ImVec2(c.x - 1, c.y + s), ImVec2(c.x - s - 3, c.y), col);
                drawDiamond(draw, ImVec2(c.x + s * 0.7f, c.y), s * 0.8f, col);
                break;
            case 2:  // play / pausa
                if (st.playing) {
                    draw->AddRectFilled(ImVec2(c.x - s, c.y - s), ImVec2(c.x - s * 0.3f, c.y + s), col);
                    draw->AddRectFilled(ImVec2(c.x + s * 0.3f, c.y - s), ImVec2(c.x + s, c.y + s), col);
                } else {
                    draw->AddTriangleFilled(ImVec2(c.x - s * 0.8f, c.y - s), ImVec2(c.x - s * 0.8f, c.y + s),
                                            ImVec2(c.x + s, c.y), col);
                }
                break;
            case 3:  // ◆>
                drawDiamond(draw, ImVec2(c.x - s * 0.7f, c.y), s * 0.8f, col);
                draw->AddTriangleFilled(ImVec2(c.x + 1, c.y - s), ImVec2(c.x + 1, c.y + s), ImVec2(c.x + s + 3, c.y), col);
                break;
            default:  // >|
                draw->AddTriangleFilled(ImVec2(c.x - s, c.y - s), ImVec2(c.x - s, c.y + s), ImVec2(c.x + s - 1, c.y), col);
                draw->AddRectFilled(ImVec2(c.x + s, c.y - s), ImVec2(c.x + s + 2, c.y + s), col);
                break;
        }
        ImGui::SetItemTooltip("%s", tip);
        ImGui::SameLine(0.0f, 2.0f);
        return pressed;
    };
    ImGui::BeginDisabled(playing());
    if (transport("##start", 0, "Al inicio (Mayús+Izquierda)")) st.time = start;
    if (transport("##prevkey", 1, "Clave anterior (Abajo)")) animationJumpKey(false);
    if (transport("##play", 2, "Reproducir / pausa (Espacio)")) st.playing = !st.playing;
    if (transport("##nextkey", 3, "Clave siguiente (Arriba)")) animationJumpKey(true);
    if (transport("##end", 4, "Al final (Mayús+Derecha)")) st.time = end;
    int frame = static_cast<int>(std::lround(st.time * fps));
    ImGui::SetNextItemWidth(70.0f);
    if (ImGui::DragInt("##frame", &frame, 0.2f)) {
        st.time = static_cast<float>(frame) / fps;
        st.playing = false;
    }
    ImGui::SetItemTooltip("Fotograma actual");
    if (a != nullptr) {
        int range[2] = {static_cast<int>(std::lround(a->start * a->fps)), static_cast<int>(std::lround(a->end * a->fps))};
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::DragInt2("##range", range, 0.2f)) {
            range[1] = std::max(range[1], range[0] + 1);
            a->start = static_cast<float>(range[0]) / a->fps;
            a->end = static_cast<float>(range[1]) / a->fps;
            dirty_ = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) commit();
        ImGui::SetItemTooltip("Inicio y fin de \"%s\" (fotogramas)", active.name().c_str());
        float f = a->fps;
        ImGui::SetNextItemWidth(56.0f);
        if (ImGui::DragFloat("##fps", &f, 0.1f, 1.0f, 240.0f, "%.0f fps")) {
            // Las claves se quedan en el mismo segundo; el rango tambien.
            a->fps = std::clamp(f, 1.0f, 240.0f);
            dirty_ = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) commit();
    }
    ImGui::EndDisabled();
}

void EditorApp::drawAnimationWindow() {
    if (console_dock_id_ != 0) ImGui::SetNextWindowDockID(console_dock_id_, ImGuiCond_FirstUseEver);
    KeyframeEditorState& st = keyframeEditor();
    // Animacion activa: la del seleccionado, o la que ya habia.
    if (const ecs::Entity sel = world_.find(active_); sel.valid()) {
        const ecs::Entity owner = animationOwnerFor(sel, false);
        if (owner.valid()) st.active = owner.uuid();
    }
    if (const ecs::Entity e = world_.find(st.active); !e.valid() || !e.has<KeyframeAnimation>()) {
        st.active = {};
        const auto view = world_.registry().view<KeyframeAnimation>();
        if (view.begin() != view.end()) st.active = world_.wrap(*view.begin()).uuid();
    }
    if (!beginArea(AreaEditor::Animation, &show_animation_, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse,
                   [this] { drawAnimationHeader(); })) {
        endArea();
        return;
    }
    buildAnimationRows();
    if (playing()) {
        ImGui::TextDisabled("En Play las animaciones suenan solas (Empezar al dar Play). Para editar claves, para el juego.");
    }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < kChannelWidth + 60.0f || avail.y < kRulerHeight + 30.0f) {
        endArea();
        return;
    }
    const ImVec2 channels_min = origin;
    const ImVec2 channels_max(origin.x + kChannelWidth, origin.y + avail.y);
    const ImVec2 canvas_min(origin.x + kChannelWidth + 1.0f, origin.y);
    const ImVec2 canvas_max(origin.x + avail.x, origin.y + avail.y);

    drawAnimationChannels(channels_min, channels_max);
    drawAnimationCanvas(canvas_min, canvas_max);
    drawAnimationPopups();
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(avail);
    endArea();
}

// Lista de canales: plegar, silenciar, elegir y anadir propiedades.
void EditorApp::drawAnimationChannels(ImVec2 min, ImVec2 max) {
    KeyframeEditorState& st = keyframeEditor();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, theme::kBg1);
    draw->AddLine(ImVec2(max.x, min.y), ImVec2(max.x, max.y), theme::kGap, 1.0f);

    // Cabecera: anadir una propiedad del objeto activo.
    ImGui::SetCursorScreenPos(ImVec2(min.x + 4.0f, min.y + 2.0f));
    const ecs::Entity sel = world_.find(active_);
    ImGui::BeginDisabled(!sel.valid() || playing());
    if (ImGui::Button("+ Propiedad", ImVec2(kChannelWidth - 8.0f, kRulerHeight - 4.0f))) ImGui::OpenPopup("##addprop");
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Animar una propiedad del objeto activo: inserta una clave en el fotograma actual");
    if (ImGui::BeginPopup("##addprop")) {
        if (sel.valid()) {
            ImGui::TextDisabled("%s", sel.name().c_str());
            for (const ecs::ComponentType& type : ecs::ComponentRegistry::instance().types()) {
                if (type.name == "KeyframeAnimation" || !type.has(world_, sel.handle())) continue;
                const std::vector<anim::AnimatableProperty> props = anim::animatableProperties(world_, sel, type.name);
                if (props.empty()) continue;
                if (ImGui::BeginMenu(type.label.c_str())) {
                    for (const anim::AnimatableProperty& p : props) {
                        if (ImGui::MenuItem(p.label.c_str()) && insertKeyframes(sel, type.name, p.path)) commit();
                    }
                    ImGui::EndMenu();
                }
            }
        }
        ImGui::EndPopup();
    }

    const float top = min.y + kRulerHeight;
    const float rows_height = static_cast<float>(st.rows.size()) * kRowHeight;
    const float visible = max.y - top;
    st.scroll_rows = std::clamp(st.scroll_rows, 0.0f, std::max(0.0f, rows_height - visible));
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool hovered = ImGui::IsWindowHovered() && mouse.x >= min.x && mouse.x < max.x && mouse.y >= top && mouse.y < max.y;
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f) st.scroll_rows -= ImGui::GetIO().MouseWheel * kRowHeight * 3.0f;

    draw->PushClipRect(ImVec2(min.x, top), max, true);
    for (std::size_t i = 0; i < st.rows.size(); ++i) {
        const AnimationRow& row = st.rows[i];
        const float y = top + static_cast<float>(i) * kRowHeight - st.scroll_rows;
        if (y + kRowHeight < top || y > max.y) continue;
        const ImVec2 rmin(min.x, y);
        const ImVec2 rmax(max.x, y + kRowHeight);
        const bool active_row = row.key == st.active_row;
        ImU32 bg = row.kind == RowKind::Summary ? theme::kBg3
                   : row.kind == RowKind::Owner ? theme::kBg2
                                                : (i % 2 ? theme::kBg1 : IM_COL32(44, 44, 49, 255));
        if (active_row) bg = theme::kSelection;
        draw->AddRectFilled(rmin, rmax, bg);
        float x = min.x + 6.0f + static_cast<float>(row.depth) * 12.0f;
        const float cy = y + kRowHeight * 0.5f;
        // Flecha de plegar.
        if (row.collapsible) {
            const bool open = !st.collapsed.contains(row.key);
            const float s = 4.0f;
            if (open) draw->AddTriangleFilled(ImVec2(x, cy - s * 0.5f), ImVec2(x + 2 * s, cy - s * 0.5f), ImVec2(x + s, cy + s), theme::kTextDim);
            else draw->AddTriangleFilled(ImVec2(x + s * 0.5f, cy - s), ImVec2(x + s * 0.5f, cy + s), ImVec2(x + 2 * s, cy), theme::kTextDim);
        }
        x += 12.0f;
        if (row.kind == RowKind::Channel) {
            draw->AddRectFilled(ImVec2(x, cy - 4.0f), ImVec2(x + 8.0f, cy + 4.0f), row.color, 2.0f);
            x += 12.0f;
        }
        // Silenciada: texto apagado.
        bool muted = !row.curves.empty();
        for (const AnimationRow::Ref& r : row.curves) {
            const AnimationCurve* c = resolveCurve(world_, r.owner, r.curve);
            if (c != nullptr && !c->muted) muted = false;
        }
        const ImU32 text = muted ? theme::kTextFaint : (row.kind == RowKind::Owner ? theme::kYellow : theme::kText);
        draw->PushClipRect(rmin, ImVec2(max.x - 24.0f, rmax.y), true);
        draw->AddText(ImVec2(x, y + (kRowHeight - ImGui::GetFontSize()) * 0.5f), text, row.label.c_str());
        draw->PopClipRect();
        // Casilla de silenciar (no en el resumen).
        const ImVec2 box(max.x - 18.0f, cy);
        if (row.kind != RowKind::Summary) {
            draw->AddCircle(box, 5.0f, theme::kTextDim, 12, 1.2f);
            if (!muted) draw->AddCircleFilled(box, 3.0f, theme::kTextDim, 12);
        }
        if (!hovered || mouse.y < y || mouse.y >= y + kRowHeight || playing()) continue;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (row.kind != RowKind::Summary && std::abs(mouse.x - box.x) < 8.0f) {
                for (const AnimationRow::Ref& r : row.curves) {
                    if (AnimationCurve* c = resolveCurve(world_, r.owner, r.curve)) c->muted = !muted;
                }
                st.dirty = true;
                commit();
            } else if (row.collapsible && mouse.x < min.x + 18.0f + static_cast<float>(row.depth) * 12.0f) {
                if (!st.collapsed.erase(row.key)) st.collapsed.insert(row.key);
            } else {
                // Elegir el canal: sus claves quedan elegidas (Mayus: se suman).
                st.active_row = row.key;
                if (!ImGui::GetIO().KeyShift) animationSelectAll(false);
                for (const AnimationRow::Ref& r : row.curves) {
                    if (AnimationCurve* c = resolveCurve(world_, r.owner, r.curve)) {
                        for (Keyframe& k : c->keys) k.selected = true;
                    }
                }
                if (row.kind == RowKind::Owner) {
                    st.active = world_.find(Uuid::parse(row.key)).valid() ? Uuid::parse(row.key) : st.active;
                }
            }
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && row.kind != RowKind::Summary) {
            st.active_row = row.key;
            ImGui::OpenPopup("##channelmenu");
        }
    }
    draw->PopClipRect();

    if (ImGui::BeginPopup("##channelmenu")) {
        const AnimationRow* row = nullptr;
        for (const AnimationRow& r : st.rows) {
            if (r.key == st.active_row) row = &r;
        }
        if (row != nullptr) {
            ImGui::TextDisabled("%s", row->label.c_str());
            if (ImGui::BeginMenu("Extrapolación")) {
                const auto set = [&](anim::Extrapolation e) {
                    for (const AnimationRow::Ref& r : row->curves) {
                        if (AnimationCurve* c = resolveCurve(world_, r.owner, r.curve)) {
                            c->extrapolation = e;
                            c->recalcHandles();
                        }
                    }
                    st.dirty = true;
                    commit();
                };
                if (ImGui::MenuItem("Constante")) set(anim::Extrapolation::Constant);
                if (ImGui::MenuItem("Lineal")) set(anim::Extrapolation::Linear);
                if (ImGui::MenuItem("En ciclo (repetir)")) set(anim::Extrapolation::Cycle);
                ImGui::EndMenu();
            }
            if (row->kind == RowKind::Owner && ImGui::MenuItem("Seleccionar el objeto")) {
                selectOnly(Uuid::parse(row->key));
            }
            if (ImGui::MenuItem("Borrar canales")) {
                // De atras adelante por animacion (los indices no se mueven).
                std::vector<AnimationRow::Ref> refs = row->curves;
                std::sort(refs.begin(), refs.end(), [](const AnimationRow::Ref& a, const AnimationRow::Ref& b) {
                    return a.curve > b.curve;
                });
                for (const AnimationRow::Ref& r : refs) {
                    const ecs::Entity e = world_.find(r.owner);
                    if (!e.valid() || !e.has<KeyframeAnimation>()) continue;
                    KeyframeAnimation& owner_animation = e.get<KeyframeAnimation>();
                    auto& curves = owner_animation.curves;
                    if (r.curve < curves.size()) {
                        curves.erase(curves.begin() + static_cast<std::ptrdiff_t>(r.curve));
                        owner_animation.touchCurves();  // la cache del KeyframeSystem se rehace
                    }
                }
                st.dirty = true;
                commit();
            }
        }
        ImGui::EndPopup();
    }
}

// Regla, Dope Sheet o Graficas y toda la interaccion con el raton.
void EditorApp::drawAnimationCanvas(ImVec2 min, ImVec2 max) {
    KeyframeEditorState& st = keyframeEditor();
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float fps = animationFps();
    const ecs::Entity active = world_.find(st.active);
    const KeyframeAnimation* active_anim = active.valid() ? active.tryGet<KeyframeAnimation>() : nullptr;
    const float range_start = active_anim != nullptr ? active_anim->start : 0.0f;
    const float range_end = active_anim != nullptr ? active_anim->end : 4.0f;
    const float width = max.x - min.x;
    const float area_top = min.y + kRulerHeight;
    const float area_h = max.y - area_top;

    // Encuadrar.
    if (st.fit_request) {
        st.fit_request = false;
        float t0 = range_start;
        float t1 = range_end;
        float v0 = 1e30f;
        float v1 = -1e30f;
        for (const AnimationRow::Ref& r : st.rows.empty() ? std::vector<AnimationRow::Ref>{} : st.rows[0].curves) {
            if (const AnimationCurve* c = resolveCurve(world_, r.owner, r.curve)) {
                for (const Keyframe& k : c->keys) {
                    t0 = std::min(t0, k.time);
                    t1 = std::max(t1, k.time);
                    v0 = std::min(v0, k.value);
                    v1 = std::max(v1, k.value);
                }
            }
        }
        const float span = std::max(t1 - t0, 0.1f);
        st.px_per_sec = std::clamp(width * 0.9f / span, 2.0f, 20000.0f);
        st.view_start = t0 - span * 0.05f;
        if (v0 <= v1) {
            const float vs = std::max(v1 - v0, 0.1f);
            st.value_center = (v0 + v1) * 0.5f;
            st.px_per_unit = std::clamp(area_h * 0.8f / vs, 0.001f, 1e6f);
        }
    }

    const auto to_x = [&](float t) { return min.x + (t - st.view_start) * st.px_per_sec; };
    const auto to_t = [&](float x) { return st.view_start + (x - min.x) / st.px_per_sec; };
    const float mid_y = area_top + area_h * 0.5f;
    const auto to_y = [&](float v) { return mid_y - (v - st.value_center) * st.px_per_unit; };
    const auto to_v = [&](float y) { return st.value_center + (mid_y - y) / st.px_per_unit; };
    const auto row_y = [&](std::size_t i) { return area_top + static_cast<float>(i) * kRowHeight - st.scroll_rows; };

    draw->AddRectFilled(min, max, kCanvasBg);
    draw->PushClipRect(min, max, true);

    // Fuera del rango de la animacion activa: mas oscuro.
    draw->AddRectFilled(ImVec2(min.x, area_top), ImVec2(std::max(min.x, to_x(range_start)), max.y), kOutsideRange);
    draw->AddRectFilled(ImVec2(std::min(max.x, to_x(range_end)), area_top), max, kOutsideRange);

    // Cuadricula y regla (fotogramas).
    const float frames_step = std::max(1.0f, niceStep(fps / st.px_per_sec, 60.0f));
    const float minor_frames = frames_step >= 5.0f ? frames_step / 5.0f : 1.0f;
    const float minor_px = minor_frames / fps * st.px_per_sec;
    const float first_frame = std::floor(to_t(min.x) * fps / frames_step) * frames_step;
    for (float f = first_frame; to_x(f / fps) < max.x; f += frames_step) {
        const float x = to_x(f / fps);
        draw->AddLine(ImVec2(x, min.y + 10.0f), ImVec2(x, max.y), kGridMajor);
        char text[16];
        std::snprintf(text, sizeof(text), "%d", static_cast<int>(std::lround(f)));
        draw->AddText(ImVec2(x + 3.0f, min.y + 2.0f), theme::kTextDim, text);
        if (minor_px >= 8.0f) {
            for (float m = minor_frames; m < frames_step - 1e-3f; m += minor_frames) {
                const float xm = to_x((f + m) / fps);
                draw->AddLine(ImVec2(xm, area_top), ImVec2(xm, max.y), kGridMinor);
            }
        }
    }
    if (st.graph) {
        const float vstep = niceStep(1.0f / st.px_per_unit, 40.0f);
        const float first_v = std::floor(to_v(max.y) / vstep) * vstep;
        for (float v = first_v; to_y(v) > area_top; v += vstep) {
            const float y = to_y(v);
            draw->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), std::abs(v) < vstep * 0.01f ? kGridMajor : kGridMinor);
            char text[24];
            std::snprintf(text, sizeof(text), "%g", std::abs(v) < vstep * 0.01f ? 0.0f : v);
            draw->AddText(ImVec2(min.x + 3.0f, y - ImGui::GetFontSize() - 1.0f), theme::kTextFaint, text);
        }
    }
    draw->AddRectFilled(min, ImVec2(max.x, area_top), theme::kBg2);
    draw->AddLine(ImVec2(min.x, area_top), ImVec2(max.x, area_top), theme::kGap);
    for (float f = first_frame; to_x(f / fps) < max.x; f += frames_step) {
        const float x = to_x(f / fps);
        draw->AddLine(ImVec2(x, area_top - 6.0f), ImVec2(x, area_top), theme::kTextFaint);
        char text[16];
        std::snprintf(text, sizeof(text), "%d", static_cast<int>(std::lround(f)));
        draw->AddText(ImVec2(x + 3.0f, min.y + 4.0f), theme::kTextDim, text);
    }

    // --- Contenido ---
    struct Hit {
        Uuid owner;
        std::size_t curve = 0;
        std::size_t key = 0;
        int handle = 0;  // 0 clave, -1 asa izquierda, 1 derecha
        bool valid = false;
    };
    Hit hit;
    float best = kPickRadius * kPickRadius;
    const ImVec2 mouse = io.MousePos;
    const bool mouse_in_area = mouse.x >= min.x && mouse.x < max.x && mouse.y >= area_top && mouse.y < max.y;
    // Dope Sheet: la fila (resumen o grupo) y el tiempo de la clave bajo el raton.
    int hit_row = -1;
    float hit_time = 0.0f;

    if (!st.graph) {
        draw->PushClipRect(ImVec2(min.x, area_top), max, true);
        for (std::size_t i = 0; i < st.rows.size(); ++i) {
            const AnimationRow& row = st.rows[i];
            const float y = row_y(i);
            if (y + kRowHeight < area_top || y > max.y) continue;
            if (row.kind == RowKind::Summary || row.kind == RowKind::Owner) {
                draw->AddRectFilled(ImVec2(min.x, y), ImVec2(max.x, y + kRowHeight), IM_COL32(255, 255, 255, 8));
            }
            // Claves de la fila agrupadas por fotograma.
            std::map<int, bool> frames;  // fotograma -> alguna elegida
            for (const AnimationRow::Ref& r : row.curves) {
                const AnimationCurve* c = resolveCurve(world_, r.owner, r.curve);
                if (c == nullptr) continue;
                for (const Keyframe& k : c->keys) {
                    bool& sel = frames[static_cast<int>(std::lround(k.time * fps * 16.0f))];
                    sel = sel || k.selected;
                }
            }
            const float cy = y + kRowHeight * 0.5f;
            const float r = row.kind == RowKind::Channel ? 5.0f : 6.0f;
            for (const auto& [f, sel] : frames) {
                const float t = static_cast<float>(f) / (fps * 16.0f);
                const float x = to_x(t);
                if (x < min.x - 10.0f || x > max.x + 10.0f) continue;
                drawDiamond(draw, ImVec2(x, cy), r, sel ? kKeySelected : kKey);
                const float dx = mouse.x - x;
                const float dy = mouse.y - cy;
                if (mouse_in_area && dx * dx + dy * dy < best) {
                    best = dx * dx + dy * dy;
                    hit_row = static_cast<int>(i);
                    hit_time = t;
                }
            }
        }
        draw->PopClipRect();
    } else {
        draw->PushClipRect(ImVec2(min.x, area_top), max, true);
        for (const AnimationRow& row : st.rows) {
            if (row.kind != RowKind::Channel) continue;
            const AnimationRow::Ref& r = row.curves.front();
            const AnimationCurve* c = resolveCurve(world_, r.owner, r.curve);
            if (c == nullptr || c->keys.empty()) continue;
            const bool highlighted = st.active_row.empty() || row.key == st.active_row ||
                                     row.key.rfind(st.active_row + "|", 0) == 0;
            const ImU32 col = c->muted ? theme::withAlpha(row.color, 70) : theme::withAlpha(row.color, highlighted ? 255 : 130);
            // La curva, muestreada cada 2 px.
            ImVec2 prev{};
            for (float x = min.x; x <= max.x + 2.0f; x += 2.0f) {
                const ImVec2 p(x, to_y(c->evaluate(to_t(x))));
                if (x > min.x) draw->AddLine(prev, p, col, highlighted ? 1.8f : 1.2f);
                prev = p;
            }
            // Claves y asas (las de las elegidas).
            for (std::size_t k = 0; k < c->keys.size(); ++k) {
                const Keyframe& key = c->keys[k];
                const ImVec2 kp(to_x(key.time), to_y(key.value));
                if (key.selected) {
                    for (const int side : {-1, 1}) {
                        if (!interpolationIs(*c, k, side < 0, Interpolation::Bezier)) continue;
                        const Vec2& h = side < 0 ? key.left : key.right;
                        const ImVec2 hp(to_x(h.x), to_y(h.y));
                        const bool hs = side < 0 ? key.left_selected : key.right_selected;
                        draw->AddLine(kp, hp, kHandleLine, 1.0f);
                        draw->AddCircleFilled(hp, 3.5f, hs ? kKeySelected : IM_COL32(200, 200, 200, 255), 10);
                        const float dx = mouse.x - hp.x;
                        const float dy = mouse.y - hp.y;
                        if (mouse_in_area && dx * dx + dy * dy < best) {
                            best = dx * dx + dy * dy;
                            hit = Hit{r.owner, r.curve, k, side, true};
                        }
                    }
                }
                draw->AddRectFilled(ImVec2(kp.x - 3.5f, kp.y - 3.5f), ImVec2(kp.x + 3.5f, kp.y + 3.5f),
                                    key.selected ? kKeySelected : IM_COL32(20, 20, 20, 255));
                draw->AddRect(ImVec2(kp.x - 3.5f, kp.y - 3.5f), ImVec2(kp.x + 3.5f, kp.y + 3.5f),
                              key.selected ? kKeySelected : col);
                const float dx = mouse.x - kp.x;
                const float dy = mouse.y - kp.y;
                // Las claves ganan a las asas a igual distancia.
                if (mouse_in_area && dx * dx + dy * dy < best + 4.0f) {
                    best = dx * dx + dy * dy;
                    hit = Hit{r.owner, r.curve, k, 0, true};
                }
            }
        }
        draw->PopClipRect();
    }

    // Cabezal.
    {
        const float x = to_x(st.time);
        draw->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), kPlayhead, 2.0f);
        char text[16];
        std::snprintf(text, sizeof(text), "%d", static_cast<int>(std::lround(st.time * fps)));
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const ImVec2 b0(x - ts.x * 0.5f - 5.0f, min.y + 2.0f);
        const ImVec2 b1(x + ts.x * 0.5f + 5.0f, area_top - 2.0f);
        draw->AddRectFilled(b0, b1, kPlayhead, 4.0f);
        draw->AddText(ImVec2(x - ts.x * 0.5f, min.y + (kRulerHeight - ts.y) * 0.5f), theme::kText, text);
    }

    // Caja de seleccion.
    if (st.drag == KeyframeEditorState::Drag::Box) {
        const ImVec2 a(std::min(st.press.x, mouse.x), std::min(st.press.y, mouse.y));
        const ImVec2 b(std::max(st.press.x, mouse.x), std::max(st.press.y, mouse.y));
        draw->AddRectFilled(a, b, kBoxFill);
        draw->AddRect(a, b, kBoxLine);
    }
    if (st.rows.size() <= 1) {
        const char* hint = st.only_selected ? "Sin animación en la selección. Pulsa I sobre una propiedad del Inspector,\n"
                                              "I en la Escena o + Propiedad para crear la primera clave."
                                            : "No hay animaciones en la escena.";
        draw->AddText(ImVec2(min.x + 16.0f, area_top + 30.0f), theme::kTextFaint, hint);
    }
    draw->PopClipRect();

    // --- Raton ---
    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton("##animcanvas", ImVec2(width, max.y - min.y),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                               ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool editing = !playing();

    // Zoom y desplazamiento.
    if (hovered && io.MouseWheel != 0.0f) {
        const float factor = std::pow(1.15f, io.MouseWheel);
        if (st.graph && io.KeyCtrl) {
            const float v = to_v(mouse.y);
            st.px_per_unit = std::clamp(st.px_per_unit * factor, 1e-4f, 1e6f);
            st.value_center = v - (mid_y - mouse.y) / st.px_per_unit;
        } else if (!st.graph && io.KeyCtrl) {
            st.scroll_rows -= io.MouseWheel * kRowHeight * 3.0f;
        } else {
            const float t = to_t(mouse.x);
            st.px_per_sec = std::clamp(st.px_per_sec * factor, 2.0f, 20000.0f);
            st.view_start = t - (mouse.x - min.x) / st.px_per_sec;
            if (st.graph && !io.KeyShift) {
                const float v = to_v(mouse.y);
                st.px_per_unit = std::clamp(st.px_per_unit * factor, 1e-4f, 1e6f);
                st.value_center = v - (mid_y - mouse.y) / st.px_per_unit;
            }
        }
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) st.drag = KeyframeEditorState::Drag::Pan;
    if (st.drag == KeyframeEditorState::Drag::Pan) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            st.drag = KeyframeEditorState::Drag::None;
        } else {
            st.view_start -= io.MouseDelta.x / st.px_per_sec;
            if (st.graph) st.value_center += io.MouseDelta.y / st.px_per_unit;
            else st.scroll_rows -= io.MouseDelta.y;
        }
    }

    // Pulsar.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        st.press = mouse;
        st.last_mouse = mouse;
        if (mouse.y < area_top) {
            st.drag = KeyframeEditorState::Drag::Scrub;
            st.playing = false;
        } else if (editing) {
            const bool additive = io.KeyShift;
            st.press_on_key = false;
            if (st.graph && hit.valid) {
                AnimationCurve* c = resolveCurve(world_, hit.owner, hit.curve);
                Keyframe& k = c->keys[hit.key];
                if (hit.handle != 0) {
                    if (!additive) animationSelectAll(false);
                    k.selected = true;
                    (hit.handle < 0 ? k.left_selected : k.right_selected) = true;
                    st.drag = KeyframeEditorState::Drag::Handle;
                    st.handle_owner = hit.owner;
                    st.handle_curve = hit.curve;
                    st.handle_key = hit.key;
                    st.handle_left = hit.handle < 0;
                } else {
                    if (additive) {
                        k.selected = !k.selected;
                    } else if (!k.selected) {
                        animationSelectAll(false);
                        k.selected = true;
                    }
                    st.press_on_key = k.selected;
                    st.drag = KeyframeEditorState::Drag::Pending;
                }
            } else if (!st.graph && hit_row >= 0) {
                // Todas las claves de la fila en ese fotograma.
                const AnimationRow& row = st.rows[static_cast<std::size_t>(hit_row)];
                bool all_selected = true;
                std::vector<Keyframe*> keys;
                for (const AnimationRow::Ref& r : row.curves) {
                    if (AnimationCurve* c = resolveCurve(world_, r.owner, r.curve)) {
                        for (Keyframe& k : c->keys) {
                            if (std::abs(k.time - hit_time) < 0.5f / (fps * 16.0f)) {
                                keys.push_back(&k);
                                all_selected = all_selected && k.selected;
                            }
                        }
                    }
                }
                if (additive) {
                    for (Keyframe* k : keys) k->selected = !all_selected;
                } else if (!all_selected) {
                    animationSelectAll(false);
                    for (Keyframe* k : keys) k->selected = true;
                }
                st.press_on_key = additive ? !all_selected : true;
                st.drag = KeyframeEditorState::Drag::Pending;
            } else {
                st.drag = KeyframeEditorState::Drag::Box;
            }
        }
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && editing && mouse.y >= area_top) {
        ImGui::OpenPopup("##keymenu");
    }

    // Arrastrar.
    switch (st.drag) {
        case KeyframeEditorState::Drag::Scrub:
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) st.drag = KeyframeEditorState::Drag::None;
            else st.time = snapTime(to_t(mouse.x), fps);
            break;
        case KeyframeEditorState::Drag::Pending:
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                st.drag = KeyframeEditorState::Drag::None;
            } else if (st.press_on_key && std::abs(mouse.x - st.press.x) + std::abs(mouse.y - st.press.y) > 3.0f) {
                animationBeginMove();
                st.drag = KeyframeEditorState::Drag::Keys;
            }
            break;
        case KeyframeEditorState::Drag::Keys: {
            float dt = (mouse.x - st.press.x) / st.px_per_sec;
            if (!io.KeyCtrl) dt = snapTime(dt, fps);  // Ctrl: sin ajustar a fotogramas
            float dv = st.graph ? (st.press.y - mouse.y) / st.px_per_unit : 0.0f;
            // Mayus en Graficas: solo en un eje (el que mas se movio).
            if (st.graph && io.KeyShift) {
                if (std::abs(mouse.x - st.press.x) > std::abs(mouse.y - st.press.y)) dv = 0.0f;
                else dt = 0.0f;
            }
            animationApplyMove(dt, dv);
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                st.drag = KeyframeEditorState::Drag::None;
                st.moving.clear();
                commit();
            }
            break;
        }
        case KeyframeEditorState::Drag::Handle: {
            AnimationCurve* c = resolveCurve(world_, st.handle_owner, st.handle_curve);
            if (c != nullptr && st.handle_key < c->keys.size()) {
                Keyframe& k = c->keys[st.handle_key];
                Vec2 h{to_t(mouse.x), to_v(mouse.y)};
                if (st.handle_left) h.x = std::min(h.x, k.time);
                else h.x = std::max(h.x, k.time);
                // Como Blender: al mover un asa automatica pasa a alineada; una vector, a libre.
                const auto convert = [](HandleType t) {
                    return t == HandleType::Auto || t == HandleType::AutoClamped ? HandleType::Aligned
                           : t == HandleType::Vector                             ? HandleType::Free
                                                                                 : t;
                };
                k.left_type = convert(k.left_type);
                k.right_type = convert(k.right_type);
                Vec2& moved = st.handle_left ? k.left : k.right;
                Vec2& other = st.handle_left ? k.right : k.left;
                moved = h;
                const HandleType other_type = st.handle_left ? k.right_type : k.left_type;
                if (other_type == HandleType::Aligned) {
                    const Vec2 d{moved.x - k.time, moved.y - k.value};
                    const float dl = std::sqrt(d.x * d.x + d.y * d.y);
                    const Vec2 o{other.x - k.time, other.y - k.value};
                    const float ol = std::sqrt(o.x * o.x + o.y * o.y);
                    if (dl > 1e-6f) other = Vec2{k.time - d.x / dl * ol, k.value - d.y / dl * ol};
                }
                c->recalcHandles();
                st.dirty = true;
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                st.drag = KeyframeEditorState::Drag::None;
                commit();
            }
            break;
        }
        case KeyframeEditorState::Drag::Box:
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                st.drag = KeyframeEditorState::Drag::None;
                const ImVec2 a(std::min(st.press.x, mouse.x), std::min(st.press.y, mouse.y));
                const ImVec2 b(std::max(st.press.x, mouse.x), std::max(st.press.y, mouse.y));
                const bool tiny = b.x - a.x < 3.0f && b.y - a.y < 3.0f;
                if (!io.KeyShift) animationSelectAll(false);
                if (tiny) break;  // clic en vacio: solo deselecciona
                const auto inside = [&](float x, float y) { return x >= a.x && x <= b.x && y >= a.y && y <= b.y; };
                if (st.graph) {
                    for (const AnimationRow& row : st.rows) {
                        if (row.kind != RowKind::Channel) continue;
                        AnimationCurve* c = resolveCurve(world_, row.curves.front().owner, row.curves.front().curve);
                        if (c == nullptr) continue;
                        for (Keyframe& k : c->keys) {
                            if (inside(to_x(k.time), to_y(k.value))) k.selected = true;
                        }
                    }
                } else {
                    for (std::size_t i = 0; i < st.rows.size(); ++i) {
                        const float cy = row_y(i) + kRowHeight * 0.5f;
                        if (cy < a.y || cy > b.y) continue;
                        for (const AnimationRow::Ref& r : st.rows[i].curves) {
                            if (AnimationCurve* c = resolveCurve(world_, r.owner, r.curve)) {
                                for (Keyframe& k : c->keys) {
                                    if (inside(to_x(k.time), cy)) k.selected = true;
                                }
                            }
                        }
                    }
                }
            }
            break;
        default: break;
    }

    // Menu del boton derecho.
    if (ImGui::BeginPopup("##keymenu")) {
        if (ImGui::MenuItem("Insertar clave en lo seleccionado", "I")) insertSelectionKeys(false);
        if (ImGui::MenuItem("Copiar", "Ctrl+C")) animationCopy();
        if (ImGui::MenuItem("Pegar en el cabezal", "Ctrl+V", false, !st.clipboard.empty())) animationPaste();
        if (ImGui::MenuItem("Borrar", "X")) animationDeleteSelected();
        ImGui::Separator();
        if (ImGui::MenuItem("Constante")) animationSetInterpolation(Interpolation::Constant);
        if (ImGui::MenuItem("Lineal")) animationSetInterpolation(Interpolation::Linear);
        if (ImGui::MenuItem("Bézier")) animationSetInterpolation(Interpolation::Bezier);
        ImGui::Separator();
        if (ImGui::MenuItem("Asas automáticas")) animationSetHandles(HandleType::AutoClamped);
        if (ImGui::MenuItem("Asas vector")) animationSetHandles(HandleType::Vector);
        if (ImGui::MenuItem("Asas libres")) animationSetHandles(HandleType::Free);
        ImGui::EndPopup();
    }

    // --- Teclado (con el raton sobre la ventana) ---
    const bool window_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    if (!window_hovered || io.WantTextInput || ImGui::IsAnyItemActive()) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && editing) st.playing = !st.playing;
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) st.fit_request = true;
    if (editing) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) st.time = io.KeyShift ? range_start : snapTime(st.time, fps) - 1.0f / fps;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) st.time = io.KeyShift ? range_end : snapTime(st.time, fps) + 1.0f / fps;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) animationJumpKey(true);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) animationJumpKey(false);
        if (ImGui::IsKeyPressed(ImGuiKey_A, false) && !io.KeyCtrl) animationSelectAll(!io.KeyAlt);
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || (ImGui::IsKeyPressed(ImGuiKey_X, false) && !io.KeyCtrl)) {
            animationDeleteSelected();
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) animationCopy();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) animationPaste();
        if (ImGui::IsKeyPressed(ImGuiKey_T, false)) ImGui::OpenPopup("##interp");
        if (ImGui::IsKeyPressed(ImGuiKey_V, false) && !io.KeyCtrl) ImGui::OpenPopup("##handles");
        if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_E, false)) ImGui::OpenPopup("##extrap");
        if (ImGui::IsKeyPressed(ImGuiKey_I, false) && !io.KeyCtrl) insertSelectionKeys(io.KeyAlt);
    }
}

// Los popups de T, V y Mayus+E (fuera del return temprano del teclado).
void EditorApp::drawAnimationPopups() {
    if (ImGui::BeginPopup("##interp")) {
        ImGui::TextDisabled("Interpolación");
        if (ImGui::MenuItem("Constante")) animationSetInterpolation(Interpolation::Constant);
        if (ImGui::MenuItem("Lineal")) animationSetInterpolation(Interpolation::Linear);
        if (ImGui::MenuItem("Bézier")) animationSetInterpolation(Interpolation::Bezier);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##handles")) {
        ImGui::TextDisabled("Tipo de asa");
        if (ImGui::MenuItem("Automática limitada")) animationSetHandles(HandleType::AutoClamped);
        if (ImGui::MenuItem("Automática")) animationSetHandles(HandleType::Auto);
        if (ImGui::MenuItem("Vector")) animationSetHandles(HandleType::Vector);
        if (ImGui::MenuItem("Alineada")) animationSetHandles(HandleType::Aligned);
        if (ImGui::MenuItem("Libre")) animationSetHandles(HandleType::Free);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##extrap")) {
        ImGui::TextDisabled("Extrapolación");
        if (ImGui::MenuItem("Constante")) animationSetExtrapolation(anim::Extrapolation::Constant);
        if (ImGui::MenuItem("Lineal")) animationSetExtrapolation(anim::Extrapolation::Linear);
        if (ImGui::MenuItem("En ciclo")) animationSetExtrapolation(anim::Extrapolation::Cycle);
        ImGui::EndPopup();
    }
}

// -----------------------------------------------------------------------------
// Operaciones sobre las claves elegidas
// -----------------------------------------------------------------------------

template <typename Fn>
void EditorApp::forEachAnimationCurve(Fn&& fn) {
    for (const entt::entity handle : world_.registry().view<KeyframeAnimation>()) {
        KeyframeAnimation& a = world_.registry().get<KeyframeAnimation>(handle);
        const Uuid owner = world_.wrap(handle).uuid();
        for (std::size_t i = 0; i < a.curves.size(); ++i) fn(owner, i, a.curves[i]);
    }
}

void EditorApp::animationSelectAll(bool select) {
    forEachAnimationCurve([&](const Uuid&, std::size_t, AnimationCurve& c) {
        for (Keyframe& k : c.keys) {
            k.selected = select;
            k.left_selected = k.right_selected = false;
        }
    });
}

void EditorApp::animationBeginMove() {
    KeyframeEditorState& st = keyframeEditor();
    st.moving.clear();
    forEachAnimationCurve([&](const Uuid& owner, std::size_t index, AnimationCurve& c) {
        KeyframeEditorState::Moving m{owner, index, {}};
        for (const Keyframe& k : c.keys) {
            if (k.selected) m.original.push_back(k);
        }
        if (!m.original.empty()) st.moving.push_back(std::move(m));
    });
}

// Las elegidas conservan su orden entre ellas al moverse juntas: la i-esima
// elegida de la curva es la i-esima original.
void EditorApp::animationApplyMove(float dt, float dv) {
    KeyframeEditorState& st = keyframeEditor();
    for (const KeyframeEditorState::Moving& m : st.moving) {
        AnimationCurve* c = resolveCurve(world_, m.owner, m.curve);
        if (c == nullptr) continue;
        std::size_t n = 0;
        for (Keyframe& k : c->keys) {
            if (!k.selected || n >= m.original.size()) continue;
            const Keyframe& o = m.original[n++];
            k.time = o.time + dt;
            k.value = o.value + dv;
            k.left = Vec2{o.left.x + dt, o.left.y + dv};
            k.right = Vec2{o.right.x + dt, o.right.y + dv};
        }
        c->sort();
    }
    st.dirty = true;
}

void EditorApp::animationDeleteSelected() {
    bool any = false;
    for (const entt::entity handle : world_.registry().view<KeyframeAnimation>()) {
        KeyframeAnimation& a = world_.registry().get<KeyframeAnimation>(handle);
        for (std::size_t i = a.curves.size(); i-- > 0;) {
            AnimationCurve& c = a.curves[i];
            const std::size_t before = c.keys.size();
            std::erase_if(c.keys, [](const Keyframe& k) { return k.selected; });
            if (c.keys.size() == before) continue;
            any = true;
            // Como Blender: la curva sin claves desaparece.
            if (c.keys.empty()) {
                a.curves.erase(a.curves.begin() + static_cast<std::ptrdiff_t>(i));
                a.touchCurves();  // la cache del KeyframeSystem se rehace
            } else {
                c.recalcHandles();
            }
        }
    }
    if (!any) return;
    keyframeEditor().dirty = true;
    commit();
}

void EditorApp::animationSetInterpolation(Interpolation mode) {
    bool any = false;
    forEachAnimationCurve([&](const Uuid&, std::size_t, AnimationCurve& c) {
        for (Keyframe& k : c.keys) {
            if (!k.selected) continue;
            k.interpolation = mode;
            any = true;
        }
        c.recalcHandles();
    });
    keyframeEditor().default_interpolation = mode;
    if (!any) return;
    keyframeEditor().dirty = true;
    commit();
}

void EditorApp::animationSetHandles(HandleType type) {
    bool any = false;
    forEachAnimationCurve([&](const Uuid&, std::size_t, AnimationCurve& c) {
        bool touched = false;
        for (Keyframe& k : c.keys) {
            if (!k.selected) continue;
            k.left_type = k.right_type = type;
            touched = true;
        }
        if (touched) c.recalcHandles();
        any |= touched;
    });
    if (!any) return;
    keyframeEditor().dirty = true;
    commit();
}

void EditorApp::animationSetExtrapolation(anim::Extrapolation mode) {
    bool any = false;
    forEachAnimationCurve([&](const Uuid&, std::size_t, AnimationCurve& c) {
        if (std::none_of(c.keys.begin(), c.keys.end(), [](const Keyframe& k) { return k.selected; })) return;
        c.extrapolation = mode;
        c.recalcHandles();
        any = true;
    });
    if (!any) return;
    keyframeEditor().dirty = true;
    commit();
}

void EditorApp::animationCopy() {
    KeyframeEditorState& st = keyframeEditor();
    std::vector<KeyframeEditorState::Clip> clips;
    float first = 1e30f;
    forEachAnimationCurve([&](const Uuid& owner, std::size_t, AnimationCurve& c) {
        for (const Keyframe& k : c.keys) {
            if (!k.selected) continue;
            clips.push_back({owner, c.target, c.component, c.property, c.channel, k});
            first = std::min(first, k.time);
        }
    });
    if (clips.empty()) return;
    for (KeyframeEditorState::Clip& clip : clips) {
        clip.key.time -= first;
        clip.key.left.x -= first;
        clip.key.right.x -= first;
    }
    st.clipboard = std::move(clips);
}

void EditorApp::animationPaste() {
    KeyframeEditorState& st = keyframeEditor();
    if (st.clipboard.empty() || playing()) return;
    animationSelectAll(false);
    bool any = false;
    for (const KeyframeEditorState::Clip& clip : st.clipboard) {
        const ecs::Entity owner = world_.find(clip.owner);
        if (!owner.valid() || !owner.has<KeyframeAnimation>()) continue;
        KeyframeAnimation& a = owner.get<KeyframeAnimation>();
        const float at = snapTime(st.time, a.fps);
        AnimationCurve& c = a.curve(clip.target, clip.component, clip.property, clip.channel);
        Keyframe k = clip.key;
        k.time += at;
        k.left.x += at;
        k.right.x += at;
        k.selected = true;
        if (const int existing = c.find(k.time, 0.5f / a.fps); existing >= 0) {
            c.keys[static_cast<std::size_t>(existing)] = k;
        } else {
            c.keys.push_back(k);
        }
        c.sort();
        any = true;
    }
    if (!any) return;
    st.dirty = true;
    commit();
}

void EditorApp::animationJumpKey(bool forward) {
    KeyframeEditorState& st = keyframeEditor();
    const float fps = animationFps();
    const float now = snapTime(st.time, fps);
    const float eps = 0.25f / fps;
    float best = forward ? 1e30f : -1e30f;
    for (const AnimationRow::Ref& r : st.rows.empty() ? std::vector<AnimationRow::Ref>{} : st.rows[0].curves) {
        const AnimationCurve* c = resolveCurve(world_, r.owner, r.curve);
        if (c == nullptr) continue;
        for (const Keyframe& k : c->keys) {
            if (forward && k.time > now + eps) best = std::min(best, k.time);
            if (!forward && k.time < now - eps) best = std::max(best, k.time);
        }
    }
    if (std::abs(best) < 1e29f) {
        st.time = best;
        st.playing = false;
    }
}

}  // namespace cramion::editor
