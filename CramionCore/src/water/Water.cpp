#include "CramionCore/water/Water.h"

#include "CramionCore/ecs/World.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace cramion::water {

using core::Vec2;
using core::Vec3;
using ecs::FloatRange;
using ecs::Vec3Kind;

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kGravity = 9.81f;

// Oleaje: espectro JONSWAP (el del mar real, medido en el Mar del Norte)
// muestreado en 24 ondas de Gerstner. Longitudes de 2 a 0.06 veces la
// principal; la amplitud de cada una sale del espectro (el pico, cerca de la
// principal) y esta normalizada para que la altura sea la ALTURA SIGNIFICATIVA
// (la media del tercio mas alto de las olas, la que usan los oceanografos).
// Direcciones repartidas alrededor del viento (mas abiertas cuanto mas corta
// la onda) y fases pseudoaleatorias: sin el patron regular de pocas ondas.
// MISMAS tablas en CramionFX/shaders/water_common.glsl (lo que se dibuja).
constexpr int kWaves = 24;
constexpr std::array<float, kWaves> kWaveLengths = {2.000000f, 1.717188f, 1.474368f, 1.265883f, 1.086880f, 0.933189f, 0.801230f, 0.687931f, 0.590654f, 0.507132f, 0.435420f, 0.373849f, 0.320985f, 0.275596f, 0.236625f, 0.203165f, 0.174436f, 0.149770f, 0.128591f, 0.110408f, 0.094795f, 0.081391f, 0.069882f, 0.060000f};
constexpr std::array<float, kWaves> kWaveAmplitudes = {0.029095f, 0.048279f, 0.068845f, 0.097571f, 0.152326f, 0.166827f, 0.122764f, 0.094758f, 0.084349f, 0.076526f, 0.068538f, 0.060708f, 0.053334f, 0.046573f, 0.040490f, 0.035085f, 0.030329f, 0.026171f, 0.022553f, 0.019417f, 0.016704f, 0.014363f, 0.012346f, 0.010609f};
constexpr std::array<float, kWaves> kWaveDirections = {-0.083431f, 0.569487f, 0.071680f, -0.290316f, 0.415380f, -0.021591f, -0.618493f, 0.222443f, -0.215726f, 0.735936f, 0.037831f, -0.551784f, 0.467343f, -0.104373f, -1.029364f, 0.192298f, -0.424478f, 0.817133f, -0.002207f, -0.904800f, 0.451188f, -0.257296f, 1.281280f, 0.120902f};
constexpr std::array<float, kWaves> kWavePhases = {1.193805f, 5.936841f, 4.396692f, 2.856543f, 1.316394f, 6.059431f, 4.519282f, 2.979132f, 1.438983f, 6.182020f, 4.641871f, 3.101722f, 1.561573f, 0.021424f, 4.764460f, 3.224311f, 1.684162f, 0.144013f, 4.887049f, 3.346900f, 1.806751f, 0.266602f, 5.009638f, 3.469489f};

float g_time = 0.0f;

Vec3 transformPoint(const core::Mat4& m, const Vec3& p) {
    const core::Vec4 r = m * core::Vec4{p.x, p.y, p.z, 1.0f};
    return Vec3{r.x, r.y, r.z};
}

}  // namespace

