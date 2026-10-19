// Pruebas de VisualScriptRuntime.cpp (el interprete de los .crgraph): eventos
// Start / Update, Delay dentro de un Sequence (la cadena sigue donde iba),
// For Loop y variables, lo del Inspector, Custom Event y temporizadores,
// Flip Flop, Send Event entre objetos, errores, depuracion, recarga en
// caliente y Event Destroy. Fase 2: funciones (puras, con ejecucion y
// recursivas), macros (varias salidas exec, datos y Delay dentro), Timeline,
// Construction Script fuera de Play, depuracion en vivo (cables y entradas),
// firmas, reglas de compileGraph y el archivo (los .crgraph de antes igual).

#include "ApiTest.h"

#include "CramionCore/scripting/VisualScript.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace cramion;
using namespace cramion::apitest;

namespace {

const std::filesystem::path kRoot = std::filesystem::temp_directory_path() / "cramion_vs_runtime_test";

void writeGraph(const char* name, const std::string& text) {
    std::filesystem::create_directories(kRoot / "Graphs");
    std::ofstream(kRoot / "Graphs" / name, std::ios::binary) << text;
}

ecs::Entity withGraph(ecs::World& world, const char* object, const char* graph) {
    ecs::Entity e = world.create(object);
    e.add<vscript::VisualScript>().graph = std::string("Graphs/") + graph;
    return e;
}

int logCount(const ApiFixture& t, const std::string& part) {
    int n = 0;
    for (const auto& [level, message] : t.log) {
        if (message.find(part) != std::string::npos) ++n;
    }
    return n;
}

std::string variable(ApiFixture& t, ecs::Entity e, const char* name) {
    scripting::ScriptSystem::VisualScriptDebug d;
    if (!t.scripts.visualScriptDebug(e, d)) return "(sin instancia)";
    const auto it = d.variables.find(name);
    return it == d.variables.end() ? "(no existe)" : it->second;
}

int dataPins(const vscript::Node& n, vscript::PinType type) {
    int count = 0;
    for (const auto* pins : {&n.inputs, &n.outputs}) {
        for (const vscript::Pin& p : *pins) {
            if (p.type == type) ++count;
        }
    }
    return count;
}

// Biblioteca de nodos de la 2.5 sin Play: tipos de la API, plantillas, JSON,
// aspecto y validacion.
void libraryWithoutPlay() {
    using vscript::PinType;
    // typeFromDocText (Doc.returns).
    check(vscript::typeFromDocText("bool") == PinType::Bool && vscript::typeFromDocText("booleano (false si no puede)") == PinType::Bool &&
              vscript::typeFromDocText("numero") == PinType::Float && vscript::typeFromDocText("texto") == PinType::String &&
              vscript::typeFromDocText("Vec3") == PinType::Vector && vscript::typeFromDocText("Vec3 o nil") == PinType::Vector &&
              vscript::typeFromDocText("Entity") == PinType::Entity && vscript::typeFromDocText("Entity o nil") == PinType::Entity,
          "typeFromDocText: bool, numero, texto, Vec3, Entity y \"X o nil\"");
    check(vscript::typeFromDocText("objeto") == PinType::Any && vscript::typeFromDocText("valor") == PinType::Any &&
              vscript::typeFromDocText("Quat") == PinType::Any && vscript::typeFromDocText("Mesh") == PinType::Any &&
              vscript::typeFromDocText("lista de Entity") == PinType::Any && vscript::typeFromDocText("bool, numero o texto") == PinType::Any &&
              vscript::typeFromDocText("") == PinType::Any,
          "typeFromDocText: objeto, valor, Quat, Mesh, listas y varias opciones -> Any");

    // Plantillas de la API: tipadas, menos pines Any que antes y Set solo si se puede escribir.
    const scripting::api::NativeApi& api = scripting::ScriptSystem::apiRegistry();
    const std::vector<vscript::ApiNodeTemplate>& templates = vscript::apiNodeTemplates(api);
    check(&templates == &vscript::apiNodeTemplates(api), "apiNodeTemplates: se construye una vez (cache)");
    int any_before = 0;
    int any_after = 0;
    int writable = 0;
    int sets = 0;
    for (const scripting::api::Entry& e : api.entries()) {
        if (e.owner.empty() || e.name.empty() || e.name.rfind("__", 0) == 0) continue;
        const std::string callee = e.owner + (e.member() ? ":" : ".") + e.name;
        if (e.kind == scripting::api::Entry::Kind::Property) {
            any_before += dataPins(vscript::makePropertyNode(callee, false, e.doc.description), PinType::Any);
            any_before += dataPins(vscript::makePropertyNode(callee, true, e.doc.description), PinType::Any);
            if (e.assign) ++writable;
        } else {
            any_before += dataPins(vscript::makeCallNode(callee, e.doc.args, e.doc.description), PinType::Any);
        }
    }
    const vscript::ApiNodeTemplate* simulated = nullptr;
    for (const vscript::ApiNodeTemplate& t : templates) {
        any_after += dataPins(t.node, PinType::Any);
        if (t.title.rfind("Set ", 0) == 0) ++sets;
        if (t.node.fn == "Physics.simulatedTime") simulated = &t;
    }
    std::printf("  (pines Any: %d antes, %d con Doc.returns)\n", any_before, any_after);
    check(!templates.empty() && any_after < any_before, "apiNodeTemplates: menos pines Any que antes");
    check(sets == writable, "apiNodeTemplates: Set solo para las propiedades que se pueden escribir");
    check(simulated != nullptr && !simulated->node.outputs.empty() && simulated->node.outputs.back().type == PinType::Float &&
              simulated->category == "API: Physics",
          "apiNodeTemplates: \"numero\" da un pin Float");

    // Conversiones, aspecto, mascaras y Format Text.
    check(vscript::conversionKind(PinType::Bool, PinType::Int) == std::optional<std::string>("conv.bool_to_int") &&
              vscript::conversionKind(PinType::Entity, PinType::Vector) == std::optional<std::string>("entity.get_position") &&
              !vscript::conversionKind(PinType::Int, PinType::Float) && !vscript::conversionKind(PinType::Exec, PinType::Bool),
          "conversionKind: Bool->Int y Entity->Vector; Int->Float ya enlaza");
    check(vscript::nodeLook(vscript::makeNode("math.add")) == vscript::NodeLook::Compact &&
              std::string(vscript::nodeSymbol(vscript::makeNode("math.add"))) == "+" &&
              vscript::nodeLook(vscript::makeNode("event.start")) == vscript::NodeLook::Event &&
              vscript::nodeLook(vscript::makeNode("flow.branch")) == vscript::NodeLook::Flow &&
              vscript::nodeLook(vscript::makeNode("vec.make")) == vscript::NodeLook::Pure &&
              vscript::nodeLook(vscript::makeNode("entity.set_position")) == vscript::NodeLook::Function &&
              std::string(vscript::nodeSymbol(vscript::makeNode("flow.branch"))).empty(),
          "nodeLook / nodeSymbol");
    const std::uint32_t branch_in = vscript::inputTypeMask(vscript::makeNode("flow.branch"));
    check((branch_in & (1u << static_cast<int>(PinType::Exec))) != 0 && (branch_in & (1u << static_cast<int>(PinType::Bool))) != 0 &&
              (branch_in & (1u << static_cast<int>(PinType::Vector))) == 0 &&
              vscript::outputTypeMask(vscript::makeNode("vec.make")) == (1u << static_cast<int>(PinType::Vector)),
          "mascaras de tipos de entradas y salidas");
    check(vscript::formatTextArguments("Hola {a} y {b}, otra vez {a}") == std::vector<std::string>{"a", "b"},
          "formatTextArguments: los {nombre} sin repetir");
    const vscript::NodeInfo* key = vscript::findNodeInfo("event.key");
    check(key != nullptr && key->inputs.size() > 1 && key->inputs[1].options.size() == 3 && vscript::findNodeInfo("flow.switch_int") != nullptr &&
              vscript::findNodeInfo("flow.switch_int")->dynamic_outputs,
          "catalogo: Key Event con opciones y Switch con salidas dinamicas");

    // Un Switch de 5 casos sobrevive a guardar y cargar (con sus enlaces).
    vscript::Graph graph;
    const bool parsed = vscript::graphFromJson(R"({
        "nodes": [
            {"id": 1, "kind": "event.start"},
            {"id": 2, "kind": "flow.switch_int", "outputs": [{"name": "Default", "type": "exec"}, {"name": "0", "type": "exec"},
             {"name": "1", "type": "exec"}, {"name": "2", "type": "exec"}, {"name": "3", "type": "exec"}, {"name": "4", "type": "exec"}]},
            {"id": 3, "kind": "debug.print", "disabled": true, "comment_color": 4278190335},
            {"id": 4, "kind": "debug.print"}
        ],
        "links": [{"from": 1, "to": 2}, {"from": 2, "out": "4", "to": 3}, {"from": 2, "out": "3", "to": 4}]
    })", graph);
    vscript::Graph again;
    const bool reparsed = parsed && vscript::graphFromJson(vscript::graphToJson(graph), again);
    const vscript::Node* sw = again.find(2);
    const vscript::Node* off = again.find(3);
    check(reparsed && sw != nullptr && sw->outputs.size() == 6 && sw->outputs[5].name == "4" && again.links.size() == 3 &&
              again.links[1].from_pin == 5,
          "Switch de 5 casos: guardar y cargar conserva las salidas y los enlaces");
    check(off != nullptr && off->disabled && off->comment_color == 4278190335u && !again.find(4)->disabled,
          "JSON: disabled y comment_color ida y vuelta");
    const std::string plain = vscript::graphToJson(vscript::exampleGraph());
    check(plain.find("disabled") == std::string::npos && plain.find("comment_color") == std::string::npos,
          "JSON v1: un grafo sin nodos desactivados se guarda como antes");

