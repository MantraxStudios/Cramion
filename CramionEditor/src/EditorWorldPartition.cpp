// World Partition en el editor: estadisticas en el Inspector (cuantos objetos
// se descargarian, celdas cargadas en Play, memoria guardada), las rejillas,
// las Data Layers (verlas u ocultarlas en la Escena, activarlas en Play), las
// dependencias entre celdas y la rejilla de celdas en la Escena (verde =
// cargada, roja = descargada).

#include "EditorApp.h"

#include "Dialogs.h"

#include <CramionFX/asset/ProxyMesh.h>

#include <ImGuizmo.h>
#include <imgui.h>

#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace cramion::editor {

using core::Vec3;
using worldpart::WorldPartitionSystem;

namespace {

// El World Partition de la escena (el primero), para leerlo o cambiarlo.
worldpart::WorldPartition* scenePartition(ecs::World& world) {
    for (const entt::entity h : world.registry().view<worldpart::WorldPartition>()) {
        return &world.registry().get<worldpart::WorldPartition>(h);
    }
    return nullptr;
}

ecs::Entity rootOf(ecs::Entity e) {
    while (e.valid() && e.parent().valid()) e = e.parent();
    return e;
}

std::string joinLayers(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& n : names) {
        if (!out.empty()) out += ',';
        out += n;
    }
    return WorldPartitionSystem::canonicalLayers(out);
}

}  // namespace

// Estado de la ventana "Regiones cargadas" (ver mas abajo).
struct EditorApp::LoadedRegionsUi {
    // Preferencias de la escena (Library/WorldPartition/<uuid>.regions).
    bool enabled = true;
    worldpart::WorldPartitionSystem::EditorRegions regions;
    std::vector<std::pair<std::string, worldpart::CellCoord>> pins;  // rejilla por nombre
    // Vista de la cuadricula: centro (en celdas) y pixeles por celda.
    int grid = 0;
    float zoom = 14.0f;
    float pan_x = 0.0f;
    float pan_z = 0.0f;
    bool follow_camera = true;
    bool dragged = false;  // el clic actual se arrastro: al soltar no fija la celda
    bool selecting = false;
    bool has_selection = false;
    int sel_x0 = 0;
    int sel_z0 = 0;
    int sel_x1 = 0;
    int sel_z1 = 0;
    std::vector<worldpart::CellInfo> cells;  // se reutiliza cada frame
    // loadEpoch() la ultima vez que el editor tuvo el mundo: lo que lo rehace
    // (parar Play, volver de una pestana) es una instantanea de despues, con
    // las celdas cargadas hasta entonces enteras.
    std::uint64_t seen_epoch = 0;
    // El mundo se fue a Play o a una pestana (la vuelta es de seen_epoch). Si
    // se rehace sin irse (el autoguardado recuperado), la instantanea es de no
    // se sabe cuando: se recupera todo lo cargado (de mas, nunca de menos).
    bool away = false;
    std::uint64_t synced_changes = 0;  // worldChanges() que ya tiene el historial de deshacer
    int frame = 0;
};

