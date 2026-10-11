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
//
// Linux (CramionScriptHost): el bloque de memoria compartida (descriptor 4) y
// el canal con el motor (un socket, el 3) llegan heredados; la .so de los
// scripts se carga con dlopen. Los fallos de hardware (SIGSEGV, SIGBUS,
// SIGFPE, SIGILL) se atrapan con un manejador de senal en una pila aparte
// (sigaltstack: tambien la pila llena) que vuelve con siglongjmp a la llamada
// protegida; el archivo y la linea los da llvm-symbolizer o addr2line.
// abort() y std::terminate tiran el proceso, como en Windows: queda el motivo
// en el bloque y el motor lo arranca otra vez.

#include <CramionCore/scripting/CppScriptIpc.h>
#include <cramion/CppProtocol.h>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <malloc.h>
#else
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <cerrno>
extern char** environ;
#endif

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
#if defined(_WIN32)
HANDLE g_to_host = nullptr;
HANDLE g_to_engine = nullptr;
HANDLE g_engine = nullptr;
HMODULE g_dll = nullptr;
#else
int g_channel = -1;  // socket con el motor: un byte despierta al otro lado; cerrado = el motor se fue
void* g_dll = nullptr;
std::string g_dll_path;
const void* g_dll_base = nullptr;
#endif
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
using SnapshotFn = int (*)(void*, char*, int, char*, int);
using RestoreFn = int (*)(void*, const char*, char*, int);
CreateFn g_create = nullptr;
CallFn g_call = nullptr;
DestroyFn g_destroy = nullptr;
DescribeFn g_describe = nullptr;
CallbackFn g_callback = nullptr;
MessageFn g_message = nullptr;  // opcional
SnapshotFn g_snapshot = nullptr;  // opcional (recarga en caliente)
RestoreFn g_restore = nullptr;    // opcional

std::unordered_map<std::uint32_t, void*> g_instances;

// --- Turnos ---
#if defined(_WIN32)
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
#else
void relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    sched_yield();
#endif
}

// Despierta al motor (si el canal esta lleno, ya esta despierto).
void wake() {
    const char byte = 1;
    ::send(g_channel, &byte, 1, MSG_DONTWAIT | MSG_NOSIGNAL);
}

void waitTurn(std::uint32_t mine) {
    for (int spin = 0;; ++spin) {
        if (g_block->turn.load(std::memory_order_acquire) == mine) return;
        if (spin < 2000) {
            relax();
            continue;
        }
        pollfd p{g_channel, POLLIN, 0};
        if (poll(&p, 1, 100) <= 0) continue;
        char buffer[64];
        const ssize_t n = recv(g_channel, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) _exit(0);  // el motor se fue
    }
}
#endif

void send(ipc::Op op, const std::uint8_t* data, std::size_t size) {
    if (size > ipc::kDataSize) size = ipc::kDataSize;
    if (size > 0) std::memcpy(g_block->data, data, size);
    g_block->op = static_cast<std::uint32_t>(op);
    g_block->size = static_cast<std::uint32_t>(size);
    g_block->turn.store(2, std::memory_order_release);
#if defined(_WIN32)
    SetEvent(g_to_engine);
#else
    wake();
#endif
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

#if defined(_WIN32)
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
#else
// --- Fallos (senales) ---
struct Fault {
    int signal;
    int code;                // si_code
    std::uintptr_t address;  // el dato al que se accedio (o la instruccion en SIGFPE/SIGILL)
    std::uintptr_t pc;       // la instruccion que fallo (0 si no se sabe)
    int access;              // 0 leer, 1 escribir, 2 ejecutar, -1 no se sabe
    bool overflow;           // la pila llena
    void* frames[16];
    int frame_count;
};
Fault g_fault{};
sigjmp_buf g_jump;
volatile sig_atomic_t g_guarded = 0;  // dentro de una llamada protegida
std::uintptr_t g_stack_low = 0;       // la pila del hilo (para reconocer la pila llena)
std::uintptr_t g_stack_high = 0;
alignas(16) char g_alt_stack[1 << 16];  // donde corre el manejador (con la pila llena no hay otra)

const char* signalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGFPE: return "SIGFPE";
        case SIGILL: return "SIGILL";
        case SIGABRT: return "SIGABRT";
        default: return "senal";
    }
}

