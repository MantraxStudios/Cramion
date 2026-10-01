#include "CramionCore/fire/Fire.h"

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/terrain/Terrain.h"
#include "CramionCore/terrain/TerrainTools.h"
#include "CramionCore/water/Water.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace cramion::fire {

using core::Vec2;
using core::Vec3;

namespace {

constexpr float kStep = 0.1f;        // s por paso de la simulacion (10 Hz)
constexpr int kMaxStepsPerFrame = 5; // si el frame tarda mucho, no se pone al dia de golpe
constexpr int kMaxResolution = static_cast<int>(gfx::kFireMapSize);
constexpr float kFlaming = 0.3f;     // calor desde el que una celda tiene llamas (y contagia)
constexpr float kSmolderHeat = 0.28f;

std::uint64_t mix(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

float random01(std::uint64_t seed, std::uint64_t tick, std::uint64_t cell, std::uint64_t stream) {
    const std::uint64_t h = mix(seed * 0x100000001B3ull ^ (tick << 24) ^ cell ^ (stream << 58));
    return static_cast<float>(h >> 40) / static_cast<float>(1ull << 24);
}

// Cuanto arde una capa del terreno, por su nombre (las del generador y las
// de partida: Hierba, Hierba seca, Tierra, Roca, Arena, Grava, Nieve, Barro).
float layerFlammability(const std::string& name) {
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto has = [&](const char* word) { return n.find(word) != std::string::npos; };
    if (has("seca") || has("dry") || has("paja") || has("straw")) return 1.0f;
    if (has("hoja") || has("leaf") || has("leaves") || has("bosque") || has("forest") || has("matorral") ||
        has("bush") || has("shrub")) {
        return 0.9f;
    }
    if (has("hierba") || has("grass") || has("cesped") || has("pasto") || has("prado") || has("meadow")) return 0.8f;
    if (has("roca") || has("rock") || has("piedra") || has("stone") || has("arena") || has("sand") || has("grava") ||
        has("gravel") || has("nieve") || has("snow") || has("hielo") || has("ice") || has("agua") || has("water") ||
        has("asfalto") || has("asphalt") || has("camino") || has("road") || has("cemento") || has("concrete")) {
        return 0.0f;
    }
    if (has("barro") || has("mud")) return 0.03f;
    if (has("tierra") || has("dirt") || has("soil") || has("suelo")) return 0.2f;
    return 0.4f;
}

struct GroundSource {
    std::shared_ptr<terrain::TerrainData> data;
    terrain::Terrain terrain;
    Vec3 origin{};  // esquina (x, z minimas), mundo
    std::vector<float> flammability;
};

struct WaterSource {
    water::WaterType type = water::WaterType::Lake;
    Vec3 a{};
    Vec3 b{};
    float width = 0.0f;
    Vec2 half{};
    float angle = 0.0f;
    float level = 0.0f;
};

struct Environment {
    bool ready = false;
    std::vector<GroundSource> ground;
    std::vector<WaterSource> water;
    Vec2 sky_wind{};
    bool has_sky = false;
};

}  // namespace

struct FireState {
    entt::entity owner = entt::null;
    bool has_mode = false;
    FireMode mode = FireMode::Edit;
    std::uint64_t signature = 0;
    bool started = false;

    // Rejilla: esquina minima (x, z) respecto al objeto, lado y celdas.
    Vec2 offset{};
    float size = 0.0f;
    float cell = 0.0f;
    int res = 0;
    std::vector<float> fuel;    // lo que queda (0..1)
    std::vector<float> fuel0;   // lo que habia
    std::vector<float> heat;    // 0..1 (>= kFlaming: llamas; menos: brasas)
    std::vector<float> burnt;   // carbonizado 0..1
    std::vector<float> smoke;   // humo que sale (sigue al calor con retraso)
    std::vector<float> wide;    // humo de lo alto (mas lento)
    std::vector<float> ground;  // altura del suelo respecto al objeto
    std::vector<float> scratch;
    std::vector<float> scratch2;
    float ground_min = 0.0f;
    float ground_max = 0.0f;
    std::shared_ptr<std::vector<std::uint8_t>> cells;
    std::shared_ptr<std::vector<float>> heights;
    bool visible = false;  // hay algo que dibujar

    float accumulator = 0.0f;
    std::uint64_t tick = 0;
    Vec2 wind{};

    struct Op {
        bool ignite = true;
        Vec2 local{};  // respecto al objeto (x, z)
        float radius = 1.0f;
    };
    std::vector<Op> pending;
    bool extinguish_all = false;

    struct Cluster {
        Vec3 local{};  // respecto al objeto
        float area = 0.0f;  // m2 con llamas
    };
    std::vector<Cluster> clusters;
    FireStats stats;

