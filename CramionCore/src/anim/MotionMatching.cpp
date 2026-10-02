#include "CramionCore/anim/MotionMatching.h"

#include "CramionCore/anim/Humanoid.h"
#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/ecs/World.h"  // la definicion de ComponentRegistry::registerComponent<T>

#include <CramionFX/anim/Animator.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>

namespace cramion::anim {

using core::Mat4;
using core::Quat;
using core::Vec3;
using nlohmann::json;

namespace {

constexpr float kLn2 = 0.69314718f;

bool writeText(const std::filesystem::path& path, const std::string& text, std::string* error) {
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary);
        if (!file) {
            if (error) *error = "no se pudo escribir " + path.string();
            return false;
        }
        file << text;
        if (!file) {
            if (error) *error = "error escribiendo " + path.string();
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        if (error) *error = ec.message();
        return false;
    }
    return true;
}

json vec3Json(const Vec3& v) { return json::array({v.x, v.y, v.z}); }

Vec3 readVec3(const json& j, Vec3 fallback) {
    if (j.is_array() && j.size() == 3 && j[0].is_number() && j[1].is_number() && j[2].is_number()) {
        return Vec3{j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
    }
    return fallback;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Angulo en (-pi, pi].
float wrapAngle(float a) {
    while (a > core::kPi) a -= 2.0f * core::kPi;
    while (a <= -core::kPi) a += 2.0f * core::kPi;
    return a;
}

Vec3 translationOf(const Mat4& m) { return Vec3{m.m[3][0], m.m[3][1], m.m[3][2]}; }

Vec3 flat(const Vec3& v) { return Vec3{v.x, 0.0f, v.z}; }

// Busca un nodo por nombre exacto o, si no, que lo contenga (sin mayusculas).
int findNode(const std::vector<asset::Node>& nodes, const std::string& name) {
    if (name.empty()) return -1;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].name == name) return static_cast<int>(i);
    }
    const std::string wanted = lower(name);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (lower(nodes[i].name).find(wanted) != std::string::npos) return static_cast<int>(i);
    }
    return -1;
}

// Media movil (ventana +-radius) con los bordes repetidos.
template <typename T>
std::vector<T> smooth(const std::vector<T>& values, int radius) {
    if (radius <= 0 || values.size() < 3) return values;
    const int n = static_cast<int>(values.size());
    std::vector<T> out(values.size());
    for (int i = 0; i < n; ++i) {
        T sum{};
        int count = 0;
        for (int k = -radius; k <= radius; ++k) {
            const int j = std::clamp(i + k, 0, n - 1);
            sum = sum + values[static_cast<std::size_t>(j)];
            ++count;
        }
        out[static_cast<std::size_t>(i)] = sum * (1.0f / static_cast<float>(count));
    }
    // Los extremos se quedan donde estaban (la raiz empieza en 0).
    out.front() = values.front();
    return out;
}

