#ifndef CRAMION_EDITOR_LOCOMOTION_PACK_H
#define CRAMION_EDITOR_LOCOMOTION_PACK_H

// Importa el "Locomotion Pack" de Mixamo (un .zip o su carpeta: el personaje
// y sus animaciones en FBX) a un proyecto, para la plantilla Tercera persona
// avanzada. El motor no reparte el pack (licencia de Mixamo): cada usuario
// usa el suyo, descargado de mixamo.com.
//
//   - El personaje (el FBX con malla) se importa como modelo animado.
//   - Cada animacion se pasa a un clip suelto (.cranim) y se deja "en el
//     sitio": el desplazamiento de la cadera en horizontal (y el giro de las
//     de girar) se quita, porque al personaje lo mueve su script. Lo que
//     avanzaba cada clip se mide (metros por segundo, grados) para que el
//     script y el Blend Tree vayan a la misma velocidad y los pies no patinen.

#include <CramionCore/Uuid.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace cramion::editor::locomotion {

struct ClipInfo {
    std::string key;       // idle, walk, run, strafe_walk_left... (ver clipKey)
    std::string relative;  // ruta del .cranim dentro de Assets
    Uuid uuid;
    float duration = 0.0f;
    float speed = 0.0f;         // m/s que avanzaba la cadera (antes de quitarlo)
    float turn_degrees = 0.0f;  // giro de la cadera en todo el clip (+ = izquierda)
    float rise = 0.0f;          // lo que sube la cadera como mucho (m)
    // Saltos: cuando la cadera deja atras el agachado y sube por encima de su
    // altura del principio (despega) y cuando vuelve a ella (aterriza), en s.
    float takeoff = 0.0f;
    float landing = 0.0f;
};

struct PackResult {
    bool ok = false;
    std::string error;
    Uuid character;
    std::string character_relative;
    float hips_height = 0.0f;  // altura de la cadera en reposo (m)
    std::vector<ClipInfo> clips;
    std::vector<std::string> log;
    const ClipInfo* clip(const std::string& key) const;
};

// El pack en Descargas (Locomotion Pack.zip o una carpeta con esos FBX), o vacio.
std::filesystem::path findDownloadedPack();
// Tiene pinta de ser el pack (zip o carpeta con idle, walking y running).
bool looksLikePack(const std::filesystem::path& source);

// Importa el pack en `assets_folder` (Models/Locomotion y Animations/Locomotion).
PackResult importPack(const std::filesystem::path& source, const std::filesystem::path& assets_folder,
                      const std::function<void(const std::string& stage, float fraction)>& progress = {});

}  // namespace cramion::editor::locomotion

#endif  // CRAMION_EDITOR_LOCOMOTION_PACK_H
