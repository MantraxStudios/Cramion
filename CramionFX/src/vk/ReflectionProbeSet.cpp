#include "CramionFX/vk/ReflectionProbeSet.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cramion::gfx {

std::size_t bakedCubeMipOffset(std::uint32_t size, std::uint32_t mip) {
    std::size_t offset = 0;
    for (std::uint32_t m = 0; m < mip; ++m) {
        const std::size_t side = std::max(size >> m, 1u);
        offset += 6u * side * side * 4u;
    }
    return offset;
}

std::size_t bakedCubeTexelCount(std::uint32_t size, std::uint32_t mips) {
    return bakedCubeMipOffset(size, mips);
}

bool bakedCubeValid(const BakedReflectionCube& cube) {
    return cube.size > 0 && cube.mips > 0 && cube.mips <= 16 && (cube.size >> (cube.mips - 1)) > 0 &&
           cube.texels.size() == bakedCubeTexelCount(cube.size, cube.mips);
}

float placedProbeWeight(const PlacedReflectionProbe& probe, const core::Vec3& p) {
    // Distancia hacia dentro a la cara mas cercana (negativa fuera).
    const float inside = std::min({p.x - probe.box_min.x, probe.box_max.x - p.x, p.y - probe.box_min.y,
                                   probe.box_max.y - p.y, p.z - probe.box_min.z, probe.box_max.z - p.z});
    if (inside < 0.0f) return 0.0f;
    if (probe.blend_distance <= 1e-4f) return 1.0f;
    return std::clamp(inside / probe.blend_distance, 0.0f, 1.0f);
}

core::Vec3 boxProjectedDirection(const PlacedReflectionProbe& probe, const core::Vec3& p, const core::Vec3& r) {
    if (!probe.box_projection) return r;
    // Rayo contra la caja desde dentro: la salida es la menor de las
    // distancias a los planos de delante en cada eje.
    const auto axis = [](float origin, float direction, float lo, float hi) {
        if (std::abs(direction) < 1e-6f) return std::numeric_limits<float>::max();
        return ((direction > 0.0f ? hi : lo) - origin) / direction;
    };
    const float t = std::max(std::min({axis(p.x, r.x, probe.box_min.x, probe.box_max.x),
                                       axis(p.y, r.y, probe.box_min.y, probe.box_max.y),
                                       axis(p.z, r.z, probe.box_min.z, probe.box_max.z)}),
                             0.0f);
    const core::Vec3 hit = p + r * t;
    const core::Vec3 d = hit - probe.position;
    const float len = core::length(d);
    return len > 1e-5f ? d * (1.0f / len) : r;
}

