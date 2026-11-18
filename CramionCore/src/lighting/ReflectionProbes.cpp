#include "CramionCore/lighting/ReflectionProbes.h"

#include <CramionFX/scene/Scene.h>
#include <CramionFX/vk/VulkanRenderer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <system_error>

namespace cramion::lighting {
namespace {

constexpr char kMagic[4] = {'C', 'R', 'R', 'F'};
// Tope de lo que se acepta al leer (un archivo danado no pide gigas).
constexpr std::uint32_t kMaxCubesInFile = 4096;
constexpr std::uint32_t kMaxCubeSize = 1024;

std::uint64_t fnv1a(const std::uint16_t* data, std::size_t count) {
    std::uint64_t hash = 1469598103934665603ull;
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < count * sizeof(std::uint16_t); ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

template <typename T>
void put(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
bool get(std::ifstream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(in);
}

core::Vec3 boxSize(const ReflectionProbeBox& probe) {
    return core::Vec3{std::max(std::fabs(probe.size.x), 0.1f), std::max(std::fabs(probe.size.y), 0.1f),
                      std::max(std::fabs(probe.size.z), 0.1f)};
}

}  // namespace

// --- Componente ------------------------------------------------------------------------

void ReflectionProbeBox::reflect(ecs::PropertyVisitor& v) {
    v.field({"size", "Tamaño", "Caja de influencia y de proyeccion, centrada en el objeto (m, sin giro)"}, size,
            ecs::Vec3Kind::Scale);
    v.field({"blend_distance", "Fundido", "Metros hacia dentro desde el borde de la caja en que la sonda entra poco a poco"},
            blend_distance, ecs::FloatRange{0.0f, 20.0f, 0.05f, "%.2f m"});
    v.field({"priority", "Prioridad", "Donde se solapan, manda la de mas prioridad (a igual prioridad, la caja mas pequena)"},
            priority, -100, 100);
    v.field({"intensity", "Intensidad", "Multiplica el reflejo de la sonda"}, intensity,
            ecs::FloatRange{0.0f, 4.0f, 0.01f, "%.2f"});
    v.field({"box_projection", "Proyeccion de caja",
             "Corrige el reflejo con las paredes de la caja (interiores); apagado, como un cielo lejano"},
            box_projection);
}

void registerReflectionProbeComponent() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("ReflectionProbeBox") == nullptr) {
        registry.registerComponent<ReflectionProbeBox>("ReflectionProbeBox", "Reflection Probe", "Iluminacion");
    }
}

gfx::PlacedProbeId placedProbeId(const Uuid& id) { return gfx::PlacedProbeId{id.high, id.low}; }

gfx::PlacedReflectionProbe placedProbeOf(const ReflectionProbeBox& probe, const Uuid& id, const core::Vec3& position) {
    gfx::PlacedReflectionProbe out;
    out.id = placedProbeId(id);
    out.position = position;
    const core::Vec3 half = boxSize(probe) * 0.5f;
    out.box_min = position - half;
    out.box_max = position + half;
    out.blend_distance = std::max(probe.blend_distance, 0.0f);
    out.intensity = std::max(probe.intensity, 0.0f);
    out.priority = probe.priority;
    out.box_projection = probe.box_projection;
    return out;
}

// --- Archivo ---------------------------------------------------------------------------

std::filesystem::path reflectionProbesFile(const std::filesystem::path& scene_file) {
    std::filesystem::path p = scene_file;
    p.replace_extension(kReflectionProbesExtension);
    return p;
}

bool saveReflectionProbes(const std::filesystem::path& file, const std::vector<gfx::BakedReflectionCube>& cubes,
                          std::string* error) {
    std::vector<const gfx::BakedReflectionCube*> valid;
    for (const gfx::BakedReflectionCube& cube : cubes) {
        if (cube.id.valid() && gfx::bakedCubeValid(cube)) valid.push_back(&cube);
    }
    std::error_code ec;
    if (valid.empty()) {
        std::filesystem::remove(file, ec);
        return true;
    }
    std::filesystem::path temporary = file;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "no se pudo escribir " + file.string();
            return false;
        }
        out.write(kMagic, 4);
        put(out, kReflectionProbesVersion);
        put(out, static_cast<std::uint32_t>(valid.size()));
        put(out, std::uint32_t{0});
        for (const gfx::BakedReflectionCube* cube : valid) {
            put(out, cube->id.high);
            put(out, cube->id.low);
            put(out, cube->position.x);
            put(out, cube->position.y);
            put(out, cube->position.z);
            put(out, cube->size);
            put(out, cube->mips);
            put(out, static_cast<std::uint64_t>(cube->texels.size()));
            put(out, fnv1a(cube->texels.data(), cube->texels.size()));
            out.write(reinterpret_cast<const char*>(cube->texels.data()),
                      static_cast<std::streamsize>(cube->texels.size() * sizeof(std::uint16_t)));
        }
        if (!out) {
            if (error) *error = "no se pudo escribir " + file.string();
            out.close();
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
    std::filesystem::rename(temporary, file, ec);
    if (ec) {
        if (error) *error = ec.message();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

bool loadReflectionProbes(const std::filesystem::path& file, std::vector<gfx::BakedReflectionCube>& cubes,
                          std::string* error) {
    cubes.clear();
    const auto damaged = [&] {
        if (error) *error = "archivo de sondas de reflexion danado: " + file.string();
        cubes.clear();
        return false;
    };
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        if (error) *error = "no existe " + file.string();
        return false;
    }
    char magic[4] = {};
    in.read(magic, 4);
    std::uint32_t version = 0;
    std::uint32_t count = 0;
    std::uint32_t reserved = 0;
    if (!in || std::memcmp(magic, kMagic, 4) != 0 || !get(in, version) || !get(in, count) || !get(in, reserved)) {
        return damaged();
    }
    if (version < 1 || version > kReflectionProbesVersion || count > kMaxCubesInFile) return damaged();
    std::error_code ec;
    const std::uint64_t file_size = std::filesystem::file_size(file, ec);
    cubes.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        gfx::BakedReflectionCube cube;
        std::uint64_t texel_count = 0;
        std::uint64_t hash = 0;
        if (!get(in, cube.id.high) || !get(in, cube.id.low) || !get(in, cube.position.x) || !get(in, cube.position.y) ||
            !get(in, cube.position.z) || !get(in, cube.size) || !get(in, cube.mips) || !get(in, texel_count) ||
            !get(in, hash)) {
            return damaged();
        }
        if (cube.size == 0 || cube.size > kMaxCubeSize || cube.mips == 0 || cube.mips > 16 ||
            texel_count != gfx::bakedCubeTexelCount(cube.size, cube.mips)) {
            return damaged();
        }
        // Que quepa en lo que queda del archivo antes de reservar.
        const auto position = static_cast<std::uint64_t>(in.tellg());
        if (ec || position + texel_count * sizeof(std::uint16_t) > file_size) return damaged();
        cube.texels.resize(static_cast<std::size_t>(texel_count));
        in.read(reinterpret_cast<char*>(cube.texels.data()),
                static_cast<std::streamsize>(texel_count * sizeof(std::uint16_t)));
        if (!in || fnv1a(cube.texels.data(), cube.texels.size()) != hash) return damaged();
        cubes.push_back(std::move(cube));
    }
    return true;
}

// --- Con el renderizador -------------------------------------------------------------------

void syncReflectionProbes(ecs::World& world, gfx::VulkanRenderer& renderer) {
    std::vector<gfx::PlacedReflectionProbe> probes;
    for (const entt::entity h : world.registry().view<ReflectionProbeBox>()) {
        const ecs::Entity e = world.wrap(h);
        if (!e.activeInHierarchy()) continue;
        probes.push_back(placedProbeOf(e.get<ReflectionProbeBox>(), e.uuid(), e.worldPosition()));
    }
    renderer.setPlacedReflectionProbes(std::move(probes));
}

void loadSceneReflectionProbes(const std::filesystem::path& scene_file, gfx::VulkanRenderer& renderer) {
    std::vector<gfx::BakedReflectionCube> cubes;
    if (!scene_file.empty()) {
        const std::filesystem::path file = reflectionProbesFile(scene_file);
        std::error_code ec;
        std::string error;
        if (std::filesystem::exists(file, ec) && !loadReflectionProbes(file, cubes, &error)) {
            std::cerr << "[Iluminacion] " << error << "\n";
            cubes.clear();
        }
    }
    // Sin sondas antes y sin sondas ahora: nada que subir (ni esperar a la GPU).
    if (cubes.empty() && renderer.bakedReflectionCubes().empty()) return;
    renderer.setBakedReflectionCubes(std::move(cubes));
}

ReflectionBakeReport bakeReflectionProbes(ecs::World& world, gfx::VulkanRenderer& renderer, const scene::Scene& scene,
                                          const std::filesystem::path& scene_file, const std::vector<Uuid>& only) {
    ReflectionBakeReport report;
    if (scene_file.empty()) {
        report.error = "guarda la escena primero: las sondas se guardan junto a ella (.crrefl)";
        return report;
    }
    report.file = reflectionProbesFile(scene_file);

    // Las sondas de la escena (las que siguen existiendo conservan su
    // horneado si no se hornean ahora).
    struct Target {
        Uuid id;
        core::Vec3 position;
    };
    std::vector<Target> targets;
    std::vector<gfx::PlacedProbeId> existing;
    for (const entt::entity h : world.registry().view<ReflectionProbeBox>()) {
        const ecs::Entity e = world.wrap(h);
        existing.push_back(placedProbeId(e.uuid()));
        if (!e.activeInHierarchy()) continue;
        ++report.total;
        if (!only.empty() && std::find(only.begin(), only.end(), e.uuid()) == only.end()) continue;
        targets.push_back(Target{e.uuid(), e.worldPosition()});
    }
    std::vector<gfx::BakedReflectionCube> cubes;
    for (const gfx::BakedReflectionCube& cube : renderer.bakedReflectionCubes()) {
        if (std::find(existing.begin(), existing.end(), cube.id) != existing.end()) cubes.push_back(cube);
    }
    if (targets.empty() && only.empty() && report.total == 0) {
        report.error = "no hay ninguna Reflection Probe en la escena";
    }

    for (const Target& target : targets) {
        gfx::BakedReflectionCube cube;
        std::string error;
        if (!renderer.bakeReflectionCube(scene, target.position, cube, &error)) {
            ++report.failed;
            report.error = error;
            // Lo que impide una impide todas (modo compatible, rayos...).
            break;
        }
        cube.id = placedProbeId(target.id);
        const auto same = std::find_if(cubes.begin(), cubes.end(),
                                       [&](const gfx::BakedReflectionCube& c) { return c.id == cube.id; });
        if (same != cubes.end()) {
            *same = std::move(cube);
        } else {
            cubes.push_back(std::move(cube));
        }
        ++report.baked;
    }
    if (cubes.size() > gfx::kMaxPlacedProbeCubes) {
        report.error = "hay mas de " + std::to_string(gfx::kMaxPlacedProbeCubes) +
                       " sondas horneadas: solo se usan las primeras";
    }
    std::string error;
    if (report.baked > 0 || cubes.size() != renderer.bakedReflectionCubes().size()) {
        if (!saveReflectionProbes(report.file, cubes, &error)) {
            report.error = error;
        }
        renderer.setBakedReflectionCubes(std::move(cubes));
    }
    return report;
}

}  // namespace cramion::lighting
