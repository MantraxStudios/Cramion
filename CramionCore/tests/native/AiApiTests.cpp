// Pruebas de AiApi.cpp, parte de los Behavior Trees (la de las maquinas de
// estados esta en tests/StateMachineTests.cpp): Run Script como mensajes
// OnBtTask / OnBtAbort a los scripts de C++ del objeto, BehaviorTree.finishTask,
// la pizarra (get/set/has/clear/vars), activeTask, broadcast, stop/restart y
// el aviso cuando nadie hace la tarea.

#include "ApiTest.h"

#include "CramionCore/ai/BehaviorTree.h"
#include "CramionCore/scripting/CppScripts.h"

#include <filesystem>
#include <string>
#include <vector>

using namespace cramion;
using namespace cramion::apitest;

namespace {

const std::filesystem::path kRoot = std::filesystem::temp_directory_path() / "cramion_ai_api_test";

struct Message {
    std::string object;
    std::string method;
    json value;
};

// Raiz -> Sequence -> [Run Script "Atacar", Set Blackboard hecho = true].
ai::BehaviorTreeAsset guardTree() {
    ai::BehaviorTreeAsset tree;
    tree.uuid = Uuid{0xb7ull, 0x42ull};
    tree.ensureRoot();
    for (const char* name : {"hecho", "alerta"}) {
        ai::Variable v;
        v.name = name;
        v.value.type = ai::VarType::Bool;
        tree.blackboard.push_back(v);
    }
    ai::Variable vida;
    vida.name = "vida";
    vida.value.type = ai::VarType::Float;
    vida.value.n = 100.0;
    tree.blackboard.push_back(vida);
    const int sequence = tree.addNode(ai::BtNodeKind::Sequence, 0);
    const int task = tree.addNode(ai::BtNodeKind::RunScript, sequence);
    tree.nodes[static_cast<std::size_t>(task)].name = "Atacar";
    tree.nodes[static_cast<std::size_t>(task)].params.text = "Atacar";
    const int set = tree.addNode(ai::BtNodeKind::SetBlackboard, sequence);
    tree.nodes[static_cast<std::size_t>(set)].params.key = "hecho";
    tree.nodes[static_cast<std::size_t>(set)].params.value = "true";
    return tree;
}

ecs::Entity withTree(ecs::World& world, const char* name, const ai::BehaviorTreeAsset& tree, bool scripts) {
    ecs::Entity e = world.create(name);
    e.add<ai::BehaviorTree>().tree = assets::AssetRef{tree.uuid, assets::AssetType::BehaviorTree};
    if (scripts) e.add<scripting::CppScript>().class_name = "Guardia";
    return e;
}

}  // namespace

