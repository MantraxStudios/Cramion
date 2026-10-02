#ifndef CRAMION_CORE_PROJECT_SAVE_FILE_H
#define CRAMION_CORE_PROJECT_SAVE_FILE_H

// Archivos de partida (.crsave): texto JSON, o comprimido con zstd con una
// cabecera "CRSV" + version + tamano original. Se lee cualquiera de los dos.
// La escritura es atomica (temporal + renombrar): un corte de luz a mitad no
// deja la partida rota, se queda la anterior.

#include <filesystem>
#include <string>

namespace cramion::project {

bool writeSaveFile(const std::filesystem::path& file, const std::string& text, bool compress, std::string* error = nullptr);
bool readSaveFile(const std::filesystem::path& file, std::string& text, std::string* error = nullptr);

}  // namespace cramion::project

#endif  // CRAMION_CORE_PROJECT_SAVE_FILE_H
