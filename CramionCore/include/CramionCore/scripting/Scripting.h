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
// instantiate, create), Input (getKey, getKeyDown, getAxis, raton), Time,
// Physics.raycast, Audio.playOneShot, Debug.log, Mathf.

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Script nuevo con los metodos de siempre.
std::string scriptTemplate(const std::string& class_name);

void registerScriptComponents();

}  // namespace cramion::scripting

#endif  // CRAMION_CORE_SCRIPTING_H
