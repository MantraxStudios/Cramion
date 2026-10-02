#ifndef CRAMION_CORE_CPP_SCRIPT_IPC_H
#define CRAMION_CORE_CPP_SCRIPT_IPC_H

// Canal entre el motor y CramionScriptHost.exe (el proceso aparte donde corre
// la DLL de los scripts de C++): un bloque de memoria compartida y dos eventos
// de Windows. Va por turnos: quien escribe pone `turn` al otro lado y lo
// despierta. Los mensajes son los de cramion/CppProtocol.h.
//
// Motor -> proceso: Load, Create, Destroy, Call, Quit. El proceso contesta
// Done; mientras ejecuta un script, sus llamadas al motor van como Rpc y el
// motor contesta RpcReply.
//
// Si el proceso muere (fallo de un script que no se pudo atrapar), antes deja
// en `crash` lo que paso; el motor lo lee, desactiva el script y lo reinicia.

#include <atomic>
#include <cstdint>
#include <string>

namespace cramion::cppipc {

constexpr std::uint32_t kDataSize = 4u << 20;  // 4 MB por mensaje

enum class Op : std::uint32_t {
    Load = 1,     // str ruta de la DLL, u32 sesion -> Done: u32 estado, str error, u32 n, str clase x n
    Create = 2,   // u32 id, str clase, u64 entidad, u32 n, (str clave, str valor) x n -> Done
    Destroy = 3,  // u32 id -> Done
    Call = 4,     // u32 id, u32 evento, EventArgs -> Done
    Quit = 5,
    Describe = 6,  // -> Done: str JSON con las clases, su archivo y sus propiedades
    Callback = 7,  // u64 id, str JSON con los argumentos -> Done (un callback que el script dio a la API)
    Message = 8,   // u32 id, str metodo, str JSON del valor -> Done (botones de UI: on_click "OnJugar"...)
    Snapshot = 9,  // u32 id -> Done: str JSON {"props":{...},"state":...} (recarga en caliente)
    Restore = 10,  // u32 id, str JSON de Snapshot -> Done (propiedades + onAfterReload(state))
    Rpc = 100,       // proceso -> motor: u32 rpc, datos
    RpcReply = 101,  // motor -> proceso: la respuesta
    Done = 200,      // proceso -> motor: u32 estado, str error
};

enum class Status : std::uint32_t {
    Ok = 0,
    Exception = 1,  // excepcion de C++ (std::exception o lo que sea)
    Crash = 2,      // fallo de hardware atrapado (acceso invalido, division por 0, pila llena...)
    Missing = 3,    // no hay esa clase / instancia, o la DLL no carga
};

struct Block {
    std::atomic<std::uint32_t> turn;  // 0 nadie, 1 le toca al proceso, 2 le toca al motor
    std::uint32_t op;
    std::uint32_t size;
    std::uint32_t host_ready;  // el proceso arranco y escucha
    char crash[4096];          // lo ultimo que paso antes de morir (texto)
    std::uint8_t data[kDataSize];
};

inline std::string blockName(std::uint32_t engine_pid, std::uint32_t serial) {
    return "Local\\CramionScripts_" + std::to_string(engine_pid) + "_" + std::to_string(serial);
}

}  // namespace cramion::cppipc

#endif  // CRAMION_CORE_CPP_SCRIPT_IPC_H
