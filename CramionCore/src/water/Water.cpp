#include "CramionCore/water/Water.h"

#include "CramionCore/ecs/World.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <map>
#include <mutex>

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
        if (all || type == WaterType::Ocean) {
            v.field({"wind_speed", "Viento",
                     "Velocidad del viento (m/s). Mayor que 0: la altura y la longitud de onda salen del espectro "
                     "JONSWAP con el viento y el fetch; 0 = las de arriba"},
                    wind_speed, FloatRange{0.0f, 40.0f, 0.1f, "%.1f m/s"});
            v.field({"fetch", "Fetch", "Distancia sobre la que sopla el viento (km): mas, olas mas grandes y largas"},
                    fetch, FloatRange{1.0f, 2000.0f, 1.0f, "%.0f km"});
            v.field({"swell_height", "Mar de fondo", "Olas largas que llegan de lejos (altura significativa)"},
                    swell_height, FloatRange{0.0f, 8.0f, 0.01f, "%.2f m"});
            v.field({"swell_wavelength", "Longitud del mar de fondo"}, swell_wavelength,
                    FloatRange{20.0f, 600.0f, 1.0f, "%.0f m"});
            v.field({"swell_direction", "Direccion del mar de fondo"}, swell_direction,
                    FloatRange{-180.0f, 180.0f, 1.0f, "%.0f°", true});
        }
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
        if (all || type == WaterType::Ocean) {
            v.field({"foam_persistence", "Duracion de la espuma", "Segundos que tarda en deshacerse la de las crestas"},
                    foam_persistence, FloatRange{0.1f, 20.0f, 0.05f, "%.2f s"});
        }
        if (!all) v.endGroup();
    }
    if (all || v.beginGroup("Bajo el agua")) {
        v.field({"god_rays", "Rayos de sol", "Haces de luz bajo la superficie (con las causticas y la sombra)"},
                god_rays, FloatRange{0.0f, 3.0f, 0.01f, "%.2f", true});
        v.field({"particles", "Particulas", "Motas en suspension que se ven buceando"}, particles,
                FloatRange{0.0f, 3.0f, 0.01f, "%.2f", true});
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
    // Extremos: puntos fantasma prolongando el primer y el ultimo tramo (con
    // el punto repetido, el centripeto dividiria por cero).
    const auto at = [&](std::ptrdiff_t i) -> Vec3 {
        const auto n = static_cast<std::ptrdiff_t>(count);
        if (i < 0) return p[0] * 2.0f - p[1];
        if (i >= n) return p[count - 1] * 2.0f - p[count - 2];
        return p[static_cast<std::size_t>(i)];
    };
    // Catmull-Rom centripeto (alfa 0.5, Barry-Goldman): a diferencia del
    // uniforme, no hace bucles ni se sale de los puntos cuando estan a
    // distancias muy distintas (lo normal al colocarlos a mano).
    const auto knot = [](const Vec3& a, const Vec3& b) {
        return std::max(std::sqrt(core::length(b - a)), 1e-3f);
    };
    for (std::size_t seg = 0; seg + 1 < count; ++seg) {
        const Vec3 p0 = at(static_cast<std::ptrdiff_t>(seg) - 1);
        const Vec3 p1 = at(static_cast<std::ptrdiff_t>(seg));
        const Vec3 p2 = at(static_cast<std::ptrdiff_t>(seg) + 1);
        const Vec3 p3 = at(static_cast<std::ptrdiff_t>(seg) + 2);
        const float t0 = 0.0f;
        const float t1 = t0 + knot(p0, p1);
        const float t2 = t1 + knot(p1, p2);
        const float t3 = t2 + knot(p2, p3);
        const float len = core::length(p2 - p1);
        const int steps = std::max(1, static_cast<int>(std::ceil(len / std::max(step, 0.1f))));
        const bool last = seg + 2 == count;
        for (int s = 0; s < steps + (last ? 1 : 0); ++s) {
            const float u = static_cast<float>(s) / static_cast<float>(steps);
            const float t = t1 + (t2 - t1) * u;
            const Vec3 a1 = p0 * ((t1 - t) / (t1 - t0)) + p1 * ((t - t0) / (t1 - t0));
            const Vec3 a2 = p1 * ((t2 - t) / (t2 - t1)) + p2 * ((t - t1) / (t2 - t1));
            const Vec3 a3 = p2 * ((t3 - t) / (t3 - t2)) + p3 * ((t - t2) / (t3 - t2));
            const Vec3 b1 = a1 * ((t2 - t) / (t2 - t0)) + a2 * ((t - t0) / (t2 - t0));
            const Vec3 b2 = a2 * ((t3 - t) / (t3 - t1)) + a3 * ((t - t1) / (t3 - t1));
            RiverSample sample;
            sample.position = b1 * ((t2 - t) / (t2 - t1)) + b2 * ((t - t1) / (t2 - t1));
            sample.width = body.points[seg].width + (body.points[seg + 1].width - body.points[seg].width) * u;
            out.push_back(sample);
        }
    }
    // Tangente (horizontal) de las muestras vecinas y distancia a lo largo.
    float distance = 0.0f;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const Vec3 prev = out[i > 0 ? i - 1 : i].position;
        const Vec3 next = out[i + 1 < out.size() ? i + 1 : i].position;
        const Vec3 flat{next.x - prev.x, 0.0f, next.z - prev.z};
        out[i].tangent = core::length(flat) > 1e-5f ? core::normalize(flat)
                                                    : (i > 0 ? out[i - 1].tangent : Vec3{1.0f, 0.0f, 0.0f});
        if (i > 0) distance += core::length(out[i].position - out[i - 1].position);
        out[i].distance = distance;
    }
    return out;
}

