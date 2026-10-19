// Ventana > Iluminación (como Lighting de Unity): modo de la luz rebotada
// (tiempo real o horneada), ajustes del horneado y el boton Generar. El
// horneado corre en otro hilo (barra de progreso, se puede cancelar) y el
// resultado se guarda junto a la escena (<escena>.crbake); se carga solo al
// abrirla, en el editor y en el juego exportado.
//
// Desde el .crbake v4 (PLAN-LIGHTMAPS.md, Fase 2) el horneado son lightmaps
// por texel de UV2 en los objetos Static rigidos (lighting::bakeLighting) mas
// las sondas para todo lo demas. Las UV2 se generan solas (xatlas, cache en
// Library/Cache/LightmapUV) y van dentro del .crbake: al abrir la escena,
// el AssetManager carga cada modelo con su disposicion (setLightmapUvs).

#include "EditorApp.h"

#include <CramionCore/cvar/CVar.h>

#include "Dialogs.h"

#include <CramionCore/asset/BakedLightingFile.h>
#include <CramionCore/asset/LightmapUv.h>
#include <CramionCore/lighting/Lightmapper.h>
#include <CramionCore/lighting/ProbeBaker.h>
#include <CramionCore/physics/PhysicsComponents.h>
#include <CramionCore/terrain/Terrain.h>
#include <CramionCore/terrain/TerrainGrid.h>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <new>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#ifndef CRAMION_VERSION_STRING
#define CRAMION_VERSION_STRING "desconocida"
#endif

namespace cramion::editor {

namespace {

using LayoutList = std::vector<std::pair<lighting::LightmapUvPartKey, std::shared_ptr<const lighting::LightmapUvLayout>>>;

// Lo que dejo el boton "Generar UV de lightmap" (y el MCP generate_lightmap_uvs).
struct UvGenerationStats {
    std::size_t parts = 0, cache_hits = 0, charts = 0, ignored_triangles = 0, triangles = 0;
    float utilization = 0.0f;  // media
    std::uint32_t max_layout_size = 0;
    double seconds = 0.0;
};

float maxAxisScale(const core::Mat4& m) {
    const auto len = [&](int c) { return core::length(core::Vec3{m.m[c][0], m.m[c][1], m.m[c][2]}); };
    return std::max({len(0), len(1), len(2)});
}

}  // namespace

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
    lighting::LightmapBakeStats stats;
    lighting::BakeStats material_stats;
    LayoutList layouts;
    std::filesystem::path target;
    std::filesystem::path scene;  // la escena horneada (si se cambia de escena, no se aplica a la otra)
    bool auto_volume = false;
    // Modo del sol si la escena no tiene Light direccional (LightBakeMode).
    int sun_mode = static_cast<int>(ecs::LightBakeMode::Mixed);
    // Lo que entro en el ultimo horneado y por que no lo demas.
    std::string report_text;
    // Lightmaps (PLAN-LIGHTMAPS 2.8).
    lighting::LightmapBakeSettings lightmaps = lighting::lightmapPreset(lighting::LightmapPreset::Medium);
    int preset = 1;  // 0 Vista previa, 1 Media, 2 Alta, 3 Personalizada
    float probe_coverage = 1.0f;
    double seconds = 0.0;
    std::size_t receivers = 0, probe_only = 0;  // al reunir la escena
    // Estimacion (areas de las piezas Static con lightmap, cada poco).
    std::unordered_map<std::uint64_t, float> part_area;  // (modelo, pieza) -> area del modelo
    double estimate_area = 0.0;                          // sum(area_mundo * escala^2)
    std::chrono::steady_clock::time_point estimate_time{};
    // "Generar UV de lightmap" (Fase 1)
    std::thread uv_thread;
    std::shared_ptr<lighting::BakeProgress> uv_progress;
    std::atomic<bool> uv_finished{false};
    bool uv_running = false;
    std::shared_ptr<lighting::LightmapUvSet> uv_set;
    std::vector<Uuid> uv_models;
    UvGenerationStats uv_stats;
    std::string uv_error;
    float uv_texels_per_meter = 20.0f;

