#ifndef CRAMION_ANIM_ANIMATOR_H
#define CRAMION_ANIM_ANIMATOR_H

#include "CramionFX/asset/Model.h"
#include "CramionFX/core/Math.h"

#include <cstdint>
#include <vector>

namespace cramion::anim {

// Caja alineada con los ejes.
struct Aabb {
    core::Vec3 min{};
    core::Vec3 max{};
};

// Reproduce un clip de un modelo y calcula la matriz final de cada hueso.
//
// La pose se evalua en tres pasos:
//
//   1. Local:  cada nodo animado interpola sus claves (lerp para posicion y
//              escala, slerp para la rotacion). Los que no tienen pista
//              conservan su transformacion de reposo.
//   2. Global: se recorre la jerarquia de padres a hijos acumulando
//              global = global(padre) * local.
//   3. Hueso:  global(nodo del hueso) * offset. El offset lleva el vertice
//              al espacio del hueso en reposo; la global lo devuelve al
//              modelo ya en la pose animada.
//
// No hay tope de huesos: las matrices van en un std::vector del tamano del
// esqueleto y en la GPU en un storage buffer de longitud variable.
class Animator {
public:
    Animator() = default;
    explicit Animator(const asset::ModelData& model);

    // Clip a reproducir; -1 = pose de reposo.
    void play(std::int32_t clip, bool loop = true);

    // Avanza el tiempo y recalcula las matrices de los huesos.
    void update(float delta_seconds);

    // Recalcula la pose en el instante actual sin avanzar el tiempo.
    void evaluate();

    // Una matriz por hueso de asset::ModelData::bones, en espacio del modelo.
    const std::vector<core::Mat4>& boneMatrices() const { return bone_matrices_; }

    float time() const { return time_; }
    void setTime(float seconds);
    void setSpeed(float speed) { speed_ = speed; }

    std::int32_t clip() const { return clip_; }

private:
    const asset::ModelData* model_ = nullptr;

    std::int32_t clip_ = -1;
    bool loop_ = true;
    float time_ = 0.0f;
    float speed_ = 1.0f;

    // Pista del clip actual para cada nodo (-1 = sin animar).
    std::vector<std::int32_t> node_channel_;

    std::vector<core::Mat4> local_;
    std::vector<core::Mat4> global_;
    std::vector<core::Mat4> bone_matrices_;
};

// Deforma todos los vertices en la CPU con la pose dada y devuelve su caja.
// Se usa una vez al cargar, para colocar y escalar el modelo en el mundo.
Aabb skinnedBounds(const asset::ModelData& model, const std::vector<core::Mat4>& bones);

}  // namespace cramion::anim

#endif  // CRAMION_ANIM_ANIMATOR_H
