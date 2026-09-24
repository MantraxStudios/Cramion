// Componentes de cinematicas: su reflexion (Inspector y .crscene), el riel
// evaluado (DollyPath) y las curvas de mezcla.

#include "CramionCore/cinematics/Cinematics.h"

#include "CramionCore/ecs/MathUtil.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace cramion::cinema {

using core::Vec3;
using ecs::FloatRange;
using ecs::Vec3Kind;

namespace {

constexpr std::array<const char*, 3> kUnits = {"Metros", "Normalizado (0-1)", "Unidades de punto"};
constexpr std::array<const char*, 5> kCurves = {"Corte", "Suave (entrada y salida)", "Lineal", "Suave al entrar",
                                                "Suave al salir"};

}  // namespace

float applyBlendCurve(BlendCurve curve, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    switch (curve) {
        case BlendCurve::Cut: return 1.0f;
        case BlendCurve::Linear: return t;
        case BlendCurve::EaseIn: return t * t;
        case BlendCurve::EaseOut: return 1.0f - (1.0f - t) * (1.0f - t);
        case BlendCurve::EaseInOut: return t * t * (3.0f - 2.0f * t);
    }
    return t;
}

// -----------------------------------------------------------------------------
// Reflexion
// -----------------------------------------------------------------------------

void DollyTrack::reflect(ecs::PropertyVisitor& v) {
    static constexpr std::array<const char*, 2> kModes = {"Suave (automatica)", "Bezier (asas editables)"};
    ecs::enumField(v, {"mode", "Curva", "Suave: tangentes automaticas. Bezier: cada punto con sus asas"}, mode, kModes);
    v.field({"looped", "En bucle", "El ultimo punto se une con el primero"}, looped);
    v.field({"resolution", "Resolucion", "Tramos por segmento para medir la curva"}, resolution, 2, 64);
    const bool bezier = v.wantsAllFields() || mode == PathMode::Bezier;
    ecs::listField(v, {"waypoints", "Puntos"}, waypoints, [bezier](DollyWaypoint& w, ecs::PropertyVisitor& item) {
        item.field({"position", "Posicion"}, w.position, Vec3Kind::Position);
        item.field({"roll", "Inclinacion", "Grados de giro de la camara en este punto"}, w.roll,
                   FloatRange{-180.0f, 180.0f, 0.5f, "%.1f°"});
        if (bezier) {
            item.field({"tangent", "Tangente", "Asa de salida (la de entrada es la opuesta). Cero = automatica"},
                       w.tangent, Vec3Kind::Position);
        }
    });
}

void DollyCart::reflect(ecs::PropertyVisitor& v) {
    v.entity({"track", "Riel", "Arrastra aqui un objeto con Dolly Track"}, track);
    v.field({"speed", "Velocidad"}, speed, FloatRange{-1000.0f, 1000.0f, 0.05f, "%.2f"});
    v.field({"position", "Posicion"}, position, FloatRange{0.0f, 0.0f, 0.01f, "%.2f"});
    ecs::enumField(v, {"units", "Unidades"}, units, kUnits);
    v.field({"orient_to_path", "Orientar al riel"}, orient_to_path);
    v.field({"play_in_editor", "Moverse en el editor", "Tambien fuera de Play"}, play_in_editor);
}

