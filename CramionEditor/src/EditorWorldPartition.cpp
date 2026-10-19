// World Partition en el editor: estadisticas en el Inspector (cuantos objetos
// se descargarian, celdas cargadas en Play, memoria guardada), las rejillas,
// las Data Layers (verlas u ocultarlas en la Escena, activarlas en Play), las
// dependencias entre celdas y la rejilla de celdas en la Escena (verde =
// cargada, roja = descargada).

#include "EditorApp.h"

#include "Dialogs.h"

#include <CramionFX/asset/ProxyMesh.h>

#include <imgui.h>

#include <iostream>
#include <map>

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

}  // namespace cramion::editor
