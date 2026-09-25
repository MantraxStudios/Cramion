#include "CramionCore/ecs/RenderSync.h"

#include "CramionCore/ecs/MathUtil.h"

#include <CramionFX/asset/ImageFile.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

namespace cramion::ecs {

using core::Mat4;
using core::Vec3;

namespace {
constexpr float kDegToRad = core::kPi / 180.0f;

std::filesystem::path fromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

bool readBytes(const std::filesystem::path& file, std::vector<std::uint8_t>& out) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamsize size = in.tellg();
    in.seekg(0);
    out.resize(static_cast<std::size_t>(std::max<std::streamsize>(size, 0)));
    return static_cast<bool>(in.read(reinterpret_cast<char*>(out.data()), size));
}

// Solo los factores de `from` (las texturas de `to` se quedan).
void copyFactors(asset::MaterialData& to, const asset::MaterialData& from) {
    to.base_color = from.base_color;
    to.emissive = from.emissive;
    to.metallic = from.metallic;
    to.roughness = from.roughness;
    to.occlusion_strength = from.occlusion_strength;
    to.normal_scale = from.normal_scale;
    to.normal_map_directx = from.normal_map_directx;
    to.reflectance = from.reflectance;
}
}  // namespace

std::shared_ptr<const assets::MaterialAsset> RenderSync::material(const Uuid& uuid) {
    if (const auto it = materials_.find(uuid); it != materials_.end()) return it->second.data;
    if (failed_materials_.contains(uuid)) return nullptr;
    const auto info = assets_.database().find(uuid);
    auto data = std::make_shared<assets::MaterialAsset>();
    std::string error;
    if (!info || info->type != assets::AssetType::Material || !assets::loadMaterial(info->path, *data, &error)) {
        failed_materials_.insert(uuid);
        std::cerr << "[RenderSync] No se pudo leer el material " << uuid.toString() << " " << error << "\n";
        return nullptr;
    }
    data->uuid = uuid;
    materials_[uuid] = MaterialEntry{data, assets::materialStructureHash(*data)};
    return data;
}

void RenderSync::reloadMaterial(const Uuid& uuid) {
    failed_materials_.erase(uuid);
    const auto it = materials_.find(uuid);
    if (it == materials_.end()) return;  // nadie lo usa aun: se leera al usarlo
    const std::uint64_t old_structure = it->second.structure;
    materials_.erase(it);
    if (!material(uuid)) return;
    if (materials_[uuid].structure != old_structure) {
        rebuild_materials_.insert(uuid);
    } else {
        live_materials_.insert(uuid);
    }
}

void RenderSync::updateMaterial(const Uuid& uuid, const assets::MaterialAsset& data) {
    failed_materials_.erase(uuid);
    auto shared = std::make_shared<assets::MaterialAsset>(data);
    shared->uuid = uuid;
    const std::uint64_t structure = assets::materialStructureHash(*shared);
    const auto it = materials_.find(uuid);
    const bool known = it != materials_.end();
    const std::uint64_t old_structure = known ? it->second.structure : 0;
    materials_[uuid] = MaterialEntry{shared, structure};
    if (!known) return;
    if (structure != old_structure) {
        rebuild_materials_.insert(uuid);
    } else {
        live_materials_.insert(uuid);
    }
}