void EditorApp::drawWorldPartitionInspector(ecs::Entity entity) {
    worldpart::WorldPartition* wp = entity.tryGet<worldpart::WorldPartition>();
    if (wp == nullptr) return;
    if (world_partition_.active()) {
        const worldpart::PartitionStats& s = world_partition_.stats();
        ImGui::Text("Celdas cargadas: %d de %d", s.loaded_cells, s.cells);
        ImGui::Text("Objetos descargados: %d de %d", s.unloaded_entities, s.streamed_entities);
        ImGui::Text("En disco: %.1f KB  |  en memoria: %.1f KB", static_cast<double>(s.disk_bytes) / 1024.0,
                    static_cast<double>(s.stored_bytes) / 1024.0);
        ImGui::Text("Cargas %d, descargas %d%s", s.loads, s.unloads, s.loading_cells > 0 ? "  (leyendo...)" : "");
        // Por rejilla (si hay mas de una).
        const worldpart::WorldPartition& settings = world_partition_.settings();
        if (!settings.grids.empty()) {
            std::vector<std::pair<int, int>> per_grid(settings.grids.size() + 1, {0, 0});  // cargadas, total
            for (const worldpart::CellInfo& c : world_partition_.cellInfos()) {
                if (c.grid < 0 || c.grid >= static_cast<int>(per_grid.size())) continue;
                per_grid[static_cast<std::size_t>(c.grid)].first += c.loaded ? 1 : 0;
                ++per_grid[static_cast<std::size_t>(c.grid)].second;
            }
            for (std::size_t g = 0; g < per_grid.size(); ++g) {
                const std::string name = g == 0 ? std::string("Principal") : WorldPartitionSystem::gridName(settings, static_cast<int>(g));
                ImGui::TextDisabled("  %s: %d de %d celdas", name.c_str(), per_grid[g].first, per_grid[g].second);
            }
        }
        // Data Layers en el juego: activarlas y apagarlas (como WorldPartition.setDataLayer).
        const auto layers = world_partition_.dataLayers();
        if (!layers.empty()) {
            ImGui::SeparatorText("Data Layers (en Play)");
            for (const auto& [name, on] : layers) {
                bool value = on;
                ImGui::PushID(name.c_str());
                if (ImGui::Checkbox(name.c_str(), &value)) world_partition_.setDataLayerActive(name, value);
                ImGui::PopID();
            }
        }
        return;
    }
    if (wp->external_cells) {
        if (scene_path_.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Guarda la escena para crear sus celdas en disco");
        } else {
            ImGui::TextDisabled("Al guardar: %s/ (un archivo por celda)",
                                dialogs::utf8(WorldPartitionSystem::cellsFolder(scene_path_).filename()).c_str());
        }
        ImGui::TextDisabled("El juego y el servidor solo leen las celdas cercanas a cada jugador.");
        // Regiones cargadas: el editor tampoco tiene la escena entera.
        if (editor_regions_.active()) {
            const worldpart::PartitionStats& rs = editor_regions_.stats();
            ImGui::TextDisabled("En el editor: %d de %d celdas cargadas (lo de abajo cuenta solo esas)", rs.loaded_cells,
                                rs.cells);
        } else if (!scene_path_.empty()) {
            ImGui::TextDisabled("En el editor: la escena entera (regiones cargadas apagadas)");
        }
        if (ImGui::Button("Regiones cargadas...", ImVec2(-1.0f, 0.0f))) show_loaded_regions_ = true;
    }
    // Fuera de Play: lo que pasaria al darle, por rejilla y por capa.
    struct GridCount {
        int objects = 0;
        std::set<std::pair<int, int>> cells;
    };
    std::vector<GridCount> grids(wp->grids.size() + 1);
    std::map<std::string, int> per_layer;
    int streamable = 0;
    world_.forEachDepthFirst([&](ecs::Entity e) {
        if (e.parent().valid() || !WorldPartitionSystem::streamable(e)) return;
        ++streamable;
        const int g = WorldPartitionSystem::gridOf(*wp, e);
        GridCount& count = grids[static_cast<std::size_t>(g)];
        ++count.objects;
        const worldpart::CellCoord c =
            WorldPartitionSystem::cellAt(world_.absolute(e.worldPosition()), WorldPartitionSystem::gridCellSize(*wp, g));
        count.cells.insert({c.x, c.z});
        for (const std::string& layer : WorldPartitionSystem::splitLayers(WorldPartitionSystem::layersOf(e))) ++per_layer[layer];
    });
    int total_cells = 0;
    for (const GridCount& g : grids) total_cells += static_cast<int>(g.cells.size());
    ImGui::TextDisabled("En Play: %d objetos en %d celdas se cargan y descargan", streamable, total_cells);
    if (!wp->grids.empty()) {
        for (std::size_t g = 0; g < grids.size(); ++g) {
            const int index = static_cast<int>(g);
            const std::string name = g == 0 ? std::string("Principal") : WorldPartitionSystem::gridName(*wp, index);
            ImGui::TextDisabled("  %s (%.0f m, carga a %.0f m): %d objetos en %d celdas", name.empty() ? "(sin nombre)" : name.c_str(),
                                static_cast<double>(WorldPartitionSystem::gridCellSize(*wp, index)),
                                static_cast<double>(WorldPartitionSystem::gridLoadRange(*wp, index)), grids[g].objects,
                                static_cast<int>(grids[g].cells.size()));
        }
        ImGui::TextDisabled("Un objeto va a la rejilla de su \"Objeto del World Partition\" o a la de su etiqueta.");
    }
    ImGui::TextDisabled("Sin Fuente de carga, carga la camara. \"Siempre cargado\" lo excluye.");

    // Data Layers: verlas en la Escena y activas al empezar (lo mismo que la
    // lista de abajo, a mano).
    if (!wp->data_layers.empty() || !per_layer.empty()) {
        ImGui::SeparatorText("Data Layers");
        std::set<std::string> declared;
        for (std::size_t i = 0; i < wp->data_layers.size(); ++i) {
            worldpart::DataLayer& layer = wp->data_layers[i];
            const std::string name = WorldPartitionSystem::canonicalLayers(layer.name);  // sin espacios
            declared.insert(name);
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Checkbox("##visible", &layer.editor_visible)) commit();
            ImGui::SetItemTooltip("Visible en la Escena del editor (no cambia la escena guardada)");
            ImGui::SameLine();
            if (ImGui::Checkbox("##active", &layer.active)) commit();
            ImGui::SetItemTooltip("Activa al empezar Play / el juego");
            ImGui::SameLine();
            const auto it = per_layer.find(name);
            ImGui::Text("%s  (%d objetos)", name.empty() ? "(sin nombre)" : name.c_str(), it != per_layer.end() ? it->second : 0);
            ImGui::PopID();
        }
        std::string undeclared;
        for (const auto& [name, count] : per_layer) {
            if (declared.count(name) != 0) continue;
            if (!undeclared.empty()) undeclared += ", ";
            undeclared += name;
        }
        if (!undeclared.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Capas sin declarar (siempre activas): %s", undeclared.c_str());
            if (ImGui::Button("Declararlas", ImVec2(-1.0f, 0.0f))) {
                for (const auto& [name, count] : per_layer) {
                    if (declared.count(name) != 0) continue;
                    worldpart::DataLayer layer;
                    layer.name = name;
                    wp->data_layers.push_back(layer);
                }
                commit();
            }
        }
        ImGui::TextDisabled("Las capas de un objeto, en su \"Objeto del World Partition\".");
    }
    if (!wp->dependencies.empty()) {
        ImGui::TextDisabled("%d dependencias entre celdas (una celda que carga otra).", static_cast<int>(wp->dependencies.size()));
    }

    if (ImGui::Button("Construir HLOD (mallas lejanas por celda)", ImVec2(-1.0f, 0.0f))) {
        // Con regiones cargadas, las celdas en disco tambien (si no, su HLOD
        // se borraria sin rehacerse). Lo lejano vuelve a salir poco a poco.
        if (editor_regions_.active() && editor_regions_.stats().loaded_cells < editor_regions_.stats().cells) {
            editor_regions_.loadAll(world_);
            history_.sync(world_);
        }
        const int built = buildHlods(*wp);
        pushToast(built > 0 ? "HLOD: " + std::to_string(built) + " celdas" : "HLOD: no hay mallas estaticas que juntar");
    }
    ImGui::SetItemTooltip("Junta las mallas de cada celda en una sola muy simplificada (Assets/HLOD/<escena>/).\n"
                          "En Play se ve en lugar de la celda cuando esta descargada (el pueblo se sigue viendo de lejos).\n"
                          "Solo la rejilla principal y los objetos sin Data Layers.");
    bool has_source = !world_.registry().view<worldpart::StreamingSource>().empty();
    if (!has_source) {
        ecs::Entity player = world_.findWithTag("Player");
        if (player.valid() && ImGui::Button("Anadir Fuente de carga al jugador", ImVec2(-1.0f, 0.0f))) {
            player.add<worldpart::StreamingSource>();
            commit();
        }
    }
}

