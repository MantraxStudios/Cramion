#ifndef CRAMION_CORE_SCRIPTING_H
#define CRAMION_CORE_SCRIPTING_H

// El sistema de scripting del juego: la API del motor
// en C++ (NativeApi.h: Audio.playOneShot, Entity:translate, Time.deltaTime...)
// que usan los scripts de C++ (CppScripts.h, el SDK de sdk/cramion), los
// Visual Scripts (.crgraph), las maquinas de estados y los Behavior Trees, y
// lo que vive mientras corre el juego aunque cambie la escena: la red
// (Network), HTTP, Prefs, las partidas guardadas, los dialogos, las acciones
// de entrada (Enhanced Input), los DataPacks y la configuracion grafica.
//
// Cada parte de la API es un modulo (src/scripting/native/*.cpp) que registra
// sus funciones y se engancha a las fases del frame (ScriptRuntime.h).

#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/scripting/CursorLock.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cramion::dm {
class Input;
class TouchControls;
}
namespace cramion::physics {
class PhysicsSystem;
}
namespace cramion::gameplay {
class SaveSystem;
class DialogueSystem;
}
namespace cramion::input {
struct InputActionSettings;
class InputMapper;
}
namespace cramion::audio {
class AudioSystem;
}
namespace cramion::navigation {
class NavigationSystem;
}
namespace cramion::video {
class VideoSystem;
}
namespace cramion::voxel {
class VoxelSystem;
}

namespace cramion::xr {
class XrSystem;
class XrRig;
}
namespace cramion::ui {
class UiSystem;
}
namespace cramion::asset {
struct ModelData;
}

namespace cramion::scripting {

namespace api {
class NativeApi;
}

enum class PropertyType : int { Number = 0, Bool = 1, Text = 2, Vector = 3 };

struct ScriptProperty {
    std::string name;
    PropertyType type = PropertyType::Number;
    std::string value;  // "5.0", "true", "hola", "1 2 3"
};

// --- Graphics ---
// La configuracion grafica es del programa que ejecuta los scripts (el
// editor en Play o el juego exportado), no del motor de scripts: este solo ve
// opciones por clave y el host las aplica al renderizador y a la ventana.
using GraphicsValue = std::variant<bool, double, std::string>;

struct GraphicsOption {
    std::string key;
    GraphicsValue value;
    bool writable = true;
    std::string description;
    std::vector<std::string> choices;  // valores validos de las de texto
};

class GraphicsHost {
public:
    virtual ~GraphicsHost() = default;
    // Todas las opciones con su valor actual.
    virtual std::vector<GraphicsOption> options() const = 0;
    // false + `error` si la clave no existe, es de solo lectura o el valor no vale.
    virtual bool set(const std::string& key, const GraphicsValue& value, std::string& error) = 0;
    // Calidades rapidas ("Baja", "Media", "Alta", "Ultra").
    virtual std::vector<std::string> qualityLevels() const = 0;
    virtual bool setQuality(const std::string& level, std::string& error) = 0;
    // La ultima calidad rapida aplicada, o "Personalizada" si se toco algo despues.
    virtual std::string quality() const = 0;
    // Resoluciones del monitor (ancho, alto), de mayor a menor.
    virtual std::vector<std::pair<int, int>> resolutions() const = 0;
    // Guarda la configuracion (el juego la recupera al volver a abrirse).
    virtual bool save(std::string& error) = 0;
};

struct ScriptError {
    std::string file;
    int line = 0;
    std::string message;
};

class ScriptSystem {
public:
    ScriptSystem();
    ~ScriptSystem();
    ScriptSystem(const ScriptSystem&) = delete;
    ScriptSystem& operator=(const ScriptSystem&) = delete;

    void setAssetsRoot(const std::filesystem::path& root);

