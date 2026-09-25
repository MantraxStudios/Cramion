// Prueba del autocompletado de Lua (consola). Devuelve 0 si todo va.
#include "../src/LuaCompletion.h"

#include <cstdio>
#include <string>

using namespace cramion::editor;

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "OK   " : "FALLO", what);
    if (!ok) ++failures;
}
bool has(const std::vector<LuaCompletion>& list, const std::string& label) {
    for (const auto& c : list) if (c.label == label) return true;
    return false;
}
std::vector<LuaCompletion> at(const std::string& text) {
    LuaCompletionContext ctx;
    if (!luaCompletionContext(text, text.size(), ctx)) return {};
    return luaCompletions(text, ctx);
}

int main() {
    const std::string head = "local J = { properties = { velocidad = 5, salto = 2 } }\nfunction J:Update(dt)\n    ";
    check(has(at(head + "Inp"), "Input"), "globales: Inp -> Input");
    check(has(at(head + "Input."), "getKey") && has(at(head + "Input.getA"), "getAxis"), "Input. -> getKey, getAxis");
    check(has(at(head + "self."), "velocidad") && has(at(head + "self."), "entity"), "self. -> propiedades y entity");
    check(has(at(head + "self.entity:"), "translate") && has(at(head + "self.entity:add"), "addForce"),
          "self.entity: -> metodos de Entity");
    check(has(at(head + "self.entity."), "position"), "self.entity. -> propiedades de Entity");
    check(has(at(head + "Vec3."), "up"), "Vec3. -> up");
    check(has(at("local C = {}\nfunction C:"), "OnCollisionEnter"), "function C: -> metodos del motor");
    check(at(head + "-- Inp").empty(), "nada dentro de un comentario");
    check(has(at(head + "fu"), "function"), "palabras clave");
    const auto list = at(head + "Scene.f");
    check(!list.empty() && list[0].label.rfind("f", 0) == 0, "primero lo que empieza igual");
    std::printf("\n%d fallos\n", failures);
    return failures == 0 ? 0 : 1;
}
