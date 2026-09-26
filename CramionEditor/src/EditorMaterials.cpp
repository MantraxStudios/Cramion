// Materiales como en Unity: crear un .crmat (en blanco o a partir de una
// imagen con sus companeras PBR), editarlo en el Inspector con vista previa,
// y arrastrarlo a un objeto (Escena, Jerarquia o hueco del Mesh Renderer).
// Los cambios de color y factores se ven al momento; los de texturas y
// tiling rehacen la variante del modelo al soltar el control.

#include "EditorApp.h"

#include "Dialogs.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace cramion::editor {

namespace {

float toSrgb(float linear) { return std::pow(std::clamp(linear, 0.0f, 1.0f), 1.0f / 2.2f); }
float toLinear(float srgb) { return std::pow(std::clamp(srgb, 0.0f, 1.0f), 2.2f); }

std::string safeName(std::string name) {
    for (char& c : name) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' ||
            c == '*' || static_cast<unsigned char>(c) < 32) {
            c = '_';
        }
    }
    return name.empty() ? std::string("Material") : name;
}

std::filesystem::path freePath(const std::filesystem::path& folder, const std::string& stem) {
    std::filesystem::path path = folder / dialogs::fromUtf8(stem + ".crmat");
    for (int i = 2; std::filesystem::exists(path); ++i) {
        path = folder / dialogs::fromUtf8(stem + " " + std::to_string(i) + ".crmat");
    }
    return path;
}

}  // namespace

// Esfera de vista previa (dibujada, sin render): la textura de color
// recortada en circulo, sombreada, con un brillo segun la rugosidad y el metal.
void EditorApp::drawMaterialBall(ImDrawList* draw, ImVec2 center, float radius, const Uuid& material) {
    const std::shared_ptr<const assets::MaterialAsset> mat = sync_ ? sync_->material(material) : nullptr;
    if (!mat) {
        draw->AddCircleFilled(center, radius, IM_COL32(60, 60, 66, 255));
        draw->AddCircle(center, radius, IM_COL32(200, 80, 80, 255), 0, 1.5f);
        return;
    }
    const auto channel = [&](float linear) { return static_cast<int>(toSrgb(linear) * 255.0f + 0.5f); };
    const ImU32 tint = IM_COL32(channel(mat->base_color.x), channel(mat->base_color.y), channel(mat->base_color.z), 255);
    const ImVec2 min{center.x - radius, center.y - radius};
    const ImVec2 max{center.x + radius, center.y + radius};
    ImTextureID texture = 0;
    if (!mat->albedo.empty()) {
        texture = imgui_.thumbnail(project_.assetsFolder() / dialogs::fromUtf8(mat->albedo));
    }
    if (texture != 0) {
        // Tiling aproximado en la miniatura.
        const ImVec2 uv1{std::max(mat->tiling.x, 0.05f), std::max(mat->tiling.y, 0.05f)};
        draw->AddImageRounded(texture, min, max, ImVec2(0.0f, 0.0f), ImVec2(std::min(uv1.x, 4.0f), std::min(uv1.y, 4.0f)),
                              tint, radius);
    } else {
        draw->AddCircleFilled(center, radius, tint, 48);
    }
    // Borde oscurecido: da volumen de esfera.
    draw->AddCircle(center, radius * 0.96f, IM_COL32(0, 0, 0, 70), 48, radius * 0.12f);
    draw->AddCircle(center, radius * 0.88f, IM_COL32(0, 0, 0, 35), 48, radius * 0.1f);
    // Brillo: pequeno y fuerte si es liso, ancho y suave si es rugoso.
    const float rough = std::clamp(mat->roughness, 0.05f, 1.0f);
    const float spot = radius * (0.12f + rough * 0.35f);
    const int strength = static_cast<int>((1.0f - rough) * 200.0f + 25.0f);
    const ImVec2 highlight{center.x - radius * 0.38f, center.y - radius * 0.38f};
    const ImU32 spec = mat->metallic > 0.5f ? ((tint & 0x00FFFFFFu) | (static_cast<ImU32>(strength) << 24))
                                            : IM_COL32(255, 255, 255, strength);
    for (int i = 4; i >= 1; --i) {
        const float k = static_cast<float>(i) / 4.0f;
        draw->AddCircleFilled(highlight, spot * k, (spec & 0x00FFFFFFu) | (static_cast<ImU32>(strength / 4) << 24), 24);
    }
    // Emision: un halo.
    const float glow = std::max({mat->emissive.x, mat->emissive.y, mat->emissive.z}) * mat->emissive_intensity;
    if (glow > 0.01f) {
        const ImU32 e = IM_COL32(channel(std::min(mat->emissive.x, 1.0f)), channel(std::min(mat->emissive.y, 1.0f)),
                                 channel(std::min(mat->emissive.z, 1.0f)), static_cast<int>(std::min(glow, 1.0f) * 160));
        draw->AddCircle(center, radius + 2.0f, e, 48, 3.0f);
    }
    if (mat->mode == assets::MaterialMode::Transparent) {
        draw->AddCircle(center, radius, IM_COL32(150, 210, 255, 200), 48, 1.5f);
    }
}

