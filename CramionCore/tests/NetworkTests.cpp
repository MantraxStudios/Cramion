// Multijugador: un servidor y dos clientes en el mismo proceso (127.0.0.1).
// Valores, conexion, mensajes (y su reenvio), objetos replicados (spawn,
// posicion solo del dueno, variables), jugadores que entran tarde, salidas y
// cambio de escena.

#include "CramionCore/ecs/Prefab.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/net/Network.h"
#include "CramionCore/net/NetworkObject.h"
#include "CramionCore/scripting/Scripting.h"

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace cramion;
using net::NetEvent;
using net::NetValue;

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

struct Peer {
    net::NetworkSession session;
    std::vector<NetEvent> events;
    void pump() {
        session.update();
        for (NetEvent& e : session.takeEvents()) events.push_back(std::move(e));
    }
    const NetEvent* find(NetEvent::Type type, const std::string& text = {}) const {
        for (const NetEvent& e : events) {
            if (e.type == type && (text.empty() || e.text == text)) return &e;
        }
        return nullptr;
    }
};

// Hace girar a todos hasta que se cumpla `done` (max 3 s).
bool until(std::vector<Peer*> peers, const std::function<bool()>& done) {
    for (int i = 0; i < 300; ++i) {
        for (Peer* p : peers) p->pump();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}


void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

bool hasLog(const std::vector<std::string>& log, const std::string& text) {
    return std::any_of(log.begin(), log.end(), [&](const std::string& l) { return l.find(text) != std::string::npos; });
}

// Dos juegos en el mismo proceso (servidor y cliente), todo desde Lua.
void testLua() {
    std::printf("\nLua: Network\n");
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_net_lua";
    std::filesystem::remove_all(root);
    writeFile(root / "Scripts" / "Bola.lua", R"(
local B = {}
function B:Update(dt)
    if self.entity:isMine() then
        self.entity.position = self.entity.position + Vec3(2, 0, 0) * dt
        if not self.enviado then self.entity:setNetVar("color", "rojo"); self.enviado = true end
    end
end
function B:OnNetVar(clave, valor) Debug.log("var " .. clave .. "=" .. tostring(valor)) end
return B
)");
    writeFile(root / "Scripts" / "Servidor.lua", R"(
local S = {}
function S:Start()
    local ok, err = Network.host(27781, 4)
    Debug.log("host " .. tostring(ok))
    Network.onPlayerJoined(function(id)
        Debug.log("entra " .. id)
        Network.spawn("Prefabs/Bola", Vec3(0, 1, 0), id)
        Network.send("hola", { texto = "bienvenido", n = id, pos = Vec3(1, 2, 3) }, id)
    end)
    Network.on("chat", function(d, de) Debug.log("chat de " .. de .. ": " .. d.texto) end)
end
return S
)");
    writeFile(root / "Scripts" / "Cliente.lua", R"(
local C = {}
function C:Start()
    Network.connect("127.0.0.1", 27781)
    Network.onConnected(function(id)
        Debug.log("dentro " .. id .. " servidor=" .. tostring(Network.isServer()))
        Network.send("chat", { texto = "hola a todos" })
    end)
    Network.on("hola", function(d, de) Debug.log("hola de " .. de .. ": " .. d.texto .. " " .. d.n .. " y=" .. d.pos.y) end)
end
return C
)");
    scripting::registerScriptComponents();
    {
        // El prefab de red: un script y un NetworkObject rapido.
        ecs::World maker;
        ecs::Entity bola = maker.create("Bola");
        bola.add<scripting::Script>().file = "Scripts/Bola.lua";
        bola.add<net::NetworkObject>().send_rate = 60.0f;
        std::string error;
        check(ecs::createPrefab(maker, bola, root / "Prefabs" / "Bola.crprefab", &error), "crear el prefab de red");
    }

    ecs::World server_world, client_world;
    server_world.create("Servidor").add<scripting::Script>().file = "Scripts/Servidor.lua";
    client_world.create("Cliente").add<scripting::Script>().file = "Scripts/Cliente.lua";
    scripting::ScriptSystem server, client;
    std::vector<std::string> server_log, client_log;
    server.setAssetsRoot(root);
    client.setAssetsRoot(root);
    server.setLog([&](int, const std::string& m) { server_log.push_back(m); });
    client.setLog([&](int, const std::string& m) { client_log.push_back(m); });
    server.start(server_world);
    client.start(client_world);
    ecs::Entity client_ball, server_ball;
    for (int i = 0; i < 300; ++i) {
        server.update(server_world, 0.016f);
        client.update(client_world, 0.016f);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        for (const auto h : client_world.registry().view<net::NetworkObject>()) client_ball = client_world.wrap(h);
        for (const auto h : server_world.registry().view<net::NetworkObject>()) server_ball = server_world.wrap(h);
        if (client_ball.valid() && server_ball.valid() && server_ball.worldPosition().x > 0.5f && hasLog(server_log, "var color")) break;
    }
    if (std::getenv("NET_LOG")) { for (auto& l : server_log) std::printf("S: %s\n", l.c_str()); for (auto& l : client_log) std::printf("C: %s\n", l.c_str()); }
    check(hasLog(server_log, "host true"), "Network.host desde Lua");
    check(hasLog(client_log, "dentro 2 servidor=false"), "onConnected con mi id");
    check(hasLog(server_log, "entra 2"), "onPlayerJoined en el servidor");
    check(hasLog(server_log, "chat de 2: hola a todos"), "Network.send / Network.on con una tabla (y quien la manda)");
    check(hasLog(client_log, "hola de 1: bienvenido 2 y=2.0"), "mensaje del servidor a un jugador, con numeros y un Vec3");
    check(client_ball.valid() && client_ball.get<net::NetworkObject>().owner == 2, "Network.spawn crea el prefab en el cliente, suyo");
    check(client_ball.valid() && client_ball.worldPosition().x > 0.5f, "el cliente mueve su objeto (isMine)");
    check(server_ball.valid() && server_ball.worldPosition().x > 0.3f, "y el servidor lo ve moverse (sincronizado y suavizado)");
    check(hasLog(server_log, "var color=rojo"), "setNetVar del dueno llega como OnNetVar");
    check(client.networkStatus().rfind("Cliente 2", 0) == 0 && server.networkStatus().find("2 jugador") != std::string::npos,
          "estado de la red para el editor");

    // Cambio de escena: la sesion sigue viva tras stop()/start().
    client.stop();
    client.start(client_world);
    check(client.networkStatus().rfind("Cliente 2", 0) == 0, "stop()/start() (Scene.load) no desconecta");
    client.shutdownNetwork();
    server.shutdownNetwork();
    check(client.networkStatus().empty() && server.networkStatus().empty(), "shutdownNetwork cierra la sesion");
    server.stop();
    client.stop();
    std::filesystem::remove_all(root);
}
}  // namespace

