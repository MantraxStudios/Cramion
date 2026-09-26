// Miniatura del proyecto para el Hub: una captura de la vista Escena en
// <proyecto>/Library/thumbnail.png. Se hace unos segundos despues de abrir el
// proyecto (ya con la escena dibujada) y cada vez que se guarda la escena, asi
// que el Hub siempre ensena como quedo el proyecto la ultima vez. La imagen
// queda en disco (cache) y el Hub la carga una vez; si cambia, la recarga.

#include "EditorApp.h"

#include <CramionFX/asset/ImageFile.h>

#include <algorithm>

namespace cramion::editor {

std::vector<std::uint8_t> captureEditorWindow(HWND hwnd, const std::filesystem::path& png);

std::filesystem::path projectThumbnailFile(const std::filesystem::path& project_file) {
    return project_file.parent_path() / "Library" / "thumbnail.png";
}

void EditorApp::saveProjectThumbnail() {
    if (!has_project_ || view_w_ < 64.0f || view_h_ < 64.0f) return;
    RECT client{};
    GetClientRect(window_.handle(), &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;  // minimizada
    const std::vector<std::uint8_t> pixels = captureEditorWindow(window_.handle(), {});
    if (pixels.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4) return;

    // Solo la imagen de la vista Escena (en pixeles de la ventana).
    const int x0 = std::clamp(static_cast<int>(view_x_), 0, width - 1);
    const int y0 = std::clamp(static_cast<int>(view_y_), 0, height - 1);
    const int x1 = std::clamp(static_cast<int>(view_x_ + view_w_), x0 + 1, width);
    const int y1 = std::clamp(static_cast<int>(view_y_ + view_h_), y0 + 1, height);
    // Reducida (media de cajas) a ~640 px de ancho: pesa poco y se ve nitida
    // en la tarjeta.
    const int step = std::max(1, (x1 - x0) / 640);
    asset::ImageRgba8 image;
    image.width = static_cast<std::uint32_t>((x1 - x0) / step);
    image.height = static_cast<std::uint32_t>((y1 - y0) / step);
    if (image.width == 0 || image.height == 0) return;
    image.pixels.resize(static_cast<std::size_t>(image.width) * image.height * 4);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            std::uint32_t sum[3] = {0, 0, 0};
            for (int dy = 0; dy < step; ++dy) {
                for (int dx = 0; dx < step; ++dx) {
                    const std::size_t src =
                        (static_cast<std::size_t>(y0 + static_cast<int>(y) * step + dy) * width + x0 +
                         static_cast<int>(x) * step + dx) * 4;
                    for (int c = 0; c < 3; ++c) sum[c] += pixels[src + c];
                }
            }
            std::uint8_t* out = &image.pixels[(static_cast<std::size_t>(y) * image.width + x) * 4];
            for (int c = 0; c < 3; ++c) out[c] = static_cast<std::uint8_t>(sum[c] / static_cast<std::uint32_t>(step * step));
            out[3] = 255;
        }
    }
    std::error_code e;
    std::filesystem::create_directories(project_.libraryFolder(), e);
    asset::saveImagePng(project_.libraryFolder() / "thumbnail.png", image);
}

}  // namespace cramion::editor
