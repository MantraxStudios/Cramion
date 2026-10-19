#ifndef CRAMION_EDITOR_VS_GRAPH_EDITOR_H
#define CRAMION_EDITOR_VS_GRAPH_EDITOR_H

// Edicion de un grafo de Visual Script sin ventana propia: lienzo, Mi
// Blueprint (grafos, funciones, macros y variables), Detalles y resultados de
// compilacion. Lo usan la pestana Blueprint (EditorBlueprint.cpp) y el editor
// .crgraph suelto. El lienzo ensena un grafo cada vez (Document::active): el
// Event Graph, el Construction Script o una funcion / macro.

#include "NodeGraph.h"

#include <CramionCore/scripting/Scripting.h>
#include <CramionCore/scripting/VisualScript.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace cramion::editor::vsedit {

struct GraphHistory {
    std::deque<std::string> undo, redo;
    std::string current;
    static constexpr std::size_t kMax = 200;
    void reset(const std::string& state);
    bool push(std::string state);          // false si no cambio
    bool undoTo(std::string& out);
    bool redoTo(std::string& out);
};

enum class Status : std::uint8_t { Unknown, Dirty, Good, Warnings, Errors };

// Un objeto soltado en el lienzo (payload de entidad: su Uuid), visto desde
// el Blueprint. ok = es la raiz o esta debajo de ella.
struct EntityDrop {
    bool ok = false;
    bool root = false;             // la raiz del Blueprint: nodo Self
    std::string path;              // ruta desde la raiz (vscript::entityChildPath)
    std::string name;              // nombre del objeto
    std::string error;             // por que no (si !ok)
    std::vector<std::pair<std::string, std::string>> components;  // (nombre, etiqueta) de sus componentes
};

struct Host {
    std::filesystem::path assets_root;
    // Soltar un objeto (Componentes o Jerarquia) en el lienzo: crea su referencia
    // relativa a Self. Vacio = el lienzo no acepta objetos (editor .crgraph suelto).
    std::function<EntityDrop(const Uuid& uuid)> entity_drop;
    // Valores en vivo del objeto que se depura (vacio o false = sin Play).
    std::function<bool(const std::string& relative, scripting::ScriptSystem::VisualScriptDebug& out)> debug;
    std::function<void(const std::string& relative)> reload;       // scripts_.reloadFile
    std::function<std::vector<std::string>()> entity_names;        // para el selector Entity ("Self" primero)
    // Puntos de ruptura del grafo (scripts_.setVisualScriptBreakpoints); se llama al cambiar.
    std::function<void(const std::string& relative, const std::vector<int>& nodes)> breakpoints;
    // Aviso corto al usuario (pushToast): "No se pueden enlazar", motivo.
    std::function<void(const std::string& title, const std::string& text)> notify;
};

struct Document {
    std::filesystem::path path;      // absoluto
    std::string relative;            // relativo a Assets/
    vscript::Graph graph;
    GraphHistory history;
    nodegraph::Canvas canvas;
    vscript::CompileResult compile;
    bool dirty = false;              // cambios sin guardar
    bool needs_compile = true;       // cambios sin compilar (estado Dirty)
    bool focused = false;            // el lienzo tiene el foco (atajos)
    int details_node = 0;            // 0 = ninguno
    std::string details_variable;    // vacio = ninguna
    // estado interno de busqueda/portapapeles (privado de VsGraphEditor.cpp)
    std::string search;
    core::Vec2 create_at{};
    std::optional<nodegraph::PinRef> create_from;
    std::vector<vscript::Node> clipboard;
    std::vector<vscript::Link> clipboard_links;
    std::vector<int> recent;
    bool compiled = false;               // compileGraph corrio al menos una vez (si no: estado Unknown)
    bool snapshot_pending = false;       // cambio sin instantanea (se toma al soltar el raton)
    bool breakpoints_changed = true;     // avisar a Host::breakpoints
    std::string saved_state;             // JSON del ultimo guardado (dirty = el historial es distinto)
    bool context_sensitive = true;       // busqueda filtrada por el pin arrastrado
    std::optional<nodegraph::PinRef> pin_menu;
    std::string var_drop;                // variable soltada en el lienzo sin Ctrl/Alt (menu Get/Set)
    core::Vec2 var_drop_at{};
    std::optional<EntityDrop> entity_drop;  // objeto soltado con Ctrl (menu de sus componentes)
    core::Vec2 entity_drop_at{};
    // Fase 2: el grafo que se ve ("" Event Graph, "construction", "fn:X",
    // "macro:X"), la funcion/macro en Detalles y los pines vigilados en Play
    // (nodo, pin, salida): burbuja fija con su valor.
    std::string active;
    std::string details_function;
    std::set<std::tuple<int, int, bool>> watches;
    std::map<std::string, std::pair<core::Vec2, float>> views;  // pan y zoom de cada grafo al dejarlo
};

bool open(Document& doc, const std::filesystem::path& file, const std::filesystem::path& assets_root,
          std::string* error = nullptr);
bool save(Document& doc, std::string* error = nullptr);
// compileGraph + save (si compila) + host.reload. Devuelve compile.ok.
bool compile(Document& doc, const Host& host);
Status status(const Document& doc);
bool undo(Document& doc);
bool redo(Document& doc);
// Ensena otro grafo del documento ("" Event Graph, "construction", "fn:X",
// "macro:X"). El Construction Script se crea con su nodo la primera vez.
void showGraph(Document& doc, const std::string& key);
// Lleva la vista a un nodo (cambia de grafo si hace falta), lo elige y lo ensena en Detalles.
void focusNode(Document& doc, int id);
// Nombre del grafo para la interfaz ("Event Graph", "Funcion: Danar"...).
std::string graphLabel(const std::string& key);

// save/compile: el usuario lo pidio con el lienzo enfocado (Ctrl+S / F7); lo hace quien llama.
struct DrawResult { bool changed = false; bool save = false; bool compile = false; };
DrawResult drawGraph(Document& doc, const Host& host, ImVec2 size);   // solo el lienzo (+ popups)
bool drawMyBlueprint(Document& doc, const Host& host);                 // contenido, sin Begin/End
bool drawDetails(Document& doc, const Host& host);                     // nodo/variable elegidos
void drawCompilerResults(Document& doc);                               // clic -> frameNode
void drawCompileButton(Document& doc, const Host& host);               // boton + icono de estado

// Editor tipado de un valor (en linea y en Detalles). Entity: muestra "Self"
// pero escribe "" (parseLiteral: vacio = este objeto).
bool drawPinValueEditor(vscript::Pin& pin, float width, bool compact, const Host& host);

// Color de un tipo de pin (estilo Unreal).
ImU32 vsPinColor(vscript::PinType type);

// Catalogo + plantillas de API (vscript::apiNodeTemplates), una vez.
struct PaletteEntry { nodegraph::SearchItem item; vscript::Node node; };
const std::vector<PaletteEntry>& palette();

}  // namespace cramion::editor::vsedit

#endif  // CRAMION_EDITOR_VS_GRAPH_EDITOR_H
