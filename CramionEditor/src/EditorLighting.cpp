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
#include <CramionCore/terrain/TerrainGrid.h>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <new>
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
    std::filesystem::path scene;  // la escena horneada (si se cambia de escena, no se aplica a la otra)
    bool auto_volume = false;
    // Modo del sol si la escena no tiene Light direccional (LightBakeMode).
    int sun_mode = static_cast<int>(ecs::LightBakeMode::Mixed);
    // Lo que entro en el ultimo horneado y por que no lo demas.
    std::string report_text;

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
    loadSceneLighting(scene_path_);
}

void EditorApp::loadSceneLighting(const std::filesystem::path& scene) {
    gfx::BakedLighting data;
    gfx::LightingMode mode = gfx::LightingMode::Realtime;
    if (!scene.empty()) {
        const std::filesystem::path file = lighting::bakedLightingFile(scene);
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
    if (playing()) {
        // En Play el World Partition descarga las celdas lejanas y lo que se
        // mueve ya no esta en su sitio: se hornearia (y guardaria) a medias.
        st.error = "Sal de Play para hornear (en Play las cosas se mueven y el World Partition descarga las celdas lejanas).";
        pushToast("Sal de Play para hornear", "En Play las cosas se mueven y las celdas lejanas del mundo no están cargadas", 2);
        return;
    }
    if (!asset_manager_) return;
    auto scene = std::make_shared<lighting::BakeScene>();
    // Mallas (Static, propio o heredado del padre) y terreno desde su mapa de
    // alturas (no depende de su collider). La malla de cada pieza ya dibujada
    // esta en la escena del renderizador (RenderSync la saco del AssetManager).
    lighting::BakeGatherReport report;
    lighting::gatherBakeScene(
        world_, *asset_manager_, *scene, {},
        [this](ecs::Entity e) -> std::shared_ptr<const terrain::TerrainData> {
            const terrain::Terrain* comp = e.tryGet<terrain::Terrain>();
            if (comp == nullptr) return nullptr;
            // Teselas de un TerrainGrid: solo las cargadas (las lee el
            // streaming; get() crearia una plana si aun no esta en memoria).
            return e.has<terrain::TerrainTile>() ? terrain_store_.find(*comp) : terrain_store_.get(*comp);
        },
        &report,
        [this](const Uuid& model, int part) -> const asset::ModelData* {
            return sync_ ? sync_->partModelData(model, part, scene_) : nullptr;
        });
    // Todas las luces del mundo con su modo (no solo las que el renderizador
    // tiene cerca de la camara); el sol, el de la hora del dia.
    lighting::gatherBakeLights(world_, scene_.lights(), *scene, static_cast<ecs::LightBakeMode>(st.sun_mode), &report);
    scene->settings = st.settings;
    st.report_text = report.summary();
    st.auto_volume = scene->volumes.empty();
    if (scene->volumes.empty() && !scene->triangles.empty()) {
        // La caja cubre las mallas y el terreno si cabe en su tope (si no, va
        // alrededor de las mallas) y, si con la separacion pedida no cabe en
        // el tope de sondas, se separan mas.
        const lighting::BakeVolume volume =
            lighting::volumeAround(scene->triangles, st.auto_spacing, scene->settings.max_probes, report.mesh_triangles);
        scene->volumes.push_back(volume);
        const float spacing = volume.size.x / static_cast<float>(std::max(volume.nx, 2u) - 1u);
        // Las que saldrian con la separacion pedida (como volumeAround, sin el tope).
        const auto along = [&](float size) {
            return std::max(1.0, std::round(static_cast<double>(size) / std::max(st.auto_spacing, 0.25f))) + 1.0;
        };
        const bool spread = static_cast<double>(volume.nx) * volume.ny * volume.nz <
                            along(volume.size.x) * along(volume.size.y) * along(volume.size.z);
        char text[160];
        std::snprintf(text, sizeof(text), "Volumen automático de %.0f x %.0f x %.0f m con sondas cada %.2f m%s",
                      static_cast<double>(volume.size.x), static_cast<double>(volume.size.y),
                      static_cast<double>(volume.size.z), static_cast<double>(spacing),
                      spread ? " (más separadas: con la separación pedida no cabían)" : "");
        st.report_text += std::string("\n") + text;
    }
    std::cout << "[Iluminacion] " << st.report_text << "\n";
    if (scene->triangles.empty()) {
        // El motivo, tambien para el MCP (lighting_state.last_error).
        st.error = st.report_text;
        pushToast("Nada que hornear", st.report_text.substr(0, st.report_text.find('\n')), 2);
        return;
    }
    st.progress = std::make_shared<lighting::BakeProgress>();
    st.finished = false;
    st.running = true;
    st.ok = false;
    st.error.clear();
    st.target = lighting::bakedLightingFile(scene_path_);
    st.scene = scene_path_;
    if (st.thread.joinable()) st.thread.join();
    LightingEditorState* state = &st;
    std::shared_ptr<lighting::BakeProgress> progress = st.progress;
    st.thread = std::thread([state, scene, progress] {
        gfx::BakedLighting out;
        lighting::BakeStats stats;
        std::string error;
        bool ok = false;
        // Una excepcion aqui (sin memoria con una escena enorme) cerraba el
        // editor entero (std::terminate): se cuenta como un horneado fallido.
        try {
            ok = lighting::bakeProbes(*scene, out, progress.get(), &stats, &error);
        } catch (const std::bad_alloc&) {
            out = gfx::BakedLighting{};
            error = "sin memoria: haz más pequeños los Light Probe Volume o sube su Separación o el Tamaño del texel";
        } catch (const std::exception& e) {
            out = gfx::BakedLighting{};
            error = e.what();
        }
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
                // Se guarda siempre, pero solo se aplica si sigue abierta la
                // escena horneada y no se esta en Play (al parar, exitPlay
                // recarga el .crbake de la escena abierta).
                if (st.scene == scene_path_) {
                    if (playing()) {
                        play_lighting_changed_ = true;
                    } else {
                        renderer_.setBakedLighting(st.result);
                        renderer_.setLightingMode(gfx::LightingMode::Baked);
                    }
                }
                refreshDatabase();
                char text[256];
                std::snprintf(text, sizeof(text), "%zu sondas, %zu texeles, %zu triángulos, %zu luces horneadas, %.1f s%s",
                              st.stats.probes, st.stats.surface_texels, st.stats.triangles, st.stats.baked_lights,
                              st.stats.seconds, st.auto_volume ? " (volumen automático)" : "");
                // Lo que el horneado tuvo que cambiar, tambien en el informe.
                std::string notes;
                if (st.stats.surface_texel_grown) {
                    char grown[160];
                    std::snprintf(grown, sizeof(grown),
                                  "\nLightmap de superficie con texeles de %.2f m (con %.2f m no cabían en el tope)",
                                  static_cast<double>(st.stats.surface_texel),
                                  static_cast<double>(st.settings.auto_surface_texel));
                    notes += grown;
                }
                if (st.stats.outside_texels_dropped) {
                    notes += "\nLa luz horneada no llega a lo que queda fuera de los Light Probe Volume: esos texeles no "
                             "cabían en el lightmap (agranda el texel o pon esas luces en Mixta)";
                }
                if (st.stats.dropped_volumes > 0) {
                    notes += "\n" + std::to_string(st.stats.dropped_volumes) + " Light Probe Volume de más: solo se usan los " +
                             std::to_string(gfx::kMaxBakedVolumes) + " más pequeños";
                }
                if (!notes.empty()) {
                    st.report_text += notes;
                    std::cout << "[Iluminacion]" << notes << "\n";
                }
                pushToast("Iluminación horneada", text + notes, 1);
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
        // Se guarda con el horneado (si lo hay). No si lo cargado no es el
        // .crbake de la escena (Scene.load en Play, u horneado nuevo que
        // espera a parar): se escribiria lo de otro nivel o el viejo.
        if (!scene_path_.empty() && !renderer_.bakedLighting().empty() && !play_lighting_changed_) {
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
        if (baked.sun_baked || !baked.baked_lights.empty()) {
            ImGui::TextDisabled("Horneado entero (luz directa y sombras): %s%zu luces", baked.sun_baked ? "el sol y " : "",
                                baked.baked_lights.size());
            if (renderer_.lightingMode() != gfx::LightingMode::Baked) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "En tiempo real esas luces se dibujan como siempre.");
            }
        }
        if (baked.sun_baked && !gfx::bakedSunMatches(baked, scene_.lights().sun.direction)) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                               "El sol se ha movido desde el horneado (hora del día): vuelve a hornear.\n"
                               "Mientras, el sol va en tiempo real.");
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
    ImGui::SeparatorText("Luz directa y sombras horneadas");
    {
        // El modo del sol lo da el Light direccional (si hay) o este ajuste.
        const ecs::Light* sun_light = nullptr;
        std::string sun_name;
        for (const entt::entity h : world_.registry().view<ecs::Light>()) {
            const ecs::Entity e = world_.wrap(h);
            const ecs::Light& l = e.get<ecs::Light>();
            if (l.type == ecs::LightType::Directional && e.activeInHierarchy()) {
                sun_light = &l;
                sun_name = e.name();
                break;
            }
        }
        static const char* kSunModes[] = {"Mixto (directa en tiempo real)", "Horneado (directa y sombras)",
                                          "Solo tiempo real (no se hornea)"};
        if (sun_light != nullptr) {
            const int m = std::clamp(static_cast<int>(sun_light->bake_mode), 0, 2);
            ImGui::TextDisabled("Sol: %s (Modo del Light '%s')", kSunModes[m], sun_name.c_str());
        } else {
            ImGui::Combo("Sol", &st.sun_mode, kSunModes, 3);
            ImGui::SetItemTooltip("Horneado: la luz del sol y sus sombras van al lightmap y a las sondas (casi gratis,\n"
                                  "pero fijo: si la hora del día lo mueve, vuelve a tiempo real). Mixto: sombras en\n"
                                  "tiempo real y solo el rebote horneado (recomendado al aire libre).");
        }
        ImGui::DragInt("Rayos de sombra por luz", &st.settings.direct_samples, 1.0f, 1, 128);
        ImGui::SetItemTooltip("Luces horneadas: rayos por texel hacia la bombilla (su Radio de la fuente) o el disco\n"
                              "del sol. Más = penumbras más suaves y menos ruido.");
        ImGui::TextWrapped("Las luces con Modo \"Horneada\" (Inspector > Light) hornean su luz directa y sus sombras "
                           "suaves y dejan de dibujarse en tiempo real (modo Horneada arriba). Con luces horneadas el "
                           "lightmap de superficie se enciende solo (texel de 0.5 m) si está apagado.");
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped("Aportan luz los objetos marcados Static (o hijos de uno Static: basta marcar la raíz de un "
                       "modelo o prefab; si no hay ninguno, las mallas que no se mueven) y el terreno. Las sondas se "
                       "ponen con el componente Light Probe Volume.");
    if (!st.report_text.empty() && !st.running) {
        if (ImGui::TreeNode("Último horneado: qué entró")) {
            ImGui::TextWrapped("%s", st.report_text.c_str());
            ImGui::TreePop();
        }
    }
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

void EditorApp::configureLightingBake(int rays, int bounces, float spacing, int sun_mode, float surface_texel,
                                      int direct_samples) {
    LightingEditorState& st = lightingEditor();
    if (rays > 0) st.settings.rays = rays;
    if (bounces >= 0) st.settings.bounces = bounces;
    if (spacing > 0.0f) st.auto_spacing = spacing;
    if (sun_mode >= 0 && sun_mode <= 2) st.sun_mode = sun_mode;
    if (surface_texel == 0.0f) st.settings.surface_texel = 0.0f;
    if (surface_texel > 0.0f) st.settings.surface_texel = std::clamp(surface_texel, 0.1f, 4.0f);
    if (direct_samples > 0) st.settings.direct_samples = std::min(direct_samples, 256);
}

std::string EditorApp::lightingStateJson() {
    LightingEditorState& st = lightingEditor();
    const gfx::BakedLighting& baked = renderer_.bakedLighting();
    // El hilo del horneado escribe st.error: solo se lee cuando ya no corre
    // (o ya acabo: finished se publica despues de escribirlo).
    const bool error_ready = !st.running || st.finished.load();
    nlohmann::json out{{"baking", st.running},
                       {"progress", st.progress ? st.progress->fraction.load() : 0.0f},
                       {"mode", renderer_.lightingMode() == gfx::LightingMode::Baked ? "baked" : "realtime"},
                       {"active", renderer_.bakedGiActive()},
                       {"volumes", baked.volumes.size()},
                       {"probes", baked.probes.size() / gfx::kBakedProbeVec4},
                       {"surface_texels", baked.surface.count},
                       {"sun_baked", baked.sun_baked},
                       {"baked_lights", baked.baked_lights.size()},
                       {"gather_report", error_ready ? st.report_text : std::string()},
                       {"last_error", error_ready ? st.error : std::string()}};
    return out.dump();
}

}  // namespace cramion::editor
