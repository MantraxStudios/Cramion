// Pruebas de native/NetworkApi.cpp: Network sin red (valores por defecto,
// errores), un servidor solo (host, spawn de un prefab, variables de red,
// objetos, compensacion de lag, envio de lo que se mueve, destroy) y lo de
// red de las entidades. Servidor y clientes juntos: tests/NetworkTests.cpp.

#include "ApiTest.h"

#include "CramionCore/cvar/CVar.h"
#include "CramionCore/ecs/Prefab.h"
#include "CramionCore/net/NetworkObject.h"

#include <cmath>
#include <filesystem>

using namespace cramion;
using namespace cramion::apitest;

namespace {

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

bool throws(ApiFixture& t, const std::string& key, const Value::Array& args, const Value& self = Value::nil()) {
    try {
        t.call(key, args, self);
    } catch (const scripting::api::Error&) {
        return true;
    }
    return false;
}

std::string cvarText(const char* name) {
    const cvar::CVarBase* v = cvar::Registry::instance().find(name);
    return v != nullptr ? v->toString() : std::string("?");
}

}  // namespace

int main() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_netapi_tests";
    std::filesystem::remove_all(root);
    scripting::registerScriptComponents();
    {
        // Prefab de red: un objeto con NetworkObject.
        ecs::World maker;
        ecs::Entity box = maker.create("Caja");
        box.add<net::NetworkObject>().send_rate = 60.0f;
        std::string error;
        check(ecs::createPrefab(maker, box, root / "Prefabs" / "Caja.crprefab", &error), "crear el prefab de red");
    }

    std::vector<std::pair<std::uint64_t, std::string>> callbacks;
    ApiFixture t(false);
    t.scripts.setAssetsRoot(root);
    t.scripts.start(t.world);
    t.scripts.setBridgeCallbackSink([&](std::uint64_t id, const std::string& args) { callbacks.emplace_back(id, args); });

    std::printf("Sin red\n");
    check(t.get("Network.SERVER").asNumber() == 1.0, "Network.SERVER es 1");
    check(!t.call("Network.isActive").truthy() && !t.call("Network.isServer").truthy() &&
              !t.call("Network.isClient").truthy() && !t.call("Network.isConnected").truthy() &&
              !t.call("Network.isConnecting").truthy(),
          "sin sesion: isActive/isServer/isClient/isConnected/isConnecting son false");
    check(t.call("Network.myId").asNumber() == 0.0 && t.call("Network.playerCount").asNumber() == 0.0 &&
              t.call("Network.players").isArray() && t.call("Network.players").size() == 0 &&
              t.call("Network.ping").asNumber() == 0.0,
          "myId 0, sin jugadores, ping 0");
    const Value stats0 = t.call("Network.stats");
    check(stats0["objects"].asNumber() == 0.0 && stats0["sent"].asNumber() == 0.0 && stats0["bytesReceived"].isNumber(),
          "Network.stats sin red: ceros");
    check(t.call("Network.send", {Value("chat"), Value("hola")}).isBool() &&
              !t.call("Network.send", {Value("chat"), Value("hola")}).truthy(),
          "Network.send sin conexion devuelve false");
    check(t.call("Network.spawn", {Value("Prefabs/Caja")}).isNil() && t.logged("solo el servidor crea objetos"),
          "Network.spawn sin ser servidor: nil y un error");
    t.call("Network.loadScene", {Value("Nivel2")});
    check(t.logged("solo el servidor cambia la escena"), "Network.loadScene sin ser servidor: error");
    check(t.call("Network.positionAt", {Value(1), Value(0.0)}).isNil() &&
              t.call("Network.lagCompensatedRaycast", {Value(core::Vec3{}), Value(core::Vec3{0, 0, 1}), Value(10), Value(2)}).isNil(),
          "positionAt y lagCompensatedRaycast sin red: nil");
    check(throws(t, "Network.on", {Value("chat"), Value(3)}), "Network.on con algo que no es una funcion: error");

    ecs::Entity plain = t.world.create("Normal");
    const Value plain_v = ApiFixture::entity(plain);
    check(t.call("Entity:isMine", {}, plain_v).truthy(), "sin red, todo es mio (isMine)");
    check(t.get("netId", plain_v).asNumber() == 0.0 && t.get("netOwner", plain_v).asNumber() == 0.0,
          "netId y netOwner 0 en un objeto que no es de red");
    t.call("Entity:setNetVar", {Value("vida"), Value(10)}, plain_v);
    check(t.call("Entity:getNetVar", {Value("vida")}, plain_v).isNil(), "setNetVar/getNetVar sin red: nada");

    // Servidor dedicado: CramionServer hace Network.dedicated = true.
    check(!t.call("Network.isDedicated").truthy() && !t.get("Network.dedicated").truthy(), "no es dedicado por defecto");
    const json set_r = t.bridge({{"op", "set"}, {"key", "Network.dedicated"}, {"value", true}});
    check(set_r["ok"] == true && t.call("Network.isDedicated").truthy(), "Network.dedicated = true (puente) -> isDedicated");
    t.scripts.stop();
    t.scripts.start(t.world);
    check(t.call("Network.isDedicated").truthy(), "sigue siendo dedicado tras stop()/start()");
    t.set("Network.dedicated", Value(false));
    check(!t.call("Network.isDedicated").truthy(), "y se puede quitar");

    // Simulador de red mala (CVars).
    Value sim = Value::object();
    sim.set("latency", 50);
    sim.set("jitter", 10.7);
    sim.set("loss", 2);
    t.call("Network.simulate", {sim});
    check(cvarText("net.SimLatencyMs") == "50" && cvarText("net.SimJitterMs") == "10" && cvarText("net.SimLossPercent") == "2",
          "Network.simulate{latency, jitter, loss} cambia las CVars");
    t.call("Network.simulate", {Value::object()});
    check(cvarText("net.SimLatencyMs") == "0" && cvarText("net.SimLossPercent") == "0", "Network.simulate{} vuelve a la normal");

    std::printf("\nServidor\n");
    const json host = t.bridge({{"fn", "Network.host"}, {"args", {27791, 4}}});
    check(host["ok"] == true && host["result"].is_array() && host["result"][0] == true && host["result"][1] == "",
          "Network.host devuelve {ok, error} (puente)");
    check(t.call("Network.isServer").truthy() && t.call("Network.isConnected").truthy() && t.call("Network.isActive").truthy(),
          "isServer, isConnected e isActive");
    check(t.call("Network.myId").asNumber() == 1.0 && t.call("Network.playerCount").asNumber() == 1.0 &&
              t.call("Network.players")[0].asNumber() == 1.0,
          "el servidor es el jugador 1 (players = {1})");

    const Value box_v = t.call("Network.spawn", {Value("Prefabs/Caja"), Value(core::Vec3{1, 2, 3}), Value(), Value(core::Vec3{0, 90, 0})});
    const ecs::Entity box = t.world.wrap(box_v.asEntity());
    check(box_v.isEntity() && box.valid() && box.has<net::NetworkObject>(), "Network.spawn crea el prefab con NetworkObject");
    check(box.valid() && near(box.worldPosition().y, 2.0f) && std::fabs(box.localRotation().y) > 0.5f,
          "en su posicion y con su giro (grados)");
    check(t.get("netId", box_v).asNumber() == 1.0 && t.get("netOwner", box_v).asNumber() == 1.0 &&
              t.call("isMine", {}, box_v).truthy(),
          "netId 1, del servidor e isMine");
    check(t.call("Network.spawn", {Value("Prefabs/NoExiste")}).isNil() && t.logged("no existe el prefab \"Prefabs/NoExiste\""),
          "un prefab que no existe: nil y un error");
    const Value objects = t.call("Network.objects");
    check(objects.size() == 1 && objects[0].asEntity() == box.handle(), "Network.objects");
    check(t.call("Network.find", {Value(1)}).asEntity() == box.handle() && t.call("Network.find", {Value(99)}).isNil(),
          "Network.find por id de red (nil si no hay)");
    check(t.call("Network.stats")["objects"].asNumber() == 1.0, "Network.stats cuenta los objetos");

    // Variables de red: numeros, tablas (listas y objetos), Vec3 y entidades.
    t.call("Entity:setNetVar", {Value("vida"), Value(100)}, box_v);
    check(t.call("Entity:getNetVar", {Value("vida")}, box_v).asNumber() == 100.0, "setNetVar / getNetVar con un numero");
    Value data = Value::object();
    data.set("lista", Value(Value::Array{Value(1), Value("dos"), Value(true)}));
    data.set("pos", Value(core::Vec3{4, 5, 6}));
    data.set("yo", box_v);
    t.call("Entity:setNetVar", {Value("datos"), data}, box_v);
    const Value back = t.call("Entity:getNetVar", {Value("datos")}, box_v);
    check(back.isObject() && back["lista"].isArray() && back["lista"].size() == 3 && back["lista"][1].asString() == "dos" &&
              back["lista"][2].truthy(),
          "una lista vuelve como lista");
    check(back["pos"].isVec3() && near(back["pos"].asVec3().z, 6.0f), "un Vec3 vuelve como Vec3");
    check(back["yo"].asNumber() == 1.0, "una entidad viaja como su id de red");
    check(t.call("Entity:getNetVar", {Value("nada")}, box_v).isNil(), "una variable que no existe: nil");

    // Compensacion de lag: la caja (del servidor) vista por el jugador 2.
    const Value at = t.call("Network.positionAt", {Value(1), Value(0.0)});
    check(at.isVec3() && near(at.asVec3().x, 1.0f), "Network.positionAt");
    const Value hit = t.call("Network.lagCompensatedRaycast",
                             {Value(core::Vec3{1, 3, -10}), Value(core::Vec3{0, 0, 2}), Value(50), Value(2)});
    check(hit.isObject() && hit["netId"].asNumber() == 1.0 && near(static_cast<float>(hit["distance"].asNumber()), 12.5f, 0.01f) &&
              hit["point"].isVec3(),
          "Network.lagCompensatedRaycast toca la caja {netId, point, distance}");
    check(t.call("Network.lagCompensatedRaycast", {Value(core::Vec3{1, 3, -10}), Value(core::Vec3{0, 0, 1}), Value(50), Value(1)}).isNil(),
          "no toca lo del propio jugador");

    // Lo mio que se mueve sale a la red al final del frame.
    ecs::Entity moving = box;
    moving.setWorldPosition(core::Vec3{5, 2, 3});
    t.frame(0.05f);
    const Value moved = t.call("Network.positionAt", {Value(1), Value(0.0)});
    check(moved.isVec3() && near(moved.asVec3().x, 5.0f), "la posicion nueva sale en la fase End");

    // Mensajes y callbacks (se guardan; los llama la red).
    t.call("Network.on", {Value("chat"), Value::function(7)});
    t.call("Network.off", {Value("chat")});
    t.call("Network.onPlayerJoined", {Value::function(8)});
    t.call("Network.onPlayerJoined", {Value()});
    check(t.call("Network.send", {Value("chat"), data}).truthy(), "Network.send con sesion devuelve true");

    t.call("Network.destroy", {box_v});
    t.frame();
    check(!t.world.valid(box_v.asEntity()) && t.call("Network.objects").size() == 0 && t.call("Network.find", {Value(1)}).isNil(),
          "Network.destroy lo borra (al final del frame)");

    // Cambio de escena: la sesion sigue.
    t.scripts.stop();
    t.scripts.start(t.world);
    check(t.call("Network.isServer").truthy(), "stop()/start() no cierra la sesion");
    t.call("Network.disconnect");
    check(!t.call("Network.isActive").truthy() && !t.call("Network.isServer").truthy(), "Network.disconnect la cierra");
    check(callbacks.empty(), "sin eventos, ningun callback");

    const json bad = t.bridge({{"fn", "Network.host"}, {"args", {27791, 4}}});
    check(bad["ok"] == true && bad["result"][0] == true, "se puede volver a abrir");
    t.call("Network.disconnect");
    t.scripts.shutdownNetwork();

    t.scripts.stop();
    std::filesystem::remove_all(root);
    return finish();
}
