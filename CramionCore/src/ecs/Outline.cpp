#include "CramionCore/ecs/Outline.h"

#include <array>

namespace cramion::ecs {

void Outline::reflect(PropertyVisitor& v) {
    v.field({"enabled", "Activo"}, enabled);
    v.field({"color", "Color"}, color, Vec3Kind::Color);
    v.field({"opacity", "Opacidad"}, opacity, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
    v.field({"width", "Grosor", "En pixeles de pantalla"}, width, FloatRange{0.5f, 15.0f, 0.1f, "%.1f px", true});
    static constexpr std::array<const char*, 3> kModes = {"Siempre", "Solo tapado", "Solo visible"};
    enumField(v,
              {"mode", "Cuando",
               "Siempre: toda la silueta. Solo tapado: lo que algo tapa (a traves de paredes). Solo visible: lo que "
               "se ve"},
              mode, kModes);
    if (v.wantsAllFields() || mode != OutlineMode::Visible) {
        v.field({"occluded_fill", "Relleno tapado", "Opacidad del relleno de la parte tapada (0 = solo el borde)"},
                occluded_fill, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
    }
    v.field({"pulse_speed", "Pulso", "Ciclos por segundo (0 = fijo)"}, pulse_speed,
            FloatRange{0.0f, 10.0f, 0.05f, "%.2f Hz"});
    if (v.wantsAllFields() || pulse_speed > 0.0f) {
        v.field({"pulse_min", "Pulso: opacidad minima"}, pulse_min, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
    }
    v.field({"include_children", "Incluir hijos", "Tambien los Mesh Renderer de debajo (personajes por piezas)"},
            include_children);
}

}  // namespace cramion::ecs
