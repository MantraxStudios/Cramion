// Pruebas de las maquinas de estados de IA (consola): pizarra, condiciones,
// prioridades, triggers, temporizadores, sm:go, el JSON del .crfsm y la
// ejecucion en Play (mensajes OnStateEnter/OnStateUpdate/OnStateExit para los
// scripts de C++, expresiones, la API StateMachine y la recarga en caliente).
// Devuelve 0 si todo va.

#include "CramionCore/ai/StateMachine.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/scripting/NativeApi.h"
#include "CramionCore/scripting/Scripting.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace cramion;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

const std::filesystem::path kRoot = std::filesystem::temp_directory_path() / "cramion_fsm_assets";

ai::Condition variable(const char* name, ai::Compare compare, const char* value) {
    ai::Condition c;
    c.kind = ai::ConditionKind::Variable;
    c.variable = name;
    c.compare = compare;
    c.value = value;
    return c;
}

ai::Condition trigger(const char* name) {
    ai::Condition c;
    c.kind = ai::ConditionKind::Trigger;
    c.variable = name;
    return c;
}

ai::Condition timer(float seconds) {
    ai::Condition c;
    c.kind = ai::ConditionKind::Timer;
    c.seconds = seconds;
    return c;
}

ai::StateMachineAsset threeStates() {
    ai::StateMachineAsset m;
    m.variables = {{"distancia", ai::Value::parse(ai::VarType::Float, "50")},
                   {"vision", ai::Value::parse(ai::VarType::Float, "10")},
                   {"alerta", ai::Value::parse(ai::VarType::Bool, "false")},
                   {"vida", ai::Value::parse(ai::VarType::Int, "100")},
                   {"modo", ai::Value::parse(ai::VarType::String, "normal")},
                   {"punto", ai::Value::parse(ai::VarType::Vector, "1 2 3")}};
    for (const char* name : {"Patrullar", "Perseguir", "Huir"}) {
        ai::State s;
        s.name = name;
        m.states.push_back(s);
    }
    return m;
}

void testValues() {
    std::printf("Pizarra\n");
    check(ai::Value::parse(ai::VarType::Bool, "true").b && !ai::Value::parse(ai::VarType::Bool, "0").b, "bool desde texto");
    check(ai::Value::parse(ai::VarType::Int, "2.6").n == 3.0, "int redondea");
    check(std::abs(ai::Value::parse(ai::VarType::Float, "2.5").n - 2.5) < 1e-9, "float");
    const ai::Value v = ai::Value::parse(ai::VarType::Vector, "1, 2, 3");
    check(v.v.x == 1.0f && v.v.y == 2.0f && v.v.z == 3.0f && v.text() == "1 2 3", "vec3 con comas y de vuelta a texto");
    check(ai::varTypeFromKey("vec3") == ai::VarType::Vector && ai::varTypeFromKey("entity") == ai::VarType::Entity,
          "tipos por nombre");

    ai::StateMachineAsset m = threeStates();
    ai::Runtime rt;
    ai::resetRuntime(m, rt, {{"vision", 2, "20"}, {"vida", 1, "40"}, {"no_existe", 2, "1"}});
    check(rt.find("vision")->n == 20.0 && rt.find("vida")->n == 40.0 && rt.find("no_existe") == nullptr,
          "valores del Inspector por objeto (los que no existen se ignoran)");
    const auto holds = [&](const ai::Condition& c) { return ai::conditionHolds(m, rt, c); };
    check(holds(variable("distancia", ai::Compare::Greater, "$vision")), "comparar con otra variable ($vision)");
    check(holds(variable("distancia", ai::Compare::Equal, "50")) && !holds(variable("distancia", ai::Compare::NotEqual, "50")),
          "igual / distinto");
    check(holds(variable("vida", ai::Compare::LessEqual, "40")) && !holds(variable("vida", ai::Compare::Less, "40")),
          "menor o igual / menor");
    check(holds(variable("alerta", ai::Compare::IsFalse, "")) && !holds(variable("alerta", ai::Compare::IsTrue, "")),
          "bool verdadero / falso");
    check(holds(variable("modo", ai::Compare::Equal, "normal")) && holds(variable("modo", ai::Compare::NotEqual, "furia")),
          "texto");
    check(holds(variable("punto", ai::Compare::Equal, "1 2 3")) && holds(variable("punto", ai::Compare::Greater, "3")),
          "vec3: igual y longitud");
    check(!holds(variable("nada", ai::Compare::Equal, "1")), "variable que no existe: no se cumple");
}