    // Validacion: un nodo desactivado no da errores ni deja usar su valor.
    vscript::Graph checked;
    vscript::graphFromJson(R"({
        "nodes": [
            {"id": 1, "kind": "event.start"},
            {"id": 2, "kind": "var.set", "fn": "noExiste", "disabled": true},
            {"id": 3, "kind": "debug.print"}
        ],
        "links": [{"from": 1, "to": 2}, {"from": 2, "to": 3}]
    })", checked);
    check(vscript::compileGraph(checked).ok, "compileGraph: un nodo desactivado no se comprueba");
    checked.find(2)->disabled = false;
    check(!vscript::compileGraph(checked).ok, "compileGraph: el mismo nodo activo es un error");
    vscript::Graph used;
    vscript::graphFromJson(R"({
        "nodes": [{"id": 1, "kind": "event.start"}, {"id": 2, "kind": "math.add", "disabled": true}, {"id": 3, "kind": "debug.print"}],
        "links": [{"from": 1, "to": 3}, {"from": 2, "to": 3, "in": "Texto"}]
    })", used);
    check(!vscript::compileGraph(used).ok, "compileGraph: no se puede usar el valor de un nodo puro desactivado");
}

// --- Fase 2: funciones, macros, Timeline y Construction Script ---------------------

// Funciones: Doble (pura), Factorial (pura y recursiva) y Saludar (con ejecucion).
const char* const kFunctionsGraph = R"({
    "variables": [{"name": "fact", "type": "float", "value": "0"}],
    "functions": [
        {"name": "Doble", "kind": "function", "pure": true, "inputs": [{"name": "X", "type": "float", "value": "0"}],
         "outputs": [{"name": "Y", "type": "float"}]},
        {"name": "Factorial", "kind": "function", "pure": true, "inputs": [{"name": "N", "type": "float", "value": "1"}],
         "outputs": [{"name": "R", "type": "float"}]},
        {"name": "Saludar", "kind": "function", "inputs": [{"name": "Texto", "type": "string", "value": "hola"}], "outputs": []}
    ],
    "nodes": [
        {"id": 1, "kind": "event.start"},
        {"id": 2, "kind": "fn.call", "fn": "Saludar", "values": {"Texto": "funcion saludar"}},
        {"id": 3, "kind": "debug.print"},
        {"id": 4, "kind": "str.format", "inputs": [{"name": "Formato", "type": "string", "value": "doble={d}"}, {"name": "d", "type": "any"}]},
        {"id": 5, "kind": "fn.call", "fn": "Doble", "values": {"X": "21"}},
        {"id": 6, "kind": "var.set", "fn": "fact"},
        {"id": 7, "kind": "fn.call", "fn": "Factorial", "values": {"N": "5"}},
        {"id": 10, "kind": "fn.entry", "fn": "Doble", "graph": "fn:Doble"},
        {"id": 11, "kind": "fn.return", "fn": "Doble", "graph": "fn:Doble"},
        {"id": 12, "kind": "math.mul", "graph": "fn:Doble", "values": {"B": "2"}},
        {"id": 20, "kind": "fn.entry", "fn": "Factorial", "graph": "fn:Factorial"},
        {"id": 21, "kind": "flow.branch", "graph": "fn:Factorial"},
        {"id": 22, "kind": "logic.less_equal", "graph": "fn:Factorial", "values": {"B": "1"}},
        {"id": 23, "kind": "fn.return", "fn": "Factorial", "graph": "fn:Factorial", "values": {"R": "1"}},
        {"id": 24, "kind": "fn.return", "fn": "Factorial", "graph": "fn:Factorial"},
        {"id": 25, "kind": "math.mul", "graph": "fn:Factorial"},
        {"id": 26, "kind": "fn.call", "fn": "Factorial", "graph": "fn:Factorial"},
        {"id": 27, "kind": "math.sub", "graph": "fn:Factorial", "values": {"B": "1"}},
        {"id": 30, "kind": "fn.entry", "fn": "Saludar", "graph": "fn:Saludar"},
        {"id": 31, "kind": "debug.print", "graph": "fn:Saludar"},
        {"id": 32, "kind": "fn.return", "fn": "Saludar", "graph": "fn:Saludar"}
    ],
    "links": [
        {"from": 1, "to": 2},
        {"from": 2, "to": 6},
        {"from": 7, "out": "R", "to": 6, "in": "Valor"},
        {"from": 6, "to": 3},
        {"from": 5, "out": "Y", "to": 4, "in": "d"},
        {"from": 4, "to": 3, "in": "Texto"},
        {"from": 10, "to": 11},
        {"from": 10, "out": "X", "to": 12, "in": "A"},
        {"from": 12, "to": 11, "in": "Y"},
        {"from": 20, "to": 21},
        {"from": 20, "out": "N", "to": 22, "in": "A"},
        {"from": 22, "to": 21, "in": "Condicion"},
        {"from": 21, "out": "Verdadero", "to": 23},
        {"from": 21, "out": "Falso", "to": 24},
        {"from": 20, "out": "N", "to": 25, "in": "A"},
        {"from": 26, "out": "R", "to": 25, "in": "B"},
        {"from": 25, "to": 24, "in": "R"},
        {"from": 20, "out": "N", "to": 27, "in": "A"},
        {"from": 27, "to": 26, "in": "N"},
        {"from": 30, "to": 31},
        {"from": 30, "out": "Texto", "to": 31, "in": "Texto"},
        {"from": 31, "to": 32}
    ]
})";

