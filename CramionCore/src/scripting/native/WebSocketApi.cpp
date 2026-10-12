// WebSocket (conexiones ws:// y wss:// con el servidor del juego) y Base64.
//
// Las conexiones son de rt.websockets (net::WebSocketHub): sobreviven a
// stop()/start() las que tienen nombre (WebSocket.connect{name = "mmo"}); las
// demas se cierran (1001) al cambiar de escena. Se cierran todas en
// shutdownNetwork() (salir de Play, cerrar el juego).
//
// Los eventos llegan de dos formas, nunca las dos a la vez:
//   - Con escuchas (WebSocket.on): al principio del frame (fase Begin,
//     despues de Http) se reparten a sus funciones, como mucho
//     net.WsMaxEventsPerFrame por frame entre todas las conexiones.
//   - Sin escuchas: se quedan en la conexion hasta WebSocket.poll(id).
// Los bytes (sendBinary, mensajes binarios) cruzan siempre en base64.

#include "JsonValue.h"
#include "Modules.h"

#include "../../net/ws/WsProtocol.h"
#include "CramionCore/cvar/CVar.h"
#include "CramionCore/net/Encoding.h"
#include "CramionCore/net/WebSocket.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cramion::scripting::native {
namespace {

using json = nlohmann::json;

struct Listener {
    std::uint64_t token = 0;
    std::string key;   // "#<id>" o el nombre de la conexion
    std::string kind;  // open, message, close, error, reconnecting, any
    api::Value fn;
};

struct WsApiState {
    std::vector<Listener> listeners;
    std::uint64_t next_token = 1;
    std::size_t rr = 0;  // por que conexion se empieza a repartir (reparto justo)
    std::unordered_map<std::uint32_t, double> last_warn;  // id -> segundo del ultimo aviso de envio
    const cvar::CVarBase* events_cvar = nullptr;           // net.WsMaxEventsPerFrame (WebSocket.cpp)
};

double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string idKey(std::uint32_t id) { return "#" + std::to_string(id); }

bool validKind(const std::string& kind) {
    return kind == "open" || kind == "message" || kind == "close" || kind == "error" || kind == "reconnecting" ||
           kind == "any";
}

const char* kindName(net::WsEvent::Kind kind) {
    switch (kind) {
        case net::WsEvent::Kind::Open: return "open";
        case net::WsEvent::Kind::Text:
        case net::WsEvent::Kind::Binary: return "message";
        case net::WsEvent::Kind::Close: return "close";
        case net::WsEvent::Kind::Error: return "error";
        case net::WsEvent::Kind::Reconnecting: return "reconnecting";
    }
    return "error";
}

// El evento para los scripts: siempre {type, id, name} y lo suyo.
api::Value eventValue(std::uint32_t id, const std::string& name, const net::WsEvent& ev) {
    api::Value t = api::Value::object();
    t.set("type", kindName(ev.kind));
    t.set("id", static_cast<double>(id));
    t.set("name", name);
    switch (ev.kind) {
        case net::WsEvent::Kind::Open: t.set("protocol", ev.protocol); break;
        case net::WsEvent::Kind::Text: {
            t.set("binary", false);
            t.set("text", ev.data);
            // JSON: ya decodificado en `data` (solo si parece un objeto o una lista).
            const std::size_t first = ev.data.find_first_not_of(" \t\r\n");
            if (first != std::string::npos && (ev.data[first] == '{' || ev.data[first] == '[') &&
                ev.data.size() <= (1u << 20)) {
                const json j = json::parse(ev.data, nullptr, false);
                if (!j.is_discarded()) t.set("data", jsonToValue(j));
            }
            break;
        }
        case net::WsEvent::Kind::Binary:
            t.set("binary", true);
            t.set("base64", net::base64Encode(ev.data));
            break;
        case net::WsEvent::Kind::Close:
            t.set("code", ev.code);
            t.set("reason", ev.data);
            t.set("clean", ev.clean);
            t.set("reconnect", ev.will_reconnect);
            t.set("dropped", static_cast<double>(ev.dropped));
            break;
        case net::WsEvent::Kind::Error:
            t.set("error", ev.data);
            t.set("status", ev.http_status);
            t.set("reconnect", ev.will_reconnect);
            break;
        case net::WsEvent::Kind::Reconnecting:
            t.set("attempt", ev.code);
            t.set("delay", static_cast<double>(ev.delay_ms) / 1000.0);
            break;
    }
    return t;
}

// "nil, motivo": el motivo va a la consola.
api::Value failed(Runtime& rt, const std::string& where, const std::string& error) {
    rt.write(2, where + ": " + error);
    return {};
}

// La conexion: su id (numero) o su nombre (texto). 0 si no hay.
std::uint32_t idArg(Runtime& rt, const api::Call& c, std::size_t i) {
    const api::Value& v = c.arg(i);
    if (v.isNumber()) {
        const double d = v.asNumber();
        return d >= 1.0 && d <= 4294967295.0 ? static_cast<std::uint32_t>(d) : 0u;
    }
    if (v.isString()) return rt.websockets ? rt.websockets->find(v.asString()) : 0u;
    throw api::Error("se esperaba el id o el nombre de la conexion");
}

std::shared_ptr<net::WebSocket> socketOf(Runtime& rt, std::uint32_t id) {
    return id != 0 && rt.websockets ? rt.websockets->get(id) : nullptr;
}

// Codigo de cierre que puede pedir un script (1000 o 3000-4999).
int closeCodeArg(const api::Call& c, std::size_t i) {
    if (!c.has(i)) return 1000;
    const api::Value& v = c.arg(i);
    if (!v.isNumber()) throw api::Error("codigo de cierre no valido (1000 o 3000-4999)");
    const double d = v.asNumber();
    const int code = std::isfinite(d) && std::floor(d) == d && std::abs(d) < 100000.0 ? static_cast<int>(d) : -1;
    if (!net::ws::validSendCloseCode(code)) throw api::Error("codigo de cierre no valido (1000 o 3000-4999)");
    return code;
}

// Aviso de un envio que no se pudo: como mucho uno por segundo y conexion.
bool sendFailed(Runtime& rt, WsApiState& s, const char* where, std::uint32_t id, const std::string& error) {
    const double now = nowSeconds();
    const auto it = s.last_warn.find(id);
    if (it == s.last_warn.end() || now - it->second >= 1.0) {
        s.last_warn[id] = now;
        rt.write(1, std::string(where) + ": " + error);
    }
    return false;
}

// Segundos (numero) -> milisegundos; `fallback` si no es un numero.
int msOption(const api::Value& v, int fallback) {
    if (!v.isNumber()) return fallback;
    const double d = v.asNumber();
    if (!std::isfinite(d)) return fallback;
    return static_cast<int>(std::clamp(d * 1000.0, -1.0, 2147483647.0));
}

// Opciones de WebSocket.connect. false + motivo si algo no vale.
bool buildOptions(const api::Value& o, net::WebSocketOptions& out, std::string& error) {
    const api::Value& url = o["url"];
    if (!url.isString()) {
        error = "direccion no valida (usa ws:// o wss://)";
        return false;
    }
    out.url = url.asString();
    const api::Value& headers = o["headers"];
    if (!headers.isNil()) {
        if (!headers.isObject()) throw api::Error("headers debe ser una tabla { nombre = valor }");
        for (const auto& [name, v] : headers.fields()) {
            const std::string value = v.isString()   ? v.asString()
                                      : v.isNumber() ? std::to_string(static_cast<long long>(v.asNumber()))
                                                     : std::string();
            if (!net::validHeader(name, value)) {
                error = "cabecera no valida: " + name;
                return false;
            }
            out.headers.emplace_back(name, value);
        }
    }
    const api::Value& protocols = o["protocols"];
    if (protocols.isString()) {
        out.protocols.push_back(protocols.asString());
    } else if (protocols.isArray()) {
        for (const api::Value& p : protocols.items()) {
            if (!p.isString()) throw api::Error("protocols debe ser una lista de textos");
            out.protocols.push_back(p.asString());
        }
    } else if (!protocols.isNil()) {
        throw api::Error("protocols debe ser una lista de textos");
    }
    // reconnect = true/false o { min = 0.5, max = 30, tries = -1, appCodes = false } (segundos).
    const api::Value& reconnect = o["reconnect"];
    if (reconnect.isBool()) {
        out.reconnect.enabled = reconnect.truthy();
    } else if (reconnect.isObject()) {
        out.reconnect.enabled = true;
        out.reconnect.min_delay_ms = std::max(0, msOption(reconnect["min"], out.reconnect.min_delay_ms));
        out.reconnect.max_delay_ms =
            std::max(out.reconnect.min_delay_ms, msOption(reconnect["max"], out.reconnect.max_delay_ms));
        const api::Value& tries = reconnect["tries"];
        if (tries.isNumber()) out.reconnect.max_tries = static_cast<int>(std::clamp(tries.asNumber(), -1.0, 1000000.0));
        out.reconnect.on_app_codes = reconnect["appCodes"].truthy();
    } else if (!reconnect.isNil()) {
        throw api::Error("reconnect debe ser true/false o { min, max, tries, appCodes }");
    }
    out.connect_timeout_ms = std::max(1, msOption(o["timeout"], out.connect_timeout_ms));
    const api::Value& max_size = o["maxSize"];
    if (max_size.isNumber()) {
        out.max_message_bytes = static_cast<std::size_t>(
            std::clamp(max_size.asNumber(), 1.0, static_cast<double>(net::kWsHardMaxMessage)));
    } else if (const auto* kb = dynamic_cast<const cvar::CVar<int>*>(cvar::Registry::instance().find("net.WsMaxMessageKB"))) {
        // Sin maxSize: el de net.WsMaxMessageKB.
        out.max_message_bytes = std::min(static_cast<std::size_t>(std::max(1, kb->get())) * 1024u, net::kWsHardMaxMessage);
    }
    // 0 = sin ping (open() sube lo demas a 1 s como minimo).
    out.ping_interval_ms = std::max(0, msOption(o["pingInterval"], out.ping_interval_ms));
    if (o["idleTimeout"].isNumber()) out.idle_timeout_ms = std::max(0, msOption(o["idleTimeout"], 0));
    const api::Value& heartbeat = o["heartbeat"];
    if (heartbeat.isString()) out.heartbeat_text = heartbeat.asString();
    out.resend_queued = o["resend"].truthy();
    return true;
}

// Reparte los eventos de las conexiones con escuchas (fase Begin).
void deliver(Runtime& rt, WsApiState& s) {
    if (!rt.websockets) return;
    if (s.events_cvar == nullptr) s.events_cvar = cvar::Registry::instance().find("net.WsMaxEventsPerFrame");
    std::size_t budget = 512;
    if (const auto* v = dynamic_cast<const cvar::CVar<int>*>(s.events_cvar)) budget = static_cast<std::size_t>(std::max(1, v->get()));

    const std::vector<std::uint32_t> ids = rt.websockets->ids();
    if (!ids.empty()) {
        const std::size_t start = s.rr % ids.size();
        s.rr = start + 1;
        for (std::size_t k = 0; k < ids.size() && budget > 0; ++k) {
            if (!rt.websockets) return;  // una funcion cerro la red
            const std::uint32_t id = ids[(start + k) % ids.size()];
            const std::shared_ptr<net::WebSocket> ws = rt.websockets->get(id);
            if (!ws) continue;
            const std::string name = rt.websockets->name(id);
            const std::string key = idKey(id);
            const auto mine = [&](const Listener& l) { return l.key == key || (!name.empty() && l.key == name); };
            if (std::none_of(s.listeners.begin(), s.listeners.end(), mine)) continue;  // sin escuchas: poll
            const std::vector<net::WsEvent> events = ws->poll(budget);
            budget -= std::min(budget, events.size());
            for (const net::WsEvent& ev : events) {
                const std::string kind = kindName(ev.kind);
                // Copias: una funcion puede llamar a on/off mientras tanto.
                std::vector<api::Value> fns;
                for (const Listener& l : s.listeners) {
                    if (mine(l) && (l.kind == kind || l.kind == "any")) fns.push_back(l.fn);
                }
                if (fns.empty()) continue;  // esa conexion va con escuchas: lo que nadie escucha se tira
                const api::Value value = eventValue(id, name, ev);
                for (const api::Value& fn : fns) rt.native.invoke(fn, {value});
            }
        }
    }
    if (rt.websockets) rt.websockets->collect();
}

}  // namespace

