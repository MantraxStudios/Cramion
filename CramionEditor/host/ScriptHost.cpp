// CramionScriptHost.exe: el proceso donde corren los scripts de C++.
//
// El motor lo arranca al entrar en Play (o al empezar el juego) con el nombre
// del bloque de memoria compartida y su PID. Aqui se carga la DLL de los
// scripts y se ejecutan sus eventos; cada llamada va protegida: un acceso a
// memoria invalido, una division por cero, la pila llena o una excepcion de
// C++ se atrapan y se devuelven como error de ese script (con el archivo y la
// linea si la DLL tiene simbolos). Si aun asi el proceso muere, deja escrito
// el motivo en el bloque y el motor sigue (el motor no comparte memoria con
// los scripts: no puede corromperse).
//
//   CramionScriptHost.exe <bloque> <pid del motor>

#include <CramionCore/scripting/CppScriptIpc.h>
#include <cramion/CppProtocol.h>

#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <malloc.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace cramion;
namespace proto = cramion::cppproto;
namespace ipc = cramion::cppipc;

ipc::Block* g_block = nullptr;
HANDLE g_to_host = nullptr;
HANDLE g_to_engine = nullptr;
HANDLE g_engine = nullptr;
HMODULE g_dll = nullptr;
std::vector<std::uint8_t> g_reply;
const char* g_current = "";  // lo que se esta ejecutando (para el informe de un fallo)

// --- Exportaciones de la DLL ---
using AbiFn = std::uint32_t (*)();
using InitFn = void (*)(const proto::Api*, std::uint64_t);
using CountFn = int (*)();
using NameFn = const char* (*)(int);
using CreateFn = void* (*)(const char*, std::uint64_t, const char*, char*, int);
using DescribeFn = int (*)(char*, int, char*, int);
using CallbackFn = int (*)(std::uint64_t, const char*, char*, int);
using MessageFn = int (*)(void*, const char*, const char*, char*, int);
using CallFn = int (*)(void*, std::uint32_t, const proto::EventArgs*, char*, int);
using DestroyFn = void (*)(void*);
CreateFn g_create = nullptr;
CallFn g_call = nullptr;
DestroyFn g_destroy = nullptr;
DescribeFn g_describe = nullptr;
CallbackFn g_callback = nullptr;
MessageFn g_message = nullptr;  // opcional

std::unordered_map<std::uint32_t, void*> g_instances;

// --- Turnos ---
void waitTurn(std::uint32_t mine) {
    for (int spin = 0;; ++spin) {
        if (g_block->turn.load(std::memory_order_acquire) == mine) return;
        if (spin < 2000) {
            YieldProcessor();
            continue;
        }
        HANDLE handles[2] = {g_to_host, g_engine};
        const DWORD r = WaitForMultipleObjects(2, handles, FALSE, 100);
        if (r == WAIT_OBJECT_0 + 1) ExitProcess(0);  // el motor se fue
    }
}

void send(ipc::Op op, const std::uint8_t* data, std::size_t size) {
    if (size > ipc::kDataSize) size = ipc::kDataSize;
    if (size > 0) std::memcpy(g_block->data, data, size);
    g_block->op = static_cast<std::uint32_t>(op);
    g_block->size = static_cast<std::uint32_t>(size);
    g_block->turn.store(2, std::memory_order_release);
    SetEvent(g_to_engine);
}

void sendDone(ipc::Status status, const std::string& error, const proto::Writer* extra = nullptr) {
    proto::Writer w;
    w.u32(static_cast<std::uint32_t>(status)).str(error);
    if (extra != nullptr) w.raw(extra->bytes.data(), extra->bytes.size());
    send(ipc::Op::Done, w.bytes.data(), w.bytes.size());
}

// La puerta de la DLL al motor: un mensaje y se espera la respuesta.
std::uint32_t hostRpc(std::uint32_t op, const void* data, std::uint32_t size, const void** reply) {
    proto::Writer w;
    w.u32(op).raw(data, size);
    send(ipc::Op::Rpc, w.bytes.data(), w.bytes.size());
    waitTurn(1);
    g_reply.assign(g_block->data, g_block->data + g_block->size);
    if (reply != nullptr) *reply = g_reply.data();
    return static_cast<std::uint32_t>(g_reply.size());
}

