// Lightmapper (PLAN-LIGHTMAPS.md 3.3): filtro a trous guiado en el espacio del
// atlas, solo para la luz rebotada (nunca para la visibilidad del sol ni para
// la directa de las luces locales). Nucleo B3 de 5x5 con pasos 1, 2, 4, 8:
//
//   w = h[dx] h[dy] * exp(-|p_x - p_y|^2 / (2 (lado_x * paso)^2))   misma superficie
//       * max(0, n_x . n_y)^32                                     misma orientacion
//       * exp(-|lum_x - lum_y| / (4 sqrt(var_x) + 1e-4))           ruido, no detalle
//       * (instancia_x == instancia_y)                             sin cruzar objetos

#include "LightmapperInternal.h"

#include <algorithm>
#include <cmath>

namespace cramion::lighting::lm {

namespace {

constexpr float kB3[5] = {1.0f / 16.0f, 1.0f / 4.0f, 3.0f / 8.0f, 1.0f / 4.0f, 1.0f / 16.0f};

float luminance(const Vec3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

}  // namespace

void denoisePage(const Context& ctx, Page& page, std::vector<Vec3>& irradiance, const std::vector<float>& variance,
                 int iterations) {
    const int w = static_cast<int>(page.width);
    const int h = static_cast<int>(page.height);
    if (w == 0 || h == 0 || irradiance.size() != page.state.size()) return;
    std::vector<float> var = variance;
    std::vector<Vec3> next(irradiance.size());
    std::vector<float> next_var(var.size());
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const int step = 1 << iteration;
        next = irradiance;
        next_var = var;
        constexpr int kRows = 16;
        const std::size_t jobs = static_cast<std::size_t>((h + kRows - 1) / kRows);
        parallelFor(ctx, jobs, [&](std::size_t job) {
            const int y_begin = static_cast<int>(job) * kRows;
            const int y_end = std::min(h, y_begin + kRows);
            for (int y = y_begin; y < y_end; ++y) {
                for (int x = 0; x < w; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
                    if (page.state[i] != 1) continue;
                    const std::uint32_t ti = page.texel_of[i];
                    if (ti == kNone) continue;
                    const Texel& tx = page.texels[ti];
                    const float lum_x = luminance(irradiance[i]);
                    const float sigma_l = 4.0f * std::sqrt(std::max(var[i], 0.0f)) + 1e-4f;
                    const float sigma_p = std::max(tx.side * static_cast<float>(step), 1e-5f);
                    const float inv_2p2 = 1.0f / (2.0f * sigma_p * sigma_p);
                    Vec3 sum{};
                    float weight_sum = 0.0f;
                    float var_sum = 0.0f;
                    for (int dy = -2; dy <= 2; ++dy) {
                        const int ny = y + dy * step;
                        if (ny < 0 || ny >= h) continue;
                        for (int dx = -2; dx <= 2; ++dx) {
                            const int nx = x + dx * step;
                            if (nx < 0 || nx >= w) continue;
                            const std::size_t j = static_cast<std::size_t>(ny) * static_cast<std::size_t>(w) + static_cast<std::size_t>(nx);
                            if (page.state[j] != 1) continue;
                            const std::uint32_t tj = page.texel_of[j];
                            if (tj == kNone) continue;
                            const Texel& ty = page.texels[tj];
                            if (ty.instance != tx.instance) continue;
                            const float n_dot = std::max(0.0f, core::dot(tx.normal, ty.normal));
                            if (n_dot <= 0.0f) continue;
                            const Vec3 dp = tx.center - ty.center;
                            const float w_pos = std::exp(-core::dot(dp, dp) * inv_2p2);
                            float w_normal = n_dot * n_dot;  // ^2
                            w_normal *= w_normal;            // ^4
                            w_normal *= w_normal;            // ^8
                            w_normal *= w_normal;            // ^16
                            w_normal *= w_normal;            // ^32
                            const float w_lum = std::exp(-std::fabs(lum_x - luminance(irradiance[j])) / sigma_l);
                            const float weight = kB3[dx + 2] * kB3[dy + 2] * w_pos * w_normal * w_lum;
                            sum += irradiance[j] * weight;
                            weight_sum += weight;
                            var_sum += weight * weight * var[j];
                        }
                    }
                    if (weight_sum > 1e-12f) {
                        next[i] = sum * (1.0f / weight_sum);
                        next_var[i] = var_sum / (weight_sum * weight_sum);
                    }
                }
            }
        });
        irradiance.swap(next);
        var.swap(next_var);
        if (ctx.cancelled()) return;
    }
}

}  // namespace cramion::lighting::lm
