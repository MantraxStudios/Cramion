// Valor <-> JSON (Json.encode/decode, Http, WebSocket). Antes en HttpApi.cpp.
//
// Una lista es un array y un objeto, un objeto JSON; un Vec3 se escribe como
// {x, y, z}. Al leer, null = nil.

#include "JsonValue.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace cramion::scripting::native {

using json = nlohmann::json;

// Valor -> JSON. Numeros enteros como enteros; Vec3 = {x, y, z}. false +
// motivo si hay algo que no se puede escribir (funciones, entidades...).
bool valueToJson(const api::Value& v, json& out, std::string& error, int depth) {
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
api::Value jsonToValue(const json& j, int depth) {
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

}  // namespace cramion::scripting::native
