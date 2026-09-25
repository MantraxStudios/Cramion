#include "PropertyInspector.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <cmath>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <algorithm>

namespace cramion::editor {

namespace {

// Ancho de la columna de etiquetas, como el Inspector de Unity.
float labelWidth() {
    return std::max(ImGui::GetContentRegionAvail().x * 0.38f, 90.0f);
}

// Lo que muestra un campo con valores distintos (como Unity).
constexpr const char* kMixed = "\xe2\x80\x94";  // "—"

FieldValue makeValue(FieldValue::Kind kind) {
    FieldValue v;
    v.kind = kind;
    return v;
}

}  // namespace

// -----------------------------------------------------------------------------
// Rutas de los campos
// -----------------------------------------------------------------------------

std::string FieldPath::pathOf(const char* key) const {
    std::string path;
    for (const std::string& part : stack_) {
        path += part;
        path += '/';
    }
    path += key != nullptr ? key : "";
    return path;
}

// -----------------------------------------------------------------------------
// Leer los valores de una entidad
// -----------------------------------------------------------------------------

bool CollectFieldsVisitor::beginGroup(const char* label, bool) {
    push(label != nullptr ? label : "");
    return true;
}

bool CollectFieldsVisitor::field(const ecs::Meta& meta, float& value, const ecs::FloatRange&) {
    FieldValue v = makeValue(FieldValue::Kind::Float);
    v.f = value;
    out_[pathOf(meta.key)] = v;
    return false;
}

bool CollectFieldsVisitor::field(const ecs::Meta& meta, int& value, int, int) {
    FieldValue v = makeValue(FieldValue::Kind::Int);
    v.i = value;
    out_[pathOf(meta.key)] = v;
    return false;
}

bool CollectFieldsVisitor::field(const ecs::Meta& meta, bool& value) {
    FieldValue v = makeValue(FieldValue::Kind::Bool);
    v.b = value;
    out_[pathOf(meta.key)] = v;
    return false;
}

bool CollectFieldsVisitor::field(const ecs::Meta& meta, std::string& value) {
    FieldValue v = makeValue(FieldValue::Kind::String);
    v.s = value;
    out_[pathOf(meta.key)] = std::move(v);
    return false;
}

bool CollectFieldsVisitor::field(const ecs::Meta& meta, core::Vec3& value, ecs::Vec3Kind) {
    FieldValue v = makeValue(FieldValue::Kind::Vec3);
    v.v3 = value;
    out_[pathOf(meta.key)] = v;
    return false;
}

bool CollectFieldsVisitor::field(const ecs::Meta& meta, core::Vec2& value, float) {
    FieldValue v = makeValue(FieldValue::Kind::Vec2);
    v.v2 = value;
    out_[pathOf(meta.key)] = v;
    return false;
}

bool CollectFieldsVisitor::enumeration(const ecs::Meta& meta, int& value, std::span<const char* const>) {
    FieldValue v = makeValue(FieldValue::Kind::Enum);
    v.i = value;
    out_[pathOf(meta.key)] = v;
    return false;
}

bool CollectFieldsVisitor::asset(const ecs::Meta& meta, assets::AssetRef& ref, assets::AssetType) {
    FieldValue v = makeValue(FieldValue::Kind::Asset);
    v.asset = ref;
    out_[pathOf(meta.key)] = v;
    return false;
}

bool CollectFieldsVisitor::layerMask(const ecs::Meta& meta, std::uint32_t& mask) {
    FieldValue v = makeValue(FieldValue::Kind::Mask);
    v.mask = mask;
    out_[pathOf(meta.key)] = v;
    return false;
}

bool CollectFieldsVisitor::entity(const ecs::Meta& meta, Uuid& ref) {
    FieldValue v = makeValue(FieldValue::Kind::Entity);
    v.uuid = ref;
    out_[pathOf(meta.key)] = v;
    return false;
}

bool CollectFieldsVisitor::beginList(const ecs::Meta& meta, std::size_t& count) {
    FieldValue v = makeValue(FieldValue::Kind::ListCount);
    v.count = count;
    out_[pathOf(meta.key)] = v;
    push(meta.key);
    return true;
}

bool CollectFieldsVisitor::beginListItem(std::size_t index) {
    push(std::to_string(index));
    return true;
}

// -----------------------------------------------------------------------------
// Copiar un campo a otra entidad
// -----------------------------------------------------------------------------

bool ApplyFieldVisitor::beginGroup(const char* label, bool) {
    push(label != nullptr ? label : "");
    return true;
}

bool ApplyFieldVisitor::field(const ecs::Meta& meta, float& value, const ecs::FloatRange&) {
    if (!matches(meta.key, FieldValue::Kind::Float) || value == value_.f) return false;
    value = value_.f;
    return true;
}

bool ApplyFieldVisitor::field(const ecs::Meta& meta, int& value, int, int) {
    if (!matches(meta.key, FieldValue::Kind::Int) || value == value_.i) return false;
    value = value_.i;
    return true;
}

bool ApplyFieldVisitor::field(const ecs::Meta& meta, bool& value) {
    if (!matches(meta.key, FieldValue::Kind::Bool) || value == value_.b) return false;
    value = value_.b;
    return true;
}

bool ApplyFieldVisitor::field(const ecs::Meta& meta, std::string& value) {
    if (!matches(meta.key, FieldValue::Kind::String) || value == value_.s) return false;
    value = value_.s;
    return true;
}

bool ApplyFieldVisitor::field(const ecs::Meta& meta, core::Vec3& value, ecs::Vec3Kind kind) {
    if (!matches(meta.key, FieldValue::Kind::Vec3)) return false;
    const core::Vec3 before = value;
    if (value_.axes & 1) value.x = value_.v3.x;
    if (value_.axes & 2) value.y = value_.v3.y;
    if (value_.axes & 4) value.z = value_.v3.z;
    if (kind == ecs::Vec3Kind::Direction) {
        const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
        if (length > 1e-5f) value = value * (1.0f / length);
    }
    return value.x != before.x || value.y != before.y || value.z != before.z;
}

bool ApplyFieldVisitor::field(const ecs::Meta& meta, core::Vec2& value, float) {
    if (!matches(meta.key, FieldValue::Kind::Vec2)) return false;
    const core::Vec2 before = value;
    if (value_.axes & 1) value.x = value_.v2.x;
    if (value_.axes & 2) value.y = value_.v2.y;
    return value.x != before.x || value.y != before.y;
}

bool ApplyFieldVisitor::enumeration(const ecs::Meta& meta, int& value, std::span<const char* const> names) {
    if (!matches(meta.key, FieldValue::Kind::Enum) || value == value_.i) return false;
    if (value_.i < 0 || value_.i >= static_cast<int>(names.size())) return false;
    value = value_.i;
    return true;
}

bool ApplyFieldVisitor::asset(const ecs::Meta& meta, assets::AssetRef& ref, assets::AssetType type) {
    if (!matches(meta.key, FieldValue::Kind::Asset) || ref.uuid == value_.asset.uuid) return false;
    ref.uuid = value_.asset.uuid;
    ref.type = type;
    return true;
}

bool ApplyFieldVisitor::layerMask(const ecs::Meta& meta, std::uint32_t& mask) {
    if (!matches(meta.key, FieldValue::Kind::Mask) || mask == value_.mask) return false;
    mask = value_.mask;
    return true;
}

bool ApplyFieldVisitor::entity(const ecs::Meta& meta, Uuid& ref) {
    if (!matches(meta.key, FieldValue::Kind::Entity) || ref == value_.uuid) return false;
    ref = value_.uuid;
    return true;
}

bool ApplyFieldVisitor::beginList(const ecs::Meta& meta, std::size_t& count) {
    const std::string path = pathOf(meta.key);
    if (value_.kind == FieldValue::Kind::ListCount && path == path_) count = value_.count;
    lists_.push_back(path);
    push(meta.key);
    return true;
}

bool ApplyFieldVisitor::beginListItem(std::size_t index) {
    push(std::to_string(index));
    return true;
}

int ApplyFieldVisitor::endList() {
    pop();
    const bool remove = value_.kind == FieldValue::Kind::ListRemove && !lists_.empty() && lists_.back() == path_;
    if (!lists_.empty()) lists_.pop_back();
    return remove ? value_.i : -1;
}

// -----------------------------------------------------------------------------
// Campos distintos
// -----------------------------------------------------------------------------

MixedFields mixedFields(const std::vector<FieldValues>& values) {
    MixedFields mixed;
    if (values.size() < 2) return mixed;
    for (const auto& [path, first] : values[0]) {
        int axes = 0;
        for (std::size_t n = 1; n < values.size(); ++n) {
            const auto it = values[n].find(path);
            if (it == values[n].end()) continue;  // no lo tiene visible
            const FieldValue& other = it->second;
            if (other.kind != first.kind) {
                axes |= 1;
                continue;
            }
            switch (first.kind) {
                case FieldValue::Kind::Float: axes |= first.f != other.f ? 1 : 0; break;
                case FieldValue::Kind::Int:
                case FieldValue::Kind::Enum: axes |= first.i != other.i ? 1 : 0; break;
                case FieldValue::Kind::Bool: axes |= first.b != other.b ? 1 : 0; break;
                case FieldValue::Kind::String: axes |= first.s != other.s ? 1 : 0; break;
                case FieldValue::Kind::Vec3:
                    axes |= (first.v3.x != other.v3.x ? 1 : 0) | (first.v3.y != other.v3.y ? 2 : 0) |
                            (first.v3.z != other.v3.z ? 4 : 0);
                    break;
                case FieldValue::Kind::Vec2:
                    axes |= (first.v2.x != other.v2.x ? 1 : 0) | (first.v2.y != other.v2.y ? 2 : 0);
                    break;
                case FieldValue::Kind::Asset: axes |= !(first.asset.uuid == other.asset.uuid) ? 1 : 0; break;
                case FieldValue::Kind::Mask: axes |= first.mask != other.mask ? 1 : 0; break;
                case FieldValue::Kind::Entity: axes |= !(first.uuid == other.uuid) ? 1 : 0; break;
                case FieldValue::Kind::ListCount: axes |= first.count != other.count ? 1 : 0; break;
                case FieldValue::Kind::ListRemove: break;
            }
        }
        if (axes != 0) mixed[path] = axes;
    }
    return mixed;
}

// -----------------------------------------------------------------------------
// Inspector (ImGui)
// -----------------------------------------------------------------------------

int ImGuiPropertyVisitor::mixed(const char* key) const {
    if (mixed_ == nullptr) return 0;
    const auto it = mixed_->find(pathOf(key));
    return it == mixed_->end() ? 0 : it->second;
}

// Etiqueta a la izquierda y control a la derecha. Devuelve true siempre (el
// control va a continuacion con su ID oculto).
bool ImGuiPropertyVisitor::label(const ecs::Meta& meta) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(meta.label);
    if (meta.tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s", meta.tooltip);
    }
    ImGui::SameLine(labelWidth());
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::PushID(meta.key);
    return true;
}

