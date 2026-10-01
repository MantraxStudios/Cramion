// Pruebas de las maquinas de estados de IA (consola): pizarra, condiciones,
// prioridades, triggers, temporizadores, sm:go, el JSON del .crfsm y la
// ejecucion en Lua (OnEnter/OnUpdate/OnExit, Cualquier estado, expresiones,
// errores con estado y linea, recarga en caliente). Devuelve 0 si todo va.

#include "CramionCore/ai/StateMachine.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/scripting/Scripting.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
    const auto holds = [&](const ai::Condition& c) { return ai::conditionHolds(m, rt, c, 0, 0, {}); };
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

    // Expresiones Lua (aqui una funcion de prueba).
    ai::Condition lua;
    lua.kind = ai::ConditionKind::Lua;
    lua.expression = "vida < 50";
    m.transitions.push_back(ai::Transition{0, 1, 0, false, {lua}});
    rt.find("vida")->n = 100.0;
    m.transitions[2].conditions[0].value = "0";
    int asked = 0;
    ai::stepStateMachine(m, rt, 0.1f, [&](int t, int k, const std::string& e) {
        ++asked;
        return t == 4 && k == 0 && e == "vida < 50";
    });
    check(asked == 1 && rt.state == 1, "condicion Lua evaluada por quien tiene Lua");

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
              back.variables.size() == m.variables.size() && back.states[2].code == m.states[2].code &&
              back.any_code == m.any_code,
          "mismos estados, transiciones, variables y codigo");
    check(back.transitions[5].from == ai::kAnyState && back.transitions[5].priority == 10 &&
              back.transitions[3].conditions.size() == 2 &&
              back.transitions[3].conditions[1].kind == ai::ConditionKind::Timer &&
              back.transitions[6].conditions[0].kind == ai::ConditionKind::Lua,
          "Cualquier estado, prioridad, temporizador y expresion Lua");
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

void writeFile(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

ai::StateMachineAsset luaMachine(const char* broken_code) {
    ai::StateMachineAsset m;
    m.uuid = Uuid{0x1234ull, 0x5678ull};
    m.variables = {{"n", ai::Value::parse(ai::VarType::Int, "0")},
                   {"listo", ai::Value::parse(ai::VarType::Bool, "false")},
                   {"objetivo", ai::Value::parse(ai::VarType::Entity, "Jugador")},
                   {"dist", ai::Value::parse(ai::VarType::Float, "100")}};
    m.any_code = R"(
function OnEnter(self, sm) self.arranques = (self.arranques or 0) + 1 end
function OnUpdate(self, dt)
    local o = self.vars.objetivo
    self.vars.dist = o and self.entity:distanceTo(o) or 999
end
function ayuda(self) return "compartida" end
)";
    ai::State idle;
    idle.name = "Idle";
    idle.code = R"(
function OnEnter(self, sm) self.entradas = (self.entradas or 0) + 1; self.ayudo = self:ayuda() end
function OnUpdate(self, dt) self.vars.n = self.vars.n + 1 end
function OnExit(self) self.salidas = (self.salidas or 0) + 1 end
)";
    ai::State chase;
    chase.name = "Chase";
    chase.code = R"(
function OnEnter(self, sm) Scene.create("entro_chase"); self.sm_ok = (sm.state == "Chase") end
function OnUpdate(self, dt) if self.vars.n > 100 then self.sm:go("Idle") end end
function OnTriggerEnter(self, other) end
)";
    ai::State done;
    done.name = "Done";
    done.code = R"(
local S = {}
function S:OnEnter(sm) self.vars.listo = true end
return S
)";
    ai::State broken;
    broken.name = "Broken";
    broken.code = broken_code;
    m.states = {idle, chase, done, broken};
    ai::Condition expr;
    expr.kind = ai::ConditionKind::Lua;
    expr.expression = "n >= 1000 and entity ~= nil and sm.state ~= nil";
    m.transitions.push_back(ai::Transition{0, 1, 0, false, {variable("dist", ai::Compare::Less, "5")}});
    m.transitions.push_back(ai::Transition{1, 2, 0, false, {trigger("fin")}});
    m.transitions.push_back(ai::Transition{2, 0, 0, false, {timer(0.25f)}});
    m.transitions.push_back(ai::Transition{ai::kAnyState, 3, 1, false, {expr}});
    return m;
}

