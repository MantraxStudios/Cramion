// cramion_asset_tool: prueba de consola del sistema de assets.
//
//   cramion_asset_tool [archivo ...]
//
// Crea un proyecto en %TEMP%/CramionAssetTest, importa los archivos (por
// defecto el coche y el cielo de las escenas de demostracion), abre la base
// de datos, vuelve a cargar cada asset con el AssetManager (piezas, nodos,
// texturas decodificadas) y extrae el HDR. Imprime tiempos y la jerarquia.

#include "CramionCore/asset/AssetDatabase.h"
#include "CramionCore/asset/AssetManager.h"
#include "CramionCore/asset/Importer.h"
#include "CramionCore/project/Project.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace cramion;
using Clock = std::chrono::steady_clock;

float secondsSince(Clock::time_point start) {
    return std::chrono::duration<float>(Clock::now() - start).count();
}

void printTree(const assets::ModelAsset& model, std::int32_t node, int depth, int& printed) {
    if (printed >= 40) {
        return;
    }
    const assets::ModelNode& n = model.nodes[static_cast<std::size_t>(node)];
    std::cout << "      " << std::string(static_cast<std::size_t>(depth) * 2, ' ') << "- " << n.name;
    if (n.part >= 0) {
        const auto& part = *model.parts[static_cast<std::size_t>(n.part)];
        std::cout << "  [pieza " << n.part << ": " << part.indices.size() / 3 << " tri, "
                  << part.materials.size() << " mat, " << part.textures.size() << " tex]";
    }
    std::cout << "  pos(" << n.local.m[3][0] << ", " << n.local.m[3][1] << ", " << n.local.m[3][2]
              << ")\n";
    ++printed;
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        if (model.nodes[i].parent == node) {
            printTree(model, static_cast<std::int32_t>(i), depth + 1, printed);
        }
    }
}

