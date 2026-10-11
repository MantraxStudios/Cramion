#include "CramionCore/environment/Environment.h"

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/World.h"

#include <CramionFX/scene/Scene.h>
#include <CramionFX/vk/FoliagePass.h>
#include <CramionFX/vk/PostProcessSettings.h>
#include <CramionFX/vk/TerrainPass.h>
#include <CramionFX/vk/VulkanRenderer.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <span>

namespace cramion::environment {

using core::Vec3;
using ecs::FloatRange;

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDegToRad = kPi / 180.0f;

constexpr std::array<const char*, kWeatherPresetCount> kPresetNames = {
    "Clear", "Cloudy", "Overcast", "Foggy", "LightRain", "Rain", "Storm", "LightSnow", "Snow", "Blizzard", "Sandstorm"};
// Lo que ve el usuario y la clave guardada en la escena (enumeration).
constexpr std::array<const char*, kWeatherPresetCount> kPresetLabels = {
    "Despejado", "Nubes", "Cubierto", "Niebla", "Llovizna", "Lluvia",
    "Tormenta", "Nevada ligera", "Nieve", "Ventisca", "Tormenta de arena"};
constexpr std::array<const char*, 4> kSeasonNames = {"Spring", "Summer", "Autumn", "Winter"};
constexpr std::array<const char*, 4> kSeasonLabels = {"Primavera", "Verano", "Otono", "Invierno"};

float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float random01(std::uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(state >> 8) * (1.0f / 16777216.0f);
}

// Ruido suave de una dimension (rachas, vaiven del viento): 0..1.
float noise1(double t, std::uint32_t seed) {
    const double i = std::floor(t);
    const float f = static_cast<float>(t - i);
    const auto hash = [seed](std::int64_t n) {
        std::uint32_t x = static_cast<std::uint32_t>(n) * 0x9E3779B1u ^ seed;
        x ^= x >> 16;
        x *= 0x7feb352du;
        x ^= x >> 15;
        x *= 0x846ca68bu;
        x ^= x >> 16;
        return static_cast<float>(x) * (1.0f / 4294967295.0f);
    };
    const float a = hash(static_cast<std::int64_t>(i));
    const float b = hash(static_cast<std::int64_t>(i) + 1);
    const float u = f * f * (3.0f - 2.0f * f);
    return a + (b - a) * u;
}

std::string normalizeName(std::string_view name) {
    std::string out;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) out += static_cast<char>(std::tolower(u));
        else if (u >= 0x80) out += static_cast<char>(c);  // acentos tal cual (se comparan abajo)
    }
    // "otono" con la enie en UTF-8 -> "otono".
    const std::string enie = "\xc3\xb1";
    for (std::size_t p = out.find(enie); p != std::string::npos; p = out.find(enie)) out.replace(p, enie.size(), "n");
    return out;
}

int daysInMonth(int month) {
    static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return kDays[std::clamp(month, 1, 12) - 1];
}

// Duracion al azar de un clima.
float randomDuration(Environment& env) {
    const float lo = std::max(env.min_duration, 5.0f);
    const float hi = std::max(env.max_duration, lo);
    return lo + (hi - lo) * random01(env.runtime.rng);
}

// Probabilidades del clima al azar por estacion (pesos).
WeatherPreset randomPreset(Environment& env, Season season, WeatherPreset avoid) {
    // Despejado, Nubes, Cubierto, Niebla, Llovizna, Lluvia, Tormenta,
    // Nevada ligera, Nieve, Ventisca, Arena.
    static constexpr float kWeights[4][kWeatherPresetCount] = {
        {24, 24, 10, 8, 15, 10, 6, 2, 1, 0, 0},   // primavera
        {40, 24, 5, 3, 9, 7, 12, 0, 0, 0, 0},     // verano
        {14, 20, 18, 15, 14, 12, 5, 2, 0, 0, 0},  // otono
        {16, 14, 16, 10, 3, 2, 0, 18, 15, 6, 0},  // invierno
    };
    const float* w = kWeights[static_cast<int>(season)];
    float total = 0.0f;
    for (int i = 0; i < kWeatherPresetCount; ++i) total += i == static_cast<int>(avoid) ? 0.0f : w[i];
    float pick = random01(env.runtime.rng) * total;
    for (int i = 0; i < kWeatherPresetCount; ++i) {
        if (i == static_cast<int>(avoid)) continue;
        pick -= w[i];
        if (pick <= 0.0f) return static_cast<WeatherPreset>(i);
    }
    return WeatherPreset::Clear;
}

// Superficies al instante con el clima de ahora.
void snapSurfaces(Environment& env) {
    EnvironmentRuntime& rt = env.runtime;
    rt.wetness = rt.current.wetness;
    rt.puddles = rt.current.puddles;
    rt.snow_cover = rt.current.snow > 0.01f && env.snow_accumulation ? std::min(1.0f, 0.55f + rt.current.snow) : 0.0f;
    rt.melt = 0.0f;
}

void startTransition(Environment& env, WeatherPreset preset, float seconds) {
    EnvironmentRuntime& rt = env.runtime;
    rt.from = rt.current;
    rt.from_preset = rt.target;
    rt.target = preset;
    rt.transition_seconds = std::max(seconds, 0.0f);
    if (seconds <= 0.0f) {
        rt.transition = 1.0f;
        rt.current = presetState(preset);
        snapSurfaces(env);
    } else {
        rt.transition = 0.0f;
    }
}

