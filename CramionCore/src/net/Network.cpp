// Multijugador sobre ENet (ver Network.h).

#include "CramionCore/net/Network.h"

#include <enet/enet.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <set>

namespace cramion::net {

namespace {

// ENet se inicia una vez aunque haya varias sesiones (pruebas, editor).
std::mutex g_enet_mutex;
int g_enet_users = 0;

bool acquireEnet() {
    std::lock_guard lock(g_enet_mutex);
    if (g_enet_users == 0 && enet_initialize() != 0) return false;
    ++g_enet_users;
    return true;
}

void releaseEnet() {
    std::lock_guard lock(g_enet_mutex);
    if (g_enet_users > 0 && --g_enet_users == 0) enet_deinitialize();
}

// Tipos de paquete.
enum Msg : std::uint8_t {
    kWelcome = 1,    // servidor -> nuevo: tu id y los jugadores
    kJoin = 2,       // servidor -> todos: entra un jugador
    kLeave = 3,      // servidor -> todos: sale un jugador
    kUser = 4,       // mensaje de Lua (from, target, nombre, valor)
    kSpawn = 5,      // objeto nuevo
    kDespawn = 6,    // objeto borrado
    kTransform = 7,  // posicion y giro
    kVar = 8,        // variable de red
    kScene = 9,      // cargar escena
};

constexpr std::uint8_t kChannelReliable = 0;
constexpr std::uint8_t kChannelFast = 1;
constexpr int kMaxDepth = 16;

// --- Escritura y lectura binaria (little endian, como x64) ---
void put8(std::string& out, std::uint8_t v) { out.push_back(static_cast<char>(v)); }
void put32(std::string& out, std::uint32_t v) {
    char b[4];
    std::memcpy(b, &v, 4);
    out.append(b, 4);
}
void putF(std::string& out, float v) {
    char b[4];
    std::memcpy(b, &v, 4);
    out.append(b, 4);
}
void putD(std::string& out, double v) {
    char b[8];
    std::memcpy(b, &v, 8);
    out.append(b, 8);
}
void putStr(std::string& out, const std::string& s) {
    put32(out, static_cast<std::uint32_t>(s.size()));
    out += s;
}
void putVec3(std::string& out, const core::Vec3& v) {
    putF(out, v.x);
    putF(out, v.y);
    putF(out, v.z);
}
void putQuat(std::string& out, const core::Quat& q) {
    putF(out, q.x);
    putF(out, q.y);
    putF(out, q.z);
    putF(out, q.w);
}

struct Reader {
    const std::string& data;
    std::size_t at = 0;
    bool ok = true;

    bool need(std::size_t n) {
        if (!ok || at + n > data.size()) ok = false;
        return ok;
    }
    std::uint8_t u8() {
        if (!need(1)) return 0;
        return static_cast<std::uint8_t>(data[at++]);
    }
    std::uint32_t u32() {
        std::uint32_t v = 0;
        if (need(4)) {
            std::memcpy(&v, data.data() + at, 4);
            at += 4;
        }
        return v;
    }
    float f32() {
        float v = 0.0f;
        if (need(4)) {
            std::memcpy(&v, data.data() + at, 4);
            at += 4;
        }
        return v;
    }
    double f64() {
        double v = 0.0;
        if (need(8)) {
            std::memcpy(&v, data.data() + at, 8);
            at += 8;
        }
        return v;
    }
    std::string str() {
        const std::uint32_t n = u32();
        if (!need(n)) return {};
        std::string s = data.substr(at, n);
        at += n;
        return s;
    }
    core::Vec3 vec3() {
        core::Vec3 v;
        v.x = f32();
        v.y = f32();
        v.z = f32();
        return v;
    }
    core::Quat quat() {
        core::Quat q;
        q.x = f32();
        q.y = f32();
        q.z = f32();
        q.w = f32();
        return q;
    }
};

// Windows: un UDP a un puerto cerrado (o a un cliente que se cayo) devuelve un
// ICMP que Winsock convierte en error del socket (WSAECONNRESET); ENet corta
// entonces cada vuelta antes de contar los tiempos de espera y la conexion no
// caduca nunca. Se desactiva ese aviso en el socket.
void ignoreConnectionReset(ENetHost* host) {
#ifdef _WIN32
    constexpr DWORD kSioUdpConnReset = 0x9800000C;  // SIO_UDP_CONNRESET
    BOOL report = FALSE;
    DWORD bytes = 0;
    WSAIoctl(static_cast<SOCKET>(host->socket), kSioUdpConnReset, &report, sizeof(report), nullptr, 0, &bytes, nullptr, nullptr);
#else
    (void)host;
#endif
}

std::uint32_t peerId(const ENetPeer* peer) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(peer->data));
}

}  // namespace