void VirtualCamera::reflect(ecs::PropertyVisitor& v) {
    static constexpr std::array<const char*, 5> kBodies = {"Nada", "Transposer (distancia fija)",
                                                           "Riel (Tracked Dolly)", "Orbita", "Pegada al objetivo"};
    static constexpr std::array<const char*, 2> kBindings = {"Ejes del mundo", "Ejes del objetivo"};
    static constexpr std::array<const char*, 4> kAims = {"Nada", "Composer (mirar con suavidad)", "Mirar al instante",
                                                         "Igual que el objetivo Follow"};
    const bool all = v.wantsAllFields();
    v.field({"priority", "Prioridad", "La de mayor prioridad es la que usa la camara real (Camera Brain)"}, priority,
            -1000, 1000);
    if (v.beginGroup("Lente", true)) {
        v.field({"fov", "Campo de vision"}, fov, FloatRange{1.0f, 170.0f, 0.2f, "%.1f°", true});
        v.field({"dutch", "Dutch (inclinacion)"}, dutch, FloatRange{-180.0f, 180.0f, 0.5f, "%.1f°", true});
        v.endGroup();
    }
    v.entity({"follow", "Seguir (Follow)", "Objeto al que acompana la camara"}, follow);
    v.entity({"look_at", "Mirar a (Look At)", "Objeto al que apunta la camara"}, look_at);
    if (v.beginGroup("Cuerpo (Body)", true)) {
        ecs::enumField(v, {"body", "Modo"}, body, kBodies);
        if (all || body == BodyMode::Transposer) {
            ecs::enumField(v, {"binding", "Desfase en"}, binding, kBindings);
            v.field({"follow_offset", "Desfase"}, follow_offset, Vec3Kind::Position);
        }
        if (all || body == BodyMode::Transposer || body == BodyMode::Orbital || body == BodyMode::HardLockToTarget) {
            v.field({"damping", "Amortiguacion", "Segundos en alcanzar la posicion (0 = rigida)"}, damping,
                    Vec3Kind::Scale);
        }
        if (all || body == BodyMode::Orbital) {
            v.field({"orbit_radius", "Radio"}, orbit_radius, FloatRange{0.0f, 1000.0f, 0.05f, "%.2f m"});
            v.field({"orbit_height", "Altura"}, orbit_height, FloatRange{-1000.0f, 1000.0f, 0.05f, "%.2f m"});
            v.field({"orbit_angle", "Angulo inicial"}, orbit_angle, FloatRange{-360.0f, 360.0f, 0.5f, "%.1f°"});
            v.field({"orbit_speed", "Velocidad de giro"}, orbit_speed, FloatRange{-720.0f, 720.0f, 0.5f, "%.1f°/s"});
        }
        if (all || body == BodyMode::TrackedDolly) {
            v.entity({"track", "Riel", "Objeto con Dolly Track"}, track);
            v.field({"auto_dolly", "Auto dolly", "Ir al punto del riel mas cercano al objetivo Follow"},
                    auto_dolly);
            if (all || auto_dolly) {
                v.field({"auto_dolly_offset", "Adelanto", "Unidades de punto por delante (+) o por detras (-)"},
                        auto_dolly_offset, FloatRange{-100.0f, 100.0f, 0.01f, "%.2f"});
            }
            if (all || !auto_dolly) {
                v.field({"path_position", "Posicion en el riel"}, path_position, FloatRange{0.0f, 0.0f, 0.005f, "%.3f"});
                ecs::enumField(v, {"path_units", "Unidades"}, path_units, kUnits);
            }
            v.field({"dolly_damping", "Amortiguacion en el riel"}, dolly_damping,
                    FloatRange{0.0f, 20.0f, 0.01f, "%.2f s"});
        }
        v.endGroup();
    }
    if (v.beginGroup("Apuntar (Aim)", true)) {
        ecs::enumField(v, {"aim", "Modo"}, aim, kAims);
        if (all || aim == AimMode::Composer || aim == AimMode::HardLookAt) {
            v.field({"look_offset", "Desfase del objetivo", "Donde mirar respecto al origen del objetivo"}, look_offset,
                    Vec3Kind::Position);
        }
        if (all || aim == AimMode::Composer) {
            v.field({"aim_damping", "Amortiguacion"}, aim_damping, FloatRange{0.0f, 20.0f, 0.01f, "%.2f s"});
        }
        v.endGroup();
    }
    if (v.beginGroup("Ruido (camara en mano)", false)) {
        v.field({"noise_amplitude", "Amplitud"}, noise_amplitude, FloatRange{0.0f, 30.0f, 0.05f, "%.2f°"});
        v.field({"noise_frequency", "Frecuencia"}, noise_frequency, FloatRange{0.0f, 20.0f, 0.01f, "%.2f Hz"});
        v.endGroup();
    }
}