void EditorApp::drawPartitionObjectInspector(ecs::Entity entity) {
    worldpart::PartitionObject* po = entity.tryGet<worldpart::PartitionObject>();
    if (po == nullptr) return;
    worldpart::WorldPartition* wp = scenePartition(world_);
    if (wp == nullptr) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "No hay World Partition en la escena: no hace nada");
        return;
    }
    const ecs::Entity root = rootOf(entity);
    if (root != entity) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Solo cuenta en la raiz (el objeto sin padre)");
        return;
    }
    if (!WorldPartitionSystem::streamable(root)) {
        ImGui::TextDisabled("Este objeto no se descarga (camara, jugador, red, Siempre cargado...)");
    }
    if (!po->grid.empty() && WorldPartitionSystem::findGrid(*wp, po->grid) < 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "No hay ninguna rejilla \"%s\"", po->grid.c_str());
    }
    const int grid = WorldPartitionSystem::gridOf(*wp, root);
    const worldpart::CellCoord cell =
        WorldPartitionSystem::cellAt(world_.absolute(root.worldPosition()), WorldPartitionSystem::gridCellSize(*wp, grid));
    const std::string grid_name = WorldPartitionSystem::gridName(*wp, grid);
    ImGui::TextDisabled("Rejilla %s, celda %d, %d", grid == 0 ? "principal" : grid_name.c_str(), cell.x, cell.z);

    // Capas: una casilla por cada una declarada en el World Partition.
    if (!wp->data_layers.empty()) {
        std::vector<std::string> mine = WorldPartitionSystem::splitLayers(WorldPartitionSystem::canonicalLayers(po->data_layers));
        for (std::size_t i = 0; i < wp->data_layers.size(); ++i) {
            const std::string name = WorldPartitionSystem::canonicalLayers(wp->data_layers[i].name);
            if (name.empty()) continue;
            bool in = std::find(mine.begin(), mine.end(), name) != mine.end();
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Checkbox(name.c_str(), &in)) {
                if (in) mine.push_back(name);
                else mine.erase(std::remove(mine.begin(), mine.end(), name), mine.end());
                po->data_layers = joinLayers(mine);
                commit();
            }
            ImGui::PopID();
        }
    }

    // Dependencias: la celda de este objeto carga la de los otros seleccionados.
    int loads = 0;
    for (const worldpart::CellDependency& d : wp->dependencies) {
        if (WorldPartitionSystem::findGrid(*wp, d.grid) == grid && d.x == cell.x && d.z == cell.z) ++loads;
    }
    if (loads > 0) ImGui::TextDisabled("Su celda carga %d celda(s) mas al cargarse.", loads);
    std::vector<ecs::Entity> others;
    for (const ecs::Entity& s : selectedEntities()) {
        const ecs::Entity r = rootOf(s);
        if (r.valid() && r != root && std::find(others.begin(), others.end(), r) == others.end()) others.push_back(r);
    }
    if (!others.empty()) {
        const std::string label = "Su celda carga la de los otros seleccionados (" + std::to_string(others.size()) + ")";
        if (ImGui::Button(label.c_str(), ImVec2(-1.0f, 0.0f))) {
            int added = 0;
            for (const ecs::Entity& other : others) {
                const int g = WorldPartitionSystem::gridOf(*wp, other);
                const worldpart::CellCoord c = WorldPartitionSystem::cellAt(world_.absolute(other.worldPosition()),
                                                                            WorldPartitionSystem::gridCellSize(*wp, g));
                if (g == grid && c == cell) continue;  // la misma celda
                worldpart::CellDependency d;
                d.grid = grid_name;
                d.x = cell.x;
                d.z = cell.z;
                d.load_grid = WorldPartitionSystem::gridName(*wp, g);
                d.load_x = c.x;
                d.load_z = c.z;
                const bool exists = std::any_of(wp->dependencies.begin(), wp->dependencies.end(), [&](const worldpart::CellDependency& o) {
                    return o.grid == d.grid && o.x == d.x && o.z == d.z && o.load_grid == d.load_grid && o.load_x == d.load_x &&
                           o.load_z == d.load_z;
                });
                if (exists) continue;
                wp->dependencies.push_back(d);
                ++added;
            }
            if (added > 0) commit();
            pushToast(added > 0 ? "World Partition: " + std::to_string(added) + " dependencia(s) nueva(s)"
                                : "World Partition: ya estaban (o es la misma celda)");
        }
        ImGui::SetItemTooltip("Cuando la celda de este objeto se carga, tambien las de los otros (el interior de un edificio\n"
                              "con su fachada, las dos orillas de un puente). Se guarda en el World Partition.");
    }
}

// Data Layers con "Visible en el editor" apagado: sus objetos no se dibujan en
// la Escena. Solo las raices con PartitionObject (y sus hijos). En Play no se
// oculta nada (manda el estado de la capa en el juego).
void EditorApp::updateDataLayerVisibility() {
    std::set<std::string> hidden;
    if (!playing()) {
        if (const worldpart::WorldPartition* wp = scenePartition(world_); wp != nullptr && wp->enabled) {
            for (const worldpart::DataLayer& layer : wp->data_layers) {
                const std::string name = WorldPartitionSystem::canonicalLayers(layer.name);
                if (!layer.editor_visible && !name.empty()) hidden.insert(name);
            }
        }
    }
    if (hidden.empty() && data_layer_hidden_.empty()) return;  // lo normal: nada que hacer
    std::string signature;
    for (const std::string& name : hidden) signature += name + '\n';
    const bool structure_changed = world_.structureVersion() != data_layer_structure_;
    // Las capas de un objeto cambian sin tocar la estructura (el Inspector):
    // un repaso completo cada medio segundo, mas o menos.
    const bool periodic = ++data_layer_frame_ % 30 == 0;
    if (!structure_changed && !periodic && signature == data_layer_signature_) return;
    data_layer_structure_ = world_.structureVersion();
    data_layer_signature_ = signature;

    entt::registry& registry = world_.registry();
    const auto apply = [&](entt::entity root, bool hide) {
        std::vector<entt::entity> stack{root};
        while (!stack.empty()) {
            const entt::entity h = stack.back();
            stack.pop_back();
            if (!registry.valid(h)) continue;
            if (ecs::EntityInfo* info = registry.try_get<ecs::EntityInfo>(h)) info->editor_hidden = hide;
            for (const entt::entity c : world_.wrap(h).children()) stack.push_back(c);
        }
    };
    std::unordered_set<entt::entity> now;
    if (!hidden.empty()) {
        for (const entt::entity h : registry.view<worldpart::PartitionObject>()) {
            const ecs::Entity e = world_.wrap(h);
            if (e.parent().valid()) continue;  // solo cuenta en la raiz
            const worldpart::PartitionObject& po = registry.get<worldpart::PartitionObject>(h);
            if (po.data_layers.empty()) continue;
            bool hide = false;
            for (const std::string& name : WorldPartitionSystem::splitLayers(po.data_layers)) hide = hide || hidden.count(name) != 0;
            if (!hide) continue;
            now.insert(h);
            // Siempre que se repasa: los hijos nuevos tambien se ocultan.
            apply(h, true);
        }
    }
    for (const entt::entity h : data_layer_hidden_) {
        if (now.count(h) == 0 && registry.valid(h)) apply(h, false);
    }
    data_layer_hidden_ = std::move(now);
}

