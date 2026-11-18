#ifndef CRAMION_VK_STROKE_FONT_H
#define CRAMION_VK_STROKE_FONT_H

// Letra de trazos (segmentos) para el texto de depuracion en el mundo
// (Debug.drawText): sin textura ni atlas, se dibuja con las mismas lineas
// de grosor constante que los gizmos (OverlayPass). Mayusculas, cifras y la
// puntuacion comun; las minusculas salen en mayusculas, las vocales con
// tilde y la enie sin la marca (a, e, n...) y lo que no conoce, como '?'.
//
// Cada letra va en una rejilla de 4 x 8 unidades (y hacia arriba) y avanza 6.

#include <cstddef>
#include <string_view>
#include <vector>

namespace cramion::gfx {

struct StrokeSegment {
    float x0, y0, x1, y1;  // en pixeles respecto al centro del texto (y hacia arriba)
};

// Los segmentos de `text` (UTF-8) con letras de `height` pixeles de alto,
// centrado en (0, 0). Varias lineas con '\n'. Hasta `max_chars` letras.
// Devuelve el numero de segmentos anadidos a `out`.
std::size_t strokeText(std::string_view text, float height, std::vector<StrokeSegment>& out,
                       std::size_t max_chars = 256);

// Los trazos de una letra en la rejilla (pares x y de 0..4 / 0..8, polilineas
// separadas por espacio). nullptr si no la conoce. Para las pruebas.
const char* strokeGlyph(char c);

}  // namespace cramion::gfx

#endif  // CRAMION_VK_STROKE_FONT_H