const char* faultName(const Fault& f) {
    if (f.overflow) return "desbordamiento de pila (recursion infinita?)";
    switch (f.signal) {
        case SIGSEGV: return "acceso a memoria invalido";
        case SIGBUS: return "error de bus (dato desalineado o archivo mapeado)";
        case SIGILL: return "instruccion ilegal";
        case SIGFPE:
            if (f.code == FPE_INTDIV) return "division entera por cero";
            if (f.code == FPE_FLTDIV) return "division de coma flotante por cero";
            if (f.code == FPE_INTOVF) return "desbordamiento entero";
            return "error aritmetico";
        default: return "excepcion";
    }
}

// El motivo en el bloque, sin reservar memoria (se llama desde un manejador de senal).
void crashText(const char* a, const char* b = "", const char* c = "") {
    if (g_block == nullptr) return;
    std::size_t n = 0;
    for (const char* part : {a, b, c}) {
        for (const char* p = part; p != nullptr && *p != 0 && n + 1 < sizeof(g_block->crash); ++p) g_block->crash[n++] = *p;
    }
    g_block->crash[n] = 0;
}

void onFault(int sig, siginfo_t* info, void* context) {
    if (g_guarded == 0) {
        // Fuera de una llamada protegida (cargar la .so...): el proceso cae
        // con la senal y el motor lee el motivo.
        Fault f{};
        f.signal = sig;
        f.code = info != nullptr ? info->si_code : 0;
        crashText(faultName(f), " durante ", g_current);
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    g_guarded = 0;
    g_fault = Fault{};
    g_fault.signal = sig;
    g_fault.code = info != nullptr ? info->si_code : 0;
    g_fault.address = reinterpret_cast<std::uintptr_t>(info != nullptr ? info->si_addr : nullptr);
    g_fault.access = -1;
#if defined(__linux__) && defined(__x86_64__)
    const auto* uc = static_cast<const ucontext_t*>(context);
    g_fault.pc = static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
    if (sig == SIGSEGV) {
        const auto err = uc->uc_mcontext.gregs[REG_ERR];  // el codigo del fallo de pagina
        g_fault.access = (err & 0x10) != 0 ? 2 : ((err & 0x2) != 0 ? 1 : 0);
    }
#elif defined(__linux__) && defined(__aarch64__)
    g_fault.pc = static_cast<std::uintptr_t>(static_cast<const ucontext_t*>(context)->uc_mcontext.pc);
#else
    (void)context;
#endif
    if (g_fault.pc == 0 && sig != SIGSEGV && sig != SIGBUS) g_fault.pc = g_fault.address;
    // La pila llena: el acceso cae en la zona de la pila o justo debajo (la
    // separacion de guarda del sistema, 1 MB).
    g_fault.overflow = sig == SIGSEGV && g_stack_high != 0 && g_fault.address < g_stack_high &&
                       g_fault.address + (std::uintptr_t(1) << 20) >= g_stack_low;
    // La pila desde el fallo (no con la pila llena: no queda sitio).
    if (!g_fault.overflow) g_fault.frame_count = backtrace(g_fault.frames, 16);
    siglongjmp(g_jump, 1);
}

std::string baseName(const char* path) {
    const char* name = path;
    for (const char* p = path; *p != 0; ++p) {
        if (*p == '/' || *p == '\\') name = p + 1;
    }
    return name;
}

// Ejecuta una herramienta (el simbolizador) y devuelve su salida; false si no esta o falla.
bool runTool(const std::vector<std::string>& args, std::string& output) {
    int out[2];
    if (pipe(out) != 0) return false;
    fcntl(out[0], F_SETFD, FD_CLOEXEC);
    fcntl(out[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    std::vector<std::string> copy = args;
    std::vector<char*> argv;
    for (std::string& a : copy) argv.push_back(a.data());
    argv.push_back(nullptr);
    // Sin el tope de memoria de este proceso (lo heredaria).
    rlimit saved{};
    const bool limited = getrlimit(RLIMIT_DATA, &saved) == 0;
    if (limited) {
        const rlimit open{saved.rlim_max, saved.rlim_max};
        setrlimit(RLIMIT_DATA, &open);
    }
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    if (limited) setrlimit(RLIMIT_DATA, &saved);
    posix_spawn_file_actions_destroy(&actions);
    ::close(out[1]);
    if (rc != 0) {
        ::close(out[0]);
        return false;
    }
    char buffer[4096];
    for (int waited = 0; waited < 5000;) {
        pollfd p{out[0], POLLIN, 0};
        const int r = poll(&p, 1, 100);
        if (r == 0) {
            waited += 100;
            continue;
        }
        const ssize_t n = read(out[0], buffer, sizeof(buffer));
        if (n > 0) output.append(buffer, static_cast<std::size_t>(n));
        else if (n == 0 || errno != EINTR) break;
    }
    ::close(out[0]);
    kill(pid, SIGKILL);  // sin efecto si ya termino (sigue sin recoger)
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && !output.empty();
}

// Funcion y " (archivo:linea)" de cada desplazamiento dentro de la .so de los
// scripts (llvm-symbolizer o addr2line; vacio si no hay ninguno).
std::vector<std::pair<std::string, std::string>> symbolize(const std::vector<std::uintptr_t>& offsets) {
    std::vector<std::pair<std::string, std::string>> out;
    if (offsets.empty() || g_dll_path.empty()) return out;
    std::vector<std::string> hex;
    for (const std::uintptr_t o : offsets) {
        char text[32];
        std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(o));
        hex.push_back(text);
    }
    std::string output;
    std::vector<std::string> args{"llvm-symbolizer", "--obj=" + g_dll_path, "--no-inlines"};
    args.insert(args.end(), hex.begin(), hex.end());
    if (!runTool(args, output)) {
        output.clear();
        args = {"addr2line", "-f", "-C", "-e", g_dll_path};
        args.insert(args.end(), hex.begin(), hex.end());
        if (!runTool(args, output)) return out;
    }
    // Dos lineas por direccion: la funcion y archivo:linea[:columna].
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < output.size()) {
        std::size_t end = output.find('\n', start);
        if (end == std::string::npos) end = output.size();
        std::string line = output.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
        start = end + 1;
    }
    for (std::size_t i = 0; i + 1 < lines.size() && out.size() < offsets.size(); i += 2) {
        std::string function = lines[i] == "??" ? std::string() : lines[i];
        std::string at = lines[i + 1];
        const std::size_t extra = at.find(" (");
        if (extra != std::string::npos) at.erase(extra);  // " (discriminator N)"
        // archivo:linea o archivo:linea:columna
        std::string location;
        const std::size_t last = at.rfind(':');
        if (last != std::string::npos && last > 0) {
            std::size_t colon = last;
            const std::size_t before = at.rfind(':', last - 1);
            const auto digits = [](const std::string& s) { return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos; };
            if (before != std::string::npos && digits(at.substr(before + 1, last - before - 1)) && digits(at.substr(last + 1))) colon = before;
            const std::string file = at.substr(0, colon);
            const std::string line = at.substr(colon + 1, at.find(':', colon + 1) - colon - 1);
            if (file != "??" && digits(line) && line != "0") location = " (" + baseName(file.c_str()) + ":" + line + ")";
        }
        out.emplace_back(function, location);
    }
    return out;
}

// Donde esta cada direccion: modulo+desplazamiento y, en la .so de los
// scripts, funcion y archivo:linea.
std::vector<std::string> whereAll(const std::vector<std::uintptr_t>& addresses) {
    std::vector<std::string> out;
    std::vector<std::string> symbols;
    std::vector<int> slot;
    std::vector<std::uintptr_t> offsets;
    for (const std::uintptr_t address : addresses) {
        Dl_info info{};
        std::string module = "?";
        std::uintptr_t offset = address;
        std::string symbol;
        int index = -1;
        if (dladdr(reinterpret_cast<void*>(address), &info) != 0) {
            if (info.dli_fname != nullptr) module = baseName(info.dli_fname);
            offset = address - reinterpret_cast<std::uintptr_t>(info.dli_fbase);
            if (info.dli_sname != nullptr) {
                int status = 0;
                char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
                symbol = status == 0 && demangled != nullptr ? demangled : info.dli_sname;
                std::free(demangled);
            }
            if (info.dli_fbase == g_dll_base) {
                index = static_cast<int>(offsets.size());
                offsets.push_back(offset);
            }
        }
        char text[300];
        std::snprintf(text, sizeof(text), "%s+0x%llx", module.c_str(), static_cast<unsigned long long>(offset));
        out.push_back(text);
        symbols.push_back(symbol);
        slot.push_back(index);
    }
    const auto lines = symbolize(offsets);
    for (std::size_t i = 0; i < out.size(); ++i) {
        std::string function = symbols[i];
        std::string location;
        if (slot[i] >= 0 && static_cast<std::size_t>(slot[i]) < lines.size()) {
            if (!lines[slot[i]].first.empty()) function = lines[slot[i]].first;
            location = lines[slot[i]].second;
        }
        if (!function.empty()) out[i] += " " + function;
        out[i] += location;
    }
    return out;
}

bool inModule(std::uintptr_t address, const void* base) {
    Dl_info info{};
    return base != nullptr && dladdr(reinterpret_cast<void*>(address), &info) != 0 && info.dli_fbase == base;
}

std::string describeFault() {
    std::string out = faultName(g_fault);
    out += std::string(" [") + signalName(g_fault.signal) + "]";
    if ((g_fault.signal == SIGSEGV || g_fault.signal == SIGBUS) && !g_fault.overflow) {
        const char* kind = g_fault.access == 0 ? "leer" : (g_fault.access == 1 ? "escribir" : (g_fault.access == 2 ? "ejecutar" : "acceder a"));
        char text[160];
        std::snprintf(text, sizeof(text), " al %s 0x%llx%s", kind, static_cast<unsigned long long>(g_fault.address),
                      g_fault.address < 0x10000 ? " (puntero nulo)" : "");
        out += text;
    }
    // La instruccion que fallo y quien la llamo (hasta este proceso).
    Dl_info self{};
    const void* host_base = dladdr(reinterpret_cast<void*>(&describeFault), &self) != 0 ? self.dli_fbase : nullptr;
    std::uintptr_t pc = g_fault.pc;
    int first = -1;
    for (int i = 0; i < g_fault.frame_count && first < 0; ++i) {
        const auto a = reinterpret_cast<std::uintptr_t>(g_fault.frames[i]);
        if (pc != 0 ? a == pc : inModule(a, g_dll_base)) {
            pc = a;
            first = i + 1;
        }
    }
    if (pc == 0) return out;
    std::vector<std::uintptr_t> chain{pc};
    for (int i = first; i >= 0 && i < g_fault.frame_count && chain.size() < 7; ++i) {
        const auto a = reinterpret_cast<std::uintptr_t>(g_fault.frames[i]);
        if (inModule(a, host_base)) break;
        chain.push_back(a - 1);  // la llamada, no la vuelta
    }
    const std::vector<std::string> places = whereAll(chain);
    out += " en " + places[0];
    for (std::size_t i = 1; i < places.size(); ++i) out += "\n    llamado desde " + places[i];
    return out;
}
#endif

// Llamadas protegidas: sin objetos de C++ en esta funcion (SEH no se mezcla
// con su limpieza).
struct Job {
    int kind;  // 0 crear, 1 evento, 2 destruir, 3 describir, 4 callback, 5 mensaje (cls = metodo), 6 snapshot, 7 restaurar
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
    } else if (j->kind == 6) {
        j->result = g_snapshot != nullptr ? g_snapshot(j->instance, j->out, j->out_size, j->error, j->error_size) : 0;
    } else if (j->kind == 7) {
        j->result = g_restore != nullptr ? g_restore(j->instance, j->json, j->error, j->error_size) : 0;
    } else {
        j->result = g_callback(j->entity, j->json, j->error, j->error_size);
    }
}

