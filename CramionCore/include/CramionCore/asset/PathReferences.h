#ifndef CRAMION_CORE_ASSET_PATH_REFERENCES_H
#define CRAMION_CORE_ASSET_PATH_REFERENCES_H

// Referencias por ruta al mover o renombrar archivos de Assets/.
//
// Los assets con UUID (.crdata, .crscene, .crprefab, .crmat...) se referencian
// por UUID y moverlos no rompe nada. Pero audio, imagenes de UI, texturas de
// los .crmat, scripts, videos, grafos y algunas escenas se guardan por su ruta
// dentro de Assets ("Audio/golpe.wav"): al moverlos, todo lo que los usaba se
// quedaba apuntando a la ruta vieja sin avisar. Esto busca y reescribe esas
// rutas en los JSON del proyecto (escenas, prefabs, materiales, controladores,
// VFX, grafos...), como los redirectores de UE5 pero sin dejar nada detras.
//
// Se cambian solo valores de texto JSON que son exactamente la ruta (o, si es
// una carpeta, que empiezan por "carpeta/"), tambien con el prefijo
// "Assets/". El texto que lo contiene a medias no se toca.

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace cramion::assets {

// Ruta relativa a Assets con '/' (como la escribe el editor).
struct PathRename {
    std::string from;
    std::string to;
    bool folder = false;  // tambien todo lo que hay dentro
};

// Un valor (un campo de texto de un componente en memoria): true si cambio.
bool rewritePath(std::string& value, std::span<const PathRename> renames);

// Reescribe los literales de texto de un JSON. Devuelve cuantos cambio.
int rewritePathReferences(std::string& json_text, std::span<const PathRename> renames);

struct PathRewriteReport {
    std::size_t scanned = 0;  // archivos leidos
    int references = 0;       // rutas cambiadas
    std::vector<std::filesystem::path> updated;  // archivos reescritos
    std::vector<std::filesystem::path> failed;   // no se pudieron escribir
};

// Todos los JSON del proyecto dentro de `assets_root` (en paralelo; solo se
// reescriben los que usan alguna ruta, con temporal + rename). `skip`: archivos
// que no se tocan (los que el editor guarda el mismo, p. ej. la escena abierta
// si se quiere conservar). Para cualquier hilo.
PathRewriteReport rewriteProjectReferences(const std::filesystem::path& assets_root,
                                           std::span<const PathRename> renames,
                                           std::span<const std::filesystem::path> skip = {});

}  // namespace cramion::assets

#endif  // CRAMION_CORE_ASSET_PATH_REFERENCES_H
