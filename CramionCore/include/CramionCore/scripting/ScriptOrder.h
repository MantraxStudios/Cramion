#ifndef CRAMION_CORE_SCRIPTING_SCRIPT_ORDER_H
#define CRAMION_CORE_SCRIPTING_SCRIPT_ORDER_H

// Orden de ejecucion de los scripts de C++ (como Script Execution Order de
// Unity): cada clase tiene una prioridad (menor = antes; 0 por defecto) y, a
// igual prioridad, van en el orden en que se crearon sus instancias. El mismo
// en el editor, el juego y el servidor dedicado (no depende de punteros ni de
// tablas hash).
//
// La prioridad de una clase sale de (lo primero que la diga):
//   1. los ajustes del proyecto: la CVar script.ExecutionOrder
//      ("Jugador=-100; Camara=50"; se guarda en ProjectSettings/CVars.json),
//   2. la clase: static constexpr int kExecutionOrder = -100; (Script.h),
//   3. 0.
// script.cpp.StableOrder = false vuelve al orden de antes (sin garantias).

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cramion::scripting {

// "Jugador=-100; Camara = 50, Enemigo:10" (separado por ';', ',' o saltos de
// linea; '=' o ':'). Lo que no se entiende se salta.
std::unordered_map<std::string, int> parseExecutionOrder(std::string_view text);

// La clave de orden de una instancia: prioridad y despues creacion.
struct ScriptOrderKey {
    int priority = 0;
    std::uint64_t sequence = 0;
    friend bool operator<(const ScriptOrderKey& a, const ScriptOrderKey& b) {
        return a.priority != b.priority ? a.priority < b.priority : a.sequence < b.sequence;
    }
};

}  // namespace cramion::scripting

#endif  // CRAMION_CORE_SCRIPTING_SCRIPT_ORDER_H
