// Tabla de dimensiones y su ambiente (ver VoxelDimensions.h).
//
// Ambiente que aplica el motor al cambiar de dimension (el script puede
// cambiarlo despues con Environment / Sky / PostProcessing como siempre):
//
//               hora   clima         nubes  sol (color, x intensidad)  niebla (densidad, caida)  mar del motor
//   superficie  lo que habia en la escena antes de salir (se restaura tal cual)
//   abismo      18.9   despejado*    no     (1.00 0.45 0.25) x0.55     0.010 / 0.004 (roja)      apagado
//   cielo       10.5   despejado*    si     (1.00 0.96 0.88) x1.10     0.0012 / 0.02             apagado
//   cristal      0.0   despejado*    no     (0.55 0.62 1.00) x0.40     0.004 / 0.03              encendido, oscuro
//   (*) sin clima al azar ni lluvia; la hora no avanza.

#include "VoxelDimensions.h"

#include "CramionCore/ecs/World.h"

#include <array>

namespace cramion::voxel {

namespace {

const std::array<DimensionInfo, 4>& table() {
    static const std::array<DimensionInfo, 4> list = {{
        {"superficie", "Overworld", "superficie", "day"},
        {"abismo", "The Cinderdeep", "abismo", "abyss"},
        {"cielo", "The Driftreach", "cielo", "skyrealm"},
        {"cristal", "Glimmerwood", "cristal", "night"},
    }};
    return list;
}

struct Preset {
    float hours = 12.0f;
    bool clouds = false;
    core::Vec3 sun_color{1.0f, 1.0f, 1.0f};
    float sun_scale = 1.0f;
    float fog = 0.002f;
    float fog_falloff = 0.05f;
    bool sea = false;
    core::Vec3 sea_shallow{};
    core::Vec3 sea_deep{};
};

Preset presetFor(const std::string& dimension) {
    Preset p;
    if (dimension == "abismo") {
        p.hours = 18.9f;
        p.sun_color = {1.0f, 0.45f, 0.25f};
        p.sun_scale = 0.55f;
        p.fog = 0.010f;
        p.fog_falloff = 0.004f;
    } else if (dimension == "cielo") {
        p.hours = 10.5f;
        p.clouds = true;
        p.sun_color = {1.0f, 0.96f, 0.88f};
        p.sun_scale = 1.1f;
        p.fog = 0.0012f;
        p.fog_falloff = 0.02f;
    } else {  // cristal
        p.hours = 0.0f;
        p.sun_color = {0.55f, 0.62f, 1.0f};
        p.sun_scale = 0.4f;
        p.fog = 0.004f;
        p.fog_falloff = 0.03f;
        p.sea = true;
        p.sea_shallow = {0.10f, 0.30f, 0.45f};
        p.sea_deep = {0.01f, 0.03f, 0.08f};
    }
    return p;
}

template <typename T>
std::optional<std::pair<entt::entity, T>> firstOf(ecs::World& world) {
    for (const entt::entity e : world.registry().view<T>()) return std::make_pair(e, world.registry().get<T>(e));
    return std::nullopt;
}

template <typename T>
T* live(ecs::World& world, const std::optional<std::pair<entt::entity, T>>& saved) {
    if (!saved || !world.registry().valid(saved->first)) return nullptr;
    return world.registry().try_get<T>(saved->first);
}

void take(ecs::World& world, AmbienceSnapshot& s) {
    auto& reg = world.registry();
    s = AmbienceSnapshot{};
    s.taken = true;
    s.environment = firstOf<environment::Environment>(world);
    s.sky = firstOf<ecs::Sky>(world);
    for (const entt::entity e : reg.view<ecs::Light>()) {
        const ecs::Light& l = reg.get<ecs::Light>(e);
        if (l.type == ecs::LightType::Directional && world.wrap(e).activeInHierarchy()) {
            s.sun = std::make_pair(e, l);
            break;
        }
    }
    for (const entt::entity e : reg.view<ecs::PostProcessing>()) {
        if (reg.get<ecs::PostProcessing>(e).isGlobal()) {
            s.post = std::make_pair(e, reg.get<ecs::PostProcessing>(e));
            break;
        }
    }
    for (const entt::entity e : reg.view<water::WaterBody>()) {
        const water::WaterBody& b = reg.get<water::WaterBody>(e);
        if (b.type != water::WaterType::Ocean) continue;
        s.seas.push_back({e, world.wrap(e).activeSelf(), b});
    }
}

void restore(ecs::World& world, AmbienceSnapshot& s) {
    auto& reg = world.registry();
    if (auto* env = live(world, s.environment)) *env = s.environment->second;
    if (auto* sky = live(world, s.sky)) *sky = s.sky->second;
    if (auto* sun = live(world, s.sun)) *sun = s.sun->second;
    if (auto* post = live(world, s.post)) *post = s.post->second;
    for (const AmbienceSnapshot::Sea& sea : s.seas) {
        if (!reg.valid(sea.entity)) continue;
        if (auto* body = reg.try_get<water::WaterBody>(sea.entity)) *body = sea.body;
        world.wrap(sea.entity).setActive(sea.active);
    }
    s = AmbienceSnapshot{};
}

}  // namespace

const std::vector<DimensionInfo>& dimensions() {
    static const std::vector<DimensionInfo> list(table().begin(), table().end());
    return list;
}

const DimensionInfo* findDimension(const std::string& name) {
    for (const DimensionInfo& d : table()) {
        if (d.name == name) return &d;
    }
    return nullptr;
}

VoxelGenerator dimensionGenerator(const std::string& name, VoxelGenerator surface) {
    if (name == "abismo") return VoxelGenerator::Abyss;
    if (name == "cielo") return VoxelGenerator::Sky;
    if (name == "cristal") return VoxelGenerator::Crystal;
    return surface;
}

void applyDimensionAmbience(ecs::World& world, const std::string& dimension, AmbienceSnapshot& s) {
    if (dimension == kSurfaceDimension) {
        if (s.taken) restore(world, s);
        return;
    }
    if (!s.taken) take(world, s);
    const Preset p = presetFor(dimension);
    auto& reg = world.registry();
    if (auto* env = live(world, s.environment)) {
        const environment::Environment& before = s.environment->second;
        *env = before;
        env->random_weather = false;
        env->time_progress = false;
        env->time_of_day = p.hours;
        env->fog_strength = 0.0f;  // la niebla la pone el post-procesado
        environment::setWeather(*env, environment::WeatherPreset::Clear, 0.0f);
    }
    if (auto* sky = live(world, s.sky)) {
        *sky = s.sky->second;
        sky->clouds = p.clouds && s.sky->second.clouds;
        sky->day_cycle = false;
        sky->time_of_day = p.hours;
        if (p.clouds) sky->cloud_coverage = 0.3f;
    }
    if (auto* sun = live(world, s.sun)) {
        sun->color = p.sun_color;
        sun->intensity = s.sun->second.intensity * p.sun_scale;
    }
    if (auto* post = live(world, s.post)) {
        post->settings.fog_density = p.fog;
        post->settings.fog_height_falloff = p.fog_falloff;
    }
    for (const AmbienceSnapshot::Sea& sea : s.seas) {
        if (!reg.valid(sea.entity)) continue;
        world.wrap(sea.entity).setActive(p.sea && sea.active);
        if (auto* body = reg.try_get<water::WaterBody>(sea.entity)) {
            *body = sea.body;
            if (p.sea) {
                body->shallow_color = p.sea_shallow;
                body->deep_color = p.sea_deep;
            }
        }
    }
}

}  // namespace cramion::voxel
