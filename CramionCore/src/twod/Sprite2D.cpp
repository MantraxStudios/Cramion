#include "CramionCore/twod/Sprite2D.h"

#include <CramionFX/asset/ImageFile.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>

namespace cramion::twod {

using json = nlohmann::json;

// -----------------------------------------------------------------------------
// Hoja de sprites
// -----------------------------------------------------------------------------

int SpriteSheet::frameCount() const {
    return mode == SpriteMode::Multiple ? static_cast<int>(frames.size()) : 1;
}

SpriteFrame SpriteSheet::frame(int index) const {
    if (mode == SpriteMode::Multiple && !frames.empty()) {
        if (index < 0 || index >= static_cast<int>(frames.size())) index = 0;
        return frames[static_cast<std::size_t>(index)];
    }
    SpriteFrame whole;
    whole.name = "0";
    whole.w = width;
    whole.h = height;
    whole.pivot = pivot;
    return whole;
}

int SpriteSheet::findFrame(const std::string& name) const {
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

std::filesystem::path spriteSettingsFile(const std::filesystem::path& image) {
    std::filesystem::path file = image;
    file += kSpriteExtension;
    return file;
}

std::string spriteSheetToJson(const SpriteSheet& sheet) {
    json j;
    j["version"] = 1;
    j["mode"] = sheet.mode == SpriteMode::Multiple ? "multiple" : "single";
    j["pixels_per_unit"] = sheet.pixels_per_unit;
    j["filter"] = sheet.filter == SpriteFilter::Point ? "point" : "bilinear";
    j["pivot"] = {sheet.pivot.x, sheet.pivot.y};
    json frames = json::array();
    for (const SpriteFrame& f : sheet.frames) {
        frames.push_back(json{{"name", f.name}, {"rect", {f.x, f.y, f.w, f.h}}, {"pivot", {f.pivot.x, f.pivot.y}}});
    }
    j["frames"] = frames;
    return j.dump(2);
}

bool spriteSheetFromJson(const std::string& text, SpriteSheet& sheet) {
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    sheet.mode = j.value("mode", std::string("single")) == "multiple" ? SpriteMode::Multiple : SpriteMode::Single;
    sheet.pixels_per_unit = std::max(j.value("pixels_per_unit", 100.0f), 0.01f);
    sheet.filter = j.value("filter", std::string("point")) == "bilinear" ? SpriteFilter::Bilinear : SpriteFilter::Point;
    if (j.contains("pivot") && j["pivot"].is_array() && j["pivot"].size() == 2) {
        sheet.pivot = core::Vec2{j["pivot"][0].get<float>(), j["pivot"][1].get<float>()};
    }
    sheet.frames.clear();
    if (j.contains("frames") && j["frames"].is_array()) {
        for (const json& f : j["frames"]) {
            if (!f.is_object()) continue;
            SpriteFrame frame;
            frame.name = f.value("name", std::to_string(sheet.frames.size()));
            if (f.contains("rect") && f["rect"].is_array() && f["rect"].size() == 4) {
                frame.x = f["rect"][0].get<int>();
                frame.y = f["rect"][1].get<int>();
                frame.w = f["rect"][2].get<int>();
                frame.h = f["rect"][3].get<int>();
            }
            if (f.contains("pivot") && f["pivot"].is_array() && f["pivot"].size() == 2) {
                frame.pivot = core::Vec2{f["pivot"][0].get<float>(), f["pivot"][1].get<float>()};
            }
            sheet.frames.push_back(frame);
        }
    }
    return true;
}

bool loadSpriteSheet(const std::filesystem::path& image, SpriteSheet& sheet) {
    sheet = SpriteSheet{};
    std::ifstream in(spriteSettingsFile(image), std::ios::binary);
    if (in) {
        std::stringstream text;
        text << in.rdbuf();
        spriteSheetFromJson(text.str(), sheet);
    }
    asset::ImageRgba8 pixels;
    if (!asset::loadImageRgba8(image, pixels)) return false;
    sheet.width = static_cast<int>(pixels.width);
    sheet.height = static_cast<int>(pixels.height);
    return true;
}

bool saveSpriteSheet(const std::filesystem::path& image, const SpriteSheet& sheet, std::string* error) {
    std::ofstream out(spriteSettingsFile(image), std::ios::binary | std::ios::trunc);
    out << spriteSheetToJson(sheet);
    if (!out) {
        if (error != nullptr) *error = "no se pudo escribir " + spriteSettingsFile(image).string();
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// Corte
// -----------------------------------------------------------------------------

namespace {

bool regionEmpty(const std::vector<std::uint8_t>& rgba, int width, int x0, int y0, int w, int h) {
    for (int y = y0; y < y0 + h; ++y) {
        for (int x = x0; x < x0 + w; ++x) {
            if (rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4 + 3] > 0) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

std::vector<SpriteFrame> sliceGrid(int width, int height, int cell_w, int cell_h, int offset_x, int offset_y,
                                   int spacing_x, int spacing_y, const std::string& base_name,
                                   const std::vector<std::uint8_t>* rgba, core::Vec2 pivot) {
    std::vector<SpriteFrame> frames;
    if (cell_w <= 0 || cell_h <= 0 || width <= 0 || height <= 0) return frames;
    const bool check = rgba != nullptr && rgba->size() >= static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    for (int y = std::max(offset_y, 0); y + cell_h <= height; y += cell_h + std::max(spacing_y, 0)) {
        for (int x = std::max(offset_x, 0); x + cell_w <= width; x += cell_w + std::max(spacing_x, 0)) {
            if (check && regionEmpty(*rgba, width, x, y, cell_w, cell_h)) continue;
            SpriteFrame f;
            f.name = base_name + "_" + std::to_string(frames.size());
            f.x = x;
            f.y = y;
            f.w = cell_w;
            f.h = cell_h;
            f.pivot = pivot;
            frames.push_back(f);
        }
    }
    return frames;
}

std::vector<SpriteFrame> sliceCount(int width, int height, int columns, int rows, const std::string& base_name,
                                    const std::vector<std::uint8_t>* rgba, core::Vec2 pivot) {
    if (columns <= 0 || rows <= 0) return {};
    return sliceGrid(width, height, width / columns, height / rows, 0, 0, 0, 0, base_name, rgba, pivot);
}

std::vector<SpriteFrame> sliceAutomatic(const std::vector<std::uint8_t>& rgba, int width, int height,
                                        const std::string& base_name, int min_size, int alpha_threshold,
                                        core::Vec2 pivot) {
    std::vector<SpriteFrame> frames;
    if (width <= 0 || height <= 0 || rgba.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4) {
        return frames;
    }
    const auto opaque = [&](int x, int y) {
        return rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4 + 3] >
               alpha_threshold;
    };
    std::vector<std::uint8_t> seen(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
    std::vector<int> stack;
    struct Box {
        int x0, y0, x1, y1;
    };
    std::vector<Box> boxes;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
            if (seen[index] || !opaque(x, y)) continue;
            Box box{x, y, x, y};
            stack.clear();
            stack.push_back(static_cast<int>(index));
            seen[index] = 1;
            while (!stack.empty()) {
                const int i = stack.back();
                stack.pop_back();
                const int px = i % width;
                const int py = i / width;
                box.x0 = std::min(box.x0, px);
                box.y0 = std::min(box.y0, py);
                box.x1 = std::max(box.x1, px);
                box.y1 = std::max(box.y1, py);
                // 8 vecinos: las piezas que se tocan en diagonal son una.
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = px + dx;
                        const int ny = py + dy;
                        if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                        const std::size_t n = static_cast<std::size_t>(ny) * static_cast<std::size_t>(width) + static_cast<std::size_t>(nx);
                        if (seen[n] || !opaque(nx, ny)) continue;
                        seen[n] = 1;
                        stack.push_back(static_cast<int>(n));
                    }
                }
            }
            if (box.x1 - box.x0 + 1 >= min_size && box.y1 - box.y0 + 1 >= min_size) boxes.push_back(box);
        }
    }
    // Orden de lectura: por filas (las que se solapan en Y son la misma fila).
    std::sort(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) {
        if (a.y1 < b.y0) return true;
        if (b.y1 < a.y0) return false;
        return a.x0 < b.x0;
    });
    for (const Box& b : boxes) {
        SpriteFrame f;
        f.name = base_name + "_" + std::to_string(frames.size());
        f.x = b.x0;
        f.y = b.y0;
        f.w = b.x1 - b.x0 + 1;
        f.h = b.y1 - b.y0 + 1;
        f.pivot = pivot;
        frames.push_back(f);
    }
    return frames;
}

std::vector<int> parseFrameList(const std::string& text) {
    std::vector<int> out;
    std::string token;
    const auto flush = [&]() {
        std::string t;
        for (char c : token) {
            if (!std::isspace(static_cast<unsigned char>(c))) t.push_back(c);
        }
        token.clear();
        if (t.empty()) return;
        const std::size_t dash = t.find('-', 1);
        try {
            if (dash != std::string::npos) {
                const int a = std::stoi(t.substr(0, dash));
                const int b = std::stoi(t.substr(dash + 1));
                const int step = a <= b ? 1 : -1;
                for (int i = a;; i += step) {
                    out.push_back(i);
                    if (i == b || out.size() > 4096) break;
                }
            } else {
                out.push_back(std::stoi(t));
            }
        } catch (...) {
        }
    };
    for (char c : text) {
        if (c == ',' || c == ';') {
            flush();
        } else {
            token.push_back(c);
        }
    }
    flush();
    return out;
}

// -----------------------------------------------------------------------------
// Biblioteca
// -----------------------------------------------------------------------------

void SpriteLibrary::setRoot(const std::filesystem::path& assets_root) {
    if (assets_root != root_) sheets_.clear();
    root_ = assets_root;
}

std::filesystem::path SpriteLibrary::absolute(const std::string& relative) const {
    return root_ / std::filesystem::path(std::u8string(relative.begin(), relative.end()));
}

const SpriteSheet* SpriteLibrary::get(const std::string& relative) {
    if (relative.empty()) return nullptr;
    const auto it = sheets_.find(relative);
    if (it != sheets_.end()) return it->second.get();
    auto sheet = std::make_unique<SpriteSheet>();
    if (!loadSpriteSheet(absolute(relative), *sheet)) sheet.reset();
    const SpriteSheet* result = sheet.get();
    sheets_[relative] = std::move(sheet);
    return result;
}

void SpriteLibrary::invalidate(const std::string& relative) {
    if (relative.empty()) {
        sheets_.clear();
        return;
    }
    sheets_.erase(relative);
}

// -----------------------------------------------------------------------------
// Capas de orden
// -----------------------------------------------------------------------------

namespace {
std::vector<std::string>& layersStorage() {
    static std::vector<std::string> layers = defaultSortingLayers();
    return layers;
}
}  // namespace

std::vector<std::string> defaultSortingLayers() { return {"Fondo", "Default", "Primer plano"}; }

const std::vector<std::string>& sortingLayers() { return layersStorage(); }

void setSortingLayers(std::vector<std::string> layers) {
    if (std::find(layers.begin(), layers.end(), "Default") == layers.end()) layers.insert(layers.begin(), "Default");
    layersStorage() = std::move(layers);
}

bool loadSortingLayers(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        setSortingLayers(defaultSortingLayers());
        return false;
    }
    const json j = json::parse(in, nullptr, false);
    std::vector<std::string> layers;
    if (!j.is_discarded() && j.contains("layers") && j["layers"].is_array()) {
        for (const json& l : j["layers"]) {
            if (l.is_string()) layers.push_back(l.get<std::string>());
        }
    }
    if (layers.empty()) layers = defaultSortingLayers();
    setSortingLayers(std::move(layers));
    return true;
}

bool saveSortingLayers(const std::filesystem::path& file, const std::vector<std::string>& layers) {
    json j;
    j["layers"] = layers;
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << j.dump(2);
    return static_cast<bool>(out);
}

int sortingLayerIndex(const std::string& name) {
    const std::vector<std::string>& layers = sortingLayers();
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (layers[i] == name) return static_cast<int>(i);
    }
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (layers[i] == "Default") return static_cast<int>(i);
    }
    return 0;
}