#if defined(_WIN32)
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
#else
// Un fallo dentro vuelve aqui con siglongjmp (g_fault dice cual). Como en
// Windows, lo que el script tenia a medias se queda sin limpiar: se desactiva.
bool guarded(Job* j) {
    if (sigsetjmp(g_jump, 1) != 0) return false;
    g_guarded = 1;
    runJob(j);
    g_guarded = 0;
    return true;
}

void onTerminate() {
    crashText("std::terminate durante ", g_current);
    _exit(3);
}

void onAbort(int) {
    crashText("abort() durante ", g_current);
    _exit(3);
}
#endif

// --- Comandos ---
#if defined(_WIN32)
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
    g_snapshot = reinterpret_cast<SnapshotFn>(GetProcAddress(g_dll, "cramion_snapshot"));
    g_restore = reinterpret_cast<RestoreFn>(GetProcAddress(g_dll, "cramion_restore"));
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
#else
void load(proto::Reader& r) {
    const std::string path = r.str();
    const std::uint64_t session = r.u32();
    if (g_dll != nullptr) {
        sendDone(ipc::Status::Missing, "ya hay una DLL cargada");
        return;
    }
    g_current = "cargar la DLL (constructores estaticos)";
    g_dll = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (g_dll == nullptr) {
        const char* why = dlerror();
        sendDone(ipc::Status::Missing, "no se pudo cargar " + path + " (" + (why != nullptr ? why : "?") + ")");
        return;
    }
    g_dll_path = path;
    const auto get = [](const char* symbol) { return dlsym(g_dll, symbol); };
    void* abi_symbol = get("cramion_abi");
    const auto abi = reinterpret_cast<AbiFn>(abi_symbol);
    const auto init = reinterpret_cast<InitFn>(get("cramion_init"));
    const auto count = reinterpret_cast<CountFn>(get("cramion_class_count"));
    const auto name = reinterpret_cast<NameFn>(get("cramion_class_name"));
    g_create = reinterpret_cast<CreateFn>(get("cramion_create"));
    g_call = reinterpret_cast<CallFn>(get("cramion_call"));
    g_destroy = reinterpret_cast<DestroyFn>(get("cramion_destroy"));
    g_describe = reinterpret_cast<DescribeFn>(get("cramion_describe"));
    g_callback = reinterpret_cast<CallbackFn>(get("cramion_callback"));
    g_message = reinterpret_cast<MessageFn>(get("cramion_message"));
    g_snapshot = reinterpret_cast<SnapshotFn>(get("cramion_snapshot"));
    g_restore = reinterpret_cast<RestoreFn>(get("cramion_restore"));
    if (abi == nullptr || init == nullptr || count == nullptr || name == nullptr || g_create == nullptr || g_call == nullptr ||
        g_destroy == nullptr || g_describe == nullptr || g_callback == nullptr) {
        sendDone(ipc::Status::Missing, "la DLL no es de scripts de Cramion (faltan exportaciones del SDK)");
        return;
    }
    if (abi() != proto::kAbiVersion) {
        sendDone(ipc::Status::Missing, "la DLL se compilo con otra version del SDK: vuelve a compilar los scripts");
        return;
    }
    // Para el archivo y la linea de los fallos: donde empieza la .so.
    Dl_info info{};
    if (dladdr(abi_symbol, &info) != 0) g_dll_base = info.dli_fbase;
    init(&g_api, session);
    proto::Writer extra;
    const int n = count();
    extra.u32(static_cast<std::uint32_t>(n));
    for (int i = 0; i < n; ++i) extra.str(name(i));
    sendDone(ipc::Status::Ok, {}, &extra);
}
#endif

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

