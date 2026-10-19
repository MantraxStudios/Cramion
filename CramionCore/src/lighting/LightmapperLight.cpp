// Lightmapper (PLAN-LIGHTMAPS.md 2.2 G y Fase 3): visibilidad del sol (canal
// a), luz directa de las luces locales Horneadas y luz rebotada por
// recoleccion sobre el propio atlas (rgb).
//
// Unidades (1.5): E en el convenio del render (difuso = albedo x E; las
// intensidades ya llevan pi). La indirecta es la media de la radiancia que
// llega por rayos repartidos por coseno; el cielo, el del render (sus
// armonicos) sin el disco del sol.

#include "CramionCore/lighting/BakeLights.h"
#include "LightmapperInternal.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace cramion::lighting::lm {

namespace {

constexpr float kSunTangent = 0.00465f;  // radio angular del sol (lighting.frag kSunSize)
constexpr float kPi = 3.14159265358979f;
constexpr std::size_t kChunk = 256;

float luminance(const Vec3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

// Las submuestras validas de un texel, la representativa primero.
int validSamples(const Texel& t, int out[kSubSamples]) {
    int n = 0;
    if ((t.valid_mask >> t.representative) & 1u) out[n++] = t.representative;
    for (int k = 0; k < kSubSamples; ++k) {
        if (k != t.representative && ((t.valid_mask >> k) & 1u)) out[n++] = k;
    }
    return n;
}

// Visibilidad (0..1) de una luz local desde las submuestras validas: rayos a
// puntos de la bombilla (source_radius), con la fuerza de su sombra.
float localVisibility(const Context& ctx, const Texel& t, const int* samples, int n, const Vec3& light, float radius,
                      bool cast_shadows, float strength, int rays, std::uint32_t seed, std::uint64_t& count) {
    if (!cast_shadows || strength <= 0.0f || n == 0) return 1.0f;
    int lit = 0;
    for (int s = 0; s < rays; ++s) {
        const Vec3 p = t.sample(samples[s % n]);
        Vec3 target = light;
        if (radius > 0.0f && rays > 1) {
            // Punto uniforme en la esfera de la bombilla (determinista).
            const std::uint32_t h = hashCombine(seed, static_cast<std::uint32_t>(s));
            const float u1 = hashToUnit(h), u2 = hashToUnit(hash32(h)), u3 = hashToUnit(hash32(h + 0x9E3779B9u));
            const float z = 1.0f - 2.0f * u1;
            const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
            const float phi = 2.0f * kPi * u2;
            target = light + Vec3{r * std::cos(phi), z, r * std::sin(phi)} * (radius * std::cbrt(u3));
        }
        const Vec3 origin = offsetRay(p, t.geo_normal);
        Vec3 d = target - origin;
        const float dist = core::length(d);
        if (dist < 1e-4f) {
            ++lit;
            continue;
        }
        d = d * (1.0f / dist);
        ++count;
        if (!ctx.tracer.occluded(origin, d, dist - 1e-3f, hashCombine(seed, static_cast<std::uint32_t>(s) + 7u))) ++lit;
    }
    const float vis = static_cast<float>(lit) / static_cast<float>(std::max(rays, 1));
    return 1.0f - std::clamp(strength, 0.0f, 1.0f) * (1.0f - vis);
}

// Luz de las luces locales en un texel: `baked` = las Horneadas (con
// `samples` rayos), si no las Mixtas (4 rayos, solo para el rebote).
Vec3 localLights(const Context& ctx, const Texel& t, const int* samples, int n, bool baked, int rays, std::uint32_t seed,
                 std::uint64_t& count) {
    const BakeScene& scene = ctx.in->scene;
    if (n == 0) return Vec3{};
    const Vec3 p = t.sample(samples[0]);
    const Vec3& normal = t.normal;
    Vec3 e{};
    for (std::size_t i = 0; i < scene.lights.points.size(); ++i) {
        const bool is_baked = i < scene.point_baked.size() && scene.point_baked[i] != 0;
        if (is_baked != baked) continue;
        const scene::PointLight& l = scene.lights.points[i];
        Vec3 v = l.position - p;
        const float dist = core::length(v);
        if (dist < 1e-4f || dist > l.range) continue;
        v = v * (1.0f / dist);
        const float c = core::dot(normal, v);
        if (c <= 0.0f) continue;
        const float vis = localVisibility(ctx, t, samples, n, l.position, std::min(l.source_radius, dist * 0.5f), l.cast_shadows,
                                          l.shadow_strength, rays, hashCombine(seed, static_cast<std::uint32_t>(i) * 2u), count);
        e += srgbToLinear(l.color) * (l.intensity * lightAttenuation(dist, l.range) * c * vis);
    }
    for (std::size_t i = 0; i < scene.lights.spots.size(); ++i) {
        const bool is_baked = i < scene.spot_baked.size() && scene.spot_baked[i] != 0;
        if (is_baked != baked) continue;
        const scene::SpotLight& l = scene.lights.spots[i];
        Vec3 v = l.position - p;
        const float dist = core::length(v);
        if (dist < 1e-4f || dist > l.range) continue;
        v = v * (1.0f / dist);
        const float c = core::dot(normal, v);
        if (c <= 0.0f) continue;
        const float cone = spotCone(core::dot(v * -1.0f, core::normalize(l.direction)), std::cos(l.inner_angle),
                                    std::cos(l.outer_angle));
        if (cone <= 0.0f) continue;
        const float vis = localVisibility(ctx, t, samples, n, l.position, std::min(l.source_radius, dist * 0.5f), l.cast_shadows,
                                          l.shadow_strength, rays, hashCombine(seed, static_cast<std::uint32_t>(i) * 2u + 1u), count);
        e += srgbToLinear(l.color) * (l.intensity * lightAttenuation(dist, l.range) * c * cone * vis);
    }
    return e;
}

// Luz directa en un punto que no lleva lightmap (rebote sobre la escena): un
// rayo de sombra por luz (next event estimation).
Vec3 directAt(const Context& ctx, const Vec3& x, const Vec3& n, std::uint32_t seed, std::uint64_t& count) {
    const BakeScene& scene = ctx.in->scene;
    Vec3 e{};
    const Vec3 origin = offsetRay(x, n);
    if (ctx.sun_active) {
        const float c = core::dot(n, ctx.to_sun);
        if (c > 0.0f) {
            ++count;
            const float strength = scene.sun_shadow_strength;
            const float vis = strength > 0.0f && ctx.tracer.occluded(origin, ctx.to_sun, 1e30f, seed) ? 1.0f - strength : 1.0f;
            e += ctx.sun_radiance * (c * vis);
        }
    }
    const auto local = [&](const Vec3& pos, const Vec3& color, float intensity, float range, float cone_factor, bool shadows,
                           float strength, std::uint32_t salt) {
        Vec3 v = pos - x;
        const float dist = core::length(v);
        if (dist < 1e-4f || dist > range) return;
        v = v * (1.0f / dist);
        const float c = core::dot(n, v);
        if (c <= 0.0f || cone_factor <= 0.0f) return;
        float vis = 1.0f;
        if (shadows && strength > 0.0f) {
            ++count;
            if (ctx.tracer.occluded(origin, v, dist - 1e-3f, hashCombine(seed, salt))) vis = 1.0f - std::clamp(strength, 0.0f, 1.0f);
        }
        e += srgbToLinear(color) * (intensity * lightAttenuation(dist, range) * c * cone_factor * vis);
    };
    for (std::size_t i = 0; i < scene.lights.points.size(); ++i) {
        const scene::PointLight& l = scene.lights.points[i];
        local(l.position, l.color, l.intensity, l.range, 1.0f, l.cast_shadows, l.shadow_strength, static_cast<std::uint32_t>(i) * 2u + 11u);
    }
    for (std::size_t i = 0; i < scene.lights.spots.size(); ++i) {
        const scene::SpotLight& l = scene.lights.spots[i];
        const Vec3 to = core::normalize(x - l.position);
        const float cone = spotCone(core::dot(to, core::normalize(l.direction)), std::cos(l.inner_angle), std::cos(l.outer_angle));
        local(l.position, l.color, l.intensity, l.range, cone, l.cast_shadows, l.shadow_strength, static_cast<std::uint32_t>(i) * 2u + 12u);
    }
    return e;
}

Vec3 skyRadiance(const Context& ctx, const Vec3& d) {
    const BakeScene& scene = ctx.in->scene;
    const float k = scene.settings.sky_intensity;
    if (scene.has_sky_sh) return skyRadianceFromSh(scene.sky_sh, d) * k;
    // Sin el cielo del render (pruebas): el degradado de siempre.
    const float daylight = std::clamp(scene.lights.sky.daylight, 0.0f, 1.0f) * k;
    if (d.y >= 0.0f) {
        const float t = std::sqrt(std::clamp(d.y, 0.0f, 1.0f));
        return (scene.sky_horizon + (scene.sky_zenith - scene.sky_horizon) * t) * daylight;
    }
    return scene.sky_horizon * (0.25f * daylight);
}

// Bilineal de una imagen por pixel de la pagina (clamp a los bordes).
Vec3 bilinear(const std::vector<Vec3>& image, const Page& page, float px, float py) {
    const float fx = px - 0.5f, fy = py - 0.5f;
    const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
    const auto at = [&](int x, int y) {
        x = std::clamp(x, 0, static_cast<int>(page.width) - 1);
        y = std::clamp(y, 0, static_cast<int>(page.height) - 1);
        return image[static_cast<std::size_t>(y) * page.width + static_cast<std::size_t>(x)];
    };
    const Vec3 a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * tx;
    const Vec3 b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * tx;
    return a + (b - a) * ty;
}

// Dilata una imagen por pixel (vecinos 8-conexos de los texeles con dato).
void dilateImage(const Page& page, std::vector<Vec3>& image, int passes) {
    const int w = static_cast<int>(page.width), h = static_cast<int>(page.height);
    std::vector<std::uint8_t> has(page.state.size(), 0);
    for (std::size_t i = 0; i < has.size(); ++i) has[i] = page.state[i] == 1 ? 1 : 0;
    for (int pass = 0; pass < passes; ++pass) {
        const std::vector<std::uint8_t> before = has;
        const std::vector<Vec3> values = image;
        bool changed = false;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
                if (before[i]) continue;
                Vec3 sum{};
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                        const std::size_t j = static_cast<std::size_t>(ny) * static_cast<std::size_t>(w) + static_cast<std::size_t>(nx);
                        if (!before[j]) continue;
                        sum += values[j];
                        ++n;
                    }
                }
                if (n == 0) continue;
                image[i] = sum * (1.0f / static_cast<float>(n));
                has[i] = 1;
                changed = true;
            }
        }
        if (!changed) break;
    }
}

}  // namespace

