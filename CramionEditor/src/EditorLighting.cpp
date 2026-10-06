// Ventana > Iluminación (como Lighting de Unity): modo de la luz rebotada
// (tiempo real o horneada), ajustes del horneado y el boton Generar. El
// horneado corre en otro hilo (barra de progreso, se puede cancelar) y el
// resultado se guarda junto a la escena (<escena>.crbake); se carga solo al
// abrirla, en el editor y en el juego exportado.

#include "EditorApp.h"

#include <CramionCore/cvar/CVar.h>

#include "Dialogs.h"

#include <CramionCore/lighting/ProbeBaker.h>
#include <CramionCore/terrain/Terrain.h>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <iostream>
#include <thread>

namespace cramion::editor {

struct LightingEditorState {
    bool show = false;
    lighting::BakeSettings settings;
    float auto_spacing = 2.0f;
    // Horneado en marcha
    std::thread thread;
    std::shared_ptr<lighting::BakeProgress> progress;
    std::atomic<bool> finished{false};
    bool running = false;
    bool ok = false;
    std::string error;
    gfx::BakedLighting result;
    lighting::BakeStats stats;
    std::filesystem::path target;
    bool auto_volume = false;

    ~LightingEditorState() {
        if (progress) progress->cancel = true;
        if (thread.joinable()) thread.join();
    }
};

bool& lightingWindowFlag(LightingEditorState& state) { return state.show; }

LightingEditorState& EditorApp::lightingEditor() {
    if (!lighting_editor_) lighting_editor_ = std::make_shared<LightingEditorState>();
    return *lighting_editor_;
}

void EditorApp::loadSceneLighting() {
    gfx::BakedLighting data;
    gfx::LightingMode mode = gfx::LightingMode::Realtime;
    if (!scene_path_.empty()) {
        const std::filesystem::path file = lighting::bakedLightingFile(scene_path_);
        std::string error;
        if (std::filesystem::exists(file) && !lighting::loadBakedLighting(file, data, mode, &error)) {
            std::cerr << "[Iluminacion] " << error << "\n";
        }
    }
    renderer_.setBakedLighting(std::move(data));
    renderer_.setLightingMode(mode);
}

void EditorApp::startLightingBake() {
    LightingEditorState& st = lightingEditor();
    if (st.running) return;
    if (scene_path_.empty()) {
        pushToast("Guarda la escena primero", "La iluminación se guarda junto a la escena (.crbake)", 2);
        return;
    }
    if (!asset_manager_) return;
    auto scene = std::make_shared<lighting::BakeScene>();
    // El terreno, con las formas de la fisica (en el editor existen siempre).
    lighting::gatherBakeScene(world_, *asset_manager_, *scene, [this](std::vector<lighting::BakeTriangle>& tris) {
        for (const entt::entity h : world_.registry().view<terrain::Terrain>()) {
            const ecs::Entity e = world_.wrap(h);
            if (!e.activeInHierarchy()) continue;
            std::vector<core::Vec3> points;
            if (!physics_.bodyTriangles(e, points, 400000)) continue;
            for (std::size_t i = 0; i + 2 < points.size(); i += 3) {
                tris.push_back(lighting::BakeTriangle{points[i], points[i + 1], points[i + 2], core::Vec3{0.18f, 0.2f, 0.12f}, {}});
            }
        }
    });
    scene->lights = scene_.lights();
    scene->settings = st.settings;
    st.auto_volume = scene->volumes.empty();
    if (scene->volumes.empty()) scene->volumes.push_back(lighting::volumeAround(scene->triangles, st.auto_spacing));
    if (scene->triangles.empty()) {
        pushToast("Nada que hornear", "La escena no tiene mallas estáticas", 2);
        return;
    }
    st.progress = std::make_shared<lighting::BakeProgress>();
    st.finished = false;
    st.running = true;
    st.ok = false;
    st.error.clear();
    st.target = lighting::bakedLightingFile(scene_path_);
    if (st.thread.joinable()) st.thread.join();
    LightingEditorState* state = &st;
    std::shared_ptr<lighting::BakeProgress> progress = st.progress;
    st.thread = std::thread([state, scene, progress] {
        gfx::BakedLighting out;
        lighting::BakeStats stats;
        std::string error;
        const bool ok = lighting::bakeProbes(*scene, out, progress.get(), &stats, &error);
        state->result = std::move(out);
        state->stats = stats;
        state->error = error;
        state->ok = ok;
        state->finished = true;
    });
}

void EditorApp::drawLightingWindow() {
    LightingEditorState& st = lightingEditor();
    // Termino un horneado: guardar y aplicar.
    if (st.running && st.finished.load()) {
        if (st.thread.joinable()) st.thread.join();
        st.running = false;
        if (st.ok) {
            std::string error;
            if (lighting::saveBakedLighting(st.target, st.result, gfx::LightingMode::Baked, &error)) {
                renderer_.setBakedLighting(st.result);
                renderer_.setLightingMode(gfx::LightingMode::Baked);
                refreshDatabase();
                char text[160];
                std::snprintf(text, sizeof(text), "%zu sondas, %zu triángulos, %.1f s%s", st.stats.probes, st.stats.triangles,
                              st.stats.seconds, st.auto_volume ? " (volumen automático)" : "");
                pushToast("Iluminación horneada", text, 1);
            } else {
                pushToast("No se pudo guardar la iluminación", error, 3);
            }
        } else if (st.error != "cancelado") {
            pushToast("El horneado falló", st.error, 3);
        }
        st.result = gfx::BakedLighting{};
    }
    if (!st.show) return;
    ImGui::SetNextWindowSize(ImVec2(440.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Iluminación###lighting_window", &st.show)) {
        ImGui::End();
        return;
    }
    ImGui::SeparatorText("Luz rebotada (GI)");
    int mode = static_cast<int>(renderer_.lightingMode());
    static const char* kModes[] = {"Tiempo real (SSGI / rayos)", "Horneada (sondas)"};
    if (ImGui::Combo("Modo", &mode, kModes, 2)) {
        renderer_.setLightingMode(static_cast<gfx::LightingMode>(mode));
        // Se guarda con el horneado (si lo hay).
        if (!scene_path_.empty() && !renderer_.bakedLighting().empty()) {
            lighting::saveBakedLighting(lighting::bakedLightingFile(scene_path_), renderer_.bakedLighting(),
                                        static_cast<gfx::LightingMode>(mode));
        }
    }
    ImGui::SetItemTooltip("Horneada: casi gratis en el juego (Android, PCs sin rayos). Tiempo real: se adapta a\n"
                          "lo que cambia, pero cuesta mucho más.");
    if (mode == static_cast<int>(gfx::LightingMode::Baked)) {
        cvar::Registry& reg = cvar::Registry::instance();
        bool dynamic = reg.execute("render.gi.DynamicProbes").find("true") != std::string::npos;
        if (ImGui::Checkbox("Sondas dinámicas (DDGI, con trazado de rayos)", &dynamic)) {
            reg.execute(std::string("render.gi.DynamicProbes ") + (dynamic ? "true" : "false"));
        }
        ImGui::SetItemTooltip("Con trazado de rayos las sondas se rehacen solas unas pocas por frame: siguen a la hora\n"
                              "del día, a las luces que se mueven y a lo que cambia. Sin rayos, se quedan horneadas.");
        if (dynamic && !renderer_.rayTracingActive()) ImGui::TextDisabled("(activa el trazado de rayos para que se actualicen)");
    }
    const gfx::BakedLighting& baked = renderer_.bakedLighting();
    if (baked.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "Esta escena no tiene iluminación horneada.");
    } else {
        ImGui::TextDisabled("%zu volúmenes, %zu sondas (%.1f MB)", baked.volumes.size(), baked.probes.size() / gfx::kBakedProbeVec4,
                            static_cast<double>(baked.probes.size() * sizeof(core::Vec4)) / (1024.0 * 1024.0));
        if (!baked.surface.empty()) {
            ImGui::TextDisabled("Lightmap de superficie: %u texeles de %.2f m (%.1f MB)", baked.surface.count,
                                baked.surface.cell_size,
                                static_cast<double>(baked.surface.texels.size() * sizeof(core::Vec4) +
                                                    baked.surface.keys.size() * 4) / (1024.0 * 1024.0));
        }
        if (renderer_.lightingMode() == gfx::LightingMode::Baked && !renderer_.bakedGiActive()) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "Los datos no son válidos: vuelve a hornear.");
        }
    }

    ImGui::SeparatorText("Horneado");
    ImGui::BeginDisabled(st.running);
    ImGui::DragInt("Rayos por sonda", &st.settings.rays, 4.0f, 16, 4096);
    ImGui::SetItemTooltip("Más = menos ruido (y más tiempo)");
    ImGui::SliderInt("Rebotes", &st.settings.bounces, 0, 6);
    ImGui::SetItemTooltip("Rebotes además del primero (0 = un rebote)");
    ImGui::DragFloat("Intensidad del cielo", &st.settings.sky_intensity, 0.01f, 0.0f, 10.0f, "%.2f");
    ImGui::DragFloat("Separación automática (m)", &st.auto_spacing, 0.05f, 0.5f, 20.0f, "%.2f");
    ImGui::SetItemTooltip("Si la escena no tiene Light Probe Volume, uno que cubre todo con esta separación");
    {
        bool surface = st.settings.surface_texel > 0.0f;
        if (ImGui::Checkbox("Lightmap de superficie (texeles en el mundo)", &surface)) {
            st.settings.surface_texel = surface ? 0.5f : 0.0f;
        }
        ImGui::SetItemTooltip("Además de las sondas, un texel de luz rebotada cada pocos centímetros pegado a toda la\n"
                              "geometría: el detalle de un lightmap (rincones, bajo las mesas, junto a paredes) sin\n"
                              "UV2, también en el terreno, el follaje y lo que se mueve.");
        if (surface) {
            ImGui::DragFloat("Tamaño del texel (m)", &st.settings.surface_texel, 0.01f, 0.1f, 4.0f, "%.2f");
            ImGui::DragInt("Rayos por texel", &st.settings.surface_rays, 2.0f, 16, 1024);
        }
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped("Aportan luz los objetos marcados Static (o, si no hay ninguno, las mallas que no caen con "
                       "física) y el terreno. Las sondas se ponen con el componente Light Probe Volume.");
    if (ImGui::Button("+ Light Probe Volume")) {
        ecs::Entity e = world_.create("Light Probe Volume");
        const scene::Camera& camera = scene_.camera();
        e.setLocalPosition(camera.position() + camera.forward() * 10.0f);
        e.add<lighting::LightProbeVolume>();
        selectOnly(e.uuid());
        commit();
    }
    ImGui::Separator();
    if (st.running) {
        const float f = st.progress ? st.progress->fraction.load() : 0.0f;
        ImGui::ProgressBar(f, ImVec2(-1.0f, 0.0f));
        ImGui::TextDisabled("%s", st.progress ? st.progress->currentStage().c_str() : "");
        if (ImGui::Button("Cancelar", ImVec2(-1.0f, 0.0f)) && st.progress) st.progress->cancel = true;
    } else {
        if (ImGui::Button("Generar iluminación", ImVec2(-1.0f, 32.0f))) startLightingBake();
        ImGui::BeginDisabled(baked.empty());
        if (ImGui::Button("Borrar la iluminación horneada", ImVec2(-1.0f, 0.0f))) {
            std::error_code ec;
            if (!scene_path_.empty()) std::filesystem::remove(lighting::bakedLightingFile(scene_path_), ec);
            renderer_.setBakedLighting({});
            renderer_.setLightingMode(gfx::LightingMode::Realtime);
            refreshDatabase();
        }
        ImGui::EndDisabled();
    }
    ImGui::End();
}

}  // namespace cramion::editor

namespace cramion::editor {

void EditorApp::configureLightingBake(int rays, int bounces, float spacing) {
    LightingEditorState& st = lightingEditor();
    if (rays > 0) st.settings.rays = rays;
    if (bounces >= 0) st.settings.bounces = bounces;
    if (spacing > 0.0f) st.auto_spacing = spacing;
}

std::string EditorApp::lightingStateJson() {
    LightingEditorState& st = lightingEditor();
    const gfx::BakedLighting& baked = renderer_.bakedLighting();
    nlohmann::json out{{"baking", st.running},
                       {"progress", st.progress ? st.progress->fraction.load() : 0.0f},
                       {"mode", renderer_.lightingMode() == gfx::LightingMode::Baked ? "baked" : "realtime"},
                       {"active", renderer_.bakedGiActive()},
                       {"volumes", baked.volumes.size()},
                       {"probes", baked.probes.size() / gfx::kBakedProbeVec4},
                       {"last_error", st.error}};
    return out.dump();
}

}  // namespace cramion::editor
