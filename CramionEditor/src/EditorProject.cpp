// Ventana Proyecto (como la de Unity): arbol de carpetas de Assets/ y rejilla
// con los assets de la carpeta. Se importan archivos (boton, menu o soltando
// desde el Explorador), se arrastran a la escena, la jerarquia o un campo del
// Inspector, se mueven a otra carpeta arrastrandolos y se renombran/borran.
// Los assets son .crdata/.crscene con UUID: moverlos no rompe referencias.
//
// Rendimiento: nada de disco por frame. Windows avisa de los cambios en
// Assets/ (FindFirstChangeNotification) y solo entonces se rehacen la base de
// datos y la cache de carpetas que dibuja el panel.

#include "EditorApp.h"

#include "Dialogs.h"

#include <shellapi.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <iostream>

namespace cramion::editor {

namespace {

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

ImU32 assetColor(assets::AssetType type) {
    switch (type) {
        case assets::AssetType::Model: return IM_COL32(70, 130, 210, 255);
        case assets::AssetType::Environment: return IM_COL32(225, 140, 50, 255);
        case assets::AssetType::Scene: return IM_COL32(150, 95, 200, 255);
        case assets::AssetType::AnimatorController: return IM_COL32(60, 160, 90, 255);
        case assets::AssetType::AnimationClip: return IM_COL32(40, 150, 160, 255);
        default: return IM_COL32(110, 110, 115, 255);
    }
}

Icon assetIcon(assets::AssetType type) {
    switch (type) {
        case assets::AssetType::Model: return Icon::MeshRenderer;
        case assets::AssetType::Environment: return Icon::ReflectionProbe;
        case assets::AssetType::Scene: return Icon::AssetBrowser;
        case assets::AssetType::AnimatorController: return Icon::SkinnedMesh;
        case assets::AssetType::AnimationClip: return Icon::SkinnedMesh;
        case assets::AssetType::Material: return Icon::MeshRenderer;
        default: return Icon::AssetBrowser;
    }
}

// Tarjeta del asset: fondo oscuro y el icono de su tipo en su color.
void drawAssetTile(ImDrawList* draw, const ImGuiLayer& imgui, ImVec2 min, float size, Icon icon, ImU32 color) {
    const ImVec2 max{min.x + size, min.y + size};
    draw->AddRectFilled(min, max, IM_COL32(44, 44, 50, 255), size * 0.1f);
    draw->AddRect(min, max, (color & 0x00FFFFFFu) | 0x90000000u, size * 0.1f, 0, 1.5f);
    const float inner = size * 0.62f;
    imgui.drawIcon(draw, icon, ImVec2(min.x + (size - inner) * 0.5f, min.y + (size - inner) * 0.5f), inner, color);
}

// Imagen (miniatura) encajada en un cuadrado sin deformarla, sobre un
// damero (se ven las transparencias).
void drawThumbnailTile(ImDrawList* draw, ImTextureID texture, ImVec2 image_size, ImVec2 min, float size) {
    const ImVec2 max{min.x + size, min.y + size};
    const float cell = std::max(size / 8.0f, 4.0f);
    draw->AddRectFilled(min, max, IM_COL32(70, 70, 76, 255), size * 0.06f);
    for (float y = 0.0f; y < size; y += cell) {
        for (float x = ((static_cast<int>(y / cell) % 2) != 0 ? cell : 0.0f); x < size; x += cell * 2.0f) {
            draw->AddRectFilled(ImVec2(min.x + x, min.y + y),
                                ImVec2(std::min(min.x + x + cell, max.x), std::min(min.y + y + cell, max.y)),
                                IM_COL32(90, 90, 98, 255));
        }
    }
    const float aspect = image_size.y > 0.0f ? image_size.x / image_size.y : 1.0f;
    ImVec2 fit(size, size);
    if (aspect > 1.0f) fit.y = size / aspect;
    else fit.x = size * aspect;
    const ImVec2 a{min.x + (size - fit.x) * 0.5f, min.y + (size - fit.y) * 0.5f};
    draw->AddImage(texture, a, ImVec2(a.x + fit.x, a.y + fit.y));
    draw->AddRect(min, max, IM_COL32(0, 0, 0, 120), size * 0.06f);
}

}  // namespace

void EditorApp::refreshDatabase() {
    if (database_) {
        database_->refresh();
    }
    ++database_version_;  // la cache del navegador se rehara
    model_previews_.invalidate();  // reimportados: se vuelve a mirar su fecha
    clip_source_.reset();
    clip_source_uuid_ = {};
}

// Sin recorrer el disco: Windows senala el HANDLE cuando cambia algo dentro
// de Assets/ (archivos nuevos, copiados, borrados, renombrados o escritos).
// Una rafaga de cambios (importar escribe varias veces) se agrupa en un solo
// refresco un momento despues.
void EditorApp::watchAssets() {
    if (!has_project_ || !database_) {
        return;
    }
    if (assets_watch_ == nullptr) {
        const HANDLE handle = FindFirstChangeNotificationW(
            project_.assetsFolder().wstring().c_str(), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_LAST_WRITE);
        assets_watch_ = handle == INVALID_HANDLE_VALUE ? nullptr : handle;
    }
    const double now = ImGui::GetTime();
    if (assets_watch_ != nullptr && WaitForSingleObject(assets_watch_, 0) == WAIT_OBJECT_0) {
        refresh_at_ = now + 0.3;
        FindNextChangeNotification(assets_watch_);
    }
    if (refresh_at_ >= 0.0 && now >= refresh_at_) {
        refresh_at_ = -1.0;
        refreshDatabase();
    }
}

// Lee del disco lo que dibuja el panel (arbol, subcarpetas y assets de la
// carpeta actual). Solo se llama si cambio la base de datos o la carpeta.
void EditorApp::rebuildBrowserCache() {
    cached_version_ = database_version_;
    cached_folder_ = current_folder_;

    folder_nodes_.clear();
    std::error_code error;
    const std::function<std::size_t(const std::filesystem::path&)> add =
        [&](const std::filesystem::path& folder) -> std::size_t {
        const std::size_t index = folder_nodes_.size();
        folder_nodes_.push_back(FolderNode{folder, dialogs::utf8(folder.filename()), {}});
        std::vector<std::filesystem::path> children;
        for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
            if (entry.is_directory(error)) children.push_back(entry.path());
        }
        std::sort(children.begin(), children.end());
        for (const std::filesystem::path& child : children) {
            const std::size_t child_index = add(child);
            folder_nodes_[index].children.push_back(child_index);
        }
        return index;
    };
    add(project_.assetsFolder());