    // --- DataPacks (DataPack.load / loadScene / unload / list / info) ---
    // Siguen montados entre escenas. `callback`: se llama al montar o
    // desmontar (el programa vuelve a leer la base de assets). `journal`:
    // diario de lo escrito, para limpiarlo si el programa se cierra sin
    // desmontar (el editor; project::cleanupDataPackJournal).
    void setAssetsChangedCallback(std::function<void()> callback);
    void setDataPackJournal(const std::filesystem::path& journal);
    // Desmonta todos (el editor al salir de Play: el proyecto queda como estaba).
    void unmountDataPacks();
    std::vector<std::string> mountedDataPacks() const;
    void setInput(const dm::Input* input);
    // Acciones y contextos del proyecto (ProjectSettings/InputActions.json,
    // como el Enhanced Input de Unreal): Input.getAction, Input.bindAction,
    // Input.addMappingContext... Sin llamar: defaultInputActions(). Se puede
    // cambiar en Play (el editor la manda al guardar).
    void setInputActions(const input::InputActionSettings& settings);
    // Estado de las acciones (el editor lo ensena en Play).
    const input::InputMapper& inputMapper() const;
    void setPhysics(physics::PhysicsSystem* physics);
    void setAudio(audio::AudioSystem* audio);
    // Entity:videoPlay..., Video.isSupported/info y los eventos OnVideo* (video/VideoSystem.h).
    void setVideo(video::VideoSystem* video);
    // entity().moveTo, isMoving... y la tabla Navigation (NavAgent y la malla).
    void setNavigation(navigation::NavigationSystem* navigation);
    // La tabla Voxel (el mundo de bloques de la escena).
    void setVoxels(voxel::VoxelSystem* voxels);
    // Esqueletos: la pose de cada frame y el esqueleto de un modelo los tiene
    // el programa (RenderSync). Sin esto, las funciones de huesos devuelven nil.
    struct SkeletonHost {
        std::function<bool(ecs::Entity, const std::string& bone, core::Mat4& world)> bone_world;
        std::function<std::vector<std::string>(ecs::Entity)> bone_names;
        // Esqueleto de la entidad (o de su pieza animada) y su escala (metros por unidad).
        std::function<const asset::ModelData*(ecs::Entity, float* scale)> skeleton;
    };
    void setSkeletonHost(SkeletonHost host);
    // La tabla Graphics (nullptr: sus funciones avisan y no hacen nada; el
    // post-procesado, Graphics.post, funciona siempre: es de la escena).
    void setGraphics(GraphicsHost* graphics);
    // Input.lockCursor(on): quien tiene la ventana captura o suelta el raton.
    // Al parar los scripts se suelta solo. (Solo el modo Locked; para los
    // tres modos, setCursorMode.)
    using CursorLockCallback = std::function<void(bool locked)>;
    void setCursorLock(CursorLockCallback callback);
    // Modo del cursor (Input.setCursorLockMode / setCursorVisible, como
    // Cursor.lockState / Cursor.visible de Unity): se llama cuando cambia lo
    // que hay que aplicar (el modo efectivo y si el cursor va oculto sobre el
    // juego). Lo que pide el juego y lo que se aplica: CursorLock.h.
    using CursorModeCallback = std::function<void(CursorLockMode effective, bool hidden)>;
    void setCursorMode(CursorModeCallback callback);
    // Input.vibrate(ms): vibracion del movil (quien tiene la ventana).
    using VibrateCallback = std::function<void(int milliseconds)>;
    void setVibrate(VibrateCallback callback);
    // Los controles tactiles en pantalla del juego (Input.setTouchControls,
    // Input.setTouchButton...). nullptr: esas funciones no hacen nada.
    void setTouchControls(dm::TouchControls* controls);
    // Tabla Screen: tamano de la pantalla y orientacion (en moviles,
    // Screen.setOrientation la cambia o la deja fija).
    struct ScreenHost {
        std::function<int()> width;
        std::function<int()> height;
        std::function<bool(const std::string& mode)> set_orientation;  // false si no es un modo valido
        std::function<std::string()> orientation_mode;                  // el ultimo pedido ("auto"...)
    };
    void setScreen(ScreenHost host);
    // Tabla XR (realidad virtual): casco, mandos y vibracion. Las poses salen
    // en el mundo con el origen del rig. nullptr: XR.isAvailable() da false.
    void setXr(xr::XrSystem* system, const xr::XrRig* rig);
    // La interfaz del juego en pantalla (tabla UI: UI.pointerOverUi,
    // UI.isTyping, UI.focus...; Entity:uiScreenRect). nullptr: dan false / nil.
    void setUi(ui::UiSystem* ui);
    // El raton esta capturado ahora (modo Locked aplicado).
    bool cursorLocked() const;
    // El programa solto el raton por su cuenta (Escape en el editor, pasar a
    // otra vista): el juego sigue pidiendo su modo y un clic en la vista lo
    // vuelve a aplicar (cursorClick).
    void releaseCursor();
    // Lo que pide el juego y lo que se aplica ahora.
    CursorLockMode cursorLockMode() const;
    CursorLockMode effectiveCursorLockMode() const;
    bool cursorHidden() const;
    // La ventana gana o pierde el foco. resume_on_focus (por defecto true,
    // el juego exportado): al volver el foco se aplica solo; el editor lo pone
    // a false (como Unity, hace falta un clic en la vista Juego).
    void cursorFocus(bool focused);
    void setCursorResumeOnFocus(bool on);
    // La vista del juego tiene el foco (editor: la pestana Juego).
    void cursorViewActive(bool active);
    // Clic en la vista del juego: vuelve el modo que pide el juego.
    void cursorClick();
    // Antes del stop() de Scene.load: el proximo start() sigue con el modo
    // que pedia el juego en vez del del proyecto (input.CursorLockMode).
    void carryCursorToNextStart();
    // Mensajes de Debug.log / print (por defecto std::cout y std::cerr).
    using LogCallback = std::function<void(int level, const std::string& message)>;  // 0 info, 1 aviso, 2 error
    void setLog(LogCallback log);