    ~LightingEditorState() {
        if (progress) progress->cancel = true;
        if (uv_progress) uv_progress->cancel = true;
        if (thread.joinable()) thread.join();
        if (uv_thread.joinable()) uv_thread.join();
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
    auto uvs = std::make_shared<lighting::LightmapUvSet>();
    if (!scene.empty()) {
        const std::filesystem::path file = lighting::bakedLightingFile(scene);
        std::string error;
        if (std::filesystem::exists(file) && !lighting::loadBakedLighting(file, data, mode, &error, uvs.get())) {
            std::cerr << "[Iluminacion] " << error << "\n";
            data = gfx::BakedLighting{};
            uvs->parts.clear();
        }
    }
    // Las UV de lightmap de esta escena (.crbake v4): los modelos que llevaban
    // las de la anterior o que ahora tienen se vuelven a leer con ellas.
    if (asset_manager_) {
        std::unordered_set<Uuid> models;
        if (const auto& before = asset_manager_->lightmapUvs()) {
            for (const auto& [key, source] : before->parts) models.insert(key.model);
        }
        for (const auto& [key, source] : uvs->parts) models.insert(key.model);
        asset_manager_->setLightmapUvs(uvs->empty() ? nullptr : std::shared_ptr<const lighting::LightmapUvSet>(uvs));
        for (const Uuid& model : models) asset_manager_->unload(model);
    }
    renderer_.setBakedLighting(std::move(data));
    renderer_.setLightingMode(mode);
}

namespace {

// Los MeshRenderer Static rigidos que llevan lightmap (PLAN-LIGHTMAPS 2.2,
// Receptores): de las mallas que entraron en gatherBakeScene. Lo demas Static
// solo tapa la luz y lo iluminan las sondas.
std::vector<lighting::LightmapReceiver> collectReceivers(ecs::World& world, assets::AssetManager& assets,
                                                         const lighting::BakeScene& scene,
                                                         const std::function<const asset::ModelData*(const Uuid&, int)>& part,
                                                         std::size_t& probe_only) {
    std::vector<lighting::LightmapReceiver> out;
    probe_only = 0;
    for (const lighting::BakeMeshEntry& entry : scene.meshes) {
        const ecs::Entity e = world.find(entry.entity);
        if (!e.valid()) continue;
        const ecs::MeshRenderer* r = e.tryGet<ecs::MeshRenderer>();
        if (r == nullptr || r->mesh || !r->model.valid()) continue;
        bool ok = lighting::staticInHierarchy(e) && r->lightmap_scale > 0.0f;
        if (const physics::Rigidbody* rb = e.tryGet<physics::Rigidbody>(); rb != nullptr && rb->type == physics::BodyType::Dynamic) {
            ok = false;
        }
        if (const ecs::Animator* animator = e.tryGet<ecs::Animator>(); animator != nullptr && animator->playing) {
            const asset::ModelData* data = part ? part(r->model.uuid, r->part) : nullptr;
            if (data == nullptr || !data->animations.empty()) ok = false;
        }
        if (const asset::ModelData* data = part ? part(r->model.uuid, r->part) : nullptr) {
            if (data->bones.size() > 1 || !data->animations.empty()) ok = false;  // se deforma
        }
        bool opaque = false;
        for (const std::uint8_t t : entry.transparent) opaque = opaque || t == 0;
        if (!ok || !opaque || entry.count == 0) {
            ++probe_only;
            continue;
        }
        lighting::LightmapReceiver rec;
        rec.entity = entry.entity;
        rec.model = entry.model;
        rec.part = entry.part;
        rec.world = e.worldMatrix();
        rec.lightmap_scale = r->lightmap_scale;
        const std::optional<assets::AssetInfo> info = assets.database().find(entry.model);
        if (info && info->type == assets::AssetType::Model) rec.file = info->path;
        rec.name = e.name();
        for (const assets::AssetRef& ref : r->materials) rec.material_overrides.push_back(ref.valid() ? ref.uuid : Uuid{});
        rec.materials = entry.materials;
        rec.transparent = entry.transparent;
        rec.scene_first = entry.first;
        rec.scene_count = entry.count;
        out.push_back(std::move(rec));
    }
    return out;
}

const char* kPresetNames[] = {"Vista previa", "Media", "Alta", "Personalizada"};

void applyPreset(LightingEditorState& st, int preset) {
    st.preset = std::clamp(preset, 0, 3);
    if (st.preset == 3) return;
    const bool enabled = st.lightmaps.enabled;
    st.lightmaps = lighting::lightmapPreset(static_cast<lighting::LightmapPreset>(st.preset));
    st.lightmaps.enabled = enabled;
    st.settings.bounces = st.lightmaps.bounces;
}

std::string settingsJson(const LightingEditorState& st) {
    const lighting::LightmapBakeSettings& l = st.lightmaps;
    nlohmann::json j{{"preset", st.preset},
                     {"lightmaps", l.enabled},
                     {"texels_per_meter", l.texels_per_meter},
                     {"max_page_size", l.max_page_size},
                     {"texel_budget", l.texel_budget},
                     {"sun_samples", l.sun_samples},
                     {"indirect_samples", l.indirect_samples},
                     {"bounces", l.bounces},
                     {"denoise", l.denoise},
                     {"min_component_texels", l.min_component_texels},
                     {"rays", st.settings.rays},
                     {"probe_bounces", st.settings.bounces},
                     {"direct_samples", st.settings.direct_samples}};
    return j.dump();
}

}  // namespace

void EditorApp::startLightingBake() {
    LightingEditorState& st = lightingEditor();
    if (st.running) return;
    if (st.uv_running) {
        pushToast("Espera a las UV de lightmap", "Se están generando las UV de lightmap", 2);
        return;
    }
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
    auto input = std::make_shared<lighting::LightmapBakeInput>();
    lighting::BakeScene& scene = input->scene;
    // Mallas (Static, propio o heredado del padre) y terreno desde su mapa de
    // alturas (no depende de su collider). La malla de cada pieza ya dibujada
    // esta en la escena del renderizador (RenderSync la saco del AssetManager).
    lighting::BakeGatherReport report;
    const auto part_of = [this](const Uuid& model, int part) -> const asset::ModelData* {
        return sync_ ? sync_->partModelData(model, part, scene_) : nullptr;
    };
    lighting::gatherBakeScene(
        world_, *asset_manager_, scene, {},
        [this](ecs::Entity e) -> std::shared_ptr<const terrain::TerrainData> {
            const terrain::Terrain* comp = e.tryGet<terrain::Terrain>();
            if (comp == nullptr) return nullptr;
            // Teselas de un TerrainGrid: solo las cargadas (las lee el
            // streaming; get() crearia una plana si aun no esta en memoria).
            return e.has<terrain::TerrainTile>() ? terrain_store_.find(*comp) : terrain_store_.get(*comp);
        },
        &report, part_of);
    // Todas las luces del mundo con su modo (no solo las que el renderizador
    // tiene cerca de la camara); el sol, el de la hora del dia.
    lighting::gatherBakeLights(world_, scene_.lights(), scene, static_cast<ecs::LightBakeMode>(st.sun_mode), &report);
    scene.settings = st.settings;
    // .crbake v4: la directa del sol no va a las sondas y no hay celdas.
    scene.settings.exclude_sun_direct = true;
    scene.settings.surface_texel = 0.0f;
    scene.settings.auto_surface_texel = 0.0f;
    // El cielo del render (sus armonicos), no un degradado fijo.
    scene.sky_sh = renderer_.readSkyIrradianceSh();
    scene.has_sky_sh = true;
    st.report_text = report.summary();
    st.auto_volume = scene.volumes.empty();
    st.probe_coverage = 1.0f;
    if (!scene.volumes.empty()) {
        // Lo Static que queda fuera de los Light Probe Volume del usuario no
        // tendria rebote ni sondas: un volumen automatico mas si pasa del 5%.
        std::vector<std::uint32_t> uncovered;
        st.probe_coverage = lighting::probeCoverage(scene, report.mesh_triangles, &uncovered);
        if (st.probe_coverage < 0.95f && !uncovered.empty()) {
            const lighting::BakeVolume volume =
                lighting::volumeAround(scene.triangles, uncovered, st.auto_spacing, scene.settings.max_probes);
            scene.volumes.push_back(volume);
            char text[220];
            std::snprintf(text, sizeof(text),
                          "Se añadió un volumen automático de %.0f × %.0f × %.0f m: el %.0f %% de las mallas Static "
                          "quedaba fuera de los Light Probe Volume",
                          static_cast<double>(volume.size.x), static_cast<double>(volume.size.y),
                          static_cast<double>(volume.size.z), static_cast<double>((1.0f - st.probe_coverage) * 100.0f));
            st.report_text += std::string("\n") + text;
        }
    }
    if (scene.volumes.empty() && !scene.triangles.empty()) {
        // La caja cubre las mallas y el terreno si cabe en su tope (si no, va
        // alrededor de las mallas) y, si con la separacion pedida no cabe en
        // el tope de sondas, se separan mas.
        const lighting::BakeVolume volume =
            lighting::volumeAround(scene.triangles, st.auto_spacing, scene.settings.max_probes, report.mesh_triangles);
        scene.volumes.push_back(volume);
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
    if (scene.triangles.empty()) {
        // El motivo, tambien para el MCP (lighting_state.last_error).
        st.error = st.report_text;
        pushToast("Nada que hornear", st.report_text.substr(0, st.report_text.find('\n')), 2);
        return;
    }
    // Receptores de los lightmaps.
    input->lightmaps = st.lightmaps;
    input->lightmaps.direct_samples = st.settings.direct_samples;
    if (st.lightmaps.enabled) {
        input->receivers = collectReceivers(world_, *asset_manager_, scene, part_of, st.probe_only);
    } else {
        st.probe_only = 0;
    }
    st.receivers = input->receivers.size();
    input->uv_cache = project_.libraryFolder() / "Cache" / "LightmapUV";
    input->scene_uuid = world_.sceneUuid();
    input->world_origin = {world_.origin().x, world_.origin().y, world_.origin().z};
    lighting::BakedLightingExtras extras;
    extras.engine_version = CRAMION_VERSION_STRING;
    extras.settings_json = settingsJson(st);
    extras.sun_direction = scene.lights.sun.direction;
    extras.sun_color = scene.lights.sun.color;
    extras.sun_intensity = scene.lights.sun.intensity;
    extras.sun_mode = scene.sun_baked ? 1u : (scene.lights.sun.intensity > 0.0f ? 0u : 2u);

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
    const std::filesystem::path target = st.target;
    st.thread = std::thread([state, input, progress, target, extras]() mutable {
        const auto start = std::chrono::steady_clock::now();
        gfx::BakedLighting out;
        lighting::LightmapBakeStats stats;
        lighting::BakeStats material_stats;
        LayoutList layouts;
        std::string error;
        bool ok = false;
        // Una excepcion aqui (sin memoria con una escena enorme) cerraba el
        // editor entero (std::terminate): se cuenta como un horneado fallido.
        try {
            // Los colores de los materiales (texturas) se leen aqui, no en el
            // hilo principal (PLAN-LIGHTMAPS 0.2).
            lighting::resolveBakeMaterials(input->scene, progress.get(), &material_stats);
            ok = lighting::bakeLighting(*input, out, layouts, progress.get(), &stats, &error);
            if (ok) {
                progress->setStage("Guardando el .crbake");
                const auto save_start = std::chrono::steady_clock::now();
                for (const auto& [key, layout] : layouts) {
                    extras.layouts.push_back(lighting::BakedLightingExtras::Layout{key.model, key.part, layout});
                }
                ok = lighting::saveBakedLighting(target, out, gfx::LightingMode::Baked, &error, &extras);
                stats.seconds_save = std::chrono::duration<double>(std::chrono::steady_clock::now() - save_start).count();
            }
        } catch (const std::bad_alloc&) {
            ok = false;
            error = "sin memoria: baja los Texeles por metro o el presupuesto, o sube la Separación de los Light Probe Volume";
        } catch (const std::exception& e) {
            ok = false;
            error = e.what();
        }
        state->stats = std::move(stats);
        state->material_stats = material_stats;
        state->layouts = std::move(layouts);
        state->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        state->error = error;
        state->ok = ok;
        state->finished = true;
    });
}

void EditorApp::startLightmapUvGeneration(float texels_per_meter) {
    LightingEditorState& st = lightingEditor();
    if (st.uv_running || st.running || !asset_manager_) return;
    // Los receptores sin hornear: Static, de un modelo, con escala > 0.
    struct Piece {
        Uuid model;
        int part = 0;
        float max_scale = 0.0f;  // escala del mundo x Escala en lightmap
        std::filesystem::path file;
        std::string name;
    };
    std::map<std::pair<Uuid, int>, Piece> pieces;
    for (const entt::entity h : world_.registry().view<ecs::MeshRenderer>()) {
        const ecs::Entity e = world_.wrap(h);
        const ecs::MeshRenderer& r = e.get<ecs::MeshRenderer>();
        if (!e.activeInHierarchy() || !r.visible || r.mesh || !r.model.valid() || r.lightmap_scale <= 0.0f) continue;
        if (!lighting::staticInHierarchy(e)) continue;
        Piece& piece = pieces[{r.model.uuid, r.part}];
        piece.model = r.model.uuid;
        piece.part = r.part;
        piece.max_scale = std::max(piece.max_scale, maxAxisScale(e.worldMatrix()) * r.lightmap_scale);
        if (piece.file.empty()) {
            const std::optional<assets::AssetInfo> info = asset_manager_->database().find(r.model.uuid);
            if (info && info->type == assets::AssetType::Model) {
                piece.file = info->path;
                piece.name = info->name;
            }
        }
    }
    if (pieces.empty()) {
        st.uv_error = "no hay objetos Static de un modelo con Escala en lightmap > 0";
        pushToast("Nada que desenvolver", st.uv_error, 2);
        return;
    }
    std::vector<Piece> list;
    for (auto& [key, piece] : pieces) list.push_back(piece);
    st.uv_progress = std::make_shared<lighting::BakeProgress>();
    st.uv_finished = false;
    st.uv_running = true;
    st.uv_error.clear();
    st.uv_texels_per_meter = texels_per_meter;
    if (st.uv_thread.joinable()) st.uv_thread.join();
    LightingEditorState* state = &st;
    std::shared_ptr<lighting::BakeProgress> progress = st.uv_progress;
    const std::filesystem::path cache = project_.libraryFolder() / "Cache" / "LightmapUV";
    const lighting::LightmapBakeSettings settings = st.lightmaps;
    st.uv_thread = std::thread([state, progress, list, cache, settings, texels_per_meter]() {
        const auto start = std::chrono::steady_clock::now();
        auto set = std::make_shared<lighting::LightmapUvSet>();
        UvGenerationStats stats;
        std::string error;
        std::vector<Uuid> models;
        double utilization = 0.0;
        try {
            // Modelo a modelo (solo las piezas que hacen falta, sin texturas).
            std::map<Uuid, std::vector<const Piece*>> by_model;
            for (const auto& piece : list) by_model[piece.model].push_back(&piece);
            std::size_t done = 0;
            for (const auto& [uuid, model_pieces] : by_model) {
                if (progress->cancel.load()) break;
                std::vector<bool> wanted;
                for (const auto* piece : model_pieces) {
                    const auto p = static_cast<std::size_t>(std::max(piece->part, 0));
                    if (wanted.size() <= p) wanted.resize(p + 1, false);
                    wanted[p] = true;
                }
                const std::shared_ptr<assets::ModelAsset> geometry = assets::AssetManager::readModelGeometry(
                    uuid, model_pieces.front()->file, model_pieces.front()->name, wanted);
                if (!geometry) continue;
                models.push_back(uuid);
                for (const auto* piece : model_pieces) {
                    if (piece->part < 0 || static_cast<std::size_t>(piece->part) >= geometry->parts.size()) continue;
                    const asset::ModelData& data = *geometry->parts[static_cast<std::size_t>(piece->part)];
                    if (data.indices.size() < 3 || data.bones.size() > 1 || !data.animations.empty()) continue;
                    progress->setStage("UV de lightmap: " + piece->name + " (" + std::to_string(done + 1) + " de " +
                                       std::to_string(list.size()) + ")");
                    lighting::LightmapUvSettings s;
                    s.texels_per_unit = std::max(texels_per_meter * std::max(piece->max_scale, 1e-3f), 1e-3f);
                    s.padding = settings.padding;
                    s.min_component_texels = settings.min_component_texels;
                    s.max_size = settings.max_page_size;
                    s.chart_iterations = settings.chart_iterations;
                    const std::uint64_t source_hash = lighting::lightmapMeshHash(data);
                    const std::filesystem::path file = lighting::lightmapUvCacheFile(cache, source_hash, s.hash());
                    auto layout = std::make_shared<lighting::LightmapUvLayout>();
                    std::error_code ec;
                    bool hit = std::filesystem::exists(file, ec) && lighting::loadLightmapUvLayout(file, *layout) &&
                               layout->source_hash == source_hash;
                    if (hit) {
                        ++stats.cache_hits;
                    } else {
#if CRAMION_HAS_LIGHTMAP_UNWRAP
                        std::string unwrap_error;
                        const std::function<bool(float)> on_progress = [&](float f) {
                            progress->fraction.store((static_cast<float>(done) + f) / static_cast<float>(list.size()));
                            return !progress->cancel.load();
                        };
                        if (!lighting::unwrapLightmapUvs(data, s, *layout, on_progress, &unwrap_error)) {
                            if (unwrap_error != "cancelado" && error.empty()) error = piece->name + ": " + unwrap_error;
                            ++done;
                            continue;
                        }
                        lighting::saveLightmapUvLayout(file, *layout);
#else
                        error = "este binario no genera UV de lightmap";
                        break;
#endif
                    }
                    lighting::LightmapUvSet::Source source;
                    source.file = file;
                    source.layout = layout;
                    set->parts[lighting::LightmapUvPartKey{uuid, piece->part}] = source;
                    ++stats.parts;
                    stats.charts += layout->charts;
                    stats.ignored_triangles += layout->ignored_triangles;
                    stats.triangles += layout->source_indices / 3;
                    stats.max_layout_size = std::max({stats.max_layout_size, layout->width, layout->height});
                    utilization += layout->utilization;
                    ++done;
                    progress->fraction.store(static_cast<float>(done) / static_cast<float>(list.size()));
                }
            }
        } catch (const std::bad_alloc&) {
            error = "sin memoria al desenvolver las UV de lightmap";
        } catch (const std::exception& e) {
            error = e.what();
        }
        stats.utilization = stats.parts > 0 ? static_cast<float>(utilization / static_cast<double>(stats.parts)) : 0.0f;
        stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        state->uv_set = progress->cancel.load() ? nullptr : set;
        state->uv_models = std::move(models);
        state->uv_stats = stats;
        state->uv_error = progress->cancel.load() ? std::string("cancelado") : error;
        state->uv_finished = true;
    });
}

void EditorApp::drawLightingWindow() {
    LightingEditorState& st = lightingEditor();
    // Terminaron las UV de lightmap (boton): se cargan los modelos con ellas.
    if (st.uv_running && st.uv_finished.load()) {
        if (st.uv_thread.joinable()) st.uv_thread.join();
        st.uv_running = false;
        if (st.uv_set && asset_manager_ && !playing()) {
            asset_manager_->setLightmapUvs(st.uv_set);
            if (sync_) sync_->reset(scene_);
            for (const Uuid& model : st.uv_models) asset_manager_->unload(model);
            char text[200];
            std::snprintf(text, sizeof(text), "%zu piezas (%zu de la caché), %zu cartas, %.0f %% de ocupación, %.1f s",
                          st.uv_stats.parts, st.uv_stats.cache_hits, st.uv_stats.charts,
                          static_cast<double>(st.uv_stats.utilization * 100.0f), st.uv_stats.seconds);
            pushToast("UV de lightmap generadas", text, 1);
        }
        if (!st.uv_error.empty() && st.uv_error != "cancelado") pushToast("UV de lightmap", st.uv_error, 3);
    }
    // Termino un horneado: aplicar (ya esta guardado).
    if (st.running && st.finished.load()) {
        if (st.thread.joinable()) st.thread.join();
        st.running = false;
        if (st.ok) {
            // Se guarda siempre, pero solo se aplica si sigue abierta la
            // escena horneada y no se esta en Play (al parar, exitPlay
            // recarga el .crbake de la escena abierta).
            if (st.scene == scene_path_) {
                if (playing()) {
                    play_lighting_changed_ = true;
                } else {
                    // Las disposiciones de UV2 que no llevan los modelos cargados:
                    // esos modelos se vuelven a leer con ellas.
                    std::vector<Uuid> changed;
                    for (const auto& [key, layout] : st.layouts) {
                        const asset::ModelData* data = sync_ ? sync_->partModelData(key.model, key.part, scene_) : nullptr;
                        if (data == nullptr || !layout || data->lightmap_layout_hash != layout->layout_hash) {
                            // find_if: el std::find vectorizado de MSVC no admite Uuid (16 bytes).
                            const auto same = [&](const Uuid& u) { return u == key.model; };
                            if (std::find_if(changed.begin(), changed.end(), same) == changed.end()) changed.push_back(key.model);
                        }
                    }
                    loadSceneLighting();  // relee del disco (sin copiar el resultado)
                    if (!changed.empty() && sync_) {
                        sync_->reset(scene_);
                        if (asset_manager_) {
                            for (const Uuid& model : changed) asset_manager_->unload(model);
                        }
                        pushToast("Aplicando lightmaps", "Recargando " + std::to_string(changed.size()) + " modelos", 1);
                    }
                }
            }
            refreshDatabase();
            const lighting::LightmapBakeStats& s = st.stats;
            const double invalid = s.texels > 0 ? 100.0 * static_cast<double>(s.invalid_texels) /
                                                      static_cast<double>(std::max<std::size_t>(s.valid_texels + s.invalid_texels, 1))
                                                : 0.0;
            char text[320];
            std::snprintf(text, sizeof(text),
                          "%zu objetos con lightmap, %zu con sondas · %zu páginas · %.1f %% de texeles no válidos · "
                          "%zu sondas · %.1f s",
                          s.receivers, s.probe_only + st.probe_only, s.pages, invalid, s.probes.probes, st.seconds);
            std::string notes;
            for (const std::string& w : s.warnings) notes += "\n" + w;
            if (s.probes.dropped_volumes > 0) {
                notes += "\n" + std::to_string(s.probes.dropped_volumes) + " Light Probe Volume de más: solo se usan los " +
                         std::to_string(gfx::kMaxBakedVolumes) + " más pequeños";
            }
            if (!notes.empty()) {
                st.report_text += notes;
                std::cout << "[Iluminacion]" << notes << "\n";
            }
            std::cout << "[Iluminacion] " << text << "\n";
            pushToast("Iluminación horneada", text + notes, 1);
        } else if (st.error != "cancelado") {
            pushToast("El horneado falló", st.error, 3);
        }
    }
    if (!st.show) return;
    ImGui::SetNextWindowSize(ImVec2(460.0f, 640.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Iluminación###lighting_window", &st.show)) {
        ImGui::End();
        return;
    }
    ImGui::SeparatorText("Luz rebotada (GI)");
    int mode = static_cast<int>(renderer_.lightingMode());
    static const char* kModes[] = {"Tiempo real (SSGI / rayos)", "Horneada (lightmaps y sondas)"};
    if (ImGui::Combo("Modo", &mode, kModes, 2)) {
        renderer_.setLightingMode(static_cast<gfx::LightingMode>(mode));
        // Se guarda con el horneado (si lo hay): solo el byte del modo, sin
        // reescribir el archivo (las paginas no se tocan). No si lo cargado no
        // es el .crbake de la escena (Scene.load en Play, u horneado nuevo
        // que espera a parar): se escribiria lo de otro nivel o el viejo.
        if (!scene_path_.empty() && !renderer_.bakedLighting().empty() && !play_lighting_changed_) {
            std::string error;
            const std::filesystem::path file = lighting::bakedLightingFile(scene_path_);
            if (std::filesystem::exists(file) && !lighting::setBakedLightingMode(file, static_cast<gfx::LightingMode>(mode), &error)) {
                std::cerr << "[Iluminacion] " << error << "\n";
            }
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
        const gfx::LightmapAtlas& atlas = renderer_.lightmapAtlas();
        if (!baked.lightmaps.records.empty()) {
            ImGui::TextDisabled("Lightmaps: %zu objetos, %u páginas de %u x %u (%.1f MB), %.1f texeles/m%s",
                                baked.lightmaps.records.size(), atlas.layers(), atlas.width(), atlas.height(),
                                static_cast<double>(atlas.bytes()) / (1024.0 * 1024.0),
                                static_cast<double>(baked.lightmaps.texels_per_meter),
                                baked.lightmaps.has_irradiance ? ", con luz rebotada" : "");
            if (sync_) {
                ImGui::TextDisabled("%zu objetos se dibujan con su lightmap", sync_->lightmappedActors());
                if (sync_->staleLightmaps() > 0) {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "%zu objetos se movieron desde el horneado",
                                       sync_->staleLightmaps());
                }
            }
        }
        if (!baked.surface.empty()) {
            ImGui::TextDisabled("Lightmap de superficie: %u texeles de %.2f m (%.1f MB)", baked.surface.count,
                                baked.surface.cell_size,
                                static_cast<double>(baked.surface.texels.size() * sizeof(core::Vec4) +
                                                    baked.surface.keys.size() * 4) / (1024.0 * 1024.0));
        }
        if (renderer_.lightingMode() == gfx::LightingMode::Baked && !renderer_.bakedGiActive() && !renderer_.lightmapsActive()) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "Los datos no son válidos: vuelve a hornear.");
        }
        if (baked.version <= 3) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f),
                               "Iluminación de una versión anterior: genera de nuevo para tener lightmaps.");
            if (baked.sun_baked || !baked.baked_lights.empty()) {
                ImGui::TextDisabled("Horneado entero (luz directa y sombras): %s%zu luces", baked.sun_baked ? "el sol y " : "",
                                    baked.baked_lights.size());
            }
            if (baked.sun_baked && !gfx::bakedSunMatches(baked, scene_.lights().sun.direction)) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                                   "El sol se ha movido desde el horneado (hora del día): vuelve a hornear.\n"
                                   "Mientras, el sol va en tiempo real.");
            }
        } else {
            if (!baked.baked_lights.empty()) {
                ImGui::TextDisabled("Luces horneadas (directa y sombras en el lightmap): %zu", baked.baked_lights.size());
            }
            if (renderer_.sunBakeMode() && baked.lightmaps.sun_visibility &&
                !gfx::bakedSunDirectionMatches(baked, scene_.lights().sun.direction)) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                                   "El sol se ha movido: usa las cascadas hasta que vuelvas a hornear.");
            }
        }
    }

    ImGui::SeparatorText("Lightmaps");
    ImGui::BeginDisabled(st.running || st.uv_running);
    ImGui::Checkbox("Lightmaps (objetos Static)", &st.lightmaps.enabled);
    ImGui::SetItemTooltip("Luz horneada por texel en los objetos Static que no se mueven (UV de lightmap automáticas).\n"
                          "Lo pequeño, lo que se mueve y lo que tiene Escala en lightmap 0 usa las sondas.");
    if (st.lightmaps.enabled) {
        int preset = st.preset;
        if (ImGui::Combo("Calidad", &preset, kPresetNames, 4)) applyPreset(st, preset);
        bool custom = false;
        custom |= ImGui::DragFloat("Texeles por metro", &st.lightmaps.texels_per_meter, 0.25f, 1.0f, 100.0f, "%.1f");
        ImGui::SetItemTooltip("Resolución de los lightmaps: 20 = un texel cada 5 cm.");
        int page = static_cast<int>(st.lightmaps.max_page_size);
        static const char* kPages[] = {"1024", "2048", "4096"};
        int page_index = page >= 4096 ? 2 : (page >= 2048 ? 1 : 0);
        if (ImGui::Combo("Tamaño máximo de página", &page_index, kPages, 3)) {
            st.lightmaps.max_page_size = 1024u << page_index;
            custom = true;
        }
        float budget = static_cast<float>(st.lightmaps.texel_budget) / 1048576.0f;
        if (ImGui::DragFloat("Presupuesto (millones de texeles)", &budget, 0.1f, 0.5f, 64.0f, "%.1f")) {
            st.lightmaps.texel_budget = static_cast<std::uint32_t>(std::clamp(budget, 0.5f, 64.0f) * 1048576.0f);
            custom = true;
        }
        ImGui::SetItemTooltip("Si la escena pide más, se baja la densidad de toda la escena (con un aviso).");
        custom |= ImGui::DragInt("Muestras de sombra del sol", &st.lightmaps.sun_samples, 1.0f, 1, 1024);
        custom |= ImGui::DragInt("Muestras de luz rebotada", &st.lightmaps.indirect_samples, 2.0f, 0, 4096);
        custom |= ImGui::SliderInt("Rebotes", &st.lightmaps.bounces, 0, 6);
        custom |= ImGui::Checkbox("Quitar ruido", &st.lightmaps.denoise);
        custom |= ImGui::DragFloat("Tamaño mínimo de pieza (texeles)", &st.lightmaps.min_component_texels, 0.1f, 0.0f, 32.0f, "%.1f");
        ImGui::SetItemTooltip("Las piezas sueltas más pequeñas no llevan lightmap: las iluminan las sondas.");
        if (custom) st.preset = 3;
        float ao = renderer_.lightmapAo();
        if (ImGui::SliderFloat("SSAO sobre lo horneado", &ao, 0.0f, 1.0f, "%.2f")) renderer_.setLightmapAo(ao);
        // Estimacion (cada segundo): area de las piezas Static con lightmap.
        const auto now = std::chrono::steady_clock::now();
        if (now - st.estimate_time > std::chrono::seconds(1) && sync_) {
            st.estimate_time = now;
            st.estimate_area = 0.0;
            for (const entt::entity h : world_.registry().view<ecs::MeshRenderer>()) {
                const ecs::Entity e = world_.wrap(h);
                const ecs::MeshRenderer& r = e.get<ecs::MeshRenderer>();
                if (!e.activeInHierarchy() || !r.visible || r.mesh || !r.model.valid() || r.lightmap_scale <= 0.0f ||
                    !lighting::staticInHierarchy(e)) {
                    continue;
                }
                const std::uint64_t key = r.model.uuid.high ^ (r.model.uuid.low * 0x9E3779B97F4A7C15ull) ^
                                          (static_cast<std::uint64_t>(static_cast<std::uint32_t>(r.part)) << 7);
                auto it = st.part_area.find(key);
                if (it == st.part_area.end()) {
                    const asset::ModelData* data = sync_->partModelData(r.model.uuid, r.part, scene_);
                    if (data == nullptr) continue;
                    it = st.part_area.emplace(key, lighting::meshSurfaceArea(*data)).first;
                }
                const float s = maxAxisScale(e.worldMatrix()) * r.lightmap_scale;
                st.estimate_area += static_cast<double>(it->second) * static_cast<double>(s) * static_cast<double>(s);
            }
        }
        if (st.estimate_area > 0.0) {
            const double budget_texels = static_cast<double>(st.lightmaps.texel_budget);
            const double density = std::min(static_cast<double>(st.lightmaps.texels_per_meter),
                                            std::sqrt(0.6 * budget_texels / st.estimate_area));
            const double texels = st.estimate_area * density * density / 0.6;  // con el relleno de las cartas
            const double page_texels = static_cast<double>(st.lightmaps.max_page_size) * st.lightmaps.max_page_size;
            const int pages = std::max(1, static_cast<int>(std::ceil(texels / page_texels)));
            ImGui::TextDisabled("≈ %.1f M texeles · %d páginas de %u×%u · %.0f MB · densidad efectiva %.1f t/m", texels / 1e6,
                                pages, st.lightmaps.max_page_size, st.lightmaps.max_page_size, texels * 8.0 / (1024.0 * 1024.0),
                                density);
        }
        // UV de lightmap sin hornear (vista "UV de lightmap" de la vista Escena).
        if (ImGui::Button("Generar UV de lightmap")) startLightmapUvGeneration(st.lightmaps.texels_per_meter);
        ImGui::SetItemTooltip("Solo las UV2 (xatlas), sin hornear: se ven con la vista \"UV de lightmap\" de la vista\n"
                              "Escena. Se guardan en Library/Cache/LightmapUV; el horneado las reutiliza.");
    }
    ImGui::EndDisabled();
    if (st.uv_running) {
        ImGui::ProgressBar(st.uv_progress ? st.uv_progress->fraction.load() : 0.0f, ImVec2(-1.0f, 0.0f));
        ImGui::TextDisabled("%s", st.uv_progress ? st.uv_progress->currentStage().c_str() : "");
        if (ImGui::Button("Cancelar las UV", ImVec2(-1.0f, 0.0f)) && st.uv_progress) st.uv_progress->cancel = true;
    }
    if (!st.running && st.stats.pages + st.stats.receivers > 0) {
        const lighting::LightmapBakeStats& s = st.stats;
        if (ImGui::TreeNode("Último horneado: lightmaps")) {
            const double invalid = s.valid_texels + s.invalid_texels > 0
                                       ? 100.0 * static_cast<double>(s.invalid_texels) /
                                             static_cast<double>(s.valid_texels + s.invalid_texels)
                                       : 0.0;
            ImGui::Text("%zu objetos con lightmap, %zu con sondas (pequeños, escala 0 o que se mueven)", s.receivers,
                        s.probe_only + st.probe_only);
            ImGui::Text("%zu páginas · %.1f %% de texeles no válidos · %.1f t/m (pedidos %.1f)", s.pages, invalid,
                        static_cast<double>(s.effective_texels_per_meter), static_cast<double>(s.texels_per_meter));
            ImGui::Text("UV2: %zu disposiciones (%zu de la caché)", s.layouts, s.layout_cache_hits);
            ImGui::Text("Tiempos: UV %.1f s · geometría %.1f · BVH %.1f · texeles %.1f · validez %.1f · sol %.1f · rebote %.1f "
                        "(filtro %.1f) · sondas %.1f · guardar %.1f",
                        s.seconds_uv, s.seconds_geometry, s.seconds_bvh, s.seconds_raster, s.seconds_validity, s.seconds_sun,
                        s.seconds_indirect, s.seconds_denoise, s.seconds_probes, s.seconds_save);
            ImGui::Text("%.0f M rayos (%.1f M/s)", s.rays / 1e6, s.rays_per_second / 1e6);
            for (const std::string& w : s.warnings) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "%s", w.c_str());
            ImGui::TreePop();
        }
    }

    ImGui::SeparatorText("Sondas");
    ImGui::BeginDisabled(st.running);
    ImGui::DragInt("Rayos por sonda", &st.settings.rays, 4.0f, 16, 4096);
    ImGui::SetItemTooltip("Más = menos ruido (y más tiempo)");
    ImGui::SliderInt("Rebotes de las sondas", &st.settings.bounces, 0, 6);
    ImGui::SetItemTooltip("Rebotes además del primero (0 = un rebote)");
    ImGui::DragFloat("Intensidad del cielo", &st.settings.sky_intensity, 0.01f, 0.0f, 10.0f, "%.2f");
    ImGui::DragFloat("Separación automática (m)", &st.auto_spacing, 0.05f, 0.5f, 20.0f, "%.2f");
    ImGui::SetItemTooltip("Si la escena no tiene Light Probe Volume (o no cubren lo Static), uno que cubre todo con esta\n"
                          "separación");
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
        static const char* kSunModes[] = {"Mixto (luz y sombras en tiempo real)",
                                          "Horneado (sombras horneadas, sin coste de sombras)",
                                          "Solo tiempo real (no se hornea)"};
        if (sun_light != nullptr) {
            const int m = std::clamp(static_cast<int>(sun_light->bake_mode), 0, 2);
            ImGui::TextDisabled("Sol: %s (Modo del Light '%s')", kSunModes[m], sun_name.c_str());
        } else {
            ImGui::Combo("Sol", &st.sun_mode, kSunModes, 3);
            ImGui::SetItemTooltip("Horneado: el sol se dibuja en tiempo real (misma luz, especular y normal maps) y sus\n"
                                  "sombras salen del lightmap (fijas: si la hora del día lo mueve, vuelven las cascadas).\n"
                                  "Mixto: sombras en tiempo real y solo el rebote horneado (recomendado al aire libre).");
        }
        ImGui::DragInt("Rayos de sombra por luz", &st.settings.direct_samples, 1.0f, 1, 128);
        ImGui::SetItemTooltip("Luces horneadas: rayos por texel hacia la bombilla (su Radio de la fuente). Más =\n"
                              "penumbras más suaves y menos ruido.");
        ImGui::TextWrapped("Las puntuales y los focos con Modo \"Horneada\" (Inspector > Light) hornean su luz directa y "
                           "sus sombras suaves y dejan de dibujarse en tiempo real (modo Horneada arriba).");
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
        ImGui::BeginDisabled(st.uv_running);
        if (ImGui::Button("Generar iluminación", ImVec2(-1.0f, 32.0f))) startLightingBake();
        ImGui::EndDisabled();
        ImGui::BeginDisabled(baked.empty());
        if (ImGui::Button("Borrar la iluminación horneada", ImVec2(-1.0f, 0.0f))) {
            std::error_code ec;
            if (!scene_path_.empty()) std::filesystem::remove(lighting::bakedLightingFile(scene_path_), ec);
            loadSceneLighting();  // sin archivo: sin sondas, sin lightmaps y sin UV2
            if (sync_) sync_->reset(scene_);
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
    // (El lightmap de superficie ya no se genera: el .crbake v4 lleva lightmaps por UV2.)
    (void)surface_texel;
    if (direct_samples > 0) st.settings.direct_samples = std::min(direct_samples, 256);
}

