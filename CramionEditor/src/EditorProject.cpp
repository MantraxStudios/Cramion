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
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <fstream>
#include <iostream>

namespace cramion::editor {

namespace {

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

Icon assetIcon(assets::AssetType type) {
    switch (type) {
        case assets::AssetType::Model: return Icon::MeshRenderer;
        case assets::AssetType::Environment: return Icon::ReflectionProbe;
        case assets::AssetType::Scene: return Icon::AssetBrowser;
        case assets::AssetType::AnimatorController: return Icon::SkinnedMesh;
        case assets::AssetType::AnimationClip: return Icon::SkinnedMesh;
        case assets::AssetType::Material: return Icon::MeshRenderer;
        case assets::AssetType::Prefab: return Icon::ColliderBox;
        default: return Icon::AssetBrowser;
    }
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
    const auto refresh_start = std::chrono::steady_clock::now();
    ++frame_refreshes_;
    if (database_) {
        database_->refresh();
    }
    ++database_version_;  // la cache del navegador se rehara
    model_previews_.invalidate();  // reimportados: se vuelve a mirar su fecha
    clip_source_.reset();
    clip_source_uuid_ = {};
    syncPrefabInstances();  // un .crprefab cambiado en disco
    if (sync_) sync_->reloadSurfaceShaders();  // un .crshader editado fuera del editor
    frame_refresh_ms_ +=
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - refresh_start).count();
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
    current_shaders_.clear();
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
            if (ext == assets::kSurfaceShaderExtension) current_shaders_.push_back(entry.path());
            if (audio::isAudioFile(entry.path())) current_audio_.push_back(entry.path());
        }
        std::sort(current_images_.begin(), current_images_.end());
        std::sort(current_scripts_.begin(), current_scripts_.end());
        std::sort(current_shaders_.begin(), current_shaders_.end());
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

// =============================================================================
// Navegador de contenido (como el Content Browser de Unreal)
// =============================================================================

namespace {

constexpr ImU32 kBrand = IM_COL32(10, 176, 255, 255);

// Filtros por tipo (bits de browser_filter_).
enum BrowserFilter : std::uint32_t {
    kFilterModels = 1u << 0,
    kFilterMaterials = 1u << 1,
    kFilterTextures = 1u << 2,
    kFilterScenes = 1u << 3,
    kFilterPrefabs = 1u << 4,
    kFilterScripts = 1u << 5,
    kFilterShaders = 1u << 6,
    kFilterAudio = 1u << 7,
    kFilterSkies = 1u << 8,
    kFilterAnimation = 1u << 9,
};
struct FilterInfo {
    std::uint32_t bit;
    const char* name;
    ImU32 color;
};
constexpr FilterInfo kFilters[] = {
    {kFilterModels, "Modelos", IM_COL32(70, 130, 210, 255)},    {kFilterMaterials, "Materiales", IM_COL32(60, 190, 110, 255)},
    {kFilterTextures, "Texturas", IM_COL32(200, 80, 140, 255)}, {kFilterScenes, "Escenas", IM_COL32(150, 95, 200, 255)},
    {kFilterPrefabs, "Prefabs", IM_COL32(95, 170, 255, 255)},    {kFilterScripts, "Scripts", IM_COL32(90, 170, 250, 255)},
    {kFilterShaders, "Shaders", IM_COL32(190, 120, 255, 255)},   {kFilterAudio, "Audio", IM_COL32(240, 170, 60, 255)},
    {kFilterSkies, "Cielos", IM_COL32(225, 140, 50, 255)},       {kFilterAnimation, "Animación", IM_COL32(40, 150, 160, 255)},
};

using Item = EditorApp::BrowserItem;
using Kind = EditorApp::BrowserItem::Kind;

std::uint32_t categoryOf(const Item& item) {
    switch (item.kind) {
        case Kind::Folder: return 0;
        case Kind::Image: return kFilterTextures;
        case Kind::Script: return kFilterScripts;
        case Kind::Audio: return kFilterAudio;
        case Kind::Shader: return kFilterShaders;
        case Kind::Asset: break;
    }
    switch (item.info.type) {
        case assets::AssetType::Model: return kFilterModels;
        case assets::AssetType::Material: return kFilterMaterials;
        case assets::AssetType::Scene: return kFilterScenes;
        case assets::AssetType::Prefab: return kFilterPrefabs;
        case assets::AssetType::Environment: return kFilterSkies;
        case assets::AssetType::AnimatorController:
        case assets::AssetType::AnimationClip: return kFilterAnimation;
        default: return 0;
    }
}

ImU32 itemColor(const Item& item) {
    const std::uint32_t category = categoryOf(item);
    for (const FilterInfo& f : kFilters) {
        if (f.bit == category) return f.color;
    }
    return IM_COL32(110, 110, 115, 255);
}

Icon itemIcon(const Item& item) {
    switch (item.kind) {
        case Kind::Folder: return Icon::FolderClosed;
        case Kind::Image: return Icon::Decal;
        case Kind::Script: return Icon::AssetBrowser;
        case Kind::Audio: return Icon::AudioSource;
        case Kind::Shader: return Icon::MeshRenderer;
        case Kind::Asset: break;
    }
    if (item.info.type == assets::AssetType::Prefab) return Icon::ColliderBox;
    return assetIcon(item.info.type);
}

std::string sizeText(std::uint64_t bytes) {
    char text[32];
    if (bytes >= (1ull << 30)) std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / (1ull << 30));
    else if (bytes >= (1ull << 20)) std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1ull << 20));
    else if (bytes >= 1024) std::snprintf(text, sizeof(text), "%.0f KB", static_cast<double>(bytes) / 1024.0);
    else std::snprintf(text, sizeof(text), "%llu B", static_cast<unsigned long long>(bytes));
    return text;
}

// Texto recortado con "..." para que quepa en `width` (hasta `lines` lineas).
std::string fitText(const std::string& text, float width, int lines) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width * static_cast<float>(lines) * 0.95f) return text;
    std::string cut = text;
    while (!cut.empty() && ImGui::CalcTextSize((cut + "...").c_str()).x > width * static_cast<float>(lines) * 0.92f) {
        cut.pop_back();
        while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80) cut.pop_back();  // UTF-8
    }
    return cut + "...";
}

}  // namespace