// HLOD: por celda, las mallas de los objetos que se descargan juntas,
// coloreadas y simplificadas, en un .obj; una entidad HlodProxy lo dibuja.
// Solo la rejilla principal y los objetos sin Data Layers (el HLOD se ve
// cuando su celda esta descargada; el de una capa apagada se veria igual).
int EditorApp::buildHlods(const worldpart::WorldPartition& wp) {
    if (!has_project_ || !database_) return 0;
    // Fuera los HLOD de antes.
    std::vector<ecs::Entity> old;
    for (const entt::entity h : world_.registry().view<worldpart::HlodProxy>()) old.push_back(world_.wrap(h));
    for (ecs::Entity& e : old) world_.destroy(e);

    const float size = std::max(wp.cell_size, 1.0f);
    std::map<std::pair<int, int>, std::vector<asset::ProxyPart>> cells;
    std::vector<ecs::Entity> roots;
    world_.forEachDepthFirst([&](ecs::Entity e) {
        if (!e.parent().valid() && WorldPartitionSystem::streamable(e) && WorldPartitionSystem::gridOf(wp, e) == 0 &&
            WorldPartitionSystem::layersOf(e).empty()) {
            roots.push_back(e);
        }
    });
    for (const ecs::Entity& root : roots) {
        // Celdas de la posicion absoluta (como WorldPartitionSystem); la esquina, en local.
        const worldpart::CellCoord cc = WorldPartitionSystem::cellAt(world_.absolute(root.worldPosition()), size);
        const std::pair<int, int> key{cc.x, cc.z};
        const Vec3 corner = world_.local(ecs::DVec3{static_cast<double>(cc.x) * size, 0.0, static_cast<double>(cc.z) * size});
        // Altura absoluta: la esquina es la altura 0 del mundo (no la y = 0
        // local, que se mueve con el origen flotante).
        const Vec3 origin = corner;
        core::Mat4 to_cell = core::Mat4::identity();
        to_cell.m[3][0] = -origin.x;
        to_cell.m[3][1] = -origin.y;
        to_cell.m[3][2] = -origin.z;
        std::vector<ecs::Entity> stack{root};
        while (!stack.empty()) {
            ecs::Entity e = stack.back();
            stack.pop_back();
            for (const entt::entity c : e.children()) stack.push_back(world_.wrap(c));
            const ecs::MeshRenderer* mr = e.tryGet<ecs::MeshRenderer>();
            if (mr == nullptr || !mr->visible || mr->mesh) continue;
            const asset::ModelData* data = sync_->actorModelData(e, scene_);
            if (data == nullptr || !data->animations.empty()) continue;
            cells[key].push_back(asset::ProxyPart{data, to_cell * e.worldMatrix()});
        }
    }
    const std::u8string stem8 = scene_path_.empty() ? std::u8string(u8"Escena") : scene_path_.stem().u8string();
    const std::filesystem::path folder = project_.assetsFolder() / "HLOD" / std::filesystem::path(stem8);
    std::vector<std::pair<std::pair<int, int>, std::filesystem::path>> written;
    for (const auto& [key, parts] : cells) {
        asset::ProxyMesh mesh;
        if (!asset::buildProxyMesh(parts, 0.04f, 20000, 12, mesh)) continue;
        const std::string name = "cell_" + std::to_string(key.first) + "_" + std::to_string(key.second);
        const std::filesystem::path file = folder / (name + ".obj");
        std::string error;
        if (!asset::writeProxyObj(mesh, file, name, &error)) {
            std::cerr << "[HLOD] " << error << "\n";
            continue;
        }
        std::cout << "[HLOD] " << name << ": " << mesh.sourceTriangles << " -> " << mesh.triangles() << " triangulos\n";
        written.emplace_back(key, file);
    }
    database_->refresh();
    int created = 0;
    for (const auto& [key, file] : written) {
        Uuid uuid{};
        std::error_code ec;
        for (const assets::AssetInfo& info : database_->all()) {
            const std::filesystem::path full = info.path.is_absolute() ? info.path : database_->root() / info.path;
            if (info.type == assets::AssetType::Model && std::filesystem::equivalent(full, file, ec)) {
                uuid = info.uuid;
                break;
            }
        }
        if (!uuid.valid()) continue;
        ecs::Entity e = world_.create("HLOD " + std::to_string(key.first) + "," + std::to_string(key.second));
        const Vec3 corner =
            world_.local(ecs::DVec3{static_cast<double>(key.first) * size, 0.0, static_cast<double>(key.second) * size});
        e.setWorldPosition(corner);  // altura 0 absoluta (con el origen flotante, no la y local)
        ecs::MeshRenderer& mr = e.add<ecs::MeshRenderer>();
        mr.model = assets::AssetRef{uuid, assets::AssetType::Model};
        mr.visible = false;  // solo en Play, con la celda descargada
        worldpart::HlodProxy& p = e.add<worldpart::HlodProxy>();
        p.cell_x = key.first;
        p.cell_z = key.second;
        ++created;
    }
    if (created > 0) commit();
    return created;
}

void EditorApp::drawWorldPartitionGrid() {
    const worldpart::WorldPartition* wp = nullptr;
    bool selected = false;
    for (const entt::entity h : world_.registry().view<worldpart::WorldPartition>()) {
        wp = &world_.registry().get<worldpart::WorldPartition>(h);
        for (const ecs::Entity& s : selectedEntities()) selected = selected || s.handle() == h;
        break;
    }
    if (wp == nullptr || !wp->show_grid || (!selected && !world_partition_.active())) return;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(ImVec2(view_x_, view_y_), ImVec2(view_x_ + view_w_, view_y_ + view_h_), true);
    const auto line = [&](const Vec3& a, const Vec3& b, ImU32 color) {
        float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
        if (worldToScreen(a, ax, ay) && worldToScreen(b, bx, by)) draw->AddLine(ImVec2(ax, ay), ImVec2(bx, by), color, 1.5f);
    };
    const float size = std::max(wp->cell_size, 1.0f);
    const Vec3 cam = scene_.camera().position();
    float ground = 0.0f;
    const float y = groundHeightAt(cam.x, cam.z, ground) ? ground + 0.5f : 0.0f;
    // Las celdas son de la posicion absoluta: la esquina, en local (origen flotante).
    const auto square = [&](int cx, int cz, float cell, float lift, ImU32 color) {
        const Vec3 corner =
            world_.local(ecs::DVec3{static_cast<double>(cx) * cell, 0.0, static_cast<double>(cz) * cell});
        const float x0 = corner.x, z0 = corner.z, h = y + lift;
        const Vec3 a{x0, h, z0}, b{x0 + cell, h, z0}, c{x0 + cell, h, z0 + cell}, d{x0, h, z0 + cell};
        line(a, b, color);
        line(b, c, color);
        line(c, d, color);
        line(d, a, color);
    };
    const auto circle = [&](float radius, ImU32 color) {
        constexpr int kSegments = 64;
        for (int i = 0; i < kSegments; ++i) {
            const float a0 = static_cast<float>(i) / kSegments * 6.2831853f;
            const float a1 = static_cast<float>(i + 1) / kSegments * 6.2831853f;
            line(Vec3{cam.x + std::cos(a0) * radius, y, cam.z + std::sin(a0) * radius},
                 Vec3{cam.x + std::cos(a1) * radius, y, cam.z + std::sin(a1) * radius}, color);
        }
    };
    if (world_partition_.active()) {
        // Cada rejilla un poco mas alta (no se tapan); la principal verde/roja,
        // las demas cian/naranja. Una celda con varias capas se dibuja una vez.
        std::set<std::tuple<int, int, int>> drawn;
        for (const worldpart::CellInfo& c : world_partition_.cellInfos()) {
            // Otra capa del mismo sitio: solo si esta cargada (el verde encima).
            if (!drawn.insert(std::make_tuple(c.grid, c.coord.x, c.coord.z)).second && !c.loaded) continue;
            const bool primary = c.grid == 0;
            const ImU32 color = c.loaded ? (primary ? IM_COL32(80, 230, 120, 200) : IM_COL32(80, 200, 240, 200))
                                         : (primary ? IM_COL32(230, 70, 70, 160) : IM_COL32(240, 150, 60, 160));
            square(c.coord.x, c.coord.z, c.cell_size, 0.25f * static_cast<float>(c.grid), color);
        }
    } else if (editor_regions_.active()) {
        // Regiones cargadas: las celdas de la rejilla principal cerca de la
        // camara (verde = cargada, ambar = cargandose, gris = en disco; borde
        // amarillo = fijada) y el radio de carga del editor.
        LoadedRegionsUi& ui = regionsUi();
        editor_regions_.cellInfos(ui.cells);
        const worldpart::CellCoord cam_cell = WorldPartitionSystem::cellAt(world_.absolute(cam), size);
        const worldpart::WorldPartitionSystem::EditorRegions& regions = editor_regions_.editorRegions();
        const int reach = std::clamp(static_cast<int>(std::ceil((regions.radius + wp->unload_margin) / size)) + 2, 2, 24);
        for (std::size_t i = 0; i < ui.cells.size(); ++i) {
            const worldpart::CellInfo& c = ui.cells[i];
            if (c.grid != 0 || std::abs(c.coord.x - cam_cell.x) > reach || std::abs(c.coord.z - cam_cell.z) > reach) continue;
            // Las capas de un sitio van seguidas: se dibuja una vez.
            if (i > 0 && ui.cells[i - 1].grid == c.grid && ui.cells[i - 1].coord == c.coord) continue;
            bool loaded = true;
            bool loading = false;
            bool pinned = false;
            for (std::size_t j = i; j < ui.cells.size() && ui.cells[j].grid == c.grid && ui.cells[j].coord == c.coord; ++j) {
                loaded = loaded && ui.cells[j].loaded;
                loading = loading || ui.cells[j].loading;
                pinned = pinned || ui.cells[j].pinned;
            }
            const ImU32 color = pinned    ? IM_COL32(255, 220, 80, 220)
                                : loading ? IM_COL32(230, 170, 60, 200)
                                : loaded  ? IM_COL32(80, 230, 120, 200)
                                          : IM_COL32(150, 155, 170, 120);
            square(c.coord.x, c.coord.z, size, 0.0f, color);
        }
        if (regions.around_camera) circle(regions.radius, IM_COL32(255, 255, 255, 180));
    } else {
        // La rejilla principal alrededor de la camara y el circulo de carga
        // de cada rejilla.
        const worldpart::CellCoord cam_cell = WorldPartitionSystem::cellAt(world_.absolute(cam), size);
        const int cx = cam_cell.x;
        const int cz = cam_cell.z;
        const int r = std::clamp(static_cast<int>(std::ceil(wp->load_range / size)) + 1, 1, 12);
        for (int z = cz - r; z <= cz + r; ++z)
            for (int x = cx - r; x <= cx + r; ++x) square(x, z, size, 0.0f, IM_COL32(120, 160, 255, 90));
        circle(wp->load_range, IM_COL32(255, 220, 80, 200));
        for (const worldpart::PartitionGrid& g : wp->grids) circle(g.load_range, IM_COL32(80, 220, 255, 160));
    }
    draw->PopClipRect();
}


