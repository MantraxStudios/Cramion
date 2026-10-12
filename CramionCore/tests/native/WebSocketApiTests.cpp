// Pruebas de native/WebSocketApi.cpp (WebSocket.* y Base64.*) contra el
// servidor de prueba de tests/WsTestServer.h en 127.0.0.1: direcciones que no
// valen, escuchas (on) y poll, binario en base64, conexiones con nombre que
// sobreviven al cambio de escena, resetBridgeCallbacks (recarga de los scripts
// de C++), shutdownNetwork, sendJson, el tope de eventos por frame, el puente
// con bytes que no son UTF-8 y el nodo On WebSocket de Visual Scripting.

#include "../WsTestServer.h"  // antes que nada (winsock2.h antes que windows.h)

#include "ApiTest.h"

#include "CramionCore/cvar/CVar.h"
#include "CramionCore/net/Encoding.h"
#include "CramionCore/scripting/VisualScript.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace cramion;
using namespace cramion::apitest;

namespace {

const std::filesystem::path kRoot = std::filesystem::temp_directory_path() / "cramion_ws_api_test";

struct Calls {
    std::vector<std::pair<std::uint64_t, json>> list;
    // La llamada numero nth (desde 0) a ese callback, o null.
    json nth(std::uint64_t id, std::size_t n) const {
        for (const auto& [i, args] : list) {
            if (i != id) continue;
            if (n == 0) return args;
            --n;
        }
        return nullptr;
    }
    std::size_t count(std::uint64_t id) const {
        std::size_t n = 0;
        for (const auto& [i, args] : list) n += i == id ? 1 : 0;
        return n;
    }
};

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Frames hasta que llegue la llamada nth al callback `id` (max ~3 s).
json waitFor(ApiFixture& t, Calls& calls, std::uint64_t id, std::size_t nth = 0) {
    for (int i = 0; i < 300; ++i) {
        t.frame();
        json args = calls.nth(id, nth);
        if (!args.is_null()) return args;
        sleepMs(10);
    }
    return nullptr;
}

Value connectTo(ApiFixture& t, const std::string& url, const std::string& name = {}) {
    Value o = Value::object();
    o.set("url", url);
    if (!name.empty()) o.set("name", name);
    return t.call("WebSocket.connect", {o});
}

// Frames hasta que la conexion este abierta (max ~5 s).
bool waitOpen(ApiFixture& t, const Value& id) {
    for (int i = 0; i < 500; ++i) {
        if (t.call("WebSocket.state", {id}).asString() == "open") return true;
        t.frame();
        sleepMs(10);
    }
    return false;
}

// Los mensajes de texto de una lista de eventos (de WebSocket.poll).
std::vector<std::string> texts(const Value& events) {
    std::vector<std::string> out;
    if (!events.isArray()) return out;
    for (const Value& ev : events.items()) {
        if (ev["type"].asString() == "message" && !ev["binary"].truthy()) out.push_back(ev["text"].asString());
    }
    return out;
}

// poll hasta tener `want` mensajes de texto (max ~3 s).
std::vector<std::string> pollTexts(ApiFixture& t, const Value& id, std::size_t want) {
    std::vector<std::string> got;
    for (int i = 0; i < 300 && got.size() < want; ++i) {
        for (std::string& s : texts(t.call("WebSocket.poll", {id}))) got.push_back(std::move(s));
        if (got.size() < want) {
            t.frame();
            sleepMs(10);
        }
    }
    return got;
}

bool waitServerClose(const wstest::WsTestServer& server, int code) {
    for (int i = 0; i < 300; ++i) {
        if (server.sawClose(code)) return true;
        sleepMs(10);
    }
    return false;
}

std::string variable(ApiFixture& t, ecs::Entity e, const char* name) {
    scripting::ScriptSystem::VisualScriptDebug d;
    if (!t.scripts.visualScriptDebug(e, d)) return "(sin instancia)";
    const auto it = d.variables.find(name);
    return it == d.variables.end() ? "(no existe)" : it->second;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    wstest::WsTestServer server;
    std::printf("Servidor de prueba en el puerto %d\n", server.port());

    {
        Calls calls;
        ApiFixture t;
        t.scripts.setBridgeCallbackSink([&](std::uint64_t id, const std::string& args) {
            calls.list.emplace_back(id, json::parse(args, nullptr, false));
        });

        std::printf("Direcciones\n");
        check(connectTo(t, "ftp://ejemplo.com/").isNil() && t.logged("WebSocket.connect:"),
              "1. una direccion que no es ws:// ni wss://: nil y un error");
        check(connectTo(t, "ws://8.8.8.8/").isNil(), "1. ws:// sin cifrar a Internet: nil");
        bool threw = false;
        try {
            t.call("WebSocket.connect", {Value(5)});
        } catch (const scripting::api::Error&) {
            threw = true;
        }
        check(threw, "1. connect con un numero: error de argumentos");
        check(t.call("WebSocket.state", {Value(12345)}).asString() == "closed" && t.call("WebSocket.info", {Value(12345)}).isNil(),
              "una conexion que no existe: \"closed\" e info nil");

        std::printf("\nEscuchas (on)\n");
        const Value id = connectTo(t, server.url("/eco"));
        check(id.isNumber() && id.asNumber() > 0.0, "2. connect devuelve el id");
        json on = t.bridge({{"fn", "WebSocket.on"}, {"args", json::array({id.asNumber(), "message", json{{"$f", 7}}})}});
        check(on["ok"] == true && on["result"].is_number(), "2. on por el puente devuelve el numero de la escucha");
        check(waitOpen(t, id), "2. se abre");
        check(t.call("WebSocket.send", {id, Value("{\"puntos\": 5}")}).truthy(), "2. send devuelve true");
        const json m = waitFor(t, calls, 7);
        check(m.is_array() && m.size() == 1 && m[0]["type"] == "message" && m[0]["text"] == "{\"puntos\": 5}" &&
                  m[0]["binary"] == false && m[0]["id"] == id.asNumber(),
              "2. la funcion recibe {type = \"message\", id, text}");
        check(m.is_array() && !m.empty() && m[0]["data"].is_object() && m[0]["data"]["puntos"] == 5,
              "2. y data con el JSON ya decodificado");
        const std::string raw("\x00\x01\xFF hola", 8);
        check(t.call("WebSocket.sendBinary", {id, Value(net::base64Encode(raw))}).truthy(), "3. sendBinary (base64)");
        const json b = waitFor(t, calls, 7, 1);
        check(b.is_array() && !b.empty() && b[0]["binary"] == true && b[0]["base64"] == net::base64Encode(raw) &&
                  !b[0].contains("text"),
              "3. el binario llega como base64 (nunca en bruto)");
        check(!t.call("WebSocket.sendBinary", {id, Value("@@ no es base64")}).truthy() && t.logged("base64 no valido"),
              "3. sendBinary con base64 malo: false y un aviso");
        threw = false;
        try {
            t.call("WebSocket.close", {id, Value(1234)});
        } catch (const scripting::api::Error&) {
            threw = true;
        }
        check(threw, "codigo de cierre no valido (1234): error");
        const Value info = t.call("WebSocket.info", {id});
        check(info.isObject() && info["state"].asString() == "open" && info["url"].asString() == server.url("/eco") &&
                  info["sent"].asNumber() >= 2.0,
              "info: estado, url y mensajes enviados");

        std::printf("\nSin escuchas (poll)\n");
        const Value id2 = connectTo(t, server.url("/eco"));
        check(waitOpen(t, id2), "4. se abre");
        t.call("WebSocket.send", {id2, Value("uno")});
        t.call("WebSocket.send", {id2, Value("dos")});
        const std::vector<std::string> got = pollTexts(t, id2, 2);
        check(got == std::vector<std::string>{"uno", "dos"}, "4. poll devuelve los mensajes en orden");
        for (int i = 0; i < 20; ++i) {
            t.frame();
            sleepMs(5);
        }
        check(texts(t.call("WebSocket.poll", {id2})).empty(), "4. y nunca dos veces");

        std::printf("\nCon nombre\n");
        const Value mmo = connectTo(t, server.url("/eco"), "mmo");
        check(mmo.isNumber() && connectTo(t, server.url("/eco"), "mmo").asNumber() == mmo.asNumber(),
              "5. connect{name = \"mmo\"} dos veces: el mismo id");
        check(t.call("WebSocket.find", {Value("mmo")}).asNumber() == mmo.asNumber(), "5. find(\"mmo\")");
        check(waitOpen(t, mmo), "5. se abre");
        check(t.call("WebSocket.list").size() == 3 && t.call("WebSocket.count").asNumber() == 3.0, "list y count: 3");

        std::printf("\nCambio de escena (stop / start)\n");
        check(t.call("WebSocket.send", {Value("mmo"), Value("hueco")}).truthy(), "6. send por nombre");
        t.scripts.stop();
        sleepMs(300);
        t.scripts.start(t.world);
        check(waitServerClose(server, 1001), "6. las que no tienen nombre se cierran (el servidor ve 1001)");
        check(t.call("WebSocket.state", {id}).asString() != "open" && t.call("WebSocket.state", {id2}).asString() != "open",
              "6. ...y ya no estan abiertas");
        check(t.call("WebSocket.find", {Value("mmo")}).asNumber() == mmo.asNumber() &&
                  t.call("WebSocket.state", {mmo}).asString() == "open",
              "6. la de nombre sigue abierta y con el mismo id");
        const std::vector<std::string> gap = pollTexts(t, mmo, 1);
        check(std::find(gap.begin(), gap.end(), "hueco") != gap.end(), "6. lo que llego durante el cambio se puede recoger con poll");

        std::printf("\nRecarga de los scripts de C++ (resetBridgeCallbacks)\n");
        std::vector<std::uint64_t> local;
        t.api().setLocalInvoker([&](std::uint64_t cb, const Value::Array&) { local.push_back(cb); });
        const std::uint64_t local_id = scripting::api::NativeApi::kLocalCallbackBase + 9;
        json on8 = t.bridge({{"fn", "WebSocket.on"}, {"args", json::array({mmo.asNumber(), "message", json{{"$f", 8}}})}});
        t.call("WebSocket.on", {Value("mmo"), Value("message"), Value::function(local_id)});
        check(on8["ok"] == true, "7. dos escuchas: una de C++ ($f) y una local (>= 2^63)");
        t.scripts.resetBridgeCallbacks();
        t.call("WebSocket.send", {mmo, Value("tras recargar")});
        for (int i = 0; i < 300 && local.empty(); ++i) {
            t.frame();
            sleepMs(10);
        }
        check(std::find(local.begin(), local.end(), local_id) != local.end(), "7. la local sigue");
        check(calls.count(8) == 0, "7. la de C++ se olvido (su proceso se reinicio)");
        check(t.call("WebSocket.state", {mmo}).asString() == "open", "7. la conexion sigue abierta");

        std::printf("\nsendJson\n");
        const Value id4 = connectTo(t, server.url("/eco"));
        t.bridge({{"fn", "WebSocket.on"}, {"args", json::array({id4.asNumber(), "message", json{{"$f", 10}}})}});
        check(waitOpen(t, id4), "9. se abre");
        Value table = Value::object();
        table.set("a", 1);
        table.set("b", "x");
        table.set("lista", Value(Value::Array{Value(1), Value("dos")}));
        check(t.call("WebSocket.sendJson", {id4, table}).truthy(), "9. sendJson devuelve true");
        const json j = waitFor(t, calls, 10);
        check(j.is_array() && !j.empty() && j[0]["data"]["a"] == 1 && j[0]["data"]["b"] == "x" &&
                  j[0]["data"]["lista"] == json::array({1, "dos"}),
              "9. ida y vuelta: data con la tabla");

        std::printf("\nTope de eventos por frame\n");
        std::string error;
        check(cvar::Registry::instance().set("net.WsMaxEventsPerFrame", "16", &error), "10. net.WsMaxEventsPerFrame = 16");
        const Value burst = connectTo(t, server.url("/rafaga"));
        t.bridge({{"fn", "WebSocket.on"}, {"args", json::array({burst.asNumber(), "message", json{{"$f", 11}}})}});
        std::size_t max_per_frame = 0;
        int frames = 0;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (calls.count(11) < 1000 && std::chrono::steady_clock::now() < end) {
            const std::size_t before = calls.count(11);
            t.frame();
            const std::size_t delivered = calls.count(11) - before;
            max_per_frame = std::max(max_per_frame, delivered);
            frames += delivered > 0 ? 1 : 0;
            sleepMs(1);
        }
        std::printf("    %zu mensajes en %d frames (max %zu por frame)\n", calls.count(11), frames, max_per_frame);
        check(calls.count(11) == 1000, "10. llegan los 1000");
        check(max_per_frame <= 16 && frames >= 1000 / 16, "10. repartidos: como mucho 16 por frame");
        bool ordered = true;
        for (std::size_t i = 0; i < 1000 && ordered; ++i) {
            json ev = calls.nth(11, i);
            char number[8];
            std::snprintf(number, sizeof(number), "%04zu", i);
            ordered = ev.is_array() && !ev.empty() && ev[0]["text"].is_string() &&
                      ev[0]["text"].get<std::string>().compare(0, 4, number) == 0;
        }
        check(ordered, "10. en orden");
        cvar::Registry::instance().set("net.WsMaxEventsPerFrame", "512", &error);

        std::printf("\nBase64\n");
        check(t.call("Base64.encode", {Value("hola")}).asString() == "aG9sYQ==", "Base64.encode");
        check(t.call("Base64.decode", {Value("aG9sYQ==")}).asString() == "hola" &&
                  t.call("Base64.decode", {Value("aG9sYQ")}).asString() == "hola",
              "Base64.decode (con y sin relleno)");
        check(t.call("Base64.decode", {Value("@@@")}).isNil() && t.logged("base64 no valido"), "Base64.decode malo: nil y un aviso");
        bool bridged = false;
        try {
            const json r = t.bridge({{"fn", "Base64.decode"}, {"args", json::array({net::base64Encode(std::string("\xFF\xFE\x80 abc"))})}});
            bridged = r["ok"] == true && r["result"].is_string();
        } catch (const std::exception& e) {
            std::printf("    excepcion: %s\n", e.what());
        }
        check(bridged, "11. bytes que no son UTF-8 cruzan el puente sin excepcion");

        std::printf("\nshutdownNetwork\n");
        server.clear();
        t.scripts.shutdownNetwork();
        check(t.call("WebSocket.count").asNumber() == 0.0 && t.call("WebSocket.find", {Value("mmo")}).isNil(),
              "8. shutdownNetwork: count() = 0");
        check(waitServerClose(server, 1001), "8. y el servidor ve los cierres");
        t.scripts.stop();
    }

    std::printf("\nVisual Scripting: On WebSocket\n");
    {
        std::filesystem::remove_all(kRoot);
        std::filesystem::create_directories(kRoot / "Graphs");
        std::ofstream(kRoot / "Graphs" / "Ws.crgraph", std::ios::binary) << R"({
            "variables": [{"name": "recibido", "type": "any", "value": ""}],
            "nodes": [
                {"id": 1, "kind": "event.websocket", "values": {"Conexion": "vs", "Tipo": "message"}},
                {"id": 2, "kind": "var.set", "fn": "recibido"}
            ],
            "links": [{"from": 1, "to": 2}, {"from": 1, "out": "Texto", "to": 2, "in": "Valor"}]
        })";
        vscript::registerVisualScriptComponents();
        ApiFixture tv(false);
        tv.scripts.setAssetsRoot(kRoot);
        ecs::Entity e = tv.world.create("Red");
        e.add<vscript::VisualScript>().graph = "Graphs/Ws.crgraph";
        tv.scripts.start(tv.world);
        tv.frame();  // el grafo escucha "vs" antes de que exista la conexion
        const Value vs = connectTo(tv, server.url("/eco"), "vs");
        check(waitOpen(tv, vs), "12. se abre");
        tv.call("WebSocket.send", {vs, Value("desde el grafo")});
        for (int i = 0; i < 300 && variable(tv, e, "recibido") != "desde el grafo"; ++i) {
            tv.frame();
            sleepMs(10);
        }
        check(variable(tv, e, "recibido") == "desde el grafo", "12. el nodo On WebSocket recibe el mensaje (Texto)");
        tv.scripts.stop();
        std::filesystem::remove_all(kRoot);
    }

    check(server.unmaskedFrames() == 0, "todas las tramas del cliente con mascara");
    server.stop();
    return finish();
}