namespace {
WaterSample sampleWaterWith(const WaterBody& body, const core::Mat4& world, const Vec3& position, float time,
                            const std::vector<RiverSample>* river_line);
}  // namespace

WaterSample sampleWater(const WaterBody& body, const core::Mat4& world, const Vec3& position, float time) {
    return sampleWaterWith(body, world, position, time, nullptr);
}

WaterSample sampleWater(const WaterBody& body, const core::Mat4& world, const Vec3& position, float time,
                        const std::vector<RiverSample>& river_line) {
    return sampleWaterWith(body, world, position, time, &river_line);
}

namespace {
WaterSample sampleWaterWith(const WaterBody& body, const core::Mat4& world, const Vec3& position, float time,
                            const std::vector<RiverSample>* river_line) {
    WaterSample result;
    const Vec3 origin = transformPoint(world, Vec3{});
    if (body.type == WaterType::River) {
        std::vector<RiverSample> computed;
        if (river_line == nullptr) computed = riverCenterline(body, world, 1.0f);
        const std::vector<RiverSample>& line = river_line != nullptr ? *river_line : computed;
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
    if (body.type == WaterType::Ocean) {
        // Oceano FFT: la misma superficie que water_fft.comp.
        for (int i = 0; i < 4; ++i) {
            const Vec3 d = oceanDisplacement(body, x0, z0, time);
            x0 = px - d.x;
            z0 = pz - d.z;
        }
        Vec3 n{};
        Vec3 v{};
        const Vec3 d = oceanDisplacement(body, x0, z0, time, &n, &v);
        result.inside = true;
        result.height = origin.y + d.y;
        result.normal = n;
        result.velocity = v;
        return result;
    }
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
}  // namespace

// ---------------------------------------------------------------------------
// Oceano FFT (Tessendorf, "Simulating Ocean Water"; cascadas como Crest)
// ---------------------------------------------------------------------------
namespace {

constexpr int kN = kOceanResolution;
constexpr int kCpuN = 64;               // la CPU: todo lo de las 3 cascadas grandes cabe en 64 x 64
constexpr int kCpuCascades = 3;         // la ultima (rizado de centimetros) no mueve lo que flota
constexpr float kCascadeRatio = 3.7f;   // no entero: las cascadas no se repiten a la vez
constexpr float kCascadeLowCut = 4.0f;  // cada cascada empieza en 4 ondas por lado (sin repeticion visible)

struct SpectrumParams {
    float height, wavelength, wind, spread, speed, chop, swell_h, swell_l, swell_dir, wind_speed, fetch;
};

SpectrumParams spectrumParams(const WaterBody& b) {
    return SpectrumParams{b.wave_height,     b.wavelength,   b.wind_direction, b.wind_spread,
                          b.wave_speed,      b.steepness,    b.swell_height,   b.swell_wavelength,
                          b.swell_direction, b.wind_speed,   b.fetch};
}

std::uint64_t paramsKey(const SpectrumParams& p) {
    std::uint64_t h = 1469598103934665603ull;
    const float values[] = {p.height, p.wavelength, p.wind,      p.spread,     p.speed, p.chop,
                            p.swell_h, p.swell_l,   p.swell_dir, p.wind_speed, p.fetch};
    for (const float f : values) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        h = (h ^ bits) * 1099511628211ull;
    }
    return h;
}

// Numero aleatorio gaussiano fijo por modo (la misma mar cada vez).
std::uint32_t pcgHash(std::uint32_t v) {
    const std::uint32_t state = v * 747796405u + 2891336453u;
    const std::uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
std::complex<float> gaussianPair(std::uint32_t seed) {
    const std::uint32_t a = pcgHash(seed);
    const std::uint32_t b = pcgHash(a ^ 0x68E31DA4u);
    const float u1 = (static_cast<float>(a >> 8) + 0.5f) * (1.0f / 16777216.0f);
    const float u2 = static_cast<float>(b >> 8) * (1.0f / 16777216.0f);
    const float r = std::sqrt(-2.0f * std::log(u1));
    return {r * std::cos(2.0f * kPi * u2), r * std::sin(2.0f * kPi * u2)};
}

// JONSWAP S(omega) sin la constante alfa (se normaliza despues).
double jonswap(double omega, double omega_p) {
    if (omega <= 1e-4) return 0.0;
    const double sigma = omega <= omega_p ? 0.07 : 0.09;
    const double r = std::exp(-(omega - omega_p) * (omega - omega_p) / (2.0 * sigma * sigma * omega_p * omega_p));
    const double g2 = static_cast<double>(kGravity) * kGravity;
    return g2 / std::pow(omega, 5.0) * std::exp(-1.25 * std::pow(omega_p / omega, 4.0)) * std::pow(3.3, r);
}

// Reparto por direcciones: cos^p alrededor del viento (p segun la dispersion,
// la mitad de energia a +-spread) y, en las ondas cortas, algo en todas las
// direcciones (el rizado no va todo en fila). Normalizado en el circulo.
double directional(double delta, double spread, double k_over_kp) {
    const double s = std::clamp(spread, 0.02, 1.5);
    const double p = std::log(0.5) / std::log(std::cos(s));
    const double c = std::cos(delta);
    const double norm = std::sqrt(static_cast<double>(kPi)) * std::tgamma(p * 0.5 + 0.5) / std::tgamma(p * 0.5 + 1.0);
    const double cosine = c > 0.0 ? std::pow(c, p) / norm : 0.0;
    const double turbulence = std::clamp(0.04 + 0.08 * std::log2(std::max(k_over_kp, 1.0)), 0.04, 0.35);
    return cosine * (1.0 - turbulence) + turbulence / (2.0 * kPi);
}

std::shared_ptr<OceanSpectrum> buildSpectrum(const SpectrumParams& p) {
    auto out = std::make_shared<OceanSpectrum>();
    double omega_p = 0.0;
    double hs = p.height;
    if (p.wind_speed > 0.1f) {
        // JONSWAP limitado por el fetch (Hasselmann 1973); como mucho, mar
        // totalmente desarrollada (Pierson-Moskowitz).
        const double u = p.wind_speed;
        const double fetch = std::max(p.fetch, 0.5f) * 1000.0;
        const double x = kGravity * fetch / (u * u);
        omega_p = std::max(22.0 * (kGravity / u) * std::pow(x, -0.33), 0.855 * kGravity / u);
        const double alpha = std::max(0.076 * std::pow(x, -0.22), 0.0081);
        double m0 = 0.0;
        const double d = omega_p * 0.005;
        for (double w = omega_p * 0.3; w < omega_p * 8.0; w += d) m0 += alpha * jonswap(w, omega_p) * d;
        hs = 4.0 * std::sqrt(m0);
    } else {
        omega_p = std::sqrt(kGravity * 2.0 * kPi / std::max(p.wavelength, 0.5f));
    }
    const double k_p = omega_p * omega_p / kGravity;
    out->peak_wavelength = static_cast<float>(2.0 * kPi / k_p);
    out->significant_height = static_cast<float>(hs);
    out->choppiness = std::clamp(p.chop, 0.0f, 1.0f) * 1.6f;
    out->wave_speed = p.speed;
    const float base = std::clamp(out->peak_wavelength * 8.0f, 40.0f, 4000.0f);
    for (int c = 0; c < kOceanCascades; ++c) out->sizes[c] = base / std::pow(kCascadeRatio, static_cast<float>(c));

    const double wind = p.wind * kPi / 180.0;
    const double spread = p.spread * kPi / 180.0;
    const double swell_k = 2.0 * kPi / std::max(p.swell_l, 5.0f);
    const double swell_dir = p.swell_dir * kPi / 180.0;
    const double omega_step = 2.0 * kPi / kOceanPeriod;

    // Varianza de la superficie en cada modo, de las olas de viento y del mar
    // de fondo; luego se normaliza cada parte a su altura significativa.
    const std::size_t count = static_cast<std::size_t>(kOceanCascades) * kN * kN;
    std::vector<double> wind_part(count, 0.0);
    std::vector<double> swell_part(count, 0.0);
    double wind_total = 0.0;
    double swell_total = 0.0;
    for (int c = 0; c < kOceanCascades; ++c) {
        const double dk = 2.0 * kPi / out->sizes[c];
        const double k_low = c == 0 ? 0.5 * dk : kCascadeLowCut * dk;
        const double k_high = c + 1 < kOceanCascades ? kCascadeLowCut * 2.0 * kPi / out->sizes[c + 1] : 1e30;
        for (int n = 0; n < kN; ++n) {
            for (int m = 0; m < kN; ++m) {
                const int ux = m < kN / 2 ? m : m - kN;
                const int uz = n < kN / 2 ? n : n - kN;
                if (ux == -kN / 2 || uz == -kN / 2) continue;  // Nyquist: sin pareja simetrica
                const double kx = ux * dk;
                const double kz = uz * dk;
                const double k = std::sqrt(kx * kx + kz * kz);
                if (k < k_low || k >= k_high) continue;
                const std::size_t i = (static_cast<std::size_t>(c) * kN + n) * kN + m;
                const double omega = std::sqrt(kGravity * k);
                const double theta = std::atan2(kz, kx);
                // S(k) = S(omega) domega/dk / k * D(theta)
                double s = jonswap(omega, omega_p) * (kGravity / (2.0 * omega)) / k *
                           directional(theta - wind, spread, k / k_p);
                // Sin rizado capilar por debajo de ~1 cm.
                s *= std::exp(-(k * 0.01) * (k * 0.01));
                wind_part[i] = s * dk * dk;
                wind_total += wind_part[i];
                if (p.swell_h > 0.0f) {
                    const double rel = (k - swell_k) / (0.12 * swell_k);
                    const double c2 = std::cos(theta - swell_dir);
                    const double sw = std::exp(-0.5 * rel * rel) * (c2 > 0.0 ? std::pow(c2, 60.0) : 0.0) / k;
                    swell_part[i] = sw * dk * dk;
                    swell_total += swell_part[i];
                }
            }
        }
    }
    const double wind_scale = wind_total > 0.0 ? (hs * hs / 16.0) / wind_total : 0.0;
    const double swell_scale =
        swell_total > 0.0 ? (static_cast<double>(p.swell_h) * p.swell_h / 16.0) / swell_total : 0.0;

    // Amplitudes: h(k) y h(-k) se reparten la varianza (<|h0|^2> = var / 2).
    std::vector<std::complex<float>> h0(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double variance = wind_part[i] * wind_scale + swell_part[i] * swell_scale;
        if (variance <= 0.0) continue;
        h0[i] = gaussianPair(static_cast<std::uint32_t>(i) * 2654435761u + 12345u) *
                static_cast<float>(std::sqrt(variance * 0.5) / std::sqrt(2.0));
    }
    out->modes.assign(count * 8, 0.0f);
    for (int c = 0; c < kOceanCascades; ++c) {
        const double dk = 2.0 * kPi / out->sizes[c];
        double slope = 0.0;
        for (int n = 0; n < kN; ++n) {
            for (int m = 0; m < kN; ++m) {
                const std::size_t i = (static_cast<std::size_t>(c) * kN + n) * kN + m;
                const std::size_t mirror =
                    (static_cast<std::size_t>(c) * kN + (kN - n) % kN) * kN + (kN - m) % kN;
                const int ux = m < kN / 2 ? m : m - kN;
                const int uz = n < kN / 2 ? n : n - kN;
                const double kx = ux * dk;
                const double kz = uz * dk;
                const double k = std::sqrt(kx * kx + kz * kz);
                // Frecuencia redondeada: todo se repite cada kOceanPeriod s
                // (sin perder precision con el reloj).
                const double omega =
                    std::floor(std::sqrt(kGravity * k) * std::max(p.speed, 0.0f) / omega_step) * omega_step;
                const std::complex<float> a = h0[i];
                const std::complex<float> b = std::conj(h0[mirror]);
                float* o = &out->modes[i * 8];
                o[0] = a.real();
                o[1] = a.imag();
                o[2] = b.real();
                o[3] = b.imag();
                o[4] = static_cast<float>(omega);
                o[5] = static_cast<float>(kx);
                o[6] = static_cast<float>(kz);
                slope += k * k * (std::norm(a) + std::norm(b));
            }
        }
        out->slope_variance[c] = static_cast<float>(slope);
    }
    out->key = paramsKey(p);
    return out;
}

std::mutex g_spectrum_mutex;
std::map<std::uint64_t, std::shared_ptr<OceanSpectrum>> g_spectra;

// Estado en la CPU (la suma de las 3 cascadas grandes en un instante).
struct OceanCpuState {
    std::uint64_t key = 0;
    float time = -1.0f;
    // Por cascada: dx, dy, dz, vx, vy, vz en kCpuN x kCpuN.
    std::array<std::array<std::vector<float>, 6>, kCpuCascades> fields;
};
std::mutex g_state_mutex;
std::map<std::uint64_t, OceanCpuState> g_states;

// FFT inversa (sin dividir por N), en el sitio, de tamano potencia de 2.
void inverseFft(std::complex<float>* data, int n, int stride) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i * stride], data[j * stride]);
    }
    // Factores de giro de cada etapa, calculados una vez (con las mismas
    // cuentas: los mismos valores) y el producto complejo a mano: con
    // std::complex cada producto llamaba a __mulsc3 (casos de infinito y NaN
    // que aqui no hay) y los cos/sin se repetian en cada mariposa. El oceano
    // de la CPU se evalua cada frame: de 2.4 a ~0.6 ms.
    static const std::vector<std::complex<float>> kTwiddles = [] {
        std::vector<std::complex<float>> table;
        for (int len = 2; len <= kCpuN; len <<= 1) {
            const float angle = 2.0f * kPi / static_cast<float>(len);
            for (int j = 0; j < len / 2; ++j) table.emplace_back(std::cos(angle * j), std::sin(angle * j));
        }
        return table;
    }();
    std::size_t stage_start = 0;
    for (int len = 2; len <= n; len <<= 1) {
        const float angle = 2.0f * kPi / static_cast<float>(len);
        const bool tabled = n <= kCpuN;
        for (int i = 0; i < n; i += len) {
            for (int j = 0; j < len / 2; ++j) {
                const std::complex<float> w = tabled ? kTwiddles[stage_start + static_cast<std::size_t>(j)]
                                                     : std::complex<float>(std::cos(angle * j), std::sin(angle * j));
                const std::complex<float> u = data[(i + j) * stride];
                const std::complex<float> x = data[(i + j + len / 2) * stride];
                const std::complex<float> v(x.real() * w.real() - x.imag() * w.imag(),
                                            x.real() * w.imag() + x.imag() * w.real());
                data[(i + j) * stride] = std::complex<float>(u.real() + v.real(), u.imag() + v.imag());
                data[(i + j + len / 2) * stride] = std::complex<float>(u.real() - v.real(), u.imag() - v.imag());
            }
        }
        stage_start += static_cast<std::size_t>(len / 2);
    }
}

