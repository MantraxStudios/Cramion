#ifndef CRAMION_EDITOR_CODE_COMPLETION_H
#define CRAMION_EDITOR_CODE_COMPLETION_H

// Autocompletado del editor de codigo: las sugerencias que da clangd para los
// scripts de C++ (EditorScripting.cpp las pide) y la documentacion de la API
// del motor (sale del registro: ScriptSystem::apiRegistry).

#include <cstddef>
#include <string>
#include <vector>

namespace cramion::editor {

struct CodeCompletion {
    std::string label;   // lo que se ve (y se filtra)
    std::string insert;  // lo que se escribe (puede llevar parentesis)
    std::string detail;  // firma / descripcion
    int kind = 0;        // 0 palabra clave, 1 tipo/espacio, 2 funcion, 3 propiedad, 4 variable, 5 del motor
};

// Lo que se esta escribiendo: donde empieza y el texto.
struct CodeCompletionContext {
    std::size_t prefix_start = 0;
    std::string prefix;
};

// La lista filtrada por lo escrito (sin mayusculas): primero las que empiezan
// igual, luego las que lo contienen, en el orden de `raw`; como mucho 200.
std::vector<CodeCompletion> filterCompletions(const std::vector<CodeCompletion>& raw, const std::string& prefix);

// La documentacion de "Tabla.funcion" o "Entity:metodo" (tambien
// "Entity.propiedad"): los argumentos ("clip, volumen"), la descripcion y si
// es una funcion. false si no esta en la API.
bool apiDoc(const std::string& callee, std::string& args, std::string& description, bool& function);

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_CODE_COMPLETION_H