void EditorApp::drawFolderNode(std::size_t index) {
    const FolderNode& node = folder_nodes_[index];
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_FramePadding;
    if (node.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (node.path == current_folder_) flags |= ImGuiTreeNodeFlags_Selected;
    if (index == 0) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    // La carpeta actual deja sus padres abiertos.
    if (!current_folder_.empty() && node.path != current_folder_) {
        const std::string mine = node.path.string();
        const std::string current = current_folder_.string();
        if (current.size() > mine.size() && current.compare(0, mine.size(), mine) == 0) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        }
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 3.0f));
    const float label_x = ImGui::GetCursorScreenPos().x;
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(index + 1), flags, "%s", "");
    ImGui::PopStyleVar();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    if (ImGui::IsItemVisible()) folder_drop_zones_.push_back(FolderDropZone{min.x, min.y, max.x, max.y, node.path});
    {
        // Icono de carpeta y nombre (posicion exacta, como la Jerarquia).
        const float h = max.y - min.y;
        const float x = label_x + ImGui::GetTreeNodeToLabelSpacing();
        const float icon = h - 6.0f;
        imgui_.drawIcon(ImGui::GetWindowDrawList(), open && !node.children.empty() ? Icon::FolderOpen : Icon::FolderClosed,
                        ImVec2(x, min.y + 3.0f), icon, IM_COL32(232, 194, 96, 255));
        ImGui::GetWindowDrawList()->AddText(ImVec2(x + icon + 6.0f, min.y + (h - ImGui::GetTextLineHeight()) * 0.5f),
                                            ImGui::GetColorU32(ImGuiCol_Text), index == 0 ? "Assets" : node.name.c_str());
    }
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) navigateTo(node.path);
    // Soltar un asset en la carpeta: se mueve alli; un objeto: prefab.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            AssetPayload asset{};
            std::memcpy(&asset, payload->Data, sizeof(asset));
            if (const auto info = database_->find(asset.uuid); info && !info->path.empty()) {
                database_->move(asset.uuid, node.path / info->path.filename());
                refreshDatabase();
            }
        }
        if (ImGui::AcceptDragDropPayload(kEntityPayload)) createPrefabsFromSelection(node.path);
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("folder_tree_menu")) {
        Item folder;
        folder.kind = Kind::Folder;
        folder.path = node.path;
        folder.name = node.name;
        browserItemMenu(folder);
        ImGui::EndPopup();
    }
    if (open) {
        for (const std::size_t child : node.children) drawFolderNode(child);
        ImGui::TreePop();
    }
}

void EditorApp::navigateTo(const std::filesystem::path& folder) {
    if (folder == current_folder_) return;
    browser_back_.push_back(current_folder_);
    if (browser_back_.size() > 64) browser_back_.erase(browser_back_.begin());
    browser_forward_.clear();
    current_folder_ = folder;
    browser_selection_.clear();
    project_filter_.clear();
}

void EditorApp::loadBrowserFavorites() {
    browser_favorites_.clear();
    std::ifstream in(project_.settingsFolder() / "BrowserFavorites.txt", std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const std::filesystem::path folder = project_.assetsFolder() / dialogs::fromUtf8(line);
        std::error_code error;
        if (std::filesystem::is_directory(folder, error)) browser_favorites_.push_back(folder);
    }
}

void EditorApp::saveBrowserFavorites() {
    std::error_code error;
    std::filesystem::create_directories(project_.settingsFolder(), error);
    std::ofstream out(project_.settingsFolder() / "BrowserFavorites.txt", std::ios::binary | std::ios::trunc);
    for (const std::filesystem::path& folder : browser_favorites_) out << assetRelative(folder) << "\n";
}

// Lo que se ve: la carpeta actual o, con busqueda, todo el proyecto; y los
// filtros por tipo. Solo cuando cambia algo (no cada frame).
void EditorApp::buildBrowserItems() {
    browser_cached_filter_ = project_filter_;
    browser_cached_bits_ = browser_filter_;
    browser_items_.clear();
    const std::string needle = lower(project_filter_);
    const bool searching = !needle.empty();
    const auto wanted = [&](const Item& item) {
        if (browser_filter_ != 0 && (categoryOf(item) & browser_filter_) == 0) return false;
        return !searching || lower(item.name).find(needle) != std::string::npos;
    };
    const auto file_item = [](const std::filesystem::path& path, Kind kind, const char* type) {
        Item item;
        item.kind = kind;
        item.path = path;
        item.name = dialogs::utf8(path.filename());
        item.type = type;
        std::error_code error;
        item.size = std::filesystem::file_size(path, error);
        return item;
    };

    if (searching || (browser_filter_ != 0 && !current_folder_.empty())) {
        // Busqueda o filtro: todo el proyecto (como Unreal con "Search all").
        if (browser_all_loose_.empty() || cached_version_ != database_version_) {
            browser_all_loose_.clear();
            std::error_code error;
            for (auto it = std::filesystem::recursive_directory_iterator(project_.assetsFolder(), error);
                 !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
                if (!it->is_regular_file(error)) continue;
                const std::filesystem::path& p = it->path();
                std::string ext = lower(p.extension().string());
                if (isDecalImage(p)) browser_all_loose_.push_back(file_item(p, Kind::Image, "Textura"));
                else if (ext == ".lua") browser_all_loose_.push_back(file_item(p, Kind::Script, "Script Lua"));
                else if (ext == assets::kSurfaceShaderExtension) browser_all_loose_.push_back(file_item(p, Kind::Shader, "Shader"));
                else if (audio::isAudioFile(p)) browser_all_loose_.push_back(file_item(p, Kind::Audio, "Audio"));
            }
        }
        for (const assets::AssetInfo& info : database_->all()) {
            if (info.path.empty() && !current_folder_.empty()) continue;
            Item item;
            item.kind = Kind::Asset;
            item.path = info.path;
            item.name = info.name;
            item.type = assets::assetTypeName(info.type);
            item.info = info;
            item.size = info.size_bytes;
            if (wanted(item)) browser_items_.push_back(std::move(item));
        }
        for (const Item& item : browser_all_loose_) {
            if (wanted(item)) browser_items_.push_back(item);
        }
    } else {
        for (const std::filesystem::path& folder : current_subfolders_) {
            Item item;
            item.kind = Kind::Folder;
            item.path = folder;
            item.name = dialogs::utf8(folder.filename());
            item.type = "Carpeta";
            browser_items_.push_back(std::move(item));
        }
        for (const assets::AssetInfo& info : current_assets_) {
            Item item;
            item.kind = Kind::Asset;
            item.path = info.path;
            item.name = info.name;
            item.type = assets::assetTypeName(info.type);
            item.info = info;
            item.size = info.size_bytes;
            if (wanted(item)) browser_items_.push_back(std::move(item));
        }
        for (const std::filesystem::path& p : current_images_) browser_items_.push_back(file_item(p, Kind::Image, "Textura"));
        for (const std::filesystem::path& p : current_scripts_) browser_items_.push_back(file_item(p, Kind::Script, "Script Lua"));
        for (const std::filesystem::path& p : current_shaders_) browser_items_.push_back(file_item(p, Kind::Shader, "Shader"));
        for (const std::filesystem::path& p : current_audio_) browser_items_.push_back(file_item(p, Kind::Audio, "Audio"));
    }
    // Carpetas primero; luego por tipo y por nombre (como Unreal).
    std::stable_sort(browser_items_.begin(), browser_items_.end(), [](const Item& a, const Item& b) {
        if ((a.kind == Kind::Folder) != (b.kind == Kind::Folder)) return a.kind == Kind::Folder;
        if (a.type != b.type) return a.type < b.type;
        return lower(a.name) < lower(b.name);
    });
}

