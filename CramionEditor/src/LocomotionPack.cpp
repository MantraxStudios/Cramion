#include "LocomotionPack.h"

#include <CramionCore/anim/Humanoid.h>
#include <CramionCore/asset/AssetManager.h>
#include <CramionCore/asset/Importer.h>
#include <CramionCore/ecs/AnimatorController.h>
#include <CramionCore/ecs/MathUtil.h>
#include <CramionUpdater/Update.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <random>

namespace cramion::editor::locomotion {
namespace {

namespace fs = std::filesystem;
using core::Quat;
using core::Vec3;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Nombre del archivo del pack -> clave del clip ("" = no es de los conocidos).
std::string clipKey(const fs::path& file) {
    static const std::map<std::string, std::string> keys = {
        {"idle", "idle"},
        {"walking", "walk"},
        {"running", "run"},
        {"left strafe walking", "strafe_walk_left"},
        {"right strafe walking", "strafe_walk_right"},
        {"left strafe", "strafe_run_left"},
        {"right strafe", "strafe_run_right"},
        {"left turn", "turn_left"},
        {"right turn", "turn_right"},
        {"left turn 90", "turn90_left"},
        {"right turn 90", "turn90_right"},
        {"jump", "jump"},
    };
    const auto it = keys.find(lower(file.stem().string()));
    return it != keys.end() ? it->second : std::string();
}

std::vector<fs::path> fbxFiles(const fs::path& folder) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && lower(it->path().extension().string()) == ".fbx") out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Globales en reposo de los nodos (los padres van antes que los hijos).
std::vector<core::Mat4> restGlobals(const std::vector<asset::Node>& nodes) {
    std::vector<core::Mat4> g(nodes.size(), core::Mat4::identity());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const int p = nodes[i].parent;
        g[i] = p >= 0 && static_cast<std::size_t>(p) < i ? g[static_cast<std::size_t>(p)] * nodes[i].local : nodes[i].local;
    }
    return g;
}

// Solo el giro de una matriz (sin escala: el pack viene en centimetros con
// una escala de 0.01 arriba).
Quat rotationOf(const core::Mat4& m) {
    core::Mat4 r = m;
    for (int c = 0; c < 3; ++c) {
        const float len = std::sqrt(r.m[c][0] * r.m[c][0] + r.m[c][1] * r.m[c][1] + r.m[c][2] * r.m[c][2]);
        if (len > 1e-8f) {
            for (int k = 0; k < 3; ++k) r.m[c][k] /= len;
        }
    }
    r.m[3][0] = r.m[3][1] = r.m[3][2] = 0.0f;
    return core::normalize(ecs::quatFromRotationMatrix(r));
}

Quat yawRotation(float radians) { return Quat{0.0f, std::sin(radians * 0.5f), 0.0f, std::cos(radians * 0.5f)}; }

// Giro alrededor de la vertical del mundo de `q` (swing-twist).
float yawOf(const Quat& q) { return 2.0f * std::atan2(q.y, q.w); }

