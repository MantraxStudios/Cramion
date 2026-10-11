// Pruebas de la API de scripting en C++ (NativeApi.h), sin Lua (consola).
// Devuelve 0 si todo va.

#include "CramionCore/scripting/NativeApi.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace cramion;
using namespace cramion::scripting;
using json = nlohmann::json;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

struct Counter final : api::Handle {
    int value = 0;
    std::string_view typeName() const override { return "Counter"; }
};

json call(api::NativeApi& a, const json& request) {
    return json::parse(a.bridgeCall(request));
}

void testValues() {
    std::printf("Value\n");
    api::NativeApi a;
    const api::Value v = a.fromJson(json::parse(R"({"n":2.5,"t":"hola","b":true,"l":[1,2,3],
        "v":{"$v":[1,2,3]},"q":{"$q":[0,0,0,1]},"e":{"$e":5},"f":{"$f":7}})"));
    check(v.type() == api::Value::Type::Object, "un objeto");
    check(v["n"].asNumber() == 2.5 && v["t"].asString() == "hola" && v["b"].truthy(), "numero, texto y bool");
    check(v["l"].size() == 3 && v["l"][2].asNumber() == 3.0, "lista");
    check(v["v"].type() == api::Value::Type::Vec3 && v["v"].asVec3().z == 3.0f, "Vec3");
    check(v["q"].type() == api::Value::Type::Quat && v["q"].asQuat().w == 1.0f, "Quat");
    check(v["e"].type() == api::Value::Type::Entity && entt::to_integral(v["e"].asEntity()) == 4, "entidad (id - 1)");
    check(v["f"].callbackId() == 7, "funcion");
    check(a.toJson(v["e"]) == json{{"$e", 5}}, "la entidad vuelve con el mismo id");
    check(a.toJson(v["v"]) == json{{"$v", {1.0f, 2.0f, 3.0f}}}, "el Vec3 vuelve igual");
    check(api::Value(3.0).asString() == "3" && api::Value(0.5).asString() == "0.5", "numeros como texto");
    check(!api::Value().truthy() && !api::Value(false).truthy() && api::Value(0).truthy(), "verdad como en Lua");
}

