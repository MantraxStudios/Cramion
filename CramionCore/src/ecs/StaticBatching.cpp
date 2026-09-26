#include "CramionCore/ecs/StaticBatching.h"

#include "CramionCore/asset/AssetManager.h"
#include "CramionCore/asset/Importer.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/physics/PhysicsComponents.h"

#include <CramionFX/asset/Model.h>

#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace cramion::ecs {

namespace {

using core::Mat4;
using core::Vec3;
using core::Vec4;

// FNV-1a de 64 bits.
struct Hasher {
    std::uint64_t h = 1469598103934665603ull;
    void bytes(const void* data, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    }
    template <typename T>
    void pod(const T& value) {
        bytes(&value, sizeof(T));
    }
};

// Transform global del hueso de una pieza rigida (nodo x offset): la pose de
// reposo con la que el renderizador la dibuja.
Mat4 restMatrix(const asset::ModelData& part) {
    if (part.bones.empty()) return Mat4::identity();
    const asset::Bone& bone = part.bones[0];
    Mat4 global = Mat4::identity();
    for (std::int32_t n = bone.node; n >= 0 && static_cast<std::size_t>(n) < part.nodes.size();
         n = part.nodes[static_cast<std::size_t>(n)].parent) {
        global = part.nodes[static_cast<std::size_t>(n)].local * global;
    }
    return global * bone.offset;
}

float determinant3(const Mat4& m) {
    const Vec3 c0{m.m[0][0], m.m[0][1], m.m[0][2]};
    const Vec3 c1{m.m[1][0], m.m[1][1], m.m[1][2]};
    const Vec3 c2{m.m[2][0], m.m[2][1], m.m[2][2]};
    return c0.x * (c1.y * c2.z - c1.z * c2.y) - c1.x * (c0.y * c2.z - c0.z * c2.y) +
           c2.x * (c0.y * c1.z - c0.z * c1.y);
}

Vec3 mulPoint(const Mat4& m, const Vec3& p) {
    const Vec4 r = m * Vec4{p.x, p.y, p.z, 1.0f};
    return Vec3{r.x, r.y, r.z};
}

Vec3 mulDirection(const Mat4& m, const Vec3& d) {
    const Vec4 r = m * Vec4{d.x, d.y, d.z, 0.0f};
    return Vec3{r.x, r.y, r.z};
}

// Normales: inversa traspuesta (escalas no uniformes).
Vec3 mulNormal(const Mat4& inverse, const Vec3& n) {
    return Vec3{inverse.m[0][0] * n.x + inverse.m[0][1] * n.y + inverse.m[0][2] * n.z,
                inverse.m[1][0] * n.x + inverse.m[1][1] * n.y + inverse.m[1][2] * n.z,
                inverse.m[2][0] * n.x + inverse.m[2][1] * n.y + inverse.m[2][2] * n.z};
}

Vec3 safeNormalize(const Vec3& v, const Vec3& fallback) {
    const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return length > 1e-12f ? Vec3{v.x / length, v.y / length, v.z / length} : fallback;
}

bool hasComponent(World& world, Entity e, const char* name) {
    const ComponentType* type = ComponentRegistry::instance().find(name);
    return type != nullptr && type->has(world, e.handle());
}

// Algo que puede mover el objeto en el juego.
bool movable(World& world, Entity e) {
    if (e.has<Animator>() || hasComponent(world, e, "Script")) return true;
    for (Entity p = e; p.valid(); p = p.parent()) {
        if (const auto* body = p.tryGet<physics::Rigidbody>(); body != nullptr && body->type != physics::BodyType::Static) {
            return true;
        }
    }
    return false;
}

struct Candidate {
    Entity entity;
    std::shared_ptr<const assets::ModelAsset> model;
    const asset::ModelData* part = nullptr;
    Uuid model_uuid;
    int part_index = 0;
    std::vector<Uuid> overrides;  // .crmat por hueco (invalido = el suyo)
    ShadowCasting shadows = ShadowCasting::On;
};

// Una pieza del lote en construccion.
struct Batch {
    ShadowCasting shadows = ShadowCasting::On;
    asset::ModelData data;
    std::vector<Uuid> overrides;  // por hueco de material del lote
    std::unordered_map<std::uint64_t, std::uint32_t> material_slots;
    std::unordered_map<std::uint64_t, std::int32_t> texture_slots;
};

class Combiner {
public:
    explicit Combiner(std::size_t max_vertices) : max_vertices_(max_vertices) {}