    void clearGrid() {
        res = 0;
        size = 0.0f;
        fuel.clear();
        fuel0.clear();
        heat.clear();
        burnt.clear();
        smoke.clear();
        wide.clear();
        ground.clear();
        cells.reset();
        heights.reset();
        clusters.clear();
        stats = FireStats{};
        visible = false;
        started = false;
        accumulator = 0.0f;
        tick = 0;
    }
};

// -----------------------------------------------------------------------------
// Componente
// -----------------------------------------------------------------------------

void Fire::reflect(ecs::PropertyVisitor& v) {
    v.field({"enabled", "Activo"}, enabled);
    if (v.wantsAllFields() || v.beginGroup("Zona", true)) {
        v.field({"size", "Tamano (m)", "Lado del cuadrado donde puede arder, centrado en el objeto"}, size,
                ecs::FloatRange{4.0f, 4000.0f, 1.0f, "%.0f m"});
        v.field({"cell_size", "Celda (m)", "Detalle del fuego. Mas de 256 celdas por lado: se agrandan solas"}, cell_size,
                ecs::FloatRange{0.1f, 20.0f, 0.05f, "%.2f m"});
        v.field({"spread", "Se propaga", "Las celdas que arden encienden a sus vecinas"}, spread);
        v.field({"limit_to_zone", "Solo en la zona", "No sale del cuadrado. Si no, la zona crece cuando el fuego llega al borde"},
                limit_to_zone);
        v.field({"max_size", "Tamano maximo (m)", "Hasta donde crece la zona sin limite"}, max_size,
                ecs::FloatRange{10.0f, 20000.0f, 10.0f, "%.0f m"});
        v.field({"simulate_in_editor", "Simular en el editor", "Ver el fuego sin darle a Play"}, simulate_in_editor);
        v.field({"seed", "Semilla", "La misma semilla, el mismo incendio"}, seed, 0, 1000000);
        if (!v.wantsAllFields()) v.endGroup();
    }
    if (v.wantsAllFields() || v.beginGroup("Fuego", true)) {
        v.field({"spread_speed", "Velocidad", "Metros por segundo que avanza el frente (sin viento ni cuesta)"}, spread_speed,
                ecs::FloatRange{0.0f, 20.0f, 0.01f, "%.2f m/s"});
        v.field({"burn_time", "Tiempo de llama", "Segundos que arde una celda con todo su combustible"}, burn_time,
                ecs::FloatRange{0.5f, 600.0f, 0.5f, "%.1f s"});
        v.field({"smolder_time", "Brasas", "Segundos de brasas y humo cuando se acaba la llama"}, smolder_time,
                ecs::FloatRange{0.0f, 600.0f, 0.5f, "%.1f s"});
        v.field({"flame_height", "Altura de las llamas"}, flame_height, ecs::FloatRange{0.2f, 40.0f, 0.05f, "%.2f m"});
        v.field({"flame_intensity", "Brillo de las llamas"}, flame_intensity, ecs::FloatRange{0.0f, 20.0f, 0.01f, "%.2f"});
        if (!v.wantsAllFields()) v.endGroup();
    }
    if (v.wantsAllFields() || v.beginGroup("Humo", true)) {
        v.field({"smoke_amount", "Cantidad"}, smoke_amount, ecs::FloatRange{0.0f, 5.0f, 0.01f, "%.2f"});
        v.field({"smoke_height", "Altura", "Hasta donde sube la columna"}, smoke_height,
                ecs::FloatRange{2.0f, 400.0f, 0.5f, "%.0f m"});
        v.field({"smoke_rise", "Subida", "Metros por segundo que sube (con viento, se tumba mas)"}, smoke_rise,
                ecs::FloatRange{0.2f, 30.0f, 0.05f, "%.2f m/s"});
        v.field({"smoke_color", "Color"}, smoke_color, ecs::Vec3Kind::Color);
        if (!v.wantsAllFields()) v.endGroup();
    }
    if (v.wantsAllFields() || v.beginGroup("Viento y terreno", true)) {
        v.field({"sky_wind", "Viento del cielo", "Direccion del viento del Cielo y la mitad de su velocidad"}, sky_wind);
        v.field({"wind_speed", "Viento", "Si no usa el del cielo"}, wind_speed, ecs::FloatRange{0.0f, 40.0f, 0.1f, "%.1f m/s"});
        v.field({"wind_direction", "Direccion del viento", "0 = hacia +X"}, wind_direction,
                ecs::FloatRange{0.0f, 360.0f, 1.0f, "%.0f°"});
        v.field({"wind_influence", "Efecto del viento", "Cuanto acelera el fuego a favor del viento"}, wind_influence,
                ecs::FloatRange{0.0f, 4.0f, 0.01f, "%.2f"});
        v.field({"slope_influence", "Efecto de la pendiente", "Cuanto mas rapido sube por las cuestas"}, slope_influence,
                ecs::FloatRange{0.0f, 4.0f, 0.01f, "%.2f"});
        v.field({"terrain_fuel", "Combustible del terreno",
                 "Hierba y hierba seca arden bien; tierra poco; roca, arena, nieve, barro y agua nada"},
                terrain_fuel);
        v.field({"default_fuel", "Combustible sin terreno", "Donde no hay terreno debajo (0..1)"}, default_fuel,
                ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f"});
        v.field({"fuel_multiplier", "Multiplicador de combustible"}, fuel_multiplier,
                ecs::FloatRange{0.0f, 3.0f, 0.01f, "%.2f"});
        if (!v.wantsAllFields()) v.endGroup();
    }
    if (v.wantsAllFields() || v.beginGroup("Encendido y luces", true)) {
        v.field({"ignite_on_start", "Encender al empezar", "En los puntos de abajo al darle a Play"}, ignite_on_start);
        ecs::listField(v, {"ignitions", "Puntos de encendido", "Respecto al objeto"}, ignitions,
                       [](FireIgnition& p, ecs::PropertyVisitor& item) {
                           item.field({"position", "Posicion", "Respecto al objeto (solo cuentan x y z)"}, p.position,
                                      ecs::Vec3Kind::Position);
                           item.field({"radius", "Radio"}, p.radius, ecs::FloatRange{0.1f, 500.0f, 0.1f, "%.1f m"});
                       });
        v.field({"lights", "Luces", "Luces puntuales que parpadean donde mas arde"}, lights);
        v.field({"max_lights", "Maximo de luces"}, max_lights, 0, 16);
        v.field({"light_intensity", "Intensidad de las luces"}, light_intensity, ecs::FloatRange{0.0f, 20.0f, 0.01f, "%.2f"});
        if (!v.wantsAllFields()) v.endGroup();
    }
}

void registerFireComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("Fire") == nullptr) {
        registry.registerComponent<Fire>("Fire", "Fuego (incendio que se propaga)", "Entorno");
    }
}

// -----------------------------------------------------------------------------
// Simulacion
// -----------------------------------------------------------------------------

