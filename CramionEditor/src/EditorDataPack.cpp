// DataPacks (Archivo > Exportar escena como DataPack...): una o varias escenas
// con todo lo que usan (modelos, materiales, texturas, prefabs, scripts,
// sonidos...) en un .datapack, sin el ejecutable, como los AssetBundles de
// Unity. El juego los carga en marcha con DataPack.load / DataPack.loadScene
// (project/DataPack.h).

#include "EditorApp.h"

#include "Dialogs.h"

#include "CramionCore/ecs/Prefab.h"
#include "CramionCore/project/DataPack.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <fstream>
#include <iostream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace cramion::editor {

namespace {

std::string sizeText(std::uint64_t bytes) {
    char text[32];
    if (bytes >= (1ull << 20)) {
        std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1 << 20));
    } else {
        std::snprintf(text, sizeof(text), "%.0f KB", static_cast<double>(bytes) / 1024.0);
    }
    return text;
}

// Nombre valido de archivo (sin \ / : * ? " < > |).
std::string safeName(std::string name) {
    for (char& c : name) {
        if (std::string_view("\\/:*?\"<>|").find(c) != std::string_view::npos) c = '_';
    }
    return name.empty() ? std::string("DataPack") : name;
}

}  // namespace

void EditorApp::openDataPackExport() {
    if (!has_project_) return;
    if (playing()) {
        datapack_message_ = "Sal del modo Play antes de exportar.";
        datapack_done_ = true;
        show_datapack_ = true;
        return;
    }
    returnToSceneWorkspace();
    datapack_scenes_.clear();
    if (!scene_path_.empty()) datapack_scenes_.push_back(scene_path_);
    datapack_name_ = scene_path_.empty() ? std::string("DataPack") : dialogs::utf8(scene_path_.stem());
    if (datapack_folder_.empty()) datapack_folder_ = dialogs::utf8(project_.folder.parent_path() / "DataPacks");
    datapack_message_.clear();
    datapack_done_ = false;
    show_datapack_ = true;
}

// Clic derecho en la Jerarquia: el objeto (con sus hijos) como prefab y la
// ventana de exportar con el ya elegido. Si ya es una instancia sin cambios de
// un prefab se usa ese; si no, se crea en Assets/Prefabs (como en Unity, un
// bundle de objetos lleva prefabs).
std::filesystem::path EditorApp::prefabForDataPack(ecs::Entity entity, std::string& message) {
    if (!entity.valid()) return {};
    if (entity.has<ecs::PrefabInstance>() && database_) {
        const ecs::PrefabInstance& instance = entity.get<ecs::PrefabInstance>();
        if (instance.overrides.empty()) {
            if (const auto info = database_->find(instance.prefab.uuid); info && !info->path.empty()) return info->path;
        }
    }
    const std::filesystem::path folder = project_.assetsFolder() / "Prefabs";
    std::string base = safeName(entity.name());
    std::filesystem::path path = folder / dialogs::fromUtf8(base + ecs::kPrefabExtension);
    for (int i = 2; std::filesystem::exists(path); ++i) {
        path = folder / dialogs::fromUtf8(base + " " + std::to_string(i) + ecs::kPrefabExtension);
    }
    std::string error;
    if (!ecs::createPrefab(world_, entity, path, &error)) {
        message = "No se pudo crear el prefab: " + error;
        return {};
    }
    std::cout << "[DataPack] Prefab " << dialogs::utf8(path.filename()) << " creado para empaquetar \"" << entity.name()
              << "\"\n";
    refreshDatabase();
    commit();
    return path;
}

void EditorApp::openDataPackExportFor(ecs::Entity entity) {
    if (!has_project_ || !entity.valid()) return;
    if (playing()) {
        datapack_message_ = "Sal del modo Play antes de exportar.";
        datapack_done_ = true;
        show_datapack_ = true;
        return;
    }
    std::string message;
    const std::filesystem::path prefab = prefabForDataPack(entity, message);
    datapack_scenes_.clear();
    datapack_message_ = message;
    datapack_done_ = prefab.empty();
    if (!prefab.empty()) datapack_scenes_.push_back(prefab);
    datapack_name_ = safeName(entity.name());
    if (datapack_folder_.empty()) datapack_folder_ = dialogs::utf8(project_.folder.parent_path() / "DataPacks");
    show_datapack_ = true;
}

