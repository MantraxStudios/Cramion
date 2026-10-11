#ifndef CRAMION_CORE_SCRIPTING_SCRIPT_RUNTIME_H
#define CRAMION_CORE_SCRIPTING_SCRIPT_RUNTIME_H

// El estado de ScriptSystem (privado de src/scripting): lo que vive mientras
// corre el juego y lo que comparten los modulos de la API.
//
// Cada modulo (native/*.cpp, ver native/Modules.h) se registra una vez al
// crear el sistema: anade sus funciones a `native` y se engancha a las fases
// del frame. Su estado propio lo guarda en un shared_ptr que capturan sus
// lambdas (asi no hace falta tocar este archivo para anadir un modulo).
//
// Orden de un frame (ScriptSystem::update), como en las versiones con Lua:
//   Begin     la red (lo que llego) y las respuestas HTTP
//   Gameplay  partida pendiente, autoguardado, eventos de dialogo
//   Input     acciones (Enhanced Input) y sus enlaces
//   Events    eventos de colision (3D y 2D) y roturas
//   Update    Visual Scripts (Start y Update)
//   Ai        maquinas de estados y Behavior Trees
//   Late      Visual Scripts (LateUpdate)
//   End       envio de la red
// y al final las destrucciones pendientes (Entity:destroy).

#include "CramionCore/scripting/NativeApi.h"
#include "CramionCore/scripting/Scripting.h"

#include "CramionCore/ecs/World.h"
#include "CramionCore/gameplay/Dialogue.h"
#include "CramionCore/gameplay/SaveGame.h"
#include "CramionCore/input/InputActions.h"
#include "CramionCore/net/Http.h"
#include "CramionCore/net/Network.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/project/DataPack.h"

#include <CramionDM/Input.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace cramion::scripting {

enum class Phase : int { Begin, Gameplay, Input, Events, Update, Ai, Late, End, Count };
inline constexpr std::size_t kPhaseCount = static_cast<std::size_t>(Phase::Count);

// Texto en minusculas (claves de teclas, nombres de acciones...).
std::string lowerText(std::string s);
// Ruta de un texto UTF-8 (nombres de archivos de los scripts).
std::filesystem::path pathFromUtf8(const std::string& text);
bool readTextFile(const std::filesystem::path& file, std::string& out);
// "(1.000, 2.000, 3.000)".
std::string vec3Text(const core::Vec3& v);

// Depuracion de los Visual Scripts (la da su modulo).
class VisualScriptDebugHost {
public:
    virtual ~VisualScriptDebugHost() = default;
    virtual bool debug(ecs::Entity entity, ScriptSystem::VisualScriptDebug& out) = 0;
    virtual void setDebugging(bool on) = 0;
    virtual void setBreakpoints(const std::string& graph, const std::vector<int>& nodes) = 0;
    virtual bool takeBreak(std::string& graph, int& node, entt::entity& entity) = 0;
    virtual std::vector<entt::entity> objects(const std::string& graph) const = 0;
};

// Pruebas automaticas del juego (las da su modulo).
class TestHost {
public:
    virtual ~TestHost() = default;
    virtual bool load(const std::string& file, std::string* error) = 0;
    virtual std::string call(const std::string& operation, const std::string& arg) = 0;
    virtual std::string step(float delta_seconds) = 0;
};

struct ScriptSystem::Impl {
    Impl();
    ~Impl();

    // --- Programa (lo pone quien ejecuta el juego) ---
    std::filesystem::path root;  // Assets
    const dm::Input* input = nullptr;
    physics::PhysicsSystem* physics = nullptr;
    audio::AudioSystem* audio = nullptr;
    navigation::NavigationSystem* navigation = nullptr;
    voxel::VoxelSystem* voxels = nullptr;
    GraphicsHost* graphics = nullptr;
    SkeletonHost skeleton_host;
    CursorLockCallback cursor_lock;
    VibrateCallback vibrate;
    dm::TouchControls* touch_controls = nullptr;
    ScreenHost screen;
    xr::XrSystem* xr_system = nullptr;
    const xr::XrRig* xr_rig = nullptr;
    bool cursor_locked = false;
    LogCallback log;
    std::vector<ScriptError> errors;
    // Para los scripts de C++ del objeto: mensajes (Send Message, eventos de
    // la interfaz...) y variables de red que llegan (OnNetVar).
    std::function<void(ecs::Entity, const std::string&, const std::string&)> message_listener;
    std::function<void(ecs::Entity, const std::string&, const std::string&)> net_var_listener;

    // --- La API ---
    api::NativeApi native;

    // --- Estado del juego en marcha ---
    ecs::World* world = nullptr;
    bool running = false;
    float time = 0.0f;           // Time.time (segundos de juego)
    float delta = 0.0f;          // Time.deltaTime del frame
    float fixed_delta = 0.02f;   // Time.fixedDeltaTime
    std::uint64_t frame = 0;     // Time.frameCount
    int physics_listener = -1;
    std::vector<physics::PhysicsEvent> events;        // los que llegan de la fisica
    std::vector<physics::PhysicsEvent> frame_events;  // los de este frame (fase Events)
    std::vector<entt::entity> pending_destroy;
    bool warned_lua = false;

