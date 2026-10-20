#ifndef CRAMION_CORE_ASSET_DATABASE_H
#define CRAMION_CORE_ASSET_DATABASE_H

// CONTRATO COMPARTIDO. Implementacion: CramionCore/src/asset/ (target
// CramionAssets).

#include "CramionCore/asset/AssetTypes.h"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cramion::assets {

// Lo leido del disco para unas rutas concretas (AssetDatabase::scanPaths):
// los assets que hay ahora dentro de cada una (archivo o carpeta, exista o
// no). Se hace en cualquier hilo; applyScan lo aplica en el de la base.
struct AssetScan {
    std::vector<std::filesystem::path> roots;  // las rutas pedidas
    std::vector<AssetInfo> found;
    std::vector<std::filesystem::file_time_type> stamps;  // fecha de cada uno de `found`
};

// Lo que cambio en la base al aplicar un AssetScan.
struct AssetChanges {
    std::vector<Uuid> added;
    std::vector<Uuid> removed;
    std::vector<Uuid> modified;  // mismo UUID y ruta, otra fecha: su contenido cambio
    std::vector<Uuid> moved;     // mismo UUID en otra ruta (movido o renombrado fuera)
    bool any() const { return !added.empty() || !removed.empty() || !modified.empty() || !moved.empty(); }
};

// Indice de todos los assets de un proyecto: recorre Assets/ (recursivo) y
// lee solo la cabecera de cada .crdata y el UUID de cada .crscene. No carga
// datos pesados. Incluye tambien los assets integrados (primitivas).
class AssetDatabase {
public:
    // `assets_root` = <proyecto>/Assets. Hace un refresh().
    void open(const std::filesystem::path& assets_root);
    void close();
    bool isOpen() const { return !root_.empty(); }

    // Vuelve a recorrer la carpeta (archivos nuevos, borrados, movidos).
    void refresh();

    // Rescan incremental: solo `paths` (archivos o carpetas, recursivo), las
    // que avisa el vigilante de carpetas (FileWatcher). scanPaths lee el disco
    // y no toca la base (se puede llamar desde otro hilo, con `root` copiada);
    // applyScan pone el resultado en la base y dice que cambio. Un asset que
    // estaba dentro de una ruta pedida y ya no aparece sale; uno con el UUID
    // de otro que ya no existe en su ruta vieja se toma como movido.
    static AssetScan scanPaths(const std::filesystem::path& root, std::vector<std::filesystem::path> paths);
    AssetChanges applyScan(const AssetScan& scan);
    AssetChanges refreshPaths(std::vector<std::filesystem::path> paths) {
        return applyScan(scanPaths(root_, std::move(paths)));
    }

    const std::filesystem::path& root() const { return root_; }

    std::optional<AssetInfo> find(const Uuid& uuid) const;
    // Todos los assets (los integrados primero).
    const std::vector<AssetInfo>& all() const { return assets_; }
    // Los de una carpeta concreta (no recursivo), para el navegador.
    std::vector<AssetInfo> inFolder(const std::filesystem::path& folder) const;

    // Mueve/renombra un asset dentro de Assets/ (su UUID no cambia) y borra.
    bool move(const Uuid& uuid, const std::filesystem::path& new_path);
    bool remove(const Uuid& uuid);

private:
    std::filesystem::path root_;
    std::vector<AssetInfo> assets_;
    std::unordered_map<Uuid, std::size_t> by_uuid_;
    // Fecha de cada archivo al leerlo (applyScan distingue "cambio su
    // contenido" de "solo se aviso otra vez").
    std::unordered_map<Uuid, std::filesystem::file_time_type> stamps_;
    void reindex();
};

}  // namespace cramion::assets

#endif  // CRAMION_CORE_ASSET_DATABASE_H