// -----------------------------------------------------------------------------
// Regiones cargadas (como las Loaded Regions de Unreal 5)
// -----------------------------------------------------------------------------
// Con "Celdas en disco" el editor no carga la escena entera: un segundo
// WorldPartitionSystem (editor_regions_, en modo editor) tiene solo las celdas
// a un radio de la camara del editor y las fijadas en la ventana. Lo que entra
// y sale no es un paso de deshacer (el historial se pone al dia sin paso), se
// guarda con la escena aunque no este cargado y, al dar Play, el World
// Partition del juego toma las celdas descargadas sin leerlas.

namespace {

std::filesystem::path regionsPrefsFile(const std::filesystem::path& library, const Uuid& scene) {
    return library / "WorldPartition" / (scene.toString() + ".regions");
}

// El World Partition que guarda las celdas en disco (como saveScene).
const worldpart::WorldPartition* diskPartition(ecs::World& world) {
    for (const entt::entity h : world.registry().view<worldpart::WorldPartition>()) {
        const worldpart::WorldPartition& wp = world.registry().get<worldpart::WorldPartition>(h);
        if (wp.enabled && wp.external_cells) return &wp;
    }
    return nullptr;
}

std::string trimmed(const std::string& s) {
    const std::size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t") - b + 1);
}

}  // namespace

EditorApp::LoadedRegionsUi& EditorApp::regionsUi() {
    if (!regions_ui_) regions_ui_ = std::make_shared<LoadedRegionsUi>();
    return *regions_ui_;
}

void EditorApp::loadRegionsPrefs() {
    LoadedRegionsUi& ui = regionsUi();
    ui.enabled = true;
    ui.regions = {};
    ui.pins.clear();
    if (!has_project_) return;
    std::ifstream in(regionsPrefsFile(project_.libraryFolder(), world_.sceneUuid()));
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream words(line);
        std::string key;
        words >> key;
        if (key == "enabled") {
            int on = 1;
            words >> on;
            ui.enabled = on != 0;
        } else if (key == "camera") {
            int on = 1;
            float radius = ui.regions.radius;
            words >> on >> radius;
            ui.regions.around_camera = on != 0;
            if (std::isfinite(radius) && radius > 0.0f) ui.regions.radius = std::clamp(radius, 16.0f, 100000.0f);
        } else if (key == "cell") {
            int x = 0;
            int z = 0;
            if (!(words >> x >> z)) continue;
            std::string grid;
            std::getline(words, grid);
            ui.pins.emplace_back(trimmed(grid), worldpart::CellCoord{x, z});
        }
    }
}

void EditorApp::saveRegionsPrefs() {
    if (!has_project_) return;
    LoadedRegionsUi& ui = regionsUi();
    if (editor_regions_.active()) {
        ui.regions = editor_regions_.editorRegions();
        ui.pins.clear();
        for (const auto& [grid, c] : editor_regions_.pinnedCells()) {
            ui.pins.emplace_back(WorldPartitionSystem::gridName(editor_regions_.settings(), grid), c);
        }
    }
    const std::filesystem::path file = regionsPrefsFile(project_.libraryFolder(), world_.sceneUuid());
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "[Regiones] No se pudo escribir " << dialogs::utf8(file) << "\n";
        return;
    }
    out << "#CramionLoadedRegions 1\n";
    out << "enabled " << (ui.enabled ? 1 : 0) << "\n";
    out << "camera " << (ui.regions.around_camera ? 1 : 0) << " " << ui.regions.radius << "\n";
    for (const auto& [grid, c] : ui.pins) {
        out << "cell " << c.x << " " << c.z;
        if (!grid.empty()) out << " " << grid;
        out << "\n";
    }
}

bool EditorApp::beginEditorRegions(const std::filesystem::path& scene) {
    // Lo de otra escena (abrir) no vuelve: quien sigue con el mismo mundo
    // llama antes a end(world_, true).
    if (editor_regions_.active()) editor_regions_.end(world_, false);
    LoadedRegionsUi& ui = regionsUi();
    ui.seen_epoch = 0;
    ui.away = false;
    if (!has_project_ || scene.empty() || diskPartition(world_) == nullptr) return false;
    loadRegionsPrefs();
    if (!ui.enabled) return false;  // apagadas para esta escena: el editor lo carga todo
    // Las celdas en disco no estan en el mundo: se registran sin leer.
    editor_regions_.setSceneFile(scene, /*cells_in_world=*/false);
    editor_regions_.setEditorRegions(ui.regions);
    editor_regions_.beginEditor(world_, scene_.camera().position());
    if (!editor_regions_.active()) return false;
    for (const auto& [name, c] : ui.pins) {
        const int grid = WorldPartitionSystem::findGrid(editor_regions_.settings(), name);
        if (grid >= 0) editor_regions_.setCellPinned(grid, c, true);
    }
    ui.grid = 0;
    ui.follow_camera = true;
    ui.has_selection = false;
    ui.synced_changes = editor_regions_.worldChanges();
    return true;
}

