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
};

}  // namespace cramion::assets

#endif  // CRAMION_CORE_ASSET_DATABASE_H
