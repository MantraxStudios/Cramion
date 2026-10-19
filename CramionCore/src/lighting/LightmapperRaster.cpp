// Lightmapper (PLAN-LIGHTMAPS.md 2.2): raster de texeles (E), validez (F) y
// dilatacion (H).

#include "CramionCore/lighting/BakeLights.h"
#include "LightmapperInternal.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace cramion::lighting::lm {

namespace {

constexpr std::uint32_t kTile = 32;

Vec3 transformPoint(const core::Mat4& m, const Vec3& p) {
    const Vec4 r = m * Vec4{p.x, p.y, p.z, 1.0f};
    return Vec3{r.x, r.y, r.z};
}

// Normales al mundo con la matriz de cofactores (como skinned.vert): vale con
// escala no uniforme.
Vec3 transformNormal(const core::Mat4& m, const Vec3& n) {
    const Vec3 c0{m.m[0][0], m.m[0][1], m.m[0][2]};
    const Vec3 c1{m.m[1][0], m.m[1][1], m.m[1][2]};
    const Vec3 c2{m.m[2][0], m.m[2][1], m.m[2][2]};
    const Vec3 a = core::cross(c1, c2);
    const Vec3 b = core::cross(c2, c0);
    const Vec3 c = core::cross(c0, c1);
    return a * n.x + b * n.y + c * n.z;
}

struct Pt {
    float x, y;
};

// Baricentricas (w1, w2) de q en el triangulo A, B, C (w0 = 1 - w1 - w2).
// Fuera del triangulo tambien (extrapolado en su plano).
struct Barycentric {
    Pt a;
    float ex, ey, fx, fy;  // B - A, C - A
    float inv_det;
    bool valid;
    Barycentric(Pt A, Pt B, Pt C) : a(A), ex(B.x - A.x), ey(B.y - A.y), fx(C.x - A.x), fy(C.y - A.y) {
        const float det = ex * fy - ey * fx;
        valid = std::fabs(det) > 1e-12f;
        inv_det = valid ? 1.0f / det : 0.0f;
    }
    void at(float qx, float qy, float& w1, float& w2) const {
        const float dx = qx - a.x;
        const float dy = qy - a.y;
        w1 = (fy * dx - fx * dy) * inv_det;
        w2 = (-ey * dx + ex * dy) * inv_det;
    }
};

// Punto del triangulo (ABC, en pixeles) mas cercano a q.
Pt closestPoint(Pt q, Pt a, Pt b, Pt c) {
    const auto on_segment = [](Pt q, Pt p0, Pt p1) {
        const float dx = p1.x - p0.x;
        const float dy = p1.y - p0.y;
        const float len = dx * dx + dy * dy;
        float t = len > 0.0f ? ((q.x - p0.x) * dx + (q.y - p0.y) * dy) / len : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        return Pt{p0.x + dx * t, p0.y + dy * t};
    };
    const Pt candidates[3] = {on_segment(q, a, b), on_segment(q, b, c), on_segment(q, c, a)};
    Pt best = candidates[0];
    float best_d = 1e30f;
    for (const Pt& p : candidates) {
        const float d = (p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y);
        if (d < best_d) {
            best_d = d;
            best = p;
        }
    }
    return best;
}

// El triangulo toca el cuadrado [x, x+1] x [y, y+1]? (ejes separadores: los
// del cuadrado por la caja y las normales de las aristas).
bool touchesSquare(Pt a, Pt b, Pt c, float x, float y) {
    if (std::max({a.x, b.x, c.x}) < x || std::min({a.x, b.x, c.x}) > x + 1.0f) return false;
    if (std::max({a.y, b.y, c.y}) < y || std::min({a.y, b.y, c.y}) > y + 1.0f) return false;
    const float orient = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    const Pt corners[4] = {{x, y}, {x + 1.0f, y}, {x, y + 1.0f}, {x + 1.0f, y + 1.0f}};
    const Pt edges[3][2] = {{a, b}, {b, c}, {c, a}};
    for (const auto& e : edges) {
        bool all_outside = true;
        for (const Pt& q : corners) {
            const float side = (e[1].x - e[0].x) * (q.y - e[0].y) - (e[1].y - e[0].y) * (q.x - e[0].x);
            if (side * orient >= 0.0f) {
                all_outside = false;
                break;
            }
        }
        if (all_outside) return false;
    }
    return true;
}

struct Candidate {
    float coverage = 0.0f;
    std::uint32_t instance = kNone, triangle = kNone;
    float w1 = 0.0f, w2 = 0.0f;  // baricentricas medias (dentro del triangulo)
    std::uint16_t mask = 0;
};

}  // namespace