namespace {

float boxDistance(const PlacedReflectionProbe& probe, const core::Vec3& p) {
    const float dx = std::max({probe.box_min.x - p.x, 0.0f, p.x - probe.box_max.x});
    const float dy = std::max({probe.box_min.y - p.y, 0.0f, p.y - probe.box_max.y});
    const float dz = std::max({probe.box_min.z - p.z, 0.0f, p.z - probe.box_max.z});
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float boxVolume(const PlacedReflectionProbe& probe) {
    const core::Vec3 s = probe.box_max - probe.box_min;
    return std::max(s.x, 0.0f) * std::max(s.y, 0.0f) * std::max(s.z, 0.0f);
}

bool idLess(const PlacedProbeId& a, const PlacedProbeId& b) {
    return a.high != b.high ? a.high < b.high : a.low < b.low;
}

}  // namespace

std::vector<SelectedPlacedProbe> selectPlacedProbes(const std::vector<PlacedReflectionProbe>& probes,
                                                    const std::vector<PlacedProbeId>& baked, const core::Vec3& eye,
                                                    std::uint32_t max_count) {
    struct Candidate {
        SelectedPlacedProbe pick;
        float distance;
    };
    std::vector<Candidate> candidates;
    for (std::uint32_t i = 0; i < probes.size(); ++i) {
        const PlacedReflectionProbe& probe = probes[i];
        if (!(probe.intensity > 0.0f) || !probe.id.valid()) continue;
        const core::Vec3 size = probe.box_max - probe.box_min;
        if (!(size.x > 0.0f && size.y > 0.0f && size.z > 0.0f)) continue;
        const auto layer = std::find(baked.begin(), baked.end(), probe.id);
        if (layer == baked.end()) continue;
        candidates.push_back(
            Candidate{SelectedPlacedProbe{i, static_cast<std::uint32_t>(layer - baked.begin())}, boxDistance(probe, eye)});
    }
    // Las mas cercanas (a igual distancia, por id: estable entre frames).
    std::sort(candidates.begin(), candidates.end(), [&](const Candidate& a, const Candidate& b) {
        if (a.distance != b.distance) return a.distance < b.distance;
        return idLess(probes[a.pick.probe].id, probes[b.pick.probe].id);
    });
    if (candidates.size() > max_count) candidates.resize(max_count);
    // Orden de la GPU: prioridad, luego la mas pequena (la de una habitacion
    // manda sobre la de todo el edificio), luego el id.
    std::sort(candidates.begin(), candidates.end(), [&](const Candidate& a, const Candidate& b) {
        const PlacedReflectionProbe& pa = probes[a.pick.probe];
        const PlacedReflectionProbe& pb = probes[b.pick.probe];
        if (pa.priority != pb.priority) return pa.priority > pb.priority;
        const float va = boxVolume(pa);
        const float vb = boxVolume(pb);
        if (va != vb) return va < vb;
        return idLess(pa.id, pb.id);
    });
    std::vector<SelectedPlacedProbe> out;
    out.reserve(candidates.size());
    for (const Candidate& c : candidates) out.push_back(c.pick);
    return out;
}

PlacedProbeBlend blendPlacedProbes(const std::vector<PlacedReflectionProbe>& ordered, const core::Vec3& p) {
    // Manda la primera de la lista que pesa algo (la lista ya va por
    // prioridad y, a igualdad, la caja mas pequena delante), con su propio
    // peso aunque otra pese mas: asi una habitacion dentro de un edificio
    // entra fundiendose en su borde (antes, con la clave prioridad * 2 +
    // peso, el edificio pesaba 1 y mandaba hasta el final del fundido, y la
    // habitacion aparecia de golpe). La segunda es la que mas pesa de las
    // demas y solo rellena lo que deja la primera (w0 + w1 <= 1). Igual que
    // placedProbeReflection de lighting_body.glsl.
    PlacedProbeBlend blend;
    float w1 = 0.0f;
    for (int i = 0; i < static_cast<int>(ordered.size()); ++i) {
        const float w = placedProbeWeight(ordered[i], p);
        if (w <= 0.0f) continue;
        if (blend.first < 0) {
            blend.first = i;
            blend.first_weight = w;
            if (w >= 0.999f) break;  // cubre del todo: no hace falta otra
        } else if (w > w1) {
            blend.second = i;
            w1 = w;
        }
    }
    blend.second_weight = blend.second >= 0 ? std::min(w1, 1.0f - blend.first_weight) : 0.0f;
    if (blend.second >= 0 && blend.second_weight <= 0.0f) {
        blend.second = -1;
        blend.second_weight = 0.0f;
    }
    return blend;
}

GpuPlacedProbe packPlacedProbe(const PlacedReflectionProbe& probe, std::uint32_t layer,
                               const core::Vec3& baked_position) {
    GpuPlacedProbe out;
    out.center_intensity = core::Vec4{baked_position.x, baked_position.y, baked_position.z, probe.intensity};
    out.box_min_blend = core::Vec4{probe.box_min.x, probe.box_min.y, probe.box_min.z, std::max(probe.blend_distance, 0.0f)};
    out.box_max_priority = core::Vec4{probe.box_max.x, probe.box_max.y, probe.box_max.z, static_cast<float>(probe.priority)};
    out.params = core::Vec4{static_cast<float>(layer), probe.box_projection ? 1.0f : 0.0f, 0.0f, 0.0f};
    return out;
}

}  // namespace cramion::gfx
