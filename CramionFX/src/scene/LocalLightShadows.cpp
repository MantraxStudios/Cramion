#include "CramionFX/scene/LocalLightShadows.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cramion::scene {

using core::Mat4;
using core::Vec3;

namespace {

// "Arriba" de la camara de la luz: cualquiera que no sea paralelo a la
// direccion en la que mira.
Vec3 upFor(const Vec3& direction) {
    return (std::abs(direction.y) > 0.99f) ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};
}

}  // namespace

bool LocalLightShadows::Signature::operator==(const Signature& other) const {
    return valid == other.valid && position.x == other.position.x &&
           position.y == other.position.y && position.z == other.position.z &&
           direction.x == other.direction.x && direction.y == other.direction.y &&
           direction.z == other.direction.z && range == other.range && angle == other.angle;
}

Vec3 LocalLightShadows::faceDirection(std::uint32_t face) {
    // Mismo orden que el shader al elegir cara por el eje dominante.
    static constexpr std::array<Vec3, kPointShadowFaceCount> kDirections = {{
        {1.0f, 0.0f, 0.0f},
        {-1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, -1.0f},
    }};
    return kDirections[face];
}

void LocalLightShadows::invalidate() {
    spot_rendered_.fill(Signature{});
    point_rendered_.fill(Signature{});
}

std::int32_t LocalLightShadows::spotSlot(std::size_t spot_index) const {
    return (spot_index < spot_slots_.size()) ? spot_slots_[spot_index] : -1;
}

std::int32_t LocalLightShadows::pointSlot(std::size_t point_index) const {
    return (point_index < point_slots_.size()) ? point_slots_[point_index] : -1;
}

void LocalLightShadows::update(const Camera& camera, const LightSet& lights,
                               std::uint32_t spot_map_size, std::uint32_t point_map_size) {
    updateSpots(lights, spot_map_size);
    updatePoints(camera, lights, point_map_size);
}

void LocalLightShadows::updateSpots(const LightSet& lights, std::uint32_t map_size) {
    spot_slots_.assign(lights.spots.size(), -1);

    // Los huecos siguen el mismo orden que los focos que se suben a la GPU:
    // los encendidos, en orden, hasta el tope.
    std::uint32_t slot = 0;
    for (std::size_t i = 0; i < lights.spots.size() && slot < kMaxShadowedSpotLights; ++i) {
        const SpotLight& light = lights.spots[i];
        if (!light.enabled) {
            continue;
        }

        const Vec3 direction = core::normalize(light.direction);

        // El frustum abarca el cono exterior con un par de grados de margen
        // para el PCF del borde.
        const float half_angle = std::min(light.outer_angle + core::radians(2.0f),
                                          core::radians(80.0f));
        const float tan_half = std::tan(half_angle);

        const Mat4 view = core::lookAt(light.position, light.position + direction, upFor(direction));
        const Mat4 projection = core::perspective(2.0f * half_angle, 1.0f, kNearPlane,
                                                  std::max(light.range, kNearPlane * 2.0f));

        SpotShadow& shadow = spots_[slot];
        shadow.light_view_projection = projection * view;
        shadow.position = light.position;
        shadow.range = light.range;
        shadow.texel_scale = 2.0f * tan_half / static_cast<float>(map_size);
        shadow.active = true;

        const Signature signature{light.position, direction, light.range, half_angle, true};
        shadow.dirty = !(signature == spot_rendered_[slot]);
        spot_rendered_[slot] = signature;

        spot_slots_[i] = static_cast<std::int32_t>(slot);
        ++slot;
    }

    for (; slot < kMaxShadowedSpotLights; ++slot) {
        spots_[slot].active = false;
        spots_[slot].dirty = false;
    }
}

