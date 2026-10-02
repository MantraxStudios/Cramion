#ifndef CRAMION_CORE_TWOD_SPRITE2D_H
#define CRAMION_CORE_TWOD_SPRITE2D_H

// Sprites 2D (como los de Unity):
//
//   Hoja de sprites   una imagen de Assets/ importada como Sprite. Sus
//                     ajustes van en un archivo al lado, "<imagen>.crsprite"
//                     (JSON): modo Single (la imagen entera) o Multiple
//                     (cortes por rejilla o automaticos), pixeles por unidad,
//                     filtro (Point para pixel art) y el pivote de cada corte.
//   SpriteRenderer    dibuja un corte (`frame`) de la hoja: tinte, volteo,
//                     capa de orden (Sorting Layer) y orden en la capa,
//                     iluminado o no, modo Simple o Mosaico.
//   SpriteAnimator    clips de fotogramas ("0-5, 8") a N fps con bucle; el
//                     juego elige el clip (Lua: entity:playSpriteAnimation).
//   Light2D           luz 2D puntual (circulo) o global (la luz ambiente de
//                     los sprites iluminados).
//
// Las capas de orden del proyecto estan en ProjectSettings/SortingLayers.json
// (de atras adelante). El dibujo lo hace System2D (System2D.h).

#include "CramionCore/ecs/Reflection.h"

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cramion::twod {

inline constexpr const char* kSpriteExtension = ".crsprite";

enum class SpriteMode : int { Single = 0, Multiple = 1 };
enum class SpriteFilter : int { Point = 0, Bilinear = 1 };

// Un corte de la hoja, en pixeles (origen arriba a la izquierda, como en el
// editor de imagenes). El pivote va de 0..1 desde abajo a la izquierda.
struct SpriteFrame {
    std::string name;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    core::Vec2 pivot{0.5f, 0.5f};
};

struct SpriteSheet {
    SpriteMode mode = SpriteMode::Single;
    float pixels_per_unit = 100.0f;
    SpriteFilter filter = SpriteFilter::Point;
    core::Vec2 pivot{0.5f, 0.5f};  // Single
    std::vector<SpriteFrame> frames;  // Multiple

    // De la imagen (no se guardan).
    int width = 0;
    int height = 0;

    int frameCount() const;
    // El corte `index` (Single: la imagen entera; fuera de rango: el 0).
    SpriteFrame frame(int index) const;
    int findFrame(const std::string& name) const;  // -1 si no esta
};

std::filesystem::path spriteSettingsFile(const std::filesystem::path& image);
// Lee los ajustes (si no hay archivo: Single, 100 px/u, Point) y el tamano de
// la imagen. false si la imagen no se puede leer.
bool loadSpriteSheet(const std::filesystem::path& image, SpriteSheet& sheet);
bool saveSpriteSheet(const std::filesystem::path& image, const SpriteSheet& sheet, std::string* error = nullptr);
std::string spriteSheetToJson(const SpriteSheet& sheet);
bool spriteSheetFromJson(const std::string& text, SpriteSheet& sheet);

// --- Corte (Sprite Editor) ---
// Rejilla de celdas de w x h pixeles con margen y separacion. `rgba` (opcional,
// width*height*4) salta las celdas vacias (todo transparente).
std::vector<SpriteFrame> sliceGrid(int width, int height, int cell_w, int cell_h, int offset_x, int offset_y,
                                   int spacing_x, int spacing_y, const std::string& base_name,
                                   const std::vector<std::uint8_t>* rgba = nullptr, core::Vec2 pivot = {0.5f, 0.5f});
// Por columnas y filas (divide la imagen en cols x rows).
std::vector<SpriteFrame> sliceCount(int width, int height, int columns, int rows, const std::string& base_name,
                                    const std::vector<std::uint8_t>* rgba = nullptr, core::Vec2 pivot = {0.5f, 0.5f});
// Automatico: islas de pixeles opacos (alfa > `alpha_threshold`) unidas por
// vecindad; cada una es un corte (en orden de lectura: filas, izquierda a
// derecha). Las islas de menos de `min_size` pixeles de lado se ignoran.
std::vector<SpriteFrame> sliceAutomatic(const std::vector<std::uint8_t>& rgba, int width, int height,
                                        const std::string& base_name, int min_size = 2, int alpha_threshold = 8,
                                        core::Vec2 pivot = {0.5f, 0.5f});

// Lista de fotogramas "0-5, 8, 10-12" -> {0,1,2,3,4,5,8,10,11,12}.
std::vector<int> parseFrameList(const std::string& text);

// Hojas en uso, por ruta relativa a Assets (las lee una vez).
class SpriteLibrary {
public:
    void setRoot(const std::filesystem::path& assets_root);
    const std::filesystem::path& root() const { return root_; }
    std::filesystem::path absolute(const std::string& relative) const;
    // nullptr si la imagen no existe o no se puede leer.
    const SpriteSheet* get(const std::string& relative);
    void invalidate(const std::string& relative);
    void clear() { sheets_.clear(); }

private:
    std::filesystem::path root_;
    std::map<std::string, std::unique_ptr<SpriteSheet>> sheets_;
};

// --- Capas de orden (Sorting Layers) ---
std::vector<std::string> defaultSortingLayers();
// La lista del proyecto abierto (el editor y el juego la cargan al abrirlo).
const std::vector<std::string>& sortingLayers();
void setSortingLayers(std::vector<std::string> layers);
bool loadSortingLayers(const std::filesystem::path& file);
bool saveSortingLayers(const std::filesystem::path& file, const std::vector<std::string>& layers);
// Indice de la capa (0 = la del fondo); las desconocidas cuentan como "Default".
int sortingLayerIndex(const std::string& name);

// --- Componentes ---

enum class SpriteDrawMode : int { Simple = 0, Tiled = 1 };

struct SpriteRenderer {
    std::string sprite;   // imagen en Assets/ (ruta relativa)
    int frame = 0;        // corte de la hoja
    core::Vec3 color{1.0f, 1.0f, 1.0f};
    float opacity = 1.0f;
    bool flip_x = false;
    bool flip_y = false;
    std::string sorting_layer = "Default";
    int order_in_layer = 0;
    bool lit = false;            // le afectan las Light2D
    float alpha_cutoff = 0.0f;   // > 0: recorte duro (pixel art sin bordes semitransparentes)
    SpriteDrawMode draw_mode = SpriteDrawMode::Simple;
    core::Vec2 size{1.0f, 1.0f};  // Mosaico: tamano en unidades (repite el corte)

    void reflect(ecs::PropertyVisitor& v);
};

struct SpriteClip {
    std::string name = "Clip";
    std::string frames = "0";  // "0-5, 8"
    float fps = 12.0f;
    bool loop = true;
};

struct SpriteAnimatorState {
    std::string clip;      // el que suena
    float time = 0.0f;
    bool finished = false;
};

struct SpriteAnimator {
    std::vector<SpriteClip> clips;
    std::string default_clip;   // el que empieza (vacio = el primero)
    float speed = 1.0f;
    bool playing = true;
    bool preview_in_editor = false;  // animar tambien fuera de Play
    SpriteAnimatorState state;  // no se guarda

    const SpriteClip* findClip(const std::string& name) const;
    void play(const std::string& clip, bool restart = false);

    void reflect(ecs::PropertyVisitor& v);
};

enum class Light2DType : int { Point = 0, Global = 1 };

struct Light2D {
    Light2DType type = Light2DType::Point;
    core::Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float radius = 5.0f;    // puntual (unidades)
    float falloff = 1.5f;   // exponente: mas = borde mas suave

    void reflect(ecs::PropertyVisitor& v);
};

}  // namespace cramion::twod

#endif  // CRAMION_CORE_TWOD_SPRITE2D_H