void registerWebSocketApi(Runtime& rt) {
    auto state = std::make_shared<WsApiState>();
    // Despues de Http (registrada antes).
    rt.onFrame(Phase::Begin, [&rt, state](float) { deliver(rt, *state); });
    // Cambio de escena: las escuchas son de los scripts; las conexiones sin
    // nombre se cierran y las que tienen nombre siguen.
    rt.onStop([&rt, state](bool) {
        state->listeners.clear();
        state->last_warn.clear();
        if (rt.websockets) rt.websockets->closeAll(1001, "Cambio de escena", /*unnamed_only*/ true);
    });
    // El proceso de los scripts de C++ se reinicio: sus funciones ya no valen
    // (las de los Visual Scripts, ids locales, si).
    rt.onBridgeReset([state]() {
        std::erase_if(state->listeners,
                      [](const Listener& l) { return l.fn.callbackId() < api::NativeApi::kLocalCallbackBase; });
    });

    api::NativeApi& n = rt.native;
    n.function("WebSocket.close", [&rt](api::Call& c) {
        const std::uint32_t id = idArg(rt, c, 0);
        const int code = closeCodeArg(c, 1);
        const std::string reason = net::truncateUtf8(c.string(2, ""), net::ws::kMaxCloseReason);
        if (id != 0 && rt.websockets) rt.websockets->close(id, code, reason);
        return api::Value{};
    }, {"id, 1000, \"motivo\"", "cierre limpio (1000, o 3000-4999 propios); no reconecta", ""});
    n.function("WebSocket.closeAll", [&rt](api::Call& c) {
        const int code = closeCodeArg(c, 0);
        const std::string reason = net::truncateUtf8(c.string(1, ""), net::ws::kMaxCloseReason);
        if (rt.websockets) rt.websockets->closeAll(code, reason);
        return api::Value{};
    }, {"1000, \"motivo\"", "cierra todas las conexiones", ""});
    n.function("WebSocket.connect", [&rt](api::Call& c) {
        net::WebSocketOptions options;
        std::string error;
        api::Value o;
        if (c.arg(0).isString()) {  // WebSocket.connect("wss://...")
            o = api::Value::object();
            o.set("url", c.arg(0));
        } else {
            o = c.object(0);
        }
        if (!buildOptions(o, options, error)) return failed(rt, "WebSocket.connect", error);
        const api::Value& name_value = o["name"];
        if (!name_value.isNil() && !name_value.isString()) throw api::Error("name debe ser un texto");
        const std::string name = name_value.isString() ? name_value.asString() : std::string();
        if (!rt.websockets) rt.websockets = std::make_unique<net::WebSocketHub>();
        const std::uint32_t id = rt.websockets->open(std::move(options), name, &error);
        if (id == 0) return failed(rt, "WebSocket.connect", error);
        return api::Value(static_cast<double>(id));
    }, {"{ url = \"wss://...\", name = \"mmo\", headers = {}, protocols = {}, reconnect = true, timeout = 10, maxSize = 4194304, pingInterval = 20, idleTimeout = 60, heartbeat = \"\" }",
        "abre una conexion WebSocket (no espera); con name, si ya hay una viva devuelve esa",
        "id de la conexion (nil si no vale)"});
    n.function("WebSocket.count", [&rt](api::Call&) { return api::Value(rt.websockets ? rt.websockets->count() : 0); },
               {"", "conexiones vivas (no cerradas)", "numero"});
    n.function("WebSocket.find", [&rt](api::Call& c) {
        const std::uint32_t id = rt.websockets ? rt.websockets->find(c.string(0)) : 0u;
        return id != 0 ? api::Value(static_cast<double>(id)) : api::Value{};
    }, {"\"mmo\"", "la conexion con ese nombre", "id (nil si no hay)"});
    n.function("WebSocket.info", [&rt](api::Call& c) {
        const std::uint32_t id = idArg(rt, c, 0);
        const std::shared_ptr<net::WebSocket> ws = socketOf(rt, id);
        if (!ws) return api::Value{};
        const net::WsStats st = ws->stats();
        api::Value t = api::Value::object();
        t.set("id", static_cast<double>(id));
        t.set("name", rt.websockets->name(id));
        t.set("url", ws->url());
        t.set("state", net::wsStateName(st.state));
        t.set("protocol", st.protocol);
        t.set("sent", static_cast<double>(st.sent_messages));
        t.set("received", static_cast<double>(st.received_messages));
        t.set("sentBytes", static_cast<double>(st.sent_bytes));
        t.set("receivedBytes", static_cast<double>(st.received_bytes));
        t.set("buffered", static_cast<double>(st.queued_send_bytes));
        t.set("rtt", st.rtt_ms);
        t.set("reconnects", st.reconnects);
        return t;
    }, {"id", "datos de la conexion",
        "{ id, name, url, state, protocol, sent, received, sentBytes, receivedBytes, buffered, rtt, reconnects } (nil si no existe)"});
    n.function("WebSocket.list", [&rt](api::Call&) {
        api::Value::Array list;
        if (!rt.websockets) return api::Value(std::move(list));
        for (const std::uint32_t id : rt.websockets->ids()) {
            const std::shared_ptr<net::WebSocket> ws = rt.websockets->get(id);
            if (!ws) continue;
            const net::WsState st = ws->state();
            if (st == net::WsState::Closed) continue;
            api::Value t = api::Value::object();
            t.set("id", static_cast<double>(id));
            t.set("name", rt.websockets->name(id));
            t.set("state", net::wsStateName(st));
            t.set("url", ws->url());
            list.push_back(std::move(t));
        }
        return api::Value(std::move(list));
    }, {"", "conexiones vivas", "lista de { id, name, state, url }"});
    n.function("WebSocket.off", [state](api::Call& c) {
        const double token = c.number(0);
        std::erase_if(state->listeners, [token](const Listener& l) { return static_cast<double>(l.token) == token; });
        return api::Value{};
    }, {"escucha", "deja de escuchar (el numero que devolvio on)", ""});
    n.function("WebSocket.on", [state](api::Call& c) {
        const api::Value& target = c.arg(0);
        std::string key;
        if (target.isNumber()) {
            const double d = target.asNumber();
            if (!(d >= 1.0 && d <= 4294967295.0)) throw api::Error("id de conexion no valido");
            key = idKey(static_cast<std::uint32_t>(d));
        } else if (target.isString() && !target.asString().empty()) {
            key = target.asString();  // por nombre: vale antes de conectar
        } else {
            throw api::Error("se esperaba el id o el nombre de la conexion");
        }
        const std::string kind = c.string(1, "message");
        if (!validKind(kind)) throw api::Error("evento no valido: " + kind + " (open, message, close, error, reconnecting o any)");
        const api::Value& fn = c.function(2);
        if (fn.isNil()) {  // sin funcion: quita las de esa conexion y evento
            std::erase_if(state->listeners, [&](const Listener& l) { return l.key == key && l.kind == kind; });
            return api::Value{};
        }
        Listener l;
        l.token = state->next_token++;
        l.key = std::move(key);
        l.kind = kind;
        l.fn = fn;
        state->listeners.push_back(std::move(l));
        return api::Value(static_cast<double>(state->listeners.back().token));
    }, {"id, \"message\", function(ev) end", "escucha un evento: open, message, close, error, reconnecting o any (id o nombre)",
        "numero de la escucha (para off)"});
    n.function("WebSocket.poll", [&rt](api::Call& c) {
        const std::uint32_t id = idArg(rt, c, 0);
        api::Value::Array list;
        const std::shared_ptr<net::WebSocket> ws = socketOf(rt, id);
        if (!ws) return api::Value(std::move(list));
        const double max = c.number(1, 0.0);
        const std::size_t count = max >= 1.0 ? static_cast<std::size_t>(std::min(max, 1000000.0)) : 1000u;
        const std::string name = rt.websockets->name(id);
        for (const net::WsEvent& ev : ws->poll(count)) list.push_back(eventValue(id, name, ev));
        return api::Value(std::move(list));
    }, {"id, 100", "saca los eventos pendientes (si no usas on); un viaje por frame", "lista de eventos"});
    n.function("WebSocket.send", [&rt, state](api::Call& c) {
        const std::uint32_t id = idArg(rt, c, 0);
        std::string text = c.string(1);
        const std::shared_ptr<net::WebSocket> ws = socketOf(rt, id);
        if (!ws) return api::Value(sendFailed(rt, *state, "WebSocket.send", id, "la conexion no existe"));
        std::string error;
        if (!ws->send(std::move(text), false, &error)) return api::Value(sendFailed(rt, *state, "WebSocket.send", id, error));
        return api::Value(true);
    }, {"id, \"texto\"", "envia texto (UTF-8)", "true si se encolo"});
    n.function("WebSocket.sendBinary", [&rt, state](api::Call& c) {
        const std::uint32_t id = idArg(rt, c, 0);
        std::string bytes;
        if (!net::base64Decode(c.string(1), bytes)) {
            return api::Value(sendFailed(rt, *state, "WebSocket.sendBinary", id, "base64 no valido"));
        }
        const std::shared_ptr<net::WebSocket> ws = socketOf(rt, id);
        if (!ws) return api::Value(sendFailed(rt, *state, "WebSocket.sendBinary", id, "la conexion no existe"));
        std::string error;
        if (!ws->send(std::move(bytes), true, &error)) {
            return api::Value(sendFailed(rt, *state, "WebSocket.sendBinary", id, error));
        }
        return api::Value(true);
    }, {"id, \"base64\"", "envia bytes (en base64)", "true si se encolo"});
    n.function("WebSocket.sendJson", [&rt, state](api::Call& c) {
        const std::uint32_t id = idArg(rt, c, 0);
        json j;
        std::string error;
        if (!valueToJson(c.arg(1), j, error)) return api::Value(sendFailed(rt, *state, "WebSocket.sendJson", id, error));
        const std::shared_ptr<net::WebSocket> ws = socketOf(rt, id);
        if (!ws) return api::Value(sendFailed(rt, *state, "WebSocket.sendJson", id, "la conexion no existe"));
        if (!ws->send(j.dump(-1, ' ', false, json::error_handler_t::replace), false, &error)) {
            return api::Value(sendFailed(rt, *state, "WebSocket.sendJson", id, error));
        }
        return api::Value(true);
    }, {"id, tabla", "envia la tabla como texto JSON", "true si se encolo"});
    n.function("WebSocket.setHeader", [&rt](api::Call& c) {
        const std::uint32_t id = idArg(rt, c, 0);
        const std::shared_ptr<net::WebSocket> ws = socketOf(rt, id);
        if (!ws) return api::Value(false);
        std::string error;
        if (!ws->setHeader(c.string(1), c.string(2, ""), &error)) {
            rt.write(1, "WebSocket.setHeader: " + error);
            return api::Value(false);
        }
        return api::Value(true);
    }, {"id, \"Authorization\", \"Bearer ...\"", "cabecera para la proxima reconexion (vacio la quita)", "true si vale"});
    n.function("WebSocket.state", [&rt](api::Call& c) {
        const std::shared_ptr<net::WebSocket> ws = socketOf(rt, idArg(rt, c, 0));
        return api::Value(ws ? net::wsStateName(ws->state()) : "closed");
    }, {"id", "connecting, open, closing, closed o reconnecting", "texto (\"closed\" si no existe)"});

    // --- Base64 ---
    n.function("Base64.decode", [&rt](api::Call& c) {
        std::string out;
        if (!net::base64Decode(c.string(0), out)) {
            rt.write(1, "Base64.decode: base64 no valido");
            return api::Value{};
        }
        return api::Value(std::move(out));
    }, {"\"base64\"", "base64 -> texto o bytes (nil y un aviso si no es valido)", "texto"});
    n.function("Base64.encode", [](api::Call& c) { return api::Value(net::base64Encode(c.string(0))); },
               {"\"texto\"", "texto o bytes -> base64", "texto"});
}

}  // namespace cramion::scripting::native