void testRegistry() {
    std::printf("Registro y puente\n");
    api::NativeApi a;
    double volume = 1.0;
    std::string last;
    a.function("Audio.add", [](api::Call& c) { return api::Value(c.number(0) + c.number(1, 10.0)); }, {"a, b", "Suma"});
    a.function("Debug.log", [&last](api::Call& c) { last = c.string(0); return api::Value{}; });
    a.property("Audio", "volume", [&volume](api::Call&) { return api::Value(volume); },
               [&volume](api::Call& c) { volume = c.number(0); return api::Value{}; });
    a.property("Time", "time", [](api::Call&) { return api::Value(4.0); });
    a.method("Entity", "id", [](api::Call& c) { return api::Value(static_cast<double>(entt::to_integral(c.selfEntity()))); });
    a.function("Counter.new", [](api::Call&) { return api::Value::handle(std::make_shared<Counter>()); });
    a.method("Counter", "add", [](api::Call& c) {
        auto h = c.self().as<Counter>();
        h->value += static_cast<int>(c.number(0));
        return api::Value(h->value);
    });
    a.property("Counter", "value", [](api::Call& c) { return api::Value(c.self().as<Counter>()->value); }, {}, {}, true);

    check(a.find("Audio.add") != nullptr && a.find("Audio.add")->doc.args == "a, b", "se encuentra con su documentacion");
    check(a.find("Entity:id") != nullptr && a.find("Entity.id") == nullptr, "los metodos van con ':'");

    json r = call(a, {{"op", "call"}, {"fn", "Audio.add"}, {"args", {2, 3}}});
    check(r["ok"] == true && r["result"] == 5.0, "llamar a una funcion");
    r = call(a, {{"fn", "Audio.add"}, {"args", {2}}});
    check(r["result"] == 12.0, "argumento opcional");
    r = call(a, {{"fn", "Audio.add"}, {"args", {"x"}}});
    check(r["ok"] == false && r["error"].get<std::string>().find("argumento 1") != std::string::npos, "un tipo equivocado es un error");
    call(a, {{"fn", "Debug.log"}, {"args", {"hola"}}});
    check(last == "hola", "la funcion se ejecuta");

    r = call(a, {{"op", "get"}, {"fn", "Audio.volume"}});
    check(r["result"] == 1.0, "leer una propiedad");
    r = call(a, {{"op", "set"}, {"fn", "Audio.volume"}, {"value", 0.25}});
    check(r["ok"] == true && volume == 0.25, "cambiar una propiedad");
    r = call(a, {{"op", "set"}, {"fn", "Time.time"}, {"value", 1}});
    check(r["ok"] == false, "una propiedad de solo lectura no cambia");

    r = call(a, {{"fn", "id"}, {"self", {{"$e", 8}}}});
    check(r["result"] == 7.0, "metodo de una entidad");

    r = call(a, {{"fn", "Counter.new"}});
    const json handle = r["result"];
    check(handle.contains("$h") && handle["$h"].get<std::uint64_t>() >= api::NativeApi::kHandleBase, "un handle nativo");
    call(a, {{"fn", "add"}, {"self", handle}, {"args", {2}}});
    r = call(a, {{"fn", "add"}, {"self", handle}, {"args", {3}}});
    check(r["result"] == 5.0, "metodos de un handle (mismo objeto)");
    r = call(a, {{"op", "get"}, {"key", "value"}, {"self", handle}});
    check(r["result"] == 5.0, "propiedad de un handle");
    a.clearHandles();
    r = call(a, {{"fn", "add"}, {"self", handle}, {"args", {1}}});
    check(r["ok"] == false, "un handle soltado ya no existe");

    r = call(a, {{"fn", "Audio.playOneShot"}});
    check(r["ok"] == false && r["error"].get<std::string>().find("no existe") != std::string::npos,
          "lo que no existe es un error claro");
    r = call(a, {{"fn", "x"}, {"self", {{"$h", 3}}}});
    check(r["ok"] == false, "un handle desconocido es un error");

    // Llamadas directas (consola, Visual Scripts).
    check(a.call("Audio.add", {api::Value(1), api::Value(2)}).asNumber() == 3.0, "call directo");
    a.set("Audio.volume", api::Value(0.75));
    check(a.get("Audio.volume").asNumber() == 0.75, "get y set directos");
    bool threw = false;
    try {
        a.call("Nada.de.nada");
    } catch (const api::Error&) {
        threw = true;
    }
    check(threw, "call directo de algo que no existe lanza api::Error");

    // Propiedades con nombre libre (Graphics.vsync...).
    std::map<std::string, double> options{{"vsync", 1.0}};
    a.dynamicProperties(
        "Graphics", [&options](api::Call& c) { return api::Value(options[c.string(0)]); },
        [&options](api::Call& c) {
            options[c.string(0)] = c.number(1);
            return api::Value{};
        });
    r = call(a, {{"op", "set"}, {"fn", "Graphics.sombras"}, {"value", 3}});
    check(r["ok"] == true && options["sombras"] == 3.0, "escribir una propiedad dinamica");
    r = call(a, {{"op", "get"}, {"fn", "Graphics.vsync"}});
    check(r["result"] == 1.0, "leer una propiedad dinamica");

    // Handles internados: el mismo objeto, el mismo id.
    int engine_object = 0;
    auto h1 = a.intern<Counter>(&engine_object, [] { return std::make_shared<Counter>(); });
    auto h2 = a.intern<Counter>(&engine_object, [] { return std::make_shared<Counter>(); });
    check(h1 == h2 && a.handleId(h1) == a.handleId(h2), "el mismo handle para el mismo objeto");
}

void testCallbacks() {
    std::printf("Callbacks\n");
    api::NativeApi a;
    std::uint64_t sent_id = 0;
    std::string sent_args;
    a.setCallbackSink([&](std::uint64_t id, const std::string& args) {
        sent_id = id;
        sent_args = args;
    });
    std::uint64_t local_id = 0;
    double local_value = 0;
    a.setLocalInvoker([&](std::uint64_t id, const api::Value::Array& args) {
        local_id = id;
        local_value = args.empty() ? 0 : args[0].asNumber();
    });
    a.function("UI.onClick", [&a](api::Call& c) {
        a.invoke(c.function(0), {api::Value("boton"), api::Value(core::Vec3{1, 2, 3})});
        return api::Value{};
    });
    call(a, {{"fn", "UI.onClick"}, {"args", {{{"$f", 42}}}}});
    check(sent_id == 42, "el callback va a su script");
    check(json::parse(sent_args) == json::parse(R"(["boton",{"$v":[1,2,3]}])"), "con sus argumentos en JSON");
    a.invoke(api::Value::function(api::NativeApi::kLocalCallbackBase + 1), {api::Value(9)});
    check(local_id == api::NativeApi::kLocalCallbackBase + 1 && local_value == 9.0, "los del mismo proceso van al invocador local");
}

}  // namespace

int main() {
    testValues();
    testRegistry();
    testCallbacks();
    std::printf("\n%d/%d pruebas bien\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