bool rasterizeTexels(Context& ctx) {
    const auto start = std::chrono::steady_clock::now();
    ctx.stage("Lightmaps: texeles", 0.45f);
    for (std::uint32_t p = 0; p < ctx.pages.size(); ++p) {
        Page& page = ctx.pages[p];
        const std::uint32_t tiles_x = (page.width + kTile - 1) / kTile;
        const std::uint32_t tiles_y = (page.height + kTile - 1) / kTile;
        // Que triangulos tocan cada tile (en orden: el raster es determinista).
        std::vector<std::vector<std::uint64_t>> bins(static_cast<std::size_t>(tiles_x) * tiles_y);
        for (std::uint32_t i = 0; i < ctx.instances.size(); ++i) {
            const Instance& inst = ctx.instances[i];
            if (inst.page != p) continue;
            const Mesh& mesh = ctx.meshes[inst.mesh];
            const float sw = static_cast<float>(inst.w), sh = static_cast<float>(inst.h);
            for (std::size_t t = 0; t * 3 + 2 < mesh.indices.size(); ++t) {
                if (inst.tracer_of_triangle[t] == kNone) continue;  // transparente
                const Vec2& ua = mesh.uv[mesh.indices[t * 3]];
                const Vec2& ub = mesh.uv[mesh.indices[t * 3 + 1]];
                const Vec2& uc = mesh.uv[mesh.indices[t * 3 + 2]];
                if (ua.x < 0.0f || ub.x < 0.0f || uc.x < 0.0f) continue;  // sin lightmap (sondas)
                const float min_x = std::min({ua.x, ub.x, uc.x}) * sw + static_cast<float>(inst.x);
                const float max_x = std::max({ua.x, ub.x, uc.x}) * sw + static_cast<float>(inst.x);
                const float min_y = std::min({ua.y, ub.y, uc.y}) * sh + static_cast<float>(inst.y);
                const float max_y = std::max({ua.y, ub.y, uc.y}) * sh + static_cast<float>(inst.y);
                const int x0 = std::max(static_cast<int>(std::floor(min_x)) - 1, static_cast<int>(inst.x) - 1);
                const int x1 = std::min(static_cast<int>(std::floor(max_x)) + 1, static_cast<int>(inst.x + inst.w));
                const int y0 = std::max(static_cast<int>(std::floor(min_y)) - 1, static_cast<int>(inst.y) - 1);
                const int y1 = std::min(static_cast<int>(std::floor(max_y)) + 1, static_cast<int>(inst.y + inst.h));
                const int tx0 = std::max(x0, 0) / static_cast<int>(kTile);
                const int tx1 = std::min(x1, static_cast<int>(page.width) - 1) / static_cast<int>(kTile);
                const int ty0 = std::max(y0, 0) / static_cast<int>(kTile);
                const int ty1 = std::min(y1, static_cast<int>(page.height) - 1) / static_cast<int>(kTile);
                for (int ty = ty0; ty <= ty1; ++ty) {
                    for (int tx = tx0; tx <= tx1; ++tx) {
                        bins[static_cast<std::size_t>(ty) * tiles_x + static_cast<std::size_t>(tx)].push_back(
                            (static_cast<std::uint64_t>(i) << 32) | static_cast<std::uint64_t>(t));
                    }
                }
            }
        }
        // Cada tile por su cuenta (sus texeles son solo suyos).
        std::vector<std::vector<Texel>> tile_texels(bins.size());
        std::vector<std::vector<std::uint32_t>> tile_pixels(bins.size());
        parallelFor(ctx, bins.size(), [&](std::size_t tile) {
            const std::vector<std::uint64_t>& bin = bins[tile];
            if (bin.empty()) return;
            const std::uint32_t ox = static_cast<std::uint32_t>(tile % tiles_x) * kTile;
            const std::uint32_t oy = static_cast<std::uint32_t>(tile / tiles_x) * kTile;
            const std::uint32_t tw = std::min(kTile, page.width - ox);
            const std::uint32_t th = std::min(kTile, page.height - oy);
            std::vector<Candidate> best(static_cast<std::size_t>(tw) * th);
            for (const std::uint64_t entry : bin) {
                const auto i = static_cast<std::uint32_t>(entry >> 32);
                const auto t = static_cast<std::uint32_t>(entry & 0xFFFFFFFFu);
                const Instance& inst = ctx.instances[i];
                const Mesh& mesh = ctx.meshes[inst.mesh];
                const float sw = static_cast<float>(inst.w), sh = static_cast<float>(inst.h);
                const auto px = [&](std::uint32_t corner) {
                    const Vec2& uv = mesh.uv[mesh.indices[static_cast<std::size_t>(t) * 3 + corner]];
                    return Pt{uv.x * sw + static_cast<float>(inst.x), uv.y * sh + static_cast<float>(inst.y)};
                };
                const Pt a = px(0), b = px(1), c = px(2);
                const Barycentric bary(a, b, c);
                if (!bary.valid) continue;
                const int x0 = std::max({static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))) - 1, static_cast<int>(ox),
                                         static_cast<int>(inst.x) - 1});
                const int x1 = std::min({static_cast<int>(std::floor(std::max({a.x, b.x, c.x}))) + 1,
                                         static_cast<int>(ox + tw) - 1, static_cast<int>(inst.x + inst.w)});
                const int y0 = std::max({static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))) - 1, static_cast<int>(oy),
                                         static_cast<int>(inst.y) - 1});
                const int y1 = std::min({static_cast<int>(std::floor(std::max({a.y, b.y, c.y}))) + 1,
                                         static_cast<int>(oy + th) - 1, static_cast<int>(inst.y + inst.h)});
                for (int y = y0; y <= y1; ++y) {
                    for (int x = x0; x <= x1; ++x) {
                        int count = 0;
                        float sum1 = 0.0f, sum2 = 0.0f;
                        std::uint16_t mask = 0;
                        for (int k = 0; k < kSubSamples; ++k) {
                            const float qx = static_cast<float>(x) + (static_cast<float>(k & 3) + 0.5f) * 0.25f;
                            const float qy = static_cast<float>(y) + (static_cast<float>(k >> 2) + 0.5f) * 0.25f;
                            float w1 = 0.0f, w2 = 0.0f;
                            bary.at(qx, qy, w1, w2);
                            if (w1 >= 0.0f && w2 >= 0.0f && w1 + w2 <= 1.0f) {
                                ++count;
                                sum1 += w1;
                                sum2 += w2;
                                mask = static_cast<std::uint16_t>(mask | (1u << k));
                            }
                        }
                        float coverage = static_cast<float>(count);
                        float w1 = count > 0 ? sum1 / static_cast<float>(count) : 0.0f;
                        float w2 = count > 0 ? sum2 / static_cast<float>(count) : 0.0f;
                        if (count == 0) {
                            if (!touchesSquare(a, b, c, static_cast<float>(x), static_cast<float>(y))) continue;
                            // Lo toca sin cubrir ninguna submuestra (borde fino de
                            // una carta): el punto del triangulo mas cercano.
                            coverage = 0.5f;
                            const Pt q = closestPoint(Pt{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f}, a, b, c);
                            bary.at(q.x, q.y, w1, w2);
                        }
                        Candidate& cand = best[static_cast<std::size_t>(y - static_cast<int>(oy)) * tw +
                                               static_cast<std::size_t>(x - static_cast<int>(ox))];
                        if (coverage > cand.coverage) {
                            cand.coverage = coverage;
                            cand.instance = i;
                            cand.triangle = t;
                            cand.w1 = w1;
                            cand.w2 = w2;
                            cand.mask = mask;
                        }
                    }
                }
            }
            std::vector<Texel>& out = tile_texels[tile];
            std::vector<std::uint32_t>& pixels = tile_pixels[tile];
            for (std::uint32_t ly = 0; ly < th; ++ly) {
                for (std::uint32_t lx = 0; lx < tw; ++lx) {
                    const Candidate& cand = best[static_cast<std::size_t>(ly) * tw + lx];
                    if (cand.instance == kNone) continue;
                    const Instance& inst = ctx.instances[cand.instance];
                    const Mesh& mesh = ctx.meshes[inst.mesh];
                    const std::size_t base = static_cast<std::size_t>(cand.triangle) * 3;
                    const std::uint32_t ia = mesh.indices[base], ib = mesh.indices[base + 1], ic = mesh.indices[base + 2];
                    const Vec3 pa = transformPoint(inst.world, mesh.positions[ia]);
                    const Vec3 pb = transformPoint(inst.world, mesh.positions[ib]);
                    const Vec3 pc = transformPoint(inst.world, mesh.positions[ic]);
                    const float sw = static_cast<float>(inst.w), sh = static_cast<float>(inst.h);
                    const Pt a{mesh.uv[ia].x * sw + static_cast<float>(inst.x), mesh.uv[ia].y * sh + static_cast<float>(inst.y)};
                    const Pt b{mesh.uv[ib].x * sw + static_cast<float>(inst.x), mesh.uv[ib].y * sh + static_cast<float>(inst.y)};
                    const Pt c{mesh.uv[ic].x * sw + static_cast<float>(inst.x), mesh.uv[ic].y * sh + static_cast<float>(inst.y)};
                    const Barycentric bary(a, b, c);
                    const std::uint32_t x = ox + lx, y = oy + ly;
                    Texel texel;
                    texel.instance = cand.instance;
                    texel.triangle = cand.triangle;
                    // Derivadas del mundo respecto a la pagina (Jacobiano del triangulo).
                    const Vec3 db = pb - pa, dc = pc - pa;
                    texel.du = db * (bary.fy * bary.inv_det) + dc * (-bary.ey * bary.inv_det);
                    texel.dv = db * (-bary.fx * bary.inv_det) + dc * (bary.ex * bary.inv_det);
                    float cw1 = 0.0f, cw2 = 0.0f;
                    bary.at(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, cw1, cw2);
                    texel.center = pa + db * cw1 + dc * cw2;
                    const Vec3 na = transformNormal(inst.world, mesh.normals[ia]);
                    const Vec3 nb = transformNormal(inst.world, mesh.normals[ib]);
                    const Vec3 nc = transformNormal(inst.world, mesh.normals[ic]);
                    Vec3 geo = core::normalize(core::cross(db, dc));
                    Vec3 smooth = core::normalize(na * (1.0f - cand.w1 - cand.w2) + nb * cand.w1 + nc * cand.w2);
                    if (core::dot(smooth, smooth) < 0.5f) smooth = geo;
                    if (core::dot(geo, smooth) < 0.0f) geo = geo * -1.0f;
                    texel.normal = smooth;
                    texel.geo_normal = geo;
                    const float area_world = 0.5f * core::length(core::cross(db, dc));
                    const float area_px = 0.5f * std::fabs(bary.ex * bary.fy - bary.ey * bary.fx);
                    texel.side = area_px > 0.0f ? std::sqrt(area_world / area_px) : 0.0f;
                    texel.valid_mask = 0xFFFFu;
                    out.push_back(texel);
                    pixels.push_back(y * page.width + x);
                }
            }
        });
        if (ctx.cancelled()) return false;
        // Todos los texeles de la pagina, en el orden de los tiles.
        for (std::size_t tile = 0; tile < bins.size(); ++tile) {
            for (std::size_t k = 0; k < tile_texels[tile].size(); ++k) {
                const std::uint32_t pixel = tile_pixels[tile][k];
                page.texel_of[pixel] = static_cast<std::uint32_t>(page.texels.size());
                page.texels.push_back(tile_texels[tile][k]);
                page.state[pixel] = 1;
            }
        }
    }
    ctx.stats->seconds_raster = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return true;
}

