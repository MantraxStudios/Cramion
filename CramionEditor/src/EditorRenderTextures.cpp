// Render Textures (.crrt) en el editor: crearlas, su Inspector (tamano y lo
// que se ve en ella ahora mismo) y como usarlas.

#include "EditorApp.h"

#include "Dialogs.h"

#include <CramionCore/asset/RenderTextureAsset.h>

#include <imgui.h>

#include <algorithm>
#include <iostream>

namespace cramion::editor {

void EditorApp::createRenderTextureAsset(const std::filesystem::path& folder) {
    assets::RenderTextureAsset texture;
    std::filesystem::path path = folder / dialogs::fromUtf8(std::string("Nueva Render Texture") + assets::kRenderTextureExtension);
    for (int i = 2; std::filesystem::exists(path); ++i) {
        path = folder / dialogs::fromUtf8("Nueva Render Texture " + std::to_string(i) + assets::kRenderTextureExtension);
    }
    std::string error;
    if (!assets::saveRenderTexture(texture, path, &error)) {
        std::cerr << "[Editor] No se pudo crear la Render Texture: " << error << "\n";
        return;
    }
    refreshDatabase();
    std::cout << "[Editor] Render Texture creada: " << dialogs::utf8(path.filename()) << "\n";
    inspected_material_ = {};
    inspected_render_texture_ = texture.uuid;
}

void EditorApp::drawRenderTextureEditor(const Uuid& uuid) {
    const auto info = database_->find(uuid);
    if (!info || info->type != assets::AssetType::RenderTexture) {
        inspected_render_texture_ = {};
        return;
    }
    // Se lee del disco al cambiar de asset o si otro lo cambio.
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(info->path, ec);
    if (render_texture_edit_uuid_ != uuid || stamp != render_texture_edit_stamp_) {
        std::string error;
        if (!assets::loadRenderTexture(info->path, render_texture_edit_, &error)) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "No se pudo leer: %s", error.c_str());
            return;
        }
        render_texture_edit_uuid_ = uuid;
        render_texture_edit_stamp_ = stamp;
    }
    assets::RenderTextureAsset& t = render_texture_edit_;

    ImGui::TextUnformatted(info->name.c_str());
    ImGui::TextDisabled("Render Texture  (%s)", assetRelative(info->path).c_str());
    ImGui::Separator();

    bool changed = false;
    ImGui::SetNextItemWidth(90.0f);
    changed |= ImGui::InputInt("Ancho", &t.width, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    changed |= ImGui::InputInt("Alto", &t.height, 0);
    ImGui::TextDisabled("Tamaño:");
    for (const int side : {128, 256, 512, 1024, 2048}) {
        ImGui::SameLine();
        const std::string label = std::to_string(side);
        if (ImGui::SmallButton(label.c_str())) {
            t.width = t.height = side;
            changed = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("16:9")) {
        t.height = std::max(1, t.width * 9 / 16);
        changed = true;
    }
    if (changed) {
        t.width = std::clamp(t.width, 1, 8192);
        t.height = std::clamp(t.height, 1, 8192);
        std::string error;
        if (!assets::saveRenderTexture(t, info->path, &error)) {
            std::cerr << "[Editor] No se pudo guardar la Render Texture: " << error << "\n";
        }
        render_texture_edit_stamp_ = std::filesystem::last_write_time(info->path, ec);
    }

    // Lo que tiene ahora (si alguna camara la usa, se ve en vivo).
    ImGui::Spacing();
    const std::int32_t id = sync_ ? sync_->renderTextureId(uuid) : -1;
    if (const ImTextureID texture = id >= 0 ? imgui_.renderTexture(id) : 0; texture != 0) {
        const float width = std::max(ImGui::GetContentRegionAvail().x, 32.0f);
        const float aspect = static_cast<float>(t.height) / static_cast<float>(std::max(t.width, 1));
        ImGui::Image(texture, ImVec2(width, std::min(width * aspect, 600.0f)));
    } else {
        ImGui::TextDisabled("(sin imagen todavía)");
    }
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Úsala así: ponla en el campo Target Texture de una Camera (lo que ve esa cámara se dibuja aquí en vez de en la "
        "pantalla) y arrástrala al hueco de Color o de Emisión de un material. Pantallas, cámaras de seguridad, "
        "espejos, minimapas o retratos, como los RenderTexture de Unity.");
    ImGui::TextDisabled("Cada cámara con Target Texture dibuja la escena otra vez: úsalas con medida.");
}

}  // namespace cramion::editor
