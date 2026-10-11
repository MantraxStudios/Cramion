// Multijugador: un servidor y dos clientes en el mismo proceso (127.0.0.1).
// Valores, conexion, mensajes (y su reenvio), objetos replicados (spawn,
// posicion solo del dueno, variables), jugadores que entran tarde, salidas y
// cambio de escena. Al final, dos juegos (servidor y cliente) con la API de
// scripting (Network.*, Entity:isMine...) como la usan los scripts de C++.

#include "CramionCore/ecs/Prefab.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/net/Network.h"
#include "CramionCore/net/NetworkObject.h"
#include "CramionCore/scripting/NativeApi.h"
#include "CramionCore/scripting/Scripting.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace cramion;
using net::NetEvent;
using net::NetValue;
using scripting::api::Value;
using json = nlohmann::json;

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


bool hasLog(const std::vector<std::string>& log, const std::string& text) {
    return std::any_of(log.begin(), log.end(), [&](const std::string& l) { return l.find(text) != std::string::npos; });
}

// Un juego con su escena y su ScriptSystem, usado como lo usa un script de
// C++: llamadas a la API (directas o con el JSON del puente) y los callbacks
// que le llegan (id, argumentos), que la prueba atiende como lo haria el script.
struct Game {
    ecs::World world;
    scripting::ScriptSystem scripts;
    std::vector<std::string> log;                            // consola y lo que hacen los "scripts"
    std::vector<std::pair<std::uint64_t, json>> callbacks;   // pendientes de atender
    std::vector<std::string> messages;                       // mensajes a los scripts (OnNetCorrection...)
    bool sent_var = false;

    explicit Game(const std::filesystem::path& root) {
        scripts.setAssetsRoot(root);
        scripts.setLog([this](int, const std::string& m) { log.push_back(m); });
        scripts.setBridgeCallbackSink([this](std::uint64_t id, const std::string& args) {
            callbacks.emplace_back(id, json::parse(args, nullptr, false));
        });
        // Lo que reciben los scripts de C++ del objeto (onNetVar / onMessage).
        scripts.setNetVarListener([this](ecs::Entity, const std::string& key, const std::string& value) {
            const json v = json::parse(value, nullptr, false);
            log.push_back("var " + key + "=" + (v.is_string() ? v.get<std::string>() : v.dump()));
        });
        scripts.setMessageListener([this](ecs::Entity, const std::string& name, const std::string& value) {
            messages.push_back(name + " " + value);
        });
    }

    Value call(const std::string& key, const Value::Array& args = {}, const Value& self = Value::nil()) {
        return scripts.nativeApi().call(key, args, self);
    }
    Value get(const std::string& key, const Value& self) { return scripts.nativeApi().get(key, self); }
    json bridge(const json& request) { return json::parse(scripts.bridgeCall(request.dump())); }
    Value value(const json& j) { return scripts.nativeApi().fromJson(j); }
    void frame() { scripts.update(world, 0.016f); }
    std::vector<std::pair<std::uint64_t, json>> take() { return std::exchange(callbacks, {}); }

    ecs::Entity ball() {
        ecs::Entity found;
        for (const auto h : world.registry().view<net::NetworkObject>()) found = world.wrap(h);
        return found;
    }

    // El script de la bola: el dueno la mueve y pone una variable de red.
    void updateBall(float dt) {
        ecs::Entity b = ball();
        if (!b.valid()) return;
        const Value self = Value::entity(b.handle());
        if (!call("Entity:isMine", {}, self).truthy()) return;
        b.setWorldPosition(b.worldPosition() + core::Vec3{2.0f, 0.0f, 0.0f} * dt);
        if (!sent_var) {
            call("Entity:setNetVar", {Value("color"), Value("rojo")}, self);
            sent_var = true;
        }
    }
};

// Callbacks (ids que elige el "script").
enum : std::uint64_t { kJoined = 1, kChat, kLeft, kConnected = 11, kHello, kDisconnected };