float wrapPi(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

Uuid headerUuid(const fs::path& cranim) {
    std::ifstream in(cranim, std::ios::binary);
    std::string line;
    std::getline(in, line);
    const nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
    return j.is_object() && j.contains("uuid") ? Uuid::parse(j["uuid"].get<std::string>()) : Uuid{};
}

// Mide el clip y lo deja en el sitio (ver la cabecera).
void processClip(asset::ModelData& model, asset::AnimationClip& clip, ClipInfo& info, bool remove_turn) {
    const humanoid::Map map = humanoid::detect(model.nodes);
    int hips = map[humanoid::Bone::Hips];
    if (hips < 0) {
        for (std::size_t i = 0; i < model.nodes.size(); ++i) {
            if (lower(model.nodes[i].name).find("hips") != std::string::npos) {
                hips = static_cast<int>(i);
                break;
            }
        }
    }
    info.duration = clip.duration;
    if (hips < 0) return;
    asset::AnimationChannel* channel = nullptr;
    for (asset::AnimationChannel& c : clip.channels) {
        if (c.node == hips) channel = &c;
    }
    if (channel == nullptr) return;

    const std::vector<core::Mat4> globals = restGlobals(model.nodes);
    const int parent = model.nodes[static_cast<std::size_t>(hips)].parent;
    const core::Mat4 P = parent >= 0 ? globals[static_cast<std::size_t>(parent)] : core::Mat4::identity();
    const core::Mat4 Pinv = core::inverse(P);
    const Quat Prot = rotationOf(P);
    const Quat ProtInv = ecs::quatConjugate(Prot);
    const core::Mat4& rest_local = model.nodes[static_cast<std::size_t>(hips)].local;
    const Vec3 rest = ecs::transformPoint(P, Vec3{rest_local.m[3][0], rest_local.m[3][1], rest_local.m[3][2]});
    const float duration = std::max(clip.duration, 1e-3f);

    // Giro de la cadera (desenrollado) en cada clave de rotacion.
    std::vector<std::pair<float, float>> yaw;
    if (!channel->rotations.empty()) {
        const Quat first = ecs::quatMultiply(Prot, channel->rotations.front().value);
        const Quat first_inv = ecs::quatConjugate(first);
        float previous = 0.0f;
        float total = 0.0f;
        for (const asset::QuatKey& k : channel->rotations) {
            const Quat g = ecs::quatMultiply(Prot, k.value);
            const float a = yawOf(core::normalize(ecs::quatMultiply(g, first_inv)));
            total += wrapPi(a - previous);
            previous = a;
            yaw.emplace_back(k.time, total);
        }
        info.turn_degrees = total * 57.2957795f;
    }
    const auto yawAt = [&](float t) {
        if (yaw.empty()) return 0.0f;
        if (t <= yaw.front().first) return yaw.front().second;
        for (std::size_t i = 1; i < yaw.size(); ++i) {
            if (t <= yaw[i].first) {
                const float span = std::max(yaw[i].first - yaw[i - 1].first, 1e-6f);
                const float f = (t - yaw[i - 1].first) / span;
                return yaw[i - 1].second + (yaw[i].second - yaw[i - 1].second) * f;
            }
        }
        return yaw.back().second;
    };

    // Avance horizontal (de la primera a la ultima clave) y lo que sube.
    if (!channel->positions.empty()) {
        const Vec3 start = ecs::transformPoint(P, channel->positions.front().value);
        const Vec3 end = ecs::transformPoint(P, channel->positions.back().value);
        const Vec3 drift{end.x - start.x, 0.0f, end.z - start.z};
        info.speed = core::length(drift) / duration;
        float top = -1e9f;
        for (const asset::VectorKey& k : channel->positions) top = std::max(top, ecs::transformPoint(P, k.value).y);
        info.rise = std::max(0.0f, top - rest.y);

        // Despegue y aterrizaje: del punto mas alto hacia atras hasta cruzar la
        // altura del primer fotograma (lo agachado queda antes) y hacia delante
        // hasta volver a ella.
        if (info.rise > 0.05f) {
            std::vector<float> heights;
            for (const asset::VectorKey& k : channel->positions) heights.push_back(ecs::transformPoint(P, k.value).y);
            const std::size_t peak = static_cast<std::size_t>(std::max_element(heights.begin(), heights.end()) - heights.begin());
            const float base = start.y + 0.1f * (heights[peak] - start.y);
            std::size_t up = peak;
            while (up > 0 && heights[up - 1] > base) --up;
            std::size_t down = peak;
            while (down + 1 < heights.size() && heights[down + 1] > base) ++down;
            info.takeoff = channel->positions[up].time;
            info.landing = channel->positions[down].time;
        }

        // En el sitio: la cadera se queda encima de su punto de reposo con su
        // balanceo, sin avanzar (y sin girar, en los de girar).
        for (asset::VectorKey& k : channel->positions) {
            Vec3 w = ecs::transformPoint(P, k.value);
            const float s = std::clamp(k.time / duration, 0.0f, 1.0f);
            w.x -= (start.x - rest.x) + drift.x * s;
            w.z -= (start.z - rest.z) + drift.z * s;
            if (remove_turn) {
                const Vec3 offset = ecs::quatRotate(yawRotation(-yawAt(k.time)), Vec3{w.x - rest.x, 0.0f, w.z - rest.z});
                w.x = rest.x + offset.x;
                w.z = rest.z + offset.z;
            }
            k.value = ecs::transformPoint(Pinv, w);
        }
    }
    if (remove_turn) {
        for (asset::QuatKey& k : channel->rotations) {
            const Quat g = ecs::quatMultiply(Prot, k.value);
            const Quat straight = ecs::quatMultiply(yawRotation(-yawAt(k.time)), g);
            k.value = core::normalize(ecs::quatMultiply(ProtInv, straight));
        }
    }
}

}  // namespace

