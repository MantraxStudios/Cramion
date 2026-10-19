#include "CramionCore/foliage/Foliage.h"

#include "CramionCore/ecs/World.h"
#include "CramionCore/jobs/JobSystem.h"
#include "CramionCore/terrain/TerrainTools.h"

#include <algorithm>
#include <cmath>

namespace cramion::foliage {

namespace {

// splitmix64: numero al azar estable por celda (el mismo bosque siempre).
std::uint64_t mix(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Altura 0..1 de un snapshot en uv (0..1), bilineal: lo mismo que
// TerrainData::sample pero sobre datos inmutables (seguro en otro hilo).
float sampleSnapshot(const terrain::TerrainSnapshot& s, float u, float v) {
    if (!s.heights || s.resolution < 2 || s.heights->size() < static_cast<std::size_t>(s.resolution) * s.resolution) {
        return 0.0f;
    }
    const std::vector<float>& h = *s.heights;
    const std::uint32_t res = s.resolution;
    const float n = static_cast<float>(res - 1);
    const float x = std::clamp(u, 0.0f, 1.0f) * n;
    const float y = std::clamp(v, 0.0f, 1.0f) * n;
    const auto x0 = static_cast<std::uint32_t>(std::min(std::floor(x), n - 1.0f));
    const auto y0 = static_cast<std::uint32_t>(std::min(std::floor(y), n - 1.0f));
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);
    const auto at = [&](std::uint32_t px, std::uint32_t py) { return h[static_cast<std::size_t>(py) * res + px]; };
    const float a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * tx;
    const float b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * tx;
    return a + (b - a) * ty;
}

// Altura en el mundo bajo (x, z): del snapshot si lo hay, si no de los datos.
float groundHeightAt(const FoliageGround& g, float x, float z) {
    if (g.snapshot) {
        const float size = std::max(g.terrain.size, 1.0f);
        return g.origin.y + sampleSnapshot(*g.snapshot, (x - g.origin.x) / size, (z - g.origin.z) / size) * g.terrain.height;
    }
    return terrain::heightAt(*g.data, g.terrain, g.origin, x, z);
}

core::Vec3 groundNormalAt(const FoliageGround& g, float x, float z) {
    if (!g.snapshot) return terrain::normalAt(*g.data, g.terrain, g.origin, x, z);
    const float e = g.terrain.size / static_cast<float>(std::max<std::uint32_t>(g.snapshot->resolution - 1, 1));
    const float hl = groundHeightAt(g, x - e, z);
    const float hr = groundHeightAt(g, x + e, z);
    const float hd = groundHeightAt(g, x, z - e);
    const float hu = groundHeightAt(g, x, z + e);
    return core::normalize(core::Vec3{hl - hr, 2.0f * e, hd - hu});
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
    v.field({"pine", "Peso especie 1", "Cuanto de la mezcla es la especie 1 (por defecto pinos)"}, pine,
            ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f"});
    v.field({"oak", "Peso especie 2", "Cuanto de la mezcla es la especie 2 (por defecto robles)"}, oak,
            ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f"});
    v.field({"birch", "Peso especie 3", "Cuanto de la mezcla es la especie 3 (por defecto abedules)"}, birch,
            ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f"});
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
    if (v.wantsAllFields() || v.beginGroup("Especies (arboles procedurales)", true)) {
        static constexpr const char* kKinds[] = {"Pino", "Abeto", "Roble", "Abedul", "Palmera", "Sauce"};
        v.enumeration({"species1", "Especie 1", "Tipo de arbol del peso 1"}, species1, kKinds);
        v.enumeration({"species2", "Especie 2", "Tipo de arbol del peso 2"}, species2, kKinds);
        v.enumeration({"species3", "Especie 3", "Tipo de arbol del peso 3"}, species3, kKinds);
        v.field({"tree_seed", "Semilla de los arboles", "Otra forma de ramas y hojas"}, tree_seed, 0, 100000);
        v.field({"tree_height", "Altura", "Multiplica la altura de cada especie"}, tree_height,
                ecs::FloatRange{0.3f, 3.0f, 0.01f, "%.2f"});
        v.field({"leaf_density", "Hojas", "Cuantas hojas (1 = normal)"}, leaf_density, ecs::FloatRange{0.1f, 3.0f, 0.01f, "%.2f"});
        v.field({"branch_density", "Ramas", "Cuantas ramas (1 = normal)"}, branch_density,
                ecs::FloatRange{0.2f, 2.5f, 0.01f, "%.2f"});
        v.field({"gnarl", "Torcido", "Ramas mas rectas (0) o mas retorcidas"}, gnarl, ecs::FloatRange{0.0f, 3.0f, 0.01f, "%.2f"});
        if (!v.wantsAllFields()) v.endGroup();
    }
}

std::array<asset::TreeSpecies, gfx::FoliagePass::kSpecies> Foliage::species() const {
    std::array<asset::TreeSpecies, gfx::FoliagePass::kSpecies> out{};
    const int kinds[3] = {species1, species2, species3};
    for (int i = 0; i < 3; ++i) {
        asset::TreeSpecies s = asset::treePreset(static_cast<asset::TreeKind>(std::clamp(kinds[i], 0, asset::kTreeKindCount - 1)));
        s.seed = static_cast<std::uint32_t>(std::max(tree_seed, 0)) * 31U + static_cast<std::uint32_t>(i) * 7U + 1U;
        s.height *= std::max(tree_height, 0.1f);
        s.leaf_density = leaf_density;
        s.branch_density = branch_density;
        s.gnarl = gnarl;
        out[static_cast<std::size_t>(i)] = s;
    }
    return out;
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
    // Agua de la escena (compartida entre bosques; nula = ninguna).
    static const std::vector<FoliageWater> kNoWater;
    const std::vector<FoliageWater>& waters = f.water != nullptr ? *f.water : kNoWater;

    // Filas repartidas en trozos fijos con el job system (no hilos propios):
    // el resultado sale en el mismo orden con cualquier numero de hilos.
    const std::size_t chunks = static_cast<std::size_t>(std::min<std::uint64_t>(per_side, 256));
    std::vector<std::vector<gfx::FoliageInstance>> parts(chunks);
    std::vector<std::uint64_t> rejected(chunks, 0);
    const auto work = [&](std::size_t t) {
        std::vector<gfx::FoliageInstance>& out = parts[t];
        const std::uint64_t row_begin = per_side * t / chunks;
        const std::uint64_t row_end = per_side * (t + 1) / chunks;
        out.reserve(static_cast<std::size_t>((row_end - row_begin) * per_side + 16));
        for (std::uint64_t row = row_begin; row < row_end; ++row) {
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
                        if ((!g.data && !g.snapshot) || x < g.origin.x || z < g.origin.z || x > g.origin.x + size ||
                            z > g.origin.z + size) {
                            continue;
                        }
                        const core::Vec3 n = groundNormalAt(g, x, z);
                        if (n.y < min_normal_y) break;  // demasiado empinado
                        y = groundHeightAt(g, x, z);
                        placed = true;
                        break;
                    }
                    if (ground.empty()) placed = true;  // sin terreno: a la altura del objeto
                }
                if (!placed || y < f.min_height || y > f.max_height) {
                    ++rejected[t];
                    continue;
                }
                // Ni en los rios ni bajo el agua de los lagos.
                bool wet = false;
                for (const FoliageWater& w : waters) {
                    if (w.river) {
                        const float abx = w.b.x - w.a.x, abz = w.b.z - w.a.z;
                        const float len2 = abx * abx + abz * abz;
                        const float tt = len2 > 0.0f ? std::clamp(((x - w.a.x) * abx + (z - w.a.z) * abz) / len2, 0.0f, 1.0f) : 0.0f;
                        const float dx = x - (w.a.x + abx * tt), dz = z - (w.a.z + abz * tt);
                        const float reach = w.width * 0.5f + 3.0f;  // el cauce y un poco de orilla
                        if (dx * dx + dz * dz < reach * reach) wet = true;
                    } else {
                        const float c = std::cos(w.angle), s = std::sin(w.angle);
                        const float lx = (x - w.a.x) * c + (z - w.a.z) * s;
                        const float lz = -(x - w.a.x) * s + (z - w.a.z) * c;
                        if (std::abs(lx) < w.half.x && std::abs(lz) < w.half.y && y < w.level + 0.6f) wet = true;
                    }
                    if (wet) break;
                }
                if (wet) {
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
    jobs::parallelFor(chunks, 1, [&](std::size_t begin, std::size_t end) {
        for (std::size_t t = begin; t < end; ++t) work(t);
    });
    std::size_t total = 0;
    for (const auto& p : parts) total += p.size();
    result.instances.reserve(total);
    for (auto& p : parts) result.instances.insert(result.instances.end(), p.begin(), p.end());
    for (const std::uint64_t r : rejected) result.rejected += r;
    return result;
}

void Grass::reflect(ecs::PropertyVisitor& v) {
    v.field({"layer", "Capa", "Capa del terreno donde crece (0 = la primera)"}, layer, 0, 7);
    v.field({"threshold", "Umbral", "Peso minimo de la capa: mas alto = solo donde esta bien pintada"}, threshold,
            ecs::FloatRange{0.0f, 0.99f, 0.01f, "%.2f"});
    v.field({"dry_layer", "Capa seca", "Donde el terreno tiene esta capa, la hierba se seca (-1 = ninguna)"}, dry_layer,
            -1, 7);
    v.field({"density", "Densidad", "Briznas por metro cuadrado (de cerca)"}, density,
            ecs::FloatRange{1.0f, 2000.0f, 1.0f, "%.0f / m2"});
    v.field({"max_distance", "Distancia", "Hasta donde se dibuja; lejos, menos briznas y mas anchas"}, max_distance,
            ecs::FloatRange{5.0f, 400.0f, 1.0f, "%.0f m"});
    v.field({"detail_distance", "Detalle hasta", "Briznas curvas y con todo el detalle hasta esta distancia"},
            detail_distance, ecs::FloatRange{1.0f, 200.0f, 0.5f, "%.0f m"});
    v.field({"height", "Altura"}, height, ecs::FloatRange{0.02f, 3.0f, 0.01f, "%.2f m"});
    v.field({"height_variation", "Variacion de altura"}, height_variation, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f"});
    v.field({"width", "Anchura", "Anchura de cada brizna en la base. Finas (0.005-0.01) y muchas = pelo"}, width,
            ecs::FloatRange{0.002f, 0.3f, 0.0005f, "%.4f m"});
    v.field({"bend", "Curvatura", "Cuanto se doblan solas"}, bend, ecs::FloatRange{0.0f, 1.5f, 0.01f, "%.2f"});
    v.field({"base_color", "Color base"}, base_color, ecs::Vec3Kind::Color);
    v.field({"tip_color", "Color de la punta"}, tip_color, ecs::Vec3Kind::Color);
    v.field({"dry_color", "Color seca"}, dry_color, ecs::Vec3Kind::Color);
    v.field({"color_variation", "Variacion de color",
             "Tono de cada mata, manchas del campo (frondosas y pobres), puntas secas y briznas de paja (0 = todas iguales)"},
            color_variation, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f"});
    v.field({"wind", "Viento", "Cuanto se mece (0 = quieta)"}, wind, ecs::FloatRange{0.0f, 4.0f, 0.01f, "%.2f"});
    v.field({"wind_direction", "Direccion del viento"}, wind_direction, ecs::FloatRange{0.0f, 360.0f, 1.0f, "%.0f°"});
    v.field({"interaction", "Interaccion", "Cuanto la apartan y aplastan los objetos fisicos y los personajes"},
            interaction, ecs::FloatRange{0.0f, 3.0f, 0.01f, "%.2f"});
    v.field({"max_blades", "Maximo de briznas", "Tope de memoria de la GPU (32 bytes por brizna)"}, max_blades, 10000,
            8000000);
}

gfx::GrassDesc Grass::desc() const {
    gfx::GrassDesc d;
    d.enabled = true;
    d.layer = layer;
    d.threshold = threshold;
    d.dry_layer = dry_layer;
    d.density = density;
    d.max_distance = max_distance;
    d.near_distance = detail_distance;
    d.height = height;
    d.height_variation = height_variation;
    d.width = width;
    // Escenas guardadas con los valores de fabrica de antes (0.028 m y 70 por
    // m2, briznas anchas: "de papel"): si nadie los toco, los de ahora (finas
    // y muchas: pelo). Lo que el usuario cambio se respeta.
    if (std::abs(width - 0.028f) < 1e-5f && std::abs(density - 70.0f) < 1e-3f) {
        d.width = 0.007f;
        d.density = 500.0f;
    }
    d.bend = bend;
    d.base_color = base_color;
    d.tip_color = tip_color;
    d.dry_color = dry_color;
    d.color_variation = color_variation;
    d.wind = wind;
    d.wind_direction = wind_direction;
    d.interaction = interaction;
    d.max_blades = static_cast<std::uint32_t>(std::max(max_blades, 10000));
    return d;
}

void registerFoliageComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("Foliage") == nullptr) {
        registry.registerComponent<Foliage>("Foliage", "Vegetacion (bosque instanciado)", "Entorno");
    }
    if (registry.find("Grass") == nullptr) {
        registry.registerComponent<Grass>("Grass", "Hierba (en un terreno)", "Entorno");
    }
}

}  // namespace cramion::foliage