void CameraBrain::reflect(ecs::PropertyVisitor& v) {
    v.field({"default_blend", "Mezcla por defecto", "Segundos al cambiar de camara virtual (0 = corte)"},
            default_blend, FloatRange{0.0f, 30.0f, 0.05f, "%.2f s"});
    ecs::enumField(v, {"curve", "Curva de mezcla"}, curve, kCurves);
    v.field({"update_in_editor", "Mover en el editor", "La camara sigue a las virtuales tambien fuera de Play"},
            update_in_editor);
}

void CinematicSequence::reflect(ecs::PropertyVisitor& v) {
    v.field({"play_on_start", "Empezar al dar Play"}, play_on_start);
    v.field({"loop", "En bucle"}, loop);
    v.field({"speed", "Velocidad"}, speed, FloatRange{0.0f, 10.0f, 0.01f, "%.2fx"});
    ecs::listField(v, {"shots", "Planos"}, shots, [](CinematicShot& shot, ecs::PropertyVisitor& item) {
        item.entity({"camera", "Camara virtual"}, shot.camera);
        item.field({"start", "Empieza"}, shot.start, FloatRange{0.0f, 100000.0f, 0.01f, "%.2f s"});
        item.field({"duration", "Dura"}, shot.duration, FloatRange{0.01f, 100000.0f, 0.01f, "%.2f s"});
        item.field({"blend_in", "Mezcla de entrada"}, shot.blend_in, FloatRange{0.0f, 60.0f, 0.01f, "%.2f s"});
        ecs::enumField(item, {"curve", "Curva"}, shot.curve, kCurves);
    });
    ecs::listField(v, {"activations", "Objetos activos"}, activations,
                   [](CinematicActivation& a, ecs::PropertyVisitor& item) {
                       item.entity({"entity", "Objeto"}, a.entity);
                       item.field({"start", "Desde"}, a.start, FloatRange{0.0f, 100000.0f, 0.01f, "%.2f s"});
                       item.field({"end", "Hasta"}, a.end, FloatRange{0.0f, 100000.0f, 0.01f, "%.2f s"});
                   });
}

float CinematicSequence::duration() const {
    float end = 0.0f;
    for (const CinematicShot& s : shots) end = std::max(end, s.start + s.duration);
    for (const CinematicActivation& a : activations) end = std::max(end, a.end);
    return end;
}

int CinematicSequence::shotAt(float time) const {
    int best = -1;
    for (std::size_t i = 0; i < shots.size(); ++i) {
        const CinematicShot& s = shots[i];
        if (time >= s.start && time < s.start + s.duration &&
            (best < 0 || s.start >= shots[static_cast<std::size_t>(best)].start)) {
            best = static_cast<int>(i);
        }
    }
    return best;
}

void registerCinematicComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    registry.registerComponent<VirtualCamera>("VirtualCamera", "Virtual Camera", "Cinematicas");
    registry.registerComponent<CameraBrain>("CameraBrain", "Camera Brain", "Cinematicas");
    registry.registerComponent<DollyTrack>("DollyTrack", "Dolly Track (riel)", "Cinematicas");
    registry.registerComponent<DollyCart>("DollyCart", "Dolly Cart (carro)", "Cinematicas");
    registry.registerComponent<CinematicSequence>("CinematicSequence", "Cinematic Sequence", "Cinematicas");
}

// -----------------------------------------------------------------------------
// DollyPath
// -----------------------------------------------------------------------------

