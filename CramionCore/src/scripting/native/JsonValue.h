#ifndef CRAMION_CORE_SCRIPTING_NATIVE_JSONVALUE_H
#define CRAMION_CORE_SCRIPTING_NATIVE_JSONVALUE_H
// Valor <-> JSON (Json.encode/decode, Http, WebSocket). Antes en HttpApi.cpp.
#include "CramionCore/scripting/NativeApi.h"
#include <nlohmann/json.hpp>
#include <string>
namespace cramion::scripting::native {
bool valueToJson(const api::Value& v, nlohmann::json& out, std::string& error, int depth = 0);
api::Value jsonToValue(const nlohmann::json& j, int depth = 0);
}
#endif