void WaterBody::reflect(ecs::PropertyVisitor& v) {
    static constexpr std::array<const char*, 3> kTypes = {"Oceano / playa", "Lago", "Rio"};
    ecs::enumField(v, {"type", "Tipo"}, type, kTypes);
    const bool all = v.wantsAllFields();
    if (all || type == WaterType::Lake) {
        v.field({"size", "Tamano", "Ancho y largo (m); la orilla la pone el terreno"}, size, 0.5f);
    }
    if (all || v.beginGroup("Oleaje")) {
        v.field({"wave_height", "Altura de ola",
                 "Altura significativa: la media del tercio mas alto de las olas (espectro JONSWAP)"},
                wave_height, FloatRange{0.0f, 12.0f, 0.01f, "%.2f m"});
        v.field({"wavelength", "Longitud de onda"}, wavelength, FloatRange{0.5f, 300.0f, 0.1f, "%.1f m"});
        v.field({"wave_speed", "Velocidad"}, wave_speed, FloatRange{0.0f, 4.0f, 0.01f, "%.2f"});
        v.field({"steepness", "Crestas", "0 = redondeadas, 1 = afiladas"}, steepness,
                FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.field({"wind_direction", "Direccion del viento"}, wind_direction,
                FloatRange{-180.0f, 180.0f, 1.0f, "%.0f°", true});
        v.field({"wind_spread", "Dispersion"}, wind_spread, FloatRange{0.0f, 90.0f, 1.0f, "%.0f°", true});
        if (all || type == WaterType::River) {
            v.field({"flow_speed", "Corriente"}, flow_speed, FloatRange{0.0f, 10.0f, 0.05f, "%.2f m/s"});
        }
        if (!all) v.endGroup();
    }
    if (all || v.beginGroup("Aspecto")) {
        v.field({"shallow_color", "Color (dispersion)"}, shallow_color, Vec3Kind::Color);
        v.field({"deep_color", "Color profundo"}, deep_color, Vec3Kind::Color);
        v.field({"clarity", "Transparencia", "Metros hasta que el fondo casi no se ve"}, clarity,
                FloatRange{0.1f, 60.0f, 0.05f, "%.2f m"});
        v.field({"roughness", "Rugosidad"}, roughness, FloatRange{0.0f, 0.5f, 0.005f, "%.3f", true});
        v.field({"refraction", "Refraccion"}, refraction, FloatRange{0.0f, 2.0f, 0.01f, "%.2f", true});
        v.field({"detail", "Ondulacion fina"}, detail, FloatRange{0.0f, 3.0f, 0.01f, "%.2f", true});
        v.field({"foam", "Espuma"}, foam, FloatRange{0.0f, 3.0f, 0.01f, "%.2f", true});
        v.field({"shore_foam", "Espuma de orilla"}, shore_foam, FloatRange{0.0f, 10.0f, 0.05f, "%.2f m"});
        if (all || type == WaterType::Ocean) {
            v.field({"shore_waves", "Olas en la playa"}, shore_waves, FloatRange{0.0f, 3.0f, 0.01f, "%.2f", true});
        }
        v.field({"caustics", "Causticas"}, caustics, FloatRange{0.0f, 3.0f, 0.01f, "%.2f", true});
        v.field({"scattering", "Luz en las crestas"}, scattering, FloatRange{0.0f, 3.0f, 0.01f, "%.2f", true});
        if (!all) v.endGroup();
    }
    if (all || v.beginGroup("Fisica")) {
        v.field({"buoyancy", "Flotacion", "Los Rigidbody que entran flotan (Jolt)"}, buoyancy);
        v.field({"density", "Empuje", "1 = agua; mas, flota mas"}, density, FloatRange{0.0f, 4.0f, 0.01f, "%.2f"});
        v.field({"drag", "Frenado"}, drag, FloatRange{0.0f, 5.0f, 0.01f, "%.2f"});
        if (!all) v.endGroup();
    }
    if (all || type == WaterType::River) {
        ecs::listField(v, {"points", "Puntos del rio", "Locales a la entidad"}, points,
                       [](RiverPoint& p, ecs::PropertyVisitor& item) {
                           item.field({"position", "Posicion"}, p.position, Vec3Kind::Position);
                           item.field({"width", "Ancho"}, p.width, FloatRange{0.5f, 500.0f, 0.1f, "%.1f m"});
                       });
    }
}

WaterBody oceanPreset() {
    WaterBody b;
    b.type = WaterType::Ocean;
    b.wave_height = 1.1f;
    b.wavelength = 42.0f;
    b.steepness = 0.62f;
    b.wind_spread = 40.0f;
    b.shallow_color = Vec3{0.06f, 0.42f, 0.44f};
    b.deep_color = Vec3{0.004f, 0.030f, 0.062f};
    b.clarity = 7.0f;
    b.roughness = 0.06f;
    b.shore_foam = 3.0f;
    b.shore_waves = 1.0f;
    b.scattering = 1.2f;
    return b;
}

WaterBody lakePreset() {
    WaterBody b;
    b.type = WaterType::Lake;
    b.wave_height = 0.12f;
    b.wavelength = 6.0f;
    b.steepness = 0.35f;
    b.shallow_color = Vec3{0.12f, 0.36f, 0.30f};
    b.deep_color = Vec3{0.010f, 0.045f, 0.045f};
    b.clarity = 3.5f;
    b.roughness = 0.03f;
    b.shore_foam = 0.6f;
    b.shore_waves = 0.0f;
    b.foam = 0.5f;
    return b;
}

WaterBody riverPreset() {
    WaterBody b;
    b.type = WaterType::River;
    b.wave_height = 0.04f;
    b.wavelength = 2.5f;
    b.steepness = 0.3f;
    b.flow_speed = 2.0f;
    b.shallow_color = Vec3{0.20f, 0.36f, 0.26f};
    b.deep_color = Vec3{0.030f, 0.050f, 0.030f};
    b.clarity = 1.8f;
    b.roughness = 0.05f;
    b.shore_foam = 0.8f;
    b.shore_waves = 0.0f;
    b.foam = 1.2f;
    b.detail = 1.4f;
    // Un meandro de ejemplo (locales): se editan en el Inspector.
    b.points = {RiverPoint{Vec3{-40.0f, 0.0f, 0.0f}, 9.0f}, RiverPoint{Vec3{-15.0f, 0.0f, 10.0f}, 10.0f},
                RiverPoint{Vec3{10.0f, 0.0f, -6.0f}, 12.0f}, RiverPoint{Vec3{40.0f, 0.0f, 4.0f}, 10.0f}};
    return b;
}

Vec3 gerstner(const WaterBody& body, float x, float z, float time, Vec3* normal, float* jacobian) {
    Vec3 offset{};
    Vec3 n{0.0f, 1.0f, 0.0f};
    float j = 1.0f;
    const float wind = body.wind_direction * kPi / 180.0f;
    const float spread = body.wind_spread * kPi / 180.0f;
    const float steep = std::clamp(body.steepness, 0.0f, 1.0f);
    const float peak = std::max(body.wavelength, 0.05f);
    for (int i = 0; i < kWaves; ++i) {
        const float length = peak * kWaveLengths[i];
        const float amplitude = body.wave_height * kWaveAmplitudes[i];
        const float angle = wind + kWaveDirections[i] * spread;
        const float dx = std::cos(angle);
        const float dz = std::sin(angle);
        const float k = 2.0f * kPi / length;
        const float omega = std::sqrt(kGravity * k) * body.wave_speed;
        const float q = amplitude > 1e-6f ? steep / (k * amplitude * kWaves) : 0.0f;
        const float theta = k * (dx * x + dz * z) - omega * time + kWavePhases[i];
        const float c = std::cos(theta);
        const float s = std::sin(theta);
        offset.x += q * amplitude * dx * c;
        offset.z += q * amplitude * dz * c;
        offset.y += amplitude * s;
        n.x -= dx * k * amplitude * c;
        n.z -= dz * k * amplitude * c;
        n.y -= q * k * amplitude * s;
        j -= q * k * amplitude * s;
    }
    if (normal != nullptr) *normal = core::normalize(n);
    if (jacobian != nullptr) *jacobian = j;
    return offset;
}

std::vector<RiverSample> riverCenterline(const WaterBody& body, const core::Mat4& world, float step) {
    std::vector<RiverSample> out;
    const std::size_t count = body.points.size();
    if (count < 2) return out;
    std::vector<Vec3> p(count);
    for (std::size_t i = 0; i < count; ++i) p[i] = transformPoint(world, body.points[i].position);
    const auto at = [&](std::ptrdiff_t i) { return p[static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(i, 0, count - 1))]; };
    float distance = 0.0f;
    for (std::size_t seg = 0; seg + 1 < count; ++seg) {
        const Vec3 p0 = at(static_cast<std::ptrdiff_t>(seg) - 1);
        const Vec3 p1 = at(static_cast<std::ptrdiff_t>(seg));
        const Vec3 p2 = at(static_cast<std::ptrdiff_t>(seg) + 1);
        const Vec3 p3 = at(static_cast<std::ptrdiff_t>(seg) + 2);
        const float len = core::length(p2 - p1);
        const int steps = std::max(1, static_cast<int>(std::ceil(len / std::max(step, 0.1f))));
        const bool last = seg + 2 == count;
        for (int s = 0; s < steps + (last ? 1 : 0); ++s) {
            const float t = static_cast<float>(s) / static_cast<float>(steps);
            const float t2 = t * t;
            const float t3 = t2 * t;
            // Catmull-Rom uniforme y su derivada.
            const Vec3 pos = (p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 +
                              (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) *
                             0.5f;
            const Vec3 der = ((p2 - p0) + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * (2.0f * t) +
                              (p1 * 3.0f - p0 - p2 * 3.0f + p3) * (3.0f * t2)) *
                             0.5f;
            RiverSample sample;
            sample.position = pos;
            sample.width = body.points[seg].width + (body.points[seg + 1].width - body.points[seg].width) * t;
            const Vec3 flat{der.x, 0.0f, der.z};
            sample.tangent = core::length(flat) > 1e-5f ? core::normalize(flat) : Vec3{1.0f, 0.0f, 0.0f};
            if (!out.empty()) distance += core::length(pos - out.back().position);
            sample.distance = distance;
            out.push_back(sample);
        }
    }
    return out;
}

WaterSample sampleWater(const WaterBody& body, const core::Mat4& world, const Vec3& position, float time) {
    WaterSample result;
    const Vec3 origin = transformPoint(world, Vec3{});
    if (body.type == WaterType::River) {
        const std::vector<RiverSample> line = riverCenterline(body, world, 1.0f);
        float best = 1e30f;
        const RiverSample* nearest = nullptr;
        for (const RiverSample& s : line) {
            const float dx = position.x - s.position.x;
            const float dz = position.z - s.position.z;
            const float d = dx * dx + dz * dz;
            if (d < best) {
                best = d;
                nearest = &s;
            }
        }
        if (nearest == nullptr || std::sqrt(best) > nearest->width * 0.5f) return result;
        Vec3 n{};
        const Vec3 wave = gerstner(body, position.x - origin.x, position.z - origin.z, time, &n);
        result.inside = true;
        result.height = nearest->position.y + wave.y;
        result.normal = n;
        result.velocity = nearest->tangent * body.flow_speed;
        return result;
    }
    if (body.type == WaterType::Lake) {
        // Rectangulo girado con la entidad (solo el giro en Y cuenta).
        const Vec3 axis_x = core::normalize(Vec3{world.m[0][0], 0.0f, world.m[0][2]});
        const Vec3 local{position.x - origin.x, 0.0f, position.z - origin.z};
        const float u = core::dot(local, axis_x);
        const float v = core::dot(local, Vec3{-axis_x.z, 0.0f, axis_x.x});
        if (std::abs(u) > body.size.x * 0.5f || std::abs(v) > body.size.y * 0.5f) return result;
    }
    // La ola desplaza en horizontal: se busca el punto de la cuadricula que
    // acaba bajo `position` (unas iteraciones de punto fijo).
    // Relativo al origen del cuerpo de agua, como water_common.glsl (asi las
    // olas no saltan con el origen flotante).
    const float px = position.x - origin.x;
    const float pz = position.z - origin.z;
    float x0 = px;
    float z0 = pz;
    for (int i = 0; i < 4; ++i) {
        const Vec3 d = gerstner(body, x0, z0, time);
        x0 = px - d.x;
        z0 = pz - d.z;
    }
    Vec3 n{};
    const Vec3 d = gerstner(body, x0, z0, time, &n);
    const Vec3 ahead = gerstner(body, x0, z0, time + 0.05f);
    result.inside = true;
    result.height = origin.y + d.y;
    result.normal = n;
    result.velocity = (ahead - d) * (1.0f / 0.05f);
    return result;
}

float waterTime() { return g_time; }

namespace {
UnderwaterOverride g_underwater;
}

void setUnderwaterOverride(UnderwaterOverride test) { g_underwater = std::move(test); }
int underwaterOverride(const core::Vec3& camera) { return g_underwater ? g_underwater(camera) : -1; }

void advanceWaterTime(float delta_seconds) {
    g_time += std::clamp(delta_seconds, 0.0f, 0.25f);
    // Sin perder precision tras horas abiertas (las ondas se repiten mucho antes).
    if (g_time > 36000.0f) g_time -= 36000.0f;
}

void registerWaterComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("WaterBody") == nullptr) {
        registry.registerComponent<WaterBody>("WaterBody", "Agua", "Entorno");
    }
}

}  // namespace cramion::water