void inverseFft2d(std::vector<std::complex<float>>& grid) {
    for (int r = 0; r < kCpuN; ++r) inverseFft(grid.data() + r * kCpuN, kCpuN, 1);
    for (int c = 0; c < kCpuN; ++c) inverseFft(grid.data() + c, kCpuN, kCpuN);
}

void evaluate(const OceanSpectrum& spectrum, OceanCpuState& state, float time) {
    const float t = std::fmod(std::max(time, 0.0f), kOceanPeriod);
    std::vector<std::complex<float>> a(kCpuN * kCpuN), b(kCpuN * kCpuN), c(kCpuN * kCpuN);
    const std::complex<float> i_unit(0.0f, 1.0f);
    for (int cascade = 0; cascade < kCpuCascades; ++cascade) {
        std::fill(a.begin(), a.end(), std::complex<float>{});
        std::fill(b.begin(), b.end(), std::complex<float>{});
        std::fill(c.begin(), c.end(), std::complex<float>{});
        for (int n = 0; n < kN; ++n) {
            const int uz = n < kN / 2 ? n : n - kN;
            if (uz < -kCpuN / 2 || uz >= kCpuN / 2) continue;
            for (int m = 0; m < kN; ++m) {
                const int ux = m < kN / 2 ? m : m - kN;
                if (ux < -kCpuN / 2 || ux >= kCpuN / 2) continue;
                const float* o = &spectrum.modes[((static_cast<std::size_t>(cascade) * kN + n) * kN + m) * 8];
                if (o[0] == 0.0f && o[1] == 0.0f && o[2] == 0.0f && o[3] == 0.0f) continue;
                const float kx = o[5];
                const float kz = o[6];
                const float k = std::sqrt(kx * kx + kz * kz);
                // e^(-i w t), como water_fft.comp: la ola avanza a favor del viento.
                const std::complex<float> e(std::cos(o[4] * t), -std::sin(o[4] * t));
                const std::complex<float> h0(o[0], o[1]);
                const std::complex<float> h0m(o[2], o[3]);
                const std::complex<float> h = h0 * e + h0m * std::conj(e);
                const std::complex<float> dh = -i_unit * o[4] * (h0 * e - h0m * std::conj(e));
                const float ix = k > 0.0f ? kx / k : 0.0f;
                const float iz = k > 0.0f ? kz / k : 0.0f;
                // Desplazamiento horizontal: -i k/|k| h, por -choppiness (abajo)
                // para que vaya hacia las crestas.
                const std::complex<float> dx = -i_unit * ix * h;
                const std::complex<float> dz = -i_unit * iz * h;
                const std::complex<float> vx = -i_unit * ix * dh;
                const std::complex<float> vz = -i_unit * iz * dh;
                const int gi = ((uz + kCpuN) % kCpuN) * kCpuN + (ux + kCpuN) % kCpuN;
                a[gi] = dx + i_unit * dz;
                b[gi] = h + i_unit * dh;
                c[gi] = vx + i_unit * vz;
            }
        }
        inverseFft2d(a);
        inverseFft2d(b);
        inverseFft2d(c);
        auto& f = state.fields[cascade];
        for (auto& v : f) v.resize(kCpuN * kCpuN);
        // Negativo: con la FFT inversa e^(+i k x), -i k/|k| h aleja los puntos
        // de las crestas (ver water_fft.comp).
        const float chop = -spectrum.choppiness;
        for (int i = 0; i < kCpuN * kCpuN; ++i) {
            f[0][i] = a[i].real() * chop;
            f[2][i] = a[i].imag() * chop;
            f[1][i] = b[i].real();
            f[4][i] = b[i].imag();
            f[3][i] = c[i].real() * chop;
            f[5][i] = c[i].imag() * chop;
        }
    }
    state.time = time;
    state.key = spectrum.key;
}

