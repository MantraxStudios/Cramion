#ifndef CRAMION_CORE_ECS_MATH_UTIL_H
#define CRAMION_CORE_ECS_MATH_UTIL_H

// Utilidades de transformaciones que CramionFX no trae: cuaterniones <->
// Euler (orden YXZ como Unity: primero Z, luego X, luego Y) y descomposicion
// de matrices. Las rotaciones se guardan en cuaternion (sin bloqueo de
// cardan); los grados solo son para la interfaz.

#include <CramionFX/core/Math.h>

namespace cramion::ecs {

core::Quat quatMultiply(const core::Quat& a, const core::Quat& b);
core::Quat quatConjugate(const core::Quat& q);
core::Vec3 quatRotate(const core::Quat& q, const core::Vec3& v);

// Grados, orden YXZ: R = Ry * Rx * Rz.
core::Quat quatFromEulerDegrees(const core::Vec3& degrees);
core::Vec3 quatToEulerDegrees(const core::Quat& q);

// Cuaternion de una matriz de rotacion pura (columnas unitarias).
core::Quat quatFromRotationMatrix(const core::Mat4& m);

// Posicion, rotacion y escala de una matriz afin (sin cizalla).
void decomposeMatrix(const core::Mat4& m, core::Vec3& position, core::Quat& rotation,
                     core::Vec3& scale);

core::Vec3 transformPoint(const core::Mat4& m, const core::Vec3& p);
core::Vec3 transformDirection(const core::Mat4& m, const core::Vec3& d);
float maxAxisScale(const core::Mat4& m);

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_ECS_MATH_UTIL_H
