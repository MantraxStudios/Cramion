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

void Profiler::reflect(PropertyVisitor& v) {
    v.field({"show_fps", "FPS", "Fotogramas por segundo (media y el peor del ultimo medio segundo)"}, show_fps);
    v.field({"show_cpu", "CPU", "Milisegundos de trabajo de la CPU por frame y % de CPU del proceso"}, show_cpu);
    v.field({"show_gpu", "GPU", "Milisegundos de la GPU por frame (medidos en la GPU) y % ocupada"}, show_gpu);
    v.field({"show_memory", "Memoria", "RAM que usa el juego"}, show_memory);
    v.field({"show_graph", "Grafica", "Tiempo de cada frame (la linea es 60 FPS)"}, show_graph);
    static constexpr std::array<const char*, 4> kCorners = {"Arriba derecha", "Arriba izquierda", "Abajo derecha",
                                                            "Abajo izquierda"};
    enumField(v, {"corner", "Esquina"}, corner, kCorners);
    v.field({"scale", "Tamano"}, scale, FloatRange{0.5f, 3.0f, 0.05f, "%.2f"});
    v.field({"opacity", "Opacidad del fondo"}, opacity, FloatRange{0.0f, 1.0f, 0.01f, "%.2f"});
}

float PostProcessing::influence(const core::Mat4& world, const core::Vec3& point) const {
    const float w = std::clamp(weight, 0.0f, 1.0f);
    if (isGlobal() || w <= 0.0f) return w;
    float outside = 0.0f;  // metros de `point` a la forma (0 = dentro)
    if (shape == PostVolumeShape::Sphere) {
        const core::Vec3 center{world.m[3][0], world.m[3][1], world.m[3][2]};
        outside = std::max(core::length(point - center) - radius * maxAxisScale(world), 0.0f);
    } else {
        // Caja: el punto mas cercano en el espacio de la entidad, de vuelta
        // al mundo (asi la distancia es en metros aunque este escalada).
        const core::Vec3 local = transformPoint(core::inverse(world), point);
        const core::Vec3 half = size * 0.5f;
        const core::Vec3 clamped{std::clamp(local.x, -half.x, half.x), std::clamp(local.y, -half.y, half.y),
                                 std::clamp(local.z, -half.z, half.z)};
        outside = core::length(transformPoint(world, clamped) - point);
    }
    if (outside <= 0.0f) return w;
    if (blend_distance <= 1e-4f) return 0.0f;
    const float x = std::clamp(1.0f - outside / blend_distance, 0.0f, 1.0f);
    return w * x * x * (3.0f - 2.0f * x);  // suave al entrar y al salir
}

namespace {
float lerpf(float a, float b, float t) { return a + (b - a) * t; }
core::Vec3 lerpv(const core::Vec3& a, const core::Vec3& b, float t) { return a + (b - a) * t; }
}  // namespace

