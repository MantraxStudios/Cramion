#ifndef CRAMION_CORE_ANIM_INERTIALIZATION_H
#define CRAMION_CORE_ANIM_INERTIALIZATION_H

// Inercializacion (como el nodo Inertialization de Unreal y las transiciones
// de Gears of War / For Honor): al cambiar de animacion NO se mezclan dos
// poses durante el fundido. Se pasa en seco a la pose nueva y se le suma la
// diferencia con la pose que se estaba viendo; esa diferencia se apaga como
// un muelle criticamente amortiguado que conserva la velocidad que llevaba
// cada hueso. Resultado: el cuerpo sigue su impulso y entra en la animacion
// nueva sin el "flotar" del fundido cruzado ni pies que se deslizan entre
// dos ciclos, y una transicion que interrumpe a otra parte de lo que se ve.
//
// Por nodo: desfase de posicion (vector) y de giro (eje * angulo), cada uno
// con su velocidad. Tambien guarda la pose que salio los dos ultimos frames
// para saber la velocidad de cada hueso en el momento del cambio.

#include <CramionFX/core/Math.h>

#include <vector>

namespace cramion::anim {

class Inertializer {
public:
    // Pose de este frame (locales de cada nodo) SIN el desfase: la aplica
    // sobre `locals` y recuerda la salida.
    void apply(std::vector<core::Mat4>& locals, float delta_seconds);

    // Empieza una transicion: `fresh` es la pose nueva en este instante y
    // `fresh_before` la misma animacion `delta_seconds` antes (su velocidad);
    // `duration` = segundos hasta que casi no queda desfase. Sin pose previa
    // (primer frame) no hace nada.
    void start(const std::vector<core::Mat4>& fresh, const std::vector<core::Mat4>& fresh_before,
               float duration, float delta_seconds);

    bool active() const { return active_; }
    bool hasHistory() const { return frames_ > 0; }
    void reset() {
        active_ = false;
        frames_ = 0;
    }

private:
    struct Node {
        core::Vec3 position_offset{};
        core::Vec3 position_velocity{};
        core::Vec3 rotation_offset{};  // eje * angulo (radianes)
        core::Vec3 rotation_velocity{};
    };
    std::vector<Node> nodes_;
    // Salida de los dos ultimos frames (posicion y giro de cada local).
    std::vector<core::Vec3> last_position_, previous_position_;
    std::vector<core::Quat> last_rotation_, previous_rotation_;
    float last_delta_ = 1.0f / 60.0f;
    int frames_ = 0;
    bool active_ = false;
    float halflife_ = 0.05f;
    float elapsed_ = 0.0f;
    float duration_ = 0.2f;
};

// Muelle criticamente amortiguado que lleva `x` a 0 con su velocidad `v`
// (exacto para cualquier paso de tiempo). `halflife` = segundos en que cae a
// la mitad sin velocidad inicial.
void decaySpring(core::Vec3& x, core::Vec3& v, float halflife, float delta_seconds);

// Eje * angulo de un giro (camino corto) y al reves.
core::Vec3 quatToScaledAxis(const core::Quat& q);
core::Quat quatFromScaledAxis(const core::Vec3& v);

}  // namespace cramion::anim

#endif  // CRAMION_CORE_ANIM_INERTIALIZATION_H
