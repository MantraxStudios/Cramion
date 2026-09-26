#ifndef CRAMION_CORE_PREFAB_H
#define CRAMION_CORE_PREFAB_H

// Prefabs: una entidad con sus hijos guardada como asset (.crprefab) para
// reutilizarla. Cada copia en una escena es una instancia enlazada al prefab:
//
//   - La raiz de la instancia lleva PrefabInstance (que prefab, en que
//     revision y sus cambios propios) y cada entidad PrefabLink (a que
//     entidad del prefab corresponde).
//   - Cambios propios (overrides): lo que difiere del prefab en una instancia
//     se apunta (recordOverrides) y se respeta al actualizarla. La posicion,
//     el giro, la escala y el nombre de la raiz son siempre de la instancia.
//     Tambien se apuntan los componentes anadidos o quitados y los hijos
//     borrados.
//   - Aplicar (applyInstance): la instancia pasa a ser la nueva version del
//     prefab. syncInstance lleva las demas instancias a esa version.
//   - Revertir: olvida los cambios propios. Desempaquetar: quita el enlace
//     (queda como entidades normales).
//
// Un .crprefab es JSON como las escenas:
//   { "format": "CramionPrefab", "version": 1, "uuid": "...", "name": "Enemigo",
//     "revision": 3, "entities": [ registros de entidad ] }
// Los UUID de sus entidades son los identificadores estables que usan los
// PrefabLink. Los prefabs dentro de prefabs se guardan como copias sueltas.

#include "CramionCore/Uuid.h"
#include "CramionCore/asset/AssetTypes.h"
#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/World.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace cramion::ecs {

inline constexpr const char* kPrefabExtension = ".crprefab";

struct PrefabInstance {
    assets::AssetRef prefab{{}, assets::AssetType::Prefab};
    int revision = 0;                    // version del prefab con la que esta al dia
    std::vector<std::string> overrides;  // "entidad|Componente|campo", "entidad|+Comp", "entidad|-"...

    void reflect(PropertyVisitor& v);
};

struct PrefabLink {
    std::string source;  // UUID de la entidad dentro del prefab

    void reflect(PropertyVisitor& v);
};

void registerPrefabComponents();

// --- Archivos ---
// El texto de un .crprefab (vacio si no se puede leer).
std::string readPrefabFile(const std::filesystem::path& file);
Uuid prefabUuid(const std::string& prefab_text);
int prefabRevision(const std::string& prefab_text);
std::string prefabName(const std::string& prefab_text);

// --- Crear e instanciar ---
// Guarda `root` (y sus hijos) como prefab nuevo en `file`; `root` pasa a ser su
// primera instancia. Los prefabs que hubiera dentro quedan como copias.
bool createPrefab(World& world, Entity root, const std::filesystem::path& file, std::string* error = nullptr);
// Una instancia nueva bajo `parent` (entidades con UUID nuevos).
Entity instantiatePrefab(World& world, const std::string& prefab_text, Entity parent = {});
Entity instantiatePrefab(World& world, const std::filesystem::path& file, Entity parent = {});

// --- Instancias ---
// La raiz de la instancia a la que pertenece `e` (la entidad con
// PrefabInstance mas cercana hacia arriba), o una entidad vacia.
Entity prefabRoot(Entity e);
// Todas las instancias de un prefab (sus raices).
std::vector<Entity> prefabInstances(World& world, const Uuid& prefab);
// Apunta como cambios propios lo que difiere de `prefab_text`.
void recordOverrides(World& world, Entity root, const std::string& prefab_text);
// Lleva la instancia a `prefab_text` respetando sus cambios propios.
void syncInstance(World& world, Entity root, const std::string& prefab_text);
// La instancia pasa a ser la nueva version del prefab (se escribe `file`).
// Devuelve el texto nuevo (vacio si no se pudo escribir).
std::string applyInstance(World& world, Entity root, const std::filesystem::path& file, std::string* error = nullptr);
// Olvida los cambios propios y vuelve a ser igual que el prefab.
void revertInstance(World& world, Entity root, const std::string& prefab_text);
// Quita el enlace: quedan entidades normales.
void unpackInstance(World& world, Entity root);

// Tras duplicar o pegar `copy`: si no es una instancia entera (una parte de
// una), sus entidades pierden el enlace y pasan a ser anadidas por la
// instancia. Las instancias completas que haya dentro se quedan igual.
void detachCopiedLinks(World& world, Entity copy);

// Lleva a la ultima revision las instancias que se quedaron atras (al abrir
// una escena o si el .crprefab cambio). `text_of` da el texto de un prefab
// por su UUID (vacio si no existe). Devuelve cuantas instancias cambiaron.
int syncOutdatedInstances(World& world, const std::function<std::string(const Uuid&)>& text_of);

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_PREFAB_H