// --- Valores -----------------------------------------------------------------------

NetValue NetValue::boolean(bool b) {
    NetValue v;
    v.type = Type::Bool;
    v.flag = b;
    return v;
}
NetValue NetValue::num(double n) {
    NetValue v;
    v.type = Type::Number;
    v.number = n;
    return v;
}
NetValue NetValue::str(std::string s) {
    NetValue v;
    v.type = Type::String;
    v.text = std::move(s);
    return v;
}
NetValue NetValue::vec3(const core::Vec3& value) {
    NetValue v;
    v.type = Type::Vec3;
    v.vector = value;
    return v;
}

bool NetValue::operator==(const NetValue& o) const {
    if (type != o.type) return false;
    switch (type) {
        case Type::Nil: return true;
        case Type::Bool: return flag == o.flag;
        case Type::Number: return number == o.number;
        case Type::String: return text == o.text;
        case Type::Vec3: return vector.x == o.vector.x && vector.y == o.vector.y && vector.z == o.vector.z;
        case Type::Table: return entries == o.entries;
    }
    return false;
}

void encodeValue(std::string& out, const NetValue& value) {
    put8(out, static_cast<std::uint8_t>(value.type));
    switch (value.type) {
        case NetValue::Type::Nil: break;
        case NetValue::Type::Bool: put8(out, value.flag ? 1 : 0); break;
        case NetValue::Type::Number: putD(out, value.number); break;
        case NetValue::Type::String: putStr(out, value.text); break;
        case NetValue::Type::Vec3: putVec3(out, value.vector); break;
        case NetValue::Type::Table:
            put32(out, static_cast<std::uint32_t>(value.entries.size()));
            for (const auto& [k, v] : value.entries) {
                encodeValue(out, k);
                encodeValue(out, v);
            }
            break;
    }
}

bool decodeValue(const std::string& in, std::size_t& at, NetValue& out, int depth) {
    if (depth > kMaxDepth) return false;
    Reader r{in, at};
    const std::uint8_t tag = r.u8();
    if (!r.ok || tag > static_cast<std::uint8_t>(NetValue::Type::Table)) return false;
    out = NetValue{};
    out.type = static_cast<NetValue::Type>(tag);
    switch (out.type) {
        case NetValue::Type::Nil: break;
        case NetValue::Type::Bool: out.flag = r.u8() != 0; break;
        case NetValue::Type::Number: out.number = r.f64(); break;
        case NetValue::Type::String: out.text = r.str(); break;
        case NetValue::Type::Vec3: out.vector = r.vec3(); break;
        case NetValue::Type::Table: {
            const std::uint32_t count = r.u32();
            // Cada entrada ocupa al menos 2 bytes: no se reserva de mas.
            if (!r.ok || count > (in.size() - r.at) / 2) return false;
            at = r.at;
            out.entries.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                NetValue k, v;
                if (!decodeValue(in, at, k, depth + 1) || !decodeValue(in, at, v, depth + 1)) return false;
                out.entries.emplace_back(std::move(k), std::move(v));
            }
            return true;
        }
    }
    if (!r.ok) return false;
    at = r.at;
    return true;
}

// --- Sesion ------------------------------------------------------------------------

struct NetworkSession::Impl {
    ENetHost* host = nullptr;
    ENetPeer* server = nullptr;                   // cliente: el servidor
    std::map<std::uint32_t, ENetPeer*> peers;     // servidor: los clientes por id
    std::set<std::uint32_t> players;              // cliente: los jugadores que conoce
    std::uint32_t next_player = kServerId + 1;
    bool enet = false;
    // Cliente: cuando empezo a conectar (ENet tarda ~15 s en rendirse solo).
    std::chrono::steady_clock::time_point connect_start{};
};