// Rayo: destello, trazo y su trueno.
void strike(Environment& env, const Vec3& camera, float distance) {
    EnvironmentRuntime& rt = env.runtime;
    const float d = distance > 0.0f ? distance : 350.0f + std::pow(random01(rt.rng), 0.7f) * 3800.0f;
    const float angle = random01(rt.rng) * 2.0f * kPi;
    const Vec3 ground{camera.x + std::cos(angle) * d, std::min(camera.y - 2.0f, 0.0f), camera.z + std::sin(angle) * d};
    rt.flash_position = ground;
    rt.flash_age = 0.0f;
    rt.flash_peak = std::max(env.lightning_brightness, 0.0f) * std::clamp(1.6f - d / 3200.0f, 0.35f, 1.4f) * 2.6f;

    // Thunder: el sonido llega con el retraso de la distancia (343 m/s).
    ThunderEvent thunder;
    thunder.time = rt.elapsed + static_cast<double>(d / 343.0f);
    thunder.volume = std::clamp(1.35f - d / 4200.0f, 0.2f, 1.0f);
    thunder.distance = d;
    rt.thunder.push_back(thunder);
    if (rt.thunder.size() > 8) rt.thunder.erase(rt.thunder.begin());

    // Trazo (como un rayo de verdad: canal principal que zigzaguea hacia el
    // suelo con ramas). Uno de cada tres es dentro de la nube: solo destello.
    rt.bolt.clear();
    if (!env.lightning_bolts || (distance <= 0.0f && random01(rt.rng) < 0.3f)) return;
    const float top = std::max(rt.cloud_bottom, 300.0f) + 150.0f;
    const Vec3 start{ground.x + (random01(rt.rng) - 0.5f) * 300.0f, top, ground.z + (random01(rt.rng) - 0.5f) * 300.0f};
    const int kSegments = 30;
    const float width = std::clamp(d * 0.0012f, 0.6f, 4.0f);
    std::vector<Vec3> channel;
    channel.push_back(start);
    Vec3 p = start;
    for (int i = 1; i <= kSegments; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSegments);
        const Vec3 straight = start + (ground - start) * t;
        const float step = (top - ground.y) / static_cast<float>(kSegments);
        Vec3 next = p + Vec3{(random01(rt.rng) - 0.5f) * step * 1.1f, -step, (random01(rt.rng) - 0.5f) * step * 1.1f};
        // Hacia el punto de impacto (cada vez mas al acercarse al suelo).
        next = next + (straight - next) * (0.25f + 0.5f * t);
        if (i == kSegments) next = ground;
        channel.push_back(next);
        p = next;
    }
    const auto add = [&](const Vec3& a, const Vec3& b, float w, float brightness) {
        if (rt.bolt.size() / 2 >= gfx::PrecipitationPass::kMaxBoltSegments) return;
        rt.bolt.push_back(core::Vec4{a.x, a.y, a.z, w});
        rt.bolt.push_back(core::Vec4{b.x, b.y, b.z, brightness});
    };
    for (std::size_t i = 0; i + 1 < channel.size(); ++i) add(channel[i], channel[i + 1], width, 1.0f);
    const int branches = 3 + static_cast<int>(random01(rt.rng) * 4.0f);
    for (int b = 0; b < branches; ++b) {
        std::size_t from = 2 + static_cast<std::size_t>(random01(rt.rng) * static_cast<float>(kSegments - 8));
        Vec3 q = channel[from];
        const float dir = random01(rt.rng) * 2.0f * kPi;
        const int length = 4 + static_cast<int>(random01(rt.rng) * 8.0f);
        const float step = (top - ground.y) / static_cast<float>(kSegments);
        for (int k = 0; k < length; ++k) {
            const Vec3 next = q + Vec3{std::cos(dir) * step * 0.7f + (random01(rt.rng) - 0.5f) * step * 0.6f,
                                       -step * (0.4f + 0.5f * random01(rt.rng)),
                                       std::sin(dir) * step * 0.7f + (random01(rt.rng) - 0.5f) * step * 0.6f};
            const float fade = 1.0f - static_cast<float>(k) / static_cast<float>(length);
            add(q, next, width * 0.55f, 0.45f * fade);
            q = next;
        }
    }
}

// Brillo del destello `age` segundos despues de caer: tres descargas por el
// mismo canal (parpadeo) y un resplandor que se apaga.
float flashEnvelope(float age) {
    static constexpr float kStrokes[3] = {0.0f, 0.09f, 0.24f};
    static constexpr float kStrength[3] = {1.0f, 0.65f, 0.8f};
    float f = 0.0f;
    for (int i = 0; i < 3; ++i) {
        if (age >= kStrokes[i]) f += kStrength[i] * std::exp(-(age - kStrokes[i]) * 20.0f);
    }
    return f + 0.12f * std::exp(-age * 3.0f);
}