    // Play: los modulos empiezan (Visual Scripts, maquinas de estados,
    // partidas...). Los scripts de C++ los lleva CppScriptSystem.
    void start(ecs::World& world);
    // Cada frame (despues de la fisica), por fases (ScriptRuntime.h): red,
    // partidas, acciones, eventos de colision, Visual Scripts, IA, envio de
    // red y destrucciones pendientes.
    void update(ecs::World& world, float delta_seconds);
    // Tantas veces como pasos dio la fisica este frame.
    void fixedUpdate(ecs::World& world, float step, int steps);
    // Origen flotante (ecs/FloatingOrigin.h): el mundo se desplazo -offset.
    // Llama a OnOriginShift(offset) en los Visual Scripts: los que guardan
    // posiciones del mundo en variables (un destino, un punto de spawn) les
    // restan `offset`. Las posiciones que se leen cada frame ya vienen bien.
    void shiftOrigin(const core::Vec3& offset);
    void stop();
    bool running() const;
    // La sesion de red sobrevive a stop()/start() (Scene.load y
    // Network.loadScene cambian de escena sin desconectar). Se cierra aqui:
    // al salir de Play en el editor o al cerrar el juego.
    void shutdownNetwork();
    // Estado de la red para el editor ("Servidor: 3 jugadores", "Cliente 2"...).
    std::string networkStatus() const;

    // Vuelve a leer un archivo (ruta en Assets: .crgraph, .crfsm, .crbt): en
    // Play, las instancias siguen con sus datos.
    void reloadFile(const std::string& file);
    // Lo llama CppScriptSystem al recargar en caliente o reiniciar el proceso
    // de scripts: olvida los callbacks de C++ (las conexiones siguen).
    void resetBridgeCallbacks();