NetworkSession::NetworkSession() : impl_(std::make_unique<Impl>()) {}

NetworkSession::~NetworkSession() { close(); }

bool NetworkSession::host(std::uint16_t port, int max_players, std::string* error) {
    close();
    if (!acquireEnet()) {
        if (error) *error = "no se pudo iniciar la red (ENet)";
        return false;
    }
    impl_->enet = true;
    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;
    const std::size_t clients = static_cast<std::size_t>(std::clamp(max_players - 1, 1, 4000));
    impl_->host = enet_host_create(&address, clients, 2, 0, 0);
    if (impl_->host == nullptr) {
        if (error) *error = "no se pudo abrir el puerto " + std::to_string(port) + " (¿ya esta en uso?)";
        close();
        return false;
    }
    ignoreConnectionReset(impl_->host);
    role_ = NetRole::Server;
    local_id_ = kServerId;
    impl_->next_player = kServerId + 1;
    next_net_id_ = 1;
    return true;
}

bool NetworkSession::connect(const std::string& address_text, std::uint16_t port, std::string* error) {
    close();
    if (!acquireEnet()) {
        if (error) *error = "no se pudo iniciar la red (ENet)";
        return false;
    }
    impl_->enet = true;
    impl_->host = enet_host_create(nullptr, 1, 2, 0, 0);
    if (impl_->host == nullptr) {
        if (error) *error = "no se pudo crear el cliente";
        close();
        return false;
    }
    ignoreConnectionReset(impl_->host);
    ENetAddress address{};
    if (enet_address_set_host(&address, address_text.empty() ? "127.0.0.1" : address_text.c_str()) != 0) {
        if (error) *error = "direccion no valida: " + address_text;
        close();
        return false;
    }
    address.port = port;
    impl_->server = enet_host_connect(impl_->host, &address, 2, 0);
    if (impl_->server == nullptr) {
        if (error) *error = "no se pudo conectar";
        close();
        return false;
    }
    enet_peer_timeout(impl_->server, 32, 5000, 10000);
    impl_->connect_start = std::chrono::steady_clock::now();
    role_ = NetRole::Client;
    local_id_ = 0;
    return true;
}

