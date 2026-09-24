#include "PropertyInspector.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <cmath>
#include <cstring>

namespace cramion::editor {

namespace {

// Ancho de la columna de etiquetas, como el Inspector de Unity.
float labelWidth() {
    return std::max(ImGui::GetContentRegionAvail().x * 0.38f, 90.0f);
}

}  // namespace

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

bool ImGuiPropertyVisitor::beginGroup(const char* text, bool default_open) {
    ImGui::PushID(text);
    const bool open = ImGui::TreeNodeEx(text, (default_open ? ImGuiTreeNodeFlags_DefaultOpen : 0) |
                                                  ImGuiTreeNodeFlags_SpanAvailWidth |
                                                  ImGuiTreeNodeFlags_FramePadding);
    if (!open) {
        ImGui::PopID();
        return false;
    }
    ++depth_;
    return true;
}

void ImGuiPropertyVisitor::endGroup() {
    ImGui::TreePop();
    ImGui::PopID();
    --depth_;
}

bool ImGuiPropertyVisitor::field(const ecs::Meta& meta, float& value, const ecs::FloatRange& range) {
    label(meta);
    bool changed = false;
    const bool limited = range.max > range.min;
    if (range.slider && limited) {
        changed = ImGui::SliderFloat("##v", &value, range.min, range.max, range.format);
    } else {
        changed = ImGui::DragFloat("##v", &value, range.speed, limited ? range.min : 0.0f,
                                   limited ? range.max : 0.0f, range.format,
                                   limited ? ImGuiSliderFlags_AlwaysClamp : 0);
    }
    finishItem();
    return changed;
}

bool ImGuiPropertyVisitor::field(const ecs::Meta& meta, int& value, int min, int max) {
    label(meta);
    const bool changed = max > min ? ImGui::SliderInt("##v", &value, min, max)
                                   : ImGui::DragInt("##v", &value, 0.1f);
    finishItem();
    return changed;
}

bool ImGuiPropertyVisitor::field(const ecs::Meta& meta, bool& value) {
    label(meta);
    const bool changed = ImGui::Checkbox("##v", &value);
    if (changed) {
        edit_finished_ = true;  // un clic es una accion completa
    }
    ImGui::PopID();
    return changed;
}

bool ImGuiPropertyVisitor::field(const ecs::Meta& meta, std::string& value) {
    label(meta);
    const bool changed = ImGui::InputText("##v", &value);
    finishItem();
    return changed;
}

bool ImGuiPropertyVisitor::field(const ecs::Meta& meta, core::Vec3& value, ecs::Vec3Kind kind) {
    label(meta);
    bool changed = false;
    switch (kind) {
        case ecs::Vec3Kind::Color:
            changed = ImGui::ColorEdit3("##v", &value.x, ImGuiColorEditFlags_Float);
            break;
        case ecs::Vec3Kind::ColorHdr:
            changed = ImGui::ColorEdit3("##v", &value.x,
                                        ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
            break;
        case ecs::Vec3Kind::Euler:
            changed = ImGui::DragFloat3("##v", &value.x, 0.5f, 0.0f, 0.0f, "%.1f°");
            break;
        case ecs::Vec3Kind::Scale:
            changed = ImGui::DragFloat3("##v", &value.x, 0.01f, 0.0f, 0.0f, "%.3f");
            break;
        case ecs::Vec3Kind::Direction:
            changed = ImGui::DragFloat3("##v", &value.x, 0.01f, -1.0f, 1.0f, "%.3f");
            if (changed) {
                const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
                if (length > 1e-5f) {
                    value = value * (1.0f / length);
                }
            }
            break;
        case ecs::Vec3Kind::Position:
        default:
            changed = ImGui::DragFloat3("##v", &value.x, 0.02f, 0.0f, 0.0f, "%.3f");
            break;
    }
    finishItem();
    return changed;
}

bool ImGuiPropertyVisitor::field(const ecs::Meta& meta, core::Vec2& value, float speed) {
    label(meta);
    const bool changed = ImGui::DragFloat2("##v", &value.x, speed);
    finishItem();
    return changed;
}

bool ImGuiPropertyVisitor::enumeration(const ecs::Meta& meta, int& value,
                                       std::span<const char* const> names) {
    label(meta);
    bool changed = false;
    const char* preview =
        value >= 0 && value < static_cast<int>(names.size()) ? names[static_cast<std::size_t>(value)] : "?";
    if (ImGui::BeginCombo("##v", preview)) {
        for (int i = 0; i < static_cast<int>(names.size()); ++i) {
            if (ImGui::Selectable(names[static_cast<std::size_t>(i)], i == value)) {
                value = i;
                changed = true;
                edit_finished_ = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    return changed;
}

// Campo de asset: nombre del asset en una caja (como el "Object field" de
// Unity), acepta soltar un asset del tipo correcto y tiene un boton para
// vaciarlo.
bool ImGuiPropertyVisitor::asset(const ecs::Meta& meta, assets::AssetRef& ref,
                                 assets::AssetType type) {
    label(meta);
    bool changed = false;

    std::string text = "Ninguno (" + std::string(assets::assetTypeName(type)) + ")";
    if (ref.valid()) {
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
    if (ImGui::IsItemHovered() && ref.valid()) {
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
    if (ImGui::Button("x", ImVec2(clear_width, 0.0f)) && ref.valid()) {
        ref.uuid = {};
        changed = true;
        edit_finished_ = true;
    }
    ImGui::PopID();
    return changed;
}

}  // namespace cramion::editor
