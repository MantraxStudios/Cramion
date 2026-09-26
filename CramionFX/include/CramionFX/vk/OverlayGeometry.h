#ifndef CRAMION_VK_OVERLAY_GEOMETRY_H
#define CRAMION_VK_OVERLAY_GEOMETRY_H

// CONTRATO COMPARTIDO (renderizador <-> editor): geometria de ayuda en 3D
// (gizmos de mover/rotar/escalar, alcance de luces, cajas de decals...) que
// el renderizador dibuja CON prueba de profundidad contra la escena, como
// Unity y Unreal: opaca donde se ve y en "rayos X" (atenuada) donde queda
// dentro o detras de un objeto. Asi se ve donde atraviesa el objeto.
//
// Se dibuja sobre la imagen final de la escena (la que muestra el editor),
// despues del contorno de seleccion. Coste cero si esta vacia.

#include "CramionFX/core/Math.h"

#include <cstdint>
#include <vector>

namespace cramion::gfx {

struct OverlayVertex {
    core::Vec3 position{};           // en el mundo
    std::uint32_t color = 0xFFFFFFFF;  // RGBA8 (IM_COL32: r | g<<8 | b<<16 | a<<24)
};

struct OverlayGeometry {
    // Segmentos: cada par de vertices es una linea, con grosor en pixeles
    // constante (se expanden en pantalla).
    std::vector<OverlayVertex> lines;
    // Triangulos rellenos (puntas de flecha, cubos de escala, planos).
    std::vector<OverlayVertex> triangles;
    // Lo mismo pero SIN prueba de profundidad, siempre encima y opaco: el
    // gizmo de mover/rotar/escalar (como Unity y Unreal), que casi siempre
    // queda dentro del objeto seleccionado. Se dibuja despues de lo anterior.
    std::vector<OverlayVertex> top_lines;
    std::vector<OverlayVertex> top_triangles;
    float line_width = 2.5f;        // pixeles
    float top_line_width = 5.0f;    // pixeles de las lineas "top" (gizmo, gruesas como Unreal)
    float occluded_alpha = 0.25f;   // opacidad de lo que queda tapado (0 = oculto)

    bool empty() const {
        return lines.empty() && triangles.empty() && top_lines.empty() && top_triangles.empty();
    }
    void clear() {
        lines.clear();
        triangles.clear();
        top_lines.clear();
        top_triangles.clear();
    }
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_OVERLAY_GEOMETRY_H