// -----------------------------------------------------------------------------
// Componentes
// -----------------------------------------------------------------------------

void SpriteRenderer::reflect(ecs::PropertyVisitor& v) {
    v.field({"sprite", "Sprite", "Imagen de Assets (ruta relativa). Arrastrala desde el Proyecto"}, sprite);
    v.field({"frame", "Corte", "Indice del corte de la hoja (modo Multiple, ver el Sprite Editor)"}, frame, 0, 4095);
    v.field({"color", "Color", "Tinte (multiplica la imagen)"}, color, ecs::Vec3Kind::Color);
    v.field({"opacity", "Opacidad"}, opacity, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
    v.field({"flip_x", "Voltear X"}, flip_x);
    v.field({"flip_y", "Voltear Y"}, flip_y);
    v.field({"sorting_layer", "Capa de orden", "Sorting Layer: las de atras se dibujan primero"}, sorting_layer);
    v.field({"order_in_layer", "Orden en la capa", "Dentro de la capa: mas = delante"}, order_in_layer, -32768, 32767);
    v.field({"lit", "Iluminado", "Le afectan las Light2D (si no, se ve tal cual)"}, lit);
    v.field({"alpha_cutoff", "Corte alfa", "0 = mezcla suave; > 0 descarta los pixeles con menos alfa"}, alpha_cutoff,
            ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
    static constexpr std::array<const char*, 2> kModes = {"Simple", "Mosaico"};
    ecs::enumField(v, {"draw_mode", "Modo de dibujo", "Mosaico: repite el corte hasta llenar el tamano"}, draw_mode, kModes);
    if (v.wantsAllFields() || draw_mode == SpriteDrawMode::Tiled) {
        v.field({"size", "Tamano", "Mosaico: ancho y alto en unidades"}, size, 0.05f);
    }
}

const SpriteClip* SpriteAnimator::findClip(const std::string& name) const {
    for (const SpriteClip& c : clips) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

void SpriteAnimator::play(const std::string& clip, bool restart) {
    if (!restart && state.clip == clip && !state.finished) {
        playing = true;
        return;
    }
    state.clip = clip;
    state.time = 0.0f;
    state.finished = false;
    playing = true;
}

void SpriteAnimator::reflect(ecs::PropertyVisitor& v) {
    ecs::listField(v, {"clips", "Clips", "Animaciones por fotogramas de la hoja del Sprite Renderer"}, clips,
                   [](SpriteClip& c, ecs::PropertyVisitor& item) {
                       item.field({"name", "Nombre"}, c.name);
                       item.field({"frames", "Fotogramas", "Cortes de la hoja: \"0-5, 8\""}, c.frames);
                       item.field({"fps", "Fotogramas por segundo"}, c.fps, ecs::FloatRange{0.1f, 120.0f, 0.1f, "%.1f"});
                       item.field({"loop", "Bucle"}, c.loop);
                   });
    v.field({"default_clip", "Clip inicial", "El que suena al empezar (vacio = el primero)"}, default_clip);
    v.field({"speed", "Velocidad"}, speed, ecs::FloatRange{0.0f, 10.0f, 0.01f, "%.2f"});
    v.field({"playing", "Reproduciendo"}, playing);
    v.field({"preview_in_editor", "Vista previa en el editor", "Animar tambien fuera de Play"}, preview_in_editor);
}

void Light2D::reflect(ecs::PropertyVisitor& v) {
    static constexpr std::array<const char*, 2> kTypes = {"Puntual", "Global"};
    ecs::enumField(v, {"type", "Tipo", "Global: la luz ambiente de todos los sprites iluminados"}, type, kTypes);
    v.field({"color", "Color"}, color, ecs::Vec3Kind::Color);
    v.field({"intensity", "Intensidad"}, intensity, ecs::FloatRange{0.0f, 20.0f, 0.01f, "%.2f"});
    if (v.wantsAllFields() || type == Light2DType::Point) {
        v.field({"radius", "Radio", "Unidades"}, radius, ecs::FloatRange{0.01f, 1000.0f, 0.05f, "%.2f"});
        v.field({"falloff", "Caida", "Exponente: mas = borde mas suave"}, falloff, ecs::FloatRange{0.05f, 8.0f, 0.01f, "%.2f"});
    }
}

}  // namespace cramion::twod
