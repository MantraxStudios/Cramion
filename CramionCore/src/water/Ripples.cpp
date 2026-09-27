#include "CramionCore/water/Ripples.h"

#include <algorithm>
#include <cmath>

namespace cramion::water {

namespace {
constexpr float kStep = 1.0f / 60.0f;
constexpr int kMaxSteps = 4;  // un frame muy lento no dispara la simulacion
constexpr int kN = static_cast<int>(RippleSimulation::kSize);
// Borde que absorbe (sin rebotes en el limite de la rejilla), en celdas.
constexpr int kBorder = 12;
}  // namespace

void RippleSimulation::clear() {
    std::fill(current_.begin(), current_.end(), 0.0f);
    std::fill(previous_.begin(), previous_.end(), 0.0f);
    active_ = false;
    accumulator_ = 0.0f;
}

// Desplaza el contenido cuando la rejilla se mueve `dx`, `dz` celdas: lo que
// queda fuera se pierde y lo nuevo entra en calma.
void RippleSimulation::shift(int dx, int dz) {
    if (dx == 0 && dz == 0) return;
    if (std::abs(dx) >= kN || std::abs(dz) >= kN) {
        clear();
        return;
    }
    const auto move = [&](std::vector<float>& grid) {
        std::fill(next_.begin(), next_.end(), 0.0f);
        for (int z = 0; z < kN; ++z) {
            const int sz = z + dz;
            if (sz < 0 || sz >= kN) continue;
            for (int x = 0; x < kN; ++x) {
                const int sx = x + dx;
                if (sx < 0 || sx >= kN) continue;
                next_[static_cast<std::size_t>(z * kN + x)] = grid[static_cast<std::size_t>(sz * kN + sx)];
            }
        }
        grid.swap(next_);
    };
    move(current_);
    move(previous_);
}

// Celdas ocupadas por los obstaculos (se rehace cada frame: se pueden mover).
void RippleSimulation::rasterize(const std::vector<RippleObstacle>& obstacles) {
    if (any_solid_) std::fill(solid_.begin(), solid_.end(), std::uint8_t{0});
    any_solid_ = false;
    for (const RippleObstacle& o : obstacles) {
        const float reach = std::sqrt(o.half_u * o.half_u + o.half_v * o.half_v);
        const float gx = o.x / kCellSize - static_cast<float>(cell_x_);
        const float gz = o.z / kCellSize - static_cast<float>(cell_z_);
        const float r = reach / kCellSize + 1.0f;
        const int x0 = std::max(static_cast<int>(std::floor(gx - r)), 1);
        const int x1 = std::min(static_cast<int>(std::ceil(gx + r)), kN - 2);
        const int z0 = std::max(static_cast<int>(std::floor(gz - r)), 1);
        const int z1 = std::min(static_cast<int>(std::ceil(gz + r)), kN - 2);
        const float radius = std::max(o.half_u, o.half_v);
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                // Centro de la celda en el mundo, relativo al obstaculo.
                const float wx = (static_cast<float>(cell_x_ + x) + 0.5f) * kCellSize - o.x;
                const float wz = (static_cast<float>(cell_z_ + z) + 0.5f) * kCellSize - o.z;
                bool inside;
                if (o.round) {
                    inside = wx * wx + wz * wz <= radius * radius;
                } else {
                    const float u = wx * o.axis_x + wz * o.axis_z;
                    const float v = -wx * o.axis_z + wz * o.axis_x;
                    inside = std::abs(u) <= o.half_u && std::abs(v) <= o.half_v;
                }
                if (inside) {
                    solid_[static_cast<std::size_t>(z * kN + x)] = 1;
                    any_solid_ = true;
                }
            }
        }
    }
}

void RippleSimulation::update(const core::Vec3& center, float delta_seconds, const std::vector<RippleSource>& sources,
                              const std::vector<RippleObstacle>& obstacles) {
    // La esquina, en celdas enteras del mundo (la rejilla no se mueve a
    // medias: las ondas se quedan quietas en el mundo).
    const int cx = static_cast<int>(std::floor(center.x / kCellSize)) - kN / 2;
    const int cz = static_cast<int>(std::floor(center.z / kCellSize)) - kN / 2;
    if (!placed_) {
        cell_x_ = cx;
        cell_z_ = cz;
        placed_ = true;
    } else if (cx != cell_x_ || cz != cell_z_) {
        shift(cx - cell_x_, cz - cell_z_);
        cell_x_ = cx;
        cell_z_ = cz;
    }
    if (!active_ && sources.empty()) return;  // agua en calma: nada que hacer
    rasterize(obstacles);

    accumulator_ = std::min(accumulator_ + std::max(delta_seconds, 0.0f), kStep * kMaxSteps);
    while (accumulator_ >= kStep) {
        step(kStep, sources);
        accumulator_ -= kStep;
    }
}