    void add(const Candidate& c, const Mat4& world_matrix) {
        const asset::ModelData& part = *c.part;
        bool overrides = false;
        for (const Uuid& o : c.overrides) overrides = overrides || o.valid();
        Batch& batch = batchFor(c.shadows, overrides, part.vertices.size());

        const Mat4 to_world = world_matrix * restMatrix(part);
        const Mat4 inverse = core::inverse(to_world);
        const bool mirrored = determinant3(to_world) < 0.0f;

        // Huecos de material de esta pieza en el lote.
        std::vector<std::uint32_t> slot_of(part.materials.size(), 0);
        for (std::size_t m = 0; m < part.materials.size(); ++m) {
            const Uuid override = m < c.overrides.size() ? c.overrides[m] : Uuid{};
            slot_of[m] = materialSlot(batch, part, static_cast<std::uint32_t>(m), override);
        }

        const auto base = static_cast<std::uint32_t>(batch.data.vertices.size());
        batch.data.vertices.reserve(batch.data.vertices.size() + part.vertices.size());
        for (asset::SkinnedVertex v : part.vertices) {
            v.position = mulPoint(to_world, v.position);
            v.normal = safeNormalize(mulNormal(inverse, v.normal), Vec3{0.0f, 1.0f, 0.0f});
            const Vec3 t = safeNormalize(mulDirection(to_world, Vec3{v.tangent.x, v.tangent.y, v.tangent.z}),
                                         Vec3{1.0f, 0.0f, 0.0f});
            // Con un espejo (escala negativa) la bitangente cambia de lado.
            v.tangent = Vec4{t.x, t.y, t.z, mirrored ? -v.tangent.w : v.tangent.w};
            v.joints[0] = v.joints[1] = v.joints[2] = v.joints[3] = 0;
            v.weights[0] = 1.0f;
            v.weights[1] = v.weights[2] = v.weights[3] = 0.0f;
            batch.data.vertices.push_back(v);
        }

        for (const asset::SubMesh& submesh : part.submeshes) {
            if (submesh.material >= slot_of.size()) continue;
            asset::SubMesh out{};
            out.first_index = static_cast<std::uint32_t>(batch.data.indices.size());
            out.material = slot_of[submesh.material];
            out.node = 0;
            for (std::uint32_t i = 0; i + 2 < submesh.index_count; i += 3) {
                const std::uint32_t* tri = &part.indices[submesh.first_index + i];
                // Espejo: se invierte el orden para que la cara siga mirando fuera.
                batch.data.indices.push_back(base + tri[0]);
                batch.data.indices.push_back(base + (mirrored ? tri[2] : tri[1]));
                batch.data.indices.push_back(base + (mirrored ? tri[1] : tri[2]));
            }
            out.index_count = static_cast<std::uint32_t>(batch.data.indices.size()) - out.first_index;
            if (out.index_count > 0) batch.data.submeshes.push_back(out);
        }
    }

