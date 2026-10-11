// Network (sesion, mensajes, objetos replicados) y lo de red de las entidades.
//
// La sesion (rt.network) y sus objetos (rt.net_entities) son del runtime y
// siguen vivos tras stop()/start() (un cambio de escena no desconecta). Este
// modulo lee lo que llega al principio del frame (fase Begin, antes que
// nada), manda lo mio que se movio al final (fase End) y da la API.
//
// Los valores viajan como net::NetValue: nil, bool, numero, texto, Vec3 y
// tablas de ellos. Una lista sale como tabla con claves 1..n (como las
// tablas de Lua) y vuelve como lista; un objeto, con sus claves de texto.
// Una entidad viaja como su id de red (0 si no es de red).

#include "Modules.h"

#include "CramionCore/audio/VoiceChat.h"
#include "CramionCore/cvar/CVar.h"
#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/ecs/Prefab.h"
#include "CramionCore/net/NetworkObject.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace cramion::scripting::native {
namespace {

using core::Quat;
using core::Vec3;
using net::NetValue;

struct NetState {
    // Network.on("nombre", fn) y los avisos de la sesion (son de los scripts:
    // se olvidan al parar; la sesion sigue).
    std::unordered_map<std::string, api::Value> handlers;
    api::Value on_player_joined, on_player_left, on_connected, on_disconnected;
    // Network.dedicated = true (CramionServer, servidor dedicado sin ventana).
    bool dedicated = false;
};

// --- Valores ------------------------------------------------------------------------

net::NetworkObject* netObject(const ecs::Entity& e) {
    return e.valid() ? e.tryGet<net::NetworkObject>() : nullptr;
}

NetValue toNet(const Runtime& rt, const api::Value& v, int depth = 0) {
    switch (v.type()) {
        case api::Value::Type::Bool: return NetValue::boolean(v.truthy());
        case api::Value::Type::Number: return NetValue::num(v.asNumber());
        case api::Value::Type::String: return NetValue::str(v.asString());
        case api::Value::Type::Vec3: return NetValue::vec3(v.asVec3());
        case api::Value::Type::Entity: {
            // Una entidad viaja como su id de red (0 si no es de red).
            const net::NetworkObject* n = netObject(rt.entity(v.asEntity()));
            return NetValue::num(n != nullptr ? n->net_id : 0);
        }
        case api::Value::Type::Array: {
            NetValue t;
            t.type = NetValue::Type::Table;
            if (depth > 12) return t;
            const api::Value::Array& items = v.items();
            for (std::size_t i = 0; i < items.size(); ++i) {
                t.entries.emplace_back(NetValue::num(static_cast<double>(i + 1)), toNet(rt, items[i], depth + 1));
            }
            return t;
        }
        case api::Value::Type::Object: {
            NetValue t;
            t.type = NetValue::Type::Table;
            if (depth > 12) return t;
            for (const auto& [key, item] : v.fields()) {
                if (item.isNil()) continue;  // como en una tabla de Lua: sin la clave
                t.entries.emplace_back(NetValue::str(key), toNet(rt, item, depth + 1));
            }
            return t;
        }
        default: return {};  // nil, Quat, funciones y objetos del motor
    }
}

std::string keyText(const NetValue& k) {
    if (k.type == NetValue::Type::String) return k.text;
    if (k.type == NetValue::Type::Number) {
        if (k.number == std::floor(k.number) && std::abs(k.number) < 9.0e15) {
            return std::to_string(static_cast<long long>(k.number));
        }
        return api::Value(k.number).asString();
    }
    return {};
}

api::Value fromNet(const NetValue& v, int depth = 0) {
    switch (v.type) {
        case NetValue::Type::Nil: return {};
        case NetValue::Type::Bool: return api::Value(v.flag);
        case NetValue::Type::Number: return api::Value(v.number);
        case NetValue::Type::String: return api::Value(v.text);
        case NetValue::Type::Vec3: return api::Value(v.vector);
        case NetValue::Type::Table: {
            if (depth > 16) return api::Value::object();
            // Claves 1..n (en cualquier orden): una lista. El resto, un objeto.
            const std::size_t n = v.entries.size();
            std::vector<const NetValue*> slots(n, nullptr);
            bool sequence = n > 0;
            for (const auto& [k, value] : v.entries) {
                if (k.type != NetValue::Type::Number || k.number != std::floor(k.number) || k.number < 1.0 ||
                    k.number > static_cast<double>(n)) {
                    sequence = false;
                    break;
                }
                const std::size_t at = static_cast<std::size_t>(k.number) - 1;
                if (slots[at] != nullptr) {
                    sequence = false;
                    break;
                }
                slots[at] = &value;
            }
            if (sequence) {
                api::Value::Array list;
                list.reserve(n);
                for (const NetValue* item : slots) list.push_back(fromNet(*item, depth + 1));
                return api::Value(std::move(list));
            }
            api::Value o = api::Value::object();
            for (const auto& [k, value] : v.entries) {
                if (k.type != NetValue::Type::String && k.type != NetValue::Type::Number) continue;
                o.set(keyText(k), fromNet(value, depth + 1));
            }
            return o;
        }
    }
    return {};
}

// --- Objetos de red -------------------------------------------------------------------

net::NetworkSession& session(Runtime& rt) {
    if (!rt.network) rt.network = std::make_unique<net::NetworkSession>();
    return *rt.network;
}

// Al salir de la partida, los objetos de red desaparecen de la escena.
void destroyNetEntities(Runtime& rt) {
    for (const auto& [id, handle] : rt.net_entities) {
        if (rt.world != nullptr && rt.world->registry().valid(handle)) rt.destroyLater(handle);
    }
    rt.net_entities.clear();
}

ecs::Entity netEntity(const Runtime& rt, std::uint32_t net_id) {
    const auto it = rt.net_entities.find(net_id);
    if (it == rt.net_entities.end()) return {};
    return rt.entity(it->second);
}

// Prefab de red: la misma ruta en todos (dentro de Assets, con o sin .crprefab).
ecs::Entity instantiateNetPrefab(Runtime& rt, const std::string& prefab) {
    if (rt.world == nullptr) return {};
    std::filesystem::path file = rt.root / pathFromUtf8(prefab);
    if (file.extension() != ecs::kPrefabExtension) file += ecs::kPrefabExtension;
    const std::string text = rt.prefabText(file);
    if (text.empty()) {
        rt.write(2, "Network: no existe el prefab \"" + prefab + "\"");
        return {};
    }
    return ecs::instantiatePrefab(*rt.world, text);
}

ecs::Entity attachNet(Runtime& rt, ecs::Entity e, std::uint32_t net_id, std::uint32_t owner, const Vec3& position,
                      const Quat& rotation) {
    if (!e.valid()) return e;
    e.setWorldPosition(position);
    e.setLocalRotation(rotation);
    net::NetworkObject& n = e.has<net::NetworkObject>() ? e.get<net::NetworkObject>() : e.add<net::NetworkObject>();
    n.net_id = net_id;
    n.owner = owner;
    n.has_target = false;
    n.target_position = position;
    n.target_rotation = rotation;
    n.target_velocity = Vec3{};
    n.target_age = 0.0f;
    n.last_position = position;
    n.last_rotation = rotation;
    n.predicted = false;
    n.snapshots.clear();
    n.has_offset = false;
    // El servidor filtra y valida con los ajustes del componente.
    if (rt.network) {
        rt.network->setRelevance(net_id, n.relevance);
        rt.network->setMaxSpeed(net_id, n.max_speed);
    }
    // La copia de otro la mueve la red: su Rigidbody dinamico pasa a
    // cinematico (sigue la posicion recibida y empuja a los demas), salvo
    // con fisica local: sigue dinamico aqui (se puede empujar) y la red lo
    // corrige.
    if (rt.network && owner != rt.network->localId()) {
        if (physics::Rigidbody* rb = e.tryGet<physics::Rigidbody>(); rb != nullptr && rb->type == physics::BodyType::Dynamic) {
            if (n.local_physics) n.predicted = true;
            else rb->type = physics::BodyType::Kinematic;
        }
    }
    rt.net_entities[net_id] = e.handle();
    return e;
}

// Copia con fisica local: se simula aqui y se corrige hacia donde esta en
// el dueno (su ultima posicion, adelantada con su velocidad lo que tarda
// el siguiente envio). La correccion es mas suave que el seguimiento de
// los cinematicos para que un empujon local se note antes de que el
// dueno lo confirme; los errores pequenos no se tocan (reposo).
void followPredicted(Runtime& rt, ecs::Entity e, net::NetworkObject& n, float dt) {
    const Vec3 goal = n.target_position + n.target_velocity * std::min(n.target_age, 0.25f);
    const Vec3 current = e.worldPosition();
    const Vec3 error = goal - current;
    const float distance = core::length(error);
    if (distance > 4.0f) {
        e.setWorldPosition(goal);
        e.setLocalRotation(n.target_rotation);
        if (rt.physics != nullptr) rt.physics->setLinearVelocity(e, n.target_velocity);
        return;
    }
    const float t = 1.0f - std::exp(-std::max(n.smoothing, 0.1f) * 0.4f * dt);
    if (distance > 0.02f) e.setWorldPosition(current + error * t);
    e.setLocalRotation(core::slerp(e.localRotation(), n.target_rotation, t));
    if (rt.physics != nullptr) {
        const Vec3 v = rt.physics->linearVelocity(e);
        rt.physics->setLinearVelocity(e, v + (n.target_velocity - v) * t);
    }
}

// Lo que llego: avisos, mensajes, objetos nuevos, posiciones, variables...
void handleEvent(Runtime& rt, NetState& s, net::NetEvent& ev) {
    net::NetworkSession& network = *rt.network;
    switch (ev.type) {
        case net::NetEvent::Type::Connected: rt.native.invoke(s.on_connected, {api::Value(ev.peer)}); break;
        case net::NetEvent::Type::Disconnected:
            destroyNetEntities(rt);
            rt.native.invoke(s.on_disconnected, {api::Value(ev.text)});
            break;
        case net::NetEvent::Type::PlayerJoined: rt.native.invoke(s.on_player_joined, {api::Value(ev.peer)}); break;
        case net::NetEvent::Type::PlayerLeft: rt.native.invoke(s.on_player_left, {api::Value(ev.peer)}); break;
        case net::NetEvent::Type::Message: {
            const auto it = s.handlers.find(ev.text);
            if (it == s.handlers.end()) break;
            const api::Value fn = it->second;  // el callback puede cambiar los manejadores
            rt.native.invoke(fn, {fromNet(ev.value), api::Value(ev.peer)});
            break;
        }
        case net::NetEvent::Type::Spawn: {
            if (netEntity(rt, ev.net_id).valid()) break;
            const ecs::Entity e = instantiateNetPrefab(rt, ev.text);
            attachNet(rt, e, ev.net_id, ev.owner, ev.position, ev.rotation);
            break;
        }
        case net::NetEvent::Type::Despawn: {
            if (const ecs::Entity e = netEntity(rt, ev.net_id); e.valid()) rt.destroyLater(e.handle());
            rt.net_entities.erase(ev.net_id);
            break;
        }
        case net::NetEvent::Type::Transform: {
            const ecs::Entity e = netEntity(rt, ev.net_id);
            net::NetworkObject* n = netObject(e);
            if (n == nullptr) break;
            // Velocidad del dueno entre las dos ultimas posiciones (la
            // prediccion sigue rodando entre envios).
            if (n->has_target) {
                const float gap = std::max(n->target_age, 1.0f / 60.0f);
                Vec3 v = (ev.position - n->target_position) * (1.0f / gap);
                if (core::length(v) > 50.0f || n->target_age > 0.5f) v = Vec3{};
                n->target_velocity = v;
            }
            n->target_age = 0.0f;
            n->target_position = ev.position;
            n->target_rotation = ev.rotation;
            n->has_target = true;
            // Interpolacion con bufer: la hora del dueno pasada a la local (el
            // desfase minimo visto = sin el retraso de la red; deriva despacio
            // por si los relojes van distintos).
            const double now = network.time();
            const double offset = now - ev.time;
            if (!n->has_offset || offset < n->clock_offset) n->clock_offset = offset;
            else n->clock_offset += std::min(offset - n->clock_offset, 0.002);
            n->has_offset = true;
            const double local_time = ev.time + n->clock_offset;
            if (n->snapshots.empty() || local_time > n->snapshots.back().time) {
                n->snapshots.push_back(net::NetworkObject::Snapshot{local_time, ev.position, ev.rotation});
                if (n->snapshots.size() > 32) n->snapshots.erase(n->snapshots.begin());
            }
            break;
        }
        case net::NetEvent::Type::Voice: {
            if (audio::VoiceChat* v = audio::activeVoiceChat()) v->receive(ev.peer, ev.text);
            break;
        }
        case net::NetEvent::Type::Correction: {
            // El servidor rechazo un movimiento mio: vuelvo a donde dice.
            ecs::Entity e = netEntity(rt, ev.net_id);
            if (!e.valid()) break;
            e.setWorldPosition(ev.position);
            e.setLocalRotation(ev.rotation);
            if (rt.physics != nullptr) rt.physics->setLinearVelocity(e, Vec3{});
            if (net::NetworkObject* n = e.tryGet<net::NetworkObject>()) {
                n->last_position = ev.position;
                n->last_rotation = ev.rotation;
            }
            // Y a los scripts de C++ del objeto (on("OnNetCorrection", ...)).
            if (rt.message_listener) {
                rt.message_listener(e, "OnNetCorrection", rt.native.toJson(api::Value(ev.position)).dump());
            }
            break;
        }
        case net::NetEvent::Type::Var: {
            // Los scripts de C++ lo reciben como onNetVar(clave, valor).
            const ecs::Entity e = netEntity(rt, ev.net_id);
            if (e.valid() && rt.net_var_listener) {
                rt.net_var_listener(e, ev.text, rt.native.toJson(fromNet(ev.value)).dump());
            }
            break;
        }
        case net::NetEvent::Type::Scene:
            rt.net_entities.clear();
            rt.scene_request = rt.findScene(ev.text);
            if (rt.scene_request.empty()) rt.write(2, "Network.loadScene: no existe la escena \"" + ev.text + "\"");
            break;
    }
}

// Eventos de la red (lo primero del frame: los objetos que llegan ya estan
// cuando corren los scripts) y el seguimiento de los objetos de otros.
void pollNetwork(Runtime& rt, NetState& s, float dt) {
    if (!rt.network || rt.network->role() == net::NetRole::None) return;
    rt.network->update();
    for (net::NetEvent& ev : rt.network->takeEvents()) {
        if (!rt.network) return;
        handleEvent(rt, s, ev);
    }
    if (!rt.network || rt.world == nullptr) return;
    // Los objetos de otros siguen la ultima posicion recibida, suavizando
    // (y saltan si esta muy lejos: teletransporte o recien llegado).
    const std::uint32_t me = rt.network->localId();
    for (const auto& [id, handle] : rt.net_entities) {
        if (!rt.world->registry().valid(handle)) continue;
        ecs::Entity e = rt.world->wrap(handle);
        net::NetworkObject* n = e.tryGet<net::NetworkObject>();
        if (n == nullptr || !n->has_target || n->owner == me) continue;
        n->target_age += dt;
        if (n->predicted) {
            followPredicted(rt, e, *n, dt);
            continue;
        }
        if (n->interpolation == 1 && n->snapshots.size() >= 2) {
            // Interpolacion con bufer: el estado de hace `delay` segundos,
            // entre las dos posiciones recibidas que lo rodean.
            const double render_time = rt.network->time() - std::max(n->interpolation_delay, 0.0f);
            const auto& snaps = n->snapshots;
            std::size_t k = 1;
            while (k < snaps.size() && snaps[k].time < render_time) ++k;
            if (k >= snaps.size()) {
                // Sin datos tan nuevos: se queda en la ultima (o adelanta un poco).
                const auto& last = snaps.back();
                const float ahead = static_cast<float>(std::min(render_time - last.time, 0.1));
                e.setWorldPosition(last.position + n->target_velocity * std::max(ahead, 0.0f));
                e.setLocalRotation(last.rotation);
            } else {
                const auto& a = snaps[k - 1];
                const auto& b = snaps[k];
                const double span = b.time - a.time;
                const float u = span > 1e-6 ? static_cast<float>(std::clamp((render_time - a.time) / span, 0.0, 1.0)) : 1.0f;
                e.setWorldPosition(core::lerp(a.position, b.position, u));
                e.setLocalRotation(core::slerp(a.rotation, b.rotation, u));
                // Lo ya pasado sobra (deja uno para interpolar).
                if (k >= 2) n->snapshots.erase(n->snapshots.begin(), n->snapshots.begin() + static_cast<std::ptrdiff_t>(k - 1));
            }
            continue;
        }
        const Vec3 current = e.worldPosition();
        const Vec3 aim = n->interpolation == 2 ? n->target_position + n->target_velocity * std::min(n->target_age, 0.25f)
                                               : n->target_position;
        const Vec3 delta = aim - current;
        const float t = 1.0f - std::exp(-std::max(n->smoothing, 0.1f) * dt);
        if (core::length(delta) > 10.0f) {
            e.setWorldPosition(aim);
            e.setLocalRotation(n->target_rotation);
        } else {
            e.setWorldPosition(current + delta * t);
            e.setLocalRotation(core::slerp(e.localRotation(), n->target_rotation, t));
        }
    }
}

// Lo mio que se movio sale a la red (a su frecuencia).
void sendNetworkTransforms(Runtime& rt, float dt) {
    if (!rt.network || !rt.network->connected() || rt.world == nullptr) return;
    net::NetworkSession& network = *rt.network;
    const std::uint32_t me = network.localId();
    // Chat de voz: lo del microfono sale y cada jugador suena donde esta.
    if (audio::VoiceChat* v = audio::activeVoiceChat(); v != nullptr && v->running()) {
        for (const std::string& frame : v->takeOutgoing()) network.sendVoice(frame);
        for (const auto& [id, handle] : rt.net_entities) {
            if (!rt.world->registry().valid(handle)) continue;
            const ecs::Entity e = rt.world->wrap(handle);
            const net::NetworkObject* n = e.tryGet<net::NetworkObject>();
            if (n == nullptr) continue;
            if (n->owner == me) v->setListener(e.worldPosition());
            else v->setSpeakerPosition(n->owner, e.worldPosition());
        }
    }
    for (const auto& [id, handle] : rt.net_entities) {
        if (!rt.world->registry().valid(handle)) continue;
        ecs::Entity e = rt.world->wrap(handle);
        net::NetworkObject* n = e.tryGet<net::NetworkObject>();
        if (n == nullptr || !n->sync_transform || n->owner != me) continue;
        n->send_timer += dt;
        n->since_sent += dt;
        if (n->send_timer < 1.0f / std::max(n->send_rate, 1.0f)) continue;
        n->send_timer = 0.0f;
        const Vec3 p = e.worldPosition();
        const Quat q = e.localRotation();
        const float dq = std::abs(q.x * n->last_rotation.x + q.y * n->last_rotation.y + q.z * n->last_rotation.z +
                                  q.w * n->last_rotation.w);
        const bool moved = core::length(p - n->last_position) > 0.001f || dq < 0.99999f;
        // Quieto: uno por segundo igualmente (por si se perdio el ultimo).
        if (!moved && n->since_sent < 1.0f) continue;
        network.sendTransform(n->net_id, p, q);
        n->last_position = p;
        n->last_rotation = q;
        n->since_sent = 0.0f;
    }
}

api::Value idList(const std::vector<std::uint32_t>& ids) {
    api::Value::Array out;
    out.reserve(ids.size());
    for (const std::uint32_t id : ids) out.emplace_back(id);
    return api::Value(std::move(out));
}

}  // namespace