// Caja del modelo en el espacio de su raiz, componiendo los transforms de
// los nodos (padres antes que hijos) con las cajas de las submallas: debe
// salir igual se parta como se parta.
void printWorldBounds(const assets::ModelAsset& model) {
    std::vector<core::Mat4> global(model.nodes.size(), core::Mat4::identity());
    core::Vec3 low{1e30f, 1e30f, 1e30f};
    core::Vec3 high{-1e30f, -1e30f, -1e30f};
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const assets::ModelNode& n = model.nodes[i];
        global[i] = n.parent >= 0 ? global[static_cast<std::size_t>(n.parent)] * n.local : n.local;
        if (n.part < 0) {
            continue;
        }
        const auto& part = *model.parts[static_cast<std::size_t>(n.part)];
        for (const asset::SkinnedVertex& v : part.vertices) {
            const core::Vec4 p = global[i] * core::Vec4{v.position.x, v.position.y, v.position.z, 1.0f};
            low = core::Vec3{std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
            high = core::Vec3{std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
        }
    }
    std::cout << "    caja en el mundo: (" << low.x << ", " << low.y << ", " << low.z << ") - ("
              << high.x << ", " << high.y << ", " << high.z << ")\n";
}

// Comprueba lo que el renderizador necesita de cada pieza.
bool checkModel(const assets::ModelAsset& model) {
    bool ok = !model.parts.empty() && !model.nodes.empty();
    std::size_t textures = 0;
    std::size_t decoded = 0;
    for (const auto& part : model.parts) {
        ok = ok && !part->indices.empty() && !part->submeshes.empty() && !part->materials.empty() &&
             !part->bones.empty();
        for (const asset::SubMesh& s : part->submeshes) {
            ok = ok && s.material < part->materials.size() &&
                 s.first_index + s.index_count <= part->indices.size();
        }
        for (const asset::TextureData& t : part->textures) {
            ++textures;
            decoded += (!t.pixels.empty() && t.encoded.empty() && t.width > 0) ? 1 : 0;
        }
    }
    for (const assets::ModelNode& n : model.nodes) {
        ok = ok && n.part < static_cast<std::int32_t>(model.parts.size()) &&
             n.parent < static_cast<std::int32_t>(model.nodes.size());
    }
    std::cout << "    texturas decodificadas: " << decoded << " / " << textures << "\n";
    return ok && decoded == textures;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::filesystem::path> files;
    for (int i = 1; i < argc; ++i) {
        files.emplace_back(argv[i]);
    }
    if (files.empty()) {
        const std::filesystem::path demo = "C:/Users/tupap/OneDrive/Desktop/Cramion/build/assets";
        files = {demo / "sportscar/sportsCar.obj",
                 demo / "bistro/Bistro_v5_2/san_giuseppe_bridge_4k.hdr"};
    }

    const std::filesystem::path parent = std::filesystem::temp_directory_path() / "CramionAssetTest";
    std::filesystem::remove_all(parent / "Prueba");
    const project::ProjectInfo project = project::createProject(parent, "Prueba");
    project::addRecentProject(project);

    // --- Importar ---
    int failures = 0;
    for (const auto& file : files) {
        const auto start = Clock::now();
        const assets::ImportResult result = assets::importAny(file, project.assetsFolder());
        std::cout << "  importar " << file.filename().string() << ": "
                  << (result.ok ? "OK" : "FALLO") << " en " << secondsSince(start) << " s\n";
        failures += result.ok ? 0 : 1;
    }

    // --- Base de datos ---
    auto start = Clock::now();
    assets::AssetDatabase database;
    database.open(project.assetsFolder());
    std::cout << "  base de datos: " << database.all().size() << " assets en " << secondsSince(start)
              << " s\n";
    for (const assets::AssetInfo& info : database.all()) {
        std::cout << "    " << info.uuid.toString() << "  " << assets::assetTypeName(info.type)
                  << "  " << info.name << "  " << info.size_bytes / 1024 << " KB\n";
    }

    // --- Cargar ---
    assets::AssetManager manager(database);
    manager.setCacheFolder(project.libraryFolder() / "Cache");
    for (const assets::AssetInfo& info : database.all()) {
        start = Clock::now();
        if (info.type == assets::AssetType::Model) {
            const auto model = manager.loadModel(info.uuid);
            if (!model) {
                std::cout << "  cargar " << info.name << ": FALLO\n";
                ++failures;
                continue;
            }
            std::size_t triangles = 0;
            for (const auto& part : model->parts) {
                triangles += part->indices.size() / 3;
            }
            std::cout << "  cargar " << info.name << ": " << model->parts.size() << " piezas, "
                      << model->nodes.size() << " nodos, " << triangles << " tri, "
                      << (model->animated ? "animado" : "estatico") << ", " << secondsSince(start)
                      << " s\n";
            printWorldBounds(*model);
            if (!checkModel(*model)) {
                std::cout << "    ERROR: datos incoherentes\n";
                ++failures;
            }
            if (!info.path.empty()) {
                int printed = 0;
                printTree(*model, 0, 0, printed);
            }
        } else if (info.type == assets::AssetType::Environment) {
            const std::filesystem::path hdr = manager.environmentFile(info.uuid);
            const bool ok = !hdr.empty() && std::filesystem::file_size(hdr) > 0;
            std::cout << "  extraer " << info.name << ": " << (ok ? hdr.string() : "FALLO") << " en "
                      << secondsSince(start) << " s\n";
            failures += ok ? 0 : 1;
            // La segunda vez no se vuelve a extraer.
            start = Clock::now();
            manager.environmentFile(info.uuid);
            std::cout << "    segunda vez: " << secondsSince(start) << " s\n";
        }
    }

    // --- Proyecto y recientes ---
    const auto reopened = project::openProject(project.folder);
    const bool reopened_ok = reopened && reopened->name == "Prueba";
    std::cout << "  reabrir proyecto: " << (reopened_ok ? "OK" : "FALLO") << "\n";
    failures += reopened_ok ? 0 : 1;
    const auto recent = project::recentProjects();
    std::cout << "  recientes: " << recent.size()
              << (recent.empty() ? "" : ", el primero " + recent.front().name) << "\n";
    project::removeRecentProject(project.file);

    std::cout << (failures == 0 ? "TODO OK" : "HAY FALLOS: " + std::to_string(failures)) << "\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