asset::ModelData RenderSync::buildVariant(const asset::ModelData& base, const std::vector<Uuid>& overrides) {
    asset::ModelData v;
    v.name = base.name;
    v.vertices = base.vertices;
    v.indices = base.indices;
    v.submeshes = base.submeshes;
    v.nodes = base.nodes;
    v.bones = base.bones;
    v.animations = base.animations;

    // Texturas del modelo que siguen usandose (las de los huecos sustituidos
    // no se copian) y las de los materiales, una vez por archivo.
    std::vector<std::int32_t> kept(base.textures.size(), -1);
    const auto keep = [&](std::int32_t t) -> std::int32_t {
        if (t < 0 || static_cast<std::size_t>(t) >= base.textures.size()) return -1;
        if (kept[t] < 0) {
            kept[t] = static_cast<std::int32_t>(v.textures.size());
            v.textures.push_back(base.textures[t]);
        }
        return kept[t];
    };
    const std::filesystem::path& root = assets_.database().root();
    std::unordered_map<std::string, std::int32_t> files;
    const auto file = [&](const std::string& relative) -> std::int32_t {
        if (relative.empty()) return -1;
        if (const auto it = files.find(relative); it != files.end()) return it->second;
        asset::TextureData texture;
        texture.name = relative;
        if (!readBytes(root / fromUtf8(relative), texture.encoded)) {
            std::cerr << "[RenderSync] Falta la textura " << relative << "\n";
            files[relative] = -1;
            return -1;
        }
        const auto index = static_cast<std::int32_t>(v.textures.size());
        v.textures.push_back(std::move(texture));
        files[relative] = index;
        return index;
    };
    // Metal y rugosidad en mapas sueltos: se juntan en uno (G = rugosidad,
    // B = metal, como glTF).
    const auto packed = [&](const std::string& metal, const std::string& rough) -> std::int32_t {
        if (metal.empty() && rough.empty()) return -1;
        const std::string key = "mr:" + metal + "|" + rough;
        if (const auto it = files.find(key); it != files.end()) return it->second;
        asset::ImageRgba8 m;
        asset::ImageRgba8 r;
        const bool has_m = !metal.empty() && asset::loadImageRgba8(root / fromUtf8(metal), m);
        const bool has_r = !rough.empty() && asset::loadImageRgba8(root / fromUtf8(rough), r);
        if (!has_m && !has_r) {
            files[key] = -1;
            return -1;
        }
        const asset::ImageRgba8& size = has_r ? r : m;
        asset::TextureData texture;
        texture.name = key;
        texture.width = size.width;
        texture.height = size.height;
        texture.pixels.resize(static_cast<std::size_t>(size.width) * size.height * 4);
        const auto at = [](const asset::ImageRgba8& img, std::uint32_t x, std::uint32_t y, std::uint32_t w,
                           std::uint32_t h) {
            const std::uint32_t sx = static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * img.width / w);
            const std::uint32_t sy = static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * img.height / h);
            return img.pixels[(static_cast<std::size_t>(sy) * img.width + sx) * 4];
        };
        for (std::uint32_t y = 0; y < size.height; ++y) {
            for (std::uint32_t x = 0; x < size.width; ++x) {
                std::uint8_t* p = &texture.pixels[(static_cast<std::size_t>(y) * size.width + x) * 4];
                p[0] = 255;
                p[1] = has_r ? at(r, x, y, size.width, size.height) : 255;
                p[2] = has_m ? at(m, x, y, size.width, size.height) : 255;
                p[3] = 255;
            }
        }
        const auto index = static_cast<std::int32_t>(v.textures.size());
        v.textures.push_back(std::move(texture));
        files[key] = index;
        return index;
    };

    std::vector<std::uint8_t> transformed(v.vertices.size(), 0);
    for (std::size_t m = 0; m < base.materials.size(); ++m) {
        const asset::MaterialData& original = base.materials[m];
        std::shared_ptr<const assets::MaterialAsset> mat =
            m < overrides.size() && overrides[m].valid() ? material(overrides[m]) : nullptr;
        if (!mat) {
            asset::MaterialData copy = original;
            copy.albedo_texture = keep(original.albedo_texture);
            copy.metallic_roughness_texture = keep(original.metallic_roughness_texture);
            copy.normal_texture = keep(original.normal_texture);
            copy.occlusion_texture = keep(original.occlusion_texture);
            copy.emissive_texture = keep(original.emissive_texture);
            v.materials.push_back(copy);
            continue;
        }
        asset::MaterialData d = assets::toMaterialData(*mat, original.name);
        d.albedo_texture = file(mat->albedo);
        d.normal_texture = file(mat->normal);
        d.occlusion_texture = file(mat->occlusion);
        d.emissive_texture = file(mat->emissive_map);
        d.metallic_roughness_texture = packed(mat->metallic_map, mat->roughness_map);
        v.materials.push_back(d);

        // Tiling y desplazamiento: se hornean en las UV de sus submallas.
        const bool identity = mat->tiling.x == 1.0f && mat->tiling.y == 1.0f && mat->offset.x == 0.0f &&
                              mat->offset.y == 0.0f;
        if (identity) continue;
        for (const asset::SubMesh& submesh : v.submeshes) {
            if (submesh.material != m) continue;
            for (std::uint32_t i = 0; i < submesh.index_count; ++i) {
                const std::uint32_t vertex = v.indices[submesh.first_index + i];
                if (vertex >= v.vertices.size() || transformed[vertex]) continue;
                transformed[vertex] = 1;
                core::Vec2& uv = v.vertices[vertex].uv;
                uv = core::Vec2{uv.x * mat->tiling.x + mat->offset.x, uv.y * mat->tiling.y + mat->offset.y};
            }
        }
    }
    asset::finalizeModel(v, v.name);
    return v;
}

std::uint32_t RenderSync::resolveVariant(std::uint32_t base, const std::vector<assets::AssetRef>& overrides,
                                         scene::Scene& scene, bool& added) {
    const asset::ModelData& data = *scene.models()[base];
    std::vector<Uuid> slots(std::min(overrides.size(), data.materials.size()));
    bool any = false;
    std::string key = std::to_string(base);
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (overrides[i].valid() && material(overrides[i].uuid)) {
            slots[i] = overrides[i].uuid;
            any = true;
        }
        key += ':';
        key += slots[i].valid() ? slots[i].toString() : std::string{};
    }
    if (!any) return base;
    if (const auto it = variant_lookup_.find(key); it != variant_lookup_.end()) {
        return variants_[it->second].index;
    }

    asset::ModelData variant;
    try {
        variant = buildVariant(data, slots);
    } catch (const std::exception& e) {
        std::cerr << "[RenderSync] Material: " << e.what() << "\n";
        return base;
    }
    const std::uint32_t index = scene.addModel(std::move(variant));
    if (model_bounds_.size() <= index) model_bounds_.resize(index + 1);
    model_bounds_[index] = model_bounds_[base];
    variant_lookup_[key] = static_cast<std::uint32_t>(variants_.size());
    variants_.push_back(Variant{base, index, std::move(slots)});
    added = true;
    return index;
}