void update(Environment& env, float dt, const Vec3& camera) {
    EnvironmentRuntime& rt = env.runtime;
    if (!rt.initialized) {
        rt.initialized = true;
        rt.target = rt.from_preset = env.weather;
        rt.current = rt.from = presetState(env.weather);
        rt.transition = 1.0f;
        rt.wind_direction = env.wind_direction;
        snapSurfaces(env);
        rt.next_change = randomDuration(env);
        rt.lightning_timer = 3.0f;
    }
    // El Inspector (o la escena cargada) cambio el clima: transicion.
    if (env.weather != rt.target) startTransition(env, env.weather, env.transition_time);
    rt.elapsed += dt;

    // --- Transicion ---
    if (rt.transition < 1.0f && dt > 0.0f) {
        rt.transition = std::min(1.0f, rt.transition + dt / std::max(rt.transition_seconds, 0.01f));
    }
    {
        const float t = smoothstep(0.0f, 1.0f, rt.transition);
        rt.current = lerpState(rt.from, presetState(rt.target), t);
    }
    const WeatherState& s = rt.current;

    // --- Hora y fecha ---
    if (env.time_progress && dt > 0.0f) {
        env.time_of_day += dt * 24.0f / (std::max(env.day_length, 0.05f) * 60.0f);
        while (env.time_of_day >= 24.0f) {
            env.time_of_day -= 24.0f;
            if (++env.day > daysInMonth(env.month)) {
                env.day = 1;
                env.month = env.month % 12 + 1;
            }
        }
    }
    env.time_of_day = std::clamp(env.time_of_day, 0.0f, 24.0f);
    env.day = std::clamp(env.day, 1, daysInMonth(env.month));
    env.month = std::clamp(env.month, 1, 12);
    const int doy = dayOfYear(env.day, env.month);
    const float latitude = std::clamp(env.latitude, -89.0f, 89.0f);
    rt.to_sun = sunDirection(env.time_of_day, doy, latitude);
    // Luna: al otro lado del cielo (luna llena) con la declinacion contraria.
    rt.to_moon = sunDirection(std::fmod(env.time_of_day + 12.0f, 24.0f), (doy + 182) % 365 + 1, latitude);

    // --- Estacion ---
    rt.season_now = env.season_from_date ? seasonForDate(env.day, env.month, latitude) : env.season;
    {
        // Tinte: verde en primavera y verano, amarillo/rojo en otono (pico a
        // finales de octubre en el norte), algo seco en invierno.
        float tint = 0.0f;
        if (env.season_from_date) {
            int d = doy;
            if (latitude < 0.0f) d = (doy + 182) % 365 + 1;
            if (d >= 244 && d < 305) tint = static_cast<float>(d - 244) / 61.0f;           // sep-oct
            else if (d >= 305 && d < 335) tint = 1.0f;                                      // noviembre
            else if (d >= 335 || d < 60) tint = 0.65f;                                       // invierno
            else if (d >= 60 && d < 120) tint = 0.65f * (1.0f - static_cast<float>(d - 60) / 60.0f);  // brota
        } else {
            static constexpr float kTint[4] = {0.0f, 0.0f, 1.0f, 0.65f};
            tint = kTint[static_cast<int>(env.season)];
        }
        rt.season_tint = tint;
    }

    // --- Temperatura ---
    {
        float base;
        if (env.season_from_date) {
            int d = doy;
            if (latitude < 0.0f) d = (doy + 182) % 365 + 1;
            base = 12.0f - 13.0f * std::cos(2.0f * kPi * static_cast<float>(d - 20) / 365.0f);
        } else {
            static constexpr float kBase[4] = {13.0f, 25.0f, 12.0f, -2.0f};
            base = kBase[static_cast<int>(env.season)];
        }
        base -= 0.35f * (std::abs(latitude) - 40.0f);
        base += 5.0f * std::clamp(rt.to_sun.y, -0.3f, 1.0f);
        float t = base + s.temperature;
        if (s.snow > 0.05f) t = std::min(t, -1.0f - 5.0f * s.snow);
        rt.temperature = t;
    }

    // --- Viento ---
    {
        const double clock = rt.elapsed;
        const float gust = noise1(clock * 0.35, 11u) * 0.7f + noise1(clock * 1.3, 23u) * 0.3f;
        const float base = std::max(s.wind * std::max(env.wind_strength, 0.0f), 0.0f);
        rt.wind_speed = base * std::max(0.1f, 1.0f + s.gusts * (gust * 1.3f - 0.4f));
        float direction = env.wind_direction;
        if (env.wind_wander) {
            direction += 25.0f * (noise1(clock * 0.015, 37u) - 0.5f) + 10.0f * s.gusts * (noise1(clock * 0.4, 41u) - 0.5f);
        }
        rt.wind_direction = direction;
        const float a = direction * kDegToRad;
        rt.wind = Vec3{std::cos(a), 0.0f, std::sin(a)} * rt.wind_speed;
    }

    // --- Superficies: se mojan, se secan, se nieva y se derrite ---
    if (dt > 0.0f) {
        const float sun = std::clamp(rt.to_sun.y, 0.0f, 1.0f) * s.sun;
        const float wet_target = std::max(s.wetness, rt.melt * 0.8f);
        if (rt.wetness < wet_target) rt.wetness = std::min(wet_target, rt.wetness + dt / 25.0f * (0.3f + s.rain));
        else rt.wetness = std::max(wet_target, rt.wetness - dt / 140.0f * (1.0f + sun));
        const float puddle_target = std::max(s.puddles, rt.melt * 0.35f);
        if (rt.puddles < puddle_target) rt.puddles = std::min(puddle_target, rt.puddles + dt / 60.0f * (0.3f + s.rain));
        else rt.puddles = std::max(puddle_target, rt.puddles - dt / 260.0f * (1.0f + sun));

        if (!env.snow_accumulation) {
            rt.snow_cover = 0.0f;
            rt.melt = std::max(0.0f, rt.melt - dt / 30.0f);
        } else if (s.snow > 0.01f) {
            rt.snow_cover = std::min(1.0f, rt.snow_cover + dt * s.snow * std::max(env.accumulation_speed, 0.0f) / 150.0f);
            rt.melt = std::max(0.0f, rt.melt - dt / 30.0f);
        } else if (rt.temperature > 0.5f && rt.snow_cover > 0.0f) {
            const float rate = std::max(env.melt_speed, 0.0f) * std::clamp(rt.temperature / 10.0f, 0.1f, 3.0f) *
                               (1.0f + s.rain * 2.0f) / 180.0f;
            rt.snow_cover = std::max(0.0f, rt.snow_cover - rate * dt);
            rt.melt = std::min(1.0f, rt.melt + dt / 20.0f) * (rt.snow_cover > 0.0f ? 1.0f : 0.0f);
            if (rt.snow_cover <= 0.0f) rt.melt = 0.6f;  // queda mojado
        } else {
            rt.melt = std::max(0.0f, rt.melt - dt / 90.0f);
        }
    }

    // --- Rayos ---
    if (dt > 0.0f) {
        const float rate = env.lightning ? s.lightning * std::max(env.lightning_frequency, 0.0f) : 0.0f;
        if (rt.strike_requested) {
            rt.strike_requested = false;
            strike(env, camera, rt.strike_distance);
        } else if (rate > 0.01f) {
            rt.lightning_timer -= dt;
            if (rt.lightning_timer <= 0.0f) {
                strike(env, camera, -1.0f);
                const float u = std::max(random01(rt.rng), 0.02f);
                rt.lightning_timer = std::clamp(-std::log(u) * 60.0f / rate, 1.5f, 120.0f);
            }
        } else {
            rt.lightning_timer = std::max(rt.lightning_timer, 2.0f);
        }
        rt.flash_age += dt;
    }
    rt.flash = rt.flash_age < 3.0f ? rt.flash_peak * flashEnvelope(rt.flash_age) : 0.0f;
    if (rt.flash_age > 0.6f) rt.bolt.clear();

    // --- Clima al azar ---
    if (env.random_weather && dt > 0.0f) {
        rt.next_change -= dt;
        if (rt.next_change <= 0.0f) {
            const WeatherPreset next = randomPreset(env, rt.season_now, rt.target);
            env.weather = next;
            startTransition(env, next, env.random_transition);
            rt.next_change = randomDuration(env);
        }
    }
}

}  // namespace