void LocalLightShadows::updatePoints(const Camera& camera, const LightSet& lights,
                                     std::uint32_t map_size) {
    point_slots_.assign(lights.points.size(), -1);

    // --- 1) Candidatas, de mas a menos relevante ---
    // Se ordena por la distancia de la camara a la esfera de influencia: una
    // luz que envuelve a la camara importa aunque su centro este lejos.
    struct Candidate {
        std::size_t index = 0;
        float score = 0.0f;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(lights.points.size());

    for (std::size_t i = 0; i < lights.points.size(); ++i) {
        const PointLight& light = lights.points[i];
        if (light.range <= 0.0f || light.intensity <= 0.0f) {
            continue;
        }
        const float distance = core::length(light.position - camera.position());
        candidates.push_back(Candidate{i, std::max(distance - light.range, 0.0f) + distance * 0.01f});
    }

    const std::size_t selected_count =
        std::min<std::size_t>(candidates.size(), kMaxShadowedPointLights);
    std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(selected_count),
                      candidates.end(),
                      [](const Candidate& a, const Candidate& b) { return a.score < b.score; });

    // Puntuacion de la mejor luz que se queda SIN hueco: las elegidas cuya
    // puntuacion se le acerca van apagando su sombra, y cuando dos se cruzan
    // las dos estan ya a 0, sin salto visible.
    float cutoff_score = std::numeric_limits<float>::max();
    for (std::size_t i = selected_count; i < candidates.size(); ++i) {
        cutoff_score = std::min(cutoff_score, candidates[i].score);
    }
    std::vector<float> score_by_light(lights.points.size(), 0.0f);
    for (std::size_t i = 0; i < selected_count; ++i) {
        score_by_light[candidates[i].index] = candidates[i].score;
    }
    candidates.resize(selected_count);

    // --- 2) Las que ya tenian hueco lo conservan ---
    std::array<bool, kMaxShadowedPointLights> taken{};
    std::vector<std::size_t> pending;

    for (const Candidate& candidate : candidates) {
        bool kept = false;
        for (std::uint32_t slot = 0; slot < kMaxShadowedPointLights; ++slot) {
            if (!taken[slot] &&
                points_[slot].light_index == static_cast<std::int32_t>(candidate.index)) {
                taken[slot] = true;
                point_slots_[candidate.index] = static_cast<std::int32_t>(slot);
                kept = true;
                break;
            }
        }
        if (!kept) {
            pending.push_back(candidate.index);
        }
    }

    // --- 3) Las nuevas ocupan los huecos libres; los sobrantes se vacian ---
    std::size_t next_pending = 0;
    for (std::uint32_t slot = 0; slot < kMaxShadowedPointLights; ++slot) {
        if (taken[slot]) {
            continue;
        }
        if (next_pending < pending.size()) {
            const std::size_t index = pending[next_pending++];
            points_[slot].light_index = static_cast<std::int32_t>(index);
            point_slots_[index] = static_cast<std::int32_t>(slot);
            taken[slot] = true;
        } else {
            points_[slot].light_index = -1;
            points_[slot].dirty = false;
        }
    }

    // --- 4) Matrices de las seis caras ---
    // Con tan(fov/2) = t, el borde de los 90 grados cae en 1/t del mapa; se
    // elige t para que queden kPointGuardTexels texeles libres hasta el borde.
    const auto size = static_cast<float>(map_size);
    const float tan_half = 1.0f / (1.0f - 2.0f * kPointGuardTexels / size);
    const float fov = 2.0f * std::atan(tan_half);

    for (std::uint32_t slot = 0; slot < kMaxShadowedPointLights; ++slot) {
        PointShadow& shadow = points_[slot];
        if (!shadow.active()) {
            continue;
        }

        const PointLight& light = lights.points[static_cast<std::size_t>(shadow.light_index)];
        const Mat4 projection =
            core::perspective(fov, 1.0f, kNearPlane, std::max(light.range, kNearPlane * 2.0f));

        for (std::uint32_t face = 0; face < kPointShadowFaceCount; ++face) {
            const Vec3 direction = faceDirection(face);
            const Mat4 view =
                core::lookAt(light.position, light.position + direction, upFor(direction));
            shadow.face_view_projection[face] = projection * view;
        }

        shadow.position = light.position;
        shadow.range = light.range;
        shadow.texel_scale = 2.0f * tan_half / size;

        const float score = score_by_light[static_cast<std::size_t>(shadow.light_index)];
        shadow.fade = std::clamp((cutoff_score - score) / kPointShadowFadeBand, 0.0f, 1.0f);

        // La intensidad no entra en la firma: el parpadeo de una luz no
        // cambia la sombra.
        const Signature signature{light.position, Vec3{}, light.range, fov, true};
        shadow.dirty = !(signature == point_rendered_[slot]);
        point_rendered_[slot] = signature;
    }
}

}  // namespace cramion::scene
