#ifndef CRAMION_CORE_ANIM_IK_H
#define CRAMION_CORE_ANIM_IK_H

// Cinematica inversa sobre la pose de un esqueleto (en el espacio del
// modelo, despues de la animacion y antes de deformar la malla):
//
//   - twoBone: brazo o pierna (3 huesos: hombro/cadera, codo/rodilla, mano/pie)
//     que llega a un punto; el codo o la rodilla apunta hacia el "pole"
//     (o se dobla hacia donde ya se doblaba). Analitico (ley del coseno):
//     exacto, sin iteraciones.
//   - lookAt: gira un hueso para que su "delante" mire a un punto, con angulo
//     maximo (la cabeza no da la vuelta).
//
// `local` y `global` son las matrices de todos los nodos (padres antes que
// hijos); las funciones cambian las locales que tocan y rehacen las globales.

#include <CramionFX/asset/Model.h>
#include <CramionFX/core/Math.h>

#include <vector>

namespace cramion::ik {

struct Pose {
    const std::vector<asset::Node>* nodes = nullptr;
    std::vector<core::Mat4>* local = nullptr;
    std::vector<core::Mat4>* global = nullptr;
};

// Globales desde `from` (y los que siguen) a partir de las locales.
void recomputeGlobals(const Pose& pose, int from = 0);
// Gira el nodo `node` con `delta` (espacio del modelo) alrededor de su
// pivote; sus hijos lo siguen.
void rotateGlobal(const Pose& pose, int node, const core::Quat& delta);
// Mueve el nodo `offset` (espacio del modelo); sus hijos lo siguen.
void translateGlobal(const Pose& pose, int node, const core::Vec3& offset);
// Pone el giro del nodo en el modelo (sus hijos lo siguen).
void setGlobalRotation(const Pose& pose, int node, const core::Quat& rotation);

core::Vec3 nodePosition(const Pose& pose, int node);
core::Quat nodeRotation(const Pose& pose, int node);

// `weight` 0..1 mezcla entre la pose animada y la resuelta. `pole` opcional.
// false si los huesos no forman una cadena valida.
bool twoBone(const Pose& pose, int upper, int mid, int end, const core::Vec3& target, const core::Vec3* pole,
             float weight);

// Cadena de varios huesos (patas de animal de 3 segmentos, cuellos, colas,
// tentaculos): `joints` va del de arriba al extremo, cada uno hijo del
// anterior. FABRIK (Aristidou 2011): mueve las articulaciones hacia el
// objetivo sin cambiar los largos y luego gira cada hueso hacia la nueva.
// Con `pole`, las articulaciones de en medio se doblan hacia el. Con 3
// articulaciones es lo mismo que twoBone. false si no es una cadena valida.
bool chain(const Pose& pose, const std::vector<int>& joints, const core::Vec3& target, const core::Vec3* pole,
           float weight, int iterations = 12);

// La cadena de `bones` huesos que acaba en `end`: sus `bones` antepasados
// mas el (de arriba abajo). Vacia si no hay tantos antepasados.
std::vector<int> chainTo(const std::vector<asset::Node>& nodes, int end, int bones);

// Giro (mundo) que inclina un cuerpo con el suelo bajo sus patas: `feet`
// son los puntos de apoyo (mundo) y `heights` cuanto sube (+) o baja (-) el
// suelo en cada uno respecto a donde lo pone la animacion. Recta de minimos
// cuadrados de la altura a lo largo de delante y de la derecha: si el suelo
// baja por delante, el morro baja; si sube por la derecha, ese lado sube.
// `forward`: delante del cuerpo (mundo). Como mucho `max_degrees` por eje.
core::Quat groundTilt(const std::vector<core::Vec3>& feet, const std::vector<float>& heights, const core::Vec3& forward,
                      float max_degrees = 30.0f);

// Mirar con varios huesos (cuello y cabeza): el giro total se calcula una
// vez desde la cabeza (`bones.back()`), se limita a `max_angle_degrees` y se
// reparte entre los huesos (de arriba a la cabeza). `forward_of` da hacia
// donde mira ahora cada hueso (modelo). Nunca gira mas del maximo en total.
void lookChain(const Pose& pose, const std::vector<int>& bones, const core::Vec3& head_forward, const core::Vec3& target,
               float weight, float max_angle_degrees);

// `forward`: hacia donde mira ahora el hueso (espacio del modelo, unitario).
void lookAt(const Pose& pose, int bone, const core::Vec3& forward, const core::Vec3& target, float weight,
            float max_angle_degrees);

// Giro minimo de la direccion `a` a la `b` (unitarias).
core::Quat rotationBetween(const core::Vec3& a, const core::Vec3& b);

}  // namespace cramion::ik

#endif  // CRAMION_CORE_ANIM_IK_H
