// Test: las pruebas automaticas del juego.
//
// Las pruebas son scripts de C++ (CRAMION_TEST, sdk/cramion/Test.h) que se
// compilan con los del proyecto. El script CramionTests, puesto en un objeto
// en Play, las ejecuta en orden y cuenta aqui lo que pasa:
//
//   Test.begin("nombre")         empieza un caso (termina el anterior)
//   Test.check(ok, "mensaje")    una comprobacion (false = fallo con ese mensaje)
//   Test.log("texto")            una linea en la salida del caso
//   Test.finish()                termina el caso
//   Test.done()                  ya no hay mas casos
//   Test.results()               {done, passed, failed, cases = {{name, passed,
//                                 assertions, failures, messages, seconds,
//                                 failure (el primer fallo), file}...}}
//
// Los resultados se borran al empezar Play. El ejecutor de pruebas del editor
// (y --run-tests) los lee con ScriptSystem::loadTestFile/testCall/testStep
// (TestHost): avanza los frames de Play hasta que done sea true.

#include "Modules.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cramion::scripting::native {
namespace {

constexpr const char* kDefaultClass = "CramionTests";

struct TestCase {
    std::string name;
    std::string file;  // el .cpp (si el SDK lo dice)
    int assertions = 0;
    int failures = 0;
    std::vector<std::string> messages;  // fallos y Test.log, en orden
    std::string failure;                // el primer fallo
    float start = 0.0f;                 // Time.time al empezar
    float seconds = 0.0f;
    bool open = false;
};

struct TestState {
    Runtime* rt = nullptr;
    std::vector<TestCase> cases;
    bool done = false;
    // Lo que pide el ejecutor (TestHost).
    std::string file;                  // loadTestFile (vacio = todas)
    std::string cls = kDefaultClass;   // la clase que ejecuta las pruebas
    bool aborted = false;
    float elapsed = 0.0f;              // testStep

    void reset() {
        cases.clear();
        done = false;
        aborted = false;
        elapsed = 0.0f;
    }
    TestCase* current() { return !cases.empty() && cases.back().open ? &cases.back() : nullptr; }
    void close(TestCase& c) {
        if (!c.open) return;
        c.open = false;
        c.seconds = rt->time - c.start;
    }
    TestCase& begin(std::string name, std::string file_name) {
        if (TestCase* c = current()) close(*c);
        TestCase c;
        c.name = std::move(name);
        c.file = std::move(file_name);
        c.start = rt->time;
        c.open = true;
        cases.push_back(std::move(c));
        return cases.back();
    }
    // Las comprobaciones sin Test.begin van a un caso sin nombre.
    TestCase& active() {
        if (TestCase* c = current()) return *c;
        return begin("(sin caso)", {});
    }
    int passed() const {
        int n = 0;
        for (const TestCase& c : cases) n += c.failures == 0;
        return n;
    }
    int failed() const { return static_cast<int>(cases.size()) - passed(); }
    int assertions() const {
        int n = 0;
        for (const TestCase& c : cases) n += c.assertions;
        return n;
    }
    // El primer fallo ("caso: mensaje"), vacio si no hay.
    std::string firstFailure() const {
        for (const TestCase& c : cases) {
            if (c.failures > 0) return c.name + ": " + c.failure;
        }
        return {};
    }

    api::Value caseValue(const TestCase& c) const {
        api::Value v = api::Value::object();
        v.set("name", c.name);
        if (!c.file.empty()) v.set("file", c.file);
        v.set("passed", c.failures == 0);
        v.set("assertions", c.assertions);
        v.set("failures", c.failures);
        api::Value::Array messages;
        for (const std::string& m : c.messages) messages.emplace_back(m);
        v.set("messages", api::Value(std::move(messages)));
        v.set("seconds", c.open ? rt->time - c.start : c.seconds);
        v.set("failure", c.failure);
        return v;
    }
    api::Value casesValue() const {
        api::Value::Array list;
        for (const TestCase& c : cases) list.push_back(caseValue(c));
        return api::Value(std::move(list));
    }
    api::Value results() const {
        api::Value v = api::Value::object();
        v.set("done", done);
        v.set("passed", passed());
        v.set("failed", failed());
        v.set("cases", casesValue());
        return v;
    }
    // Todas las lineas de salida ("caso: linea"), para el ejecutor.
    api::Value logValue() const {
        api::Value::Array lines;
        for (const TestCase& c : cases) {
            for (const std::string& m : c.messages) lines.emplace_back(c.name + ": " + m);
        }
        return api::Value(std::move(lines));
    }
    std::string json(const api::Value& v) const { return rt->native.toJson(v).dump(); }
};

std::string joinArgs(const api::Call& c, std::size_t from) {
    std::string text;
    for (std::size_t i = from; i < c.count(); ++i) {
        if (i > from) text += " ";
        text += c.arg(i).asString();
    }
    return text;
}

// El ejecutor de pruebas del editor y de --run-tests.
class NativeTestHost final : public TestHost {
public:
    explicit NativeTestHost(std::shared_ptr<TestState> state) : s_(std::move(state)) {}