// Macros: Comprobar (dos salidas exec y un dato) y Esperar (un Delay dentro).
const char* const kMacrosGraph = R"({
    "functions": [
        {"name": "Comprobar", "kind": "macro",
         "inputs": [{"name": "Entrada", "type": "exec"}, {"name": "Valor", "type": "float", "value": "0"}],
         "outputs": [{"name": "Si", "type": "exec"}, {"name": "No", "type": "exec"}, {"name": "Doble", "type": "float"}]},
        {"name": "Esperar", "kind": "macro", "inputs": [{"name": "Entrada", "type": "exec"}], "outputs": [{"name": "Salida", "type": "exec"}]}
    ],
    "nodes": [
        {"id": 1, "kind": "event.start"},
        {"id": 2, "kind": "flow.sequence", "outputs": [{"name": "Then 0", "type": "exec"}, {"name": "Then 1", "type": "exec"},
         {"name": "Then 2", "type": "exec"}]},
        {"id": 3, "kind": "macro.call", "fn": "Comprobar", "values": {"Valor": "5"}},
        {"id": 4, "kind": "debug.print"},
        {"id": 5, "kind": "str.format", "inputs": [{"name": "Formato", "type": "string", "value": "macro si doble={d}"}, {"name": "d", "type": "any"}]},
        {"id": 6, "kind": "debug.print", "values": {"Texto": "macro no por error"}},
        {"id": 7, "kind": "macro.call", "fn": "Comprobar", "values": {"Valor": "-1"}},
        {"id": 8, "kind": "debug.print", "values": {"Texto": "macro no negativo"}},
        {"id": 9, "kind": "macro.call", "fn": "Esperar"},
        {"id": 10, "kind": "debug.print", "values": {"Texto": "despues de la macro con delay"}},
        {"id": 20, "kind": "macro.entry", "fn": "Comprobar", "graph": "macro:Comprobar"},
        {"id": 21, "kind": "flow.branch", "graph": "macro:Comprobar"},
        {"id": 22, "kind": "logic.greater", "graph": "macro:Comprobar", "values": {"B": "0"}},
        {"id": 23, "kind": "macro.exit", "fn": "Comprobar", "graph": "macro:Comprobar"},
        {"id": 24, "kind": "math.mul", "graph": "macro:Comprobar", "values": {"B": "2"}},
        {"id": 30, "kind": "macro.entry", "fn": "Esperar", "graph": "macro:Esperar"},
        {"id": 31, "kind": "flow.delay", "graph": "macro:Esperar", "values": {"Segundos": "0.25"}},
        {"id": 32, "kind": "macro.exit", "fn": "Esperar", "graph": "macro:Esperar"}
    ],
    "links": [
        {"from": 1, "to": 2},
        {"from": 2, "out": "Then 0", "to": 3},
        {"from": 3, "out": "Si", "to": 4},
        {"from": 3, "out": "Doble", "to": 5, "in": "d"},
        {"from": 5, "to": 4, "in": "Texto"},
        {"from": 3, "out": "No", "to": 6},
        {"from": 2, "out": "Then 1", "to": 7},
        {"from": 7, "out": "No", "to": 8},
        {"from": 2, "out": "Then 2", "to": 9},
        {"from": 9, "out": "Salida", "to": 10},
        {"from": 20, "out": "Entrada", "to": 21},
        {"from": 20, "out": "Valor", "to": 22, "in": "A"},
        {"from": 22, "to": 21, "in": "Condicion"},
        {"from": 21, "out": "Verdadero", "to": 23, "in": "Si"},
        {"from": 21, "out": "Falso", "to": 23, "in": "No"},
        {"from": 20, "out": "Valor", "to": 24, "in": "A"},
        {"from": 24, "to": 23, "in": "Doble"},
        {"from": 30, "out": "Entrada", "to": 31},
        {"from": 31, "out": "Completado", "to": 32, "in": "Salida"}
    ]
})";

// Timeline de 1 s: Alpha de 0 a 10 (y una pista vector); Update pone la variable.
const char* const kTimelineGraph = R"({
    "variables": [{"name": "valor", "type": "float", "value": "0"}],
    "nodes": [
        {"id": 1, "kind": "event.start"},
        {"id": 2, "kind": "flow.timeline", "timeline": {"length": 1, "tracks": [
            {"name": "Alpha", "type": "float", "keys": [{"t": 0, "v": 0}, {"t": 1, "v": 10}]},
            {"name": "Pos", "type": "vec3", "keys": [[0, [0, 0, 0]], [1, [2, 4, 6]]]}]}},
        {"id": 3, "kind": "var.set", "fn": "valor"},
        {"id": 4, "kind": "debug.print", "values": {"Texto": "timeline fin"}}
    ],
    "links": [
        {"from": 1, "to": 2, "in": "Play desde el inicio"},
        {"from": 2, "out": "Update", "to": 3},
        {"from": 2, "out": "Alpha", "to": 3, "in": "Valor"},
        {"from": 2, "out": "Finished", "to": 4}
    ]
})";

// Construction Script: escala con la variable del Inspector y un Spawn de "Plantilla".
const char* const kConstructionGraph = R"({
    "variables": [{"name": "tam", "type": "vec3", "value": "1 1 1"}],
    "nodes": [
        {"id": 1, "kind": "event.construction", "graph": "construction"},
        {"id": 2, "kind": "entity.set_scale", "graph": "construction"},
        {"id": 3, "kind": "var.get", "fn": "tam", "graph": "construction"},
        {"id": 4, "kind": "entity.spawn", "graph": "construction", "values": {"Posicion": "0 5 0"}},
        {"id": 5, "kind": "entity.find", "graph": "construction", "values": {"Nombre": "Plantilla"}}
    ],
    "links": [
        {"from": 1, "to": 2},
        {"from": 3, "to": 2, "in": "Escala"},
        {"from": 2, "to": 4},
        {"from": 5, "to": 4, "in": "Prefab"}
    ]
})";

bool compiles(const std::string& text) {
    vscript::Graph g;
    return vscript::graphFromJson(text, g) && vscript::compileGraph(g).ok;
}

const vscript::Link* linkFrom(const vscript::Graph& g, int from, int to) {
    for (const vscript::Link& l : g.links) {
        if (l.from_node == from && l.to_node == to) return &l;
    }
    return nullptr;
}