void ImGuiPropertyVisitor::finishItem() {
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        edit_finished_ = true;
    }
    ImGui::PopID();
}

// Varios DragFloat en una fila (x, y, z), cada eje con "—" si es distinto
// entre los objetos seleccionados.
bool ImGuiPropertyVisitor::dragAxes(float* values, int count, float speed, float min, float max,
                                    const char* format, int mixed_axes, int& changed_axes) {
    changed_axes = 0;
    const float full = ImGui::GetContentRegionAvail().x;
    ImGui::GetCurrentContext()->NextItemData.ClearFlags();  // el -1 de label()
    ImGui::PushMultiItemsWidths(count, full);
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        ImGui::PushID(i);
        if (i > 0) ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        const char* fmt = (mixed_axes >> i) & 1 ? kMixed : format;
        if (ImGui::DragFloat("##a", &values[i], speed, min, max, fmt)) {
            changed = true;
            changed_axes |= 1 << i;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) edit_finished_ = true;
        ImGui::PopID();
        ImGui::PopItemWidth();
    }
    return changed;
}

bool ImGuiPropertyVisitor::beginGroup(const char* text, bool default_open) {
    ImGui::PushID(text);
    const bool open = ImGui::TreeNodeEx(text, (default_open ? ImGuiTreeNodeFlags_DefaultOpen : 0) |
                                                  ImGuiTreeNodeFlags_SpanAvailWidth |
                                                  ImGuiTreeNodeFlags_FramePadding);
    if (!open) {
        ImGui::PopID();
        return false;
    }
    push(text != nullptr ? text : "");
    ++depth_;
    return true;
}

