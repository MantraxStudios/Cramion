#ifndef CRAMION_CORE_NET_NETWORK_H
#define CRAMION_CORE_NET_NETWORK_H

// Multijugador: una sesion de red (servidor o cliente) sobre UDP con ENet.
//
//   Servidor   Network.host(puerto): escucha y ademas juega (es el jugador 1).
//   Cliente    Network.connect(ip, puerto): recibe su id (2, 3...) al entrar.
//
// Todo pasa por el servidor: un cliente manda al servidor y este reenvia a los
// demas (y dice quien lo mando). Asi los clientes no necesitan verse entre
// si y el servidor puede comprobar lo que llega.
//
// Mensajes     nombre + un valor de Lua (nil, bool, numero, texto, Vec3 o una
//              tabla de ellos), fiables y en orden.
// Objetos      el servidor crea (spawn) un prefab en todos con un id de red y
//              un dueno (un jugador o el servidor). El dueno manda su posicion
//              y giro (sin fiabilidad: el ultimo que llega manda) y sus
//              variables de red (fiables); el servidor solo acepta lo que
//              manda el dueno de verdad. Los que entran tarde reciben todos
//              los objetos, su ultima posicion y sus variables.
// Salidas      si un jugador se va, sus objetos desaparecen en todos.
// Netcode      cada posicion lleva la hora del dueno (interpolacion con
//              bufer en los demas, como Source/Overwatch) y el giro va
//              comprimido (smallest three, 4 bytes). El servidor solo reenvia
//              a cada jugador lo que tiene cerca (relevancia), rechaza
//              movimientos imposibles (velocidad maxima: corrige al dueno) y
//              guarda un segundo y medio de historia para la compensacion de
//              lag (positionAt). CVars net.SimLatencyMs / net.SimJitterMs /
//              net.SimLossPercent simulan una red mala para probar.
//
// La sesion no sabe nada del ECS: da eventos (takeEvents) y el sistema de
// scripts crea, mueve y borra las entidades.

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cramion::net {

// Un valor de Lua que viaja por la red.
struct NetValue {
    enum class Type : std::uint8_t { Nil = 0, Bool = 1, Number = 2, String = 3, Vec3 = 4, Table = 5 };
    Type type = Type::Nil;
    bool flag = false;
    double number = 0.0;
    std::string text;
    core::Vec3 vector{};
    std::vector<std::pair<NetValue, NetValue>> entries;  // tabla: clave -> valor

    // Fuera de linea (en Network.cpp): el par de la tabla necesita el tipo
    // completo y libstdc++ lo comprueba al generar los implicitos aqui dentro.
    NetValue();
    NetValue(const NetValue& other);
    NetValue(NetValue&& other) noexcept;
    NetValue& operator=(const NetValue& other);
    NetValue& operator=(NetValue&& other) noexcept;
    ~NetValue();

    static NetValue boolean(bool b);
    static NetValue num(double n);
    static NetValue str(std::string s);
    static NetValue vec3(const core::Vec3& v);
    bool operator==(const NetValue& other) const;
};

// Binario compacto (tambien lo usan las pruebas).
void encodeValue(std::string& out, const NetValue& value);
bool decodeValue(const std::string& in, std::size_t& at, NetValue& out, int depth = 0);

inline constexpr std::uint32_t kServerId = 1;   // el servidor es el jugador 1
inline constexpr std::uint32_t kEveryone = 0;   // destino: todos (menos quien lo manda)
inline constexpr std::uint16_t kDefaultPort = 7777;
inline constexpr int kConnectTimeoutSeconds = 6;  // conectar: si no responde, Disconnected

enum class NetRole { None, Server, Client };

struct NetEvent {
    enum class Type {
        Connected,     // cliente: dentro (peer = mi id)
        Disconnected,  // cliente: fuera (text = motivo); servidor: cerrado
        PlayerJoined,  // peer = el que entra
        PlayerLeft,    // peer = el que sale
        Message,       // peer = quien lo manda, text = nombre, value
        Spawn,         // net_id, owner, text = prefab, position, rotation
        Despawn,       // net_id
        Transform,     // net_id, position, rotation
        Var,           // net_id, text = clave, value
        Scene,         // text = escena que cargan todos
        Correction,    // net_id, position, rotation: el servidor corrige lo mio
        Voice,         // peer = quien habla, text = 20 ms de voz (mu-law)
    };
    Type type = Type::Message;
    std::uint32_t peer = 0;
    std::uint32_t net_id = 0;
    std::uint32_t owner = 0;
    std::string text;
    NetValue value;
    core::Vec3 position{};
    core::Quat rotation{};
    double time = 0.0;  // Transform: hora del dueno (s) cuando la mando
};

// Objeto replicado (lo que sabe la sesion de el).
struct NetObject {
    std::uint32_t owner = kServerId;
    std::string prefab;
    core::Vec3 position{};
    core::Quat rotation{};
    std::map<std::string, NetValue> vars;
    float relevance = 0.0f;  // servidor: solo a jugadores a menos de esto (0 = a todos)
    float max_speed = 0.0f;  // servidor: m/s maximos del dueno (0 = sin comprobar)
    double last_time = -1.0; // servidor: hora local del ultimo movimiento aceptado
};

class NetworkSession {
public:
    NetworkSession();
    ~NetworkSession();
    NetworkSession(const NetworkSession&) = delete;
    NetworkSession& operator=(const NetworkSession&) = delete;

    // Servidor en `port` para `max_players` (contandose a si mismo).
    bool host(std::uint16_t port, int max_players, std::string* error = nullptr);
    // Cliente: conecta en segundo plano (llega Connected o Disconnected).
    bool connect(const std::string& address, std::uint16_t port, std::string* error = nullptr);
    // Cierra (avisando a los demas). Tambien lo hace el destructor.
    void close();

    // Recibe y manda lo pendiente. Una vez por frame.
    void update();
    // Eventos desde la ultima llamada.
    std::vector<NetEvent> takeEvents();

    NetRole role() const { return role_; }
    bool isServer() const { return role_ == NetRole::Server; }
    bool isClient() const { return role_ == NetRole::Client; }
    // Servidor abierto o cliente ya dentro (con id).
    bool connected() const;
    bool connecting() const { return role_ == NetRole::Client && local_id_ == 0; }
    std::uint32_t localId() const { return local_id_; }
    // Ids de todos los jugadores, incluido el local.
    std::vector<std::uint32_t> players() const;
    // Ida y vuelta en ms con ese jugador (el cliente solo conoce la del servidor).
    int ping(std::uint32_t player) const;

    // Mensaje a `target` (kEveryone = todos menos yo, kServerId = el servidor,
    // u otro id). Un cliente lo manda al servidor y este lo reenvia.
    void send(const std::string& name, const NetValue& value, std::uint32_t target = kEveryone);

    // --- Objetos (el spawn y el despawn, solo el servidor) ---
    // Devuelve el id de red (0 si no es servidor). No genera evento local: el
    // servidor ya crea la entidad.
    std::uint32_t spawn(const std::string& prefab, const core::Vec3& position, const core::Quat& rotation,
                        std::uint32_t owner = kServerId);
    void despawn(std::uint32_t net_id);
    // Posicion y giro (solo el dueno; sin fiabilidad).
    void sendTransform(std::uint32_t net_id, const core::Vec3& position, const core::Quat& rotation);
    // Variable de red (solo el dueno o el servidor; fiable).
    void setVar(std::uint32_t net_id, const std::string& key, const NetValue& value);
    // Todos cargan una escena (solo el servidor): los objetos se olvidan.
    void loadScene(const std::string& scene);

    const std::map<std::uint32_t, NetObject>& objects() const { return objects_; }
    const NetObject* object(std::uint32_t net_id) const;
    // Soy yo quien manda en este objeto.
    bool owns(std::uint32_t net_id) const;

    // Chat de voz: un trozo (VoiceChat) a todos (sin fiabilidad).
    void sendVoice(const std::string& frame);

    // --- Netcode ---
    // Hora de la sesion (s desde que se abrio).
    double time() const;
    // Relevancia y velocidad maxima de un objeto (las usa el servidor).
    void setRelevance(std::uint32_t net_id, float meters);
    void setMaxSpeed(std::uint32_t net_id, float meters_per_second);
    // Compensacion de lag: donde estaba el objeto hace `seconds_ago` (en
    // este equipo; el servidor lo usa con el ping del que dispara).
    bool positionAt(std::uint32_t net_id, float seconds_ago, core::Vec3& position, core::Quat& rotation) const;
    // Perdida de paquetes (0..1) con ese jugador.
    float packetLoss(std::uint32_t player) const;
    // Bytes por segundo (media del ultimo segundo).
    float sendRate() const { return send_rate_; }
    float receiveRate() const { return receive_rate_; }

    // Bytes enviados y recibidos desde que se abrio (estadisticas).
    std::uint64_t bytesSent() const { return bytes_sent_; }
    std::uint64_t bytesReceived() const { return bytes_received_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    NetRole role_ = NetRole::None;
    std::uint32_t local_id_ = 0;
    std::uint32_t next_net_id_ = 1;
    std::map<std::uint32_t, NetObject> objects_;
    std::vector<NetEvent> events_;
    std::uint64_t bytes_sent_ = 0;
    std::uint64_t bytes_received_ = 0;

    struct HistorySample {
        double time = 0.0;
        core::Vec3 position{};
        core::Quat rotation{};
    };
    std::map<std::uint32_t, std::vector<HistorySample>> history_;
    std::map<std::uint32_t, core::Vec3> focus_;  // servidor: donde esta cada jugador
    struct Delayed {
        double at = 0.0;
        std::uint32_t to = 0;
        bool broadcast = false;
        std::uint32_t except = 0;
        std::string data;
        bool reliable = false;
    };
    std::vector<Delayed> delayed_;
    float send_rate_ = 0.0f;
    float receive_rate_ = 0.0f;
    double rate_time_ = 0.0;
    std::uint64_t rate_sent_ = 0;
    std::uint64_t rate_received_ = 0;
    void record(std::uint32_t net_id, const core::Vec3& position, const core::Quat& rotation);
    void forwardTransform(const std::string& data, std::uint32_t from, std::uint32_t net_id);
    bool relevantTo(const NetObject& o, std::uint32_t player) const;
    void sendNow(std::uint32_t to, const std::string& data, bool reliable);
    void broadcastNow(const std::string& data, bool reliable, std::uint32_t except);

    void handlePacket(std::uint32_t from, const std::string& data);
    void sendRaw(std::uint32_t to, const std::string& data, bool reliable);
    void broadcastRaw(const std::string& data, bool reliable, std::uint32_t except = 0);
    void despawnOwnedBy(std::uint32_t player);
};

}  // namespace cramion::net

#endif  // CRAMION_CORE_NET_NETWORK_H