// Recarga en caliente: el estado de una instancia (propiedades + onBeforeReload).
void snapshot(proto::Reader& r) {
    const std::uint32_t id = r.u32();
    const auto it = g_instances.find(id);
    if (it == g_instances.end() || g_snapshot == nullptr) {
        proto::Writer extra;
        extra.str("{}");
        sendDone(ipc::Status::Ok, {}, &extra);
        return;
    }
    char error[2048] = {};
    g_current = "onBeforeReload()";
    std::vector<char> out(1 << 16);
    for (int attempt = 0; attempt < 2; ++attempt) {
        Job job{6, nullptr, 0, nullptr, out.data(), static_cast<int>(out.size()), it->second, 0, nullptr, error, sizeof(error), nullptr, 0};
        if (!guarded(&job)) {
            sendDone(ipc::Status::Crash, describeFault());
            return;
        }
        if (job.result <= static_cast<int>(out.size())) {
            proto::Writer extra;
            extra.str(job.result > 0 ? out.data() : "{}");
            sendDone(error[0] != 0 ? ipc::Status::Exception : ipc::Status::Ok, error, &extra);
            return;
        }
        out.resize(static_cast<std::size_t>(job.result) + 16);
    }
    sendDone(ipc::Status::Exception, "estado demasiado grande para la recarga");
}

