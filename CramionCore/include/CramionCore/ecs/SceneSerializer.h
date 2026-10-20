#ifndef CRAMION_CORE_ECS_SCENE_SERIALIZER_H
#define CRAMION_CORE_ECS_SCENE_SERIALIZER_H

// Escenas .crscene (JSON legible, apto para control de versiones):
//
// {
//   "format": "CramionScene", "version": 1,
//   "uuid": "...", "name": "Escena",
//   "entities": [                       // en profundidad: padres antes que hijos,
//     { "uuid": "...", "name": "Coche", //   hermanos en su orden
//       "parent": "..." | null, "active": true, "tag": "", "layer": 0,
//       "components": { "Transform": {...}, "MeshRenderer": {...} } } ]
// }
//
// Al leer se ignoran campos y componentes desconocidos (con aviso) y los que
// faltan se quedan con su valor por defecto: una escena vieja o de una
// version nueva se abre igual.

#include "CramionCore/ecs/World.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace cramion::ecs {

// 2: la densidad volumetrica por defecto paso de 0.02 a 0.004 (las escenas
// de la version 1 con el valor por defecto viejo se pasan al nuevo).
inline constexpr int kSceneFormatVersion = 2;

// Todo el mundo a texto JSON / de texto JSON (sustituye el contenido).
std::string serializeWorld(const World& world);
bool deserializeWorld(World& world, const std::string& json, std::string* error = nullptr);

bool saveScene(const World& world, const std::filesystem::path& path, std::string* error = nullptr);
// Sin las raices que diga `skip_root` (las de las celdas en disco del World
// Partition, que van en su propio archivo). Las transitorias nunca se guardan.
std::string serializeWorld(const World& world, const std::function<bool(const Entity&)>& skip_root);
bool saveScene(const World& world, const std::filesystem::path& path, std::string* error,
               const std::function<bool(const Entity&)>& skip_root);
// Abre otra escena: vacia el mundo entero (tambien las raices transitorias)
// y sube su generacion (World::sceneGeneration). Para deshacer o parar Play
// (misma escena, las teselas se quedan) usar deserializeWorld.
bool loadScene(World& world, const std::filesystem::path& path, std::string* error = nullptr);

// Una entidad y sus hijos (copiar/pegar, prefabs). pasteEntities crea copias
// con UUIDs nuevos bajo `parent` y devuelve la raiz.
std::string serializeEntity(const World& world, Entity entity);

// Un componente de una entidad como objeto JSON ({"campo": valor}), por su
// nombre del registro ("Light", "Transform"...). Vacio si no lo tiene.
std::string componentToJson(World& world, Entity entity, const std::string& component);
// Aplica campos JSON a un componente (lo anade si no lo tiene; los campos que
// no vengan no cambian). Para herramientas externas como el MCP del editor.
bool componentFromJson(World& world, Entity entity, const std::string& component, const std::string& fields,
                       std::string* error = nullptr);
Entity pasteEntities(World& world, const std::string& json, Entity parent = {});
// Como pasteEntities pero con los mismos UUID (recrear lo que se guardo con
// serializeEntity y se borro: World Partition, deshacer).
// Un registro cuyo UUID ya esta en el mundo no se recrea (ni sus hijos): se
// avisa y se deja el que ya estaba (nunca dos copias del mismo objeto).
Entity restoreEntities(World& world, const std::string& json, Entity parent = {});

// Lo mismo en dos pasos: parseEntities lee el JSON en cualquier hilo (sin
// tocar el mundo) y restoreParsedEntities crea las entidades en el hilo
// principal (World Partition: leer y parsear las celdas fuera del frame).
// nullptr si el texto no es valido.
struct ParsedEntities;
std::shared_ptr<const ParsedEntities> parseEntities(const std::string& json);
Entity restoreParsedEntities(World& world, const ParsedEntities& parsed, Entity parent = {});
// UUID de la raiz (el primer registro); vacio si no tiene. Para saber, sin
// recrear nada ni avisar, si ya esta en el mundo (World Partition en el editor).
Uuid parsedRootUuid(const ParsedEntities& parsed);

// Lee solo el UUID de un .crscene (para la base de datos de assets).
Uuid readSceneUuid(const std::filesystem::path& path);

// --- Registros sueltos (historial de deshacer del editor) ---------------------
// Una sola entidad (sin hijos) como registro JSON compacto, el mismo de las
// escenas: uuid, nombre, padre (su UUID o null), estado y componentes. Solo
// lee (los visitantes de escritura no tocan nada): se puede llamar desde
// varios hilos a la vez con entidades distintas mientras nadie cambie el mundo.
std::string serializeEntityRecord(const World& world, Entity entity);
// Pone `entity` como dice `record` (de serializeEntityRecord): nombre, estado
// y componentes. Los que no esten en el registro se quitan; los que cambian se
// rehacen desde cero, como al leer la escena (sin estado de ejecucion viejo),
// salvo el Transform, que se escribe en su sitio. Con `current` (el registro
// de como esta ahora) los componentes iguales en los dos no se tocan. No
// cambia el padre ni el orden: el UUID del padre sale en `parent` (invalido =
// raiz).
bool applyEntityRecord(World& world, Entity entity, const std::string& record, const std::string* current = nullptr,
                       Uuid* parent = nullptr, std::string* error = nullptr);
// Una escena como la de serializeWorld (en compacto) a partir de registros ya
// hechos, en profundidad (padres antes que hijos). No toca el mundo: puede ir
// en otro hilo mientras los textos sigan vivos.
struct SceneHeader {
    Uuid uuid{};
    std::string name;
    DVec3 origin{};
};
std::string assembleScene(const SceneHeader& header, std::span<const std::string* const> records);

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_ECS_SCENE_SERIALIZER_H