void testTransitions() {
    std::printf("Transiciones\n");
    ai::StateMachineAsset m = threeStates();
    m.transitions.push_back(ai::Transition{0, 1, 0, false, {variable("distancia", ai::Compare::Less, "$vision")}});
    m.transitions.push_back(ai::Transition{1, 0, 0, false, {variable("distancia", ai::Compare::Greater, "15"), timer(1.0f)}});
    m.transitions.push_back(ai::Transition{ai::kAnyState, 2, 5, false, {variable("vida", ai::Compare::Less, "30")}});
    m.transitions.push_back(ai::Transition{0, 2, 0, false, {trigger("ruido")}});
    check(ai::validateStateMachine(m).empty(), "la maquina es valida");

    ai::Runtime rt;
    ai::resetRuntime(m, rt);
    check(!ai::stepStateMachine(m, rt, 0.1f).changed, "sin empezar no hace nada");
    ai::StepResult r = ai::startStateMachine(m, rt);
    check(r.changed && rt.state == 0 && rt.last_transition == -1, "empieza en la entrada");
    r = ai::stepStateMachine(m, rt, 0.1f);
    check(!r.changed && std::abs(rt.state_time - 0.1f) < 1e-6f, "sin condiciones cumplidas sigue y cuenta el tiempo");

    rt.find("distancia")->n = 5.0;
    r = ai::stepStateMachine(m, rt, 0.1f);
    check(r.changed && r.from == 0 && r.to == 1 && r.transition == 0 && rt.state_time == 0.0f, "Patrullar -> Perseguir");

    rt.find("distancia")->n = 30.0;
    ai::stepStateMachine(m, rt, 0.5f);
    check(rt.state == 1, "temporizador: aun no (0.5 s de 1 s)");
    ai::stepStateMachine(m, rt, 0.6f);
    check(rt.state == 0, "temporizador: despues de 1 s vuelve (todas las condiciones)");

    rt.triggers.push_back("ruido");
    rt.find("distancia")->n = 5.0;  // tambien se cumple la 0, que va antes en la lista
    ai::stepStateMachine(m, rt, 0.1f);
    check(rt.state == 1 && rt.triggers.empty(), "a igual prioridad gana el orden; los triggers se consumen");

    ai::stepStateMachine(m, rt, 0.1f);  // vuelve a Patrullar? no: distancia 5 < 15
    rt.state = 0;
    rt.triggers.push_back("ruido");
    rt.find("distancia")->n = 50.0;
    ai::stepStateMachine(m, rt, 0.1f);
    check(rt.state == 2, "trigger -> Huir");

    rt.find("vida")->n = 10.0;
    rt.state = 1;
    ai::stepStateMachine(m, rt, 0.1f);
    check(rt.state == 2 && rt.last_transition == 2, "Cualquier estado con mas prioridad");
    ai::stepStateMachine(m, rt, 0.1f);
    check(rt.state == 2 && rt.state_time > 0.0f, "Cualquier estado no se repite sobre si mismo (sin allow_self)");
    m.transitions[2].allow_self = true;
    ai::stepStateMachine(m, rt, 0.1f);
    check(rt.state_time == 0.0f, "con allow_self vuelve a entrar");

    rt.requested = 0;
    r = ai::stepStateMachine(m, rt, 0.1f);
    check(r.changed && r.transition == -2 && rt.state == 0 && rt.previous == 2, "sm:go manda sobre las transiciones");
    check(!rt.history.empty() && rt.history.back().find("(codigo)") != std::string::npos, "historial para el editor");

    // Expresiones (ai/Expression.h) con la pizarra.
    ai::Condition expr;
    expr.kind = ai::ConditionKind::Expression;
    expr.expression = "vida < 50 and not alerta";
    m.transitions.push_back(ai::Transition{0, 1, 0, false, {expr}});
    rt.find("vida")->n = 100.0;
    m.transitions[2].conditions[0].value = "0";
    ai::stepStateMachine(m, rt, 0.1f);
    check(rt.state == 0, "expresion falsa: sigue");
    rt.find("vida")->n = 40.0;
    ai::stepStateMachine(m, rt, 0.1f);
    check(rt.state == 1 && rt.last_transition == 4, "expresion verdadera (vida < 50 and not alerta)");
    ai::Condition broken;
    broken.kind = ai::ConditionKind::Expression;
    broken.expression = "self.entity:distanceTo(x) < 3";
    check(!ai::conditionHolds(m, rt, broken), "expresion que no se puede leer: no se cumple");

    rt.running = false;
    check(!ai::stepStateMachine(m, rt, 0.1f).changed, "parada (sm:stop) no avanza");

    // Quitar un estado arregla los indices.
    m.removeState(0);
    check(m.states.size() == 2 && m.states[0].name == "Perseguir", "quitar un estado");
    bool fine = true;
    for (const ai::Transition& t : m.transitions) fine = fine && t.from >= ai::kAnyState && t.from < 2 && t.to >= 0 && t.to < 2;
    check(fine, "las transiciones siguen apuntando a estados validos");
}