    // Ultimos errores (archivo, linea, mensaje).
    const std::vector<ScriptError>& errors() const;
    void clearErrors();

    // Eventos de la interfaz: el Custom Event `method` del Visual Script de
    // `target` (con el control que lo lanzo o con su valor) y el mensaje para
    // sus scripts de C++ (setMessageListener).
    void callMethod(ecs::Entity target, const std::string& method, ecs::Entity source);
    void callMethod(ecs::Entity target, const std::string& method, float value);
    void callMethod(ecs::Entity target, const std::string& method, const std::string& value);
    void callMethod(ecs::Entity target, const std::string& method, bool value);

    // Consola: llamadas a la API ("Audio.playOneShot('clic.wav')",
    // "Time.timeScale = 0.5", "Scene.find('Jugador'):translate(Vec3(0,1,0))").
    // Devuelve false si hay error. Fuera de Play trabaja sobre `world` (la
    // escena del editor) y lo que se destruya (Scene.destroy, Entity:destroy)
    // se borra al terminar el comando; en Play, al final del frame.
    bool run(const std::string& code, std::string* output = nullptr, ecs::World* world = nullptr);

    // Scene.load("Nivel2"): la escena pedida (ruta del .crscene) o vacio. El
    // programa (editor en Play o juego) la carga al terminar el frame:
    // parar los sistemas, cargar, volver a empezar. takeSceneRequest la
    // devuelve una vez.
    std::filesystem::path takeSceneRequest();
    // Game.quit(): el juego quiere cerrarse (en el editor, salir de Play).
    bool takeQuitRequest();
    // Nombre de la escena actual (Scene.name).
    void setSceneName(const std::string& name);

    // Prefs: se conservan al cambiar de escena y, con archivo, entre partidas.
    void setPrefsFile(const std::filesystem::path& file);
    // Partidas guardadas (tabla Save): carpeta de las .crsave. El editor usa
    // Library/Saves; el juego exportado %APPDATA%/<juego>/saves.
    void setSaveFolder(const std::filesystem::path& folder);
    gameplay::SaveSystem& saveSystem();
    // El dialogo que corre (tabla Dialogue y caja de dialogo de la UI).
    gameplay::DialogueSystem& dialogueSystem();

    // La API tal como la ve un script (autocompletado, paleta de los Visual
    // Scripts): cada tabla ("Input", "XR"...) y cada tipo ("Entity:", "Mesh:":
    // los miembros de sus objetos) con sus miembros. "" = funciones globales.
    // Sale del registro (apiRegistry), asi que siempre esta al dia.
    struct ApiMember {
        std::string name;
        bool function = false;
    };
    static std::map<std::string, std::vector<ApiMember>> apiReference();
    // Todas las entradas con su documentacion (cramion_sdkgen genera el SDK).
    static const api::NativeApi& apiRegistry();
    // La de este sistema (consola, pruebas).
    api::NativeApi& nativeApi();

    // Puente para los scripts de C++: una llamada a la API en JSON
    // ({"op":"call|get|set","fn":"Audio.playOneShot","self":valor,"key":"x",
    // "args":[...],"value":v}); devuelve {"ok":true,"result":...} o
    // {"ok":false,"error":"..."}. Ver NativeApi::bridgeCall.
    std::string bridgeCall(const std::string& request_json);
    // Donde van los callbacks que los scripts de C++ dieron a la API.
    void setBridgeCallbackSink(std::function<void(std::uint64_t id, const std::string& args_json)> sink);
    // Una variable de red que llego a un objeto (OnNetVar): tambien para los scripts de C++.
    void setNetVarListener(std::function<void(ecs::Entity e, const std::string& key, const std::string& value_json)> listener);
    // Mensajes para los scripts de C++ del objeto (Send Message y Run Script de
    // los Behavior Trees): Script::onMessage(method, value).
    void setMessageListener(std::function<void(ecs::Entity e, const std::string& method, const std::string& value_json)> listener);
    // Un objeto (o un hijo) que se va a destruir al final del frame: se avisa
    // mientras aun es valido, despues de los modulos (onDestroy de los scripts de C++).
    void setDestroyListener(std::function<void(ecs::Entity e)> listener);
    // Entity::destroy de los scripts de C++: el mismo camino que Entity:destroy
    // (al final del frame, con sus hijos y los avisos de arriba).
    void destroyLater(ecs::Entity e);