    current_subfolders_.clear();
    current_assets_.clear();
    current_images_.clear();
    current_scripts_.clear();
    current_audio_.clear();
    if (current_folder_.empty()) {
        for (const assets::AssetInfo& info : database_->all()) {
            if (info.path.empty()) current_assets_.push_back(info);
        }
    } else {
        for (const auto& entry : std::filesystem::directory_iterator(current_folder_, error)) {
            if (entry.is_directory(error)) current_subfolders_.push_back(entry.path());
        }
        std::sort(current_subfolders_.begin(), current_subfolders_.end());
        current_assets_ = database_->inFolder(current_folder_);
        for (const auto& entry : std::filesystem::directory_iterator(current_folder_, error)) {
            if (!entry.is_regular_file(error)) continue;
            if (isDecalImage(entry.path())) current_images_.push_back(entry.path());
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".lua") current_scripts_.push_back(entry.path());
            if (audio::isAudioFile(entry.path())) current_audio_.push_back(entry.path());
        }
        std::sort(current_images_.begin(), current_images_.end());
        std::sort(current_scripts_.begin(), current_scripts_.end());
        std::sort(current_audio_.begin(), current_audio_.end());
    }
}

void EditorApp::createSceneAsset(const std::filesystem::path& folder) {
    std::filesystem::path path = folder / "Nueva escena.crscene";
    for (int i = 2; std::filesystem::exists(path); ++i) {
        path = folder / ("Nueva escena " + std::to_string(i) + ".crscene");
    }
    ecs::World world;
    world.setSceneName(dialogs::utf8(path.stem()));
    ecs::populateDefaultScene(world);
    std::string error;
    if (ecs::saveScene(world, path, &error)) {
        refreshDatabase();
        std::cout << "[Editor] Escena creada: " << dialogs::utf8(path.filename()) << "\n";
    } else {
        std::cerr << "[Editor] No se pudo crear la escena: " << error << "\n";
    }
}

