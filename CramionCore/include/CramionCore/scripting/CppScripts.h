#ifndef CRAMION_CORE_CPP_SCRIPTS_H
#define CRAMION_CORE_CPP_SCRIPTS_H

// Scripts en C++ (como los MonoBehaviour de Unity, pero en C++), aislados en
// otro proceso:
//
//   Assets/**/*.cpp  --(clang incluido, o el del sistema)-->  Library/CppScripts/scripts_N.dll
//   CramionScriptHost.exe carga la DLL y ejecuta los eventos de cada script.
//
// El motor no carga la DLL: si un script falla (puntero nulo, excepcion, pila
// llena) solo cae su llamada o, como mucho, el proceso de los scripts; el
// motor lo detecta, muestra el error (archivo:linea), desactiva ese script y
// arranca el proceso otra vez para los demas. Un bucle infinito se corta con
// un tiempo maximo por llamada (script.cpp.TimeoutMs) y la memoria del proceso
// tiene un tope (script.cpp.MemoryLimitMB, un Job Object de Windows).
//
// Al compilar, la DLL se describe (en un proceso aparte): las clases, el
// archivo de cada una y sus Property<T> con su tipo, rango, opciones... El
// Inspector del componente "C++ Script" las muestra con su control (como los
// campos publicos de Unity) y sus valores llegan al script al crearlo.
//
// Los scripts tienen toda la API de Lua a traves de un puente
// (ScriptSystem::bridgeCall) y llamadas propias rapidas (transformaciones,
// fisica, entrada...) que se envian en lote.
//
// Solo Windows (en Android los scripts de C++ no se ejecutan).

#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/scripting/Scripting.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cramion::dm {
class Input;
}
namespace cramion::physics {
class PhysicsSystem;
}

namespace cramion::scripting {

// Un valor puesto en el Inspector (JSON: numero, texto, [x,y,z], lista, la
// referencia a una entidad {"$uuid":...} o a un asset {"uuid":...}).
struct CppScriptValue {
    std::string name;
    std::string json;
};

struct CppScript {
    std::string script;      // el .cpp dentro de Assets (arrastrado)
    std::string class_name;  // la de CRAMION_SCRIPT(...) en ese archivo
    bool enabled = true;
    std::vector<CppScriptValue> values;

    void reflect(ecs::PropertyVisitor& v);
    const CppScriptValue* value(const std::string& name) const;
    void setValue(const std::string& name, const std::string& json);
};

void registerCppScriptComponents();

// --- Lo que declaran las clases (al compilar) ---
struct CppPropertyInfo {
    std::string name, label, tooltip, header;
    std::string kind;     // bool, int, float, string, vec2, vec3, color, entity, asset, file, enum, list
    std::string element;  // listas: el tipo de cada elemento
    std::string type;     // asset: AssetType (numero); file: 0 imagen, 1 audio, 2 Lua, 3 shader, 4 cualquiera; entity: componente requerido
    bool has_range = false;
    double min = 0.0, max = 0.0;
    std::vector<std::string> options;
    std::string default_json;
};

struct CppClassInfo {
    std::string name;
    std::string file;  // dentro de Assets
    std::vector<CppPropertyInfo> properties;
};

// Las clases de la ultima DLL compilada (las comparte el editor y el Inspector).
const std::vector<CppClassInfo>& cppScriptClasses();
const CppClassInfo* findCppScriptClass(const std::string& name);
std::vector<const CppClassInfo*> cppScriptClassesInFile(const std::string& relative_file);

// Plantilla de un script nuevo (clase = nombre del archivo): Clase.h con la
// declaracion y las propiedades, y Clase.cpp con los metodos y CRAMION_SCRIPT.
std::string cppScriptHeaderTemplate(const std::string& class_name);
std::string cppScriptTemplate(const std::string& class_name);

struct CppCompileResult {
    bool ok = false;
    bool nothing_to_compile = false;  // no hay ningun .cpp en Assets
    std::vector<ScriptError> errors;
    std::string log;  // la salida entera del compilador
    std::filesystem::path dll;
    std::string compiler;
    double seconds = 0.0;
    std::vector<CppClassInfo> classes;  // descritas tras compilar
};

class CppScriptSystem {
public:
    CppScriptSystem();
    ~CppScriptSystem();
    CppScriptSystem(const CppScriptSystem&) = delete;
    CppScriptSystem& operator=(const CppScriptSystem&) = delete;