void RenderSync::applyMaterialChanges(scene::Scene& scene, gfx::VulkanRenderer& renderer, bool& added) {
    if (rebuild_materials_.empty() && live_materials_.empty()) return;
    for (const Variant& variant : variants_) {
        bool rebuild = false;
        for (const Uuid& uuid : variant.overrides) {
            rebuild = rebuild || (uuid.valid() && rebuild_materials_.contains(uuid));
        }
        if (rebuild) {
            try {
                scene.replaceModel(variant.index, buildVariant(*scene.models()[variant.base], variant.overrides));
                added = true;
            } catch (const std::exception& e) {
                std::cerr << "[RenderSync] Material: " << e.what() << "\n";
            }
            continue;
        }
        asset::ModelData* data = scene.modelData(variant.index);
        for (std::size_t slot = 0; data != nullptr && slot < variant.overrides.size(); ++slot) {
            const Uuid& uuid = variant.overrides[slot];
            if (!uuid.valid() || !live_materials_.contains(uuid)) continue;
            const std::shared_ptr<const assets::MaterialAsset> mat = material(uuid);
            if (!mat || slot >= data->materials.size()) continue;
            copyFactors(data->materials[slot], assets::toMaterialData(*mat, data->materials[slot].name));
            renderer.updateModelMaterial(variant.index, static_cast<std::uint32_t>(slot), data->materials[slot]);
        }
    }
    rebuild_materials_.clear();
    live_materials_.clear();
}

std::shared_ptr<const AnimatorController> RenderSync::animatorController(const Uuid& uuid) {
    if (const auto it = controllers_.find(uuid); it != controllers_.end()) {
        return it->second;
    }
    if (failed_controllers_.contains(uuid)) {
        return nullptr;
    }
    const auto info = assets_.database().find(uuid);
    auto controller = std::make_shared<AnimatorController>();
    std::string error;
    if (!info || info->type != assets::AssetType::AnimatorController ||
        !loadAnimatorController(info->path, *controller, &error)) {
        failed_controllers_.insert(uuid);
        std::cerr << "[RenderSync] No se pudo cargar el Animator " << uuid.toString() << " " << error << "\n";
        return nullptr;
    }
    controllers_[uuid] = controller;
    return controller;
}

void RenderSync::reloadAnimatorController(const Uuid& uuid) {
    controllers_.erase(uuid);
    failed_controllers_.erase(uuid);
    // Los clips sueltos del controlador pueden haber cambiado tambien.
    for (auto it = external_clips_.begin(); it != external_clips_.end();) {
        it = it->second < 0 ? external_clips_.erase(it) : std::next(it);
    }
}

int RenderSync::externalClip(std::uint32_t model, const Uuid& clip, scene::Scene& scene) {
    const ClipKey key{model, clip};
    if (const auto it = external_clips_.find(key); it != external_clips_.end()) {
        return it->second;
    }
    int index = -1;
    const auto info = assets_.database().find(clip);
    if (info && info->type == assets::AssetType::AnimationClip && model < scene.models().size()) {
        asset::ModelData& data = *scene.models()[model];
        asset::AnimationClip loaded;
        std::string error;
        if (loadAnimationClip(info->path, data, loaded, &error) && !loaded.channels.empty()) {
            // Se anade al modelo (el Animator reproduce por indice).
            data.animations.push_back(std::move(loaded));
            index = static_cast<int>(data.animations.size()) - 1;
        } else {
            std::cerr << "[RenderSync] Clip " << info->name << " no sirve para este modelo " << error << "\n";
        }
    }
    external_clips_[key] = index;
    return index;
}

const asset::ModelData* RenderSync::actorModelData(Entity entity, const scene::Scene& scene) const {
    const int actor = actorIndex(entity);
    if (actor < 0 || static_cast<std::size_t>(actor) >= actor_models_.size()) {
        return nullptr;
    }
    const std::uint32_t model = actor_models_[static_cast<std::size_t>(actor)];
    return model < scene.models().size() ? scene.models()[model].get() : nullptr;
}

void RenderSync::reset(scene::Scene& scene) {
    scene.actors().clear();
    scene.clear();
    for (const auto& [uuid, asset] : loaded_) {
        // Las piezas se movieron a la escena: el AssetManager ya no las tiene
        // completas. Se descargan para que la proxima vez se relean.
        assets_.unload(uuid);
    }
    models_.clear();
    loaded_.clear();
    failed_.clear();
    materials_.clear();
    failed_materials_.clear();
    variants_.clear();
    variant_lookup_.clear();
    rebuild_materials_.clear();
    live_materials_.clear();
    model_bounds_.clear();
    animations_.clear();
    controllers_.clear();
    failed_controllers_.clear();
    external_clips_.clear();
    decal_textures_.clear();
    rivers_.clear();
    actor_entities_.clear();
    previous_entities_.clear();
    destroyTerrains();
    actor_models_.clear();
    entity_actor_.clear();
    loaded_environment_ = {};
    failed_environment_ = {};
}

