#include "CramionFX/asset/SettlementGenerator.h"

#include "CramionFX/asset/MedievalBuildings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <random>

namespace cramion::asset {

namespace {

using core::Vec2;
using core::Vec3;

constexpr float kPi = 3.14159265f;

struct Rnd {
    std::mt19937 engine;
    explicit Rnd(std::uint32_t seed) : engine(seed * 2654435761U + 977U) {}
    float range(float a, float b) { return std::uniform_real_distribution<float>(a, b)(engine); }
    bool chance(float p) { return range(0.0f, 1.0f) < p; }
    int pick(int n) { return n <= 1 ? 0 : std::min(static_cast<int>(range(0.0f, static_cast<float>(n))), n - 1); }
};

Vec2 xz(const Vec3& p) { return Vec2{p.x, p.z}; }
Vec2 add(const Vec2& a, const Vec2& b) { return Vec2{a.x + b.x, a.y + b.y}; }
Vec2 sub(const Vec2& a, const Vec2& b) { return Vec2{a.x - b.x, a.y - b.y}; }
Vec2 mul(const Vec2& a, float s) { return Vec2{a.x * s, a.y * s}; }
float dot2(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
float len2(const Vec2& a) { return std::sqrt(dot2(a, a)); }
Vec2 norm2(const Vec2& a) {
    const float l = len2(a);
    return l > 1e-6f ? mul(a, 1.0f / l) : Vec2{1.0f, 0.0f};
}
float angleDiff(float a, float b) { return std::abs(std::remainder(a - b, 2.0f * kPi)); }

// Caja orientada en el suelo.
struct Obb {
    Vec2 c{};
    Vec2 ax{1.0f, 0.0f};
    Vec2 az{0.0f, 1.0f};
    Vec2 half{};
    std::array<Vec2, 4> corners() const {
        const Vec2 x = mul(ax, half.x);
        const Vec2 z = mul(az, half.y);
        return {sub(sub(c, x), z), sub(add(c, x), z), add(add(c, x), z), add(sub(c, x), z)};
    }
    bool contains(const Vec2& p, float grow) const {
        const Vec2 l = sub(p, c);
        return std::abs(dot2(l, ax)) <= half.x + grow && std::abs(dot2(l, az)) <= half.y + grow;
    }
};

// Ejes del modelo en el suelo para un giro en Y (grados): +X local y +Z local.
Vec2 axisX(float yaw) {
    const float a = yaw * kPi / 180.0f;
    return Vec2{std::cos(a), -std::sin(a)};
}
Vec2 axisZ(float yaw) {
    const float a = yaw * kPi / 180.0f;
    return Vec2{std::sin(a), std::cos(a)};
}
float yawFacing(const Vec2& dir) { return std::atan2(dir.x, dir.y) * 180.0f / kPi; }

Obb lotObb(const SettlementLot& l, float pad = 0.0f) {
    Obb o;
    o.ax = axisX(l.yaw);
    o.az = axisZ(l.yaw);
    o.c = add(add(xz(l.position), mul(o.ax, l.offset.x)), mul(o.az, l.offset.y));
    o.half = Vec2{l.half.x + pad, l.half.y + pad};
    return o;
}

Obb fieldObb(const SettlementField& f, float pad = 0.0f) {
    Obb o;
    o.ax = axisX(f.yaw);
    o.az = axisZ(f.yaw);
    o.c = xz(f.center);
    o.half = Vec2{f.half.x + pad, f.half.y + pad};
    return o;
}

bool overlap(const Obb& a, const Obb& b) {
    const Vec2 d = sub(b.c, a.c);
    for (const Vec2& n : {a.ax, a.az, b.ax, b.az}) {
        const float ra = a.half.x * std::abs(dot2(a.ax, n)) + a.half.y * std::abs(dot2(a.az, n));
        const float rb = b.half.x * std::abs(dot2(b.ax, n)) + b.half.y * std::abs(dot2(b.az, n));
        if (std::abs(dot2(d, n)) > ra + rb) return false;
    }
    return true;
}

float distPointSeg(const Vec2& p, const Vec2& a, const Vec2& b) {
    const Vec2 ab = sub(b, a);
    const float l2 = dot2(ab, ab);
    const float t = l2 > 1e-8f ? std::clamp(dot2(sub(p, a), ab) / l2, 0.0f, 1.0f) : 0.0f;
    return len2(sub(p, add(a, mul(ab, t))));
}

float distToPolyline(const Vec2& p, const std::vector<Vec3>& pts) {
    float best = 1e30f;
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) best = std::min(best, distPointSeg(p, xz(pts[i]), xz(pts[i + 1])));
    return best;
}

// Un tramo pasa a menos de `clearance` de la caja?
bool segmentNearObb(const Vec2& a, const Vec2& b, const Obb& o, float clearance) {
    // Lejos del todo: descarte rapido.
    const float r = len2(o.half) + clearance;
    if (distPointSeg(o.c, a, b) > r) return false;
    const auto corners = o.corners();
    for (int i = 0; i < 4; ++i) {
        if (distPointSeg(corners[static_cast<std::size_t>(i)], a, b) < clearance) return true;
        const Vec2 mid = mul(add(corners[static_cast<std::size_t>(i)], corners[static_cast<std::size_t>((i + 1) % 4)]), 0.5f);
        if (distPointSeg(mid, a, b) < clearance) return true;
    }
    const float l = len2(sub(b, a));
    const int samples = std::max(2, static_cast<int>(l / 0.8f));
    for (int k = 0; k <= samples; ++k) {
        const Vec2 p = add(a, mul(sub(b, a), static_cast<float>(k) / static_cast<float>(samples)));
        if (o.contains(p, clearance)) return true;
    }
    return false;
}

bool pointInPolygon(const Vec2& p, const std::vector<Vec3>& ring) {
    bool in = false;
    for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
        const Vec2 a = xz(ring[i]);
        const Vec2 b = xz(ring[j]);
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
    }
    return in;
}

float distToRing(const Vec2& p, const std::vector<Vec3>& ring) {
    float best = 1e30f;
    for (std::size_t i = 0; i < ring.size(); ++i) best = std::min(best, distPointSeg(p, xz(ring[i]), xz(ring[(i + 1) % ring.size()])));
    return best;
}

// Recorrido por longitud de arco de una polilinea.
struct Path {
    const std::vector<Vec3>* pts = nullptr;
    std::vector<float> cum;
    float length = 0.0f;
    explicit Path(const std::vector<Vec3>& points) : pts(&points) {
        cum.push_back(0.0f);
        for (std::size_t i = 0; i + 1 < points.size(); ++i) {
            length += len2(sub(xz(points[i + 1]), xz(points[i])));
            cum.push_back(length);
        }
    }
    std::size_t segment(float s) const {
        const auto it = std::upper_bound(cum.begin(), cum.end(), s);
        const std::size_t i = it == cum.begin() ? 0 : static_cast<std::size_t>(it - cum.begin()) - 1;
        return std::min(i, pts->size() >= 2 ? pts->size() - 2 : 0);
    }
    Vec2 at(float s) const {
        if (pts->empty()) return Vec2{};
        if (pts->size() < 2) return xz(pts->front());
        const std::size_t i = segment(s);
        const float l = cum[i + 1] - cum[i];
        const float t = l > 1e-6f ? std::clamp((s - cum[i]) / l, 0.0f, 1.0f) : 0.0f;
        return add(xz((*pts)[i]), mul(sub(xz((*pts)[i + 1]), xz((*pts)[i])), t));
    }
    Vec2 tangent(float s) const {
        if (pts->size() < 2) return Vec2{1.0f, 0.0f};
        const std::size_t i = segment(s);
        return norm2(sub(xz((*pts)[i + 1]), xz((*pts)[i])));
    }
};

class Planner {
public:
    Planner(const SettlementSettings& s, const SettlementTerrain& t, const SettlementFootprint& f)
        : s_(s), t_(t), foot_(f), rng_(s.seed * 7919U + 31U) {}

