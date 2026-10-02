// Herramientas de fisica de la 2.1:
//
//   Fracturar (Ventana > Fisica > Fracturar)  parte la malla del objeto
//       seleccionado en trozos de Voronoi y guarda un .crfracture (con niveles
//       y trozos pequenos alrededor de un punto si se quiere). Con un clic le
//       pone el componente Destructible.
//   Asistente de vehiculo  busca las ruedas por su nombre (wheel, rueda,
//       tire, llanta...) en los hijos del objeto, crea sus Wheel Collider
//       (radio sacado de la malla, delanteras con direccion, traccion
//       trasera o 4x4) y le pone Rigidbody, Box Collider y Vehicle.

#include "EditorApp.h"

#include "Dialogs.h"

#include <CramionCore/physics/Destruction.h>
#include <CramionCore/physics/Fracture.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <iostream>

namespace cramion::editor {

// EditorLighting.cpp: la casilla de la ventana Iluminacion.
bool& lightingWindowFlag(LightingEditorState& state);

struct PhysicsToolsState {
    bool show_fracture = false;
    physics::FractureSettings settings;
    bool add_destructible = true;
    std::string status;
    bool show_vehicle = false;
    int drive = 1;  // 0 delantera, 1 trasera, 2 4x4
    float mass = 1200.0f;
    std::string vehicle_status;
};

namespace {

std::string lowerText(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool isWheelName(const std::string& name) {
    const std::string n = lowerText(name);
    for (const char* key : {"wheel", "rueda", "tire", "tyre", "llanta", "neumatico", "rim"}) {
        if (n.find(key) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

PhysicsToolsState& EditorApp::physicsTools() {
    if (!physics_tools_) physics_tools_ = std::make_shared<PhysicsToolsState>();
    return *physics_tools_;
}

void EditorApp::drawFractureWindow() {
    PhysicsToolsState& st = physicsTools();
    if (!st.show_fracture) return;
    ImGui::SetNextWindowSize(ImVec2(400.0f, 420.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Fracturar###fracture_tool", &st.show_fracture)) {
        ImGui::End();
        return;
    }
    ecs::Entity target = world_.find(active_);
    const ecs::MeshRenderer* renderer = target.valid() ? target.tryGet<ecs::MeshRenderer>() : nullptr;
    if (renderer == nullptr) {
        ImGui::TextWrapped("Selecciona un objeto con malla (MeshRenderer) para partirlo en trozos.");
    } else {
        ImGui::Text("Objeto: %s", target.name().c_str());
    }
    physics::FractureSettings& s = st.settings;
    ImGui::DragInt("Trozos", &s.pieces, 0.2f, 2, 256);
    int seed = static_cast<int>(s.seed);
    if (ImGui::DragInt("Semilla", &seed, 0.2f, 0, 1000000)) s.seed = static_cast<std::uint32_t>(seed);
    ImGui::SliderInt("Niveles", &s.levels, 1, 3);
    ImGui::SetItemTooltip("2 o 3: los trozos grandes se vuelven a romper con más golpes");
    if (s.levels > 1) ImGui::DragInt("Trozos por trozo", &s.sub_pieces, 0.1f, 2, 16);
    ImGui::DragFloat("Repetición de la textura interior", &s.interior_uv_scale, 0.01f, 0.01f, 20.0f, "%.2f / m");
    ImGui::Checkbox("Concentrar alrededor de un punto", &s.cluster);
    if (s.cluster) {
        ImGui::DragFloat3("Punto (local)", &s.cluster_point.x, 0.01f);
        ImGui::DragFloat("Radio", &s.cluster_radius, 0.01f, 0.01f, 100.0f, "%.2f m");
    }
    ImGui::Checkbox("Añadir Destructible al objeto", &st.add_destructible);
    ImGui::BeginDisabled(renderer == nullptr);
    if (ImGui::Button("Fracturar", ImVec2(-1.0f, 30.0f))) {
        physics::FractureSourceMesh source;
        bool have = false;
        if (renderer->mesh) {
            source = physics::sourceFromMesh(*renderer->mesh);
            have = true;
        } else if (renderer->model.valid() && asset_manager_) {
            if (const auto model = asset_manager_->loadModel(renderer->model.uuid)) {
                if (renderer->part >= 0 && renderer->part < static_cast<int>(model->parts.size()) &&
                    model->parts[static_cast<std::size_t>(renderer->part)]) {
                    source = physics::sourceFromModel(*model->parts[static_cast<std::size_t>(renderer->part)]);
                    have = true;
                }
            }
        }
        physics::FractureData data;
        std::string error;
        if (!have) {
            st.status = "No se pudo leer la malla del objeto.";
        } else if (!physics::fractureMesh(source, s, data, &error)) {
            st.status = "No salió ningún trozo: " + error;
        } else {
            data.uuid = Uuid::generate();
            data.source_model = renderer->model.uuid;
            data.source_part = renderer->part;
            const std::filesystem::path folder = project_.assetsFolder() / "Fracturas";
            std::error_code ec;
            std::filesystem::create_directories(folder, ec);
            std::filesystem::path path = folder / dialogs::fromUtf8(target.name() + physics::kFractureExtension);
            for (int i = 2; std::filesystem::exists(path); ++i) {
                path = folder / dialogs::fromUtf8(target.name() + " " + std::to_string(i) + physics::kFractureExtension);
            }
            if (physics::saveFracture(path, data, &error)) {
                refreshDatabase();
                st.status = std::to_string(data.pieces.size()) + " trozos (" + std::to_string(data.triangleCount()) + " triángulos) en " +
                            assetRelative(path);
                if (st.add_destructible) {
                    physics::Destructible& d = target.has<physics::Destructible>() ? target.get<physics::Destructible>()
                                                                                   : target.add<physics::Destructible>();
                    d.fracture = assets::AssetRef{data.uuid, assets::AssetType::Fracture};
                    commit();
                }
                pushToast("Fractura creada", st.status, 1);
            } else {
                st.status = "No se pudo guardar: " + error;
            }
        }
    }
    ImGui::EndDisabled();
    if (!st.status.empty()) ImGui::TextWrapped("%s", st.status.c_str());
    ImGui::TextDisabled("En Play: se rompe con golpes fuertes o con self.entity:fracture(punto, fuerza).");
    ImGui::End();
}

void EditorApp::drawVehicleWizard() {
    PhysicsToolsState& st = physicsTools();
    if (!st.show_vehicle) return;
    ImGui::SetNextWindowSize(ImVec2(420.0f, 320.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Asistente de vehículo###vehicle_wizard", &st.show_vehicle)) {
        ImGui::End();
        return;
    }
    ecs::Entity root = world_.find(active_);
    if (!root.valid()) {
        ImGui::TextWrapped("Selecciona la raíz del coche (el objeto que tiene la carrocería y las ruedas como hijos).");
        ImGui::End();
        return;
    }
    // Ruedas por nombre en toda la jerarquia.
    std::vector<ecs::Entity> wheels;
    std::vector<entt::entity> stack(root.children().begin(), root.children().end());
    while (!stack.empty()) {
        const ecs::Entity e = world_.wrap(stack.back());
        stack.pop_back();
        if (isWheelName(e.name()) && !e.has<physics::WheelCollider>()) {
            wheels.push_back(e);
            continue;  // sus hijos son parte de la rueda
        }
        for (const entt::entity c : e.children()) stack.push_back(c);
    }
    ImGui::Text("Coche: %s", root.name().c_str());
    ImGui::Text("Ruedas encontradas: %zu", wheels.size());
    for (const ecs::Entity& w : wheels) ImGui::BulletText("%s", w.name().c_str());
    static const char* kDrive[] = {"Tracción delantera", "Tracción trasera", "4x4"};
    ImGui::Combo("Tracción", &st.drive, kDrive, 3);
    ImGui::DragFloat("Masa (kg)", &st.mass, 10.0f, 100.0f, 50000.0f, "%.0f");
    ImGui::BeginDisabled(wheels.empty());
    if (ImGui::Button("Montar el vehículo", ImVec2(-1.0f, 30.0f))) {
        // Delante: -Z local de la raiz (el frente de los objetos del motor).
        const core::Mat4 inv = core::inverse(root.worldMatrix());
        float min_z = 1e30f, max_z = -1e30f;
        std::vector<core::Vec3> local;
        for (const ecs::Entity& w : wheels) {
            const core::Vec3 p = ecs::transformPoint(inv, w.worldPosition());
            local.push_back(p);
            min_z = std::min(min_z, p.z);
            max_z = std::max(max_z, p.z);
        }
        const float mid = (min_z + max_z) * 0.5f;
        int made = 0;
        for (std::size_t i = 0; i < wheels.size(); ++i) {
            const ecs::Entity w = wheels[i];
            const std::string name = lowerText(w.name());
            bool front = local[i].z < mid;
            if (name.find("front") != std::string::npos || name.find("del") != std::string::npos || name.find("fl") == 0 ||
                name.find("fr") == 0) {
                front = true;
            }
            if (name.find("rear") != std::string::npos || name.find("back") != std::string::npos || name.find("tras") != std::string::npos) {
                front = false;
            }
            // Radio por la caja de la malla de la rueda (si se sabe).
            float radius = 0.38f;
            if (sync_) {
                const int actor = sync_->actorIndex(w);
                core::Vec3 mn, mx;
                if (actor >= 0 && sync_->actorLocalBounds(static_cast<std::uint32_t>(actor), mn, mx)) {
                    radius = std::clamp(std::max(mx.y - mn.y, mx.z - mn.z) * 0.5f, 0.1f, 2.0f);
                }
            }
            ecs::Entity collider = world_.create(w.name() + " (Wheel Collider)", root);
            collider.setWorldPosition(w.worldPosition());
            physics::WheelCollider& wc = collider.add<physics::WheelCollider>();
            wc.radius = radius;
            wc.max_steer_angle = front ? 35.0f : 0.0f;
            wc.drive = st.drive == 2 || (st.drive == 0 && front) || (st.drive == 1 && !front);
            wc.max_handbrake_torque = front ? 0.0f : 3000.0f;
            wc.visual = w.uuid();
            ++made;
        }
        if (!root.has<physics::Rigidbody>()) root.add<physics::Rigidbody>().mass = st.mass;
        else root.get<physics::Rigidbody>().mass = st.mass;
        if (!physics::hasCollider(root)) physics::addDefaultCollider(root);
        if (!root.has<physics::Vehicle>()) root.add<physics::Vehicle>();
        commit();
        st.vehicle_status = std::to_string(made) + " ruedas montadas. Dale a Play: W/S acelerar, A/D girar, Espacio freno de mano.";
        pushToast("Vehículo montado", st.vehicle_status, 1);
    }
    ImGui::EndDisabled();
    if (!st.vehicle_status.empty()) ImGui::TextWrapped("%s", st.vehicle_status.c_str());
    ImGui::End();
}

void EditorApp::drawPhysicsToolWindows() {
    if (!has_project_) return;
    drawFractureWindow();
    drawVehicleWizard();
    drawMotionDatabaseEditor();
    drawMotionMatchingDebug();
    drawLightingWindow();
}

void EditorApp::drawPhysicsToolMenu() {
    PhysicsToolsState& st = physicsTools();
    ImGui::MenuItem("Iluminación (horneado)", nullptr, &lightingWindowFlag(lightingEditor()));
    ImGui::MenuItem("Fracturar (destrucción)", nullptr, &st.show_fracture);
    ImGui::MenuItem("Asistente de vehículo", nullptr, &st.show_vehicle);
}

}  // namespace cramion::editor