DollyPath::DollyPath(const DollyTrack& track, const core::Mat4& world)
    : looped_(track.looped && track.waypoints.size() >= 3), resolution_(std::clamp(track.resolution, 2, 64)) {
    points_.reserve(track.waypoints.size());
    for (const DollyWaypoint& w : track.waypoints) {
        points_.push_back(ecs::transformPoint(world, w.position));
        rolls_.push_back(w.roll);
    }
    if (!valid()) return;
    // Tangentes: las de Catmull-Rom (pasa por todos los puntos) o, en modo
    // Bezier, las del usuario donde las haya.
    tangents_.resize(points_.size());
    for (std::size_t i = 0; i < points_.size(); ++i) {
        const Vec3& manual = track.waypoints[i].tangent;
        const bool use_manual = track.mode == PathMode::Bezier && core::dot(manual, manual) > 1e-10f;
        tangents_[i] = use_manual ? ecs::transformDirection(world, manual) : autoTangent(i);
    }
    const int steps = static_cast<int>(segments() * static_cast<float>(resolution_));
    samples_.reserve(static_cast<std::size_t>(steps) + 1);
    cumulative_.reserve(static_cast<std::size_t>(steps) + 1);
    float distance = 0.0f;
    for (int s = 0; s <= steps; ++s) {
        const Vec3 p = evaluate(static_cast<float>(s) / static_cast<float>(resolution_));
        if (!samples_.empty()) distance += core::length(p - samples_.back());
        samples_.push_back(p);
        cumulative_.push_back(distance);
    }
}

float DollyPath::segments() const {
    const auto n = static_cast<float>(points_.size());
    return looped_ ? n : n - 1.0f;
}

float DollyPath::wrap(float u) const {
    const float total = segments();
    if (total <= 0.0f) return 0.0f;
    if (looped_) {
        u = std::fmod(u, total);
        return u < 0.0f ? u + total : u;
    }
    return std::clamp(u, 0.0f, total);
}

Vec3 DollyPath::autoTangent(std::size_t i) const {
    const int n = static_cast<int>(points_.size());
    if (n < 2) return Vec3{};
    const auto at = [&](int k) -> const Vec3& {
        if (looped_) return points_[static_cast<std::size_t>(((k % n) + n) % n)];
        return points_[static_cast<std::size_t>(std::clamp(k, 0, n - 1))];
    };
    // Catmull-Rom uniforme como Bezier: asa = (siguiente - anterior) / 6.
    const int k = static_cast<int>(i);
    return (at(k + 1) - at(k - 1)) * (1.0f / 6.0f);
}

Vec3 DollyPath::evaluate(float u) const {
    const int n = static_cast<int>(points_.size());
    u = wrap(u);
    int segment = static_cast<int>(std::floor(u));
    if (!looped_) segment = std::min(segment, n - 2);
    const float t = u - static_cast<float>(segment);
    const auto index = [&](int i) {
        return static_cast<std::size_t>(looped_ ? ((i % n) + n) % n : std::clamp(i, 0, n - 1));
    };
    // Bezier cubica entre dos puntos con sus asas (con las tangentes
    // automaticas es exactamente Catmull-Rom).
    const std::size_t a = index(segment);
    const std::size_t b = index(segment + 1);
    const Vec3& p0 = points_[a];
    const Vec3 p1 = points_[a] + tangents_[a];
    const Vec3 p2 = points_[b] - tangents_[b];
    const Vec3& p3 = points_[b];
    const float s = 1.0f - t;
    return p0 * (s * s * s) + p1 * (3.0f * s * s * t) + p2 * (3.0f * s * t * t) + p3 * (t * t * t);
}

Vec3 DollyPath::point(float u) const {
    return valid() ? evaluate(u) : (points_.empty() ? Vec3{} : points_.front());
}

