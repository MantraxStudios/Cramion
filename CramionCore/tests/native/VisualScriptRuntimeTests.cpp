// Pruebas de VisualScriptRuntime.cpp (el interprete de los .crgraph): eventos
// Start / Update, Delay dentro de un Sequence (la cadena sigue donde iba),
// For Loop y variables, lo del Inspector, Custom Event y temporizadores,
// Flip Flop, Send Event entre objetos, errores, depuracion, recarga en
// caliente y Event Destroy.

#include "ApiTest.h"

#include "CramionCore/scripting/VisualScript.h"

#include <filesystem>
#include <fstream>
#include <string>

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

}  // namespace

int main() {
    std::printf("VisualScriptRuntime\n");
    std::filesystem::remove_all(kRoot);
    vscript::registerVisualScriptComponents();

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

    ApiFixture t(false);
    t.scripts.setAssetsRoot(kRoot);
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
