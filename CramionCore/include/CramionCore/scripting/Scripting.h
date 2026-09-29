#ifndef CRAMION_CORE_SCRIPTING_H
#define CRAMION_CORE_SCRIPTING_H

// Scripts en Lua 5.4, como los MonoBehaviour de Unity:
//
//   -- Assets/Scripts/Jugador.lua
//   local Jugador = { properties = { velocidad = 5.0 } }   -- editables en el Inspector
//   function Jugador:Start() end
//   function Jugador:Update(dt)
//       local mover = Vec3(Input.getAxis("Horizontal"), 0, Input.getAxis("Vertical"))
//       self.entity:translate(mover * self.velocidad * dt)
//   end
//   function Jugador:OnCollisionEnter(other, contact) Debug.log("choque con " .. other.name) end
//   return Jugador
//
// Componente Script: el archivo y los valores de sus propiedades. En Play
// cada entidad tiene su instancia (self) con `self.entity`; se llaman Awake,
// Start, Update, LateUpdate, FixedUpdate, OnCollisionEnter/Stay/Exit,
// OnTriggerEnter/Stay/Exit y OnDestroy. Al guardar un script en Play se
// recarga sin perder el estado de las instancias.
//
// API: Vec3, Entity (posicion, giro, escala, translate, rotate, lookAt,
// fisica, sonido, animacion, destroy...), Scene (find, findWithTag,
// instantiate, create, load), Input (getKey, getKeyDown, getAxis, raton),
// Time, Physics.raycast, Audio.playOneShot, Prefs (datos guardados, como el
// PlayerPrefs de Unity), Game.quit, Debug.log, Mathf. Navegacion:
// entity:moveTo(destino), stopMoving, isMoving, remainingDistance y la tabla
// Navigation (findPath, projectPoint, randomPoint, raycast, isReady).
// Mundos de bloques: la tabla Voxel (bloques, rayo, colision de una caja,
// mundos guardados). Input.lockCursor(true) captura el raton (primera persona).
// Mallas por codigo: Mesh.new/cube/plane/sphere..., entity.mesh y
// entity:addComponent("MeshCollider") (como el Mesh de Unity).
// Esqueletos: entity:getBonePosition/setBoneRotation, IK (setIKTarget,
// setLookAt, setupCreatureIK), ragdoll (entity.ragdoll, addRagdollForce),
// Bone Sockets (attachToBone) y cualquier campo de cualquier componente con
// entity:getField / entity:setField ("PhysBones", "chains[1].pull").
// Configuracion grafica: la tabla Graphics (calidad, escalado, resolucion,
// sombras, texturas, VSync, ventana, efectos de render y el post-procesado
// global en Graphics.post), como QualitySettings + Screen de Unity.
// Multijugador: la tabla Network (host, connect, send/on, spawn de prefabs
// replicados, jugadores, cambiar de escena todos a la vez) y en las entidades
// isMine, netId, netOwner, setNetVar/getNetVar y el metodo OnNetVar.

#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/World.h"

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
namespace cramion::voxel {
class VoxelSystem;
}

namespace cramion::xr {
class XrSystem;
class XrRig;
}
namespace cramion::asset {
struct ModelData;
}

namespace cramion::scripting {

enum class PropertyType : int { Number = 0, Bool = 1, Text = 2, Vector = 3 };

struct ScriptProperty {
    std::string name;
    PropertyType type = PropertyType::Number;
    std::string value;  // "5.0", "true", "hola", "1 2 3"
};

struct Script {
    std::string file;  // ruta del .lua dentro de Assets
    bool enabled = true;
    std::vector<ScriptProperty> properties;

    void reflect(ecs::PropertyVisitor& v);
};

// --- Graphics (Lua) ---
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
    // entity:moveTo, isMoving... y la tabla Navigation (NavAgent y la malla).
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
    // Al parar los scripts se suelta solo.
    using CursorLockCallback = std::function<void(bool locked)>;
    void setCursorLock(CursorLockCallback callback);
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
    bool cursorLocked() const;
    // El programa solto el raton por su cuenta (Escape en el editor, perder el foco).
    void releaseCursor();
    // Mensajes de Debug.log / print (por defecto std::cout y std::cerr).
    using LogCallback = std::function<void(int level, const std::string& message)>;  // 0 info, 1 aviso, 2 error
    void setLog(LogCallback log);

    // Play: estado de Lua nuevo, instancias, Awake.
    void start(ecs::World& world);
    // Cada frame (despues de la fisica): eventos de colision, Start de las
    // nuevas, Update y LateUpdate, destrucciones pendientes.
    void update(ecs::World& world, float delta_seconds);
    // Tantas veces como pasos dio la fisica este frame.
    void fixedUpdate(ecs::World& world, float step, int steps);
    // Origen flotante (ecs/FloatingOrigin.h): el mundo se desplazo -offset.
    // Llama a OnOriginShift(offset) en todos los scripts: los que guardan
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

    // Vuelve a leer un script (ruta en Assets): en Play, las instancias siguen
    // con sus datos y las funciones nuevas.
    void reloadFile(const std::string& file);

    // Propiedades que declara un script (su tabla `properties`), para el
    // Inspector. Vacio si no se puede leer.
    std::vector<ScriptProperty> describe(const std::string& file, std::string* error = nullptr);

    // Ultimos errores (archivo, linea, mensaje).
    const std::vector<ScriptError>& errors() const;
    void clearErrors();

    // Llama a target:method(...) (eventos de la interfaz): con el control que
    // lo lanzo, o con su valor (numero, texto o si/no).
    void callMethod(ecs::Entity target, const std::string& method, ecs::Entity source);
    void callMethod(ecs::Entity target, const std::string& method, float value);
    void callMethod(ecs::Entity target, const std::string& method, const std::string& value);
    void callMethod(ecs::Entity target, const std::string& method, bool value);

    // Ejecuta codigo suelto (consola). Devuelve false si hay error. Fuera de
    // Play usa un estado de Lua temporal sobre `world` (la escena del editor).
    bool run(const std::string& code, std::string* output = nullptr, ecs::World* world = nullptr);

    // Scene.load("Nivel2"): la escena pedida (ruta del .crscene) o vacio. El
    // programa (editor en Play o juego) la carga al terminar el frame:
    // parar los sistemas, cargar, volver a empezar. takeSceneRequest la
    // devuelve una vez.
    std::filesystem::path takeSceneRequest();
    // Game.quit(): el juego quiere cerrarse (en el editor, salir de Play).
    bool takeQuitRequest();
    // Nombre de la escena actual (Scene.name en Lua).
    void setSceneName(const std::string& name);

    // Prefs: se conservan al cambiar de escena y, con archivo, entre partidas.
    void setPrefsFile(const std::filesystem::path& file);

    // La API de Lua tal como la ve un script (para el autocompletado del
    // editor): cada tabla global ("Input", "XR", "math"...) y cada tipo
    // ("Entity:", "Vec3:", "Quat:", "Mesh:": los miembros de sus objetos) con sus
    // miembros. "" = funciones globales. Se saca de un estado de Lua con
    // todos los bindings, asi que siempre esta al dia.
    struct ApiMember {
        std::string name;
        bool function = false;
    };
    static std::map<std::string, std::vector<ApiMember>> apiReference();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Script nuevo con los metodos de siempre.
std::string scriptTemplate(const std::string& class_name);

void registerScriptComponents();

}  // namespace cramion::scripting

#endif  // CRAMION_CORE_SCRIPTING_H