    SettlementLayout run(const Vec3& hint, float search) {
        defaults();
        if (!findCenter(hint, search)) return std::move(out_);
        mainRoads();
        if (s_.type != SettlementType::Hamlet) sideStreets();
        if (town_ && s_.walls) walls();
        specials();
        houses();
        if (s_.fields) fields();
        windmill();
        if (s_.props) props();
        out_.ok = true;
        out_.houses = houses_;
        return std::move(out_);
    }

private:
    SettlementSettings s_;
    const SettlementTerrain& t_;
    const SettlementFootprint& foot_;
    Rnd rng_;
    SettlementLayout out_;
    bool town_ = false;
    float R_ = 95.0f;
    float plaza_ = 14.0f;
    int target_ = 28;
    int houses_ = 0;
    float max_house_dist_ = 0.0f;  // la casa mas lejana (los campos, despues)
    std::vector<float> headings_;  // de las calles principales
    std::vector<Obb> taken_;       // huellas con su holgura

    float height(float x, float z) const { return t_.height ? t_.height(x, z) : 0.0f; }
    bool dry(float x, float z, float margin) const { return !t_.dry || t_.dry(x, z, margin); }
    bool inTerrain(const Vec2& p, float margin) const {
        return p.x > t_.min_x + margin && p.y > t_.min_z + margin && p.x < t_.max_x - margin && p.y < t_.max_z - margin;
    }
    Vec3 ground(const Vec2& p) const { return Vec3{p.x, height(p.x, p.y), p.y}; }
    Vec2 center2() const { return xz(out_.center); }
    float distC(const Vec2& p) const { return len2(sub(p, center2())); }
    bool insideWalls(const Vec2& p, float margin) const {
        if (out_.wall.size() < 3) return true;
        return pointInPolygon(p, out_.wall) && distToRing(p, out_.wall) > margin;
    }

    void defaults() {
        town_ = s_.type == SettlementType::Town;
        switch (s_.type) {
            case SettlementType::Hamlet: R_ = 55.0f; plaza_ = 8.0f; target_ = 10; break;
            case SettlementType::Village: R_ = 95.0f; plaza_ = 14.0f; target_ = 28; break;
            case SettlementType::Town: R_ = 130.0f; plaza_ = 20.0f; target_ = 75; break;
        }
        if (s_.radius > 1.0f) R_ = std::clamp(s_.radius, 25.0f, 600.0f);
        if (s_.houses > 0) target_ = s_.houses;
        // Mas casas que sitio: el radio crece (densidad por tipo).
        const float per_house = s_.type == SettlementType::Town ? 210.0f : (s_.type == SettlementType::Village ? 330.0f : 300.0f);
        if (s_.radius <= 1.0f) R_ = std::max(R_, std::sqrt(static_cast<float>(target_) * per_house / kPi) + plaza_);
        out_.radius = R_;
        out_.plaza_radius = plaza_;
    }

    // Alturas en una huella circular: diferencia entre la mas alta y la mas baja.
    float roughness(const Vec2& c, float r) const {
        float lo = 1e30f;
        float hi = -1e30f;
        for (int i = 0; i < 7; ++i) {
            for (int j = 0; j < 7; ++j) {
                const float x = c.x + (static_cast<float>(i) / 6.0f - 0.5f) * 2.0f * r;
                const float z = c.y + (static_cast<float>(j) / 6.0f - 0.5f) * 2.0f * r;
                const float h = height(x, z);
                lo = std::min(lo, h);
                hi = std::max(hi, h);
            }
        }
        return hi - lo;
    }