void blendPostProcess(gfx::PostProcessSettings& o, const gfx::PostProcessSettings& v, float t, std::uint32_t mask) {
    t = std::clamp(t, 0.0f, 1.0f);
    if (t <= 0.0f) return;
    const bool flip = t >= 0.5f;
    const auto f = [&](float& a, float b) { a = lerpf(a, b, t); };
    const auto c = [&](core::Vec3& a, const core::Vec3& b) { a = lerpv(a, b, t); };
    const auto b = [&](bool& a, bool bv) {
        if (flip) a = bv;
    };
    if (mask & kPostExposure) {
        b(o.auto_exposure, v.auto_exposure);
        f(o.exposure_compensation, v.exposure_compensation);
        f(o.manual_exposure, v.manual_exposure);
        f(o.min_ev, v.min_ev);
        f(o.max_ev, v.max_ev);
        f(o.adaptation_speed_up, v.adaptation_speed_up);
        f(o.adaptation_speed_down, v.adaptation_speed_down);
    }
    if ((mask & kPostTonemapping) && flip) o.tonemapper = v.tonemapper;
    if (mask & kPostBloom) {
        // Encender o apagar el bloom tambien es gradual: la intensidad va
        // desde/hacia 0.
        const float from = o.bloom ? o.bloom_intensity : 0.0f;
        const float to = v.bloom ? v.bloom_intensity : 0.0f;
        o.bloom_intensity = lerpf(from, to, t);
        o.bloom = o.bloom_intensity > 0.0f;
        f(o.bloom_threshold, v.bloom_threshold);
        f(o.bloom_scatter, v.bloom_scatter);
        c(o.bloom_tint, v.bloom_tint);
    }
    if (mask & kPostColor) {
        f(o.temperature, v.temperature);
        f(o.tint, v.tint);
        f(o.contrast, v.contrast);
        f(o.saturation, v.saturation);
        f(o.vibrance, v.vibrance);
        c(o.color_filter, v.color_filter);
        c(o.lift, v.lift);
        c(o.gamma, v.gamma);
        c(o.gain, v.gain);
    }
    if (mask & kPostVignette) {
        const float from = o.vignette ? o.vignette_intensity : 0.0f;
        const float to = v.vignette ? v.vignette_intensity : 0.0f;
        o.vignette_intensity = lerpf(from, to, t);
        o.vignette = o.vignette_intensity > 0.0f;
        f(o.vignette_smoothness, v.vignette_smoothness);
        c(o.vignette_color, v.vignette_color);
    }
    if (mask & kPostLens) {
        f(o.chromatic_aberration, v.chromatic_aberration);
        f(o.film_grain, v.film_grain);
    }
    if (mask & kPostLightShafts) {
        const float from = o.light_shafts ? o.light_shaft_intensity : 0.0f;
        const float to = v.light_shafts ? v.light_shaft_intensity : 0.0f;
        o.light_shaft_intensity = lerpf(from, to, t);
        o.light_shafts = o.light_shaft_intensity > 0.0f;
    }
    if (mask & kPostAntialiasing) b(o.fxaa, v.fxaa);
    if (mask & kPostEffects) {
        b(o.ambient_occlusion, v.ambient_occlusion);
        b(o.global_illumination, v.global_illumination);
        b(o.reflections, v.reflections);
        b(o.contact_shadows, v.contact_shadows);
        f(o.contact_shadow_length, v.contact_shadow_length);
        // La luz volumetrica se funde por su densidad.
        const float from = o.volumetric_light ? o.volumetric_density : 0.0f;
        const float to = v.volumetric_light ? v.volumetric_density : 0.0f;
        o.volumetric_density = lerpf(from, to, t);
        o.volumetric_light = o.volumetric_density > 0.0f;
        f(o.volumetric_anisotropy, v.volumetric_anisotropy);
    }
    if (mask & kPostPerformance) {
        b(o.lods, v.lods);
        f(o.lod_pixel_error, v.lod_pixel_error);
    }
}