void registerNetworkApi(Runtime& rt) {
    auto state = std::make_shared<NetState>();

    // Lo primero del frame: lo que llego. Lo ultimo: lo mio que se movio.
    rt.onFrame(Phase::Begin, [&rt, state](float dt) { pollNetwork(rt, *state, dt); });
    rt.onFrame(Phase::End, [&rt](float dt) { sendNetworkTransforms(rt, dt); });
    // Los callbacks son de los scripts que se van (la sesion sigue abierta).
    rt.onStop([state](bool) {
        state->handlers.clear();
        state->on_player_joined = state->on_player_left = state->on_connected = state->on_disconnected = api::Value{};
    });

    api::NativeApi& n = rt.native;
    n.property("Network", "SERVER", [](api::Call&) { return api::Value(net::kServerId); }, {},
               {"", "id del servidor (1)", "numero"});

    // --- Sesion ---
    n.function("Network.host", [&rt](api::Call& c) {
        std::string error;
        const auto port = static_cast<std::uint16_t>(c.integer(0, net::kDefaultPort));
        const bool ok = session(rt).host(port, static_cast<int>(c.integer(1, 16)), &error);
        if (!ok) rt.write(2, "Network.host: " + error);
        rt.net_entities.clear();
        return api::Value(api::Value::Array{api::Value(ok), api::Value(error)});
    }, {"7777, 8", "crea la partida (eres el servidor y juegas); devuelve ok, error", "lista {ok, error}"});
    n.function("Network.connect", [&rt](api::Call& c) {
        std::string error;
        const std::string address = c.string(0, "127.0.0.1");
        const auto port = static_cast<std::uint16_t>(c.integer(1, net::kDefaultPort));
        const bool ok = session(rt).connect(address, port, &error);
        if (!ok) rt.write(2, "Network.connect: " + error);
        rt.net_entities.clear();
        return api::Value(api::Value::Array{api::Value(ok), api::Value(error)});
    }, {"\"127.0.0.1\", 7777", "se une a una partida (llega onConnected u onDisconnected)", "lista {ok, error}"});
    n.function("Network.disconnect", [&rt](api::Call&) {
        if (rt.network) rt.network->close();
        destroyNetEntities(rt);
        return api::Value{};
    }, {"", "sale de la partida (o la cierra si eres el servidor)"});
    n.function("Network.isServer", [&rt](api::Call&) { return api::Value(rt.network && rt.network->isServer()); },
               {"", "eres el servidor?", "bool"});
    n.function("Network.isClient", [&rt](api::Call&) { return api::Value(rt.network && rt.network->isClient()); },
               {"", "eres un cliente?", "bool"});
    n.function("Network.isConnected", [&rt](api::Call&) { return api::Value(rt.network && rt.network->connected()); },
               {"", "en partida (servidor abierto o cliente dentro)", "bool"});
    n.function("Network.isConnecting", [&rt](api::Call&) { return api::Value(rt.network && rt.network->connecting()); },
               {"", "cliente esperando respuesta", "bool"});
    n.function("Network.isActive", [&rt](api::Call&) {
        return api::Value(rt.network && rt.network->role() != net::NetRole::None);
    }, {"", "hay sesion de red", "bool"});
    n.function("Network.myId", [&rt](api::Call&) { return api::Value(rt.network ? rt.network->localId() : 0u); },
               {"", "tu id de jugador (el servidor es 1)", "numero"});
    n.function("Network.players", [&rt](api::Call&) {
        return rt.network ? idList(rt.network->players()) : api::Value(api::Value::Array{});
    }, {"", "lista de ids de jugadores", "lista de numeros"});
    n.function("Network.playerCount", [&rt](api::Call&) {
        return api::Value(rt.network ? static_cast<int>(rt.network->players().size()) : 0);
    }, {"", "cuantos jugadores", "numero"});
    n.function("Network.ping", [&rt](api::Call& c) {
        const auto player = static_cast<std::uint32_t>(c.number(0, net::kServerId));
        return api::Value(rt.network ? rt.network->ping(player) : 0);
    }, {"id", "ida y vuelta en ms", "numero"});
    n.function("Network.stats", [&rt](api::Call&) {
        const net::NetworkSession* ns = rt.network.get();
        const double sent = ns != nullptr ? static_cast<double>(ns->bytesSent()) : 0.0;
        const double received = ns != nullptr ? static_cast<double>(ns->bytesReceived()) : 0.0;
        const int objects = ns != nullptr ? static_cast<int>(ns->objects().size()) : 0;
        api::Value t = api::Value::object();
        t.set("sent", sent);
        t.set("received", received);
        t.set("objects", objects);
        t.set("sendRate", ns != nullptr ? ns->sendRate() : 0.0f);
        t.set("receiveRate", ns != nullptr ? ns->receiveRate() : 0.0f);
        t.set("bytesSent", sent);
        t.set("bytesReceived", received);
        // Un cliente: su ping y su perdida con el servidor.
        if (ns != nullptr && ns->isClient()) {
            t.set("ping", ns->ping(net::kServerId));
            t.set("packetLoss", ns->packetLoss(net::kServerId));
        }
        return t;
    }, {"", "{sent, received, objects, sendRate, receiveRate, bytesSent, bytesReceived, ping, packetLoss}", "objeto"});

    // --- Mensajes: Network.send("chat", datos[, destino]) y Network.on("chat", fn(datos, de)) ---
    n.function("Network.send", [&rt](api::Call& c) {
        const std::string name = c.string(0);
        if (!rt.network || !rt.network->connected()) return api::Value(false);
        std::uint32_t to = net::kEveryone;
        const api::Value& target = c.arg(2);
        if (target.isNumber()) to = static_cast<std::uint32_t>(target.asNumber());
        else if (target.isString() && target.asString() == "server") to = net::kServerId;
        rt.network->send(name, toNet(rt, c.arg(1)), to);
        return api::Value(true);
    }, {"\"chat\", datos, destino", "mensaje (destino: nil = todos, \"server\" o un id)", "bool"});
    n.function("Network.on", [state](api::Call& c) {
        const std::string name = c.string(0);
        const api::Value& fn = c.function(1);
        if (fn.isNil()) state->handlers.erase(name);
        else state->handlers[name] = fn;
        return api::Value{};
    }, {"\"chat\", function(datos, de) end", "recibe un mensaje"});
    n.function("Network.off", [state](api::Call& c) {
        state->handlers.erase(c.string(0));
        return api::Value{};
    }, {"\"chat\"", "deja de recibirlo"});
    n.function("Network.onPlayerJoined", [state](api::Call& c) { state->on_player_joined = c.function(0); return api::Value{}; },
               {"function(id) end", "entra un jugador"});
    n.function("Network.onPlayerLeft", [state](api::Call& c) { state->on_player_left = c.function(0); return api::Value{}; },
               {"function(id) end", "sale un jugador"});
    n.function("Network.onConnected", [state](api::Call& c) { state->on_connected = c.function(0); return api::Value{}; },
               {"function(id) end", "cliente: ya estas dentro"});
    n.function("Network.onDisconnected", [state](api::Call& c) { state->on_disconnected = c.function(0); return api::Value{}; },
               {"function(motivo) end", "fuera de la partida"});

    // --- Objetos: solo el servidor los crea y los borra ---
    n.function("Network.spawn", [&rt](api::Call& c) {
        const std::string prefab = c.string(0);
        if (!rt.network || !rt.network->isServer()) {
            rt.write(2, "Network.spawn: solo el servidor crea objetos de red (un cliente se lo pide con Network.send)");
            return api::Value{};
        }
        const Vec3 p = c.vec3(1, Vec3{});
        const auto owner = static_cast<std::uint32_t>(c.number(2, net::kServerId));
        // Giro en grados (Vec3) o un Quat.
        Quat q{};
        if (c.arg(3).isQuat()) q = c.quat(3);
        else if (c.has(3)) q = ecs::quatFromEulerDegrees(c.vec3(3));
        const ecs::Entity e = instantiateNetPrefab(rt, prefab);
        if (!e.valid()) return api::Value{};
        const std::uint32_t id = rt.network->spawn(prefab, p, q, owner);
        attachNet(rt, e, id, owner, p, q);
        return rt.entityValue(e);
    }, {"\"Prefabs/Jugador\", posicion, dueno, giro", "crea un objeto de red en todos (solo el servidor)", "Entity"});
    n.function("Network.destroy", [&rt](api::Call& c) {
        const ecs::Entity e = rt.entityArg(c, 0);
        const net::NetworkObject* no = netObject(e);
        if (rt.network && rt.network->isServer() && no != nullptr && no->net_id != 0) {
            rt.network->despawn(no->net_id);
            rt.net_entities.erase(no->net_id);
        }
        if (e.valid()) rt.destroyLater(e.handle());
        return api::Value{};
    }, {"entity", "lo borra en todos (solo el servidor)"});
    n.function("Network.objects", [&rt](api::Call&) {
        api::Value::Array out;
        for (const auto& [id, handle] : rt.net_entities) {
            if (rt.world != nullptr && rt.world->registry().valid(handle)) out.push_back(rt.entityValue(handle));
        }
        return api::Value(std::move(out));
    }, {"", "todas las entidades de red", "lista de Entity"});
    n.function("Network.find", [&rt](api::Call& c) {
        const ecs::Entity e = netEntity(rt, static_cast<std::uint32_t>(c.number(0)));
        return e.valid() ? rt.entityValue(e) : api::Value{};
    }, {"netId", "entidad por su id de red", "Entity"});
    n.function("Network.loadScene", [&rt](api::Call& c) {
        const std::string name = c.string(0);
        if (!rt.network || !rt.network->isServer()) {
            rt.write(2, "Network.loadScene: solo el servidor cambia la escena de todos");
            return api::Value{};
        }
        rt.network->loadScene(name);
        rt.net_entities.clear();
        rt.scene_request = rt.findScene(name);
        if (rt.scene_request.empty()) rt.write(2, "Network.loadScene: no existe la escena \"" + name + "\"");
        return api::Value{};
    }, {"\"Nivel2\"", "todos cargan la escena (solo el servidor)"});

    // --- Netcode: servidor dedicado, compensacion de lag y simulador ---
    // CramionServer pone Network.dedicated = true al cargar cada escena.
    n.property("Network", "dedicated", [state](api::Call&) { return api::Value(state->dedicated); },
               [state](api::Call& c) { state->dedicated = c.arg(0).truthy(); return api::Value{}; },
               {"", "true en un servidor dedicado (lo pone CramionServer)", "bool"});
    n.function("Network.isDedicated", [state](api::Call&) { return api::Value(state->dedicated); },
               {"", "true en CramionServer (servidor dedicado sin ventana)", "bool"});
    // Donde estaba un objeto de red hace `segundos` (historia de 1,5 s).
    n.function("Network.positionAt", [&rt](api::Call& c) {
        const auto id = static_cast<std::uint32_t>(c.integer(0));
        const auto seconds_ago = static_cast<float>(c.number(1));
        if (!rt.network) return api::Value{};
        Vec3 p{};
        Quat q{};
        if (!rt.network->positionAt(id, seconds_ago, p, q)) return api::Value{};
        return api::Value(p);
    }, {"netId, segundos", "donde estaba un objeto de red hace unos segundos (historia de 1,5 s)", "Vec3"});
    // Servidor: el disparo de `jugador` contra donde el veia a los demas (su
    // ping / 2 + el retraso de interpolacion). Devuelve id de red, punto y
    // distancia del primero que toca (esferas de `radio` a `altura`).
    n.function("Network.lagCompensatedRaycast", [&rt](api::Call& c) {
        const Vec3 origin = c.vec3(0);
        const Vec3 direction = c.vec3(1);
        const auto max_distance = static_cast<float>(c.number(2));
        const auto shooter = static_cast<std::uint32_t>(c.integer(3));
        const auto r = static_cast<float>(c.number(4, 0.5));
        const auto up = static_cast<float>(c.number(5, 1.0));
        if (!rt.network) return api::Value{};
        const Vec3 dir = core::normalize(direction);
        const float ping_s = static_cast<float>(rt.network->ping(shooter)) / 2000.0f;
        std::uint32_t best_id = 0;
        float best_t = max_distance;
        for (const auto& [id, obj] : rt.network->objects()) {
            if (obj.owner == shooter) continue;
            float delay = 0.1f;
            if (const net::NetworkObject* no = netObject(netEntity(rt, id))) delay = no->interpolation_delay;
            Vec3 p{};
            Quat q{};
            if (!rt.network->positionAt(id, ping_s + delay, p, q)) continue;
            const Vec3 center = p + Vec3{0.0f, up, 0.0f};
            const Vec3 oc = origin - center;
            const float b = core::dot(oc, dir);
            const float cc = core::dot(oc, oc) - r * r;
            const float disc = b * b - cc;
            if (disc < 0.0f) continue;
            const float t = -b - std::sqrt(disc);
            if (t >= 0.0f && t < best_t) {
                best_t = t;
                best_id = id;
            }
        }
        if (best_id == 0) return api::Value{};
        api::Value hit = api::Value::object();
        hit.set("netId", best_id);
        hit.set("point", origin + dir * best_t);
        hit.set("distance", best_t);
        return hit;
    }, {"origen, direccion, distancia, jugador, radio, altura",
        "servidor: disparo de un jugador contra donde el veia a los demas (compensacion de lag)",
        "{netId, point, distance} o nil"});
    // Network.simulate{latency = 120, jitter = 30, loss = 5} (todo a 0 = red normal).
    n.function("Network.simulate", [](api::Call& c) {
        const api::Value& o = c.object(0);
        const auto field = [&o](const char* key) { return std::to_string(static_cast<int>(o[key].asNumber(0.0))); };
        cvar::Registry& reg = cvar::Registry::instance();
        reg.execute("net.SimLatencyMs " + field("latency"));
        reg.execute("net.SimJitterMs " + field("jitter"));
        reg.execute("net.SimLossPercent " + field("loss"));
        return api::Value{};
    }, {"{ latency = 120, jitter = 30, loss = 5 }", "simula una red mala (ms de retraso, variacion y % de perdida; 0 = normal)"});

    // --- En las entidades ---
    n.method("Entity", "isMine", [&rt](api::Call& c) {
        const net::NetworkObject* no = netObject(rt.selfEntity(c));
        // Sin red: todo es mio.
        if (no == nullptr || no->net_id == 0 || !rt.network || !rt.network->connected()) return api::Value(true);
        return api::Value(no->owner == rt.network->localId());
    }, {"", "red: este objeto lo controlas tu (sin red, siempre)", "bool"});
    n.property("Entity", "netId", [&rt](api::Call& c) {
        const net::NetworkObject* no = netObject(rt.selfEntity(c));
        return api::Value(no != nullptr ? no->net_id : 0u);
    }, {}, {"", "red: id del objeto (0 = no es de red)", "numero"}, true);
    n.property("Entity", "netOwner", [&rt](api::Call& c) {
        const net::NetworkObject* no = netObject(rt.selfEntity(c));
        return api::Value(no != nullptr ? no->owner : 0u);
    }, {}, {"", "red: id del jugador dueno", "numero"}, true);
    n.method("Entity", "setNetVar", [&rt](api::Call& c) {
        const net::NetworkObject* no = netObject(rt.selfEntity(c));
        const std::string key = c.string(0);
        if (no == nullptr || no->net_id == 0 || !rt.network) return api::Value{};
        rt.network->setVar(no->net_id, key, toNet(rt, c.arg(1)));
        return api::Value{};
    }, {"\"vida\", 100", "red: variable sincronizada (dueno o servidor)"});
    n.method("Entity", "getNetVar", [&rt](api::Call& c) {
        const net::NetworkObject* no = netObject(rt.selfEntity(c));
        const std::string key = c.string(0);
        if (no == nullptr || no->net_id == 0 || !rt.network) return api::Value{};
        const net::NetObject* o = rt.network->object(no->net_id);
        if (o == nullptr) return api::Value{};
        const auto it = o->vars.find(key);
        return it != o->vars.end() ? fromNet(it->second) : api::Value{};
    }, {"\"vida\"", "red: lee una variable sincronizada", "valor"});
}

}  // namespace cramion::scripting::native