    // Cierra los lotes: un hueso identidad y clusteres espaciales (que
    // tambien juntan las submallas del mismo material).
    std::vector<Batch> finish() {
        for (auto& [mode, batch] : open_) {
            (void)mode;
            done_.push_back(std::move(batch));
        }
        open_.clear();
        std::vector<Batch> result;
        for (Batch& batch : done_) {
            if (batch.data.submeshes.empty()) continue;
            const std::string name = "Static Batch " + std::to_string(result.size());
            batch.data.name = name;
            batch.data.nodes = {asset::Node{name, -1, Mat4::identity()}};
            batch.data.bones = {asset::Bone{name, 0, Mat4::identity()}};
            asset::clusterSubmeshes(batch.data);
            result.push_back(std::move(batch));
        }
        done_.clear();
        return result;
    }

private:
    Batch& batchFor(ShadowCasting shadows, bool overrides, std::size_t incoming_vertices) {
        const int key = static_cast<int>(shadows) * 2 + (overrides ? 1 : 0);
        auto it = open_.find(key);
        if (it != open_.end() && !it->second.data.vertices.empty() &&
            it->second.data.vertices.size() + incoming_vertices > max_vertices_) {
            done_.push_back(std::move(it->second));
            open_.erase(it);
            it = open_.end();
        }
        if (it == open_.end()) {
            it = open_.emplace(key, Batch{}).first;
            it->second.shadows = shadows;
        }
        return it->second;
    }

    std::uint64_t textureHash(const asset::ModelData& part, std::int32_t index) {
        if (index < 0 || static_cast<std::size_t>(index) >= part.textures.size()) return 0;
        const auto key = std::make_pair(&part, index);
        if (const auto it = texture_hashes_.find(key); it != texture_hashes_.end()) return it->second;
        const asset::TextureData& t = part.textures[static_cast<std::size_t>(index)];
        Hasher h;
        h.pod(t.width);
        h.pod(t.height);
        h.pod(t.format);
        h.pod(t.mip_levels);
        h.pod(t.height_map);
        h.bytes(t.encoded.data(), t.encoded.size());
        h.bytes(t.pixels.data(), t.pixels.size());
        h.bytes(t.source_path.data(), t.source_path.size());
        const std::uint64_t value = h.h | 1;  // nunca 0 (0 = sin textura)
        texture_hashes_.emplace(key, value);
        return value;
    }

    std::int32_t textureSlot(Batch& batch, const asset::ModelData& part, std::int32_t index) {
        const std::uint64_t hash = textureHash(part, index);
        if (hash == 0) return -1;
        const auto [it, inserted] =
            batch.texture_slots.try_emplace(hash, static_cast<std::int32_t>(batch.data.textures.size()));
        if (inserted) batch.data.textures.push_back(part.textures[static_cast<std::size_t>(index)]);
        return it->second;
    }

    std::uint32_t materialSlot(Batch& batch, const asset::ModelData& part, std::uint32_t m, const Uuid& override) {
        const asset::MaterialData& src = part.materials[m];
        // Por contenido: dos modelos con el mismo material (factores y
        // texturas) comparten hueco y, por tanto, llamada de dibujo.
        Hasher h;
        h.pod(src.base_color);
        h.pod(src.emissive);
        h.pod(src.metallic);
        h.pod(src.roughness);
        h.pod(src.occlusion_strength);
        h.pod(src.normal_scale);
        h.pod(src.reflectance);
        h.pod(src.normal_map_directx);
        h.pod(src.transparent);
        h.pod(src.surface_shader);
        h.pod(src.surface_params);
        for (const std::int32_t t : {src.albedo_texture, src.metallic_roughness_texture, src.normal_texture,
                                     src.occlusion_texture, src.emissive_texture}) {
            h.pod(textureHash(part, t));
        }
        for (const std::int32_t t : src.surface_textures) h.pod(textureHash(part, t));
        h.pod(override);
        const auto [it, inserted] =
            batch.material_slots.try_emplace(h.h, static_cast<std::uint32_t>(batch.data.materials.size()));
        if (inserted) {
            asset::MaterialData material = src;
            material.albedo_texture = textureSlot(batch, part, src.albedo_texture);
            material.metallic_roughness_texture = textureSlot(batch, part, src.metallic_roughness_texture);
            material.normal_texture = textureSlot(batch, part, src.normal_texture);
            material.occlusion_texture = textureSlot(batch, part, src.occlusion_texture);
            material.emissive_texture = textureSlot(batch, part, src.emissive_texture);
            for (std::int32_t& t : material.surface_textures) t = textureSlot(batch, part, t);
            batch.data.materials.push_back(std::move(material));
            batch.overrides.push_back(override);
        }
        return it->second;
    }

