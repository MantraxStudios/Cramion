#include "PropertyInspector.h"

#include "Theme.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <cfloat>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <algorithm>

namespace cramion::editor {

namespace {

// Ancho de la columna de etiquetas, como el Inspector de Unity: una parte
// fija del ancho (todas las filas alineadas) con minimo y maximo.
float labelWidth() {
    return std::clamp(ImGui::GetContentRegionAvail().x * 0.40f, 96.0f, 240.0f);
}

// Abreviatura del tipo de asset (la etiqueta pequena del campo).
const char* assetTypeTag(assets::AssetType type) {
    switch (type) {
        case assets::AssetType::Model: return "MDL";
        case assets::AssetType::Environment: return "HDR";
        case assets::AssetType::Scene: return "ESC";
        case assets::AssetType::AnimatorController: return "ANM";
        case assets::AssetType::AnimationClip: return "CLIP";
        case assets::AssetType::Material: return "MAT";
        case assets::AssetType::Prefab: return "PRE";
        case assets::AssetType::RenderTexture: return "RT";
        case assets::AssetType::StateMachine: return "FSM";
        default: return "AST";
    }
}

// Caja de un campo de referencia (asset u objeto): fondo de campo, etiqueta
// del tipo a la izquierda y el nombre. Devuelve si se hizo clic.
bool referenceBox(const char* id, const char* tag, const std::string& text, bool assigned, float width) {
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(std::max(width, 20.0f), h));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 q(p.x + std::max(width, 20.0f), p.y + h);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float rounding = ImGui::GetStyle().FrameRounding;
    draw->AddRectFilled(p, q, hovered ? theme::kBg4 : theme::kBg3, rounding);
    draw->AddRect(p, q, hovered ? theme::kBorderStrong : theme::kBorder, rounding);
    // Etiqueta del tipo.
    const float small_size = theme::smallFontSize();
    ImFont* font = ImGui::GetFont();
    const ImVec2 ts = font->CalcTextSizeA(small_size, FLT_MAX, 0.0f, tag);
    const ImVec2 ta(p.x + 4.0f, p.y + 4.0f);
    const ImVec2 tb(ta.x + ts.x + 8.0f, q.y - 4.0f);
    draw->AddRectFilled(ta, tb, assigned ? theme::kBg5 : theme::kBg2, 2.0f);
    draw->AddText(font, small_size, ImVec2(ta.x + 4.0f, ta.y + (tb.y - ta.y - ts.y) * 0.5f),
                  assigned ? theme::kText : theme::kTextFaint, tag);
    // Nombre (recortado al ancho).
    const float x = tb.x + 6.0f;
    draw->PushClipRect(ImVec2(x, p.y), ImVec2(q.x - 4.0f, q.y), true);
    draw->AddText(ImVec2(x, p.y + ImGui::GetStyle().FramePadding.y), assigned ? theme::kText : theme::kTextDim,
                  text.c_str());
    draw->PopClipRect();
    return pressed;
}

// Boton cuadrado pequeno con un simbolo dibujado (x de vaciar, diana de elegir).
enum class Glyph { Clear, Pick };
bool glyphButton(const char* id, Glyph glyph) {
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(h, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + h, p.y + h);
    const float rounding = ImGui::GetStyle().FrameRounding;
    draw->AddRectFilled(p, q, ImGui::IsItemActive() ? theme::kBg5 : (hovered ? theme::kBg4 : theme::kBg3), rounding);
    draw->AddRect(p, q, theme::kBorder, rounding);
    const ImU32 color = hovered ? theme::kText : theme::kTextDim;
    const ImVec2 c(std::floor((p.x + q.x) * 0.5f) + 0.5f, std::floor((p.y + q.y) * 0.5f) + 0.5f);
    const float s = h * 0.18f;
    if (glyph == Glyph::Clear) {
        draw->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), color, 1.5f);
        draw->AddLine(ImVec2(c.x + s, c.y - s), ImVec2(c.x - s, c.y + s), color, 1.5f);
    } else {
        draw->AddCircle(c, s * 1.5f, color, 16, 1.4f);
        draw->AddCircleFilled(c, 1.8f, color, 8);
    }
    return pressed;
}

constexpr ImU32 kAxisColors[4] = {theme::kAxisX, theme::kAxisY, theme::kAxisZ, theme::kTextFaint};
constexpr const char* kAxisNames[4] = {"X", "Y", "Z", "W"};

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

