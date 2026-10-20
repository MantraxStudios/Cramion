#ifndef CRAMION_CORE_ASSET_FILE_WATCHER_H
#define CRAMION_CORE_ASSET_FILE_WATCHER_H

// Vigilancia de una carpeta (recursiva) en un hilo propio, para la recarga en
// caliente de assets editados fuera del editor (como el Auto Refresh de Unity
// o el Auto Reimport de UE5).
//
//   Windows  ReadDirectoryChangesW con OVERLAPPED (un solo HANDLE para todo el
//            arbol; dice QUE ruta cambio, no solo "algo cambio").
//   Linux    inotify (un watch por carpeta; las carpetas nuevas se anaden
//            solas y sus archivos ya creados se avisan como nuevos).
//   Otros    sin avisos (start() devuelve false: quien lo usa refresca a mano).
//
// Las rafagas se juntan: una ruta se entrega cuando lleva `debounce` (300 ms)
// sin cambiar, y como mucho `max_delay` (2 s) despues del primer aviso aunque
// siga cambiando (copiar una carpeta grande no espera al final). Varias
// escrituras del mismo archivo son un solo cambio. Un renombrado llega como
// Removed (ruta vieja) + Added (ruta nueva).
//
// El hilo solo toca su propio estado: poll() se llama desde cualquier hilo
// (normalmente el principal, cada frame) y no bloquea.

#include <chrono>
#include <filesystem>
#include <memory>
#include <vector>

namespace cramion::assets {

class FileWatcher {
public:
    enum class Kind { Added, Removed, Modified };
    struct Change {
        std::filesystem::path path;  // absoluta (dentro de la raiz)
        Kind kind = Kind::Modified;
    };
    struct Batch {
        std::vector<Change> changes;
        // El sistema perdio avisos (buffer desbordado, carpeta raiz borrada o
        // movida): hay que recorrer todo otra vez.
        bool overflow = false;
        bool empty() const { return changes.empty() && !overflow; }
    };

    FileWatcher();
    ~FileWatcher();
    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    // Empieza a vigilar `root` (para el hilo de antes si lo habia). false si
    // la plataforma no avisa o no se pudo abrir la carpeta.
    bool start(const std::filesystem::path& root,
               std::chrono::milliseconds debounce = std::chrono::milliseconds(300),
               std::chrono::milliseconds max_delay = std::chrono::milliseconds(2000));
    // Para el hilo (espera a que salga: unos milisegundos).
    void stop();
    bool running() const;
    const std::filesystem::path& root() const;

    // Lo que ya esta listo (tranquilo durante `debounce`). false si no hay nada.
    bool poll(Batch& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cramion::assets

#endif  // CRAMION_CORE_ASSET_FILE_WATCHER_H