// Lo de la fase 2 sin Play: archivo, firmas, validacion y curvas.
void phase2WithoutPlay() {
    using vscript::PinType;
    std::printf("Fase 2 sin Play\n");
    // Un grafo de antes se guarda igual (sin claves nuevas).
    const std::string plain = vscript::graphToJson(vscript::exampleGraph());
    check(plain.find("\"functions\"") == std::string::npos && plain.find("\"graph\"") == std::string::npos &&
              plain.find("\"timeline\"") == std::string::npos && plain.find("\"version\": 1") != std::string::npos,
          "JSON: un grafo sin fase 2 se guarda como antes (version 1, sin claves nuevas)");

    // Funciones: los pines salen de la firma.
    vscript::Graph g;
    check(vscript::graphFromJson(kFunctionsGraph, g) && g.functions.size() == 3, "funciones: se leen las firmas");
    const vscript::Node* doble = g.find(5);
    const vscript::Node* saludar = g.find(2);
    check(doble != nullptr && doble->pure && doble->inputs.size() == 1 && doble->inputs[0].name == "X" && doble->inputs[0].value == "21" &&
              doble->outputs.size() == 1 && doble->outputs[0].type == PinType::Float,
          "fn.call puro: sin exec, X = 21 y Y Float");
    check(saludar != nullptr && !saludar->pure && saludar->inputs.size() == 2 && saludar->inputs[0].type == PinType::Exec &&
              saludar->outputs.size() == 1,
          "fn.call con ejecucion: exec de entrada y de salida");
    check(g.find(10) != nullptr && g.find(10)->outputs.size() == 2 && g.find(10)->outputs[1].name == "X" && g.find(11)->inputs.size() == 2,
          "fn.entry da los parametros y fn.return pide los resultados");
    const vscript::CompileResult compiled = vscript::compileGraph(g);
    for (const vscript::NodeError& e : compiled.errors) std::printf("    nodo %d: %s\n", e.node, e.message.c_str());
    check(compiled.ok, "compileGraph: el grafo de funciones no tiene errores");
    vscript::Graph again;
    const std::string saved = vscript::graphToJson(g);
    check(saved.find("\"version\": 2") != std::string::npos && vscript::graphFromJson(saved, again) && again.functions.size() == 3 &&
              again.find(26) != nullptr && again.find(26)->graph == "fn:Factorial" && again.links.size() == g.links.size() &&
              again.find(5)->inputs[0].value == "21",
          "JSON ida y vuelta: firmas, grafo de cada nodo, enlaces y valores (version 2)");

    // Cambiar la firma: los enlaces y valores siguen por nombre.
    vscript::Subgraph* def = g.findSubgraph("fn:Doble");
    def->inputs.insert(def->inputs.begin(), vscript::Pin{"Escala", PinType::Float, "1", {}});
    g.syncSubgraph("fn:Doble");
    const vscript::Link* x_link = linkFrom(g, 10, 12);
    check(g.find(5)->inputs.size() == 2 && g.find(5)->inputs[0].name == "Escala" && g.find(5)->inputs[1].value == "21" &&
              x_link != nullptr && x_link->from_pin == 2,
          "syncSubgraph: parametro nuevo; valores y enlaces por nombre");
    check(g.renameSubgraph("fn:Doble", "Duplicar") && g.find(5)->fn == "Duplicar" && g.find(10)->graph == "fn:Duplicar" &&
              g.findSubgraph("fn:Duplicar") != nullptr && !g.renameSubgraph("fn:Duplicar", "Factorial"),
          "renameSubgraph: sus nodos y sus llamadas (no a un nombre usado)");
    check(vscript::compileGraph(g).ok, "y sigue compilando");
    g.removeSubgraph("fn:Duplicar");
    check(g.find(5) == nullptr && g.find(10) == nullptr && g.find(12) == nullptr && g.findSubgraph("fn:Duplicar") == nullptr &&
              linkFrom(g, 5, 4) == nullptr,
          "removeSubgraph: con sus nodos, sus llamadas y sus enlaces");

    // addSubgraph: entrada y salida enlazadas; nombres unicos.
    vscript::Graph made;
    const std::string first = made.addSubgraph("Mia", vscript::SubgraphKind::Function).key();
    const std::string second = made.addSubgraph("Mia", vscript::SubgraphKind::Function).key();
    const std::string macro = made.addSubgraph({}, vscript::SubgraphKind::Macro).key();
    check(first == "fn:Mia" && second == "fn:Mia2" && macro == "macro:NuevaMacro" && made.nodes.size() == 6 && made.links.size() == 3 &&
              vscript::compileGraph(made).ok,
          "addSubgraph: entrada + salida enlazadas y nombre unico");
    made.editing = "fn:Mia";
    check(made.add(vscript::makeNode("debug.print")).graph == "fn:Mia", "Graph::editing: los nodos nuevos van al grafo que se ve");
    std::string why;
    check(!made.connect(1, 0, 3, 0, &why) && !why.empty(), "connect: no enlaza nodos de grafos distintos");

    // Macros: pines exec en la firma.
    vscript::Graph macros;
    check(vscript::graphFromJson(kMacrosGraph, macros) && macros.find(3)->inputs.size() == 2 &&
              macros.find(3)->outputs.size() == 3 && macros.find(3)->outputs[1].type == PinType::Exec &&
              macros.find(20)->outputs.size() == 2 && macros.find(23)->inputs.size() == 3,
          "macro.call / entry / exit con los pines de la firma (exec incluidos)");
    check(vscript::compileGraph(macros).ok, "compileGraph: el grafo de macros no tiene errores");

    // Reglas de cada grafo.
    const std::string base = R"({"functions": [{"name": "F", "kind": "function"}, {"name": "M", "kind": "macro",
        "inputs": [{"name": "In", "type": "exec"}], "outputs": [{"name": "Out", "type": "exec"}]}],
        "nodes": [{"id": 1, "kind": "fn.entry", "fn": "F", "graph": "fn:F"}, {"id": 2, "kind": "macro.entry", "fn": "M", "graph": "macro:M"},
                  {"id": 3, "kind": "event.start"})";
    check(compiles(base + "]}"), "base de las reglas: compila");
    check(!compiles(base + R"(, {"id": 4, "kind": "flow.delay", "graph": "fn:F"}]})"), "una funcion no puede tener Delay");
    check(compiles(base + R"(, {"id": 4, "kind": "flow.delay", "graph": "macro:M"}]})"), "una macro si");
    check(!compiles(base + R"(, {"id": 4, "kind": "event.update", "graph": "fn:F"}]})"), "un evento no va en una funcion");
    check(!compiles(base + R"(, {"id": 4, "kind": "flow.timeline", "graph": "fn:F"}]})"), "un Timeline no va en una funcion");
    check(!compiles(base + R"(, {"id": 4, "kind": "macro.call", "fn": "M", "graph": "macro:M"}]})"), "una macro no se usa a si misma");
    check(!compiles(base + R"(, {"id": 4, "kind": "event.construction"}]})"), "Construction Script solo en su grafo");
    check(compiles(base + R"(, {"id": 4, "kind": "event.construction", "graph": "construction"}]})"), "y en su grafo, si");
    check(!compiles(base + R"(, {"id": 4, "kind": "fn.call", "fn": "NoExiste"}]})"), "una llamada a una funcion que no existe");
    check(!compiles(base + R"(, {"id": 4, "kind": "debug.print", "graph": "fn:Otra"}]})"), "un nodo en un grafo que no existe");
    check(!compiles(base + R"(, {"id": 4, "kind": "debug.print"}], "links": [{"from": 1, "to": 4}]})"),
          "un enlace de una funcion al Event Graph");
    check(!compiles(base + R"(, {"id": 4, "kind": "flow.delay", "graph": "macro:M"},
                     {"id": 5, "kind": "macro.call", "fn": "M", "graph": "fn:F"}]})"),
          "una funcion no puede usar una macro que espera");

    // Timeline: curvas, archivo y pines.
    vscript::TimelineTrack track;
    track.keys = {vscript::TimelineKey{0.0f, core::Vec3{0.0f, 0.0f, 0.0f}, vscript::CurveInterp::Linear},
                  vscript::TimelineKey{1.0f, core::Vec3{10.0f, 0.0f, 0.0f}, vscript::CurveInterp::Smooth},
                  vscript::TimelineKey{2.0f, core::Vec3{20.0f, 0.0f, 0.0f}, vscript::CurveInterp::Constant},
                  vscript::TimelineKey{3.0f, core::Vec3{0.0f, 0.0f, 0.0f}, vscript::CurveInterp::Linear}};
    check(vscript::evaluateTrack(track, -1.0f).x == 0.0f && vscript::evaluateTrack(track, 0.5f).x == 5.0f &&
              vscript::evaluateTrack(track, 1.5f).x == 15.0f && vscript::evaluateTrack(track, 1.25f).x < 12.5f &&
              vscript::evaluateTrack(track, 2.5f).x == 20.0f && vscript::evaluateTrack(track, 9.0f).x == 0.0f,
          "evaluateTrack: lineal, suave, constante y fuera de las claves");
    vscript::Graph timeline;
    check(vscript::graphFromJson(kTimelineGraph, timeline) && timeline.find(2)->timeline.tracks.size() == 2 &&
              timeline.find(2)->outputs.size() == 6 && timeline.find(2)->outputs[5].type == PinType::Vector &&
              linkFrom(timeline, 2, 3) != nullptr,
          "Timeline: una salida por pista (float y vector)");
    vscript::Graph timeline_again;
    check(vscript::graphFromJson(vscript::graphToJson(timeline), timeline_again) &&
              timeline_again.find(2)->timeline.tracks[1].keys.size() == 2 &&
              timeline_again.find(2)->timeline.tracks[1].keys[1].value.z == 6.0f && timeline_again.links.size() == timeline.links.size(),
          "Timeline: JSON ida y vuelta (pistas, claves y enlaces)");
    check(vscript::compileGraph(timeline).ok, "compileGraph: el Timeline no tiene errores");
    timeline.find(2)->timeline.tracks.erase(timeline.find(2)->timeline.tracks.begin());  // sin Alpha
    timeline.syncTimeline(2);
    bool alpha_link = false;
    for (const vscript::Link& l : timeline.links) alpha_link = alpha_link || (l.from_node == 2 && l.to_node == 3 && l.to_pin == 1);
    check(timeline.find(2)->outputs.size() == 5 && !alpha_link && linkFrom(timeline, 2, 4) != nullptr,
          "syncTimeline: quitar una pista quita sus enlaces (los demas siguen)");
    check(compiles(kConstructionGraph), "compileGraph: el Construction Script no tiene errores");
}

}  // namespace

