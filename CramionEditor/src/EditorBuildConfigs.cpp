// Configuraciones de compilacion (Archivo > Configuraciones de compilacion):
// una lista de perfiles a la izquierda y sus ajustes a la derecha, como los
// Build Profiles de Unity. La activa es la que usa Exportar juego.

#include "EditorApp.h"

#include "Dialogs.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <iostream>

namespace cramion::editor {

void EditorApp::ensureBuildConfigs() {
    if (!has_project_) return;
    const std::filesystem::path file = project_.settingsFolder() / "BuildConfigs.json";
    if (build_configs_file_ == file) return;
    build_configs_file_ = file;
    build_configs_ = loadBuildConfigs(file);
    build_config_selected_ = build_configs_.active;
}

void EditorApp::saveBuildConfigsNow() {
    if (build_configs_file_.empty()) return;
    if (!saveBuildConfigs(build_configs_file_, build_configs_)) {
        std::cerr << "[Compilacion] No se pudo guardar " << dialogs::utf8(build_configs_file_) << "\n";
    }
}

std::string EditorApp::buildGameName() {
    ensureBuildConfigs();
    const std::string& name = build_configs_.current().game_name;
    return name.empty() ? project_.name : name;
}

std::filesystem::path EditorApp::buildIconPath(const BuildConfig& config) const {
    if (config.icon.empty()) return {};
    const std::filesystem::path path = dialogs::fromUtf8(config.icon);
    return path.is_absolute() ? path : project_.folder / path;
}

void EditorApp::drawBuildConfigsWindow() {
    if (!show_build_configs_) return;
    ensureBuildConfigs();
    ImGui::SetNextWindowSize(ImVec2(820.0f, 620.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Configuraciones de compilación", &show_build_configs_, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }
    bool changed = false;
    std::vector<BuildConfig>& configs = build_configs_.configs;
    build_config_selected_ = std::clamp(build_config_selected_, 0, static_cast<int>(configs.size()) - 1);

    // --- Lista ---
    ImGui::BeginChild("##configs", ImVec2(210.0f, 0.0f), ImGuiChildFlags_Borders);
    for (int i = 0; i < static_cast<int>(configs.size()); ++i) {
        ImGui::PushID(i);
        const bool active = i == build_configs_.active;
        const std::string label = configs[static_cast<std::size_t>(i)].name + (active ? "  (activa)" : "");
        if (ImGui::Selectable(label.c_str(), i == build_config_selected_)) build_config_selected_ = i;
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            build_configs_.active = i;
            changed = true;
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    if (ImGui::Button("Nueva")) {
        BuildConfig config;
        config.name = "Configuracion " + std::to_string(configs.size() + 1);
        configs.push_back(config);
        build_config_selected_ = static_cast<int>(configs.size()) - 1;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Duplicar")) {
        BuildConfig copy = configs[static_cast<std::size_t>(build_config_selected_)];
        copy.name += " copia";
        configs.push_back(copy);
        build_config_selected_ = static_cast<int>(configs.size()) - 1;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(configs.size() <= 1);
    if (ImGui::Button("Borrar")) {
        configs.erase(configs.begin() + build_config_selected_);
        if (build_configs_.active >= build_config_selected_ && build_configs_.active > 0) --build_configs_.active;
        build_config_selected_ = std::min(build_config_selected_, static_cast<int>(configs.size()) - 1);
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::SameLine();

    // --- Ajustes de la elegida ---
    BuildConfig& c = configs[static_cast<std::size_t>(build_config_selected_)];
    ImGui::BeginChild("##config", ImVec2(0.0f, 0.0f));
    const float label_w = 150.0f;
    const auto row = [&](const char* label) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-1.0f);
    };
    row("Configuracion");
    changed |= ImGui::InputText("##name", &c.name);
    row("Plataforma");
    {
        static const char* kPlatforms[] = {"Windows (.exe)", "Android (APK / AAB)", "Linux (x64)"};
        int platform = static_cast<int>(c.platform);
        if (ImGui::Combo("##platform", &platform, kPlatforms, 3)) {
            c.platform = static_cast<BuildPlatform>(platform);
            changed = true;
        }
    }
    const bool android = c.platform == BuildPlatform::Android;
    row("Nombre del juego");
    changed |= ImGui::InputTextWithHint("##game", project_.name.c_str(), &c.game_name);
    ImGui::SetItemTooltip("El .exe, la carpeta exportada, el titulo de la ventana y la pantalla de carga.\n"
                          "Vacio = el nombre del proyecto.");
    row("Version");
    changed |= ImGui::InputText("##version", &c.version);
    ImGui::Dummy(ImVec2(label_w - ImGui::GetStyle().ItemSpacing.x, 0.0f));
    ImGui::SameLine(label_w);
    changed |= ImGui::Checkbox("Version en el titulo de la ventana", &c.version_in_title);

    // Icono: vista previa, soltar una imagen del Proyecto o Examinar.
    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Icono de la app");
    ImGui::SameLine(label_w);
    const std::filesystem::path icon = buildIconPath(c);
    const float preview = 64.0f;
    const ImVec2 box = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(box, ImVec2(box.x + preview, box.y + preview), IM_COL32(30, 32, 38, 255), 6.0f);
    ImVec2 size{};
    std::filesystem::path shown = icon;
    if (shown.empty()) {  // el del motor (editor_icons/logo.png junto al editor)
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        shown = std::filesystem::path(exe).parent_path() / "editor_icons" / "logo.png";
    }
    const ImTextureID texture = imgui_.image(shown, &size);
    if (texture != 0 && size.x > 0.0f && size.y > 0.0f) {
        const float scale = std::min(preview / size.x, preview / size.y);
        const ImVec2 d(size.x * scale, size.y * scale);
        const ImVec2 a(box.x + (preview - d.x) * 0.5f, box.y + (preview - d.y) * 0.5f);
        draw->AddImage(texture, a, ImVec2(a.x + d.x, a.y + d.y));
    } else if (!icon.empty()) {
        draw->AddText(ImVec2(box.x + 8.0f, box.y + 24.0f), IM_COL32(170, 170, 180, 255), ".ico");
    }
    ImGui::InvisibleButton("##icon_drop", ImVec2(preview, preview));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kImagePayload)) {
            const std::filesystem::path file = dialogs::fromUtf8(static_cast<const char*>(payload->Data));
            std::error_code e;
            const std::filesystem::path relative = std::filesystem::relative(file, project_.folder, e);
            c.icon = dialogs::utf8(!e && !relative.empty() && *relative.begin() != ".." ? relative : file);
            changed = true;
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SetItemTooltip("Suelta aqui una imagen del Proyecto (PNG, JPG, TGA, BMP) o elige un .ico.\n"
                          "Se escribe dentro del .exe: Explorador, barra de tareas y ventana.\n"
                          "Mejor cuadrada y de 256 px o mas.");
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::InputTextWithHint("##icon", "(icono del motor)", &c.icon);
    if (ImGui::Button("Examinar...")) {
        const std::filesystem::path file = dialogs::openFile(
            window_.handle(), L"Iconos e imagenes (*.png;*.jpg;*.tga;*.bmp;*.ico)\0*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.ico\0",
            project_.assetsFolder());
        if (!file.empty()) {
            std::error_code e;
            const std::filesystem::path relative = std::filesystem::relative(file, project_.folder, e);
            c.icon = dialogs::utf8(!e && !relative.empty() && *relative.begin() != ".." ? relative : file);
            changed = true;
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(c.icon.empty());
    if (ImGui::Button("Quitar")) {
        c.icon.clear();
        changed = true;
    }
    ImGui::EndDisabled();
    if (!icon.empty() && (!std::filesystem::exists(icon) || !isIconSource(icon))) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "No existe o no es una imagen/.ico");
    }
    ImGui::EndGroup();
    ImGui::Separator();

    // Escena inicial.
    row("Escena inicial");
    std::string scene_label = "(la escena inicial del proyecto)";
    const Uuid chosen = Uuid::parse(c.startup_scene);
    if (chosen.valid() && database_) {
        const auto info = database_->find(chosen);
        scene_label = info ? dialogs::utf8(info->path.stem()) : std::string("(escena borrada)");
    }
    if (ImGui::BeginCombo("##scene", scene_label.c_str())) {
        if (ImGui::Selectable("(la escena inicial del proyecto)", !chosen.valid())) {
            c.startup_scene.clear();
            changed = true;
        }
        if (database_) {
            for (const assets::AssetInfo& info : database_->all()) {
                if (info.type != assets::AssetType::Scene) continue;
                if (ImGui::Selectable(dialogs::utf8(info.path.stem()).c_str(), info.uuid == chosen)) {
                    c.startup_scene = info.uuid.toString();
                    changed = true;
                }
            }
        }
        ImGui::EndCombo();
    }

    if (android) {
        ImGui::Separator();
        changed |= drawAndroidBuildSettings(c);
    }
    // Ventana.
    if (!android) {
    row("Ventana");
    static const char* kModes[] = {"Maximizada", "Pantalla completa (sin bordes)", "Ventana"};
    changed |= ImGui::Combo("##window", &c.window_mode, kModes, 3);
    row("Tamano de la ventana");
    int size_wh[2] = {c.width, c.height};
    if (ImGui::InputInt2("##size", size_wh)) {
        c.width = std::clamp(size_wh[0], 320, 16384);
        c.height = std::clamp(size_wh[1], 240, 16384);
        changed = true;
    }
    ImGui::SetItemTooltip("En modo Ventana; maximizada es el tamano al restaurar.");
    }
    ImGui::Dummy(ImVec2(label_w - ImGui::GetStyle().ItemSpacing.x, 0.0f));
    ImGui::SameLine(label_w);
    changed |= ImGui::Checkbox("Combinar mallas estaticas (static batching)", &c.static_batching);
    ImGui::Dummy(ImVec2(label_w - ImGui::GetStyle().ItemSpacing.x, 0.0f));
    ImGui::SameLine(label_w);
    changed |= ImGui::Checkbox("Mostrar FPS (desarrollo)", &c.show_fps);
    changed |= ImGui::Checkbox("Servidor dedicado (<Juego>Server.exe)", &c.dedicated_server);
    changed |= ImGui::Checkbox("Permitir mods", &c.allow_mods);
    ImGui::SetItemTooltip("El juego carga los mods de <juego>/Mods y %%LOCALAPPDATA%%/Cramion/Mods/<juego>:\n"
                          "carpetas con mod.json, Assets/ y main.lua, o archivos .datapack. Lua: Mods.list()");
    ImGui::SetItemTooltip("Copia tambien un servidor sin ventana ni GPU para multijugador: <Juego>Server.exe --port 7777\n"
                          "Simula la escena y abre la partida; los scripts lo saben con Network.isDedicated().");
    if (c.platform == BuildPlatform::Windows) {
        ImGui::Dummy(ImVec2(label_w - ImGui::GetStyle().ItemSpacing.x, 0.0f));
        ImGui::SameLine(label_w);
        changed |= ImGui::Checkbox("Realidad virtual (OpenXR)", &c.vr);
        ImGui::SetItemTooltip("El juego se ve en el casco (SteamVR, Meta Quest Link, WMR...) si hay uno conectado;\n"
                              "la ventana hace de espejo. Sin casco se juega normal. Ver Manual > Realidad virtual.");
    }
    drawPlatformBuildSettings(c, changed);  // Steam y Linux (EditorPlatform.cpp)

    ImGui::Separator();
    const bool is_active = build_config_selected_ == build_configs_.active;
    ImGui::BeginDisabled(is_active);
    if (ImGui::Button(is_active ? "Es la activa" : "Usar esta configuracion")) {
        build_configs_.active = build_config_selected_;
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(export_job_ != nullptr || playing());
    if (ImGui::Button("Exportar...")) {
        build_configs_.active = build_config_selected_;
        changed = true;
        exportGame(false);
    }
    ImGui::SameLine();
    if (ImGui::Button(android ? "Exportar e instalar..." : "Exportar y jugar...")) {
        build_configs_.active = build_config_selected_;
        changed = true;
        exportGame(true);
    }
    ImGui::EndDisabled();
    const std::string game = c.game_name.empty() ? project_.name : c.game_name;
    if (android) {
        const AndroidBuildSettings& a = c.android;
        const std::string package = a.package.empty() ? defaultAndroidPackage(game) : a.package;
        ImGui::TextDisabled("Sale como %s%s%s (%s)", game.c_str(), a.make_apk || !a.make_aab ? ".apk" : "",
                            a.make_aab ? (a.make_apk ? " + .aab" : ".aab") : "", package.c_str());
    } else {
        ImGui::TextDisabled("Sale como %s.exe", game.c_str());
    }
    ImGui::EndChild();
    ImGui::End();
    if (changed) saveBuildConfigsNow();
}

}  // namespace cramion::editor