void EditorApp::configureLightmapBake(const std::string& args_json) {
    LightingEditorState& st = lightingEditor();
    const nlohmann::json args = nlohmann::json::parse(args_json, nullptr, false);
    if (!args.is_object()) return;
    if (args.contains("preset") && args["preset"].is_string()) {
        const std::string p = args["preset"].get<std::string>();
        applyPreset(st, p == "preview" ? 0 : (p == "high" ? 2 : 1));
    }
    bool custom = false;
    if (args.contains("lightmaps") && args["lightmaps"].is_boolean()) st.lightmaps.enabled = args["lightmaps"].get<bool>();
    if (args.contains("texels_per_meter") && args["texels_per_meter"].is_number()) {
        st.lightmaps.texels_per_meter = std::clamp(args["texels_per_meter"].get<float>(), 0.5f, 200.0f);
        custom = true;
    }
    if (args.contains("max_page_size") && args["max_page_size"].is_number_integer()) {
        const int size = args["max_page_size"].get<int>();
        st.lightmaps.max_page_size = size >= 4096 ? 4096u : (size >= 2048 ? 2048u : 1024u);
        custom = true;
    }
    if (args.contains("texel_budget_millions") && args["texel_budget_millions"].is_number()) {
        st.lightmaps.texel_budget =
            static_cast<std::uint32_t>(std::clamp(args["texel_budget_millions"].get<float>(), 0.25f, 128.0f) * 1048576.0f);
        custom = true;
    }
    if (args.contains("sun_samples") && args["sun_samples"].is_number_integer()) {
        st.lightmaps.sun_samples = std::clamp(args["sun_samples"].get<int>(), 1, 4096);
        custom = true;
    }
    if (args.contains("indirect_samples") && args["indirect_samples"].is_number_integer()) {
        st.lightmaps.indirect_samples = std::clamp(args["indirect_samples"].get<int>(), 0, 8192);
        custom = true;
    }
    if (args.contains("min_piece_texels") && args["min_piece_texels"].is_number()) {
        st.lightmaps.min_component_texels = std::clamp(args["min_piece_texels"].get<float>(), 0.0f, 64.0f);
        custom = true;
    }
    if (args.contains("denoise") && args["denoise"].is_boolean()) {
        st.lightmaps.denoise = args["denoise"].get<bool>();
        custom = true;
    }
    if (args.contains("bounces") && args["bounces"].is_number_integer()) {
        st.lightmaps.bounces = std::clamp(args["bounces"].get<int>(), 0, 8);
    }
    if (args.contains("lightmap_ao") && args["lightmap_ao"].is_number()) renderer_.setLightmapAo(args["lightmap_ao"].get<float>());
    if (custom && !args.contains("preset")) st.preset = 3;
}