std::optional<std::uint32_t> RenderSync::resolveModel(const assets::AssetRef& ref, int part,
                                                       scene::Scene& scene, bool& added) {
    if (!ref.valid() || part < 0) {
        return std::nullopt;
    }
    const PartKey key{ref.uuid, part};
    if (const auto it = models_.find(key); it != models_.end()) {
        return it->second;
    }
    if (failed_.contains(ref.uuid)) {
        return std::nullopt;
    }

    std::shared_ptr<const assets::ModelAsset> asset;
    if (const auto it = loaded_.find(ref.uuid); it != loaded_.end()) {
        asset = it->second;
    } else {
        asset = assets_.loadModel(ref.uuid);
        if (!asset) {
            failed_.insert(ref.uuid);
            std::cerr << "[RenderSync] No se pudo cargar el modelo " << ref.uuid.toString() << "\n";
            return std::nullopt;
        }
        loaded_[ref.uuid] = asset;
    }
    if (static_cast<std::size_t>(part) >= asset->parts.size() || !asset->parts[part] ||
        asset->parts[part]->indices.empty()) {
        return std::nullopt;
    }

    // La pieza pasa a la escena UNA vez (sin copiar gigas de mallas): el
    // AssetManager se queda con un ModelData vacio de esa pieza.
    const std::uint32_t index = scene.addModel(std::move(*asset->parts[part]));
    const asset::ModelData& data = *scene.models()[index];
    anim::Animator bind(data);
    bind.play(-1);
    if (model_bounds_.size() <= index) {
        model_bounds_.resize(index + 1);
    }
    model_bounds_[index] = anim::skinnedBounds(data, bind.boneMatrices());
    models_[key] = index;
    added = true;
    return index;
}

// Agua: los cuerpos de este frame al renderizador (parametros de sus olas y
// color) y la cinta de cada rio, que solo se rehace si cambian sus puntos.
void RenderSync::syncWater(World& world, gfx::VulkanRenderer& renderer, float delta_seconds,
                           const core::Vec3& camera_position) {
    water::advanceWaterTime(delta_seconds);
    std::vector<gfx::WaterBodyDesc> bodies;
    std::unordered_map<entt::entity, RiverMesh> seen;
    int underwater = -1;
    world.forEachDepthFirst([&](Entity e) {
        const water::WaterBody* body = e.tryGet<water::WaterBody>();
        if (body == nullptr || !e.activeInHierarchy() || bodies.size() >= gfx::kMaxWaterBodies) return;
        const Mat4& m = e.worldMatrix();
        const Vec3 origin = e.worldPosition();
        constexpr float kRad = kDegToRad;
        gfx::WaterBodyDesc desc;
        gfx::GpuWaterBody& p = desc.params;
        p.origin = core::Vec4{origin.x, origin.y, origin.z, std::atan2(-m.m[0][2], m.m[0][0])};
        p.extent = core::Vec4{body->size.x * 0.5f, body->size.y * 0.5f, static_cast<float>(body->type), 0.0f};
        p.shallow = core::Vec4{body->shallow_color.x, body->shallow_color.y, body->shallow_color.z, body->clarity};
        p.deep = core::Vec4{body->deep_color.x, body->deep_color.y, body->deep_color.z, body->foam};
        p.waves = core::Vec4{body->wave_height, body->wavelength, body->wave_speed, body->steepness};
        p.wind = core::Vec4{body->wind_direction * kRad, body->wind_spread * kRad, body->flow_speed, body->detail};
        p.look = core::Vec4{body->roughness, body->refraction, body->caustics, body->shore_foam};
        p.extra = core::Vec4{body->shore_waves, body->scattering, 0.0f, 0.0f};

        // Camara cerca o bajo la superficie (oceano y lagos): el shader decide
        // por pixel con la misma ola.
        if (underwater < 0 && body->type != water::WaterType::River) {
            const water::WaterSample at_camera = water::sampleWater(*body, m, camera_position, water::waterTime());
            if (at_camera.inside && camera_position.y < at_camera.height + body->wave_height + 0.5f) {
                underwater = static_cast<int>(bodies.size());
            }
        }
        if (body->type == water::WaterType::River) {
            // Firma de lo que da forma a la cinta: puntos, anchos y la matriz.
            std::uint64_t hash = 1469598103934665603ull;
            const auto mix = [&](float f) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &f, sizeof(bits));
                hash = (hash ^ bits) * 1099511628211ull;
            };
            for (const water::RiverPoint& point : body->points) {
                mix(point.position.x);
                mix(point.position.y);
                mix(point.position.z);
                mix(point.width);
            }
            for (int c = 0; c < 4; ++c) {
                for (int r = 0; r < 4; ++r) mix(m.m[c][r]);
            }
            RiverMesh mesh;
            if (const auto it = rivers_.find(e.handle()); it != rivers_.end() && it->second.hash == hash) {
                mesh = std::move(it->second);
            } else {
                mesh.hash = hash;
                mesh.version = ++river_version_;
                const std::vector<water::RiverSample> line = water::riverCenterline(*body, m, 1.5f);
                constexpr std::uint32_t kAcross = 8;
                for (const water::RiverSample& sample : line) {
                    const Vec3 side{-sample.tangent.z, 0.0f, sample.tangent.x};
                    for (std::uint32_t j = 0; j < kAcross; ++j) {
                        const float a = static_cast<float>(j) / static_cast<float>(kAcross - 1);
                        const Vec3 pos = sample.position + side * ((a - 0.5f) * sample.width);
                        mesh.vertices.push_back(gfx::WaterVertex{{pos.x, pos.y, pos.z},
                                                                 {a, sample.distance},
                                                                 {sample.tangent.x, sample.tangent.z}});
                    }
                }
                for (std::uint32_t i = 0; i + 1 < line.size(); ++i) {
                    for (std::uint32_t j = 0; j + 1 < kAcross; ++j) {
                        const std::uint32_t a = i * kAcross + j;
                        const std::uint32_t b = a + kAcross;
                        mesh.indices.insert(mesh.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
                    }
                }
            }
            desc.vertices = mesh.vertices;
            desc.indices = mesh.indices;
            desc.mesh_version = mesh.version;
            seen[e.handle()] = std::move(mesh);
        }
        bodies.push_back(std::move(desc));
    });
    rivers_ = std::move(seen);
    renderer.setWaterBodies(bodies, water::waterTime(), underwater);
}

