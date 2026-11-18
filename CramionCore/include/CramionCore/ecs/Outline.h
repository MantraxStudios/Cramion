#ifndef CRAMION_CORE_ECS_OUTLINE_H
#define CRAMION_CORE_ECS_OUTLINE_H

// Contorno de juego (RM-capas): resalta un objeto con una linea de color
// alrededor de su silueta, en el juego exportado y en la vista Juego (el de
// seleccion del editor es otra cosa). Como el Custom Depth/Stencil de UE5: el
// renderizador dibuja una mascara propia con la silueta y la parte visible, asi
// que puede marcar solo lo tapado (silueta a traves de paredes de un MOBA, con
// relleno), solo lo visible o todo. Va en la entidad del Mesh Renderer o en
// una raiz (con `include_children`, todos los Mesh Renderer de debajo: un
// personaje de varias piezas).
//
// Lo usa RenderSync (gatherOutlines): cada frame junta los estilos (hasta 15
// distintos a la vez; los demas comparten el ultimo) y se los pasa al
// renderizador (VulkanRenderer::setGameOutlines).

#include "CramionCore/ecs/Reflection.h"

#include <CramionFX/core/Math.h>

namespace cramion::ecs {

// Siempre: toda la silueta. Tapado: solo lo que algo tapa (a traves de
// paredes). Visible: solo lo que se ve.
enum class OutlineMode : int { Always = 0, Occluded = 1, Visible = 2 };

struct Outline {
    bool enabled = true;
    core::Vec3 color{1.0f, 0.6f, 0.12f};  // sRGB
    float opacity = 1.0f;
    float width = 2.0f;  // pixeles de pantalla
    OutlineMode mode = OutlineMode::Always;
    // Relleno de la parte tapada (0 = solo el borde), en Siempre y Tapado.
    float occluded_fill = 0.0f;
    // Pulso: ciclos por segundo (0 = fijo) y opacidad minima del ciclo.
    float pulse_speed = 0.0f;
    float pulse_min = 0.35f;
    // Tambien los Mesh Renderer de los hijos.
    bool include_children = true;

    void reflect(PropertyVisitor& v);
};

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_ECS_OUTLINE_H