std::string EditorApp::setLightingModeJson(const std::string& args_json) {
    const nlohmann::json args = nlohmann::json::parse(args_json, nullptr, false);
    if (args.is_object()) {
        if (args.contains("mode") && args["mode"].is_string()) {
            const gfx::LightingMode mode =
                args["mode"].get<std::string>() == "baked" ? gfx::LightingMode::Baked : gfx::LightingMode::Realtime;
            renderer_.setLightingMode(mode);
            // persist: solo el byte del modo en el .crbake (las paginas no se tocan).
            if (args.value("persist", false) && !scene_path_.empty() && !play_lighting_changed_) {
                const std::filesystem::path file = lighting::bakedLightingFile(scene_path_);
                std::string error;
                if (std::filesystem::exists(file) && !lighting::setBakedLightingMode(file, mode, &error)) {
                    std::cerr << "[Iluminacion] " << error << "\n";
                }
            }
        }
        if (args.contains("exposure")) {
            renderer_.setExposureOverride(args["exposure"].is_number() ? args["exposure"].get<float>() : 0.0f);
        }
        if (args.contains("debug_view") && args["debug_view"].is_string()) {
            const std::string view = args["debug_view"].get<std::string>();
            lightmap_debug_view_ = view == "lightmap" ? 1 : (view == "sun_visibility" ? 2 : (view == "uv2" ? 3 : 0));
        }
    }
    return lightingStateJson();
}