void ImGuiPropertyVisitor::endGroup() {
    ImGui::TreePop();
    ImGui::PopID();
    pop();
    --depth_;
}

bool ImGuiPropertyVisitor::field(const ecs::Meta& meta, float& value, const ecs::FloatRange& range) {
    const char* format = mixed(meta.key) ? kMixed : range.format;
    label(meta);
    bool changed = false;
    const bool limited = range.max > range.min;
    if (range.slider && limited) {
        changed = ImGui::SliderFloat("##v", &value, range.min, range.max, format);
    } else {
        changed = ImGui::DragFloat("##v", &value, range.speed, limited ? range.min : 0.0f,
                                   limited ? range.max : 0.0f, format,
                                   limited ? ImGuiSliderFlags_AlwaysClamp : 0);
    }
    finishItem();
    if (changed) {
        FieldValue v = makeValue(FieldValue::Kind::Float);
        v.f = value;
        record(meta.key, v);
    }
    return changed;
}

bool ImGuiPropertyVisitor::field(const ecs::Meta& meta, int& value, int min, int max) {
    const char* format = mixed(meta.key) ? kMixed : "%d";
    label(meta);
    const bool changed = max > min ? ImGui::SliderInt("##v", &value, min, max, format)
                                   : ImGui::DragInt("##v", &value, 0.1f, 0, 0, format);
    finishItem();
    if (changed) {
        FieldValue v = makeValue(FieldValue::Kind::Int);
        v.i = value;
        record(meta.key, v);
    }
    return changed;
}