bool bakeSunVisibility(Context& ctx) {
    const auto start = std::chrono::steady_clock::now();
    ctx.stage("Lightmaps: sombras del sol", 0.55f);
    if (!ctx.sun_active) {
        // Sin sol: nada que tapar (el canal a no se usa: sun_visibility = false).
        for (Page& page : ctx.pages) {
            for (std::size_t pixel = 0; pixel < page.state.size(); ++pixel) {
                if (page.state[pixel] == 1) page.value[pixel].w = 1.0f;
            }
        }
        return true;
    }
    const int samples = std::clamp(ctx.settings.sun_samples, 1, 4096);
    Vec3 tl, bl;
    orthonormalBasis(ctx.to_sun, tl, bl);
    for (std::uint32_t p = 0; p < ctx.pages.size(); ++p) {
        Page& page = ctx.pages[p];
        std::vector<float> visibility(page.texels.size(), 0.0f);
        const std::size_t chunks = (page.texels.size() + kChunk - 1) / kChunk;
        parallelFor(ctx, chunks, [&](std::size_t chunk) {
            std::uint64_t rays = 0;
            const std::size_t end = std::min(page.texels.size(), (chunk + 1) * kChunk);
            for (std::size_t i = chunk * kChunk; i < end; ++i) {
                const Texel& t = page.texels[i];
                int valid[kSubSamples];
                const int n = validSamples(t, valid);
                if (n == 0) continue;
                // Giro de Cranley-Patterson por texel: sin patrones entre vecinos.
                const std::uint32_t h = hash32(static_cast<std::uint32_t>(i) * 0x9E3779B9u + p * 0x85EBCA77u + 3u);
                const float rot_x = hashToUnit(h), rot_y = hashToUnit(hash32(h));
                int lit = 0;
                for (int j = 0; j < samples; ++j) {
                    float r1 = 0.0f, r2 = 0.0f;
                    r2Sample(static_cast<std::uint32_t>(j), rot_x, rot_y, r1, r2);
                    const float radius = kSunTangent * std::sqrt(r1);
                    const float phi = 2.0f * kPi * r2;
                    const Vec3 d = core::normalize(ctx.to_sun + tl * (radius * std::cos(phi)) + bl * (radius * std::sin(phi)));
                    const Vec3 origin = offsetRay(t.sample(valid[j % n]), t.geo_normal);
                    ++rays;
                    if (!ctx.tracer.occluded(origin, d, 1e30f,
                                             raySeed(p, static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j), 2u))) {
                        ++lit;
                    }
                }
                // Sin filtro: ya va supermuestreada (PLAN-LIGHTMAPS 4.3, punto 4).
                visibility[i] = static_cast<float>(lit) / static_cast<float>(samples);
            }
            ctx.rays.fetch_add(rays, std::memory_order_relaxed);
        });
        if (ctx.cancelled()) return false;
        for (std::size_t pixel = 0; pixel < page.texel_of.size(); ++pixel) {
            const std::uint32_t i = page.texel_of[pixel];
            if (i == kNone || page.state[pixel] != 1) continue;
            page.value[pixel].w = visibility[i];
        }
    }
    ctx.stats->seconds_sun = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return true;
}

