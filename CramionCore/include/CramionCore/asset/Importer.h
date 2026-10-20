#ifndef CRAMION_CORE_ASSET_IMPORTER_H
#define CRAMION_CORE_ASSET_IMPORTER_H

// CONTRATO COMPARTIDO. Implementacion: CramionCore/src/asset/ (target
// CramionAssets).

#include "CramionCore/asset/AssetManager.h"
#include "CramionCore/asset/AssetTypes.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace cramion::assets {

// Avance de una importacion, para mostrarlo desde otro hilo (el editor
// importa en segundo plano). El importador escribe; la interfaz lee.
class ImportProgress {
public:
    void report(float fraction, std::string stage) {
        fraction_.store(fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction));
        std::lock_guard lock(mutex_);
        stage_ = std::move(stage);
    }
    // Solo la fraccion (p. ej. mientras assimp lee): la etapa no cambia.
    void report(float fraction) {
        fraction_.store(fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction));
    }
    float fraction() const { return fraction_.load(); }
    std::string stage() const {
        std::lock_guard lock(mutex_);
        return stage_;
    }

private:
    std::atomic<float> fraction_{0.0f};
    mutable std::mutex mutex_;
    std::string stage_ = "En cola";
};

enum class ModelLodMode : std::uint8_t { Off = 0, Auto = 1, Manual = 2 };

struct ModelImportSettings {
    // Personaje animado: una sola pieza con esqueleto y animaciones (no se
    // parte en nodos). Si es false, cada malla del archivo es una pieza con
    // su nodo (hijos en la jerarquia al instanciarlo, como Unity).
    bool animated = false;
    // Scale Factor (como Unity): escala uniforme horneada en la malla, los
    // huesos y las animaciones.
    float scale = 1.0f;
    // Convert Units: ademas, la unidad que dice el archivo (un FBX en
    // centimetros de Mixamo, Maya o 3ds Max mide 100 veces menos). Los
    // importados antes de la 0.7.2 no la tenian (al reimportarlos sigue
    // apagada salvo que se active).
    bool convert_units = true;
    // (Solo lectura: metros por unidad que dijo el archivo al importarlo.)
    float file_unit_scale = 1.0f;
    // Normal maps de convenio DirectX (+Y hacia abajo).
    bool directx_normals = false;
    // Modelos con muchas piezas (una palmera con 60 hojas, cada una su nodo):
    // pasadas de `combine_above_parts` se juntan por material (la palmera
    // queda en tronco + hojas). Cada pieza es un objeto en la escena, con su
    // coste de CPU (sincronizar, sombras por cascada, gizmos): 5 palmeras
    // eran 1600 objetos. false = siempre una pieza por nodo (jerarquia entera).
    bool combine_meshes = true;
    int combine_above_parts = 16;
    // LODs del modelo (como el LOD Group de Unity o los LOD de Unreal):
    //   Off    = sin LODs (las mallas _LOD1.._LODn del archivo se descartan);
    //   Auto   = las _LOD1.._LODn del archivo si las hay; si no, automaticos
    //            (meshoptimizer, al cargar);
    //   Manual = solo las _LOD1.._LODn del archivo (sin ellas, ninguno).
    ModelLodMode lod_mode = ModelLodMode::Auto;
};

// LODs de un .crdata: el modo y, por cada pieza con LODs hechos a mano, sus
// piezas por nivel (la primera, LOD0; las demas no tienen nodo y solo sirven
// de LOD). Lo escribe el importador en la cabecera y lo usa
// AssetManager::readModel.
struct ModelLodInfo {
    ModelLodMode mode = ModelLodMode::Auto;
    std::vector<std::vector<std::int32_t>> groups;
};
ModelLodInfo modelLodInfo(const std::string& settings_json);

struct ImportResult {
    bool ok = false;
    AssetInfo info;       // el asset creado (uuid, ruta del .crdata...)
    std::string message;  // error o resumen para la consola
};

// Convierte un archivo original en un .crdata dentro de `destination_folder`
// (una carpeta de Assets/). El original NO se copia: el .crdata lo contiene
// todo (texturas incluidas) y lleva un UUID nuevo. `progress` (opcional)
// recibe la etapa y la fraccion completada.
ImportResult importModel(const std::filesystem::path& source,
                         const std::filesystem::path& destination_folder,
                         const ModelImportSettings& settings = {},
                         ImportProgress* progress = nullptr);

// Cielo HDR (.hdr equirectangular): el archivo va incrustado tal cual.
ImportResult importEnvironment(const std::filesystem::path& source,
                               const std::filesystem::path& destination_folder,
                               ImportProgress* progress = nullptr);

// Vuelve a importar un modelo desde su archivo original (el que guarda la
// cabecera del .crdata) con `settings`, en el MISMO archivo y con el MISMO
// UUID: las escenas y los prefabs que lo usan siguen apuntando a el (sus
// piezas pueden cambiar: ver EditorApp::rebuildModelInstances). Falla si el
// original ya no esta en el disco.
ImportResult reimportModel(const AssetInfo& info, const ModelImportSettings& settings,
                           ImportProgress* progress = nullptr);
// Los ajustes con que se importo un .crdata (o los de por defecto).
ModelImportSettings modelImportSettings(const std::filesystem::path& file);
// Ruta (UTF-8) del archivo original de un .crdata ("" si no la guarda).
std::string modelImportSource(const std::filesystem::path& file);

// Escribe un modelo generado por el motor (no importado de un archivo), p. ej.
// el lote de mallas estaticas de una escena al exportar. `nodes` y `parts`
// como en ModelAsset; las texturas de las piezas deben ir comprimidas
// (`encoded`) o sin decodificar. Devuelve false y rellena `error` si falla.
// `animated`: un personaje (una pieza con esqueleto y clips), como los
// modelos importados con animaciones.
bool writeGeneratedModel(const std::filesystem::path& file, const Uuid& uuid, const std::string& name,
                         const std::vector<ModelNode>& nodes, const std::vector<asset::ModelData>& parts,
                         std::string* error = nullptr, bool animated = false);

// Segun la extension: .obj .fbx .gltf .glb .dae -> modelo, .hdr -> cielo.
ImportResult importAny(const std::filesystem::path& source,
                       const std::filesystem::path& destination_folder,
                       ImportProgress* progress = nullptr);

}  // namespace cramion::assets

#endif  // CRAMION_CORE_ASSET_IMPORTER_H
