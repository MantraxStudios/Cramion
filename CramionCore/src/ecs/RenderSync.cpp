#include "CramionCore/ecs/RenderSync.h"
#include "CramionCore/cvar/CVar.h"
#include "CramionCore/modeling/EditableMesh.h"

#include "CramionCore/physics/Cloth.h"
#include "CramionCore/physics/SoftBody.h"

#include "CramionCore/foliage/Foliage.h"
#include "CramionCore/fire/Fire.h"
#include "CramionCore/asset/RenderTextureAsset.h"

#include "CramionCore/ecs/Rigging.h"

#include "CramionCore/anim/IK.h"
#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/navigation/Navigation.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/terrain/TerrainTools.h"

#include <CramionFX/asset/ImageFile.h>
#include <CramionFX/vk/ShaderCompiler.h>

#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>

namespace cramion::ecs {
namespace {

// Streaming de modelos: leerlos en segundo plano (sin congelar el frame al
// abrir una escena o instanciar) y subirlos a la GPU poco a poco.
cvar::CVar<bool> g_async_models("render.streaming.AsyncModels", true,
                                "Leer los modelos en segundo plano: lo nuevo aparece al terminar de leerse, sin "
                                "congelar el frame (apagado: se esperan, como antes)",
                                cvar::Saved);
cvar::CVar<float> g_upload_ms("render.streaming.UploadMs", 4.0f,
                              "Milisegundos por frame para subir modelos nuevos a la GPU (al menos uno por frame)",
                              cvar::Saved, 0.5f, 100.0f);
// Residencia en la GPU: lo que no se ve (mas pequeno que unos pixeles un rato)
// libera su memoria de video y vuelve solo al acercarse.
cvar::CVar<bool> g_gpu_residency("render.streaming.GpuResidency", true,
                                 "Sacar de la memoria de video los modelos que no se ven (mas pequenos que MinPixels "
                                 "durante IdleSeconds); vuelven solos al acercarse",
                                 cvar::Saved);
cvar::CVar<float> g_min_pixels("render.streaming.MinPixels", 0.5f,
                               "Tamano en pantalla (pixeles) por debajo del cual un modelo no se ve", cvar::Saved, 0.05f,
                               64.0f);
cvar::CVar<float> g_idle_seconds("render.streaming.IdleSeconds", 5.0f,
                                 "Segundos sin verse antes de salir de la memoria de video", cvar::Saved, 0.5f, 600.0f);

}  // namespace


using core::Mat4;
using core::Quat;
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
    to.height_scale = from.height_scale;
    to.tessellation = from.tessellation;
    to.tessellation_density = from.tessellation_density;
    to.parallax_shadows = from.parallax_shadows;
    to.shading_model = from.shading_model;
    to.specular_tint = from.specular_tint;
    to.clearcoat = from.clearcoat;
    to.clearcoat_roughness = from.clearcoat_roughness;
    to.sheen = from.sheen;
    to.sheen_tint = from.sheen_tint;
    to.subsurface = from.subsurface;
    to.translucency = from.translucency;
    to.subsurface_thickness = from.subsurface_thickness;
    to.anisotropy = from.anisotropy;
    to.anisotropy_rotation = from.anisotropy_rotation;
    to.ior = from.ior;
    to.transmission_thickness = from.transmission_thickness;
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

void RenderSync::compileSurfaceShader(const std::string& path, SurfaceShaderEntry& entry) {
    const std::filesystem::path file = assets_.database().root() / fromUtf8(path);
    std::error_code ec;
    entry.time = std::filesystem::last_write_time(file, ec);
    std::string error;
    entry.parsed = assets::loadSurfaceShader(file, entry.source, &error);
    std::vector<std::uint32_t> vertex, fragment;
    // Sin compilador (el juego en Android): el SPIR-V que dejo el editor al exportar.
    const bool compiled = gfx::shaders::compilerAvailable()
                              ? assets::compileSurfaceShader(entry.source, assets::surfaceTemplateDirectory(), vertex, fragment, &error)
                              : assets::loadPrecompiledSurfaceShader(file, vertex, fragment) ||
                                    (error = "sin compilador de shaders ni SPIR-V precompilado para " + path, false);
    if (entry.parsed && renderer_ != nullptr && compiled) {
        const bool ok = entry.id >= 0 ? renderer_->updateSurfaceShader(entry.id, vertex, fragment, &error)
                                      : (entry.id = renderer_->createSurfaceShader(vertex, fragment, &error)) >= 0;
        if (ok) {
            entry.error.clear();
            std::cout << "[Shader] " << path << " compilado\n";
            return;
        }
    }
    if (renderer_ == nullptr && error.empty()) error = "sin renderizador";
    entry.error = error;
    std::cerr << "[Shader] " << error << "\n";
}

std::int32_t RenderSync::surfaceShader(const std::string& path) {
    if (path.empty()) return -1;
    auto it = surface_shaders_.find(path);
    if (it == surface_shaders_.end()) {
        it = surface_shaders_.emplace(path, SurfaceShaderEntry{}).first;
        compileSurfaceShader(path, it->second);
    }
    return it->second.id;
}

const assets::SurfaceShaderSource* RenderSync::surfaceShaderSource(const std::string& path) {
    if (path.empty()) return nullptr;
    surfaceShader(path);
    const auto it = surface_shaders_.find(path);
    return it != surface_shaders_.end() && it->second.parsed ? &it->second.source : nullptr;
}

std::string RenderSync::surfaceShaderError(const std::string& path) const {
    const auto it = surface_shaders_.find(path);
    return it != surface_shaders_.end() ? it->second.error : std::string{};
}

int RenderSync::reloadSurfaceShaders() {
    int changed = 0;
    for (auto& [path, entry] : surface_shaders_) {
        std::error_code ec;
        const auto time = std::filesystem::last_write_time(assets_.database().root() / fromUtf8(path), ec);
        if (ec || time == entry.time) continue;
        compileSurfaceShader(path, entry);
        ++changed;
        // Las propiedades pueden haber cambiado de hueco: se rehacen sus materiales.
        for (const auto& [uuid, material] : materials_) {
            if (material.data && material.data->shader == path) rebuild_materials_.insert(uuid);
        }
    }
    return changed;
}

void RenderSync::applySurface(asset::MaterialData& data, const assets::MaterialAsset& material,
                              const std::function<std::int32_t(const std::string&)>* texture) {
    data.surface_shader = material.shader.empty() ? -1 : surfaceShader(material.shader);
    const assets::SurfaceShaderSource* source = data.surface_shader >= 0 ? surfaceShaderSource(material.shader) : nullptr;
    if (source == nullptr) {
        data.surface_shader = -1;
        return;
    }
    for (const assets::ShaderProperty& p : source->properties) {
        if (p.type == assets::ShaderPropertyType::Texture) {
            if (texture == nullptr || p.slot < 0 || p.slot >= assets::kMaxShaderTextures) continue;
            const auto it = material.shader_textures.find(p.name);
            data.surface_textures[static_cast<std::size_t>(p.slot)] =
                it != material.shader_textures.end() && !it->second.empty() ? (*texture)(it->second) : -1;
            continue;
        }
        if (p.slot < 0 || p.slot >= assets::kMaxShaderValues) continue;
        const auto it = material.shader_values.find(p.name);
        data.surface_params[static_cast<std::size_t>(p.slot)] = it != material.shader_values.end() ? it->second : p.value;
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
    // Mapas grises sueltos juntados en una textura RGBA8 (cada canal de un
    // archivo, remuestreado al tamano del mayor). Canal sin archivo = `fill`.
    struct Channel {
        const std::string* path = nullptr;
        std::uint8_t fill = 255;
        bool invert = false;     // brillo -> rugosidad
        float strength = 1.0f;   // mezcla con `fill` (fuerza de la cavidad)
    };
    const auto pack = [&](const std::string& key, const std::array<Channel, 4>& channels) -> std::int32_t {
        if (const auto it = files.find(key); it != files.end()) return it->second;
        std::array<asset::ImageRgba8, 4> images;
        std::array<bool, 4> loaded{};
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        for (std::size_t c = 0; c < 4; ++c) {
            const std::string* path = channels[c].path;
            if (path == nullptr || path->empty()) continue;
            // El mismo archivo en dos canales se lee una vez.
            for (std::size_t p = 0; p < c && !loaded[c]; ++p) {
                if (loaded[p] && *channels[p].path == *path) {
                    images[c] = images[p];
                    loaded[c] = true;
                }
            }
            if (!loaded[c]) {
                loaded[c] = asset::loadImageRgba8(root / fromUtf8(*path), images[c]);
                if (!loaded[c]) std::cerr << "[RenderSync] Falta la textura " << *path << "\n";
            }
            if (loaded[c] && images[c].width * images[c].height > width * height) {
                width = images[c].width;
                height = images[c].height;
            }
        }
        if (width == 0) {
            files[key] = -1;
            return -1;
        }
        asset::TextureData texture;
        texture.name = key;
        texture.width = width;
        texture.height = height;
        texture.pixels.resize(static_cast<std::size_t>(width) * height * 4);
        for (std::size_t c = 0; c < 4; ++c) {
            const Channel& channel = channels[c];
            const asset::ImageRgba8& img = images[c];
            const float strength = std::clamp(channel.strength, 0.0f, 1.0f);
            for (std::uint32_t y = 0; y < height; ++y) {
                const std::uint32_t sy = loaded[c] ? static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * img.height / height) : 0;
                for (std::uint32_t x = 0; x < width; ++x) {
                    std::uint8_t value = channel.fill;
                    if (loaded[c]) {
                        const std::uint32_t sx = static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * img.width / width);
                        std::uint8_t g = img.pixels[(static_cast<std::size_t>(sy) * img.width + sx) * 4];
                        if (channel.invert) g = static_cast<std::uint8_t>(255 - g);
                        value = static_cast<std::uint8_t>(channel.fill + (static_cast<float>(g) - channel.fill) * strength + 0.5f);
                    }
                    texture.pixels[(static_cast<std::size_t>(y) * width + x) * 4 + c] = value;
                }
            }
        }
        const auto index = static_cast<std::int32_t>(v.textures.size());
        v.textures.push_back(std::move(texture));
        files[key] = index;
        return index;
    };
    // R = reflectancia (specular), G = rugosidad (o 1 - brillo), B = metal
    // (como glTF) y A = cavidad.
    const auto packedSurface = [&](const assets::MaterialAsset& mat) -> std::int32_t {
        const std::string& rough = mat.roughness_map.empty() ? mat.gloss_map : mat.roughness_map;
        if (mat.metallic_map.empty() && rough.empty() && mat.specular_map.empty() && mat.cavity_map.empty()) return -1;
        const std::string key = "mr:" + mat.metallic_map + "|" + rough + "|" + mat.specular_map + "|" + mat.cavity_map +
                                "|" + std::to_string(mat.cavity_strength);
        return pack(key, {Channel{&mat.specular_map, 128},
                          Channel{&rough, 255, mat.roughness_map.empty()},
                          Channel{&mat.metallic_map, 255},
                          Channel{&mat.cavity_map, 255, false, mat.cavity_strength}});
    };
    // R = oclusion, G = altura (parallax). Sin altura, la oclusion tal cual.
    const auto packedOcclusion = [&](const assets::MaterialAsset& mat) -> std::int32_t {
        if (mat.height_map.empty()) return file(mat.occlusion);
        return pack("ao:" + mat.occlusion + "|" + mat.height_map,
                    {Channel{&mat.occlusion, 255}, Channel{&mat.height_map, 255}, Channel{}, Channel{}});
    };
    // Bump (gris) como normal map si no hay uno de verdad: se convierte al
    // decodificarlo (TextureData::height_map).
    const auto bumpAsNormal = [&](const std::string& relative) -> std::int32_t {
        if (relative.empty()) return -1;
        const std::string key = relative + "#bump";
        if (const auto it = files.find(key); it != files.end()) return it->second;
        asset::TextureData texture;
        texture.name = relative;
        texture.height_map = true;
        if (!readBytes(root / fromUtf8(relative), texture.encoded)) {
            files[key] = -1;
            return -1;
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
        // Una Render Texture (.crrt) en el color o en la emision: la lee el
        // renderizador en vez de una imagen.
        const auto render_texture = [&](const std::string& relative) -> std::int32_t {
            if (relative.size() < 5) return -1;
            std::string ext = relative.substr(relative.size() - 5);
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext != assets::kRenderTextureExtension) return -1;
            const std::int32_t id = renderTextureForPath(root / fromUtf8(relative));
            return id >= 0 ? id : -2;  // -2: es una .crrt que no se pudo crear (sin imagen)
        };
        const std::int32_t albedo_rt = render_texture(mat->albedo);
        const std::int32_t emissive_rt = render_texture(mat->emissive_map);
        d.albedo_texture = albedo_rt == -1 ? file(mat->albedo) : -1;
        d.albedo_render_texture = std::max(albedo_rt, -1);
        // Sin normal map, el bump; y si el relieve es teselado y tampoco hay
        // bump, la propia altura (la malla sube, pero la luz necesita la
        // inclinacion de cada piedra).
        const std::string& bump = !mat->bump_map.empty() || mat->relief != assets::ReliefMode::Tessellation
                                      ? mat->bump_map
                                      : mat->height_map;
        d.normal_texture = !mat->normal.empty() ? file(mat->normal) : bumpAsNormal(bump);
        if (mat->normal.empty() && !bump.empty()) d.normal_map_directx = false;
        d.occlusion_texture = packedOcclusion(*mat);
        d.emissive_texture = emissive_rt == -1 ? file(mat->emissive_map) : -1;
        d.emissive_render_texture = std::max(emissive_rt, -1);
        d.metallic_roughness_texture = packedSurface(*mat);
        if (d.occlusion_texture < 0) d.height_scale = 0.0f;
        const std::function<std::int32_t(const std::string&)> texture = [&](const std::string& path) { return file(path); };
        applySurface(d, *mat, &texture);
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

std::optional<std::uint32_t> RenderSync::resolveRuntimeMesh(const std::shared_ptr<Mesh>& mesh, scene::Scene& scene,
                                                           gfx::VulkanRenderer& renderer) {
    auto it = runtime_meshes_.find(mesh.get());
    if (it != runtime_meshes_.end() && it->second.mesh.lock() != mesh) {
        // Otra malla en la misma direccion (la anterior se destruyo): su hueco se reutiliza.
        free_runtime_models_.push_back(it->second.index);
        forgetVariantsOf(it->second.index);
        runtime_meshes_.erase(it);
        it = runtime_meshes_.end();
    }
    if (it != runtime_meshes_.end() && it->second.version == mesh->version()) {
        RuntimeSlot& slot = it->second;
        if (slot.material_version == mesh->materialVersion()) return slot.index;
        if (slot.material_layout == mesh->materialLayout()) {
            // Solo factores (color, brillo...): al momento, sin volver a subir la malla.
            slot.material_version = mesh->materialVersion();
            if (asset::ModelData* data = scene.modelData(slot.index)) {
                for (std::size_t i = 0; i < data->materials.size(); ++i) {
                    Mesh::applyFactors(i < mesh->materials.size() ? mesh->materials[i] : MeshMaterial{}, &data->materials[i]);
                    renderer.updateModelMaterial(slot.index, static_cast<std::uint32_t>(i), data->materials[i]);
                }
            }
            return slot.index;
        }
        // Otras texturas o repeticion: se rehace entera (abajo).
    }
    const std::string problem = mesh->validate();
    if (!problem.empty()) {
        if (warned_meshes_.insert(mesh.get()).second) {
            std::cerr << "[RenderSync] Malla \"" << mesh->name << "\": " << problem << "\n";
        }
        // Se sigue viendo la ultima version buena (si la hubo).
        return it != runtime_meshes_.end() ? std::optional<std::uint32_t>(it->second.index) : std::nullopt;
    }
    warned_meshes_.erase(mesh.get());
    asset::ModelData data = mesh->toModelData(assets_.database().root());
    try {
        asset::finalizeModel(data, mesh->name);  // lee y decodifica sus texturas
    } catch (const std::exception& e) {
        std::cerr << "[RenderSync] Malla \"" << mesh->name << "\": " << e.what() << "\n";
        return it != runtime_meshes_.end() ? std::optional<std::uint32_t>(it->second.index) : std::nullopt;
    }
    std::uint32_t index = 0;
    if (it != runtime_meshes_.end()) {
        index = it->second.index;
        scene.replaceModel(index, std::move(data));
        it->second.version = mesh->version();
        it->second.material_version = mesh->materialVersion();
        it->second.material_layout = mesh->materialLayout();
    } else {
        if (!free_runtime_models_.empty()) {
            index = free_runtime_models_.back();
            free_runtime_models_.pop_back();
            scene.replaceModel(index, std::move(data));
        } else {
            index = scene.addModel(std::move(data));
        }
        runtime_meshes_[mesh.get()] = RuntimeSlot{mesh, mesh->version(), mesh->materialVersion(), mesh->materialLayout(), index};
    }
    const asset::ModelData& stored = *scene.models()[index];
    anim::Animator bind(stored);
    bind.play(-1);
    if (model_bounds_.size() <= index) model_bounds_.resize(index + 1);
    model_bounds_[index] = anim::skinnedBounds(stored, bind.boneMatrices());
    // Solo este modelo a la GPU (si no se va a subir la escena entera ya).
    if (index < renderer.uploadedModelCount()) renderer.uploadModel(scene, index);
    // Sus variantes de material (.crmat del MeshRenderer) con la malla nueva.
    for (const Variant& variant : variants_) {
        if (variant.base != index) continue;
        try {
            scene.replaceModel(variant.index, buildVariant(stored, variant.overrides));
            if (model_bounds_.size() <= variant.index) model_bounds_.resize(variant.index + 1);
            model_bounds_[variant.index] = model_bounds_[index];
            if (variant.index < renderer.uploadedModelCount()) renderer.uploadModel(scene, variant.index);
        } catch (const std::exception& e) {
            std::cerr << "[RenderSync] Material: " << e.what() << "\n";
        }
    }
    return index;
}

std::optional<std::uint32_t> RenderSync::resolveClothModel(Entity e, const physics::Cloth& cloth, scene::Scene& scene,
                                                          gfx::VulkanRenderer& renderer) {
    const std::string layout = cloth.layoutKey();
    auto it = cloth_models_.find(e.handle());
    if (it != cloth_models_.end() && it->second.layout == layout) {
        it->second.frame = frame_;
        return it->second.index;
    }
    asset::ModelData data = physics::clothModel(cloth);
    std::uint32_t index = 0;
    if (it != cloth_models_.end()) {
        index = it->second.index;
        forgetVariantsOf(index);
        scene.replaceModel(index, std::move(data));
    } else if (!free_runtime_models_.empty()) {
        index = free_runtime_models_.back();
        free_runtime_models_.pop_back();
        scene.replaceModel(index, std::move(data));
    } else {
        index = scene.addModel(std::move(data));
    }
    cloth_models_[e.handle()] = ClothSlot{layout, index, frame_, nullptr};
    const asset::ModelData& stored = *scene.models()[index];
    anim::Animator bind(stored);
    bind.play(-1);
    if (model_bounds_.size() <= index) model_bounds_.resize(index + 1);
    model_bounds_[index] = anim::skinnedBounds(stored, bind.boneMatrices());
    if (index < renderer.uploadedModelCount()) renderer.uploadModel(scene, index);
    return index;
}

std::optional<std::uint32_t> RenderSync::resolveSoftBodyModel(Entity e, const physics::SoftBody& body, scene::Scene& scene,
                                                             gfx::VulkanRenderer& renderer) {
    const std::string layout = "soft/" + body.layoutKey();
    auto it = cloth_models_.find(e.handle());
    if (it != cloth_models_.end() && it->second.layout == layout) {
        it->second.frame = frame_;
        return it->second.index;
    }
    asset::ModelData data = physics::softBodyModel(body);
    std::uint32_t index = 0;
    if (it != cloth_models_.end()) {
        index = it->second.index;
        forgetVariantsOf(index);
        scene.replaceModel(index, std::move(data));
    } else if (!free_runtime_models_.empty()) {
        index = free_runtime_models_.back();
        free_runtime_models_.pop_back();
        scene.replaceModel(index, std::move(data));
    } else {
        index = scene.addModel(std::move(data));
    }
    cloth_models_[e.handle()] =
        ClothSlot{layout, index, frame_, std::make_shared<const physics::SoftBodyMesh>(physics::softBodyMesh(body))};
    const asset::ModelData& stored = *scene.models()[index];
    anim::Animator bind(stored);
    bind.play(-1);
    if (model_bounds_.size() <= index) model_bounds_.resize(index + 1);
    model_bounds_[index] = anim::skinnedBounds(stored, bind.boneMatrices());
    if (index < renderer.uploadedModelCount()) renderer.uploadModel(scene, index);
    return index;
}

void RenderSync::releaseClothModels() {
    for (auto it = cloth_models_.begin(); it != cloth_models_.end();) {
        if (it->second.frame == frame_) {
            ++it;
            continue;
        }
        free_runtime_models_.push_back(it->second.index);
        forgetVariantsOf(it->second.index);
        it = cloth_models_.erase(it);
    }
}

void RenderSync::forgetVariantsOf(std::uint32_t base) {
    for (auto it = variant_lookup_.begin(); it != variant_lookup_.end();) {
        if (variants_[it->second].base == base) {
            variants_[it->second].base = UINT32_MAX;  // ya no se rehace
            it = variant_lookup_.erase(it);
        } else {
            ++it;
        }
    }
}

void RenderSync::releaseRuntimeMeshes() {
    for (auto it = runtime_meshes_.begin(); it != runtime_meshes_.end();) {
        if (it->second.mesh.expired()) {
            free_runtime_models_.push_back(it->second.index);
            forgetVariantsOf(it->second.index);
            warned_meshes_.erase(it->first);
            it = runtime_meshes_.erase(it);
        } else {
            ++it;
        }
    }
}

void RenderSync::applyMaterialChanges(scene::Scene& scene, gfx::VulkanRenderer& renderer) {
    if (rebuild_materials_.empty() && live_materials_.empty()) return;
    for (const Variant& variant : variants_) {
        if (variant.base >= scene.models().size()) continue;  // de una malla ya destruida
        bool rebuild = false;
        for (const Uuid& uuid : variant.overrides) {
            rebuild = rebuild || (uuid.valid() && rebuild_materials_.contains(uuid));
        }
        if (rebuild) {
            try {
                scene.replaceModel(variant.index, buildVariant(*scene.models()[variant.base], variant.overrides));
                // Ya en la GPU: se sube de nuevo (si no, la cola la subira).
                if (variant.index < renderer.uploadedModelCount()) renderer.uploadModel(scene, variant.index);
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
            applySurface(data->materials[slot], *mat, nullptr);
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
    // Leyendose en segundo plano: se mira si ya llego, sin esperar.
    if (auto pending = pending_clips_.find(key); pending != pending_clips_.end()) {
        if (pending->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return -1;
        std::optional<asset::AnimationClip> loaded = pending->second.get();
        pending_clips_.erase(pending);
        int index = -1;
        if (loaded && model < scene.models().size()) {
            // Se anade al modelo (el Animator reproduce por indice).
            asset::ModelData& data = *scene.models()[model];
            data.animations.push_back(std::move(*loaded));
            index = static_cast<int>(data.animations.size()) - 1;
        }
        external_clips_[key] = index;
        return index;
    }
    int index = -1;
    const auto info = assets_.database().find(clip);
    if (info && info->type == assets::AssetType::AnimationClip && model < scene.models().size() &&
        g_async_models.get()) {
        // Streaming de animaciones: el .cranim se lee en otro hilo con una copia
        // de la jerarquia del modelo (lo unico que hace falta para emparejar las
        // pistas). Leer y convertir un clip de Mixamo en el hilo principal era
        // un tiron la primera vez que un estado lo usaba.
        auto skeleton = std::make_shared<asset::ModelData>();
        skeleton->nodes = scene.models()[model]->nodes;
        const std::filesystem::path path = info->path;
        const std::string name = info->name;
        pending_clips_.emplace(key, std::async(std::launch::async,
                                               [skeleton, path, name]() -> std::optional<asset::AnimationClip> {
                                                   asset::AnimationClip loaded;
                                                   std::string error;
                                                   if (loadAnimationClip(path, *skeleton, loaded, &error) &&
                                                       !loaded.channels.empty()) {
                                                       return loaded;
                                                   }
                                                   std::cerr << "[RenderSync] Clip " << name
                                                             << " no sirve para este modelo " << error << "\n";
                                                   return std::nullopt;
                                               }));
        return -1;
    }
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

void RenderSync::prefetchClips(std::uint32_t model, const Uuid& controller_uuid, const AnimatorController& controller,
                               scene::Scene& scene) {
    if (!g_async_models.get() || !prefetched_controllers_.insert(ClipKey{model, controller_uuid}).second) return;
    for (const AnimatorState& state : controller.states) {
        if (state.clip.valid()) externalClip(model, state.clip.uuid, scene);
        for (const BlendTreeChild& child : state.children) {
            if (child.clip.valid()) externalClip(model, child.clip.uuid, scene);
        }
    }
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
    physbones_.clear();
    ragdoll_tips_.clear();
    shared_poses_.clear();
    socket_sources_.clear();
    drive_sockets_.clear();
    failed_.clear();
    materials_.clear();
    failed_materials_.clear();
    variants_.clear();
    variant_lookup_.clear();
    runtime_meshes_.clear();
    free_runtime_models_.clear();
    cloth_models_.clear();
    warned_meshes_.clear();
    rebuild_materials_.clear();
    live_materials_.clear();
    model_bounds_.clear();
    animations_.clear();
    controllers_.clear();
    failed_controllers_.clear();
    motion_features_.clear();  // por indice de modelo: se rehacen
    external_clips_.clear();
    pending_clips_.clear();  // (los futuros esperan a su hilo al destruirse)
    prefetched_controllers_.clear();
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
        if (g_async_models.get()) {
            // Streaming: lo lee un hilo de fondo; mientras, esta entidad no se
            // dibuja (el resto del frame sigue sin esperar).
            asset = assets_.requestModel(ref.uuid);
            if (!asset && !assets_.loadFailed(ref.uuid)) return std::nullopt;
        } else {
            asset = assets_.loadModel(ref.uuid);
        }
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

// --- Cinematica inversa --------------------------------------------------------

const RenderSync::HumanoidInfo& RenderSync::humanoidInfo(std::uint32_t model, const asset::ModelData& data) {
    auto it = humanoids_.find(model);
    if (it != humanoids_.end() && it->second.rest.size() == data.nodes.size()) return it->second;
    HumanoidInfo info;
    info.map = humanoid::detect(data.nodes);
    info.rest = humanoid::restGlobals(data.nodes);
    info.rig = creature::detect(data.nodes, info.rest);
    if (info.map.valid) {
        Vec3 right{};
        humanoid::characterAxes(info.map, info.rest, right, info.up, info.forward);
    } else if (info.rig.valid) {
        info.up = info.rig.up;
        info.forward = info.rig.forward;
    }
    return humanoids_[model] = std::move(info);
}

void RenderSync::applyInverseKinematics(World& world, Entity entity, const InverseKinematics& ik,
                                        anim::Animator& animator, const asset::ModelData& data, std::uint32_t model,
                                        IKSmoothing& smooth, float delta_seconds) {
    if (animator.locals().size() != data.nodes.size()) return;
    // Fraccion del camino hacia lo pedido este frame (amortiguado: `rate` por
    // segundo; sin tiempo, directo).
    const auto follow = [delta_seconds](float rate) {
        return delta_seconds > 0.0f ? 1.0f - std::exp(-rate * std::min(delta_seconds, 0.1f)) : 1.0f;
    };
    constexpr float kFootRate = 14.0f;   // pies y cadera (~70 ms)
    constexpr float kLimbRate = 12.0f;   // manos y pies a su objetivo
    constexpr float kLookRate = 5.0f;    // la mirada pasa de un objeto a otro (~0.2 s)
    constexpr float kLookFade = 3.0f;    // peso de la mirada al aparecer o perder el objetivo
    ik::Pose pose{&data.nodes, &animator.locals(), &animator.globals()};
    const Mat4& world_matrix = entity.worldMatrix();
    const Mat4 to_model = core::inverse(world_matrix);
    const auto point_to_model = [&](const Vec3& p) { return transformPoint(to_model, p); };
    const auto dir_to_model = [&](const Vec3& d) { return core::normalize(transformDirection(to_model, d)); };
    // Posicion (y giro) de una entidad objetivo en el espacio del modelo.
    const auto target_of = [&](const Uuid& id, Vec3& position, Quat* rotation) {
        if (!id.valid()) return false;
        const Entity t = world.find(id);
        if (!t.valid() || !t.activeInHierarchy()) return false;
        const Mat4 m = to_model * t.worldMatrix();
        Vec3 s{};
        Quat r{};
        decomposeMatrix(m, position, r, s);
        if (rotation != nullptr) *rotation = r;
        return true;
    };
    // Objetivo como entidad o como punto del mundo (Lua).
    const auto goal_of = [&](const Uuid& id, bool use_position, const Vec3& position, Vec3& out, Quat* rotation) {
        if (use_position) {
            out = point_to_model(position);
            return true;
        }
        return target_of(id, out, rotation);
    };
    const HumanoidInfo& human = humanoidInfo(model, data);
    const humanoid::Map& map = human.map;
    using humanoid::Bone;
    const auto node = [&](Bone b) { return map.valid ? map[b] : -1; };

    // --- Pies en el suelo ---
    if (ik.foot_grounding && map.valid && ground_query_ && ik.grounding_weight > 0.0f) {
        const Vec3 up_world{0.0f, 1.0f, 0.0f};
        const float base_y = world_matrix.m[3][1];  // los pies del personaje, en la animacion
        struct Foot {
            Bone upper, lower, foot;
            bool hit = false;
            Vec3 world{};
            float delta = 0.0f;
            Vec3 normal{0.0f, 1.0f, 0.0f};
        };
        Foot feet[2] = {{Bone::LeftUpperLeg, Bone::LeftLowerLeg, Bone::LeftFoot},
                        {Bone::RightUpperLeg, Bone::RightLowerLeg, Bone::RightFoot}};
        float lowest = 0.0f;
        int foot_index = -1;
        for (Foot& f : feet) {
            float& smoothed = smooth.feet[++foot_index];
            f.world = transformPoint(world_matrix, ik::nodePosition(pose, node(f.foot)));
            const float lift = f.world.y - base_y;  // cuanto levanta el pie la animacion
            Vec3 ground{};
            Vec3 normal{};
            const Vec3 origin{f.world.x, base_y + ik.max_step + 0.05f, f.world.z};
            if (!ground_query_(origin, up_world * -1.0f, ik.max_step * 2.0f + 0.1f, ground, normal, entity)) {
                smoothed += (0.0f - smoothed) * follow(kFootRate);
                continue;
            }
            f.hit = true;
            f.normal = normal;
            const float wanted = std::clamp(ground.y + lift - f.world.y, -ik.max_step, ik.max_step);
            smoothed += (wanted - smoothed) * follow(kFootRate);
            f.delta = smoothed;
            lowest = std::min(lowest, f.delta);
        }
        // La cadera baja lo que baje el pie mas bajo (si no, esa pierna no llega).
        if (lowest < 0.0f) {
            const Vec3 offset = transformDirection(to_model, up_world * (lowest * ik.grounding_weight));
            ik::translateGlobal(pose, node(Bone::Hips), offset);
        }
        for (const Foot& f : feet) {
            if (!f.hit) continue;
            const Vec3 target = point_to_model(Vec3{f.world.x, f.world.y + f.delta, f.world.z});
            ik::twoBone(pose, node(f.upper), node(f.lower), node(f.foot), target, nullptr, ik.grounding_weight);
            if (ik.align_feet) {
                const Quat tilt = ik::rotationBetween(dir_to_model(up_world), dir_to_model(f.normal));
                ik::rotateGlobal(pose, node(f.foot), core::slerp(Quat{}, tilt, ik.grounding_weight));
            }
        }
    }

    // --- Pies bloqueados (anti-patinaje) ---
    // Apoyado = el pie esta abajo (cerca de la altura minima que ha tenido) y
    // casi quieto en el mundo. Entonces se clava donde esta; se suelta cuando
    // la animacion lo levanta o la pierna tendria que estirarse demasiado.
    if (ik.foot_locking && map.valid) {
        const float base_y = world_matrix.m[3][1];
        const Bone legs[2][3] = {{Bone::LeftUpperLeg, Bone::LeftLowerLeg, Bone::LeftFoot},
                                 {Bone::RightUpperLeg, Bone::RightLowerLeg, Bone::RightFoot}};
        for (int i = 0; i < 2; ++i) {
            const int foot = node(legs[i][2]);
            if (foot < 0 || node(legs[i][0]) < 0 || node(legs[i][1]) < 0) continue;
            IKSmoothing::FootLock& lock = smooth.lock[i];
            const Vec3 p = transformPoint(world_matrix, ik::nodePosition(pose, foot));
            // Cuanto levanta el pie la animacion (sin lo que lo movio el suelo).
            const float lift = p.y - base_y - (ik.foot_grounding ? smooth.feet[i] : 0.0f);
            const Vec3 moved = p - lock.last;
            if (!lock.has_last || core::length(moved) > 2.0f) {
                // Primer frame o teletransporte: se empieza de cero.
                lock = IKSmoothing::FootLock{};
                lock.floor = lift;
            }
            // El suelo del pie baja al instante y sube despacio (se adapta).
            lock.floor = std::min(lift, lock.floor + 0.05f * delta_seconds);
            const float speed = lock.has_last && delta_seconds > 0.0f
                                    ? std::sqrt(moved.x * moved.x + moved.z * moved.z) / delta_seconds
                                    : 0.0f;
            const bool down = lift < lock.floor + 0.035f;
            const bool planted = lock.has_last && delta_seconds > 0.0f && down && speed < ik.foot_lock_speed;
            if (!lock.locked && planted) {
                lock.locked = true;
                lock.position = p;
            } else if (lock.locked) {
                const float dx = lock.position.x - p.x;
                const float dz = lock.position.z - p.z;
                if (lift > lock.floor + 0.06f || std::sqrt(dx * dx + dz * dz) > ik.foot_lock_release) {
                    lock.locked = false;
                }
            }
            lock.weight += ((lock.locked ? 1.0f : 0.0f) - lock.weight) * follow(lock.locked ? 40.0f : 12.0f);
            lock.last = p;
            lock.has_last = true;
            if (lock.weight > 0.001f) {
                // La altura la manda la animacion (y el suelo); se clava en horizontal.
                const Vec3 held{lock.position.x, p.y, lock.position.z};
                const Vec3 goal = p + (held - p) * lock.weight;
                ik::twoBone(pose, node(legs[i][0]), node(legs[i][1]), foot, point_to_model(goal), nullptr, 1.0f);
            }
        }
    }

    // --- Manos y pies a sus objetivos ---
    const auto limb = [&](int index, const IKLimb& l, Bone upper, Bone lower, Bone end) {
        Vec3 target{};
        Quat rotation{};
        if (!map.valid || l.weight <= 0.0f || !goal_of(l.target, l.use_position, l.position, target, &rotation)) {
            smooth.limb_valid[index] = false;
            return;
        }
        // Cambiar de objetivo lleva la mano alli en un momento, no de golpe.
        const Vec3 wanted = transformPoint(world_matrix, target);
        if (!smooth.limb_valid[index]) smooth.limb[index] = wanted;
        smooth.limb[index] = smooth.limb[index] + (wanted - smooth.limb[index]) * follow(kLimbRate);
        smooth.limb_valid[index] = true;
        target = point_to_model(smooth.limb[index]);
        Vec3 hint{};
        const bool has_hint = target_of(l.hint, hint, nullptr);
        ik::twoBone(pose, node(upper), node(lower), node(end), target, has_hint ? &hint : nullptr, l.weight);
        if (l.match_rotation && !l.use_position) {
            // Objetivo sin girar = el giro de reposo de la mano/el pie.
            const Quat rest = [&] {
                Vec3 t{};
                Quat r{};
                Vec3 s{};
                decomposeMatrix(human.rest[static_cast<std::size_t>(node(end))], t, r, s);
                return r;
            }();
            const Quat wanted = quatMultiply(rotation, rest);
            ik::setGlobalRotation(pose, node(end), core::slerp(ik::nodeRotation(pose, node(end)), wanted, l.weight));
        }
    };
    limb(0, ik.left_hand, Bone::LeftUpperArm, Bone::LeftLowerArm, Bone::LeftHand);
    limb(1, ik.right_hand, Bone::RightUpperArm, Bone::RightLowerArm, Bone::RightHand);
    limb(2, ik.left_foot, Bone::LeftUpperLeg, Bone::LeftLowerLeg, Bone::LeftFoot);
    limb(3, ik.right_foot, Bone::RightUpperLeg, Bone::RightLowerLeg, Bone::RightFoot);

    // --- Mirar: el giro se reparte entre el cuello y la cabeza ---
    // El hueso que mira: el pedido, la cabeza del humanoide o la del animal.
    Vec3 look{};
    int look_node = !ik.look_bone.empty() ? findBone(data, ik.look_bone)
                                          : (map.valid ? node(Bone::Head) : human.rig.head);
    // El punto mirado se desplaza hacia el objetivo (de un objeto a otro la
    // cabeza gira, no salta) y el peso sube o baja poco a poco al aparecer o
    // perder el objetivo.
    const bool has_look = look_node >= 0 && ik.look_weight > 0.0f &&
                          goal_of(ik.look_at, ik.look_use_position, ik.look_position, look, nullptr);
    if (has_look) {
        const Vec3 wanted = transformPoint(world_matrix, look);
        if (!smooth.look_valid) smooth.look = wanted;
        smooth.look = smooth.look + (wanted - smooth.look) * follow(kLookRate);
        smooth.look_valid = true;
    }
    const float look_goal = has_look ? ik.look_weight : 0.0f;
    smooth.look_weight += (look_goal - smooth.look_weight) * follow(kLookFade);
    if (!has_look && smooth.look_weight < 1e-3f) smooth.look_valid = false;
    if (look_node >= 0 && smooth.look_valid && smooth.look_weight > 1e-3f) {
        look = point_to_model(smooth.look);
        // Hacia donde mira ahora un hueso: el "delante" del personaje llevado
        // por lo que ese hueso ha girado desde su reposo.
        const auto facing = [&](int bone) {
            Vec3 t{};
            Quat rest{};
            Vec3 s{};
            decomposeMatrix(human.rest[static_cast<std::size_t>(bone)], t, rest, s);
            const Quat delta = quatMultiply(ik::nodeRotation(pose, bone), quatConjugate(rest));
            return quatRotate(delta, human.forward);
        };
        // Del de arriba (cuello) a la cabeza; el giro total se limita al maximo
        // y se reparte entre ellos.
        std::vector<int> bones{look_node};
        for (int k = 1; k < std::clamp(ik.look_chain, 1, 12); ++k) {
            const int parent = data.nodes[static_cast<std::size_t>(bones.back())].parent;
            if (parent < 0) break;
            bones.push_back(parent);
        }
        std::reverse(bones.begin(), bones.end());
        ik::lookChain(pose, bones, facing(look_node), look, smooth.look_weight, ik.look_max_angle);
    }

    // --- Patas al suelo (cualquier esqueleto: animales) ---
    // Como los pies del humanoide: cada pie se apoya en lo que tiene debajo, el
    // cuerpo baja lo que baje el pie mas bajo y, con 3 o mas patas, se inclina
    // con la pendiente (cuesta arriba, de lado).
    struct GroundFoot {
        std::vector<int> joints;
        const IKChain* chain = nullptr;
        Vec3 world{};
        float delta = 0.0f;
        Vec3 normal{0.0f, 1.0f, 0.0f};
        bool hit = false;
    };
    std::vector<GroundFoot> grounded;
    if (ground_query_ && ik.grounding_weight > 0.0f) {
        std::size_t leg_index = 0;
        const Vec3 up_world{0.0f, 1.0f, 0.0f};
        const float base_y = world_matrix.m[3][1];
        for (const IKChain& chain : ik.chains) {
            if (!chain.ground || chain.weight <= 0.0f) continue;
            GroundFoot f;
            f.chain = &chain;
            f.joints = ik::chainTo(data.nodes, findBone(data, chain.bone), std::clamp(chain.length, 1, 16));
            if (f.joints.size() < 2) continue;
            f.world = transformPoint(world_matrix, ik::nodePosition(pose, f.joints.back()));
            const float lift = f.world.y - base_y;
            Vec3 ground{};
            Vec3 normal{};
            const Vec3 origin{f.world.x, base_y + ik.max_step + 0.05f, f.world.z};
            if (smooth.legs.size() <= leg_index) smooth.legs.resize(leg_index + 1, 0.0f);
            float& smoothed = smooth.legs[leg_index++];
            if (ground_query_(origin, up_world * -1.0f, ik.max_step * 2.0f + 0.1f, ground, normal, entity)) {
                f.hit = true;
                f.normal = normal;
                const float wanted = std::clamp(ground.y + lift - f.world.y, -ik.max_step, ik.max_step);
                smoothed += (wanted - smoothed) * follow(kFootRate);
                f.delta = smoothed;
            } else {
                smoothed += (0.0f - smoothed) * follow(kFootRate);
            }
            grounded.push_back(std::move(f));
        }
        std::vector<int> roots;
        float lowest = 0.0f;
        float mean = 0.0f;
        int hits = 0;
        for (const GroundFoot& f : grounded) {
            roots.push_back(f.joints.front());
            if (!f.hit) continue;
            lowest = std::min(lowest, f.delta);
            mean += f.delta;
            ++hits;
        }
        mean = hits > 0 ? mean / static_cast<float>(hits) : 0.0f;
        const int body = procedural::commonAncestor(data.nodes, roots);
        const bool tilting = ik.align_body && ik.body_align_weight > 0.0f && hits >= 3;
        if (body >= 0 && !grounded.empty()) {
            // El cuerpo baja lo que baje la pata mas baja (si no, no llega);
            // si ademas se inclina, la inclinacion reparte las diferencias y
            // basta con bajar la media.
            const float drop = tilting ? std::min(mean, 0.0f) : lowest;
            if (drop < 0.0f) {
                ik::translateGlobal(pose, body, transformDirection(to_model, up_world * (drop * ik.grounding_weight)));
            }
            // Inclinar con la pendiente bajo las patas (3 o mas apoyadas).
            if (tilting) {
                std::vector<Vec3> feet;
                std::vector<float> heights;
                for (const GroundFoot& f : grounded) {
                    if (!f.hit) continue;
                    feet.push_back(f.world);
                    heights.push_back(f.delta);
                }
                const Vec3 forward = transformDirection(world_matrix, human.forward);
                const Quat tilt = ik::groundTilt(feet, heights, forward);
                // El giro del mundo con su eje pasado al modelo.
                const float w = std::clamp(tilt.w, -1.0f, 1.0f);
                const float s = std::sqrt(std::max(1.0f - w * w, 0.0f));
                if (s > 1e-5f) {
                    const Vec3 axis = core::normalize(transformDirection(to_model, Vec3{tilt.x / s, tilt.y / s, tilt.z / s}));
                    const float angle = 2.0f * std::acos(w) * ik.body_align_weight * ik.grounding_weight;
                    const Vec3 a = axis * std::sin(angle * 0.5f);
                    // Gira alrededor del centro de las caderas y los hombros (no
                    // de la cadera: el pecho bajaria el doble).
                    const auto center = [&] {
                        Vec3 c{};
                        for (const int r : roots) c = c + ik::nodePosition(pose, r);
                        return c * (1.0f / static_cast<float>(roots.size()));
                    };
                    const Vec3 before = center();
                    ik::rotateGlobal(pose, body, Quat{a.x, a.y, a.z, std::cos(angle * 0.5f)});
                    ik::translateGlobal(pose, body, before - center());
                }
            }
        }
        for (const GroundFoot& f : grounded) {
            if (!f.hit) continue;
            const Vec3 target = point_to_model(Vec3{f.world.x, f.world.y + f.delta, f.world.z});
            Vec3 hint{};
            const bool has_hint = target_of(f.chain->hint, hint, nullptr);
            ik::chain(pose, f.joints, target, has_hint ? &hint : nullptr, ik.grounding_weight * f.chain->weight);
            if (ik.align_feet) {
                const Quat tilt = ik::rotationBetween(dir_to_model(up_world), dir_to_model(f.normal));
                ik::rotateGlobal(pose, f.joints.back(), core::slerp(Quat{}, tilt, ik.grounding_weight));
            }
        }
    }

    // --- Cadenas a un objetivo (cualquier esqueleto) ---
    for (const IKChain& chain : ik.chains) {
        if (chain.ground || chain.bone.empty() || chain.weight <= 0.0f) continue;
        Vec3 target{};
        Quat rotation{};
        if (!goal_of(chain.target, chain.use_position, chain.position, target, &rotation)) continue;
        const std::vector<int> joints = ik::chainTo(data.nodes, findBone(data, chain.bone), std::clamp(chain.length, 1, 16));
        if (joints.size() < 2) continue;
        Vec3 hint{};
        const bool has_hint = target_of(chain.hint, hint, nullptr);
        ik::chain(pose, joints, target, has_hint ? &hint : nullptr, chain.weight);
        if (chain.match_rotation && !chain.use_position) {
            Vec3 t{};
            Quat rest{};
            Vec3 s{};
            decomposeMatrix(human.rest[static_cast<std::size_t>(joints.back())], t, rest, s);
            const Quat wanted = quatMultiply(rotation, rest);
            ik::setGlobalRotation(pose, joints.back(), core::slerp(ik::nodeRotation(pose, joints.back()), wanted, chain.weight));
        }
    }
    animator.updateBones();
}

// --- Animacion procedural -------------------------------------------------------

namespace {
int nodeByName(const asset::ModelData& data, const std::string& name) {
    if (name.empty()) return -1;
    for (std::size_t i = 0; i < data.nodes.size(); ++i) {
        if (data.nodes[i].name == name) return static_cast<int>(i);
    }
    // Sin el prefijo del programa ("mixamorig:Head" vale como "Head").
    for (std::size_t i = 0; i < data.nodes.size(); ++i) {
        const std::string& n = data.nodes[i].name;
        const auto cut = n.find_last_of(":|");
        if (cut != std::string::npos && n.compare(cut + 1, std::string::npos, name) == 0) return static_cast<int>(i);
    }
    return -1;
}
}  // namespace

void RenderSync::applyProceduralBefore(World& world, Entity entity, const ProceduralAnimation& proc,
                                       anim::Animator& animator, const asset::ModelData& data, std::uint32_t model,
                                       float delta_seconds) {
    (void)world;
    if (animator.locals().size() != data.nodes.size()) return;
    ik::Pose pose{&data.nodes, &animator.locals(), &animator.globals()};
    ProceduralState& state = procedural_[entity.handle()];
    const Mat4& world_matrix = entity.worldMatrix();
    const Vec3 position{world_matrix.m[3][0], world_matrix.m[3][1], world_matrix.m[3][2]};

    // Rehacer cadenas y patas si cambio lo que hay que simular.
    std::uint64_t signature = 1469598103934665603ull ^ model;
    const auto mix = [&](const std::string& text) {
        for (const char c : text) signature = (signature ^ static_cast<unsigned char>(c)) * 1099511628211ull;
        signature = (signature ^ 0xFFu) * 1099511628211ull;
    };
    for (const SpringBoneChain& s : proc.springs) mix(s.bone);
    for (const ProceduralLeg& l : proc.legs) mix(l.bone + "#" + std::to_string(l.group));
    if (signature != state.signature || state.model != model) {
        state = ProceduralState{};
        state.signature = signature;
        state.model = model;
        for (const SpringBoneChain& s : proc.springs) {
            state.springs.push_back(procedural::makeSpringChain(data.nodes, nodeByName(data, s.bone)));
        }
        bool automatic = std::all_of(proc.legs.begin(), proc.legs.end(), [](const ProceduralLeg& l) { return l.group < 0; });
        std::vector<int> uppers;
        for (std::size_t i = 0; i < proc.legs.size(); ++i) {
            procedural::Leg leg;
            leg.end = nodeByName(data, proc.legs[i].bone);
            leg.mid = leg.end >= 0 ? data.nodes[static_cast<std::size_t>(leg.end)].parent : -1;
            leg.upper = leg.mid >= 0 ? data.nodes[static_cast<std::size_t>(leg.mid)].parent : -1;
            leg.group = automatic ? static_cast<int>(i % 2) : std::max(proc.legs[i].group, 0);
            if (leg.upper < 0) continue;
            uppers.push_back(leg.upper);
            state.legs.push_back(leg);
        }
        state.body = procedural::commonAncestor(data.nodes, uppers);
    }

    // Movimiento del personaje (suavizado): para inclinarse y adelantar pasos.
    if (delta_seconds > 0.0f) {
        state.time += delta_seconds;
        if (state.has_last && core::length(position - state.last_position) < 5.0f) {
            const Vec3 velocity = (position - state.last_position) * (1.0f / delta_seconds);
            const float blend = 1.0f - std::exp(-delta_seconds * 10.0f);
            const Vec3 acceleration = (velocity - state.velocity) * (1.0f / delta_seconds);
            state.acceleration = state.acceleration + (acceleration - state.acceleration) * (1.0f - std::exp(-delta_seconds * 6.0f));
            state.velocity = state.velocity + (velocity - state.velocity) * blend;
        }
        state.last_position = position;
        state.has_last = true;
    }

    // --- Capas (humanoides) ---
    const HumanoidInfo& human = humanoidInfo(model, data);
    if (human.map.valid) {
        using humanoid::Bone;
        const Vec3 right = core::cross(human.forward, human.up);
        if (proc.breathing && proc.breath_amount > 0.0f) {
            const float breath = std::sin(state.time * proc.breath_rate / 60.0f * 2.0f * core::kPi) * proc.breath_amount;
            procedural::rotateBone(pose, human.map[Bone::Chest], right, -breath * 0.6f);
            procedural::rotateBone(pose, human.map[Bone::UpperChest], right, -breath * 0.4f);
        }
        if (proc.lean && proc.lean_amount > 0.0f) {
            // Aceleracion hacia delante y hacia la derecha del personaje.
            const Vec3 forward_world = core::normalize(transformDirection(world_matrix, human.forward));
            const Vec3 right_world = core::normalize(transformDirection(world_matrix, right));
            const float per_unit = proc.lean_amount / 6.0f;  // del todo a 6 m/s²
            const float pitch = std::clamp(-core::dot(state.acceleration, forward_world) * per_unit, -proc.lean_amount,
                                           proc.lean_amount);
            const float roll = std::clamp(core::dot(state.acceleration, right_world) * per_unit, -proc.lean_amount,
                                          proc.lean_amount);
            procedural::rotateBone(pose, human.map[Bone::Spine], right, pitch * 0.6f);
            procedural::rotateBone(pose, human.map[Bone::Chest], right, pitch * 0.4f);
            procedural::rotateBone(pose, human.map[Bone::Spine], human.forward, roll * 0.6f);
            procedural::rotateBone(pose, human.map[Bone::Chest], human.forward, roll * 0.4f);
        }
    }
    for (std::size_t i = 0; i < proc.noise.size(); ++i) {
        const ProceduralNoise& n = proc.noise[i];
        const int bone = nodeByName(data, n.bone);
        if (bone < 0 || n.amplitude <= 0.0f) continue;
        const float seed = static_cast<float>(i) * 12.9898f + static_cast<float>(bone) * 4.1414f;
        procedural::rotateBone(pose, bone, Vec3{1, 0, 0}, procedural::smoothNoise(state.time, n.frequency, seed) * n.amplitude);
        procedural::rotateBone(pose, bone, Vec3{0, 1, 0}, procedural::smoothNoise(state.time, n.frequency, seed + 3.1f) * n.amplitude);
        procedural::rotateBone(pose, bone, Vec3{0, 0, 1}, procedural::smoothNoise(state.time, n.frequency, seed + 7.3f) * n.amplitude);
    }

    // --- Patas ---
    if (!state.legs.empty()) {
        procedural::LegSettings settings;
        settings.step_distance = proc.step_distance;
        settings.step_height = proc.step_height;
        settings.step_duration = proc.step_duration;
        settings.overshoot = proc.step_overshoot;
        settings.adjust_body = proc.adjust_body;
        settings.body_weight = proc.body_weight;
        procedural::GroundQuery ground;
        if (ground_query_) {
            ground = [&](const Vec3& o, const Vec3& d, float max, Vec3& p, Vec3& n) {
                return ground_query_(o, d, max, p, n, entity);
            };
        }
        procedural::updateLegs(pose, world_matrix, state.legs, state.body, settings, ground, state.velocity, delta_seconds);
    }
}

void RenderSync::applyProceduralSprings(Entity entity, const ProceduralAnimation& proc, anim::Animator& animator,
                                        const asset::ModelData& data, std::uint32_t model, float delta_seconds) {
    const auto it = procedural_.find(entity.handle());
    if (it == procedural_.end() || it->second.springs.empty() || animator.locals().size() != data.nodes.size()) return;
    ik::Pose pose{&data.nodes, &animator.locals(), &animator.globals()};
    const Mat4& world_matrix = entity.worldMatrix();
    // Esferas del cuerpo (humanoides): cabeza, pecho y cadera.
    std::vector<procedural::Sphere> colliders;
    const HumanoidInfo& human = humanoidInfo(model, data);
    if (proc.body_colliders && human.map.valid) {
        using humanoid::Bone;
        const auto at = [&](Bone b) { return transformPoint(world_matrix, ik::nodePosition(pose, human.map[b])); };
        const float size = core::length(at(Bone::Head) - at(Bone::Hips));  // cadera a cabeza
        colliders.push_back({at(Bone::Head) + (at(Bone::Head) - at(Bone::Neck)) * 0.5f, size * 0.16f});
        const Bone chest = human.map[Bone::UpperChest] >= 0 ? Bone::UpperChest : Bone::Chest;
        colliders.push_back({at(chest), size * 0.24f});
        colliders.push_back({at(Bone::Hips), size * 0.24f});
    }
    for (std::size_t i = 0; i < it->second.springs.size() && i < proc.springs.size(); ++i) {
        const SpringBoneChain& s = proc.springs[i];
        procedural::SpringSettings settings;
        settings.stiffness = s.stiffness;
        settings.damping = s.damping;
        settings.gravity = s.gravity;
        settings.radius = s.radius;
        procedural::updateSprings(pose, world_matrix, it->second.springs[i], settings, colliders, delta_seconds);
    }
}

// Agua: los cuerpos de este frame al renderizador (parametros de sus olas y
// color) y la cinta de cada rio, que solo se rehace si cambian sus puntos.
void RenderSync::syncWater(World& world, gfx::VulkanRenderer& renderer, float delta_seconds,
                           const core::Vec3& camera_position) {
    water::advanceWaterTime(delta_seconds);
    std::vector<gfx::WaterBodyDesc> bodies;
    std::unordered_map<entt::entity, RiverMesh> seen;
    std::vector<std::pair<const water::WaterBody*, Mat4>> water_bodies;
    int underwater = -1;
    std::shared_ptr<const water::OceanSpectrum> ocean_spectrum;
    float ocean_foam = 1.0f;
    float ocean_foam_persistence = 4.0f;
    world.forEachDepthFirst([&](Entity e) {
        const water::WaterBody* body = e.tryGet<water::WaterBody>();
        if (body == nullptr || !e.activeInHierarchy() || bodies.size() >= gfx::kMaxWaterBodies) return;
        const Mat4& m = e.worldMatrix();
        const Vec3 origin = e.worldPosition();
        water_bodies.emplace_back(body, m);
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
        p.under = core::Vec4{body->god_rays, body->particles, 0.0f, 0.0f};
        // Oceano FFT: el espectro del primero (el mismo que usa la flotacion).
        if (body->type == water::WaterType::Ocean && ocean_spectrum == nullptr) {
            ocean_spectrum = water::oceanSpectrum(*body);
            ocean_foam = body->foam;
            ocean_foam_persistence = body->foam_persistence;
        }

        // Camara cerca o bajo la superficie de cualquier agua (oceano, lago o
        // rio): el shader decide por pixel con la misma ola. Solo si ahi hay
        // agua de verdad: el suelo bajo la camara esta por debajo de la
        // superficie (el rectangulo de un lago tambien cubre orillas y valles
        // secos, y bajo tierra tampoco se esta en el agua).
        if (underwater < 0) {
            const int forced = water::underwaterOverride(camera_position);
            const water::WaterSample at_camera = water::sampleWater(*body, m, camera_position, water::waterTime());
            bool inside = false;
            if (forced == 1) {
                inside = true;
            } else if (forced < 0 && at_camera.inside) {
                // Oceano FFT: la altura de sus olas sale del espectro (viento).
                const float waves = body->type == water::WaterType::Ocean
                                        ? std::max(body->wave_height, water::oceanSpectrum(*body)->significant_height)
                                        : body->wave_height;
                const float margin = body->type == water::WaterType::River ? 0.3f : waves + 0.5f;
                if (camera_position.y < at_camera.height + margin) {
                    float ground = 0.0f;
                    const bool has_ground = groundHeight(camera_position.x, camera_position.z, ground);
                    inside = !has_ground || (ground < at_camera.height - 0.05f && camera_position.y > ground - 0.5f);
                }
            }
            if (inside) {
                underwater = static_cast<int>(bodies.size());
                p.extra.z = at_camera.height;  // superficie aqui (el rio no es un plano)
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
                for (std::size_t s = 0; s < line.size(); ++s) {
                    const water::RiverSample& sample = line[s];
                    const Vec3 side{-sample.tangent.z, 0.0f, sample.tangent.x};
                    // Desnivel (m por m) con los vecinos: rapidos donde baja.
                    const water::RiverSample& before = line[s > 0 ? s - 1 : s];
                    const water::RiverSample& after = line[s + 1 < line.size() ? s + 1 : s];
                    const float run = after.distance - before.distance;
                    const float drop = run > 1e-3f ? std::max(before.position.y - after.position.y, 0.0f) / run : 0.0f;
                    for (std::uint32_t j = 0; j < kAcross; ++j) {
                        const float a = static_cast<float>(j) / static_cast<float>(kAcross - 1);
                        const Vec3 pos = sample.position + side * ((a - 0.5f) * sample.width);
                        // flow = direccion * ancho: el shader saca de ahi las
                        // coordenadas del rio en metros (a traves, a lo largo).
                        const float width = std::max(sample.width, 0.01f);
                        mesh.vertices.push_back(gfx::WaterVertex{{pos.x, pos.y, pos.z},
                                                                 {a, sample.distance},
                                                                 {sample.tangent.x * width, sample.tangent.z * width},
                                                                 drop});
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
    gfx::WaterSpectrumDesc spectrum;
    if (ocean_spectrum != nullptr) {
        spectrum.modes = &ocean_spectrum->modes;
        spectrum.key = ocean_spectrum->key;
        static_assert(water::kOceanCascades == 4 && gfx::kOceanCascades == 4, "4 cascadas en la CPU y la GPU");
        const auto& sizes = ocean_spectrum->sizes;
        const auto& slopes = ocean_spectrum->slope_variance;
        spectrum.sizes = core::Vec4{sizes[0], sizes[1], sizes[2], sizes[3]};
        spectrum.slope_variance = core::Vec4{slopes[0], slopes[1], slopes[2], slopes[3]};
        spectrum.choppiness = ocean_spectrum->choppiness;
        spectrum.significant_height = ocean_spectrum->significant_height;
        spectrum.foam = ocean_foam;
        spectrum.foam_persistence = ocean_foam_persistence;
        spectrum.period = water::kOceanPeriod;
    }
    renderer.setWaterSpectrum(spectrum);
    updateRipples(world, renderer, delta_seconds, camera_position, water_bodies);
}

// Olas interactivas: todo lo que tiene collider (o Rigidbody, o es un agente
// de navegacion) y cruza la superficie:
//   - si se mueve, empuja el agua: al caer, segun su velocidad hacia abajo
//     (salpicadura); al avanzar, segun su velocidad (estela). Da igual como
//     se mueva: fisica, script, animacion o el gizmo del editor.
//   - si esta quieto y no es enorme (poste, roca, pilar, una caja flotando),
//     es un obstaculo: las ondas chocan con el y rebotan.
void RenderSync::updateRipples(World& world, gfx::VulkanRenderer& renderer, float delta_seconds,
                               const Vec3& camera_position,
                               const std::vector<std::pair<const water::WaterBody*, Mat4>>& bodies) {
    if (bodies.empty()) {
        if (ripples_.active()) ripples_.clear();
        ripple_previous_.clear();
        renderer.setWaterRipples({}, 0, 0.0f, 0.0f, 0.0f);
        return;
    }
    if (delta_seconds <= 0.0f) return;  // la segunda vista del editor: ya se hizo
    std::vector<water::RippleSource> sources;
    std::vector<water::RippleObstacle> obstacles;
    std::unordered_map<entt::entity, Vec3> positions;
    const float half_extent = water::RippleSimulation::kSize * water::RippleSimulation::kCellSize * 0.5f;
    constexpr float kMaxObstacle = 5.0f;  // medio lado maximo de un obstaculo (m)

    // Caja local (en el espacio de la entidad) de su forma de colision.
    const auto local_shape = [&](Entity e, Vec3& mn, Vec3& mx, bool& round) {
        round = false;
        if (const auto* box = e.tryGet<physics::BoxCollider>()) {
            mn = box->center - box->size * 0.5f;
            mx = box->center + box->size * 0.5f;
            return;
        }
        if (const auto* sphere = e.tryGet<physics::SphereCollider>()) {
            const Vec3 r{sphere->radius, sphere->radius, sphere->radius};
            mn = sphere->center - r;
            mx = sphere->center + r;
            round = true;
            return;
        }
        if (const auto* capsule = e.tryGet<physics::CapsuleCollider>()) {
            Vec3 half{capsule->radius, capsule->radius, capsule->radius};
            (&half.x)[static_cast<int>(capsule->axis)] = std::max(capsule->height * 0.5f, capsule->radius);
            mn = capsule->center - half;
            mx = capsule->center + half;
            round = capsule->axis == physics::CapsuleAxis::Y;
            return;
        }
        if (const auto* wheel = e.tryGet<physics::WheelCollider>()) {
            const Vec3 half{wheel->width * 0.5f, wheel->radius, wheel->radius};
            mn = half * -1.0f;
            mx = half;
            return;
        }
        if (const auto* agent = e.tryGet<navigation::NavAgent>()) {
            mn = Vec3{-agent->radius, 0.0f, -agent->radius};
            mx = Vec3{agent->radius, agent->height, agent->radius};
            round = true;
            return;
        }
        // Mesh Collider (y lo demas): la caja de su modelo.
        const int actor = actorIndex(e);
        if (actor >= 0 && actorLocalBounds(static_cast<std::uint32_t>(actor), mn, mx)) return;
        mn = Vec3{-0.5f, -0.5f, -0.5f};
        mx = Vec3{0.5f, 0.5f, 0.5f};
    };

    const auto consider = [&](Entity e) {
        if (!e.activeInHierarchy()) return;
        const Mat4& m = e.worldMatrix();
        Vec3 mn{};
        Vec3 mx{};
        bool round = false;
        local_shape(e, mn, mx, round);
        // Centro y extension en el mundo (las 8 esquinas).
        const Vec3 local_center = (mn + mx) * 0.5f;
        const Vec3 center = transformPoint(m, local_center);
        positions[e.handle()] = center;
        if (std::abs(center.x - camera_position.x) > half_extent + kMaxObstacle ||
            std::abs(center.z - camera_position.z) > half_extent + kMaxObstacle) {
            return;
        }
        // Eje u: el X de la entidad en el plano xz.
        Vec3 axis{m.m[0][0], 0.0f, m.m[0][2]};
        axis = core::length(axis) > 1e-5f ? core::normalize(axis) : Vec3{1.0f, 0.0f, 0.0f};
        const Vec3 side{-axis.z, 0.0f, axis.x};
        float bottom = 1e30f;
        float top = -1e30f;
        float half_u = 0.0f;
        float half_v = 0.0f;
        for (int c = 0; c < 8; ++c) {
            const Vec3 corner = transformPoint(m, Vec3{(c & 1) ? mx.x : mn.x, (c & 2) ? mx.y : mn.y, (c & 4) ? mx.z : mn.z});
            bottom = std::min(bottom, corner.y);
            top = std::max(top, corner.y);
            const Vec3 d = corner - center;
            half_u = std::max(half_u, std::abs(core::dot(d, axis)));
            half_v = std::max(half_v, std::abs(core::dot(d, side)));
        }
        // Velocidad por el frame anterior.
        const auto previous = ripple_previous_.find(e.handle());
        Vec3 velocity{};
        if (previous != ripple_previous_.end()) velocity = (center - previous->second) * (1.0f / delta_seconds);
        const float speed = core::length(velocity);
        if (speed > 30.0f) return;  // teletransporte, no movimiento
        const bool moving = speed > 0.08f;
        // Quieto y enorme (un suelo, la orilla): ni empuja ni hace de obstaculo.
        if (!moving && std::max(half_u, half_v) > kMaxObstacle) return;

        // La superficie del agua en su centro.
        for (const auto& [body, bm] : bodies) {
            const water::WaterSample surface = water::sampleWater(*body, bm, center, water::waterTime());
            if (!surface.inside) continue;
            const float radius = std::clamp(std::max(half_u, half_v), 0.15f, 6.0f);
            if (moving) {
                // Cruza la superficie o va justo por debajo.
                if (bottom > surface.height + 0.1f || top < surface.height - radius) return;
                const float fall = std::max(-velocity.y, 0.0f);
                const float rise = std::max(velocity.y, 0.0f);
                const float along = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
                // Salpicadura (caer) + estela (avanzar); al salir tira un poco hacia arriba.
                // Tope: algo muy rapido (una bala, un teletransporte corto) no
                // rompe la superficie en picos.
                const float push = std::clamp(fall * 0.9f + along * 0.35f - rise * 0.3f, -2.0f, 5.0f);
                if (std::abs(push) < 0.01f) return;
                sources.push_back(water::RippleSource{center, radius, push * std::min(radius, 1.5f)});
            } else if (bottom < surface.height - 0.02f && top > surface.height + 0.02f) {
                // Atraviesa la superficie: obstaculo.
                water::RippleObstacle o;
                o.x = center.x;
                o.z = center.z;
                o.axis_x = axis.x;
                o.axis_z = axis.z;
                o.half_u = half_u;
                o.half_v = half_v;
                o.round = round;
                obstacles.push_back(o);
            }
            return;
        }
    };
    // Cada entidad una vez, tenga los componentes que tenga.
    std::unordered_set<entt::entity> done;
    const auto each = [&](auto view) {
        for (const entt::entity h : view) {
            if (done.insert(h).second) consider(world.wrap(h));
        }
    };
    entt::registry& registry = world.registry();
    each(registry.view<physics::Rigidbody>());
    each(registry.view<physics::BoxCollider>());
    each(registry.view<physics::SphereCollider>());
    each(registry.view<physics::CapsuleCollider>());
    each(registry.view<physics::MeshCollider>());
    each(registry.view<physics::WheelCollider>());
    each(registry.view<navigation::NavAgent>());
    ripple_previous_ = std::move(positions);

    ripples_.update(camera_position, delta_seconds, sources, obstacles);
    if (ripples_.active()) {
        renderer.setWaterRipples(ripples_.heights(), water::RippleSimulation::kSize, ripples_.originX(), ripples_.originZ(),
                                 water::RippleSimulation::kCellSize);
    } else {
        renderer.setWaterRipples({}, 0, 0.0f, 0.0f, 0.0f);
    }
}

void RenderSync::sync(World& world, scene::Scene& scene, gfx::VulkanRenderer& renderer,
                      float delta_seconds, const Options& options) {
    renderer_ = &renderer;  // los .crshader se compilan al construir variantes
    // Mallas editables (modelado) -> su malla del MeshRenderer.
    modeling::updateEditableMeshes(world);
    // La animacion la lleva el componente Animator, no scene.update().
    scene.setAnimateActors(false);
    syncActors(world, scene, renderer, delta_seconds);
    environment_delta_ = delta_seconds;
    syncLightsAndEnvironment(world, scene, renderer);
    syncTerrains(world, renderer, scene.camera().position());
    syncFoliage(world, renderer);
    if (options.apply_main_camera) {
        syncCamera(world, scene);
    }
    // Despues de la camara: el agua mira si la camara esta sumergida.
    syncWater(world, renderer, delta_seconds, scene.camera().position());
    // Al final (la escena ya esta lista): lo que ven las camaras con Target
    // Texture, antes del frame de la pantalla.
    if (options.render_textures) renderCameraTextures(world, scene, renderer);
}

RenderSync::~RenderSync() {
    destroyTerrains();
    destroyRenderTextures();
}

// -----------------------------------------------------------------------------
// Render Textures
// -----------------------------------------------------------------------------

std::int32_t RenderSync::renderTextureForPath(const std::filesystem::path& file) {
    if (renderer_ == nullptr) return -1;
    const std::string key = file.lexically_normal().generic_string();
    if (const auto it = render_textures_.find(key); it != render_textures_.end()) return it->second.id;
    assets::RenderTextureAsset asset;
    std::string error;
    RenderTextureGpu gpu;
    if (!assets::loadRenderTexture(file, asset, &error)) {
        std::cerr << "[RenderSync] Render Texture " << file.generic_string() << ": " << error << "\n";
    } else {
        gpu.id = renderer_->createRenderTexture(static_cast<std::uint32_t>(asset.width),
                                                static_cast<std::uint32_t>(asset.height));
        std::error_code ec;
        gpu.stamp = std::filesystem::last_write_time(file, ec);
    }
    render_textures_[key] = gpu;  // tambien los que fallan: no se reintenta cada frame
    return gpu.id;
}

std::int32_t RenderSync::renderTextureIdForAsset(const std::string& relative) {
    return renderTextureForPath(assets_.database().root() / fromUtf8(relative));
}

std::int32_t RenderSync::renderTextureFor(const Uuid& uuid) {
    const auto info = assets_.database().find(uuid);
    if (!info || info->type != assets::AssetType::RenderTexture) return -1;
    return renderTextureForPath(info->path);
}

// Cada ~segundo: un .crrt con otro tamano (editado en el Inspector) se rehace.
void RenderSync::refreshRenderTextures() {
    if (renderer_ == nullptr || ++render_texture_checks_ % 60 != 0) return;
    for (auto& [key, gpu] : render_textures_) {
        std::error_code ec;
        const std::filesystem::path file(key);
        const auto stamp = std::filesystem::last_write_time(file, ec);
        if (ec || stamp == gpu.stamp) continue;
        gpu.stamp = stamp;
        assets::RenderTextureAsset asset;
        if (!assets::loadRenderTexture(file, asset)) continue;
        if (gpu.id < 0) {
            gpu.id = renderer_->createRenderTexture(static_cast<std::uint32_t>(asset.width),
                                                    static_cast<std::uint32_t>(asset.height));
        } else {
            renderer_->resizeRenderTexture(gpu.id, static_cast<std::uint32_t>(asset.width),
                                           static_cast<std::uint32_t>(asset.height));
        }
    }
}

void RenderSync::renderCameraTextures(World& world, scene::Scene& scene, gfx::VulkanRenderer& renderer) {
    refreshRenderTextures();
    struct Job {
        Entity entity;
        std::int32_t texture;
    };
    std::vector<Job> jobs;
    world.forEachDepthFirst([&](Entity e) {
        const Camera* camera = e.tryGet<Camera>();
        if (camera == nullptr || !camera->target_texture.valid() || !e.activeInHierarchy()) return;
        const std::int32_t id = renderTextureFor(camera->target_texture.uuid);
        if (id >= 0) jobs.push_back({e, id});
    });
    for (const Job& job : jobs) {
        const Camera& camera = job.entity.get<Camera>();
        scene::Camera view = scene.camera();
        view.setPosition(job.entity.worldPosition());
        view.setOrientation(job.entity.forward(), job.entity.up());
        view.setFovY(camera.fov * kDegToRad);
        view.setOrthographic(camera.orthographic, camera.ortho_size);
        view.setClipPlanes(camera.near_plane, camera.far_plane);
        renderer.renderToTexture(scene, view, job.texture);
    }
}

void RenderSync::destroyRenderTextures() {
    if (renderer_ != nullptr) {
        for (auto& [key, gpu] : render_textures_) renderer_->destroyRenderTexture(gpu.id);
    }
    render_textures_.clear();
}

void RenderSync::destroyTerrains() {
    if (renderer_ != nullptr) {
        for (auto& [entity, gpu] : terrains_) renderer_->destroyTerrain(gpu.id);
    }
    terrains_.clear();
}

// Vegetacion: firma de todo lo que influye en la siembra; si cambia, se
// siembra de nuevo en otro hilo (el editor no se para) y al terminar se sube.
void RenderSync::syncFoliage(World& world, gfx::VulkanRenderer& renderer) {
    std::uint64_t signature = 1469598103934665603ull;
    const auto hash_bytes = [&](const void* data, std::size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) signature = (signature ^ p[i]) * 1099511628211ull;
    };
    struct Job {
        foliage::Foliage params;
        core::Vec3 center;
    };
    std::vector<Job> jobs;
    const foliage::Foliage* first = nullptr;
    // Coordenadas absolutas (sin el origen flotante): desplazar el mundo no
    // cambia la firma ni obliga a sembrar otra vez.
    const core::Vec3 world_origin{static_cast<float>(world.origin().x), static_cast<float>(world.origin().y),
                                  static_cast<float>(world.origin().z)};
    renderer.setFoliageOrigin(world_origin);
    for (const entt::entity handle : world.registry().view<foliage::Foliage>()) {
        const Entity e = world.wrap(handle);
        if (!e.activeInHierarchy()) continue;
        const foliage::Foliage& f = e.get<foliage::Foliage>();
        if (first == nullptr) first = &f;
        const core::Vec3 p = e.worldPosition() + world_origin;
        // Solo lo que cambia la siembra (no las distancias de dibujo).
        const float values[] = {f.area, f.density, f.pine, f.oak, f.birch, f.min_scale, f.max_scale, f.min_height,
                                f.max_height, f.max_slope, p.x, p.y, p.z};
        hash_bytes(values, sizeof(values));
        const int ints[] = {f.seed, f.max_instances, f.on_terrain ? 1 : 0};
        hash_bytes(ints, sizeof(ints));
        for (const foliage::FoliageClearing& c : f.clearings) {
            const float cv[] = {c.center.x, c.center.z, c.radius};
            hash_bytes(cv, sizeof(cv));
        }
        jobs.push_back({f, p});
        for (foliage::FoliageClearing& c : jobs.back().params.clearings) c.center = c.center + world_origin;
    }
    // El agua de la escena (rios y lagos): alli no crecen arboles.
    if (!jobs.empty()) {
        std::vector<foliage::FoliageWater> water;
        for (const entt::entity handle : world.registry().view<water::WaterBody>()) {
            const Entity e = world.wrap(handle);
            if (!e.activeInHierarchy()) continue;
            const water::WaterBody& body = e.get<water::WaterBody>();
            const core::Mat4& m = e.worldMatrix();
            if (body.type == water::WaterType::River) {
                const std::vector<water::RiverSample> line = water::riverCenterline(body, m, 4.0f);
                for (std::size_t i = 0; i + 1 < line.size(); ++i) {
                    foliage::FoliageWater w;
                    w.river = true;
                    w.a = line[i].position + world_origin;
                    w.b = line[i + 1].position + world_origin;
                    w.width = std::max(line[i].width, line[i + 1].width);
                    water.push_back(w);
                }
            } else if (body.type == water::WaterType::Lake) {
                foliage::FoliageWater w;
                w.a = e.worldPosition() + world_origin;
                w.half = core::Vec2{body.size.x * 0.5f, body.size.y * 0.5f};
                w.angle = std::atan2(-m.m[0][2], m.m[0][0]);
                w.level = w.a.y;
                water.push_back(w);
            }
        }
        for (const foliage::FoliageWater& w : water) {
            const float values[] = {w.a.x, w.a.y, w.a.z, w.b.x, w.b.z, w.width, w.half.x, w.half.y, w.angle};
            hash_bytes(values, sizeof(values));
        }
        for (Job& job : jobs) job.params.water = water;
    }
    std::vector<foliage::FoliageGround> ground;
    if (!jobs.empty() && terrain_store_ != nullptr) {
        for (const entt::entity handle : world.registry().view<terrain::Terrain>()) {
            const Entity e = world.wrap(handle);
            if (!e.activeInHierarchy()) continue;
            const terrain::Terrain& t = e.get<terrain::Terrain>();
            std::shared_ptr<terrain::TerrainData> data = terrain_store_->get(t);
            if (!data) continue;
            const core::Vec3 origin = e.worldPosition() + world_origin;
            // El terreno cuenta al terminar cada trazo (no en cada toque del pincel).
            const std::uint64_t version = data->collisionVersion();
            const float values[] = {t.size, t.height, origin.x, origin.y, origin.z};
            hash_bytes(values, sizeof(values));
            hash_bytes(&version, sizeof(version));
            ground.push_back({data, t, origin});
        }
    }
    if (first != nullptr) {
        gfx::FoliageSettings foliage_settings = first->settings();
        environment::applyToFoliage(environment_frame_, foliage_settings);  // viento del ambiente
        renderer.setFoliageSettings(foliage_settings);
        renderer.setFoliageSpecies(first->species());
    }

    // Termino una siembra: a la GPU.
    if (foliage_job_.valid() && foliage_job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        std::vector<gfx::FoliageInstance> instances = foliage_job_.get();
        foliage_count_ = instances.size();
        renderer.setFoliage(instances);
        foliage_signature_ = foliage_job_signature_;
        foliage_uploaded_ = true;
    }
    if (jobs.empty()) {
        if (foliage_uploaded_) {
            renderer.clearFoliage();
            foliage_uploaded_ = false;
            foliage_count_ = 0;
        }
        foliage_signature_ = 0;
        return;
    }
    if (signature != foliage_signature_ && !foliage_job_.valid()) {
        foliage_job_signature_ = signature;
        foliage_job_ = std::async(std::launch::async, [jobs = std::move(jobs), ground = std::move(ground)]() {
            std::vector<gfx::FoliageInstance> all;
            for (const Job& job : jobs) {
                foliage::FoliageResult r = foliage::generateFoliage(job.params, job.center, ground);
                all.insert(all.end(), r.instances.begin(), r.instances.end());
                if (all.size() >= gfx::FoliagePass::kMaxInstances) break;
            }
            return all;
        });
    }
}

// Terrenos: se crean en el renderizador la primera vez, se suben las regiones
// que cambiaron (esculpir/pintar) y su descripcion (posicion, capas) cada frame.
bool RenderSync::groundHeight(float x, float z, float& height) const {
    for (const TerrainSample& t : terrain_samples_) {
        const float size = std::max(t.terrain.size, 1.0f);
        if (x < t.origin.x || z < t.origin.z || x > t.origin.x + size || z > t.origin.z + size) continue;
        height = terrain::heightAt(*t.data, t.terrain, t.origin, x, z);
        return true;
    }
    return false;
}

void RenderSync::syncTerrains(World& world, gfx::VulkanRenderer& renderer, const core::Vec3& eye) {
    grass_eye_ = eye;
    terrain_samples_.clear();
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
            terrain_samples_.push_back({data, comp, e.worldPosition()});
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
            // Hierba: el componente en la misma entidad.
            if (const foliage::Grass* grass = e.tryGet<foliage::Grass>()) desc.grass = grass->desc();
            environment::applyToGrass(environment_frame_, desc.grass);  // viento, estacion y nieve
            renderer.setTerrainDesc(gpu.id, desc);
        }
    }
    // Lo que aparta la hierba: los cuerpos fisicos que se mueven y los
    // personajes cerca de la camara (hasta 32, los mas cercanos).
    {
        std::vector<core::Vec4> spheres;
        const core::Vec3 eye = grass_eye_;
        world.forEachDepthFirst([&](Entity e) {
            if (!e.activeInHierarchy()) return;
            // Cuerpos que se mueven (dinamicos y cinematicos: jugadores,
            // vehiculos...), con el tamano de su collider.
            const physics::Rigidbody* body = e.tryGet<physics::Rigidbody>();
            if (body == nullptr || body->type == physics::BodyType::Static) return;
            const core::Vec3 s = e.localScale();
            const float scale = std::max(std::abs(s.x), std::max(std::abs(s.y), std::abs(s.z)));
            float radius = 0.5f * scale;
            if (const physics::SphereCollider* c = e.tryGet<physics::SphereCollider>()) radius = c->radius * scale;
            if (const physics::CapsuleCollider* c = e.tryGet<physics::CapsuleCollider>()) radius = c->radius * scale * 1.2f;
            if (const physics::BoxCollider* c = e.tryGet<physics::BoxCollider>()) {
                radius = 0.5f * scale * std::max(c->size.x, c->size.z);
            }
            radius = std::clamp(radius, 0.15f, 5.0f);
            const core::Vec3 p = e.worldPosition();
            if (core::length(p - eye) > 120.0f) return;
            spheres.push_back(core::Vec4{p.x, p.y, p.z, radius});
        });
        std::sort(spheres.begin(), spheres.end(), [&](const core::Vec4& a, const core::Vec4& b) {
            const core::Vec3 da{a.x - eye.x, a.y - eye.y, a.z - eye.z};
            const core::Vec3 db{b.x - eye.x, b.y - eye.y, b.z - eye.z};
            return core::dot(da, da) < core::dot(db, db);
        });
        if (spheres.size() > 32) spheres.resize(32);
        renderer.setGrassInteractors(spheres);
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
    // Modelos que terminaron de leer los hilos de fondo (streaming).
    assets_.pollLoads();
    renderer.setModelStreaming(g_gpu_residency.get(), g_min_pixels.get(), g_idle_seconds.get());
    // Materiales guardados desde el editor: factores en vivo o variantes rehechas.
    applyMaterialChanges(scene, renderer);
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
        // Una tela se dibuja aunque no tenga Mesh Renderer (con su color).
        static const MeshRenderer kClothRenderer{};
        if (renderer_component == nullptr && (e.has<physics::Cloth>() || e.has<physics::SoftBody>())) {
            renderer_component = &kClothRenderer;
        }
        if (renderer_component == nullptr || !renderer_component->visible ||
            !e.activeInHierarchy()) {
            return;
        }
        // Ya va dentro del lote estatico de la escena (juego exportado).
        if (const EntityInfo* info = e.tryGet<EntityInfo>(); info != nullptr && info->static_batched) {
            return;
        }
        // Una tela o una malla creada por codigo mandan sobre el modelo del asset.
        const physics::Cloth* cloth = e.tryGet<physics::Cloth>();
        const physics::SoftBody* soft = cloth == nullptr ? e.tryGet<physics::SoftBody>() : nullptr;
        std::optional<std::uint32_t> model =
            cloth != nullptr ? resolveClothModel(e, *cloth, scene, renderer)
            : soft != nullptr ? resolveSoftBodyModel(e, *soft, scene, renderer)
            : renderer_component->mesh
                ? resolveRuntimeMesh(renderer_component->mesh, scene, renderer)
                : resolveModel(renderer_component->model, renderer_component->part, scene, added);
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
        bool posed = false;  // la pose de este frame ya se evaluo
        bool inertial_tracked = false;  // pose de animacion: la inercializacion la sigue
        // Motion Matching (RenderSyncMotion.cpp): si esta activo y tiene base,
        // manda sobre el Animator.
        if (anim::MotionMatching* mm = e.tryGet<anim::MotionMatching>();
            mm != nullptr && mm->enabled && mm->database.valid()) {
            const MotionStep step = updateMotionMatching(e, *mm, *model, data, state, scene, delta_seconds);
            if (step == MotionStep::Waiting) return;
            if (step == MotionStep::Posed) {
                animating = true;
                posed = true;
                inertial_tracked = true;
            }
        }
        // Un modelo sin clips propios tambien anima con un controlador (sus
        // clips son .cranim sueltos: el personaje de Mixamo y su pack).
        if (Animator* animator = posed ? nullptr : e.tryGet<Animator>();
            animator != nullptr && (!data.animations.empty() || animator->controller.valid())) {
            animating = true;
            int clip = animator->clip;
            bool loop = animator->loop;
            float speed = animator->speed;
            bool restart = false;
            bool controlled = false;  // la pose ya la hizo el controlador

            // Con controlador: la maquina de estados elige el clip.
            const std::shared_ptr<const AnimatorController> controller =
                animator->controller.valid() ? animatorController(animator->controller.uuid) : nullptr;
            if (controller && !controller->states.empty()) {
                prefetchClips(*model, animator->controller.uuid, *controller, scene);
                // El clip del estado actual aun se esta leyendo (streaming): este
                // frame no se dibuja (mejor que verlo un instante en pose T).
                {
                    const int current = std::clamp(animator->runtime.state, 0,
                                                   static_cast<int>(controller->states.size()) - 1);
                    const AnimatorState& now = controller->states[static_cast<std::size_t>(current)];
                    bool waiting = now.clip.valid() && clipPending(*model, now.clip.uuid);
                    for (const BlendTreeChild& child : now.children) {
                        waiting = waiting || (child.clip.valid() && clipPending(*model, child.clip.uuid));
                    }
                    if (waiting) {
                        // Mira si ya llegaron (y se quedan en el modelo).
                        if (now.clip.valid()) externalClip(*model, now.clip.uuid, scene);
                        for (const BlendTreeChild& child : now.children) {
                            if (child.clip.valid()) externalClip(*model, child.clip.uuid, scene);
                        }
                        waiting = now.clip.valid() && clipPending(*model, now.clip.uuid);
                        for (const BlendTreeChild& child : now.children) {
                            waiting = waiting || (child.clip.valid() && clipPending(*model, child.clip.uuid));
                        }
                    }
                    if (waiting) return;
                }
                // La maquina de estados elige que suena: un clip o un Blend
                // Tree (varios clips con pesos y el ciclo sincronizado), con
                // fundido entre el estado viejo y el nuevo.
                AnimatorRuntime& runtime = animator->runtime;
                const auto resolveClip = [&](const std::string& name, const assets::AssetRef& ref) {
                    if (ref.valid()) return externalClip(*model, ref.uuid, scene);
                    for (std::size_t i = 0; i < data.animations.size(); ++i) {
                        if (data.animations[i].name == name) return static_cast<int>(i);
                    }
                    return -1;
                };
                // Muestras de un estado en su fase (0..1) con un peso; devuelve la
                // duracion del ciclo (la media de sus clips segun sus pesos).
                const auto stateSamples = [&](int index, float phase, float weight, std::vector<anim::ClipSample>* out) {
                    if (index < 0 || index >= static_cast<int>(controller->states.size())) return 0.0f;
                    const AnimatorState& st = controller->states[static_cast<std::size_t>(index)];
                    if (!st.isBlendTree()) {
                        const int c = resolveClip(st.clip_name, st.clip);
                        const float d = c >= 0 ? data.animations[static_cast<std::size_t>(c)].duration : 0.0f;
                        if (out != nullptr) out->push_back({c, phase * d, weight});
                        return d;
                    }
                    const float px = animatorParameterValue(*controller, runtime, st.blend_parameter);
                    const float py = animatorParameterValue(*controller, runtime, st.blend_parameter_y);
                    const std::vector<float> weights = blendTreeWeights(st, px, py);
                    float cycle = 0.0f, used = 0.0f;
                    const std::size_t first = out != nullptr ? out->size() : 0;
                    for (std::size_t k = 0; k < st.children.size(); ++k) {
                        if (weights[k] <= 1e-4f) continue;
                        const BlendTreeChild& child = st.children[k];
                        const int c = resolveClip(child.clip_name, child.clip);
                        if (c < 0) continue;  // sin clip: no cuenta (se reparte entre los demas)
                        const float d = data.animations[static_cast<std::size_t>(c)].duration /
                                        std::max(std::abs(child.speed), 0.01f);
                        cycle += weights[k] * d;
                        used += weights[k];
                        if (out != nullptr) {
                            // Velocidad negativa: el clip va hacia atras (andar de espaldas).
                            const float t = child.speed < 0.0f ? 1.0f - phase : phase;
                            out->push_back({c, t * data.animations[static_cast<std::size_t>(c)].duration, weights[k]});
                        }
                    }
                    if (out != nullptr && used > 1e-6f) {
                        for (std::size_t k = first; k < out->size(); ++k) (*out)[k].weight *= weight / used;
                    }
                    return used > 1e-6f ? cycle / used : 0.0f;
                };
                const auto advance = [&](int index, float& phase, float dt) {
                    const float cycle = stateSamples(index, phase, 0.0f, nullptr);
                    if (cycle <= 0.0f) return;
                    const AnimatorState& st = controller->states[static_cast<std::size_t>(index)];
                    phase += dt * animator->speed * st.speed / cycle;
                    phase = st.loop ? phase - std::floor(phase) : std::clamp(phase, 0.0f, 1.0f);
                };

                if (!runtime.replay_control) {  // repeticion: el estado lo pone ella
                    stepAnimatorController(*controller, runtime, stateSamples(runtime.state, state.phase, 0.0f, nullptr));
                }
                runtime.state = std::clamp(runtime.state, 0, static_cast<int>(controller->states.size()) - 1);
                float inertial_duration = 0.0f;
                if (runtime.state != state.controller_state) {
                    // Fundido o inercializacion desde el estado que sonaba (si
                    // la transicion lo pide).
                    const AnimatorTransition* transition =
                        state.controller_state >= 0 && runtime.last_transition >= 0 &&
                                runtime.last_transition < static_cast<int>(controller->transitions.size())
                            ? &controller->transitions[static_cast<std::size_t>(runtime.last_transition)]
                            : nullptr;
                    const float fade = transition != nullptr ? transition->duration : 0.0f;
                    const float old_phase = state.phase;
                    if (fade > 0.0f && transition->inertial) {
                        // Se deja de mezclar: el desfase con lo que se veia se apaga solo.
                        state.fade_state = -1;
                        inertial_duration = fade;
                    } else if (fade > 0.0f) {
                        state.fade_state = state.controller_state;
                        state.fade_phase = state.phase;
                        state.fade_time = 0.0f;
                        state.fade_duration = fade;
                    } else {
                        state.fade_state = -1;
                    }
                    state.controller_state = runtime.state;
                    state.phase = transition != nullptr && transition->sync_phase ? old_phase : 0.0f;
                    if (!runtime.replay_control) animator->time = 0.0f;  // repeticion: su tiempo manda
                }
                const AnimatorState& st = controller->states[static_cast<std::size_t>(runtime.state)];
                const float cycle = stateSamples(runtime.state, state.phase, 0.0f, nullptr);
                if (animator->playing) {
                    runtime.state_time += delta_seconds * std::abs(animator->speed * st.speed);
                    advance(runtime.state, state.phase, delta_seconds);
                    if (state.fade_state >= 0) {
                        advance(state.fade_state, state.fade_phase, delta_seconds);
                        state.fade_time += delta_seconds;
                        if (state.fade_time >= state.fade_duration) state.fade_state = -1;
                    }
                    animator->time = state.phase * cycle;
                } else if (cycle > 0.0f) {
                    // Pausado: el tiempo del Inspector manda (arrastrarlo = scrub).
                    state.phase = std::clamp(animator->time / cycle, 0.0f, 1.0f);
                }
                std::vector<anim::ClipSample> samples;
                float fresh = 1.0f;
                if (state.fade_state >= 0 && state.fade_duration > 0.0f) {
                    const float t = std::clamp(state.fade_time / state.fade_duration, 0.0f, 1.0f);
                    fresh = t * t * (3.0f - 2.0f * t);
                    stateSamples(state.fade_state, state.fade_phase, 1.0f - fresh, &samples);
                }
                stateSamples(runtime.state, state.phase, fresh, &samples);
                state.animator.evaluateBlend(samples);
                if (inertial_duration > 0.0f && state.inertial.hasHistory() && animator->playing) {
                    // La pose nueva un instante antes: su velocidad al empezar.
                    float before_phase = state.phase;
                    if (cycle > 0.0f) {
                        before_phase -= delta_seconds * animator->speed * st.speed / cycle;
                        before_phase = st.loop ? before_phase - std::floor(before_phase)
                                               : std::clamp(before_phase, 0.0f, 1.0f);
                    }
                    const std::vector<Mat4> now = state.animator.locals();
                    std::vector<anim::ClipSample> earlier;
                    if (state.fade_state >= 0) {
                        stateSamples(state.fade_state, state.fade_phase, 1.0f - fresh, &earlier);
                    }
                    stateSamples(runtime.state, before_phase, fresh, &earlier);
                    state.animator.evaluateBlend(earlier);
                    const std::vector<Mat4> before = state.animator.locals();
                    state.animator.evaluateBlend(samples);
                    state.inertial.start(now, before, inertial_duration, delta_seconds);
                }
                inertial_tracked = animator->playing;
                state.clip = -3;  // sin controlador otra vez: vuelve a elegir clip
                posed = true;
                controlled = true;
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
            // Sin controlador: un clip (con controlador la pose ya esta hecha).
            if (!controlled) {
                // Otro clip (el juego lo cambia): transicion inercial.
                const bool switched = clip != state.clip && state.clip >= -1 && state.clip != -3;
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
                    posed = true;
                    if (switched && animator->blend_time > 0.0f && state.inertial.hasHistory()) {
                        const std::vector<Mat4> now = state.animator.locals();
                        state.animator.setTime(animator->time - delta_seconds * speed);
                        state.animator.evaluate();
                        const std::vector<Mat4> before = state.animator.locals();
                        state.animator.setTime(animator->time);
                        state.animator.evaluate();
                        state.inertial.start(now, before, animator->blend_time, delta_seconds);
                    }
                    inertial_tracked = true;
                } else if (std::abs(state.animator.time() - animator->time) > 1e-5f) {
                    // Pausado: el tiempo del Inspector manda (arrastrarlo = scrub).
                    state.animator.setTime(animator->time);
                    state.animator.evaluate();
                    posed = true;
                }
            }
        }
        // Transicion inercial en curso: el desfase se suma a la pose animada
        // (antes del IK, que sigue apoyando los pies donde toca).
        if (inertial_tracked && data.nodes.size() > 1) {
            const bool was_active = state.inertial.active();
            state.inertial.apply(state.animator.locals(), delta_seconds);
            if (was_active) {
                ik::recomputeGlobals(ik::Pose{&data.nodes, &state.animator.locals(), &state.animator.globals()});
                state.animator.updateBones();
            }
        } else if (!animating) {
            state.inertial.reset();
        }
        // Esqueleto sobre la pose de este frame (sin animar o en pausa se
        // parte de la pose limpia: nada se acumula). Los componentes pueden
        // estar en la pieza o en un antepasado (la raiz del modelo): entonces
        // la primera pieza calcula la pose y las demas la copian.
        // Orden: animacion -> huesos movidos y sockets -> procedural (capas y
        // patas) -> IK -> muelles -> phys bones -> ragdoll.
        const RigComponents rig = gatherRig(e);
        const ProceduralAnimation* proc = rig.proc;
        const bool use_ik = rig.ik != nullptr && rig.ik->enabled;
        const bool use_proc = proc != nullptr && proc->enabled;
        // Solo esqueletos de verdad (los modelos estaticos tienen un hueso).
        if (rig.any() && data.nodes.size() > 1 && data.bones.size() > 1) {
            if (copySharedPose(rig, state.animator, data)) {
                animating = true;
            } else {
                if (!posed) state.animator.evaluate();
                if (rig.skeleton != nullptr) applyBoneOverrides(*rig.skeleton, state.animator, data);
                if (rig.drive) applyDriveSockets(world, e, state.animator, data);
                if (use_proc) applyProceduralBefore(world, e, *proc, state.animator, data, *model, delta_seconds);
                if (use_ik) applyInverseKinematics(world, e, *rig.ik, state.animator, data, *model, state.ik, delta_seconds);
                if (use_proc) applyProceduralSprings(e, *proc, state.animator, data, *model, delta_seconds);
                if (rig.physbones != nullptr && rig.physbones->enabled) {
                    applyPhysBones(world, e, *rig.physbones, state.animator, data, delta_seconds);
                }
                if (rig.ragdoll != nullptr) applyRagdoll(e, *rig.ragdoll, state.animator, data, delta_seconds);
                state.animator.updateBones();
                storeSharedPose(rig, state.animator);
                animating = true;
            }
        }
        if (proc == nullptr) procedural_.erase(e.handle());
        if (rig.physbones == nullptr) physbones_.erase(e.handle());
        // Tela simulada: cada hueso es una particula (en la pose de reposo si
        // no hay simulacion, fuera de Play).
        const std::vector<Vec3>* cloth_positions = nullptr;
        if (cloth != nullptr && cloth->runtime.ptr && cloth->runtime.ptr->simulated &&
            cloth->runtime.ptr->positions.size() == static_cast<std::size_t>(cloth->particleCount()) &&
            state.animator.globals().size() == cloth->runtime.ptr->positions.size()) {
            cloth_positions = &cloth->runtime.ptr->positions;
            physics::clothBoneGlobals(*cloth, e.worldMatrix(), *cloth_positions, state.animator.globals());
            state.animator.updateBones();
            animating = true;
        }
        float soft_margin = 0.0f;
        if (soft != nullptr && soft->runtime.ptr && soft->runtime.ptr->simulated &&
            state.animator.globals().size() == soft->runtime.ptr->positions.size()) {
            const auto slot = cloth_models_.find(e.handle());
            if (slot != cloth_models_.end() && slot->second.soft_mesh) {
                cloth_positions = &soft->runtime.ptr->positions;
                physics::softBodyBoneGlobals(*slot->second.soft_mesh, e.worldMatrix(), *cloth_positions,
                                             state.animator.globals());
                state.animator.updateBones();
                animating = true;
                soft_margin = soft->thickness;
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
        if (cloth_positions != nullptr && !cloth_positions->empty()) {
            // La tela se aleja de su sitio (cae, ondea): la caja de sus particulas.
            Vec3 lo = cloth_positions->front(), hi = lo;
            for (const Vec3& p : *cloth_positions) {
                lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
            actor.bounds_center = (lo + hi) * 0.5f;
            actor.bounds_radius = core::length(hi - lo) * 0.5f + (cloth != nullptr ? cloth->thickness : soft_margin) + 0.05f;
        }
        actor.cast_shadows = renderer_component->cast_shadows != ShadowCasting::Off;
        actor.shadows_only = renderer_component->cast_shadows == ShadowCasting::ShadowsOnly;

        actor_entities_.push_back(e.handle());
        actor_models_.push_back(*model);
        ++count;
    });
    actors.resize(count);
    // Bone Sockets: lo enganchado a un hueso va con el (y los que mueven
    // huesos se apuntan para el frame que viene).
    updateSockets(world, scene);

    // El indice entidad -> actor solo se rehace si la lista cambio.
    if (actor_entities_ != previous_entities_) {
        entity_actor_.clear();
        for (std::size_t i = 0; i < actor_entities_.size(); ++i) {
            entity_actor_[actor_entities_[i]] = static_cast<std::uint32_t>(i);
        }
    }

    // Las mallas de codigo y las telas que ya no existen dejan su hueco libre.
    releaseRuntimeMeshes();
    releaseClothModels();

    // Olvida los animadores de lo que ya no se dibuja.
    for (auto it = animations_.begin(); it != animations_.end();) {
        if (it->second.seen == frame_) {
            ++it;
            continue;
        }
        physbones_.erase(it->first);
        ragdoll_tips_.erase(it->first);
        drive_sockets_.erase(it->first);
        it = animations_.erase(it);
    }
    for (auto it = shared_poses_.begin(); it != shared_poses_.end();) {
        it = it->second.frame + 2 < frame_ ? shared_poses_.erase(it) : std::next(it);
    }

    // Piezas nuevas a la GPU, en orden y poco a poco: como mucho unos
    // milisegundos por frame (render.streaming.UploadMs), y al menos una. Antes
    // se subia la escena ENTERA (todas las mallas y texturas, parando la GPU)
    // cada vez que aparecia un modelo: instanciar algo en el juego daba un
    // tiron tanto mayor cuanto mas grande la escena. Lo que aun no se ha subido
    // simplemente no se dibuja todavia.
    (void)added;
    const auto upload_start = std::chrono::steady_clock::now();
    const float budget_ms = g_upload_ms.get();
    while (renderer.uploadedModelCount() < scene.models().size()) {
        renderer.uploadModel(scene, renderer.uploadedModelCount());
        const float spent =
            std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - upload_start).count();
        if (spent >= budget_ms) break;
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
    struct Volume {
        const PostProcessing* post;
        Entity entity;
    };
    std::vector<Volume> volumes;

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
                    // Apagada: no ocupa hueco (el limite es por frame).
                    if (light->intensity > 0.0f) {
                        scene::PointLight p{};
                        p.position = e.worldPosition();
                        p.color = light->color;
                        p.intensity = light->intensity;
                        p.range = light->range;
                        p.cast_shadows = light->cast_shadows;
                        p.shadow_strength = std::clamp(light->shadow_strength, 0.0f, 1.0f);
                        p.source_radius = std::clamp(light->source_radius, 0.0f, 2.0f);
                        lights.points.push_back(p);
                    }
                    break;
                case LightType::Spot:
                    if (light->intensity > 0.0f) {
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
                        s.shadow_strength = std::clamp(light->shadow_strength, 0.0f, 1.0f);
                        s.source_radius = std::clamp(light->source_radius, 0.0f, 2.0f);
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
            volumes.push_back(Volume{p, e});
        }
    });

    // Fuego: zonas (mapas de calor, quemado y humo) para el renderizador y
    // luces que parpadean donde mas arde.
    {
        static const auto fire_epoch = std::chrono::steady_clock::now();
        const float fire_seconds =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - fire_epoch).count();
        std::vector<gfx::FireZone> fire_zones;
        std::vector<scene::PointLight> fire_lights;
        fire::collectFireRender(world, fire_seconds, fire_zones, fire_lights);
        renderer.setFireZones(fire_zones);
        lights.points.insert(lights.points.end(), fire_lights.begin(), fire_lights.end());
    }

    // Mas luces que huecos (32 puntuales, 8 focos): se quedan las que mas
    // cuentan para la camara (cerca de ella o con mucho alcance), no las
    // primeras de la Jerarquia. Antes, en un mapa grande, las salas del final
    // se quedaban a oscuras aunque fueran las unicas a la vista.
    {
        const Vec3 eye = scene.camera().position();
        const auto keepNearest = [&](auto& list, std::size_t max) {
            if (list.size() <= max) return;
            std::stable_sort(list.begin(), list.end(), [&](const auto& a, const auto& b) {
                return core::length(a.position - eye) - a.range < core::length(b.position - eye) - b.range;
            });
            list.resize(max);
        };
        keepNearest(lights.points, scene::kMaxPointLights);
        keepNearest(lights.spots, scene::kMaxSpotLights);
    }

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
    if (sky != nullptr) {
        gfx::CloudSettings clouds;
        clouds.coverage = std::clamp(sky->cloud_coverage, 0.0f, 1.0f);
        clouds.density = std::max(sky->cloud_density, 0.0f);
        clouds.type = std::clamp(sky->cloud_type, 0.0f, 1.0f);
        clouds.bottom = std::max(sky->cloud_height, 50.0f);
        clouds.thickness = std::max(sky->cloud_thickness, 100.0f);
        clouds.wind_speed = std::max(sky->wind_speed, 0.0f);
        clouds.wind_direction = sky->wind_direction;
        clouds.shadows = sky->cloud_shadows;
        clouds.shadow_strength = std::clamp(sky->cloud_shadow_strength, 0.0f, 1.0f);
        renderer.setCloudSettings(clouds);
    } else {
        renderer.setCloudSettings(gfx::CloudSettings{});
    }

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
        renderer.setSunShadowStrength(std::clamp(light.shadow_strength, 0.0f, 1.0f));
    } else if (hdr_active) {
        renderer.setSunShadowsEnabled(true);
        renderer.setSunShadowStrength(1.0f);
        scene.setFixedSun(renderer.environmentSunDirection());
    } else {
        renderer.setSunShadowsEnabled(true);
        renderer.setSunShadowStrength(1.0f);
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
    // Volumenes como en Unity: de menor a mayor prioridad (a igual
    // prioridad, los globales primero), cada uno lleva el resultado hacia
    // sus valores segun su peso y lo cerca que esta la camara.
    std::stable_sort(volumes.begin(), volumes.end(), [](const Volume& a, const Volume& b) {
        if (a.post->priority != b.post->priority) return a.post->priority < b.post->priority;
        return a.post->isGlobal() && !b.post->isGlobal();
    });
    gfx::PostProcessSettings blended{};
    const core::Vec3 eye = scene.camera().position();
    for (const Volume& v : volumes) {
        const float t = v.post->influence(v.entity.worldMatrix(), eye);
        blendPostProcess(blended, v.post->settings, t, v.post->isGlobal() ? kPostAll : v.post->overrides);
    }
    renderer.setPostProcess(blended);

    // --- Ambiente (Environment): clima, hora, viento, lluvia, nieve, rayos ---
    // Al final: conduce lo de arriba (nubes, sol, humedad, niebla).
    environment_frame_ = environment::applyEnvironment(
        world, scene, renderer, environment_delta_, blended,
        directional.valid() ? directional.tryGet<Light>() : nullptr, hdr_active,
        global_weather != nullptr ? global_weather->wetness : 0.0f, global_weather != nullptr ? global_weather->puddles : 0.0f,
        global_weather != nullptr && global_weather->rain);
}

void RenderSync::syncCamera(World& world, scene::Scene& scene) {
    Entity main;
    world.forEachDepthFirst([&](Entity e) {
        const Camera* camera = e.tryGet<Camera>();
        // Las que dibujan en una Render Texture no son la del juego.
        if (camera != nullptr && camera->target_texture.valid()) return;
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
    view.setOrthographic(camera.orthographic, camera.ortho_size);
    // Plano cercano y lejano del componente (lo que queda mas alla del lejano
    // no se dibuja: se ve el cielo). Antes no se aplicaban y la camara del
    // juego usaba siempre 0.1 - 500 m.
    view.setClipPlanes(camera.near_plane, camera.far_plane);
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
