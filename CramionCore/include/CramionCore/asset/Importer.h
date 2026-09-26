#ifndef CRAMION_CORE_ASSET_IMPORTER_H
#define CRAMION_CORE_ASSET_IMPORTER_H

// CONTRATO COMPARTIDO. Implementacion: CramionCore/src/asset/ (target
// CramionAssets).

#include "CramionCore/asset/AssetManager.h"
#include "CramionCore/asset/AssetTypes.h"

#include <atomic>
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

struct ModelImportSettings {
    // Personaje animado: una sola pieza con esqueleto y animaciones (no se
    // parte en nodos). Si es false, cada malla del archivo es una pieza con
    // su nodo (hijos en la jerarquia al instanciarlo, como Unity).
    bool animated = false;
    // Escala uniforme aplicada al importar (unidades del archivo -> metros).
    float scale = 1.0f;
    // Normal maps de convenio DirectX (+Y hacia abajo).
    bool directx_normals = false;
    // Modelos con muchas piezas (una palmera con 60 hojas, cada una su nodo):
    // pasadas de `combine_above_parts` se juntan por material (la palmera
    // queda en tronco + hojas). Cada pieza es un objeto en la escena, con su
    // coste de CPU (sincronizar, sombras por cascada, gizmos): 5 palmeras
    // eran 1600 objetos. false = siempre una pieza por nodo (jerarquia entera).
    bool combine_meshes = true;
    int combine_above_parts = 16;
};

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

// Escribe un modelo generado por el motor (no importado de un archivo), p. ej.
// el lote de mallas estaticas de una escena al exportar. `nodes` y `parts`
// como en ModelAsset; las texturas de las piezas deben ir comprimidas
// (`encoded`) o sin decodificar. Devuelve false y rellena `error` si falla.
bool writeGeneratedModel(const std::filesystem::path& file, const Uuid& uuid, const std::string& name,
                         const std::vector<ModelNode>& nodes, const std::vector<asset::ModelData>& parts,
                         std::string* error = nullptr);

// Segun la extension: .obj .fbx .gltf .glb .dae -> modelo, .hdr -> cielo.
ImportResult importAny(const std::filesystem::path& source,
                       const std::filesystem::path& destination_folder,
                       ImportProgress* progress = nullptr);

}  // namespace cramion::assets

#endif  // CRAMION_CORE_ASSET_IMPORTER_H