const ClipInfo* PackResult::clip(const std::string& key) const {
    for (const ClipInfo& c : clips) {
        if (c.key == key) return &c;
    }
    return nullptr;
}

bool looksLikePack(const fs::path& source) {
    std::error_code ec;
    if (fs::is_directory(source, ec)) {
        int known = 0;
        for (const fs::path& f : fbxFiles(source)) {
            const std::string key = clipKey(f);
            if (key == "idle" || key == "walk" || key == "run") ++known;
        }
        return known >= 3;
    }
    return fs::is_regular_file(source, ec) && lower(source.extension().string()) == ".zip";
}

fs::path findDownloadedPack() {
    const char* profile = std::getenv("USERPROFILE");
    if (profile == nullptr) return {};
    const fs::path downloads = fs::path(profile) / "Downloads";
    std::error_code ec;
    fs::path best;
    for (fs::directory_iterator it(downloads, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = lower(it->path().filename().string());
        if (name.find("locomotion") == std::string::npos) continue;
        if (looksLikePack(it->path())) {
            best = it->path();
            if (it->is_directory(ec)) break;  // una carpeta ya descomprimida, mejor
        }
    }
    return best;
}

PackResult importPack(const fs::path& source, const fs::path& assets_folder,
                      const std::function<void(const std::string&, float)>& progress) {
    PackResult result;
    const auto report = [&](const std::string& stage, float fraction) {
        if (progress) progress(stage, fraction);
    };
    std::random_device rd;
    const fs::path work = fs::temp_directory_path() / ("cramion_locomotion_" + std::to_string(rd()));
    std::error_code ec;
    fs::create_directories(work, ec);
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(path, e);
        }
    } cleanup{work};

    // --- Los FBX: del zip o de la carpeta ---
    fs::path folder = source;
    if (fs::is_regular_file(source, ec)) {
        report("Descomprimiendo el pack", 0.02f);
        std::string error;
        if (!update::extractZip(source, work / "pack", {}, &error)) {
            result.error = "No se pudo descomprimir " + source.filename().string() + ": " + error;
            return result;
        }
        folder = work / "pack";
    }
    const std::vector<fs::path> files = fbxFiles(folder);
    fs::path character;
    std::uintmax_t character_size = 0;
    std::vector<fs::path> animations;
    for (const fs::path& f : files) {
        if (!clipKey(f).empty()) {
            animations.push_back(f);
        } else if (const std::uintmax_t size = fs::file_size(f, ec); !ec && size > character_size) {
            character = f;
            character_size = size;
        }
    }
    if (character.empty() || animations.empty()) {
        result.error = "No parece el Locomotion Pack de Mixamo (hace falta el personaje y sus animaciones en FBX)";
        return result;
    }

    // --- El personaje ---
    report("Importando el personaje (puede tardar)", 0.05f);
    const fs::path models = assets_folder / "Models" / "Locomotion";
    fs::create_directories(models, ec);
    assets::ModelImportSettings settings;
    settings.animated = true;
    assets::ImportResult imported = assets::importModel(character, models, settings);
    if (!imported.ok) {
        result.error = "No se pudo importar el personaje: " + imported.message;
        return result;
    }
    // El esqueleto del personaje (las animaciones se asignan a sus huesos) y
    // su altura. El FBX de Mixamo dice que va en centimetros y el importador
    // lo pasa a metros (Convert Units); si un FBX no lo dijera y la cadera
    // saliera a mas de 10 m, se reimporta con Scale Factor 0.01.
    std::shared_ptr<assets::ModelAsset> model;
    asset::ModelData* skeleton = nullptr;
    int hips_node = -1;
    const auto readSkeleton = [&] {
        model = assets::AssetManager::readModel(imported.info.uuid, imported.info.path, imported.info.name, false);
        skeleton = nullptr;
        hips_node = -1;
        if (!model) return;
        for (const auto& part : model->parts) {
            const humanoid::Map map = humanoid::detect(part->nodes);
            if (map[humanoid::Bone::Hips] >= 0) {
                skeleton = part.get();
                hips_node = map[humanoid::Bone::Hips];
                return;
            }
        }
    };
    const auto hipsHeight = [&] { return restGlobals(skeleton->nodes)[static_cast<std::size_t>(hips_node)].m[3][1]; };
    readSkeleton();
    if (skeleton == nullptr) {
        result.error = "El personaje no tiene un esqueleto humanoide";
        return result;
    }
    if (hipsHeight() > 10.0f) {
        settings.scale = 0.01f;
        imported = assets::reimportModel(imported.info, settings);
        if (!imported.ok) {
            result.error = "No se pudo importar el personaje a escala: " + imported.message;
            return result;
        }
        readSkeleton();
        if (skeleton == nullptr) {
            result.error = "El personaje no tiene un esqueleto humanoide";
            return result;
        }
    }
    result.character = imported.info.uuid;
    result.character_relative = fs::relative(imported.info.path, assets_folder, ec).generic_string();
    result.hips_height = hipsHeight();
    result.log.push_back("personaje " + character.filename().string() + " -> " + result.character_relative +
                         " (cadera a " + std::to_string(result.hips_height) + " m)");

    // --- Las animaciones: clip suelto, medido y en el sitio ---
    const fs::path clips_folder = assets_folder / "Animations" / "Locomotion";
    fs::create_directories(clips_folder, ec);
    for (std::size_t i = 0; i < animations.size(); ++i) {
        const fs::path& file = animations[i];
        const std::string key = clipKey(file);
        report("Animacion " + file.stem().string(), 0.5f + 0.45f * static_cast<float>(i) / static_cast<float>(animations.size()));
        std::string error;
        // (Con la misma escala que el personaje: Scale Factor y unidades.)
        std::vector<asset::AnimationClip> loaded =
            asset::loadAnimationClips(file, skeleton->nodes, &error, settings.scale, settings.convert_units);
        if (loaded.empty()) {
            result.log.push_back("  " + key + ": no se pudo leer (" + error + ")");
            continue;
        }
        asset::AnimationClip clip = std::move(loaded.front());
        clip.name = key;
        ClipInfo info;
        info.key = key;
        const bool turn = key.rfind("turn", 0) == 0;
        processClip(*skeleton, clip, info, turn);
        const fs::path out = clips_folder / (key + ".cranim");
        if (!ecs::saveAnimationClip(*skeleton, clip, out, &error)) {
            result.log.push_back("  " + key + ": no se pudo guardar (" + error + ")");
            continue;
        }
        info.relative = fs::relative(out, assets_folder, ec).generic_string();
        info.uuid = headerUuid(out);
        char line[240];
        std::snprintf(line, sizeof(line), "  %-18s %.2f s  avanza %.2f m/s  gira %.0f grados  sube %.2f m  (aire %.2f..%.2f s)",
                      key.c_str(), info.duration, info.speed, info.turn_degrees, info.rise, info.takeoff, info.landing);
        result.log.push_back(line);
        result.clips.push_back(info);
    }
    report("Listo", 1.0f);
    result.ok = result.clip("idle") != nullptr && result.clip("walk") != nullptr && result.clip("run") != nullptr;
    if (!result.ok) result.error = "Faltan animaciones basicas (idle, walking o running) en el pack";
    return result;
}

}  // namespace cramion::editor::locomotion
