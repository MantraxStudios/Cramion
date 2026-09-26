#ifndef CRAMION_CORE_ECS_STATIC_BATCHING_H
#define CRAMION_CORE_ECS_STATIC_BATCHING_H

// Static batching (como el de Unity), al exportar el juego: las mallas de los
// objetos marcados "Static" se hornean en el espacio del mundo y se combinan
// en un modelo por escena. Mismo material = misma llamada de dibujo, aunque
// vengan de modelos distintos.
//
// Como se hace para que siga siendo rapido (y no "una malla gigante"):
//   - Materiales iguales se juntan por contenido (factores y los bytes de sus
//     texturas), no por nombre: dos modelos con la misma textura comparten
//     lote. Los .crmat asignados se conservan (hueco sustituido en el lote).
//   - El lote se parte en clusteres espaciales (asset::clusterSubmeshes): el
//     culling en GPU (frustum + oclusion) sigue descartando lo que no se ve.
//     Todos los clusteres de un material van en UNA llamada indirecta.
//   - Una pieza lote por modo de sombra (si / no / solo sombras) y por si
//     lleva .crmat o no (los .crmat hacen una copia de la pieza al cargar:
//     asi solo se copia lo que los usa) y, si pasa de max_vertices, otra.
//   - Solo se copia lo que sale barato: una malla que aparece una vez se
//     mueve al lote (no ocupa mas), pero una repetida (el mismo acantilado
//     4 veces, el mismo arbol 500) solo entra si todas sus copias juntas no
//     pasan de max_copied_vertices. Si no, se queda instanciada: la GPU ya
//     dibuja todas sus copias en una llamada por material, y copiarla
//     multiplicaria la memoria de video (un escaneo de 2 millones de
//     vertices x 4 = 1 GB y el juego sin memoria).
//   - No se combina lo que se puede mover: con Animator, Script o un
//     Rigidbody que no sea estatico (en el o en un padre), o mallas creadas
//     por codigo.
//
// Los objetos originales se quedan en la escena (colisiones, scripts que los
// busquen, navegacion) con EntityInfo::static_batched: RenderSync ya no los
// dibuja. Como en Unity, un objeto Static no debe moverse, ocultarse ni
// cambiar de material en el juego: su malla va dentro del lote.

#include "CramionCore/asset/AssetDatabase.h"
#include "CramionCore/ecs/World.h"

#include <cstddef>
#include <filesystem>
#include <string>

namespace cramion::ecs {

struct StaticBatchOptions {
    // Una malla repetida entra en el lote solo si copias x vertices no pasa
    // de esto (cuenta tambien las copias que no son Static: esas siguen
    // necesitando la malla original).
    std::size_t max_copied_vertices = 32768;
    // Vertices por pieza del lote (mas, y se parte en otra).
    std::size_t max_vertices = std::size_t{4} << 20;
};

struct StaticBatchReport {
    std::size_t combined = 0;          // objetos dentro del lote
    std::size_t kept_instanced = 0;    // Static pero repetidos y grandes: instanciados
    std::size_t kept_movable = 0;      // Static con Animator/Script/Rigidbody
    std::size_t draws_before = 0;      // lotes (malla x material) que habia
    std::size_t draws_after = 0;       // materiales del lote
    std::size_t triangles = 0;
    std::size_t parts = 0;
    std::string message;               // resumen para la consola o el error
};

// Combina las mallas Static de `world` en un modelo nuevo que escribe en
// `model_file` (.crdata con UUID nuevo) y cambia `world`: marca los objetos
// combinados y anade la entidad "Static Batch" que dibuja el lote. Devuelve
// false si no habia nada que combinar o si fallo (en `report.message`); en
// ese caso `world` y el disco no cambian.
bool buildStaticBatch(World& world, const assets::AssetDatabase& database,
                      const std::filesystem::path& model_file, const StaticBatchOptions& options,
                      StaticBatchReport& report);

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_ECS_STATIC_BATCHING_H