std::string lowerText(const char* text) {
    std::string out = text != nullptr ? text : "";
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool FilterMatchVisitor::check(const ecs::Meta& meta) {
    if (!matched_ && (needle_.empty() || lowerText(meta.label).find(needle_) != std::string::npos)) matched_ = true;
    return false;
}

bool ImGuiPropertyVisitor::visible(const ecs::Meta& meta) const {
    if (filter_.empty() || filter_bypass_ > 0) return true;
    return lowerText(meta.label).find(filter_) != std::string::npos;
}

// Etiqueta a la izquierda (gris, blanca al pasar el raton; recortada con
// "..." si no cabe) y control a la derecha. Devuelve true siempre (el
// control va a continuacion con su ID oculto).
bool ImGuiPropertyVisitor::label(const ecs::Meta& meta) {
    const float column = labelWidth();
    const float start_x = ImGui::GetCursorPosX();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float max_w = std::max(column - start_x - ImGui::GetStyle().ItemSpacing.x, 10.0f);
    const float h = ImGui::GetFrameHeight();
    ImGui::PushID(meta.key);
    ImGui::Dummy(ImVec2(max_w, h));
    ImGui::PopID();
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 ts = ImGui::CalcTextSize(meta.label);
    const bool clipped = ts.x > max_w;
    ImGui::PushStyleColor(ImGuiCol_Text, hovered ? theme::kText : theme::kLabel);
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), ImVec2(pos.x, pos.y + ImGui::GetStyle().FramePadding.y),
                              ImVec2(pos.x + max_w, pos.y + h), pos.x + max_w, meta.label, nullptr, &ts);
    ImGui::PopStyleColor();
    if ((meta.tooltip != nullptr || clipped) && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        if (meta.tooltip != nullptr && clipped) {
            ImGui::SetTooltip("%s\n%s", meta.label, meta.tooltip);
        } else {
            ImGui::SetTooltip("%s", meta.tooltip != nullptr ? meta.tooltip : meta.label);
        }
    }
    ImGui::SameLine(column);
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
    // Cada eje: una pestana de color con su letra (X rojo, Y verde, Z azul)
    // pegada a su campo.
    const float h = ImGui::GetFrameHeight();
    const float tag_w = std::floor(h * 0.78f);
    const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
    const float each = std::max((full - gap * static_cast<float>(count - 1)) / static_cast<float>(count), tag_w + 20.0f);
    const float rounding = ImGui::GetStyle().FrameRounding;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        ImGui::PushID(i);
        if (i > 0) ImGui::SameLine(0.0f, gap);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        draw->AddRectFilled(p, ImVec2(p.x + tag_w, p.y + h), kAxisColors[std::min(i, 3)], rounding,
                            ImDrawFlags_RoundCornersLeft);
        const ImVec2 ts = ImGui::CalcTextSize(kAxisNames[std::min(i, 3)]);
        draw->AddText(ImVec2(p.x + std::floor((tag_w - ts.x) * 0.5f), p.y + std::floor((h - ts.y) * 0.5f)),
                      IM_COL32(255, 255, 255, 255), kAxisNames[std::min(i, 3)]);
        ImGui::SetCursorScreenPos(ImVec2(p.x + tag_w, p.y));
        ImGui::SetNextItemWidth(each - tag_w);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        const char* fmt = (mixed_axes >> i) & 1 ? kMixed : format;
        if (ImGui::DragFloat("##a", &values[i], speed, min, max, fmt)) {
            changed = true;
            changed_axes |= 1 << i;
        }
        ImGui::PopStyleVar();
        if (ImGui::IsItemDeactivatedAfterEdit()) edit_finished_ = true;
        ImGui::PopID();
    }
    return changed;
}

// Subgrupo (Bloom, Vineta...): una barra fina con el titulo en seminegrita.
// Con filtro no se dibuja (se ven solo los campos que coinciden).
bool ImGuiPropertyVisitor::beginGroup(const char* text, bool default_open) {
    if (!filter_.empty() && filter_bypass_ == 0) {
        push(text != nullptr ? text : "");
        groups_drawn_.push_back(false);
        ++depth_;
        return true;
    }
    ImGui::PushID(text);
    ImGui::PushStyleColor(ImGuiCol_Header, theme::withAlpha(theme::kText, 9));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, theme::withAlpha(theme::kText, 18));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, theme::withAlpha(theme::kText, 26));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.0f));
    if (theme::boldFont() != nullptr) ImGui::PushFont(theme::boldFont(), 0.0f);
    const bool open = ImGui::TreeNodeEx(text, (default_open ? ImGuiTreeNodeFlags_DefaultOpen : 0) |
                                                  ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_Framed |
                                                  ImGuiTreeNodeFlags_FramePadding);
    if (theme::boldFont() != nullptr) ImGui::PopFont();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    if (!open) {
        ImGui::PopID();
        return false;
    }
    push(text != nullptr ? text : "");
    groups_drawn_.push_back(true);
    ++depth_;
    return true;
}

void ImGuiPropertyVisitor::endGroup() {
    const bool drawn = groups_drawn_.empty() || groups_drawn_.back();
    if (!groups_drawn_.empty()) groups_drawn_.pop_back();
    if (drawn) {
        ImGui::TreePop();
        ImGui::PopID();
    }
    pop();
    --depth_;
}