void RenderSync::sync(World& world, scene::Scene& scene, gfx::VulkanRenderer& renderer,
                      float delta_seconds, const Options& options) {
    // La animacion la lleva el componente Animator, no scene.update().
    scene.setAnimateActors(false);
    syncActors(world, scene, renderer, delta_seconds);
    syncLightsAndEnvironment(world, scene, renderer);
    syncTerrains(world, renderer);
    if (options.apply_main_camera) {
        syncCamera(world, scene);
    }
    // Despues de la camara: el agua mira si la camara esta sumergida.
    syncWater(world, renderer, delta_seconds, scene.camera().position());
}

RenderSync::~RenderSync() {
    destroyTerrains();
}

void RenderSync::destroyTerrains() {
    if (renderer_ != nullptr) {
        for (auto& [entity, gpu] : terrains_) renderer_->destroyTerrain(gpu.id);
    }
    terrains_.clear();
}

// Terrenos: se crean en el renderizador la primera vez, se suben las regiones
// que cambiaron (esculpir/pintar) y su descripcion (posicion, capas) cada frame.
void RenderSync::syncTerrains(World& world, gfx::VulkanRenderer& renderer) {
    renderer_ = &renderer;
    std::unordered_set<entt::entity> alive;
    if (terrain_store_ != nullptr) {
        for (const entt::entity handle : world.registry().view<terrain::Terrain>()) {
            const Entity e = world.wrap(handle);
            if (!e.activeInHierarchy()) continue;
            const terrain::Terrain& comp = e.get<terrain::Terrain>();
            const std::shared_ptr<terrain::TerrainData> data = terrain_store_->get(comp);
            if (!data) continue;
            alive.insert(handle);
            TerrainGpu& gpu = terrains_[handle];
            if (gpu.id == 0 || gpu.data != data || gpu.resolution != data->resolution() ||
                gpu.splat_resolution != data->splatResolution()) {
                if (gpu.id != 0) renderer.destroyTerrain(gpu.id);
                gpu.id = renderer.createTerrain(data->resolution(), data->splatResolution());
                gpu.data = data;
                gpu.resolution = data->resolution();
                gpu.splat_resolution = data->splatResolution();
                data->markAll();
            }
            if (gpu.id == 0) continue;
            if (const terrain::DirtyRegion r = data->takeDirtyHeights(); r.valid()) {
                renderer.updateTerrainHeights(gpu.id, data->heights().data(), static_cast<std::uint32_t>(r.x0),
                                              static_cast<std::uint32_t>(r.y0), static_cast<std::uint32_t>(r.x1 - r.x0 + 1),
                                              static_cast<std::uint32_t>(r.y1 - r.y0 + 1));
            }
            if (const terrain::DirtyRegion r = data->takeDirtySplat(); r.valid()) {
                renderer.updateTerrainSplat(gpu.id, data->splat0().data(), data->splat1().data(),
                                            static_cast<std::uint32_t>(r.x0), static_cast<std::uint32_t>(r.y0),
                                            static_cast<std::uint32_t>(r.x1 - r.x0 + 1),
                                            static_cast<std::uint32_t>(r.y1 - r.y0 + 1));
            }
            gfx::TerrainDesc desc;
            desc.origin = e.worldPosition();
            desc.size = std::max(comp.size, 1.0f);
            desc.max_height = std::max(comp.height, 0.01f);
            desc.cast_shadows = comp.cast_shadows;
            desc.lod_distance = comp.lod_distance;
            for (const terrain::TerrainLayer& layer : comp.layers) {
                if (desc.layers.size() >= gfx::kMaxTerrainLayers) break;
                gfx::TerrainLayerDesc l;
                if (!layer.albedo.empty()) l.albedo = assets_.database().root() / std::filesystem::path(layer.albedo);
                if (!layer.normal.empty()) l.normal = assets_.database().root() / std::filesystem::path(layer.normal);
                l.tiling = layer.tiling;
                l.roughness = layer.roughness;
                l.metallic = layer.metallic;
                l.normal_strength = layer.normal_strength;
                l.tint = layer.tint;
                desc.layers.push_back(std::move(l));
            }
            renderer.setTerrainDesc(gpu.id, desc);
        }
    }
    for (auto it = terrains_.begin(); it != terrains_.end();) {
        if (alive.count(it->first) == 0) {
            renderer.destroyTerrain(it->second.id);
            it = terrains_.erase(it);
        } else {
            ++it;
        }
    }
}