int main() {
    std::printf("Valores\n");
    {
        NetValue table;
        table.type = NetValue::Type::Table;
        table.entries.emplace_back(NetValue::str("vida"), NetValue::num(87.5));
        table.entries.emplace_back(NetValue::num(1), NetValue::vec3(core::Vec3{1.0f, 2.0f, 3.0f}));
        NetValue inner;
        inner.type = NetValue::Type::Table;
        inner.entries.emplace_back(NetValue::str("ok"), NetValue::boolean(true));
        table.entries.emplace_back(NetValue::str("anidada"), inner);
        std::string bytes;
        net::encodeValue(bytes, table);
        NetValue back;
        std::size_t at = 0;
        check(net::decodeValue(bytes, at, back) && back == table && at == bytes.size(), "tabla anidada con numeros, texto, Vec3 y bool");
        std::string cut = bytes.substr(0, bytes.size() - 3);
        at = 0;
        check(!net::decodeValue(cut, at, back), "un paquete cortado se rechaza");
        std::string huge;
        huge.push_back(static_cast<char>(NetValue::Type::Table));
        huge.append("\xff\xff\xff\x7f", 4);
        at = 0;
        check(!net::decodeValue(huge, at, back), "una tabla con 2.000 millones de entradas falsas se rechaza");
    }

    const std::uint16_t port = 27777;
    Peer server, a, b;
    std::string error;
    std::printf("\nConexion\n");
    check(server.session.host(port, 8, &error), ("el servidor abre el puerto " + error).c_str());
    check(server.session.isServer() && server.session.localId() == net::kServerId, "el servidor es el jugador 1");
    check(a.session.connect("127.0.0.1", port, &error), "el cliente A conecta");
    check(until({&server, &a}, [&] { return a.session.connected(); }), "A recibe su id");
    check(a.session.localId() == 2 && a.find(NetEvent::Type::Connected) != nullptr, "A es el jugador 2 (evento Connected)");
    check(server.find(NetEvent::Type::PlayerJoined) != nullptr && server.session.players().size() == 2, "el servidor ve entrar a A");

    std::printf("\nObjetos\n");
    const std::uint32_t box = server.session.spawn("Prefabs/Caja", core::Vec3{1, 2, 3}, core::Quat{}, net::kServerId);
    const std::uint32_t avatar_a = server.session.spawn("Prefabs/Jugador", core::Vec3{0, 0, 0}, core::Quat{}, 2);
    server.session.setVar(box, "color", NetValue::str("rojo"));
    check(until({&server, &a}, [&] { return a.session.object(avatar_a) != nullptr && a.find(NetEvent::Type::Var, "color") != nullptr; }),
          "A recibe los dos objetos y la variable");
    const NetEvent* spawn = a.find(NetEvent::Type::Spawn);
    check(spawn != nullptr && spawn->text == "Prefabs/Caja" && spawn->position.y == 2.0f, "con su prefab y su posicion");
    check(a.session.owns(avatar_a) && !a.session.owns(box), "A es dueno de su jugador, no de la caja");

    // A mueve su jugador; tambien intenta mover la caja (no es suya).
    a.session.sendTransform(avatar_a, core::Vec3{5, 0, 0}, core::Quat{});
    a.session.sendTransform(box, core::Vec3{99, 99, 99}, core::Quat{});
    check(until({&server, &a}, [&] { return server.find(NetEvent::Type::Transform) != nullptr; }), "el servidor recibe la posicion de A");
    check(server.session.object(avatar_a)->position.x == 5.0f, "y la guarda");
    check(server.session.object(box)->position.x == 1.0f, "la caja no se mueve: A no es su dueno");

    std::printf("\nEntrar tarde y mensajes\n");
    check(b.session.connect("127.0.0.1", port, &error), "el cliente B conecta tarde");
    check(until({&server, &a, &b}, [&] {
              return b.session.connected() && b.session.object(avatar_a) != nullptr &&
                     b.find(NetEvent::Type::Var, "color") != nullptr;
          }),
          "B recibe los objetos que ya habia, su variable...");
    check(b.session.object(avatar_a)->position.x == 5.0f, "...y la ultima posicion del jugador de A");
    check(a.find(NetEvent::Type::PlayerJoined) != nullptr, "A ve entrar a B");
    check(b.session.players().size() == 3, "B conoce a los 3 jugadores");

    a.events.clear();
    b.events.clear();
    server.events.clear();
    NetValue hello;
    hello.type = NetValue::Type::Table;
    hello.entries.emplace_back(NetValue::str("texto"), NetValue::str("hola"));
    a.session.send("chat", hello);  // a todos
    check(until({&server, &a, &b}, [&] { return server.find(NetEvent::Type::Message, "chat") && b.find(NetEvent::Type::Message, "chat"); }),
          "un mensaje de A a todos llega al servidor y a B");
    check(b.find(NetEvent::Type::Message, "chat")->peer == 2, "B sabe que lo mando A (el servidor lo reenvia)");
    check(a.find(NetEvent::Type::Message, "chat") == nullptr, "a A no le vuelve su propio mensaje");
    b.session.send("privado", NetValue::num(7), 2);
    check(until({&server, &a, &b}, [&] { return a.find(NetEvent::Type::Message, "privado") != nullptr; }), "B manda solo a A");
    check(server.find(NetEvent::Type::Message, "privado") == nullptr, "el servidor lo reenvia sin quedarselo");
    server.session.send("ronda", NetValue::num(3));
    check(until({&server, &a, &b}, [&] { return a.find(NetEvent::Type::Message, "ronda") && b.find(NetEvent::Type::Message, "ronda"); }),
          "el servidor manda a todos");

    // Variable del dueno: llega a todos.
    a.session.setVar(avatar_a, "vida", NetValue::num(50));
    check(until({&server, &a, &b}, [&] { return b.find(NetEvent::Type::Var, "vida") != nullptr; }), "la variable de A llega a B por el servidor");

    std::printf("\nSalidas y escena\n");
    b.events.clear();
    a.session.close();
    check(until({&server, &b}, [&] { return b.find(NetEvent::Type::PlayerLeft) != nullptr && b.find(NetEvent::Type::Despawn) != nullptr; }),
          "al irse A, B lo ve salir y su jugador desaparece");
    check(server.session.object(avatar_a) == nullptr && server.session.object(box) != nullptr, "la caja del servidor sigue");
    server.session.loadScene("Nivel2");
    check(until({&server, &b}, [&] { return b.find(NetEvent::Type::Scene, "Nivel2") != nullptr; }), "el servidor manda cargar otra escena");
    check(b.session.objects().empty(), "y los objetos se olvidan");
    server.session.close();
    check(until({&b}, [&] { return b.find(NetEvent::Type::Disconnected) != nullptr; }), "al cerrar el servidor, B se entera");
    check(!b.session.connected(), "B queda desconectado");

    Peer lost;
    check(lost.session.connect("127.0.0.1", 27779, &error), "conectar a un puerto sin servidor...");
    bool gave_up = false;
    for (int i = 0; i < 1200 && !gave_up; ++i) {  // ENet se rinde en unos segundos
        lost.pump();
        gave_up = lost.find(NetEvent::Type::Disconnected) != nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(gave_up, "...acaba en Disconnected (no se queda colgado)");

    testLua();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
