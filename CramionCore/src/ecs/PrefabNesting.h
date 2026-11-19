#ifndef CRAMION_CORE_ECS_PREFAB_NESTING_H
#define CRAMION_CORE_ECS_PREFAB_NESTING_H

// Interno de Prefab.cpp y PrefabNesting.cpp: resolver un .crprefab con
// anidados o variante en el documento plano de siempre y la operacion
// inversa al guardar (ver "Anidados y variantes" en Prefab.h).

#include "CramionCore/Uuid.h"
#include "CramionCore/ecs/Prefab.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace cramion::ecs::detail {

// Una instancia de otro prefab que estaba dentro de la que se aplica y pasa a
// ser un enlace (prefab.SaveNested). `id` es el de su raiz en este prefab.
struct NewNestedPart {
    std::string id;
    Uuid prefab;
    std::filesystem::path file;
};

// prefab.SaveNested (las instancias de otros prefabs que hay dentro se guardan
// como enlaces en vez de copias sueltas).
bool saveNestedEnabled();
// Apunta que se escribio un .crprefab (prefabWriteGeneration).
void notePrefabWrite();

// El .crprefab tiene variante (variantOf) o algun registro anidado (nestedPrefab).
bool hasPrefabStructure(const nlohmann::json& raw);
// Documento plano. false (y `error`) si falta la base o hay un ciclo.
bool resolvePrefabJson(const nlohmann::json& raw, const std::filesystem::path& file, nlohmann::json& out,
                       std::string* error);
// Lo inverso: `records` son los registros planos nuevos (ids del prefab).
// Escribe en `out` "entities" (con los registros anidados y sus
// diferencias) y, en una variante, "variantOf" y "patches". `self` es el
// UUID del prefab que se guarda (detecta ciclos).
bool unresolvePrefab(const nlohmann::json& previous_raw, const std::filesystem::path& file, const Uuid& self,
                     const nlohmann::json& records, const std::vector<NewNestedPart>& added, nlohmann::json& out,
                     std::string* error);
// El prefab `prefab` (en `file`) depende de `target` (o es el mismo).
bool prefabFileDependsOn(const std::filesystem::path& file, const Uuid& target);
// Id de la entidad `child` de un anidado cuya raiz es `nested` en el prefab que lo contiene.
std::string nestedEntityId(const std::string& nested, const std::string& child);
// Ruta de un prefab para un enlace: relativa a la carpeta de `from`, con "/".
std::string prefabLinkPath(const std::filesystem::path& target, const std::filesystem::path& from);

// Edita el documento resuelto de `file` (sus registros planos) y lo guarda
// con su estructura (revision + 1). `resolved_out`: el texto resuelto nuevo.
bool editPrefabResolved(const std::filesystem::path& file, const std::function<bool(nlohmann::json& records)>& edit,
                        std::string* error, std::string* resolved_out);
// Quita de `file` sus diferencias para la entidad `source` (id resuelto) en
// `what` ("Componente|campo" o "#clave"). Sin diferencias, no toca nada.
bool removeOwnPatches(const std::filesystem::path& file, const std::string& source, const std::string& what,
                      std::string* error);
// El registro de `source` como lo da su base o su anidado (sin las
// diferencias de `file`), con los ids de este prefab.
bool originRecord(const std::filesystem::path& file, const std::string& source, nlohmann::json& record,
                  PrefabOrigin* where, std::string* error);
// Pasa los ids de este prefab que haya en `value` a los del origen de `source`.
void toOriginSpace(const std::filesystem::path& file, const std::string& source, nlohmann::json& value);

}  // namespace cramion::ecs::detail

#endif  // CRAMION_CORE_ECS_PREFAB_NESTING_H
