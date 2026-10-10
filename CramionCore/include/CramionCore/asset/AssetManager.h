#ifndef CRAMION_CORE_ASSET_MANAGER_H
#define CRAMION_CORE_ASSET_MANAGER_H

// CONTRATO COMPARTIDO. Implementacion: CramionCore/src/asset/ (target
// CramionAssets). Lo usa el puente con el renderizador (RenderSync, ECS).

#include "CramionCore/asset/AssetDatabase.h"

#include <CramionFX/CramionFX.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cramion::assets {

// Un nodo de la jerarquia de un modelo importado. Al instanciar el modelo en
// una escena, cada nodo es una entidad hija (Transform con `local`), y los
// que tienen `part` >= 0 llevan un MeshRenderer que dibuja esa pieza.
struct ModelNode {
    std::string name;
    std::int32_t parent = -1;              // -1 = raiz
    core::Mat4 local = core::Mat4::identity();
    std::int32_t part = -1;                // indice en ModelAsset::parts, -1 = vacio
};

// Un modelo importado y ya en memoria (texturas decodificadas). Cada pieza es
// un ModelData completo para el renderizador (sus vertices en el espacio de
// SU nodo). Los animados tienen una sola pieza con esqueleto.
struct ModelAsset {
    Uuid uuid{};
    std::string name;
    std::vector<ModelNode> nodes;          // nodes[0] = raiz
    std::vector<std::shared_ptr<asset::ModelData>> parts;
    bool animated = false;
    std::vector<std::string> animation_names;  // de la pieza animada
};

// Carga y guarda en memoria los assets de un proyecto. Un asset se carga una
// vez aunque lo usen muchas entidades.
class AssetManager {
public:
    explicit AssetManager(AssetDatabase& database);
    ~AssetManager();
    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    // Carpeta para archivos derivados (p. ej. <proyecto>/Library/Cache).
    void setCacheFolder(const std::filesystem::path& folder) { cache_folder_ = folder; }

    // nullptr si no existe o no se pudo leer (el error va a std::cerr). Las
    // primitivas integradas (builtin::k...) se generan por codigo.
    std::shared_ptr<const ModelAsset> loadModel(const Uuid& uuid);
    // Lee un modelo sin cache ni base de datos (se puede llamar desde otro
    // hilo: miniaturas, extraer clips). `file` vacio para las primitivas.
    // `decode_textures` = false: las texturas quedan comprimidas tal como
    // estan en el .crdata (para reescribirlas, p. ej. el lote estatico).
    static std::shared_ptr<ModelAsset> readModel(const Uuid& uuid, const std::filesystem::path& file,
                                                 const std::string& name, bool decode_textures = true);
    // Las texturas incrustadas de un .crdata tal como estan (sin decodificar,
    // con su uso: TextureData::usage), sin las mallas: exportar el juego.
    static std::vector<asset::TextureData> readModelTextures(const std::filesystem::path& file);

    // Ruta de un .hdr en disco para el renderizador
    // (VulkanRenderer::loadEnvironment): el HDR incrustado en el .crdata se
    // extrae una vez a la carpeta de cache. Vacia si no existe.
    std::filesystem::path environmentFile(const Uuid& uuid);

    // --- Carga en segundo plano (streaming) ---
    // Pide un modelo SIN esperar: si ya esta en memoria lo devuelve; si no, lo
    // lee un hilo de fondo (leer el .crdata, decodificar las texturas, generar
    // los LODs) y devuelve nullptr hasta que pollLoads() lo recoja. loadModel()
    // de algo que ya se esta leyendo espera a ese hilo (no lo lee dos veces).
    std::shared_ptr<const ModelAsset> requestModel(const Uuid& uuid);
    // En el hilo principal (cada frame): guarda lo que terminaron los hilos.
    // Devuelve cuantos modelos llegaron.
    std::size_t pollLoads();
    // No se pudo leer en segundo plano (no se vuelve a intentar hasta unload()).
    bool loadFailed(const Uuid& uuid) const { return failed_loads_.contains(uuid); }
    // Modelos pedidos que aun no han llegado.
    std::size_t loadsInFlight() const;
    // Ya en memoria (sin pedirlo ni esperar): el progreso de una carga.
    bool isLoaded(const Uuid& uuid) const { return models_.contains(uuid); }

    // Olvida lo cargado (p. ej. al reimportar o cerrar el proyecto).
    void unload(const Uuid& uuid);
    void clear();

    AssetDatabase& database() { return database_; }

private:
    struct Loader;  // hilos de fondo y sus colas
    void takeFinished(const Uuid* only);

    AssetDatabase& database_;
    std::filesystem::path cache_folder_;
    std::unordered_map<Uuid, std::shared_ptr<ModelAsset>> models_;
    std::unordered_set<Uuid> failed_loads_;
    std::unique_ptr<Loader> loader_;
};

}  // namespace cramion::assets

#endif  // CRAMION_CORE_ASSET_MANAGER_H
