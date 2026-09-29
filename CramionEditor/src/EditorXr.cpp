// Realidad virtual en el editor: GameObject > Realidad virtual > XR Origin y
// Play en el casco (Editar > Play en realidad virtual; se aplica al abrir el
// editor, porque Vulkan se crea en la GPU que pide el casco).

#include "EditorApp.h"
#include "LoadingScreen.h"

#include <CramionCore/ecs/ModelInstantiation.h>
#include <CramionCore/xr/XrRig.h>

#include <imgui.h>

#include <fstream>
#include <iostream>
#include <string>

namespace cramion::editor {

using core::Vec3;

namespace {

std::filesystem::path xrPreferenceFile() { return localDataFolder("Settings") / "EditorVR.ini"; }

}  // namespace

bool xrPlayPreference() {
    std::ifstream in(xrPreferenceFile());
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("vr_play=", 0) == 0) return line.substr(8, 1) == "1";
    }
    return false;
}

void setXrPlayPreference(bool on) {
    std::ofstream out(xrPreferenceFile());
    out << "vr_play=" << (on ? 1 : 0) << "\n";
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
        ecs::Entity model = ecs::createPrimitive(world_, assets::builtin::kCube, "Modelo", hand);
        model.setLocalScale(Vec3{0.05f, 0.05f, 0.12f});
    }
    selectOnly(origin.uuid());
    revealInHierarchy(origin.uuid());
    commit();
    return origin;
}

void EditorApp::drawXrMenu() {
    bool on = xr_play_preference_;
    if (ImGui::MenuItem("Play en realidad virtual (OpenXR)", nullptr, &on)) {
        xr_play_preference_ = on;
        setXrPlayPreference(on);
        std::cout << "[VR] Play en realidad virtual " << (on ? "activado" : "desactivado")
                  << ": se aplica al volver a abrir el editor" << std::endl;
    }
    if (ImGui::IsItemHovered()) {
        std::string tip = "En Play el juego se ve en el casco (SteamVR, Meta Quest Link, WMR...).\n"
                          "Se aplica al volver a abrir el editor.\n\n";
        if (renderer_.xrAvailable()) {
            tip += "Casco: " + renderer_.xr().systemName() + " (" + renderer_.xr().runtimeName() + ")";
        } else if (!xr::XrSystem::compiled()) {
            tip += "Este editor se compilo sin OpenXR.";
        } else if (xr_play_preference_ && !renderer_.xr().error().empty()) {
            tip += "Sin casco: " + renderer_.xr().error();
        } else {
            tip += "Sin sesion de VR ahora.";
        }
        ImGui::SetTooltip("%s", tip.c_str());
    }
}

// Antes de la logica: espera al casco, lee cabeza y mandos, y sus botones a
// la entrada del juego. Fuera de Play no se toca el casco.
void EditorApp::beginXrFrame(dm::Input& input) {
    xr_frame_ = false;
    if (!renderer_.xrAvailable() || !playing()) {
        xr_rig_.reset();
        return;
    }
    renderer_.xr().beginFrame();
    xr_frame_ = true;
    renderer_.xr().applyToInput(input);
    renderer_.xr().applyToInput(touch_sim_input_);
    if (play_state_ == PlayState::Paused) return;
    xr_rig_.update(world_, renderer_.xr());
    if (renderer_.xr().exitRequested()) quit_play_requested_ = true;
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