void EditorApp::drawFolderNode(std::size_t index) {
    const FolderNode& node = folder_nodes_[index];
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_OpenOnDoubleClick;
    if (node.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (node.path == current_folder_) flags |= ImGuiTreeNodeFlags_Selected;
    if (index == 0) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(index + 1), flags, "      %s", node.name.c_str());
    {
        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();
        if (ImGui::IsItemVisible()) folder_drop_zones_.push_back(FolderDropZone{a.x, a.y, b.x, b.y, node.path});
    }
    {
        // Icono de carpeta (abierta si esta desplegada) delante del nombre.
        const ImVec2 min = ImGui::GetItemRectMin();
        const float h = ImGui::GetItemRectSize().y;
        const float icon = h - 2.0f;
        imgui_.drawIcon(ImGui::GetWindowDrawList(), open && !node.children.empty() ? Icon::FolderOpen : Icon::FolderClosed,
                        ImVec2(min.x + ImGui::GetTreeNodeToLabelSpacing() - 2.0f, min.y + 1.0f), icon,
                        IM_COL32(232, 194, 96, 255));
    }
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        current_folder_ = node.path;
    }
    // Soltar un asset en la carpeta: se mueve alli.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            AssetPayload asset{};
            std::memcpy(&asset, payload->Data, sizeof(asset));
            if (const auto info = database_->find(asset.uuid); info && !info->path.empty()) {
                database_->move(asset.uuid, node.path / info->path.filename());
                refreshDatabase();
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (open) {
        // La cache no se rehace durante el dibujo (solo al empezar el panel).
        for (const std::size_t child : node.children) {
            drawFolderNode(child);
        }
        ImGui::TreePop();
    }
}

