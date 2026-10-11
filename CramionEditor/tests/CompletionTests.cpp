// Prueba del autocompletado del editor de codigo (consola): el filtro de la
// lista de clangd y la documentacion de la API (la del registro, la que usan
// los nodos de los Visual Scripts). Devuelve 0 si todo va.
#include "../src/CodeCompletion.h"

#include <CramionCore/scripting/NativeApi.h>
#include <CramionCore/scripting/Scripting.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace cramion::editor;

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "OK   " : "FALLO", what);
    if (!ok) ++failures;
}

std::vector<std::string> labels(const std::vector<CodeCompletion>& list) {
    std::vector<std::string> out;
    for (const CodeCompletion& c : list) out.push_back(c.label);
    return out;
}

int main() {
    std::printf("Filtro de la lista\n");
    std::vector<CodeCompletion> raw;
    for (const char* label : {"setPosition", "position", "Position2D", "lookAt", "worldPosition", "translate"}) {
        CodeCompletion c;
        c.label = label;
        raw.push_back(c);
    }
    const std::vector<std::string> pos = labels(filterCompletions(raw, "pos"));
    check(pos == std::vector<std::string>{"position", "Position2D", "setPosition", "worldPosition"},
          "primero las que empiezan igual (sin mayusculas), luego las que lo contienen, en su orden");
    check(filterCompletions(raw, "").size() == raw.size(), "sin nada escrito: toda la lista");
    check(filterCompletions(raw, "zzz").empty(), "nada coincide: vacia");
    std::vector<CodeCompletion> many(500);
    for (std::size_t i = 0; i < many.size(); ++i) many[i].label = "x" + std::to_string(i);
    check(filterCompletions(many, "x").size() == 200, "como mucho 200");

    std::printf("Documentacion de la API\n");
    std::string args, description;
    bool function = false;
    check(apiDoc("Audio.playOneShot", args, description, function) && function && !description.empty(),
          "Audio.playOneShot: funcion con su descripcion");
    check(apiDoc("Entity.effectParticles", args, description, function) && !function,
          "una propiedad de Entity con '.' (como en los nodos)");
    check(!apiDoc("NoExiste.nada", args, description, function), "lo que no esta en la API: false");
    int documented = 0, total = 0;
    for (const auto& e : cramion::scripting::ScriptSystem::apiRegistry().entries()) {
        ++total;
        documented += e.doc.description.empty() ? 0 : 1;
    }
    std::printf("  (%d de %d entradas de la API con descripcion)\n", documented, total);
    check(total > 500 && documented > total / 2, "la API tiene sus entradas y casi todas documentadas");

    std::printf("\n%d fallos\n", failures);
    return failures == 0 ? 0 : 1;
}