void RippleSimulation::step(float dt, const std::vector<RippleSource>& sources) {
    // --- Empujes: una campana (gaussiana) de su radio ---
    for (const RippleSource& s : sources) {
        if (s.push == 0.0f) continue;
        const float gx = s.position.x / kCellSize - static_cast<float>(cell_x_);
        const float gz = s.position.z / kCellSize - static_cast<float>(cell_z_);
        const float r = std::max(s.radius / kCellSize, 1.0f);
        const int x0 = std::max(static_cast<int>(gx - r * 2.0f), 1);
        const int x1 = std::min(static_cast<int>(gx + r * 2.0f), kN - 2);
        const int z0 = std::max(static_cast<int>(gz - r * 2.0f), 1);
        const int z1 = std::min(static_cast<int>(gz + r * 2.0f), kN - 2);
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                const float d2 = ((static_cast<float>(x) - gx) * (static_cast<float>(x) - gx) +
                                  (static_cast<float>(z) - gz) * (static_cast<float>(z) - gz)) /
                                 (r * r);
                if (d2 > 4.0f) continue;
                current_[static_cast<std::size_t>(z * kN + x)] -= s.push * dt * std::exp(-d2 * 2.0f);
            }
        }
    }

    // --- Ecuacion de onda (Verlet): h' = 2h - h_prev + c2 * laplaciano ---
    const float c = wave_speed * dt / kCellSize;
    const float c2 = std::min(c * c, 0.45f);  // estable (CFL)
    float energy = 0.0f;
    for (int z = 1; z < kN - 1; ++z) {
        for (int x = 1; x < kN - 1; ++x) {
            const std::size_t i = static_cast<std::size_t>(z * kN + x);
            const float h = current_[i];
            const float lap = current_[i - 1] + current_[i + 1] + current_[i - kN] + current_[i + kN] - 4.0f * h;
            // El borde absorbe (las ondas se apagan al salir, no rebotan).
            const int edge = std::min(std::min(x, kN - 1 - x), std::min(z, kN - 1 - z));
            const float absorb = edge < kBorder ? 0.85f + 0.15f * static_cast<float>(edge) / kBorder : 1.0f;
            // Tope de altura: como mucho +-0.6 m (el agua no hace agujas).
            const float value = std::clamp((2.0f * h - previous_[i] + c2 * lap) * damping * absorb, -0.6f, 0.6f);
            next_[i] = value;
            energy = std::max(energy, std::abs(value));
        }
    }
    // Obstaculos: el agua no se mueve dentro, asi que las ondas rebotan en ellos.
    if (any_solid_) {
        for (std::size_t i = 0; i < next_.size(); ++i) {
            if (solid_[i]) next_[i] = 0.0f;
        }
    }
    // Los bordes (no se calculan) siempre en calma.
    for (int i = 0; i < kN; ++i) {
        next_[static_cast<std::size_t>(i)] = 0.0f;
        next_[static_cast<std::size_t>((kN - 1) * kN + i)] = 0.0f;
        next_[static_cast<std::size_t>(i * kN)] = 0.0f;
        next_[static_cast<std::size_t>(i * kN + kN - 1)] = 0.0f;
    }
    previous_.swap(current_);
    current_.swap(next_);
    // Por debajo de medio milimetro: agua en calma (no se simula ni se sube).
    active_ = energy > 0.0005f || !sources.empty();
    if (!active_) {
        std::fill(current_.begin(), current_.end(), 0.0f);
        std::fill(previous_.begin(), previous_.end(), 0.0f);
    }
}

float RippleSimulation::heightAt(float x, float z) const {
    const float gx = x / kCellSize - static_cast<float>(cell_x_);
    const float gz = z / kCellSize - static_cast<float>(cell_z_);
    if (gx < 0.0f || gz < 0.0f || gx >= static_cast<float>(kN - 1) || gz >= static_cast<float>(kN - 1)) return 0.0f;
    const int ix = static_cast<int>(gx);
    const int iz = static_cast<int>(gz);
    const float fx = gx - static_cast<float>(ix);
    const float fz = gz - static_cast<float>(iz);
    const auto at = [&](int xx, int zz) { return current_[static_cast<std::size_t>(zz * kN + xx)]; };
    const float a = at(ix, iz) + (at(ix + 1, iz) - at(ix, iz)) * fx;
    const float b = at(ix, iz + 1) + (at(ix + 1, iz + 1) - at(ix, iz + 1)) * fx;
    return a + (b - a) * fz;
}

}  // namespace cramion::water