const proto::Api g_api{proto::kAbiVersion, &hostRpc};

// --- Fallos ---
struct Fault {
    DWORD code = 0;
    void* address = nullptr;
    ULONG_PTR info[2] = {0, 0};
    DWORD64 frames[12] = {};
    int frame_count = 0;
};
Fault g_fault;
bool g_symbols = false;

const char* codeName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "acceso a memoria invalido";
        case EXCEPTION_STACK_OVERFLOW: return "desbordamiento de pila (recursion infinita?)";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "division entera por cero";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "instruccion ilegal";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "fuera de los limites de un array";
        case EXCEPTION_IN_PAGE_ERROR: return "error de pagina";
        case EXCEPTION_PRIV_INSTRUCTION: return "instruccion privilegiada";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "division de coma flotante por cero";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "dato desalineado";
        case 0xE06D7363: return "excepcion de C++ sin atrapar";
        case 0xC0000409: return "comprobacion de seguridad (pila corrompida / abort)";
        case 0xC0000374: return "heap corrompido";
        default: return "excepcion";
    }
}

// Donde: modulo+desplazamiento y, con simbolos, funcion y archivo:linea.
std::string where(DWORD64 address) {
    char text[600];
    HMODULE module = nullptr;
    char module_name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &module)) {
        char full[MAX_PATH];
        if (GetModuleFileNameA(module, full, MAX_PATH) > 0) {
            const char* slash = std::strrchr(full, '\\');
            std::snprintf(module_name, sizeof(module_name), "%s", slash != nullptr ? slash + 1 : full);
        }
    }
    const unsigned long long offset = module != nullptr ? address - reinterpret_cast<DWORD64>(module) : address;
    std::snprintf(text, sizeof(text), "%s+0x%llx", module_name, offset);
    std::string out = text;
    if (g_symbols) {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol)) out += std::string(" ") + symbol->Name;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD line_displacement = 0;
        if (SymGetLineFromAddr64(GetCurrentProcess(), address, &line_displacement, &line) && line.FileName != nullptr) {
            const char* name = line.FileName;
            for (const char* p = name; *p != 0; ++p) {
                if (*p == '\\' || *p == '/') name = p + 1;
            }
            std::snprintf(text, sizeof(text), " (%s:%lu)", name, static_cast<unsigned long>(line.LineNumber));
            out += text;
        }
    }
    return out;
}

std::string describeFault() {
    std::string out = codeName(g_fault.code);
    char text[160];
    std::snprintf(text, sizeof(text), " [0x%08lX]", static_cast<unsigned long>(g_fault.code));
    out += text;
    if (g_fault.code == EXCEPTION_ACCESS_VIOLATION) {
        const char* kind = g_fault.info[0] == 0 ? "leer" : (g_fault.info[0] == 1 ? "escribir" : "ejecutar");
        std::snprintf(text, sizeof(text), " al %s 0x%llx%s", kind, static_cast<unsigned long long>(g_fault.info[1]),
                      g_fault.info[1] < 0x10000 ? " (puntero nulo)" : "");
        out += text;
    }
    out += " en " + where(reinterpret_cast<DWORD64>(g_fault.address));
    // Pila (sin la propia llamada protegida del proceso).
    int shown = 0;
    for (int i = 1; i < g_fault.frame_count && shown < 6; ++i) {
        const std::string w = where(g_fault.frames[i]);
        if (w.rfind("CramionScriptHost", 0) == 0) break;
        out += "\n    llamado desde " + w;
        ++shown;
    }
    return out;
}