bool EditorApp::browserSelected(const BrowserItem& item) const {
    const std::string key = item.key();
    return std::find(browser_selection_.begin(), browser_selection_.end(), key) != browser_selection_.end();
}

// Clic: solo este; Ctrl: anadir/quitar; Mayus: rango desde el ultimo.
void EditorApp::browserClick(const BrowserItem& item, std::size_t index) {
    const std::string key = item.key();
    ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl) {
        const auto it = std::find(browser_selection_.begin(), browser_selection_.end(), key);
        if (it != browser_selection_.end()) browser_selection_.erase(it);
        else browser_selection_.push_back(key);
    } else if (io.KeyShift && !browser_selection_.empty()) {
        std::size_t anchor = index;
        for (std::size_t i = 0; i < browser_items_.size(); ++i) {
            if (browser_items_[i].key() == browser_selection_.back()) anchor = i;
        }
        browser_selection_.clear();
        for (std::size_t i = std::min(anchor, index); i <= std::max(anchor, index); ++i) {
            browser_selection_.push_back(browser_items_[i].key());
        }
    } else {
        browser_selection_ = {key};
    }
    // Un material elegido se ve y se edita en el Inspector, pero al SOLTAR
    // sin arrastrar (como Unity): si se arrastra a los objetos seleccionados
    // en la Jerarquia, el Inspector no debe cambiar ni perder esa seleccion.
    if (item.kind == Kind::Asset && item.info.type == assets::AssetType::Material && !io.KeyCtrl && !io.KeyShift) {
        pending_inspect_material_ = item.info.uuid;
    }
}

void EditorApp::openBrowserItem(const BrowserItem& item) {
    switch (item.kind) {
        case Kind::Folder: navigateTo(item.path); return;
        case Kind::Script:
        case Kind::Shader: openScript(item.path); return;
        case Kind::Audio:
            if (audio_.previewing()) audio_.stopPreview();
            else audio_.preview(item.path);
            return;
        case Kind::Image:
            stamp_brush_.texture = decalImageInAssets(item.path);
            stamp_brush_.type = 0;
            stamp_mode_ = true;
            return;
        case Kind::Asset: break;
    }
    const assets::AssetInfo& info = item.info;
    switch (info.type) {
        case assets::AssetType::Scene: runOrAskToSave(PendingAction::OpenScene, info.path); break;
        case assets::AssetType::Model: instantiateAsset(info.uuid, {}, std::nullopt); break;
        case assets::AssetType::Prefab: instantiatePrefabAsset(info.uuid, {}, std::nullopt); break;
        case assets::AssetType::Environment: assignEnvironment(info.uuid); break;
        case assets::AssetType::AnimatorController: openAnimatorEditor(info.uuid); break;
        case assets::AssetType::Material: inspected_material_ = info.uuid; break;
        default: break;
    }
}

void EditorApp::browserDragSource(const BrowserItem& item) {
    if (!ImGui::BeginDragDropSource()) return;
    switch (item.kind) {
        case Kind::Asset: {
            const AssetPayload payload{item.info.uuid, item.info.type};
            ImGui::SetDragDropPayload(kAssetPayload, &payload, sizeof(payload));
            break;
        }
        case Kind::Image: {
            const std::string path = dialogs::utf8(item.path);
            ImGui::SetDragDropPayload(kImagePayload, path.c_str(), path.size() + 1);
            break;
        }
        case Kind::Script:
        case Kind::Audio: {
            const std::string path = dialogs::utf8(item.path);
            ImGui::SetDragDropPayload(item.kind == Kind::Script ? kScriptPayload : kAudioPayload, path.c_str(), path.size() + 1);
            break;
        }
        default: break;
    }
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + 4.0f, pos.y + ImGui::GetTextLineHeight()), itemColor(item));
    ImGui::Dummy(ImVec2(6.0f, 0.0f));
    ImGui::SameLine();
    ImGui::Text("%s", item.name.c_str());
    ImGui::TextDisabled("%s", item.type.c_str());
    ImGui::EndDragDropSource();
}

void EditorApp::browserDropTarget(const BrowserItem& item) {
    if (item.kind != Kind::Folder || !ImGui::BeginDragDropTarget()) return;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
        AssetPayload asset{};
        std::memcpy(&asset, payload->Data, sizeof(asset));
        if (const auto info = database_->find(asset.uuid); info && !info->path.empty()) {
            database_->move(asset.uuid, item.path / info->path.filename());
            refreshDatabase();
        }
    }
    if (ImGui::AcceptDragDropPayload(kEntityPayload)) createPrefabsFromSelection(item.path);
    ImGui::EndDragDropTarget();
}

