#ifndef CRAMION_EDITOR_MODEL_PREVIEWS_H
#define CRAMION_EDITOR_MODEL_PREVIEWS_H

// Miniaturas de los modelos del navegador de proyecto, como las de Unity:
// cada modelo se dibuja una vez (rasterizador en CPU, en un hilo aparte, vista
// 3/4 con sus colores y texturas) y se guarda en Library/Thumbnails/<uuid>.png.
// Las siguientes veces se lee el PNG; se rehace si el .crdata es mas nuevo.

#include <CramionCore/Uuid.h>

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace cramion::asset {
struct ImageRgba8;
}
namespace cramion::assets {
struct ModelAsset;
}

namespace cramion::editor {

class ModelPreviews {
public:
    ModelPreviews() = default;
    ~ModelPreviews() { stop(); }
    ModelPreviews(const ModelPreviews&) = delete;
    ModelPreviews& operator=(const ModelPreviews&) = delete;

    // Carpeta de la cache (Library/Thumbnails del proyecto). Arranca el hilo.
    void start(const std::filesystem::path& cache_folder);
    void stop();
    // Olvida lo comprobado (tras reimportar: se vuelve a mirar la fecha).
    void invalidate();

    // El PNG del modelo si ya esta hecho; si no, lo encarga y devuelve nada.
    // `file` = el .crdata (vacio en las primitivas).
    std::optional<std::filesystem::path> preview(const Uuid& uuid, const std::filesystem::path& file,
                                                 const std::string& name);

    // El dibujo en si (tambien para las pruebas): `size` x `size` RGBA.
    static bool render(const assets::ModelAsset& model, std::uint32_t size, asset::ImageRgba8& out);

private:
    struct Job {
        Uuid uuid;
        std::filesystem::path file;
        std::string name;
    };
    void run();
    std::filesystem::path pngFor(const Uuid& uuid) const;

    std::filesystem::path cache_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> queue_;
    std::unordered_set<Uuid> queued_;
    std::unordered_set<Uuid> failed_;
    std::unordered_map<Uuid, bool> ready_;  // comprobado en disco: al dia
    bool quit_ = false;
    std::thread thread_;
};

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_MODEL_PREVIEWS_H
