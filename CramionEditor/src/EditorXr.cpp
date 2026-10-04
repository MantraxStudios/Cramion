// Realidad virtual en el editor: GameObject > Realidad virtual > XR Origin y
// el boton Play on VR (al lado de Play). El casco se conecta al dar Play on VR
// y se suelta al parar, como Unity; Editar > Runtime de OpenXR elige con que
// runtime (SteamVR, Meta...).

#include "EditorApp.h"
#include "LoadingScreen.h"

#include <CramionCore/ecs/ModelInstantiation.h>
#include <CramionCore/physics/PhysicsComponents.h>
#include <CramionCore/xr/XrRig.h>
#include <CramionUpdater/Update.h>

#include <imgui.h>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>

namespace cramion::editor {

using core::Vec3;

namespace {

std::filesystem::path xrPreferenceFile() { return localDataFolder("Settings") / "EditorVR.ini"; }

struct XrPreferences {
    bool vr_play = false;          // preparar Vulkan para el casco al abrir
    std::string runtime = "auto";  // xr::runtimeChoiceKey
    bool mono = false;             // true: una sola imagen para los dos ojos (sin estereo)
};

XrPreferences readXrPreferences() {
    XrPreferences prefs;
    std::ifstream in(xrPreferenceFile());
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("vr_play=", 0) == 0) prefs.vr_play = line.substr(8, 1) == "1";
        if (line.rfind("runtime=", 0) == 0) prefs.runtime = line.substr(8);
        if (line.rfind("mono=", 0) == 0) prefs.mono = line.substr(5, 1) == "1";
    }
    return prefs;
}

void writeXrPreferences(const XrPreferences& prefs) {
    std::ofstream out(xrPreferenceFile());
    out << "vr_play=" << (prefs.vr_play ? 1 : 0) << "\n";
    out << "runtime=" << prefs.runtime << "\n";
    out << "mono=" << (prefs.mono ? 1 : 0) << "\n";
}

// Que va a probar cada opcion (para los tooltips).
std::string runtimeChoiceHelp(xr::RuntimeChoice choice) {
    switch (choice) {
        case xr::RuntimeChoice::SteamVR:
            return "SteamVR (Steam Link, Virtual Desktop, Index, Vive, WMR por SteamVR...).\nSi esta cerrado, se abre y "
                   "se espera al casco.";
        case xr::RuntimeChoice::Meta:
            return "El runtime de Meta: el Quest por Quest Link (cable) o Air Link.";
        case xr::RuntimeChoice::System:
            return "El runtime activo de Windows (el ultimo que lo pidio: SteamVR, Meta...).\n"
                   "Lo que haria cualquier juego de OpenXR.";
        default:
            return "SteamVR si esta abierto; si no, el runtime activo de Windows; si ese no ve el casco, Meta.\n"
                   "Un Quest por Steam Link o Virtual Desktop esta en SteamVR aunque el activo sea el de Meta.";
    }
}

}  // namespace

bool xrPlayPreference() { return readXrPreferences().vr_play; }

void setXrPlayPreference(bool on) {
    XrPreferences prefs = readXrPreferences();
    prefs.vr_play = on;
    writeXrPreferences(prefs);
}

int xrRuntimePreference() { return static_cast<int>(xr::runtimeChoiceFromKey(readXrPreferences().runtime)); }

void setXrRuntimePreference(int choice) {
    XrPreferences prefs = readXrPreferences();
    prefs.runtime = xr::runtimeChoiceKey(static_cast<xr::RuntimeChoice>(choice));
    writeXrPreferences(prefs);
}

bool xrStereoPreference() { return !readXrPreferences().mono; }

void setXrStereoPreference(bool on) {
    XrPreferences prefs = readXrPreferences();
    prefs.mono = !on;
    writeXrPreferences(prefs);
}