bool ImGuiPropertyVisitor::field(const ecs::Meta& meta, bool& value) {
    const bool is_mixed = mixed(meta.key) != 0;
    label(meta);
    ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, is_mixed);
    const bool changed = ImGui::Checkbox("##v", &value);
    ImGui::PopItemFlag();
    if (changed) {
        edit_finished_ = true;  // un clic es una accion completa
    }
    ImGui::PopID();
    if (changed) {
        FieldValue v = makeValue(FieldValue::Kind::Bool);
        v.b = value;
        record(meta.key, v);
    }
    return changed;
}

bool ImGuiPropertyVisitor::field(const ecs::Meta& meta, std::string& value) {
    const bool is_mixed = mixed(meta.key) != 0;
    label(meta);
    bool changed = false;
    if (is_mixed) {
        // Vacio con "—": lo que se escriba va a todos.
        std::string text;
        if (ImGui::InputTextWithHint("##v", kMixed, &text)) {
            value = text;
            changed = true;
        }
    } else {
        changed = ImGui::InputText("##v", &value);
    }
    finishItem();
    if (changed) {
        FieldValue v = makeValue(FieldValue::Kind::String);
        v.s = value;
        record(meta.key, v);
    }
    return changed;
}

bool ImGuiPropertyVisitor::field(const ecs::Meta& meta, core::Vec3& value, ecs::Vec3Kind kind) {
    const int mixed_axes = mixed(meta.key);
    label(meta);
    bool changed = false;
    int axes = 0x7;
    switch (kind) {
        case ecs::Vec3Kind::Color:
        case ecs::Vec3Kind::ColorHdr: {
            ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float;
            if (kind == ecs::Vec3Kind::ColorHdr) flags |= ImGuiColorEditFlags_HDR;
            ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, mixed_axes != 0);
            changed = ImGui::ColorEdit3("##v", &value.x, flags);
            ImGui::PopItemFlag();
            if (ImGui::IsItemDeactivatedAfterEdit()) edit_finished_ = true;
            break;
        }
        case ecs::Vec3Kind::Euler:
            changed = dragAxes(&value.x, 3, 0.5f, 0.0f, 0.0f, "%.1f°", mixed_axes, axes);
            break;
        case ecs::Vec3Kind::Scale:
            changed = dragAxes(&value.x, 3, 0.01f, 0.0f, 0.0f, "%.3f", mixed_axes, axes);
            break;
        case ecs::Vec3Kind::Direction:
            changed = dragAxes(&value.x, 3, 0.01f, -1.0f, 1.0f, "%.3f", mixed_axes, axes);
            if (changed) {
                const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
                if (length > 1e-5f) {
                    value = value * (1.0f / length);
                }
                axes = 0x7;  // normalizar toca los tres
            }
            break;
        case ecs::Vec3Kind::Position:
        default:
            changed = dragAxes(&value.x, 3, 0.02f, 0.0f, 0.0f, "%.3f", mixed_axes, axes);
            break;
    }
    ImGui::PopID();
    if (changed) {
        FieldValue v = makeValue(FieldValue::Kind::Vec3);
        v.v3 = value;
        v.axes = axes;
        record(meta.key, v);
    }
    return changed;
}

