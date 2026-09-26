// Exportar el juego (Archivo > Exportar juego): una carpeta que se puede
// copiar a otro PC y jugar, como el Build de Unity:
//
//   <Carpeta>/<Proyecto>/
//     <Proyecto>.exe   CramionPlayer (banner del motor, escena inicial)
//     shaders/         del motor
//     *.dll            las que haya junto al editor
//     Game/            el proyecto sin Library (caches), banner.png y game.ini
//
// La copia va en otro hilo (el editor sigue respondiendo) con su barra de
// progreso y se puede cancelar; al volver a exportar, lo que no cambio no se
// copia otra vez.

#include "EditorApp.h"

#include "Dialogs.h"

#include <CramionCore/ecs/SceneSerializer.h>
#include <CramionCore/ecs/StaticBatching.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>

#include <shellapi.h>

namespace cramion::editor {

namespace {

std::filesystem::path editorFolder() {
    wchar_t buffer[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path();
}

std::string safeFolderName(std::string name) {
    for (char& c : name) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') c = '_';
    }
    return name.empty() ? std::string("Juego") : name;
}

bool isInside(const std::filesystem::path& child, const std::filesystem::path& parent) {
    std::error_code e;
    const std::filesystem::path c = std::filesystem::weakly_canonical(child, e);
    const std::filesystem::path p = std::filesystem::weakly_canonical(parent, e);
    auto ci = c.begin();
    for (auto pi = p.begin(); pi != p.end(); ++pi, ++ci) {
        if (ci == c.end() || *ci != *pi) return false;
    }
    return true;
}

std::filesystem::path exportFolderMemory(const project::ProjectInfo& project) {
    return project.libraryFolder() / "export_folder.txt";
}

}  // namespace

// Abre la ventana de exportar (carpeta de destino y boton Exportar). No usa el
// selector de Windows directamente: si se cuelga, el editor no se congela.
void EditorApp::exportGame(bool run_after) {
    if (!has_project_ || export_job_) return;
    if (playing()) {
        export_message_ = "Sal del modo Play antes de exportar.";
        return;
    }
    export_run_after_ = run_after;
    export_setup_ = true;
    if (export_folder_.empty()) {
        std::ifstream in(exportFolderMemory(project_));
        std::getline(in, export_folder_);
        if (export_folder_.empty()) export_folder_ = dialogs::utf8(project_.folder.parent_path() / "Builds");
    }
}

void EditorApp::startExport(const std::filesystem::path& parent) {
    // Lo que no esta guardado no llegaria al juego.
    if (dirty_ || scene_path_.empty()) {
        if (!saveScene()) return;
    }
    std::error_code created;
    std::filesystem::create_directories(parent, created);
    if (!std::filesystem::is_directory(parent, created)) {
        export_message_ = "No se pudo crear la carpeta " + dialogs::utf8(parent) + ".";
        return;
    }
    {
        std::error_code e;
        std::filesystem::create_directories(project_.libraryFolder(), e);
        std::ofstream(exportFolderMemory(project_)) << dialogs::utf8(parent);
    }
    const bool run_after = export_run_after_;
    const std::filesystem::path target = parent / dialogs::fromUtf8(safeFolderName(project_.name));
    if (isInside(target, project_.folder)) {
        export_message_ = "Elige una carpeta fuera del proyecto (el juego se copiaria dentro de si mismo).";
        std::cerr << "[Exportar] " << export_message_ << '\n';
        return;
    }
    const std::filesystem::path source = editorFolder();
    if (!std::filesystem::exists(source / "CramionPlayer.exe")) {
        export_message_ = "Falta CramionPlayer.exe junto al editor: compila el proyecto.";
        std::cerr << "[Exportar] " << export_message_ << '\n';
        return;
    }

    // Lista de copias (se decide aqui, se copia en el otro hilo).
    auto job = std::make_unique<ExportJob>();
    job->target = target;
    job->exe = target / dialogs::fromUtf8(safeFolderName(project_.name) + ".exe");
    job->run_after = run_after;
    std::error_code error;
    const auto add_file = [&](const std::filesystem::path& from, const std::filesystem::path& to) {
        std::error_code e;
        const auto size = std::filesystem::file_size(from, e);
        if (e) return;
        job->files.push_back(ExportJob::Copy{from, to});
        job->total += size;
    };
    const auto add_folder = [&](const std::filesystem::path& from, const std::filesystem::path& to) {
        std::error_code e;
        std::filesystem::recursive_directory_iterator it(from, std::filesystem::directory_options::skip_permission_denied, e);
        for (; !e && it != std::filesystem::recursive_directory_iterator(); it.increment(e)) {
            std::error_code fe;
            if (it->is_regular_file(fe)) add_file(it->path(), to / std::filesystem::relative(it->path(), from, fe));
        }
    };
    add_file(source / "CramionPlayer.exe", job->exe);
    add_folder(source / "shaders", target / "shaders");
    for (std::filesystem::directory_iterator it(source, error); !error && it != std::filesystem::directory_iterator();
         it.increment(error)) {
        std::error_code fe;
        if (it->is_regular_file(fe) && it->path().extension() == ".dll") add_file(it->path(), target / it->path().filename());
    }
    const std::filesystem::path game = target / "Game";
    add_file(source / "player_banner.png", game / "banner.png");

    // Los assets del juego, comprimidos en un solo archivo .crpack.
    job->pack_file = game / dialogs::fromUtf8(safeFolderName(project_.name) + ".crpack");
    const auto pack_file = [&](const std::filesystem::path& from, const std::filesystem::path& inside) {
        std::error_code e;
        const auto size = std::filesystem::file_size(from, e);
        if (e) return;
        const std::u8string generic = inside.generic_u8string();  // con '/'
        job->pack.push_back(project::PackInput{from, std::string(generic.begin(), generic.end())});
        job->total += size;
    };
    const auto pack_folder = [&](const std::filesystem::path& from, const std::filesystem::path& inside) {
        std::error_code e;
        std::filesystem::recursive_directory_iterator it(from, std::filesystem::directory_options::skip_permission_denied, e);
        for (; !e && it != std::filesystem::recursive_directory_iterator(); it.increment(e)) {
            std::error_code fe;
            if (it->is_regular_file(fe)) pack_file(it->path(), inside / std::filesystem::relative(it->path(), from, fe));
        }
    };
    pack_file(project_.file, project_.file.filename());
    pack_folder(project_.assetsFolder(), "Assets");
    pack_folder(project_.settingsFolder(), "ProjectSettings");

    // Escena inicial: la del proyecto o la que esta abierta.
    std::filesystem::path first_scene = scene_path_;
    if (project_.startup_scene.valid()) {
        if (const auto info = database_->find(project_.startup_scene)) first_scene = info->path;
    }
    job->game_ini = "scene=" + assetRelative(first_scene) + "\n";
    job->game_folder = game;
    job->scene_name = dialogs::utf8(first_scene.filename());
    job->static_batching = export_static_batching_;
    job->assets_root = project_.assetsFolder();
    job->batch_cache = project_.libraryFolder() / "ExportCache" / "StaticBatches";

    // --- El hilo que copia ---
    ExportJob* j = job.get();
    job->thread = std::thread([j] {
        std::vector<char> buffer(4u << 20);
        for (const ExportJob::Copy& copy : j->files) {
            if (j->cancel) break;
            {
                std::lock_guard lock(j->mutex);
                j->current = dialogs::utf8(copy.to.filename());
            }
            std::error_code e;
            const auto size = std::filesystem::file_size(copy.from, e);
            // Ya copiado y sin cambios: se salta.
            std::error_code te;
            if (!e && std::filesystem::exists(copy.to, te) && std::filesystem::file_size(copy.to, te) == size &&
                std::filesystem::last_write_time(copy.to, te) >= std::filesystem::last_write_time(copy.from, te)) {
                j->done += size;
                continue;
            }
            std::filesystem::create_directories(copy.to.parent_path(), e);
            std::ifstream in(copy.from, std::ios::binary);
            std::ofstream out(copy.to, std::ios::binary | std::ios::trunc);
            if (!in || !out) {
                std::lock_guard lock(j->mutex);
                j->error = "No se pudo copiar " + dialogs::utf8(copy.from);
                break;
            }
            while (in && !j->cancel) {
                in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const std::streamsize n = in.gcount();
                if (n <= 0) break;
                out.write(buffer.data(), n);
                j->done += static_cast<std::uint64_t>(n);
            }
            if (!out) {
                std::lock_guard lock(j->mutex);
                j->error = "No se pudo escribir " + dialogs::utf8(copy.to) + " (disco lleno?)";
                break;
            }
        }
        // Static batching: cada escena con objetos Static va al paquete como
        // una copia con sus mallas combinadas (el proyecto no se toca).
        if (j->static_batching && !j->cancel && j->error.empty()) {
            assets::AssetDatabase database;
            database.open(j->assets_root);
            std::error_code e;
            std::filesystem::remove_all(j->batch_cache, e);
            std::filesystem::create_directories(j->batch_cache, e);
            const std::size_t count = j->pack.size();
            for (std::size_t i = 0; i < count && !j->cancel; ++i) {
                std::string extension = dialogs::utf8(j->pack[i].source.extension());
                std::transform(extension.begin(), extension.end(), extension.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (extension != ".crscene") continue;
                const std::string scene_name = dialogs::utf8(j->pack[i].source.stem());
                {
                    std::lock_guard lock(j->mutex);
                    j->current = "Combinando mallas estaticas: " + scene_name;
                }
                ecs::World world;
                std::string error;
                if (!ecs::loadScene(world, j->pack[i].source, &error)) {
                    std::cerr << "[Exportar] No se pudo abrir " << scene_name << " para combinar: " << error << '\n';
                    continue;
                }
                const std::string id = std::to_string(i) + "_" + safeFolderName(scene_name);
                const std::filesystem::path model = j->batch_cache / dialogs::fromUtf8(id + ".crdata");
                ecs::StaticBatchReport report;
                if (!ecs::buildStaticBatch(world, database, model, ecs::StaticBatchOptions{}, report)) {
                    std::cout << "[Exportar] " << scene_name << ": " << report.message << '\n';
                    continue;
                }
                const std::filesystem::path scene = j->batch_cache / dialogs::fromUtf8(id + ".crscene");
                if (!ecs::saveScene(world, scene, &error)) {
                    std::cerr << "[Exportar] No se pudo guardar la escena combinada " << scene_name << ": " << error
                              << '\n';
                    std::filesystem::remove(model, e);
                    continue;
                }
                j->total += std::filesystem::file_size(model, e);
                j->pack[i].source = scene;
                j->pack.push_back(project::PackInput{model, "Assets/_StaticBatches/" + id + ".crdata"});
                std::cout << "[Exportar] Static batching en " << scene_name << ": " << report.message << '\n';
                j->batch_summary += "\n" + scene_name + ": " + report.message;
            }
        }
        // Los assets al paquete (siempre se rehace: es rapido y asi nunca queda viejo).
        if (!j->cancel && j->error.empty()) {
            const std::uint64_t base = j->done;
            std::string error;
            const bool ok = project::writePack(j->pack_file, j->pack, 6, [j, base](std::uint64_t done, const std::string& current) {
                j->done = base + done;
                if (!current.empty()) {
                    std::lock_guard lock(j->mutex);
                    j->current = "Comprimiendo " + current;
                }
                return !j->cancel.load();
            }, &error);
            if (!ok && !j->cancel) {
                std::lock_guard lock(j->mutex);
                j->error = error;
            }
        }
        if (!j->cancel && j->error.empty()) {
            // Restos de exportaciones anteriores (assets sueltos).
            std::error_code e;
            std::filesystem::remove_all(j->game_folder / "Assets", e);
            std::filesystem::remove_all(j->game_folder / "ProjectSettings", e);
            for (std::filesystem::directory_iterator it(j->game_folder, e); !e && it != std::filesystem::directory_iterator(); it.increment(e)) {
                if (it->path().extension() == ".crproj") std::filesystem::remove(it->path(), e);
            }
            std::ofstream(j->game_folder / "game.ini") << j->game_ini;
        }
        j->finished = true;
    });
    export_job_ = std::move(job);
    export_message_.clear();
}

void EditorApp::drawExportProgress() {
    const bool wanted = export_job_ || export_setup_ || !export_message_.empty();
    if (wanted && !ImGui::IsPopupOpen("Exportar juego")) ImGui::OpenPopup("Exportar juego");
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Always);
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Exportar juego", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) return;
    if (export_job_) {
        ExportJob& j = *export_job_;
        const double total = std::max<double>(static_cast<double>(j.total.load()), 1.0);
        const double done = static_cast<double>(j.done.load());
        const float fraction = static_cast<float>(std::min(done / total, 1.0));
        std::string current;
        {
            std::lock_guard lock(j.mutex);
            current = j.current;
        }
        ImGui::TextUnformatted(j.cancel ? "Cancelando..." : "Copiando el juego...");
        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), "%.0f %%  (%.1f / %.1f MB)", fraction * 100.0f, done / 1048576.0,
                      total / 1048576.0);
        ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), overlay);
        ImGui::TextDisabled("%s", current.c_str());
        if (!j.cancel && ImGui::Button("Cancelar", ImVec2(-1.0f, 0.0f))) j.cancel = true;
        if (j.finished) {
            j.thread.join();
            if (j.cancel) {
                export_message_ = "Exportacion cancelada.";
            } else if (!j.error.empty()) {
                export_message_ = j.error;
            } else {
                export_message_ = "Juego exportado en " + dialogs::utf8(j.target) + " (escena inicial: " + j.scene_name + ")";
                if (!j.batch_summary.empty()) export_message_ += "\n\nStatic batching:" + j.batch_summary;
                if (j.run_after) {
                    ShellExecuteW(nullptr, L"open", j.exe.wstring().c_str(), nullptr, j.target.wstring().c_str(), SW_SHOWNORMAL);
                } else {
                    ShellExecuteW(nullptr, L"open", j.target.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            std::cout << "[Exportar] " << export_message_ << '\n';
            export_job_.reset();
        }
    } else if (export_setup_) {
        // Resultado del selector de Windows (en su hilo).
        const bool picking = export_pick_ && !export_pick_->done;
        if (export_pick_ && export_pick_->done) {
            if (!export_pick_->result.empty()) export_folder_ = dialogs::utf8(export_pick_->result);
            export_pick_.reset();
        }
        ImGui::TextUnformatted("Carpeta de destino");
        ImGui::SetNextItemWidth(-110.0f);
        ImGui::InputText("##carpeta", &export_folder_);
        ImGui::SameLine();
        ImGui::BeginDisabled(picking);
        if (ImGui::Button("Examinar...", ImVec2(-1.0f, 0.0f))) {
            export_pick_ = dialogs::pickFolderAsync(dialogs::fromUtf8(export_folder_));
        }
        ImGui::EndDisabled();
        if (picking) {
            ImGui::TextDisabled("Elige la carpeta en la ventana de Windows (o escribe la ruta arriba).");
        }
        const std::filesystem::path parent = dialogs::fromUtf8(export_folder_);
        ImGui::TextDisabled("Se creara: %s", dialogs::utf8(parent / dialogs::fromUtf8(safeFolderName(project_.name))).c_str());
        ImGui::Checkbox("Ejecutar el juego al terminar", &export_run_after_);
        ImGui::Checkbox("Combinar mallas estaticas (static batching)", &export_static_batching_);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Las mallas de los objetos marcados Static se combinan en un lote por escena:\n"
                              "una llamada de dibujo por material, con el culling por zonas intacto.\n"
                              "El proyecto no cambia: solo la copia que va al juego.");
        }
        ImGui::Spacing();
        const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        ImGui::BeginDisabled(export_folder_.empty() || !parent.is_absolute());
        if (ImGui::Button("Exportar", ImVec2(w, 0.0f))) {
            export_setup_ = false;
            export_pick_.reset();  // si la ventana de Windows sigue abierta, se ignora
            startExport(parent);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancelar", ImVec2(w, 0.0f))) {
            export_setup_ = false;
            export_pick_.reset();
        }
        if (!export_setup_ && !export_job_ && export_message_.empty()) ImGui::CloseCurrentPopup();
    } else {
        ImGui::TextWrapped("%s", export_message_.c_str());
        if (ImGui::Button("Cerrar", ImVec2(-1.0f, 0.0f))) {
            export_message_.clear();
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndPopup();
}

void EditorApp::cancelExport() {
    if (!export_job_) return;
    export_job_->cancel = true;
    if (export_job_->thread.joinable()) export_job_->thread.join();
    export_job_.reset();
}

}  // namespace cramion::editor
