// Actualizaciones dentro del editor:
//
//   - Al arrancar (si esta activado en el actualizador o en el Hub) busca en
//     segundo plano la ultima version publicada.
//   - Con proyecto abierto, un aviso abajo a la derecha: "Cramion X
//     disponible" con Actualizar / Ver novedades. Ayuda > Buscar
//     actualizaciones abre el mismo dialogo.
//   - Actualizar: se guarda TODO primero (escena, prefabs abiertos, scripts,
//     material y Animator; una escena nueva va a Assets/Scenes sin preguntar),
//     se apunta el proyecto en reopen.json, se abre CramionUpdater.exe y el
//     editor se cierra. El actualizador instala y vuelve a abrir el proyecto.
//   - Si el actualizador se abrio por su cuenta, manda kPrepareMessage y el
//     editor hace lo mismo (requestUpdateShutdown).

#include "EditorApp.h"

#include <CramionUpdater/NotesView.h>

#include <imgui.h>

#include <cstdlib>
#include <iostream>

namespace cramion::editor {

namespace {

constexpr ImU32 kAccent = theme::kRed;  // tema: rojo para la accion principal
constexpr ImU32 kGreen = theme::kOk;

bool updateCheckDisabledByEnvironment() {
    wchar_t value[8] = {};
    return GetEnvironmentVariableW(L"CRAMION_NO_UPDATE_CHECK", value, 8) > 0 && value[0] != L'0';
}

}  // namespace

void EditorApp::startUpdateCheck(bool manual) {
    if (update_check_ && update_check_->state == update::CheckJob::State::Checking) return;
    update_check_ = update::startCheck();
    update_check_started_ = true;
    update_check_manual_ = manual;
    update_check_error_.clear();
}

void EditorApp::pollUpdates() {
    // Busqueda automatica unos segundos despues de abrir (no en la prueba
    // automatica ni con CRAMION_NO_UPDATE_CHECK).
    if (!update_check_started_ && ImGui::GetTime() > update_check_after_) {
        update_check_started_ = true;
        if (update_settings_.check_on_startup && self_test_folder_.empty() && !updateCheckDisabledByEnvironment()) {
            startUpdateCheck(false);
        }
    }
    if (update_check_ && update_check_->state != update::CheckJob::State::Checking) {
        if (update_check_->state == update::CheckJob::State::Done) {
            update_release_ = update_check_->release;
            update_known_ = true;
            update_notes_ = update::parseNotes(update_release_.notes);
            if (updateAvailable()) {
                std::cout << "[Actualizaciones] Cramion " << update_release_.version.str() << " disponible (tienes la "
                          << update::currentVersion().str() << ")\n";
            }
        } else {
            update_check_error_ = update_check_->error;
            if (update_check_manual_) std::cerr << "[Actualizaciones] " << update_check_error_ << "\n";
        }
        update_check_.reset();
    }

    // El actualizador pide guardar y cerrar.
    if (update_shutdown_requested_) {
        update_shutdown_requested_ = false;
        std::cout << "[Actualizaciones] El actualizador pide cerrar: guardando todo\n";
        std::string error;
        if (saveEverythingForUpdate(&error)) {
            if (has_project_) {
                update::addReopenProject(project_.file);
            } else {
                update::markReopenHub();
            }
            quit_ = true;
        } else {
            update_error_ = "No se pudo guardar todo para actualizar: " + error +
                            ". Guarda a mano y pulsa \"Volver a pedirlo\" en el actualizador.";
            show_update_dialog_ = true;
        }
    }
}

bool EditorApp::updateAvailable() const {
    return update_known_ && update_release_.version > update::currentVersion() &&
           update_release_.version.str() != update_settings_.skipped_version;
}

const update::Release* EditorApp::latestRelease() const { return update_known_ ? &update_release_ : nullptr; }

std::vector<std::string> EditorApp::unsavedWorkSummary() const {
    std::vector<std::string> items;
    if (!has_project_) return items;
    if (playing()) items.emplace_back("Se sale del modo Play (lo cambiado en Play no se guarda)");
    int prefabs = 0;
    for (const Workspace& ws : workspaces_) {
        if (ws.kind == WorkspaceKind::Prefab && (ws.id == world_workspace_ ? dirty_ : ws.dirty)) ++prefabs;
    }
    // La escena: con su mundo cargado, dirty_; si se esta en otra pestana,
    // lo que guardo su espacio.
    bool scene_dirty = false;
    for (const Workspace& ws : workspaces_) {
        if (ws.kind == WorkspaceKind::Scene) scene_dirty = scene_dirty || (ws.id == world_workspace_ ? dirty_ : ws.dirty);
    }
    if (workspaces_.empty()) scene_dirty = dirty_;
    if (scene_dirty) {
        items.push_back("Escena \"" + world_.sceneName() + "\"" + (scene_path_.empty() ? " (nueva: se guarda en Assets/Scenes)" : ""));
    }
    if (prefabs > 0) items.push_back(std::to_string(prefabs) + " prefab(s) abiertos con cambios");
    int scripts = 0;
    for (const ScriptTab& tab : script_tabs_) {
        if (tab.text != tab.saved) ++scripts;
    }
    if (scripts > 0) items.push_back(std::to_string(scripts) + " script(s) con cambios");
    if (animator_dirty_) items.emplace_back("El Animator Controller abierto");
    if (material_unsaved_) items.emplace_back("El material que se esta editando");
    return items;
}

bool EditorApp::saveEverythingForUpdate(std::string* error) {
    if (!has_project_) return true;
    if (playing()) exitPlay();
    bool ok = true;
    const auto fail = [&](const std::string& what) {
        ok = false;
        if (error != nullptr) *error += (error->empty() ? "" : "; ") + what;
    };
    if (animator_dirty_) saveAnimatorEditor();
    if (material_unsaved_ && material_edit_uuid_.valid()) {
        std::string e;
        if (assets::saveMaterial(material_edit_, material_edit_path_, &e)) {
            material_unsaved_ = false;
        } else {
            fail("material: " + e);
        }
    }
    for (ScriptTab& tab : script_tabs_) {
        if (tab.text != tab.saved && !saveScript(tab)) fail("script " + tab.relative);
    }
    // Prefabs abiertos con cambios (como al cerrar el editor).
    std::vector<int> dirty_prefabs;
    for (const Workspace& ws : workspaces_) {
        if (ws.kind == WorkspaceKind::Prefab && (ws.id == world_workspace_ ? dirty_ : ws.dirty)) dirty_prefabs.push_back(ws.id);
    }
    for (const int id : dirty_prefabs) {
        loadWorkspaceWorld(id);
        if (!savePrefabWorkspace()) fail("un prefab abierto");
    }
    returnToSceneWorkspace();
    if (dirty_) {
        // Escena nueva: a Assets/Scenes con un nombre libre (sin dialogo: el
        // actualizador espera y un dialogo nativo puede colgarse aqui).
        if (scene_path_.empty()) {
            const std::filesystem::path folder = project_.assetsFolder() / "Scenes";
            std::error_code ec;
            std::filesystem::create_directories(folder, ec);
            std::string base = world_.sceneName().empty() ? std::string("Escena") : world_.sceneName();
            std::filesystem::path path = folder / dialogs::fromUtf8(base + ".crscene");
            for (int i = 2; std::filesystem::exists(path, ec); ++i) {
                path = folder / dialogs::fromUtf8(base + " " + std::to_string(i) + ".crscene");
            }
            world_.setSceneName(dialogs::utf8(path.stem()));
            scene_path_ = path;
        }
        if (!saveScene()) fail("la escena " + world_.sceneName());
    }
    if (ok) std::cout << "[Actualizaciones] Todo guardado\n";
    return ok;
}

bool EditorApp::launchUpdater(UpdaterMode mode) {
    const std::filesystem::path exe = update::installFolder() / update::kUpdaterExe;
    if (!std::filesystem::exists(exe)) {
        update_error_ = "No se encuentra " + dialogs::utf8(exe) + ". Descarga Cramion de nuevo desde GitHub.";
        return false;
    }
    std::wstring args;
    if (mode != UpdaterMode::Open) {
        args = std::wstring(mode == UpdaterMode::Reinstall ? L"--reinstall" : L"--install") + L" --relaunch --wait-pid " +
               std::to_wstring(GetCurrentProcessId());
    }
    if (update::launch(exe, args) == 0) {
        update_error_ = "No se pudo abrir el actualizador.";
        return false;
    }
    return true;
}

void EditorApp::beginUpdateInstall(bool reinstall) {
    update_error_.clear();
    std::string error;
    if (!saveEverythingForUpdate(&error)) {
        update_error_ = "No se pudo guardar todo: " + error + ". No se ha cerrado nada.";
        return;
    }
    if (has_project_) {
        update::addReopenProject(project_.file);
    } else {
        update::markReopenHub();
    }
    if (!launchUpdater(reinstall ? UpdaterMode::Reinstall : UpdaterMode::Install)) {
        update::takeReopenList();  // no se va a cerrar: nada que reabrir
        return;
    }
    std::cout << "[Actualizaciones] Abriendo el actualizador; el editor se cierra\n";
    quit_ = true;
}

// Una linea con el estado: buscando / al dia / disponible / error.
void EditorApp::drawUpdateStatusLine(bool compact) {
    const std::string current = update::currentVersion().str();
    if (update_check_) {
        ImGui::TextDisabled("Buscando actualizaciones...");
    } else if (updateAvailable()) {
        ImGui::TextColored(theme::vec(theme::kYellow), "Cramion %s disponible", update_release_.version.str().c_str());
        if (!compact) {
            ImGui::SameLine();
            ImGui::TextDisabled("(tienes la %s)", current.c_str());
        }
    } else if (update_known_) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kGreen), "Al día");
        if (!compact) {
            ImGui::SameLine();
            ImGui::TextDisabled("Cramion %s es la última versión", current.c_str());
        }
    } else if (!update_check_error_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.35f, 1.0f), "%s", compact ? "Sin conexión" : update_check_error_.c_str());
    } else {
        ImGui::TextDisabled("Cramion %s", current.c_str());
    }
}

