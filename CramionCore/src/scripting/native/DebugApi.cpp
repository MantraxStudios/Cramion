// Debug.log / warn / error y print: escriben en la Consola.

#include "Modules.h"

namespace cramion::scripting::native {
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

void registerDebugApi(Runtime& rt) {
    rt.native.function("Debug.log", [&rt](api::Call& c) { rt.write(0, joinArgs(c)); return api::Value{}; },
                       {"...", "Escribe en la consola"});
    rt.native.function("Debug.warn", [&rt](api::Call& c) { rt.write(1, joinArgs(c)); return api::Value{}; },
                       {"...", "Escribe un aviso en la consola"});
    rt.native.function("Debug.error", [&rt](api::Call& c) { rt.write(2, joinArgs(c)); return api::Value{}; },
                       {"...", "Escribe un error en la consola"});
    rt.native.function("print", [&rt](api::Call& c) { rt.write(0, joinArgs(c)); return api::Value{}; },
                       {"...", "Como Debug.log"});
}

}  // namespace cramion::scripting::native