// Menu contextual de cada cosa (el de Unreal: acciones del tipo y comunes).
void EditorApp::browserItemMenu(const BrowserItem& item) {
    const auto show_in_explorer = [](const std::filesystem::path& path, bool select) {
        if (select) {
            const std::wstring args = L"/select,\"" + path.wstring() + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        } else {
            ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    };
    ImGui::TextDisabled("%s", item.type.c_str());
    ImGui::Separator();
    switch (item.kind) {
        case Kind::Folder: {
            if (ImGui::MenuItem("Abrir")) navigateTo(item.path);
            const bool favorite = std::find(browser_favorites_.begin(), browser_favorites_.end(), item.path) != browser_favorites_.end();
            if (ImGui::MenuItem(favorite ? "Quitar de favoritos" : "Añadir a favoritos")) {
                if (favorite) std::erase(browser_favorites_, item.path);
                else browser_favorites_.push_back(item.path);
                saveBrowserFavorites();
            }
            if (item.path != project_.assetsFolder()) {
                if (ImGui::MenuItem("Renombrar", "F2")) {
                    renaming_folder_ = item.path;
                    folder_rename_buffer_ = item.name;
                }
            }
            if (ImGui::MenuItem("Nueva carpeta dentro")) createFolderIn(item.path);
            if (ImGui::MenuItem("Mostrar en el Explorador")) show_in_explorer(item.path, false);
            std::error_code error;
            if (item.path != project_.assetsFolder() &&
                ImGui::MenuItem("Borrar (vacía)", nullptr, false, std::filesystem::is_empty(item.path, error))) {
                std::filesystem::remove(item.path, error);
                refreshDatabase();
            }
            return;
        }
        case Kind::Image: {
            if (ImGui::MenuItem("Crear material")) {
                const std::string relative = decalImageInAssets(item.path);
                if (!relative.empty()) createMaterialAsset(item.path.parent_path(), relative);
            }
            if (ImGui::MenuItem("Estampar con esta imagen")) openBrowserItem(item);
            bool any_decal = false;
            for (const ecs::Entity e : selectedEntities()) any_decal = any_decal || e.has<ecs::Decal>();
            if (ImGui::MenuItem("Asignar a los decals seleccionados", nullptr, false, any_decal)) {
                const std::string texture = decalImageInAssets(item.path);
                for (ecs::Entity e : selectedEntities()) {
                    if (ecs::Decal* d = e.tryGet<ecs::Decal>()) d->texture = texture;
                }
                commit();
            }
            break;
        }
        case Kind::Script:
        case Kind::Audio: {
            const bool is_script = item.kind == Kind::Script;
            if (is_script && ImGui::MenuItem("Editar")) openScript(item.path);
            if (!is_script && ImGui::MenuItem(audio_.previewing() ? "Parar" : "Escuchar")) openBrowserItem(item);
            if (ImGui::MenuItem("Asignar a la selección", nullptr, false, !selection_.empty())) {
                for (ecs::Entity e : selectedEntities()) {
                    if (is_script) {
                        scripting::Script& s = e.has<scripting::Script>() ? e.get<scripting::Script>() : e.add<scripting::Script>();
                        s.file = assetRelative(item.path);
                    } else {
                        audio::AudioSource& a = e.has<audio::AudioSource>() ? e.get<audio::AudioSource>() : e.add<audio::AudioSource>();
                        a.clip = assetRelative(item.path);
                    }
                }
                commit();
            }
            break;
        }
        case Kind::Shader: {
            if (ImGui::MenuItem("Editar")) openScript(item.path);
            if (ImGui::MenuItem("Crear material con este shader")) {
                std::filesystem::path target = item.path.parent_path() / (item.path.stem().wstring() + L".crmat");
                for (int i = 2; std::filesystem::exists(target); ++i) {
                    target = item.path.parent_path() / (item.path.stem().wstring() + L" " + std::to_wstring(i) + L".crmat");
                }
                assets::MaterialAsset material;
                material.shader = assetRelative(item.path);
                if (assets::saveMaterial(material, target)) {
                    refreshDatabase();
                    inspected_material_ = material.uuid;
                }
            }
            break;
        }
        case Kind::Asset: {
            const assets::AssetInfo& info = item.info;
            switch (info.type) {
                case assets::AssetType::Scene:
                    if (ImGui::MenuItem("Abrir")) runOrAskToSave(PendingAction::OpenScene, info.path);
                    break;
                case assets::AssetType::Model:
                    if (ImGui::MenuItem("Poner en la escena")) instantiateAsset(info.uuid, {}, std::nullopt);
                    if (ImGui::MenuItem("Reimportar (combinar mallas)")) startReimport(info.uuid);
                    ImGui::SetItemTooltip("Lo vuelve a importar desde el archivo original. Si tiene muchas piezas\n"
                                          "(una palmera con cada hoja suelta) las junta por material: muchos\n"
                                          "menos objetos y mucho menos coste de CPU. Rehace sus instancias\n"
                                          "en la escena conservando posicion, materiales y Static.");
                    if (ImGui::BeginMenu("Animaciones")) {
                        drawModelAssetAnimationsMenu(info);
                        ImGui::EndMenu();
                    }
                    break;
                case assets::AssetType::Prefab: {
                    if (ImGui::MenuItem("Poner en la escena")) instantiatePrefabAsset(info.uuid, {}, std::nullopt);
                    const std::size_t count = ecs::prefabInstances(world_, info.uuid).size();
                    if (ImGui::MenuItem(("Seleccionar instancias (" + std::to_string(count) + ")").c_str(), nullptr, false, count > 0)) {
                        selectPrefabInstances(info.uuid);
                    }
                    break;
                }
                case assets::AssetType::Environment:
                    if (ImGui::MenuItem("Usar como cielo")) assignEnvironment(info.uuid);
                    break;
                case assets::AssetType::Material:
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
                            std::filesystem::path target = info.path.parent_path() / (info.path.stem().wstring() + L" copia.crmat");
                            for (int i = 2; std::filesystem::exists(target); ++i) {
                                target = info.path.parent_path() /
                                         (info.path.stem().wstring() + L" copia " + std::to_wstring(i) + L".crmat");
                            }
                            if (assets::saveMaterial(copy, target)) refreshDatabase();
                        }
                    }
                    break;
                case assets::AssetType::AnimatorController:
                    if (ImGui::MenuItem("Abrir en el editor Animator")) openAnimatorEditor(info.uuid);
                    if (ImGui::MenuItem("Asignar a la selección", nullptr, false, !selection_.empty())) {
                        assignAnimatorToSelection(info.uuid);
                    }
                    break;
                default: break;
            }
            if (info.path.empty()) {  // integrado: sin archivo
                if (ImGui::MenuItem("Copiar UUID")) ImGui::SetClipboardText(info.uuid.toString().c_str());
                return;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Renombrar", "F2")) {
                renaming_asset_ = info.uuid;
                asset_rename_buffer_ = info.name;
            }
            if (ImGui::MenuItem("Copiar UUID")) ImGui::SetClipboardText(info.uuid.toString().c_str());
            if (ImGui::MenuItem("Copiar ruta")) ImGui::SetClipboardText(assetRelative(info.path).c_str());
            if (ImGui::MenuItem("Mostrar en el Explorador")) show_in_explorer(info.path, true);
            ImGui::Separator();
            if (ImGui::MenuItem("Borrar", "Supr")) pending_delete_asset_ = info.uuid;
            return;
        }
    }
    // Archivos sueltos: comunes.
    ImGui::Separator();
    if (ImGui::MenuItem("Renombrar", "F2")) {
        renaming_file_ = item.path;
        asset_rename_buffer_ = dialogs::utf8(item.path.stem());
    }
    if (ImGui::MenuItem("Copiar ruta")) ImGui::SetClipboardText(assetRelative(item.path).c_str());
    if (ImGui::MenuItem("Mostrar en el Explorador")) show_in_explorer(item.path, true);
}