void testJson() {
    std::printf("Archivo .crfsm\n");
    ai::StateMachineAsset m = ai::exampleEnemyStateMachine();
    m.uuid = Uuid::generate();
    check(ai::validateStateMachine(m).empty(), "el ejemplo (Patrullar, Perseguir, Atacar, Huir, Volver) es valido");
    const std::string text = ai::stateMachineToJson(m);
    ai::StateMachineAsset back;
    check(ai::stateMachineFromJson(text, back), "se vuelve a leer");
    check(back.uuid == m.uuid && back.states.size() == m.states.size() && back.transitions.size() == m.transitions.size() &&
              back.variables.size() == m.variables.size() && back.states[2].send_update == m.states[2].send_update,
          "mismos estados, transiciones, variables y OnStateUpdate");
    check(back.transitions[5].from == ai::kAnyState && back.transitions[5].priority == 10 &&
              back.transitions[3].conditions.size() == 2 &&
              back.transitions[3].conditions[1].kind == ai::ConditionKind::Timer &&
              back.transitions[6].conditions[0].kind == ai::ConditionKind::Expression &&
              back.transitions[6].conditions[0].expression == "vida >= 60",
          "Cualquier estado, prioridad, temporizador y expresion");
    check(back.findVariable("casa")->value.type == ai::VarType::Vector &&
              back.findVariable("objetivo")->value.type == ai::VarType::Entity &&
              back.findVariable("objetivo")->value.s == "Jugador",
          "tipos de las variables");

    // Escrito a mano (o por una IA): nombres de estados, variables como objeto.
    const char* hand = R"({
        "variables": {"listo": false, "n": 3, "nombre": "x", "p": [1, 2, 3]},
        "states": [{"name": "A", "code": "function OnUpdate(self, dt) end"}, {"name": "B"}],
        "entry": "B",
        "transitions": [{"from": "A", "to": "B", "conditions": [{"type": "variable", "variable": "n", "compare": ">=", "value": 5}]},
                        {"from": "any", "to": "A", "conditions": [{"type": "trigger", "trigger": "reset"}]},
                        {"from": "Z", "to": "A"}]
    })";
    ai::StateMachineAsset h;
    std::string error;
    check(ai::stateMachineFromJson(hand, h, &error) && h.entry_state == 1 && h.transitions.size() == 2 &&
              h.transitions[1].from == ai::kAnyState && h.transitions[0].conditions[0].value == "5" &&
              h.findVariable("listo")->value.type == ai::VarType::Bool &&
              h.findVariable("p")->value.type == ai::VarType::Vector && !error.empty(),
          "JSON a mano: estados por nombre, 'any', variables con su tipo y aviso de la transicion rota");
    h.transitions[0].conditions[0].variable = "falta";
    check(!ai::validateStateMachine(h).empty(), "validar avisa de una variable que no existe");
    h.renameVariable("n", "m");
    check(h.findVariable("m") != nullptr, "renombrar una variable");

    std::filesystem::create_directories(kRoot);
    const std::filesystem::path file = kRoot / "Ejemplo.crfsm";
    check(ai::saveStateMachine(m, file) && ai::loadStateMachine(file, back) && back.states.size() == 5, "guardar y cargar");
}

struct Message {
    std::string entity;
    std::string method;
    nlohmann::json value;
};

