#ifndef CRAMION_TESTS_NATIVE_API_TEST_H
#define CRAMION_TESTS_NATIVE_API_TEST_H

// Ayudas para las pruebas de los modulos de la API de scripting (una prueba
// por modulo: tests/native/<Modulo>Tests.cpp, cada una con su main). Sin GPU
// ni ventana: un mundo, el ScriptSystem en Play y las llamadas a la API como
// las hace un script de C++ (el mismo JSON) o directas.
//
//   ApiFixture t;                       // mundo + ScriptSystem.start
//   auto e = t.world.create("Caja");
//   check(t.call("Entity:translate", {Value(...)}, t.entity(e)).isNil(), "...");
//   json r = t.bridge({{"fn", "Audio.playOneShot"}, {"args", {"clic.wav"}}});

#include "CramionCore/ecs/World.h"
#include "CramionCore/scripting/NativeApi.h"
#include "CramionCore/scripting/Scripting.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace cramion::apitest {

using scripting::api::Value;
using json = nlohmann::json;

inline int g_failures = 0;
inline int g_checks = 0;

inline void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++g_failures;
}

inline int finish() {
    std::printf("\n%d comprobaciones, %d fallos\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

struct ApiFixture {
    ecs::World world;
    scripting::ScriptSystem scripts;
    std::vector<std::pair<int, std::string>> log;  // (nivel, mensaje)

    explicit ApiFixture(bool start = true) {
        scripting::registerScriptComponents();
        scripts.setLog([this](int level, const std::string& message) { log.emplace_back(level, message); });
        if (start) scripts.start(world);
    }
    ~ApiFixture() { scripts.stop(); }

    scripting::api::NativeApi& api() { return scripts.nativeApi(); }
    static Value entity(const ecs::Entity& e) { return Value::entity(e.handle()); }

    // Directo (lanza scripting::api::Error si falla).
    Value call(const std::string& key, const Value::Array& args = {}, const Value& self = Value::nil()) {
        return api().call(key, args, self);
    }
    Value get(const std::string& key, const Value& self = Value::nil()) { return api().get(key, self); }
    void set(const std::string& key, const Value& v, const Value& self = Value::nil()) { api().set(key, v, self); }

    // Por el puente de los scripts de C++ (JSON del SDK).
    json bridge(const json& request) { return json::parse(scripts.bridgeCall(request.dump())); }

    // Un frame del juego.
    void frame(float dt = 1.0f / 60.0f) { scripts.update(world, dt); }

    bool logged(const std::string& part) const {
        for (const auto& [level, message] : log) {
            if (message.find(part) != std::string::npos) return true;
        }
        return false;
    }
};

}  // namespace cramion::apitest

#endif  // CRAMION_TESTS_NATIVE_API_TEST_H