void restore(proto::Reader& r) {
    const std::uint32_t id = r.u32();
    const std::string state = r.str();
    const auto it = g_instances.find(id);
    if (it == g_instances.end()) {
        sendDone(ipc::Status::Missing, "no existe la instancia");
        return;
    }
    char error[2048] = {};
    g_current = "onAfterReload()";
    Job job{7, nullptr, 0, state.c_str(), nullptr, 0, it->second, 0, nullptr, error, sizeof(error), nullptr, 0};
    if (!guarded(&job)) sendDone(ipc::Status::Crash, describeFault());
    else if (job.result != 0) sendDone(ipc::Status::Exception, error);
    else sendDone(ipc::Status::Ok, {});
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
#if defined(_WIN32)
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
#else
    std::set_terminate(onTerminate);
    // Los fallos de hardware, en una pila aparte (la pila llena tambien).
    stack_t alt{};
    alt.ss_sp = g_alt_stack;
    alt.ss_size = sizeof(g_alt_stack);
    sigaltstack(&alt, nullptr);
    struct sigaction fault{};
    fault.sa_sigaction = onFault;
    fault.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&fault.sa_mask);
    for (const int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL}) sigaction(sig, &fault, nullptr);
    struct sigaction aborted{};
    aborted.sa_handler = onAbort;
    aborted.sa_flags = SA_ONSTACK;
    sigemptyset(&aborted.sa_mask);
    sigaction(SIGABRT, &aborted, nullptr);
    signal(SIGPIPE, SIG_IGN);
    if (argc < 3) {
        std::fprintf(stderr, "Uso: CramionScriptHost <bloque> <pid del motor> (con el canal en el descriptor 3 y el bloque en el 4)\n");
        return 2;
    }
    const auto engine_pid = static_cast<pid_t>(std::strtol(argv[2], nullptr, 10));
