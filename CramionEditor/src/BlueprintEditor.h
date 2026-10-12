#ifndef CRAMION_EDITOR_BLUEPRINT_EDITOR_H
#define CRAMION_EDITOR_BLUEPRINT_EDITOR_H

// Estado de la pestana Blueprint de actor (EditorBlueprint.cpp) y del modal
// "Nuevo Blueprint de actor".

#include "VsGraphEditor.h"

#include <CramionCore/ecs/Blueprint.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace cramion::editor {

struct BlueprintEditorState {
    Uuid prefab{};
    std::filesystem::path prefab_path;
    ecs::BlueprintInfo info;
    vsedit::Document graph;          // Event Graph
    unsigned int dock = 0;
    unsigned int center_dock = 0;
    bool layout_built = false;
    bool graph_focused = false;      // Ctrl+Z/Y/S van al grafo
    std::string component_filter;
    bool show_bones = true;
    bool show_sockets = true;
    enum class Details : std::uint8_t { Entity, Graph, Class } details = Details::Entity;
    // Estado de la pestana (EditorBlueprint.cpp).
    bool info_dirty = false;         // descripcion/categoria/tick sin guardar
    bool save_requested = false;     // Ctrl+S o "Guardar" del grafo: se guarda al principio del frame
    bool show_graph = false;         // centro: false = Ventana grafica, true = Event Graph
    int last_details_node = 0;       // para pasar Detalles al nodo/variable que se elige
    std::string last_details_variable;
    Uuid last_active{};              // para pasar Detalles al objeto que se elige
    std::string find_filter;         // Buscar (nodos del grafo)
};

struct NewBlueprintRequest {         // modal "Nuevo Blueprint de actor"
    bool open = false;
    Uuid source{};
    assets::AssetType source_type = assets::AssetType::Model;
    char name[128] = {};
    int bones = 0;                   // 0 Ninguno, 1 Todos
    bool collider = false;
    bool focus_name = false;         // el nombre recibe el foco al abrirse
    std::string source_name;         // para el texto del modal
    std::filesystem::path folder;    // relativa a Assets (vacia = Blueprints)
};

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_BLUEPRINT_EDITOR_H