// XR Origin en el suelo delante de la camara del editor, con la camara
// principal a la altura de los ojos y un mando en cada mano (un cubo pequeno
// para verlos; se cambia por el modelo que se quiera).
ecs::Entity EditorApp::createXrOrigin(ecs::Entity parent) {
    if (!parent.valid()) parent = prefabStageRoot();
    ecs::Entity origin = world_.create("XR Origin", parent);
    origin.add<xr::XrOrigin>();
    if (!parent.valid()) {
        const scene::Camera& view = scene_.camera();
        Vec3 at = view.position() + view.forward() * 4.0f;
        at.y = 0.0f;
        origin.setWorldPosition(at);
    }
    // Una sola camara principal: la del rig.
    world_.forEachDepthFirst([&](ecs::Entity e) {
        if (ecs::Camera* c = e.tryGet<ecs::Camera>(); c != nullptr && !c->target_texture.valid()) c->is_main = false;
    });
    ecs::Entity camera = ecs::createCamera(world_, origin);
    camera.setName("Main Camera");
    camera.setLocalPosition(Vec3{0.0f, 1.6f, 0.0f});
    camera.get<ecs::Camera>().is_main = true;
    camera.get<ecs::Camera>().near_plane = 0.05f;
    static constexpr const char* kNames[] = {"Mano izquierda", "Mano derecha"};
    for (int h = 0; h < 2; ++h) {
        ecs::Entity hand = world_.create(kNames[h], origin);
        hand.setLocalPosition(Vec3{h == 0 ? -0.25f : 0.25f, 1.1f, -0.3f});
        xr::XrController& controller = hand.add<xr::XrController>();
        controller.hand = static_cast<xr::Hand>(h);
        hand.add<xr::XrInteractor>();  // coger (XR Grabbable) y el rayo de la UI en el mundo
        ecs::Entity model = ecs::createPrimitive(world_, assets::builtin::kCube, "Modelo", hand);
        model.setLocalScale(Vec3{0.05f, 0.05f, 0.12f});
    }
    selectOnly(origin.uuid());
    revealInHierarchy(origin.uuid());
    commit();
    return origin;
}

// Jugador VR en primera persona: el XR Origin con su capsula (Character
// Controller integrado, sin teclado ni giro propio) y el XrPlayer.
ecs::Entity EditorApp::createXrPlayer(ecs::Entity parent) {
    ecs::Entity origin = createXrOrigin(parent);
    origin.setName("Jugador VR");
    physics::CharacterController& cc = origin.add<physics::CharacterController>();
    cc.height = 1.8f;
    cc.radius = 0.25f;
    cc.center = Vec3{0.0f, 0.9f, 0.0f};
    cc.crouch_height = 1.1f;
    cc.keyboard = false;
    cc.rotate_to_movement = false;
    cc.walk_speed = 2.5f;  // en VR mas despacio marea menos
    cc.run_speed = 4.5f;
    cc.crouch_speed = 1.5f;
    cc.jump_height = 0.8f;
    origin.add<xr::XrPlayer>();
    commit();
    return origin;
}