void testLua() {
    std::printf("Lua\n");
    std::filesystem::create_directories(kRoot / "IA");
    const std::filesystem::path file = kRoot / "IA" / "Prueba.crfsm";
    ai::StateMachineAsset m = luaMachine("function OnUpdate(self, dt)\n    local x = nil + 1\nend\n");
    check(ai::saveStateMachine(m, file), "asset en disco");

    scripting::registerScriptComponents();
    ecs::World world;
    ecs::Entity player = world.create("Jugador");
    player.setWorldPosition(core::Vec3{50.0f, 0.0f, 0.0f});
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
    system.start(world);
    system.update(world, 0.1f);
    const ai::Runtime& rt = enemy.get<ai::StateMachine>().runtime;
    check(rt.started && rt.state == 0, "empieza en Idle");
    check(rt.find("n")->n == 11.0, "OnUpdate cambia la pizarra (empezando en el valor del Inspector)");
    check(std::abs(rt.find("dist")->n - 50.0) < 1e-3, "Cualquier estado calcula sensores (objetivo por nombre)");
    std::string out;
    check(system.run("return Scene.find('Enemigo'):getStateMachine().state", &out) && out == "Idle",
          "entity:getStateMachine().state");
    check(system.run("local sm = Scene.find('Enemigo'):getStateMachine(); return sm:get('n') + 0", &out) && out == "11",
          "sm:get");

    player.setWorldPosition(core::Vec3{2.0f, 0.0f, 0.0f});
    system.update(world, 0.1f);
    check(rt.state == 1 && world.findByName("entro_chase").valid(), "transicion por variable: OnEnter del nuevo estado");
    check(system.run("return Scene.find('Enemigo'):getStateMachine():isIn('Chase')", &out) && out == "true", "sm:isIn");

    check(system.run("Scene.find('Enemigo'):getStateMachine():trigger('fin')", &out), "sm:trigger desde otro script");
    system.update(world, 0.1f);
    check(rt.state == 2 && rt.find("listo")->b, "trigger -> Done (estado escrito como tabla, self.vars)");
    for (int i = 0; i < 3; ++i) system.update(world, 0.1f);
    check(rt.state == 0 || rt.state == 1, "temporizador de 0.25 s: vuelve a Idle");
    check(system.run("local e = Scene.find('Enemigo'); return 1", &out), "consola");

    // self es el mismo en todos los estados y ve lo de Cualquier estado.
    system.run("local sm = StateMachine.of(Scene.find('Enemigo')); sm:set('n', 200)", &out);
    system.update(world, 0.1f);
    system.update(world, 0.1f);
    bool from_code = false;
    for (const std::string& h : rt.history) from_code = from_code || h.find("(codigo)") != std::string::npos;
    check(from_code, "sm:go desde OnUpdate (cambio por codigo)");

    // Expresion Lua -> Broken -> error con estado y linea.
    system.run("Scene.find('Enemigo'):getStateMachine():set('n', 1000)", &out);
    system.update(world, 0.1f);
    system.update(world, 0.1f);
    check(rt.state == 3, "condicion con expresion Lua (ve variables, entity y sm)");
    bool located = false;
    for (const scripting::ScriptError& e : system.errors()) {
        located = located || (e.file == "IA/Prueba.crfsm#Broken" && e.line == 2);
    }
    if (!located) {
        for (const scripting::ScriptError& e : system.errors()) {
            std::printf("    error: %s:%d %s\n", e.file.c_str(), e.line, e.message.c_str());
        }
    }
    check(located, "error de un estado: archivo#estado y linea");
    const std::size_t errors = system.errors().size();
    system.update(world, 0.1f);
    check(system.errors().size() == errors, "el estado con error no repite el error cada frame");

    // Recarga en caliente: sigue en su estado y con sus variables.
    m = luaMachine("function OnUpdate(self, dt) self.vars.listo = false end\n");
    m.variables.push_back({"nueva", ai::Value::parse(ai::VarType::Float, "7")});
    ai::saveStateMachine(m, file);
    system.clearErrors();
    system.reloadFile("IA/Prueba.crfsm");
    system.update(world, 0.1f);
    check(rt.state == 3 && system.errors().empty() && !rt.find("listo")->b && rt.find("n")->n >= 1000.0 &&
              rt.find("nueva") != nullptr,
          "recarga en caliente: mismo estado, codigo nuevo, variables conservadas y las nuevas");

    player.setWorldPosition(core::Vec3{50.0f, 0.0f, 0.0f});
    check(system.run("local e = Scene.find('Enemigo'); local sm = e:getStateMachine(); sm:restart(); return sm.running", &out) &&
              out == "true",
          "sm:restart");
    system.update(world, 0.1f);
    check(rt.state == 0 && rt.find("n")->n >= 10.0 && rt.find("n")->n < 20.0, "restart vuelve a la entrada con la pizarra inicial");
    check(system.run("return StateMachine.broadcast('fin')", &out) && out == "1", "StateMachine.broadcast");
    check(system.run("local e = Scene.find('Jugador'); return e:getStateMachine() == nil", &out) && out == "true",
          "getStateMachine sin componente -> nil");

    // Orden fijo entre varias maquinas y API para el autocompletado.
    const auto api = scripting::ScriptSystem::apiReference();
    bool has_go = false;
    if (const auto it = api.find("StateMachine:"); it != api.end()) {
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
    testLua();
    std::filesystem::remove_all(kRoot);
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
