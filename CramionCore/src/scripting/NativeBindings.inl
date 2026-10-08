// La API de scripting en C++ (NativeApi.h): las tablas ya migradas desde
// sol2. Cada una esta disponible para los scripts de C++ (bridgeCall) y, de
// momento, tambien para Lua (NativeLuaAdapter.inl). Ver PLAN-SIN-LUA.md.
//
// Se incluye al final de Scripting.cpp (necesita ScriptSystem::Impl).

namespace {

// Los argumentos como un texto, separados por espacios (Debug.log("a", 1) -> "a 1").
std::string joinArgs(const api::Call& c) {
    std::string text;
    for (std::size_t i = 0; i < c.count(); ++i) {
        if (i > 0) text += " ";
        text += c.arg(i).asString();
    }
    return text;
}

}  // namespace

void ScriptSystem::Impl::registerNativeApi() {
    // --- Debug ---
    native.function("Debug.log", [this](api::Call& c) { write(0, joinArgs(c)); return api::Value{}; },
                    {"...", "Escribe en la consola"});
    native.function("Debug.warn", [this](api::Call& c) { write(1, joinArgs(c)); return api::Value{}; },
                    {"...", "Escribe un aviso en la consola"});
    native.function("Debug.error", [this](api::Call& c) { write(2, joinArgs(c)); return api::Value{}; },
                    {"...", "Escribe un error en la consola"});
    native.function("print", [this](api::Call& c) { write(0, joinArgs(c)); return api::Value{}; },
                    {"...", "Como Debug.log"});
}
