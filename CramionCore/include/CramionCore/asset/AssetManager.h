#ifndef CRAMION_CORE_ASSET_MANAGER_H
#define CRAMION_CORE_ASSET_MANAGER_H

// CONTRATO COMPARTIDO. Implementacion: CramionCore/src/asset/ (target
// CramionAssets). Lo usa el puente con el renderizador (RenderSync, ECS).

#include "CramionCore/asset/AssetDatabase.h"

#include <CramionFX/CramionFX.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cramion::lighting {
struct LightmapUvSet;
}

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

// Los colores de un material tal como los ve el horneado de luz
// (readMaterialColors, PLAN-LIGHTMAPS 0.2): factor del material por la media
// lineal de su textura, en las unidades del render.
struct MaterialColors {
    core::Vec3 albedo{1.0f, 1.0f, 1.0f};  // lineal: base_color x media(toLinear(textura)); sin limitar
    core::Vec3 emission{};                // lineal: emisivo x 6 x media(toLinear(textura emisiva)) (skinned.frag)
    float opaque_fraction = 1.0f;         // texeles con alfa >= 0.5 de la textura de color (recorte por alfa)
    bool transparent = false;             // vidrio, agua
    float metallic = 0.0f;
    float roughness = 0.8f;
    bool albedo_texture = false;          // tiene textura de color...
    bool albedo_texture_read = false;     // ... y se pudo leer (si no, el factor x 0.6)
};

// RAM de los modelos que guarda el AssetManager (ramStats). Lo que
// RenderSync ya movio a la escena no cuenta (aqui queda vacio).
struct AssetRamStats {
    std::size_t models = 0;               // en memoria
    std::uint64_t bytes = 0;              // lo que ocupan (aprox.: mallas, texturas, animaciones)
    std::size_t evictable = 0;            // sin nadie mas que los use (se pueden soltar)
    std::uint64_t evictable_bytes = 0;
    std::uint64_t budget = 0;             // presupuesto (0 = sin limite)
    std::uint64_t evicted = 0;            // soltados desde el arranque
    std::uint64_t evicted_bytes = 0;
    std::uint64_t trims = 0;              // avisos de memoria baja atendidos (trim)
};

// Aviso de memoria baja del sistema (Android onTrimMemory/onLowMemory).
enum class TrimLevel : int {
    Moderate = 0,  // TRIM_MEMORY_RUNNING_MODERATE / UI_HIDDEN: lo que lleva un rato sin usarse
    Low = 1,       // RUNNING_LOW / BACKGROUND: todo lo que nadie usa
    Critical = 2,  // RUNNING_CRITICAL / COMPLETE / onLowMemory: igual, y avisar en el log
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
    // `uvs`: las UV de lightmap de la escena (LightmapUvSet): cada pieza que
    // tenga disposicion se parte y lleva su UV2 antes de reagrupar y de los
    // LODs (PLAN-LIGHTMAPS 1.4). Sin el (lote estatico, miniaturas), sin UV2.
    static std::shared_ptr<ModelAsset> readModel(const Uuid& uuid, const std::filesystem::path& file,
                                                 const std::string& name, bool decode_textures = true,
                                                 const lighting::LightmapUvSet* uvs = nullptr);
    // Solo las mallas (vertices, indices, submallas, materiales, nodos y
    // huesos): sin los bytes de las texturas, sin LODs y sin cache. Para
    // quien necesita la geometria de un modelo que el renderizador ya se
    // llevo (RenderSync mueve cada pieza dibujada a scene::Scene y aqui queda
    // vacia), como el horneado de luz. `wanted`: que piezas (vacio = todas;
    // las demas quedan vacias). Las primitivas se generan. Cualquier hilo.
    static std::shared_ptr<ModelAsset> readModelGeometry(const Uuid& uuid, const std::filesystem::path& file,
                                                         const std::string& name,
                                                         const std::vector<bool>& wanted = {});
    // Las texturas incrustadas de un .crdata tal como estan (sin decodificar,
    // con su uso: TextureData::usage), sin las mallas: exportar el juego.
    static std::vector<asset::TextureData> readModelTextures(const std::filesystem::path& file);
    // Colores de los materiales de un modelo para el horneado de luz: lee el
    // .crdata con sus texturas, resuelve las perezosas y devuelve, por pieza y
    // por material, el color medio (MaterialColors). `wanted`: que piezas
    // (vacio = todas; las demas quedan vacias). Suelta las texturas al
    // terminar. Las primitivas se generan. Cualquier hilo (el del horneado).
    static std::vector<std::vector<MaterialColors>> readMaterialColors(const Uuid& uuid, const std::filesystem::path& file,
                                                                       const std::vector<bool>& wanted = {});

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

    // --- RAM (presupuesto y expulsion LRU) ---
    // Un modelo se puede soltar si nadie mas tiene su shared_ptr (ni
    // RenderSync ni un hilo): soltarlo libera de verdad su memoria y, si se
    // vuelve a pedir, se lee otra vez del .crdata. Lo que esta en uso nunca
    // se suelta. Presupuesto 0 = sin limite (como antes: nunca se suelta nada).
    void setRamBudget(std::uint64_t bytes) { ram_budget_ = bytes; }
    std::uint64_t ramBudget() const { return ram_budget_; }
    // Si lo que ocupa pasa del presupuesto, suelta los que se pueden, del
    // usado hace mas tiempo al mas reciente, hasta quedar por debajo. Lo
    // usado en los ultimos `min_idle_ms` no se toca (lo recien leido que aun
    // no ha recogido nadie). Devuelve los bytes soltados.
    std::uint64_t enforceRamBudget(std::uint64_t min_idle_ms = 2000);
    // Memoria baja (Android): Moderate suelta lo que no se usa desde hace
    // `idle_ms`; Low y Critical, todo lo que se puede. Devuelve los bytes
    // soltados.
    std::uint64_t trim(TrimLevel level, std::uint64_t idle_ms = 30000);
    AssetRamStats ramStats() const;
    // Lo que ocupa un modelo en RAM (aprox.).
    static std::uint64_t modelBytes(const ModelAsset& model);

    // --- UV de lightmap de la escena abierta (.crbake v4 o "Generar UV de
    // lightmap") --- Lo que se lea desde ahora sale con sus UV2; lo ya cargado
    // no cambia (quien las cambia recarga: RenderSync::reset). nullptr = sin UV2.
    void setLightmapUvs(std::shared_ptr<const lighting::LightmapUvSet> uvs);
    const std::shared_ptr<const lighting::LightmapUvSet>& lightmapUvs() const { return lightmap_uvs_; }

private:
    struct Loader;  // hilos de fondo y sus colas
    void takeFinished(const Uuid* only);

    AssetDatabase& database_;
    std::filesystem::path cache_folder_;
    std::unordered_map<Uuid, std::shared_ptr<ModelAsset>> models_;
    // RAM: cuando se uso cada modelo por ultima vez (ms de steady_clock) y lo soltado.
    void touch(const Uuid& uuid);
    std::uint64_t release(std::uint64_t target, std::uint64_t min_idle_ms);
    std::unordered_map<Uuid, std::uint64_t> last_use_;
    std::uint64_t ram_budget_ = 0;
    std::uint64_t evicted_ = 0;
    std::uint64_t evicted_bytes_ = 0;
    std::uint64_t trims_ = 0;
    std::unordered_set<Uuid> failed_loads_;
    std::unique_ptr<Loader> loader_;
    std::shared_ptr<const lighting::LightmapUvSet> lightmap_uvs_;
};

}  // namespace cramion::assets

#endif  // CRAMION_CORE_ASSET_MANAGER_H