    bool findCenter(const Vec3& hint, float search) {
        Vec2 best{hint.x, hint.z};
        if (search > 1.0f) {
            float best_score = 1e30f;
            for (int i = 0; i < 900; ++i) {
                const float a = rng_.range(0.0f, 2.0f * kPi);
                const float r = search * std::sqrt(rng_.range(0.0f, 1.0f));
                const Vec2 c{hint.x + std::cos(a) * r, hint.z + std::sin(a) * r};
                if (!inTerrain(c, R_ * 0.6f + 10.0f) || !dry(c.x, c.y, plaza_ + 20.0f)) continue;
                // Tierra seca alrededor (no en una punta rodeada de agua).
                int wet = 0;
                for (int k = 0; k < 12; ++k) {
                    const float b = 2.0f * kPi * static_cast<float>(k) / 12.0f;
                    if (!dry(c.x + std::cos(b) * R_ * 0.7f, c.y + std::sin(b) * R_ * 0.7f, 2.0f)) ++wet;
                }
                if (wet > 3) continue;
                const float score = roughness(c, plaza_ * 2.5f) + roughness(c, R_ * 0.6f) * 0.25f + height(c.x, c.y) * 0.01f + wet * 4.0f;
                if (score < best_score) {
                    best_score = score;
                    best = c;
                }
            }
            if (best_score >= 1e29f) {
                out_.error = "no hay sitio llano y seco para el pueblo";
                return false;
            }
        } else if (!dry(best.x, best.y, 2.0f)) {
            out_.error = "el centro del pueblo cae en el agua";
            return false;
        }
        out_.center = ground(best);
        return true;
    }

    float roadWidth(bool main) const {
        if (town_) return main ? 6.0f : 4.2f;
        if (s_.type == SettlementType::Hamlet) return main ? 3.6f : 3.0f;
        return main ? 5.0f : 3.6f;
    }

    // Avanza un camino desde `start` con rumbo `heading` buscando lo llano.
    std::vector<Vec3> trace(const Vec2& start, float heading, float length, float step, int self, float stop_near_roads,
                            float max_dist_center, int parent = -1) const {
        std::vector<Vec3> pts{ground(start)};
        Vec2 cur = start;
        float h = heading;
        float walked = 0.0f;
        while (walked < length) {
            float best_cost = 1e30f;
            float best_h = h;
            Vec2 best_p{};
            for (float d : {-0.42f, -0.21f, 0.0f, 0.21f, 0.42f}) {
                const float hh = h + d;
                const Vec2 next{cur.x + std::cos(hh) * step, cur.y + std::sin(hh) * step};
                if (!inTerrain(next, 8.0f)) continue;
                float cost = std::abs(height(next.x, next.y) - height(cur.x, cur.y)) * 3.0f + std::abs(d) * 0.9f +
                             angleDiff(hh, heading) * 1.4f;
                if (!dry(next.x, next.y, 3.0f)) cost += 1000.0f;
                if (cost < best_cost) {
                    best_cost = cost;
                    best_h = hh;
                    best_p = next;
                }
            }
            if (best_cost >= 1000.0f) break;
            if (max_dist_center > 0.0f && distC(best_p) > max_dist_center) break;
            if (stop_near_roads > 0.0f && walked > 8.0f) {
                bool near = false;
                for (std::size_t r = 0; r < out_.roads.size() && !near; ++r) {
                    if (static_cast<int>(r) == self) continue;
                    if (static_cast<int>(r) == parent && walked < 25.0f) continue;  // de la que sale
                    near = distToPolyline(best_p, out_.roads[r].points) < stop_near_roads;
                }
                if (near) {
                    // Llega a la otra calle: ultimo punto pegado a ella.
                    pts.push_back(ground(best_p));
                    break;
                }
            }
            cur = best_p;
            h = best_h;
            walked += step;
            pts.push_back(ground(cur));
        }
        return pts;
    }

    void mainRoads() {
        int n = s_.main_roads;
        if (n <= 0) n = s_.type == SettlementType::Hamlet ? 2 + rng_.pick(2) : (town_ ? 4 : 3 + rng_.pick(2));
        n = std::clamp(n, 1, 8);
        const float base = rng_.range(0.0f, 2.0f * kPi);
        const float reach = R_ * (town_ ? 1.9f : (s_.type == SettlementType::Hamlet ? 2.5f : 1.6f));
        for (int i = 0; i < n; ++i) {
            const float h = base + 2.0f * kPi * static_cast<float>(i) / static_cast<float>(n) + rng_.range(-0.25f, 0.25f);
            SettlementRoad road;
            road.main = true;
            road.width = roadWidth(true);
            road.points = trace(center2(), h, reach, 6.0f, -1, 0.0f, 0.0f);
            if (road.points.size() < 3) continue;
            headings_.push_back(h);
            out_.roads.push_back(std::move(road));
        }
    }

