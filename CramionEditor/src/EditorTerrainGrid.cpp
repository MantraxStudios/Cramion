// Terreno grande por teselas (TerrainGrid) en el editor:
//
//   - GameObject > Terreno grande (teselas) crea uno de 12 x 12 km (8 x 8
//     teselas de 1536 m con 1025 x 1025 alturas: 1,5 m por vertice).
//   - El Inspector importa un mapa de alturas (RAW/R16 de 16 bits, R32 o PNG
//     de 16 bits) o pasa un terreno de la escena a las teselas, en otro hilo
//     y con barra de progreso.
//   - En el editor solo se cargan las teselas cerca de la camara (y en Play,
//     las de las Fuentes de carga): esculpir y pintar funcionan en cada
//     tesela; al terminar el trazo sus bordes pasan a las vecinas.

#include "EditorApp.h"

#include "Dialogs.h"

#include <CramionCore/cvar/CVar.h>

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>

namespace cramion::editor {

using core::Vec3;

ecs::Entity EditorApp::createTerrainGridEntity() {
    terrain::TerrainGrid grid;
    // Carpeta libre de verdad: ni en disco ni usada por otra rejilla de esta
    // escena. Y se crea ya: una rejilla sin esculpir no escribe nada, y antes
    // la de otra escena volvia a recibir "Terrains/Mundo" (las dos escenas
    // compartian teselas: lo de una aparecia en la otra).
    const auto taken = [&](const std::string& candidate) {
        std::error_code ec;
        if (std::filesystem::exists(project_.assetsFolder() / dialogs::fromUtf8(candidate), ec)) return true;
        for (const entt::entity h : world_.registry().view<terrain::TerrainGrid>()) {
            if (world_.registry().get<terrain::TerrainGrid>(h).folder == candidate) return true;
        }
        return false;
    };
    std::string folder = "Terrains/Mundo";
    for (int i = 2; taken(folder); ++i) folder = "Terrains/Mundo " + std::to_string(i);
    {
        std::error_code ec;
        std::filesystem::create_directories(project_.assetsFolder() / dialogs::fromUtf8(folder), ec);
    }
    grid.folder = folder;
    ecs::Entity entity = world_.create("Terreno grande");
    // Centrado en el origen. Las teselas que no existen salen planas a 0.
    entity.setWorldPosition(Vec3{-grid.worldSizeX() * 0.5f, 0.0f, -grid.worldSizeZ() * 0.5f});
    entity.add<terrain::TerrainGrid>() = grid;
    selectOnly(entity.uuid());
    revealInHierarchy(entity.uuid());
    commit();
    pushToast("Terreno grande creado", "Importa un mapa de alturas desde el Inspector (RAW o PNG de 16 bits)");
    return entity;
}

void EditorApp::updateTerrainStreaming() {
    pollGridImport();
    // En el editor, la camara; en Play, tambien las Fuentes de carga. Lo
    // esculpido se guarda al soltar la tesela (en Play se queda en memoria).
    // Los presupuestos (ms por frame en el hilo principal, teselas en vuelo,
    // MB subidos por frame) son CVars del streamer: stream.MainBudgetMs,
    // stream.MaxInFlight, stream.UploadMBPerFrame.
    const auto start = std::chrono::steady_clock::now();
    // Bytes por frame para subir terrenos a la GPU (cambios y teselas nuevas).
    static cvar::CVarBase* upload_mb = nullptr;
    static std::uint64_t upload_version = ~0ull;
    if (upload_mb == nullptr) upload_mb = cvar::Registry::instance().find("stream.UploadMBPerFrame");
    if (upload_mb != nullptr && upload_mb->version() != upload_version) {
        upload_version = upload_mb->version();
        double mb = 16.0;
        if (!cvar::detail::parseNumber(upload_mb->toString(), mb)) mb = 16.0;
        renderer_.setTerrainUploadBudget(static_cast<std::size_t>(std::max(mb, 0.25) * 1024.0 * 1024.0));
    }
    terrain_streamer_.update(world_, terrain_store_, scene_.camera().position(), playing(), !playing());
    frame_streaming_ms_ +=
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void EditorApp::startGridImport(ecs::Entity grid_entity, terrain::Heightmap map, const terrain::GridImportOptions& options) {
    if (grid_import_ || !grid_entity.valid() || !grid_entity.has<terrain::TerrainGrid>()) return;
    auto job = std::make_unique<GridImport>();
    job->grid = grid_entity.uuid();
    job->progress = std::make_shared<terrain::GridImportProgress>();
    const terrain::TerrainGrid grid = grid_entity.get<terrain::TerrainGrid>();
    job->folder = grid.folder;
    // Lo que haya cargado de esta rejilla se suelta (se va a reescribir) y no
    // se vuelve a leer hasta que acabe: el streaming abriendo los archivos a
    // la vez que el otro hilo los escribe hace fallar el rename en Windows.
    terrain_streamer_.pauseFolder(grid.folder, true);
    terrain_streamer_.clear(world_, terrain_store_, false);
    for (int z = 0; z < grid.tiles_z; ++z) {
        for (int x = 0; x < grid.tiles_x; ++x) terrain_store_.release(terrain::tilePath(grid, x, z));
    }
    job->job = std::async(std::launch::async, [map = std::move(map), grid, root = project_.assetsFolder(), options,
                                               progress = job->progress]() {
        std::string error;
        if (!terrain::writeTerrainGrid(map, grid, root, options, progress.get(), &error)) return error.empty() ? std::string("error") : error;
        return std::string{};
    });
    grid_import_ = std::move(job);
}

void EditorApp::pollGridImport() {
    if (!grid_import_ || !grid_import_->job.valid()) return;
    if (grid_import_->job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    const std::string error = grid_import_->job.get();
    // Las teselas viejas que se hayan colado mientras tanto, fuera.
    terrain_streamer_.clear(world_, terrain_store_, false);
    for (const entt::entity h : world_.registry().view<terrain::TerrainGrid>()) {
        const terrain::TerrainGrid& g = world_.registry().get<terrain::TerrainGrid>(h);
        if (g.folder != grid_import_->folder) continue;
        for (int z = 0; z < g.tiles_z; ++z) {
            for (int x = 0; x < g.tiles_x; ++x) terrain_store_.release(terrain::tilePath(g, x, z));
        }
    }
    terrain_streamer_.pauseFolder(grid_import_->folder, false);
    if (error.empty()) {
        pushToast("Terreno importado", std::to_string(grid_import_->progress->total.load()) + " teselas en " + grid_import_->folder);
        refreshDatabase();
    } else {
        pushToast("No se pudo importar el terreno", error, 3);
        std::cerr << "[Terreno] Importar: " << error << "\n";
    }
    grid_import_.reset();
}

void EditorApp::drawTerrainGridInspector(ecs::Entity entity) {
    terrain::TerrainGrid& grid = entity.get<terrain::TerrainGrid>();
    const float metres_per_vertex = grid.tile_size / static_cast<float>(grid.tileResolution() - 1);
    ImGui::SeparatorText("Mundo");
    ImGui::Text("%.2f x %.2f km  |  %.2f m por vertice", static_cast<double>(grid.worldSizeX()) / 1000.0,
                static_cast<double>(grid.worldSizeZ()) / 1000.0, static_cast<double>(metres_per_vertex));
    ImGui::TextDisabled("%u x %u alturas en total  |  %d teselas de %u x %u", grid.worldResolutionX(),
                        grid.worldResolutionZ(), grid.tiles_x * grid.tiles_z, grid.tileResolution(), grid.tileResolution());
    const double tile_mb = (static_cast<double>(grid.tileResolution()) * grid.tileResolution() * 4.0 +
                            static_cast<double>(grid.splat_resolution) * grid.splat_resolution * 8.0) /
                           (1024.0 * 1024.0);
    ImGui::TextDisabled("%.1f MB por tesela  |  %.2f GB en disco el mundo entero", tile_mb,
                        tile_mb * grid.tiles_x * grid.tiles_z / 1024.0);
    const terrain::TerrainStreamStats& s = terrain_streamer_.stats();
    ImGui::Text("Cargadas: %d teselas (%.0f MB)%s", s.tiles_loaded, static_cast<double>(s.bytes) / (1024.0 * 1024.0),
                s.tiles_loading > 0 ? "  leyendo..." : "");
    ImGui::SetItemTooltip("Solo estan en memoria las teselas a la distancia de carga de la camara (en Play, de las\n"
                          "Fuentes de carga). El juego y el servidor hacen lo mismo con cada jugador.");

    ImGui::SeparatorText("Importar relieve");
    if (grid_import_) {
        const int done = grid_import_->progress->done.load();
        const int total = std::max(grid_import_->progress->total.load(), 1);
        char label[64];
        std::snprintf(label, sizeof(label), "Teselas %d / %d", done, total);
        ImGui::ProgressBar(static_cast<float>(done) / static_cast<float>(total), ImVec2(-1.0f, 0.0f), label);
        if (ImGui::Button("Cancelar", ImVec2(-1.0f, 0.0f))) grid_import_->progress->cancel = true;
        return;
    }
    ImGui::Checkbox("Pintar roca en las pendientes", &grid_import_paint_);
    ImGui::SameLine();
    ImGui::Checkbox("Voltear en Z", &grid_import_flip_);
    ImGui::SetItemTooltip("La fila de arriba de la imagen pasa a ser el norte (+Z). Usalo si el mapa sale al reves.");
    terrain::GridImportOptions options;
    options.auto_paint = grid_import_paint_;
    if (ImGui::Button("Importar mapa de alturas (RAW / PNG 16 bits)...", ImVec2(-1.0f, 0.0f))) {
        const std::filesystem::path file = dialogs::openFile(
            window_.handle(),
            L"Mapas de alturas (*.raw;*.r16;*.r32;*.png;*.tga;*.bmp)\0*.raw;*.r16;*.r32;*.png;*.tga;*.bmp\0Todos\0*.*\0",
            project_.assetsFolder());
        if (!file.empty()) {
            terrain::Heightmap map;
            std::string error;
            if (!terrain::loadHeightmap(file, map, &error)) {
                pushToast("No se pudo leer el mapa de alturas", error, 3);
            } else {
                if (grid_import_flip_) terrain::flipHeightmapZ(map);
                std::cout << "[Terreno] " << dialogs::utf8(file.filename()) << ": " << map.width << " x " << map.height
                          << " -> " << grid.worldResolutionX() << " x " << grid.worldResolutionZ() << " vertices\n";
                if (map.source_min != 0.0f || map.source_max != 1.0f) {
                    std::cout << "[Terreno] Valores del archivo de " << map.source_min << " a " << map.source_max << "\n";
                }
                startGridImport(entity, std::move(map), options);
            }
        }
    }
    ImGui::SetItemTooltip("Se reparte entre las teselas (se amplia o reduce al tamano del mundo, con filtro bicubico).\n"
                          "Los bordes de las teselas salen identicos: sin grietas.");
    // Desde un terreno normal de la escena (el del generador, por ejemplo).
    std::vector<ecs::Entity> sources;
    for (const entt::entity h : world_.registry().view<terrain::Terrain>()) {
        const ecs::Entity e = world_.wrap(h);
        if (!e.get<ecs::EntityInfo>().transient) sources.push_back(e);
    }
    if (!sources.empty() && ImGui::Button("Desde un terreno de la escena...", ImVec2(-1.0f, 0.0f))) {
        ImGui::OpenPopup("grid_from_terrain");
    }
    ImGui::SetItemTooltip("Pasa un terreno normal (el del Generador, por ejemplo) a las teselas, ampliado al tamano del mundo.");
    if (ImGui::BeginPopup("grid_from_terrain")) {
        for (const ecs::Entity& e : sources) {
            // Id por entidad: los terrenos suelen llamarse todos "Terreno".
            ImGui::PushID(static_cast<int>(entt::to_integral(e.handle())));
            const bool picked = ImGui::MenuItem(e.name().c_str());
            ImGui::PopID();
            if (!picked) continue;
            const terrain::Terrain& t = e.get<terrain::Terrain>();
            if (const std::shared_ptr<terrain::TerrainData> data = terrain_store_.get(t)) {
                grid.height = t.height;  // la misma escala de alturas
                Vec3 p = entity.worldPosition();
                p.y = e.worldPosition().y;
                entity.setWorldPosition(p);
                commit();
                startGridImport(entity, terrain::heightmapFromTerrain(*data), options);
            }
        }
        ImGui::EndPopup();
    }
    ImGui::TextDisabled("Capas, hierba (Grass en esta entidad), sombras y colision valen para todas las teselas.");
}

}  // namespace cramion::editor