    // Las pruebas ya estan compiladas con los scripts del proyecto: solo se
    // recuerda que ejecutar ("" = todas, un .cpp de Assets o el nombre de la
    // clase que las ejecuta).
    bool load(const std::string& file, std::string* error) override {
        const Runtime& rt = *s_->rt;
        if (!rt.running) {
            if (error != nullptr) *error = "no hay Play";
            return false;
        }
        const std::filesystem::path path = pathFromUtf8(file);
        if (!file.empty() && !path.has_extension()) {
            s_->cls = file;
            return true;
        }
        if (!file.empty()) {
            std::error_code e;
            const std::filesystem::path full = path.is_absolute() ? path : rt.root / path;
            if (!std::filesystem::exists(full, e)) {
                if (error != nullptr) *error = "no existe " + file;
                return false;
            }
        }
        s_->file = file;
        return true;
    }

    std::string call(const std::string& operation, const std::string& /*arg*/) override {
        std::string op = operation;
        // Los nombres de antes (__cramion_test_list, __cramion_scene_status...).
        for (const char* prefix : {"__cramion_test_", "__cramion_scene_"}) {
            const std::string p = prefix;
            if (op.rfind(p, 0) == 0) op = op.substr(p.size());
        }
        TestState& s = *s_;
        if (op == "results") return s.json(s.results());
        if (op == "cases" || op == "list") return s.json(s.casesValue());
        if (op == "done") return s.done ? "true" : "false";
        if (op == "class") return s.cls;
        if (op == "file") return s.file;
        // Todas se ejecutan en Play: no hay pruebas "de edicion".
        if (op == "run_edit") return "[]";
        // Una ejecucion nueva (los resultados de antes se olvidan).
        if (op == "begin") {
            s.reset();
            return "ok";
        }
        if (op == "abort") {
            s.aborted = true;
            return {};
        }
        if (op == "reset" || op == "reset_cases") {
            s.reset();
            return {};
        }
        if (op == "status") {
            api::Value v = api::Value::object();
            v.set("result", s.done ? (s.failed() == 0 ? "passed" : "failed") : "");
            v.set("message", s.firstFailure());
            v.set("assertions", s.assertions());
            v.set("failures", s.failed());
            v.set("log", s.logValue());
            return s.json(v);
        }
        return {};
    }

    // {state = running | passed | failed, message, assertions, passed, failed,
    //  cases, log, elapsed}. Los frames de Play los avanza quien llama.
    std::string step(float delta_seconds) override {
        TestState& s = *s_;
        s.elapsed += delta_seconds;
        std::string state = "running";
        std::string message;
        if (s.aborted) {
            state = "failed";
            message = "cancelada";
        } else if (s.done) {
            message = s.firstFailure();
            state = message.empty() ? "passed" : "failed";
        }
        api::Value v = api::Value::object();
        v.set("state", state);
        v.set("message", message);
        v.set("assertions", s.assertions());
        v.set("passed", s.passed());
        v.set("failed", s.failed());
        v.set("cases", s.casesValue());
        v.set("log", s.logValue());
        v.set("elapsed", s.elapsed);
        return s.json(v);
    }

private:
    std::shared_ptr<TestState> s_;
};

}  // namespace

void registerTestApi(Runtime& rt) {
    auto state = std::make_shared<TestState>();
    state->rt = &rt;
    rt.tests = std::make_shared<NativeTestHost>(state);

    // Cada Play empieza sin resultados.
    rt.onStart([state] {
        state->reset();
        state->file.clear();
        state->cls = kDefaultClass;
    });

    rt.native.function("Test.begin",
                       [state](api::Call& c) {
                           state->begin(c.string(0), c.string(1, ""));
                           return api::Value{};
                       },
                       {"\"nombre\", archivo", "empieza un caso de prueba (termina el anterior)"});
    rt.native.function("Test.check",
                       [state, &rt](api::Call& c) {
                           const bool ok = c.arg(0).truthy();
                           TestCase& t = state->active();
                           ++t.assertions;
                           if (!ok) {
                               std::string message = joinArgs(c, 1);
                               if (message.empty()) message = "comprobacion fallida";
                               if (t.failures++ == 0) t.failure = message;
                               t.messages.push_back(message);
                               rt.write(1, "[Test] " + t.name + ": " + message);
                           }
                           return api::Value(ok);
                       },
                       {"ok, \"mensaje\"", "una comprobacion del caso (false = fallo con ese mensaje)", "bool"});
    rt.native.function("Test.finish",
                       [state](api::Call&) {
                           TestCase* t = state->current();
                           if (t == nullptr) return api::Value{};
                           state->close(*t);
                           return api::Value(t->failures == 0);
                       },
                       {"", "termina el caso (true si fue bien)", "bool"});
    rt.native.function("Test.done",
                       [state, &rt](api::Call&) {
                           if (TestCase* t = state->current()) state->close(*t);
                           if (!state->done) {
                               rt.write(0, "[Test] " + std::to_string(state->passed()) + " bien, " +
                                               std::to_string(state->failed()) + " mal");
                           }
                           state->done = true;
                           return api::Value{};
                       },
                       {"", "ya terminaron todos los casos"});
    rt.native.function("Test.log",
                       [state, &rt](api::Call& c) {
                           const std::string text = joinArgs(c, 0);
                           if (TestCase* t = state->current()) t->messages.push_back(text);
                           else if (!state->cases.empty()) state->cases.back().messages.push_back(text);
                           rt.write(0, "[Test] " + text);
                           return api::Value{};
                       },
                       {"\"texto\"", "una linea en la salida del caso"});
    rt.native.function("Test.results", [state](api::Call&) { return state->results(); },
                       {"", "{done, passed, failed, cases = {name, passed, assertions, failures, messages, seconds}}", "objeto"});
}

}  // namespace cramion::scripting::native