// -----------------------------------------------------------------------------
// Presets
// -----------------------------------------------------------------------------

WeatherState presetState(WeatherPreset preset) {
    WeatherState s;
    switch (preset) {
        case WeatherPreset::Clear:
            s.cloud_coverage = 0.12f;
            s.cloud_density = 0.8f;
            s.cloud_type = 0.45f;
            s.wind = 2.5f;
            s.gusts = 0.25f;
            break;
        case WeatherPreset::Cloudy:
            s.cloud_coverage = 0.48f;
            s.cloud_density = 1.0f;
            s.cloud_type = 0.5f;
            s.wind = 4.5f;
            s.gusts = 0.35f;
            s.sun = 0.95f;
            s.temperature = -1.0f;
            break;
        case WeatherPreset::Overcast:
            s.cloud_coverage = 0.9f;
            s.cloud_density = 1.5f;
            s.cloud_type = 0.15f;
            s.wind = 5.0f;
            s.gusts = 0.35f;
            s.sun = 0.55f;
            s.ambient = 0.85f;
            s.fog = 0.0025f;
            s.temperature = -3.0f;
            break;
        case WeatherPreset::Foggy:
            s.cloud_coverage = 0.7f;
            s.cloud_density = 1.1f;
            s.cloud_type = 0.05f;
            s.wind = 1.0f;
            s.gusts = 0.1f;
            s.fog = 0.028f;
            s.sky_fog = 0.85f;
            s.sun = 0.55f;
            s.ambient = 0.9f;
            s.wetness = 0.25f;
            s.temperature = -4.0f;
            break;
        case WeatherPreset::LightRain:
            s.cloud_coverage = 0.82f;
            s.cloud_density = 1.5f;
            s.cloud_type = 0.25f;
            s.wind = 4.5f;
            s.gusts = 0.4f;
            s.rain = 0.35f;
            s.wetness = 0.65f;
            s.puddles = 0.25f;
            s.fog = 0.004f;
            s.sky_fog = 0.2f;
            s.sun = 0.45f;
            s.ambient = 0.8f;
            s.temperature = -4.0f;
            break;
        case WeatherPreset::Rain:
            s.cloud_coverage = 0.93f;
            s.cloud_density = 1.9f;
            s.cloud_type = 0.35f;
            s.wind = 7.0f;
            s.gusts = 0.5f;
            s.rain = 0.75f;
            s.wetness = 0.9f;
            s.puddles = 0.55f;
            s.fog = 0.007f;
            s.sky_fog = 0.35f;
            s.sun = 0.3f;
            s.ambient = 0.7f;
            s.temperature = -5.0f;
            break;
        case WeatherPreset::Storm:
            s.cloud_coverage = 0.98f;
            s.cloud_density = 2.5f;
            s.cloud_type = 0.95f;
            s.wind = 14.0f;
            s.gusts = 0.85f;
            s.rain = 1.0f;
            s.wetness = 1.0f;
            s.puddles = 0.8f;
            s.lightning = 7.0f;
            s.fog = 0.009f;
            s.sky_fog = 0.45f;
            s.sun = 0.18f;
            s.ambient = 0.55f;
            s.temperature = -6.0f;
            break;
        case WeatherPreset::LightSnow:
            s.cloud_coverage = 0.8f;
            s.cloud_density = 1.3f;
            s.cloud_type = 0.2f;
            s.wind = 3.0f;
            s.gusts = 0.3f;
            s.snow = 0.35f;
            s.fog = 0.006f;
            s.sky_fog = 0.3f;
            s.sun = 0.55f;
            s.ambient = 0.9f;
            s.temperature = -12.0f;
            break;
        case WeatherPreset::Snow:
            s.cloud_coverage = 0.93f;
            s.cloud_density = 1.7f;
            s.cloud_type = 0.25f;
            s.wind = 5.0f;
            s.gusts = 0.4f;
            s.snow = 0.75f;
            s.fog = 0.013f;
            s.sky_fog = 0.55f;
            s.sun = 0.4f;
            s.ambient = 0.85f;
            s.temperature = -15.0f;
            break;
        case WeatherPreset::Blizzard:
            s.cloud_coverage = 1.0f;
            s.cloud_density = 2.2f;
            s.cloud_type = 0.3f;
            s.wind = 17.0f;
            s.gusts = 0.9f;
            s.snow = 1.0f;
            s.fog = 0.035f;
            s.sky_fog = 0.9f;
            s.sun = 0.25f;
            s.ambient = 0.8f;
            s.temperature = -22.0f;
            break;
        case WeatherPreset::Sandstorm:
            s.cloud_coverage = 0.25f;
            s.cloud_density = 0.7f;
            s.cloud_type = 0.1f;
            s.wind = 16.0f;
            s.gusts = 0.8f;
            s.dust = 1.0f;
            s.fog = 0.03f;
            s.sky_fog = 0.85f;
            s.sun = 0.45f;
            s.ambient = 0.85f;
            s.tint = Vec3{1.0f, 0.72f, 0.45f};
            s.temperature = 6.0f;
            break;
    }
    return s;
}