int main() {
    std::printf("VisualScriptRuntime\n");
    std::filesystem::remove_all(kRoot);
    vscript::registerVisualScriptComponents();
    libraryWithoutPlay();
    phase2WithoutPlay();
    writeGraph("Funciones.crgraph", kFunctionsGraph);
    writeGraph("Macros.crgraph", kMacrosGraph);
    writeGraph("Timeline.crgraph", kTimelineGraph);
    writeGraph("Construction.crgraph", kConstructionGraph);

    // Delay dentro de un Sequence: Then 1 espera a que termine Then 0.
    writeGraph("Delay.crgraph", R"({
        "nodes": [
            {"id": 1, "kind": "event.start"},
            {"id": 2, "kind": "flow.sequence"},
            {"id": 3, "kind": "flow.delay", "values": {"Segundos": "0.625"}},
            {"id": 4, "kind": "debug.print", "values": {"Texto": "despues del delay"}},
            {"id": 5, "kind": "debug.print", "values": {"Texto": "then 1"}}
        ],
        "links": [
            {"from": 1, "to": 2},
            {"from": 2, "out": "Then 0", "to": 3},
            {"from": 3, "out": "Completado", "to": 4},
            {"from": 2, "out": "Then 1", "to": 5}
        ]
    })");
    // For Loop: suma de 1 a 4 en una variable y la escribe.
    writeGraph("Suma.crgraph", R"({
        "variables": [{"name": "suma", "type": "int", "value": "0"}, {"name": "saludo", "type": "string", "value": "hola"}],
        "nodes": [
            {"id": 1, "kind": "event.start"},
            {"id": 2, "kind": "flow.for", "values": {"Desde": "1", "Hasta": "4"}},
            {"id": 3, "kind": "var.set", "fn": "suma"},
            {"id": 4, "kind": "var.get", "fn": "suma"},
            {"id": 5, "kind": "math.add"},
            {"id": 6, "kind": "str.append", "values": {"A": "suma="}},
            {"id": 7, "kind": "debug.print"},
            {"id": 8, "kind": "var.get", "fn": "suma"},
            {"id": 9, "kind": "flow.sequence"},
            {"id": 10, "kind": "debug.print"},
            {"id": 11, "kind": "var.get", "fn": "saludo"}
        ],
        "links": [
            {"from": 1, "to": 9},
            {"from": 9, "out": "Then 0", "to": 2},
            {"from": 2, "out": "Cuerpo", "to": 3},
            {"from": 4, "to": 5, "in": "A"},
            {"from": 2, "out": "Indice", "to": 5, "in": "B"},
            {"from": 5, "to": 3, "in": "Valor"},
            {"from": 2, "out": "Completado", "to": 7},
            {"from": 8, "to": 6, "in": "B"},
            {"from": 6, "to": 7, "in": "Texto"},
            {"from": 9, "out": "Then 1", "to": 10},
            {"from": 11, "to": 10, "in": "Texto"}
        ]
    })");
    // Temporizador que repite un Custom Event; la interfaz tambien lo llama.
    writeGraph("Timer.crgraph", R"({
        "variables": [{"name": "cuenta", "type": "int", "value": "0"}],
        "nodes": [
            {"id": 1, "kind": "event.start"},
            {"id": 2, "kind": "flow.set_timer", "values": {"Evento": "Ping", "Segundos": "0.25", "Repetir": "true"}},
            {"id": 3, "kind": "event.custom", "values": {"Nombre": "Ping"}},
            {"id": 4, "kind": "var.set", "fn": "cuenta"},
            {"id": 5, "kind": "var.get", "fn": "cuenta"},
            {"id": 6, "kind": "math.add", "values": {"B": "1"}}
        ],
        "links": [
            {"from": 1, "to": 2},
            {"from": 3, "to": 4},
            {"from": 5, "to": 6, "in": "A"},
            {"from": 6, "to": 4, "in": "Valor"}
        ]
    })");
    // Flip Flop en Update.
    writeGraph("Flip.crgraph", R"({
        "variables": [{"name": "a", "type": "int", "value": "0"}, {"name": "b", "type": "int", "value": "0"}],
        "nodes": [
            {"id": 1, "kind": "event.update"},
            {"id": 2, "kind": "flow.flip_flop"},
            {"id": 3, "kind": "var.set", "fn": "a"},
            {"id": 4, "kind": "var.get", "fn": "a"},
            {"id": 5, "kind": "math.add", "values": {"B": "1"}},
            {"id": 6, "kind": "var.set", "fn": "b"},
            {"id": 7, "kind": "var.get", "fn": "b"},
            {"id": 8, "kind": "math.add", "values": {"B": "1"}}
        ],
        "links": [
            {"from": 1, "to": 2},
            {"from": 2, "out": "A", "to": 3},
            {"from": 4, "to": 5, "in": "A"},
            {"from": 5, "to": 3, "in": "Valor"},
            {"from": 2, "out": "B", "to": 6},
            {"from": 7, "to": 8, "in": "A"},
            {"from": 8, "to": 6, "in": "Valor"}
        ]
    })");
    // Send Event a otro objeto (por nombre) y lo que le llega.
    writeGraph("Emisor.crgraph", R"({
        "nodes": [
            {"id": 1, "kind": "event.start"},
            {"id": 2, "kind": "flow.send_event", "values": {"Objeto": "Receptor", "Evento": "Hola", "Valor": "7"}}
        ],
        "links": [{"from": 1, "to": 2}]
    })");
    writeGraph("Receptor.crgraph", R"({
        "variables": [{"name": "recibido", "type": "any", "value": ""}],
        "nodes": [
            {"id": 1, "kind": "event.custom", "values": {"Nombre": "Hola"}},
            {"id": 2, "kind": "var.set", "fn": "recibido"}
        ],
        "links": [{"from": 1, "to": 2}, {"from": 1, "out": "Valor", "to": 2, "in": "Valor"}]
    })");
    // Una funcion que no existe: error con el nodo y la instancia parada.
    writeGraph("Roto.crgraph", R"({
        "nodes": [
            {"id": 1, "kind": "event.start"},
            {"id": 2, "kind": "call", "fn": "NoExiste.funcion", "inputs": [{"name": "", "type": "exec"}],
             "outputs": [{"name": "", "type": "exec"}]},
            {"id": 3, "kind": "event.update"},
            {"id": 4, "kind": "debug.print", "values": {"Texto": "no deberia salir"}}
        ],
        "links": [{"from": 1, "to": 2}, {"from": 3, "to": 4}]
    })");
    const auto speaker = [](const char* text) {
        return std::string(R"({"nodes": [{"id": 1, "kind": "event.custom", "values": {"Nombre": "Hablar"}},
                          {"id": 2, "kind": "debug.print", "values": {"Texto": ")") + text + R"("}},
                          {"id": 3, "kind": "event.destroy"},
                          {"id": 4, "kind": "debug.print", "values": {"Texto": "adios"}}],
             "links": [{"from": 1, "to": 2}, {"from": 3, "to": 4}]})";
    };
    writeGraph("Habla.crgraph", speaker("version 1"));
    {
        vscript::Graph example = vscript::exampleGraph();
        vscript::saveGraph(example, kRoot / "Graphs" / "Ejemplo.crgraph");
    }

    // Biblioteca de la 2.5: un nodo de cada grupo en un Sequence de Start.
    writeGraph("Biblioteca.crgraph", R"({
        "variables": [{"name": "suma", "type": "int", "value": "0"}, {"name": "lista", "type": "any", "value": ""}],
        "nodes": [
            {"id": 1, "kind": "event.start"},
            {"id": 2, "kind": "flow.sequence", "outputs": [{"name": "Then 0", "type": "exec"}, {"name": "Then 1", "type": "exec"},
             {"name": "Then 2", "type": "exec"}, {"name": "Then 3", "type": "exec"}, {"name": "Then 4", "type": "exec"},
             {"name": "Then 5", "type": "exec"}, {"name": "Then 6", "type": "exec"}, {"name": "Then 7", "type": "exec"},
             {"name": "Then 8", "type": "exec"}, {"name": "Then 9", "type": "exec"}]},
            {"id": 3, "kind": "flow.switch_int", "values": {"Selector": "3"}, "outputs": [{"name": "Default", "type": "exec"},
             {"name": "0", "type": "exec"}, {"name": "1", "type": "exec"}, {"name": "2", "type": "exec"}, {"name": "3", "type": "exec"}]},
            {"id": 4, "kind": "debug.print", "values": {"Texto": "switch int 3"}},
            {"id": 5, "kind": "debug.print", "values": {"Texto": "switch int default"}},
            {"id": 6, "kind": "flow.switch_string", "values": {"Selector": "B"}},
            {"id": 7, "kind": "debug.print", "values": {"Texto": "switch string b"}},
            {"id": 8, "kind": "flow.for_break", "values": {"Desde": "0", "Hasta": "9"}},
            {"id": 9, "kind": "flow.branch"},
            {"id": 10, "kind": "logic.greater_equal", "values": {"B": "3"}},
            {"id": 11, "kind": "var.set", "fn": "suma"},
            {"id": 12, "kind": "math.add"},
            {"id": 13, "kind": "var.get", "fn": "suma"},
            {"id": 14, "kind": "debug.print"},
            {"id": 15, "kind": "str.format", "inputs": [{"name": "Formato", "type": "string", "value": "for break suma={s}"},
             {"name": "s", "type": "any"}]},
            {"id": 16, "kind": "var.get", "fn": "suma"},
            {"id": 17, "kind": "flow.is_valid"},
            {"id": 18, "kind": "entity.find", "values": {"Nombre": "NoExiste"}},
            {"id": 19, "kind": "debug.print", "values": {"Texto": "objeto no valido"}},
            {"id": 20, "kind": "debug.print", "values": {"Texto": "valido por error"}},
            {"id": 21, "kind": "flow.is_valid"},
            {"id": 22, "kind": "entity.self"},
            {"id": 23, "kind": "debug.print", "values": {"Texto": "self valido"}},
            {"id": 24, "kind": "debug.print"},
            {"id": 25, "kind": "flow.select_int", "inputs": [{"name": "Indice", "type": "int", "value": "2"},
             {"name": "Opcion 0", "type": "any", "value": "sel-a"}, {"name": "Opcion 1", "type": "any", "value": "sel-b"},
             {"name": "Opcion 2", "type": "any", "value": "sel-c"}]},
            {"id": 26, "kind": "array.add", "values": {"Elemento": "x"}},
            {"id": 27, "kind": "var.get", "fn": "lista"},
            {"id": 28, "kind": "array.add", "values": {"Elemento": "y"}},
            {"id": 29, "kind": "var.get", "fn": "lista"},
            {"id": 30, "kind": "debug.print"},
            {"id": 31, "kind": "str.format", "inputs": [{"name": "Formato", "type": "string", "value": "lista len={n} find={f}"},
             {"name": "n", "type": "any"}, {"name": "f", "type": "any"}]},
            {"id": 32, "kind": "array.length"},
            {"id": 33, "kind": "var.get", "fn": "lista"},
            {"id": 34, "kind": "array.find", "values": {"Elemento": "y"}},
            {"id": 35, "kind": "var.get", "fn": "lista"},
            {"id": 36, "kind": "debug.print"},
            {"id": 37, "kind": "str.format", "inputs": [{"name": "Formato", "type": "string", "value": "fi={a} up={b} sp={c}"},
             {"name": "a", "type": "any"}, {"name": "b", "type": "any"}, {"name": "c", "type": "any"}]},
            {"id": 38, "kind": "math.finterp_to", "values": {"Actual": "0", "Objetivo": "10", "Delta": "0.1", "Velocidad": "5"}},
            {"id": 39, "kind": "str.upper", "values": {"Texto": "hola"}},
            {"id": 40, "kind": "array.length"},
            {"id": 41, "kind": "str.split", "values": {"Texto": "a,b,c"}},
            {"id": 42, "kind": "physics.line_trace", "values": {"Fin": "0 0 5"}},
            {"id": 43, "kind": "debug.print"},
            {"id": 44, "kind": "str.format", "inputs": [{"name": "Formato", "type": "string", "value": "trace hit={h}"},
             {"name": "h", "type": "any"}]},
            {"id": 45, "kind": "component.add", "values": {"Componente": "Transform"}},
            {"id": 46, "kind": "debug.print"},
            {"id": 47, "kind": "str.format", "inputs": [{"name": "Formato", "type": "string", "value": "componentes add={d} has={h}"},
             {"name": "d", "type": "any"}, {"name": "h", "type": "any"}]},
            {"id": 48, "kind": "component.has", "values": {"Componente": "VisualScript"}}
        ],
        "links": [
            {"from": 1, "to": 2},
            {"from": 2, "out": "Then 0", "to": 3},
            {"from": 3, "out": "3", "to": 4},
            {"from": 3, "out": "Default", "to": 5},
            {"from": 2, "out": "Then 1", "to": 6},
            {"from": 6, "out": "B", "to": 7},
            {"from": 2, "out": "Then 2", "to": 8},
            {"from": 8, "out": "Cuerpo", "to": 9},
            {"from": 8, "out": "Indice", "to": 10, "in": "A"},
            {"from": 10, "to": 9, "in": "Condicion"},
            {"from": 9, "out": "Verdadero", "to": 8, "in": "Break"},
            {"from": 9, "out": "Falso", "to": 11},
            {"from": 13, "to": 12, "in": "A"},
            {"from": 8, "out": "Indice", "to": 12, "in": "B"},
            {"from": 12, "to": 11, "in": "Valor"},
            {"from": 8, "out": "Completado", "to": 14},
            {"from": 16, "to": 15, "in": "s"},
            {"from": 15, "to": 14, "in": "Texto"},
            {"from": 2, "out": "Then 3", "to": 17},
            {"from": 18, "to": 17, "in": "Objeto"},
            {"from": 17, "out": "No valido", "to": 19},
            {"from": 17, "out": "Valido", "to": 20},
            {"from": 2, "out": "Then 4", "to": 21},
            {"from": 22, "to": 21, "in": "Objeto"},
            {"from": 21, "out": "Valido", "to": 23},
            {"from": 2, "out": "Then 5", "to": 24},
            {"from": 25, "to": 24, "in": "Texto"},
            {"from": 2, "out": "Then 6", "to": 26},
            {"from": 27, "to": 26, "in": "Lista"},
            {"from": 26, "to": 28},
            {"from": 29, "to": 28, "in": "Lista"},
            {"from": 28, "to": 30},
            {"from": 33, "to": 32, "in": "Lista"},
            {"from": 35, "to": 34, "in": "Lista"},
            {"from": 32, "to": 31, "in": "n"},
            {"from": 34, "to": 31, "in": "f"},
            {"from": 31, "to": 30, "in": "Texto"},
            {"from": 2, "out": "Then 7", "to": 36},
            {"from": 38, "to": 37, "in": "a"},
            {"from": 39, "to": 37, "in": "b"},
            {"from": 41, "to": 40, "in": "Lista"},
            {"from": 40, "to": 37, "in": "c"},
            {"from": 37, "to": 36, "in": "Texto"},
            {"from": 2, "out": "Then 8", "to": 42},
            {"from": 42, "to": 43},
            {"from": 42, "out": "Hit", "to": 44, "in": "h"},
            {"from": 44, "to": 43, "in": "Texto"},
            {"from": 2, "out": "Then 9", "to": 45},
            {"from": 45, "to": 46},
            {"from": 45, "out": "Hecho", "to": 47, "in": "d"},
            {"from": 48, "to": 47, "in": "h"},
            {"from": 47, "to": 46, "in": "Texto"}
        ]
    })");
    // MultiGate (sin Repetir: cada salida una vez) y Retriggerable Delay (Do N 3 veces seguidas: termina una sola).
    writeGraph("Puertas.crgraph", R"({
        "nodes": [
            {"id": 1, "kind": "event.update"},
            {"id": 2, "kind": "flow.sequence"},
            {"id": 3, "kind": "flow.multigate"},
            {"id": 4, "kind": "debug.print", "values": {"Texto": "multigate-0"}},
            {"id": 5, "kind": "debug.print", "values": {"Texto": "multigate-1"}},
            {"id": 6, "kind": "debug.print", "values": {"Texto": "multigate-2"}},
            {"id": 7, "kind": "flow.do_n", "values": {"N": "3"}},
            {"id": 8, "kind": "flow.retrig_delay", "values": {"Segundos": "0.3"}},
            {"id": 9, "kind": "debug.print", "values": {"Texto": "retrig fin"}}
        ],
        "links": [
            {"from": 1, "to": 2},
            {"from": 2, "out": "Then 0", "to": 3},
            {"from": 3, "out": "Salida 0", "to": 4},
            {"from": 3, "out": "Salida 1", "to": 5},
            {"from": 3, "out": "Salida 2", "to": 6},
            {"from": 2, "out": "Then 1", "to": 7},
            {"from": 7, "out": "Salida", "to": 8},
            {"from": 8, "out": "Completado", "to": 9}
        ]
    })");
    // Un nodo desactivado deja pasar la ejecucion.
    writeGraph("Desactivado.crgraph", R"({
        "nodes": [
            {"id": 1, "kind": "event.start"},
            {"id": 2, "kind": "debug.print", "disabled": true, "values": {"Texto": "no deberia salir: desactivado"}},
            {"id": 3, "kind": "debug.print", "values": {"Texto": "despues del desactivado"}}
        ],
        "links": [{"from": 1, "to": 2}, {"from": 2, "to": 3}]
    })");
    // Un objeto por su nombre: primero el de debajo de la instancia.
    writeGraph("Mano.crgraph", R"({
        "variables": [{"name": "mano", "type": "entity", "value": "Mano_R"}],
        "nodes": [{"id": 1, "kind": "event.start"}]
    })");
    // Objeto hijo por ruta (arrastrar un componente al Event Graph): cada instancia el suyo.
    writeGraph("Ruta.crgraph", R"({
        "variables": [{"name": "hueso", "type": "entity", "value": ""}, {"name": "arma", "type": "entity", "value": ""},
                      {"name": "cuerpo", "type": "entity", "value": ""}, {"name": "nada", "type": "entity", "value": ""}],
        "nodes": [
            {"id": 1, "kind": "event.start"},
            {"id": 2, "kind": "var.set", "fn": "hueso"},
            {"id": 3, "kind": "var.set", "fn": "arma"},
            {"id": 4, "kind": "var.set", "fn": "cuerpo"},
            {"id": 5, "kind": "entity.child", "fn": "Cuerpo/Hueso[2]"},
            {"id": 6, "kind": "entity.child", "fn": "Vieja/Arma"},
            {"id": 7, "kind": "entity.child", "fn": "Cuerpo"},
            {"id": 8, "kind": "var.set", "fn": "nada"},
            {"id": 9, "kind": "entity.child", "fn": "Cuerpo/NoExiste"}
        ],
        "links": [
            {"from": 1, "to": 2}, {"from": 2, "to": 3}, {"from": 3, "to": 4}, {"from": 4, "to": 8},
            {"from": 5, "to": 2, "in": "Valor"}, {"from": 6, "to": 3, "in": "Valor"}, {"from": 7, "to": 4, "in": "Valor"},
            {"from": 9, "to": 8, "in": "Valor"}
        ]
    })");

    ApiFixture t(false);
    t.scripts.setAssetsRoot(kRoot);
    ecs::Entity library = withGraph(t.world, "Biblioteca", "Biblioteca.crgraph");
    withGraph(t.world, "Puertas", "Puertas.crgraph");
    withGraph(t.world, "Desactivado", "Desactivado.crgraph");
    ecs::Entity actor_a = withGraph(t.world, "ActorA", "Mano.crgraph");
    ecs::Entity actor_b = withGraph(t.world, "ActorB", "Mano.crgraph");
    ecs::Entity hand_a = t.world.create("Mano_R");
    ecs::Entity hand_b = t.world.create("Mano_R");
    hand_a.setParent(actor_a);
    hand_b.setParent(actor_b);
    // Dos instancias con la misma forma: Cuerpo/{Hueso, Hueso, Mano/Arma}.
    struct RouteActor {
        ecs::Entity root, body, bone2, weapon;
    };
    const auto routeActor = [&](const char* name) {
        RouteActor r;
        r.root = withGraph(t.world, name, "Ruta.crgraph");
        r.body = t.world.create("Cuerpo", r.root);
        t.world.create("Hueso", r.body);
        r.bone2 = t.world.create("Hueso", r.body);
        r.weapon = t.world.create("Arma", t.world.create("Mano", r.body));
        return r;
    };
    const RouteActor route_a = routeActor("RutaA");
    const RouteActor route_b = routeActor("RutaB");
    check(vscript::entityChildPath(route_a.root, route_a.bone2) == "Cuerpo/Hueso[2]" &&
              vscript::entityChildPath(route_a.root, route_a.weapon) == "Cuerpo/Mano/Arma" &&
              vscript::entityChildPath(route_a.root, route_a.root).empty() &&
              vscript::entityChildPath(route_a.root, route_b.bone2).empty(),
          "entityChildPath: ruta desde la raiz (hermanos repetidos con [n]; fuera del actor = vacia)");
    check(vscript::resolveEntityChildPath(route_b.root, "Cuerpo/Hueso[2]").handle() == route_b.bone2.handle() &&
              !vscript::resolveEntityChildPath(route_b.root, "Cuerpo/Hueso[3]").valid() &&
              vscript::entityChildPathName("Cuerpo/Hueso[2]") == "Hueso [2]" && vscript::entityChildPathName("A\\/B") == "A/B",
          "resolveEntityChildPath y entityChildPathName (con escapes)");
    ecs::Entity delay = withGraph(t.world, "Delay", "Delay.crgraph");
    ecs::Entity suma = withGraph(t.world, "Suma", "Suma.crgraph");
    ecs::Entity custom = withGraph(t.world, "Inspector", "Suma.crgraph");
    custom.get<vscript::VisualScript>().properties.push_back({"saludo", scripting::PropertyType::Text, "adios"});
    ecs::Entity timer = withGraph(t.world, "Timer", "Timer.crgraph");
    ecs::Entity flip = withGraph(t.world, "Flip", "Flip.crgraph");
    withGraph(t.world, "Emisor", "Emisor.crgraph");
    ecs::Entity receiver = withGraph(t.world, "Receptor", "Receptor.crgraph");
    ecs::Entity broken = withGraph(t.world, "Roto", "Roto.crgraph");
    ecs::Entity talker = withGraph(t.world, "Habla", "Habla.crgraph");
    ecs::Entity example = withGraph(t.world, "Ejemplo", "Ejemplo.crgraph");
    std::vector<std::string> messages;
    t.scripts.setMessageListener([&](ecs::Entity e, const std::string& method, const std::string&) {
        messages.push_back(e.name() + ":" + method);
    });

    // Construction Script (fuera de Play, como en el editor).
    t.world.create("Plantilla");
    ecs::Entity builder = withGraph(t.world, "Constructor", "Construction.crgraph");
    builder.get<vscript::VisualScript>().properties.push_back({"tam", scripting::PropertyType::Vector, "2 2 2"});
    check(t.scripts.hasConstructionScript("Graphs/Construction.crgraph") && !t.scripts.hasConstructionScript("Graphs/Delay.crgraph"),
          "hasConstructionScript: solo el grafo que lo tiene");
    const std::size_t entities_before = t.world.entityCount();
    std::string construction_error;
    const bool constructed = t.scripts.runConstructionScript(t.world, builder, &construction_error);
    if (!construction_error.empty()) std::printf("    %s\n", construction_error.c_str());
    check(constructed && builder.localScale().x == 2.0f && t.world.entityCount() == entities_before + 1,
          "Construction Script: la escala del Inspector y un Spawn");
    check(t.scripts.runConstructionScript(t.world, builder, &construction_error) && t.world.entityCount() == entities_before + 1,
          "otra pasada: lo que creo la anterior se borra (no se acumula)");
    int owned = 0;
    for (const entt::entity h : t.world.registry().view<vscript::ConstructionOwned>()) {
        if (t.world.registry().get<vscript::ConstructionOwned>(h).owner == builder.uuid()) ++owned;
    }
    check(owned == 1, "lo creado queda marcado con su dueno (ConstructionOwned)");

    // Funciones, macros y Timeline en Play (con la depuracion en vivo).
    ecs::Entity functions = withGraph(t.world, "Funciones", "Funciones.crgraph");
    withGraph(t.world, "Macros", "Macros.crgraph");
    ecs::Entity timeline = withGraph(t.world, "Timeline", "Timeline.crgraph");
    t.scripts.setVisualScriptLiveDebug(true);

    t.scripts.setVisualScriptDebugging(true);
    t.scripts.start(t.world);
    check(!t.scripts.runConstructionScript(t.world, builder), "en Play el Construction Script no corre");

    t.frame(0.125f);
    check(t.logged("Hola desde Visual Scripting"), "el grafo de ejemplo: Event Start -> Print");
    check(!t.logged("despues del delay") && !t.logged("then 1"), "Delay: la cadena espera (y Then 1 del Sequence tambien)");
    check(t.logged("suma=10"), "For Loop: suma de 1 a 4 en una variable (Get, +, Set, Append)");
    check(variable(t, suma, "suma") == "10", "la variable queda en la instancia");
    check(logCount(t, "hola") == 1 && t.logged("adios"), "variable del Inspector de un objeto (saludo = adios)");
    check(variable(t, receiver, "recibido") == "7", "Send Event: el Custom Event del otro objeto con su valor");
    bool sent = false;
    for (const std::string& m : messages) sent = sent || m == "Receptor:Hola";
    check(sent, "Send Event: tambien a los scripts de C++ del objeto");

    // Biblioteca de la 2.5 (un nodo de cada grupo).
    check(t.logged("switch int 3") && !t.logged("switch int default"), "Switch on Int: la salida con el numero");
    check(t.logged("switch string b"), "Switch on String: la salida con ese nombre");
    check(t.logged("for break suma=3") && variable(t, library, "suma") == "3", "For Loop with Break: Break para el bucle (0+1+2)");
    check(t.logged("objeto no valido") && !t.logged("valido por error") && t.logged("self valido"),
          "Is Valid con salidas: No valido (nil) y Valido (Self)");
    check(t.logged("sel-c") && !t.logged("sel-a") && !t.logged("sel-b"), "Select: la opcion del Indice");
    check(t.logged("lista len=2 find=2"), "Array: Add (cambia la variable), Length y Find");
    check(t.logged("fi=5 up=HOLA sp=3"), "FInterp To, To Upper, Split y Format Text");
    check(t.logged("trace hit=false"), "Line Trace sin nada delante: Hit = false");

    // Fase 2 en Play.
    check(t.logged("funcion saludar"), "funcion con ejecucion: su parametro dentro del cuerpo");
    check(t.logged("doble=42"), "funcion pura: Doble(21) = 42");
    check(variable(t, functions, "fact") == "120", "funcion recursiva: Factorial(5) = 120");
    check(t.logged("macro si doble=10") && !t.logged("macro no por error") && t.logged("macro no negativo"),
          "macro: sale por Si / No segun su entrada y da su dato (Doble)");
    check(!t.logged("despues de la macro con delay"), "macro con Delay: la cadena espera");
    {
        scripting::ScriptSystem::VisualScriptDebug live;
        check(t.scripts.visualScriptDebug(functions, live) && live.flows.contains("1:0") && live.flows.contains("21:1") &&
                  live.inputs.contains("5:0") && live.inputs["5:0"] == "21",
              "depuracion en vivo: cables por los que paso la ejecucion y valor de las entradas");
    }
    check(t.logged("componentes add=false has=true") && t.logged("no se puede cambiar desde un grafo"),
          "Add Component no toca Transform; Has Component");
    check(t.logged("despues del desactivado") && !t.logged("no deberia salir: desactivado"),
          "un nodo desactivado deja pasar la ejecucion");
    check(variable(t, actor_a, "mano") == ApiFixture::entity(hand_a).asString() &&
              variable(t, actor_b, "mano") == ApiFixture::entity(hand_b).asString() && hand_a.handle() != hand_b.handle(),
          "un objeto por su nombre: cada instancia el suyo (\"Mano_R\" de debajo)");
    for (const RouteActor* r : {&route_a, &route_b}) {
        scripting::ScriptSystem::VisualScriptDebug route_debug;
        check(variable(t, r->root, "hueso") == ApiFixture::entity(r->bone2).asString() &&
                  variable(t, r->root, "cuerpo") == ApiFixture::entity(r->body).asString(),
              (std::string("Objeto hijo (ruta): \"Cuerpo/Hueso[2]\" y \"Cuerpo\" de su instancia (") + r->root.name() + ")").c_str());
        check(variable(t, r->root, "arma") == ApiFixture::entity(r->weapon).asString(),
              (std::string("Objeto hijo (ruta): una ruta vieja busca por nombre debajo de Self (") + r->root.name() + ")").c_str());
        check(t.scripts.visualScriptDebug(r->root, route_debug) && !route_debug.failed &&
                  variable(t, r->root, "nada") != ApiFixture::entity(r->bone2).asString(),
              (std::string("Objeto hijo (ruta): una ruta que no existe da nil sin error (") + r->root.name() + ")").c_str());
    }
    check(route_a.bone2.handle() != route_b.bone2.handle() &&
              variable(t, route_a.root, "hueso") != variable(t, route_b.root, "hueso"),
          "Objeto hijo (ruta): dos instancias, dos objetos distintos");

    // Error: la Consola dice el nodo y la instancia se para.
    bool located = false;
    for (const scripting::ScriptError& e : t.scripts.errors()) {
        located = located || (e.file == "Graphs/Roto.crgraph" && e.message.find("nodo 2") != std::string::npos);
    }
    check(located, "error de la API: archivo y nodo");
    scripting::ScriptSystem::VisualScriptDebug debug;
    check(t.scripts.visualScriptDebug(broken, debug) && debug.failed, "la depuracion lo marca como fallado");
    check(!t.logged("no deberia salir"), "una instancia que fallo no sigue (ni su Update)");

    // Ejemplo: Update gira el objeto (Get * Delta -> Make Vector -> Rotate).
    const core::Vec3 before = example.localEulerDegrees();
    t.frame(0.125f);
    check(example.localEulerDegrees().y != before.y, "Event Update -> Rotate con Delta");
    t.frame(0.125f);
    t.frame(0.125f);
    check(!t.logged("despues del delay"), "Delay: aun no (0.5 de 0.625 s, el frame de Start cuenta)");
    check(t.logged("despues de la macro con delay"), "macro con Delay: sigue por la salida de la macro (0.25 s)");
    check(variable(t, timeline, "valor") == "5" && !t.logged("timeline fin"), "Timeline: Update cada frame con la curva (0.5 s -> 5)");
    t.frame(0.125f);
    check(t.logged("despues del delay") && t.logged("then 1"), "Delay terminado: sigue la cadena y despues el Sequence");
    check(variable(t, flip, "a") == "3" && variable(t, flip, "b") == "2", "Flip Flop alterna A / B cada frame");
    for (int i = 0; i < 4; ++i) t.frame(0.125f);
    check(variable(t, timer, "cuenta") == "4", "Set Timer con Repetir: Ping cada 0.25 s (1 s -> 4)");
    check(variable(t, timeline, "valor") == "10" && logCount(t, "timeline fin") == 1, "Timeline: llega al final (10) y Finished una vez");
    t.scripts.callMethod(timer, "Ping", 0.0f);
    check(variable(t, timer, "cuenta") == "5", "la interfaz (callMethod) lanza el Custom Event");

    // Depuracion: hora de cada nodo y ultimo valor de los pines.
    check(t.scripts.visualScriptDebug(suma, debug) && debug.graph == "Graphs/Suma.crgraph" && debug.executed.contains(1) &&
              debug.values.contains("2:1") && debug.values["2:1"] == "4",
          "depuracion: nodos ejecutados y valores (Indice del For = 4)");
    check(t.scripts.visualScriptObjects("Graphs/Suma.crgraph").size() == 2, "visualScriptObjects por grafo");
    check(logCount(t, "multigate-0") == 1 && logCount(t, "multigate-1") == 1 && logCount(t, "multigate-2") == 1,
          "MultiGate: cada salida una vez, en orden (sin Repetir)");
    check(logCount(t, "retrig fin") == 1, "Retriggerable Delay: tres entradas seguidas terminan una sola vez");

    // Puntos de ruptura (los del editor).
    t.scripts.setVisualScriptBreakpoints("Graphs/Habla.crgraph", {2});
    t.scripts.callMethod(talker, "Hablar", std::string());
    std::string graph;
    int node = 0;
    entt::entity where = entt::null;
    check(t.scripts.takeVisualScriptBreak(graph, node, where) && graph == "Graphs/Habla.crgraph" && node == 2 &&
              where == talker.handle(),
          "punto de ruptura: grafo, nodo y objeto");
    check(t.logged("version 1"), "Custom Event desde la interfaz");

    // Recarga en caliente.
    writeGraph("Habla.crgraph", speaker("version 2"));
    t.scripts.reloadFile("Graphs/Habla.crgraph");
    t.scripts.callMethod(talker, "Hablar", std::string());
    check(t.logged("Visual Script recargado: Graphs/Habla.crgraph") && t.logged("version 2"),
          "recarga en caliente: el mismo objeto con el grafo nuevo");

    // Event Destroy al destruir el objeto (al final del frame).
    t.call("Entity:destroy", {}, ApiFixture::entity(talker));
    t.frame(0.125f);
    check(logCount(t, "adios") == 2, "Event Destroy al destruir el objeto");
    (void)delay;

    t.scripts.stop();
    std::filesystem::remove_all(kRoot);
    return finish();
}