    // --- Sobreviven a stop()/start() (cambios de escena) ---
    // DataPacks montados (DataPack.load).
    std::vector<project::DataPackMount> data_packs;
    std::filesystem::path data_pack_journal;
    std::function<void()> assets_changed;
    // Partidas guardadas y dialogos.
    gameplay::SaveSystem saves;
    gameplay::DialogueSystem dialogue;
    // La sesion de red (Network.host/connect) y el cliente HTTP.
    std::unique_ptr<net::NetworkSession> network;
    std::unordered_map<std::uint32_t, entt::entity> net_entities;
    std::unique_ptr<net::HttpClient> http;
    // Scene.load, Game.quit, Prefs.
    std::filesystem::path scene_request;
    bool quit_request = false;
    std::string scene_name;
    std::map<std::string, std::string> prefs;
    std::filesystem::path prefs_file;
    // Acciones (Enhanced Input).
    input::InputMapper actions;
    static constexpr const char* kBindingsPref = "__input_bindings";
    // Teclas por nombre ("w", "space", "leftshift"...).
    std::unordered_map<std::string, dm::Key> keys;

    // --- Modulos ---
    // Fases del frame, en el orden en que se registran dentro de cada fase.
    struct Hooks {
        std::array<std::vector<std::function<void(float)>>, kPhaseCount> frame;
        std::vector<std::function<void(float)>> fixed;
        // Al empezar Play (ya con `world`) y al pararlo (con `running` aun
        // como estaba: false si no llego a empezar).
        std::vector<std::function<void()>> start;
        std::vector<std::function<void(bool was_running)>> stop;
        // Antes de borrar una entidad (y cada hijo) con Entity:destroy.
        std::vector<std::function<void(entt::entity)>> destroying;
        // reloadFile: true si el archivo era suyo.
        std::vector<std::function<bool(const std::string& file)>> reload;
        // Eventos de la interfaz (callMethod) y origen flotante.
        std::vector<std::function<void(ecs::Entity target, const std::string& method, const api::Value& arg)>> event;
        std::vector<std::function<void(const core::Vec3& offset)>> origin_shift;
    } hooks;
    void onFrame(Phase phase, std::function<void(float)> fn) {
        hooks.frame[static_cast<std::size_t>(phase)].push_back(std::move(fn));
    }
    void onFixed(std::function<void(float)> fn) { hooks.fixed.push_back(std::move(fn)); }
    void onStart(std::function<void()> fn) { hooks.start.push_back(std::move(fn)); }
    void onStop(std::function<void(bool)> fn) { hooks.stop.push_back(std::move(fn)); }
    void onDestroying(std::function<void(entt::entity)> fn) { hooks.destroying.push_back(std::move(fn)); }
    void onReload(std::function<bool(const std::string&)> fn) { hooks.reload.push_back(std::move(fn)); }
    void onEvent(std::function<void(ecs::Entity, const std::string&, const api::Value&)> fn) {
        hooks.event.push_back(std::move(fn));
    }
    void onOriginShift(std::function<void(const core::Vec3&)> fn) { hooks.origin_shift.push_back(std::move(fn)); }

    std::shared_ptr<VisualScriptDebugHost> vs_debug;
    std::shared_ptr<TestHost> tests;
    // La consola (ScriptSystem::run): la pone native/Console.cpp.
    std::function<bool(const std::string& code, std::string* output)> console;

    // --- Mensajes y errores ---
    void write(int level, const std::string& message);  // 0 info, 1 aviso, 2 error
    void fail(const std::string& where, const std::string& message);

    // --- Entidades en la API ---
    // La entidad si sigue existiendo en el mundo ({} si no).
    ecs::Entity entity(entt::entity handle) const;
    // El argumento i (nil = {}); lanza api::Error si ya no existe.
    ecs::Entity entityArg(const api::Call& c, std::size_t i) const;
    // El `self` de un metodo de Entity; lanza api::Error si ya no existe.
    ecs::Entity selfEntity(const api::Call& c) const;
    api::Value entityValue(ecs::Entity e) const { return api::Value::entity(e.valid() ? e.handle() : entt::entity{entt::null}); }
    api::Value entityValue(entt::entity h) const { return api::Value::entity(h); }
    // El mundo del juego; lanza api::Error si no hay.
    ecs::World& requireWorld() const;

    // --- Ayudas comunes ---
    // DataPacks: buscar, ver si ya esta montado y montar (nullptr y un aviso si no).
    std::filesystem::path findDataPack(const std::string& name) const;
    const project::DataPackMount* mountedPack(const std::filesystem::path& file) const;
    const project::DataPackMount* loadDataPack(const std::string& name, const char* who);
    void savePrefs() const;
    // "Nivel2" o "Escenas/Nivel2.crscene" -> ruta del .crscene en Assets.
    std::filesystem::path findScene(const std::string& name) const;
    // Prefab leido (se relee si el archivo cambia).
    std::string prefabText(const std::filesystem::path& file);
    // Teclado, ejes y botones del mando por nombre.
    void buildKeys();
    dm::Key key(const std::string& name) const;
    bool keyDown(const std::string& name) const;
    float axis(const std::string& name) const;
    static bool gamepadButton(const std::string& name, dm::GamepadButton& out);
    // Input.lockCursor.
    void lockCursor(bool on);
    // Entity:destroy: al final del frame (con sus hijos).
    void destroyLater(entt::entity handle) { pending_destroy.push_back(handle); }
    void flushDestroys();

private:
    struct PrefabFile {
        std::filesystem::file_time_type stamp{};
        std::string text;
    };
    std::unordered_map<std::string, PrefabFile> prefabs_;
};

}  // namespace cramion::scripting

#endif  // CRAMION_CORE_SCRIPTING_SCRIPT_RUNTIME_H