WeatherState lerpState(const WeatherState& a, const WeatherState& b, float t) {
    const auto mix = [t](float x, float y) { return x + (y - x) * t; };
    WeatherState s;
    s.cloud_coverage = mix(a.cloud_coverage, b.cloud_coverage);
    s.cloud_density = mix(a.cloud_density, b.cloud_density);
    s.cloud_type = mix(a.cloud_type, b.cloud_type);
    s.fog = mix(a.fog, b.fog);
    s.sky_fog = mix(a.sky_fog, b.sky_fog);
    s.wind = mix(a.wind, b.wind);
    s.gusts = mix(a.gusts, b.gusts);
    s.rain = mix(a.rain, b.rain);
    s.snow = mix(a.snow, b.snow);
    s.dust = mix(a.dust, b.dust);
    s.wetness = mix(a.wetness, b.wetness);
    s.puddles = mix(a.puddles, b.puddles);
    s.lightning = mix(a.lightning, b.lightning);
    s.sun = mix(a.sun, b.sun);
    s.ambient = mix(a.ambient, b.ambient);
    s.temperature = mix(a.temperature, b.temperature);
    s.tint = a.tint + (b.tint - a.tint) * t;
    return s;
}

const char* presetName(WeatherPreset preset) {
    const int i = std::clamp(static_cast<int>(preset), 0, kWeatherPresetCount - 1);
    return kPresetNames[static_cast<std::size_t>(i)];
}

const char* presetLabel(WeatherPreset preset) {
    const int i = std::clamp(static_cast<int>(preset), 0, kWeatherPresetCount - 1);
    return kPresetLabels[static_cast<std::size_t>(i)];
}

bool presetFromName(std::string_view name, WeatherPreset& out) {
    const std::string key = normalizeName(name);
    if (key.empty()) return false;
    for (int i = 0; i < kWeatherPresetCount; ++i) {
        if (key == normalizeName(kPresetNames[static_cast<std::size_t>(i)]) ||
            key == normalizeName(kPresetLabels[static_cast<std::size_t>(i)])) {
            out = static_cast<WeatherPreset>(i);
            return true;
        }
    }
    // Sinonimos.
    static const std::pair<const char*, WeatherPreset> kAliases[] = {
        {"sunny", WeatherPreset::Clear},       {"soleado", WeatherPreset::Clear},
        {"nublado", WeatherPreset::Cloudy},    {"partlycloudy", WeatherPreset::Cloudy},
        {"fog", WeatherPreset::Foggy},         {"neblina", WeatherPreset::Foggy},
        {"drizzle", WeatherPreset::LightRain}, {"lluvialigera", WeatherPreset::LightRain},
        {"heavyrain", WeatherPreset::Rain},    {"thunderstorm", WeatherPreset::Storm},
        {"tormentaelectrica", WeatherPreset::Storm}, {"nevadaligera", WeatherPreset::LightSnow},
        {"snowfall", WeatherPreset::Snow},     {"nevada", WeatherPreset::Snow},
        {"snowstorm", WeatherPreset::Blizzard}, {"tormentadenieve", WeatherPreset::Blizzard},
        {"arena", WeatherPreset::Sandstorm},   {"dust", WeatherPreset::Sandstorm},
        {"tormentadearena", WeatherPreset::Sandstorm},
    };
    for (const auto& [alias, preset] : kAliases) {
        if (key == alias) {
            out = preset;
            return true;
        }
    }
    return false;
}

const char* seasonName(Season season) { return kSeasonNames[static_cast<std::size_t>(std::clamp(static_cast<int>(season), 0, 3))]; }
const char* seasonLabel(Season season) { return kSeasonLabels[static_cast<std::size_t>(std::clamp(static_cast<int>(season), 0, 3))]; }

bool seasonFromName(std::string_view name, Season& out) {
    const std::string key = normalizeName(name);
    for (int i = 0; i < 4; ++i) {
        if (key == normalizeName(kSeasonNames[static_cast<std::size_t>(i)]) ||
            key == normalizeName(kSeasonLabels[static_cast<std::size_t>(i)])) {
            out = static_cast<Season>(i);
            return true;
        }
    }
    if (key == "fall") {
        out = Season::Autumn;
        return true;
    }
    return false;
}

// -----------------------------------------------------------------------------
// Componente
// -----------------------------------------------------------------------------

