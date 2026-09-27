#include "CramionCore/foliage/Foliage.h"

#include "CramionCore/ecs/World.h"
#include "CramionCore/terrain/TerrainTools.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace cramion::foliage {

namespace {

// splitmix64: numero al azar estable por celda (el mismo bosque siempre).
std::uint64_t mix(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

struct Random {
    std::uint64_t state;
    float next() {
        state = mix(state);
        return static_cast<float>(state >> 40) / static_cast<float>(1ull << 24);
    }
};

}  // namespace

void Foliage::reflect(ecs::PropertyVisitor& v) {
    v.field({"area", "Area (m)", "Lado del cuadrado sembrado, centrado en el objeto"}, area,
            ecs::FloatRange{10.0f, 64000.0f, 10.0f, "%.0f m"});
    v.field({"density", "Densidad", "Arboles por hectarea (100 x 100 m). 60 = bosque; 300 = muy denso"}, density,
            ecs::FloatRange{0.1f, 2000.0f, 1.0f, "%.1f / ha"});
    v.field({"seed", "Semilla", "Otra semilla, otro bosque (la misma, el mismo)"}, seed, 0, 1000000);
    v.field({"pine", "Pinos", "Peso de los pinos en la mezcla"}, pine, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f"});
    v.field({"oak", "Robles", "Peso de los robles en la mezcla"}, oak, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f"});
    v.field({"birch", "Abedules", "Peso de los abedules en la mezcla"}, birch, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f"});
    v.field({"min_scale", "Escala minima"}, min_scale, ecs::FloatRange{0.25f, 4.0f, 0.01f, "%.2f"});
    v.field({"max_scale", "Escala maxima"}, max_scale, ecs::FloatRange{0.25f, 4.0f, 0.01f, "%.2f"});
    v.field({"on_terrain", "Sobre el terreno", "Apoyarlos en el terreno de debajo (si no, a la altura del objeto)"},
            on_terrain);
    v.field({"min_height", "Altura minima", "No por debajo de esta altura del mundo (el mar, la playa)"}, min_height,
            ecs::FloatRange{-10000.0f, 10000.0f, 0.1f, "%.1f m"});
    v.field({"max_height", "Altura maxima", "No por encima (cumbres sin arboles)"}, max_height,
            ecs::FloatRange{-10000.0f, 100000.0f, 1.0f, "%.0f m"});
    v.field({"max_slope", "Pendiente maxima"}, max_slope, ecs::FloatRange{0.0f, 90.0f, 0.5f, "%.0f°"});
    ecs::listField(v, {"clearings", "Claros", "Zonas sin arboles: poblados, caminos, el punto de partida"}, clearings,
                   [](FoliageClearing& c, ecs::PropertyVisitor& item) {
                       item.field({"center", "Centro", "En el mundo (solo cuentan x y z)"}, c.center, ecs::Vec3Kind::Position);
                       item.field({"radius", "Radio"}, c.radius, ecs::FloatRange{0.0f, 10000.0f, 0.5f, "%.1f m"});
                   });
    v.field({"max_instances", "Maximo de arboles", "Tope de seguridad (memoria de la GPU: ~40 bytes por arbol)"},
            max_instances, 1, static_cast<int>(gfx::FoliagePass::kMaxInstances));
    v.field({"lod1_distance", "Detalle medio desde", "Distancia (m) a la que pasan a la malla media"}, lod1_distance,
            ecs::FloatRange{5.0f, 5000.0f, 1.0f, "%.0f m"});
    v.field({"lod2_distance", "Detalle bajo desde", "Distancia (m) a la que pasan a la malla lejana"}, lod2_distance,
            ecs::FloatRange{10.0f, 20000.0f, 5.0f, "%.0f m"});
    v.field({"max_distance", "Distancia maxima", "Mas lejos no se dibujan"}, max_distance,
            ecs::FloatRange{50.0f, 50000.0f, 10.0f, "%.0f m"});
    v.field({"shadow_distance", "Sombras hasta", "Los arboles mas cerca que esto proyectan sombra"}, shadow_distance,
            ecs::FloatRange{0.0f, 2000.0f, 1.0f, "%.0f m"});
    v.field({"cast_shadows", "Proyectan sombra"}, cast_shadows);
    v.field({"wind", "Viento", "Cuanto se mecen las copas (0 = quietos)"}, wind, ecs::FloatRange{0.0f, 5.0f, 0.01f, "%.2f"});
}

gfx::FoliageSettings Foliage::settings() const {
    gfx::FoliageSettings s;
    s.lod1_distance = lod1_distance;
    s.lod2_distance = std::max(lod2_distance, lod1_distance);
    s.max_distance = max_distance;
    s.shadow_distance = shadow_distance;
    s.cast_shadows = cast_shadows;
    s.wind = wind;
    return s;
}

FoliageResult generateFoliage(const Foliage& f, const core::Vec3& center, const std::vector<FoliageGround>& ground) {
    FoliageResult result;
    const float area = std::max(f.area, 1.0f);
    const std::uint64_t limit = static_cast<std::uint64_t>(std::clamp(f.max_instances, 1, static_cast<int>(gfx::FoliagePass::kMaxInstances)));
    // Celda de un arbol: 10000 / densidad m2. Si salen demasiados, celdas mayores.
    float cell = std::sqrt(10000.0f / std::max(f.density, 0.001f));
    auto per_side = static_cast<std::uint64_t>(std::ceil(area / cell));
    if (per_side * per_side > limit) {
        per_side = static_cast<std::uint64_t>(std::floor(std::sqrt(static_cast<double>(limit))));
        cell = area / static_cast<float>(std::max<std::uint64_t>(per_side, 1));
    }
    per_side = std::max<std::uint64_t>(per_side, 1);
    result.cell = cell;
    result.candidates = per_side * per_side;

    const float weights_total = std::max(f.pine, 0.0f) + std::max(f.oak, 0.0f) + std::max(f.birch, 0.0f);
    const float w_pine = weights_total > 0.0f ? std::max(f.pine, 0.0f) / weights_total : 1.0f;
    const float w_oak = weights_total > 0.0f ? std::max(f.oak, 0.0f) / weights_total : 0.0f;
    const float min_scale = std::min(f.min_scale, f.max_scale);
    const float max_scale = std::max(f.min_scale, f.max_scale);
    const float min_normal_y = std::cos(std::clamp(f.max_slope, 0.0f, 90.0f) * 3.14159265f / 180.0f);
    const float x0 = center.x - area * 0.5f;
    const float z0 = center.z - area * 0.5f;

    const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    std::vector<std::vector<gfx::FoliageInstance>> parts(threads);
    std::vector<std::uint64_t> rejected(threads, 0);
    const auto work = [&](unsigned t) {
        std::vector<gfx::FoliageInstance>& out = parts[t];
        out.reserve(static_cast<std::size_t>(result.candidates / threads + 16));
        for (std::uint64_t row = t; row < per_side; row += threads) {
            for (std::uint64_t col = 0; col < per_side; ++col) {
                Random rng{mix(static_cast<std::uint64_t>(f.seed) * 0x100000001B3ull ^ (row << 32) ^ col)};
                const float x = x0 + (static_cast<float>(col) + rng.next()) * cell;
                const float z = z0 + (static_cast<float>(row) + rng.next()) * cell;
                bool cleared = false;
                for (const FoliageClearing& c : f.clearings) {
                    const float dx = x - c.center.x, dz = z - c.center.z;
                    if (dx * dx + dz * dz < c.radius * c.radius) {
                        cleared = true;
                        break;
                    }
                }
                if (cleared) {
                    ++rejected[t];
                    continue;
                }
                float y = center.y;
                bool placed = !f.on_terrain;
                if (f.on_terrain) {
                    for (const FoliageGround& g : ground) {
                        const float size = std::max(g.terrain.size, 1.0f);
                        if (!g.data || x < g.origin.x || z < g.origin.z || x > g.origin.x + size || z > g.origin.z + size) continue;
                        const core::Vec3 n = terrain::normalAt(*g.data, g.terrain, g.origin, x, z);
                        if (n.y < min_normal_y) break;  // demasiado empinado
                        y = terrain::heightAt(*g.data, g.terrain, g.origin, x, z);
                        placed = true;
                        break;
                    }
                    if (ground.empty()) placed = true;  // sin terreno: a la altura del objeto
                }
                if (!placed || y < f.min_height || y > f.max_height) {
                    ++rejected[t];
                    continue;
                }
                const float pick = rng.next();
                const std::uint32_t species = pick < w_pine ? 0u : (pick < w_pine + w_oak ? 1u : 2u);
                const float scale = min_scale + (max_scale - min_scale) * rng.next();
                const float yaw = rng.next() * 6.2831853f;
                // Hundido un poco: en pendiente no queda el pie en el aire.
                out.push_back(gfx::FoliageInstance::make(core::Vec3{x, y - 0.15f * scale, z}, yaw, scale, species, rng.next()));
            }
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work, t);
    work(0);
    for (std::thread& th : pool) th.join();
    std::size_t total = 0;
    for (const auto& p : parts) total += p.size();
    result.instances.reserve(total);
    for (auto& p : parts) result.instances.insert(result.instances.end(), p.begin(), p.end());
    for (const std::uint64_t r : rejected) result.rejected += r;
    return result;
}

void registerFoliageComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("Foliage") == nullptr) {
        registry.registerComponent<Foliage>("Foliage", "Vegetacion (bosque instanciado)", "Entorno");
    }
}

}  // namespace cramion::foliage