float bilinear(const std::vector<float>& grid, float u, float v) {
    u -= std::floor(u);
    v -= std::floor(v);
    const float x = u * kCpuN;
    const float y = v * kCpuN;
    const int x0 = static_cast<int>(x) % kCpuN;
    const int y0 = static_cast<int>(y) % kCpuN;
    const int x1 = (x0 + 1) % kCpuN;
    const int y1 = (y0 + 1) % kCpuN;
    const float fx = x - std::floor(x);
    const float fy = y - std::floor(y);
    const float a = grid[y0 * kCpuN + x0] + (grid[y0 * kCpuN + x1] - grid[y0 * kCpuN + x0]) * fx;
    const float b = grid[y1 * kCpuN + x0] + (grid[y1 * kCpuN + x1] - grid[y1 * kCpuN + x0]) * fx;
    return a + (b - a) * fy;
}

// Desplazamiento y velocidad (el estado ya esta al dia).
void sampleState(const OceanSpectrum& spectrum, const OceanCpuState& state, float x, float z, Vec3& d, Vec3* v) {
    d = Vec3{};
    if (v != nullptr) *v = Vec3{};
    for (int c = 0; c < kCpuCascades; ++c) {
        const float u = x / spectrum.sizes[c];
        const float w = z / spectrum.sizes[c];
        const auto& f = state.fields[c];
        d.x += bilinear(f[0], u, w);
        d.y += bilinear(f[1], u, w);
        d.z += bilinear(f[2], u, w);
        if (v != nullptr) {
            v->x += bilinear(f[3], u, w);
            v->y += bilinear(f[4], u, w);
            v->z += bilinear(f[5], u, w);
        }
    }
}

}  // namespace