void Environment::reflect(ecs::PropertyVisitor& v) {
    const bool all = v.wantsAllFields();
    if (v.beginGroup("Clima")) {
        ecs::enumField(v, {"weather", "Clima", "Al cambiarlo pasa a el poco a poco (Transicion)"}, weather,
                       std::span<const char* const>(kPresetLabels));
        v.field({"transition_time", "Transicion", "Segundos que tarda en pasar de un clima a otro"}, transition_time,
                FloatRange{0.0f, 300.0f, 0.1f, "%.1f s"});
        v.field({"random_weather", "Clima al azar", "Cambia solo, con probabilidades segun la estacion"}, random_weather);
        if (all || random_weather) {
            v.field({"min_duration", "Duracion minima"}, min_duration, FloatRange{5.0f, 3600.0f, 1.0f, "%.0f s"});
            v.field({"max_duration", "Duracion maxima"}, max_duration, FloatRange{5.0f, 7200.0f, 1.0f, "%.0f s"});
            v.field({"random_transition", "Transicion al azar"}, random_transition, FloatRange{0.0f, 600.0f, 0.5f, "%.0f s"});
        }
        v.endGroup();
    }
    if (v.beginGroup("Hora y fecha")) {
        v.field({"control_time", "Controlar el sol", "El sol y la luna siguen la hora, la fecha y la latitud (cielo fisico)"},
                control_time);
        v.field({"time_of_day", "Hora", "0..24"}, time_of_day, FloatRange{0.0f, 24.0f, 0.02f, "%.2f h", true});
        v.field({"time_progress", "El tiempo avanza"}, time_progress);
        if (all || time_progress) {
            v.field({"day_length", "Duracion del dia", "Minutos reales que dura un dia de juego"}, day_length,
                    FloatRange{0.1f, 1440.0f, 0.1f, "%.1f min"});
        }
        v.field({"day", "Dia"}, day, 1, 31);
        v.field({"month", "Mes"}, month, 1, 12);
        v.field({"latitude", "Latitud", "Grados: 0 ecuador, 40 Madrid, -34 Buenos Aires. Cambia la altura del sol y la duracion de los dias"},
                latitude, FloatRange{-89.0f, 89.0f, 0.1f, "%.1f grados"});
        v.endGroup();
    }
    if (v.beginGroup("Estaciones", false)) {
        v.field({"season_from_date", "Estacion por la fecha"}, season_from_date);
        if (all || !season_from_date) {
            ecs::enumField(v, {"season", "Estacion"}, season, std::span<const char* const>(kSeasonLabels));
        }
        v.field({"vegetation_tint", "Color de la vegetacion", "La hierba y las hojas amarillean en otono"}, vegetation_tint);
        v.endGroup();
    }
    if (v.beginGroup("Viento", false)) {
        v.field({"wind_direction", "Direccion", "Grados (0 = hacia +X)"}, wind_direction, FloatRange{0.0f, 360.0f, 1.0f, "%.0f grados"});
        v.field({"wind_strength", "Fuerza", "Multiplica el viento de cada clima"}, wind_strength, FloatRange{0.0f, 4.0f, 0.01f, "%.2f"});
        v.field({"wind_wander", "Vaiven", "La direccion cambia despacio y con las rachas"}, wind_wander);
        v.endGroup();
    }
    if (v.beginGroup("Lluvia y nieve", false)) {
        v.field({"precipitation_density", "Densidad", "Multiplica las gotas y copos (rendimiento)"}, precipitation_density,
                FloatRange{0.0f, 3.0f, 0.01f, "%.2f"});
        v.field({"splashes", "Salpicaduras"}, splashes);
        v.field({"snow_accumulation", "Nieve acumulada", "La nieve cubre lo que mira hacia arriba y se derrite con calor"},
                snow_accumulation);
        if (all || snow_accumulation) {
            v.field({"accumulation_speed", "Velocidad de acumulacion"}, accumulation_speed, FloatRange{0.0f, 10.0f, 0.01f, "%.2f"});
            v.field({"melt_speed", "Velocidad de deshielo"}, melt_speed, FloatRange{0.0f, 10.0f, 0.01f, "%.2f"});
        }
        v.endGroup();
    }
    if (v.beginGroup("Tormenta", false)) {
        v.field({"lightning", "Rayos"}, lightning);
        if (all || lightning) {
            v.field({"lightning_frequency", "Frecuencia", "Multiplica los rayos por minuto de la tormenta"}, lightning_frequency,
                    FloatRange{0.0f, 10.0f, 0.01f, "%.2f"});
            v.field({"lightning_brightness", "Brillo"}, lightning_brightness, FloatRange{0.0f, 5.0f, 0.01f, "%.2f"});
            v.field({"lightning_bolts", "Trazo en el cielo"}, lightning_bolts);
        }
        v.endGroup();
    }
    if (v.beginGroup("Niebla y audio", false)) {
        v.field({"fog_strength", "Niebla", "Multiplica la niebla de cada clima (0 = la del post-procesado)"}, fog_strength,
                FloatRange{0.0f, 3.0f, 0.01f, "%.2f"});
        v.field({"ambient_audio", "Sonido ambiente", "Lluvia, viento y truenos sintetizados (en Play)"}, ambient_audio);
        if (all || ambient_audio) {
            v.field({"audio_volume", "Volumen"}, audio_volume, FloatRange{0.0f, 2.0f, 0.01f, "%.2f"});
            v.field({"thunder_volume", "Volumen de los truenos"}, thunder_volume, FloatRange{0.0f, 2.0f, 0.01f, "%.2f"});
        }
        v.endGroup();
    }
}

// -----------------------------------------------------------------------------
// API
// -----------------------------------------------------------------------------

Environment* findEnvironment(ecs::World& world) {
    Environment* found = nullptr;
    world.forEachDepthFirst([&](ecs::Entity e) {
        if (found == nullptr && e.activeInHierarchy()) found = e.tryGet<Environment>();
    });
    if (found == nullptr) {
        // Desactivada: tambien vale para scripts y MCP (se edita igual).
        for (const entt::entity handle : world.registry().view<Environment>()) {
            found = &world.registry().get<Environment>(handle);
            break;
        }
    }
    return found;
}