void EditorApp::createMaterialAsset(const std::filesystem::path& folder, const std::string& image,
                                    const asset::MaterialData* from) {
    assets::MaterialAsset material;
    std::string stem = "Nuevo material";
    if (!image.empty()) {
        material = assets::materialFromImage(project_.assetsFolder(), image);
        std::string base = dialogs::utf8(dialogs::fromUtf8(image).stem());
        for (const char* suffix : {"_BaseColor", "_basecolor", "_Albedo", "_albedo", "_Diffuse", "_diffuse", "_Color", "_color"}) {
            const std::string s(suffix);
            if (base.size() > s.size() && base.compare(base.size() - s.size(), s.size(), s) == 0) {
                base.resize(base.size() - s.size());
                break;
            }
        }
        stem = base;
    }
    if (from != nullptr) {
        material.base_color = from->base_color;
        material.metallic = from->metallic;
        material.roughness = from->roughness;
        material.reflectance = from->reflectance;
        material.emissive = from->emissive;
        material.mode = from->transparent ? assets::MaterialMode::Transparent : assets::MaterialMode::Opaque;
        if (!from->name.empty()) stem = from->name;
    }
    const std::filesystem::path target_folder = folder.empty() ? project_.assetsFolder() : folder;
    const std::filesystem::path path = freePath(target_folder, safeName(stem));
    std::string error;
    if (!assets::saveMaterial(material, path, &error)) {
        std::cerr << "[Editor] No se pudo crear el material: " << error << "\n";
        return;
    }
    refreshDatabase();
    std::cout << "[Editor] Material creado: " << dialogs::utf8(path.filename()) << "\n";
    last_created_material_ = material.uuid;
    if (from == nullptr) {
        // Como en Unity: queda seleccionado y con el nombre listo para escribir.
        inspected_material_ = material.uuid;
        renaming_asset_ = material.uuid;
        asset_rename_buffer_ = dialogs::utf8(path.stem());
    }
}

int EditorApp::materialSlotCount(ecs::Entity entity) const {
    const ecs::MeshRenderer* mr = entity.tryGet<ecs::MeshRenderer>();
    if (mr == nullptr) return 0;
    if (sync_) {
        if (const asset::ModelData* data = sync_->actorModelData(entity, scene_)) {
            return static_cast<int>(std::max<std::size_t>(data->materials.size(), 1));
        }
    }
    return static_cast<int>(std::max<std::size_t>(mr->materials.size(), 1));
}

// slot < 0: todos los huecos (soltarlo en la Jerarquia o en el Inspector).
bool EditorApp::applyMaterial(ecs::Entity entity, const Uuid& material, int slot) {
    ecs::MeshRenderer* mr = entity.tryGet<ecs::MeshRenderer>();
    if (mr == nullptr) {
        // Un padre sin malla (modelo importado por piezas): a sus hijos.
        bool any = false;
        for (const entt::entity child : entity.children()) any = applyMaterial(world_.wrap(child), material, -1) || any;
        return any;
    }
    const int count = materialSlotCount(entity);
    if (static_cast<int>(mr->materials.size()) < count) mr->materials.resize(static_cast<std::size_t>(count));
    const assets::AssetRef ref{material, assets::AssetType::Material};
    if (slot < 0) {
        for (assets::AssetRef& r : mr->materials) r = ref;
    } else {
        if (static_cast<int>(mr->materials.size()) <= slot) mr->materials.resize(static_cast<std::size_t>(slot) + 1);
        mr->materials[static_cast<std::size_t>(slot)] = ref;
    }
    return true;
}