void EditorApp::drawProject() {
    if (!ImGui::Begin("Proyecto", &show_project_)) {
        ImGui::End();
        return;
    }
    if (current_folder_.empty() && has_project_ && cached_version_ == ~0ull) current_folder_ = project_.assetsFolder();
    if (cached_version_ != database_version_ || cached_folder_ != current_folder_) {
        const bool version_changed = cached_version_ != database_version_;
        rebuildBrowserCache();
        if (version_changed) browser_all_loose_.clear();
        buildBrowserItems();
        if (browser_favorites_.empty() && version_changed) loadBrowserFavorites();
    } else if (browser_cached_filter_ != project_filter_ || browser_cached_bits_ != browser_filter_) {
        buildBrowserItems();
    }
    folder_drop_zones_.clear();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImGuiStyle& style = ImGui::GetStyle();

    // ------------------------------------------------------------ barra
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 5.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(20, 120, 60, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(30, 150, 75, 255));
    if (ImGui::Button("+ Añadir")) ImGui::OpenPopup("add_menu");
    ImGui::PopStyleColor(2);
    const std::filesystem::path target_folder = current_folder_.empty() ? project_.assetsFolder() : current_folder_;
    if (ImGui::BeginPopup("add_menu")) {
        if (ImGui::MenuItem("Carpeta")) createFolderIn(target_folder);
        ImGui::Separator();
        if (ImGui::MenuItem("Escena")) createSceneAsset(target_folder);
        if (ImGui::MenuItem("Material")) createMaterialAsset(target_folder);
        if (ImGui::MenuItem("Script Lua")) createScriptAsset(target_folder, {});
        if (ImGui::MenuItem("Shader (GLSL)")) openScript(createShaderAsset(target_folder));
        if (ImGui::MenuItem("Animator")) createAnimatorAsset(target_folder);
        ImGui::Separator();
        if (ImGui::MenuItem("Prefab desde la selección", nullptr, false, !selection_.empty())) createPrefabsFromSelection(target_folder);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Importar")) {
        const auto files = dialogs::openFiles(
            window_.handle(),
            L"Modelos, cielos, imagenes y audio\0*.obj;*.fbx;*.gltf;*.glb;*.dae;*.hdr;*.png;*.jpg;*.jpeg;*.tga;*.wav;*.ogg;*.mp3\0Todos\0*.*\0");
        startImport(files, target_folder);
    }
    ImGui::SetItemTooltip("Importar archivos (también: arrastrarlos desde el Explorador)");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    // Historial y subir.
    ImGui::BeginDisabled(browser_back_.empty());
    if (ImGui::ArrowButton("##back", ImGuiDir_Left) || (!browser_back_.empty() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                                         ImGui::GetIO().KeyAlt && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false))) {
        browser_forward_.push_back(current_folder_);
        current_folder_ = browser_back_.back();
        browser_back_.pop_back();
        browser_selection_.clear();
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::BeginDisabled(browser_forward_.empty());
    if (ImGui::ArrowButton("##forward", ImGuiDir_Right)) {
        browser_back_.push_back(current_folder_);
        current_folder_ = browser_forward_.back();
        browser_forward_.pop_back();
        browser_selection_.clear();
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, 2.0f);
    const bool can_go_up = !current_folder_.empty() && current_folder_ != project_.assetsFolder();
    ImGui::BeginDisabled(!can_go_up);
    if (ImGui::ArrowButton("##up", ImGuiDir_Up)) navigateTo(current_folder_.parent_path());
    ImGui::EndDisabled();
    ImGui::SameLine();

    // Migas: Assets > Carpeta > ...
    {
        std::vector<std::pair<std::string, std::filesystem::path>> crumbs;
        if (current_folder_.empty()) {
            crumbs.emplace_back("Integrados", std::filesystem::path{});
        } else {
            crumbs.emplace_back("Assets", project_.assetsFolder());
            std::error_code error;
            const std::filesystem::path relative = std::filesystem::relative(current_folder_, project_.assetsFolder(), error);
            std::filesystem::path walk = project_.assetsFolder();
            for (const auto& part : relative) {
                if (part == ".") continue;
                walk /= part;
                crumbs.emplace_back(dialogs::utf8(part), walk);
            }
        }
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
        for (std::size_t i = 0; i < crumbs.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (i > 0) {
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextDisabled(">");
                ImGui::SameLine(0.0f, 0.0f);
            }
            const bool last = i + 1 == crumbs.size();
            if (last) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
            if (ImGui::Button(crumbs[i].first.c_str()) && !crumbs[i].second.empty()) navigateTo(crumbs[i].second);
            if (last) ImGui::PopStyleColor();
            // Soltar en una miga: mover alli.
            if (!crumbs[i].second.empty()) {
                Item crumb;
                crumb.kind = Kind::Folder;
                crumb.path = crumbs[i].second;
                browserDropTarget(crumb);
            }
            ImGui::PopID();
        }
        ImGui::PopStyleColor();
    }

    // A la derecha: busqueda, filtros y vista.
    const float right_width = 230.0f + 90.0f + 36.0f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8.0f, ImGui::GetWindowContentRegionMax().x - right_width));
    ImGui::SetNextItemWidth(230.0f);
    ImGui::InputTextWithHint("##buscar", "Buscar en el proyecto...", &project_filter_);
    ImGui::SameLine();
    int active_filters = 0;
    for (const FilterInfo& f : kFilters) active_filters += (browser_filter_ & f.bit) != 0 ? 1 : 0;
    const std::string filter_label = active_filters > 0 ? "Filtros (" + std::to_string(active_filters) + ")" : "Filtros";
    if (ImGui::Button(filter_label.c_str(), ImVec2(90.0f, 0.0f))) ImGui::OpenPopup("filters");
    if (ImGui::BeginPopup("filters")) {
        for (const FilterInfo& f : kFilters) {
            bool on = (browser_filter_ & f.bit) != 0;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + 4.0f, p.y + ImGui::GetFrameHeight()), f.color);
            ImGui::Dummy(ImVec2(6.0f, 0.0f));
            ImGui::SameLine();
            if (ImGui::Checkbox(f.name, &on)) browser_filter_ = on ? (browser_filter_ | f.bit) : (browser_filter_ & ~f.bit);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quitar filtros", nullptr, false, browser_filter_ != 0)) browser_filter_ = 0;
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("...", ImVec2(28.0f, 0.0f))) ImGui::OpenPopup("view_options");
    if (ImGui::BeginPopup("view_options")) {
        if (ImGui::RadioButton("Tarjetas", !browser_list_view_)) browser_list_view_ = false;
        if (ImGui::RadioButton("Lista", browser_list_view_)) browser_list_view_ = true;
        ImGui::SetNextItemWidth(160.0f);
        ImGui::SliderFloat("Tamaño", &icon_size_, 56.0f, 180.0f, "%.0f px");
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();

    // Chips de los filtros activos (clic = quitarlo).
    if (browser_filter_ != 0) {
        for (const FilterInfo& f : kFilters) {
            if ((browser_filter_ & f.bit) == 0) continue;
            ImGui::PushID(static_cast<int>(f.bit));
            ImGui::PushStyleColor(ImGuiCol_Button, (f.color & 0x00FFFFFFu) | 0x55000000u);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (f.color & 0x00FFFFFFu) | 0x88000000u);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
            if (ImGui::SmallButton((std::string(f.name) + "  x").c_str())) browser_filter_ &= ~f.bit;
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            ImGui::PopID();
            ImGui::SameLine();
        }
        ImGui::NewLine();
    }
    if (!imports_.empty()) {
        // Resumen; el detalle (archivo y etapa) va en la ventana de progreso.
        const float overall = importFraction();
        char overlay[96];
        std::snprintf(overlay, sizeof(overlay), "Importando %zu de %zu  (%.0f %%)",
                      std::min(imports_done_ + 1, imports_total_), imports_total_, overall * 100.0f);
        ImGui::ProgressBar(overall, ImVec2(-1.0f, 0.0f), overlay);
    }

    // ------------------------------------------------------------ fuentes
    const float status_height = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("sources", ImVec2(220.0f, -status_height), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
    if (!browser_favorites_.empty() &&
        ImGui::CollapsingHeader("Favoritos", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (std::size_t i = 0; i < browser_favorites_.size(); ++i) {
            const std::filesystem::path folder = browser_favorites_[i];
            ImGui::PushID(static_cast<int>(i) + 5000);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float h = ImGui::GetFrameHeight();
            if (ImGui::Selectable("##fav", folder == current_folder_, 0, ImVec2(0.0f, h))) navigateTo(folder);
            imgui_.drawIcon(ImGui::GetWindowDrawList(), Icon::FolderClosed, ImVec2(p.x + 4.0f, p.y + 3.0f), h - 6.0f,
                            IM_COL32(255, 200, 80, 255));
            ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + h + 6.0f, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f),
                                                ImGui::GetColorU32(ImGuiCol_Text), dialogs::utf8(folder.filename()).c_str());
            if (ImGui::BeginPopupContextItem("fav_menu")) {
                if (ImGui::MenuItem("Quitar de favoritos")) {
                    browser_favorites_.erase(browser_favorites_.begin() + static_cast<std::ptrdiff_t>(i));
                    saveBrowserFavorites();
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    if (ImGui::CollapsingHeader("Carpetas", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!folder_nodes_.empty()) drawFolderNode(0);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight();
        if (ImGui::Selectable("##builtin", current_folder_.empty(), 0, ImVec2(0.0f, h))) navigateTo({});
        imgui_.drawIcon(ImGui::GetWindowDrawList(), Icon::MeshRenderer, ImVec2(p.x + 4.0f, p.y + 3.0f), h - 6.0f,
                        IM_COL32(150, 150, 160, 255));
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + h + 6.0f, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f),
                                            ImGui::GetColorU32(ImGuiCol_TextDisabled), "Integrados (primitivas)");
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ------------------------------------------------------------ contenido
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(24, 24, 28, 255));
    ImGui::BeginChild("content", ImVec2(0.0f, -status_height), ImGuiChildFlags_None);
    draw = ImGui::GetWindowDrawList();
    const std::vector<Item> items = browser_items_;  // copia: una accion puede cambiar la lista
    bool clicked_item = false;
    int id = 0;

    const auto rename_field = [&](const Item& item, float width) -> bool {
        const bool asset = item.kind == Kind::Asset && renaming_asset_.valid() && renaming_asset_ == item.info.uuid;
        const bool folder = item.kind == Kind::Folder && !renaming_folder_.empty() && renaming_folder_ == item.path;
        const bool file = item.kind != Kind::Asset && item.kind != Kind::Folder && !renaming_file_.empty() && renaming_file_ == item.path;
        if (!asset && !folder && !file) return false;
        std::string& buffer = folder ? folder_rename_buffer_ : asset_rename_buffer_;
        ImGui::SetNextItemWidth(width);
        if (!ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
        const bool done = ImGui::InputText("##rename", &buffer, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (done || ImGui::IsItemDeactivated()) {
            std::error_code error;
            if (asset && !buffer.empty() && buffer != item.info.name) {
                database_->move(item.info.uuid, item.info.path.parent_path() /
                                                    (dialogs::fromUtf8(buffer).wstring() + item.info.path.extension().wstring()));
                refreshDatabase();
            } else if (folder && !buffer.empty()) {
                const std::filesystem::path renamed = item.path.parent_path() / dialogs::fromUtf8(buffer);
                if (renamed != item.path && !std::filesystem::exists(renamed)) {
                    std::filesystem::rename(item.path, renamed, error);
                    if (current_folder_ == item.path) current_folder_ = renamed;
                    refreshDatabase();
                }
            } else if (file && !buffer.empty()) {
                const std::filesystem::path renamed =
                    item.path.parent_path() / (dialogs::fromUtf8(buffer).wstring() + item.path.extension().wstring());
                if (renamed != item.path && !std::filesystem::exists(renamed)) {
                    std::filesystem::rename(item.path, renamed, error);
                    refreshDatabase();
                }
            }
            renaming_asset_ = {};
            renaming_folder_.clear();
            renaming_file_.clear();
        }
        return true;
    };

    // Miniatura de un elemento en un cuadrado.
    const auto draw_thumbnail = [&](const Item& item, ImVec2 pos, float size) {
        const ImU32 color = itemColor(item);
        if (item.kind == Kind::Folder) {
            imgui_.drawIcon(draw, Icon::FolderClosed, ImVec2(pos.x + size * 0.1f, pos.y + size * 0.1f), size * 0.8f,
                            IM_COL32(232, 194, 96, 255));
            return;
        }
        ImVec2 thumb_size{};
        ImTextureID texture = 0;
        if (item.kind == Kind::Image) texture = imgui_.thumbnail(item.path, &thumb_size);
        if (item.kind == Kind::Asset && item.info.type == assets::AssetType::Environment && !item.info.source.empty()) {
            std::error_code error;
            if (std::filesystem::exists(item.info.source, error)) texture = imgui_.thumbnail(item.info.source, &thumb_size);
        }
        if (item.kind == Kind::Asset && item.info.type == assets::AssetType::Model) {
            if (const auto png = model_previews_.preview(item.info.uuid, item.info.path, item.info.name)) {
                texture = imgui_.thumbnail(*png, &thumb_size);
            }
        }
        if (item.kind == Kind::Asset && item.info.type == assets::AssetType::Material) {
            draw->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size), IM_COL32(30, 30, 34, 255), 3.0f);
            drawMaterialBall(draw, ImVec2(pos.x + size * 0.5f, pos.y + size * 0.5f), size * 0.38f, item.info.uuid);
        } else if (texture != 0) {
            drawThumbnailTile(draw, texture, thumb_size, pos, size);
        } else {
            draw->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size), IM_COL32(30, 30, 34, 255), 3.0f);
            const float inner = size * 0.5f;
            imgui_.drawIcon(draw, itemIcon(item), ImVec2(pos.x + (size - inner) * 0.5f, pos.y + (size - inner) * 0.5f), inner, color);
            const char* tag = item.kind == Kind::Script ? "LUA" : item.kind == Kind::Shader ? "GLSL" : nullptr;
            if (tag != nullptr) {
                const ImVec2 t = ImGui::CalcTextSize(tag);
                draw->AddText(ImVec2(pos.x + (size - t.x) * 0.5f, pos.y + size * 0.78f), color, tag);
            }
        }
        // Shader con error: marca roja.
        if (item.kind == Kind::Shader && sync_ && !sync_->surfaceShaderError(assetRelative(item.path)).empty()) {
            draw->AddCircleFilled(ImVec2(pos.x + size - 8.0f, pos.y + 8.0f), 5.0f, IM_COL32(255, 80, 80, 255));
        }
    };

    const auto item_interaction = [&](const Item& item, std::size_t index, bool hovered) {
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            browserClick(item, index);
            clicked_item = true;
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !browserSelected(item)) {
            browser_selection_ = {item.key()};
            clicked_item = true;
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openBrowserItem(item);
        browserDragSource(item);
        browserDropTarget(item);
        if (ImGui::BeginPopupContextItem("item_menu")) {
            browserItemMenu(item);
            ImGui::EndPopup();
        }
        if (hovered && item.kind != Kind::Folder) {
            ImGui::SetItemTooltip("%s\n%s%s%s", item.name.c_str(), item.type.c_str(),
                                  item.size > 0 ? ("  ·  " + sizeText(item.size)).c_str() : "",
                                  item.kind == Kind::Asset && !item.info.path.empty()
                                      ? ("\n" + assetRelative(item.info.path)).c_str()
                                      : (item.kind != Kind::Asset ? ("\n" + assetRelative(item.path)).c_str() : ""));
        }
    };

    if (browser_list_view_) {
        // --- Lista: nombre, tipo, tamano ---
        if (ImGui::BeginTable("##list", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                               ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Nombre", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Tipo", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Tamaño", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(items.size()));
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    const Item& item = items[static_cast<std::size_t>(row)];
                    ImGui::PushID(row);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const float h = ImGui::GetTextLineHeight() + 4.0f;
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    if (!rename_field(item, -1.0f)) {
                        ImGui::Selectable("##row", browserSelected(item), ImGuiSelectableFlags_SpanAllColumns |
                                                                               ImGuiSelectableFlags_AllowDoubleClick,
                                          ImVec2(0.0f, h));
                        const bool hovered = ImGui::IsItemHovered();
                        if (item.kind == Kind::Folder && ImGui::IsItemVisible()) {
                            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                            folder_drop_zones_.push_back(FolderDropZone{a.x, a.y, b.x, b.y, item.path});
                        }
                        item_interaction(item, static_cast<std::size_t>(row), hovered);
                        ImDrawList* row_draw = ImGui::GetWindowDrawList();
                        row_draw->AddRectFilled(p, ImVec2(p.x + 3.0f, p.y + h), itemColor(item));
                        imgui_.drawIcon(row_draw, itemIcon(item), ImVec2(p.x + 8.0f, p.y + 2.0f), h - 4.0f,
                                        item.kind == Kind::Folder ? IM_COL32(232, 194, 96, 255) : itemColor(item));
                        row_draw->AddText(ImVec2(p.x + h + 12.0f, p.y + 2.0f), ImGui::GetColorU32(ImGuiCol_Text), item.name.c_str());
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", item.type.c_str());
                    ImGui::TableNextColumn();
                    if (item.size > 0) ImGui::TextDisabled("%s", sizeText(item.size).c_str());
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    } else {
        // --- Tarjetas (Unreal): miniatura, franja del color del tipo, nombre y tipo ---
        const float thumb = icon_size_;
        const float pad = 6.0f;
        const float card_w = thumb + pad * 2.0f;
        const float name_h = ImGui::GetTextLineHeight() * 2.0f + 2.0f;
        const float card_h = pad + thumb + 4.0f + name_h + ImGui::GetTextLineHeight() + pad;
        const float spacing = 8.0f;
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + 6.0f, ImGui::GetCursorPosY() + 6.0f));
        const float avail = ImGui::GetContentRegionAvail().x;
        const int columns = std::max(1, static_cast<int>((avail + spacing) / (card_w + spacing)));
        const int rows = static_cast<int>((items.size() + static_cast<std::size_t>(columns) - 1) / static_cast<std::size_t>(columns));
        const float start_x = ImGui::GetCursorPosX();
        ImGuiListClipper clipper;
        clipper.Begin(rows, card_h + spacing);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                for (int column = 0; column < columns; ++column) {
                    const std::size_t index = static_cast<std::size_t>(row * columns + column);
                    if (index >= items.size()) break;
                    const Item& item = items[index];
                    ImGui::PushID(id++ + row * 1000);
                    if (column > 0) ImGui::SameLine(0.0f, spacing);
                    else ImGui::SetCursorPosX(start_x);
                    const ImVec2 pos = ImGui::GetCursorScreenPos();
                    ImGui::InvisibleButton("##card", ImVec2(card_w, card_h));
                    const bool hovered = ImGui::IsItemHovered();
                    const bool selected = browserSelected(item);
                    if (item.kind == Kind::Folder && ImGui::IsItemVisible()) {
                        folder_drop_zones_.push_back(FolderDropZone{pos.x, pos.y, pos.x + card_w, pos.y + card_h, item.path});
                    }
                    item_interaction(item, index, hovered);

                    // Fondo de la tarjeta (las carpetas, sin tarjeta, como Unreal).
                    const ImVec2 end(pos.x + card_w, pos.y + card_h);
                    if (item.kind != Kind::Folder) {
                        draw->AddRectFilled(pos, end, hovered ? IM_COL32(52, 52, 60, 255) : IM_COL32(40, 40, 46, 255), 5.0f);
                    } else if (hovered) {
                        draw->AddRectFilled(pos, end, IM_COL32(255, 255, 255, 16), 5.0f);
                    }
                    if (selected) {
                        draw->AddRectFilled(pos, end, (kBrand & 0x00FFFFFFu) | 0x30000000u, 5.0f);
                        draw->AddRect(pos, end, kBrand, 5.0f, 0, 2.0f);
                    }
                    const ImVec2 thumb_pos(pos.x + pad, pos.y + pad);
                    draw_thumbnail(item, thumb_pos, thumb);
                    // Franja del color del tipo bajo la miniatura.
                    if (item.kind != Kind::Folder) {
                        draw->AddRectFilled(ImVec2(pos.x, thumb_pos.y + thumb + 1.0f), ImVec2(end.x, thumb_pos.y + thumb + 4.0f),
                                            itemColor(item));
                    }
                    // Nombre (2 lineas) y tipo.
                    const ImVec2 text_pos(pos.x + pad, thumb_pos.y + thumb + 6.0f);
                    ImGui::SetCursorScreenPos(text_pos);
                    if (!rename_field(item, thumb)) {
                        const std::string name = fitText(item.name, thumb, 2);
                        const ImVec4 clip(pos.x + pad, text_pos.y, end.x - pad, text_pos.y + name_h);
                        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), text_pos, ImGui::GetColorU32(ImGuiCol_Text),
                                      name.c_str(), nullptr, thumb, &clip);
                        if (item.kind != Kind::Folder) {
                            draw->AddText(ImVec2(text_pos.x, text_pos.y + name_h), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                          item.type.c_str());
                        }
                    }
                    ImGui::SetCursorScreenPos(pos);
                    ImGui::Dummy(ImVec2(card_w, card_h));
                    ImGui::PopID();
                }
            }
        }
    }

    if (items.empty()) {
        ImGui::SetCursorPos(ImVec2(20.0f, 20.0f));
        if (!project_filter_.empty() || browser_filter_ != 0) {
            ImGui::TextDisabled("Nada coincide con la búsqueda o los filtros.");
        } else if (!current_folder_.empty()) {
            ImGui::TextDisabled("Carpeta vacía.\nArrastra aquí archivos desde el Explorador (FBX, OBJ, glTF, HDR, imágenes, audio)\n"
                                "o usa Importar. Clic derecho: crear.");
        }
    }

    // Fondo: soltar un objeto de la Jerarquia = prefab aqui; clic = deseleccionar.
    if (ImGui::BeginDragDropTargetCustom(ImGui::GetCurrentWindow()->InnerRect, ImGui::GetID("##content_drop"))) {
        if (ImGui::AcceptDragDropPayload(kEntityPayload)) createPrefabsFromSelection(target_folder);
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !clicked_item && !ImGui::IsAnyItemHovered()) {
        browser_selection_.clear();
    }
    if (!current_folder_.empty() &&
        ImGui::BeginPopupContextWindow("content_menu", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem("Nueva carpeta")) createFolderIn(target_folder);
        if (ImGui::BeginMenu("Crear")) {
            if (ImGui::MenuItem("Escena")) createSceneAsset(target_folder);
            if (ImGui::MenuItem("Material")) createMaterialAsset(target_folder);
            if (ImGui::MenuItem("Script Lua")) createScriptAsset(target_folder, {});
            if (ImGui::MenuItem("Shader (GLSL)")) openScript(createShaderAsset(target_folder));
            if (ImGui::MenuItem("Animator")) createAnimatorAsset(target_folder);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Importar...")) {
            const auto files = dialogs::openFiles(
                window_.handle(), L"Modelos y cielos (*.obj;*.fbx;*.gltf;*.glb;*.dae;*.hdr)\0*.obj;*.fbx;*.gltf;*.glb;*.dae;*.hdr\0Todos\0*.*\0");
            startImport(files, target_folder);
        }
        ImGui::Separator();
        const bool favorite = std::find(browser_favorites_.begin(), browser_favorites_.end(), current_folder_) != browser_favorites_.end();
        if (ImGui::MenuItem(favorite ? "Quitar carpeta de favoritos" : "Añadir carpeta a favoritos")) {
            if (favorite) std::erase(browser_favorites_, current_folder_);
            else browser_favorites_.push_back(current_folder_);
            saveBrowserFavorites();
        }
        if (ImGui::MenuItem("Mostrar en el Explorador")) {
            ShellExecuteW(nullptr, L"open", current_folder_.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::EndPopup();
    }

    // Atajos (con el navegador enfocado).
    if (ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput) {
        const Item* single = nullptr;
        for (const Item& item : items) {
            if (browser_selection_.size() == 1 && item.key() == browser_selection_.front()) single = &item;
        }
        if (single != nullptr && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) openBrowserItem(*single);
        if (single != nullptr && ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
            if (single->kind == Kind::Asset && !single->info.path.empty()) {
                renaming_asset_ = single->info.uuid;
                asset_rename_buffer_ = single->info.name;
            } else if (single->kind == Kind::Folder) {
                renaming_folder_ = single->path;
                folder_rename_buffer_ = single->name;
            } else if (single->kind != Kind::Asset) {
                renaming_file_ = single->path;
                asset_rename_buffer_ = dialogs::utf8(single->path.stem());
            }
        }
        if (single != nullptr && single->kind == Kind::Asset && !single->info.path.empty() && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            pending_delete_asset_ = single->info.uuid;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false) && can_go_up) navigateTo(current_folder_.parent_path());
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
            browser_selection_.clear();
            for (const Item& item : items) browser_selection_.push_back(item.key());
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ------------------------------------------------------------ estado
    std::string status = std::to_string(items.size()) + " elementos";
    if (!browser_selection_.empty()) status += "  ·  " + std::to_string(browser_selection_.size()) + " seleccionado(s)";
    if (!project_filter_.empty() || (browser_filter_ != 0 && !current_folder_.empty())) status += "  ·  en todo el proyecto";
    ImGui::TextDisabled("%s", status.c_str());
    (void)style;
    ImGui::End();
}

}  // namespace cramion::editor