// Editar > Runtime de OpenXR y preparar Vulkan para el casco al abrir.
void EditorApp::drawXrMenu() {
    ImGui::BeginDisabled(xrConnecting() || (playing() && play_vr_));
    if (ImGui::BeginMenu("Runtime de OpenXR (VR)")) {
        for (int i = 0; i < 4; ++i) {
            const auto choice = static_cast<xr::RuntimeChoice>(i);
            std::string label = xr::runtimeChoiceLabel(choice);
            if (choice == xr::RuntimeChoice::Auto) label += " (recomendado)";
            if (choice == xr::RuntimeChoice::System) {
                const std::string active = xr::XrSystem::systemRuntimeLabel();
                label += active.empty() ? " (ninguno)" : " (" + active + ")";
            }
            if (ImGui::MenuItem(label.c_str(), nullptr, xr_runtime_preference_ == i)) {
                xr_runtime_preference_ = i;
                setXrRuntimePreference(i);
                std::cout << "[VR] Runtime de OpenXR: " << xr::runtimeChoiceLabel(choice) << std::endl;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", runtimeChoiceHelp(choice).c_str());
        }
        ImGui::EndMenu();
    }
    ImGui::EndDisabled();
    bool on = xr_play_preference_;
    if (ImGui::MenuItem("Preparar Vulkan para el casco al abrir", nullptr, &on)) {
        xr_play_preference_ = on;
        setXrPlayPreference(on);
        std::cout << "[VR] Preparar Vulkan para el casco al abrir: " << (on ? "si" : "no")
                  << " (se aplica al volver a abrir el editor)" << std::endl;
    }
    if (ImGui::IsItemHovered()) {
        std::string tip = "Play on VR conecta el casco sin reabrir el editor. Esto solo hace falta si te lo pide:\n"
                          "el casco en otra GPU (portatiles con dos GPU) o extensiones de Vulkan que faltan.\n"
                          "Se aplica al volver a abrir el editor.\n\n";
        if (xrConnecting()) {
            tip += "Buscando el casco...";
        } else if (renderer_.xrAvailable()) {
            tip += "Casco: " + renderer_.xr().systemName() + " (" + renderer_.xr().runtimeName() + ")";
        } else if (!xr::XrSystem::compiled()) {
            tip += "Este editor se compilo sin OpenXR.";
        } else if (!xr_status_.empty()) {
            tip += "Ultimo intento:\n" + xr_status_;
        } else {
            tip += "Sin casco conectado ahora.";
        }
        ImGui::SetTooltip("%s", tip.c_str());
    }
    bool stereo = xr_stereo_preference_;
    if (ImGui::MenuItem("VR: una camara por ojo (estereo)", nullptr, &stereo)) {
        xr_stereo_preference_ = stereo;
        setXrStereoPreference(stereo);
        std::cout << "[VR] " << (stereo ? "Una camara por ojo (estereo)" : "Una sola imagen para los dos ojos")
                  << std::endl;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Activado (recomendado): una camara por ojo, como Unreal. Cada ojo se dibuja desde su sitio\n"
                          "con todo lo del motor (TAA con historia por ojo, trazado de rayos, SSR, GI, post-proceso)\n"
                          "a la resolucion del casco: profundidad y tamano reales.\n\n"
                          "Desactivado: una sola imagen para los dos ojos. Cuesta la mitad, pero sin profundidad\n"
                          "todo parece lejano y gigante.");
    }
}

// Play en el casco (boton Play on VR). El Play normal no toca el casco.
void EditorApp::enterPlayVr() {
    if (playing() || xrConnecting()) return;
    if (!renderer_.xrAvailable()) {
        startXrConnect(/*then_play=*/true);  // al conectar, Play (finishXrConnect)
        return;
    }
    play_vr_ = true;
    enterPlay();
    if (!playing() && !pending_play_) play_vr_ = false;  // no se pudo (sin proyecto...)
}

// Busca el casco en otro hilo con el runtime elegido (crear la instancia
// puede abrir SteamVR y esperar a que vea el casco: segundos).
void EditorApp::startXrConnect(bool then_play) {
    if (xrConnecting() || renderer_.xrAvailable() || !xr::XrSystem::compiled()) return;
    xr_status_.clear();
    xr_status_needs_restart_ = false;
    xr_connect_then_play_ = then_play;
    const auto choice = static_cast<xr::RuntimeChoice>(xr_runtime_preference_);
    const std::string app_name = has_project_ && !project_.name.empty() ? "Cramion - " + project_.name : renderer_.xrAppName();
    xr::XrSystem* system = &renderer_.xr();
    std::cout << "[VR] Buscando el casco (" << xr::runtimeChoiceLabel(choice) << ")..." << std::endl;
    xr_connect_ = std::async(std::launch::async, [system, choice, app_name] { return system->createInstance(app_name.c_str(), choice); });
}

