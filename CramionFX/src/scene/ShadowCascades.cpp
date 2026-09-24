#include "CramionFX/scene/ShadowCascades.h"

#include <algorithm>
#include <cmath>

namespace cramion::scene {

using core::Mat4;
using core::Vec3;
using core::Vec4;

namespace {

// Las 8 esquinas del cubo de recorte en coordenadas normalizadas de Vulkan:
// x,y en [-1, 1] y z en [0, 1].
constexpr std::array<Vec3, 8> kClipCorners = {{
    {-1.0f, -1.0f, 0.0f}, {1.0f, -1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {-1.0f, 1.0f, 0.0f},
    {-1.0f, -1.0f, 1.0f}, {1.0f, -1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {-1.0f, 1.0f, 1.0f},
}};

}  // namespace

void ShadowCascades::setShadowDistance(float distance) {
    shadow_distance_ = std::max(distance, 10.0f);
}

void ShadowCascades::setDistribution(float distribution) {
    distribution_ = std::clamp(distribution, 0.0f, 1.0f);
}

void ShadowCascades::update(const Camera& camera, const DirectionalLight& sun,
                            std::uint32_t shadow_map_size) {
    const float near_plane = camera.nearPlane();
    const float far_plane = std::min(camera.farPlane(), shadow_distance_);
    // El reparto logaritmico se calcula desde un near efectivo, no desde el de
    // la camara: con near = 0.1 las primeras cascadas serian de centimetros y
    // la ultima tendria que tragarse toda la escena.
    const float distribution_near = std::max(near_plane, far_plane * kDistributionNearFraction);
    const float range = far_plane - distribution_near;
    const float ratio = far_plane / distribution_near;

    const Vec3 light_direction = core::normalize(sun.direction);
    const Mat4 camera_view = camera.view();
    const auto map_size = static_cast<float>(shadow_map_size);

    float split_near = near_plane;

    for (std::uint32_t i = 0; i < kShadowCascadeCount; ++i) {
        // --- 1) Donde acaba esta cascada ---
        // Mezcla del reparto logaritmico (mas texeles cerca, que es donde se
        // nota) con el uniforme (evita que la primera cascada sea diminuta).
        const float fraction = static_cast<float>(i + 1) / static_cast<float>(kShadowCascadeCount);
        const float logarithmic = distribution_near * std::pow(ratio, fraction);
        const float uniform = distribution_near + range * fraction;
        const float split_far = distribution_ * logarithmic + (1.0f - distribution_) * uniform;

        // --- 2) Las 8 esquinas de esta seccion del frustum, en el mundo ---
        const Mat4 section_projection =
            core::perspective(camera.fovY(), camera.aspectRatio(), split_near, split_far);
        const Mat4 inverse_view_projection = core::inverse(section_projection * camera_view);

        std::array<Vec3, 8> corners{};
        for (std::size_t corner = 0; corner < kClipCorners.size(); ++corner) {
            const Vec4 world = inverse_view_projection *
                               Vec4{kClipCorners[corner].x, kClipCorners[corner].y,
                                    kClipCorners[corner].z, 1.0f};
            corners[corner] = Vec3{world.x, world.y, world.z} * (1.0f / world.w);
        }

        // --- 3) Esfera que envuelve la seccion ---
        // Usar una esfera en vez de una caja hace que el area cubierta no
        // dependa de hacia donde mire la camara: sin esto, las sombras hierven
        // al girar.
        Vec3 center{};
        for (const Vec3& corner : corners) {
            center += corner;
        }
        center *= 1.0f / static_cast<float>(corners.size());

        float radius = 0.0f;
        for (const Vec3& corner : corners) {
            radius = std::max(radius, core::length(corner - center));
        }
        // Se redondea hacia arriba para que pequenas variaciones numericas no
        // cambien la escala de la proyeccion cada frame.
        radius = std::ceil(radius * 16.0f) / 16.0f;

        // --- 4) Camara de la luz mirando al centro de la esfera ---
        // El "arriba" se elige para que nunca sea paralelo a la luz.
        const Vec3 up = (std::abs(light_direction.y) > 0.99f) ? Vec3{0.0f, 0.0f, 1.0f}
                                                              : Vec3{0.0f, 1.0f, 0.0f};

        const float depth_range = 2.0f * radius + caster_margin_;
        const Vec3 light_eye = center - light_direction * (radius + caster_margin_);

        const Mat4 light_view = core::lookAt(light_eye, center, up);
        Mat4 light_projection =
            core::orthographic(-radius, radius, -radius, radius, 0.0f, depth_range);

        Mat4 light_view_projection = light_projection * light_view;

        // --- 5) Ajuste a la rejilla de texeles ---
        // Sin esto, al desplazarse la camara la proyeccion se mueve fracciones
        // de texel y los bordes de sombra parpadean.
        const Vec4 origin = light_view_projection * Vec4{0.0f, 0.0f, 0.0f, 1.0f};
        const float texels_x = origin.x * map_size * 0.5f;
        const float texels_y = origin.y * map_size * 0.5f;

        const float offset_x = (std::round(texels_x) - texels_x) * 2.0f / map_size;
        const float offset_y = (std::round(texels_y) - texels_y) * 2.0f / map_size;

        light_view_projection.m[3][0] += offset_x;
        light_view_projection.m[3][1] += offset_y;

        cascades_[i].light_view_projection = light_view_projection;
        cascades_[i].split_distance = split_far;
        cascades_[i].texel_world_size = (2.0f * radius) / map_size;

        // La siguiente cascada arranca algo antes de donde acaba esta, para que
        // haya solape donde mezclar las dos y la transicion no se vea.
        split_near = split_far * 0.96f;
    }
}

}  // namespace cramion::scene