bool ImGuiPropertyVisitor::field(const ecs::Meta& meta, float& value, const ecs::FloatRange& range) {
    if (!visible(meta)) return false;
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
    if (!visible(meta)) return false;
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
    if (!visible(meta)) return false;
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
    if (!visible(meta)) return false;
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
    if (!visible(meta)) return false;
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
    if (!visible(meta)) return false;
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
    if (!visible(meta)) return false;
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
    if (!visible(meta)) return false;
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
    if (!visible(meta)) return false;
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
    if (referenceBox("##entity", "OBJ", text, ref.valid() && !is_mixed, width)) ImGui::OpenPopup("pick_entity");
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
        theme::searchBox("entity_filter", filter, "Buscar...", 260.0f);
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
    if (glyphButton("##clear", Glyph::Clear) && (ref.valid() || is_mixed)) {
        ref = Uuid{};
        changed = true;
    }
    ImGui::SetItemTooltip("Vaciar");
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
    // Con filtro: la lista sale entera si su nombre coincide; si no, nada.
    if (!filter_.empty() && filter_bypass_ == 0 && !visible(meta)) {
        lists_.push_back(ListState{});
        list_paths_.push_back(path);
        push(meta.key);
        return true;
    }
    char header[128];
    if (is_mixed) {
        std::snprintf(header, sizeof(header), "%s (%s)", meta.label, kMixed);
    } else {
        std::snprintf(header, sizeof(header), "%s (%zu)", meta.label, count);
    }
    ListState state;
    if (!filter_.empty()) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        state.bypass = true;
        ++filter_bypass_;
    }
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
    if (state.bypass && filter_bypass_ > 0) --filter_bypass_;
    if (state.open) ImGui::TreePop();
    ImGui::PopID();
    return state.remove;
}

// Campo de asset: nombre del asset en una caja (como el "Object field" de
// Unity), acepta soltar un asset del tipo correcto y tiene un boton para
// vaciarlo.
bool ImGuiPropertyVisitor::asset(const ecs::Meta& meta, assets::AssetRef& ref,
                                 assets::AssetType type) {
    if (!visible(meta)) return false;
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
    // [TIPO nombre........] [◎] [x]: la caja acepta soltar, la diana abre la
    // lista de assets de ese tipo y la x lo vacia.
    const float button = ImGui::GetFrameHeight();
    const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
    const float box_width = ImGui::GetContentRegionAvail().x - (button + gap) * 2.0f;
    referenceBox("##asset", assetTypeTag(type), text, ref.valid() && !is_mixed, box_width);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        if (ref.valid() && !is_mixed) {
            ImGui::SetTooltip("%s\nUUID %s", text.c_str(), ref.uuid.toString().c_str());
        } else {
            ImGui::SetTooltip("Arrastra aquí un asset (%s) desde el Proyecto", assets::assetTypeName(type));
        }
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
    ImGui::SameLine(0.0f, gap);
    if (glyphButton("##pick", Glyph::Pick)) ImGui::OpenPopup("pick_asset");
    ImGui::SetItemTooltip("Elegir %s", assets::assetTypeName(type));
    if (ImGui::BeginPopup("pick_asset")) {
        static std::string filter;
        if (ImGui::IsWindowAppearing()) {
            filter.clear();
            ImGui::SetKeyboardFocusHere();
        }
        theme::searchBox("asset_filter", filter, "Buscar...", 280.0f);
        const std::string needle = lowerText(filter.c_str());
        theme::pushSelectionColors();
        if (ImGui::Selectable("Ninguno", !ref.valid())) {
            ref.uuid = {};
            changed = true;
            edit_finished_ = true;
        }
        ImGui::BeginChild("list", ImVec2(280.0f, 280.0f));
        int shown = 0;
        if (database_ != nullptr) {
            for (const assets::AssetInfo& info : database_->all()) {
                if (info.type != type) continue;
                if (!needle.empty() && lowerText(info.name.c_str()).find(needle) == std::string::npos) continue;
                ++shown;
                ImGui::PushID(info.uuid.toString().c_str());
                if (ImGui::Selectable(info.name.c_str(), ref.valid() && info.uuid == ref.uuid)) {
                    ref.uuid = info.uuid;
                    ref.type = type;
                    changed = true;
                    edit_finished_ = true;
                    ImGui::CloseCurrentPopup();
                }
                if (!info.path.empty()) ImGui::SetItemTooltip("%s", info.path.generic_string().c_str());
                ImGui::PopID();
            }
        }
        if (shown == 0) ImGui::TextDisabled("No hay assets de este tipo");
        ImGui::EndChild();
        ImGui::PopStyleColor(3);
        ImGui::EndPopup();
    }
    ImGui::SameLine(0.0f, gap);
    if (glyphButton("##clear", Glyph::Clear) && (ref.valid() || is_mixed)) {
        ref.uuid = {};
        changed = true;
        edit_finished_ = true;
    }
    ImGui::SetItemTooltip("Vaciar");
    ImGui::PopID();
    if (changed) {
        FieldValue v = makeValue(FieldValue::Kind::Asset);
        v.asset = ref;
        record(meta.key, v);
    }
    return changed;
}

}  // namespace cramion::editor