void RenderSync::syncActors(World& world, scene::Scene& scene, gfx::VulkanRenderer& renderer,
                            float delta_seconds) {
    bool added = false;
    // Materiales guardados desde el editor: factores en vivo o variantes rehechas.
    applyMaterialChanges(scene, renderer, added);
    // Los actores se rellenan en su sitio (sin reconstruir el vector ni copiar
    // el animador de lo que no se anima): con cientos de objetos, rehacerlo
    // todo cada frame eran miles de reservas de memoria.
    std::vector<scene::Actor>& actors = scene.actors();
    previous_entities_.swap(actor_entities_);
    actor_entities_.clear();
    actor_models_.clear();
    std::size_t count = 0;
    ++frame_;

    // En profundidad: el orden de los actores sigue al de la Jerarquia (estable
    // entre frames, lo que agradecen las cascadas de sombra).
    world.forEachDepthFirst([&](Entity e) {
        const MeshRenderer* renderer_component = e.tryGet<MeshRenderer>();
        if (renderer_component == nullptr || !renderer_component->visible ||
            !e.activeInHierarchy()) {
            return;
        }
        std::optional<std::uint32_t> model =
            resolveModel(renderer_component->model, renderer_component->part, scene, added);
        if (!model) {
            return;
        }
        if (!renderer_component->materials.empty()) {
            model = resolveVariant(*model, renderer_component->materials, scene, added);
        }
        const asset::ModelData& data = *scene.models()[*model];

        // Animador persistente por entidad (su tiempo sobrevive entre frames).
        AnimationState& state = animations_[e.handle()];
        if (state.clip == -2 || state.model != *model) {
            state = AnimationState{};
            state.model = *model;
            state.animator = anim::Animator(data);
            state.clip = -1;
            state.animator.play(-1);
        }
        state.seen = frame_;

        bool animating = false;
        if (Animator* animator = e.tryGet<Animator>(); animator != nullptr && !data.animations.empty()) {
            animating = true;
            int clip = animator->clip;
            bool loop = animator->loop;
            float speed = animator->speed;
            bool restart = false;

            // Con controlador: la maquina de estados elige el clip.
            const std::shared_ptr<const AnimatorController> controller =
                animator->controller.valid() ? animatorController(animator->controller.uuid) : nullptr;
            if (controller && !controller->states.empty()) {
                AnimatorRuntime& runtime = animator->runtime;
                const auto clipOf = [&](int index) {
                    if (index < 0 || index >= static_cast<int>(controller->states.size())) return -1;
                    const AnimatorState& st = controller->states[index];
                    if (st.clip.valid()) return externalClip(*model, st.clip.uuid, scene);
                    for (std::size_t i = 0; i < data.animations.size(); ++i) {
                        if (data.animations[i].name == st.clip_name) return static_cast<int>(i);
                    }
                    return -1;
                };
                const int current = clipOf(runtime.state);
                const float duration = current >= 0 ? data.animations[current].duration : 0.0f;
                stepAnimatorController(*controller, runtime, duration);
                const AnimatorState& st = controller->states[runtime.state];
                clip = clipOf(runtime.state);
                loop = st.loop;
                speed = animator->speed * st.speed;
                if (runtime.state != state.controller_state) {
                    state.controller_state = runtime.state;
                    animator->time = 0.0f;
                    restart = true;
                }
                if (animator->playing) runtime.state_time += delta_seconds * std::abs(speed);
            } else {
                state.controller_state = -1;
                if (!animator->clip_name.empty()) {
                    for (std::size_t i = 0; i < data.animations.size(); ++i) {
                        if (data.animations[i].name == animator->clip_name) {
                            clip = static_cast<int>(i);
                            break;
                        }
                    }
                }
            }
            clip = std::clamp(clip, -1, static_cast<int>(data.animations.size()) - 1);
            if (restart || clip != state.clip || loop != state.loop) {
                state.animator.play(clip, loop);
                state.animator.setTime(animator->time);
                state.clip = clip;
                state.loop = loop;
            }
            state.animator.setSpeed(speed);
            if (animator->playing) {
                state.animator.update(delta_seconds);
                animator->time = state.animator.time();
            } else if (std::abs(state.animator.time() - animator->time) > 1e-5f) {
                // Pausado: el tiempo del Inspector manda (arrastrarlo = scrub).
                state.animator.setTime(animator->time);
                state.animator.evaluate();
            }
        }

        const Mat4& world_matrix = e.worldMatrix();
        if (count >= actors.size()) actors.emplace_back();
        scene::Actor& actor = actors[count];
        // El mismo objeto en la misma posicion de la lista y sin animar: su
        // animador (pose de reposo) ya esta; no se copia.
        const bool same_slot = count < previous_entities_.size() && previous_entities_[count] == e.handle() &&
                               actor.model == *model;
        actor.model = *model;
        actor.transform = world_matrix;
        if (!same_slot || animating || state.animated) actor.animator = state.animator;
        state.animated = animating;
        const anim::Aabb& box = model_bounds_[*model];
        const Vec3 center = (box.min + box.max) * 0.5f;
        actor.bounds_center = transformPoint(world_matrix, center);
        actor.bounds_radius = core::length(box.max - box.min) * 0.5f * maxAxisScale(world_matrix);
        actor.cast_shadows = renderer_component->cast_shadows != ShadowCasting::Off;
        actor.shadows_only = renderer_component->cast_shadows == ShadowCasting::ShadowsOnly;

        actor_entities_.push_back(e.handle());
        actor_models_.push_back(*model);
        ++count;
    });
    actors.resize(count);

    // El indice entidad -> actor solo se rehace si la lista cambio.
    if (actor_entities_ != previous_entities_) {
        entity_actor_.clear();
        for (std::size_t i = 0; i < actor_entities_.size(); ++i) {
            entity_actor_[actor_entities_[i]] = static_cast<std::uint32_t>(i);
        }
    }

    // Olvida los animadores de lo que ya no se dibuja.
    for (auto it = animations_.begin(); it != animations_.end();) {
        it = it->second.seen == frame_ ? std::next(it) : animations_.erase(it);
    }

    if (added) {
        // Piezas nuevas: el renderizador sube la escena entera (mallas,
        // texturas y la estructura de los rayos).
        renderer.uploadModels(scene);
    }
}