// Hueco de textura: miniatura, acepta imagenes arrastradas del Proyecto (o
// de fuera, que se copian a Assets/Textures) y boton para quitarla.
bool EditorApp::materialTextureSlot(const char* label, std::string& path) {
    ImGui::PushID(label);
    bool changed = false;
    const float size = ImGui::GetFrameHeight() * 1.6f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##slot", ImVec2(size, size));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max{pos.x + size, pos.y + size};
    draw->AddRectFilled(pos, max, IM_COL32(40, 40, 46, 255), 3.0f);
    ImVec2 thumb_size{};
    if (!path.empty()) {
        if (const ImTextureID t = imgui_.thumbnail(project_.assetsFolder() / dialogs::fromUtf8(path), &thumb_size); t != 0) {
            draw->AddImage(t, ImVec2(pos.x + 2, pos.y + 2), ImVec2(max.x - 2, max.y - 2));
        }
    }
    draw->AddRect(pos, max, ImGui::IsItemHovered() ? IM_COL32(255, 160, 40, 255) : IM_COL32(0, 0, 0, 140), 3.0f);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s\n%s\nArrastra una imagen aquí (clic: elegir archivo)", label,
                          path.empty() ? "(sin textura)" : path.c_str());
    }
    if (ImGui::IsItemClicked()) {
        const std::string chosen = importDecalImage();
        if (!chosen.empty()) {
            path = chosen;
            changed = true;
        }
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kImagePayload)) {
            const std::string relative = decalImageInAssets(dialogs::fromUtf8(static_cast<const char*>(payload->Data)));
            if (!relative.empty()) {
                path = relative;
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted(label);
    if (!path.empty()) {
        ImGui::TextDisabled("%s", dialogs::utf8(dialogs::fromUtf8(path).filename()).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            path.clear();
            changed = true;
        }
    } else {
        ImGui::TextDisabled("Ninguna");
    }
    ImGui::EndGroup();
    ImGui::PopID();
    return changed;
}

void EditorApp::flushMaterialEdit(bool force_structure) {
    if (!material_edit_uuid_.valid() || !sync_) return;
    const std::uint64_t structure = assets::materialStructureHash(material_edit_);
    // Colores y factores: al momento. Texturas/tiling/modo: al soltar el control
    // (rehacen el modelo en la GPU).
    if (structure == material_pushed_structure_ || force_structure || !ImGui::IsAnyItemActive()) {
        sync_->updateMaterial(material_edit_uuid_, material_edit_);
        material_pushed_structure_ = structure;
    }
    if (material_unsaved_ && !ImGui::IsAnyItemActive()) {
        std::string error;
        if (!assets::saveMaterial(material_edit_, material_edit_path_, &error)) {
            std::cerr << "[Editor] No se pudo guardar el material: " << error << "\n";
        }
        material_unsaved_ = false;
    }
}

void EditorApp::drawMaterialEditor(const Uuid& uuid) {
    const auto info = database_->find(uuid);
    if (!info || info->type != assets::AssetType::Material) {
        ImGui::TextDisabled("El material ya no existe.");
        return;
    }
    if (material_edit_uuid_ != uuid || material_edit_path_ != info->path) {
        if (material_unsaved_) assets::saveMaterial(material_edit_, material_edit_path_);
        material_unsaved_ = false;
        std::string error;
        if (!assets::loadMaterial(info->path, material_edit_, &error)) {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "No se pudo leer: %s", error.c_str());
            material_edit_uuid_ = {};
            return;
        }
        material_edit_.uuid = uuid;
        material_edit_uuid_ = uuid;
        material_edit_path_ = info->path;
        material_pushed_structure_ = assets::materialStructureHash(material_edit_);
    }
    assets::MaterialAsset& m = material_edit_;
    bool changed = false;
    bool structural = false;

    ImGui::PushID("material_editor");
    // Cabecera: vista previa + nombre + modo.
    const float ball = 56.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(ball, ball));
    drawMaterialBall(ImGui::GetWindowDrawList(), ImVec2(pos.x + ball * 0.5f, pos.y + ball * 0.5f), ball * 0.46f, uuid);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("%s", info->name.c_str());
    ImGui::TextDisabled("Material (.crmat)");
    static constexpr const char* kModes[] = {"Opaco", "Transparente"};
    int mode = static_cast<int>(m.mode);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##mode", &mode, kModes, 2)) {
        m.mode = static_cast<assets::MaterialMode>(mode);
        changed = structural = true;
    }
    ImGui::SetItemTooltip("Opaco: recorta lo que tenga alfa (hojas, rejas).\nTransparente: vidrio, agua.");
    ImGui::EndGroup();
    ImGui::Separator();

    // --- Shader (estandar o uno propio .crshader) ---
    drawMaterialShaderSection(m, changed, structural);

    // --- Color ---
    if (materialTextureSlot("Color (albedo)", m.albedo)) changed = structural = true;
    float color[4] = {toSrgb(m.base_color.x), toSrgb(m.base_color.y), toSrgb(m.base_color.z), m.base_color.w};
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::ColorEdit4("##color", color, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) {
        m.base_color = core::Vec4{toLinear(color[0]), toLinear(color[1]), toLinear(color[2]), color[3]};
        changed = true;
    }

    // --- Normal ---
    if (materialTextureSlot("Normal map", m.normal)) changed = structural = true;
    if (!m.normal.empty()) {
        ImGui::SetNextItemWidth(-90.0f);
        changed |= ImGui::SliderFloat("Intensidad##normal", &m.normal_strength, 0.0f, 3.0f, "%.2f");
        changed |= ImGui::Checkbox("DirectX (+Y abajo, Unreal)", &m.normal_directx);
    }

    // --- Metal / rugosidad ---
    ImGui::SeparatorText("Superficie");
    if (materialTextureSlot("Metálico (mapa)", m.metallic_map)) changed = structural = true;
    ImGui::SetNextItemWidth(-90.0f);
    changed |= ImGui::SliderFloat("Metálico", &m.metallic, 0.0f, 1.0f, "%.2f");
    if (materialTextureSlot("Rugosidad (mapa)", m.roughness_map)) changed = structural = true;
    ImGui::SetNextItemWidth(-90.0f);
    changed |= ImGui::SliderFloat("Rugosidad", &m.roughness, 0.0f, 1.0f, "%.2f");
    ImGui::SetItemTooltip("0 = espejo, 1 = mate. Con mapa, lo multiplica.");
    ImGui::SetNextItemWidth(-90.0f);
    changed |= ImGui::SliderFloat("Reflectancia", &m.reflectance, 0.0f, 0.16f, "%.3f");
    ImGui::SetItemTooltip("Brillo de frente de lo no metálico (0.04 casi todo; más: mármol, laca).");
    if (materialTextureSlot("Oclusión (AO)", m.occlusion)) changed = structural = true;
    if (!m.occlusion.empty()) {
        ImGui::SetNextItemWidth(-90.0f);
        changed |= ImGui::SliderFloat("Fuerza AO", &m.occlusion_strength, 0.0f, 1.0f, "%.2f");
    }

    // --- Emision ---
    ImGui::SeparatorText("Emisión");
    if (materialTextureSlot("Emisión (mapa)", m.emissive_map)) changed = structural = true;
    float emissive[3] = {toSrgb(m.emissive.x), toSrgb(m.emissive.y), toSrgb(m.emissive.z)};
    ImGui::SetNextItemWidth(-90.0f);
    if (ImGui::ColorEdit3("Color##emissive", emissive)) {
        m.emissive = core::Vec3{toLinear(emissive[0]), toLinear(emissive[1]), toLinear(emissive[2])};
        changed = true;
    }
    ImGui::SetNextItemWidth(-90.0f);
    changed |= ImGui::DragFloat("Intensidad##emissive", &m.emissive_intensity, 0.05f, 0.0f, 100.0f, "%.2f");

    // --- UV ---
    ImGui::SeparatorText("UV");
    float tiling[2] = {m.tiling.x, m.tiling.y};
    ImGui::SetNextItemWidth(-90.0f);
    if (ImGui::DragFloat2("Tiling", tiling, 0.02f, -100.0f, 100.0f, "%.2f")) {
        m.tiling = core::Vec2{tiling[0], tiling[1]};
        changed = true;
    }
    float offset[2] = {m.offset.x, m.offset.y};
    ImGui::SetNextItemWidth(-90.0f);
    if (ImGui::DragFloat2("Offset", offset, 0.01f, -10.0f, 10.0f, "%.2f")) {
        m.offset = core::Vec2{offset[0], offset[1]};
        changed = true;
    }
    ImGui::PopID();

    if (changed) material_unsaved_ = true;
    if (changed || material_unsaved_) flushMaterialEdit(structural);
}