void NetworkSession::close() {
    if (impl_->host != nullptr) {
        // Avisar a los demas y dar un momento a que salga el aviso.
        if (role_ == NetRole::Client && impl_->server != nullptr) {
            enet_peer_disconnect(impl_->server, 0);
        } else {
            for (auto& [id, peer] : impl_->peers) enet_peer_disconnect(peer, 0);
        }
        enet_host_flush(impl_->host);
        ENetEvent event;
        for (int i = 0; i < 10 && enet_host_service(impl_->host, &event, 10) > 0;) {
            if (event.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(event.packet);
            if (event.type == ENET_EVENT_TYPE_DISCONNECT) ++i;
        }
        enet_host_destroy(impl_->host);
        impl_->host = nullptr;
    }
    if (impl_->enet) {
        releaseEnet();
        impl_->enet = false;
    }
    impl_->server = nullptr;
    impl_->peers.clear();
    impl_->players.clear();
    role_ = NetRole::None;
    local_id_ = 0;
    objects_.clear();
}

bool NetworkSession::connected() const {
    return role_ == NetRole::Server || (role_ == NetRole::Client && local_id_ != 0);
}

std::vector<std::uint32_t> NetworkSession::players() const {
    std::vector<std::uint32_t> out;
    if (role_ == NetRole::Server) {
        out.push_back(kServerId);
        for (const auto& [id, peer] : impl_->peers) out.push_back(id);
    } else if (role_ == NetRole::Client) {
        out.assign(impl_->players.begin(), impl_->players.end());
    }
    return out;
}

int NetworkSession::ping(std::uint32_t player) const {
    if (role_ == NetRole::Client) {
        return impl_->server != nullptr && player == kServerId ? static_cast<int>(impl_->server->roundTripTime) : 0;
    }
    const auto it = impl_->peers.find(player);
    return it != impl_->peers.end() ? static_cast<int>(it->second->roundTripTime) : 0;
}

const NetObject* NetworkSession::object(std::uint32_t net_id) const {
    const auto it = objects_.find(net_id);
    return it != objects_.end() ? &it->second : nullptr;
}

bool NetworkSession::owns(std::uint32_t net_id) const {
    const NetObject* o = object(net_id);
    return o != nullptr && local_id_ != 0 && o->owner == local_id_;
}

std::vector<NetEvent> NetworkSession::takeEvents() {
    std::vector<NetEvent> out;
    out.swap(events_);
    return out;
}

void NetworkSession::sendRaw(std::uint32_t to, const std::string& data, bool reliable) {
    ENetPeer* peer = nullptr;
    if (role_ == NetRole::Client) {
        peer = impl_->server;
    } else if (const auto it = impl_->peers.find(to); it != impl_->peers.end()) {
        peer = it->second;
    }
    if (peer == nullptr) return;
    ENetPacket* packet = enet_packet_create(data.data(), data.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    enet_peer_send(peer, reliable ? kChannelReliable : kChannelFast, packet);
}

void NetworkSession::broadcastRaw(const std::string& data, bool reliable, std::uint32_t except) {
    if (role_ != NetRole::Server) return;
    for (const auto& [id, peer] : impl_->peers) {
        if (id == except) continue;
        ENetPacket* packet = enet_packet_create(data.data(), data.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
        enet_peer_send(peer, reliable ? kChannelReliable : kChannelFast, packet);
    }
}

void NetworkSession::send(const std::string& name, const NetValue& value, std::uint32_t target) {
    if (!connected()) return;
    std::string data;
    put8(data, kUser);
    put32(data, local_id_);
    put32(data, target);
    putStr(data, name);
    encodeValue(data, value);
    if (role_ == NetRole::Client) {
        if (target != local_id_) sendRaw(kServerId, data, true);
    } else if (target == kEveryone) {
        broadcastRaw(data, true);
    } else if (target != kServerId) {
        sendRaw(target, data, true);
    }
}

std::uint32_t NetworkSession::spawn(const std::string& prefab, const core::Vec3& position, const core::Quat& rotation,
                                    std::uint32_t owner) {
    if (role_ != NetRole::Server) return 0;
    const std::uint32_t id = next_net_id_++;
    NetObject& o = objects_[id];
    o.owner = owner == 0 ? kServerId : owner;
    o.prefab = prefab;
    o.position = position;
    o.rotation = rotation;
    std::string data;
    put8(data, kSpawn);
    put32(data, id);
    put32(data, o.owner);
    putStr(data, prefab);
    putVec3(data, position);
    putQuat(data, rotation);
    broadcastRaw(data, true);
    return id;
}

void NetworkSession::despawn(std::uint32_t net_id) {
    if (role_ != NetRole::Server || objects_.erase(net_id) == 0) return;
    std::string data;
    put8(data, kDespawn);
    put32(data, net_id);
    broadcastRaw(data, true);
}

void NetworkSession::sendTransform(std::uint32_t net_id, const core::Vec3& position, const core::Quat& rotation) {
    const auto it = objects_.find(net_id);
    if (it == objects_.end() || !connected()) return;
    // Solo el dueno (o el servidor, que manda en todo lo que es suyo).
    if (it->second.owner != local_id_) return;
    it->second.position = position;
    it->second.rotation = rotation;
    std::string data;
    put8(data, kTransform);
    put32(data, net_id);
    putVec3(data, position);
    putQuat(data, rotation);
    if (role_ == NetRole::Client) {
        sendRaw(kServerId, data, false);
    } else {
        broadcastRaw(data, false);
    }
}

void NetworkSession::setVar(std::uint32_t net_id, const std::string& key, const NetValue& value) {
    const auto it = objects_.find(net_id);
    if (it == objects_.end() || !connected()) return;
    if (it->second.owner != local_id_ && role_ != NetRole::Server) return;
    it->second.vars[key] = value;
    std::string data;
    put8(data, kVar);
    put32(data, net_id);
    putStr(data, key);
    encodeValue(data, value);
    if (role_ == NetRole::Client) {
        sendRaw(kServerId, data, true);
    } else {
        broadcastRaw(data, true);
    }
}

void NetworkSession::loadScene(const std::string& scene) {
    if (role_ != NetRole::Server) return;
    objects_.clear();
    std::string data;
    put8(data, kScene);
    putStr(data, scene);
    broadcastRaw(data, true);
}

void NetworkSession::despawnOwnedBy(std::uint32_t player) {
    std::vector<std::uint32_t> gone;
    for (const auto& [id, o] : objects_) {
        if (o.owner == player) gone.push_back(id);
    }
    for (const std::uint32_t id : gone) {
        despawn(id);
        NetEvent e;
        e.type = NetEvent::Type::Despawn;
        e.net_id = id;
        events_.push_back(std::move(e));
    }
}

void NetworkSession::update() {
    if (impl_->host == nullptr) return;
    // Sin respuesta del servidor en unos segundos: no hay nadie ahi.
    if (connecting() && std::chrono::steady_clock::now() - impl_->connect_start > std::chrono::seconds(kConnectTimeoutSeconds)) {
        close();
        NetEvent e;
        e.type = NetEvent::Type::Disconnected;
        e.text = "no se pudo conectar con el servidor (no responde)";
        events_.push_back(std::move(e));
        return;
    }
    ENetEvent event;
    while (impl_->host != nullptr && enet_host_service(impl_->host, &event, 0) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                if (role_ != NetRole::Server) break;  // el cliente espera su Welcome
                const std::uint32_t id = impl_->next_player++;
                event.peer->data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
                enet_peer_timeout(event.peer, 32, 5000, 10000);
                impl_->peers[id] = event.peer;
                // Al nuevo: su id y quien esta.
                std::string welcome;
                put8(welcome, kWelcome);
                put32(welcome, id);
                const std::vector<std::uint32_t> list = players();
                put32(welcome, static_cast<std::uint32_t>(list.size()));
                for (const std::uint32_t p : list) put32(welcome, p);
                sendRaw(id, welcome, true);
                // A los demas: entra uno.
                std::string join;
                put8(join, kJoin);
                put32(join, id);
                broadcastRaw(join, true, id);
                // Lo que ya existe.
                for (const auto& [net_id, o] : objects_) {
                    std::string spawn_data;
                    put8(spawn_data, kSpawn);
                    put32(spawn_data, net_id);
                    put32(spawn_data, o.owner);
                    putStr(spawn_data, o.prefab);
                    putVec3(spawn_data, o.position);
                    putQuat(spawn_data, o.rotation);
                    sendRaw(id, spawn_data, true);
                    for (const auto& [key, value] : o.vars) {
                        std::string var;
                        put8(var, kVar);
                        put32(var, net_id);
                        putStr(var, key);
                        encodeValue(var, value);
                        sendRaw(id, var, true);
                    }
                }
                NetEvent e;
                e.type = NetEvent::Type::PlayerJoined;
                e.peer = id;
                events_.push_back(std::move(e));
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                const std::string data(reinterpret_cast<const char*>(event.packet->data), event.packet->dataLength);
                const std::uint32_t from = role_ == NetRole::Server ? peerId(event.peer) : kServerId;
                enet_packet_destroy(event.packet);
                handlePacket(from, data);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT: {
                if (role_ == NetRole::Server) {
                    const std::uint32_t id = peerId(event.peer);
                    impl_->peers.erase(id);
                    despawnOwnedBy(id);
                    std::string leave;
                    put8(leave, kLeave);
                    put32(leave, id);
                    broadcastRaw(leave, true);
                    NetEvent e;
                    e.type = NetEvent::Type::PlayerLeft;
                    e.peer = id;
                    events_.push_back(std::move(e));
                } else {
                    NetEvent e;
                    e.type = NetEvent::Type::Disconnected;
                    e.text = local_id_ == 0 ? "no se pudo conectar con el servidor" : "se perdio la conexion con el servidor";
                    impl_->server = nullptr;
                    events_.push_back(std::move(e));
                    // Cerrar sin perder el evento.
                    std::vector<NetEvent> keep;
                    keep.swap(events_);
                    close();
                    events_ = std::move(keep);
                    return;
                }
                break;
            }
            default:
                break;
        }
    }
    if (impl_->host != nullptr) {
        enet_host_flush(impl_->host);
        bytes_sent_ = impl_->host->totalSentData;
        bytes_received_ = impl_->host->totalReceivedData;
    }
}

void NetworkSession::handlePacket(std::uint32_t from, const std::string& data) {
    Reader r{data};
    const std::uint8_t kind = r.u8();
    const bool server = role_ == NetRole::Server;
    switch (kind) {
        case kWelcome: {
            if (server) return;
            local_id_ = r.u32();
            const std::uint32_t count = r.u32();
            impl_->players.clear();
            for (std::uint32_t i = 0; i < count && r.ok; ++i) impl_->players.insert(r.u32());
            impl_->players.insert(local_id_);
            NetEvent e;
            e.type = NetEvent::Type::Connected;
            e.peer = local_id_;
            events_.push_back(std::move(e));
            return;
        }
        case kJoin:
        case kLeave: {
            if (server) return;
            const std::uint32_t id = r.u32();
            if (kind == kJoin) impl_->players.insert(id);
            else impl_->players.erase(id);
            NetEvent e;
            e.type = kind == kJoin ? NetEvent::Type::PlayerJoined : NetEvent::Type::PlayerLeft;
            e.peer = id;
            events_.push_back(std::move(e));
            return;
        }
        case kUser: {
            std::uint32_t sender = r.u32();
            const std::uint32_t target = r.u32();
            NetEvent e;
            e.type = NetEvent::Type::Message;
            e.text = r.str();
            std::size_t at = r.at;
            if (!r.ok || !decodeValue(data, at, e.value)) return;
            if (server) {
                sender = from;  // el cliente no puede hacerse pasar por otro
                // Reenviar con el remitente de verdad.
                std::string relay;
                put8(relay, kUser);
                put32(relay, sender);
                put32(relay, target);
                putStr(relay, e.text);
                encodeValue(relay, e.value);
                if (target == kEveryone) {
                    broadcastRaw(relay, true, sender);
                } else if (target != kServerId) {
                    if (target != sender) sendRaw(target, relay, true);
                    return;  // no era para el servidor
                }
            }
            e.peer = sender;
            events_.push_back(std::move(e));
            return;
        }
        case kSpawn: {
            if (server) return;
            NetEvent e;
            e.type = NetEvent::Type::Spawn;
            e.net_id = r.u32();
            e.owner = r.u32();
            e.text = r.str();
            e.position = r.vec3();
            e.rotation = r.quat();
            if (!r.ok) return;
            NetObject& o = objects_[e.net_id];
            o.owner = e.owner;
            o.prefab = e.text;
            o.position = e.position;
            o.rotation = e.rotation;
            events_.push_back(std::move(e));
            return;
        }
        case kDespawn: {
            if (server) return;
            NetEvent e;
            e.type = NetEvent::Type::Despawn;
            e.net_id = r.u32();
            if (!r.ok) return;
            objects_.erase(e.net_id);
            events_.push_back(std::move(e));
            return;
        }
        case kTransform: {
            NetEvent e;
            e.type = NetEvent::Type::Transform;
            e.net_id = r.u32();
            e.position = r.vec3();
            e.rotation = r.quat();
            if (!r.ok) return;
            const auto it = objects_.find(e.net_id);
            if (it == objects_.end()) return;
            // El servidor solo acepta la posicion del dueno.
            if (server && it->second.owner != from) return;
            if (it->second.owner == local_id_) return;  // lo mio lo muevo yo
            it->second.position = e.position;
            it->second.rotation = e.rotation;
            if (server) broadcastRaw(data, false, from);
            events_.push_back(std::move(e));
            return;
        }
        case kVar: {
            NetEvent e;
            e.type = NetEvent::Type::Var;
            e.net_id = r.u32();
            e.text = r.str();
            std::size_t at = r.at;
            if (!r.ok || !decodeValue(data, at, e.value)) return;
            const auto it = objects_.find(e.net_id);
            if (it == objects_.end()) return;
            if (server && it->second.owner != from) return;
            it->second.vars[e.text] = e.value;
            if (server) broadcastRaw(data, true, from);
            events_.push_back(std::move(e));
            return;
        }
        case kScene: {
            if (server) return;
            NetEvent e;
            e.type = NetEvent::Type::Scene;
            e.text = r.str();
            if (!r.ok) return;
            objects_.clear();
            events_.push_back(std::move(e));
            return;
        }
        default:
            return;
    }
}

}  // namespace cramion::net