ai::StateMachineAsset playMachine() {
    ai::StateMachineAsset m;
    m.uuid = Uuid{0x1234ull, 0x5678ull};
    m.variables = {{"n", ai::Value::parse(ai::VarType::Int, "0")},
                   {"listo", ai::Value::parse(ai::VarType::Bool, "false")},
                   {"objetivo", ai::Value::parse(ai::VarType::Entity, "Jugador")},
                   {"dist", ai::Value::parse(ai::VarType::Float, "100")}};
    ai::State idle;
    idle.name = "Idle";
    idle.send_update = true;
    ai::State chase;
    chase.name = "Chase";
    ai::State done;
    done.name = "Done";
    ai::State alarm;
    alarm.name = "Alarma";
    m.states = {idle, chase, done, alarm};
    ai::Condition expr;
    expr.kind = ai::ConditionKind::Expression;
    expr.expression = "n >= 1000 and objetivo ~= nil";
    m.transitions.push_back(ai::Transition{0, 1, 0, false, {variable("dist", ai::Compare::Less, "5")}});
    m.transitions.push_back(ai::Transition{1, 2, 0, false, {trigger("fin")}});
    m.transitions.push_back(ai::Transition{2, 0, 0, false, {timer(0.25f)}});
    m.transitions.push_back(ai::Transition{ai::kAnyState, 3, 1, false, {expr}});
    return m;
}

