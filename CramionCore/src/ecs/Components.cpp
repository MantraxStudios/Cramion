#include "CramionCore/ecs/Components.h"

#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/ecs/World.h"

#include <array>

namespace cramion::ecs {

// -----------------------------------------------------------------------------
// Transform
// -----------------------------------------------------------------------------

void Transform::reflect(PropertyVisitor& v) {
    v.field({"position", "Posicion"}, position, Vec3Kind::Position);
    v.field({"rotation", "Rotacion", "Grados, orden YXZ (como Unity)"}, euler, Vec3Kind::Euler);
    v.field({"scale", "Escala"}, scale, Vec3Kind::Scale);
}

// Se edito por reflexion (Inspector o carga): los grados mandan.
void Transform::onChanged(World& world, entt::entity entity) {
    Transform& t = world.registry().get<Transform>(entity);
    t.rotation = quatFromEulerDegrees(t.euler);
    world.markTransformDirty(entity);
}

// -----------------------------------------------------------------------------
// Renderizado
// -----------------------------------------------------------------------------

void MeshRenderer::reflect(PropertyVisitor& v) {
    v.asset({"model", "Modelo"}, model, assets::AssetType::Model);
    v.field({"part", "Pieza", "Indice de la pieza del modelo (cada malla importada)"}, part, 0, 4096);
    v.field({"visible", "Visible"}, visible);
    static constexpr std::array<const char*, 3> kShadows = {"No", "Si", "Solo sombras"};
    enumField(v, {"cast_shadows", "Proyecta sombras",
                  "Solo sombras: la camara no lo ve, pero su sombra si"},
              cast_shadows, kShadows);
    // El Inspector los dibuja aparte (con el nombre de cada hueco).
    if (v.wantsAllFields()) {
        listField(v, {"materials", "Materiales"}, materials, [](assets::AssetRef& ref, PropertyVisitor& item) {
            item.asset({"material", "Material"}, ref, assets::AssetType::Material);
        });
    }
}

void Animator::reflect(PropertyVisitor& v) {
    v.asset({"controller", "Controlador", "Animator Controller (.cranimator): su maquina de estados elige el clip"},
            controller, assets::AssetType::AnimatorController);
    v.field({"clip", "Animacion", "Indice del clip (-1 = pose de reposo)"}, clip, -1, 1024);
    v.field({"clip_name", "Nombre del clip", "Si no esta vacio, elige el clip por nombre"},
            clip_name);
    v.field({"speed", "Velocidad"}, speed, FloatRange{-4.0f, 4.0f, 0.01f, "%.2f"});
    v.field({"loop", "Bucle"}, loop);
    v.field({"playing", "Reproduciendo"}, playing);
    v.field({"time", "Tiempo"}, time, FloatRange{0.0f, 0.0f, 0.01f, "%.2f s"});
}

void Light::reflect(PropertyVisitor& v) {
    static constexpr std::array<const char*, 3> kTypes = {"Direccional", "Puntual", "Foco"};
    enumField(v, {"type", "Tipo"}, type, kTypes);
    v.field({"color", "Color"}, color, Vec3Kind::Color);
    v.field({"intensity", "Intensidad",
             "Direccional: multiplica al sol fisico. Puntual/foco: intensidad de la luz"},
            intensity, FloatRange{0.0f, 2000.0f, 0.1f, "%.2f"});
    const bool all = v.wantsAllFields();
    if (all || type != LightType::Directional) {
        v.field({"range", "Alcance", "Metros hasta los que llega la luz"}, range,
                FloatRange{0.1f, 500.0f, 0.1f, "%.1f m"});
    }
    if (all || type == LightType::Spot) {
        v.field({"inner_angle", "Angulo interior"}, inner_angle,
                FloatRange{1.0f, 89.0f, 0.5f, "%.1f°", true});
        v.field({"outer_angle", "Angulo exterior"}, outer_angle,
                FloatRange{1.0f, 89.0f, 0.5f, "%.1f°", true});
    }
    v.field({"cast_shadows", "Proyecta sombras"}, cast_shadows);
}

void Camera::reflect(PropertyVisitor& v) {
    v.field({"fov", "Campo de vision", "Vertical, en grados"}, fov,
            FloatRange{10.0f, 150.0f, 0.5f, "%.1f°", true});
    v.field({"near", "Plano cercano"}, near_plane, FloatRange{0.001f, 100.0f, 0.01f, "%.3f m"});
    v.field({"far", "Plano lejano"}, far_plane, FloatRange{1.0f, 100000.0f, 1.0f, "%.0f m"});
    v.field({"is_main", "Camara principal"}, is_main);
}

void Sky::reflect(PropertyVisitor& v) {
    v.asset({"environment", "Cielo HDR"}, environment, assets::AssetType::Environment);
    v.field({"use_hdr", "Usar el HDR", "Si no, cielo fisico (dispersion atmosferica)"}, use_hdr);
    v.field({"clouds", "Nubes volumetricas"}, clouds);
    v.field({"time_of_day", "Hora del dia", "Sin luz direccional ni HDR: posicion del sol"},
            time_of_day, FloatRange{0.0f, 24.0f, 0.05f, "%.2f h", true});
    v.field({"day_cycle", "Ciclo de dia"}, day_cycle);
}

void Weather::reflect(PropertyVisitor& v) {
    v.field({"rain", "Lluvia"}, rain);
    v.field({"wetness", "Humedad"}, wetness, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
    v.field({"puddles", "Charcos"}, puddles, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
    v.field({"flood", "Zona inundada"}, flood);
    if (v.wantsAllFields() || flood) {
        v.field({"flood_center", "Centro (x, z)"}, flood_center);
        v.field({"flood_radii", "Radios (x, z)"}, flood_radii);
    }
}

void Decal::reflect(PropertyVisitor& v) {
    static constexpr std::array<const char*, 3> kTypes = {"Estampa", "Charco", "Humedad"};
    enumField(v, {"type", "Tipo"}, type, kTypes);
    const bool all = v.wantsAllFields();
    v.field({"texture", "Textura", "Imagen dentro de Assets/ (PNG, JPG, TGA); vacia = solo color o forma"},
            texture);
    v.field({"color", "Color"}, color, Vec3Kind::Color);
    v.field({"opacity", "Opacidad"}, opacity, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
    if (all || type != DecalType::Stamp) {
        v.field({"amount", type == DecalType::Wet ? "Humedad" : "Nivel del agua"}, amount,
                FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.field({"follow_rain", "Solo con lluvia", "Aparece con la lluvia global y crece con sus charcos"},
                follow_rain);
    }
    if (all || type == DecalType::Stamp) {
        v.field({"roughness", "Rugosidad"}, roughness, FloatRange{0.04f, 1.0f, 0.01f, "%.2f", true});
        v.field({"roughness_amount", "Sustituir material", "Cuanto cambia la rugosidad/metalicidad de debajo"},
                roughness_amount, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.field({"metallic", "Metalicidad"}, metallic, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
    }
    v.field({"edge_softness", "Borde suave"}, edge_softness, FloatRange{0.0f, 0.5f, 0.01f, "%.2f", true});
    v.field({"max_angle", "Angulo maximo", "No pinta superficies mas inclinadas respecto al eje"}, max_angle,
            FloatRange{1.0f, 90.0f, 0.5f, "%.0f grados", true});
}

void PostProcessing::reflect(PropertyVisitor& v) {
    gfx::PostProcessSettings& s = settings;
    v.field({"priority", "Prioridad", "Con varios activos, manda el de mayor prioridad"},
            priority, -100, 100);
    const bool all = v.wantsAllFields();

    if (v.beginGroup("Exposicion")) {
        v.field({"auto_exposure", "Automatica"}, s.auto_exposure);
        v.field({"exposure_compensation", "Compensacion"}, s.exposure_compensation,
                FloatRange{-6.0f, 6.0f, 0.05f, "%+.2f EV", true});
        if (all || !s.auto_exposure) {
            v.field({"manual_exposure", "Exposicion manual"}, s.manual_exposure,
                    FloatRange{0.0f, 64.0f, 0.01f, "%.3f"});
        }
        if (all || s.auto_exposure) {
            v.field({"min_ev", "EV minimo"}, s.min_ev, FloatRange{-16.0f, 16.0f, 0.1f, "%.1f"});
            v.field({"max_ev", "EV maximo"}, s.max_ev, FloatRange{-16.0f, 16.0f, 0.1f, "%.1f"});
            v.field({"adaptation_speed_up", "Adaptacion a mas luz"}, s.adaptation_speed_up,
                    FloatRange{0.01f, 20.0f, 0.05f, "%.2f /s"});
            v.field({"adaptation_speed_down", "Adaptacion a menos luz"}, s.adaptation_speed_down,
                    FloatRange{0.01f, 20.0f, 0.05f, "%.2f /s"});
        }
        v.endGroup();
    }

    if (v.beginGroup("Tonemapping")) {
        static constexpr std::array<const char*, 3> kTonemappers = {"PBR Neutral", "ACES",
                                                                     "Ninguno"};
        enumField(v, {"tonemapper", "Modo"}, s.tonemapper, kTonemappers);
        v.endGroup();
    }

    if (v.beginGroup("Bloom")) {
        v.field({"bloom", "Activado"}, s.bloom);
        if (all || s.bloom) {
            v.field({"bloom_intensity", "Intensidad"}, s.bloom_intensity,
                    FloatRange{0.0f, 1.0f, 0.005f, "%.3f", true});
            v.field({"bloom_threshold", "Umbral", "Luminancia desde la que brilla (0 = todo)"},
                    s.bloom_threshold, FloatRange{0.0f, 10.0f, 0.01f, "%.2f"});
            v.field({"bloom_scatter", "Dispersion"}, s.bloom_scatter,
                    FloatRange{0.5f, 2.0f, 0.01f, "%.2f", true});
            v.field({"bloom_tint", "Tinte"}, s.bloom_tint, Vec3Kind::Color);
        }
        v.endGroup();
    }

    if (v.beginGroup("Gradacion de color")) {
        v.field({"temperature", "Temperatura"}, s.temperature,
                FloatRange{-100.0f, 100.0f, 0.5f, "%.0f", true});
        v.field({"tint", "Tinte"}, s.tint, FloatRange{-100.0f, 100.0f, 0.5f, "%.0f", true});
        v.field({"contrast", "Contraste"}, s.contrast, FloatRange{0.0f, 2.0f, 0.01f, "%.2f", true});
        v.field({"saturation", "Saturacion"}, s.saturation,
                FloatRange{0.0f, 2.0f, 0.01f, "%.2f", true});
        v.field({"vibrance", "Viveza"}, s.vibrance, FloatRange{-1.0f, 1.0f, 0.01f, "%.2f", true});
        v.field({"color_filter", "Filtro de color"}, s.color_filter, Vec3Kind::ColorHdr);
        v.field({"lift", "Lift (sombras)"}, s.lift, Vec3Kind::ColorHdr);
        v.field({"gamma", "Gamma (medios)"}, s.gamma, Vec3Kind::ColorHdr);
        v.field({"gain", "Gain (luces)"}, s.gain, Vec3Kind::ColorHdr);
        v.endGroup();
    }

    if (v.beginGroup("Vineta")) {
        v.field({"vignette", "Activada"}, s.vignette);
        if (all || s.vignette) {
            v.field({"vignette_intensity", "Intensidad"}, s.vignette_intensity,
                    FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
            v.field({"vignette_smoothness", "Suavidad"}, s.vignette_smoothness,
                    FloatRange{0.01f, 1.0f, 0.01f, "%.2f", true});
            v.field({"vignette_color", "Color"}, s.vignette_color, Vec3Kind::Color);
        }
        v.endGroup();
    }

    if (v.beginGroup("Lente")) {
        v.field({"chromatic_aberration", "Aberracion cromatica"}, s.chromatic_aberration,
                FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.field({"film_grain", "Grano"}, s.film_grain, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.endGroup();
    }

    if (v.beginGroup("Rayos de luz")) {
        v.field({"light_shafts", "Activados"}, s.light_shafts);
        if (all || s.light_shafts) {
            v.field({"light_shaft_intensity", "Intensidad"}, s.light_shaft_intensity,
                    FloatRange{0.0f, 4.0f, 0.01f, "%.2f", true});
        }
        v.endGroup();
    }

    if (v.beginGroup("Antialiasing")) {
        v.field({"fxaa", "FXAA"}, s.fxaa);
        v.endGroup();
    }

    if (v.beginGroup("Efectos")) {
        v.field({"ambient_occlusion", "Oclusion ambiental (SSAO)"}, s.ambient_occlusion);
        v.field({"global_illumination", "Luz rebotada (GI)"}, s.global_illumination);
        v.field({"reflections", "Reflejos"}, s.reflections);
        v.field({"volumetric_light", "Luz volumetrica"}, s.volumetric_light);
        if (all || s.volumetric_light) {
            v.field({"volumetric_density", "Densidad del polvo"}, s.volumetric_density,
                    FloatRange{0.0f, 0.2f, 0.001f, "%.3f /m", true});
            v.field({"volumetric_anisotropy", "Anisotropia", "0 = igual en todas direcciones, "
                                                             "cerca de 1 = hacia delante"},
                    s.volumetric_anisotropy, FloatRange{-0.9f, 0.95f, 0.01f, "%.2f", true});
        }
        v.endGroup();
    }
}

// -----------------------------------------------------------------------------
// Registro
// -----------------------------------------------------------------------------

void registerBuiltinComponents(ComponentRegistry& registry) {
    registry.registerComponent<Transform>("Transform", "Transform", "General",
                                          /*removable=*/false, /*addable=*/false);
    registry.registerComponent<MeshRenderer>("MeshRenderer", "Mesh Renderer", "Renderizado");
    registry.registerComponent<Animator>("Animator", "Animator", "Animacion");
    registry.registerComponent<Light>("Light", "Luz", "Renderizado");
    registry.registerComponent<Camera>("Camera", "Camara", "Renderizado");
    registry.registerComponent<Sky>("Sky", "Cielo", "Entorno");
    registry.registerComponent<Weather>("Weather", "Clima", "Entorno");
    registry.registerComponent<PostProcessing>("PostProcessing", "Post-procesado", "Renderizado");
    registry.registerComponent<Decal>("Decal", "Decal", "Renderizado");
}

}  // namespace cramion::ecs