#if defined(__linux__)
    // Muere con el motor (aunque este en un bucle infinito); si ya no esta, nada.
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (engine_pid > 0 && getppid() != engine_pid) return 2;
#else
    (void)engine_pid;
#endif
    struct stat info{};
    if (fcntl(3, F_GETFD) < 0 || fstat(4, &info) != 0 || info.st_size < static_cast<off_t>(sizeof(ipc::Block))) return 2;
    void* mapped = mmap(nullptr, sizeof(ipc::Block), PROT_READ | PROT_WRITE, MAP_SHARED, 4, 0);
    ::close(4);
    if (mapped == MAP_FAILED) return 2;
    g_block = static_cast<ipc::Block*>(mapped);
    g_channel = 3;
    fcntl(g_channel, F_SETFD, FD_CLOEXEC);  // el simbolizador no se lo queda
    // La pila del hilo (para reconocer la pila llena) y backtrace() ya cargado
    // (dentro del manejador no puede cargar libgcc).
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        void* low = nullptr;
        std::size_t size = 0;
        if (pthread_attr_getstack(&attr, &low, &size) == 0) {
            g_stack_low = reinterpret_cast<std::uintptr_t>(low);
            g_stack_high = g_stack_low + size;
        }
        pthread_attr_destroy(&attr);
    }
    void* warm[4];
    backtrace(warm, 4);
    g_block->host_ready = 1;
    wake();
#endif
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
            case ipc::Op::Snapshot: snapshot(r); break;
            case ipc::Op::Restore: restore(r); break;
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