void EditorApp::updateEditorRegions() {
    if (!editor_regions_.active()) return;
    LoadedRegionsUi& ui = regionsUi();
    // En Play o en la pestana de un prefab el mundo no es el de la escena:
    // espera sin tocar nada (Play tiene su propio World Partition).
    if (playing() || world_workspace_ != 0) {
        ui.away = true;
        return;
    }
    if (world_.sceneGeneration() != editor_regions_.sceneGeneration()) {
        if (scene_path_ != editor_regions_.sceneFile()) {
            // Otra escena (una nueva): lo de esta no vuelve.
            editor_regions_.end(world_, false);
            return;
        }
        // La misma escena (parar Play tras un Scene.load, volver de una pestana).
        editor_regions_.adoptSceneGeneration(world_);
    }
    // A medio arrastrar (gizmo, asas, un campo del Inspector) el mundo no se
    // toca: el historial se pondria al dia con el cambio a medias.
    const bool interacting = ImGuizmo::IsUsing() || collider_handle_drag_ != 0 || light_handle_drag_ != 0 ||
                             (ImGui::IsAnyItemActive() && !flying_);
    // Sin World Partition con celdas en disco (lo apagaron o lo borraron), lo
    // descargado no tiene donde guardarse: todo al mundo, ya.
    const worldpart::WorldPartition* wp = diskPartition(world_);
    bool restart = false;
    if (wp != nullptr && !interacting && ++ui.frame % 30 == 0) {
        // Cada medio segundo: la misma rejilla (si no, las celdas son otras).
        restart = !editor_regions_.sameLayout(*wp);
        if (!restart) editor_regions_.refreshSettings(*wp);
    }
    if (wp == nullptr || restart) {
        editor_regions_.end(world_, true);
        history_.sync(world_);  // lo que entro no es un paso de deshacer
        if (restart) {
            beginEditorRegions(scene_path_);
            pushToast("Regiones cargadas", "Otra rejilla: la escena se reparte otra vez");
        } else {
            pushToast("Regiones cargadas", "Sin celdas en disco: toda la escena cargada");
        }
        return;
    }
    // Parar Play o volver de una pestana rehizo el mundo con una instantanea.
    if (editor_regions_.worldRebuilt(world_)) {
        editor_regions_.recoverAfterRebuild(world_, ui.away ? ui.seen_epoch : 0);
    }
    ui.away = false;
    ui.seen_epoch = editor_regions_.loadEpoch();
    if (interacting) return;
    // Lo del usuario va en su paso antes de que entren o salgan celdas.
    if (commit_pending_) flushCommit();
    // Las celdas de lo seleccionado no se descargan.
    if (selection_.empty()) {
        editor_regions_.holdCellsOf(world_, {});
    } else {
        std::vector<ecs::Entity> roots;
        for (const ecs::Entity& e : selectedEntities()) roots.push_back(rootOf(e));
        editor_regions_.holdCellsOf(world_, roots);
    }
    editor_regions_.update(world_, scene_.camera().position());
    // Lo que entro o salio (aqui, al guardar o antes de Play) no es un paso de
    // deshacer: el historial lo da por hecho.
    if (editor_regions_.worldChanges() != ui.synced_changes) {
        history_.sync(world_);
        ui.synced_changes = editor_regions_.worldChanges();
    }
    ui.seen_epoch = editor_regions_.loadEpoch();
}

void EditorApp::prepareRegionsForPlay() {
    if (!editor_regions_.active()) {
        world_partition_.inheritUnloadedFrom(nullptr);
        return;
    }
    // Las celdas a medias, enteras (la instantanea de Play las tiene enteras
    // y al parar no hay nada que recuperar) y lo descargado, borrado.
    editor_regions_.settle(world_);
    world_partition_.inheritUnloadedFrom(&editor_regions_);
    LoadedRegionsUi& ui = regionsUi();
    ui.seen_epoch = editor_regions_.loadEpoch();
    ui.away = true;
}

void EditorApp::prepareRegionsForSave(const worldpart::WorldPartition* external) {
    if (!editor_regions_.active()) return;
    if (external != nullptr && editor_regions_.sameLayout(*external)) {
        editor_regions_.refreshSettings(*external);
        return;
    }
    // Sin celdas en disco (o con otra rejilla) lo descargado no tiene donde
    // ir: todo al mundo y de ahi a la escena.
    editor_regions_.end(world_, true);
    history_.sync(world_);
    pushToast("Regiones cargadas", "Celdas en disco apagadas: toda la escena cargada antes de guardar");
}