void ProceduralAnimation::reflect(PropertyVisitor& v) {
    v.field({"enabled", "Activada"}, enabled);
    if (v.beginGroup("Huesos con muelle")) {
        listField(v, {"springs", "Cadenas", "Pelo, colas, capas, antenas: el hueso raiz y todos sus hijos se mueven con inercia"},
                  springs, [](SpringBoneChain& s, PropertyVisitor& item) {
                      item.field({"bone", "Hueso raiz"}, s.bone);
                      item.field({"stiffness", "Rigidez"}, s.stiffness, FloatRange{0.0f, 400.0f, 0.5f, "%.1f"});
                      item.field({"damping", "Amortiguacion"}, s.damping, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
                      item.field({"gravity", "Gravedad"}, s.gravity, FloatRange{0.0f, 30.0f, 0.1f, "%.1f m/s2"});
                      item.field({"radius", "Grosor"}, s.radius, FloatRange{0.0f, 1.0f, 0.005f, "%.3f m"});
                  });
        v.field({"body_colliders", "Chocar con el cuerpo", "No atraviesan la cabeza, el pecho ni la cadera (humanoides)"},
                body_colliders);
        v.endGroup();
    }
    if (v.beginGroup("Patas", false)) {
        listField(v, {"legs", "Patas", "Cada pie se queda en el suelo y da un paso cuando el cuerpo se aleja"}, legs,
                  [](ProceduralLeg& l, PropertyVisitor& item) {
                      item.field({"bone", "Pie"}, l.bone);
                      item.field({"group", "Grupo", "Las patas de un grupo pisan a la vez y se turnan con el otro (-1 = automatico)"},
                                 l.group, -1, 7);
                  });
        v.field({"step_distance", "Largo del paso"}, step_distance, FloatRange{0.01f, 5.0f, 0.01f, "%.2f m"});
        v.field({"step_height", "Altura del paso"}, step_height, FloatRange{0.0f, 2.0f, 0.01f, "%.2f m"});
        v.field({"step_duration", "Duracion del paso"}, step_duration, FloatRange{0.02f, 2.0f, 0.01f, "%.2f s"});
        v.field({"step_overshoot", "Adelantar el pie", "Segun la velocidad (0 = pisa donde estaba)"}, step_overshoot,
                FloatRange{0.0f, 1.5f, 0.01f, "%.2f", true});
        v.field({"adjust_body", "Mover el cuerpo", "Sube, baja y se inclina con el suelo que pisan las patas"}, adjust_body);
        v.field({"body_weight", "Peso del cuerpo"}, body_weight, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.endGroup();
    }
    if (v.beginGroup("Capas", false)) {
        v.field({"breathing", "Respirar", "El pecho sube y baja (humanoides)"}, breathing);
        v.field({"breath_rate", "Respiraciones por minuto"}, breath_rate, FloatRange{1.0f, 60.0f, 0.5f, "%.1f"});
        v.field({"breath_amount", "Cuanto"}, breath_amount, FloatRange{0.0f, 15.0f, 0.1f, "%.1f grados"});
        v.field({"lean", "Inclinarse", "Al acelerar, frenar o girar la columna se inclina (humanoides)"}, lean);
        v.field({"lean_amount", "Inclinacion maxima"}, lean_amount, FloatRange{0.0f, 45.0f, 0.5f, "%.1f grados"});
        listField(v, {"noise", "Ruido en huesos", "Balanceo suave: antenas, colas, respiracion de criaturas"}, noise,
                  [](ProceduralNoise& n, PropertyVisitor& item) {
                      item.field({"bone", "Hueso"}, n.bone);
                      item.field({"amplitude", "Amplitud"}, n.amplitude, FloatRange{0.0f, 90.0f, 0.1f, "%.1f grados"});
                      item.field({"frequency", "Frecuencia"}, n.frequency, FloatRange{0.01f, 10.0f, 0.01f, "%.2f Hz"});
                  });
        v.endGroup();
    }
}

void InverseKinematics::reflect(PropertyVisitor& v) {
    v.field({"enabled", "Activada"}, enabled);
    const auto limb = [&](const char* group, const char* key, IKLimb& l) {
        if (!v.beginGroup(group, false)) return;
        const std::string k(key);
        v.entity({(k + "_target").c_str(), "Objetivo", "Entidad a la que llega (arrastrala desde la Jerarquia)"}, l.target);
        v.entity({(k + "_hint").c_str(), "Codo / rodilla hacia", "Entidad hacia la que se dobla (opcional)"}, l.hint);
        v.field({(k + "_weight").c_str(), "Peso"}, l.weight, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.field({(k + "_rotation").c_str(), "Copiar giro", "La mano o el pie gira como el objetivo"}, l.match_rotation);
        v.endGroup();
    };
    limb("Mano izquierda", "left_hand", left_hand);
    limb("Mano derecha", "right_hand", right_hand);
    limb("Pie izquierdo", "left_foot", left_foot);
    limb("Pie derecho", "right_foot", right_foot);
    if (v.beginGroup("Mirar", false)) {
        v.entity({"look_at", "Mirar a", "La cabeza sigue a esta entidad"}, look_at);
        v.field({"look_weight", "Peso"}, look_weight, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.field({"look_max_angle", "Angulo maximo"}, look_max_angle, FloatRange{0.0f, 180.0f, 1.0f, "%.0f grados"});
        v.endGroup();
    }
    if (v.beginGroup("Pies en el suelo")) {
        v.field({"foot_grounding", "Activado", "Cada pie se apoya en el suelo que tiene debajo (escaleras, pendientes)"},
                foot_grounding);
        v.field({"grounding_weight", "Peso"}, grounding_weight, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.field({"max_step", "Escalon maximo"}, max_step, FloatRange{0.0f, 2.0f, 0.01f, "%.2f m"});
        v.field({"align_feet", "Inclinar el pie", "El pie sigue la pendiente del suelo"}, align_feet);
        v.endGroup();
    }
    listField(v, {"chains", "Cadenas (otros esqueletos)", "Dos huesos que llegan a un objetivo: el hueso final y sus dos padres"},
              chains, [](IKChain& c, PropertyVisitor& item) {
                  item.field({"bone", "Hueso final"}, c.bone);
                  item.entity({"target", "Objetivo"}, c.target);
                  item.entity({"hint", "Doblar hacia"}, c.hint);
                  item.field({"weight", "Peso"}, c.weight, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
              });
}

void PostProcessing::reflect(PropertyVisitor& v) {
    gfx::PostProcessSettings& s = settings;
    const bool all = v.wantsAllFields();

    if (v.beginGroup("Volumen")) {
        static constexpr std::array<const char*, 3> kShapes = {"Global", "Caja", "Esfera"};
        enumField(v, {"shape", "Forma", "Global: toda la escena. Caja o esfera: solo con la camara dentro"}, shape,
                  kShapes);
        v.field({"priority", "Prioridad", "Se mezclan de menor a mayor: el de mayor prioridad manda"}, priority, -100,
                100);
        v.field({"weight", "Peso", "Cuanto cuenta este volumen (0..1)"}, weight,
                FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        if (all || shape == PostVolumeShape::Box) {
            v.field({"size", "Tamano", "De la caja (lo escala tambien la entidad)"}, size, Vec3Kind::Scale);
        }
        if (all || shape == PostVolumeShape::Sphere) {
            v.field({"radius", "Radio"}, radius, FloatRange{0.01f, 10000.0f, 0.05f, "%.2f m"});
        }
        if (all || shape != PostVolumeShape::Global) {
            v.field({"blend_distance", "Distancia de mezcla",
                     "Metros fuera de la forma en los que la transicion empieza (0 = de golpe)"},
                    blend_distance, FloatRange{0.0f, 1000.0f, 0.05f, "%.2f m"});
        }
        v.endGroup();
    }
    // Un local solo cambia lo que sobrescribe: casilla al principio de cada
    // seccion (el global lo cambia todo).
    const bool local = shape != PostVolumeShape::Global;
    const auto override_field = [&](std::uint32_t bit, const char* key) {
        if (!all && !local) return;
        bool on = (overrides & bit) != 0;
        // Siempre se reescribe el bit: al leer una escena, el campo llega aqui.
        v.field({key, "Sobrescribir", "Este volumen cambia esta seccion; si no, sale de los globales"}, on);
        overrides = on ? (overrides | bit) : (overrides & ~bit);
    };

    if (v.beginGroup("Exposicion")) {
        override_field(kPostExposure, "override_exposure");
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
        override_field(kPostTonemapping, "override_tonemapping");
        static constexpr std::array<const char*, 3> kTonemappers = {"PBR Neutral", "ACES",
                                                                     "Ninguno"};
        enumField(v, {"tonemapper", "Modo"}, s.tonemapper, kTonemappers);
        v.endGroup();
    }

    if (v.beginGroup("Bloom")) {
        override_field(kPostBloom, "override_bloom");
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
        override_field(kPostColor, "override_color");
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
        override_field(kPostVignette, "override_vignette");
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
        override_field(kPostLens, "override_lens");
        v.field({"chromatic_aberration", "Aberracion cromatica"}, s.chromatic_aberration,
                FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.field({"film_grain", "Grano"}, s.film_grain, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.endGroup();
    }

    if (v.beginGroup("Rayos de luz")) {
        override_field(kPostLightShafts, "override_light_shafts");
        v.field({"light_shafts", "Activados"}, s.light_shafts);
        if (all || s.light_shafts) {
            v.field({"light_shaft_intensity", "Intensidad"}, s.light_shaft_intensity,
                    FloatRange{0.0f, 4.0f, 0.01f, "%.2f", true});
        }
        v.endGroup();
    }

    if (v.beginGroup("Antialiasing")) {
        override_field(kPostAntialiasing, "override_antialiasing");
        v.field({"fxaa", "FXAA"}, s.fxaa);
        v.endGroup();
    }

    if (v.beginGroup("Efectos")) {
        override_field(kPostEffects, "override_effects");
        v.field({"ambient_occlusion", "Oclusion ambiental (SSAO)"}, s.ambient_occlusion);
        v.field({"global_illumination", "Luz rebotada (GI)"}, s.global_illumination);
        v.field({"reflections", "Reflejos"}, s.reflections);
        v.field({"contact_shadows", "Sombras de contacto",
                 "Sombras pequenas del sol (pies en el suelo, piedras, huecos) que las cascadas no ven"},
                s.contact_shadows);
        if (all || s.contact_shadows) {
            v.field({"contact_shadow_length", "Largo maximo (m)",
                     "Tope del rayo. El largo real es el que las cascadas no resuelven (unos centimetros cerca, "
                     "mas lejos): el resto de la sombra ya lo dan las cascadas"},
                    s.contact_shadow_length,
                    FloatRange{0.05f, 3.0f, 0.01f, "%.2f m", true});
        }
        v.field({"volumetric_light", "Luz volumetrica"}, s.volumetric_light);
        if (all || s.volumetric_light) {
            v.field({"volumetric_density", "Densidad del polvo",
                     "Cuanto polvo hay en el aire (hasta 60 m de la camara). Lo ilumina el sol, las luces y "
                     "tambien el cielo, asi que se ve en todas direcciones"},
                    s.volumetric_density,
                    FloatRange{0.0f, 0.2f, 0.001f, "%.3f /m", true});
            v.field({"volumetric_anisotropy", "Anisotropia", "0 = igual en todas direcciones, "
                                                             "cerca de 1 = hacia delante"},
                    s.volumetric_anisotropy, FloatRange{-0.9f, 0.95f, 0.01f, "%.2f", true});
        }
        v.endGroup();
    }

    if (v.beginGroup("Rendimiento")) {
        override_field(kPostPerformance, "override_performance");
        v.field({"lods", "LODs automaticos",
                 "Los modelos estaticos pesados se dibujan simplificados (tambien en las sombras) "
                 "cuando la diferencia no se ve en pantalla"},
                s.lods);
        if (all || s.lods) {
            v.field({"lod_pixel_error", "Error maximo (px)",
                     "Cuantos pixeles puede desviarse la malla simplificada. Mas alto = mas FPS y menos "
                     "detalle a lo lejos"},
                    s.lod_pixel_error, FloatRange{0.25f, 8.0f, 0.05f, "%.2f px", true});
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
    registry.registerComponent<InverseKinematics>("InverseKinematics", "IK (cinematica inversa)", "Animacion");
    registry.registerComponent<ProceduralAnimation>("ProceduralAnimation", "Animacion procedural", "Animacion");
    registry.registerComponent<Light>("Light", "Luz", "Renderizado");
    registry.registerComponent<Camera>("Camera", "Camara", "Renderizado");
    registry.registerComponent<Sky>("Sky", "Cielo", "Entorno");
    registry.registerComponent<Weather>("Weather", "Clima", "Entorno");
    registry.registerComponent<PostProcessing>("PostProcessing", "Post-procesado", "Renderizado");
    registry.registerComponent<Decal>("Decal", "Decal", "Renderizado");
    registry.registerComponent<Profiler>("Profiler", "Profiler", "Depuracion");
}

}  // namespace cramion::ecs
