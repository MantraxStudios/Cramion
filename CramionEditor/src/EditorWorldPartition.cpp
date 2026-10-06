// World Partition en el editor: estadisticas en el Inspector (cuantos objetos
// se descargarian, celdas cargadas en Play, memoria guardada) y la rejilla de
// celdas en la Escena (verde = cargada, roja = descargada).

#include "EditorApp.h"

#include <CramionFX/asset/ProxyMesh.h>

#include <imgui.h>

#include <iostream>
#include <map>

#include <algorithm>
#include <cmath>
#include <set>

namespace cramion::editor {

using core::Vec3;

void EditorApp::drawWorldPartitionInspector(ecs::Entity entity) {
    const worldpart::WorldPartition* wp = entity.tryGet<worldpart::WorldPartition>();
    if (wp == nullptr) return;
    if (world_partition_.active()) {
        const worldpart::PartitionStats& s = world_partition_.stats();
        ImGui::Text("Celdas cargadas: %d de %d", s.loaded_cells, s.cells);
        ImGui::Text("Objetos descargados: %d de %d", s.unloaded_entities, s.streamed_entities);
        ImGui::Text("Guardado en memoria: %.1f KB  (cargas %d, descargas %d)", static_cast<double>(s.stored_bytes) / 1024.0,
                    s.loads, s.unloads);
        return;
    }
    // Fuera de Play: lo que pasaria al darle.
    int streamable = 0;
    std::set<std::pair<int, int>> cells;
    const float size = std::max(wp->cell_size, 1.0f);
    world_.forEachDepthFirst([&](ecs::Entity e) {
        if (e.parent().valid() || !worldpart::WorldPartitionSystem::streamable(e)) return;
        ++streamable;
        const Vec3 p = e.worldPosition();
        cells.insert({static_cast<int>(std::floor(p.x / size)), static_cast<int>(std::floor(p.z / size))});
    });
    ImGui::TextDisabled("En Play: %d objetos en %d celdas se cargan y descargan", streamable, static_cast<int>(cells.size()));
    ImGui::TextDisabled("Sin Fuente de carga, carga la camara. \"Siempre cargado\" lo excluye.");
    if (ImGui::Button("Construir HLOD (mallas lejanas por celda)", ImVec2(-1.0f, 0.0f))) {
        const int built = buildHlods(*wp);
        pushToast(built > 0 ? "HLOD: " + std::to_string(built) + " celdas" : "HLOD: no hay mallas estaticas que juntar");
    }
    ImGui::SetItemTooltip("Junta las mallas de cada celda en una sola muy simplificada (Assets/HLOD/<escena>/).\n"
                          "En Play se ve en lugar de la celda cuando esta descargada (el pueblo se sigue viendo de lejos).");
    bool has_source = !world_.registry().view<worldpart::StreamingSource>().empty();
    if (!has_source) {
        ecs::Entity player = world_.findWithTag("Player");
        if (player.valid() && ImGui::Button("Anadir Fuente de carga al jugador", ImVec2(-1.0f, 0.0f))) {
            player.add<worldpart::StreamingSource>();
            commit();
        }
    }
}

// HLOD: por celda, las mallas de los objetos que se descargan juntas,
// coloreadas y simplificadas, en un .obj; una entidad HlodProxy lo dibuja.
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
        if (!e.parent().valid() && worldpart::WorldPartitionSystem::streamable(e)) roots.push_back(e);
    });
    for (const ecs::Entity& root : roots) {
        const Vec3 rp = root.worldPosition();
        const std::pair<int, int> key{static_cast<int>(std::floor(rp.x / size)), static_cast<int>(std::floor(rp.z / size))};
        const Vec3 origin{static_cast<float>(key.first) * size, 0.0f, static_cast<float>(key.second) * size};
        core::Mat4 to_cell = core::Mat4::identity();
        to_cell.m[3][0] = -origin.x;
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
        e.setWorldPosition(Vec3{static_cast<float>(key.first) * size, 0.0f, static_cast<float>(key.second) * size});
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
    const auto square = [&](int cx, int cz, ImU32 color) {
        const float x0 = static_cast<float>(cx) * size, z0 = static_cast<float>(cz) * size;
        const Vec3 a{x0, y, z0}, b{x0 + size, y, z0}, c{x0 + size, y, z0 + size}, d{x0, y, z0 + size};
        line(a, b, color);
        line(b, c, color);
        line(c, d, color);
        line(d, a, color);
    };
    if (world_partition_.active()) {
        for (const auto& [coord, loaded] : world_partition_.cells()) {
            square(coord.x, coord.z, loaded ? IM_COL32(80, 230, 120, 200) : IM_COL32(230, 70, 70, 160));
        }
    } else {
        // La rejilla alrededor de la camara y el circulo de carga.
        const int cx = static_cast<int>(std::floor(cam.x / size));
        const int cz = static_cast<int>(std::floor(cam.z / size));
        const int r = std::clamp(static_cast<int>(std::ceil(wp->load_range / size)) + 1, 1, 12);
        for (int z = cz - r; z <= cz + r; ++z)
            for (int x = cx - r; x <= cx + r; ++x) square(x, z, IM_COL32(120, 160, 255, 90));
        constexpr int kSegments = 64;
        for (int i = 0; i < kSegments; ++i) {
            const float a0 = static_cast<float>(i) / kSegments * 6.2831853f;
            const float a1 = static_cast<float>(i + 1) / kSegments * 6.2831853f;
            line(Vec3{cam.x + std::cos(a0) * wp->load_range, y, cam.z + std::sin(a0) * wp->load_range},
                 Vec3{cam.x + std::cos(a1) * wp->load_range, y, cam.z + std::sin(a1) * wp->load_range},
                 IM_COL32(255, 220, 80, 200));
        }
    }
    draw->PopClipRect();
}

}  // namespace cramion::editor