void EditorApp::drawProject() {
    if (!ImGui::Begin("Proyecto", &show_project_)) {
        ImGui::End();
        return;
    }
    if (cached_version_ != database_version_ || cached_folder_ != current_folder_) {
        rebuildBrowserCache();
    }
    folder_drop_zones_.clear();
    // --- Barra ---
    if (ImGui::Button("Importar...")) {
        const auto files = dialogs::openFiles(
            window_.handle(),
            L"Modelos y cielos (*.obj;*.fbx;*.gltf;*.glb;*.dae;*.hdr)\0*.obj;*.fbx;*.gltf;*.glb;*.dae;*.hdr\0Todos\0*.*\0");
        startImport(files, current_folder_.empty() ? project_.assetsFolder() : current_folder_);
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Carpeta")) {
        // En "Integrados" no se puede escribir: la carpeta va a Assets.
        if (current_folder_.empty()) current_folder_ = project_.assetsFolder();
        createFolderIn(current_folder_);
    }
    ImGui::SetItemTooltip("Carpeta nueva aqui (tambien: arrastrar carpetas desde el Explorador)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputTextWithHint("##buscar", "Buscar...", &project_filter_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    ImGui::SliderFloat("##icono", &icon_size_, 40.0f, 128.0f, "icono %.0f");
    if (!imports_.empty()) {
        ImGui::SameLine();
        const char spinner[] = "|/-\\";
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "%c Importando %zu archivo(s)...",
                           spinner[static_cast<int>(ImGui::GetTime() * 8.0) % 4], imports_.size());
    }

    // --- Arbol de carpetas ---
    ImGui::BeginChild("folders", ImVec2(220.0f, 0.0f), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
    if (ImGui::Selectable("Integrados", current_folder_.empty())) {
        current_folder_.clear();  // vacio = assets integrados
    }
    if (!folder_nodes_.empty()) drawFolderNode(0);
    ImGui::EndChild();
    ImGui::SameLine();

    // --- Rejilla ---
    ImGui::BeginChild("grid", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    // Migas: Assets / Scenes / ...
    if (current_folder_.empty()) {
        ImGui::TextDisabled("Integrados (primitivas)");
    } else {
        std::error_code error;
        const std::filesystem::path relative =
            std::filesystem::relative(current_folder_, project_.assetsFolder(), error);
        std::filesystem::path walk = project_.assetsFolder();
        if (ImGui::SmallButton("Assets")) current_folder_ = walk;
        int crumb = 0;
        for (const auto& part : relative) {
            if (part == ".") continue;
            walk /= part;
            ImGui::SameLine();
            ImGui::TextDisabled(">");
            ImGui::SameLine();
            const std::filesystem::path target = walk;
            ImGui::PushID(crumb++);
            if (ImGui::SmallButton(dialogs::utf8(part).c_str())) current_folder_ = target;
            ImGui::PopID();
        }
    }
    ImGui::Separator();

    const float cell = icon_size_ + 18.0f;
    const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / cell));
    int column = 0;
    const auto next_cell = [&] {
        if (++column < columns) ImGui::SameLine();
        else column = 0;
    };
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const std::string needle = lower(project_filter_);
    int id = 0;

    // Subcarpetas.
    if (!current_folder_.empty() && needle.empty()) {
        const std::vector<std::filesystem::path> folders = current_subfolders_;
        for (const std::filesystem::path& folder : folders) {
            ImGui::PushID(id++);
            ImGui::BeginGroup();
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##folder", ImVec2(cell - 8.0f, icon_size_));
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemVisible()) {
                folder_drop_zones_.push_back(
                    FolderDropZone{pos.x, pos.y, pos.x + cell - 8.0f, pos.y + icon_size_, folder});
            }
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) current_folder_ = folder;
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                    AssetPayload asset{};
                    std::memcpy(&asset, payload->Data, sizeof(asset));
                    if (const auto info = database_->find(asset.uuid); info && !info->path.empty()) {
                        database_->move(asset.uuid, folder / info->path.filename());
                        refreshDatabase();
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopupContextItem("folder_menu")) {
                if (ImGui::MenuItem("Abrir")) current_folder_ = folder;
                if (ImGui::MenuItem("Renombrar")) {
                    renaming_folder_ = folder;
                    folder_rename_buffer_ = dialogs::utf8(folder.filename());
                }
                if (ImGui::MenuItem("Nueva carpeta dentro")) createFolderIn(folder);
                if (ImGui::MenuItem("Mostrar en el Explorador")) {
                    ShellExecuteW(nullptr, L"open", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                std::error_code e2;
                if (ImGui::MenuItem("Borrar (vacía)", nullptr, false, std::filesystem::is_empty(folder, e2))) {
                    std::filesystem::remove(folder, e2);
                    refreshDatabase();
                }
                ImGui::EndPopup();
            }
            if (hovered) draw->AddRectFilled(pos, ImVec2(pos.x + cell - 8.0f, pos.y + icon_size_), IM_COL32(255, 255, 255, 18), 4.0f);
            imgui_.drawIcon(draw, hovered ? Icon::FolderOpen : Icon::FolderClosed,
                            ImVec2(pos.x + (cell - 8.0f - icon_size_) * 0.5f, pos.y), icon_size_,
                            IM_COL32(232, 194, 96, 255));
            if (renaming_folder_ == folder) {
                // Renombrar en el sitio (carpeta recien creada o menu Renombrar).
                ImGui::SetNextItemWidth(cell - 8.0f);
                if (!ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
                const bool done = ImGui::InputText("##rename_folder", &folder_rename_buffer_,
                                                   ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                if (done || ImGui::IsItemDeactivated()) {
                    const std::filesystem::path renamed = folder.parent_path() / dialogs::fromUtf8(folder_rename_buffer_);
                    std::error_code rename_error;
                    if (!folder_rename_buffer_.empty() && renamed != folder && !std::filesystem::exists(renamed)) {
                        // Los .crdata llevan su UUID dentro: moverlos no rompe referencias.
                        std::filesystem::rename(folder, renamed, rename_error);
                        if (rename_error) std::cerr << "[Editor] No se pudo renombrar la carpeta\n";
                        refreshDatabase();
                    }
                    renaming_folder_.clear();
                }
            } else {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cell - 8.0f);
                ImGui::TextWrapped("%s", dialogs::utf8(folder.filename()).c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::EndGroup();
            ImGui::PopID();
            next_cell();
        }
    }

    // Assets de la carpeta (o todos los que coincidan con la busqueda).
    // Copia de la cache: un mover/borrar desde un menu la rehace el frame siguiente.
    std::vector<assets::AssetInfo> items;
    if (!needle.empty()) {
        for (const assets::AssetInfo& info : database_->all()) {
            if (lower(info.name).find(needle) != std::string::npos) items.push_back(info);
        }
    } else {
        items = current_assets_;
    }

    for (const assets::AssetInfo& info : items) {
        ImGui::PushID(id++);
        ImGui::BeginGroup();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##asset", ImVec2(cell - 8.0f, icon_size_));
        const bool hovered = ImGui::IsItemHovered();
        // Un material se ve y se edita en el Inspector al elegirlo.
        if (info.type == assets::AssetType::Material && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            inspected_material_ = info.uuid;
        }
        if (info.type == assets::AssetType::Material && inspected_material_ == info.uuid) {
            draw->AddRectFilled(pos, ImVec2(pos.x + cell - 8.0f, pos.y + icon_size_), IM_COL32(255, 160, 40, 40), 4.0f);
        }
        if (hovered) {
            ImGui::SetItemTooltip("%s\n%s\n%s%.1f MB", info.name.c_str(), assets::assetTypeName(info.type),
                                  info.source.empty() ? "" : ("Origen: " + dialogs::utf8(info.source) + "\n").c_str(),
                                  static_cast<double>(info.size_bytes) / (1024.0 * 1024.0));
            draw->AddRectFilled(pos, ImVec2(pos.x + cell - 8.0f, pos.y + icon_size_), IM_COL32(255, 255, 255, 18), 4.0f);
        }
        // Doble clic: una escena se abre; un modelo se pone en la escena.
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (info.type == assets::AssetType::Scene) runOrAskToSave(PendingAction::OpenScene, info.path);
            if (info.type == assets::AssetType::Model) instantiateAsset(info.uuid, {}, std::nullopt);
            if (info.type == assets::AssetType::Environment) assignEnvironment(info.uuid);
            if (info.type == assets::AssetType::AnimatorController) openAnimatorEditor(info.uuid);
        }
        if (ImGui::BeginDragDropSource()) {
            const AssetPayload payload{info.uuid, info.type};
            ImGui::SetDragDropPayload(kAssetPayload, &payload, sizeof(payload));
            ImGui::Text("%s (%s)", info.name.c_str(), assets::assetTypeName(info.type));
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginPopupContextItem("asset_menu")) {
            if (info.type == assets::AssetType::Scene && ImGui::MenuItem("Abrir")) {
                runOrAskToSave(PendingAction::OpenScene, info.path);
            }
            if (info.type == assets::AssetType::Model && ImGui::MenuItem("Poner en la escena")) {
                instantiateAsset(info.uuid, {}, std::nullopt);
            }
            if (info.type == assets::AssetType::Model && ImGui::BeginMenu("Animaciones")) {
                drawModelAssetAnimationsMenu(info);
                ImGui::EndMenu();
            }
            if (info.type == assets::AssetType::Environment && ImGui::MenuItem("Usar como cielo")) {
                assignEnvironment(info.uuid);
            }
            if (info.type == assets::AssetType::Material) {
                if (ImGui::MenuItem("Editar")) inspected_material_ = info.uuid;
                if (ImGui::MenuItem("Asignar a la selección", nullptr, false, !selection_.empty())) {
                    bool any = false;
                    for (ecs::Entity e : selectedEntities()) any = applyMaterial(e, info.uuid, -1) || any;
                    if (any) commit();
                }
                if (ImGui::MenuItem("Duplicar")) {
                    assets::MaterialAsset copy;
                    if (assets::loadMaterial(info.path, copy)) {
                        copy.uuid = {};
                        std::filesystem::path target = info.path.parent_path() /
                                                       (info.path.stem().wstring() + L" copia.crmat");
                        for (int i = 2; std::filesystem::exists(target); ++i) {
                            target = info.path.parent_path() /
                                     (info.path.stem().wstring() + L" copia " + std::to_wstring(i) + L".crmat");
                        }
                        if (assets::saveMaterial(copy, target)) refreshDatabase();
                    }
                }
            }
            if (info.type == assets::AssetType::AnimatorController) {
                if (ImGui::MenuItem("Abrir en el editor Animator")) openAnimatorEditor(info.uuid);
                if (ImGui::MenuItem("Asignar a la selección", nullptr, false, !selection_.empty())) {
                    assignAnimatorToSelection(info.uuid);
                }
            }
            const bool file = !info.path.empty();
            if (ImGui::MenuItem("Renombrar", nullptr, false, file)) {
                renaming_asset_ = info.uuid;
                asset_rename_buffer_ = info.name;
            }
            if (ImGui::MenuItem("Mostrar en el Explorador", nullptr, false, file)) {
                const std::wstring args = L"/select,\"" + info.path.wstring() + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            }
            if (ImGui::MenuItem("Copiar UUID")) ImGui::SetClipboardText(info.uuid.toString().c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Borrar", nullptr, false, file)) pending_delete_asset_ = info.uuid;
            ImGui::EndPopup();
        }
        const float icon = icon_size_ * 0.82f;
        const ImVec2 icon_pos(pos.x + (cell - 8.0f - icon) * 0.5f, pos.y + (icon_size_ - icon) * 0.5f);
        // Cielos: la miniatura de su HDR de origen si sigue en disco.
        ImVec2 thumb_size{};
        std::error_code source_error;
        ImTextureID preview = info.type == assets::AssetType::Environment && !info.source.empty() &&
                                      std::filesystem::exists(info.source, source_error)
                                  ? imgui_.thumbnail(info.source, &thumb_size)
                                  : 0;
        // Modelos: su miniatura dibujada (en cache en Library/Thumbnails).
        if (info.type == assets::AssetType::Model) {
            if (const auto png = model_previews_.preview(info.uuid, info.path, info.name)) {
                preview = imgui_.thumbnail(*png, &thumb_size);
            }
        }
        if (info.type == assets::AssetType::Material) {
            draw->AddRectFilled(icon_pos, ImVec2(icon_pos.x + icon, icon_pos.y + icon), IM_COL32(44, 44, 50, 255),
                                icon * 0.1f);
            drawMaterialBall(draw, ImVec2(icon_pos.x + icon * 0.5f, icon_pos.y + icon * 0.5f), icon * 0.4f, info.uuid);
        } else if (preview != 0) {
            drawThumbnailTile(draw, preview, thumb_size, icon_pos, icon);
        } else {
            drawAssetTile(draw, imgui_, icon_pos, icon, assetIcon(info.type), assetColor(info.type));
        }

        if (renaming_asset_ == info.uuid) {
            ImGui::SetNextItemWidth(cell - 8.0f);
            if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
            const bool done = ImGui::InputText("##rename", &asset_rename_buffer_,
                                               ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            if (done || ImGui::IsItemDeactivated()) {
                if (!asset_rename_buffer_.empty() && asset_rename_buffer_ != info.name) {
                    database_->move(info.uuid, info.path.parent_path() /
                                                   (dialogs::fromUtf8(asset_rename_buffer_).wstring() +
                                                    info.path.extension().wstring()));
                    refreshDatabase();
                }
                renaming_asset_ = {};
            }
        } else {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cell - 8.0f);
            ImGui::TextWrapped("%s", info.name.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndGroup();
        ImGui::PopID();
        next_cell();
    }
    // Imagenes (texturas para los decals): arrastrarlas a la escena las estampa.
    if (needle.empty() && cached_version_ == database_version_) {
        const std::vector<std::filesystem::path> images = current_images_;
        for (const std::filesystem::path& image : images) {
            ImGui::PushID(id++);
            ImGui::BeginGroup();
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##image", ImVec2(cell - 8.0f, icon_size_));
            const bool hovered = ImGui::IsItemHovered();
            const std::string name = dialogs::utf8(image.filename());
            if (hovered) {
                ImGui::SetItemTooltip("%s\nImagen: arrastrala a la escena para estamparla\nDoble clic: pincel de estampar",
                                      name.c_str());
                draw->AddRectFilled(pos, ImVec2(pos.x + cell - 8.0f, pos.y + icon_size_), IM_COL32(255, 255, 255, 18), 4.0f);
            }
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                stamp_brush_.texture = decalImageInAssets(image);
                stamp_brush_.type = 0;
                stamp_mode_ = true;
            }
            if (ImGui::BeginDragDropSource()) {
                const std::string path = dialogs::utf8(image);
                ImGui::SetDragDropPayload(kImagePayload, path.c_str(), path.size() + 1);
                ImGui::Text("%s (imagen)", name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginPopupContextItem("image_menu")) {
                if (ImGui::MenuItem("Crear material")) {
                    // Con sus companeras (_normal, _rough, _ao...) si las hay.
                    const std::string relative = decalImageInAssets(image);
                    if (!relative.empty()) createMaterialAsset(image.parent_path(), relative);
                }
                if (ImGui::MenuItem("Estampar con esta imagen")) {
                    stamp_brush_.texture = decalImageInAssets(image);
                    stamp_brush_.type = 0;
                    stamp_mode_ = true;
                }
                bool any_decal = false;
                for (const ecs::Entity e : selectedEntities()) any_decal = any_decal || e.has<ecs::Decal>();
                if (ImGui::MenuItem("Asignar a los decals seleccionados", nullptr, false, any_decal)) {
                    const std::string texture = decalImageInAssets(image);
                    for (ecs::Entity e : selectedEntities()) {
                        if (ecs::Decal* d = e.tryGet<ecs::Decal>()) d->texture = texture;
                    }
                    commit();
                }
                if (ImGui::MenuItem("Mostrar en el Explorador")) {
                    const std::wstring args = L"/select,\"" + image.wstring() + L"\"";
                    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
                }
                ImGui::EndPopup();
            }
            // La imagen de verdad (se decodifica en otro hilo; mientras, su icono).
            const float icon = icon_size_ * 0.82f;
            const ImVec2 icon_pos(pos.x + (cell - 8.0f - icon) * 0.5f, pos.y + (icon_size_ - icon) * 0.5f);
            ImVec2 thumb_size{};
            if (const ImTextureID preview = imgui_.thumbnail(image, &thumb_size); preview != 0) {
                drawThumbnailTile(draw, preview, thumb_size, icon_pos, icon);
            } else {
                drawAssetTile(draw, imgui_, icon_pos, icon, Icon::Decal, IM_COL32(200, 80, 140, 255));
            }
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cell - 8.0f);
            ImGui::TextWrapped("%s", name.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            ImGui::PopID();
            next_cell();
        }
        // Scripts (.lua) y audios: arrastrarlos a un objeto los engancha.
        const auto loose_tile = [&](const std::filesystem::path& file, bool is_script) {
            ImGui::PushID(id++);
            ImGui::BeginGroup();
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##loose", ImVec2(cell - 8.0f, icon_size_));
            const bool hovered = ImGui::IsItemHovered();
            const std::string name = dialogs::utf8(file.filename());
            if (hovered) {
                ImGui::SetItemTooltip(is_script ? "%s\nScript Lua: arrastralo a un objeto\nDoble clic: editar"
                                                : "%s\nAudio: arrastralo a un objeto (Audio Source) o a la escena\nDoble clic: escuchar",
                                      name.c_str());
                draw->AddRectFilled(pos, ImVec2(pos.x + cell - 8.0f, pos.y + icon_size_), IM_COL32(255, 255, 255, 18), 4.0f);
            }
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (is_script) {
                    openScript(file);
                } else if (audio_.previewing()) {
                    audio_.stopPreview();
                } else {
                    audio_.preview(file);
                }
            }
            if (ImGui::BeginDragDropSource()) {
                const std::string path = dialogs::utf8(file);
                ImGui::SetDragDropPayload(is_script ? kScriptPayload : kAudioPayload, path.c_str(), path.size() + 1);
                ImGui::Text("%s (%s)", name.c_str(), is_script ? "script" : "audio");
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginPopupContextItem("loose_menu")) {
                if (is_script && ImGui::MenuItem("Editar")) openScript(file);
                if (!is_script && ImGui::MenuItem(audio_.previewing() ? "Parar" : "Escuchar")) {
                    if (audio_.previewing()) audio_.stopPreview();
                    else audio_.preview(file);
                }
                if (ImGui::MenuItem("Asignar a la seleccion", nullptr, false, !selection_.empty())) {
                    for (ecs::Entity e : selectedEntities()) {
                        if (is_script) {
                            scripting::Script& s = e.has<scripting::Script>() ? e.get<scripting::Script>() : e.add<scripting::Script>();
                            s.file = assetRelative(file);
                        } else {
                            audio::AudioSource& a = e.has<audio::AudioSource>() ? e.get<audio::AudioSource>() : e.add<audio::AudioSource>();
                            a.clip = assetRelative(file);
                        }
                    }
                    commit();
                }
                if (ImGui::MenuItem("Mostrar en el Explorador")) {
                    const std::wstring args = L"/select,\"" + file.wstring() + L"\"";
                    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
                }
                ImGui::EndPopup();
            }
            const float icon = icon_size_ * 0.82f;
            const ImVec2 icon_pos(pos.x + (cell - 8.0f - icon) * 0.5f, pos.y + (icon_size_ - icon) * 0.5f);
            if (is_script) {
                drawAssetTile(draw, imgui_, icon_pos, icon, Icon::AssetBrowser, IM_COL32(90, 170, 250, 255));
                draw->AddText(ImVec2(icon_pos.x + icon * 0.34f, icon_pos.y + icon * 0.68f), IM_COL32(90, 170, 250, 255), "LUA");
            } else {
                drawAssetTile(draw, imgui_, icon_pos, icon, Icon::AudioSource, IM_COL32(240, 170, 60, 255));
            }
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cell - 8.0f);
            ImGui::TextWrapped("%s", name.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            ImGui::PopID();
            next_cell();
        };
        const std::vector<std::filesystem::path> scripts = current_scripts_;
        for (const std::filesystem::path& file : scripts) loose_tile(file, true);
        const std::vector<std::filesystem::path> sounds = current_audio_;
        for (const std::filesystem::path& file : sounds) loose_tile(file, false);
    }
    if (items.empty() && (current_folder_.empty() || !needle.empty())) {
        ImGui::TextDisabled("Sin assets.");
    }
    // Clic derecho en el fondo: acciones de la carpeta.
    if (!current_folder_.empty() &&
        ImGui::BeginPopupContextWindow("grid_menu", ImGuiPopupFlags_MouseButtonRight |
                                                        ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem("Importar...")) {
            const auto files = dialogs::openFiles(
                window_.handle(),
                L"Modelos y cielos (*.obj;*.fbx;*.gltf;*.glb;*.dae;*.hdr)\0*.obj;*.fbx;*.gltf;*.glb;*.dae;*.hdr\0Todos\0*.*\0");
            startImport(files, current_folder_);
        }
        if (ImGui::BeginMenu("Crear")) {
            if (ImGui::MenuItem("Carpeta")) createFolderIn(current_folder_);
            if (ImGui::MenuItem("Escena")) createSceneAsset(current_folder_);
            if (ImGui::MenuItem("Animator")) createAnimatorAsset(current_folder_);
            if (ImGui::MenuItem("Material")) createMaterialAsset(current_folder_);
            if (ImGui::MenuItem("Script Lua")) createScriptAsset(current_folder_, {});
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Mostrar en el Explorador")) {
            ShellExecuteW(nullptr, L"open", current_folder_.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::EndPopup();
    }

    if (!current_folder_.empty() && items.empty() && needle.empty()) {
        ImGui::NewLine();
        ImGui::TextDisabled("Arrastra aquí archivos desde el Explorador (FBX, OBJ, glTF, HDR)\n"
                            "o usa Importar: se convierten a .crdata con su UUID.");
    }
    ImGui::EndChild();
    ImGui::End();
}

}  // namespace cramion::editor
