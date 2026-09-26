// Shaders de superficie en el editor: crear un .crshader (Proyecto > Crear >
// Shader), elegirlo en un material y editar sus propiedades (el control
// sale del tipo: float, range, color, vector, texture). Guardarlo en el editor
// de scripts lo recompila (EditorScripting.cpp) y se ve al momento.

#include "EditorApp.h"

#include "Dialogs.h"

#include <imgui.h>

#include <algorithm>
#include <fstream>
#include <iostream>

namespace cramion::editor {

std::filesystem::path EditorApp::createShaderAsset(const std::filesystem::path& folder) {
    const std::filesystem::path target = folder.empty() ? project_.assetsFolder() / "Shaders" : folder;
    std::error_code error;
    std::filesystem::create_directories(target, error);
    std::filesystem::path path = target / dialogs::fromUtf8(std::string("NuevoShader") + assets::kSurfaceShaderExtension);
    for (int i = 2; std::filesystem::exists(path); ++i) {
        path = target / dialogs::fromUtf8("NuevoShader" + std::to_string(i) + assets::kSurfaceShaderExtension);
    }
    std::ofstream(path, std::ios::binary) << assets::surfaceShaderTemplate(dialogs::utf8(path.stem()));
    refreshDatabase();
    std::cout << "[Editor] Shader creado: " << dialogs::utf8(path.filename()) << "\n";
    return path;
}

const std::vector<std::string>& EditorApp::projectShaderFiles() {
    if (shader_files_version_ == database_version_) return shader_files_;
    shader_files_version_ = database_version_;
    shader_files_.clear();
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(project_.assetsFolder(), error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (it->is_regular_file(error) && it->path().extension() == assets::kSurfaceShaderExtension) {
            shader_files_.push_back(assetRelative(it->path()));
        }
    }
    std::sort(shader_files_.begin(), shader_files_.end());
    return shader_files_;
}

void EditorApp::drawMaterialShaderSection(assets::MaterialAsset& m, bool& changed, bool& structural) {
    ImGui::SeparatorText("Shader");
    const std::string current = m.shader.empty() ? std::string("Estándar (PBR)") : m.shader;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##shader", current.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable("Estándar (PBR)", m.shader.empty())) {
            m.shader.clear();
            changed = structural = true;
        }
        for (const std::string& file : projectShaderFiles()) {
            if (ImGui::Selectable(file.c_str(), file == m.shader)) {
                m.shader = file;
                changed = structural = true;
            }
        }
        ImGui::Separator();
        if (ImGui::Selectable("+ Nuevo shader...")) {
            const std::filesystem::path path = createShaderAsset({});
            m.shader = assetRelative(path);
            changed = structural = true;
            openScript(path);
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Estándar: el material PBR de siempre.\nUn .crshader: tu propio shader de superficie (GLSL).");
    if (m.shader.empty()) return;

    if (ImGui::SmallButton("Editar shader")) openScript(project_.assetsFolder() / dialogs::fromUtf8(m.shader));
    const assets::SurfaceShaderSource* source = sync_ ? sync_->surfaceShaderSource(m.shader) : nullptr;
    const std::string error = sync_ ? sync_->surfaceShaderError(m.shader) : std::string{};
    if (!error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 110, 110, 255));
        ImGui::TextWrapped("%s", error.c_str());
        ImGui::PopStyleColor();
        ImGui::TextDisabled("Mientras no compile, el material se ve con el shader estándar.");
    }
    if (source == nullptr) return;
    if (source->properties.empty()) {
        ImGui::TextDisabled("Este shader no tiene propiedades.");
        return;
    }
    // Las texturas y factores de arriba siguen valiendo: el shader recibe la
    // superficie ya calculada con ellos (s.albedo, s.roughness...).
    for (const assets::ShaderProperty& p : source->properties) {
        ImGui::PushID(p.name.c_str());
        if (p.type == assets::ShaderPropertyType::Texture) {
            const auto found = m.shader_textures.find(p.name);
            std::string path = found != m.shader_textures.end() ? found->second : std::string{};
            if (materialTextureSlot(p.name.c_str(), path)) {
                if (path.empty()) m.shader_textures.erase(p.name);
                else m.shader_textures[p.name] = path;
                changed = structural = true;
            }
            ImGui::PopID();
            continue;
        }
        const auto it = m.shader_values.find(p.name);
        core::Vec4 value = it != m.shader_values.end() ? it->second : p.value;
        bool edited = false;
        ImGui::SetNextItemWidth(-110.0f);
        switch (p.type) {
            case assets::ShaderPropertyType::Float: edited = ImGui::DragFloat(p.name.c_str(), &value.x, 0.01f); break;
            case assets::ShaderPropertyType::Range: edited = ImGui::SliderFloat(p.name.c_str(), &value.x, p.min, p.max); break;
            case assets::ShaderPropertyType::Color: edited = ImGui::ColorEdit3(p.name.c_str(), &value.x); break;
            case assets::ShaderPropertyType::Vector: edited = ImGui::DragFloat3(p.name.c_str(), &value.x, 0.01f); break;
            default: break;
        }
        if (ImGui::BeginPopupContextItem("reset")) {
            if (ImGui::MenuItem("Valor por defecto")) {
                m.shader_values.erase(p.name);
                changed = true;
            }
            ImGui::EndPopup();
        }
        if (edited) {
            m.shader_values[p.name] = value;
            changed = true;
        }
        ImGui::PopID();
    }
}

}  // namespace cramion::editor