std::shared_ptr<const OceanSpectrum> oceanSpectrum(const WaterBody& body) {
    const SpectrumParams params = spectrumParams(body);
    const std::uint64_t key = paramsKey(params);
    std::lock_guard<std::mutex> lock(g_spectrum_mutex);
    if (const auto it = g_spectra.find(key); it != g_spectra.end()) return it->second;
    if (g_spectra.size() > 8) g_spectra.clear();  // se estan editando: no acumular
    auto spectrum = buildSpectrum(params);
    g_spectra[key] = spectrum;
    return spectrum;
}

Vec3 oceanDisplacement(const WaterBody& body, float x, float z, float time, Vec3* normal, Vec3* velocity) {
    const std::shared_ptr<const OceanSpectrum> spectrum = oceanSpectrum(body);
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_states.size() > 8 && g_states.find(spectrum->key) == g_states.end()) g_states.clear();
    OceanCpuState& state = g_states[spectrum->key];
    if (state.key != spectrum->key || state.time != time) evaluate(*spectrum, state, time);
    Vec3 d{};
    sampleState(*spectrum, state, x, z, d, velocity);
    if (normal != nullptr) {
        // Normal de la superficie desplazada (diferencias centradas).
        constexpr float e = 0.35f;
        Vec3 px0, px1, pz0, pz1;
        sampleState(*spectrum, state, x - e, z, px0, nullptr);
        sampleState(*spectrum, state, x + e, z, px1, nullptr);
        sampleState(*spectrum, state, x, z - e, pz0, nullptr);
        sampleState(*spectrum, state, x, z + e, pz1, nullptr);
        const Vec3 tx = Vec3{2.0f * e, 0.0f, 0.0f} + (px1 - px0);
        const Vec3 tz = Vec3{0.0f, 0.0f, 2.0f * e} + (pz1 - pz0);
        Vec3 n = core::cross(tz, tx);
        if (n.y < 0.0f) n = n * -1.0f;
        *normal = core::length(n) > 1e-6f ? core::normalize(n) : Vec3{0.0f, 1.0f, 0.0f};
    }
    return d;
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
    // Multiplo del periodo del oceano FFT (sin salto al dar la vuelta).
    if (g_time > 140.0f * kOceanPeriod) g_time -= 140.0f * kOceanPeriod;
}

void registerWaterComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("WaterBody") == nullptr) {
        registry.registerComponent<WaterBody>("WaterBody", "Agua", "Entorno");
    }
}

}  // namespace cramion::water