bool computeValidity(Context& ctx) {
    const auto start = std::chrono::steady_clock::now();
    ctx.stage("Lightmaps: validez de los texeles", 0.5f);
    constexpr std::size_t kChunk = 256;
    for (std::uint32_t p = 0; p < ctx.pages.size(); ++p) {
        Page& page = ctx.pages[p];
        const std::size_t chunks = (page.texels.size() + kChunk - 1) / kChunk;
        parallelFor(ctx, chunks, [&](std::size_t chunk) {
            std::uint64_t rays = 0;
            const std::size_t end = std::min(page.texels.size(), (chunk + 1) * kChunk);
            for (std::size_t i = chunk * kChunk; i < end; ++i) {
                Texel& t = page.texels[i];
                const Vec3 tangent = core::normalize(t.du);
                const Vec3 bitangent = core::normalize(t.dv);
                const Vec3 dirs[4] = {tangent, tangent * -1.0f, bitangent, bitangent * -1.0f};
                const float range = 1.5f * t.side;
                std::uint16_t mask = 0;
                for (int k = 0; k < kSubSamples; ++k) {
                    const Vec3 origin = offsetRay(t.sample(k), t.geo_normal);
                    bool inside = false;
                    for (int d = 0; d < 4 && !inside; ++d) {
                        if (core::dot(dirs[d], dirs[d]) < 0.5f || range <= 0.0f) continue;
                        BakeHit hit;
                        ++rays;
                        if (ctx.tracer.intersect(origin, dirs[d], range, hit,
                                                 raySeed(p, static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(k * 4 + d), 1u)) &&
                            hit.backface) {
                            inside = true;
                        }
                    }
                    if (!inside) mask = static_cast<std::uint16_t>(mask | (1u << k));
                }
                t.valid_mask = mask;
                // La valida mas cercana al centro (las cuatro centrales primero).
                static constexpr int kByDistance[kSubSamples] = {5, 6, 9, 10, 1, 2, 4, 7, 8, 11, 13, 14, 0, 3, 12, 15};
                t.representative = 0;
                for (const int k : kByDistance) {
                    if ((mask >> k) & 1u) {
                        t.representative = static_cast<std::uint8_t>(k);
                        break;
                    }
                }
            }
            ctx.rays.fetch_add(rays, std::memory_order_relaxed);
        });
        if (ctx.cancelled()) return false;
        // Los que no tienen ninguna submuestra valida: dentro de la geometria.
        for (std::size_t pixel = 0; pixel < page.texel_of.size(); ++pixel) {
            const std::uint32_t i = page.texel_of[pixel];
            if (i == kNone) continue;
            if (page.texels[i].valid_mask == 0) {
                page.state[pixel] = 2;
                page.value[pixel] = Vec4{0.0f, 0.0f, 0.0f, 0.0f};
            }
        }
    }
    ctx.stats->seconds_validity = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return true;
}