void RenderSync::syncLightsAndEnvironment(World& world, scene::Scene& scene,
                                          gfx::VulkanRenderer& renderer) {
    scene::LightSet& lights = scene.lights();
    lights.points.clear();
    lights.spots.clear();

    Entity directional;
    Entity sky_entity;
    Entity weather_entity;
    const PostProcessing* post = nullptr;

    world.forEachDepthFirst([&](Entity e) {
        if (!e.activeInHierarchy()) {
            return;
        }
        if (const Light* light = e.tryGet<Light>()) {
            switch (light->type) {
                case LightType::Directional:
                    if (!directional.valid()) {
                        directional = e;
                    }
                    break;
                case LightType::Point:
                    if (lights.points.size() < scene::kMaxPointLights) {
                        scene::PointLight p{};
                        p.position = e.worldPosition();
                        p.color = light->color;
                        p.intensity = light->intensity;
                        p.range = light->range;
                        p.cast_shadows = light->cast_shadows;
                        lights.points.push_back(p);
                    }
                    break;
                case LightType::Spot:
                    if (lights.spots.size() < scene::kMaxSpotLights) {
                        scene::SpotLight s{};
                        s.position = e.worldPosition();
                        s.direction = e.forward();
                        s.color = light->color;
                        s.intensity = light->intensity;
                        s.range = light->range;
                        const float inner = std::min(light->inner_angle, light->outer_angle - 0.5f);
                        s.inner_angle = std::max(inner, 0.5f) * kDegToRad;
                        s.outer_angle = light->outer_angle * kDegToRad;
                        s.enabled = true;
                        s.cast_shadows = light->cast_shadows;
                        lights.spots.push_back(s);
                    }
                    break;
            }
        }
        if (!sky_entity.valid() && e.has<Sky>()) {
            sky_entity = e;
        }
        if (!weather_entity.valid() && e.has<Weather>()) {
            weather_entity = e;
        }
        if (const PostProcessing* p = e.tryGet<PostProcessing>()) {
            if (post == nullptr || p->priority > post->priority) {
                post = p;
            }
        }
    });

    // --- Cielo HDR ---
    Sky* sky = sky_entity.valid() ? sky_entity.tryGet<Sky>() : nullptr;
    bool hdr_active = false;
    if (sky != nullptr && sky->use_hdr && sky->environment.valid()) {
        const Uuid& wanted = sky->environment.uuid;
        if (wanted != loaded_environment_ && wanted != failed_environment_) {
            const std::filesystem::path file = assets_.environmentFile(wanted);
            if (!file.empty() && renderer.loadEnvironment(file)) {
                loaded_environment_ = wanted;
            } else {
                failed_environment_ = wanted;
                std::cerr << "[RenderSync] No se pudo cargar el cielo " << wanted.toString() << "\n";
            }
        }
        hdr_active = loaded_environment_ == wanted;
    }
    renderer.setEnvironmentEnabled(hdr_active);
    renderer.setCloudsEnabled(sky == nullptr || sky->clouds);

    // --- Sol ---
    // Luz direccional > sol de la foto HDR > hora del cielo.
    if (directional.valid()) {
        const Light& light = directional.get<Light>();
        scene.setFixedSun(-directional.forward());
        // scene.update() ya calculo el sol fisico de este frame: el componente
        // lo tiñe y lo escala.
        lights.sun.color = lights.sun.color * light.color;
        lights.sun.intensity *= light.intensity;
        renderer.setSunShadowsEnabled(light.cast_shadows);
    } else if (hdr_active) {
        renderer.setSunShadowsEnabled(true);
        scene.setFixedSun(renderer.environmentSunDirection());
    } else {
        renderer.setSunShadowsEnabled(true);
        scene.setFixedSun(std::nullopt);
        if (sky != nullptr) {
            scene.setDayCycleEnabled(sky->day_cycle);
            if (sky->day_cycle) {
                sky->time_of_day = scene.timeOfDayHours();  // el Inspector ve la hora
            } else {
                scene.setTimeOfDayHours(sky->time_of_day);
            }
        }
    }

    // --- Clima ---
    if (const Weather* weather = weather_entity.valid() ? weather_entity.tryGet<Weather>() : nullptr) {
        renderer.setRainEnabled(weather->rain);
        renderer.setWeather(weather->wetness, weather->puddles);
        renderer.setWater(weather->flood_center,
                          weather->flood ? weather->flood_radii : core::Vec2{});
        renderer.setWaterEnabled(weather->flood);
    } else {
        renderer.setRainEnabled(false);
        renderer.setWeather(0.0f, 0.0f);
        renderer.setWater(core::Vec2{}, core::Vec2{});
    }

    // --- Decals ---
    const Weather* global_weather = weather_entity.valid() ? weather_entity.tryGet<Weather>() : nullptr;
    const float rain_puddles = global_weather != nullptr && global_weather->rain ? global_weather->puddles : 0.0f;
    std::vector<gfx::VulkanRenderer::Decal> decals;
    for (const entt::entity handle : world.registry().view<Decal>()) {
        const Entity e = world.wrap(handle);
        if (!e.activeInHierarchy()) continue;
        const Decal& d = *e.tryGet<Decal>();
        gfx::VulkanRenderer::Decal out;
        out.world_to_decal = core::inverse(e.worldMatrix());
        out.axis = e.up();
        out.color = d.color;
        out.opacity = d.opacity;
        out.type = static_cast<int>(d.type);
        out.edge_softness = d.edge_softness * 0.5f + 0.001f;
        out.angle_fade = std::cos(std::clamp(d.max_angle, 1.0f, 90.0f) * kDegToRad);
        out.roughness = d.roughness;
        out.roughness_amount = d.roughness_amount;
        out.metallic = d.metallic;
        out.amount = d.amount * (d.type != DecalType::Stamp && d.follow_rain ? std::min(rain_puddles * 2.0f, 1.0f) : 1.0f);
        if (!d.texture.empty()) {
            const auto it = decal_textures_.find(d.texture);
            if (it != decal_textures_.end()) {
                out.texture = it->second;
            } else {
                const std::filesystem::path file = assets_.database().root() /
                    std::filesystem::path(std::u8string(d.texture.begin(), d.texture.end()));
                out.texture = renderer.loadDecalTexture(file);
                decal_textures_[d.texture] = out.texture;
            }
        }
        if (out.amount <= 0.0f && d.type != DecalType::Stamp) continue;
        decals.push_back(out);
    }
    renderer.setDecals(std::move(decals));

    // --- Post-proceso ---
    renderer.setPostProcess(post != nullptr ? post->settings : gfx::PostProcessSettings{});
}