// Raiz de unas muestras (posicion y giro) en `time`, con ciclos o extrapolando.
void rootAt(const std::vector<Vec3>& positions, const std::vector<float>& yaws, float duration, bool loop,
            float sample_rate, float time, Vec3& out_position, float& out_yaw) {
    out_position = Vec3{};
    out_yaw = 0.0f;
    if (positions.empty() || yaws.size() != positions.size()) return;
    const int n = static_cast<int>(positions.size());
    const auto sample = [&](float t, Vec3& p, float& y) {
        const float f = std::clamp(t * sample_rate, 0.0f, static_cast<float>(n - 1));
        const int i0 = static_cast<int>(std::floor(f));
        const int i1 = std::min(i0 + 1, n - 1);
        const float a = f - static_cast<float>(i0);
        p = core::lerp(positions[static_cast<std::size_t>(i0)], positions[static_cast<std::size_t>(i1)], a);
        y = yaws[static_cast<std::size_t>(i0)] + (yaws[static_cast<std::size_t>(i1)] - yaws[static_cast<std::size_t>(i0)]) * a;
    };
    if (n == 1 || duration <= 1e-4f) {
        sample(0.0f, out_position, out_yaw);
        return;
    }
    if (time >= 0.0f && time <= duration) {
        sample(time, out_position, out_yaw);
        return;
    }
    if (loop) {
        const Vec3 cycle_position = positions.back();
        const float cycle_yaw = yaws.back();
        Vec3 p{};
        float y = 0.0f;
        float local = time;
        int guard = 0;
        if (time > duration) {
            while (local > duration && guard++ < 64) {
                p = p + rotateYaw(cycle_position, y);
                y += cycle_yaw;
                local -= duration;
            }
        } else {
            while (local < 0.0f && guard++ < 64) {
                y -= cycle_yaw;
                p = p - rotateYaw(cycle_position, y);
                local += duration;
            }
        }
        Vec3 sp{};
        float sy = 0.0f;
        sample(std::clamp(local, 0.0f, duration), sp, sy);
        out_position = p + rotateYaw(sp, y);
        out_yaw = y + sy;
        return;
    }
    // Sin bucle: sigue con la velocidad del borde.
    const float h = 1.0f / std::max(sample_rate, 1.0f);
    Vec3 edge{}, inner{};
    float edge_yaw = 0.0f, inner_yaw = 0.0f;
    if (time > duration) {
        sample(duration, edge, edge_yaw);
        sample(duration - h, inner, inner_yaw);
        const float over = time - duration;
        out_position = edge + (edge - inner) * (over / h);
        out_yaw = edge_yaw + (edge_yaw - inner_yaw) * (over / h);
    } else {
        sample(0.0f, edge, edge_yaw);
        sample(h, inner, inner_yaw);
        const float over = -time;
        out_position = edge + (edge - inner) * (over / h);
        out_yaw = edge_yaw + (edge_yaw - inner_yaw) * (over / h);
    }
}

}  // namespace

// --- Utilidades -------------------------------------------------------------------

float yawOfDirection(const Vec3& d) { return std::atan2(d.x, d.z); }

Vec3 directionOfYaw(float yaw) { return Vec3{std::sin(yaw), 0.0f, std::cos(yaw)}; }