    void setAssetsRoot(const std::filesystem::path& folder);
    void setBuildFolder(const std::filesystem::path& folder);  // Library/CppScripts del proyecto
    void setSdkFolder(const std::filesystem::path& folder);    // la que contiene cramion/Script.h
    void setHostExecutable(const std::filesystem::path& exe);  // CramionScriptHost.exe
    void setPhysics(physics::PhysicsSystem* physics);
    void setInput(const dm::Input* input);
    // Toda la API para los scripts (el ScriptSystem del juego, en Play).
    void setScriptSystem(ScriptSystem* scripts);
    // Ruta (dentro de Assets) de un asset por su UUID (las propiedades de tipo asset).
    void setAssetPathResolver(std::function<std::string(const Uuid&)> resolver);

    // --- Compilar ---
    // El compilador: el incluido (toolchain/ junto al ejecutable), el de
    // script.cpp.Compiler, clang++ del sistema o Visual Studio. kind:
    // "clang (incluido)", "clang++" o "MSVC". Vacio si no hay.
    static std::string findCompiler(std::string* kind = nullptr);
    // clangd para el IntelliSense (el incluido o el del sistema).
    static std::filesystem::path findClangd();
    // clang-format (el incluido o el del sistema) y formatear un texto de C++.
    // style: "LLVM", "Google", "file" (el .clang-format del proyecto) o
    // "{BasedOnStyle: ..., IndentWidth: 4}". cursor (byte) se mantiene en su sitio.
    static std::filesystem::path findClangFormat();
    static bool formatSource(const std::string& text, const std::filesystem::path& file, const std::string& style,
                             std::string& out, int* cursor = nullptr, std::string* error = nullptr);
    // Otra carpeta del compilador incluido (por defecto, toolchain/ junto al ejecutable).
    static void setToolchainRoot(const std::filesystem::path& folder);
    // Argumentos de compilacion de un .cpp (para clangd: compile_commands.json).
    std::vector<std::string> compileArguments(const std::filesystem::path& source) const;
    // Escribe <proyecto>/compile_commands.json con todos los .cpp de Assets.
    void writeCompileCommands() const;
    bool hasSources() const;
    bool upToDate() const;  // la DLL es mas nueva que todas las fuentes
    // La fecha del .cpp/.h cambiado mas tarde (para no recompilar lo que ya fallo igual).
    std::filesystem::file_time_type newestSourceTime() const;
    CppCompileResult compile();
    void compileAsync();
    bool compiling() const;
    std::optional<CppCompileResult> takeCompileResult();
    // El juego exportado: la DLL ya compilada (sin compilador).
    void usePrebuilt(const std::filesystem::path& dll);
    const std::filesystem::path& dll() const;
    std::vector<std::string> classes() const;

    // --- Play ---
    void start(ecs::World& world);
    void stop();
    bool running() const;
    // Otra DLL (recien compilada) en Play: recarga en caliente. Cada script
    // guarda sus Property<T> y lo que devuelva onBeforeReload(), se cambia el
    // proceso y la DLL, y las instancias nuevas lo recuperan (onAfterReload)
    // sin volver a llamar a awake()/start() (script.cpp.HotReloadKeepState).
    void reload();
    void fixedUpdate(ecs::World& world, float step, int steps);
    void update(ecs::World& world, float delta_seconds);

    const std::vector<ScriptError>& errors() const;
    void clearErrors();
    // Un mensaje por nombre al script de C++ de un objeto (la UI: on_click
    // "OnJugar", on_change...). value_json: el valor (numero, texto, true...).
    void sendMessage(ecs::Entity target, const std::string& method, const std::string& value_json);
    // Mundos grandes: el origen se movio (todos reciben onOriginShift(offset)).
    void shiftOrigin(const core::Vec3& offset);
    // El JSON de una entidad para sendMessage ({"$e": id}).
    std::string entityJson(ecs::Entity e) const;

    struct Stats {
        bool host_running = false;
        std::uint64_t host_memory = 0;  // bytes (memoria privada del proceso)
        int instances = 0;
        int faulted = 0;
        int restarts = 0;
        int reloads = 0;  // recargas en caliente en esta sesion de Play
        std::uint64_t rpcs = 0;  // mensajes con el proceso en el ultimo frame
        std::uint64_t calls = 0;  // llamadas al motor en el ultimo frame (con las del lote)
        double frame_ms = 0.0;    // tiempo de los scripts en el ultimo frame
    };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cramion::scripting

#endif  // CRAMION_CORE_CPP_SCRIPTS_H