void testRuntime() {
    std::printf("Play (mensajes y API)\n");
    std::filesystem::create_directories(kRoot / "IA");
    const std::filesystem::path file = kRoot / "IA" / "Prueba.crfsm";
    ai::StateMachineAsset m = playMachine();
    check(ai::saveStateMachine(m, file), "asset en disco");

    scripting::registerScriptComponents();
    ecs::World world;
    ecs::Entity player = world.create("Jugador");
    ecs::Entity enemy = world.create("Enemigo");
    ai::StateMachine& sm = enemy.add<ai::StateMachine>();
    sm.machine = assets::AssetRef{m.uuid, assets::AssetType::StateMachine};
    sm.variables.push_back(ai::VariableOverride{"n", 1, "10"});

    // El componente se guarda en la escena (con lo del Inspector).
    const std::string json = ecs::componentToJson(world, enemy, "StateMachine");
    check(json.find("machine") != std::string::npos && json.find("\"n\"") != std::string::npos,
          "el componente se serializa con sus variables");

    scripting::ScriptSystem system;
    system.setAssetsRoot(kRoot);
    std::vector<std::string> log;
    system.setLog([&](int, const std::string& message) { log.push_back(message); });

    // El "script de C++" del enemigo: recibe los mensajes y usa la API como
    // lo haria Script::onMessage (en Idle suma 1 a n cada frame).
    scripting::api::NativeApi& api = system.nativeApi();
    std::vector<Message> messages;
    system.setMessageListener([&](ecs::Entity e, const std::string& method, const std::string& value) {
        messages.push_back({e.name(), method, nlohmann::json::parse(value, nullptr, false)});
        if (method == "OnStateUpdate") {
            const scripting::api::Value self = api.call("StateMachine.of", {scripting::api::Value::entity(e.handle())});
            const double n = api.call("StateMachine:get", {scripting::api::Value("n")}, self).asNumber();
            api.call("StateMachine:set", {scripting::api::Value("n"), scripting::api::Value(n + 1.0)}, self);
        }
    });
    const auto count = [&](const char* method, const char* state) {
        int c = 0;
        for (const Message& msg : messages) {
            if (msg.method == method && msg.value.value("state", "") == state) ++c;
        }
        return c;
    };

    system.start(world);
    system.update(world, 0.1f);
    const ai::Runtime& rt = enemy.get<ai::StateMachine>().runtime;
    check(rt.started && rt.state == 0, "empieza en Idle");
    check(count("OnStateEnter", "Idle") == 1 && messages.front().value["machine"] == "Prueba" &&
              messages.front().value["from"].is_null(),
          "OnStateEnter {machine, state, from = nil} al entrar");
    check(count("OnStateUpdate", "Idle") == 1 && rt.find("n")->n == 11.0,
          "OnStateUpdate: el script cambia la pizarra (empezando en el valor del Inspector)");
    check(rt.find("objetivo")->entity != ai::kNoEntity, "variable entity resuelta por nombre");

    const scripting::api::Value handle = api.call("Entity:getStateMachine", {}, scripting::api::Value::entity(enemy.handle()));
    check(api.get("StateMachine:state", handle).asString() == "Idle", "entity:getStateMachine().state");
    check(api.call("StateMachine:get", {scripting::api::Value("n")}, handle).asNumber() == 11.0, "sm:get");
    std::string out;
    check(system.run("return Scene.find('Enemigo'):getStateMachine().state", &out) && out == "Idle",
          "consola: Scene.find(...):getStateMachine().state");

    api.call("StateMachine:set", {scripting::api::Value("dist"), scripting::api::Value(2.0)}, handle);
    system.update(world, 0.1f);
    check(rt.state == 1 && count("OnStateExit", "Idle") == 1 && count("OnStateEnter", "Chase") == 1,
          "transicion por variable: OnStateExit del viejo y OnStateEnter del nuevo");
    check(count("OnStateUpdate", "Chase") == 0, "sin send_update no hay OnStateUpdate");
    check(api.call("StateMachine:isIn", {scripting::api::Value("Chase")}, handle).truthy(), "sm:isIn");

    api.call("StateMachine:trigger", {scripting::api::Value("fin")}, handle);
    system.update(world, 0.1f);
    check(rt.state == 2, "sm:trigger -> Done");
    for (int i = 0; i < 3; ++i) system.update(world, 0.1f);
    check(rt.state == 0 || rt.state == 1, "temporizador de 0.25 s: vuelve a Idle");

    // sm:go desde un mensaje: se aplica en el mismo frame.
    api.call("StateMachine:set", {scripting::api::Value("dist"), scripting::api::Value(100.0)}, handle);
    api.call("StateMachine:go", {scripting::api::Value("Done")}, handle);
    system.update(world, 0.1f);
    bool from_code = false;
    for (const std::string& h : rt.history) from_code = from_code || h.find("(codigo)") != std::string::npos;
    check(rt.state == 2 && from_code, "sm:go (cambio por codigo)");
    check(!api.call("StateMachine:go", {scripting::api::Value("NoExiste")}, handle).truthy(), "sm:go a un estado que no existe");

    // Expresion -> Alarma (Cualquier estado).
    api.call("StateMachine:set", {scripting::api::Value("n"), scripting::api::Value(1000.0)}, handle);
    system.update(world, 0.1f);
    check(rt.state == 3, "condicion con expresion (ve las variables)");

    // Recarga en caliente: sigue en su estado y con sus variables.
    m.variables.push_back({"nueva", ai::Value::parse(ai::VarType::Float, "7")});
    ai::saveStateMachine(m, file);
    system.reloadFile("IA/Prueba.crfsm");
    system.update(world, 0.1f);
    check(rt.state == 3 && rt.find("n")->n >= 1000.0 && rt.find("nueva") != nullptr,
          "recarga en caliente: mismo estado, variables conservadas y las nuevas");

    api.call("StateMachine:restart", {}, handle);
    check(api.get("StateMachine:running", handle).truthy(), "sm:restart la pone en marcha");
    system.update(world, 0.1f);
    check(rt.state == 0 && rt.find("n")->n >= 10.0 && rt.find("n")->n < 20.0,
          "restart vuelve a la entrada con la pizarra inicial");
    check(api.call("StateMachine.broadcast", {scripting::api::Value("fin")}).asNumber() == 1.0, "StateMachine.broadcast");
    check(api.call("Entity:getStateMachine", {}, scripting::api::Value::entity(player.handle())).isNil(),
          "getStateMachine sin componente -> nil");
    api.call("StateMachine:stop", {}, handle);
    const std::size_t before = messages.size();
    system.update(world, 0.1f);
    check(messages.size() == before, "sm:stop: ni transiciones ni OnStateUpdate");

    const auto reference = scripting::ScriptSystem::apiReference();
    bool has_go = false;
    if (const auto it = reference.find("StateMachine:"); it != reference.end()) {
        for (const auto& member : it->second) has_go = has_go || member.name == "go";
    }
    check(has_go, "apiReference tiene los metodos de StateMachine");
    system.stop();
}

}  // namespace

int main() {
    testValues();
    testTransitions();
    testJson();
    testRuntime();
    std::filesystem::remove_all(kRoot);
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
