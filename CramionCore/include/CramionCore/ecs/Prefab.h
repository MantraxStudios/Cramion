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
// PrefabLink. Los prefabs dentro de prefabs se guardan como copias sueltas
// (salvo con prefab.SaveNested: ver "Anidados y variantes" abajo).
//
// Anidados y variantes (opcionales; un prefab sin ellos se escribe y se lee
// exactamente igual que antes):
//   - Un registro de "entities" con "nestedPrefab": {"uuid", "path", "name"}
//     es una instancia de otro prefab guardada como enlace. Al leerlo se
//     expande con las entidades de ese prefab (ids derivados del suyo y del
//     de cada entidad: estables) y sus "patches" (diferencias: campos,
//     componentes anadidos o quitados, entidades borradas). Los hijos anadidos
//     a una parte anidada son registros normales cuyo padre es un id derivado.
//   - "variantOf": {"uuid", "path", "name"} arriba del todo: el prefab es una
//     variante de otro. Sus entidades son las de la base (mismos ids) con sus
//     "patches", mas las suyas en "entities". Se pueden encadenar.
//   - readPrefabFile, instantiatePrefab, syncInstance, PrefabSpawn y la
//     plantilla compilada trabajan con el documento ya resuelto (plano, como
//     los de siempre), asi que todos dan lo mismo. Su "revision" mezcla la
//     suya con la de lo que usa: cambiar un hijo o la base pone al dia las
//     instancias de los que lo usan.
//   - applyInstance guarda al reves: lo que difiere de la base o del anidado
//     queda como diferencia, y lo que no, se sigue heredando.
//   - Un ciclo (A dentro de B dentro de A, o variantes en circulo) se rechaza.

#include "CramionCore/Uuid.h"
#include "CramionCore/asset/AssetTypes.h"
#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/World.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
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
// El texto de un .crprefab (vacio si no se puede leer), ya resuelto si tiene
// anidados o variante (vacio si falta su base o hay un ciclo). Uno de los de
// siempre sale tal cual.
std::string readPrefabFile(const std::filesystem::path& file);
// El texto tal como esta en disco (sin resolver).
std::string readPrefabFileRaw(const std::filesystem::path& file);
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
// Un .crprefab ya parseado: el editor lo guarda (por ruta y fecha del archivo)
// para no volver a parsear el JSON en cada paso de deshacer. nullptr si el
// texto no es un objeto JSON. (El PrefabDocument de PrefabSpawn.h es el de
// instanciar en tandas: solo los registros, y nullptr si no hay entidades.)
struct ParsedPrefab;
std::shared_ptr<const ParsedPrefab> parsePrefabText(const std::string& prefab_text);
int prefabRevision(const ParsedPrefab& prefab);
// recordOverrides con el documento ya parseado. Solo escribe la lista si cambia.
void recordOverrides(World& world, Entity root, const ParsedPrefab& prefab);
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

// --- Anidados y variantes ---
inline constexpr const char* kPrefabNestedKey = "nestedPrefab";
inline constexpr const char* kPrefabVariantKey = "variantOf";

// Donde esta cada prefab por su UUID (el editor y el jugador la ponen con su
// base de datos de assets; se puede leer desde cualquier hilo). Un enlace se
// busca primero por su ruta relativa y, si no esta o es otro, aqui.
void setPrefabLocations(std::vector<std::pair<Uuid, std::filesystem::path>> locations);
std::filesystem::path locatePrefab(const Uuid& prefab);
// Sube cada vez que se escribe un .crprefab desde aqui (aplicar, variantes...):
// las caches de texto o de plantillas por fecha de archivo se vacian al
// cambiar, porque un prefab con anidados o variante cambia cuando cambia lo
// que usa aunque su archivo no.
std::uint64_t prefabWriteGeneration();

// Si el texto puede tener anidados o variante (busqueda barata de las claves).
bool prefabNeedsResolve(const std::string& prefab_text);
// El documento plano de un prefab con anidados o variante (`file`: donde
// esta, para las rutas relativas de sus enlaces). Sin ellos, el mismo texto.
// Vacio (y `error`) si falta su base o hay un ciclo.
std::string resolvePrefabText(const std::string& prefab_text, const std::filesystem::path& file = {},
                              std::string* error = nullptr);
// UUID de la base de una variante (invalido si no lo es). Texto sin resolver.
Uuid prefabVariantBase(const std::string& raw_text);
// Todos los prefabs que usa (bases y anidados, recursivo, sin repetir).
std::vector<Uuid> prefabDependencies(const std::filesystem::path& file);

// Crea `variant_file`: una variante de `base_file` sin cambios propios.
bool createPrefabVariant(const std::filesystem::path& base_file, const std::filesystem::path& variant_file,
                         std::string* error = nullptr);
// La instancia `root` pasa a ser la primera instancia de una variante nueva
// de su prefab (`base_file`); sus cambios propios quedan como diferencias de
// la variante. Devuelve el texto (resuelto) de la variante.
std::string createVariantFromInstance(World& world, Entity root, const std::filesystem::path& base_file,
                                      const std::filesystem::path& variant_file, std::string* error = nullptr);
// La variante pasa a ser un prefab normal con lo que tenia (mismos ids).
bool detachPrefabVariant(const std::filesystem::path& file, std::string* error = nullptr);
// El anidado cuya raiz es `nested` (id en el prefab) pasa a ser entidades
// propias (mismos ids: las instancias no notan nada).
bool unlinkNestedPrefab(const std::filesystem::path& file, const std::string& nested, std::string* error = nullptr);

// De donde sale una entidad del prefab (`source`: su id en el documento resuelto).
struct PrefabOrigin {
    Uuid prefab;                 // prefab del que viene (la base o el anidado)
    std::filesystem::path file;  // su archivo (vacio si se perdio)
    std::string source;          // su id alli
    std::string nested;          // id de la raiz del anidado en este prefab ("" = de la base)
    bool variant_base = false;   // viene de la base de la variante
};
std::optional<PrefabOrigin> prefabEntityOrigin(const std::filesystem::path& file, const std::string& source);
// Las diferencias guardadas en el prefab respecto a su base y a sus anidados,
// como claves de cambios propios ("id|Componente|campo", "id|#name",
// "id|+Comp", "id|-Comp", "id|-") con los ids del documento resuelto.
std::vector<std::string> prefabInheritedOverrides(const std::filesystem::path& file);

// Un cambio propio de una instancia ("id|Componente|campo" o "id|#clave"):
// Revertir (vuelve al valor del prefab) y Aplicar (su valor pasa al prefab
// `file`, o a la base / al anidado del que viene con `to_base`; en ese caso
// la diferencia que tuviera el prefab en ese campo se quita). Devuelve el
// texto nuevo (resuelto) del prefab de la instancia; vacio si no se pudo.
bool revertOverride(World& world, Entity root, const std::string& key, const std::string& prefab_text);
std::string applyOverride(World& world, Entity root, const std::string& key, const std::filesystem::path& file,
                          bool to_base = false, std::string* error = nullptr);
// En la pestana del prefab (sin instancia de por medio): la diferencia `key`
// del prefab respecto a su base o anidado. Revertir pone en `e` el valor de
// la base; aplicar escribe el valor de `e` en la base o el anidado.
bool revertInheritedOverride(World& world, Entity root, const std::string& key, const std::filesystem::path& file);
bool applyInheritedOverride(World& world, Entity root, const std::string& key, const std::filesystem::path& file,
                            std::string* error = nullptr);

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_PREFAB_H
