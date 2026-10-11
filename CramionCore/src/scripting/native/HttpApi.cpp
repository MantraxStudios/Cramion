// Http (peticiones HTTPS) y Json.
//
// Las peticiones van en hilos aparte (rt.http, net::HttpClient) y las
// respuestas se entregan al principio del frame (fase Begin, despues de la
// red) a la funcion que se paso: function(res) con {ok, status, body, time,
// error, headers, data}.
//
// JSON: una lista es un array y un objeto, un objeto JSON; un Vec3 se
// escribe como {x, y, z}. Al leer, null = nil.

#include "Modules.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>

namespace cramion::scripting::native {
namespace {

using json = nlohmann::json;

struct HttpState {
    // Id de la peticion -> su funcion (se olvidan al parar o con cancelAll).
    std::unordered_map<std::uint32_t, api::Value> callbacks;
};

// --- JSON ---------------------------------------------------------------------------

// Valor -> JSON. Numeros enteros como enteros; Vec3 = {x, y, z}. false +
// motivo si hay algo que no se puede escribir (funciones, entidades...).
bool valueToJson(const api::Value& v, json& out, std::string& error, int depth = 0) {
    if (depth > 64) {
        error = "demasiados niveles (tablas dentro de tablas)";
        return false;
    }
    switch (v.type()) {
        case api::Value::Type::Nil: out = nullptr; return true;
        case api::Value::Type::Bool: out = v.truthy(); return true;
        case api::Value::Type::Number: {
            const double d = v.asNumber();
            if (!std::isfinite(d)) {
                out = nullptr;
            } else if (std::floor(d) == d && std::abs(d) < 9007199254740992.0) {
                out = static_cast<std::int64_t>(d);
            } else {
                out = d;
            }
            return true;
        }
        case api::Value::Type::String: out = v.asString(); return true;
        case api::Value::Type::Vec3: {
            const core::Vec3 p = v.asVec3();
            out = json{{"x", p.x}, {"y", p.y}, {"z", p.z}};
            return true;
        }
        case api::Value::Type::Array: {
            out = json::array();
            for (const api::Value& item : v.items()) {
                json j;
                if (!valueToJson(item, j, error, depth + 1)) return false;
                out.push_back(std::move(j));
            }
            return true;
        }
        case api::Value::Type::Object: {
            out = json::object();
            for (const auto& [key, item] : v.fields()) {
                json j;
                if (!valueToJson(item, j, error, depth + 1)) return false;
                out[key] = std::move(j);
            }
            return true;
        }
        case api::Value::Type::Quat:
        case api::Value::Type::Entity:
        case api::Value::Type::Handle: error = "no se puede pasar a JSON un userdata"; return false;
        case api::Value::Type::Function: error = "no se puede pasar a JSON un valor de tipo function"; return false;
    }
    error = "valor desconocido";
    return false;
}

// JSON -> valor (null = nil). Sin interpretar claves especiales ("$v"...):
// es lo que mando el servidor tal cual.
api::Value jsonToValue(const json& j, int depth = 0) {
    if (depth > 256) return {};
    switch (j.type()) {
        case json::value_t::boolean: return api::Value(j.get<bool>());
        case json::value_t::number_integer: return api::Value(static_cast<double>(j.get<std::int64_t>()));
        case json::value_t::number_unsigned: return api::Value(static_cast<double>(j.get<std::uint64_t>()));
        case json::value_t::number_float: return api::Value(j.get<double>());
        case json::value_t::string: return api::Value(j.get_ref<const std::string&>());
        case json::value_t::array: {
            api::Value::Array list;
            list.reserve(j.size());
            for (const json& item : j) list.push_back(jsonToValue(item, depth + 1));
            return api::Value(std::move(list));
        }
        case json::value_t::object: {
            api::Value o = api::Value::object();
            for (const auto& [key, item] : j.items()) o.set(key, jsonToValue(item, depth + 1));
            return o;
        }
        default: return {};
    }
}

// --- HTTP ---------------------------------------------------------------------------

// Respuesta para el callback: {ok, status, body, time, error, headers, data}.
api::Value responseValue(const net::HttpResponse& r) {
    api::Value t = api::Value::object();
    t.set("ok", r.ok);
    t.set("status", r.status);
    t.set("body", r.body);
    t.set("time", r.seconds);
    if (!r.error.empty()) t.set("error", r.error);
    else if (!r.ok) t.set("error", "El servidor respondio " + std::to_string(r.status));
    api::Value headers = api::Value::object();
    std::string content_type;
    for (const auto& [name, value] : r.headers) {
        headers.set(name, value);
        if (name == "content-type") content_type = value;
    }
    t.set("headers", headers);
    // JSON: ya decodificado en `data`.
    if (content_type.find("json") != std::string::npos && !r.body.empty()) {
        const json j = json::parse(r.body, nullptr, false);
        if (!j.is_discarded()) t.set("data", jsonToValue(j));
    }
    return t;
}

// Opciones -> HttpRequest. false + motivo si algo no vale.
bool buildRequest(const std::string& method, const std::string& url, const api::Value& body, const api::Value& headers,
                  net::HttpRequest& req, std::string& error) {
    req.method = method;
    req.url = url;
    if (!net::checkUrl(url, &error)) return false;
    bool has_type = false;
    for (const auto& [name, v] : headers.fields()) {
        const std::string value = v.isString()   ? v.asString()
                                  : v.isNumber() ? std::to_string(static_cast<long long>(v.asNumber()))
                                                 : std::string();
        if (!net::validHeader(name, value)) {
            error = "cabecera no valida: " + name;
            return false;
        }
        if (lowerText(name) == "content-type") has_type = true;
        req.headers.emplace_back(name, value);
    }
    if (body.isString()) {
        req.body = body.asString();
        if (!has_type) req.headers.emplace_back("Content-Type", "text/plain; charset=utf-8");
    } else if (body.isObject() || body.isArray()) {
        json j;
        if (!valueToJson(body, j, error)) return false;
        req.body = j.dump();
        if (!has_type) req.headers.emplace_back("Content-Type", "application/json");
    } else if (!body.isNil()) {
        error = "el cuerpo debe ser texto o una tabla (se manda como JSON)";
        return false;
    }
    return true;
}

// Las cabeceras: un objeto {nombre = valor} (nil = ninguna).
const api::Value& headersArg(const api::Call& c, std::size_t i) { return c.arg(i).isObject() ? c.arg(i) : api::Value::nil(); }

// La manda y devuelve su id.
api::Value start(Runtime& rt, HttpState& s, net::HttpRequest req, const api::Value& callback) {
    if (!rt.http) rt.http = std::make_unique<net::HttpClient>();
    const std::uint32_t id = rt.http->send(std::move(req));
    if (callback.isFunction()) s.callbacks[id] = callback;
    return api::Value(id);
}

// "nil, motivo": el motivo va a la consola.
api::Value failed(Runtime& rt, const std::string& where, const std::string& error) {
    rt.write(2, where + ": " + error);
    return {};
}

// Respuestas que llegaron: se llama a su funcion.
void pollHttp(Runtime& rt, HttpState& s) {
    if (!rt.http) return;
    for (auto& [id, response] : rt.http->poll()) {
        const auto it = s.callbacks.find(id);
        if (it == s.callbacks.end()) continue;
        const api::Value fn = std::move(it->second);
        s.callbacks.erase(it);
        rt.native.invoke(fn, {responseValue(response)});
    }
}

}  // namespace

void registerHttpApi(Runtime& rt) {
    auto state = std::make_shared<HttpState>();
    // Despues de la red (registrada antes, en registerNetworkApi).
    rt.onFrame(Phase::Begin, [&rt, state](float) { pollHttp(rt, *state); });
    rt.onStop([state](bool) { state->callbacks.clear(); });

    api::NativeApi& n = rt.native;
    // Http.get(url, function(res) end[, cabeceras])
    n.function("Http.get", [&rt, state](api::Call& c) {
        const std::string url = c.string(0);
        net::HttpRequest req;
        std::string error;
        if (!buildRequest("GET", url, api::Value::nil(), headersArg(c, 2), req, error)) return failed(rt, "Http.get", error);
        return start(rt, *state, std::move(req), c.arg(1));
    }, {"\"https://...\", function(res) end, cabeceras", "GET; res = {ok, status, body, data, headers, error}",
        "id de la peticion (nil si no vale)"});
    // Http.post(url, cuerpo, function(res) end[, cabeceras]): cuerpo texto o tabla (JSON).
    n.function("Http.post", [&rt, state](api::Call& c) {
        const std::string url = c.string(0);
        net::HttpRequest req;
        std::string error;
        if (!buildRequest("POST", url, c.arg(1), headersArg(c, 3), req, error)) return failed(rt, "Http.post", error);
        return start(rt, *state, std::move(req), c.arg(2));
    }, {"\"https://...\", datos, function(res) end, cabeceras", "POST; datos texto o tabla (se manda como JSON)",
        "id de la peticion (nil si no vale)"});
    // Http.request{ url=, method=, headers=, body=, timeout= (s), maxSize= (bytes) }, function(res) end
    n.function("Http.request", [&rt, state](api::Call& c) {
        const api::Value& options = c.object(0);
        const api::Value& m = options["method"];
        std::string method = m.isString() ? m.asString() : std::string("GET");
        for (char& ch : method) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        const api::Value& u = options["url"];
        const std::string url = u.isString() ? u.asString() : std::string();
        const api::Value& h = options["headers"];
        net::HttpRequest req;
        std::string error;
        if (!buildRequest(method, url, options["body"], h.isObject() ? h : api::Value::nil(), req, error)) {
            return failed(rt, "Http.request", error);
        }
        req.timeout_ms = static_cast<int>(options["timeout"].asNumber(20.0) * 1000.0);
        const double max_size = options["maxSize"].asNumber(32.0 * 1024.0 * 1024.0);
        req.max_response_bytes = static_cast<std::size_t>(std::clamp(max_size, 1024.0, 512.0 * 1024.0 * 1024.0));
        return start(rt, *state, std::move(req), c.arg(1));
    }, {"{ url = \"https://...\", method = \"PUT\", headers = {}, body = {}, timeout = 20 }, function(res) end",
        "cualquier metodo, con tiempo maximo y tamano maximo (maxSize)", "id de la peticion (nil si no vale)"});
    n.function("Http.cancelAll", [&rt, state](api::Call&) {
        if (rt.http) rt.http->cancelAll();
        state->callbacks.clear();
        return api::Value{};
    }, {"", "cancela todas"});
    n.function("Http.pending", [&rt](api::Call&) { return api::Value(rt.http ? rt.http->pending() : 0); },
               {"", "peticiones sin terminar", "numero"});
    n.function("Http.urlEncode", [](api::Call& c) { return api::Value(net::urlEncode(c.string(0))); },
               {"\"hola mundo\"", "texto seguro para una URL", "texto"});
    // Http.query{ q = "hola mundo", page = 2 } -> "page=2&q=hola%20mundo" (claves en orden).
    n.function("Http.query", [](api::Call& c) {
        std::map<std::string, std::string> sorted;
        for (const auto& [key, v] : c.object(0).fields()) {
            std::string value;
            if (v.isString()) {
                value = v.asString();
            } else if (v.isBool()) {
                value = v.truthy() ? "true" : "false";
            } else if (v.isNumber()) {
                std::ostringstream o;
                o << v.asNumber();
                value = o.str();
            } else {
                continue;
            }
            sorted[key] = value;
        }
        std::string out;
        for (const auto& [k, v] : sorted) {
            if (!out.empty()) out += '&';
            out += net::urlEncode(k) + "=" + net::urlEncode(v);
        }
        return api::Value(out);
    }, {"{ q = \"hola\", page = 2 }", "\"page=2&q=hola\" (codificado)", "texto"});

    // --- Json ---
    n.function("Json.encode", [&rt](api::Call& c) {
        json j;
        std::string error;
        if (!valueToJson(c.arg(0), j, error)) {
            rt.write(2, "Json.encode: " + error);
            return api::Value{};
        }
        return api::Value(j.dump(c.boolean(1, false) ? 2 : -1, ' ', false, json::error_handler_t::replace));
    }, {"tabla, bonito", "tabla -> texto JSON (nil y un error en la consola si no se puede)", "texto"});
    n.function("Json.decode", [&rt](api::Call& c) {
        const json j = json::parse(c.string(0), nullptr, false);
        if (j.is_discarded()) {
            rt.write(1, "Json.decode: el texto no es JSON valido");
            return api::Value{};
        }
        return jsonToValue(j);
    }, {"texto", "texto JSON -> tabla (nil y un aviso si no es JSON)", "valor"});
}

}  // namespace cramion::scripting::native