void EditorApp::drawLoadedRegionsWindow() {
    ImGui::SetNextWindowSize(ImVec2(460.0f, 600.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Regiones cargadas", &show_loaded_regions_)) {
        ImGui::End();
        return;
    }
    LoadedRegionsUi& ui = regionsUi();
    const worldpart::WorldPartition* wp = scenePartition(world_);
    if (!has_project_) {
        ImGui::TextDisabled("Abre un proyecto");
        ImGui::End();
        return;
    }
    if (playing()) {
        ImGui::TextDisabled("En Play manda el World Partition del juego.");
        ImGui::TextDisabled("Las regiones del editor siguen como estaban al parar.");
        ImGui::End();
        return;
    }
    if (!editor_regions_.active()) {
        ImGui::TextWrapped("Con \"Celdas en disco\" el editor solo carga las celdas cerca de su camara y las que "
                           "fijes aqui (como las Loaded Regions de Unreal 5). Lo demas sigue en disco y se guarda "
                           "igual.");
        ImGui::Spacing();
        if (wp == nullptr) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "La escena no tiene World Partition.");
        } else if (!wp->enabled || !wp->external_cells) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                               "Activa \"Celdas en disco\" en el World Partition y guarda la escena.");
        } else if (scene_path_.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Guarda la escena para crear sus celdas en disco.");
        } else {
            ImGui::TextDisabled("Apagadas para esta escena: el editor la tiene entera.");
            // Con cambios sin guardar, el disco no es la escena: primero guardar
            // (si no, una celda vaciada a mano volveria del disco).
            ImGui::BeginDisabled(dirty_);
            if (ImGui::Button("Activar regiones cargadas", ImVec2(-1.0f, 0.0f))) {
                ui.enabled = true;
                saveRegionsPrefs();
                if (beginEditorRegions(scene_path_)) pushToast("Regiones cargadas", "Activadas");
            }
            ImGui::EndDisabled();
            if (dirty_) ImGui::TextDisabled("Guarda la escena antes de activarlas.");
        }
        ImGui::End();
        return;
    }

    const worldpart::PartitionStats& s = editor_regions_.stats();
    ImGui::Text("Celdas cargadas: %d de %d", s.loaded_cells, s.cells);
    ImGui::Text("Objetos sin cargar: %d  |  en disco %.1f MB, en memoria %.1f MB", s.unloaded_entities,
                static_cast<double>(s.disk_bytes) / (1024.0 * 1024.0),
                static_cast<double>(s.stored_bytes) / (1024.0 * 1024.0));
    if (s.loading_cells > 0 || editor_regions_.pendingDestroys() > 0) {
        ImGui::TextDisabled("Cargando %d celda(s), %d objeto(s) por borrar", s.loading_cells,
                            editor_regions_.pendingDestroys());
    }
    bool enabled = true;
    if (ImGui::Checkbox("Regiones cargadas en esta escena", &enabled) && !enabled) {
        // Todo al mundo, ya (puede tardar en un mundo grande).
        editor_regions_.end(world_, true);
        history_.sync(world_);
        ui.enabled = false;
        saveRegionsPrefs();
        pushToast("Regiones cargadas", "Apagadas: toda la escena cargada");
        ImGui::End();
        return;
    }
    ImGui::SetItemTooltip("Apagarlas carga la escena entera ahora (como antes).");

    worldpart::WorldPartitionSystem::EditorRegions regions = editor_regions_.editorRegions();
    bool regions_changed = ImGui::Checkbox("Cargar alrededor de la camara", &regions.around_camera);
    ImGui::BeginDisabled(!regions.around_camera);
    ImGui::SetNextItemWidth(-1.0f);
    regions_changed |= ImGui::SliderFloat("##radius", &regions.radius, 16.0f, 20000.0f, "Radio %.0f m",
                                          ImGuiSliderFlags_Logarithmic);
    const bool radius_done = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::EndDisabled();
    if (regions_changed) {
        regions.radius = std::clamp(regions.radius, 16.0f, 100000.0f);
        editor_regions_.setEditorRegions(regions);
    }
    if ((regions_changed && !ImGui::IsItemActive()) || radius_done) saveRegionsPrefs();

    const worldpart::WorldPartition& settings = editor_regions_.settings();
    ui.grid = std::clamp(ui.grid, 0, static_cast<int>(settings.grids.size()));
    if (!settings.grids.empty()) {
        const auto grid_label = [&](int g) {
            const std::string name = WorldPartitionSystem::gridName(settings, g);
            return g == 0 ? std::string("Principal") : (name.empty() ? std::string("(sin nombre)") : name);
        };
        if (ImGui::BeginCombo("Rejilla", grid_label(ui.grid).c_str())) {
            for (int g = 0; g <= static_cast<int>(settings.grids.size()); ++g) {
                if (ImGui::Selectable(grid_label(g).c_str(), g == ui.grid)) {
                    ui.grid = g;
                    ui.has_selection = false;
                }
            }
            ImGui::EndCombo();
        }
    }

    editor_regions_.cellInfos(ui.cells);
    const int sx0 = std::min(ui.sel_x0, ui.sel_x1);
    const int sx1 = std::max(ui.sel_x0, ui.sel_x1);
    const int sz0 = std::min(ui.sel_z0, ui.sel_z1);
    const int sz1 = std::max(ui.sel_z0, ui.sel_z1);
    const auto in_selection = [&](const worldpart::CellInfo& c) {
        return ui.has_selection && c.grid == ui.grid && c.coord.x >= sx0 && c.coord.x <= sx1 && c.coord.z >= sz0 &&
               c.coord.z <= sz1;
    };
    const auto pin_where = [&](bool pin, const auto& pick) {
        int n = 0;
        for (const worldpart::CellInfo& c : ui.cells) {
            if (!pick(c) || editor_regions_.cellPinned(c.grid, c.coord) == pin) continue;
            editor_regions_.setCellPinned(c.grid, c.coord, pin);
            ++n;
        }
        if (n > 0) saveRegionsPrefs();
    };
    ImGui::BeginDisabled(!ui.has_selection);
    if (ImGui::Button("Cargar seleccion")) pin_where(true, in_selection);
    ImGui::SetItemTooltip("Fija las celdas del recuadro: se quedan cargadas aunque la camara se aleje.");
    ImGui::SameLine();
    if (ImGui::Button("Descargar seleccion")) pin_where(false, in_selection);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cargar todo")) pin_where(true, [](const worldpart::CellInfo&) { return true; });
    ImGui::SetItemTooltip("Fija todas las celdas (se cargan poco a poco, sin congelar el editor).\n"
                          "Antes de hornear luz o navegacion, espera a que terminen.");
    ImGui::SameLine();
    if (ImGui::Button("Descargar todo")) {
        editor_regions_.clearPinnedCells();
        regions.around_camera = false;
        editor_regions_.setEditorRegions(regions);
        saveRegionsPrefs();
    }
    ImGui::SetItemTooltip("Quita las celdas fijadas y deja de cargar alrededor de la camara.");
    ImGui::Checkbox("Seguir la camara", &ui.follow_camera);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    ImGui::SetItemTooltip("Clic: fijar o soltar una celda. Arrastrar: recuadro de seleccion.\n"
                          "Doble clic: llevar la camara. Rueda: zoom. Boton derecho o central: mover.");

    // --- Cuadricula ---
    const float cell_size = WorldPartitionSystem::gridCellSize(settings, ui.grid);
    scene::Camera& camera = scene_.camera();
    const ecs::DVec3 cam_abs = world_.absolute(camera.position());
    const float cam_x = static_cast<float>(cam_abs.x / cell_size);
    const float cam_z = static_cast<float>(cam_abs.z / cell_size);
    if (ui.follow_camera) {
        ui.pan_x = cam_x;
        ui.pan_z = cam_z;
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size(std::max(avail.x, 64.0f), std::max(avail.y - ImGui::GetTextLineHeightWithSpacing(), 120.0f));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    const ImVec2 mid(p0.x + size.x * 0.5f, p0.y + size.y * 0.5f);
    ImGui::InvisibleButton("##region_cells", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const ImGuiIO& io = ImGui::GetIO();
    const auto cell_at = [&](const ImVec2& p, int& cx, int& cz) {
        cx = static_cast<int>(std::floor(ui.pan_x + (p.x - mid.x) / ui.zoom));
        cz = static_cast<int>(std::floor(ui.pan_z + (p.y - mid.y) / ui.zoom));
    };
    const auto to_screen = [&](float cx, float cz) {
        return ImVec2(mid.x + (cx - ui.pan_x) * ui.zoom, mid.y + (cz - ui.pan_z) * ui.zoom);
    };
    if (ImGui::IsItemActivated()) ui.dragged = false;
    // Zoom con la rueda (alrededor del raton, o de la camara si se la sigue).
    if (hovered && io.MouseWheel != 0.0f) {
        const float before_x = ui.pan_x + (io.MousePos.x - mid.x) / ui.zoom;
        const float before_z = ui.pan_z + (io.MousePos.y - mid.y) / ui.zoom;
        ui.zoom = std::clamp(ui.zoom * std::pow(1.15f, io.MouseWheel), 2.0f, 96.0f);
        if (!ui.follow_camera) {
            ui.pan_x = before_x - (io.MousePos.x - mid.x) / ui.zoom;
            ui.pan_z = before_z - (io.MousePos.y - mid.y) / ui.zoom;
        }
    }
    // Mover con el boton derecho o el central.
    if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 2.0f) ||
                   ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 2.0f))) {
        ui.pan_x -= io.MouseDelta.x / ui.zoom;
        ui.pan_z -= io.MouseDelta.y / ui.zoom;
        ui.follow_camera = false;
        ui.dragged = true;
    }
    // Recuadro de seleccion con el izquierdo.
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
        if (!ui.selecting) {
            ui.selecting = true;
            cell_at(io.MouseClickedPos[ImGuiMouseButton_Left], ui.sel_x0, ui.sel_z0);
        }
        cell_at(io.MousePos, ui.sel_x1, ui.sel_z1);
        ui.has_selection = true;
        ui.dragged = true;
    }
    int hover_x = 0;
    int hover_z = 0;
    cell_at(io.MousePos, hover_x, hover_z);
    // La celda bajo el raton (todas sus capas juntas).
    bool hover_known = false;
    bool hover_loaded = true;
    bool hover_loading = false;
    bool hover_pinned = false;
    int hover_entities = 0;
    for (const worldpart::CellInfo& c : ui.cells) {
        if (c.grid != ui.grid || c.coord.x != hover_x || c.coord.z != hover_z) continue;
        hover_known = true;
        hover_loaded = hover_loaded && c.loaded;
        hover_loading = hover_loading || c.loading;
        hover_pinned = hover_pinned || c.pinned;
        hover_entities += c.entities;
    }
    if (ImGui::IsItemDeactivated()) {
        if (ui.selecting) {
            ui.selecting = false;
        } else if (!ui.dragged && hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && hover_known) {
            editor_regions_.setCellPinned(ui.grid, worldpart::CellCoord{hover_x, hover_z}, !hover_pinned);
            saveRegionsPrefs();
        }
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        // La camara al centro de la celda, a la misma altura y mirando igual.
        const ecs::DVec3 target{(static_cast<double>(hover_x) + 0.5) * cell_size, cam_abs.y,
                                (static_cast<double>(hover_z) + 0.5) * cell_size};
        camera.setPosition(world_.local(target));
        ui.follow_camera = true;
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(p0, p1, true);
    draw->AddRectFilled(p0, p1, IM_COL32(24, 26, 32, 255));
    // Las lineas de la cuadricula (solo con zoom suficiente).
    if (ui.zoom >= 6.0f) {
        int x0 = 0, z0 = 0, x1 = 0, z1 = 0;
        cell_at(p0, x0, z0);
        cell_at(p1, x1, z1);
        for (int x = x0; x <= x1 + 1; ++x) {
            const float sx = to_screen(static_cast<float>(x), 0.0f).x;
            draw->AddLine(ImVec2(sx, p0.y), ImVec2(sx, p1.y), IM_COL32(255, 255, 255, 14));
        }
        for (int z = z0; z <= z1 + 1; ++z) {
            const float sz = to_screen(0.0f, static_cast<float>(z)).y;
            draw->AddLine(ImVec2(p0.x, sz), ImVec2(p1.x, sz), IM_COL32(255, 255, 255, 14));
        }
    }
    // Las celdas conocidas: verde cargada, ambar cargandose, gris en disco;
    // fijadas con borde amarillo. Las capas de un sitio van seguidas.
    const float inset = ui.zoom >= 5.0f ? 1.0f : 0.0f;
    for (std::size_t i = 0; i < ui.cells.size();) {
        const worldpart::CellInfo& first = ui.cells[i];
        bool loaded = true;
        bool loading = false;
        bool pinned = false;
        std::size_t j = i;
        for (; j < ui.cells.size() && ui.cells[j].grid == first.grid && ui.cells[j].coord == first.coord; ++j) {
            loaded = loaded && ui.cells[j].loaded;
            loading = loading || ui.cells[j].loading;
            pinned = pinned || ui.cells[j].pinned;
        }
        i = j;
        if (first.grid != ui.grid) continue;
        const ImVec2 a = to_screen(static_cast<float>(first.coord.x), static_cast<float>(first.coord.z));
        const ImVec2 b(a.x + ui.zoom, a.y + ui.zoom);
        if (b.x < p0.x || b.y < p0.y || a.x > p1.x || a.y > p1.y) continue;
        const ImU32 fill = loading ? IM_COL32(220, 170, 60, 200) : loaded ? IM_COL32(70, 180, 105, 210) : IM_COL32(70, 76, 92, 210);
        draw->AddRectFilled(ImVec2(a.x + inset, a.y + inset), ImVec2(b.x - inset, b.y - inset), fill);
        if (pinned) draw->AddRect(a, b, IM_COL32(255, 220, 80, 255), 0.0f, 0, 2.0f);
    }
    if (ui.has_selection) {
        const ImVec2 a = to_screen(static_cast<float>(sx0), static_cast<float>(sz0));
        const ImVec2 b = to_screen(static_cast<float>(sx1 + 1), static_cast<float>(sz1 + 1));
        draw->AddRectFilled(a, b, IM_COL32(90, 150, 255, 40));
        draw->AddRect(a, b, IM_COL32(120, 170, 255, 220), 0.0f, 0, 1.5f);
    }
    // La camara: su radio de carga, el punto y hacia donde mira.
    const ImVec2 cam_screen = to_screen(cam_x, cam_z);
    if (regions.around_camera) {
        draw->AddCircle(cam_screen, regions.radius / cell_size * ui.zoom, IM_COL32(255, 255, 255, 150), 64, 1.5f);
    }
    const Vec3 forward = camera.forward();
    const float flat = std::sqrt(forward.x * forward.x + forward.z * forward.z);
    if (flat > 1e-4f) {
        draw->AddLine(cam_screen,
                      ImVec2(cam_screen.x + forward.x / flat * 14.0f, cam_screen.y + forward.z / flat * 14.0f),
                      IM_COL32(255, 255, 255, 230), 2.0f);
    }
    draw->AddCircleFilled(cam_screen, 4.0f, IM_COL32(255, 255, 255, 255));
    if (hovered && !active) {
        const ImVec2 a = to_screen(static_cast<float>(hover_x), static_cast<float>(hover_z));
        draw->AddRect(a, ImVec2(a.x + ui.zoom, a.y + ui.zoom), IM_COL32(255, 255, 255, 200));
    }
    draw->PopClipRect();
    if (hovered && !active) {
        if (hover_known) {
            ImGui::SetTooltip("Celda %d, %d (%.0f m)\n%s, %d objeto(s)%s\nClic: %s", hover_x, hover_z,
                              static_cast<double>(cell_size),
                              hover_loading ? "Cargandose" : hover_loaded ? "Cargada" : "En disco", hover_entities,
                              hover_pinned ? "\nFijada" : "", hover_pinned ? "soltarla" : "fijarla (cargada siempre)");
        } else {
            ImGui::SetTooltip("Celda %d, %d: vacia", hover_x, hover_z);
        }
    }
    ImGui::TextDisabled("Verde: cargada  Ambar: cargandose  Gris: en disco  Borde amarillo: fijada");
    ImGui::End();
}

}  // namespace cramion::editor
