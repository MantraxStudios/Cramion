// Generador de terreno (Ventana > Generador de terreno): forma, erosion, rios,
// lagos, mar, capas con texturas y vegetacion, todo de una vez. Se genera en
// segundo plano (barra de progreso, Cancelar) y al terminar crea el grupo
// "Mundo generado" con el terreno, el oceano, los rios, los lagos y la
// vegetacion. Volver a generar reemplaza ese grupo.

#include "EditorApp.h"

#include "Dialogs.h"

#include <CramionCore/terrain/TerrainGenerator.h>
#include <CramionCore/water/Water.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>

namespace cramion::editor {

using core::Vec3;

namespace {

constexpr const char* kGroupName = "Mundo generado";
constexpr const char* kTextureFolder = "Terrains/Texturas";

// Valores de partida de cada forma (lo que se toca al elegirla).
void applyShapePreset(terrain::GenSettings& s, terrain::GenShape shape) {
    s.shape = shape;
    switch (shape) {
        case terrain::GenShape::Island:
            s.sea_level = 0.16f; s.mountains = 0.65f; s.ridges = 0.7f; s.hills = 0.45f; s.plateaus = 0.0f;
            s.ocean = true; s.rivers = 4;
            break;
        case terrain::GenShape::Archipelago:
            s.sea_level = 0.3f; s.mountains = 0.45f; s.ridges = 0.55f; s.hills = 0.5f; s.plateaus = 0.0f;
            s.ocean = true; s.rivers = 3;
            break;
        case terrain::GenShape::Continent:
            s.sea_level = 0.14f; s.mountains = 0.7f; s.ridges = 0.75f; s.hills = 0.5f; s.plateaus = 0.0f;
            s.ocean = true; s.rivers = 6;
            break;
        case terrain::GenShape::Mountains:
            s.sea_level = 0.05f; s.mountains = 0.9f; s.ridges = 0.85f; s.hills = 0.4f; s.plateaus = 0.0f;
            s.ocean = false; s.rivers = 4;
            break;
        case terrain::GenShape::Canyons:
            s.sea_level = 0.05f; s.mountains = 0.3f; s.ridges = 0.5f; s.hills = 0.3f; s.plateaus = 0.75f;
            s.ocean = false; s.rivers = 3;
            break;
    }
}

bool slider(const char* label, float* value, float lo, float hi, const char* format, const char* help) {
    ImGui::SetNextItemWidth(-150.0f);
    const bool changed = ImGui::SliderFloat(label, value, lo, hi, format);
    if (help != nullptr) ImGui::SetItemTooltip("%s", help);
    return changed;
}

}  // namespace

void EditorApp::drawTerrainGeneratorWindow() {
    // Terminado: se aplica en este hilo (crea entidades, sube el terreno).
    if (terrain_gen_job_ && terrain_gen_job_->done.load()) {
        TerrainGenJob& job = *terrain_gen_job_;
        if (job.thread.joinable()) job.thread.join();
        if (job.ok) {
            applyGeneratedTerrain(*job.result);
        } else if (!job.cancel.load()) {
            std::cerr << "[Generador] No se pudo generar el terreno" << std::endl;
        }
        terrain_gen_job_.reset();
    }
    if (!show_terrain_generator_) return;

    ImGui::SetNextWindowSize(ImVec2(470.0f, 720.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Generador de terreno", &show_terrain_generator_)) {
        ImGui::End();
        return;
    }
    terrain::GenSettings& s = terrain_gen_;
    const bool busy = terrain_gen_job_ != nullptr;
    ImGui::BeginDisabled(busy);

    ImGui::TextWrapped("Relieve con erosión, ríos, lagos, mar, capas texturizadas y vegetación en un solo paso. "
                       "Volver a generar reemplaza el grupo \"%s\".", kGroupName);
    ImGui::Spacing();

    // --- Forma ---
    int shape = static_cast<int>(s.shape);
    const char* shapes[terrain::kGenShapeCount] = {"Isla", "Archipiélago", "Continente", "Cordilleras (sin mar)",
                                                   "Cañones (sin mar)"};
    ImGui::SetNextItemWidth(-150.0f);
    if (ImGui::Combo("Forma", &shape, shapes, terrain::kGenShapeCount)) {
        applyShapePreset(s, static_cast<terrain::GenShape>(shape));
    }
    int seed = static_cast<int>(s.seed);
    ImGui::SetNextItemWidth(-190.0f);
    if (ImGui::InputInt("Semilla", &seed)) s.seed = static_cast<std::uint32_t>(std::max(seed, 0));
    ImGui::SameLine();
    if (ImGui::Button("Al azar")) s.seed = std::random_device{}() % 100000U;
    ImGui::SetItemTooltip("Misma semilla y mismos valores = mismo mundo.");

    if (ImGui::CollapsingHeader("Tamaño", ImGuiTreeNodeFlags_DefaultOpen)) {
        slider("Lado (m)", &s.size, 256.0f, 16384.0f, "%.0f", "Metros por lado del terreno.");
        slider("Altura máxima (m)", &s.height, 20.0f, 3000.0f, "%.0f", "Del fondo del mar a la cumbre más alta.");
        static constexpr int kResolutions[] = {257, 513, 1025, 2049};
        static constexpr const char* kResolutionNames[] = {"257 (rápido)", "513", "1025 (recomendado)", "2049 (detalle)"};
        int res_index = 2;
        for (int i = 0; i < 4; ++i) {
            if (kResolutions[i] == s.resolution) res_index = i;
        }
        ImGui::SetNextItemWidth(-150.0f);
        if (ImGui::Combo("Resolución", &res_index, kResolutionNames, 4)) {
            s.resolution = kResolutions[res_index];
            s.splat_resolution = std::max(s.resolution - 1, 256);
        }
        ImGui::SetItemTooltip("Vértices por lado. Más = más detalle y más tiempo de generación.");
        const float cell = s.size / static_cast<float>(s.resolution - 1);
        ImGui::TextDisabled("Un vértice cada %.1f m", cell);
        if (s.shape != terrain::GenShape::Mountains && s.shape != terrain::GenShape::Canyons) {
            slider("Nivel del mar", &s.sea_level, 0.02f, 0.6f, "%.2f", "Qué parte de la altura queda bajo el mar.");
        }
    }
    if (ImGui::CollapsingHeader("Relieve", ImGuiTreeNodeFlags_DefaultOpen)) {
        slider("Escala de las formas", &s.feature_scale, 0.2f, 4.0f, "%.2f", "1 = montañas de ~1 km. Más = formas más grandes.");
        slider("Montañas", &s.mountains, 0.0f, 1.0f, "%.2f", "Cuánto de la tierra son montañas.");
        slider("Crestas", &s.ridges, 0.0f, 1.0f, "%.2f", "Cumbres afiladas (0 = redondeadas).");
        slider("Colinas", &s.hills, 0.0f, 1.0f, "%.2f", "Ondulación de las llanuras.");
        slider("Deformación", &s.warp, 0.0f, 1.0f, "%.2f", "Formas menos regulares (domain warping).");
        slider("Mesetas", &s.plateaus, 0.0f, 1.0f, "%.2f", "Escalones y mesetas.");
        if (s.shape != terrain::GenShape::Mountains && s.shape != terrain::GenShape::Canyons) {
            slider("Costa irregular", &s.coast, 0.0f, 1.0f, "%.2f", "Bahías y cabos.");
        }
    }
    if (ImGui::CollapsingHeader("Erosión", ImGuiTreeNodeFlags_DefaultOpen)) {
        slider("Lluvia", &s.erosion, 0.0f, 2.0f, "%.2f", "Gotas de agua que excavan cauces y dejan sedimentos. 0 = sin erosión.");
        slider("Fuerza", &s.erosion_strength, 0.0f, 1.0f, "%.2f", "Cuánto arranca cada gota.");
        slider("Desmoronamiento", &s.thermal, 0.0f, 1.0f, "%.2f", "Las laderas demasiado empinadas se derrumban.");
        slider("Ángulo de reposo", &s.talus, 20.0f, 60.0f, "%.0f°", "A partir de este ángulo se desmorona.");
    }
    if (ImGui::CollapsingHeader("Agua", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (s.shape != terrain::GenShape::Mountains && s.shape != terrain::GenShape::Canyons) {
            ImGui::Checkbox("Océano", &s.ocean);
        }
        ImGui::SetNextItemWidth(-150.0f);
        ImGui::SliderInt("Ríos", &s.rivers, 0, 12);
        ImGui::SetItemTooltip("Ríos principales: nacen arriba y siguen el terreno hasta el mar (o el borde).");
        slider("Anchura de los ríos", &s.river_width, 0.3f, 3.0f, "%.2f", nullptr);
        slider("Profundidad de los ríos", &s.river_depth, 0.3f, 3.0f, "%.2f", nullptr);
        ImGui::Checkbox("Lagos en las cuencas", &s.lakes);
    }
    if (ImGui::CollapsingHeader("Capas")) {
        ImGui::Checkbox("Texturas realistas (procedurales)", &terrain_gen_textures_);
        ImGui::SetItemTooltip("Color y normal map de las 8 capas en Assets/%s (se crean una vez).", kTextureFolder);
        slider("Playa (m)", &s.beach_width, 0.0f, 40.0f, "%.1f", "Franja de arena sobre el mar.");
        slider("Roca desde (°)", &s.rock_slope, 15.0f, 60.0f, "%.0f", "Pendiente a partir de la que asoma la roca.");
        slider("Nieve desde", &s.snow_line, 0.2f, 1.0f, "%.2f", "Altura (0..1 sobre el mar) donde empieza la nieve. 1 = sin nieve.");
        slider("Hierba seca", &s.dry_grass, 0.0f, 1.0f, "%.2f", nullptr);
    }
    if (ImGui::CollapsingHeader("Vegetación")) {
        ImGui::Checkbox("Hierba (GPU)", &terrain_gen_grass_);
        ImGui::SetItemTooltip("Millones de briznas alrededor de la cámara, con viento y que se apartan con la física.");
        if (terrain_gen_grass_) {
            slider("Briznas por m2", &terrain_gen_grass_density_, 5.0f, 150.0f, "%.0f", nullptr);
        }
        ImGui::Checkbox("Árboles", &terrain_gen_trees_);
        if (terrain_gen_trees_) {
            slider("Árboles por hectárea", &terrain_gen_tree_density_, 5.0f, 800.0f, "%.0f", nullptr);
        }
    }
    if (ImGui::CollapsingHeader("Aldea")) {
        ImGui::SetNextItemWidth(-150.0f);
        ImGui::SliderInt("Casas", &terrain_gen_houses_, 0, 40);
        ImGui::SetItemTooltip("Cabañas y casas procedurales en un sitio llano y seco, mirando a la plaza (0 = sin aldea). "
                              "Ventana > Generador de casas para hacerlas a mano.");
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    if (busy) {
        TerrainGenJob& job = *terrain_gen_job_;
        std::string stage;
        {
            std::lock_guard<std::mutex> lock(job.mutex);
            stage = job.stage;
        }
        ImGui::ProgressBar(job.progress.load(), ImVec2(-90.0f, 0.0f), stage.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Cancelar", ImVec2(-1.0f, 0.0f))) job.cancel = true;
    } else {
        ImGui::BeginDisabled(!has_project_ || playing());
        if (ImGui::Button("Generar", ImVec2(-1.0f, 36.0f))) startTerrainGeneration();
        ImGui::EndDisabled();
        if (playing()) ImGui::TextDisabled("Para el modo Play para generar.");
    }
    ImGui::End();
}

void EditorApp::startTerrainGeneration() {
    if (terrain_gen_job_ || !has_project_) return;
    auto job = std::make_unique<TerrainGenJob>();
    job->settings = terrain_gen_;
    job->textures = terrain_gen_textures_;
    job->texture_folder = dialogs::utf8(project_.assetsFolder() / dialogs::fromUtf8(kTextureFolder));
    TerrainGenJob* raw = job.get();
    job->thread = std::thread([raw]() {
        raw->result = std::make_unique<terrain::GenResult>();
        const auto report = [raw](float t, const char* stage) {
            raw->progress = t;
            std::lock_guard<std::mutex> lock(raw->mutex);
            raw->stage = stage;
            return !raw->cancel.load();
        };
        raw->ok = terrain::generateTerrain(raw->settings, *raw->result, [&](float t, const char* stage) {
            return report(t * 0.92f, stage);
        });
        // Las texturas de las capas: una vez por proyecto.
        if (raw->ok && raw->textures && !terrain::generatorTexturesCurrent(raw->texture_folder)) {
            report(0.93f, "Texturas de las capas");
            terrain::writeGeneratorTextures(raw->texture_folder, 1024, 7);
        }
        raw->progress = 1.0f;
        raw->done = true;
    });
    terrain_gen_job_ = std::move(job);
}

void EditorApp::applyGeneratedTerrain(terrain::GenResult& result) {
    const terrain::GenSettings& s = terrain_gen_job_ ? terrain_gen_job_->settings : terrain_gen_;
    const bool textures = terrain_gen_job_ ? terrain_gen_job_->textures : terrain_gen_textures_;

    // Reemplazar lo generado antes (el grupo entero).
    if (ecs::Entity old = world_.findByName(kGroupName); old.valid()) world_.destroy(old);
    ecs::Entity group = world_.create(kGroupName);
    group.setWorldPosition(Vec3{0.0f, 0.0f, 0.0f});

    // --- Terreno ---
    terrain::Terrain comp;
    comp.data = "Terrains/Mundo generado.crterrain";
    comp.size = s.size;
    comp.height = s.height;
    comp.resolution = s.resolution;
    comp.splat_resolution = static_cast<int>(result.data.splatResolution());
    comp.layers = terrain::generatorLayers(textures ? kTextureFolder : "");
    comp.lod_distance = 2.0f;
    ecs::Entity terrain_entity = world_.create("Terreno", group);
    terrain_entity.setWorldPosition(result.origin);
    terrain::Terrain& added = terrain_entity.add<terrain::Terrain>();
    added = comp;
    if (const std::shared_ptr<terrain::TerrainData> data = terrain_store_.get(added)) {
        *data = std::move(result.data);
        data->markAll();
        data->commitCollision();
    }
    terrain_store_.saveAll();
    // Hierba sobre la capa de hierba (la seca la amarillea).
    if (terrain_gen_grass_) {
        foliage::Grass& grass = terrain_entity.add<foliage::Grass>();
        grass.layer = terrain::kGenLayerGrass;
        grass.dry_layer = terrain::kGenLayerDryGrass;
        grass.density = terrain_gen_grass_density_;
    }

    // --- Agua ---
    if (s.ocean) {
        ecs::Entity ocean = world_.create("Oceano", group);
        ocean.setWorldPosition(Vec3{0.0f, 0.0f, 0.0f});
        ocean.add<water::WaterBody>() = water::oceanPreset();
    }
    for (std::size_t i = 0; i < result.rivers.size(); ++i) {
        ecs::Entity river = world_.create("Rio " + std::to_string(i + 1), group);
        river.setWorldPosition(Vec3{0.0f, 0.0f, 0.0f});
        water::WaterBody body = water::riverPreset();
        body.points.clear();
        const terrain::GenRiver& r = result.rivers[i];
        for (std::size_t p = 0; p < r.points.size(); ++p) {
            water::RiverPoint point;
            point.position = r.points[p];
            point.width = r.widths[p];
            body.points.push_back(point);
        }
        river.add<water::WaterBody>() = body;
    }
    for (std::size_t i = 0; i < result.lakes.size(); ++i) {
        ecs::Entity lake = world_.create("Lago " + std::to_string(i + 1), group);
        lake.setWorldPosition(result.lakes[i].center);
        water::WaterBody body = water::lakePreset();
        body.size = result.lakes[i].size;
        lake.add<water::WaterBody>() = body;
    }

    // --- Vegetacion ---
    if (terrain_gen_trees_) {
        ecs::Entity forest = world_.create("Vegetacion", group);
        forest.setWorldPosition(Vec3{0.0f, 0.0f, 0.0f});
        foliage::Foliage& f = forest.add<foliage::Foliage>();
        f.area = s.size;
        f.density = terrain_gen_tree_density_;
        f.seed = static_cast<int>(s.seed);
        f.min_height = s.ocean ? s.beach_width + 1.5f : result.origin.y + 1.0f;
        f.max_height = (s.snow_line * (1.0f - s.sea_level) * s.height) * 0.9f;
        f.max_slope = std::min(s.rock_slope - 4.0f, 34.0f);
        f.max_instances = 6000000;
    }

    // --- Aldea (casas procedurales, fuera del agua) ---
    if (terrain_gen_houses_ > 0) {
        std::string error;
        if (placeVillage(terrain_gen_houses_, s.seed, group, &error) == 0) {
            std::cerr << "[Generador] Sin aldea: " << error << std::endl;
        }
    }

    // La camara del juego tambien ve el mundo entero.
    for (const entt::entity h : world_.registry().view<ecs::Camera>()) {
        ecs::Camera& cam = world_.registry().get<ecs::Camera>(h);
        cam.far_plane = std::max(cam.far_plane, s.size * 1.6f + s.height);
    }

    // Camara: vista general del mundo.
    const float extent = s.size * 0.55f;
    scene_.placeCamera(Vec3{extent * 0.55f, s.height * 0.9f + extent * 0.25f, extent * 0.9f}, Vec3{0.0f, 0.0f, 0.0f});
    refreshDatabase();
    selectOnly(terrain_entity.uuid());
    revealInHierarchy(terrain_entity.uuid());
    commit();
    char line[256];
    std::snprintf(line, sizeof(line), "[Generador] %s de %.0f m en %.1f s: %zu rios, %zu lagos, erosion %.0f m3",
                  terrain::genShapeName(s.shape), s.size, result.seconds, result.rivers.size(), result.lakes.size(),
                  result.erosion_volume);
    std::cout << line << std::endl;
}

}  // namespace cramion::editor
