#ifndef CRAMION_CORE_ECS_SCENE_RESET_H
#define CRAMION_CORE_ECS_SCENE_RESET_H

// Cambio de escena: UNA secuencia para todos los caminos (editor: abrir,
// nueva, Scene.load en Play, pestanas de mundo; el juego; el servidor).
//
// Antes cada camino vaciaba unas cosas y otras no, y como loadScene usaba
// World::clearPersistent, las teselas de terreno (raices transitorias) de la
// escena anterior seguian en el mundo nuevo: con el origen flotante de la
// otra escena (desplazadas) y entrando en la fisica, la navegacion, la
// vegetacion y el render antes de que el streamer las quitara. Y lo que se
// cargaba en otros hilos para la escena anterior (teselas, colliders,
// baldosas de navegacion, siembra de arboles) podia entrar en la nueva.
//
// resetSceneState, en este orden:
//   1. Sube World::sceneGeneration: todo trabajo en otro hilo lanzado antes
//      (TerrainStreamer, PhysicsSystem, NavigationSystem, WorldPartition,
//      RenderSync) tira su resultado en vez de meterlo.
//   2. World Partition: lecturas canceladas, escrituras esperadas, celdas
//      olvidadas (end sin restaurar).
//   3. Fisica parada (los colliders de terreno a medias se cancelan).
//   4. Navegacion vacia (malla, baldosas en vuelo de otra epoca).
//   5. Teselas de terreno fuera del mundo y del streamer (guardando o no lo
//      esculpido) y, si se pide, el almacen de terrenos vacio.
//   6. RenderSync: terrenos y vegetacion fuera de la GPU (la siembra en
//      marcha no se sube nunca).
//   7. El mundo vacio (tambien las raices transitorias) y su origen en 0.
// Despues el que llama lee la escena nueva (loadScene / deserializeWorld) y
// alinea con el origen nuevo lo que siga vivo (camara, renderizador...).
//
// Lo que no se pase (nullptr) no se toca: el servidor no tiene RenderSync, y
// cada uno para sus scripts, audio, particulas... como siempre.

#include <cstdint>

namespace cramion::terrain {
class TerrainStreamer;
class TerrainStore;
}  // namespace cramion::terrain
namespace cramion::worldpart {
class WorldPartitionSystem;
}
namespace cramion::physics {
class PhysicsSystem;
}
namespace cramion::navigation {
class NavigationSystem;
}
namespace cramion::video {
class VideoSystem;
}

namespace cramion::ecs {

class World;
class RenderSync;

struct SceneResetTargets {
    terrain::TerrainStreamer* terrain_streamer = nullptr;
    terrain::TerrainStore* terrain_store = nullptr;
    worldpart::WorldPartitionSystem* partition = nullptr;
    physics::PhysicsSystem* physics = nullptr;
    navigation::NavigationSystem* navigation = nullptr;
    RenderSync* render = nullptr;
    // Paso 0 (antes de todo): reproductores de video parados. El que llama
    // debe haber parado ya el video antes de audio.stop(); esto solo lo
    // asegura en un sitio (VideoSystem::stop es idempotente).
    video::VideoSystem* video = nullptr;
};

struct SceneResetOptions {
    // Las teselas con cambios se guardan a disco al soltarlas (editor, si el
    // usuario guardo). Si no, se quedan en el almacen (Play: al parar vuelve
    // la escena del editor con lo esculpido) salvo discard_terrain_data.
    bool save_terrain = false;
    // Vaciar el almacen de terrenos entero, tambien lo no guardado (el
    // editor al abrir otra escena: lo no guardado de la anterior no debe
    // aparecer en la nueva ni guardarse con ella).
    bool discard_terrain_data = false;
    // Vaciar el mundo (paso 7). Sin esto solo se quitan las teselas.
    bool clear_world = true;
};

// Devuelve la generacion nueva del mundo.
std::uint64_t resetSceneState(World& world, const SceneResetTargets& targets, const SceneResetOptions& options = {});

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_ECS_SCENE_RESET_H