    // --- Visual Scripting: depuracion del editor (VisualScriptRuntime.cpp) ---
    struct VisualScriptDebug {
        std::string graph;                               // .crgraph de la instancia
        bool failed = false;                             // no compilo o fallo en marcha
        double now = 0.0;                                // Time.time
        std::map<int, double> executed;                  // nodo -> ultima vez que corrio (Time.time)
        std::map<std::string, std::string> values;       // "nodo:pin" -> ultimo valor (texto)
        std::map<std::string, std::string> variables;    // variables del grafo
        // Con setVisualScriptLiveDebug(true):
        std::map<std::string, double> flows;             // "nodo:salida" exec -> ultima vez que paso (Time.time)
        std::map<std::string, std::string> inputs;       // "nodo:entrada" -> ultimo valor que leyo
    };
    // Lo que el editor pinta sobre el grafo de un objeto en Play. false si no tiene.
    bool visualScriptDebug(ecs::Entity entity, VisualScriptDebug& out);
    // Puntos de ruptura: con la depuracion activa, pasar por uno pausa Play.
    void setVisualScriptDebugging(bool on);
    void setVisualScriptBreakpoints(const std::string& graph, const std::vector<int>& nodes);
    // Un punto de ruptura que se alcanzo (lo consume). false si no hay.
    bool takeVisualScriptBreak(std::string& graph, int& node, entt::entity& entity);
    // Objetos que ejecutan un .crgraph (vacio = todos).
    std::vector<entt::entity> visualScriptObjects(const std::string& graph) const;
    // Depuracion en vivo (el editor la enciende; el juego exportado no paga
    // nada): por que cables pasa la ejecucion y el valor de cada entrada.
    void setVisualScriptLiveDebug(bool on);
    // Construction Script (como Unreal): el editor lo ejecuta fuera de Play al
    // colocar o cambiar un objeto con Visual Script. Trabaja sobre `world`;
    // lo que crea (Spawn) se borra la proxima vez. false + `error` si fallo
    // (o si el juego esta en marcha). hasConstructionScript: el .crgraph
    // (ruta en Assets) tiene un nodo Construction Script.
    bool runConstructionScript(ecs::World& world, ecs::Entity entity, std::string* error = nullptr);
    bool hasConstructionScript(const std::string& graph);

    // --- Pruebas automaticas del juego (native/TestApi.cpp) ---
    // Las pruebas son scripts de C++ (CRAMION_TEST, sdk/cramion/Test.h).
    // loadTestFile: un archivo de pruebas (false y el error si no vale).
    bool loadTestFile(const std::string& file, std::string* error = nullptr);
    // Operaciones del ejecutor de pruebas del editor ("list", "run_edit",
    // "begin", "abort", "reset"; los nombres __cramion_test_* de antes
    // tambien valen) con su resultado como texto (JSON). Vacio si no hay Play.
    std::string testCall(const std::string& function, const std::string& arg = {});
    std::string testStep(float delta_seconds);

    // El estado del sistema (src/scripting/ScriptRuntime.h): lo usan los
    // modulos de la API. Opaco fuera de src/scripting.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// Script nuevo (de C++) con los metodos de siempre: cppScriptTemplate.
std::string scriptTemplate(const std::string& class_name);

void registerScriptComponents();

}  // namespace cramion::scripting

#endif  // CRAMION_CORE_SCRIPTING_H