    struct PairHash {
        std::size_t operator()(const std::pair<const asset::ModelData*, std::int32_t>& k) const noexcept {
            return std::hash<const void*>{}(k.first) ^ (static_cast<std::size_t>(k.second) * 0x9E3779B97F4A7C15ull);
        }
    };

    std::size_t max_vertices_;
    std::unordered_map<int, Batch> open_;
    std::vector<Batch> done_;
    std::unordered_map<std::pair<const asset::ModelData*, std::int32_t>, std::uint64_t, PairHash> texture_hashes_;
};

std::string meshKey(const Uuid& model, int part) { return model.toString() + "#" + std::to_string(part); }

}  // namespace

bool buildStaticBatch(World& world, const assets::AssetDatabase& database, const std::filesystem::path& model_file,
                      const StaticBatchOptions& options, StaticBatchReport& report) {
    report = StaticBatchReport{};

    // --- Candidatos: Static, visibles, activos y con malla de un asset ---
    std::unordered_map<Uuid, std::shared_ptr<const assets::ModelAsset>> models;
    std::vector<Candidate> candidates;
    std::unordered_map<std::string, std::size_t> instances;
    // Copias de cada malla en la escena (Static o no).
    world.forEachDepthFirst([&](Entity e) {
        const MeshRenderer* renderer = e.tryGet<MeshRenderer>();
        if (renderer != nullptr && !renderer->mesh && renderer->model.valid()) {
            ++instances[meshKey(renderer->model.uuid, renderer->part)];
        }
    });
    world.forEachDepthFirst([&](Entity e) {
        const EntityInfo* info = e.tryGet<EntityInfo>();
        const MeshRenderer* renderer = e.tryGet<MeshRenderer>();
        if (info == nullptr || !info->is_static || info->static_batched || renderer == nullptr) return;
        if (!renderer->visible || renderer->mesh || !renderer->model.valid() || !e.activeInHierarchy()) return;
        if (movable(world, e)) {
            ++report.kept_movable;
            return;
        }
        const Uuid uuid = renderer->model.uuid;
        auto it = models.find(uuid);
        if (it == models.end()) {
            std::shared_ptr<const assets::ModelAsset> asset;
            if (const auto found = database.find(uuid); found && found->type == assets::AssetType::Model) {
                asset = assets::AssetManager::readModel(uuid, found->path, found->name, /*decode_textures=*/false);
            } else {
                asset = assets::AssetManager::readModel(uuid, {}, "", false);  // primitiva o nada
            }
            it = models.emplace(uuid, std::move(asset)).first;
        }
        const auto& asset = it->second;
        if (!asset || renderer->part < 0 || static_cast<std::size_t>(renderer->part) >= asset->parts.size()) return;
        const asset::ModelData& part = *asset->parts[static_cast<std::size_t>(renderer->part)];
        if (part.bones.size() > 1 || !part.animations.empty() || part.vertices.empty()) {
            ++report.kept_movable;  // con esqueleto: se deforma
            return;
        }
        Candidate c;
        c.entity = e;
        c.model = asset;
        c.part = &part;
        c.model_uuid = uuid;
        c.part_index = renderer->part;
        c.shadows = renderer->cast_shadows;
        for (const assets::AssetRef& ref : renderer->materials) c.overrides.push_back(ref.valid() ? ref.uuid : Uuid{});
        candidates.push_back(std::move(c));
    });

    // --- Combinar ---
    Combiner combiner(options.max_vertices);
    std::vector<Entity> combined;
    std::unordered_map<std::string, bool> counted_draws;
    for (const Candidate& c : candidates) {
        const std::size_t copies = instances[meshKey(c.model_uuid, c.part_index)];
        if (copies > 1 && copies * c.part->vertices.size() > options.max_copied_vertices) {
            ++report.kept_instanced;
            continue;
        }
        // Lotes que habia: (malla x material x .crmat); las instancias ya
        // compartian llamada.
        for (std::size_t m = 0; m < c.part->materials.size(); ++m) {
            std::string key = meshKey(c.model_uuid, c.part_index) + "/" + std::to_string(m);
            if (m < c.overrides.size()) key += "/" + c.overrides[m].toString();
            if (!counted_draws.emplace(key, true).second) continue;
            ++report.draws_before;
        }
        combiner.add(c, c.entity.worldMatrix());
        combined.push_back(c.entity);
    }
    if (combined.empty()) {
        report.message = "sin mallas estaticas que combinar";
        if (report.kept_instanced + report.kept_movable > 0) {
            report.message += " (" + std::to_string(report.kept_instanced) + " repetidas y grandes, instanciadas; " +
                              std::to_string(report.kept_movable) + " que se pueden mover)";
        }
        return false;
    }

    std::vector<Batch> batches = combiner.finish();
    std::vector<asset::ModelData> parts;
    std::vector<assets::ModelNode> nodes;
    nodes.push_back(assets::ModelNode{"Static Batch", -1, Mat4::identity(), -1});
    for (Batch& batch : batches) {
        report.triangles += batch.data.indices.size() / 3;
        report.draws_after += batch.data.materials.size();
        nodes.push_back(assets::ModelNode{batch.data.name, 0, Mat4::identity(), static_cast<std::int32_t>(parts.size())});
        parts.push_back(batch.data);
    }
    report.parts = parts.size();

    const Uuid uuid = Uuid::generate();
    std::string error;
    if (!assets::writeGeneratedModel(model_file, uuid, "Static Batch", nodes, parts, &error)) {
        report.message = error;
        return false;
    }

    // --- La escena: los originales se marcan y el lote se dibuja aparte ---
    for (Entity e : combined) e.get<EntityInfo>().static_batched = true;
    report.combined = combined.size();
    // Sin la marca Static: el lote no se vuelve a combinar.
    Entity root = world.create("Static Batch");
    for (std::size_t i = 0; i < batches.size(); ++i) {
        Entity child = batches.size() == 1 ? root : world.create(batches[i].data.name, root);
        MeshRenderer& renderer = child.add<MeshRenderer>();
        renderer.model = assets::AssetRef{uuid, assets::AssetType::Model};
        renderer.part = static_cast<int>(i);
        renderer.cast_shadows = batches[i].shadows;
        bool any_override = false;
        for (const Uuid& o : batches[i].overrides) any_override = any_override || o.valid();
        if (any_override) {
            for (const Uuid& o : batches[i].overrides) {
                renderer.materials.push_back(o.valid() ? assets::AssetRef{o, assets::AssetType::Material}
                                                       : assets::AssetRef{{}, assets::AssetType::Material});
            }
        }
    }

    report.message = std::to_string(report.combined) + " objetos en " + std::to_string(report.parts) +
                     " lote(s): " + std::to_string(report.draws_before) + " -> " +
                     std::to_string(report.draws_after) + " llamadas de dibujo, " +
                     std::to_string(report.triangles) + " triangulos";
    if (report.kept_instanced > 0) {
        report.message += "; " + std::to_string(report.kept_instanced) + " repetidos y grandes se quedan instanciados";
    }
    if (report.kept_movable > 0) {
        report.message += "; " + std::to_string(report.kept_movable) + " con Animator/Script/Rigidbody no se combinan";
    }
    return true;
}

}  // namespace cramion::ecs
