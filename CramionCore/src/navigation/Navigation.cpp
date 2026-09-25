// Componentes de navegacion y ajustes del proyecto (Navigation.json). El
// sistema (Recast/Detour) esta en NavigationSystem.cpp.

#include "CramionCore/navigation/Navigation.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <type_traits>

namespace cramion::navigation {

using ecs::FloatRange;
using ecs::Vec3Kind;

void NavMeshBounds::reflect(ecs::PropertyVisitor& v) {
    v.field({"size", "Tamano", "Medidas del volumen (m); la malla solo se genera dentro"}, size, Vec3Kind::Scale);
}

void NavModifier::reflect(ecs::PropertyVisitor& v) {
    static constexpr std::array<const char*, 2> kAreas = {"Bloquear", "Evitar"};
    v.field({"size", "Tamano", "Medidas de la caja (m)"}, size, Vec3Kind::Scale);
    ecs::enumField(v, {"area", "Zona", "Bloquear: sin navegacion. Evitar: mas cara (se rodea si se puede)"}, area,
                   kAreas);
}

void NavAgent::reflect(ecs::PropertyVisitor& v) {
    v.field({"speed", "Velocidad"}, speed, FloatRange{0.0f, 50.0f, 0.05f, "%.2f m/s"});
    v.field({"acceleration", "Aceleracion"}, acceleration, FloatRange{0.1f, 200.0f, 0.1f, "%.1f m/s2"});
    v.field({"stopping_distance", "Distancia de parada"}, stopping_distance, FloatRange{0.0f, 10.0f, 0.01f, "%.2f m"});
    v.field({"rotate_to_movement", "Girar hacia el movimiento"}, rotate_to_movement);
    v.field({"turn_speed", "Velocidad de giro"}, turn_speed, FloatRange{0.0f, 2000.0f, 1.0f, "%.0f°/s"});
    v.field({"base_offset", "Altura del pivote", "Metros del pivote sobre la malla (0 = pies)"}, base_offset,
            FloatRange{-5.0f, 5.0f, 0.01f, "%.2f m"});
    if (v.wantsAllFields() || v.beginGroup("Evitacion", false)) {
        v.field({"avoidance", "Esquivar agentes"}, avoidance);
        v.field({"radius", "Radio"}, radius, FloatRange{0.05f, 5.0f, 0.01f, "%.2f m"});
        v.field({"height", "Altura"}, height, FloatRange{0.1f, 10.0f, 0.01f, "%.2f m"});
        if (!v.wantsAllFields()) v.endGroup();
    }
}

void registerNavigationComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("NavMeshBounds") == nullptr) {
        registry.registerComponent<NavMeshBounds>("NavMeshBounds", "Volumen de NavMesh", "Navegacion");
    }
    if (registry.find("NavModifier") == nullptr) {
        registry.registerComponent<NavModifier>("NavModifier", "Modificador de navegacion", "Navegacion");
    }
    if (registry.find("NavAgent") == nullptr) {
        registry.registerComponent<NavAgent>("NavAgent", "Agente de navegacion", "Navegacion");
    }
}

// --- Ajustes ---------------------------------------------------------------------

bool NavigationSettings::sameGeometry(const NavigationSettings& o) const {
    return agent_radius == o.agent_radius && agent_height == o.agent_height && max_step_height == o.max_step_height &&
           max_slope == o.max_slope && cell_size == o.cell_size && cell_height == o.cell_height &&
           tile_size == o.tile_size && min_region_area == o.min_region_area &&
           detail_sample_distance == o.detail_sample_distance && detail_max_error == o.detail_max_error;
}

namespace {

// Nombres del archivo <-> campos (los mismos en load y save).
template <typename Fn>
void forEachField(NavigationSettings& s, Fn&& fn) {
    fn("agent_radius", s.agent_radius);
    fn("agent_height", s.agent_height);
    fn("max_step_height", s.max_step_height);
    fn("max_slope", s.max_slope);
    fn("cell_size", s.cell_size);
    fn("cell_height", s.cell_height);
    fn("tile_size", s.tile_size);
    fn("min_region_area", s.min_region_area);
    fn("detail_sample_distance", s.detail_sample_distance);
    fn("detail_max_error", s.detail_max_error);
    fn("avoid_cost", s.avoid_cost);
    fn("max_agents", s.max_agents);
    fn("runtime_generation", s.runtime_generation);
    fn("build_threads", s.build_threads);
}

}  // namespace

bool loadNavigationSettings(const std::filesystem::path& file, NavigationSettings& settings) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
    if (json.is_discarded() || !json.is_object()) return false;
    forEachField(settings, [&](const char* key, auto& value) {
        const auto it = json.find(key);
        if (it == json.end()) return;
        using T = std::remove_reference_t<decltype(value)>;
        if constexpr (std::is_same_v<T, bool>) {
            if (it->is_boolean()) value = it->template get<bool>();
        } else {
            if (it->is_number()) value = it->template get<T>();
        }
    });
    // Valores con sentido aunque el archivo se editara a mano.
    settings.cell_size = std::max(settings.cell_size, 0.02f);
    settings.cell_height = std::max(settings.cell_height, 0.01f);
    settings.tile_size = std::clamp(settings.tile_size, 16, 256);
    settings.agent_radius = std::max(settings.agent_radius, 0.0f);
    settings.agent_height = std::max(settings.agent_height, settings.cell_height * 3.0f);
    settings.max_agents = std::clamp(settings.max_agents, 1, 4096);
    return true;
}

bool saveNavigationSettings(const std::filesystem::path& file, const NavigationSettings& settings) {
    nlohmann::json json = nlohmann::json::object();
    NavigationSettings copy = settings;
    forEachField(copy, [&](const char* key, auto& value) { json[key] = value; });
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << json.dump(2);
    return static_cast<bool>(out);
}

}  // namespace cramion::navigation