    void sideStreets() {
        const std::size_t mains = out_.roads.size();
        const float limit = town_ ? R_ * 0.95f : R_ * 0.9f;
        for (std::size_t r = 0; r < mains; ++r) {
            // Copia: al anadir callejas el vector de calles se realoja.
            const std::vector<Vec3> parent = out_.roads[r].points;
            const Path path(parent);
            float s = plaza_ + rng_.range(14.0f, 22.0f);
            int side = rng_.chance(0.5f) ? 1 : -1;
            while (s < std::min(path.length, limit)) {
                if (rng_.chance(town_ ? 0.95f : 0.75f)) {
                    const Vec2 p = path.at(s);
                    const Vec2 tg = path.tangent(s);
                    const float th = std::atan2(tg.y, tg.x);
                    const float h = th + static_cast<float>(side) * (kPi * 0.5f + rng_.range(-0.25f, 0.25f));
                    SettlementRoad road;
                    road.main = false;
                    road.width = roadWidth(false);
                    road.points = trace(p, h, rng_.range(R_ * 0.25f, R_ * 0.6f), 5.0f, static_cast<int>(out_.roads.size()),
                                        road.width + 6.0f, limit, static_cast<int>(r));
                    if (Path(road.points).length > 14.0f) out_.roads.push_back(std::move(road));
                    side = -side;
                }
                s += rng_.range(town_ ? 24.0f : 28.0f, town_ ? 34.0f : 44.0f);
            }
        }
        // Ciudad: calle de ronda que une las principales.
        if (town_) {
            SettlementRoad ring;
            ring.main = false;
            ring.width = roadWidth(false);
            const float rr = R_ * 0.56f;
            const int count = 28;
            for (int k = 0; k <= count; ++k) {
                const float a = 2.0f * kPi * static_cast<float>(k % count) / static_cast<float>(count);
                const float jitter = 1.0f + 0.05f * std::sin(a * 3.0f + static_cast<float>(s_.seed));
                const Vec2 p{out_.center.x + std::cos(a) * rr * jitter, out_.center.z + std::sin(a) * rr * jitter};
                if (!dry(p.x, p.y, 2.0f) || !inTerrain(p, 4.0f)) {
                    if (ring.points.size() >= 3) out_.roads.push_back(ring);
                    ring.points.clear();
                    continue;
                }
                ring.points.push_back(ground(p));
            }
            if (ring.points.size() >= 3) out_.roads.push_back(std::move(ring));
        }
    }

    void walls() {
        const float Rw = R_ * 1.02f;
        // Donde cruza cada calle principal el radio de la muralla.
        std::vector<float> gates;
        for (const SettlementRoad& road : out_.roads) {
            if (!road.main) continue;
            for (std::size_t i = 0; i + 1 < road.points.size(); ++i) {
                const float d0 = distC(xz(road.points[i]));
                const float d1 = distC(xz(road.points[i + 1]));
                if (d0 < Rw && d1 >= Rw) {
                    const float t = (Rw - d0) / std::max(d1 - d0, 1e-4f);
                    const Vec2 p = add(xz(road.points[i]), mul(sub(xz(road.points[i + 1]), xz(road.points[i])), t));
                    gates.push_back(std::atan2(p.y - out_.center.z, p.x - out_.center.x));
                    break;
                }
            }
        }
        std::sort(gates.begin(), gates.end());
        const float delta = 5.0f / Rw;  // medio hueco de la puerta (10 m)
        // Angulos del anillo: cada puerta son dos puntos; entre puertas,
        // puntos (torres) cada ~30 m con el radio algo irregular.
        struct Pt {
            float angle;
            float radius;
            bool gate_start;
            bool tower;
        };
        std::vector<Pt> pts;
        const auto radius_at = [&](float a) {
            float r = Rw * (0.94f + 0.12f * (0.5f + 0.5f * std::sin(a * 3.0f + static_cast<float>(s_.seed % 97U))));
            // Que no caiga en el agua.
            for (int k = 0; k < 8 && !dry(out_.center.x + std::cos(a) * r, out_.center.z + std::sin(a) * r, 6.0f); ++k) r *= 0.95f;
            return r;
        };
        if (gates.empty()) {
            const int count = std::clamp(static_cast<int>(2.0f * kPi * Rw / 30.0f), 10, 26);
            for (int k = 0; k < count; ++k) {
                const float a = 2.0f * kPi * static_cast<float>(k) / static_cast<float>(count);
                pts.push_back(Pt{a, radius_at(a), false, true});
            }
        } else {
            for (std::size_t g = 0; g < gates.size(); ++g) {
                const float a0 = gates[g];
                float a1 = gates[(g + 1) % gates.size()];
                if (a1 <= a0) a1 += 2.0f * kPi;
                pts.push_back(Pt{a0 - delta, Rw, true, false});
                pts.push_back(Pt{a0 + delta, Rw, false, false});
                const float from = a0 + delta;
                const float to = a1 - delta;
                const float arc = (to - from) * Rw;
                const int towers = std::max(1, static_cast<int>(std::round(arc / 30.0f)));
                for (int k = 1; k < towers; ++k) {
                    const float a = from + (to - from) * static_cast<float>(k) / static_cast<float>(towers);
                    pts.push_back(Pt{a, radius_at(a), false, true});
                }
                // Torres junto a las puertas: a 7 m de cada lado.
            }
        }
        for (const Pt& p : pts) {
            const Vec2 q{out_.center.x + std::cos(p.angle) * p.radius, out_.center.z + std::sin(p.angle) * p.radius};
            out_.wall.push_back(ground(q));
            out_.wall_towers.push_back(p.tower);
            out_.wall_gaps.push_back(p.gate_start);
        }
        // Las puertas (lote Gatehouse) en cada hueco.
        for (std::size_t i = 0; i < out_.wall.size(); ++i) {
            if (!out_.wall_gaps[i]) continue;
            const Vec3& a = out_.wall[i];
            const Vec3& b = out_.wall[(i + 1) % out_.wall.size()];
            const Vec2 mid = mul(add(xz(a), xz(b)), 0.5f);
            Vec2 outward = norm2(sub(mid, center2()));
            SettlementLot lot;
            lot.kind = LotKind::Gatehouse;
            lot.yaw = yawFacing(outward);
            foot_(lot.kind, 0, lot.half, lot.offset);
            lot.position = ground(mid);  // el paso, centrado en el hueco
            out_.lots.push_back(lot);
            taken_.push_back(lotObb(lot, 1.0f));
        }
    }