// Huecos de material del Mesh Renderer (debajo de sus propiedades): el del
// modelo o el .crmat que lo sustituye; se sueltan materiales encima.
void EditorApp::drawMeshMaterials(ecs::Entity entity) {
    ecs::MeshRenderer* mr = entity.tryGet<ecs::MeshRenderer>();
    if (mr == nullptr) return;
    const asset::ModelData* data = sync_ ? sync_->actorModelData(entity, scene_) : nullptr;
    const int count = materialSlotCount(entity);
    if (!ImGui::TreeNodeEx("##materials", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
                           "Materiales (%d)", count)) {
        return;
    }
    bool edited = false;
    for (int i = 0; i < count; ++i) {
        ImGui::PushID(i);
        const Uuid current = i < static_cast<int>(mr->materials.size()) ? mr->materials[i].uuid : Uuid{};
        const bool assigned = current.valid() && database_->find(current).has_value();
        const float row = ImGui::GetFrameHeight() * 1.4f;
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImGui::Dummy(ImVec2(row, row));
        const ImVec2 center{pos.x + row * 0.5f, pos.y + row * 0.5f};
        if (assigned) {
            drawMaterialBall(draw, center, row * 0.45f, current);
        } else {
            // El del modelo: su color.
            ImU32 color = IM_COL32(150, 150, 150, 255);
            if (data != nullptr && i < static_cast<int>(data->materials.size())) {
                const core::Vec4& c = data->materials[i].base_color;
                color = IM_COL32(static_cast<int>(toSrgb(c.x) * 255), static_cast<int>(toSrgb(c.y) * 255),
                                 static_cast<int>(toSrgb(c.z) * 255), 255);
            }
            draw->AddCircleFilled(center, row * 0.45f, color, 32);
            draw->AddCircle(center, row * 0.45f, IM_COL32(0, 0, 0, 120), 32, 1.0f);
        }
        ImGui::SameLine();
        const std::string slot_name = data != nullptr && i < static_cast<int>(data->materials.size()) &&
                                              !data->materials[i].name.empty()
                                          ? data->materials[i].name
                                          : "Hueco " + std::to_string(i);
        const std::string text = assigned ? database_->find(current)->name : "Del modelo";
        ImGui::BeginGroup();
        ImGui::TextDisabled("%s", slot_name.c_str());
        const float buttons = assigned ? ImGui::GetFrameHeight() * 2.0f + 8.0f : 60.0f;
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        if (ImGui::Button(text.c_str(), ImVec2(ImGui::GetContentRegionAvail().x - buttons, 0.0f)) && assigned) {
            inline_material_ = current;  // se edita abajo
        }
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip(assigned ? "Clic: editarlo aquí debajo. Suelta otro material para cambiarlo."
                                       : "Suelta un material (.crmat) del Proyecto aquí.");
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                AssetPayload dropped{};
                std::memcpy(&dropped, payload->Data, sizeof(dropped));
                if (dropped.type == assets::AssetType::Material) {
                    applyMaterial(entity, dropped.uuid, i);
                    inline_material_ = dropped.uuid;
                    edited = true;
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (assigned) {
            if (ImGui::Button("...", ImVec2(ImGui::GetFrameHeight(), 0.0f))) inspected_material_ = current;
            ImGui::SetItemTooltip("Ver el material en el Inspector");
            ImGui::SameLine();
            if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0.0f))) {
                mr->materials[static_cast<std::size_t>(i)] = {};
                // Sin ninguno: la lista vacia (el modelo tal cual).
                if (std::none_of(mr->materials.begin(), mr->materials.end(),
                                 [](const assets::AssetRef& r) { return r.valid(); })) {
                    mr->materials.clear();
                }
                edited = true;
            }
            ImGui::SetItemTooltip("Volver al material del modelo");
        } else if (ImGui::Button("Nuevo", ImVec2(-1.0f, 0.0f))) {
            // Crear un .crmat con los valores del material del modelo y usarlo.
            const asset::MaterialData* from =
                data != nullptr && i < static_cast<int>(data->materials.size()) ? &data->materials[i] : nullptr;
            asset::MaterialData base = from != nullptr ? *from : asset::MaterialData{};
            if (base.name.empty()) base.name = entity.name();
            createMaterialAsset(current_folder_.empty() ? project_.assetsFolder() / "Materials" : current_folder_, {},
                                &base);
            if (last_created_material_.valid()) {
                applyMaterial(entity, last_created_material_, i);
                inline_material_ = last_created_material_;
                edited = true;
            }
        }
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::TreePop();
    if (edited) commit();

    // El material elegido, editable aqui mismo (como en Unity, al pie).
    const bool uses_inline = std::any_of(mr->materials.begin(), mr->materials.end(),
                                         [&](const assets::AssetRef& r) { return r.uuid == inline_material_; });
    if (!uses_inline) {
        inline_material_ = {};
        for (const assets::AssetRef& r : mr->materials) {
            if (r.valid() && database_->find(r.uuid)) {
                inline_material_ = r.uuid;
                break;
            }
        }
    }
    if (inline_material_.valid()) {
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
            drawMaterialEditor(inline_material_);
        }
    }
}

}  // namespace cramion::editor
