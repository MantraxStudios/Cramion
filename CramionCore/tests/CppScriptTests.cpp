// Pruebas de las CVars y de los scripts de C++ aislados (consola, sin GPU).
// La parte de C++ compila de verdad unos scripts (si hay compilador) que
// fallan a proposito: puntero nulo, excepcion, bucle infinito, recursion
// infinita, memoria agotada y abort(). El proceso de la prueba (el "motor")
// tiene que seguir vivo y los scripts sanos, funcionando.
// Devuelve 0 si todo va.

#include "CramionCore/cvar/CVar.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/gameplay/Localization.h"
#include "CramionCore/scripting/CppScripts.h"
#include "CramionCore/scripting/NativeApi.h"
#include "CramionCore/scripting/Scripting.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <optional>
#include <thread>
#include <string>

using namespace cramion;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const std::string& what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what.c_str());
    if (!condition) ++failures;
}

cvar::CVar<int> t_int("test.Entero", 10, "Un entero", cvar::Saved, 0, 100);
cvar::CVar<float> t_float("test.Real", 0.5f, "Un real", cvar::Saved);
cvar::CVar<bool> t_bool("test.Activo", false, "Un bool");
cvar::CVar<std::string> t_text("test.Texto", "hola", "Un texto", cvar::Saved);
cvar::CVar<int> t_readonly("test.SoloLectura", 7, "No se toca", cvar::ReadOnly);
cvar::CVar<bool> t_cheat("test.Truco", false, "Un truco", cvar::Cheat);

void testCVars() {
    std::printf("CVars\n");
    cvar::Registry& reg = cvar::Registry::instance();
    check(reg.find("TEST.entero") == &t_int, "se encuentran por nombre (sin mayusculas)");
    check(t_int.get() == 10 && t_int == 10, "valor por defecto");
    int changes = 0;
    const int id = t_int.onChanged([&](cvar::CVarBase&) { ++changes; });
    t_int = 50;
    t_int = 50;
    check(t_int.get() == 50 && changes == 1, "cambia y avisa (una vez si no cambia)");
    t_int = 500;
    check(t_int.get() == 100, "se recorta a su limite (0 a 100)");
    t_int.removeCallback(id);
    std::string error;
    check(reg.set("test.Real", "2,5", &error) && std::abs(t_float.get() - 2.5f) < 1e-6f, "se cambia por texto (con coma decimal)");
    check(!reg.set("test.Real", "abc", &error) && !error.empty(), "texto que no es numero: error");
    check(reg.set("test.Activo", "on") && t_bool.get(), "bool por texto (on/off, 1/0, true/false)");
    check(!reg.set("test.SoloLectura", "1", &error) && t_readonly.get() == 7, "solo lectura: no se cambia");
    check(reg.set("test.SoloLectura", "1", nullptr, true) && t_readonly.get() == 1, "con force si");
    reg.setCheatsAllowed(false);
    check(!reg.set("test.Truco", "true"), "truco sin trucos permitidos: no");
    reg.setCheatsAllowed(true);
    check(reg.set("test.Truco", "true") && t_cheat.get(), "truco con trucos permitidos: si");

    bool handled = false;
    check(reg.execute("test.Texto \"adios mundo\"", &handled).find("adios mundo") != std::string::npos && handled &&
              t_text.get() == "adios mundo",
          "consola: nombre valor (con comillas)");
    check(reg.execute("test.Entero", &handled).find("= 100") != std::string::npos, "consola: nombre muestra el valor");
    check(reg.execute("cvars test.", &handled).find("6 variables") != std::string::npos, "consola: cvars filtro");
    reg.execute("reset test.Texto");
    check(t_text.get() == "hola", "consola: reset");
    reg.execute("print(1)", &handled);
    check(!handled, "lo que no es de CVars no se queda (va a Lua)");

    // Guardar y leer: solo las Saved que no estan por defecto.
    t_int = 42;
    t_text = "guardado";
    const std::string saved = reg.saveJson();
    check(saved.find("test.Entero") != std::string::npos && saved.find("test.Activo") == std::string::npos,
          "se guardan solo las Saved cambiadas");
    t_int.reset();
    t_text.reset();
    reg.loadJson(saved);
    check(t_int.get() == 42 && t_text.get() == "guardado", "y se leen");
    // Una guardada que aun no existe (la declara un script despues).
    reg.loadJson(R"({"juego.Vidas": 5})");
    cvar::CVarBase* vidas = reg.createDynamic("juego.Vidas", cvar::Type::Int, "3", "Vidas", cvar::Saved);
    check(vidas != nullptr && vidas->toString() == "5", "la de un script recibe su valor guardado al declararse");
    check(reg.createDynamic("juego.Vidas", cvar::Type::String, "x", "") == nullptr, "mismo nombre con otro tipo: no");
    reg.clearDynamic();
    check(reg.find("juego.Vidas") == nullptr, "al parar se quitan las de los scripts");
    check(reg.saveJson().find("juego.Vidas") != std::string::npos, "pero su valor guardado no se pierde");
    t_int.reset();
    t_text.reset();
}