// Dos juegos en el mismo proceso (servidor y cliente), todo con la API de scripting.
void testScripts() {
    std::printf("\nScripts: Network\n");
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_net_scripts";
    std::filesystem::remove_all(root);
    scripting::registerScriptComponents();
    {
        // El prefab de red: un NetworkObject rapido (y que no se teletransporte).
        ecs::World maker;
        ecs::Entity bola = maker.create("Bola");
        net::NetworkObject& n = bola.add<net::NetworkObject>();
        n.send_rate = 60.0f;
        n.max_speed = 20.0f;
        std::string error;
        check(ecs::createPrefab(maker, bola, root / "Prefabs" / "Bola.crprefab", &error), "crear el prefab de red");
    }

    Game server(root), client(root);
    server.world.create("Servidor");
    client.world.create("Cliente");
    server.scripts.start(server.world);
    client.scripts.start(client.world);

    // Start del servidor y del cliente.
    const json hosted = server.bridge({{"fn", "Network.host"}, {"args", {27781, 4}}});
    server.log.push_back("host " + std::string(hosted["ok"] == true && hosted["result"][0] == true ? "true" : "false"));
    server.call("Network.onPlayerJoined", {Value::function(kJoined)});
    server.call("Network.onPlayerLeft", {Value::function(kLeft)});
    server.call("Network.on", {Value("chat"), Value::function(kChat)});
    client.call("Network.connect", {Value("127.0.0.1"), Value(27781)});
    client.call("Network.onConnected", {Value::function(kConnected)});
    client.call("Network.on", {Value("hola"), Value::function(kHello)});
    check(client.call("Network.isConnecting").truthy(), "el cliente esta conectando");

    // Lo que hace cada script con sus callbacks.
    const auto serverScript = [&] {
        for (const auto& [id, args] : server.take()) {
            if (id == kJoined) {
                const int player = args[0].get<int>();
                server.log.push_back("entra " + std::to_string(player));
                server.call("Network.spawn", {Value("Prefabs/Bola"), Value(core::Vec3{0, 1, 0}), Value(player)});
                server.bridge({{"fn", "Network.send"},
                               {"args", {"hola", {{"texto", "bienvenido"}, {"n", player}, {"pos", {{"$v", {1, 2, 3}}}}}, player}}});
            } else if (id == kChat) {
                const Value d = server.value(args[0]);
                server.log.push_back("chat de " + std::to_string(args[1].get<int>()) + ": " + d["texto"].asString());
            } else if (id == kLeft) {
                server.log.push_back("sale " + std::to_string(args[0].get<int>()));
            }
        }
    };
    const auto clientScript = [&] {
        for (const auto& [id, args] : client.take()) {
            if (id == kConnected) {
                client.log.push_back("dentro " + std::to_string(args[0].get<int>()) +
                                     " servidor=" + (client.call("Network.isServer").truthy() ? "true" : "false"));
                Value chat = Value::object();
                chat.set("texto", "hola a todos");
                client.call("Network.send", {Value("chat"), chat});
            } else if (id == kHello) {
                const Value d = client.value(args[0]);
                char line[160];
                std::snprintf(line, sizeof(line), "hola de %d: %s %s y=%.1f", args[1].get<int>(), d["texto"].asString().c_str(),
                              d["n"].asString().c_str(), d["pos"].asVec3().y);
                client.log.push_back(line);
            } else if (id == kDisconnected) {
                client.log.push_back("fuera: " + args[0].get<std::string>());
            }
        }
    };
    const auto step = [&] {
        server.frame();
        client.frame();
        serverScript();
        clientScript();
        server.updateBall(0.016f);
        client.updateBall(0.016f);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };

    ecs::Entity client_ball, server_ball;
    for (int i = 0; i < 300; ++i) {
        step();
        client_ball = client.ball();
        server_ball = server.ball();
        if (client_ball.valid() && server_ball.valid() && server_ball.worldPosition().x > 0.5f && hasLog(server.log, "var color")) break;
    }
    if (std::getenv("NET_LOG")) {
        for (auto& l : server.log) std::printf("S: %s\n", l.c_str());
        for (auto& l : client.log) std::printf("C: %s\n", l.c_str());
    }
    check(hasLog(server.log, "host true"), "Network.host (puente)");
    check(hasLog(client.log, "dentro 2 servidor=false"), "onConnected con mi id");
    check(hasLog(server.log, "entra 2"), "onPlayerJoined en el servidor");
    check(hasLog(server.log, "chat de 2: hola a todos"), "Network.send / Network.on con un objeto (y quien lo manda)");
    check(hasLog(client.log, "hola de 1: bienvenido 2 y=2.0"), "mensaje del servidor a un jugador, con numeros y un Vec3");
    check(client_ball.valid() && client_ball.get<net::NetworkObject>().owner == 2, "Network.spawn crea el prefab en el cliente, suyo");
    check(client_ball.valid() && client.get("Entity:netOwner", Value::entity(client_ball.handle())).asNumber() == 2.0 &&
              client.call("Network.find", {client.get("Entity:netId", Value::entity(client_ball.handle()))}).asEntity() ==
                  client_ball.handle(),
          "netId / netOwner / Network.find en el cliente");
    check(client_ball.valid() && client_ball.worldPosition().x > 0.5f, "el cliente mueve su objeto (isMine)");
    check(server_ball.valid() && server_ball.worldPosition().x > 0.3f, "y el servidor lo ve moverse (sincronizado y suavizado)");
    check(server_ball.valid() && !server.call("Entity:isMine", {}, Value::entity(server_ball.handle())).truthy(),
          "en el servidor no es suyo (isMine false)");
    check(hasLog(server.log, "var color=rojo"), "setNetVar del dueno llega como OnNetVar");
    check(server_ball.valid() &&
              server.call("Entity:getNetVar", {Value("color")}, Value::entity(server_ball.handle())).asString() == "rojo",
          "y getNetVar la lee en el servidor");
    const Value players = client.call("Network.players");
    check(players.size() == 2 && client.call("Network.playerCount").asNumber() == 2.0, "el cliente conoce a los 2 jugadores");
    check(client.call("Network.stats")["ping"].isNumber() && server.call("Network.stats")["objects"].asNumber() == 1.0,
          "Network.stats en los dos");
    check(client.scripts.networkStatus().rfind("Cliente 2", 0) == 0 &&
              server.scripts.networkStatus().find("2 jugador") != std::string::npos,
          "estado de la red para el editor");

    // Un movimiento imposible (max_speed): el servidor lo corrige.
    ecs::Entity cheat = client_ball;
    if (cheat.valid()) cheat.setWorldPosition(core::Vec3{500.0f, 1.0f, 0.0f});
    bool corrected = false;
    for (int i = 0; i < 200 && !corrected; ++i) {
        step();
        corrected = hasLog(client.messages, "OnNetCorrection");
    }
    check(corrected, "el servidor corrige un movimiento imposible (OnNetCorrection al script)");
    check(cheat.valid() && cheat.worldPosition().x < 100.0f, "y el objeto vuelve a donde dice el servidor");

    // Cambio de escena: la sesion sigue viva tras stop()/start().
    client.scripts.stop();
    // La escena nueva ya no tiene los objetos de red de la anterior.
    std::vector<entt::entity> old_objects;
    for (const auto h : client.world.registry().view<net::NetworkObject>()) old_objects.push_back(h);
    for (const entt::entity h : old_objects) client.world.destroy(client.world.wrap(h));
    client.scripts.start(client.world);
    check(client.scripts.networkStatus().rfind("Cliente 2", 0) == 0, "stop()/start() (Scene.load) no desconecta");

    // El cliente sale: el servidor lo ve y su objeto desaparece.
    client.call("Network.disconnect");
    check(client.scripts.networkStatus().empty() && !client.call("Network.isActive").truthy(), "Network.disconnect");
    bool left = false;
    for (int i = 0; i < 300 && !(left && !server.ball().valid()); ++i) {
        step();
        left = hasLog(server.log, "sale 2");
    }
    check(left, "onPlayerLeft en el servidor");
    check(!server.ball().valid() && server.call("Network.objects").size() == 0, "y el objeto del que sale desaparece");

    // Vuelve a entrar y el servidor cierra: onDisconnected.
    client.call("Network.onConnected", {Value::function(kConnected)});
    client.call("Network.onDisconnected", {Value::function(kDisconnected)});
    const Value again = client.call("Network.connect", {Value(), Value(27781)});
    check(again.isArray() && again[0].truthy(), "Network.connect devuelve {ok, error} (127.0.0.1 por defecto)");
    bool inside = false;
    for (int i = 0; i < 300 && !inside; ++i) {
        step();
        inside = hasLog(client.log, "dentro 3");
    }
    check(inside, "vuelve a entrar (jugador 3)");
    server.scripts.shutdownNetwork();
    check(server.scripts.networkStatus().empty(), "shutdownNetwork cierra la sesion");
    bool out = false;
    for (int i = 0; i < 300 && !out; ++i) {
        step();
        out = hasLog(client.log, "fuera");
    }
    check(out, "al cerrar el servidor, onDisconnected en el cliente");
    check(!client.call("Network.isConnected").truthy() && !client.ball().valid(), "desconectado y sin objetos de red");
    client.scripts.shutdownNetwork();
    server.scripts.stop();
    client.scripts.stop();
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

    testScripts();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
