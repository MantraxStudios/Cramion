// Fuzzing de los .crdata (asset/CrData.h): la cabecera, los modelos (nodos,
// animaciones y piezas con sus texturas incrustadas) y los cielos HDR. Los
// .crdata viajan en los proyectos, los paquetes de la Store y los DataPacks.

#include "FuzzCommon.h"

#include "asset/CrData.h"

using namespace cramion;

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {
    const std::filesystem::path seeds = fuzz::seedsFolder(argc, argv);
    if (seeds.empty()) return 0;
    std::error_code e;
    std::filesystem::create_directories(seeds, e);
    assets::crdata::Header header;
    header.type = assets::AssetType::Model;
    header.uuid = Uuid::generate();
    header.name = "Cubo";
    header.source = "Modelos/cubo.fbx";
    header.settings_json = "{\"scale\":1}";
    assets::crdata::ModelContent model;
    assets::ModelNode root;
    root.name = "Raiz";
    model.nodes.push_back(root);
    assets::ModelNode child;
    child.name = "Hijo";
    child.parent = 0;
    model.nodes.push_back(child);
    model.animated = true;
    model.animation_names = {"Andar", "Correr"};
    assets::crdata::writeModel(seeds / "modelo.crdata", header, model);
    header.type = assets::AssetType::Environment;
    header.name = "Cielo";
    assets::crdata::writeEnvironment(seeds / "cielo.crdata", header, ".hdr",
                                     std::vector<std::uint8_t>{'#', '?', 'R', 'A', 'D', 'I', 'A', 'N', 'C', 'E'});
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::filesystem::path file = fuzz::writeTemp("entrada.crdata", data, size);
    if (!assets::crdata::readHeader(file)) return 0;
    {
        assets::crdata::Header header;
        assets::crdata::ModelContent content;
        assets::crdata::readModel(file, header, content);
    }
    {
        assets::crdata::Header header;
        assets::crdata::ModelContent content;
        assets::crdata::readModelGeometry(file, header, content);
        assets::crdata::ModelContent some;
        assets::crdata::readModelGeometry(file, header, some, {true, false});
    }
    {
        assets::crdata::Header header;
        std::string extension;
        std::vector<std::uint8_t> bytes;
        assets::crdata::readEnvironment(file, header, extension, bytes);
    }
    return 0;
}