    // Comprueba una huella: dentro del pueblo, seca, poco empinada, sin pisar
    // nada ni las calles. `plaza_ok`: puede entrar en la plaza.
    bool valid(const SettlementLot& lot, float pad, float slope, bool plaza_ok, bool outside_ok, float road_extra) const {
        const Obb o = lotObb(lot);
        const auto corners = o.corners();
        float lo = 1e30f;
        float hi = -1e30f;
        for (const Vec2& c : corners) {
            if (!inTerrain(c, 3.0f) || !dry(c.x, c.y, 2.0f)) return false;
            if (!plaza_ok && distC(c) < plaza_ + 0.5f) return false;
            if (!outside_ok) {
                if (out_.wall.size() >= 3) {
                    if (!insideWalls(c, s_.walls ? 4.5f : 0.0f)) return false;
                } else if (distC(c) > R_ * (s_.type == SettlementType::Hamlet ? 1.7f : 1.15f)) {
                    return false;
                }
            } else if (out_.wall.size() >= 3 && (pointInPolygon(c, out_.wall) || distToRing(c, out_.wall) < 5.0f)) {
                return false;
            }
            const float h = height(c.x, c.y);
            lo = std::min(lo, h);
            hi = std::max(hi, h);
        }
        const float hc = height(o.c.x, o.c.y);
        lo = std::min(lo, hc);
        hi = std::max(hi, hc);
        if (hi - lo > slope) return false;
        if (!dry(o.c.x, o.c.y, 2.0f)) return false;
        const Obb padded = lotObb(lot, pad);
        for (const Obb& other : taken_) {
            if (overlap(padded, other)) return false;
        }
        for (const SettlementField& f : out_.fields) {
            if (overlap(padded, fieldObb(f, 1.0f))) return false;
        }
        for (const SettlementRoad& road : out_.roads) {
            for (std::size_t i = 0; i + 1 < road.points.size(); ++i) {
                if (segmentNearObb(xz(road.points[i]), xz(road.points[i + 1]), o, road.width * 0.5f + road_extra)) return false;
            }
        }
        return true;
    }

    void commit(const SettlementLot& lot, float pad) {
        out_.lots.push_back(lot);
        taken_.push_back(lotObb(lot, pad));
    }

    // Lote con la fachada mirando a `dir_to_front` (desde fuera hacia el
    // frente), con el frente a `front_dist` del punto `anchor`.
    SettlementLot facing(LotKind kind, int variant, const Vec2& anchor, const Vec2& outward, float front_dist) const {
        SettlementLot lot;
        lot.kind = kind;
        lot.variant = variant;
        foot_(kind, variant, lot.half, lot.offset);
        lot.yaw = yawFacing(mul(outward, -1.0f));  // la fachada mira hacia el ancla
        const Vec2 X = axisX(lot.yaw);
        // Frente del AABB (z local = offset.y + half.y) a front_dist del ancla.
        const float d = front_dist + lot.offset.y + lot.half.y;
        const Vec2 origin = sub(add(anchor, mul(outward, d)), mul(X, lot.offset.x));
        lot.position = ground(origin);
        return lot;
    }

    void specials() {
        // Pozo en el centro de la plaza.
        {
            SettlementLot well;
            well.kind = LotKind::Well;
            foot_(well.kind, 0, well.half, well.offset);
            well.yaw = rng_.range(0.0f, 360.0f);
            well.position = out_.center;
            commit(well, 1.5f);
        }
        // Huecos entre calles principales, de mayor a menor.
        std::vector<float> hs = headings_;
        std::sort(hs.begin(), hs.end(), [](float a, float b) { return std::remainder(a, 2.0f * kPi) < std::remainder(b, 2.0f * kPi); });
        struct Gap {
            float mid;
            float size;
        };
        std::vector<Gap> gaps;
        if (hs.empty()) {
            gaps.push_back(Gap{rng_.range(0.0f, 2.0f * kPi), 2.0f * kPi});
        } else {
            for (std::size_t i = 0; i < hs.size(); ++i) {
                const float a = std::remainder(hs[i], 2.0f * kPi);
                float b = std::remainder(hs[(i + 1) % hs.size()], 2.0f * kPi);
                if (b <= a) b += 2.0f * kPi;
                gaps.push_back(Gap{(a + b) * 0.5f, b - a});
            }
        }
        std::sort(gaps.begin(), gaps.end(), [](const Gap& a, const Gap& b) { return a.size > b.size; });
        // Iglesia y taberna en la plaza (pueblo y ciudad).
        const auto onPlaza = [&](LotKind kind, int variant, std::size_t gap_index, float shift) {
            for (std::size_t g = gap_index; g < gaps.size() + gap_index; ++g) {
                const Gap& gap = gaps[g % gaps.size()];
                for (float extra = 1.5f; extra < 26.0f; extra += 3.0f) {
                    for (float sh : {shift, shift * 0.5f, -shift * 0.5f, 0.0f}) {
                        const float a = gap.mid + sh * gap.size;
                        const Vec2 dir{std::cos(a), std::sin(a)};
                        SettlementLot lot = facing(kind, variant, center2(), dir, plaza_ + extra);
                        if (valid(lot, 1.2f, s_.max_slope + 1.5f, false, false, 0.6f)) {
                            commit(lot, 1.2f);
                            return true;
                        }
                    }
                }
            }
            return false;
        };
        if (s_.type != SettlementType::Hamlet) {
            if (s_.church) onPlaza(LotKind::Church, 0, 0, 0.0f);
            onPlaza(LotKind::Tavern, 0, gaps.size() > 1 ? 1 : 0, gaps.size() > 1 ? 0.0f : 0.3f);
        }
        // Torre del homenaje (ciudad): en lo mas alto intramuros.
        if (town_) {
            float best = -1e30f;
            SettlementLot best_lot;
            bool found = false;
            for (int i = 0; i < 160; ++i) {
                const float a = rng_.range(0.0f, 2.0f * kPi);
                const float r = R_ * rng_.range(0.4f, 0.78f);
                const Vec2 p{out_.center.x + std::cos(a) * r, out_.center.z + std::sin(a) * r};
                SettlementLot lot;
                lot.kind = LotKind::Keep;
                foot_(lot.kind, 0, lot.half, lot.offset);
                lot.yaw = yawFacing(norm2(sub(center2(), p)));
                lot.position = ground(p);
                const float score = lot.position.y;
                if (score <= best) continue;
                if (!valid(lot, 3.0f, s_.max_slope + 3.0f, false, false, 1.5f)) continue;
                best = score;
                best_lot = lot;
                found = true;
            }
            if (found) commit(best_lot, 3.0f);
        }
        // Mercado: puestos alrededor del pozo mirando al centro.
        if (s_.market && s_.type != SettlementType::Hamlet) {
            const int stalls = town_ ? 7 : 4;
            const float r = plaza_ * 0.62f;
            int placed = 0;
            for (int k = 0; k < stalls * 3 && placed < stalls; ++k) {
                const float a = rng_.range(0.0f, 2.0f * kPi);
                bool on_road = false;
                for (float h : headings_) on_road = on_road || angleDiff(a, h) < std::asin(std::min(1.0f, (roadWidth(true) * 0.5f + 2.2f) / r));
                if (on_road) continue;
                const Vec2 dir{std::cos(a), std::sin(a)};
                SettlementLot lot;
                lot.kind = LotKind::MarketStall;
                lot.variant = placed;
                foot_(lot.kind, lot.variant, lot.half, lot.offset);
                lot.yaw = yawFacing(mul(dir, -1.0f));
                lot.position = ground(add(center2(), mul(dir, r)));
                if (!valid(lot, 0.8f, 2.5f, true, false, 0.2f)) continue;
                commit(lot, 0.8f);
                ++placed;
            }
        }
    }