Environment& ensureEnvironment(ecs::World& world) {
    if (Environment* env = findEnvironment(world)) return *env;
    ecs::Entity holder;
    world.forEachDepthFirst([&](ecs::Entity e) {
        if (!holder.valid() && e.has<ecs::Sky>()) holder = e;
    });
    if (!holder.valid()) {
        holder = world.create("Ambiente");
        ecs::Sky& sky = holder.add<ecs::Sky>();
        sky.use_hdr = false;
    }
    return holder.add<Environment>();
}

void setWeather(Environment& env, WeatherPreset preset, float seconds) {
    if (!env.runtime.initialized) {
        env.weather = preset;
        if (seconds > 0.0f) return;  // la primera vez ya empieza asi
    }
    env.weather = preset;
    startTransition(env, preset, seconds);
}

void strikeLightning(Environment& env, float distance) {
    env.runtime.strike_requested = true;
    env.runtime.strike_distance = distance;
}

void setSnowCover(Environment& env, float cover) {
    env.runtime.snow_cover = std::clamp(cover, 0.0f, 1.0f);
    env.runtime.melt = 0.0f;
}

void setWetness(Environment& env, float wetness, float puddles) {
    env.runtime.wetness = std::clamp(wetness, 0.0f, 1.0f);
    env.runtime.puddles = std::clamp(puddles, 0.0f, 1.0f);
}

WeatherPreset currentPreset(const Environment& env) {
    if (!env.runtime.initialized) return env.weather;
    return env.runtime.transition >= 0.5f ? env.runtime.target : env.runtime.from_preset;
}

int dayOfYear(int day, int month) {
    int n = 0;
    for (int m = 1; m < std::clamp(month, 1, 12); ++m) n += daysInMonth(m);
    return n + std::clamp(day, 1, 31);
}

Season seasonForDate(int day, int month, float latitude) {
    const int d = dayOfYear(day, month);
    Season s;
    if (d >= 80 && d < 172) s = Season::Spring;
    else if (d >= 172 && d < 266) s = Season::Summer;
    else if (d >= 266 && d < 355) s = Season::Autumn;
    else s = Season::Winter;
    if (latitude < 0.0f) s = static_cast<Season>((static_cast<int>(s) + 2) % 4);
    return s;
}

Vec3 sunDirection(float hours, int day_of_year, float latitude) {
    // Declinacion del sol (aprox. de Cooper) y angulo horario (15 grados por
    // hora desde el mediodia solar).
    const float declination = -23.44f * kDegToRad * std::cos(2.0f * kPi / 365.0f * static_cast<float>(day_of_year + 10));
    const float hour_angle = (hours - 12.0f) * 15.0f * kDegToRad;
    const float phi = latitude * kDegToRad;
    const float east = -std::cos(declination) * std::sin(hour_angle);
    const float up = std::sin(phi) * std::sin(declination) + std::cos(phi) * std::cos(declination) * std::cos(hour_angle);
    const float north = std::cos(phi) * std::sin(declination) - std::sin(phi) * std::cos(declination) * std::cos(hour_angle);
    // Mundo: X este, Y arriba, -Z norte.
    return core::normalize(Vec3{east, up, -north});
}

// -----------------------------------------------------------------------------
// Aplicacion
// -----------------------------------------------------------------------------

