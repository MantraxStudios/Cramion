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

#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/World.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cramion::dm {
class Input;
}
namespace cramion::physics {
class PhysicsSystem;
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
    void setPhysics(physics::PhysicsSystem* physics);
    void setAudio(audio::AudioSystem* audio);
    // entity:moveTo, isMoving... y la tabla Navigation (NavAgent y la malla).
    void setNavigation(navigation::NavigationSystem* navigation);
    // La tabla Voxel (el mundo de bloques de la escena).
    void setVoxels(voxel::VoxelSystem* voxels);
    // Input.lockCursor(on): quien tiene la ventana captura o suelta el raton.
    // Al parar los scripts se suelta solo.
    using CursorLockCallback = std::function<void(bool locked)>;
    void setCursorLock(CursorLockCallback callback);
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

    // Ejecuta codigo suelto (consola). Devuelve false si hay error.
    bool run(const std::string& code, std::string* output = nullptr);

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Script nuevo con los metodos de siempre.
std::string scriptTemplate(const std::string& class_name);

void registerScriptComponents();

}  // namespace cramion::scripting

#endif  // CRAMION_CORE_SCRIPTING_H
