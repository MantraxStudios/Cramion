// Scripts de C++ en el editor (compilar al guardar, Play, crear y asignar) y
// la ventana Variables (CVars) con la memoria del editor y de los scripts.

#include "EditorApp.h"

#include "Dialogs.h"
#include "EditorLog.h"

#include <CramionCore/cvar/CVar.h>
#include <CramionCore/scripting/CppScripts.h>

#include <nlohmann/json.hpp>

#include <imgui.h>
#include <imgui_stdlib.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

namespace cramion::editor {

namespace {

std::string lowerExt(const std::string& path) {
    std::string ext = path.substr(path.find_last_of('.') == std::string::npos ? path.size() : path.find_last_of('.'));
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::string megabytes(std::uint64_t bytes) {
    char text[48];
    std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

}  // namespace

void EditorApp::setupCppScripts() {
    cpp_scripts_.stop();
    cpp_scripts_.setAssetsRoot(project_.assetsFolder());
    cpp_scripts_.setBuildFolder(project_.libraryFolder() / "CppScripts");
    cpp_scripts_.setPhysics(&physics_);
    cpp_scripts_.setLuaBridge(&scripts_);  // toda la API de Lua desde C++
    cpp_scripts_.setAssetPathResolver([this](const Uuid& uuid) -> std::string {
        if (!database_) return {};
        const std::optional<assets::AssetInfo> info = database_->find(uuid);
        if (!info || info->path.empty()) return {};
        if (info->path.is_absolute()) return assetRelative(info->path);
        return info->path.generic_string();
    });
    cpp_scripts_.writeCompileCommands();  // clangd: las rutas del SDK y del proyecto
    clangd_.stop();  // otro proyecto: se arranca de nuevo al abrir un .cpp
    cpp_compile_errors_.clear();
    cpp_status_.clear();
    cpp_has_sources_ = cpp_scripts_.hasSources();
    cpp_last_check_ = 0.0;
    // Las CVars guardadas del proyecto.
    cvar::Registry& registry = cvar::Registry::instance();
    registry.setCheatsAllowed(true);
    const std::filesystem::path file = project_.settingsFolder() / "CVars.json";
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) registry.load(file);
    cvar_saved_text_ = registry.saveJson();
    cvar_saved_generation_ = registry.generation();
}

void EditorApp::updateCppScripts() {
    clangd_.poll();
    if (!mcp_type_queue_.empty()) {
        ImGui::GetIO().AddInputCharacter(static_cast<unsigned char>(mcp_type_queue_.front()));
        mcp_type_queue_.erase(0, 1);
    }
    if (!has_project_) return;
    const double now = ImGui::GetTime();
    // Compilacion terminada.
    if (std::optional<scripting::CppCompileResult> r = cpp_scripts_.takeCompileResult()) {
        cpp_compile_errors_ = r->errors;
        if (r->nothing_to_compile) {
            cpp_status_.clear();
        } else if (r->ok) {
            cpp_status_ = "Compilado con " + r->compiler + " en " + std::to_string(r->seconds).substr(0, 4) + " s (" +
                          std::to_string(cpp_scripts_.classes().size()) + " clases cargadas la ultima vez)";
            std::cout << "[C++] Scripts compilados (" << r->compiler << ", " << std::to_string(r->seconds).substr(0, 4) << " s)"
                      << std::endl;
            if (playing()) cpp_scripts_.reload();  // en Play: con la DLL nueva al momento
        } else {
            cpp_status_ = "Errores de compilacion: " + std::to_string(r->errors.size());
            for (const scripting::ScriptError& e : r->errors) {
                std::cerr << "[C++] " << (e.file.empty() ? "" : e.file + ":" + std::to_string(e.line) + ": ") << e.message << std::endl;
            }
        }
    }
    // Fuentes cambiadas: se compila sola (cada 2 s se mira; sin .cpp no hace nada).
    if (now - cpp_last_check_ > 2.0 && !cpp_scripts_.compiling()) {
        const bool forced = cpp_last_check_ < 0.0;  // al guardar, F7, Compilar
        cpp_last_check_ = now;
        cpp_has_sources_ = cpp_scripts_.hasSources();
        if (cpp_has_sources_ && !cpp_scripts_.upToDate()) {
            // Con errores la DLL sigue vieja: no se repite hasta que cambie algun archivo.
            const std::filesystem::file_time_type newest = cpp_scripts_.newestSourceTime();
            if (forced || newest != cpp_attempted_) {
                cpp_attempted_ = newest;
                cpp_status_ = "Compilando...";
                cpp_scripts_.compileAsync();
            }
        }
    }
    // CVars guardadas: al cambiar alguna (con 1 s de margen).
    cvar::Registry& registry = cvar::Registry::instance();
    if (registry.generation() != cvar_saved_generation_) {
        if (cvar_dirty_time_ < 0.0) cvar_dirty_time_ = now;
        if (now - cvar_dirty_time_ > 1.0) {
            cvar_saved_generation_ = registry.generation();
            cvar_dirty_time_ = -1.0;
            const std::string text = registry.saveJson();
            if (text != cvar_saved_text_) {
                std::string error;
                if (registry.save(project_.settingsFolder() / "CVars.json", &error)) cvar_saved_text_ = text;
            }
        }
    }
}

void EditorApp::startCppScripts() {
    cpp_scripts_.clearErrors();
    if (cpp_scripts_.hasSources() && !cpp_scripts_.upToDate()) {
        // Al dar Play con cambios sin compilar: se espera (o se compila ya).
        if (!cpp_scripts_.compiling()) cpp_scripts_.compileAsync();
        std::cout << "[C++] Compilando los scripts antes de Play..." << std::endl;
        while (cpp_scripts_.compiling()) Sleep(20);
        cpp_last_check_ = -10.0;
        updateCppScripts();
    }
    cpp_scripts_.start(world_);
}

void EditorApp::stopCppScripts() {
    cpp_scripts_.stop();
    cvar::Registry::instance().clearDynamic();  // las CVars de los scripts (su valor guardado no se pierde)
}

void EditorApp::createCppScriptAsset(const std::filesystem::path& folder, ecs::Entity attach_to) {
    // Primero el nombre (es el de la clase y el de los dos archivos).
    new_cpp_script_.open = true;
    new_cpp_script_.focus = true;
    new_cpp_script_.folder = folder.empty() ? project_.assetsFolder() / "Scripts" : folder;
    new_cpp_script_.entity = attach_to.valid() ? attach_to.uuid() : Uuid{};
    std::string base = attach_to.valid() ? attach_to.name() : std::string("NuevoScript");
    std::string id;
    for (const char c : base) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') id.push_back(c);
    }
    if (id.empty() || std::isdigit(static_cast<unsigned char>(id[0]))) id = "Script" + id;
    std::string name = id;
    for (int i = 2; !cppScriptNameProblem(new_cpp_script_.folder, name).empty() && i < 100; ++i) name = id + std::to_string(i);
    new_cpp_script_.name = name;
}

// Por que no vale un nombre de script de C++ (vacio si vale).
std::string EditorApp::cppScriptNameProblem(const std::filesystem::path& folder, const std::string& name) const {
    if (name.empty()) return "Escribe un nombre";
    if (std::isdigit(static_cast<unsigned char>(name[0]))) return "No puede empezar por un numero";
    for (const char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return "Solo letras (sin tildes), numeros y _ (es el nombre de una clase de C++)";
    }
    // Palabras de C++ y nombres del SDK (chocarian con la clase).
    static const char* reserved[] = {"Script", "Entity", "Scene", "Input", "Physics", "Time", "Debug", "Vec2", "Vec3", "Quat", "Color",
                                     "Value", "Values", "Property", "Collision", "Audio", "Graphics", "Navigation", "Network", "Http",
                                     "Prefs", "Random", "Screen", "Environment", "Weather", "Voxel", "Fire", "Fluid", "DataPack", "XR",
                                     "Lua", "CVars", "CVar", "Json", "Game", "CharacterController", "Range", "Tooltip", "Header", "Label",
                                     "Options", "Requires", "Model", "Material", "Prefab", "Texture", "AudioClip", "class", "struct",
                                     "int", "float", "double", "bool", "void", "auto", "return", "new", "delete", "this", "namespace",
                                     "template", "public", "private", "const", "static", "main", "std"};
    for (const char* r : reserved) {
        if (name == r) return "\"" + name + "\" ya es un nombre del motor o de C++";
    }
    std::error_code ec;
    if (std::filesystem::exists(folder / dialogs::fromUtf8(name + ".cpp"), ec) || std::filesystem::exists(folder / dialogs::fromUtf8(name + ".h"), ec)) {
        return "Ya hay un " + name + ".cpp o " + name + ".h en esa carpeta";
    }
    if (scripting::findCppScriptClass(name) != nullptr) return "Ya hay otra clase " + name + " en el proyecto";
    // Otro archivo con ese nombre en otra carpeta: dos clases iguales no enlazan.
    for (std::filesystem::recursive_directory_iterator it(project_.assetsFolder(), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string ext = lowerExt(dialogs::utf8(it->path().filename()));
        if ((ext == ".cpp" || ext == ".h") && dialogs::utf8(it->path().stem()) == name) {
            return "Ya existe " + assetRelative(it->path()) + " (dos clases con el mismo nombre no compilan)";
        }
    }
    return {};
}

void EditorApp::drawNewCppScriptModal() {
    if (new_cpp_script_.open) {
        ImGui::OpenPopup("Nuevo script C++");
        new_cpp_script_.open = false;
    }
    ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Nuevo script C++", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("Nombre del script (y de su clase):");
    if (new_cpp_script_.focus) {
        ImGui::SetKeyboardFocusHere();
        new_cpp_script_.focus = false;
    }
    ImGui::SetNextItemWidth(480.0f);
    const bool enter = ImGui::InputText("##nombre", &new_cpp_script_.name, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    const std::string problem = cppScriptNameProblem(new_cpp_script_.folder, new_cpp_script_.name);
    if (problem.empty()) {
        ImGui::TextDisabled("Se crean %s/%s.h y %s.cpp", assetRelative(new_cpp_script_.folder).c_str(), new_cpp_script_.name.c_str(),
                            new_cpp_script_.name.c_str());
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.45f, 1.0f), "%s", problem.c_str());
    }
    const ecs::Entity target = new_cpp_script_.entity.valid() ? world_.find(new_cpp_script_.entity) : ecs::Entity{};
    if (target.valid()) ImGui::TextDisabled("Se le pone a: %s", target.name().c_str());
    ImGui::Spacing();
    ImGui::BeginDisabled(!problem.empty());
    const bool create = ImGui::Button("Crear", ImVec2(120.0f, 0.0f)) || (enter && problem.empty());
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancelar", ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (create && problem.empty()) {
        createCppScriptFiles(new_cpp_script_.folder, target, new_cpp_script_.name);
        ImGui::CloseCurrentPopup();
    } else if (cancel) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

std::filesystem::path EditorApp::createCppScriptFiles(const std::filesystem::path& target_folder, ecs::Entity attach_to,
                                                      const std::string& cls) {
    std::error_code error;
    std::filesystem::create_directories(target_folder, error);
    const std::filesystem::path path = target_folder / dialogs::fromUtf8(cls + ".cpp");
    // Como en un proyecto de C++: la clase en el .h y el codigo en el .cpp.
    std::ofstream(target_folder / dialogs::fromUtf8(cls + ".h"), std::ios::binary) << scripting::cppScriptHeaderTemplate(cls);
    std::ofstream(path, std::ios::binary) << scripting::cppScriptTemplate(cls);
    refreshDatabase();
    std::cout << "[Editor] Script C++ creado: " << dialogs::utf8(path.filename()) << std::endl;
    if (attach_to.valid()) {
        attachScriptFile(attach_to, assetRelative(path));
        commit();
    }
    cpp_last_check_ = -10.0;  // compilar ya
    file_tree_time_ = -10.0;
    openScript(path);
    return path;
}

// El .cpp de un script soltado: el mismo si es un .cpp; si es un .h, el .cpp
// de al lado con su nombre (o el que tenga una clase declarada en el .h).
std::string EditorApp::cppSourceFor(const std::string& relative) {
    const std::string ext = lowerExt(relative);
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") return relative;
    if (ext != ".h" && ext != ".hpp") return {};
    const std::filesystem::path header = dialogs::fromUtf8(relative);
    for (const char* e : {".cpp", ".cc", ".cxx"}) {
        std::filesystem::path candidate = header;
        candidate.replace_extension(e);
        std::error_code ec;
        if (std::filesystem::exists(project_.assetsFolder() / candidate, ec)) return assetRelative(project_.assetsFolder() / candidate);
    }
    // Otro .cpp que lo incluya.
    const std::string name = dialogs::utf8(header.filename());
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(project_.assetsFolder(), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string x = lowerExt(dialogs::utf8(it->path().filename()));
        if (x != ".cpp" && x != ".cc" && x != ".cxx") continue;
        std::ifstream in(it->path(), std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        if (text.str().find("\"" + name + "\"") != std::string::npos) return assetRelative(it->path());
    }
    return {};
}

void EditorApp::attachScriptFile(ecs::Entity e, const std::string& dropped) {
    if (!e.valid()) return;
    std::string relative = dropped;
    std::string ext = lowerExt(relative);
    if (ext == ".h" || ext == ".hpp") {
        relative = cppSourceFor(dropped);
        if (relative.empty()) {
            std::cerr << "[C++] " << dropped << " no tiene un .cpp con CRAMION_SCRIPT al lado" << std::endl;
            return;
        }
        ext = lowerExt(relative);
    }
    if (ext == ".lua") {
        scripting::Script& s = e.has<scripting::Script>() ? e.get<scripting::Script>() : e.add<scripting::Script>();
        s.file = relative;
    } else if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") {
        // Como en Unity: el archivo y su clase (la de CRAMION_SCRIPT del archivo).
        scripting::CppScript& s = e.has<scripting::CppScript>() ? e.get<scripting::CppScript>() : e.add<scripting::CppScript>();
        if (s.script != relative) s.values.clear();
        s.script = relative;
        const std::vector<std::string> classes = cppClassesOfFile(relative);
        s.class_name = classes.empty() ? dialogs::utf8(dialogs::fromUtf8(relative).stem()) : classes.front();
    }
}

// Las clases de un .cpp: las de la ultima compilacion o, si aun no se compilo,
// las CRAMION_SCRIPT(...) que se leen en el archivo.
std::vector<std::string> EditorApp::cppClassesOfFile(const std::string& relative) {
    std::vector<std::string> out;
    for (const scripting::CppClassInfo* c : scripting::cppScriptClassesInFile(relative)) out.push_back(c->name);
    if (!out.empty()) return out;
    std::ifstream in(project_.assetsFolder() / dialogs::fromUtf8(relative), std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    const std::string src = text.str();
    for (std::size_t at = src.find("CRAMION_SCRIPT("); at != std::string::npos; at = src.find("CRAMION_SCRIPT(", at + 1)) {
        // Al principio de una linea (no en comentarios ni en el #define).
        std::size_t line = src.rfind('\n', at);
        line = line == std::string::npos ? 0 : line + 1;
        if (src.find_first_not_of(" \t", line) != at) continue;
        std::size_t b = at + 15;
        while (b < src.size() && std::isspace(static_cast<unsigned char>(src[b]))) ++b;
        std::size_t e = b;
        while (e < src.size() && (std::isalnum(static_cast<unsigned char>(src[e])) || src[e] == '_' || src[e] == ':')) ++e;
        if (e > b) out.push_back(src.substr(b, e - b));
    }
    return out;
}

void EditorApp::drawCppScriptInspector(ecs::Entity entity, bool header) {
    scripting::CppScript* script = entity.tryGet<scripting::CppScript>();
    if (script == nullptr) return;
    ImGui::PushID("cppscript");
    const float w = ImGui::GetContentRegionAvail().x;
    if (header) {
        // El archivo: soltar un .cpp (no se escribe nada).
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Script");
        ImGui::SameLine(110.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        const std::string label = script->script.empty() ? std::string("Suelta aqui un script de C++ (.cpp o .h)") : script->script;
        if (ImGui::Button(label.c_str(), ImVec2(-1.0f, 0.0f)) && !script->script.empty()) {
            openScript(project_.assetsFolder() / dialogs::fromUtf8(script->script));
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && !script->script.empty()) ImGui::SetTooltip("Clic: abrirlo en el editor de codigo");
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kScriptPayload)) {
                const std::string relative = assetRelative(dialogs::fromUtf8(static_cast<const char*>(payload->Data)));
                if (!cppSourceFor(relative).empty()) {
                    attachScriptFile(entity, relative);
                    script = entity.tryGet<scripting::CppScript>();
                    commit();
                } else {
                    std::cerr << "[C++] Suelta un .cpp (los .lua van en el componente Script)" << std::endl;
                }
            }
            ImGui::EndDragDropTarget();
        }
        // La clase: si el archivo tiene varias, se elige (nunca se escribe).
        const std::vector<std::string> classes = script->script.empty() ? std::vector<std::string>{} : cppClassesOfFile(script->script);
        if (classes.size() > 1 || (!classes.empty() && std::find(classes.begin(), classes.end(), script->class_name) == classes.end())) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Clase");
            ImGui::SameLine(110.0f);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##clase", script->class_name.empty() ? "(elige una)" : script->class_name.c_str())) {
                for (const std::string& c : classes) {
                    if (ImGui::Selectable(c.c_str(), c == script->class_name)) {
                        script->class_name = c;
                        script->values.clear();
                        commit();
                    }
                }
                ImGui::EndCombo();
            }
        }
        const float half = (w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        ImGui::BeginDisabled(script->script.empty());
        if (ImGui::Button("Editar", ImVec2(half, 0.0f))) {
            std::filesystem::path header = project_.assetsFolder() / dialogs::fromUtf8(script->script);
            header.replace_extension(".h");
            std::error_code ec;
            if (std::filesystem::exists(header, ec)) openScript(header);
            openScript(project_.assetsFolder() / dialogs::fromUtf8(script->script));
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Nuevo script C++", ImVec2(half, 0.0f))) createCppScriptAsset({}, entity);
        // Estado: compilando, sin compilar aun, errores...
        if (!script->script.empty()) {
            if (cpp_scripts_.compiling()) {
                ImGui::TextDisabled("Compilando...");
            } else if (scripting::findCppScriptClass(script->class_name) == nullptr) {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(230, 190, 60, 255));
                ImGui::TextWrapped("%s", classes.empty() ? "El archivo no tiene CRAMION_SCRIPT(Clase)."
                                                         : "Aun no compilado: sus propiedades salen al compilar (se compila solo al guardar).");
                ImGui::PopStyleColor();
            }
            int shown = 0;
            for (const scripting::ScriptError& e : cpp_compile_errors_) {
                if (e.file != script->script || shown++ >= 3) continue;
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 110, 110, 255));
                ImGui::TextWrapped("%d: %s", e.line, e.message.c_str());
                ImGui::PopStyleColor();
            }
        }
        ImGui::PopID();
        return;
    }
    // Abajo: las Property de archivos (imagen, sonido, Lua, shader): se sueltan.
    const scripting::CppClassInfo* info = scripting::findCppScriptClass(script->class_name);
    if (info != nullptr) {
        for (const scripting::CppPropertyInfo& p : info->properties) {
            if (p.kind != "file") continue;
            const int file_kind = std::atoi(p.type.c_str());
            const scripting::CppScriptValue* stored = script->value(p.name);
            nlohmann::json current = nlohmann::json::parse(stored != nullptr ? stored->json : p.default_json, nullptr, false);
            const std::string path = current.is_string() ? current.get<std::string>() : std::string();
            static const char* kinds[] = {"imagen", "sonido", "script Lua", "shader", "archivo"};
            ImGui::PushID(p.name.c_str());
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted((p.label.empty() ? p.name : p.label).c_str());
            if (!p.tooltip.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.tooltip.c_str());
            ImGui::SameLine(110.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
            const std::string label = path.empty() ? std::string("(suelta un ") + kinds[std::clamp(file_kind, 0, 4)] + ")" : path;
            ImGui::Button(label.c_str(), ImVec2(-28.0f, 0.0f));
            ImGui::PopStyleColor();
            bool changed = false;
            std::string next = path;
            if (ImGui::BeginDragDropTarget()) {
                const char* accepted[3] = {nullptr, nullptr, nullptr};
                switch (file_kind) {
                    case 0: accepted[0] = kImagePayload; break;
                    case 1: accepted[0] = kAudioPayload; break;
                    case 2: case 3: accepted[0] = kScriptPayload; break;
                    default: accepted[0] = kImagePayload; accepted[1] = kAudioPayload; accepted[2] = kScriptPayload; break;
                }
                for (const char* type : accepted) {
                    if (type == nullptr) continue;
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(type)) {
                        next = assetRelative(dialogs::fromUtf8(static_cast<const char*>(payload->Data)));
                        changed = true;
                    }
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::SameLine();
            if (ImGui::Button("x", ImVec2(-1.0f, 0.0f)) && !path.empty()) {
                next.clear();
                changed = true;
            }
            if (changed) {
                script->setValue(p.name, nlohmann::json(next).dump());
                commit();
            }
            ImGui::PopID();
        }
    }
    // Errores del script en Play (caidas, excepciones: archivo:linea).
    for (const scripting::ScriptError& e : cpp_scripts_.errors()) {
        if (!script->script.empty() && e.file != script->script) continue;
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 110, 110, 255));
        ImGui::TextWrapped("%s", e.message.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
}

// --- IntelliSense de C++ (clangd) ---

void EditorApp::ensureClangd() {
    if (clangd_.running() || !has_project_) return;
    static double last_try = -100.0;
    if (ImGui::GetTime() - last_try < 10.0) return;  // sin clangd: no reintentar cada frame
    last_try = ImGui::GetTime();
    const std::filesystem::path exe = scripting::CppScriptSystem::findClangd();
    if (exe.empty()) {
        std::cerr << "[C++] No hay clangd (falta toolchain/ junto al editor): sin IntelliSense de C++" << std::endl;
        return;
    }
    cpp_scripts_.writeCompileCommands();
    if (clangd_.start(exe, project_.assetsFolder().parent_path())) {
        std::cout << "[C++] IntelliSense: " << dialogs::utf8(exe) << std::endl;
        for (ScriptTab& tab : script_tabs_) tab.clangd_synced.clear();
    } else {
        std::cerr << "[C++] clangd no arranco: " << clangd_.status() << std::endl;
    }
}

void EditorApp::syncClangd(ScriptTab& tab) {
    if (!tab.cpp || !clangd_.running()) return;
    if (!clangd_.isOpen(tab.path)) {
        clangd_.open(tab.path, tab.text);
    } else if (tab.clangd_synced != tab.text) {
        clangd_.change(tab.path, tab.text);
    }
    tab.clangd_synced = tab.text;
}

EditorApp::ScriptTab* EditorApp::scriptTabFor(const std::filesystem::path& file) {
    for (ScriptTab& tab : script_tabs_) {
        if (tab.path == file) return &tab;
    }
    return nullptr;
}

void EditorApp::goToDefinition(ScriptTab& tab, int offset) {
    if (!tab.cpp || !clangd_.running()) return;
    syncClangd(tab);
    offset = std::clamp(offset, 0, static_cast<int>(tab.text.size()));
    int line = 0, line_start = 0;
    for (int k = 0; k < offset; ++k) {
        if (tab.text[static_cast<std::size_t>(k)] == '\n') {
            ++line;
            line_start = k + 1;
        }
    }
    clangd_.definition(tab.path, line, offset - line_start, [this](std::vector<ClangdLocation> where) {
        if (where.empty()) return;
        const ClangdLocation& loc = where.front();
        std::error_code ec;
        if (!std::filesystem::exists(loc.file, ec)) return;
        openScript(loc.file);  // tambien los del SDK (cramion/Script.h...)
        if (ScriptTab* t = scriptTabFor(loc.file)) {
            t->goto_line = loc.line + 1;
            int cursor = 0;
            for (int l = 0; l < loc.line && cursor < static_cast<int>(t->text.size()); ++cursor) {
                if (t->text[static_cast<std::size_t>(cursor)] == '\n') ++l;
            }
            t->set_cursor = std::min(cursor + loc.column, static_cast<int>(t->text.size()));
            t->focus = true;
        }
    });
}

void EditorApp::drawCVarsWindow() {
    if (!show_cvars_window_) return;
    ImGui::SetNextWindowSize(ImVec2(760.0f, 560.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Variables (CVars) y memoria", &show_cvars_window_)) {
        ImGui::End();
        return;
    }
    // --- Memoria ---
    if (ImGui::CollapsingHeader("Memoria", ImGuiTreeNodeFlags_DefaultOpen)) {
        PROCESS_MEMORY_COUNTERS_EX counters{};
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters));
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        GlobalMemoryStatusEx(&status);
        std::uint64_t vram_used = 0, vram_budget = 0;
        renderer_.device().videoMemory(vram_used, vram_budget);
        const scripting::CppScriptSystem::Stats s = cpp_scripts_.stats();
        const cvar::CVarBase* limit = cvar::Registry::instance().find("script.cpp.MemoryLimitMB");
        if (ImGui::BeginTable("##memoria", 2, ImGuiTableFlags_SizingStretchProp)) {
            const auto row = [](const char* label, const std::string& value) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", label);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(value.c_str());
            };
            row("Editor (memoria privada)", megabytes(counters.PrivateUsage));
            row("Editor (en RAM)", megabytes(counters.WorkingSetSize));
            row("RAM del equipo", megabytes(status.ullTotalPhys - status.ullAvailPhys) + " de " + megabytes(status.ullTotalPhys) +
                                      " (" + std::to_string(status.dwMemoryLoad) + " %)");
            row("Video (VRAM)", megabytes(vram_used) + " de " + megabytes(vram_budget));
            row("Scripts de C++ (proceso aparte)",
                s.host_running ? megabytes(s.host_memory) + " de " + (limit != nullptr ? limit->toString() : std::string("?")) + " MB"
                               : std::string("parado"));
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Los topes se cambian con las CVars (p. ej. script.cpp.MemoryLimitMB).");
    }
    // --- Scripts de C++ ---
    if (ImGui::CollapsingHeader("Scripts de C++", ImGuiTreeNodeFlags_DefaultOpen)) {
        std::string kind;
        const std::string compiler = scripting::CppScriptSystem::findCompiler(&kind);
        if (compiler.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Sin compilador: instala LLVM (clang++) o Visual Studio con C++.");
        } else {
            ImGui::TextDisabled("Compilador: %s (%s)", kind.c_str(), compiler.c_str());
        }
        ImGui::TextUnformatted(cpp_scripts_.compiling() ? "Compilando..." : (cpp_status_.empty() ? "Sin scripts de C++ en Assets." : cpp_status_.c_str()));
        ImGui::BeginDisabled(cpp_scripts_.compiling() || !cpp_has_sources_);
        if (ImGui::Button("Compilar ahora")) cpp_scripts_.compileAsync();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Nuevo script C++")) createCppScriptAsset({}, world_.find(active_));
        const scripting::CppScriptSystem::Stats s = cpp_scripts_.stats();
        if (playing()) {
            ImGui::Text("En Play: %d scripts, %d desactivados por errores, %d reinicios del proceso, %.2f ms/frame, %llu llamadas al motor",
                        s.instances, s.faulted, s.restarts, s.frame_ms, static_cast<unsigned long long>(s.rpcs));
        }
        const std::vector<std::string> classes = cpp_scripts_.classes();
        if (!classes.empty()) {
            std::string list;
            for (const std::string& c : classes) list += (list.empty() ? "" : ", ") + c;
            ImGui::TextWrapped("Clases: %s", list.c_str());
        }
        for (const scripting::ScriptError& e : cpp_compile_errors_) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
            const std::string text = (e.file.empty() ? "" : e.file + ":" + std::to_string(e.line) + ": ") + e.message;
            if (ImGui::Selectable(text.c_str()) && !e.file.empty()) openScript(project_.assetsFolder() / dialogs::fromUtf8(e.file));
            ImGui::PopStyleColor();
        }
    }
    // --- CVars ---
    if (ImGui::CollapsingHeader("Variables", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##filtro", "Filtrar (nombre o descripcion)...", &cvar_filter_);
        std::string filter = cvar_filter_;
        std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ImGui::BeginTable("##cvars", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                                ImGuiTableFlags_ScrollY,
                              ImVec2(0.0f, ImGui::GetContentRegionAvail().y))) {
            ImGui::TableSetupColumn("Nombre", ImGuiTableColumnFlags_WidthStretch, 1.3f);
            ImGui::TableSetupColumn("Valor", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24.0f);
            ImGui::TableSetupColumn("Descripcion", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            for (cvar::CVarBase* c : cvar::Registry::instance().all()) {
                if (!filter.empty()) {
                    std::string hay = c->name() + " " + c->description();
                    std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                    if (hay.find(filter) == std::string::npos) continue;
                }
                ImGui::PushID(c->name().c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool changed_from_default = !c->isDefault();
                if (changed_from_default) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.35f, 1.0f));
                ImGui::TextUnformatted(c->name().c_str());
                if (changed_from_default) ImGui::PopStyleColor();
                std::string flags;
                if ((c->flags() & cvar::Saved) != 0) flags += " guardada";
                if ((c->flags() & cvar::ReadOnly) != 0) flags += " solo lectura";
                if ((c->flags() & cvar::Cheat) != 0) flags += " truco";
                if ((c->flags() & cvar::Script) != 0) flags += " de script";
                if ((c->flags() & cvar::RequiresRestart) != 0) flags += " al reiniciar";
                ImGui::SetItemTooltip("%s\nTipo: %s%s\nPor defecto: %s", c->name().c_str(), cvar::typeName(c->type()), flags.c_str(),
                                      c->defaultString().c_str());
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::BeginDisabled((c->flags() & cvar::ReadOnly) != 0);
                std::string error;
                switch (c->type()) {
                    case cvar::Type::Bool: {
                        bool v = c->toString() == "true";
                        if (ImGui::Checkbox("##v", &v)) cvar::Registry::instance().set(c->name(), v ? "true" : "false", &error);
                        break;
                    }
                    case cvar::Type::Int:
                    case cvar::Type::Float: {
                        double v = 0.0;
                        cvar::detail::parseNumber(c->toString(), v);
                        const bool integer = c->type() == cvar::Type::Int;
                        bool edited = false;
                        if (c->hasRange()) {
                            double lo = c->rangeMin(), hi = c->rangeMax();
                            edited = ImGui::DragScalar("##v", ImGuiDataType_Double, &v, integer ? 1.0f : 0.01f, &lo, &hi,
                                                       integer ? "%.0f" : "%.4g", ImGuiSliderFlags_AlwaysClamp);
                        } else {
                            edited = ImGui::InputDouble("##v", &v, 0.0, 0.0, integer ? "%.0f" : "%.6g", ImGuiInputTextFlags_EnterReturnsTrue);
                        }
                        if (edited) cvar::Registry::instance().set(c->name(), cvar::detail::formatNumber(v, integer), &error);
                        break;
                    }
                    case cvar::Type::String: {
                        std::string v = c->toString();
                        if (ImGui::InputText("##v", &v, ImGuiInputTextFlags_EnterReturnsTrue)) cvar::Registry::instance().set(c->name(), v, &error);
                        break;
                    }
                }
                ImGui::EndDisabled();
                if (!error.empty()) std::cerr << "[CVar] " << error << std::endl;
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(!changed_from_default || (c->flags() & cvar::ReadOnly) != 0);
                if (ImGui::SmallButton("R")) c->reset();
                ImGui::SetItemTooltip("Volver al valor por defecto (%s)", c->defaultString().c_str());
                ImGui::EndDisabled();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", c->description().c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

// -----------------------------------------------------------------------------
// Editor de C++: formatear, .h/.cpp, consola propia y Configuracion del motor
// -----------------------------------------------------------------------------

namespace {

// Configuracion del motor > Editor de codigo C++ (CVars guardadas del proyecto).
cvar::CVar<bool> g_format_on_save("editor.cpp.FormatOnSave", true, "Formatear los .cpp/.h al guardar (clang-format)", cvar::Saved);
cvar::CVar<std::string> g_format_style("editor.cpp.FormatStyle", "Cramion",
                                       "Estilo de formato: Cramion, LLVM, Google, Microsoft, Mozilla, WebKit, Chromium o Archivo (.clang-format del proyecto)",
                                       cvar::Saved);
cvar::CVar<int> g_format_indent("editor.cpp.IndentWidth", 4, "Espacios de sangria al formatear C++", cvar::Saved, 1, 8);
cvar::CVar<int> g_format_columns("editor.cpp.ColumnLimit", 120, "Largo maximo de linea al formatear C++ (0 = sin limite)", cvar::Saved, 0, 400);

constexpr const char* kFormatStyles[] = {"Cramion", "LLVM", "Google", "Microsoft", "Mozilla", "WebKit", "Chromium", "Archivo"};

bool isCppSource(const std::string& ext) { return ext == ".cpp" || ext == ".cc" || ext == ".cxx"; }
bool isCppHeader(const std::string& ext) { return ext == ".h" || ext == ".hpp" || ext == ".inl"; }

}  // namespace

bool EditorApp::cppFormatOnSave() { return g_format_on_save.get(); }

std::string EditorApp::cppFormatStyle() const {
    const std::string style = g_format_style.get();
    if (style == "Archivo" || style == "file") return "file";
    const std::string indent = std::to_string(g_format_indent.get());
    const std::string columns = std::to_string(g_format_columns.get());
    if (style.empty() || style == "Cramion") {
        // El del motor: llaves en la misma linea, 4 espacios, 120 columnas.
        return "{BasedOnStyle: Google, IndentWidth: " + indent + ", ColumnLimit: " + columns + ", AccessModifierOffset: -" + indent +
               ", AllowShortFunctionsOnASingleLine: Inline, AllowShortIfStatementsOnASingleLine: WithoutElse, "
               "DerivePointerAlignment: false, PointerAlignment: Left, SortIncludes: Never, IncludeBlocks: Preserve}";
    }
    return "{BasedOnStyle: " + style + ", IndentWidth: " + indent + ", ColumnLimit: " + columns + "}";
}

bool EditorApp::formatCppTab(ScriptTab& tab, bool quiet) {
    int cursor = std::clamp(tab.cursor, 0, static_cast<int>(tab.text.size()));
    std::string formatted, error;
    if (!scripting::CppScriptSystem::formatSource(tab.text, tab.path, cppFormatStyle(), formatted, &cursor, &error)) {
        std::cerr << "[C++] Formatear " << tab.relative << ": " << error << std::endl;
        return false;
    }
    if (formatted == tab.text) {
        if (!quiet) std::cout << "[C++] " << tab.relative << ": ya estaba formateado" << std::endl;
        return true;
    }
    tab.text = std::move(formatted);
    tab.reload_text = true;  // con el campo activo, ImGui relee el texto
    tab.set_cursor = std::clamp(cursor, 0, static_cast<int>(tab.text.size()));
    tab.cursor = tab.set_cursor;
    tab.completion_open = false;
    if (!quiet) std::cout << "[C++] " << tab.relative << " formateado (" << g_format_style.get() << ")" << std::endl;
    return true;
}

std::filesystem::path EditorApp::cppCounterpart(const std::filesystem::path& file) const {
    std::string ext = dialogs::utf8(file.extension());
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::vector<const char*> candidates;
    if (isCppSource(ext)) candidates = {".h", ".hpp", ".inl"};
    if (isCppHeader(ext)) candidates = {".cpp", ".cc", ".cxx"};
    for (const char* e : candidates) {
        std::filesystem::path other = file;
        other.replace_extension(e);
        std::error_code ec;
        if (std::filesystem::exists(other, ec)) return other;
    }
    return {};
}

void EditorApp::drawCppToolbar(ScriptTab& tab) {
    if (ImGui::Button("Guardar (Ctrl+S)")) saveScript(tab);
    ImGui::SameLine();
    if (ImGui::Button("Descartar cambios")) {
        std::ifstream in(tab.path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        tab.text = text.str();
        tab.saved = tab.text;
        tab.reload_text = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    // El .h de este .cpp (o el .cpp de este .h).
    std::string ext = dialogs::utf8(tab.path.extension());
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::filesystem::path other = cppCounterpart(tab.path);
    const bool header = isCppHeader(ext);
    const std::string label = std::string(header ? "Abrir .cpp" : "Abrir cabecera (.h)") + "  Alt+O";
    ImGui::BeginDisabled(other.empty());
    if (ImGui::Button(label.c_str())) openScript(other);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", other.empty() ? (header ? "No hay un .cpp con el mismo nombre al lado" : "No hay un .h con el mismo nombre al lado")
                                              : dialogs::utf8(other.filename()).c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Formatear  Mayús+Alt+F")) formatCppTab(tab);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("clang-format, estilo %s%s\nSe cambia en Editar > Configuración del motor", g_format_style.get().c_str(),
                          g_format_on_save.get() ? " (también al guardar)" : "");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(cpp_scripts_.compiling());
    if (ImGui::Button(cpp_scripts_.compiling() ? "Compilando..." : "Compilar  F7")) {
        if (tab.text != tab.saved) saveScript(tab);
        cpp_last_check_ = -10.0;
        cpp_console_scroll_ = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Configuración")) show_engine_settings_ = true;
}

void EditorApp::drawCppConsole(ScriptTab& tab) {
    (void)tab;
    // Lo nuevo del log del editor que es de C++ (compilar, Debug::log, errores en Play).
    {
        EditorLog& log = EditorLog::instance();
        std::lock_guard lock(log.mutex());
        const auto& entries = log.entries();
        const std::uint64_t first = log.total() - entries.size();
        std::uint64_t i = std::max(cpp_console_read_, first);
        for (; i < log.total(); ++i) {
            const EditorLog::Entry& e = entries[static_cast<std::size_t>(i - first)];
            const bool cpp_line = e.text.rfind("[C++]", 0) == 0 || e.text.find("Script C++") != std::string::npos ||
                                  e.text.rfind("[CVar]", 0) == 0;
            if (!cpp_line) continue;
            CppConsoleLine line;
            line.level = e.level == EditorLog::Level::Error ? 2 : (e.level == EditorLog::Level::Warning ? 1 : 0);
            line.text = e.text;
            // "[C++] Scripts/Jugador.cpp:12: mensaje": se puede ir a la linea.
            const std::string body = e.text.rfind("[C++] ", 0) == 0 ? e.text.substr(6) : std::string();
            const std::size_t colon = body.find(':');
            if (colon != std::string::npos && colon > 2) {
                const std::string file = body.substr(0, colon);
                const std::string fext = lowerExt(file);
                if (isCppSource(fext) || isCppHeader(fext)) {
                    line.file = file;
                    line.line = std::atoi(body.c_str() + colon + 1);
                    if (line.level == 0 && body.find("error") != std::string::npos) line.level = 2;
                }
            }
            cpp_console_.push_back(std::move(line));
            cpp_console_scroll_ = true;
        }
        cpp_console_read_ = i;
        if (cpp_console_.size() > 2000) cpp_console_.erase(cpp_console_.begin(), cpp_console_.end() - 2000);
    }

    // Barra: titulo, estado y botones.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Consola C++");
    ImGui::SameLine();
    if (ImGui::SmallButton("Limpiar")) cpp_console_.clear();
    ImGui::SameLine();
    if (!cpp_status_.empty()) {
        ImGui::TextColored(cpp_compile_errors_.empty() ? ImVec4(0.55f, 0.8f, 0.55f, 1.0f) : ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s",
                           cpp_status_.c_str());
    } else {
        ImGui::TextDisabled("%s", cpp_scripts_.compiling() ? "Compilando..." : "Doble clic en un error: ir a la linea");
    }
    // Arrastrar el borde de arriba cambia el alto.
    ImGui::InvisibleButton("##cpp_console_resize", ImVec2(-1.0f, 4.0f));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive()) cpp_console_height_ = std::clamp(cpp_console_height_ - ImGui::GetIO().MouseDelta.y, 60.0f, 600.0f);

    const float list_h = std::max(30.0f, cpp_console_height_ - ImGui::GetFrameHeightWithSpacing() - 8.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(24, 24, 27, 255));
    if (ImGui::BeginChild("##cpp_console", ImVec2(0.0f, list_h), ImGuiChildFlags_Borders)) {
        // Los errores de la ultima compilacion siempre arriba (con su linea).
        for (std::size_t i = 0; i < cpp_compile_errors_.size(); ++i) {
            const scripting::ScriptError& e = cpp_compile_errors_[i];
            const std::string text = (e.file.empty() ? "" : e.file + ":" + std::to_string(e.line) + ": ") + e.message;
            ImGui::PushID(static_cast<int>(i));
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 110, 110, 255));
            if (ImGui::Selectable(text.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(0) &&
                !e.file.empty()) {
                openScript(project_.assetsFolder() / dialogs::fromUtf8(e.file));
                if (ScriptTab* t = scriptTabFor(project_.assetsFolder() / dialogs::fromUtf8(e.file))) t->goto_line = e.line;
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        if (!cpp_compile_errors_.empty()) ImGui::Separator();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(cpp_console_.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const CppConsoleLine& l = cpp_console_[static_cast<std::size_t>(i)];
                const ImU32 color = l.level == 2 ? IM_COL32(255, 110, 110, 255)
                                                 : (l.level == 1 ? IM_COL32(230, 190, 90, 255) : IM_COL32(205, 210, 220, 255));
                ImGui::PushID(1000 + i);
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                if (ImGui::Selectable(l.text.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(0) &&
                    !l.file.empty()) {
                    const std::filesystem::path f = project_.assetsFolder() / dialogs::fromUtf8(l.file);
                    openScript(f);
                    if (ScriptTab* t = scriptTabFor(f)) t->goto_line = std::max(1, l.line);
                }
                ImGui::PopStyleColor();
                if (ImGui::BeginPopupContextItem("##linea")) {
                    if (ImGui::MenuItem("Copiar")) ImGui::SetClipboardText(l.text.c_str());
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
        if (cpp_console_scroll_) {
            ImGui::SetScrollHereY(1.0f);
            cpp_console_scroll_ = false;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // Linea de comandos: CVars ("nombre valor", "cvars filtro") o Lua.
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##cpp_console_input", "C++> CVar (nombre valor, cvars filtro, reset nombre) o Lua. Enter ejecuta",
                                 &cpp_console_input_, ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (!cpp_console_input_.empty()) {
            bool handled = false;
            const std::string output = cvar::Registry::instance().execute(cpp_console_input_, &handled);
            if (handled) {
                std::cout << "[CVar] > " << cpp_console_input_ << "\n" << output << std::endl;
            } else {
                std::string result;
                const bool ok = scripts_.run(cpp_console_input_, &result, &world_);
                (ok ? std::cout : std::cerr) << "[C++] Lua> " << cpp_console_input_ << (result.empty() ? "" : "  ->  ") << result << std::endl;
            }
        }
        cpp_console_input_.clear();
        ImGui::SetKeyboardFocusHere(-1);
    }
}

void EditorApp::drawEngineSettingsWindow() {
    if (!show_engine_settings_) return;
    ImGui::SetNextWindowSize(ImVec2(620.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Configuración del motor", &show_engine_settings_)) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("Se guarda con el proyecto (ProjectSettings/CVars.json). Todas las variables: Ventana > Variables (CVars).");
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Editor de código C++", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool on = g_format_on_save.get();
        if (ImGui::Checkbox("Formatear al guardar (clang-format)", &on)) g_format_on_save.set(on);
        ImGui::SetItemTooltip("Activado: cada Ctrl+S deja el .cpp/.h con el estilo elegido.\nSiempre se puede formatear a mano con Mayús+Alt+F.");
        const std::string current = g_format_style.get();
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::BeginCombo("Estilo", current.c_str())) {
            for (const char* s : kFormatStyles) {
                if (ImGui::Selectable(s, current == s)) g_format_style.set(s);
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Cramion: el del motor (llaves en la misma línea, 4 espacios).\nArchivo: el .clang-format de la carpeta del proyecto.");
        const bool from_file = current == "Archivo";
        ImGui::BeginDisabled(from_file);
        int indent = g_format_indent.get();
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::SliderInt("Sangría (espacios)", &indent, 1, 8)) g_format_indent.set(indent);
        int columns = g_format_columns.get();
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::SliderInt("Largo de línea", &columns, 0, 200)) g_format_columns.set(columns);
        ImGui::EndDisabled();
        const std::filesystem::path clang_format_file = project_.assetsFolder().parent_path() / ".clang-format";
        std::error_code ec;
        const bool has_file = has_project_ && std::filesystem::exists(clang_format_file, ec);
        if (from_file && !has_file) {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.35f, 1.0f), "El proyecto no tiene .clang-format: se usa LLVM.");
        }
        ImGui::BeginDisabled(!has_project_ || from_file);
        if (ImGui::Button("Guardar este estilo en .clang-format del proyecto")) {
            // Lo que se elige aqui, como archivo (VS Code, CLion... lo usan igual).
            std::string style = cppFormatStyle();
            if (style.size() > 2 && style.front() == '{') style = style.substr(1, style.size() - 2);
            std::string yaml = "# Estilo de los scripts de C++ (Cramion: Configuracion del motor)\n";
            std::size_t start = 0;
            while (start < style.size()) {
                std::size_t end = style.find(", ", start);
                if (end == std::string::npos) end = style.size();
                yaml += style.substr(start, end - start) + "\n";
                start = end + 2;
            }
            std::ofstream(clang_format_file, std::ios::binary) << yaml;
            std::cout << "[C++] Escrito " << dialogs::utf8(clang_format_file) << std::endl;
        }
        ImGui::EndDisabled();
        const std::filesystem::path exe = scripting::CppScriptSystem::findClangFormat();
        ImGui::TextDisabled("clang-format: %s", exe.empty() ? "no encontrado (falta toolchain/)" : dialogs::utf8(exe).c_str());

        ImGui::Spacing();
        ImGui::SeparatorText("IntelliSense (clangd)");
        ImGui::Text("Estado: %s", clangd_.running() ? clangd_.status().c_str() : (clangd_.status().empty() ? "sin arrancar (abre un .cpp)" : clangd_.status().c_str()));
        if (ImGui::Button("Reiniciar clangd")) {
            clangd_.stop();
            for (ScriptTab& t : script_tabs_) t.clangd_synced.clear();
            ensureClangd();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(tras cambiar compile_commands.json o el SDK)");

        ImGui::Spacing();
        ImGui::SeparatorText("Compilador");
        std::string kind;
        const std::string compiler = scripting::CppScriptSystem::findCompiler(&kind);
        ImGui::TextWrapped("%s", compiler.empty() ? "No hay compilador de C++" : (kind + ": " + compiler).c_str());
        const auto cvar_bool = [](const char* name, const char* label) {
            if (cvar::CVarBase* v = cvar::Registry::instance().find(name)) {
                bool b = v->toString() == "true" || v->toString() == "1";
                if (ImGui::Checkbox(label, &b)) v->fromString(b ? "true" : "false");
                ImGui::SetItemTooltip("%s", v->description().c_str());
            }
        };
        cvar_bool("script.cpp.Optimize", "Compilar optimizado (-O2)");
        cvar_bool("script.cpp.Enabled", "Ejecutar los scripts de C++ en Play");
        if (ImGui::Button("Abrir todas las variables (CVars)")) show_cvars_window_ = true;
    }
    ImGui::End();
}

}  // namespace cramion::editor
