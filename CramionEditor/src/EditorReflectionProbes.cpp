// Sondas de reflexion colocadas en el editor (componente ReflectionProbeBox,
// CramionCore/lighting/ReflectionProbes.h): el estado del horneado y los
// botones Hornear del Inspector, y la herramienta MCP bake_reflection_probes.
// El horneado espera a la GPU (una sonda son seis caras y su prefiltrado: unas
// decenas de ms) y guarda <escena>.crrefl.

#include "EditorApp.h"

#include <CramionCore/lighting/ReflectionProbes.h>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace cramion::editor {

using nlohmann::json;

namespace {

const gfx::BakedReflectionCube* bakedCubeOf(const gfx::VulkanRenderer& renderer, const Uuid& id) {
    const gfx::PlacedProbeId key = lighting::placedProbeId(id);
    for (const gfx::BakedReflectionCube& cube : renderer.bakedReflectionCubes()) {
        if (cube.id == key) return &cube;
    }
    return nullptr;
}

}  // namespace

void EditorApp::drawReflectionProbeInspector(ecs::Entity entity) {
    if (!renderer_.placedProbesSupported()) {
        ImGui::TextDisabled("Esta GPU (o el modo compatible) no usa sondas colocadas.");
        return;
    }
    const gfx::BakedReflectionCube* cube = bakedCubeOf(renderer_, entity.uuid());
    if (cube == nullptr) {
        ImGui::TextDisabled("Sin hornear: no cambia nada hasta hornearla.");
    } else {
        const float moved = core::length(entity.worldPosition() - cube->position);
        if (moved > 0.05f) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Horneada en otro sitio (%.2f m): vuelve a hornearla.",
                               moved);
        } else {
            ImGui::TextDisabled("Horneada (%u px, %u niveles).", cube->size, cube->mips);
        }
    }
    if (!renderer_.placedProbesEnabled()) ImGui::TextDisabled("Apagadas con render.probe.Placed = 0.");

    const auto bake = [&](std::vector<Uuid> only) {
        if (playing()) {
            pushToast("Sal de Play para hornear", "En Play las cosas se mueven: se hornearía a medias", 2);
            return;
        }
        const lighting::ReflectionBakeReport report =
            lighting::bakeReflectionProbes(world_, renderer_, scene_, scene_path_, only);
        if (report.baked > 0 && report.error.empty()) {
            pushToast("Sondas de reflexión horneadas", std::to_string(report.baked) + " en " +
                                                           report.file.filename().string());
        } else if (!report.error.empty()) {
            pushToast("No se pudo hornear", report.error, 2);
        }
    };
    const float width = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button("Hornear esta sonda", ImVec2(width * 0.5f - 4.0f, 0.0f))) bake({entity.uuid()});
    ImGui::SameLine();
    if (ImGui::Button("Hornear todas", ImVec2(-1.0f, 0.0f))) bake({});
}

std::string EditorApp::bakeReflectionProbesJson(const std::string& args_json) {
    const json args = json::parse(args_json, nullptr, false);
    std::vector<Uuid> only;
    if (args.is_object() && args.contains("entity") && args["entity"].is_string()) {
        const std::string text = args["entity"].get<std::string>();
        if (!text.empty()) {
            // UUID o, si no, el nombre (la primera con ese nombre).
            const Uuid id = Uuid::parse(text);
            ecs::Entity target = id.valid() ? world_.find(id) : ecs::Entity{};
            if (!target.valid()) target = world_.findByName(text);
            if (!target.valid()) return json{{"error", "no existe la entidad " + text}}.dump();
            if (target.tryGet<lighting::ReflectionProbeBox>() == nullptr) {
                return json{{"error", "la entidad " + text + " no tiene Reflection Probe"}}.dump();
            }
            only.push_back(target.uuid());
        }
    }
    json out;
    if (playing()) {
        out["error"] = "sal de Play para hornear";
    } else {
        const lighting::ReflectionBakeReport report =
            lighting::bakeReflectionProbes(world_, renderer_, scene_, scene_path_, only);
        out["baked"] = report.baked;
        out["failed"] = report.failed;
        out["total"] = report.total;
        out["file"] = report.file.string();
        out["error"] = report.error;
    }
    out["supported"] = renderer_.placedProbesSupported();
    out["enabled"] = renderer_.placedProbesEnabled();
    out["active_last_frame"] = renderer_.activePlacedProbes();
    json probes = json::array();
    for (const entt::entity h : world_.registry().view<lighting::ReflectionProbeBox>()) {
        const ecs::Entity e = world_.wrap(h);
        const lighting::ReflectionProbeBox& p = e.get<lighting::ReflectionProbeBox>();
        const core::Vec3 position = e.worldPosition();
        const gfx::BakedReflectionCube* cube = bakedCubeOf(renderer_, e.uuid());
        probes.push_back(json{{"entity", e.uuid().toString()},
                              {"name", e.name()},
                              {"position", {position.x, position.y, position.z}},
                              {"size", {p.size.x, p.size.y, p.size.z}},
                              {"blend_distance", p.blend_distance},
                              {"priority", p.priority},
                              {"intensity", p.intensity},
                              {"box_projection", p.box_projection},
                              {"baked", cube != nullptr},
                              {"moved_since_bake", cube != nullptr ? core::length(position - cube->position) : 0.0f}});
    }
    out["probes"] = probes;
    return out.dump();
}

}  // namespace cramion::editor
