// Pruebas de native/Console.cpp: la consola (ScriptSystem::run) sin Lua.
// Usa funciones de prueba propias (Prueba.*, un handle Contador) y las de la
// API que haya (Scene, Time; Entity:name / position / translate si estan, y
// si no unas de prueba con el mismo nombre).

#include "ApiTest.h"

#include <cmath>
#include <map>

using namespace cramion;
using namespace cramion::apitest;
using core::Vec3;
using scripting::api::Call;

namespace {

struct Contador final : scripting::api::Handle {
    double valor = 0.0;
    std::string_view typeName() const override { return "Contador"; }
};

// Lo que usa la prueba y no es de la consola.
void registerTestApi(ApiFixture& t, double& valor, std::map<std::string, Value>& opciones) {
    auto& api = t.api();
    api.function("Prueba.sumar", [](Call& c) { return Value(c.number(0) + c.number(1, 0.0)); }, {"a, b", "Suma"});
    api.function("Prueba.eco", [](Call& c) { return c.arg(0); }, {"v", "Devuelve el argumento"});
    api.function("Prueba.cuantos", [](Call& c) { return Value(static_cast<double>(c.count())); }, {"...", "Argumentos"});
    api.property("Prueba", "valor", [&valor](Call&) { return Value(valor); },
                 [&valor](Call& c) {
                     valor = c.number(0);
                     return Value{};
                 });
    api.dynamicProperties("Prueba.opciones", [&opciones](Call& c) { return opciones[c.string(0)]; },
                          [&opciones](Call& c) {
                              opciones[c.string(0)] = c.arg(1);
                              return Value{};
                          });
    api.function("Prueba.contador", [](Call&) { return Value::handle(std::make_shared<Contador>()); });
    api.method("Contador", "sumar", [](Call& c) {
        auto h = c.self().as<Contador>();
        h->valor += c.number(0);
        return Value(h->valor);
    });
    api.property("Contador", "valor", [](Call& c) { return Value(c.self().as<Contador>()->valor); },
                 [](Call& c) {
                     c.self().as<Contador>()->valor = c.number(0);
                     return Value{};
                 },
                 {}, true);
    // Si el modulo de Entity aun no esta, unas iguales para la prueba.
    ecs::World* world = &t.world;
    if (api.find("Entity:name") == nullptr) {
        api.property("Entity", "name", [world](Call& c) { return Value(world->wrap(c.selfEntity()).name()); },
                     [world](Call& c) {
                         world->wrap(c.selfEntity()).setName(c.string(0));
                         return Value{};
                     },
                     {}, true);
    }
    if (api.find("Entity:position") == nullptr) {
        api.property("Entity", "position", [world](Call& c) { return Value(world->wrap(c.selfEntity()).worldPosition()); },
                     [world](Call& c) {
                         world->wrap(c.selfEntity()).setWorldPosition(c.vec3(0));
                         return Value{};
                     },
                     {}, true);
    }
    if (api.find("Entity:translate") == nullptr) {
        api.method("Entity", "translate", [world](Call& c) {
            ecs::Entity e = world->wrap(c.selfEntity());
            e.setWorldPosition(e.worldPosition() + c.vec3(0));
            return Value{};
        });
    }
}

}  // namespace