// La busqueda termino: sesion con el Vulkan del editor y, si se pidio, Play.
void EditorApp::finishXrConnect() {
    const bool found = xr_connect_.get();
    std::string error;
    bool needs_restart = false;
    if (found && renderer_.connectXrSession(&error, &needs_restart)) {
        if (xr_connect_then_play_ && !playing()) {
            play_vr_ = true;
            enterPlay();
            if (!playing() && !pending_play_) play_vr_ = false;
        }
    } else {
        xr_status_ = found ? error : renderer_.xr().error();
        xr_status_needs_restart_ = needs_restart;
        xr_status_popup_ = true;
        renderer_.stopXr();  // nada a medias
    }
    xr_connect_then_play_ = false;
}

void EditorApp::shutdownXr() {
    if (xr_connect_.valid()) xr_connect_.wait();
    xr_connect_ = {};
    renderer_.stopXr();
}

// Vulkan se crea en la GPU y con las extensiones que pide el casco: si el
// editor se abrio sin ellas, hay que reabrirlo. Se guarda todo, se activa la
// preferencia y se abre el editor otra vez con el proyecto y --play-vr.
bool EditorApp::restartForVr() {
    std::string error;
    if (!saveEverythingForUpdate(&error)) {
        std::cerr << "[VR] No se pudo guardar todo: " << error << ". No se reabre." << std::endl;
        return false;
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring args = L"--play-vr";
    if (has_project_) args = update::quoteArgument(project_.file.wstring()) + L" " + args;
    const bool was_on = xr_play_preference_;
    setXrPlayPreference(true);
    if (update::launch(std::filesystem::path(exe), args) == 0) {
        setXrPlayPreference(was_on);
        std::cerr << "[VR] No se pudo reabrir el editor" << std::endl;
        return false;
    }
    xr_play_preference_ = true;
    std::cout << "[VR] Reabriendo el editor con el casco" << std::endl;
    quit_ = true;
    return true;
}

// Al lado de Play: "Play on VR" (o "Stop VR" mientras se juega en el casco).
void EditorApp::drawPlayVrButton(float width) {
    const bool vr_playing = playing() && play_vr_;
    const bool connecting = xrConnecting();
    const bool compiled = xr::XrSystem::compiled();
    const auto choice = static_cast<xr::RuntimeChoice>(xr_runtime_preference_);
    ImGui::BeginDisabled(!compiled || connecting || (playing() && !play_vr_));
    if (vr_playing) {
        const ImVec4 color = theme::vec(theme::kYellow);
        ImGui::PushStyleColor(ImGuiCol_Button, color);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(std::min(color.x * 1.08f, 1.0f), std::min(color.y * 1.08f, 1.0f),
                                                             std::min(color.z * 1.08f, 1.0f), 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(14, 14, 16, 255));
    }
    const char* label = vr_playing ? "Stop VR" : connecting ? "Conectando..." : "Play on VR";
    const bool pressed = ImGui::Button(label, ImVec2(width, 0.0f));
    if (vr_playing) ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        std::string tip;
        if (!compiled) {
            tip = "Este editor se compilo sin OpenXR.";
        } else if (vr_playing) {
            tip = "Parar y restaurar la escena (suelta el casco)\nCasco: " + renderer_.xr().systemName() + " (" +
                  renderer_.xr().runtimeName() + ")";
        } else if (connecting) {
            tip = std::string("Buscando el casco (") + xr::runtimeChoiceLabel(choice) +
                  ")...\nSi SteamVR se esta abriendo, puede tardar unos segundos.";
        } else {
            tip = std::string("Jugar en el casco. Runtime: ") + xr::runtimeChoiceLabel(choice) +
                  " (Editar > Runtime de OpenXR)\n" + runtimeChoiceHelp(choice);
            if (!xr_status_.empty()) tip += "\n\nUltimo intento:\n" + xr_status_;
        }
        ImGui::SetTooltip("%s", tip.c_str());
    }
    if (pressed) {
        if (vr_playing) {
            exitPlay();
        } else {
            enterPlayVr();
        }
    }
    drawXrStatusPopup();
}

