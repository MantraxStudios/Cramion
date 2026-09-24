#include "CramionCore/physics/PhysicsSettings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>

namespace cramion::physics {

PhysicsSettings::PhysicsSettings() {
    layer_names[kLayerDefault] = "Default";
    layer_names[kLayerTransparentFX] = "TransparentFX";
    layer_names[kLayerIgnoreRaycast] = "Ignore Raycast";
    layer_names[kLayerWater] = "Water";
    layer_names[kLayerUI] = "UI";
    layer_collisions.fill(kAllLayers);
}

bool PhysicsSettings::layersCollide(int a, int b) const {
    if (a < 0 || a >= kLayerCount || b < 0 || b >= kLayerCount) {
        return false;
    }
    return (layer_collisions[static_cast<std::size_t>(a)] & layerBit(b)) != 0;
}

void PhysicsSettings::setLayersCollide(int a, int b, bool collide) {
    if (a < 0 || a >= kLayerCount || b < 0 || b >= kLayerCount) {
        return;
    }
    std::uint32_t& row_a = layer_collisions[static_cast<std::size_t>(a)];
    std::uint32_t& row_b = layer_collisions[static_cast<std::size_t>(b)];
    if (collide) {
        row_a |= layerBit(b);
        row_b |= layerBit(a);
    } else {
        row_a &= ~layerBit(b);
        row_b &= ~layerBit(a);
    }
}

int PhysicsSettings::layerIndex(std::string_view name) const {
    for (int i = 0; i < kLayerCount; ++i) {
        if (!layer_names[static_cast<std::size_t>(i)].empty() && layer_names[static_cast<std::size_t>(i)] == name) {
            return i;
        }
    }
    return -1;
}

std::uint32_t PhysicsSettings::mask(std::initializer_list<std::string_view> names) const {
    std::uint32_t result = 0;
    for (std::string_view name : names) {
        result |= layerBit(layerIndex(name));
    }
    return result;
}

std::string PhysicsSettings::layerLabel(int layer) const {
    if (layer < 0 || layer >= kLayerCount) {
        return "?";
    }
    const std::string& name = layer_names[static_cast<std::size_t>(layer)];
    return name.empty() ? "Capa " + std::to_string(layer) : name;
}

bool loadPhysicsSettings(const std::filesystem::path& file, PhysicsSettings& settings) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return false;
    }
    try {
        const nlohmann::json json = nlohmann::json::parse(in);
        PhysicsSettings loaded;
        if (const auto g = json.find("gravity"); g != json.end() && g->is_array() && g->size() == 3) {
            loaded.gravity = core::Vec3{(*g)[0].get<float>(), (*g)[1].get<float>(), (*g)[2].get<float>()};
        }
        loaded.fixed_step = json.value("fixed_step", loaded.fixed_step);
        loaded.max_substeps = json.value("max_substeps", loaded.max_substeps);
        loaded.collision_steps = json.value("collision_steps", loaded.collision_steps);
        loaded.queries_hit_triggers = json.value("queries_hit_triggers", loaded.queries_hit_triggers);
        loaded.sleep_threshold = json.value("sleep_threshold", loaded.sleep_threshold);
        loaded.max_bodies = json.value("max_bodies", loaded.max_bodies);
        loaded.worker_threads = json.value("worker_threads", loaded.worker_threads);
        if (const auto names = json.find("layers"); names != json.end() && names->is_array()) {
            for (std::size_t i = 0; i < names->size() && i < kLayerCount; ++i) {
                loaded.layer_names[i] = (*names)[i].get<std::string>();
            }
        }
        if (const auto matrix = json.find("collisions"); matrix != json.end() && matrix->is_array()) {
            for (std::size_t i = 0; i < matrix->size() && i < kLayerCount; ++i) {
                loaded.layer_collisions[i] = (*matrix)[i].get<std::uint32_t>();
            }
        }
        // Valores con sentido aunque el archivo se editara a mano.
        loaded.fixed_step = std::clamp(loaded.fixed_step, 1.0f / 1000.0f, 0.1f);
        loaded.max_substeps = std::clamp(loaded.max_substeps, 1, 32);
        loaded.collision_steps = std::clamp(loaded.collision_steps, 1, 8);
        settings = loaded;
        return true;
    } catch (const std::exception& error) {
        std::cerr << "[Fisica] No se pudo leer " << file.string() << ": " << error.what() << "\n";
        return false;
    }
}

bool savePhysicsSettings(const std::filesystem::path& file, const PhysicsSettings& settings) {
    nlohmann::json json;
    json["gravity"] = {settings.gravity.x, settings.gravity.y, settings.gravity.z};
    json["fixed_step"] = settings.fixed_step;
    json["max_substeps"] = settings.max_substeps;
    json["collision_steps"] = settings.collision_steps;
    json["queries_hit_triggers"] = settings.queries_hit_triggers;
    json["sleep_threshold"] = settings.sleep_threshold;
    json["max_bodies"] = settings.max_bodies;
    json["worker_threads"] = settings.worker_threads;
    json["layers"] = settings.layer_names;
    json["collisions"] = settings.layer_collisions;
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << json.dump(2);
    return static_cast<bool>(out);
}

namespace {
PhysicsSettings& projectSettingsStorage() {
    static PhysicsSettings settings;
    return settings;
}
}  // namespace

const PhysicsSettings& projectPhysicsSettings() {
    return projectSettingsStorage();
}

void setProjectPhysicsSettings(const PhysicsSettings& settings) {
    projectSettingsStorage() = settings;
}

}  // namespace cramion::physics