int main() {
    ApiFixture t;
    double valor = 0.0;
    std::map<std::string, Value> opciones;
    registerTestApi(t, valor, opciones);
    ecs::Entity player = t.world.create("Jugador");
    player.setWorldPosition(Vec3{0.0f, 1.0f, 0.0f});

    std::string out;
    // La salida si va bien ("<error> ..." si no).
    const auto eval = [&](const std::string& code) {
        std::string result;
        const bool ok = t.scripts.run(code, &result);
        return ok ? result : "<error> " + result;
    };
    // El mensaje si falla ("" si no falla).
    const auto error = [&](const std::string& code) {
        std::string result;
        return t.scripts.run(code, &result) ? std::string{} : result;
    };

    std::printf("Valores\n");
    check(eval("return 1.5") == "1.5" && eval("return -2") == "-2" && eval("return 1e3") == "1000" && eval("return .5") == "0.5",
          "numeros");
    check(eval("return 'ho\\'la'") == "ho'la" && eval("return \"a\\tb\"") == "a\tb" && eval("return 'Jugador \"1\"'") == "Jugador \"1\"",
          "textos con comillas y escapes");
    check(eval("return true") == "true" && eval("return false") == "false" && eval("return nil") == "nil", "true, false, nil");
    check(eval("return Vec3(1, -2, 3.5)") == "(1.000, -2.000, 3.500)" && eval("return Vec3()") == "(0.000, 0.000, 0.000)" &&
              eval("return -Vec3(1, 0, 0)") == "(-1.000, 0.000, 0.000)",
          "Vec3(x, y, z), Vec3() y -Vec3");
    check(eval("return Quat(0, 0, 0, 1)") == "(0.000, 0.000, 0.000, 1.000)", "Quat(x, y, z, w)");
    check(eval("return [1, 'a', true, nil]") == "[1, a, true, nil]" && eval("return []") == "[]", "listas [a, b]");
    check(eval("return {a = 1, b = 'x', c = [2]}") == "{a = 1, b = x, c = [2]}" && eval("return {1, 2}") == "[1, 2]" &&
              eval("return {}") == "{}",
          "objetos {k = v} (y {a, b} es una lista)");
    check(eval("return Vec3(1, 2, 3).y") == "2" && eval("return {p = Vec3(4, 5, 6)}.p.z") == "6" &&
              eval("return [10, 20, 30][2]") == "20" && eval("return [10][5]") == "nil" && eval("return {a = 3}['a']") == "3",
          "campos de Vec3 y objetos, elementos de listas (desde 1)");

    std::printf("Llamadas, propiedades y asignaciones\n");
    check(eval("Prueba.sumar(2, 3)") == "5", "una expresion suelta escribe su valor");
    check(eval("return Prueba.sumar(Prueba.sumar(1, 1), -4)") == "-2", "llamadas anidadas y numeros negativos");
    check(eval("return Prueba.cuantos()") == "0" && eval("return Prueba.cuantos(1, 'a', [], {})") == "4", "argumentos");
    check(eval("return Prueba.eco{a = 1}") == "{a = 1}" && eval("return Prueba.eco 'hola'") == "hola", "f{...} y f'texto'");
    t.log.clear();
    check(eval("print('hola', 3)") == "nil" && t.logged("hola 3"), "funciones globales (print)");
    check(eval("Prueba.valor = 7").empty() && valor == 7.0 && eval("Prueba.valor") == "7", "Tabla.prop = v y Tabla.prop");
    check(eval("Prueba.opciones.brillo = 0.5").empty() && opciones["brillo"].asNumber() == 0.5 &&
              eval("return Prueba.opciones.brillo") == "0.5",
          "propiedades con nombre libre de una subtabla (Prueba.opciones.brillo)");
    t.frame();
    check(eval("return Time.frameCount") == "1", "propiedades de la API (Time.frameCount)");

    std::printf("Objetos del motor\n");
    check(eval("Scene.find('Jugador'):translate(Vec3(0, 1, 0))") == "nil" && std::abs(player.worldPosition().y - 2.0f) < 1e-5f,
          "Scene.find('Jugador'):translate(Vec3(0, 1, 0))");
    check(eval("Scene.find('Jugador').name") == "Jugador", "Scene.find('Jugador').name");
    check(eval("Scene.find('Jugador').name = 'Heroe'").empty() && player.name() == "Heroe", "expr.prop = v en una entidad");
    check(eval("return Scene.find('Heroe').position.y") == "2", "propiedad y campo encadenados");
    check(eval("Scene.find('Heroe').translate(Vec3(1, 0, 0))") == "nil" && std::abs(player.worldPosition().x - 1.0f) < 1e-5f,
          "valor.metodo(...) tambien llama al metodo");
    check(eval("return Scene.find('Heroe')") == Value::entity(player.handle()).asString(), "una entidad como resultado");
    check(eval("local c = Prueba.contador(); c:sumar(5); c:sumar(2); return c.valor") == "7" &&
              eval("local c = Prueba.contador(); c.valor = 3; return c:sumar(1)") == "4",
          "metodos y propiedades de un handle");
    check(eval("return Scene.origin()[1]") == "0", "indexar el resultado de una llamada");

    std::printf("Variables y varias sentencias\n");
    check(eval("local a = 5; return Prueba.sumar(a, a)") == "10", "local");
    check(eval("local o = {a = 1}; o.b = 2; o['c'] = 3; return o") == "{a = 1, b = 2, c = 3}", "cambiar campos de un objeto");
    check(eval("j = Scene.find('Heroe')").empty() && eval("return j.name") == "Heroe", "las variables sin local siguen al siguiente comando");
    check(error("return a").find("no existe 'a'") != std::string::npos, "las locales no");
    check(eval("local j = 3; return j") == "3" && eval("return j.name") == "Heroe", "una local tapa a la de la consola solo en su comando");
    check(eval(R"(
        local a = Prueba.sumar(1, 2)  -- comentario
        Prueba.valor = a
        return Prueba.valor
    )") == "3",
          "varias lineas con comentarios");
    check(eval("return Prueba.sumar(\n  1,\n  2\n)") == "3", "una llamada en varias lineas");
    check(eval("Prueba.valor = 1; Prueba.sumar(1, 1)") == "2" && eval("Prueba.sumar(1, 1); Prueba.valor = 3").empty() &&
              eval("Prueba.valor = 4 Prueba.valor = 5").empty() && valor == 5.0,
          "el resultado es el de la ultima sentencia; ';' es opcional");
    check(eval("return").empty() && eval("").empty() && eval("  -- nada\n").empty(), "return sin valor y codigo vacio");
    check(t.scripts.run("Prueba.valor = 6", nullptr) && valor == 6.0, "sin salida (nullptr)");

    std::printf("Errores\n");
    std::string e = error("Prueba.sumar(1, 2");
    check(e.find("se esperaba ')'") != std::string::npos, "sintaxis: se esperaba ')'");
    valor = 0.0;
    check(!error("Prueba.valor = 99; Prueba.sumar(").empty() && valor == 0.0, "con un error de sintaxis no se ejecuta nada");
    check(!error("Prueba.valor = 5; Nada.hacer(); Prueba.valor = 6").empty() && valor == 5.0,
          "un error al ejecutar para ahi (lo de antes ya se hizo)");
    e = error("Nada.hacer()");
    check(e.find("no existe Nada.hacer") != std::string::npos, "funcion que no existe");
    e = error("Prueba.sumar('a')");
    check(e.rfind("Prueba.sumar: argumento 1: se esperaba un numero", 0) == 0, "error de la API con su nombre delante");
    e = error("Scene.find('Nadie'):translate(Vec3(0, 1, 0))");
    check(e.find(":translate") != std::string::npos && e.find("nil") != std::string::npos, "metodo sobre nil");
    check(error("Scene.find('Nadie').name").find("nil") != std::string::npos, "propiedad de nil");
    check(error("Time.time = 3").find("Time.time es de solo lectura") != std::string::npos, "propiedad de solo lectura");
    check(error("return 1 + 2").find("operadores") != std::string::npos && error("return 'a' .. 'b'").find("operadores") != std::string::npos,
          "sin operadores");
    check(error("for i = 1, 3 do print(i) end").find("no tiene 'for'") != std::string::npos, "sin bucles de Lua");
    check(error("print('hola)").find("texto sin cerrar") != std::string::npos, "texto sin cerrar");
    check(error("return 1; print(2)").find("'return' tiene que ser lo ultimo") != std::string::npos, "return al final");
    check(error("return Prueba.sumar").find("es una funcion") != std::string::npos, "una funcion sin ()");
    check(error("return Scene").find("es una tabla") != std::string::npos, "una tabla sola");
    check(!error("Vec3(1, 'a', 3)").empty() && !error("Quat(1, 2)").empty(), "Vec3 y Quat con argumentos malos");
    check(!error("return {1, a = 2}").empty(), "un {} no mezcla lista y objeto");
    check(error("Prueba.sumar(1, 2) = 3").find("no se puede asignar") != std::string::npos, "no se asigna a una llamada");
    check(error("return 12abc").find("numero mal escrito") != std::string::npos, "numero mal escrito");
    check(error("return Vec3(1, 2, 3).w").find("x, y, z") != std::string::npos, "campo que un Vec3 no tiene");
    e = error("\nPrueba.sumar(1, 1)\nNada.x()\n");
    check(e.rfind("linea 3: ", 0) == 0, "con varias lineas dice en cual");
    check(error("Nada.x()").rfind("linea", 0) != 0, "con una sola, no");

    std::printf("Fuera del Play\n");
    t.scripts.stop();
    check(error("return j").find("no existe 'j'") != std::string::npos, "al parar se olvidan las variables de la consola");
    ecs::World editor;
    const ecs::Entity in_editor = editor.create("Editor");
    check(t.scripts.run("return Scene.find('Editor')", &out, &editor) && out == Value::entity(in_editor.handle()).asString(),
          "sobre la escena que se pase (la del editor)");
    check(t.call("Scene.find", {Value("Editor")}).isNil(), "y despues vuelve la de antes");
    return finish();
}