// Por que no se pudo conectar, con lo que se puede hacer.
void EditorApp::drawXrStatusPopup() {
    if (xr_status_popup_) {
        ImGui::OpenPopup("Play on VR##estado");
        xr_status_popup_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f));
    if (!ImGui::BeginPopupModal("Play on VR##estado", nullptr, ImGuiWindowFlags_NoResize)) return;
    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.4f, 1.0f), "No se pudo conectar el casco de VR");
    ImGui::Spacing();
    ImGui::TextWrapped("%s", xr_status_.c_str());
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Runtime de OpenXR:");
    for (int i = 0; i < 4; ++i) {
        const auto choice = static_cast<xr::RuntimeChoice>(i);
        if (i > 0) ImGui::SameLine();
        if (ImGui::RadioButton(xr::runtimeChoiceLabel(choice), xr_runtime_preference_ == i)) {
            xr_runtime_preference_ = i;
            setXrRuntimePreference(i);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", runtimeChoiceHelp(choice).c_str());
    }
    ImGui::Spacing();
    if (ImGui::Button("Reintentar", ImVec2(120.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
        enterPlayVr();
    }
    if (xr_status_needs_restart_) {
        ImGui::SameLine();
        if (ImGui::Button("Guardar y reabrir con el casco", ImVec2(230.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
            restartForVr();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cerrar", ImVec2(100.0f, 0.0f))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// Antes de la logica: espera al casco, lee cabeza y mandos, y sus botones a
// la entrada del juego. Con sesion, el casco recibe un frame en cada frame
// del editor (vacio si no hay que dibujar): si no se leen sus eventos ni se
// le entregan frames, SteamVR da la app por colgada.
void EditorApp::beginXrFrame(dm::Input& input) {
    xr_frame_ = false;
    // Buscando el casco en otro hilo: nadie toca el XrSystem hasta que acabe.
    if (xr_connect_.valid()) {
        if (xr_connect_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            xr_rig_.reset();
            return;
        }
        finishXrConnect();
    }
    // Reabierto con --play-vr: Play en el casco en cuanto el proyecto carga.
    if (pending_vr_play_ && has_project_ && !projectLoading() && !playing()) {
        pending_vr_play_ = false;
        enterPlayVr();
        if (xr_connect_.valid()) {
            xr_rig_.reset();
            return;
        }
    }
    if (!renderer_.xrAvailable()) {
        xr_rig_.reset();
        return;
    }
    // El casco es del editor solo mientras se juega en VR: al parar se suelta
    // (SteamVR o Meta vuelven a su casa y otras apps lo pueden usar).
    if (!play_vr_ && !pending_vr_play_) {
        renderer_.stopXr();
        xr_rig_.reset();
        return;
    }
    renderer_.xr().setStereo(xr_stereo_preference_);
    renderer_.xr().beginFrame();
    xr_frame_ = true;
    if (!playing()) {
        xr_rig_.reset();  // Play en VR a punto de empezar: frame sin capas
        return;
    }
    renderer_.xr().applyToInput(input);
    renderer_.xr().applyToInput(touch_sim_input_);
    if (renderer_.xr().exitRequested()) quit_play_requested_ = true;  // se cerro desde el casco
    if (play_state_ == PlayState::Paused) return;
    xr_rig_.setPhysics(&physics_);
    xr_rig_.update(world_, renderer_.xr());
}

// Despues de sincronizar el mundo: los dos ojos (la vista del editor no se
// toca: la ventana sigue con su camara).
void EditorApp::renderXrEyes() {
    if (!xr_frame_ || !xr_rig_.active()) return;
    const scene::Camera keep = scene_.camera();
    xr_rig_.renderEyes(renderer_, scene_);
    scene_.camera() = keep;
}

void EditorApp::endXrFrame() {
    if (!xr_frame_) return;
    renderer_.xr().endFrame();
    xr_frame_ = false;
}

}  // namespace cramion::editor