namespace {

std::uint64_t hashBytes(std::uint64_t h, const void* data, std::size_t size) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

Vec3 absolutePosition(const ecs::World& world, const ecs::Entity& e) {
    return e.worldPosition() + Vec3{static_cast<float>(world.origin().x), static_cast<float>(world.origin().y),
                                    static_cast<float>(world.origin().z)};
}

// Terrenos (siempre: su version entra en la firma) y, si hace falta, agua y
// cuanto arde cada capa.
void gatherGround(ecs::World& world, terrain::TerrainStore* store, Environment& env) {
    env.ground.clear();
    if (store == nullptr) return;
    for (const entt::entity handle : world.registry().view<terrain::Terrain>()) {
        const ecs::Entity e = world.wrap(handle);
        if (!e.activeInHierarchy()) continue;
        const terrain::Terrain& t = e.get<terrain::Terrain>();
        std::shared_ptr<terrain::TerrainData> data = store->get(t);
        if (!data) continue;
        GroundSource g;
        g.data = std::move(data);
        g.terrain = t;
        g.origin = e.worldPosition();
        for (const terrain::TerrainLayer& layer : t.layers) g.flammability.push_back(layerFlammability(layer.name));
        env.ground.push_back(std::move(g));
    }
}

void gatherWater(ecs::World& world, Environment& env) {
    env.water.clear();
    for (const entt::entity handle : world.registry().view<water::WaterBody>()) {
        const ecs::Entity e = world.wrap(handle);
        if (!e.activeInHierarchy()) continue;
        const water::WaterBody& body = e.get<water::WaterBody>();
        const core::Mat4& m = e.worldMatrix();
        if (body.type == water::WaterType::River) {
            const std::vector<water::RiverSample> line = water::riverCenterline(body, m, 3.0f);
            for (std::size_t i = 0; i + 1 < line.size(); ++i) {
                WaterSource w;
                w.type = body.type;
                w.a = line[i].position;
                w.b = line[i + 1].position;
                w.width = std::max(line[i].width, line[i + 1].width);
                env.water.push_back(w);
            }
        } else {
            WaterSource w;
            w.type = body.type;
            w.a = e.worldPosition();
            w.half = Vec2{body.size.x * 0.5f, body.size.y * 0.5f};
            w.angle = std::atan2(-m.m[0][2], m.m[0][0]);
            w.level = w.a.y;
            env.water.push_back(w);
        }
    }
}

std::uint64_t environmentSignature(ecs::World& world, const Environment& env) {
    std::uint64_t h = 1469598103934665603ull;
    const Vec3 o{static_cast<float>(world.origin().x), static_cast<float>(world.origin().y),
                 static_cast<float>(world.origin().z)};
    for (const GroundSource& g : env.ground) {
        const std::uint64_t version = g.data->collisionVersion();
        const Vec3 p = g.origin + o;
        const float values[] = {p.x, p.y, p.z, g.terrain.size, g.terrain.height};
        h = hashBytes(h, values, sizeof(values));
        h = hashBytes(h, &version, sizeof(version));
        for (const float f : g.flammability) h = hashBytes(h, &f, sizeof(f));
    }
    for (const entt::entity handle : world.registry().view<water::WaterBody>()) {
        const ecs::Entity e = world.wrap(handle);
        if (!e.activeInHierarchy()) continue;
        const water::WaterBody& body = e.get<water::WaterBody>();
        const Vec3 p = e.worldPosition() + o;
        const float values[] = {p.x, p.y, p.z, body.size.x, body.size.y, static_cast<float>(body.type)};
        h = hashBytes(h, values, sizeof(values));
    }
    return h;
}

// Suelo (mundo) y combustible en un punto.
void sampleCell(const Fire& f, const Environment& env, float x, float z, float fallback_y, float& ground, float& fuel) {
    ground = fallback_y;
    fuel = std::clamp(f.default_fuel, 0.0f, 1.0f);
    for (const GroundSource& g : env.ground) {
        const float size = std::max(g.terrain.size, 1.0f);
        if (x < g.origin.x || z < g.origin.z || x > g.origin.x + size || z > g.origin.z + size) continue;
        ground = terrain::heightAt(*g.data, g.terrain, g.origin, x, z);
        if (f.terrain_fuel) {
            const std::uint32_t res = g.data->splatResolution();
            float total = 0.0f;
            float weighted = 0.0f;
            if (res > 0) {
                const float u = (x - g.origin.x) / size;
                const float v = (z - g.origin.z) / size;
                const auto sx = static_cast<std::uint32_t>(std::clamp(u * static_cast<float>(res), 0.0f, static_cast<float>(res - 1)));
                const auto sy = static_cast<std::uint32_t>(std::clamp(v * static_cast<float>(res), 0.0f, static_cast<float>(res - 1)));
                for (std::size_t l = 0; l < g.flammability.size() && l < static_cast<std::size_t>(terrain::kMaxLayers); ++l) {
                    const float w = static_cast<float>(g.data->weight(static_cast<int>(l), sx, sy)) / 255.0f;
                    total += w;
                    weighted += w * g.flammability[l];
                }
            }
            fuel = total > 0.01f ? weighted / total : 0.4f;
        }
        break;
    }
    // Agua: rios, lagos y bajo el mar.
    for (const WaterSource& w : env.water) {
        bool wet = false;
        if (w.type == water::WaterType::River) {
            const float abx = w.b.x - w.a.x, abz = w.b.z - w.a.z;
            const float len2 = abx * abx + abz * abz;
            const float t = len2 > 0.0f ? std::clamp(((x - w.a.x) * abx + (z - w.a.z) * abz) / len2, 0.0f, 1.0f) : 0.0f;
            const float dx = x - (w.a.x + abx * t), dz = z - (w.a.z + abz * t);
            const float reach = w.width * 0.5f + 1.5f;
            wet = dx * dx + dz * dz < reach * reach;
        } else if (w.type == water::WaterType::Lake) {
            const float c = std::cos(w.angle), s = std::sin(w.angle);
            const float lx = (x - w.a.x) * c + (z - w.a.z) * s;
            const float lz = -(x - w.a.x) * s + (z - w.a.z) * c;
            wet = std::abs(lx) < w.half.x && std::abs(lz) < w.half.y && ground < w.level + 0.4f;
        } else {
            wet = ground < w.level + 0.4f;  // el mar: infinito a su altura
        }
        if (wet) {
            fuel = 0.0f;
            break;
        }
    }
    fuel = std::clamp(fuel * std::max(f.fuel_multiplier, 0.0f), 0.0f, 1.0f);
}

// Rejilla de `side` metros con la esquina minima en `offset` (respecto al
// objeto). Lo que ya habia en `old` (otra rejilla) se conserva.
void buildGrid(FireState& s, const Fire& f, const Environment& env, const Vec3& position, const Vec2& offset, float side,
               const FireState* old) {
    const float wanted = std::max(f.cell_size, 0.05f);
    const int res = std::clamp(static_cast<int>(std::ceil(side / wanted)), 8, kMaxResolution);
    const float cell = side / static_cast<float>(res);
    const std::size_t n = static_cast<std::size_t>(res) * static_cast<std::size_t>(res);
    std::vector<float> fuel(n), fuel0(n), heat(n, 0.0f), burnt(n, 0.0f), smoke(n, 0.0f), wide(n, 0.0f), ground(n);
    float gmin = 1e30f, gmax = -1e30f;
    for (int row = 0; row < res; ++row) {
        for (int col = 0; col < res; ++col) {
            const std::size_t i = static_cast<std::size_t>(row) * res + col;
            const float lx = offset.x + (static_cast<float>(col) + 0.5f) * cell;
            const float lz = offset.y + (static_cast<float>(row) + 0.5f) * cell;
            bool copied = false;
            if (old != nullptr && old->res > 0) {
                const float ox = (lx - old->offset.x) / old->cell;
                const float oz = (lz - old->offset.y) / old->cell;
                if (ox >= 0.0f && oz >= 0.0f && ox < static_cast<float>(old->res) && oz < static_cast<float>(old->res)) {
                    const std::size_t j = static_cast<std::size_t>(oz) * old->res + static_cast<std::size_t>(ox);
                    fuel[i] = old->fuel[j];
                    fuel0[i] = old->fuel0[j];
                    heat[i] = old->heat[j];
                    burnt[i] = old->burnt[j];
                    smoke[i] = old->smoke[j];
                    wide[i] = old->wide[j];
                    ground[i] = old->ground[j];
                    copied = true;
                }
            }
            if (!copied) {
                float g = 0.0f, fu = 0.0f;
                sampleCell(f, env, position.x + lx, position.z + lz, position.y, g, fu);
                fuel[i] = fu;
                fuel0[i] = fu;
                ground[i] = g - position.y;
            }
            gmin = std::min(gmin, ground[i]);
            gmax = std::max(gmax, ground[i]);
        }
    }
    s.offset = offset;
    s.size = side;
    s.cell = cell;
    s.res = res;
    s.fuel = std::move(fuel);
    s.fuel0 = std::move(fuel0);
    s.heat = std::move(heat);
    s.burnt = std::move(burnt);
    s.smoke = std::move(smoke);
    s.wide = std::move(wide);
    s.ground = std::move(ground);
    s.ground_min = gmin;
    s.ground_max = gmax;
    s.heights = std::make_shared<std::vector<float>>(s.ground);
    s.scratch.assign(n, 0.0f);
    s.scratch2.assign(n, 0.0f);
}

void applyOp(FireState& s, const FireState::Op& op) {
    if (s.res <= 0) return;
    const float r = std::max(op.radius, s.cell * 0.5f);
    const int c0 = std::max(0, static_cast<int>(std::floor((op.local.x - r - s.offset.x) / s.cell)));
    const int c1 = std::min(s.res - 1, static_cast<int>(std::floor((op.local.x + r - s.offset.x) / s.cell)));
    const int r0 = std::max(0, static_cast<int>(std::floor((op.local.y - r - s.offset.y) / s.cell)));
    const int r1 = std::min(s.res - 1, static_cast<int>(std::floor((op.local.y + r - s.offset.y) / s.cell)));
    const float reach = r + s.cell * 0.5f;
    for (int row = r0; row <= r1; ++row) {
        for (int col = c0; col <= c1; ++col) {
            const float x = s.offset.x + (static_cast<float>(col) + 0.5f) * s.cell - op.local.x;
            const float z = s.offset.y + (static_cast<float>(row) + 0.5f) * s.cell - op.local.y;
            if (x * x + z * z > reach * reach) continue;
            const std::size_t i = static_cast<std::size_t>(row) * s.res + col;
            if (op.ignite) {
                if (s.fuel[i] > 0.02f && s.heat[i] < kFlaming) s.heat[i] = 0.5f;
            } else {
                s.heat[i] = 0.0f;
            }
        }
    }
}

// Desenfoque de caja separable (sumas acumuladas): de `in` a `out`.
void boxBlur(const std::vector<float>& in, std::vector<float>& out, std::vector<float>& temp, int res, int radius) {
    if (radius <= 0) {
        out = in;
        return;
    }
    const float scale = 1.0f / static_cast<float>(radius * 2 + 1);
    for (int row = 0; row < res; ++row) {
        const float* src = &in[static_cast<std::size_t>(row) * res];
        float* dst = &temp[static_cast<std::size_t>(row) * res];
        float sum = 0.0f;
        for (int k = -radius; k <= radius; ++k) sum += src[std::clamp(k, 0, res - 1)];
        for (int col = 0; col < res; ++col) {
            dst[col] = sum * scale;
            sum += src[std::min(col + radius + 1, res - 1)] - src[std::max(col - radius, 0)];
        }
    }
    for (int col = 0; col < res; ++col) {
        float sum = 0.0f;
        for (int k = -radius; k <= radius; ++k) sum += temp[static_cast<std::size_t>(std::clamp(k, 0, res - 1)) * res + col];
        for (int row = 0; row < res; ++row) {
            out[static_cast<std::size_t>(row) * res + col] = sum * scale;
            sum += temp[static_cast<std::size_t>(std::min(row + radius + 1, res - 1)) * res + col] -
                   temp[static_cast<std::size_t>(std::max(row - radius, 0)) * res + col];
        }
    }
}

void step(FireState& s, const Fire& f) {
    const int res = s.res;
    const std::size_t n = static_cast<std::size_t>(res) * res;
    const float dt = kStep;
    const std::uint64_t seed = static_cast<std::uint64_t>(std::max(f.seed, 0)) + 1;
    std::vector<float>& previous = s.scratch;
    previous = s.heat;

    // --- Combustion ---
    const float burn_time = std::max(f.burn_time, 0.1f);
    const float smolder_time = std::max(f.smolder_time, 0.05f);
    for (std::size_t i = 0; i < n; ++i) {
        float& h = s.heat[i];
        if (h <= 0.0f) continue;
        if (s.fuel[i] > 0.0f) {
            s.fuel[i] -= dt / burn_time;
            const float target = std::clamp(0.45f + 0.7f * s.fuel0[i], 0.0f, 1.0f);
            h += (target - h) * std::min(1.0f, dt * 1.3f);
            s.burnt[i] = std::max(s.burnt[i], 1.0f - std::max(s.fuel[i], 0.0f) / std::max(s.fuel0[i], 1e-3f));
            if (s.fuel[i] <= 0.0f) {
                s.fuel[i] = 0.0f;
                h = std::min(h, kSmolderHeat);
                s.burnt[i] = 1.0f;
            }
        } else {
            h = std::max(h - dt * kSmolderHeat / smolder_time, 0.0f);
        }
    }

    // --- Propagacion: las vecinas en llamas (estado anterior) ---
    const Vec2 wind = s.wind;
    const float wind_speed = std::sqrt(wind.x * wind.x + wind.y * wind.y);
    if (f.spread && f.spread_speed > 0.0f) {
        static constexpr int kDx[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
        static constexpr int kDz[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
        float wind_factor[8];
        float inv_len[8];
        for (int k = 0; k < 8; ++k) {
            const float len = std::sqrt(static_cast<float>(kDx[k] * kDx[k] + kDz[k] * kDz[k]));
            inv_len[k] = 1.0f / len;
            // Del vecino hacia esta celda: -(dx, dz).
            const float along = (-kDx[k] * wind.x - kDz[k] * wind.y) / len;
            wind_factor[k] = std::exp(std::clamp(along * 0.22f * f.wind_influence, -3.0f, 3.0f));
        }
        const float base_rate = f.spread_speed / s.cell * 0.33f;
        for (int row = 0; row < res; ++row) {
            for (int col = 0; col < res; ++col) {
                const std::size_t i = static_cast<std::size_t>(row) * res + col;
                if (s.heat[i] > 0.0f || s.fuel[i] <= 0.02f || s.burnt[i] > 0.5f) continue;
                float sum = 0.0f;
                for (int k = 0; k < 8; ++k) {
                    const int nc = col + kDx[k], nr = row + kDz[k];
                    if (nc < 0 || nr < 0 || nc >= res || nr >= res) continue;
                    const std::size_t j = static_cast<std::size_t>(nr) * res + nc;
                    const float h = previous[j];
                    if (h < kFlaming || s.fuel[j] <= 0.0f) continue;
                    const float slope = (s.ground[i] - s.ground[j]) * inv_len[k] / s.cell;
                    const float slope_factor = std::exp(std::clamp(slope, -1.0f, 1.2f) * 2.2f * f.slope_influence);
                    sum += h * wind_factor[k] * slope_factor * inv_len[k];
                }
                if (sum <= 0.0f) continue;
                const float rate = base_rate * sum * std::pow(s.fuel[i], 0.6f);
                const float p = 1.0f - std::exp(-rate * dt);
                if (random01(seed, s.tick, i, 0) < p) s.heat[i] = 0.35f;
            }
        }
        // Pavesas: con viento fuerte saltan focos por delante del frente.
        if (wind_speed > 4.0f) {
            const float chance = dt * 0.004f * (wind_speed - 4.0f) * f.wind_influence;
            const Vec2 dir{wind.x / wind_speed, wind.y / wind_speed};
            for (std::size_t i = 0; i < n; ++i) {
                if (previous[i] < kFlaming || s.fuel[i] <= 0.0f) continue;
                if (random01(seed, s.tick, i, 1) >= chance) continue;
                const float jump = 3.0f + random01(seed, s.tick, i, 2) * wind_speed * 1.5f;  // m
                const float side = (random01(seed, s.tick, i, 3) - 0.5f) * jump * 0.5f;
                const int col = static_cast<int>(i % res), row = static_cast<int>(i / res);
                const int tc = col + static_cast<int>(std::round((dir.x * jump - dir.y * side) / s.cell));
                const int tr = row + static_cast<int>(std::round((dir.y * jump + dir.x * side) / s.cell));
                if (tc < 0 || tr < 0 || tc >= res || tr >= res) continue;
                const std::size_t t = static_cast<std::size_t>(tr) * res + tc;
                if (s.heat[t] <= 0.0f && s.fuel[t] > 0.05f && s.burnt[t] < 0.5f) s.heat[t] = 0.35f;
            }
        }
    }

    // --- Humo: sigue al calor con retraso y se queda un rato ---
    const float follow = 1.0f - std::exp(-dt / 2.5f);
    const float follow_wide = 1.0f - std::exp(-dt / 7.0f);
    const float amount = std::max(f.smoke_amount, 0.0f);
    for (std::size_t i = 0; i < n; ++i) {
        const float h = s.heat[i];
        const float source = (s.fuel[i] > 0.0f ? h : h * 2.2f) * amount;  // las brasas humean mucho
        s.smoke[i] += (source - s.smoke[i]) * follow;
        s.wide[i] += (s.smoke[i] - s.wide[i]) * follow_wide;
    }
    ++s.tick;
}

// Crece la rejilla si el fuego llega al borde (sin "Solo en la zona").
bool maybeGrow(FireState& s, const Fire& f, const Environment& env, const Vec3& position) {
    if (f.limit_to_zone || !f.spread || s.size >= f.max_size - 0.01f) return false;
    const int margin = 3;
    bool near_edge = false;
    for (int row = 0; row < s.res && !near_edge; ++row) {
        for (int col = 0; col < s.res; ++col) {
            if (row >= margin && row < s.res - margin && col >= margin && col < s.res - margin) {
                col = s.res - margin - 1;  // salta el interior
                continue;
            }
            if (s.heat[static_cast<std::size_t>(row) * s.res + col] >= kFlaming) {
                near_edge = true;
                break;
            }
        }
    }
    if (!near_edge) return false;
    const float side = std::min(f.max_size, s.size * 1.5f);
    const Vec2 center{s.offset.x + s.size * 0.5f, s.offset.y + s.size * 0.5f};
    FireState old;
    old.res = s.res;
    old.cell = s.cell;
    old.offset = s.offset;
    old.fuel = std::move(s.fuel);
    old.fuel0 = std::move(s.fuel0);
    old.heat = std::move(s.heat);
    old.burnt = std::move(s.burnt);
    old.smoke = std::move(s.smoke);
    old.wide = std::move(s.wide);
    old.ground = std::move(s.ground);
    buildGrid(s, f, env, position, Vec2{center.x - side * 0.5f, center.y - side * 0.5f}, side, &old);
    return true;
}

// Mapas para la GPU, luces y estadisticas.
void publish(FireState& s, const Fire& f) {
    const int res = s.res;
    const std::size_t n = static_cast<std::size_t>(res) * res;
    // Humo de abajo: un poco difuminado; el de arriba, mucho (la columna se ensancha).
    std::vector<float> smoke_low(n), smoke_high(n);
    boxBlur(s.smoke, smoke_low, s.scratch2, res, std::max(1, static_cast<int>(std::round(1.2f / s.cell))));
    const int wide_radius = std::max(1, static_cast<int>(std::round(std::max(4.0f, f.smoke_height * 0.06f) / s.cell)));
    boxBlur(s.wide, smoke_high, s.scratch2, res, wide_radius);
    boxBlur(smoke_high, s.scratch, s.scratch2, res, wide_radius);
    auto cells = std::make_shared<std::vector<std::uint8_t>>(n * 4);
    std::uint8_t* out = cells->data();
    const auto byte = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    FireStats st;
    st.active = true;
    st.resolution = res;
    st.size = s.size;
    st.offset = s.offset;
    st.wind = s.wind;
    bool anything = false;
    for (std::size_t i = 0; i < n; ++i) {
        const float h = s.heat[i];
        const bool flaming = h >= kFlaming && s.fuel[i] > 0.0f;
        // g: llamas (las brasas casi sin llama).
        const float g = flaming ? h : h * 0.5f;
        out[i * 4 + 0] = byte(s.burnt[i]);
        out[i * 4 + 1] = byte(g);
        out[i * 4 + 2] = byte(smoke_low[i] * 1.3f);
        out[i * 4 + 3] = byte(s.scratch[i] * 2.0f);
        if (out[i * 4 + 0] | out[i * 4 + 1] | out[i * 4 + 2] | out[i * 4 + 3]) anything = true;
        if (flaming) ++st.burning_cells;
        else if (h > 0.0f) ++st.smoldering_cells;
        if (s.fuel0[i] > 0.02f) {
            ++st.burnable_cells;
            if (s.burnt[i] > 0.9f) ++st.burned_cells;
        }
    }
    const float cell_area = s.cell * s.cell;
    st.burning_area = static_cast<float>(st.burning_cells) * cell_area;
    st.burned_fraction = st.burnable_cells > 0 ? static_cast<float>(st.burned_cells) / static_cast<float>(st.burnable_cells) : 0.0f;
    st.simulated_seconds = static_cast<float>(s.tick) * kStep;
    s.stats = st;
    s.cells = std::move(cells);
    s.visible = anything;

    // Focos para las luces: bloques de la rejilla con mas llamas.
    s.clusters.clear();
    if (f.lights && f.max_lights > 0 && st.burning_cells > 0) {
        const int block = std::max(4, res / 6);
        struct Block {
            float heat = 0.0f, x = 0.0f, z = 0.0f, y = 0.0f;
            int count = 0;
        };
        const int blocks = (res + block - 1) / block;
        std::vector<Block> list(static_cast<std::size_t>(blocks) * blocks);
        for (int row = 0; row < res; ++row) {
            for (int col = 0; col < res; ++col) {
                const std::size_t i = static_cast<std::size_t>(row) * res + col;
                if (s.heat[i] < kFlaming || s.fuel[i] <= 0.0f) continue;
                Block& b = list[static_cast<std::size_t>(row / block) * blocks + col / block];
                const float h = s.heat[i];
                b.heat += h;
                b.x += h * (s.offset.x + (static_cast<float>(col) + 0.5f) * s.cell);
                b.z += h * (s.offset.y + (static_cast<float>(row) + 0.5f) * s.cell);
                b.y += h * s.ground[i];
                ++b.count;
            }
        }
        std::sort(list.begin(), list.end(), [](const Block& a, const Block& b) { return a.heat > b.heat; });
        for (const Block& b : list) {
            if (b.heat <= 0.0f || static_cast<int>(s.clusters.size()) >= std::min(f.max_lights, 16)) break;
            FireState::Cluster c;
            c.local = Vec3{b.x / b.heat, b.y / b.heat + f.flame_height * 0.8f, b.z / b.heat};
            c.area = static_cast<float>(b.count) * cell_area;
            s.clusters.push_back(c);
        }
    }
}

std::uint64_t fireSignature(const Fire& f, const Vec3& absolute) {
    std::uint64_t h = 1469598103934665603ull;
    const float values[] = {f.size, f.cell_size, f.default_fuel, f.fuel_multiplier, absolute.x, absolute.y, absolute.z};
    h = hashBytes(h, values, sizeof(values));
    const int ints[] = {f.terrain_fuel ? 1 : 0, f.limit_to_zone ? 1 : 0, f.seed};
    return hashBytes(h, ints, sizeof(ints));
}

bool zoneContains(const Fire& f, const FireState* s, const Vec2& local, float radius) {
    Vec2 lo{-f.size * 0.5f, -f.size * 0.5f};
    float side = f.size;
    if (s != nullptr && s->res > 0) {
        lo = s->offset;
        side = s->size;
    }
    return local.x + radius >= lo.x && local.y + radius >= lo.y && local.x - radius <= lo.x + side &&
           local.y - radius <= lo.y + side;
}

FireState& ensureState(Fire& f, entt::entity owner) {
    if (!f.state || f.state->owner != owner) {
        f.state = std::make_shared<FireState>();
        f.state->owner = owner;
    }
    return *f.state;
}

}  // namespace

void updateFires(ecs::World& world, float delta_seconds, FireMode mode, terrain::TerrainStore* terrains) {
    auto view = world.registry().view<Fire>();
    if (view.begin() == view.end()) return;
    Environment env;
    bool env_ground = false;
    bool env_water = false;
    std::uint64_t env_signature = 0;
    // Viento del cielo.
    for (const entt::entity handle : world.registry().view<ecs::Sky>()) {
        const ecs::Entity e = world.wrap(handle);
        if (!e.activeInHierarchy()) continue;
        const ecs::Sky& sky = e.get<ecs::Sky>();
        const float a = sky.wind_direction * core::kPi / 180.0f;
        const float speed = std::max(sky.wind_speed, 0.0f) * 0.5f;
        env.sky_wind = Vec2{std::cos(a) * speed, std::sin(a) * speed};
        env.has_sky = true;
        break;
    }
    for (const entt::entity handle : view) {
        const ecs::Entity e = world.wrap(handle);
        Fire& f = view.get<Fire>(handle);
        const bool active = f.enabled && e.activeInHierarchy();
        const bool simulate = active && (mode != FireMode::Edit || f.simulate_in_editor);
        if (!simulate) {
            if (f.state && (mode == FireMode::Edit || !active)) {
                std::vector<FireState::Op> keep = std::move(f.state->pending);
                f.state->clearGrid();
                f.state->pending = mode == FireMode::Edit ? std::vector<FireState::Op>{} : std::move(keep);
                f.state->signature = 0;
            }
            continue;
        }
        FireState& s = ensureState(f, handle);
        // Al darle a Play (o al pararlo), de cero: lo que se viera en el
        // editor no cuenta.
        if (s.has_mode && (s.mode == FireMode::Edit) != (mode == FireMode::Edit)) {
            s.clearGrid();
            s.signature = 0;
        }
        s.mode = mode;
        s.has_mode = true;

        if (!env_ground) {
            gatherGround(world, terrains, env);
            env_signature = environmentSignature(world, env);
            env_ground = true;
        }
        const Vec3 position = e.worldPosition();
        const std::uint64_t signature = fireSignature(f, absolutePosition(world, e)) ^ (env_signature * 0x9E3779B97F4A7C15ull);
        if (signature != s.signature || s.res == 0) {
            if (!env_water) {
                gatherWater(world, env);
                env_water = true;
            }
            std::vector<FireState::Op> keep = std::move(s.pending);
            s.clearGrid();
            s.pending = std::move(keep);
            const float side = std::max(f.size, 1.0f);
            buildGrid(s, f, env, position, Vec2{-side * 0.5f, -side * 0.5f}, side, nullptr);
            s.signature = signature;
        }
        // Viento.
        if (f.sky_wind && env.has_sky) {
            s.wind = env.sky_wind;
        } else {
            const float a = f.wind_direction * core::kPi / 180.0f;
            s.wind = Vec2{std::cos(a) * f.wind_speed, std::sin(a) * f.wind_speed};
        }

        bool changed = false;
        if (!s.started) {
            s.started = true;
            if (f.ignite_on_start) {
                for (const FireIgnition& p : f.ignitions) s.pending.push_back({true, Vec2{p.position.x, p.position.z}, p.radius});
            }
            changed = true;
        }
        if (s.extinguish_all) {
            std::fill(s.heat.begin(), s.heat.end(), 0.0f);
            s.extinguish_all = false;
            changed = true;
        }
        for (const FireState::Op& op : s.pending) applyOp(s, op);
        changed |= !s.pending.empty();
        s.pending.clear();

        if (mode != FireMode::Paused && delta_seconds > 0.0f) {
            s.accumulator += std::min(delta_seconds, 0.5f);
            int steps = 0;
            while (s.accumulator >= kStep && steps < kMaxStepsPerFrame) {
                s.accumulator -= kStep;
                step(s, f);
                ++steps;
            }
            if (steps == kMaxStepsPerFrame) s.accumulator = 0.0f;
            if (steps > 0) {
                changed = true;
                if (!f.limit_to_zone && !env_water) {
                    gatherWater(world, env);
                    env_water = true;
                }
                if (maybeGrow(s, f, env, position)) changed = true;
            }
        }
        if (changed || !s.cells) publish(s, f);
    }
}

void collectFireRender(ecs::World& world, float seconds, std::vector<gfx::FireZone>& zones,
                       std::vector<scene::PointLight>& lights) {
    zones.clear();
    lights.clear();
    for (const entt::entity handle : world.registry().view<Fire>()) {
        const ecs::Entity e = world.wrap(handle);
        const Fire& f = e.get<Fire>();
        if (!f.state || f.state->owner != handle || f.state->res <= 0 || !f.state->cells || !f.enabled ||
            !e.activeInHierarchy()) {
            continue;
        }
        const FireState& s = *f.state;
        if (!s.visible || zones.size() >= gfx::kFireZoneSlots) continue;
        const Vec3 p = e.worldPosition();
        gfx::FireZone z;
        z.origin = Vec3{p.x + s.offset.x, p.y, p.z + s.offset.y};
        z.size = s.size;
        z.resolution = static_cast<std::uint32_t>(s.res);
        z.cells = s.cells;
        z.heights = s.heights;
        z.ground_min = p.y + s.ground_min;
        z.ground_max = p.y + s.ground_max;
        z.flame_height = f.flame_height;
        z.flame_intensity = f.flame_intensity;
        z.smoke_height = f.smoke_height;
        z.smoke_density = f.smoke_amount;
        z.smoke_rise = f.smoke_rise;
        z.smoke_color = f.smoke_color;
        z.wind = s.wind;
        zones.push_back(z);
        // Luces que parpadean (dos senos sin relacion + un temblor rapido).
        for (std::size_t i = 0; i < s.clusters.size(); ++i) {
            const FireState::Cluster& c = s.clusters[i];
            const float k = static_cast<float>(i) * 1.731f + static_cast<float>(static_cast<std::uint32_t>(handle) % 97u);
            const float flicker = 0.78f + 0.12f * std::sin(seconds * 9.1f + k) + 0.07f * std::sin(seconds * 23.7f + k * 2.3f) +
                                  0.05f * std::sin(seconds * 3.3f + k * 0.7f);
            const float size = std::sqrt(std::max(c.area, 1.0f));
            scene::PointLight light;
            light.position = Vec3{p.x + c.local.x + 0.3f * std::sin(seconds * 5.3f + k), p.y + c.local.y,
                                  p.z + c.local.z + 0.3f * std::cos(seconds * 4.1f + k)};
            light.color = Vec3{1.0f, 0.45f, 0.14f};
            light.intensity = f.light_intensity * (6.0f + 2.5f * size) * flicker;
            light.range = std::clamp(12.0f + size * 2.0f, 12.0f, 70.0f);
            light.cast_shadows = false;
            light.source_radius = 0.5f;
            lights.push_back(light);
        }
    }
}

int ignite(ecs::World& world, const Vec3& position, float radius) {
    int count = 0;
    for (const entt::entity handle : world.registry().view<Fire>()) {
        const ecs::Entity e = world.wrap(handle);
        Fire& f = e.get<Fire>();
        if (!f.enabled || !e.activeInHierarchy()) continue;
        const Vec3 local3 = position - e.worldPosition();
        const Vec2 local{local3.x, local3.z};
        FireState& s = ensureState(f, handle);
        if (!zoneContains(f, &s, local, radius)) continue;
        s.pending.push_back({true, local, std::max(radius, 0.1f)});
        ++count;
    }
    return count;
}

int extinguish(ecs::World& world, const Vec3& position, float radius) {
    int count = 0;
    for (const entt::entity handle : world.registry().view<Fire>()) {
        const ecs::Entity e = world.wrap(handle);
        Fire& f = e.get<Fire>();
        if (!f.state || f.state->owner != handle) continue;
        const Vec3 local3 = position - e.worldPosition();
        const Vec2 local{local3.x, local3.z};
        if (!zoneContains(f, f.state.get(), local, radius)) continue;
        f.state->pending.push_back({false, local, std::max(radius, 0.1f)});
        ++count;
    }
    return count;
}

void extinguishAll(ecs::World& world) {
    for (const entt::entity handle : world.registry().view<Fire>()) {
        Fire& f = world.registry().get<Fire>(handle);
        if (!f.state || f.state->owner != handle) continue;
        f.state->pending.clear();
        f.state->extinguish_all = true;
    }
}

void resetAll(ecs::World& world) {
    for (const entt::entity handle : world.registry().view<Fire>()) {
        Fire& f = world.registry().get<Fire>(handle);
        if (!f.state || f.state->owner != handle) continue;
        f.state->clearGrid();
        f.state->pending.clear();
        f.state->signature = 0;
    }
}

void reset(Fire& fire) {
    if (!fire.state) return;
    fire.state->clearGrid();
    fire.state->pending.clear();
    fire.state->extinguish_all = false;
    fire.state->signature = 0;
}

namespace {
template <typename Fn>
float sampleZones(ecs::World& world, const Vec3& position, Fn&& value) {
    float best = 0.0f;
    for (const entt::entity handle : world.registry().view<Fire>()) {
        const ecs::Entity e = world.wrap(handle);
        const Fire& f = e.get<Fire>();
        if (!f.state || f.state->owner != handle || f.state->res <= 0) continue;
        const FireState& s = *f.state;
        const Vec3 local = position - e.worldPosition();
        const int col = static_cast<int>(std::floor((local.x - s.offset.x) / s.cell));
        const int row = static_cast<int>(std::floor((local.z - s.offset.y) / s.cell));
        if (col < 0 || row < 0 || col >= s.res || row >= s.res) continue;
        best = std::max(best, value(s, static_cast<std::size_t>(row) * s.res + col));
    }
    return best;
}
}  // namespace

float heatAt(ecs::World& world, const Vec3& position) {
    return sampleZones(world, position, [](const FireState& s, std::size_t i) {
        return s.fuel[i] > 0.0f ? s.heat[i] : s.heat[i] * 0.5f;
    });
}

float charAt(ecs::World& world, const Vec3& position) {
    return sampleZones(world, position, [](const FireState& s, std::size_t i) { return s.burnt[i]; });
}

FireStats stats(const Fire& fire) {
    return fire.state ? fire.state->stats : FireStats{};
}

FireStats totalStats(ecs::World& world) {
    FireStats total;
    float area = 0.0f;
    float burnable_area = 0.0f;
    float burned_area = 0.0f;
    for (const entt::entity handle : world.registry().view<Fire>()) {
        const Fire& f = world.registry().get<Fire>(handle);
        if (!f.state || f.state->owner != handle || f.state->res <= 0) continue;
        const FireStats& s = f.state->stats;
        const float cell_area = f.state->cell * f.state->cell;
        total.active = true;
        total.resolution = std::max(total.resolution, s.resolution);
        total.size = std::max(total.size, s.size);
        total.burning_cells += s.burning_cells;
        total.smoldering_cells += s.smoldering_cells;
        total.burned_cells += s.burned_cells;
        total.burnable_cells += s.burnable_cells;
        area += s.burning_area;
        burnable_area += static_cast<float>(s.burnable_cells) * cell_area;
        burned_area += static_cast<float>(s.burned_cells) * cell_area;
        total.wind = s.wind;
        total.simulated_seconds = std::max(total.simulated_seconds, s.simulated_seconds);
    }
    total.burning_area = area;
    total.burned_fraction = burnable_area > 0.0f ? burned_area / burnable_area : 0.0f;
    return total;
}

}  // namespace cramion::fire