bool ImGuiPropertyVisitor::field(const ecs::Meta& meta, core::Vec2& value, float speed) {
    const int mixed_axes = mixed(meta.key);
    label(meta);
    int axes = 0x3;
    const bool changed = dragAxes(&value.x, 2, speed, 0.0f, 0.0f, "%.3f", mixed_axes, axes);
    ImGui::PopID();
    if (changed) {
        FieldValue v = makeValue(FieldValue::Kind::Vec2);
        v.v2 = value;
        v.axes = axes;
        record(meta.key, v);
    }
    return changed;
}

bool ImGuiPropertyVisitor::enumeration(const ecs::Meta& meta, int& value,
                                       std::span<const char* const> names) {
    const bool is_mixed = mixed(meta.key) != 0;
    label(meta);
    bool changed = false;
    const char* preview =
        is_mixed ? kMixed
                 : (value >= 0 && value < static_cast<int>(names.size()) ? names[static_cast<std::size_t>(value)] : "?");
    if (ImGui::BeginCombo("##v", preview)) {
        for (int i = 0; i < static_cast<int>(names.size()); ++i) {
            if (ImGui::Selectable(names[static_cast<std::size_t>(i)], !is_mixed && i == value)) {
                value = i;
                changed = true;
                edit_finished_ = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    if (changed) {
        FieldValue v = makeValue(FieldValue::Kind::Enum);
        v.i = value;
        record(meta.key, v);
    }
    return changed;
}

// Mascara de capas (LayerMask de Unity): el resumen en la caja y una casilla
// por capa con nombre en la lista.
bool ImGuiPropertyVisitor::layerMask(const ecs::Meta& meta, std::uint32_t& mask) {
    const bool is_mixed = mixed(meta.key) != 0;
    label(meta);
    const physics::PhysicsSettings& settings = physics::projectPhysicsSettings();
    std::string preview;
    if (is_mixed) {
        preview = kMixed;
    } else if (mask == 0) {
        preview = "Nada";
    } else if (mask == physics::kAllLayers) {
        preview = "Todo";
    } else {
        int count = 0;
        for (int i = 0; i < physics::kLayerCount; ++i) {
            if ((mask & physics::layerBit(i)) == 0) continue;
            if (++count <= 3) preview += (preview.empty() ? "" : ", ") + settings.layerLabel(i);
        }
        if (count > 3) preview = "Mixta (" + std::to_string(count) + " capas)";
    }
    bool changed = false;
    if (ImGui::BeginCombo("##v", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
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
            // Las capas sin nombre solo aparecen si ya estan marcadas.
            if (settings.layer_names[static_cast<std::size_t>(i)].empty() && (mask & physics::layerBit(i)) == 0) {
                continue;
            }
            bool on = (mask & physics::layerBit(i)) != 0;
            const std::string item = std::to_string(i) + ": " + settings.layerLabel(i);
            if (ImGui::Checkbox(item.c_str(), &on)) {
                mask = on ? (mask | physics::layerBit(i)) : (mask & ~physics::layerBit(i));
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (changed) edit_finished_ = true;
    ImGui::PopID();
    if (changed) {
        FieldValue v = makeValue(FieldValue::Kind::Mask);
        v.mask = mask;
        record(meta.key, v);
    }
    return changed;
}

bool ImGuiPropertyVisitor::entity(const ecs::Meta& meta, Uuid& ref) {
    const bool is_mixed = mixed(meta.key) != 0;
    label(meta);
    bool changed = false;
    std::string text = is_mixed ? std::string(kMixed) : "Ninguno (objeto)";
    if (ref.valid() && !is_mixed) {
        const ecs::Entity e = world_ != nullptr ? world_->find(ref) : ecs::Entity{};
        text = e.valid() ? e.name() : "(no encontrado)";
    }
    const float clear_width = ImGui::GetFrameHeight();
    const float width = std::max(ImGui::GetContentRegionAvail().x - clear_width - 4.0f, 20.0f);
    if (ImGui::Button(text.c_str(), ImVec2(width, 0.0f))) ImGui::OpenPopup("pick_entity");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Arrastra un objeto desde la Jerarquia o haz clic para elegirlo");
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload)) {
            Uuid dropped{};
            std::memcpy(&dropped, payload->Data, sizeof(Uuid));
            ref = dropped;
            changed = true;
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopup("pick_entity")) {
        static std::string filter;
        if (ImGui::IsWindowAppearing()) {
            filter.clear();
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##filter", "Buscar...", &filter);
        if (ImGui::Selectable("Ninguno", !ref.valid())) {
            ref = Uuid{};
            changed = true;
        }
        if (world_ != nullptr) {
            std::string needle = filter;
            std::transform(needle.begin(), needle.end(), needle.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            ImGui::BeginChild("list", ImVec2(260.0f, 260.0f));
            world_->forEachDepthFirst([&](ecs::Entity e) {
                std::string name = e.name();
                std::string low = name;
                std::transform(low.begin(), low.end(), low.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (!needle.empty() && low.find(needle) == std::string::npos) return;
                ImGui::PushID(static_cast<int>(entt::to_integral(e.handle())));
                if (ImGui::Selectable(name.c_str(), e.uuid() == ref)) {
                    ref = e.uuid();
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
            });
            ImGui::EndChild();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::Button("x", ImVec2(clear_width, 0.0f)) && (ref.valid() || is_mixed)) {
        ref = Uuid{};
        changed = true;
    }
    if (changed) edit_finished_ = true;
    ImGui::PopID();
    if (changed) {
        FieldValue v = makeValue(FieldValue::Kind::Entity);
        v.uuid = ref;
        record(meta.key, v);
    }
    return changed;
}

bool ImGuiPropertyVisitor::beginList(const ecs::Meta& meta, std::size_t& count) {
    const bool is_mixed = mixed(meta.key) != 0;
    const std::string path = pathOf(meta.key);
    ImGui::PushID(meta.key);
    char header[128];
    if (is_mixed) {
        std::snprintf(header, sizeof(header), "%s (%s)", meta.label, kMixed);
    } else {
        std::snprintf(header, sizeof(header), "%s (%zu)", meta.label, count);
    }
    ListState state;
    state.open = ImGui::TreeNodeEx("##list", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding |
                                                 ImGuiTreeNodeFlags_AllowOverlap,
                                   "%s", header);
    if (meta.tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s", meta.tooltip);
    }
    ImGui::SameLine(std::max(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::GetFrameHeight(), 0.0f));
    if (ImGui::SmallButton("+")) {
        ++count;
        edit_finished_ = true;
        FieldValue v = makeValue(FieldValue::Kind::ListCount);
        v.count = count;
        recordPath(path, v);
    }
    ImGui::SetItemTooltip("Añadir");
    lists_.push_back(state);
    list_paths_.push_back(path);
    push(meta.key);
    return true;
}

bool ImGuiPropertyVisitor::beginListItem(std::size_t index) {
    if (lists_.empty() || !lists_.back().open) return false;
    ImGui::PushID(static_cast<int>(index));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("#%zu", index);
    ImGui::SameLine(std::max(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::GetFrameHeight(), 0.0f));
    if (ImGui::SmallButton("x")) {
        lists_.back().remove = static_cast<int>(index);
        edit_finished_ = true;
        FieldValue v = makeValue(FieldValue::Kind::ListRemove);
        v.i = static_cast<int>(index);
        if (!list_paths_.empty()) recordPath(list_paths_.back(), v);
    }
    ImGui::SetItemTooltip("Quitar");
    ImGui::Indent(8.0f);
    push(std::to_string(index));
    return true;
}

void ImGuiPropertyVisitor::endListItem() {
    ImGui::Unindent(8.0f);
    ImGui::Separator();
    ImGui::PopID();
    pop();
}

int ImGuiPropertyVisitor::endList() {
    if (lists_.empty()) return -1;
    const ListState state = lists_.back();
    lists_.pop_back();
    if (!list_paths_.empty()) list_paths_.pop_back();
    pop();
    if (state.open) ImGui::TreePop();
    ImGui::PopID();
    return state.remove;
}

// Campo de asset: nombre del asset en una caja (como el "Object field" de
// Unity), acepta soltar un asset del tipo correcto y tiene un boton para
// vaciarlo.
bool ImGuiPropertyVisitor::asset(const ecs::Meta& meta, assets::AssetRef& ref,
                                 assets::AssetType type) {
    const bool is_mixed = mixed(meta.key) != 0;
    label(meta);
    bool changed = false;

    std::string text = "Ninguno (" + std::string(assets::assetTypeName(type)) + ")";
    if (is_mixed) {
        text = kMixed;
    } else if (ref.valid()) {
        const auto info = database_ != nullptr ? database_->find(ref.uuid) : std::nullopt;
        text = info ? info->name : "Falta (" + ref.uuid.toString().substr(0, 8) + ")";
    }
    const float clear_width = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(-clear_width - ImGui::GetStyle().ItemSpacing.x);
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    ImGui::Button(text.c_str(), ImVec2(ImGui::GetContentRegionAvail().x - clear_width -
                                           ImGui::GetStyle().ItemSpacing.x,
                                       0.0f));
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered() && ref.valid() && !is_mixed) {
        ImGui::SetTooltip("UUID %s", ref.uuid.toString().c_str());
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            AssetPayload dropped{};
            std::memcpy(&dropped, payload->Data, sizeof(dropped));
            if (dropped.type == type) {
                ref.uuid = dropped.uuid;
                ref.type = type;
                changed = true;
                edit_finished_ = true;
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    if (ImGui::Button("x", ImVec2(clear_width, 0.0f)) && (ref.valid() || is_mixed)) {
        ref.uuid = {};
        changed = true;
        edit_finished_ = true;
    }
    ImGui::PopID();
    if (changed) {
        FieldValue v = makeValue(FieldValue::Kind::Asset);
        v.asset = ref;
        record(meta.key, v);
    }
    return changed;
}

}  // namespace cramion::editor