    // Casas (y herrerias, graneros, la segunda taberna) a los dos lados de
    // las calles, de dentro afuera: siempre se rellena la que tiene el cursor
    // mas cerca del centro.
    void houses() {
        struct Cursor {
            std::size_t road;
            int side;
            float s;
            float end;
            bool active;
        };
        std::vector<Path> paths;
        paths.reserve(out_.roads.size());
        for (const SettlementRoad& road : out_.roads) paths.emplace_back(road.points);
        std::vector<Cursor> cursors;
        const float limit = town_ ? R_ * 1.02f : R_ * (s_.type == SettlementType::Hamlet ? 1.7f : 1.1f);
        for (std::size_t r = 0; r < out_.roads.size(); ++r) {
            for (int side : {-1, 1}) {
                const float start = out_.roads[r].main ? plaza_ + 2.0f : 5.0f;
                cursors.push_back(Cursor{r, side, start + rng_.range(0.0f, 3.0f), paths[r].length, true});
            }
        }
        int smithies = s_.type == SettlementType::Hamlet ? (rng_.chance(0.5f) ? 1 : 0) : (town_ ? 2 : 1);
        int taverns = town_ ? 1 : 0;  // la de la plaza ya esta
        int shops = town_ ? 3 : (s_.type == SettlementType::Village ? 1 : 0);
        int failures = 0;
        while (houses_ < target_ && failures < 20000) {
            Cursor* best = nullptr;
            float best_d = 1e30f;
            for (Cursor& c : cursors) {
                if (!c.active) continue;
                if (c.s >= c.end) {
                    c.active = false;
                    continue;
                }
                const float d = distC(paths[c.road].at(c.s));
                if (d > limit) {
                    // Las principales siguen fuera: alli ya no hay casas.
                    c.active = false;
                    continue;
                }
                if (d < best_d) {
                    best_d = d;
                    best = &c;
                }
            }
            if (best == nullptr) break;
            Cursor& c = *best;
            const Path& path = paths[c.road];
            const SettlementRoad& road = out_.roads[c.road];
            const float zone = best_d / R_;
            // Que va aqui.
            LotKind kind = LotKind::House;
            int variant = 0;
            if (smithies > 0 && zone > 0.3f && rng_.chance(0.3f)) {
                kind = LotKind::Smithy;
            } else if (taverns > 0 && zone < 0.6f && rng_.chance(0.15f)) {
                kind = LotKind::Tavern;
            } else if (s_.fields && !town_ && zone > 0.62f && rng_.chance(0.16f)) {
                kind = LotKind::Barn;
            } else {
                const bool urban = town_ || zone < (s_.type == SettlementType::Hamlet ? 0.0f : 0.55f);
                const int urban_n = std::max(s_.urban_variants, 1);
                const int rural_n = std::max(s_.rural_variants, 0);
                if (urban || rural_n == 0) {
                    variant = rng_.pick(urban_n);
                } else {
                    variant = urban_n + rng_.pick(rural_n);
                }
                if (shops > 0 && zone < 0.45f && rng_.chance(0.12f)) variant = -1 - variant;  // tienda (variante urbana)
            }
            SettlementLot lot;
            lot.kind = kind;
            lot.variant = variant;
            foot_(kind, variant, lot.half, lot.offset);
            float gap;
            float setback;
            if (town_) {
                gap = rng_.range(0.25f, 0.7f);
                setback = rng_.range(0.3f, 0.9f);
            } else if (s_.type == SettlementType::Village) {
                gap = zone < 0.55f ? rng_.range(0.8f, 2.5f) : rng_.range(3.0f, 7.0f);
                setback = zone < 0.55f ? rng_.range(0.8f, 2.0f) : rng_.range(2.0f, 5.0f);
            } else {
                gap = rng_.range(5.0f, 12.0f);
                setback = rng_.range(2.5f, 7.0f);
            }
            const float sc = c.s + lot.half.x + gap * 0.5f;
            if (sc + lot.half.x > c.end) {
                c.active = false;
                continue;
            }
            const Vec2 p = path.at(sc);
            const Vec2 tg = path.tangent(sc);
            const Vec2 n = mul(Vec2{-tg.y, tg.x}, static_cast<float>(c.side));
            lot.yaw = yawFacing(mul(n, -1.0f));
            const Vec2 X = axisX(lot.yaw);
            const float d = road.width * 0.5f + setback + lot.offset.y + lot.half.y;
            lot.position = ground(sub(add(p, mul(n, d)), mul(X, lot.offset.x)));
            const float pad = town_ ? 0.2f : 0.8f;
            const float slope = kind == LotKind::Barn ? s_.max_slope + 1.0f : s_.max_slope;
            if (valid(lot, pad, slope, false, false, 0.35f)) {
                commit(lot, pad);
                max_house_dist_ = std::max(max_house_dist_, len2(sub(lotObb(lot).c, center2())) + std::max(lot.half.x, lot.half.y));
                c.s = sc + lot.half.x + gap * 0.5f;
                if (kind == LotKind::Smithy) {
                    --smithies;
                } else if (kind == LotKind::Tavern) {
                    --taverns;
                } else if (kind == LotKind::House) {
                    ++houses_;
                    if (lot.variant < 0) --shops;
                }
                failures = 0;
            } else {
                c.s += 2.0f;
                ++failures;
            }
        }
    }

