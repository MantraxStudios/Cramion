#ifndef CRAMION_CORE_FRUSTUM_H
#define CRAMION_CORE_FRUSTUM_H

#include "core/Math.h"

#include <cmath>

namespace cramion::core {

// Caja alineada con los ejes.
struct Aabb {
    Vec3 min{0.0f, 0.0f, 0.0f};
    Vec3 max{0.0f, 0.0f, 0.0f};
};

// Caja envolvente de `box` tras aplicarle `transform` (rotacion, escala y
// traslacion): centro transformado y semiejes proyectados con |M|.
inline Aabb transformAabb(const Mat4& transform, const Aabb& box) {
    const Vec3 center = (box.min + box.max) * 0.5f;
    const Vec3 extent = (box.max - box.min) * 0.5f;

    Vec3 world_center{transform.m[3][0], transform.m[3][1], transform.m[3][2]};
    Vec3 world_extent{0.0f, 0.0f, 0.0f};
    const float c[3] = {center.x, center.y, center.z};
    const float e[3] = {extent.x, extent.y, extent.z};
    for (int col = 0; col < 3; ++col) {
        world_center.x += transform.m[col][0] * c[col];
        world_center.y += transform.m[col][1] * c[col];
        world_center.z += transform.m[col][2] * c[col];
        world_extent.x += std::abs(transform.m[col][0]) * e[col];
        world_extent.y += std::abs(transform.m[col][1]) * e[col];
        world_extent.z += std::abs(transform.m[col][2]) * e[col];
    }
    return Aabb{world_center - world_extent, world_center + world_extent};
}

// Volumen de vision (camara, cascada de sombra o luz local) como planos que
// apuntan hacia dentro, extraidos de su matriz view-projection (metodo de
// Gribb y Hartmann, con la profundidad de Vulkan en [0, 1]).
class Frustum {
public:
    Frustum() = default;

    // `ignore_near`: sin plano cercano. Para las cascadas de sombra, que
    // dibujan con depth clamp: lo que queda entre el sol y la cascada tambien
    // proyecta sombra dentro de ella.
    explicit Frustum(const Mat4& view_projection, bool ignore_near = false) {
        const auto row = [&](int r) {
            return Vec4{view_projection.m[0][r], view_projection.m[1][r], view_projection.m[2][r],
                        view_projection.m[3][r]};
        };
        const Vec4 r0 = row(0);
        const Vec4 r1 = row(1);
        const Vec4 r2 = row(2);
        const Vec4 r3 = row(3);

        const auto plus = [](const Vec4& a, const Vec4& b) {
            return Vec4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
        };
        const auto minus = [](const Vec4& a, const Vec4& b) {
            return Vec4{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
        };

        add(plus(r3, r0));   // izquierda
        add(minus(r3, r0));  // derecha
        add(plus(r3, r1));   // abajo
        add(minus(r3, r1));  // arriba
        add(minus(r3, r2));  // lejos
        if (!ignore_near) {
            add(r2);   // cerca (z >= 0)
        }
    }

    // false solo si la caja queda entera fuera de algun plano (conservador:
    // algunas cajas en las esquinas pasan sin tocar el volumen, nunca al
    // reves).
    bool intersects(const Aabb& box) const {
        for (int i = 0; i < count_; ++i) {
            const Vec4& p = planes_[i];
            // Vertice de la caja mas adentro en la direccion del plano.
            const Vec3 farthest{p.x >= 0.0f ? box.max.x : box.min.x,
                                p.y >= 0.0f ? box.max.y : box.min.y,
                                p.z >= 0.0f ? box.max.z : box.min.z};
            if (p.x * farthest.x + p.y * farthest.y + p.z * farthest.z + p.w < 0.0f) {
                return false;
            }
        }
        return true;
    }

private:
    void add(const Vec4& plane) {
        const float length = std::sqrt(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
        const float inverse = (length > 0.0f) ? 1.0f / length : 0.0f;
        planes_[count_++] = Vec4{plane.x * inverse, plane.y * inverse, plane.z * inverse,
                                 plane.w * inverse};
    }

    Vec4 planes_[6]{};
    int count_ = 0;
};

}  // namespace cramion::core

#endif  // CRAMION_CORE_FRUSTUM_H