void EditorApp::drawUpdateToast() {
    if (!has_project_ || !updateAvailable() || update_toast_dismissed_ || show_update_dialog_ || projectLoading()) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 size(380.0f, 0.0f);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 18.0f, vp->WorkPos.y + vp->WorkSize.y - 42.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSize(size);
    ImGui::SetNextWindowBgAlpha(0.97f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(kAccent));
    if (ImGui::Begin("##update_toast", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImDrawList* d = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        d->AddCircleFilled(ImVec2(p.x + 14.0f, p.y + 14.0f), 14.0f, theme::withAlpha(kAccent, 50), 24);
        d->AddLine(ImVec2(p.x + 14.0f, p.y + 6.0f), ImVec2(p.x + 14.0f, p.y + 19.0f), kAccent, 2.5f);
        d->AddLine(ImVec2(p.x + 8.5f, p.y + 14.0f), ImVec2(p.x + 14.0f, p.y + 19.5f), kAccent, 2.5f);
        d->AddLine(ImVec2(p.x + 19.5f, p.y + 14.0f), ImVec2(p.x + 14.0f, p.y + 19.5f), kAccent, 2.5f);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 38.0f, p.y));
        ImGui::BeginGroup();
        ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.15f);
        ImGui::Text("Cramion %s disponible", update_release_.version.str().c_str());
        ImGui::PopFont();
        ImGui::TextDisabled("Tienes la %s. Se guarda todo antes de actualizar.", update::currentVersion().str().c_str());
        ImGui::EndGroup();
        const float close_x = ImGui::GetWindowContentRegionMax().x - 16.0f;
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + close_x, p.y - 4.0f));
        if (ImGui::SmallButton("x")) update_toast_dismissed_ = true;
        ImGui::SetItemTooltip("Ocultar hasta la próxima vez");
        ImGui::SetCursorScreenPos(ImVec2(p.x, ImGui::GetCursorScreenPos().y + 8.0f));
        if (ImGui::Button("Ver novedades", ImVec2(150.0f, 0.0f))) show_update_dialog_ = true;
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(kAccent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::vec(theme::kRedHover));
        if (ImGui::Button("Actualizar...", ImVec2(-1.0f, 0.0f))) {
            show_update_dialog_ = true;
            update_confirm_ = true;
        }
        ImGui::PopStyleColor(2);
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void EditorApp::drawUpdateDialog() {
    if (show_update_dialog_) {
        ImGui::OpenPopup("Actualizaciones de Cramion");
        show_update_dialog_ = false;
        if (!update_known_ && !update_check_) startUpdateCheck(true);
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(std::min(720.0f, vp->WorkSize.x - 40.0f), 0.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Actualizaciones de Cramion", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        update_confirm_ = false;
        return;
    }
    drawUpdateStatusLine(false);
    const bool available = updateAvailable() || (update_known_ && update_release_.version > update::currentVersion());
    if (update_known_) {
        ImGui::Spacing();
        ImGui::TextDisabled("Novedades de la %s%s", update_release_.version.str().c_str(),
                            update_release_.published_at.empty() ? "" : (" · " + update::formatDate(update_release_.published_at)).c_str());
        ImGui::BeginChild("update_notes", ImVec2(0.0f, 260.0f), ImGuiChildFlags_Borders);
        if (update_notes_.empty()) {
            ImGui::TextDisabled("Sin notas.");
        } else {
            update::drawReleaseNotes(update_notes_, kAccent);
        }
        ImGui::EndChild();
    }
    if (available) {
        ImGui::Spacing();
        const std::vector<std::string> work = unsavedWorkSummary();
        if (work.empty()) {
            ImGui::TextUnformatted("No hay nada sin guardar.");
        } else {
            ImGui::TextUnformatted("Antes de cerrar, Cramion guardará:");
            for (const std::string& item : work) ImGui::BulletText("%s", item.c_str());
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("El editor se cierra, el actualizador descarga e instala la %s y vuelve a abrir %s.",
                            update_release_.version.str().c_str(), has_project_ ? "este proyecto" : "el Hub");
        if (update_release_.zip_url.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.35f, 1.0f),
                               "Esta versión aún no tiene el paquete de descarga (%s). Prueba dentro de un rato.",
                               update::kPackageAsset);
        }
        ImGui::PopTextWrapPos();
    }
    if (!update_error_.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", update_error_.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::Separator();
    if (available) {
        ImGui::BeginDisabled(update_release_.zip_url.empty());
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(kAccent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::vec(theme::kRedHover));
        if (ImGui::Button("Guardar todo y actualizar", ImVec2(220.0f, 32.0f))) {
            beginUpdateInstall();
            if (quit_) ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Omitir esta versión", ImVec2(0.0f, 32.0f))) {
            update_settings_ = update::loadSettings();
            update_settings_.skipped_version = update_release_.version.str();
            update::saveSettings(update_settings_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Más tarde", ImVec2(0.0f, 32.0f))) {
            update_toast_dismissed_ = true;
            ImGui::CloseCurrentPopup();
        }
    } else {
        ImGui::BeginDisabled(update_check_ != nullptr);
        if (ImGui::Button("Buscar de nuevo", ImVec2(0.0f, 32.0f))) startUpdateCheck(true);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Abrir el actualizador", ImVec2(0.0f, 32.0f))) launchUpdater(UpdaterMode::Open);
        ImGui::SameLine();
        ImGui::BeginDisabled(!update_known_ || update_release_.zip_url.empty());
        if (ImGui::Button("Reinstalar", ImVec2(0.0f, 32.0f))) update_ask_reinstall_ = true;
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Descarga e instala otra vez la versión publicada (repara archivos que falten o estén dañados)");
        ImGui::SameLine();
        if (ImGui::Button("Cerrar", ImVec2(0.0f, 32.0f))) ImGui::CloseCurrentPopup();
        if (update_ask_reinstall_) {
            const std::vector<std::string> work = unsavedWorkSummary();
            ImGui::Spacing();
            ImGui::TextUnformatted(work.empty() ? "¿Reinstalar? No hay nada sin guardar." : "¿Reinstalar? Antes se guardará:");
            for (const std::string& item : work) ImGui::BulletText("%s", item.c_str());
            if (ImGui::Button("Guardar todo y reinstalar", ImVec2(0.0f, 30.0f))) {
                update_ask_reinstall_ = false;
                beginUpdateInstall(true);
                if (quit_) ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancelar", ImVec2(0.0f, 30.0f))) update_ask_reinstall_ = false;
        }
    }
    update_confirm_ = false;
    ImGui::EndPopup();
}

}  // namespace cramion::editor
