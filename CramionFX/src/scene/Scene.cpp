#include "CramionFX/scene/Scene.h"

#include <CramionDM/Input.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>

namespace cramion::scene {

using core::Vec3;

void Scene::initialize() {
    createLights();
}

std::uint32_t Scene::overrideMaterial(std::uint32_t model, const std::string& name,
                                     float roughness, float metallic, float reflectance,
                                     float albedo_scale, const Vec3& base_color) {
    std::uint32_t changed = 0;
    for (asset::MaterialData& material : models_.at(model)->materials) {
        if (material.name == name) {
            material.roughness = roughness;
            material.metallic = metallic;
            material.reflectance = reflectance;
            if (base_color.x >= 0.0f || base_color.y >= 0.0f || base_color.z >= 0.0f) {
                material.base_color.x = std::max(base_color.x, 0.0f);
                material.base_color.y = std::max(base_color.y, 0.0f);
                material.base_color.z = std::max(base_color.z, 0.0f);
            }
            material.base_color.x *= albedo_scale;
            material.base_color.y *= albedo_scale;
            material.base_color.z *= albedo_scale;
            ++changed;
        }
    }
    return changed;
}

void Scene::setDirectXNormalMaps(std::uint32_t model, bool directx) {
    for (asset::MaterialData& material : models_.at(model)->materials) {
        material.normal_map_directx = directx;
    }
}

void Scene::placeCamera(const Vec3& position, const Vec3& target) {
    camera_.setPosition(position);
    camera_.lookAt(target);
}

void Scene::createLights() {
    lights_.points.clear();
    lights_.spots.clear();

    // Una sola luz: la direccional. updateSun() la mueve con la hora del dia
    // (sol de dia, luna de noche) y ajusta el cielo.
    lights_.sun.direction = core::normalize(Vec3{-0.45f, -0.80f, -0.40f});
    lights_.sun.color = {1.0f, 0.95f, 0.85f};
    lights_.sun.intensity = 2.4f;

    lights_.ambient.color = {0.45f, 0.58f, 0.78f};
    lights_.ambient.intensity = 0.20f;

    updateSun(0.0f);
}

void Scene::update(const dm::Input& input, float delta_seconds) {
    time_seconds_ += delta_seconds;

    camera_.update(input, delta_seconds);

    if (input.isKeyPressed(dm::Key::T) && !fixed_sun_) {
        day_cycle_enabled_ = !day_cycle_enabled_;
    }
    if (input.isKeyPressed(dm::Key::N) && !fixed_sun_) {
        sun_angle_ += core::kPi;  // Salta 12 horas: de dia a noche y viceversa.
    }

    updateSun(delta_seconds);

    if (animate_actors_) {
        for (Actor& actor : actors_) {
            actor.animator.update(delta_seconds);
        }
    }
}

std::uint32_t Scene::loadModel(const std::filesystem::path& path, bool force_static) {
    models_.push_back(std::make_unique<asset::ModelData>(asset::loadModel(path, force_static)));
    return static_cast<std::uint32_t>(models_.size() - 1);
}

std::uint32_t Scene::addModel(asset::ModelData model) {
    models_.push_back(std::make_unique<asset::ModelData>(std::move(model)));
    return static_cast<std::uint32_t>(models_.size() - 1);
}

void Scene::replaceModel(std::uint32_t index, asset::ModelData model) {
    if (index < models_.size()) {
        *models_[index] = std::move(model);
    }
}

void Scene::truncateModels(std::size_t count) {
    if (count < models_.size()) {
        models_.resize(count);
    }
}

void Scene::spawnActor(std::uint32_t model_index, float x, float z, float height, float yaw) {
    const asset::ModelData& model = *models_.at(model_index);

    Actor actor{};
    actor.model = model_index;
    actor.animator = anim::Animator(model);
    actor.animator.play(model.animations.empty() ? -1 : 0);

    // --- Tamano y pies del modelo en su primera pose ---
    // Cada formato y cada artista usa sus unidades (Mixamo exporta en
    // centimetros): se mide el modelo ya deformado y se escala a `height`.
    const anim::Aabb box = anim::skinnedBounds(model, actor.animator.boneMatrices());
    const Vec3 size = box.max - box.min;
    const float scale = (size.y > 1e-4f) ? height / size.y : 1.0f;

    // Centro de la huella del modelo en (x, z), pies en y = 0.
    const Vec3 pivot{(box.min.x + box.max.x) * 0.5f, box.min.y, (box.min.z + box.max.z) * 0.5f};
    const Vec3 position{x, 0.0f, z};
    const core::Quat rotation{0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};

    actor.transform = core::composeTrs(position, rotation, Vec3{scale}) * core::translate(-pivot);

    // Esfera generosa: la animacion puede estirar brazos y piernas mas alla
    // de la primera pose.
    actor.bounds_center = position + Vec3{0.0f, height * 0.5f, 0.0f};
    actor.bounds_radius = core::length(size) * scale * 0.75f + 0.5f;

    std::cout << "[Escena] Actor de " << model.name << " en (" << position.x << ", " << position.y
              << ", " << position.z << "), escala " << scale << " (tamano original " << size.x
              << " x " << size.y << " x " << size.z << ")\n";

    actors_.push_back(std::move(actor));
}

void Scene::spawnStatic(std::uint32_t model_index, const core::Mat4& transform) {
    const asset::ModelData& model = *models_.at(model_index);

    Actor actor{};
    actor.model = model_index;
    actor.animator = anim::Animator(model);
    actor.animator.play(model.animations.empty() ? -1 : 0);
    actor.transform = transform;

    // Esfera que envuelve el modelo en el mundo (para las luces locales).
    const anim::Aabb box = anim::skinnedBounds(model, actor.animator.boneMatrices());
    const Vec3 center = (box.min + box.max) * 0.5f;
    const core::Vec4 world_center = transform * core::Vec4{center.x, center.y, center.z, 1.0f};
    actor.bounds_center = Vec3{world_center.x, world_center.y, world_center.z};
    actor.bounds_radius = core::length(box.max - box.min) * 0.5f;

    std::cout << "[Escena] " << model.name << " colocado: caja (" << box.min.x << ", "
              << box.min.y << ", " << box.min.z << ") - (" << box.max.x << ", " << box.max.y
              << ", " << box.max.z << ")\n";

    actors_.push_back(std::move(actor));
}

namespace {

float smoothstep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

void Scene::setTimeOfDayHours(float hours) {
    // Inversa de timeOfDayHours(): las 6:00 son el angulo 0 (amanecer).
    sun_angle_ = (hours - 6.0f) / 24.0f * (2.0f * core::kPi);
}

void Scene::clear() {
    actors_.clear();
    models_.clear();
    fixed_sun_.reset();
}

void Scene::removeActor(std::size_t index) {
    if (index < actors_.size()) {
        actors_.erase(actors_.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

float Scene::timeOfDayHours() const {
    const float hours = 6.0f + sun_angle_ / (2.0f * core::kPi) * 24.0f;
    return std::fmod(hours, 24.0f);
}

void Scene::updateSun(float delta_seconds) {
    // Un dia completo dura 2*pi / velocidad = ~52 segundos.
    constexpr float kCycleSpeed = 0.12f;
    constexpr float kTwoPi = 2.0f * core::kPi;

    if (day_cycle_enabled_) {
        sun_angle_ += delta_seconds * kCycleSpeed;
    }
    sun_angle_ = std::fmod(sun_angle_, kTwoPi);
    if (sun_angle_ < 0.0f) {
        sun_angle_ += kTwoPi;
    }

    // --- Astros ---
    // El sol recorre un circulo completo de este a oeste, inclinado hacia +Z
    // para que nunca pase justo por el cenit. La luna va en el lado opuesto
    // del arco: sale cuando el sol se pone.
    const float c = std::cos(sun_angle_);
    const float s = std::sin(sun_angle_);
    // Con un cielo fotografiado, el sol esta donde esta en la foto.
    const Vec3 to_sun = fixed_sun_ ? core::normalize(*fixed_sun_)
                                   : core::normalize(Vec3{c, s, 0.35f});
    const Vec3 to_moon = fixed_sun_ ? -to_sun : core::normalize(Vec3{-c, -s, 0.35f});

    // Cuanta luz da cada astro. El sol se apaga justo al tocar el horizonte y
    // la luna no empieza hasta que el sol esta algo por debajo: en el cambio
    // de uno a otro ambos valen casi cero, asi que no se nota el salto de
    // direccion (ni en la luz ni en las sombras en cascada).
    const float sun_strength = smoothstep(-0.02f, 0.20f, to_sun.y);
    const float moon_strength =
        smoothstep(-0.02f, 0.20f, to_moon.y) * (1.0f - smoothstep(-0.15f, 0.05f, to_sun.y));

    const float daylight = smoothstep(-0.12f, 0.25f, to_sun.y);
    const float twilight = 1.0f - smoothstep(0.0f, 0.30f, std::abs(to_sun.y + 0.03f));

    lights_.sky.to_sun = to_sun;
    lights_.sky.to_moon = to_moon;
    lights_.sky.daylight = daylight;
    lights_.sky.twilight = twilight;

    // --- Luz direccional activa: sol o luna ---
    const bool sun_is_up = to_sun.y >= 0.0f;
    Vec3 to_light = sun_is_up ? to_sun : to_moon;
    // Con el astro rasante las cascadas se estiran hasta el infinito; como en
    // ese momento su intensidad es casi nula, basta con no dejarlo bajar mas.
    to_light.y = std::max(to_light.y, 0.08f);
    lights_.sun.direction = -core::normalize(to_light);

    if (sun_is_up) {
        // Rojizo al salir y ponerse, blanco a mediodia.
        const float high_sun = smoothstep(0.0f, 0.40f, to_sun.y);
        lights_.sun.intensity = 2.4f * sun_strength;
        lights_.sun.color =
            core::lerp(Vec3{1.0f, 0.50f, 0.25f}, Vec3{1.0f, 0.95f, 0.85f}, high_sun);
    } else {
        // Luz de luna: fria y tenue, pero suficiente para ver el relieve.
        lights_.sun.intensity = 0.35f * moon_strength;
        lights_.sun.color = Vec3{0.55f, 0.65f, 1.00f};
    }

    // --- Ambiente: cielo azul de dia, anaranjado al ocaso, azul oscuro de noche ---
    const Vec3 night_ambient{0.10f, 0.13f, 0.26f};
    const Vec3 day_ambient{0.45f, 0.58f, 0.78f};
    const Vec3 dusk_ambient{0.70f, 0.45f, 0.35f};

    lights_.ambient.color =
        core::lerp(core::lerp(night_ambient, day_ambient, daylight), dusk_ambient, twilight * 0.5f);
    // Es la irradiancia de todo el cielo: con la BRDF fisica y el ACES, las
    // zonas en sombra solo reciben esto, y por debajo de ~1/8 del sol quedan
    // negras en vez de azuladas.
    lights_.ambient.intensity = 0.08f + 0.72f * daylight;
}

}  // namespace cramion::scene