    void fields() {
        int max_fields = s_.type == SettlementType::Hamlet ? 4 : (town_ ? 8 : 7);
        float start = town_ ? R_ * 1.12f : R_ * (s_.type == SettlementType::Hamlet ? 0.75f : 0.85f);
        if (!town_) start = std::max(start, max_house_dist_ + 6.0f);  // los campos, mas alla de las casas
        int made = 0;
        for (std::size_t r = 0; r < out_.roads.size() && made < max_fields; ++r) {
            if (!out_.roads[r].main) continue;
            const Path path(out_.roads[r].points);
            float s = 0.0f;
            // Primer punto del camino ya fuera.
            while (s < path.length && distC(path.at(s)) < start) s += 4.0f;
            int side = rng_.chance(0.5f) ? 1 : -1;
            while (s < path.length - 10.0f && made < max_fields) {
                SettlementField f;
                f.half = Vec2{rng_.range(14.0f, 24.0f), rng_.range(11.0f, 20.0f)};
                const float sc = s + f.half.x;
                if (sc + f.half.x > path.length) break;
                const Vec2 p = path.at(sc);
                const Vec2 tg = path.tangent(sc);
                const Vec2 n = mul(Vec2{-tg.y, tg.x}, static_cast<float>(side));
                f.yaw = yawFacing(mul(n, -1.0f));
                const Vec2 c = add(p, mul(n, out_.roads[r].width * 0.5f + 2.5f + f.half.y));
                f.center = ground(c);
                if (fieldOk(f)) {
                    out_.fields.push_back(f);
                    taken_.push_back(fieldObb(f, 2.0f));
                    fenceField(f, n);
                    ++made;
                    s = sc + f.half.x + rng_.range(4.0f, 10.0f);
                    side = -side;
                } else {
                    s += 6.0f;
                    if (rng_.chance(0.3f)) side = -side;
                }
            }
        }
    }

    bool fieldOk(const SettlementField& f) const {
        const Obb o = fieldObb(f);
        float lo = 1e30f;
        float hi = -1e30f;
        for (const Vec2& c : o.corners()) {
            if (!inTerrain(c, 4.0f) || !dry(c.x, c.y, 3.0f)) return false;
            if (out_.wall.size() >= 3 && (pointInPolygon(c, out_.wall) || distToRing(c, out_.wall) < 8.0f)) return false;
            if (out_.wall.size() < 3 && distC(c) < R_ * 0.55f) return false;
            const float h = height(c.x, c.y);
            lo = std::min(lo, h);
            hi = std::max(hi, h);
        }
        if (!dry(o.c.x, o.c.y, 3.0f) || hi - lo > 9.0f) return false;
        const Obb padded = fieldObb(f, 2.0f);
        for (const Obb& other : taken_) {
            if (overlap(padded, other)) return false;
        }
        for (const SettlementRoad& road : out_.roads) {
            for (std::size_t i = 0; i + 1 < road.points.size(); ++i) {
                if (segmentNearObb(xz(road.points[i]), xz(road.points[i + 1]), o, road.width * 0.5f + 1.0f)) return false;
            }
        }
        return true;
    }

    // Valla alrededor del campo con una entrada en el lado del camino.
    void fenceField(const SettlementField& f, const Vec2& road_side) {
        const Obb o = fieldObb(f, 0.3f);
        const auto corners = o.corners();
        // Lado que mira al camino: el de -Z local (la "fachada" del campo mira al camino).
        for (int e = 0; e < 4; ++e) {
            const Vec2 a = corners[static_cast<std::size_t>(e)];
            const Vec2 b = corners[static_cast<std::size_t>((e + 1) % 4)];
            const Vec2 mid = mul(add(a, b), 0.5f);
            const bool front = dot2(sub(mid, o.c), road_side) < -0.1f;
            const float len = len2(sub(b, a));
            const int pieces = std::max(1, static_cast<int>(std::ceil(len / 4.0f)));
            for (int k = 0; k < pieces; ++k) {
                const float t0 = static_cast<float>(k) / static_cast<float>(pieces);
                const float t1 = static_cast<float>(k + 1) / static_cast<float>(pieces);
                const Vec2 p0 = add(a, mul(sub(b, a), t0));
                const Vec2 p1 = add(a, mul(sub(b, a), t1));
                // Entrada de 4 m en el centro del lado del camino.
                if (front && len2(sub(mul(add(p0, p1), 0.5f), mid)) < 2.5f) continue;
                out_.fences.push_back({ground(p0), ground(p1)});
            }
        }
    }

