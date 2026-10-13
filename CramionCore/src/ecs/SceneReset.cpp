// Cambio de escena: la secuencia comun (ver SceneReset.h).

#include "CramionCore/ecs/SceneReset.h"

#include "CramionCore/ecs/RenderSync.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/navigation/Navigation.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/terrain/Terrain.h"
#include "CramionCore/terrain/TerrainGrid.h"
#include "CramionCore/video/VideoSystem.h"
#include "CramionCore/world/WorldPartition.h"

#include <vector>

namespace cramion::ecs {

std::uint64_t resetSceneState(World& world, const SceneResetTargets& targets, const SceneResetOptions& options) {
    // 0. Video: hilos de decodificacion y voces de stream fuera (idempotente;
    //    el que llama ya deberia haberlo parado antes de audio.stop()).
    if (targets.video != nullptr) targets.video->stop();

    // 1. Generacion nueva: lo que se este haciendo en otros hilos para la
    //    escena anterior ya no entra en ningun sistema.
    const std::uint64_t generation = world.beginNewSceneGeneration();

    // 2. World Partition: sus celdas apuntan a raices de la escena anterior.
    //    Sin restaurar (la escena se va entera); lecturas canceladas.
    if (targets.partition != nullptr) targets.partition->end(world, /*restore=*/false);

    // 3. Fisica: los cuerpos y los colliders de terreno a medias, fuera (el
    //    que llama la vuelve a arrancar con la escena nueva).
    if (targets.physics != nullptr) targets.physics->stop();

    // 4. Navegacion: malla y baldosas en vuelo (otra epoca: no entran).
    if (targets.navigation != nullptr) targets.navigation->clear();

    // 5. Teselas de terreno: fuera del mundo y del streamer (sus cargas en
    //    marcha se cancelan). Lo esculpido se guarda o se queda en memoria.
    if (targets.terrain_streamer != nullptr && targets.terrain_store != nullptr) {
        targets.terrain_streamer->clear(world, *targets.terrain_store, options.save_terrain);
    } else {
        // Sin streamer (no deberia pasar): al menos ninguna tesela se queda.
        std::vector<entt::entity> tiles;
        for (const entt::entity h : world.registry().view<terrain::TerrainTile>()) tiles.push_back(h);
        for (const entt::entity h : tiles) {
            if (world.valid(h)) world.destroy(world.wrap(h));
        }
    }
    if (targets.terrain_store != nullptr && options.discard_terrain_data) {
        // Las escrituras en marcha terminan con su foto (no se cortan a medias).
        targets.terrain_store->flushSaves();
        targets.terrain_store->clear();
    }

    // 6. Render: terrenos y vegetacion de la escena anterior fuera de la GPU.
    if (targets.render != nullptr) targets.render->clearSceneStreaming();

    // 7. El mundo vacio, origen en 0 (loadScene pone el de la escena nueva).
    if (options.clear_world) world.clear();
    return generation;
}

}  // namespace cramion::ecs