int recordFault(EXCEPTION_POINTERS* ep) {
    g_fault = Fault{};
    g_fault.code = ep->ExceptionRecord->ExceptionCode;
    g_fault.address = ep->ExceptionRecord->ExceptionAddress;
    if (ep->ExceptionRecord->NumberParameters >= 2) {
        g_fault.info[0] = ep->ExceptionRecord->ExceptionInformation[0];
        g_fault.info[1] = ep->ExceptionRecord->ExceptionInformation[1];
    }
    // La pila desde el fallo (no con la pila llena: no queda sitio).
    if (g_fault.code != EXCEPTION_STACK_OVERFLOW && g_symbols) {
        CONTEXT context = *ep->ContextRecord;
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        while (g_fault.frame_count < 12 &&
               StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &frame, &context, nullptr,
                           SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
            if (frame.AddrPC.Offset == 0) break;
            g_fault.frames[g_fault.frame_count++] = frame.AddrPC.Offset;
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// Llamadas protegidas: sin objetos de C++ en esta funcion (SEH no se mezcla
// con su limpieza).
struct Job {
    int kind;  // 0 crear, 1 evento, 2 destruir, 3 describir, 4 callback, 5 mensaje (cls = metodo)
    const char* cls;
    std::uint64_t entity;
    const char* json;
    char* out;
    int out_size;
    void* instance;
    std::uint32_t event;
    const proto::EventArgs* args;
    char* error;
    int error_size;
    void* created;
    int result;
};

void runJob(Job* j) {
    if (j->kind == 0) {
        j->created = g_create(j->cls, j->entity, j->json, j->error, j->error_size);
    } else if (j->kind == 1) {
        j->result = g_call(j->instance, j->event, j->args, j->error, j->error_size);
    } else if (j->kind == 2) {
        g_destroy(j->instance);
    } else if (j->kind == 3) {
        j->result = g_describe(j->out, j->out_size, j->error, j->error_size);
    } else if (j->kind == 5) {
        j->result = g_message != nullptr ? g_message(j->instance, j->cls, j->json, j->error, j->error_size) : 0;
    } else {
        j->result = g_callback(j->entity, j->json, j->error, j->error_size);
    }
}

bool guarded(Job* j) {
    __try {
        runJob(j);
        return true;
    } __except (recordFault(GetExceptionInformation())) {
        if (g_fault.code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
        return false;
    }
}

// Ultimo recurso: lo que no se atrapo (el proceso va a morir).
LONG WINAPI unhandled(EXCEPTION_POINTERS* ep) {
    if (g_block != nullptr) {
        std::snprintf(g_block->crash, sizeof(g_block->crash), "%s [0x%08lX] durante %s", codeName(ep->ExceptionRecord->ExceptionCode),
                      static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode), g_current);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

void onTerminate() {
    if (g_block != nullptr) std::snprintf(g_block->crash, sizeof(g_block->crash), "std::terminate durante %s", g_current);
    ExitProcess(3);
}

void onAbort(int) {
    if (g_block != nullptr) std::snprintf(g_block->crash, sizeof(g_block->crash), "abort() durante %s", g_current);
    ExitProcess(3);
}

void invalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    if (g_block != nullptr) std::snprintf(g_block->crash, sizeof(g_block->crash), "parametro invalido en la CRT durante %s", g_current);
    ExitProcess(3);
}

// --- Comandos ---
void load(proto::Reader& r) {
    const std::string path = r.str();
    const std::uint64_t session = r.u32();
    if (g_dll != nullptr) {
        sendDone(ipc::Status::Missing, "ya hay una DLL cargada");
        return;
    }
    const int wide = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(static_cast<std::size_t>(std::max(wide, 1)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wide);
    g_current = "cargar la DLL (constructores estaticos)";
    g_dll = LoadLibraryW(wpath.c_str());
    if (g_dll == nullptr) {
        sendDone(ipc::Status::Missing, "no se pudo cargar " + path + " (error " + std::to_string(GetLastError()) + ")");
        return;
    }
    const auto abi = reinterpret_cast<AbiFn>(GetProcAddress(g_dll, "cramion_abi"));
    const auto init = reinterpret_cast<InitFn>(GetProcAddress(g_dll, "cramion_init"));
    const auto count = reinterpret_cast<CountFn>(GetProcAddress(g_dll, "cramion_class_count"));
    const auto name = reinterpret_cast<NameFn>(GetProcAddress(g_dll, "cramion_class_name"));
    g_create = reinterpret_cast<CreateFn>(GetProcAddress(g_dll, "cramion_create"));
    g_call = reinterpret_cast<CallFn>(GetProcAddress(g_dll, "cramion_call"));
    g_destroy = reinterpret_cast<DestroyFn>(GetProcAddress(g_dll, "cramion_destroy"));
    g_describe = reinterpret_cast<DescribeFn>(GetProcAddress(g_dll, "cramion_describe"));
    g_callback = reinterpret_cast<CallbackFn>(GetProcAddress(g_dll, "cramion_callback"));
    g_message = reinterpret_cast<MessageFn>(GetProcAddress(g_dll, "cramion_message"));
    if (abi == nullptr || init == nullptr || count == nullptr || name == nullptr || g_create == nullptr || g_call == nullptr ||
        g_destroy == nullptr || g_describe == nullptr || g_callback == nullptr) {
        sendDone(ipc::Status::Missing, "la DLL no es de scripts de Cramion (faltan exportaciones del SDK)");
        return;
    }
    if (abi() != proto::kAbiVersion) {
        sendDone(ipc::Status::Missing, "la DLL se compilo con otra version del SDK: vuelve a compilar los scripts");
        return;
    }
    // Simbolos (archivo y linea en los fallos) si la DLL trae su .pdb.
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
    {
        // Solo los de la DLL de los scripts (los de todo el proceso son lentos);
        // el .pdb esta junto a la DLL.
        char module[MAX_PATH];
        GetModuleFileNameA(g_dll, module, MAX_PATH);
        std::string folder = module;
        folder = folder.substr(0, folder.find_last_of("\\/"));
        if (SymInitialize(GetCurrentProcess(), folder.c_str(), FALSE)) {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_dll);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(g_dll) + dos->e_lfanew);
            g_symbols = SymLoadModuleEx(GetCurrentProcess(), nullptr, module, nullptr, reinterpret_cast<DWORD64>(g_dll),
                                        nt->OptionalHeader.SizeOfImage, nullptr, 0) != 0;
        }
    }
    init(&g_api, session);
    proto::Writer extra;
    const int n = count();
    extra.u32(static_cast<std::uint32_t>(n));
    for (int i = 0; i < n; ++i) extra.str(name(i));
    sendDone(ipc::Status::Ok, {}, &extra);
}

void create(proto::Reader& r) {
    const std::uint32_t id = r.u32();
    const std::string cls = r.str();
    const std::uint64_t entity = r.u64();
    const std::string values = r.str();
    char error[2048] = {};
    std::string what = "crear " + cls;
    g_current = what.c_str();
    Job job{0, cls.c_str(), entity, values.c_str(), nullptr, 0, nullptr, 0, nullptr, error, sizeof(error), nullptr, 0};
    if (!guarded(&job)) {
        sendDone(ipc::Status::Crash, "el constructor fallo: " + describeFault());
        return;
    }
    if (job.created == nullptr) {
        sendDone(error[0] != 0 ? ipc::Status::Exception : ipc::Status::Missing,
                 error[0] != 0 ? std::string(error) : "no hay ninguna clase " + cls + " (CRAMION_SCRIPT)");
        return;
    }
    g_instances[id] = job.created;
    sendDone(ipc::Status::Ok, {});
}

// Las clases y sus propiedades (crea cada una un momento, sin llamar a nada).
void describe() {
    char error[2048] = {};
    g_current = "describir las clases (constructores)";
    std::vector<char> out(1 << 16);
    for (int attempt = 0; attempt < 2; ++attempt) {
        Job job{3, nullptr, 0, nullptr, out.data(), static_cast<int>(out.size()), nullptr, 0, nullptr, error, sizeof(error), nullptr, 0};
        if (!guarded(&job)) {
            sendDone(ipc::Status::Crash, "un constructor fallo al describir: " + describeFault());
            return;
        }
        if (job.result <= static_cast<int>(out.size())) {
            proto::Writer extra;
            extra.str(out.data());
            sendDone(error[0] != 0 ? ipc::Status::Exception : ipc::Status::Ok, error, &extra);
            return;
        }
        out.resize(static_cast<std::size_t>(job.result) + 16);
    }
    sendDone(ipc::Status::Missing, "descripcion demasiado grande");
}

void callback(proto::Reader& r) {
    const std::uint64_t id = r.u64();
    const std::string args = r.str();
    char error[2048] = {};
    g_current = "un callback";
    Job job{4, nullptr, id, args.c_str(), nullptr, 0, nullptr, 0, nullptr, error, sizeof(error), nullptr, 0};
    if (!guarded(&job)) {
        sendDone(ipc::Status::Crash, describeFault());
        return;
    }
    sendDone(job.result != 0 ? ipc::Status::Exception : ipc::Status::Ok, error);
}

void destroy(proto::Reader& r) {
    const std::uint32_t id = r.u32();
    const auto it = g_instances.find(id);
    if (it == g_instances.end()) {
        sendDone(ipc::Status::Ok, {});
        return;
    }
    g_current = "destruir un script";
    Job job{2, nullptr, 0, nullptr, nullptr, 0, it->second, 0, nullptr, nullptr, 0, nullptr, 0};
    void* instance = it->second;
    g_instances.erase(it);
    (void)instance;
    if (!guarded(&job)) {
        sendDone(ipc::Status::Crash, "el destructor fallo: " + describeFault());
        return;
    }
    sendDone(ipc::Status::Ok, {});
}

}  // namespace

int main(int argc, char** argv) {
    // Sin cuadros de dialogo de Windows si algo falla: el motor da el error.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler(invalidParameter);
    std::set_terminate(onTerminate);
    std::signal(SIGABRT, onAbort);
    SetUnhandledExceptionFilter(unhandled);
    if (argc < 3) {
        std::fprintf(stderr, "Uso: CramionScriptHost <bloque> <pid del motor>\n");
        return 2;
    }
    const std::string name = argv[1];
    const DWORD engine_pid = static_cast<DWORD>(std::strtoul(argv[2], nullptr, 10));
    HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
    g_to_host = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, (name + "_h").c_str());
    g_to_engine = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, (name + "_e").c_str());
    g_engine = OpenProcess(SYNCHRONIZE, FALSE, engine_pid);
    if (mapping == nullptr || g_to_host == nullptr || g_to_engine == nullptr || g_engine == nullptr) return 2;
    g_block = static_cast<ipc::Block*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ipc::Block)));
    if (g_block == nullptr) return 2;
    g_block->host_ready = 1;
    SetEvent(g_to_engine);
    while (true) {
        waitTurn(1);
        const auto op = static_cast<ipc::Op>(g_block->op);
        // Copia del mensaje: la respuesta (y las RPC) reescriben el bloque.
        const std::vector<std::uint8_t> message(g_block->data, g_block->data + g_block->size);
        proto::Reader r(message.data(), message.size());
        switch (op) {
            case ipc::Op::Load: load(r); break;
            case ipc::Op::Create: create(r); break;
            case ipc::Op::Call: {
                // EventArgs va tras id y evento, sin cabecera.
                const std::uint32_t id = r.u32();
                const std::uint32_t event = r.u32();
                proto::EventArgs args{};
                if (r.remaining() >= sizeof(args)) std::memcpy(&args, message.data() + 8, sizeof(args));
                const auto it = g_instances.find(id);
                if (it == g_instances.end()) {
                    sendDone(ipc::Status::Missing, "no existe la instancia");
                    break;
                }
                char error[2048] = {};
                g_current = "un evento";
                Job job{1, nullptr, 0, nullptr, nullptr, 0, it->second, event, &args, error, sizeof(error), nullptr, 0};
                if (!guarded(&job)) sendDone(ipc::Status::Crash, describeFault());
                else if (job.result != 0) sendDone(ipc::Status::Exception, error);
                else sendDone(ipc::Status::Ok, {});
                break;
            }
            case ipc::Op::Destroy: destroy(r); break;
            case ipc::Op::Describe: describe(); break;
            case ipc::Op::Callback: callback(r); break;
            case ipc::Op::Message: {
                const std::uint32_t id = r.u32();
                const std::string method = r.str();
                const std::string value = r.str();
                const auto it = g_instances.find(id);
                if (it == g_instances.end()) {
                    sendDone(ipc::Status::Missing, "no existe la instancia");
                    break;
                }
                char error[2048] = {};
                g_current = "un mensaje";
                Job job{5, method.c_str(), 0, value.c_str(), nullptr, 0, it->second, 0, nullptr, error, sizeof(error), nullptr, 0};
                if (!guarded(&job)) sendDone(ipc::Status::Crash, describeFault());
                else if (job.result != 0) sendDone(ipc::Status::Exception, error);
                else sendDone(ipc::Status::Ok, {});
                break;
            }
            case ipc::Op::Quit:
                sendDone(ipc::Status::Ok, {});
                return 0;
            default: sendDone(ipc::Status::Missing, "mensaje desconocido"); break;
        }
    }
}