    void windmill() {
        if (s_.type == SettlementType::Hamlet && !rng_.chance(0.35f)) return;
        float best = -1e30f;
        SettlementLot best_lot;
        bool found = false;
        for (int i = 0; i < 220; ++i) {
            const float a = rng_.range(0.0f, 2.0f * kPi);
            const float r = R_ * rng_.range(town_ ? 1.15f : 0.95f, town_ ? 1.6f : 1.5f);
            const Vec2 p{out_.center.x + std::cos(a) * r, out_.center.z + std::sin(a) * r};
            SettlementLot lot;
            lot.kind = LotKind::Windmill;
            foot_(lot.kind, 0, lot.half, lot.offset);
            lot.yaw = rng_.range(0.0f, 360.0f);
            lot.position = ground(p);
            const float score = lot.position.y - roughness(p, 8.0f) * 4.0f;
            if (score <= best) continue;
            if (!valid(lot, 2.0f, 3.5f, false, true, 1.0f)) continue;
            best = score;
            best_lot = lot;
            found = true;
        }
        if (found) commit(best_lot, 2.0f);
    }

    // Objetos: junto a la taberna, la herreria, los graneros, la plaza, por
    // las calles y en los campos.
    void props() {
        const auto tryProp = [&](MedievalBuilding what, const Vec2& p, float yaw) {
            SettlementLot lot;
            lot.kind = LotKind::Prop;
            lot.variant = static_cast<int>(what);
            foot_(lot.kind, lot.variant, lot.half, lot.offset);
            lot.yaw = yaw;
            lot.position = ground(p);
            if (!valid(lot, 0.15f, 1.5f, true, true, 0.3f) && !valid(lot, 0.15f, 1.5f, true, false, 0.3f)) return false;
            commit(lot, 0.15f);
            return true;
        };
        const std::vector<SettlementLot> buildings = out_.lots;
        for (const SettlementLot& b : buildings) {
            const Vec2 X = axisX(b.yaw);
            const Vec2 Z = axisZ(b.yaw);
            const Vec2 o = xz(b.position);
            const Vec2 side_r = add(o, add(mul(X, b.offset.x + b.half.x + 1.1f), mul(Z, b.offset.y + b.half.y - 1.2f)));
            const Vec2 side_l = add(o, add(mul(X, b.offset.x - b.half.x - 1.1f), mul(Z, b.offset.y + b.half.y - 1.2f)));
            switch (b.kind) {
                case LotKind::Tavern:
                    tryProp(MedievalBuilding::Barrels, side_r, b.yaw);
                    tryProp(MedievalBuilding::Cart, add(side_l, mul(Z, -1.5f)), b.yaw + 90.0f);
                    break;
                case LotKind::Smithy: tryProp(MedievalBuilding::Woodpile, side_r, b.yaw); break;
                case LotKind::Barn:
                    tryProp(MedievalBuilding::HayBales, side_r, b.yaw);
                    if (rng_.chance(0.5f)) tryProp(MedievalBuilding::Cart, side_l, b.yaw + rng_.range(-30.0f, 30.0f));
                    break;
                case LotKind::House:
                    if (rng_.chance(0.25f)) {
                        const MedievalBuilding what = rng_.chance(0.5f) ? MedievalBuilding::Barrels
                                                                        : (rng_.chance(0.5f) ? MedievalBuilding::Crates : MedievalBuilding::Woodpile);
                        tryProp(what, rng_.chance(0.5f) ? side_r : side_l, b.yaw);
                    }
                    break;
                case LotKind::Well:
                    for (int i = 0; i < 3; ++i) {
                        const float a = rng_.range(0.0f, 2.0f * kPi);
                        const Vec2 dir{std::cos(a), std::sin(a)};
                        tryProp(MedievalBuilding::Bench, add(o, mul(dir, 3.6f)), yawFacing(dir) + 180.0f);
                    }
                    break;
                case LotKind::MarketStall:
                    if (rng_.chance(0.5f)) tryProp(rng_.chance(0.5f) ? MedievalBuilding::Crates : MedievalBuilding::Barrels, side_r, b.yaw);
                    break;
                default: break;
            }
        }
        // Carros en las calles principales (al borde).
        for (const SettlementRoad& road : out_.roads) {
            if (!road.main || !rng_.chance(0.6f)) continue;
            const Path path(road.points);
            const float s = rng_.range(plaza_ + 10.0f, std::max(plaza_ + 12.0f, std::min(path.length, R_)));
            const Vec2 tg = path.tangent(s);
            const Vec2 n{-tg.y, tg.x};
            tryProp(MedievalBuilding::Cart, add(path.at(s), mul(n, road.width * 0.5f + 1.6f)), yawFacing(tg));
        }
        // Pacas en los campos.
        for (const SettlementField& f : out_.fields) {
            if (!rng_.chance(0.6f)) continue;
            const Vec2 p = add(xz(f.center), Vec2{rng_.range(-f.half.x, f.half.x) * 0.5f, rng_.range(-f.half.y, f.half.y) * 0.5f});
            SettlementLot lot;
            lot.kind = LotKind::Prop;
            lot.variant = static_cast<int>(MedievalBuilding::HayBales);
            foot_(lot.kind, lot.variant, lot.half, lot.offset);
            lot.yaw = f.yaw + rng_.range(-20.0f, 20.0f);
            lot.position = ground(p);
            out_.lots.push_back(lot);
        }
    }
};

}  // namespace

const char* settlementTypeName(SettlementType type) {
    switch (type) {
        case SettlementType::Hamlet: return "Aldea";
        case SettlementType::Village: return "Pueblo";
        case SettlementType::Town: return "Ciudad amurallada";
    }
    return "Pueblo";
}

SettlementLayout layoutSettlement(const SettlementSettings& settings, const SettlementTerrain& terrain,
                                  const SettlementFootprint& footprint, const Vec3& center, float search_radius) {
    SettlementFootprint safe = footprint;
    if (!safe) {
        safe = [](LotKind, int, Vec2& half, Vec2& offset) {
            half = Vec2{4.0f, 3.5f};
            offset = Vec2{};
        };
    }
    Planner planner(settings, terrain, safe);
    return planner.run(center, search_radius);
}

}  // namespace cramion::asset
