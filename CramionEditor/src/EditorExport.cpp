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
//
// Con una configuracion de Android sale <Carpeta>/<Juego>/<Juego>.apk (y/o
// .aab, y el .obb si los assets van aparte): el mismo .crpack, los shaders y
// android/<abi>/libmain.so de junto al editor, empaquetados con AndroidBuild.

#include "EditorApp.h"

#include "Dialogs.h"

#include <CramionCore/ecs/SceneSerializer.h>
#include <CramionCore/asset/SurfaceShader.h>
#include <CramionCore/ecs/StaticBatching.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <chrono>
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
    returnToSceneWorkspace();  // se exporta la escena (no un prefab abierto)
    ensureBuildConfigs();
    export_static_batching_ = build_configs_.current().static_batching;
    export_run_after_ = run_after;
    export_setup_ = true;
    if (build_configs_.current().platform == BuildPlatform::Android && run_after) refreshAndroidDevices();
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
    ensureBuildConfigs();
    const BuildConfig config = build_configs_.current();
    const std::string game_name = safeFolderName(buildGameName());
    const std::filesystem::path target = parent / dialogs::fromUtf8(game_name);
    if (isInside(target, project_.folder)) {
        export_message_ = "Elige una carpeta fuera del proyecto (el juego se copiaria dentro de si mismo).";
        std::cerr << "[Exportar] " << export_message_ << '\n';
        return;
    }
    const std::filesystem::path source = editorFolder();
    const bool android = config.platform == BuildPlatform::Android;
    if (!android && !std::filesystem::exists(source / "CramionPlayer.exe")) {
        export_message_ = "Falta CramionPlayer.exe junto al editor: compila el proyecto.";
        std::cerr << "[Exportar] " << export_message_ << '\n';
        return;
    }

    // Lista de copias (se decide aqui, se copia en el otro hilo).
    auto job = std::make_unique<ExportJob>();
    job->target = target;
    job->exe = target / dialogs::fromUtf8(game_name + ".exe");
    if (const std::filesystem::path icon = buildIconPath(config); !icon.empty()) {
        if (std::filesystem::exists(icon) && isIconSource(icon)) {
            job->icon = icon;
        } else {
            std::cerr << "[Exportar] El icono " << dialogs::utf8(icon) << " no existe o no es una imagen: se usa el del motor\n";
        }
    }
    job->run_after = run_after;
    job->android = android;
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
    // Android: nada se copia a la carpeta; el juego (Game/ y el paquete) se
    // prepara en _build y se empaqueta en el APK/AAB al final del hilo.
    const std::filesystem::path game = android ? target / "_build" / "Game" : target / "Game";
    if (android) {
        const AndroidBuildSettings& a = config.android;
        job->android_tools = findAndroidToolchain(source, a.target_sdk);
        if (!job->android_tools.ok()) {
            export_message_ = job->android_tools.error;
            std::cerr << "[Exportar] " << export_message_ << '\n';
            return;
        }
        AndroidPackageInput& in = job->android_input;
        in.package = a.package.empty() ? defaultAndroidPackage(game_name) : a.package;
        if (!validAndroidPackage(in.package)) {
            export_message_ = "El nombre de paquete \"" + in.package +
                              "\" no es valido (como com.estudio.juego). Cambialo en Configuraciones de compilacion.";
            return;
        }
        in.label = buildGameName();
        in.version_name = config.version.empty() ? std::string("1.0") : config.version;
        in.version_code = a.version_code;
        in.min_sdk = a.min_sdk;
        in.target_sdk = a.target_sdk;
        in.orientation = a.orientation;
        in.internet = a.internet;
        in.vibrate = a.vibrate;
        in.record_audio = a.record_audio;
        in.make_apk = a.make_apk || !a.make_aab;
        in.make_aab = a.make_aab;
        in.split_obb = a.split_obb;
        std::filesystem::path icon = a.icon.empty() ? buildIconPath(config) : dialogs::fromUtf8(a.icon);
        if (!icon.empty() && icon.is_relative()) icon = project_.folder / icon;
        std::string icon_ext = dialogs::utf8(icon.extension());
        std::transform(icon_ext.begin(), icon_ext.end(), icon_ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (icon_ext == ".ico") icon.clear();  // un .ico no sirve en Android: el del motor
        in.icon = icon;
        in.fallback_icon = source / "editor_icons" / "logo.png";
        for (const char* abi : {"arm64-v8a", "x86_64"}) {
            if (std::string(abi) == "x86_64" && !a.x86_64) continue;
            const std::filesystem::path so = source / "android" / abi / "libmain.so";
            if (std::filesystem::exists(so)) in.libraries.emplace_back(abi, so);
        }
        if (in.libraries.empty() || in.libraries.front().first != "arm64-v8a") {
            export_message_ = "Falta android/arm64-v8a/libmain.so junto al editor: compila el motor con el Android NDK "
                              "(CRAMION_ANDROID=ON) para poder exportar a Android.";
            std::cerr << "[Exportar] " << export_message_ << '\n';
            return;
        }
        // Shaders del motor y Game/ (game.ini y banner) como assets del APK.
        std::error_code se;
        for (std::filesystem::recursive_directory_iterator it(source / "shaders", se);
             !se && it != std::filesystem::recursive_directory_iterator(); it.increment(se)) {
            std::error_code fe;
            if (!it->is_regular_file(fe)) continue;
            const std::u8string rel = std::filesystem::relative(it->path(), source, fe).generic_u8string();
            in.assets.emplace_back(std::string(rel.begin(), rel.end()), it->path());
        }
        in.assets.emplace_back("Game/banner.png", source / "player_banner.png");
        in.assets.emplace_back("Game/game.ini", game / "game.ini");
        in.output_folder = target;
        in.file_stem = game_name;
        in.work_folder = target / "_build" / "work";
        in.pack_name = game_name + ".crpack";
        in.key.keystore = a.keystore.empty() ? std::filesystem::path() : dialogs::fromUtf8(a.keystore);
        if (!in.key.keystore.empty() && in.key.keystore.is_relative()) in.key.keystore = project_.folder / in.key.keystore;
        in.key.alias = a.key_alias;
        in.key.store_password = a.store_password;
        in.key.key_password = a.key_password;
        if (!in.key.keystore.empty() && in.key.store_password.empty()) {
            export_message_ = "Escribe la contrasena del keystore en Configuraciones de compilacion > Android > Firma "
                              "(no se guarda en el proyecto).";
            return;
        }
        if (run_after) {
            if (export_device_.empty()) refreshAndroidDevices();
            job->android_device = export_device_;
            if (job->android_device.empty() || !in.make_apk) {
                export_message_ = in.make_apk ? "No hay ningun dispositivo Android conectado (activa la depuracion USB o abre un emulador)."
                                              : "Para instalar en el dispositivo hace falta el APK (activalo en la configuracion).";
                return;
            }
        }
    } else {
        add_file(source / "CramionPlayer.exe", job->exe);
        add_folder(source / "shaders", target / "shaders");
        for (std::filesystem::directory_iterator it(source, error); !error && it != std::filesystem::directory_iterator();
             it.increment(error)) {
            std::error_code fe;
            if (it->is_regular_file(fe) && it->path().extension() == ".dll") add_file(it->path(), target / it->path().filename());
        }
        add_file(source / "player_banner.png", game / "banner.png");
        // Scripts de C++: el proceso aislado y la DLL (al dia) con sus simbolos.
        if (cpp_scripts_.hasSources()) {
            if (!cpp_scripts_.upToDate()) {
                while (cpp_scripts_.compiling()) Sleep(20);
                cpp_scripts_.takeCompileResult();
                const scripting::CppCompileResult built = cpp_scripts_.compile();
                cpp_compile_errors_ = built.errors;
                if (!built.ok) {
                    export_message_ = "Los scripts de C++ no compilan (" + std::to_string(built.errors.size()) +
                                      " errores): mira la Consola o Ventana > Variables.";
                    return;
                }
                cpp_scripts_.setBuildFolder(project_.libraryFolder() / "CppScripts");
            }
            add_file(source / "CramionScriptHost.exe", target / "CramionScriptHost.exe");
            add_file(cpp_scripts_.dll(), target / "scripts" / "game_scripts.dll");
            std::filesystem::path pdb = cpp_scripts_.dll();
            pdb.replace_extension(".pdb");
            std::error_code pe;
            if (std::filesystem::exists(pdb, pe)) add_file(pdb, target / "scripts" / pdb.filename());
        }
    }

    // Los assets del juego, comprimidos en un solo archivo .crpack.
    job->pack_file = game / dialogs::fromUtf8(game_name + ".crpack");
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
    // La configuracion puede elegir otra.
    if (const Uuid chosen = Uuid::parse(config.startup_scene); chosen.valid()) {
        if (const auto info = database_->find(chosen)) first_scene = info->path;
    }
    job->game_ini = "scene=" + assetRelative(first_scene) + "\n" + buildConfigIni(config, buildGameName());
    // Identificador de esta compilacion: el juego de Android recopia sus
    // archivos del APK cuando cambia.
    job->game_ini += "build=" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "\n";
    if (android) job->android_input.pack = job->pack_file;
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
        // El icono de la configuracion, dentro del .exe (siempre: la copia
        // puede haberse saltado y traer el icono de la exportacion anterior).
        if (!j->cancel && j->error.empty() && !j->icon.empty()) {
            {
                std::lock_guard lock(j->mutex);
                j->current = "Icono del juego";
            }
            std::string icon_error;
            if (setExeIcon(j->exe, j->icon, &icon_error)) {
                std::cout << "[Exportar] Icono: " << dialogs::utf8(j->icon.filename()) << '\n';
            } else {
                std::cerr << "[Exportar] No se pudo poner el icono: " << icon_error << '\n';
                j->batch_summary += "\nIcono: " + icon_error;
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
        // Android no tiene compilador de shaders: cada .crshader va al paquete
        // tambien compilado (<archivo>.vert.spv / .frag.spv al lado).
        if (j->android && !j->cancel && j->error.empty()) {
            const std::filesystem::path spv_cache = j->batch_cache.parent_path() / "AndroidShaders";
            std::error_code e;
            std::filesystem::remove_all(spv_cache, e);
            const std::size_t count = j->pack.size();
            for (std::size_t i = 0; i < count; ++i) {
                std::string extension = dialogs::utf8(j->pack[i].source.extension());
                std::transform(extension.begin(), extension.end(), extension.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                if (extension != ".crshader") continue;
                assets::SurfaceShaderSource shader;
                std::vector<std::uint32_t> vertex, fragment;
                std::string error;
                if (!assets::loadSurfaceShader(j->pack[i].source, shader, &error) ||
                    !assets::compileSurfaceShader(shader, assets::surfaceTemplateDirectory(), vertex, fragment, &error)) {
                    std::cerr << "[Exportar] El shader " << j->pack[i].path << " no compila (en el movil se vera sin el): " << error
                              << '\n';
                    continue;
                }
                const std::filesystem::path base = spv_cache / std::to_string(i);
                const std::filesystem::path vfile = std::filesystem::path(base).concat(".vert.spv");
                const std::filesystem::path ffile = std::filesystem::path(base).concat(".frag.spv");
                if (assets::writePrecompiledSurfaceShader(vfile, ffile, vertex, fragment)) {
                    j->pack.push_back(project::PackInput{vfile, j->pack[i].path + ".vert.spv"});
                    j->pack.push_back(project::PackInput{ffile, j->pack[i].path + ".frag.spv"});
                }
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
            std::vector<std::filesystem::path> stale;
            for (std::filesystem::directory_iterator it(j->game_folder, e); !e && it != std::filesystem::directory_iterator(); it.increment(e)) {
                if (it->path().extension() == ".crproj") stale.push_back(it->path());
                // Paquete de otro nombre (se cambio el nombre del juego): el
                // juego cogeria cualquiera.
                if (it->path().extension() == ".crpack" && it->path() != j->pack_file) stale.push_back(it->path());
            }
            for (const std::filesystem::path& file : stale) std::filesystem::remove(file, e);
            std::filesystem::create_directories(j->game_folder, e);
            std::ofstream(j->game_folder / "game.ini") << j->game_ini;
        }
        // Android: el APK/AAB con todo lo anterior.
        if (j->android && !j->cancel && j->error.empty()) {
            std::string error;
            const bool ok = buildAndroidPackage(j->android_tools, j->android_input, [j](float f, const std::string& what) {
                j->phase = f;
                std::lock_guard lock(j->mutex);
                j->current = what;
            }, &j->cancel, j->android_result, &error);
            std::cout << j->android_result.log;
            if (!ok && !j->cancel) {
                std::lock_guard lock(j->mutex);
                j->error = error;
            }
            if (ok && !j->android_device.empty()) {
                {
                    std::lock_guard lock(j->mutex);
                    j->current = "Instalando en " + j->android_device;
                }
                std::string log;
                if (!installAndroidPackage(j->android_tools, j->android_device, j->android_result.apk, j->android_result.obb,
                                           j->android_input.package, log, &error)) {
                    std::lock_guard lock(j->mutex);
                    j->error = "El juego se exporto pero no se pudo instalar: " + error;
                }
                std::cout << log;
            }
            // Los temporales (el paquete ya esta dentro del APK/AAB o en el .obb).
            if (ok) {
                std::error_code e;
                std::filesystem::remove_all(j->target / "_build", e);
            }
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
        const float phase = j.phase.load();
        ImGui::TextUnformatted(j.cancel ? "Cancelando..." : (phase >= 0.0f ? "Empaquetando para Android..." : "Copiando el juego..."));
        char overlay[64];
        if (phase >= 0.0f) {
            std::snprintf(overlay, sizeof(overlay), "%.0f %%", phase * 100.0f);
            ImGui::ProgressBar(phase, ImVec2(-1.0f, 0.0f), overlay);
        } else {
            std::snprintf(overlay, sizeof(overlay), "%.0f %%  (%.1f / %.1f MB)", fraction * 100.0f, done / 1048576.0,
                          total / 1048576.0);
            ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), overlay);
        }
        ImGui::TextDisabled("%s", current.c_str());
        if (!j.cancel && ImGui::Button("Cancelar", ImVec2(-1.0f, 0.0f))) j.cancel = true;
        if (j.finished) {
            j.thread.join();
            if (j.cancel) {
                export_message_ = "Exportacion cancelada.";
            } else if (!j.error.empty()) {
                export_message_ = j.error;
            } else if (j.android) {
                const AndroidPackageResult& r = j.android_result;
                export_message_ = "Juego exportado para Android en " + dialogs::utf8(j.target) + " (escena inicial: " + j.scene_name + ")";
                if (!r.apk.empty()) export_message_ += "\n  APK: " + dialogs::utf8(r.apk.filename());
                if (!r.aab.empty()) export_message_ += "\n  AAB (Google Play): " + dialogs::utf8(r.aab.filename());
                if (!r.obb.empty()) {
                    export_message_ += "\n  OBB: " + dialogs::utf8(r.obb.filename()) + "  (va en /sdcard/Android/obb/" +
                                       j.android_input.package + "/)";
                }
                export_message_ += "\n  Paquete: " + j.android_input.package;
                if (j.android_input.key.keystore.empty()) {
                    export_message_ += "\n\nFirmado con la clave de depuracion: vale para probar, no para Google Play.";
                }
                if (!j.android_device.empty()) export_message_ += "\n\nInstalado y abierto en " + j.android_device + ".";
                if (!j.batch_summary.empty()) export_message_ += "\n\nStatic batching:" + j.batch_summary;
                if (j.android_device.empty()) {
                    ShellExecuteW(nullptr, L"open", j.target.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
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
        // Configuracion de compilacion (nombre, icono, ventana...).
        ensureBuildConfigs();
        ImGui::TextUnformatted("Configuracion");
        ImGui::SetNextItemWidth(-110.0f);
        if (ImGui::BeginCombo("##build_config", build_configs_.current().name.c_str())) {
            for (int i = 0; i < static_cast<int>(build_configs_.configs.size()); ++i) {
                if (ImGui::Selectable(build_configs_.configs[static_cast<std::size_t>(i)].name.c_str(), i == build_configs_.active)) {
                    build_configs_.active = i;
                    export_static_batching_ = build_configs_.current().static_batching;
                    saveBuildConfigsNow();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Editar...", ImVec2(-1.0f, 0.0f))) {
            build_config_selected_ = build_configs_.active;
            show_build_configs_ = true;
        }
        const std::string game_name = safeFolderName(buildGameName());
        const BuildConfig& active = build_configs_.current();
        if (active.platform == BuildPlatform::Android) {
            const std::string file = game_name + (active.android.make_apk || !active.android.make_aab ? ".apk" : ".aab");
            ImGui::TextDisabled("Android: %s", dialogs::utf8(parent / dialogs::fromUtf8(game_name) / dialogs::fromUtf8(file)).c_str());
            ImGui::Checkbox("Instalar y abrir en el dispositivo", &export_run_after_);
            if (export_run_after_) {
                ImGui::SetNextItemWidth(-110.0f);
                if (ImGui::BeginCombo("##device", export_device_.empty() ? "(ninguno conectado)" : export_device_.c_str())) {
                    for (const std::string& d : export_devices_) {
                        if (ImGui::Selectable(d.c_str(), d == export_device_)) export_device_ = d;
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::Button("Buscar", ImVec2(-1.0f, 0.0f))) refreshAndroidDevices();
                ImGui::TextDisabled("Por USB con la depuracion activada, o un emulador abierto.");
            }
        } else {
            ImGui::TextDisabled("Se creara: %s", dialogs::utf8(parent / dialogs::fromUtf8(game_name) / dialogs::fromUtf8(game_name + ".exe")).c_str());
            ImGui::Checkbox("Ejecutar el juego al terminar", &export_run_after_);
        }
        if (ImGui::Checkbox("Combinar mallas estaticas (static batching)", &export_static_batching_)) {
            build_configs_.current().static_batching = export_static_batching_;
            saveBuildConfigsNow();
        }
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

void EditorApp::refreshAndroidDevices() {
    const AndroidToolchain tools = findAndroidToolchain(editorFolder(), 35);
    export_devices_ = androidDevices(tools);
    if (std::find(export_devices_.begin(), export_devices_.end(), export_device_) == export_devices_.end()) {
        export_device_ = export_devices_.empty() ? std::string() : export_devices_.front();
    }
}

void EditorApp::cancelExport() {
    if (!export_job_) return;
    export_job_->cancel = true;
    if (export_job_->thread.joinable()) export_job_->thread.join();
    export_job_.reset();
}

}  // namespace cramion::editor
