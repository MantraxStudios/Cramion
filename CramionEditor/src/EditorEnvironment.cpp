// Ambiente (environment::Environment, como Enviro Sky 3): botones de los
// climas con su transicion (tambien en modo edicion, como vista previa), el
// estado en marcha (hora, estacion, temperatura, viento, humedad, nieve) y
// acciones rapidas (rayo, nevar ya, secar). En el Inspector del componente y
// en Ventana > Ambiente. GameObject > Ambiente lo crea (con cielo fisico).

#include "EditorApp.h"

#include <CramionCore/environment/Environment.h>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cramion::editor {

namespace {

namespace envns = cramion::environment;

// Entidad con el Environment activo (o el primero).
ecs::Entity environmentEntity(ecs::World& world) {
    const envns::Environment* env = envns::findEnvironment(world);
    if (env == nullptr) return {};
    for (const entt::entity handle : world.registry().view<envns::Environment>()) {
        if (&world.registry().get<envns::Environment>(handle) == env) return world.wrap(handle);
    }
    return {};
}

// Botones de los climas, estado y acciones. true si cambio algo.
bool drawEnvironmentControls(envns::Environment& env, bool playing) {
    bool changed = false;
    const envns::EnvironmentRuntime& rt = env.runtime;
    const envns::WeatherPreset now = envns::currentPreset(env);

    ImGui::SeparatorText("Clima");
    static float transition_seconds = -1.0f;
    if (transition_seconds < 0.0f) transition_seconds = env.transition_time;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int columns = avail > 360.0f ? 4 : (avail > 250.0f ? 3 : 2);
    const float width = (avail - ImGui::GetStyle().ItemSpacing.x * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    for (int i = 0; i < envns::kWeatherPresetCount; ++i) {
        const auto preset = static_cast<envns::WeatherPreset>(i);
        if (i % columns != 0) ImGui::SameLine();
        const bool is_target = env.weather == preset;
        if (is_target) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        ImGui::PushID(i);
        if (ImGui::Button(envns::presetLabel(preset), ImVec2(width, 0.0f))) {
            envns::setWeather(env, preset, transition_seconds);
            changed = true;
        }
        ImGui::PopID();
        if (is_target) ImGui::PopStyleColor();
    }
    ImGui::SetNextItemWidth(-120.0f);
    ImGui::SliderFloat("Transición##env", &transition_seconds, 0.0f, 60.0f, "%.1f s");
    ImGui::SetItemTooltip("Segundos de la transición al pulsar un clima (0 = al instante, también la humedad y la nieve).");
    if (rt.initialized && rt.transition < 1.0f) {
        char text[96];
        std::snprintf(text, sizeof(text), "%s -> %s  %.0f%%", envns::presetLabel(rt.from_preset), envns::presetLabel(rt.target),
                      rt.transition * 100.0f);
        ImGui::ProgressBar(rt.transition, ImVec2(-1.0f, 0.0f), text);
    } else {
        ImGui::TextDisabled("Ahora: %s%s", envns::presetLabel(now), playing ? "" : "  (vista previa en edición)");
    }

    ImGui::SeparatorText("Acciones");
    const float third = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
    if (ImGui::Button("Rayo", ImVec2(third, 0.0f))) {
        envns::strikeLightning(env, -1.0f);
    }
    ImGui::SetItemTooltip("Un rayo ahora (destello, trazo y trueno en Play).");
    ImGui::SameLine();
    if (ImGui::Button("Nieve acumulada", ImVec2(third, 0.0f))) {
        envns::setSnowCover(env, rt.snow_cover > 0.5f ? 0.0f : 1.0f);
    }
    ImGui::SetItemTooltip("Cubre de nieve (o la quita) al instante.");
    ImGui::SameLine();
    if (ImGui::Button("Mojar / secar", ImVec2(third, 0.0f))) {
        if (rt.wetness > 0.3f) envns::setWetness(env, 0.0f, 0.0f);
        else envns::setWetness(env, 1.0f, 0.7f);
    }
    ImGui::SetItemTooltip("Superficies mojadas y charcos al instante (o secas).");

    ImGui::SeparatorText("Estado");
    const int hours = static_cast<int>(env.time_of_day);
    const int minutes = std::clamp(static_cast<int>((env.time_of_day - static_cast<float>(hours)) * 60.0f), 0, 59);
    ImGui::TextDisabled("%02d:%02d  %d/%d  latitud %.1f", hours % 24, minutes, env.day, env.month, env.latitude);
    ImGui::TextDisabled("%s  %.1f °C%s", envns::seasonLabel(rt.initialized ? rt.season_now : env.season), rt.temperature,
                        rt.initialized && rt.to_sun.y < -0.05f ? "  (noche)" : "");
    ImGui::TextDisabled("Viento %.1f m/s hacia %.0f°", rt.wind_speed, std::fmod(rt.wind_direction + 360.0f, 360.0f));
    ImGui::TextDisabled("Lluvia %.0f%%  Nieve %.0f%%  Niebla %.4f", rt.current.rain * 100.0f, rt.current.snow * 100.0f,
                        rt.current.fog * env.fog_strength);
    ImGui::TextDisabled("Humedad %.0f%%  Charcos %.0f%%  Nieve acumulada %.0f%%", rt.wetness * 100.0f, rt.puddles * 100.0f,
                        rt.snow_cover * 100.0f);
    return changed;
}

}  // namespace

ecs::Entity EditorApp::createEnvironmentEntity() {
    envns::ensureEnvironment(world_);
    ecs::Entity entity = environmentEntity(world_);
    if (entity.valid()) {
        selectOnly(entity.uuid());
        revealInHierarchy(entity.uuid());
    }
    dirty_ = true;
    commit();
    return entity;
}

void EditorApp::drawEnvironmentInspector(ecs::Entity entity) {
    envns::Environment* env = entity.tryGet<envns::Environment>();
    if (env == nullptr) return;
    if (drawEnvironmentControls(*env, playing())) {
        dirty_ = true;
        if (!playing()) commit();
    }
}

void EditorApp::drawEnvironmentWindow() {
    if (!show_environment_window_) return;
    ImGui::SetNextWindowSize(ImVec2(380.0f, 470.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Ambiente", &show_environment_window_)) {
        ImGui::End();
        return;
    }
    envns::Environment* env = envns::findEnvironment(world_);
    if (env == nullptr) {
        ImGui::TextWrapped("La escena no tiene ambiente. Crea uno para tener clima con transiciones (lluvia, nieve, "
                           "tormentas con rayos, niebla), hora y fecha con el recorrido real del sol, estaciones y viento.");
        if (ImGui::Button("Crear ambiente", ImVec2(-1.0f, 0.0f))) createEnvironmentEntity();
    } else {
        if (drawEnvironmentControls(*env, playing())) {
            dirty_ = true;
            if (!playing()) commit();
        }
        ImGui::Spacing();
        if (ImGui::Button("Seleccionar (todos los ajustes en el Inspector)", ImVec2(-1.0f, 0.0f))) {
            const ecs::Entity entity = environmentEntity(world_);
            if (entity.valid()) {
                selectOnly(entity.uuid());
                revealInHierarchy(entity.uuid());
            }
        }
        ImGui::TextDisabled("Lluvia y nieve dibujadas: %u", renderer_.precipitationParticles());
    }
    ImGui::End();
}

}  // namespace cramion::editor