Vec3 DollyPath::tangent(float u) const {
    if (!valid()) return Vec3{0.0f, 0.0f, -1.0f};
    const float h = 0.01f;
    const float total = segments();
    float a = u - h;
    float b = u + h;
    if (!looped_) {
        a = std::clamp(a, 0.0f, total);
        b = std::clamp(b, 0.0f, total);
    }
    const Vec3 d = evaluate(b) - evaluate(a);
    const float l = core::length(d);
    return l > 1e-6f ? d * (1.0f / l) : Vec3{0.0f, 0.0f, -1.0f};
}

float DollyPath::roll(float u) const {
    if (rolls_.empty()) return 0.0f;
    const int n = static_cast<int>(rolls_.size());
    u = wrap(u);
    const int i = static_cast<int>(std::floor(u));
    const float t = u - static_cast<float>(i);
    const float a = rolls_[static_cast<std::size_t>(looped_ ? i % n : std::min(i, n - 1))];
    const float b = rolls_[static_cast<std::size_t>(looped_ ? (i + 1) % n : std::min(i + 1, n - 1))];
    return a + (b - a) * t;
}

float DollyPath::toPathUnits(float position, PositionUnits units) const {
    if (!valid()) return 0.0f;
    if (units == PositionUnits::PathUnits) return wrap(position);
    float distance = units == PositionUnits::Normalized ? position * length() : position;
    const float total = length();
    if (total <= 1e-6f) return 0.0f;
    if (looped_) {
        distance = std::fmod(distance, total);
        if (distance < 0.0f) distance += total;
    } else {
        distance = std::clamp(distance, 0.0f, total);
    }
    const auto it = std::lower_bound(cumulative_.begin(), cumulative_.end(), distance);
    const std::size_t i = static_cast<std::size_t>(std::max<std::ptrdiff_t>(it - cumulative_.begin(), 1));
    const float d0 = cumulative_[i - 1];
    const float d1 = cumulative_[std::min(i, cumulative_.size() - 1)];
    const float frac = d1 > d0 ? (distance - d0) / (d1 - d0) : 0.0f;
    return (static_cast<float>(i - 1) + frac) / static_cast<float>(resolution_);
}

float DollyPath::fromPathUnits(float u, PositionUnits units) const {
    if (!valid() || units == PositionUnits::PathUnits) return u;
    u = wrap(u);
    const float s = u * static_cast<float>(resolution_);
    const auto i = static_cast<std::size_t>(std::clamp(std::floor(s), 0.0f, static_cast<float>(cumulative_.size() - 1)));
    const std::size_t j = std::min(i + 1, cumulative_.size() - 1);
    const float distance = cumulative_[i] + (cumulative_[j] - cumulative_[i]) * (s - static_cast<float>(i));
    return units == PositionUnits::Normalized ? (length() > 1e-6f ? distance / length() : 0.0f) : distance;
}

float DollyPath::closest(const Vec3& p) const {
    if (!valid()) return 0.0f;
    std::size_t best = 0;
    float best_distance = 1e30f;
    for (std::size_t i = 0; i < samples_.size(); ++i) {
        const Vec3 d = samples_[i] - p;
        const float dd = core::dot(d, d);
        if (dd < best_distance) {
            best_distance = dd;
            best = i;
        }
    }
    // Afinar entre las muestras vecinas (busqueda ternaria).
    const float step = 1.0f / static_cast<float>(resolution_);
    float lo = static_cast<float>(best) * step - step;
    float hi = static_cast<float>(best) * step + step;
    if (!looped_) {
        lo = std::max(lo, 0.0f);
        hi = std::min(hi, segments());
    }
    const auto distance = [&](float u) {
        const Vec3 d = evaluate(u) - p;
        return core::dot(d, d);
    };
    for (int k = 0; k < 24; ++k) {
        const float a = lo + (hi - lo) / 3.0f;
        const float b = hi - (hi - lo) / 3.0f;
        if (distance(a) < distance(b)) hi = b;
        else lo = a;
    }
    return wrap((lo + hi) * 0.5f);
}

}  // namespace cramion::cinema