Vec3 rotateYaw(const Vec3& v, float yaw) {
    const float c = std::cos(yaw);
    const float s = std::sin(yaw);
    return Vec3{v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}

void springVelocityUpdate(Vec3& velocity, Vec3& acceleration, const Vec3& goal_velocity, float halflife,
                          float delta_seconds) {
    const float y = 2.0f * kLn2 / std::max(halflife, 1e-3f);
    const Vec3 j0 = velocity - goal_velocity;
    const Vec3 j1 = acceleration + j0 * y;
    const float e = std::exp(-y * delta_seconds);
    velocity = (j0 + j1 * delta_seconds) * e + goal_velocity;
    acceleration = (acceleration - j1 * (y * delta_seconds)) * e;
}

Vec3 springPredictPosition(const Vec3& position, const Vec3& velocity, const Vec3& acceleration,
                           const Vec3& goal_velocity, float halflife, float time) {
    const float y = 2.0f * kLn2 / std::max(halflife, 1e-3f);
    const Vec3 j0 = velocity - goal_velocity;
    const Vec3 j1 = acceleration + j0 * y;
    const float e = std::exp(-y * time);
    const float yy = y * y;
    return (j1 * (-1.0f / yy) + (j0 + j1 * time) * (-1.0f / y)) * e + j1 * (1.0f / yy) + j0 * (1.0f / y) +
           goal_velocity * time + position;
}

float springPredictYaw(float yaw, float goal, float halflife, float time) {
    const float y = 2.0f * kLn2 / std::max(halflife, 1e-3f);
    const float diff = wrapAngle(yaw - goal);
    return goal + diff * (1.0f + y * time) * std::exp(-y * time);
}

std::vector<std::string> splitMotionTags(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    const auto flush = [&] {
        std::size_t a = 0;
        std::size_t b = current.size();
        while (a < b && std::isspace(static_cast<unsigned char>(current[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(current[b - 1]))) --b;
        std::string tag = lower(current.substr(a, b - a));
        if (!tag.empty() && std::find(out.begin(), out.end(), tag) == out.end()) out.push_back(std::move(tag));
        current.clear();
    };
    for (const char c : text) {
        if (c == ',' || c == ';') {
            flush();
        } else {
            current.push_back(c);
        }
    }
    flush();
    return out;
}

// --- Archivo .crmmdb -------------------------------------------------------------------

bool loadMotionDatabase(const std::filesystem::path& path, MotionDatabaseAsset& out, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "no se pudo abrir " + path.string();
        return false;
    }
    const json root = json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object()) {
        if (error) *error = "JSON danado en " + path.string();
        return false;
    }
    MotionDatabaseAsset db;
    db.uuid = Uuid::parse(root.value("uuid", std::string{}));
    db.sample_rate = std::clamp(root.value("sample_rate", 30.0f), 5.0f, 120.0f);
    if (const auto it = root.find("trajectory_times"); it != root.end() && it->is_array() &&
                                                        it->size() == static_cast<std::size_t>(kTrajectoryPoints)) {
        for (int k = 0; k < kTrajectoryPoints; ++k) {
            const json& t = (*it)[static_cast<std::size_t>(k)];
            if (t.is_number()) db.trajectory_times[static_cast<std::size_t>(k)] = std::clamp(t.get<float>(), 0.05f, 4.0f);
        }
    }
    if (const auto it = root.find("weights"); it != root.end() && it->is_object()) {
        db.weight_trajectory_position = it->value("trajectory_position", db.weight_trajectory_position);
        db.weight_trajectory_direction = it->value("trajectory_direction", db.weight_trajectory_direction);
        db.weight_foot_position = it->value("foot_position", db.weight_foot_position);
        db.weight_foot_velocity = it->value("foot_velocity", db.weight_foot_velocity);
        db.weight_hip_velocity = it->value("hip_velocity", db.weight_hip_velocity);
    }
    db.strip_root_motion = root.value("strip_root_motion", true);
    db.hips_bone = root.value("hips_bone", std::string{});
    db.left_foot_bone = root.value("left_foot_bone", std::string{});
    db.right_foot_bone = root.value("right_foot_bone", std::string{});
    if (const auto it = root.find("clips"); it != root.end() && it->is_array()) {
        for (const json& j : *it) {
            if (!j.is_object()) continue;
            MotionClipEntry e;
            e.clip.uuid = Uuid::parse(j.value("clip", std::string{}));
            e.clip_name = j.value("clip_name", std::string{});
            e.tags = j.value("tags", std::string{});
            e.loop = j.value("loop", true);
            if (const auto v = j.find("velocity"); v != j.end()) e.velocity = readVec3(*v, e.velocity);
            e.turn_rate = j.value("turn_rate", 0.0f);
            e.start = std::max(0.0f, j.value("start", 0.0f));
            e.end = std::max(0.0f, j.value("end", 0.0f));
            e.cost_bias = j.value("cost_bias", 0.0f);
            db.clips.push_back(std::move(e));
        }
    }
    out = std::move(db);
    return true;
}

bool saveMotionDatabase(MotionDatabaseAsset& db, const std::filesystem::path& path, std::string* error) {
    if (!db.uuid.valid()) db.uuid = Uuid::generate();
    json root;
    root["uuid"] = db.uuid.toString();
    root["type"] = "motion_database";
    root["sample_rate"] = db.sample_rate;
    root["trajectory_times"] = json::array({db.trajectory_times[0], db.trajectory_times[1], db.trajectory_times[2]});
    root["weights"] = {{"trajectory_position", db.weight_trajectory_position},
                       {"trajectory_direction", db.weight_trajectory_direction},
                       {"foot_position", db.weight_foot_position},
                       {"foot_velocity", db.weight_foot_velocity},
                       {"hip_velocity", db.weight_hip_velocity}};
    root["strip_root_motion"] = db.strip_root_motion;
    root["hips_bone"] = db.hips_bone;
    root["left_foot_bone"] = db.left_foot_bone;
    root["right_foot_bone"] = db.right_foot_bone;
    json& clips = root["clips"] = json::array();
    for (const MotionClipEntry& e : db.clips) {
        clips.push_back({{"clip", e.clip.valid() ? e.clip.uuid.toString() : std::string{}},
                         {"clip_name", e.clip_name},
                         {"tags", e.tags},
                         {"loop", e.loop},
                         {"velocity", vec3Json(e.velocity)},
                         {"turn_rate", e.turn_rate},
                         {"start", e.start},
                         {"end", e.end},
                         {"cost_bias", e.cost_bias}});
    }
    return writeText(path, root.dump(2), error);
}

// --- Rasgos ----------------------------------------------------------------------------

std::uint32_t MotionFeatures::tagMask(const std::string& text) const {
    std::uint32_t mask = 0;
    for (const std::string& tag : splitMotionTags(text)) {
        for (std::size_t i = 0; i < tag_names.size() && i < 32; ++i) {
            if (tag_names[i] == tag) mask |= 1u << i;
        }
    }
    return mask;
}

int MotionFeatures::frameAt(int entry, float time) const {
    if (entry < 0 || entry >= static_cast<int>(entry_first.size())) return -1;
    const int first = entry_first[static_cast<std::size_t>(entry)];
    const int count = entry_count[static_cast<std::size_t>(entry)];
    if (first < 0 || count <= 0) return -1;
    const int i = std::clamp(static_cast<int>(std::lround(time * sample_rate)), 0, count - 1);
    return first + i;
}

void motionRootAt(const MotionFeatures& f, int entry, float time, Vec3& position, float& yaw) {
    position = Vec3{};
    yaw = 0.0f;
    if (entry < 0 || entry >= static_cast<int>(f.root_positions.size())) return;
    const std::size_t e = static_cast<std::size_t>(entry);
    rootAt(f.root_positions[e], f.root_yaws[e], f.entry_duration[e], f.entry_loop[e], f.sample_rate, time, position,
           yaw);
}

void motionMeasuredRootAt(const MotionFeatures& f, int entry, float time, Vec3& position, float& yaw) {
    position = Vec3{};
    yaw = 0.0f;
    if (entry < 0 || entry >= static_cast<int>(f.measured_positions.size())) return;
    const std::size_t e = static_cast<std::size_t>(entry);
    rootAt(f.measured_positions[e], f.measured_yaws[e], f.entry_duration[e], f.entry_loop[e], f.sample_rate, time,
           position, yaw);
}

bool buildMotionFeatures(const asset::ModelData& model, const MotionDatabaseAsset& db,
                         const std::vector<int>& clip_indices, MotionFeatures& out) {
    out = MotionFeatures{};
    out.sample_rate = std::clamp(db.sample_rate, 5.0f, 120.0f);
    out.trajectory_times = db.trajectory_times;
    const float fps = out.sample_rate;
    const float h = 1.0f / fps;

    // Huesos: los pedidos o los del humanoide.
    const humanoid::Map map = humanoid::detect(model.nodes);
    out.hips = findNode(model.nodes, db.hips_bone);
    out.left_foot = findNode(model.nodes, db.left_foot_bone);
    out.right_foot = findNode(model.nodes, db.right_foot_bone);
    if (out.hips < 0) out.hips = map[humanoid::Bone::Hips];
    if (out.left_foot < 0) out.left_foot = map[humanoid::Bone::LeftFoot];
    if (out.right_foot < 0) out.right_foot = map[humanoid::Bone::RightFoot];
    if (out.hips < 0 || out.left_foot < 0 || out.right_foot < 0) {
        out.error = "el modelo no tiene cadera y pies reconocibles (pon sus nombres en la base)";
        return false;
    }
    const std::vector<Mat4> rest = humanoid::restGlobals(model.nodes);
    if (map.valid) {
        Vec3 right{}, up{}, forward{};
        humanoid::characterAxes(map, rest, right, up, forward);
        const Vec3 f = flat(forward);
        if (core::length(f) > 1e-4f) out.rest_forward = core::normalize(f);
    }
    // Delante de la cadera en sus propios ejes (para medir hacia donde gira).
    const Mat4 hips_rest = rest[static_cast<std::size_t>(out.hips)];
    Vec3 hips_rest_position{}, hips_rest_scale{};
    Quat hips_rest_rotation{};
    ecs::decomposeMatrix(hips_rest, hips_rest_position, hips_rest_rotation, hips_rest_scale);
    const Vec3 hips_local_forward = ecs::quatRotate(ecs::quatConjugate(core::normalize(hips_rest_rotation)), out.rest_forward);

    // Etiquetas de toda la base (hasta 32).
    for (const MotionClipEntry& e : db.clips) {
        for (const std::string& tag : splitMotionTags(e.tags)) {
            if (out.tag_names.size() < 32 &&
                std::find(out.tag_names.begin(), out.tag_names.end(), tag) == out.tag_names.end()) {
                out.tag_names.push_back(tag);
            }
        }
    }

    const std::size_t entries = db.clips.size();
    out.entry_first.assign(entries, -1);
    out.entry_count.assign(entries, 0);
    out.entry_duration.assign(entries, 0.0f);
    out.entry_start.assign(entries, 0.0f);
    out.entry_clip.assign(entries, -1);
    out.entry_loop.assign(entries, true);
    out.entry_bias.assign(entries, 0.0f);
    out.root_positions.assign(entries, {});
    out.root_yaws.assign(entries, {});
    out.measured_positions.assign(entries, {});
    out.measured_yaws.assign(entries, {});

    // Poses muestreadas de cada entrada (se usan despues para los rasgos).
    struct Samples {
        std::vector<Vec3> hips, left, right;
    };
    std::vector<Samples> samples(entries);
    Animator animator(model);
    for (std::size_t e = 0; e < entries; ++e) {
        const MotionClipEntry& entry = db.clips[e];
        const int clip = e < clip_indices.size() ? clip_indices[e] : -1;
        if (clip < 0 || clip >= static_cast<int>(model.animations.size())) continue;
        const float clip_duration = model.animations[static_cast<std::size_t>(clip)].duration;
        const float start = std::clamp(entry.start, 0.0f, clip_duration);
        const float end = entry.end > start ? std::min(entry.end, clip_duration) : clip_duration;
        const float duration = std::max(end - start, h);
        const int count = std::max(1, static_cast<int>(std::floor(duration * fps + 0.5f)) + 1);
        out.entry_clip[e] = clip;
        out.entry_start[e] = start;
        out.entry_duration[e] = duration;
        out.entry_loop[e] = entry.loop;
        out.entry_bias[e] = entry.cost_bias;
        out.entry_count[e] = count;

        Samples& s = samples[e];
        std::vector<Vec3> measured(static_cast<std::size_t>(count));
        std::vector<float> measured_yaw(static_cast<std::size_t>(count));
        float first_yaw = 0.0f, previous_yaw = 0.0f, unwrapped = 0.0f;
        Vec3 first_hips{};
        for (int j = 0; j < count; ++j) {
            const float tau = std::min(static_cast<float>(j) * h, duration);
            animator.evaluateBlend({ClipSample{clip, start + tau, 1.0f}});
            const std::vector<Mat4>& g = animator.globals();
            const Mat4& hips = g[static_cast<std::size_t>(out.hips)];
            s.hips.push_back(translationOf(hips));
            s.left.push_back(translationOf(g[static_cast<std::size_t>(out.left_foot)]));
            s.right.push_back(translationOf(g[static_cast<std::size_t>(out.right_foot)]));
            const Vec3 forward = flat(ecs::transformDirection(hips, hips_local_forward));
            const float yaw = core::length(forward) > 1e-5f ? yawOfDirection(forward) : previous_yaw;
            if (j == 0) {
                first_yaw = yaw;
                first_hips = flat(s.hips.back());
                unwrapped = 0.0f;
            } else {
                unwrapped += wrapAngle(yaw - previous_yaw);
            }
            previous_yaw = yaw;
            (void)first_yaw;
            measured[static_cast<std::size_t>(j)] = flat(s.hips.back()) - first_hips;
            measured_yaw[static_cast<std::size_t>(j)] = unwrapped;
        }
        // El balanceo de la cadera al andar no es "girar" ni "desviarse".
        measured = smooth(measured, static_cast<int>(std::lround(0.1f * fps)));
        measured_yaw = smooth(measured_yaw, static_cast<int>(std::lround(0.25f * fps)));
        // Ruido: un clip en el sitio no "avanza" unos milimetros.
        if (core::length(measured.back()) < 0.03f && std::abs(measured_yaw.back()) < core::radians(4.0f)) {
            std::fill(measured.begin(), measured.end(), Vec3{});
            std::fill(measured_yaw.begin(), measured_yaw.end(), 0.0f);
        }
        // Lo puesto a mano (clips en el sitio): velocidad en los ejes del
        // personaje que va girando.
        const float w = core::radians(entry.turn_rate);
        std::vector<Vec3> total(measured.size());
        std::vector<float> total_yaw(measured.size());
        for (int j = 0; j < count; ++j) {
            const float tau = std::min(static_cast<float>(j) * h, duration);
            Vec3 synth{};
            if (std::abs(w) < 1e-4f) {
                synth = entry.velocity * tau;
            } else {
                const float sw = std::sin(w * tau) / w;
                const float cw = (1.0f - std::cos(w * tau)) / w;
                synth = Vec3{entry.velocity.x * sw + entry.velocity.z * cw, entry.velocity.y * tau,
                             -entry.velocity.x * cw + entry.velocity.z * sw};
            }
            synth.y = 0.0f;
            total[static_cast<std::size_t>(j)] = measured[static_cast<std::size_t>(j)] + synth;
            total_yaw[static_cast<std::size_t>(j)] = measured_yaw[static_cast<std::size_t>(j)] + w * tau;
        }
        out.measured_positions[e] = std::move(measured);
        out.measured_yaws[e] = std::move(measured_yaw);
        out.root_positions[e] = std::move(total);
        out.root_yaws[e] = std::move(total_yaw);
    }

    // Rasgos crudos de cada fotograma.
    for (std::size_t e = 0; e < entries; ++e) {
        const int count = out.entry_count[e];
        if (count <= 0) continue;
        const Samples& s = samples[e];
        const int entry = static_cast<int>(e);
        const float duration = out.entry_duration[e];
        const std::uint32_t tags = out.tagMask(db.clips[e].tags);
        out.entry_first[e] = static_cast<int>(out.frames.size());
        // Pose en los ejes del personaje (respecto a la raiz medida).
        const auto poseLocal = [&](int j, const Vec3& p) {
            const float tau = std::min(static_cast<float>(j) * h, duration);
            Vec3 root{};
            float yaw = 0.0f;
            motionMeasuredRootAt(out, entry, tau, root, yaw);
            return rotateYaw(p - root, -yaw);
        };
        for (int j = 0; j < count; ++j) {
            const float tau = std::min(static_cast<float>(j) * h, duration);
            MotionFrame frame;
            frame.entry = entry;
            frame.clip = out.entry_clip[e];
            frame.time = tau;
            frame.tags = tags;
            // Sin bucle no se salta a los ultimos instantes (se acabaria enseguida).
            frame.can_start = out.entry_loop[e] || tau <= duration - std::min(0.2f, duration * 0.5f);
            out.frames.push_back(frame);

            std::array<float, kMotionFeatureCount> f{};
            Vec3 root{};
            float yaw = 0.0f;
            motionRootAt(out, entry, tau, root, yaw);
            for (int k = 0; k < kTrajectoryPoints; ++k) {
                Vec3 future{};
                float future_yaw = 0.0f;
                motionRootAt(out, entry, tau + out.trajectory_times[static_cast<std::size_t>(k)], future, future_yaw);
                const Vec3 p = rotateYaw(future - root, -yaw);
                const Vec3 d = rotateYaw(out.rest_forward, future_yaw - yaw);
                f[static_cast<std::size_t>(kFeatTrajectoryPosition + k * 2)] = p.x;
                f[static_cast<std::size_t>(kFeatTrajectoryPosition + k * 2 + 1)] = p.z;
                f[static_cast<std::size_t>(kFeatTrajectoryDirection + k * 2)] = d.x;
                f[static_cast<std::size_t>(kFeatTrajectoryDirection + k * 2 + 1)] = d.z;
            }
            // Velocidades: diferencia con el siguiente (el ultimo, con el anterior).
            const int a = j + 1 < count ? j : std::max(j - 1, 0);
            const int b = j + 1 < count ? j + 1 : j;
            const float dt = b > a ? h : 1.0f;
            Vec3 root_a{}, root_b{};
            float yaw_a = 0.0f, yaw_b = 0.0f;
            motionRootAt(out, entry, std::min(static_cast<float>(a) * h, duration), root_a, yaw_a);
            motionRootAt(out, entry, std::min(static_cast<float>(b) * h, duration), root_b, yaw_b);
            const Vec3 root_velocity = rotateYaw(root_b - root_a, -yaw_a) * (b > a ? 1.0f / dt : 0.0f);
            const auto velocity = [&](const std::vector<Vec3>& track) {
                const Vec3 pa = poseLocal(a, track[static_cast<std::size_t>(a)]);
                const Vec3 pb = poseLocal(b, track[static_cast<std::size_t>(b)]);
                return (pb - pa) * (b > a ? 1.0f / dt : 0.0f) + root_velocity;
            };
            const Vec3 left = poseLocal(j, s.left[static_cast<std::size_t>(j)]);
            const Vec3 right = poseLocal(j, s.right[static_cast<std::size_t>(j)]);
            const Vec3 left_velocity = velocity(s.left);
            const Vec3 right_velocity = velocity(s.right);
            const Vec3 hips_velocity = velocity(s.hips);
            const Vec3 values[] = {left, right, left_velocity, right_velocity, hips_velocity};
            for (int v = 0; v < 5; ++v) {
                f[static_cast<std::size_t>(kFeatFootPosition + v * 3)] = values[v].x;
                f[static_cast<std::size_t>(kFeatFootPosition + v * 3 + 1)] = values[v].y;
                f[static_cast<std::size_t>(kFeatFootPosition + v * 3 + 2)] = values[v].z;
            }
            out.raw.insert(out.raw.end(), f.begin(), f.end());
        }
    }
    if (out.frames.empty()) {
        out.error = "ningun clip de la base esta en el modelo (o la base esta vacia)";
        return false;
    }

    // Normalizacion por grupos: media de cada dimension y una desviacion por
    // grupo (las dimensiones de un grupo conservan su escala relativa).
    struct Group {
        int first;
        int size;
        float weight;
    };
    const Group groups[] = {{kFeatTrajectoryPosition, 6, db.weight_trajectory_position},
                            {kFeatTrajectoryDirection, 6, db.weight_trajectory_direction},
                            {kFeatFootPosition, 6, db.weight_foot_position},
                            {kFeatFootVelocity, 6, db.weight_foot_velocity},
                            {kFeatHipVelocity, 3, db.weight_hip_velocity}};
    const std::size_t frames = out.frames.size();
    for (int d = 0; d < kMotionFeatureCount; ++d) {
        double sum = 0.0;
        for (std::size_t i = 0; i < frames; ++i) sum += out.raw[i * kMotionFeatureCount + static_cast<std::size_t>(d)];
        out.mean[static_cast<std::size_t>(d)] = static_cast<float>(sum / static_cast<double>(frames));
    }
    for (const Group& g : groups) {
        double variance = 0.0;
        for (int d = g.first; d < g.first + g.size; ++d) {
            const float m = out.mean[static_cast<std::size_t>(d)];
            for (std::size_t i = 0; i < frames; ++i) {
                const float x = out.raw[i * kMotionFeatureCount + static_cast<std::size_t>(d)] - m;
                variance += static_cast<double>(x) * x;
            }
        }
        variance /= static_cast<double>(frames * static_cast<std::size_t>(g.size));
        const float deviation = std::max(static_cast<float>(std::sqrt(variance)), 1e-4f);
        for (int d = g.first; d < g.first + g.size; ++d) {
            // Peso 0: el grupo no cuenta (escala 0 = se deja a 0).
            out.scale[static_cast<std::size_t>(d)] = g.weight > 1e-6f ? deviation / g.weight : 0.0f;
        }
    }
    out.features.resize(out.raw.size());
    for (std::size_t i = 0; i < frames; ++i) {
        normalizeMotionFeature(out, out.raw.data() + i * kMotionFeatureCount,
                               out.features.data() + i * kMotionFeatureCount);
    }
    return true;
}

void normalizeMotionFeature(const MotionFeatures& f, const float* raw, float* out) {
    for (int d = 0; d < kMotionFeatureCount; ++d) {
        const float s = f.scale[static_cast<std::size_t>(d)];
        out[d] = s > 0.0f ? (raw[d] - f.mean[static_cast<std::size_t>(d)]) / s : 0.0f;
    }
}

void denormalizeMotionFeature(const MotionFeatures& f, const float* normalized, float* out) {
    for (int d = 0; d < kMotionFeatureCount; ++d) {
        const float s = f.scale[static_cast<std::size_t>(d)];
        out[d] = s > 0.0f ? normalized[d] * s + f.mean[static_cast<std::size_t>(d)] : f.mean[static_cast<std::size_t>(d)];
    }
}

float motionCost(const MotionFeatures& f, const float* query, int frame) {
    if (frame < 0 || frame >= static_cast<int>(f.frames.size())) return std::numeric_limits<float>::max();
    const float* x = f.feature(static_cast<std::size_t>(frame));
    float cost = 0.0f;
    for (int d = 0; d < kMotionFeatureCount; ++d) {
        const float diff = query[d] - x[d];
        cost += diff * diff;
    }
    const int entry = f.frames[static_cast<std::size_t>(frame)].entry;
    if (entry >= 0 && entry < static_cast<int>(f.entry_bias.size())) cost += f.entry_bias[static_cast<std::size_t>(entry)];
    return cost;
}

MotionSearchResult searchMotion(const MotionFeatures& f, const float* query, std::uint32_t required_tags,
                                int skip_entry, float skip_from, float skip_to) {
    MotionSearchResult best;
    best.cost = std::numeric_limits<float>::max();
    // Si ningun fotograma tiene todas las etiquetas pedidas, se busca sin ellas.
    bool any_tagged = required_tags == 0;
    if (!any_tagged) {
        for (const MotionFrame& fr : f.frames) {
            if ((fr.tags & required_tags) == required_tags) {
                any_tagged = true;
                break;
            }
        }
    }
    const std::uint32_t mask = any_tagged ? required_tags : 0u;
    const std::size_t count = f.frames.size();
    for (std::size_t i = 0; i < count; ++i) {
        const MotionFrame& fr = f.frames[i];
        if (!fr.can_start || (fr.tags & mask) != mask) continue;
        if (fr.entry == skip_entry && fr.time >= skip_from && fr.time <= skip_to) continue;
        float cost = fr.entry < static_cast<int>(f.entry_bias.size()) ? f.entry_bias[static_cast<std::size_t>(fr.entry)] : 0.0f;
        const float* x = f.features.data() + i * kMotionFeatureCount;
        // Poda: en cuanto supera al mejor, se deja (la trayectoria va primero,
        // es lo que mas descarta).
        int d = 0;
        for (; d < kMotionFeatureCount && cost < best.cost; ++d) {
            const float diff = query[d] - x[d];
            cost += diff * diff;
        }
        if (d == kMotionFeatureCount && cost < best.cost) {
            best.cost = cost;
            best.frame = static_cast<int>(i);
        }
    }
    if (best.frame < 0) best.cost = 0.0f;
    return best;
}

// --- Componente ------------------------------------------------------------------

void MotionMatching::reflect(ecs::PropertyVisitor& v) {
    v.asset({"database", "Base de datos", "La base de Motion Matching (.crmmdb): los clips entre los que elige"},
            database, assets::AssetType::MotionDatabase);
    v.field({"enabled", "Activo", "Mientras esta activo y tiene base, manda sobre el Animator"}, enabled);
    static constexpr const char* kInputs[] = {"Movimiento del objeto", "Script (setMotionVelocity)"};
    ecs::enumField(v, {"input", "Entrada", "De donde sale la trayectoria que se pide"}, input, kInputs);
    v.field({"search_interval", "Intervalo de busqueda (s)", "Cada cuanto se busca el mejor fotograma"},
            search_interval, {0.02f, 1.0f, 0.005f, "%.3f"});
    v.field({"transition_time", "Transicion (s)", "Inercializacion al saltar a otro fotograma"}, transition_time,
            {0.0f, 1.0f, 0.005f, "%.3f"});
    v.field({"switch_threshold", "Mejora minima", "Cuanto tiene que mejorar el coste para saltar (evita temblores)"},
            switch_threshold, {0.0f, 5.0f, 0.005f, "%.3f"});
    v.field({"velocity_halflife", "Suavizado de velocidad (s)", "Semivida del muelle de la velocidad"},
            velocity_halflife, {0.01f, 2.0f, 0.005f, "%.3f"});
    v.field({"facing_halflife", "Suavizado de giro (s)", "Semivida del muelle del giro"}, facing_halflife,
            {0.01f, 2.0f, 0.005f, "%.3f"});
    v.field({"speed", "Velocidad", "Multiplica la velocidad de reproduccion"}, speed, {0.0f, 4.0f, 0.01f, "%.2f"});
    v.field({"required_tags", "Etiquetas", "Solo fotogramas con estas etiquetas (separadas por comas)"},
            required_tags);
    v.field({"debug_draw", "Dibujar trayectorias", "En la vista de Escena: la pedida (verde) y la elegida (naranja)"},
            debug_draw);
    search_interval = std::max(search_interval, 0.01f);
    transition_time = std::max(transition_time, 0.0f);
}

void registerMotionMatchingComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("MotionMatching") == nullptr) {
        registry.registerComponent<MotionMatching>("MotionMatching", "Motion Matching", "Animacion");
    }
}

}  // namespace cramion::anim
