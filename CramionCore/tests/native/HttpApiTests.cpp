// Pruebas de native/HttpApi.cpp: Json (encode / decode), Http.urlEncode y
// query, la validacion de las peticiones y la entrega de las respuestas a su
// funcion. En Linux no hay HTTP: cada peticion valida vuelve con el error
// "HTTP no disponible en esta plataforma" (asi se prueba el camino entero).

#include "ApiTest.h"

#include <chrono>
#include <limits>
#include <thread>

using namespace cramion;
using namespace cramion::apitest;

namespace {

struct Calls {
    std::vector<std::pair<std::uint64_t, json>> list;
    const json* find(std::uint64_t id) const {
        for (const auto& [i, args] : list) {
            if (i == id) return &args;
        }
        return nullptr;
    }
};

// Frames hasta que llegue el callback `id` (max ~3 s).
const json* waitFor(ApiFixture& t, Calls& calls, std::uint64_t id) {
    for (int i = 0; i < 300; ++i) {
        t.frame();
        if (const json* args = calls.find(id)) return args;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return nullptr;
}

}  // namespace

int main() {
    Calls calls;
    ApiFixture t;
    t.scripts.setBridgeCallbackSink([&](std::uint64_t id, const std::string& args) {
        calls.list.emplace_back(id, json::parse(args, nullptr, false));
    });

    std::printf("Json\n");
    Value data = Value::object();
    data.set("n", 3);
    data.set("x", 2.5);
    data.set("texto", "hola");
    data.set("ok", true);
    data.set("pos", Value(core::Vec3{1, 2, 3}));
    data.set("lista", Value(Value::Array{Value(1), Value("dos"), Value()}));
    data.set("vacia", Value(Value::Array{}));
    data.set("objeto", Value::object());
    const Value encoded = t.call("Json.encode", {data});
    const json j = json::parse(encoded.asString(), nullptr, false);
    check(encoded.isString() && j.is_object(), "Json.encode de un objeto");
    check(j["n"].is_number_integer() && j["n"] == 3 && j["x"] == 2.5 && j["texto"] == "hola" && j["ok"] == true,
          "numeros (enteros como enteros), texto y bool");
    check(j["pos"] == json{{"x", 1.0}, {"y", 2.0}, {"z", 3.0}}, "un Vec3 se escribe como {x, y, z}");
    check(j["lista"] == json::array({1, "dos", nullptr}), "una lista es un array (nil = null)");
    check(j["vacia"].is_array() && j["objeto"].is_object(), "lista vacia [] y objeto vacio {}");
    check(encoded.asString().find('\n') == std::string::npos &&
              t.call("Json.encode", {data, Value(true)}).asString().find("\n  ") != std::string::npos,
          "compacto o bonito (sangria de 2)");
    check(t.call("Json.encode", {Value("a\"b")}).asString() == "\"a\\\"b\"" && t.call("Json.encode", {Value()}).asString() == "null",
          "texto suelto y nil");
    check(t.call("Json.encode", {Value(std::numeric_limits<double>::infinity())}).asString() == "null", "infinito = null");
    Value bad = Value::object();
    bad.set("f", Value::function(5));
    check(t.call("Json.encode", {bad}).isNil() && t.logged("Json.encode: no se puede pasar a JSON un valor de tipo function"),
          "una funcion no se puede escribir: nil y un error");
    check(t.call("Json.encode", {Value(core::Quat{})}).isNil() && t.logged("no se puede pasar a JSON un userdata"),
          "un Quat tampoco");

    const Value decoded = t.call("Json.decode", {Value(R"({"a": [1, 2.5, {"b": null}], "s": "hola", "t": true, "o": {}})")});
    check(decoded.isObject() && decoded["a"].isArray() && decoded["a"].size() == 3, "Json.decode de un objeto con una lista");
    check(decoded["a"][0].asNumber() == 1.0 && decoded["a"][1].asNumber() == 2.5 && decoded["a"][2]["b"].isNil(),
          "numeros y null = nil");
    check(decoded["s"].asString() == "hola" && decoded["t"].truthy() && decoded["o"].isObject(), "texto, bool y objeto vacio");
    check(t.call("Json.decode", {Value(R"({"$v": [1, 2, 3]})")}).isObject(), "las claves especiales del puente no se interpretan");
    check(t.call("Json.decode", {Value("[1, 2")}).isNil(), "texto que no es JSON: nil...");
    bool warned = false;
    for (const auto& [level, message] : t.log) {
        if (level == 1 && message == "Json.decode: el texto no es JSON valido") warned = true;
    }
    check(warned, "...y un aviso");
    const json round = t.bridge({{"fn", "Json.decode"}, {"args", {"{\"lista\": [1, \"x\"]}"}}});
    check(round["ok"] == true && round["result"] == json{{"lista", {1.0, "x"}}}, "Json.decode por el puente");

    std::printf("\nHttp: utilidades y validacion\n");
    check(t.call("Http.urlEncode", {Value("hola mundo/?&")}).asString() == "hola%20mundo%2F%3F%26", "Http.urlEncode");
    Value params = Value::object();
    params.set("q", "hola mundo");
    params.set("page", 2);
    params.set("ok", true);
    params.set("f", 0.5);
    params.set("nada", Value());
    check(t.call("Http.query", {params}).asString() == "f=0.5&ok=true&page=2&q=hola%20mundo", "Http.query (claves en orden)");
    check(t.call("Http.pending").asNumber() == 0.0, "Http.pending sin peticiones");

    check(t.call("Http.get", {Value("ftp://example.com/")}).isNil() && t.logged("Http.get: "), "Http.get: una URL que no es http(s)");
    check(t.call("Http.get", {Value("http://example.com/")}).isNil(), "http:// solo a este equipo");
    check(t.call("Http.post", {Value("https://example.com/"), Value(5)}).isNil() &&
              t.logged("Http.post: el cuerpo debe ser texto o una tabla (se manda como JSON)"),
          "Http.post con un cuerpo que no es texto ni tabla");
    Value bad_headers = Value::object();
    bad_headers.set("X-Malo", "a\nb");
    check(t.call("Http.get", {Value("https://example.com/"), Value(), bad_headers}).isNil() &&
              t.logged("cabecera no valida: X-Malo"),
          "una cabecera con un salto de linea se rechaza");
    check(t.call("Http.request", {Value::object()}).isNil() && t.logged("Http.request: "), "Http.request sin url");
    bool threw = false;
    try {
        t.call("Http.get", {});
    } catch (const scripting::api::Error&) {
        threw = true;
    }
    check(threw, "Http.get sin url: error de argumentos");

    std::printf("\nHttp: respuestas\n");
    Value headers = Value::object();
    headers.set("X-Juego", "Cramion");
    headers.set("X-Version", 3);
    const Value id = t.call("Http.get", {Value("https://example.com/api"), Value::function(1), headers});
    check(id.isNumber() && id.asNumber() > 0.0, "Http.get devuelve el id de la peticion");
    const json* r1 = waitFor(t, calls, 1);
    check(r1 != nullptr && r1->is_array() && r1->size() == 1, "la funcion recibe la respuesta");
    if (r1 != nullptr && r1->is_array() && !r1->empty()) {
        const json& res = (*r1)[0];
        check(res["ok"] == false && res["status"] == 0 && res["body"] == "" && res["headers"].is_object() &&
                  res["time"].is_number(),
              "res = {ok, status, body, time, headers}");
        check(res["error"] == "HTTP no disponible en esta plataforma", "res.error dice por que fallo");
    }
    Value body = Value::object();
    body.set("puntos", 10);
    const Value id2 = t.call("Http.post", {Value("https://example.com/marcador"), body, Value::function(2)});
    check(id2.isNumber() && id2.asNumber() != id.asNumber(), "Http.post con una tabla (JSON)");
    check(waitFor(t, calls, 2) != nullptr, "y su respuesta llega");

    const json req = t.bridge({{"fn", "Http.request"},
                               {"args",
                                {{{"url", "https://example.com/x"},
                                  {"method", "put"},
                                  {"timeout", 1},
                                  {"maxSize", 10},
                                  {"body", "texto"},
                                  {"headers", {{"Content-Type", "text/csv"}}}},
                                 {{"$f", 3}}}}});
    check(req["ok"] == true && req["result"].is_number(), "Http.request por el puente (metodo en minusculas)");
    const json* r3 = waitFor(t, calls, 3);
    check(r3 != nullptr && (*r3)[0]["error"] == "HTTP no disponible en esta plataforma", "su respuesta llega a la funcion");
    const json bad_method = t.bridge({{"fn", "Http.request"},
                                      {"args", {{{"url", "https://example.com/"}, {"method", "BAD METHOD"}}, {{"$f", 4}}}}});
    const json* r4 = bad_method["ok"] == true ? waitFor(t, calls, 4) : nullptr;
    check(r4 != nullptr && (*r4)[0]["error"].get<std::string>().find("Metodo no valido") != std::string::npos,
          "un metodo no valido vuelve como error de la respuesta");

    // Sin funcion: la respuesta se tira.
    t.call("Http.get", {Value("https://example.com/sin")});
    // cancelAll: lo pendiente ya no llama a nada.
    t.call("Http.get", {Value("https://example.com/c"), Value::function(5)});
    t.call("Http.cancelAll");
    for (int i = 0; i < 30; ++i) {
        t.frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(calls.find(5) == nullptr, "Http.cancelAll: su funcion ya no se llama");
    // Al parar el juego se olvidan.
    t.call("Http.get", {Value("https://example.com/p"), Value::function(6)});
    t.scripts.stop();
    t.scripts.start(t.world);
    for (int i = 0; i < 30; ++i) {
        t.frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(calls.find(6) == nullptr, "al parar el juego, las funciones pendientes se olvidan");
    for (int i = 0; i < 100 && t.call("Http.pending").asNumber() > 0.0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(t.call("Http.pending").asNumber() == 0.0, "Http.pending vuelve a 0");
    t.scripts.stop();
    return finish();
}