bool EditorApp::exportDataPack(const std::vector<std::filesystem::path>& scenes, const std::filesystem::path& file,
                               const std::string& name, std::string& message, std::size_t* file_count) {
    if (!has_project_ || !database_ || scenes.empty()) {
        message = "No hay escenas ni objetos que exportar";
        return false;
    }
    database_->refresh();
    const project::DataPackCollection deps = project::collectDependencies(scenes, project_.assetsFolder(), *database_);
    std::string error;
    if (!project::writeDataPack(file, name, scenes, deps, project_.assetsFolder(), CRAMION_VERSION_STRING, 9, {}, &error)) {
        message = error;
        return false;
    }
    if (file_count != nullptr) *file_count = deps.files.size();
    std::error_code e;
    message = std::to_string(deps.files.size()) + " archivos, " + sizeText(deps.bytes) + " -> " +
              sizeText(std::filesystem::file_size(file, e));
    for (const std::string& missing : deps.missing) message += "\nNo encontrado: " + missing;
    return true;
}

void EditorApp::startDataPackJob(const std::vector<std::filesystem::path>& scenes, const std::filesystem::path& file,
                                 const std::string& name) {
    if (datapack_job_ || !database_) return;
    database_->refresh();
    const project::DataPackCollection deps = project::collectDependencies(scenes, project_.assetsFolder(), *database_);
    auto job = std::make_unique<DataPackJob>();
    job->total = std::max<std::uint64_t>(deps.bytes, 1);
    job->file = file;
    DataPackJob* j = job.get();
    const std::filesystem::path root = project_.assetsFolder();
    job->thread = std::thread([j, deps, scenes, root, name] {
        std::error_code e;
        std::filesystem::create_directories(j->file.parent_path(), e);
        std::string error;
        j->ok = project::writeDataPack(j->file, name, scenes, deps, root, CRAMION_VERSION_STRING, 9,
                                       [j](std::uint64_t done, const std::string& current) {
                                           j->done = done;
                                           {
                                               std::lock_guard lock(j->mutex);
                                               j->current = current;
                                           }
                                           return !j->cancel.load();
                                       },
                                       &error);
        if (j->ok) {
            j->message = std::to_string(deps.files.size()) + " archivos (" + sizeText(deps.bytes) + ") -> " +
                         dialogs::utf8(j->file.filename()) + ", " + sizeText(std::filesystem::file_size(j->file, e));
            for (const std::string& missing : deps.missing) j->message += "\nNo encontrado: " + missing;
        } else {
            j->message = error;
        }
        j->finished = true;
    });
    datapack_job_ = std::move(job);
    datapack_message_.clear();
    datapack_folder_ = dialogs::utf8(file.parent_path());
    show_datapack_ = true;
}