int main() {
    std::printf("AiApi (Behavior Trees)\n");
    std::filesystem::remove_all(kRoot);
    std::filesystem::create_directories(kRoot / "IA");
    const ai::BehaviorTreeAsset tree = guardTree();
    check(ai::saveBehaviorTree(tree, kRoot / "IA" / "Guardia.crbt"), "asset en disco");

    ApiFixture t(false);
    t.scripts.setAssetsRoot(kRoot);
    std::vector<Message> messages;
    t.scripts.setMessageListener([&](ecs::Entity e, const std::string& method, const std::string& value) {
        messages.push_back({e.name(), method, json::parse(value, nullptr, false)});
    });
    const auto count = [&](const char* object, const char* method) {
        int n = 0;
        for (const Message& m : messages) n += m.object == object && m.method == method ? 1 : 0;
        return n;
    };
    ecs::Entity guardia = withTree(t.world, "Guardia", tree, true);
    ecs::Entity solo = withTree(t.world, "Solo", tree, false);
    ecs::Entity nada = t.world.create("Nada");
    t.scripts.start(t.world);

    // Run Script: mensaje OnBtTask cada tick mientras corre.
    t.frame();
    check(count("Guardia", "OnBtTask") == 1 && messages.front().value.value("function", "") == "Atacar" &&
              messages.front().value.value("first", false),
          "Run Script manda OnBtTask {function, first = true}");
    const Value bt = t.call("BehaviorTree.of", {ApiFixture::entity(guardia)});
    check(!bt.isNil(), "BehaviorTree.of da el arbol del objeto");
    check(t.call("BehaviorTree.of", {ApiFixture::entity(nada)}).isNil(), "un objeto sin arbol da nil");
    check(t.get("BehaviorTree:running", bt).truthy(), "running");
    check(t.get("BehaviorTree:activeTask", bt).asString() == "Atacar", "activeTask es la tarea que corre");
    check(t.call("BehaviorTree:isActive", {Value("Atacar")}, bt).truthy(), "isActive de la tarea");
    t.frame();
    check(count("Guardia", "OnBtTask") == 2 && !messages.back().value.value("first", true), "sigue corriendo (first = false)");
    check(!t.call("BehaviorTree:get", {Value("hecho")}, bt).truthy(), "la secuencia espera a la tarea");

    // Sin scripts de C++ ni tarea registrada: aviso una vez.
    check(t.logged("nadie hace la tarea \"Atacar\""), "aviso cuando nadie hace la tarea");

    // finishTask(entity, true): la secuencia sigue y escribe la pizarra.
    t.call("BehaviorTree.finishTask", {ApiFixture::entity(guardia), Value(true)});
    t.frame();
    check(t.call("BehaviorTree:get", {Value("hecho")}, bt).truthy(), "finishTask(true) termina la tarea y la secuencia sigue");
    check(t.get("BehaviorTree:cycles", bt).asNumber() >= 1.0, "la raiz termino una vez (cycles)");

    // La pizarra.
    t.call("BehaviorTree:set", {Value("vida"), Value(25.0)}, bt);
    check(t.call("BehaviorTree:get", {Value("vida")}, bt).asNumber() == 25.0, "set/get de una clave");
    check(t.call("BehaviorTree:has", {Value("vida")}, bt).truthy() && !t.call("BehaviorTree:has", {Value("otra")}, bt).truthy(),
          "has");
    check(t.get("BehaviorTree:vars", bt)["vida"].asNumber() == 25.0, "vars (copia de la pizarra)");
    t.call("BehaviorTree:clear", {Value("vida")}, bt);
    check(t.call("BehaviorTree:get", {Value("vida")}, bt).asNumber() == 0.0, "clear");
    check(t.call("BehaviorTree.broadcast", {Value("alerta"), Value(true)}).asNumber() == 2.0 &&
              t.call("BehaviorTree:get", {Value("alerta")}, bt).truthy(),
          "broadcast cambia la clave en todos los arboles");
    const Value bt2 = t.call("Entity:getBehaviorTree", {}, ApiFixture::entity(solo));
    check(t.call("BehaviorTree:get", {Value("alerta")}, bt2).truthy(), "getBehaviorTree del otro objeto ve el broadcast");

    // stop con la tarea en marcha: OnBtAbort.
    t.frame();
    const int tasks = count("Guardia", "OnBtTask");
    check(tasks >= 3, "la tarea vuelve a empezar en el siguiente ciclo");
    t.call("BehaviorTree:stop", {}, bt);
    check(count("Guardia", "OnBtAbort") == 1 && !t.get("BehaviorTree:running", bt).truthy(), "stop corta la tarea (OnBtAbort) y lo para");
    t.frame();
    check(count("Guardia", "OnBtTask") == tasks, "parado no hace nada");
    t.call("BehaviorTree:restart", {}, bt);
    t.call("BehaviorTree:start", {}, bt);
    t.frame();
    check(count("Guardia", "OnBtTask") == tasks + 1 && !t.call("BehaviorTree:get", {Value("hecho")}, bt).truthy(),
          "restart + start: la pizarra inicial y la tarea otra vez");

    t.scripts.stop();
    std::filesystem::remove_all(kRoot);
    return finish();
}