void RenderSync::syncCamera(World& world, scene::Scene& scene) {
    Entity main;
    world.forEachDepthFirst([&](Entity e) {
        const Camera* camera = e.tryGet<Camera>();
        if (camera != nullptr && e.activeInHierarchy() && (!main.valid() || camera->is_main)) {
            if (!main.valid() || !main.get<Camera>().is_main) {
                main = e;
            }
        }
    });
    if (!main.valid()) {
        return;
    }
    const Camera& camera = main.get<Camera>();
    scene::Camera& view = scene.camera();
    view.setPosition(main.worldPosition());
    view.setOrientation(main.forward(), main.up());
    view.setFovY(camera.fov * kDegToRad);
    // (Los planos cercano y lejano de scene::Camera no se pueden fijar desde
    // fuera todavia.)
}

int RenderSync::actorIndex(Entity entity) const {
    const auto it = entity_actor_.find(entity.handle());
    return it == entity_actor_.end() ? -1 : static_cast<int>(it->second);
}

Entity RenderSync::entityForActor(const World& world, std::uint32_t actor) const {
    if (actor >= actor_entities_.size() || !world.valid(actor_entities_[actor])) {
        return {};
    }
    return world.wrap(actor_entities_[actor]);
}

std::vector<std::uint32_t> RenderSync::actorIndicesInSubtree(Entity entity) const {
    std::vector<std::uint32_t> result;
    if (!entity.valid()) {
        return result;
    }
    std::vector<entt::entity> stack{entity.handle()};
    while (!stack.empty()) {
        const entt::entity e = stack.back();
        stack.pop_back();
        if (const auto it = entity_actor_.find(e); it != entity_actor_.end()) {
            result.push_back(it->second);
        }
        const auto& children = entity.world()->wrap(e).children();
        stack.insert(stack.end(), children.begin(), children.end());
    }
    return result;
}

std::shared_ptr<const assets::ModelAsset> RenderSync::modelAsset(const Uuid& uuid) const {
    const auto it = loaded_.find(uuid);
    return it == loaded_.end() ? nullptr : it->second;
}

bool RenderSync::actorLocalBounds(std::uint32_t actor, Vec3& min, Vec3& max) const {
    if (actor >= actor_models_.size() || actor_models_[actor] >= model_bounds_.size()) {
        return false;
    }
    min = model_bounds_[actor_models_[actor]].min;
    max = model_bounds_[actor_models_[actor]].max;
    return true;
}

}  // namespace cramion::ecs
