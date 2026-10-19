#include "CramionCore/lighting/BakeTracer.h"

#include "CramionCore/lighting/BakeLights.h"
#include "CramionCore/lighting/ProbeBaker.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numeric>

namespace cramion::lighting {

using core::Vec3;

namespace {

constexpr std::uint32_t kLeafSize = 4;
constexpr int kBins = 16;
constexpr std::uint32_t kInteriorBit = 0x80000000u;
constexpr int kStackSize = 96;

float component(const Vec3& v, int axis) { return axis == 0 ? v.x : (axis == 1 ? v.y : v.z); }

struct Box {
    Vec3 lo{1e30f, 1e30f, 1e30f};
    Vec3 hi{-1e30f, -1e30f, -1e30f};
    void grow(const Vec3& p) {
        lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    void grow(const Box& b) {
        grow(b.lo);
        grow(b.hi);
    }
    float area() const {
        const Vec3 e = hi - lo;
        if (e.x < 0.0f || e.y < 0.0f || e.z < 0.0f) return 0.0f;
        return 2.0f * (e.x * e.y + e.y * e.z + e.z * e.x);
    }
};

}  // namespace

void BakeTracer::build(std::vector<Vec3> v0, std::vector<Vec3> e1, std::vector<Vec3> e2,
                       std::vector<std::uint32_t> material, std::vector<float> coverage, BakeProgress* progress) {
    nodes_.clear();
    v0_.clear();
    e1_.clear();
    e2_.clear();
    original_.clear();
    slot_of_.clear();
    slot_coverage_.clear();
    const auto count = static_cast<std::uint32_t>(std::min({v0.size(), e1.size(), e2.size()}));
    if (count == 0) return;
    if (progress) progress->setStage("Construyendo el BVH");

    std::vector<Box> boxes(count);
    std::vector<Vec3> centroids(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        Box b;
        b.grow(v0[i]);
        b.grow(v0[i] + e1[i]);
        b.grow(v0[i] + e2[i]);
        boxes[i] = b;
        centroids[i] = (b.lo + b.hi) * 0.5f;
    }
    std::vector<std::uint32_t> order(count);
    std::iota(order.begin(), order.end(), 0u);
    nodes_.reserve(static_cast<std::size_t>(count) / 2 + 16);
    nodes_.push_back(Node{});

    struct Item {
        std::uint32_t node, first, count;
    };
    std::vector<Item> stack;
    stack.push_back(Item{0, 0, count});
    std::uint64_t placed = 0;
    while (!stack.empty()) {
        const Item item = stack.back();
        stack.pop_back();
        Box bounds, cbounds;
        for (std::uint32_t i = item.first; i < item.first + item.count; ++i) {
            bounds.grow(boxes[order[i]]);
            cbounds.grow(centroids[order[i]]);
        }
        Node& node = nodes_[item.node];
        node.min[0] = bounds.lo.x;
        node.min[1] = bounds.lo.y;
        node.min[2] = bounds.lo.z;
        node.max[0] = bounds.hi.x;
        node.max[1] = bounds.hi.y;
        node.max[2] = bounds.hi.z;
        if (item.count <= kLeafSize) {
            node.left_or_first = item.first;
            node.count_axis = item.count;
            placed += item.count;
            if (progress && (placed & 0x3FFFu) < kLeafSize) {
                progress->fraction.store(static_cast<float>(static_cast<double>(placed) / count));
            }
            continue;
        }
        // SAH por cubos en los tres ejes.
        int best_axis = -1;
        int best_split = -1;
        float best_cost = 1e30f;
        for (int axis = 0; axis < 3; ++axis) {
            const float lo = component(cbounds.lo, axis);
            const float extent = component(cbounds.hi, axis) - lo;
            if (!(extent > 1e-12f)) continue;
            const float scale = static_cast<float>(kBins) / extent;
            Box bins[kBins];
            std::uint32_t counts[kBins] = {};
            for (std::uint32_t i = item.first; i < item.first + item.count; ++i) {
                const std::uint32_t t = order[i];
                const int b = std::clamp(static_cast<int>((component(centroids[t], axis) - lo) * scale), 0, kBins - 1);
                bins[b].grow(boxes[t]);
                ++counts[b];
            }
            float right_area[kBins] = {};
            std::uint32_t right_count[kBins] = {};
            Box acc;
            std::uint32_t n = 0;
            for (int b = kBins - 1; b > 0; --b) {
                acc.grow(bins[b]);
                n += counts[b];
                right_area[b] = acc.area();
                right_count[b] = n;
            }
            Box left;
            std::uint32_t left_n = 0;
            for (int split = 1; split < kBins; ++split) {
                left.grow(bins[split - 1]);
                left_n += counts[split - 1];
                if (left_n == 0 || right_count[split] == 0) continue;
                const float cost = left.area() * static_cast<float>(left_n) + right_area[split] * static_cast<float>(right_count[split]);
                if (cost < best_cost) {
                    best_cost = cost;
                    best_axis = axis;
                    best_split = split;
                }
            }
        }
        std::uint32_t mid = item.first;
        int axis = best_axis;
        if (best_axis >= 0) {
            const float lo = component(cbounds.lo, best_axis);
            const float scale = static_cast<float>(kBins) / (component(cbounds.hi, best_axis) - lo);
            const auto split_at = std::partition(order.begin() + item.first, order.begin() + item.first + item.count,
                                                 [&](std::uint32_t t) {
                                                     const int b = std::clamp(
                                                         static_cast<int>((component(centroids[t], best_axis) - lo) * scale),
                                                         0, kBins - 1);
                                                     return b < best_split;
                                                 });
            mid = static_cast<std::uint32_t>(split_at - order.begin());
        }
        if (mid == item.first || mid == item.first + item.count) {
            // Centroides iguales (o el SAH no parte): por la mitad en el eje mas largo.
            const Vec3 e = cbounds.hi - cbounds.lo;
            axis = e.x >= e.y && e.x >= e.z ? 0 : (e.y >= e.z ? 1 : 2);
            mid = item.first + item.count / 2;
            std::nth_element(order.begin() + item.first, order.begin() + mid, order.begin() + item.first + item.count,
                             [&](std::uint32_t a, std::uint32_t b) {
                                 const float ca = component(centroids[a], axis);
                                 const float cb = component(centroids[b], axis);
                                 return ca < cb || (ca == cb && a < b);
                             });
        }
        const auto left = static_cast<std::uint32_t>(nodes_.size());
        nodes_.push_back(Node{});
        nodes_.push_back(Node{});
        Node& parent = nodes_[item.node];  // (push_back puede haber movido el vector)
        parent.left_or_first = left;
        parent.count_axis = kInteriorBit | static_cast<std::uint32_t>(axis);
        stack.push_back(Item{left + 1, mid, item.first + item.count - mid});
        stack.push_back(Item{left, item.first, mid - item.first});
    }

    // Los triangulos en el orden de las hojas (lectura seguida al recorrer).
    v0_.resize(count);
    e1_.resize(count);
    e2_.resize(count);
    original_ = order;
    slot_of_.resize(count);
    bool any_cut = false;
    for (const float c : coverage) any_cut = any_cut || c < 1.0f;
    if (any_cut) slot_coverage_.assign(count, 1.0f);
    for (std::uint32_t slot = 0; slot < count; ++slot) {
        const std::uint32_t t = order[slot];
        v0_[slot] = v0[t];
        e1_[slot] = e1[t];
        e2_[slot] = e2[t];
        slot_of_[t] = slot;
        if (any_cut && t < material.size() && material[t] < coverage.size()) slot_coverage_[slot] = coverage[material[t]];
    }
    if (progress) progress->fraction.store(1.0f);
}

bool BakeTracer::passes(std::uint32_t slot, std::uint32_t seed) const {
    if (slot_coverage_.empty()) return false;
    const float c = slot_coverage_[slot];
    if (c >= 1.0f) return false;
    return hashToUnit(hashCombine(seed, original_[slot])) > c;
}

namespace {

inline bool boxHit(const float* mn, const float* mx, const Vec3& o, const Vec3& inv, float tmax) {
    float t0 = 0.0f;
    float t1 = tmax;
    const float origin[3] = {o.x, o.y, o.z};
    const float invd[3] = {inv.x, inv.y, inv.z};
    for (int i = 0; i < 3; ++i) {
        float ta = (mn[i] - origin[i]) * invd[i];
        float tb = (mx[i] - origin[i]) * invd[i];
        if (ta > tb) std::swap(ta, tb);
        t0 = ta > t0 ? ta : t0;
        t1 = tb < t1 ? tb : t1;
        if (t0 > t1) return false;
    }
    return true;
}

// Moller-Trumbore de doble cara.
inline bool triangleHit(const Vec3& v0, const Vec3& e1, const Vec3& e2, const Vec3& o, const Vec3& d, float tmax, float& t,
                        float& u, float& v, bool& backface) {
    const Vec3 p = core::cross(d, e2);
    const float det = core::dot(e1, p);
    if (std::fabs(det) < 1e-14f) return false;
    const float inv_det = 1.0f / det;
    const Vec3 s = o - v0;
    u = core::dot(s, p) * inv_det;
    if (u < 0.0f || u > 1.0f) return false;
    const Vec3 q = core::cross(s, e1);
    v = core::dot(d, q) * inv_det;
    if (v < 0.0f || u + v > 1.0f) return false;
    t = core::dot(e2, q) * inv_det;
    if (!(t > 0.0f) || t >= tmax) return false;
    backface = det < 0.0f;  // det = -dot(d, cross(e1, e2))
    return true;
}

inline Vec3 inverseDirection(const Vec3& d) {
    const auto inv = [](float x) { return 1.0f / (std::fabs(x) > 1e-20f ? x : (x < 0.0f ? -1e-20f : 1e-20f)); };
    return Vec3{inv(d.x), inv(d.y), inv(d.z)};
}

}  // namespace

bool BakeTracer::intersect(const Vec3& o, const Vec3& d, float tmax, BakeHit& hit, std::uint32_t seed) const {
    if (nodes_.empty()) return false;
    const Vec3 inv = inverseDirection(d);
    const float dir[3] = {d.x, d.y, d.z};
    std::uint32_t stack[kStackSize];
    int sp = 0;
    stack[sp++] = 0;
    float best = tmax;
    bool found = false;
    while (sp > 0) {
        const Node& n = nodes_[stack[--sp]];
        if (!boxHit(n.min, n.max, o, inv, best)) continue;
        if ((n.count_axis & kInteriorBit) == 0) {
            const std::uint32_t end = n.left_or_first + n.count_axis;
            for (std::uint32_t slot = n.left_or_first; slot < end; ++slot) {
                float t = 0.0f, u = 0.0f, v = 0.0f;
                bool back = false;
                if (!triangleHit(v0_[slot], e1_[slot], e2_[slot], o, d, best, t, u, v, back)) continue;
                if (passes(slot, seed)) continue;
                best = t;
                found = true;
                hit.t = t;
                hit.u = u;
                hit.v = v;
                hit.backface = back;
                hit.triangle = original_[slot];
            }
            continue;
        }
        const std::uint32_t axis = n.count_axis & 3u;
        const std::uint32_t left = n.left_or_first;
        const std::uint32_t near_child = dir[axis] >= 0.0f ? left : left + 1;
        const std::uint32_t far_child = dir[axis] >= 0.0f ? left + 1 : left;
        if (sp + 2 <= kStackSize) {
            stack[sp++] = far_child;
            stack[sp++] = near_child;
        }
    }
    return found;
}

bool BakeTracer::occluded(const Vec3& o, const Vec3& d, float tmax, std::uint32_t seed) const {
    if (nodes_.empty()) return false;
    const Vec3 inv = inverseDirection(d);
    const float dir[3] = {d.x, d.y, d.z};
    std::uint32_t stack[kStackSize];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const Node& n = nodes_[stack[--sp]];
        if (!boxHit(n.min, n.max, o, inv, tmax)) continue;
        if ((n.count_axis & kInteriorBit) == 0) {
            const std::uint32_t end = n.left_or_first + n.count_axis;
            for (std::uint32_t slot = n.left_or_first; slot < end; ++slot) {
                float t = 0.0f, u = 0.0f, v = 0.0f;
                bool back = false;
                if (triangleHit(v0_[slot], e1_[slot], e2_[slot], o, d, tmax, t, u, v, back) && !passes(slot, seed)) {
                    return true;
                }
            }
            continue;
        }
        const std::uint32_t axis = n.count_axis & 3u;
        const std::uint32_t left = n.left_or_first;
        const std::uint32_t near_child = dir[axis] >= 0.0f ? left : left + 1;
        const std::uint32_t far_child = dir[axis] >= 0.0f ? left + 1 : left;
        if (sp + 2 <= kStackSize) {
            stack[sp++] = far_child;
            stack[sp++] = near_child;
        }
    }
    return false;
}

Vec3 BakeTracer::point(const BakeHit& hit) const {
    const std::uint32_t slot = slot_of_[hit.triangle];
    return v0_[slot] + e1_[slot] * hit.u + e2_[slot] * hit.v;
}

Vec3 BakeTracer::geometricNormal(std::uint32_t t) const {
    const std::uint32_t slot = slot_of_[t];
    return core::normalize(core::cross(e1_[slot], e2_[slot]));
}

Vec3 offsetRay(const Vec3& p, const Vec3& n) {
    constexpr float kOrigin = 1.0f / 32.0f;
    constexpr float kFloatScale = 1.0f / 65536.0f;
    constexpr float kIntScale = 256.0f;
    const auto offset = [&](float pc, float nc) {
        const auto of = static_cast<std::int32_t>(kIntScale * nc);
        const std::int32_t bits = std::bit_cast<std::int32_t>(pc) + (pc < 0.0f ? -of : of);
        const float moved = std::bit_cast<float>(bits);
        return std::fabs(pc) < kOrigin ? pc + kFloatScale * nc : moved;
    };
    return Vec3{offset(p.x, n.x), offset(p.y, n.y), offset(p.z, n.z)};
}

}  // namespace cramion::lighting
