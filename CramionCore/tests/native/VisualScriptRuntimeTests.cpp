// Pruebas de VisualScriptRuntime.cpp (el interprete de los .crgraph): eventos
// Start / Update, Delay dentro de un Sequence (la cadena sigue donde iba),
// For Loop y variables, lo del Inspector, Custom Event y temporizadores,
// Flip Flop, Send Event entre objetos, errores, depuracion, recarga en
// caliente y Event Destroy.

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

}  // namespace

int main() {
    std::printf("VisualScriptRuntime\n");
    std::filesystem::remove_all(kRoot);
    vscript::registerVisualScriptComponents();
    libraryWithoutPlay();

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
    t.scripts.setVisualScriptDebugging(true);
    t.scripts.start(t.world);

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
    t.frame(0.125f);
    check(t.logged("despues del delay") && t.logged("then 1"), "Delay terminado: sigue la cadena y despues el Sequence");
    check(variable(t, flip, "a") == "3" && variable(t, flip, "b") == "2", "Flip Flop alterna A / B cada frame");
    for (int i = 0; i < 4; ++i) t.frame(0.125f);
    check(variable(t, timer, "cuenta") == "4", "Set Timer con Repetir: Ping cada 0.25 s (1 s -> 4)");
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