bool bakeIrradiance(Context& ctx) {
    const BakeScene& scene = ctx.in->scene;
    const auto start = std::chrono::steady_clock::now();
    const int bounces = std::clamp(ctx.settings.bounces, 0, 8);
    const int indirect = std::clamp(ctx.settings.indirect_samples, 0, 8192);
    const int direct_rays = std::clamp(ctx.settings.direct_samples, 1, 256);
    const int dilate = static_cast<int>(ctx.settings.padding) + 2;
    bool any_baked_local = false;
    for (const std::uint8_t b : scene.point_baked) any_baked_local = any_baked_local || b != 0;
    for (const std::uint8_t b : scene.spot_baked) any_baked_local = any_baked_local || b != 0;
    if (indirect <= 0 && !any_baked_local) return true;  // solo la visibilidad del sol (Fase 2)
    ctx.stage("Lightmaps: luces horneadas", 0.6f);

    // Por pagina y por pixel: luz directa de las Horneadas (va al lightmap),
    // la que solo rebota (sol, Mixtas) y lo que emite cada texel.
    struct PageLight {
        std::vector<Vec3> baked;      // E_loc_b
        std::vector<Vec3> bounce;     // E_sol + E_loc_m
        std::vector<Vec3> albedo;     // de rebote
        std::vector<Vec3> emission;
        std::vector<Vec3> indirect;   // E_ind
        std::vector<float> variance;
        std::vector<Vec3> radiance;   // rad (dilatada)
    };
    std::vector<PageLight> light(ctx.pages.size());
    for (std::uint32_t p = 0; p < ctx.pages.size(); ++p) {
        Page& page = ctx.pages[p];
        PageLight& L = light[p];
        const std::size_t pixels = page.value.size();
        L.baked.assign(pixels, Vec3{});
        L.bounce.assign(pixels, Vec3{});
        L.albedo.assign(pixels, Vec3{});
        L.emission.assign(pixels, Vec3{});
        L.indirect.assign(pixels, Vec3{});
        L.variance.assign(pixels, 0.0f);
        const std::size_t chunks = (page.texel_of.size() + kChunk - 1) / kChunk;
        parallelFor(ctx, chunks, [&](std::size_t chunk) {
            std::uint64_t rays = 0;
            const std::size_t end = std::min(page.texel_of.size(), (chunk + 1) * kChunk);
            for (std::size_t pixel = chunk * kChunk; pixel < end; ++pixel) {
                const std::uint32_t i = page.texel_of[pixel];
                if (i == kNone || page.state[pixel] != 1) continue;
                const Texel& t = page.texels[i];
                const Instance& inst = ctx.instances[t.instance];
                const std::uint32_t tracer = inst.tracer_of_triangle[t.triangle];
                L.albedo[pixel] = tracer != kNone ? ctx.tracer_albedo[tracer] : Vec3{0.5f, 0.5f, 0.5f};
                L.emission[pixel] = tracer != kNone ? ctx.tracer_emission[tracer] : Vec3{};
                int valid[kSubSamples];
                const int n = validSamples(t, valid);
                const std::uint32_t seed = raySeed(p, static_cast<std::uint32_t>(pixel), 0u, 3u);
                L.baked[pixel] = localLights(ctx, t, valid, n, true, direct_rays, seed, rays);
                Vec3 bounce = localLights(ctx, t, valid, n, false, 4, hashCombine(seed, 1u), rays);
                if (ctx.sun_active) {
                    bounce += ctx.sun_radiance * (std::max(0.0f, core::dot(t.normal, ctx.to_sun)) * page.value[pixel].w);
                }
                L.bounce[pixel] = bounce;
            }
            ctx.rays.fetch_add(rays, std::memory_order_relaxed);
        });
        if (ctx.cancelled()) return false;
        L.radiance.assign(pixels, Vec3{});
        for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
            if (page.state[pixel] != 1) continue;
            L.radiance[pixel] = L.albedo[pixel] * (L.bounce[pixel] + L.baked[pixel]) + L.emission[pixel];
        }
        dilateImage(page, L.radiance, dilate);
    }

    // --- Recoleccion iterada en el atlas (3.2) ---
    if (indirect > 0 && bounces > 0) {
        const auto indirect_start = std::chrono::steady_clock::now();
        for (int k = 1; k <= bounces; ++k) {
            ctx.stage("Lightmaps: luz rebotada (" + std::to_string(k) + " de " + std::to_string(bounces) + ")",
                      0.65f + 0.2f * static_cast<float>(k - 1) / static_cast<float>(bounces));
            for (std::uint32_t p = 0; p < ctx.pages.size(); ++p) {
                Page& page = ctx.pages[p];
                PageLight& L = light[p];
                const std::size_t chunks = (page.texel_of.size() + kChunk - 1) / kChunk;
                parallelFor(ctx, chunks, [&](std::size_t chunk) {
                    std::uint64_t rays = 0;
                    const std::size_t end = std::min(page.texel_of.size(), (chunk + 1) * kChunk);
                    for (std::size_t pixel = chunk * kChunk; pixel < end; ++pixel) {
                        const std::uint32_t i = page.texel_of[pixel];
                        if (i == kNone || page.state[pixel] != 1) continue;
                        const Texel& t = page.texels[i];
                        int valid[kSubSamples];
                        const int n = validSamples(t, valid);
                        if (n == 0) continue;
                        Vec3 tn, bn;
                        orthonormalBasis(t.normal, tn, bn);
                        const std::uint32_t h = hashCombine(raySeed(p, static_cast<std::uint32_t>(pixel), 0u, 10u),
                                                            static_cast<std::uint32_t>(k));
                        const float rot_x = hashToUnit(h), rot_y = hashToUnit(hash32(h));
                        Vec3 sum{};
                        double sum2 = 0.0;
                        for (int j = 0; j < indirect; ++j) {
                            float r1 = 0.0f, r2 = 0.0f;
                            r2Sample(static_cast<std::uint32_t>(j), rot_x, rot_y, r1, r2);
                            const float r = std::sqrt(r1);
                            const float phi = 2.0f * kPi * r2;
                            Vec3 d = core::normalize(tn * (r * std::cos(phi)) + bn * (r * std::sin(phi)) +
                                                     t.normal * std::sqrt(std::max(0.0f, 1.0f - r1)));
                            // Por debajo de la cara real (normal suave inclinada): se refleja.
                            const float below = core::dot(d, t.geo_normal);
                            if (below <= 0.0f) d = core::normalize(d - t.geo_normal * (2.0f * below));
                            const Vec3 origin = offsetRay(t.sample(valid[j % n]), t.geo_normal);
                            const std::uint32_t seed = raySeed(p, static_cast<std::uint32_t>(pixel), static_cast<std::uint32_t>(j),
                                                               20u + static_cast<std::uint32_t>(k));
                            BakeHit hit;
                            Vec3 radiance{};
                            ++rays;
                            if (ctx.tracer.intersect(origin, d, 1e30f, hit, seed)) {
                                if (!hit.backface) {
                                    const std::uint32_t inst_index = ctx.tracer_instance[hit.triangle];
                                    bool from_atlas = false;
                                    if (inst_index != kNone) {
                                        const Instance& hi = ctx.instances[inst_index];
                                        const Mesh& mesh = ctx.meshes[hi.mesh];
                                        const std::uint32_t lt = ctx.tracer_local[hit.triangle];
                                        const Vec2& ua = mesh.uv[mesh.indices[static_cast<std::size_t>(lt) * 3]];
                                        const Vec2& ub = mesh.uv[mesh.indices[static_cast<std::size_t>(lt) * 3 + 1]];
                                        const Vec2& uc = mesh.uv[mesh.indices[static_cast<std::size_t>(lt) * 3 + 2]];
                                        if (ua.x >= 0.0f && ub.x >= 0.0f && uc.x >= 0.0f) {
                                            const float w0 = 1.0f - hit.u - hit.v;
                                            const float u = ua.x * w0 + ub.x * hit.u + uc.x * hit.v;
                                            const float v = ua.y * w0 + ub.y * hit.u + uc.y * hit.v;
                                            const float px = u * static_cast<float>(hi.w) + static_cast<float>(hi.x);
                                            const float py = v * static_cast<float>(hi.h) + static_cast<float>(hi.y);
                                            radiance = bilinear(light[hi.page].radiance, ctx.pages[hi.page], px, py);
                                            from_atlas = true;
                                        }
                                    }
                                    if (!from_atlas) {
                                        Vec3 nh = ctx.tracer.geometricNormal(hit.triangle);
                                        if (core::dot(nh, d) > 0.0f) nh = nh * -1.0f;
                                        const Vec3 x = ctx.tracer.point(hit);
                                        radiance = ctx.tracer_albedo[hit.triangle] * directAt(ctx, x, nh, hashCombine(seed, 5u), rays) +
                                                   ctx.tracer_emission[hit.triangle];
                                    }
                                }
                            } else {
                                radiance = skyRadiance(ctx, d);
                            }
                            sum += radiance;
                            const double l = luminance(radiance);
                            sum2 += l * l;
                        }
                        const Vec3 mean = sum * (1.0f / static_cast<float>(indirect));
                        const double mean_l = luminance(mean);
                        L.indirect[pixel] = mean;
                        L.variance[pixel] = static_cast<float>(std::max(0.0, (sum2 / indirect - mean_l * mean_l) / indirect));
                    }
                    ctx.rays.fetch_add(rays, std::memory_order_relaxed);
                });
                if (ctx.cancelled()) return false;
            }
            // Entre rebotes, el filtro suave; despues, la radiancia nueva.
            const bool last = k == bounces;
            for (std::uint32_t p = 0; p < ctx.pages.size(); ++p) {
                Page& page = ctx.pages[p];
                PageLight& L = light[p];
                if (ctx.settings.denoise) {
                    const auto denoise_start = std::chrono::steady_clock::now();
                    denoisePage(ctx, page, L.indirect, L.variance, last ? 4 : 2);
                    ctx.stats->seconds_denoise +=
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - denoise_start).count();
                }
                if (last) continue;
                for (std::size_t pixel = 0; pixel < page.state.size(); ++pixel) {
                    if (page.state[pixel] != 1) continue;
                    L.radiance[pixel] =
                        L.albedo[pixel] * (L.bounce[pixel] + L.baked[pixel] + L.indirect[pixel]) + L.emission[pixel];
                }
                dilateImage(page, L.radiance, dilate);
            }
        }
        ctx.stats->seconds_indirect = std::chrono::duration<double>(std::chrono::steady_clock::now() - indirect_start).count();
    }

    // --- Composicion de la pagina (3.4): rgb = E_ind filtrada + E_loc_b ---
    for (std::uint32_t p = 0; p < ctx.pages.size(); ++p) {
        Page& page = ctx.pages[p];
        const PageLight& L = light[p];
        for (std::size_t pixel = 0; pixel < page.state.size(); ++pixel) {
            if (page.state[pixel] != 1) continue;
            const Vec3 e = L.indirect[pixel] + L.baked[pixel];
            page.value[pixel].x = e.x;
            page.value[pixel].y = e.y;
            page.value[pixel].z = e.z;
        }
    }
    ctx.irradiance = true;
    (void)start;
    return true;
}

}  // namespace cramion::lighting::lm