void write(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

void testCppScripts() {
    std::printf("Scripts de C++ aislados\n");
    scripting::CppScriptSystem::setToolchainRoot(std::filesystem::path(CRAMION_BIN_DIR) / "toolchain");
    std::string kind;
    const std::string compiler = scripting::CppScriptSystem::findCompiler(&kind);
    if (compiler.empty()) {
        std::printf("  (sin compilador de C++: se salta)\n");
        return;
    }
    std::printf("  compilador: %s (%s)\n", kind.c_str(), compiler.c_str());
#if defined(_WIN32)
    check(kind == "clang (incluido)", "usa el compilador incluido con el motor (toolchain/)");
#endif
    if (!scripting::CppScriptSystem::findClangFormat().empty()) {
        // Formatear (clang-format incluido) conservando el cursor.
        const std::string messy = "class A{public:\nint  f( int x ){return x*2;}\n};\n";
        int cursor = static_cast<int>(messy.find("return"));
        std::string formatted, error;
        const bool ok = scripting::CppScriptSystem::formatSource(messy, "A.h", "{BasedOnStyle: Google, IndentWidth: 4}", formatted, &cursor, &error);
        if (!ok) std::printf("    %s\n", error.c_str());
        check(ok && formatted.find("int f(int x)") != std::string::npos && formatted.find("public:") != std::string::npos,
              "formatea C++ con el clang-format incluido");
        check(ok && cursor >= 0 && formatted.compare(static_cast<std::size_t>(cursor), 6, "return") == 0,
              "el cursor queda en el mismo sitio del codigo");
    }
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_cpp_scripts_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const std::filesystem::path assets = root / "Assets";
    // Mensajes de la UI (on_click...) y un script que usa otro (script<T>()).
    write(assets / "Scripts" / "Mensajes.cpp", R"(#include <cramion/Script.h>
using namespace cramion;
class Buzon : public Script {
public:
    int recibido = 0;
    void awake() override {
        on("OnMover", [this](const Value& v) { entity().setPosition(Vec3{v.asFloat(), 0.0f, 0.0f}); });
    }
    void onOriginShift(const Vec3& offset) override { entity().setName("origen " + std::to_string(static_cast<int>(offset.x))); }
};
class Preguntador : public Script {
public:
    void update(float) override {
        if (Buzon* b = Scene::find("Buzon").script<Buzon>()) {
            b->recibido = 7;
            entity().setName("encontrado " + std::to_string(b->recibido));
        }
    }
};
CRAMION_SCRIPT(Buzon)
CRAMION_SCRIPT(Preguntador)
)");
    write(assets / "Scripts" / "Mover.cpp", R"(#include <cramion/Script.h>
using namespace cramion;
static CVar<float> g_paso("prueba.Paso", 1.0f, "Metros por frame");
class Mover : public Script {
public:
    Property<float> extra{this, "extra", 0.0f, Range(0, 10), Tooltip("Metros extra por frame")};
    Property<Entity> objetivo{this, "objetivo", {}, Requires("Light")};
    Property<int> modo{this, "modo", 1, Options{"Andar", "Correr"}, Header("Movimiento")};
    Property<Color> tinte{this, "tinte", Color(1, 0, 0)};
    Property<Prefab> bala{this, "bala"};
    Property<AudioClip> sonido{this, "sonido"};
    Property<std::vector<float>> pesos{this, "pesos"};
    int frames = 0;
    void start() override {
        Debug::log("Mover empieza en " + entity().name());
        if (objetivo.get()) objetivo->setName("visto por Mover");
        // Toda la API del motor: un aviso con un callback y una malla (un objeto del motor).
        Api::call("Text.onLanguageChanged", {Value([this](const Values& args) { entity().setName("callback " + args[0].asString()); })});
        Value mesh = Api::call("Mesh.cube", {1.0f});
        entity().set("mesh", mesh);
        Api::call("Debug.log", {"desdeCpp " + std::to_string(static_cast<int>(pesos->size()) + modo)});
    }
    void update(float) override {
        ++frames;
        entity().translate(Vec3{g_paso.get() + extra, 0.0f, 0.0f});
        entity().setField("Light", "intensity", static_cast<float>(frames));
    }
};
CRAMION_SCRIPT(Mover)

class Lote : public Script {
public:
    Property<std::vector<Entity>> cajas{this, "cajas"};
    void update(float) override {
        std::vector<Vec3> p = Scene::getPositions(cajas);
        for (Vec3& q : p) q.y += 1.0f;
        Scene::setPositions(cajas, p);
        for (const Entity& c : cajas.get()) c.rotate(Vec3{0.0f, 1.0f, 0.0f});  // sin respuesta: van juntas
    }
};
CRAMION_SCRIPT(Lote)
)");
    write(assets / "Scripts" / "Fallos.cpp", R"(#include <cramion/Script.h>
#include <stdexcept>
#include <vector>
#include <cstdlib>
using namespace cramion;
class PunteroNulo : public Script {
    int frames = 0;
public:
    void update(float) override {
        if (++frames == 3) {
            int* volatile p = nullptr;
            *p = 42;
        }
    }
};
class Lanza : public Script {
public:
    void start() override { throw std::runtime_error("algo salio mal"); }
};
class Bucle : public Script {
public:
    void update(float) override { volatile bool siempre = true; while (siempre) {} }
};
static int recursion(int n) { volatile char hueco[512]; hueco[0] = static_cast<char>(n); return recursion(n + 1) + hueco[0]; }
class Recursion : public Script {
public:
    void update(float) override { recursion(0); }
};
class Memoria : public Script {
public:
    std::vector<std::vector<char>> bloques;
    void update(float) override { for (int i = 0; i < 64; ++i) bloques.emplace_back(64u << 20, 'x'); }
};
class Aborta : public Script {
    int frames = 0;
public:
    void update(float) override { if (++frames == 2) std::abort(); }
};
CRAMION_SCRIPT(PunteroNulo)
CRAMION_SCRIPT(Lanza)
CRAMION_SCRIPT(Bucle)
CRAMION_SCRIPT(Recursion)
CRAMION_SCRIPT(Memoria)
CRAMION_SCRIPT(Aborta)
)");
    // La plantilla de un script nuevo: Plantilla.h (clase) + Plantilla.cpp (metodos).
    write(assets / "Scripts" / "Plantilla.h", scripting::cppScriptHeaderTemplate("Plantilla"));
    write(assets / "Scripts" / "Plantilla.cpp", scripting::cppScriptTemplate("Plantilla"));
    scripting::CppScriptSystem cpp;
    cpp.setAssetsRoot(assets);
    cpp.setBuildFolder(root / "Library" / "CppScripts");
    cpp.setSdkFolder(CRAMION_SDK_DIR);
    #if defined(_WIN32)
    cpp.setHostExecutable(std::filesystem::path(CRAMION_BIN_DIR) / "CramionScriptHost.exe");
#else
    cpp.setHostExecutable(std::filesystem::path(CRAMION_BIN_DIR) / "CramionScriptHost");
#endif
    check(cpp.hasSources() && !cpp.upToDate(), "hay fuentes y la DLL no esta al dia");
    cpp.compileAsync();
    while (cpp.compiling()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const std::optional<scripting::CppCompileResult> taken = cpp.takeCompileResult();
    const scripting::CppCompileResult built = taken.value_or(scripting::CppCompileResult{});
    if (!built.ok) {
        for (const auto& e : built.errors) std::printf("    %s:%d %s\n", e.file.c_str(), e.line, e.message.c_str());
        std::printf("%s\n", built.log.substr(0, 3000).c_str());
    }
    check(built.ok, "compila (" + std::to_string(built.seconds).substr(0, 4) + " s, con la descripcion)");
    if (!built.ok) return;
    check(std::filesystem::exists(root / "compile_commands.json"), "escribe compile_commands.json para clangd");
    {
        std::ifstream in(root / "compile_commands.json", std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        check(text.str().find("Plantilla.h") != std::string::npos && text.str().find("c++-header") != std::string::npos,
              "las cabeceras van como C++ (-x c++-header) en compile_commands.json");
    }
    const scripting::CppClassInfo* plantilla = scripting::findCppScriptClass("Plantilla");
    check(plantilla != nullptr && plantilla->file == "Scripts/Plantilla.cpp" && plantilla->properties.size() == 3,
          "la plantilla .h + .cpp compila y se describe (3 propiedades, archivo .cpp)");

    // Lo que declara cada clase (el Inspector).
    const scripting::CppClassInfo* mover_info = scripting::findCppScriptClass("Mover");
    check(mover_info != nullptr && mover_info->file == "Scripts/Mover.cpp", "describe la clase y su archivo (Scripts/Mover.cpp)");
    check(scripting::cppScriptClassesInFile("Scripts/Fallos.cpp").size() == 6, "las 6 clases de Fallos.cpp");
    if (mover_info != nullptr) {
        std::map<std::string, const scripting::CppPropertyInfo*> props;
        for (const auto& p : mover_info->properties) props[p.name] = &p;
        check(props.size() == 7, "7 propiedades");
        check(props["extra"] != nullptr && props["extra"]->kind == "float" && props["extra"]->has_range && props["extra"]->max == 10.0 &&
                  props["extra"]->tooltip == "Metros extra por frame",
              "float con rango y tooltip");
        check(props["objetivo"] != nullptr && props["objetivo"]->kind == "entity" && props["objetivo"]->type == "Light",
              "entidad que requiere Light");
        check(props["modo"] != nullptr && props["modo"]->kind == "enum" && props["modo"]->options.size() == 2 &&
                  props["modo"]->header == "Movimiento",
              "enum con opciones y cabecera");
        check(props["tinte"] != nullptr && props["tinte"]->kind == "color", "color");
        check(props["bala"] != nullptr && props["bala"]->kind == "asset" && props["bala"]->type == "7", "asset Prefab (tipo 7)");
        check(props["sonido"] != nullptr && props["sonido"]->kind == "file" && props["sonido"]->type == "1", "archivo de audio");
        check(props["pesos"] != nullptr && props["pesos"]->kind == "list" && props["pesos"]->element == "float", "lista de float");
    }

    // Errores de compilacion: con archivo y linea.
    {
        scripting::CppScriptSystem broken;
        const std::filesystem::path other = root / "Roto";
        write(other / "Assets" / "Malo.cpp", "#include <cramion/Script.h>\nint x = ;\n");
        broken.setAssetsRoot(other / "Assets");
        broken.setBuildFolder(other / "Library");
        broken.setSdkFolder(CRAMION_SDK_DIR);
        const scripting::CppCompileResult r = broken.compile();
        check(!r.ok && !r.errors.empty() && r.errors[0].file == "Malo.cpp" && r.errors[0].line == 2,
              "un error de compilacion da archivo y linea (Malo.cpp:2)");
    }

    ecs::World world;
    const auto make = [&](const char* name, const char* cls) {
        ecs::Entity e = world.create(name);
        scripting::CppScript& s = e.add<scripting::CppScript>();
        s.class_name = cls;
        return e;
    };
    ecs::Entity lampara = world.create("Lampara");
    lampara.add<ecs::Light>();
    ecs::Entity mover = make("Movil", "Mover");
    scripting::CppScript& ms = mover.get<scripting::CppScript>();
    ms.setValue("extra", "0.5");
    ms.setValue("objetivo", "{\"$uuid\":\"" + lampara.uuid().toString() + "\"}");
    ms.setValue("pesos", "[1,2,3]");
    mover.add<ecs::Light>();
    ecs::Entity lote = make("Lote", "Lote");
    std::vector<ecs::Entity> cajas;
    std::string lista = "[";
    for (int i = 0; i < 3; ++i) {
        cajas.push_back(world.create("Caja"));
        lista += std::string(i > 0 ? "," : "") + "{\"$uuid\":\"" + cajas.back().uuid().toString() + "\"}";
    }
    lote.get<scripting::CppScript>().setValue("cajas", lista + "]");
    ecs::Entity buzon = make("Buzon", "Buzon");
    ecs::Entity preguntador = make("Preguntador", "Preguntador");
    for (const char* cls : {"PunteroNulo", "Lanza", "Bucle", "Recursion", "Memoria", "Aborta", "NoExiste"}) make(cls, cls);

    // Los scripts del juego (toda la API para los scripts de C++).
    scripting::ScriptSystem scripts;
    std::vector<std::string> logs;
    scripts.setLog([&logs](int, const std::string& message) { logs.push_back(message); });
    scripts.start(world);
    cpp.setScriptSystem(&scripts);

    cvar::Registry::instance().set("script.cpp.TimeoutMs", "1500");
    cvar::Registry::instance().set("script.cpp.MemoryLimitMB", "256");
    cpp.start(world);
    const auto t0 = std::chrono::steady_clock::now();
    for (int frame = 0; frame < 12; ++frame) cpp.update(world, 1.0f / 60.0f);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const auto errors = cpp.errors();
    const auto has = [&](const std::string& who, const std::string& text) {
        for (const auto& e : errors) {
            if (e.message.find(who) != std::string::npos && e.message.find(text) != std::string::npos) return true;
        }
        return false;
    };
    for (const auto& e : errors) std::printf("    [%s:%d] %s\n", e.file.c_str(), e.line, e.message.c_str());
    check(true, "el motor sigue vivo tras todos los fallos (" + std::to_string(seconds).substr(0, 4) + " s)");
    check(has("PunteroNulo", "acceso a memoria invalido") && has("PunteroNulo", "puntero nulo"), "puntero nulo: atrapado y explicado");
    bool line_found = false;
    for (const auto& e : errors) line_found = line_found || (e.file.find("Fallos.cpp") != std::string::npos && e.line == 12);
    check(line_found, "el fallo dice el archivo y la linea (Fallos.cpp:12)");
    check(has("Lanza", "algo salio mal"), "excepcion de C++: su mensaje");
    check(has("Bucle", "bucle infinito"), "bucle infinito: cortado por tiempo");
    check(has("Recursion", "desbordamiento de pila"), "recursion infinita: pila llena atrapada");
    check(has("Memoria", "bad") || has("Memoria", "memoria") || has("Memoria", "excepcion"), "memoria agotada (tope de memoria del proceso)");
    check(has("Aborta", "abort"), "abort(): el proceso cae y se informa");
    check(has("NoExiste", "no hay ninguna clase"), "clase que no existe: aviso con las que hay");
    const float x = mover.worldPosition().x;
    check(x > 10.0f, "el script sano sigue moviendose a pesar de todo (x = " + std::to_string(x) + ")");
    check(lampara.name() == "visto por Mover", "Property<Entity>: la referencia del Inspector llega como entidad");
    check(mover.get<ecs::Light>().intensity > 3.0f, "setField cambia un componente del motor");
    check(cvar::Registry::instance().find("prueba.Paso") != nullptr, "la CVar del script esta en el registro");
    // Puente con la API: un valor calculado en C++ y una malla del motor puesta en la entidad.
    bool logged = false;
    for (const std::string& l : logs) logged = logged || l == "desdeCpp 4";
    check(logged, "Api::call desde C++ (3 pesos de la lista + modo 1 = 4)");
    check(mover.has<ecs::MeshRenderer>() && mover.get<ecs::MeshRenderer>().mesh != nullptr,
          "un objeto del motor (Mesh.cube) como handle, puesto en entity.mesh");
    // Un callback del script llamado por el motor (cambio de idioma).
    gameplay::localization().languages.push_back({"xx", "Prueba"});
    scripts.nativeApi().call("Text.setLanguage", {scripting::api::Value("xx")});
    cpp.update(world, 1.0f / 60.0f);
    check(mover.name() == "callback xx", "callback de C++ llamado por el motor (" + mover.name() + ")");
    // Lote: muchas posiciones en un mensaje.
    check(cajas[0].worldPosition().y > 5.0f && cajas[2].worldPosition().y > 5.0f, "getPositions/setPositions en lote");
    // Un boton de la UI (on_click "OnMover") llega al script de C++.
    cpp.sendMessage(buzon, "OnMover", "5");
    cpp.update(world, 1.0f / 60.0f);
    check(std::abs(buzon.worldPosition().x - 5.0f) < 0.01f, "mensaje de la UI a un script de C++ (Script::on)");
    cpp.shiftOrigin(core::Vec3{100.0f, 0.0f, 0.0f});
    cpp.update(world, 1.0f / 60.0f);
    check(buzon.name() == "origen 100", "onOriginShift llega a los scripts de C++ (" + buzon.name() + ")");
    check(preguntador.name() == "encontrado 7", "un script usa el de otro objeto: script<T>() (" + preguntador.name() + ")");
    const scripting::CppScriptSystem::Stats stats = cpp.stats();
    check(stats.faulted == 7, "7 scripts desactivados (" + std::to_string(stats.faulted) + ")");
    check(stats.restarts >= 2, "el proceso se rearranco tras caer (" + std::to_string(stats.restarts) + " veces)");
    check(stats.calls > stats.rpcs, "las llamadas sin respuesta van en lote (" + std::to_string(stats.calls) + " llamadas en " +
                                         std::to_string(stats.rpcs) + " mensajes)");
    cvar::Registry::instance().set("prueba.Paso", "10");
    const float before = mover.worldPosition().x;
    cpp.update(world, 1.0f / 60.0f);
    check(mover.worldPosition().x - before > 9.0f, "la CVar se cambia desde el motor y el script la ve");
    cpp.stop();
    scripts.stop();
    check(!cpp.running(), "parar cierra el proceso");
    cvar::Registry::instance().clearDynamic();
    cvar::Registry::instance().find("script.cpp.TimeoutMs")->reset();
    cvar::Registry::instance().find("script.cpp.MemoryLimitMB")->reset();
    if (std::getenv("CRAMION_KEEP_TEST") == nullptr) std::filesystem::remove_all(root, ec);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // si algo cae, que se vea hasta donde llego
    testCVars();
    testCppScripts();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