void EditorApp::drawDataPackWindow() {
    if (datapack_pick_ && datapack_pick_->done) {
        if (!datapack_pick_->result.empty()) datapack_folder_ = dialogs::utf8(datapack_pick_->result);
        datapack_pick_.reset();
    }
    // Ventana modal como la de "Exportar juego": primero se elige que exportar,
    // luego la barra de progreso (el paquete se escribe en otro hilo) y al final
    // el resultado.
    const bool wanted = show_datapack_ || datapack_job_ != nullptr;
    if (wanted && !ImGui::IsPopupOpen("Exportar DataPack")) ImGui::OpenPopup("Exportar DataPack");
    ImGui::SetNextWindowSize(ImVec2(580.0f, 0.0f), ImGuiCond_Always);
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Exportar DataPack", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) return;

    if (datapack_job_) {
        DataPackJob& j = *datapack_job_;
        const double total = std::max<double>(static_cast<double>(j.total), 1.0);
        const double done = static_cast<double>(j.done.load());
        const float fraction = static_cast<float>(std::clamp(done / total, 0.0, 1.0));
        std::string current;
        {
            std::lock_guard lock(j.mutex);
            current = j.current;
        }
        ImGui::TextUnformatted(j.cancel ? "Cancelando..." : "Comprimiendo el DataPack...");
        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), "%.0f %%  (%.1f / %.1f MB)", fraction * 100.0f, done / 1048576.0,
                      total / 1048576.0);
        ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), overlay);
        ImGui::TextDisabled("%s", current.c_str());
        if (!j.cancel && ImGui::Button("Cancelar", ImVec2(-1.0f, 0.0f))) j.cancel = true;
        if (j.finished) {
            j.thread.join();
            if (j.cancel) {
                std::error_code e;
                std::filesystem::remove(j.file, e);
                datapack_message_ = "Exportacion cancelada.";
            } else {
                datapack_message_ = (j.ok ? "DataPack exportado: " : "Error: ") + j.message;
            }
            std::cout << "[DataPack] " << datapack_message_ << '\n';
            datapack_job_.reset();
            datapack_done_ = true;
            show_datapack_ = true;
        }
        ImGui::EndPopup();
        return;
    }

    if (datapack_done_) {
        // Resultado de la ultima exportacion.
        ImGui::TextWrapped("%s", datapack_message_.c_str());
        ImGui::Spacing();
#if defined(_WIN32)
        if (ImGui::Button("Abrir carpeta", ImVec2(160.0f, 0.0f))) {
            ShellExecuteW(nullptr, L"open", dialogs::fromUtf8(datapack_folder_).wstring().c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL);
        }
        ImGui::SameLine();
#endif
        if (ImGui::Button("Cerrar", ImVec2(160.0f, 0.0f))) {
            datapack_done_ = false;
            datapack_message_.clear();
            show_datapack_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    ImGui::TextWrapped("Un DataPack lleva escenas enteras y/o objetos (prefabs) con todo lo que usan (modelos, "
                       "materiales, texturas, scripts, sonidos...) en un solo archivo .datapack, sin el ejecutable. "
                       "El juego lo carga en marcha con DataPack.loadScene(\"archivo\") o "
                       "DataPack.instantiate(\"archivo\", \"objeto\").");
    ImGui::Spacing();

    const auto root_list = [&](const char* id, assets::AssetType type, const char* empty_text) {
        ImGui::BeginChild(id, ImVec2(0.0f, 110.0f), ImGuiChildFlags_Borders);
        bool any = false;
        if (database_) {
            for (const assets::AssetInfo& info : database_->all()) {
                if (info.type != type || info.path.empty()) continue;
                any = true;
                const auto it = std::find(datapack_scenes_.begin(), datapack_scenes_.end(), info.path);
                bool chosen = it != datapack_scenes_.end();
                std::error_code e;
                const std::string label = dialogs::utf8(std::filesystem::relative(info.path, project_.assetsFolder(), e));
                if (ImGui::Checkbox(label.c_str(), &chosen)) {
                    if (chosen) {
                        datapack_scenes_.push_back(info.path);
                    } else {
                        datapack_scenes_.erase(it);
                    }
                }
            }
        }
        if (!any) ImGui::TextDisabled("%s", empty_text);
        ImGui::EndChild();
    };
    ImGui::SeparatorText("Escenas");
    root_list("##datapack_scenes", assets::AssetType::Scene, "No hay escenas guardadas.");
    ImGui::SeparatorText("Objetos (prefabs)");
    root_list("##datapack_objects", assets::AssetType::Prefab,
              "No hay prefabs. Clic derecho en un objeto de la Jerarquia > Empaquetar y exportar como DataPack.");

    ImGui::SeparatorText("Archivo");
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::InputText("Nombre", &datapack_name_);
    const bool picking = datapack_pick_ != nullptr;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::InputText("##datapack_folder", &datapack_folder_);
    ImGui::SameLine();
    ImGui::BeginDisabled(picking);
    if (ImGui::Button("Examinar...", ImVec2(-1.0f, 0.0f))) {
        datapack_pick_ = dialogs::pickFolderAsync(dialogs::fromUtf8(datapack_folder_));
    }
    ImGui::EndDisabled();
    if (picking) ImGui::TextDisabled("Elige la carpeta en la ventana de Windows (o escribe la ruta arriba).");
    const std::filesystem::path target =
        dialogs::fromUtf8(datapack_folder_) / dialogs::fromUtf8(safeName(datapack_name_) + project::kDataPackExtension);
    ImGui::TextDisabled("%s", dialogs::utf8(target).c_str());
    if (!datapack_message_.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", datapack_message_.c_str());
    }

    ImGui::Spacing();
    const bool can_export = !datapack_scenes_.empty() && !datapack_folder_.empty() && !datapack_name_.empty();
    ImGui::BeginDisabled(!can_export);
    if (ImGui::Button("Exportar", ImVec2(160.0f, 0.0f))) {
        // Lo que no esta guardado no llegaria al paquete.
        const bool current_chosen =
            std::find(datapack_scenes_.begin(), datapack_scenes_.end(), scene_path_) != datapack_scenes_.end();
        if (!(dirty_ && current_chosen) || saveScene()) {
            startDataPackJob(datapack_scenes_, target, datapack_name_);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancelar", ImVec2(160.0f, 0.0f))) {
        datapack_message_.clear();
        show_datapack_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

}  // namespace cramion::editor