void dilatePages(Context& ctx, int passes) {
    parallelFor(ctx, ctx.pages.size(), [&](std::size_t p) {
        Page& page = ctx.pages[p];
        const int w = static_cast<int>(page.width);
        const int h = static_cast<int>(page.height);
        std::vector<std::uint8_t> state = page.state;
        std::vector<Vec4> value = page.value;
        for (int pass = 0; pass < passes; ++pass) {
            bool changed = false;
            const std::vector<std::uint8_t> before_state = state;
            const std::vector<Vec4> before_value = value;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
                    if (before_state[i] == 1 || before_state[i] == 3) continue;
                    Vec4 sum{};
                    int n = 0;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (dx == 0 && dy == 0) continue;
                            const int nx = x + dx, ny = y + dy;
                            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                            const std::size_t j = static_cast<std::size_t>(ny) * static_cast<std::size_t>(w) + static_cast<std::size_t>(nx);
                            if (before_state[j] != 1 && before_state[j] != 3) continue;
                            const Vec4& v = before_value[j];
                            sum = Vec4{sum.x + v.x, sum.y + v.y, sum.z + v.z, sum.w + v.w};
                            ++n;
                        }
                    }
                    if (n == 0) continue;
                    const float inv = 1.0f / static_cast<float>(n);
                    value[i] = Vec4{sum.x * inv, sum.y * inv, sum.z * inv, sum.w * inv};
                    state[i] = 3;
                    changed = true;
                }
            }
            if (!changed) break;
        }
        page.state = std::move(state);
        page.value = std::move(value);
    });
}

}  // namespace cramion::lighting::lm