std::string EditorApp::generateLightmapUvsJson(const std::string& args_json) {
    LightingEditorState& st = lightingEditor();
    const nlohmann::json args = nlohmann::json::parse(args_json, nullptr, false);
    float density = st.lightmaps.texels_per_meter;
    if (args.is_object() && args.contains("texels_per_meter") && args["texels_per_meter"].is_number()) {
        density = std::clamp(args["texels_per_meter"].get<float>(), 0.5f, 200.0f);
    }
    startLightmapUvGeneration(density);
    return nlohmann::json{{"started", st.uv_running},
                          {"error", st.uv_error},
                          {"note", "Corre en segundo plano: mira lighting_state.uv_generation."}}
        .dump();
}

std::string EditorApp::lightingStateJson() {
    LightingEditorState& st = lightingEditor();
    const gfx::BakedLighting& baked = renderer_.bakedLighting();
    // El hilo del horneado escribe st.error: solo se lee cuando ya no corre
    // (o ya acabo: finished se publica despues de escribirlo).
    const bool error_ready = !st.running || st.finished.load();
    const bool stats_ready = error_ready;
    const lighting::LightmapBakeStats& s = st.stats;
    const gfx::LightmapAtlas& atlas = renderer_.lightmapAtlas();
    const gfx::SunShadowSource source = renderer_.sunShadowSource(scene_.lights().sun.direction);
    static const char* kDebugViews[] = {"none", "lightmap", "sun_visibility", "uv2"};
    nlohmann::json lightmaps{{"enabled", st.lightmaps.enabled},
                             {"pages", atlas.real() ? atlas.layers() : 0u},
                             {"atlas", {atlas.real() ? atlas.width() : 0u, atlas.real() ? atlas.height() : 0u}},
                             {"format", "RGBA16F"},
                             {"megabytes", atlas.real() ? static_cast<double>(atlas.bytes()) / (1024.0 * 1024.0) : 0.0},
                             {"records", baked.lightmaps.records.size()},
                             {"lightmapped_objects", sync_ ? sync_->lightmappedActors() : 0u},
                             {"probe_only_objects", stats_ready ? s.probe_only + st.probe_only : 0u},
                             {"texels_per_meter", stats_ready ? s.texels_per_meter : 0.0f},
                             {"effective_texels_per_meter", baked.lightmaps.texels_per_meter},
                             {"invalid_texels_percent",
                              stats_ready && s.valid_texels + s.invalid_texels > 0
                                  ? 100.0 * static_cast<double>(s.invalid_texels) / static_cast<double>(s.valid_texels + s.invalid_texels)
                                  : 0.0},
                             {"layouts", stats_ready ? s.layouts : 0u},
                             {"layout_cache_hits", stats_ready ? s.layout_cache_hits : 0u},
                             {"stale_records", sync_ ? sync_->staleLightmaps() : 0u},
                             {"has_irradiance", baked.lightmaps.has_irradiance},
                             {"warnings", stats_ready ? s.warnings : std::vector<std::string>{}}};
    nlohmann::json sun{{"bake_mode", renderer_.sunBakeMode() ? "baked" : "mixed"},
                       {"shadow_source", source == gfx::SunShadowSource::BakedVisibility ? "lightmap"
                                         : source == gfx::SunShadowSource::LegacyBaked   ? "legacy_probes"
                                                                                         : "cascades"},
                       {"direction_matches", gfx::bakedSunDirectionMatches(baked, scene_.lights().sun.direction)}};
    nlohmann::json timings{{"uv2", s.seconds_uv},           {"geometry", s.seconds_geometry}, {"bvh", s.seconds_bvh},
                           {"raster", s.seconds_raster},     {"validity", s.seconds_validity}, {"sun", s.seconds_sun},
                           {"indirect", s.seconds_indirect}, {"denoise", s.seconds_denoise},   {"probes", s.seconds_probes},
                           {"save", s.seconds_save}};
    const bool uv_ready = !st.uv_running || st.uv_finished.load();
    nlohmann::json uv{{"running", st.uv_running && !st.uv_finished.load()},
                      {"progress", st.uv_progress ? st.uv_progress->fraction.load() : 0.0f}};
    if (uv_ready) {
        const UvGenerationStats& u = st.uv_stats;
        uv["parts"] = u.parts;
        uv["cache_hits"] = u.cache_hits;
        uv["charts"] = u.charts;
        uv["utilization"] = u.utilization;
        uv["ignored_triangles_percent"] =
            u.triangles > 0 ? 100.0 * static_cast<double>(u.ignored_triangles) / static_cast<double>(u.triangles) : 0.0;
        uv["max_layout_size"] = u.max_layout_size;
        uv["seconds"] = u.seconds;
        uv["error"] = st.uv_error;
    }
    nlohmann::json out{{"baking", st.running},
                       {"progress", st.progress ? st.progress->fraction.load() : 0.0f},
                       {"stage", st.running && st.progress ? st.progress->currentStage() : std::string()},
                       {"mode", renderer_.lightingMode() == gfx::LightingMode::Baked ? "baked" : "realtime"},
                       {"version", baked.empty() ? 0u : baked.version},
                       {"active", renderer_.bakedGiActive() || renderer_.lightmapsActive()},
                       {"volumes", baked.volumes.size()},
                       {"probes", baked.probes.size() / gfx::kBakedProbeVec4},
                       {"probe_coverage", st.probe_coverage},
                       {"surface_texels", baked.surface.count},
                       {"sun_baked", baked.sun_baked},
                       {"baked_lights", baked.baked_lights.size()},
                       {"seconds", stats_ready ? st.seconds : 0.0},
                       {"lightmaps", lightmaps},
                       {"sun", sun},
                       {"timings", stats_ready ? timings : nlohmann::json::object()},
                       {"debug_view", kDebugViews[std::clamp(lightmap_debug_view_, 0, 3)]},
                       {"exposure_override", renderer_.exposureOverride()},
                       {"uv_generation", uv},
                       {"gather_report", error_ready ? st.report_text : std::string()},
                       {"last_error", error_ready ? st.error : std::string()}};
    return out.dump();
}

}  // namespace cramion::editor