EnvironmentFrame applyEnvironment(ecs::World& world, scene::Scene& scene, gfx::VulkanRenderer& renderer, float delta_seconds,
                                  gfx::PostProcessSettings post, const ecs::Light* directional, bool hdr_sky,
                                  float weather_wetness, float weather_puddles, bool weather_rain) {
    EnvironmentFrame frame;
    Environment* env = nullptr;
    ecs::Sky* sky = nullptr;
    // El primero de cada uno (en el orden de la jerarquia). Los componentes
    // antes que subir por la jerarquia; y con los dos encontrados ya no se mira.
    world.forEachDepthFirst([&](ecs::Entity e) {
        if (env != nullptr && sky != nullptr) return;
        Environment* found_env = env == nullptr ? e.tryGet<Environment>() : nullptr;
        ecs::Sky* found_sky = sky == nullptr ? e.tryGet<ecs::Sky>() : nullptr;
        if ((found_env == nullptr && found_sky == nullptr) || !e.activeInHierarchy()) return;
        if (found_env != nullptr) env = found_env;
        if (found_sky != nullptr) sky = found_sky;
    });
    if (env == nullptr) {
        if (scene.celestialSet()) scene.setCelestial(std::nullopt);
        if (renderer.precipitation().drawsSomething() || renderer.precipitation().snow_cover > 0.0f ||
            renderer.precipitation().season_tint > 0.0f || renderer.precipitation().sky_fog > 0.0f ||
            renderer.precipitation().flash > 0.0f) {
            renderer.setPrecipitation(gfx::PrecipitationSettings{});
        }
        return frame;
    }
    EnvironmentRuntime& rt = env->runtime;
    rt.cloud_bottom = sky != nullptr ? std::max(sky->cloud_height, 50.0f) : 1500.0f;
    update(*env, std::clamp(delta_seconds, 0.0f, 0.25f), scene.camera().position());
    const WeatherState& s = rt.current;

    // --- Nubes (las del Sky, con la cobertura y el viento del clima) ---
    gfx::CloudSettings clouds;
    if (sky != nullptr) {
        clouds.bottom = std::max(sky->cloud_height, 50.0f);
        clouds.thickness = std::max(sky->cloud_thickness, 100.0f);
        clouds.shadows = sky->cloud_shadows;
        clouds.shadow_strength = std::clamp(sky->cloud_shadow_strength, 0.0f, 1.0f);
    }
    clouds.coverage = std::clamp(s.cloud_coverage, 0.0f, 1.0f);
    clouds.density = std::max(s.cloud_density, 0.05f);
    clouds.type = std::clamp(s.cloud_type, 0.0f, 1.0f);
    // Las torres de tormenta crecen mas.
    if (s.cloud_type > 0.7f) clouds.thickness = std::max(clouds.thickness, 2800.0f + 3500.0f * (s.cloud_type - 0.7f) / 0.3f);
    clouds.wind_speed = rt.wind_speed * 1.6f + 3.0f;  // en altura sopla mas
    clouds.wind_direction = rt.wind_direction;
    renderer.setCloudSettings(clouds);
    renderer.setCloudsEnabled(sky == nullptr || sky->clouds);

    // --- Sol y luna: recorrido real (latitud y fecha) ---
    if (env->control_time && !hdr_sky) {
        scene.setFixedSun(std::nullopt);
        scene.setDayCycleEnabled(false);
        scene.setTimeOfDayHours(env->time_of_day);
        scene.setCelestial(scene::Scene::Celestial{rt.to_sun, rt.to_moon});
        scene::LightSet& lights = scene.lights();
        if (directional != nullptr) {
            lights.sun.color = lights.sun.color * directional->color;
            lights.sun.intensity *= directional->intensity;
        }
        if (sky != nullptr) {
            sky->time_of_day = env->time_of_day;  // el Inspector del cielo lo ve
            sky->day_cycle = false;
        }
    } else if (scene.celestialSet()) {
        scene.setCelestial(std::nullopt);
    }
    {
        scene::LightSet& lights = scene.lights();
        lights.sun.intensity *= std::max(s.sun, 0.0f);
        lights.sun.color = lights.sun.color * s.tint;
        lights.ambient.intensity *= std::max(s.ambient, 0.0f);
    }

    // --- Superficies mojadas y charcos (con los del Weather, si hay) ---
    const float wetness = std::max(rt.wetness, weather_rain ? weather_wetness : 0.0f);
    const float puddles = std::max(rt.puddles, weather_rain ? weather_puddles : 0.0f);
    renderer.setRainEnabled(wetness > 0.0f || puddles > 0.0f);
    renderer.setWeather(wetness, puddles);

    // --- Niebla (se suma a la del post-procesado) ---
    const float fog = std::max(s.fog * std::max(env->fog_strength, 0.0f), 0.0f);
    if (fog > post.fog_density) {
        const float weight = std::clamp(fog / 0.02f, 0.0f, 1.0f);
        post.fog_density = fog;
        post.fog_height_falloff = post.fog_height_falloff + (0.03f - post.fog_height_falloff) * weight;
        renderer.setPostProcess(post);
    }

    // --- Precipitacion, nieve, estacion y rayos ---
    gfx::PrecipitationSettings p;
    p.rain = std::clamp(s.rain, 0.0f, 1.0f);
    p.snow = std::clamp(s.snow, 0.0f, 1.0f);
    p.dust = std::clamp(s.dust, 0.0f, 1.0f);
    p.density = std::max(env->precipitation_density, 0.0f);
    p.splashes = env->splashes;
    p.wind = rt.wind;
    p.snow_cover = rt.snow_cover;
    p.snow_thickness = rt.snow_cover * 0.25f;
    p.snow_melt = rt.snow_cover > 0.0f ? rt.melt : 0.0f;
    p.season_tint = env->vegetation_tint ? rt.season_tint : 0.0f;
    p.flash = rt.flash;
    p.flash_position = rt.flash_position;
    if (!rt.bolt.empty() && rt.flash_age < 0.6f) {
        p.bolt = rt.bolt;
        p.bolt_brightness = std::clamp(flashEnvelope(rt.flash_age), 0.0f, 2.0f) * std::max(env->lightning_brightness, 0.0f);
    }
    p.sky_fog = std::clamp(s.sky_fog * std::max(env->fog_strength, 0.0f), 0.0f, 1.0f);
    renderer.setPrecipitation(std::move(p));

    frame.valid = true;
    frame.state = s;
    frame.wind_speed = rt.wind_speed;
    frame.wind_direction = rt.wind_direction;
    frame.snow_cover = rt.snow_cover;
    frame.season_tint = rt.season_tint;
    frame.vegetation_tint = env->vegetation_tint;
    return frame;
}

void applyToGrass(const EnvironmentFrame& frame, gfx::GrassDesc& grass) {
    if (!frame.valid) return;
    grass.wind_direction = frame.wind_direction;
    grass.wind *= std::clamp(0.35f + frame.wind_speed / 7.0f, 0.2f, 3.2f);
    if (frame.vegetation_tint && frame.season_tint > 0.0f) {
        const float t = std::clamp(frame.season_tint, 0.0f, 1.0f) * 0.75f;
        grass.tip_color = grass.tip_color + (grass.dry_color - grass.tip_color) * t;
        grass.base_color = grass.base_color + (grass.dry_color * 0.5f - grass.base_color) * (t * 0.5f);
    }
    if (frame.snow_cover > 0.0f) {
        // Bajo la nieve: mas baja, aplastada y blanca en las puntas.
        const float c = std::clamp(frame.snow_cover, 0.0f, 1.0f);
        grass.tip_color = grass.tip_color + (Vec3{0.86f, 0.88f, 0.92f} - grass.tip_color) * (c * 0.8f);
        grass.base_color = grass.base_color + (Vec3{0.55f, 0.57f, 0.6f} - grass.base_color) * (c * 0.5f);
        grass.height *= 1.0f - 0.6f * c;
        grass.bend += 0.25f * c;
        grass.dry_color = grass.dry_color + (Vec3{0.8f, 0.82f, 0.86f} - grass.dry_color) * (c * 0.7f);
    }
}

void applyToFoliage(const EnvironmentFrame& frame, gfx::FoliageSettings& foliage) {
    if (!frame.valid) return;
    foliage.wind *= std::clamp(0.4f + frame.wind_speed / 8.0f, 0.25f, 3.0f);
}

}  // namespace cramion::environment
