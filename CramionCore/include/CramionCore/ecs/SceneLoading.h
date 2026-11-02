#ifndef CRAMION_CORE_ECS_SCENE_LOADING_H
#define CRAMION_CORE_ECS_SCENE_LOADING_H

// Carga de escenas en segundo plano, como SceneManager.LoadSceneAsync de Unity
// (Scene.loadAsync / Scene.unloadAsync de los scripts).
//
// Mientras carga, la escena actual sigue viva: se juega, se dibuja y la
// pantalla de carga del juego (sus scripts y su UI) se anima sin tirones. El
// motor NO dibuja nada encima. Etapas de una operacion:
//   Leyendo     leer el .crscene y parsearlo en un hilo de IO        0.00-0.10
//   Modelos     leer los modelos (mallas, texturas decodificadas,     0.10-0.60
//               LODs) en los hilos del AssetManager (requestModel)
//   Creando     crear sus entidades en el mundo por tandas con         0.60-0.90
//               scene.LoadBudgetMs por frame, bajo raices desactivadas
//   Lista       0.9; espera a allowActivation(true)
//   Activada    en UN frame: modo Single = parar los sistemas de la     1
//               escena vieja (fisica, audio, terreno, navegacion...),
//               destruir sus objetos (con OnDestroy) salvo los
//               DontDestroyOnLoad, activar los nuevos y arrancar los
//               sistemas con ellos; modo Additive = activar los nuevos
//               (la escena actual se queda).
// Los scripts NO se paran en el cambio: los objetos persistentes
// (DontDestroyOnLoad) siguen con su estado y reciben OnSceneActivated, y los
// nuevos empiezan (awake/start) como al instanciar un prefab.
//
// Lo que hace el programa (juego, editor en Play, servidor) en el cambio
// Single va en Host (cada uno tiene sus sistemas). Sin Host todo funciona
// igual sin precargar modelos ni parar nada (pruebas).

#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cramion::ecs {

// DontDestroyOnLoad: la raiz del objeto (como en Unity, solo cuentan las
// raices: se marca la de su jerarquia) sobrevive a los cambios de escena.
void setDontDestroyOnLoad(Entity entity, bool keep = true);
bool isDontDestroyOnLoad(Entity entity);  // el objeto o su raiz

enum class SceneLoadMode { Single = 0, Additive = 1 };

// Lo que ve un script de una operacion (Scene.loadProgress...).
struct SceneLoadInfo {
    int id = 0;
    bool exists = false;
    bool unload = false;  // Scene.unloadAsync
    SceneLoadMode mode = SceneLoadMode::Single;
    std::string scene;    // nombre (el del archivo sin extension)
    std::filesystem::path file;
    float progress = 0.0f;  // 0..0.9 cargando, 0.9 lista (esperando permiso), 1 hecha
    bool done = false;
    bool failed = false;
    bool allow_activation = true;
    std::string error;
};

class AsyncSceneLoader {
public:
    struct Host {
        // Pedir un modelo SIN esperar (AssetManager::requestModel); true
        // cuando ya esta en memoria o no se pudo leer (no se espera mas).
        // Vacio: no se precargan modelos.
        std::function<bool(const Uuid&)> model_ready;
        // Destruir un objeto con los avisos de los scripts (OnDestroy) y
        // aplicarlo ya. Vacio: World::destroy.
        std::function<void(Entity)> destroy;
        std::function<void()> flush_destroys;
        // Single: antes de quitar la escena vieja (parar fisica, audio, video,
        // terreno, navegacion...) y despues de activar la nueva (arrancarlos
        // con ella, iluminacion horneada, nombre de la escena...).
        std::function<void(const std::filesystem::path& file)> before_single;
        std::function<void(const std::filesystem::path& file)> after_single;
        // Additive: tras activar (por ejemplo, la malla de navegacion).
        std::function<void(const std::filesystem::path& file)> after_additive;
    };

    struct Event {
        enum class Kind { Progress, Activated, Unloaded, Failed };
        Kind kind = Kind::Progress;
        int id = 0;
        std::string scene;
        SceneLoadMode mode = SceneLoadMode::Single;
        float progress = 0.0f;
        entt::entity notify = entt::null;  // quien pidio la carga (OnSceneLoadProgress)
        std::string error;
    };

    AsyncSceneLoader();
    ~AsyncSceneLoader();
    AsyncSceneLoader(const AsyncSceneLoader&) = delete;
    AsyncSceneLoader& operator=(const AsyncSceneLoader&) = delete;

    // Empieza a leer en otro hilo. Devuelve el id de la operacion (> 0).
    // `notify`: el objeto que recibe OnSceneLoadProgress cada frame.
    int load(const std::filesystem::path& file, SceneLoadMode mode, entt::entity notify = entt::null);
    // Descarga una escena aditiva (por nombre o ruta) o, con el nombre de la
    // activa, sus objetos (salvo los persistentes y las aditivas). 0 si no
    // hay ninguna cargada con ese nombre.
    int unload(const std::string& scene, entt::entity notify = entt::null);
    // false: la operacion se queda en 0.9 (lista) hasta permitirlo.
    bool setAllowActivation(int id, bool allow);
    SceneLoadInfo info(int id) const;

    // Cada frame, en el hilo principal y con el juego en marcha.
    // budget_ms < 0: el de la CVar scene.LoadBudgetMs.
    void update(World& world, const Host& host, double budget_ms = -1.0);
    // Eventos desde la ultima llamada (para los scripts).
    std::vector<Event> takeEvents();
    // Hay operaciones sin terminar.
    bool busy() const;
    // Olvida todo sin tocar el mundo (parar Play, Scene.load sincrono: el
    // mundo se vacia entero). Las lecturas en marcha terminan solas.
    void reset();
    // Lo mismo, pero borrando lo ya creado y aun oculto.
    void cancel(World& world);

    // La escena activa (la que cargo el programa o el ultimo Single) y las
    // aditivas cargadas, en ese orden.
    void setActiveSceneName(const std::string& scene);
    std::vector<std::string> loadedScenes() const;
    // Raices de las escenas en construccion (aun ocultas): nadie las debe tocar.
    bool isPendingRoot(entt::entity root) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_ECS_SCENE_LOADING_H
