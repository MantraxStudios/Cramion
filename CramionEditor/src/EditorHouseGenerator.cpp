// Generador de casas y pueblos medievales (Ventana > Generador de casas y
// pueblos).
//
// Casas: cabanas de troncos, de tablas, casitas de piedra, casas de campo,
// casas de entramado y cabanas de paja procedurales, con interior (hogar,
// mesa, camas, escalera, alacenas...). Cada casa es un modelo (.crdata en
// Assets/Casas/Modelos) con piezas "Casa" (con MeshCollider), "Puerta"
// (bisagra en su origen, BoxCollider ajustado a su malla) y, en el molino,
// "Aspas"; 14 materiales .crmat compartidos (Assets/Casas/Materiales) con
// texturas PBR procedurales y relieve (Assets/Casas/Texturas, se crean una
// vez). Cada fuego (hogar, fragua) lleva una luz calida.
//
// Pueblos: aldea, pueblo o ciudad amurallada sobre el terreno
// (asset::layoutSettlement): calles y plaza allanadas y pintadas, iglesia,
// taberna, herrerias, tiendas, graneros, mercado, pozo, torre del homenaje,
// murallas con puertas, molino, campos con vallas y objetos.
//
// Aplanado: el componente TerrainFlatten deja plano el terreno bajo el AABB
// de cada edificio (con talud suave) para que no lo tape; si se mueve en el
// editor se rehace al soltarlo y el sitio de antes vuelve a como estaba.

#include "EditorApp.h"

#include "Dialogs.h"

#include <CramionCore/asset/Importer.h>
#include <CramionCore/asset/MaterialAsset.h>
#include <CramionCore/asset/ModelMaterials.h>
#include <CramionCore/ecs/MathUtil.h>
#include <CramionCore/ecs/ModelInstantiation.h>
#include <CramionCore/foliage/Foliage.h>
#include <CramionCore/physics/PhysicsComponents.h>
#include <CramionCore/scripting/Scripting.h>
#include <CramionCore/terrain/TerrainTools.h>
#include <CramionCore/water/Water.h>
#include <CramionFX/asset/HouseGenerator.h>
#include <CramionFX/asset/ImageFile.h>
#include <CramionFX/asset/MedievalBuildings.h>
#include <CramionFX/asset/SettlementGenerator.h>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <thread>

namespace cramion::editor {

using core::Vec3;

namespace {

constexpr const char* kHouseTextures = "Casas/Texturas";
constexpr const char* kHouseMaterials = "Casas/Materiales";
constexpr const char* kHouseModels = "Casas/Modelos";
constexpr const char* kSailsScript = "Casas/Scripts/Aspas.lua";
constexpr const char* kHouseTextureVersion = "cramion-house-textures-4";
constexpr const char* kVillageName = "Aldea";

// Mitad de resolucion (rugosidad, oclusion y altura no necesitan mas).
asset::ImageRgba8 half(const asset::ImageRgba8& in) {
    asset::ImageRgba8 out;
    out.width = std::max(in.width / 2, 1U);
    out.height = std::max(in.height / 2, 1U);
    out.pixels.resize(static_cast<std::size_t>(out.width) * out.height * 4);
    for (std::uint32_t y = 0; y < out.height; ++y) {
        for (std::uint32_t x = 0; x < out.width; ++x) {
            for (int c = 0; c < 4; ++c) {
                int sum = 0;
                for (std::uint32_t dy = 0; dy < 2; ++dy) {
                    for (std::uint32_t dx = 0; dx < 2; ++dx) {
                        sum += in.pixels[((static_cast<std::size_t>(y) * 2 + dy) * in.width + x * 2 + dx) * 4 +
                                         static_cast<std::size_t>(c)];
                    }
                }
                out.pixels[(static_cast<std::size_t>(y) * out.width + x) * 4 + static_cast<std::size_t>(c)] =
                    static_cast<std::uint8_t>((sum + 2) / 4);
            }
        }
    }
    return out;
}

std::string hashText(const std::string& text) {
    std::uint32_t h = 2166136261U;
    for (const char c : text) h = (h ^ static_cast<std::uint8_t>(c)) * 16777619U;
    char out[16];
    std::snprintf(out, sizeof(out), "%08x", h);
    return out;
}

std::string settingsKey(const asset::HouseSettings& s) {
    char text[320];
    std::snprintf(text, sizeof(text), "%d-%u-%.2f-%.2f-%d-%.2f-%.1f-%.2f-%d%d%d-%d-i%d-u%d-j%.2f", static_cast<int>(s.style), s.seed,
                  s.width, s.depth, s.floors, s.wall_height, s.roof_pitch, s.roof_overhang, s.porch ? 1 : 0, s.chimney ? 1 : 0,
                  s.shutters ? 1 : 0, s.windows, s.interior ? 1 : 0, static_cast<int>(s.use), s.jetty);
    return hashText(text);
}

// Capa del terreno por su nombre (sin mayusculas, que contenga alguno).
int findLayer(const terrain::Terrain& t, std::initializer_list<const char*> names) {
    for (std::size_t i = 0; i < t.layers.size() && i < static_cast<std::size_t>(terrain::kMaxLayers); ++i) {
        std::string lower = t.layers[i].name;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const char* n : names) {
            if (lower.find(n) != std::string::npos) return static_cast<int>(i);
        }
    }
    return -1;
}

// Media movil de las alturas de una polilinea (caminos sin baches).
void smoothHeights(std::vector<Vec3>& pts, int passes, int radius) {
    for (int p = 0; p < passes; ++p) {
        const std::vector<Vec3> copy = pts;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            float sum = 0.0f;
            int count = 0;
            for (int k = -radius; k <= radius; ++k) {
                const long j = static_cast<long>(i) + k;
                if (j < 0 || j >= static_cast<long>(copy.size())) continue;
                sum += copy[static_cast<std::size_t>(j)].y;
                ++count;
            }
            pts[i].y = sum / static_cast<float>(std::max(count, 1));
        }
    }
}

// Puntos cada `step` metros a lo largo de un tramo (incluye los extremos).
std::vector<Vec3> subdivide(const Vec3& a, const Vec3& b, float step) {
    const float len = std::hypot(b.x - a.x, b.z - a.z);
    const int n = std::max(1, static_cast<int>(std::ceil(len / step)));
    std::vector<Vec3> out;
    for (int i = 0; i <= n; ++i) out.push_back(a + (b - a) * (static_cast<float>(i) / static_cast<float>(n)));
    return out;
}

// Caja (xz) y fuegos de un modelo ya construido -> huella del trazado.
void footprintOf(const asset::HouseModel& m, core::Vec2& half_extents, core::Vec2& offset) {
    half_extents = core::Vec2{(m.bounds_max.x - m.bounds_min.x) * 0.5f, (m.bounds_max.z - m.bounds_min.z) * 0.5f};
    offset = core::Vec2{(m.bounds_min.x + m.bounds_max.x) * 0.5f, (m.bounds_min.z + m.bounds_max.z) * 0.5f};
}

std::uint64_t flattenSignature(const terrain::TerrainFlatten& f) {
    std::uint64_t h = 1469598103934665603ULL;
    const float values[] = {f.enabled ? 1.0f : 0.0f, f.margin, f.blend, f.ground_offset, f.mesh_bottom ? 1.0f : 0.0f,
                            static_cast<float>(f.paint_layer)};
    const auto* p = reinterpret_cast<const unsigned char*>(values);
    for (std::size_t i = 0; i < sizeof(values); ++i) h = (h ^ p[i]) * 1099511628211ULL;
    return h;
}

// Que el terreno no quede por encima de `max_height` bajo una huella (y la
// franja de una celda alrededor que usa el muestreo bilineal): el talud de un
// vecino mas alto no puede tapar el borde de una casa ya puesta. Bajar no
// importa (el zocalo llega 1.5 m por debajo del suelo).
bool clampUnder(terrain::TerrainData& data, const terrain::Terrain& t, const Vec3& origin, const terrain::Footprint& fp,
                float max_height) {
    if (data.resolution() < 3) return false;
    const int res = static_cast<int>(data.resolution());
    const float cell = t.size / static_cast<float>(res - 1);
    const float cover = cell * 1.5f;
    const float reach = std::hypot(fp.half.x, fp.half.y) + cover;
    const float max_n = (max_height - origin.y) / std::max(t.height, 1e-3f);
    const float a = fp.yaw_degrees * core::kPi / 180.0f;
    const float c = std::cos(a);
    const float s = std::sin(a);
    const int x0 = std::clamp(static_cast<int>(std::floor((fp.center.x - reach - origin.x) / cell)), 0, res - 1);
    const int x1 = std::clamp(static_cast<int>(std::ceil((fp.center.x + reach - origin.x) / cell)), 0, res - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor((fp.center.z - reach - origin.z) / cell)), 0, res - 1);
    const int y1 = std::clamp(static_cast<int>(std::ceil((fp.center.z + reach - origin.z) / cell)), 0, res - 1);
    bool changed = false;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float dx = origin.x + static_cast<float>(x) * cell - fp.center.x;
            const float dz = origin.z + static_cast<float>(y) * cell - fp.center.z;
            const float lx = c * dx - s * dz;
            const float lz = s * dx + c * dz;
            const float d = std::hypot(std::max(std::abs(lx) - fp.half.x, 0.0f), std::max(std::abs(lz) - fp.half.y, 0.0f));
            if (d > cover) continue;
            float& h = data.heights()[static_cast<std::size_t>(y) * res + x];
            if (h > max_n) {
                h = max_n;
                changed = true;
            }
        }
    }
    if (changed) data.markHeights(x0, y0, x1, y1);
    return changed;
}

// Misma pose en coordenadas absolutas: mover el origen flotante del mundo
// desplaza todas las entidades (y el terreno) sin que nada se haya movido.
bool samePose(const core::Mat4& a, const ecs::DVec3& ao, const core::Mat4& b, const ecs::DVec3& bo) {
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) {
            if (std::abs(a.m[c][r] - b.m[c][r]) > 1e-4f) return false;
        }
    }
    const double da[3] = {a.m[3][0] + ao.x, a.m[3][1] + ao.y, a.m[3][2] + ao.z};
    const double db[3] = {b.m[3][0] + bo.x, b.m[3][1] + bo.y, b.m[3][2] + bo.z};
    for (int i = 0; i < 3; ++i) {
        if (std::abs(da[i] - db[i]) > 1e-3) return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Materiales y modelos
// ---------------------------------------------------------------------------

bool EditorApp::ensureHouseMaterials(assets::ModelMaterialMap& map, std::string* error) {
    const std::filesystem::path root = project_.assetsFolder();
    const std::filesystem::path textures = root / dialogs::fromUtf8(kHouseTextures);
    const std::filesystem::path materials = root / dialogs::fromUtf8(kHouseMaterials);
    std::error_code ec;
    std::filesystem::create_directories(textures, ec);
    std::filesystem::create_directories(materials, ec);

    // --- Texturas (una vez por proyecto; se rehacen si cambia la version) ---
    std::string version;
    if (std::ifstream in(textures / "version.txt"); in) std::getline(in, version);
    if (version != kHouseTextureVersion) {
        std::cout << "[Casas] Creando las texturas de las casas..." << std::endl;
        std::vector<std::thread> threads;
        std::atomic<bool> ok{true};
        for (int m = 0; m < asset::kHouseMaterialCount; ++m) {
            const std::string base = asset::houseTextureName(m);
            if (base.empty()) continue;
            asset::HouseTextureSet set;
            if (!asset::generateHouseTexture(m, 1024, 7, set)) continue;
            // Guardar en paralelo (comprimir PNG es lo lento).
            threads.emplace_back([&ok, textures, base, set = std::move(set)]() {
                const auto save = [&](const char* suffix, const asset::ImageRgba8& img) {
                    if (!asset::saveImagePng(textures / dialogs::fromUtf8(base + suffix), img)) ok = false;
                };
                save("_Color.png", set.color);
                save("_Normal.png", set.normal);
                save("_Roughness.png", half(set.roughness));
                save("_AO.png", half(set.occlusion));
                save("_Height.png", half(set.height));
            });
        }
        for (std::thread& t : threads) t.join();
        if (!ok) {
            if (error) *error = "no se pudieron guardar las texturas en Assets/" + std::string(kHouseTextures);
            return false;
        }
        std::ofstream(textures / "version.txt", std::ios::binary | std::ios::trunc) << kHouseTextureVersion;
    }

    // --- Materiales (.crmat): se crean si faltan; si ya estan, se respetan ---
    map.clear();
    for (int m = 0; m < asset::kHouseMaterialCount; ++m) {
        const std::string name = asset::houseMaterialName(m);
        const std::filesystem::path file = materials / dialogs::fromUtf8(name + ".crmat");
        assets::MaterialAsset mat;
        bool exists = std::filesystem::exists(file) && assets::loadMaterial(file, mat);
        // El vidrio de antes era opaco (no se veia el interior): si nadie lo
        // cambio, pasa al vidrio transparente de ahora.
        if (exists && m == asset::kHouseGlass && mat.mode == assets::MaterialMode::Opaque &&
            std::abs(mat.base_color.x - 0.03f) < 1e-3f && std::abs(mat.base_color.z - 0.04f) < 1e-3f) {
            exists = false;
        }
        if (!exists) {
            const Uuid keep = mat.uuid;
            mat = assets::MaterialAsset{};
            const std::string tex = asset::houseTextureName(m);
            if (!tex.empty()) {
                const std::string prefix = std::string(kHouseTextures) + "/" + tex;
                mat.albedo = prefix + "_Color.png";
                mat.normal = prefix + "_Normal.png";
                mat.roughness_map = prefix + "_Roughness.png";
                mat.occlusion = prefix + "_AO.png";
                mat.height_map = prefix + "_Height.png";
                mat.roughness = 1.0f;  // el mapa manda
            }
            switch (m) {
                case asset::kHouseLogs: mat.height_scale = 0.012f; break;
                case asset::kHouseLogEnds: mat.height_scale = 0.01f; break;
                case asset::kHousePlanks: mat.height_scale = 0.01f; break;
                case asset::kHouseSiding: mat.height_scale = 0.015f; break;
                case asset::kHouseStone: mat.height_scale = 0.035f; break;
                case asset::kHouseRoof: mat.height_scale = 0.025f; break;
                case asset::kHouseTrim: mat.height_scale = 0.0f; break;
                case asset::kHousePlaster: mat.height_scale = 0.006f; break;
                case asset::kHouseThatch: mat.height_scale = 0.03f; break;
                case asset::kHouseTile: mat.height_scale = 0.03f; break;
                case asset::kHouseCloth: mat.height_scale = 0.0f; break;
                case asset::kHouseBeams: mat.height_scale = 0.01f; break;
                case asset::kHouseGlass:
                    // Vidrio soplado algo verdoso: transparente (se ve el interior).
                    mat.mode = assets::MaterialMode::Transparent;
                    mat.shading = assets::ShadingModel::Transmission;
                    mat.base_color = core::Vec4{0.74f, 0.82f, 0.76f, 0.22f};
                    mat.roughness = 0.06f;
                    mat.reflectance = 0.08f;
                    mat.ior = 1.5f;
                    mat.transmission_thickness = 0.006f;
                    mat.height_scale = 0.0f;
                    break;
                case asset::kHouseIron:
                    mat.base_color = core::Vec4{0.12f, 0.11f, 0.1f, 1.0f};
                    mat.metallic = 1.0f;
                    mat.roughness = 0.55f;
                    mat.height_scale = 0.0f;
                    break;
                default: break;
            }
            if (keep.valid()) mat.uuid = keep;  // el mismo material (las referencias siguen)
            if (!assets::saveMaterial(mat, file, error)) return false;
        }
        map[assets::modelMaterialKey(name, static_cast<std::size_t>(m))] = mat.uuid;
    }
    return true;
}

Uuid EditorApp::writeBuildingModel(const std::string& name, const std::string& key, const asset::HouseModel& model,
                                   std::string* error, bool refresh) {
    assets::ModelMaterialMap map;
    if (!ensureHouseMaterials(map, error)) return {};
    const std::filesystem::path folder = project_.assetsFolder() / dialogs::fromUtf8(kHouseModels);
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::string file_name = name + " " + key + ".crdata";
    const std::filesystem::path file = folder / dialogs::fromUtf8(file_name);

    // El mismo edificio ya hecho: se reutiliza (instancias del mismo modelo).
    const auto find_existing = [&]() -> Uuid {
        if (!database_) return {};
        for (const assets::AssetInfo& info : database_->all()) {
            if (info.type == assets::AssetType::Model && std::filesystem::equivalent(database_->root() / info.path, file, ec)) {
                return info.uuid;
            }
        }
        return {};
    };
    if (std::filesystem::exists(file)) {
        Uuid existing = find_existing();
        if (!existing.valid()) {
            refreshDatabase();
            existing = find_existing();
        }
        if (existing.valid()) {
            assets::saveModelMaterialMap(project_.settingsFolder(), existing, map);
            return existing;
        }
    }

    std::vector<assets::ModelNode> nodes;
    std::vector<asset::ModelData> parts;
    nodes.push_back(assets::ModelNode{name, -1, core::Mat4::identity(), -1});
    nodes.push_back(assets::ModelNode{"Casa", 0, core::Mat4::identity(), 0});
    parts.push_back(model.house);
    if (!model.door.vertices.empty()) {
        nodes.push_back(assets::ModelNode{"Puerta", 0, core::composeTrs(model.door_hinge, core::Quat{}, Vec3{1.0f, 1.0f, 1.0f}),
                                          static_cast<std::int32_t>(parts.size())});
        parts.push_back(model.door);
    }
    for (const asset::HousePart& p : model.parts) {
        nodes.push_back(assets::ModelNode{p.name, 0, core::composeTrs(p.origin, core::Quat{}, Vec3{1.0f, 1.0f, 1.0f}),
                                          static_cast<std::int32_t>(parts.size())});
        parts.push_back(p.model);
    }
    const Uuid uuid = Uuid::generate();
    if (!assets::writeGeneratedModel(file, uuid, name, nodes, parts, error)) return {};
    assets::saveModelMaterialMap(project_.settingsFolder(), uuid, map);
    if (refresh) refreshDatabase();
    std::cout << "[Casas] " << name << ": " << model.triangles << " triangulos, " << dialogs::utf8(file.filename()) << std::endl;
    return uuid;
}

Uuid EditorApp::writeHouseModel(const asset::HouseSettings& settings, std::string* error, asset::HouseModel* out, bool refresh) {
    const asset::HouseModel house = asset::buildHouse(settings);
    if (out != nullptr) *out = house;
    std::string name = asset::houseStyleName(settings.style);
    if (settings.use != asset::HouseUse::Home) name = asset::houseUseName(settings.use);
    return writeBuildingModel(name, settingsKey(settings), house, error, refresh);
}

// ---------------------------------------------------------------------------
// Colocar
// ---------------------------------------------------------------------------

ecs::Entity EditorApp::placeHouse(const Uuid& model_uuid, const Vec3& position, float yaw_degrees, ecs::Entity parent, bool flatten,
                                  const std::vector<Vec3>* lights, bool commit_terrain) {
    const std::shared_ptr<const assets::ModelAsset> model = asset_manager_ ? asset_manager_->loadModel(model_uuid) : nullptr;
    if (!model) return {};
    ecs::Entity root = ecs::instantiateModel(world_, *model, parent);
    applyModelMaterials(root, model_uuid);
    root.setWorldPosition(position);
    root.setLocalEulerDegrees(Vec3{0.0f, yaw_degrees, 0.0f});
    for (std::size_t i = 0; i < root.childCount(); ++i) {
        ecs::Entity child = root.child(i);
        if (child.name() == "Casa") {
            child.add<physics::MeshCollider>();
        } else if (child.name() == "Puerta") {
            // La hoja: caja ajustada a su malla (el origen es la bisagra).
            child.add<physics::BoxCollider>();
            physics_.fitColliderToMesh(child, "BoxCollider");
            if (physics::BoxCollider* box = child.tryGet<physics::BoxCollider>()) box->size.z = std::max(box->size.z, 0.04f);
        } else if (child.name() == "Aspas") {
            // Las aspas giran en Play con un script pequeno.
            const std::filesystem::path script = project_.assetsFolder() / dialogs::fromUtf8(kSailsScript);
            if (!std::filesystem::exists(script)) {
                std::error_code ec;
                std::filesystem::create_directories(script.parent_path(), ec);
                std::ofstream(script, std::ios::binary)
                    << "-- Aspas del molino: giran sin parar alrededor de su eje (Z local).\n"
                       "-- Cambia speed (grados por segundo) en el Inspector.\n"
                       "speed = 24\n\n"
                       "function update(self, dt)\n"
                       "  self.entity:rotate(Vec3.new(0, 0, (self.speed or speed) * dt))\n"
                       "end\n";
            }
            child.add<scripting::Script>().file = kSailsScript;
        }
    }
    // Una luz calida en cada fuego (hogar, fragua): sin sombras, corta.
    if (lights != nullptr) {
        for (const Vec3& p : *lights) {
            ecs::Entity fire = world_.create("Fuego", root);
            fire.setLocalPosition(p);
            ecs::Light& light = fire.add<ecs::Light>();
            light.type = ecs::LightType::Point;
            light.color = Vec3{1.0f, 0.55f, 0.24f};
            light.intensity = 5.0f;
            light.range = 7.5f;
            light.cast_shadows = false;
            light.source_radius = 0.12f;
        }
    }
    if (flatten) {
        terrain::TerrainFlatten& f = root.add<terrain::TerrainFlatten>();
        f.margin = 0.4f;
        // Talud segun lo empinado del sitio.
        terrain::Footprint fp{};
        float ground = 0.0f;
        ecs::Entity t_entity;
        std::shared_ptr<terrain::TerrainData> data;
        if (flattenFootprintOf(root, fp, ground) && terrainAt(fp.center.x, fp.center.z, t_entity, data)) {
            float lo = 0.0f;
            float hi = 0.0f;
            terrain::footprintHeight(*data, t_entity.get<terrain::Terrain>(), t_entity.worldPosition(), fp, &lo, &hi);
            f.blend = std::clamp(2.5f + 1.3f * (hi - lo), 3.0f, 10.0f);
        }
        flattenUnder(root, commit_terrain);
    }
    return root;
}

bool EditorApp::terrainAt(float x, float z, ecs::Entity& entity, std::shared_ptr<terrain::TerrainData>& data) {
    entity = {};
    data.reset();
    world_.forEachDepthFirst([&](ecs::Entity e) {
        if (data || !e.has<terrain::Terrain>() || !e.activeInHierarchy()) return;
        const terrain::Terrain& comp = e.get<terrain::Terrain>();
        const Vec3 origin = e.worldPosition();
        if (x < origin.x || z < origin.z || x > origin.x + comp.size || z > origin.z + comp.size) return;
        if (auto d = terrain_store_.get(comp)) {
            data = d;
            entity = e;
        }
    });
    return data != nullptr;
}

bool EditorApp::groundAt(float x, float z, float& y) {
    ecs::Entity e;
    std::shared_ptr<terrain::TerrainData> data;
    if (!terrainAt(x, z, e, data)) return false;
    y = terrain::heightAt(*data, e.get<terrain::Terrain>(), e.worldPosition(), x, z);
    return true;
}

// ---------------------------------------------------------------------------
// Aplanar el terreno (TerrainFlatten)
// ---------------------------------------------------------------------------

bool EditorApp::flattenFootprintOf(ecs::Entity entity, terrain::Footprint& footprint, float& ground) {
    const terrain::TerrainFlatten* f = entity.tryGet<terrain::TerrainFlatten>();
    Vec3 lo{};
    Vec3 hi{};
    if (!physics_.localMeshBounds(entity, lo, hi)) return false;
    const core::Mat4& m = entity.worldMatrix();
    const float sx = std::hypot(m.m[0][0], m.m[0][1], m.m[0][2]);
    const float sz = std::hypot(m.m[2][0], m.m[2][1], m.m[2][2]);
    const float margin = f != nullptr ? f->margin : 0.4f;
    footprint.center = ecs::transformPoint(m, Vec3{(lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f});
    footprint.half = core::Vec2{(hi.x - lo.x) * 0.5f * sx + margin, (hi.z - lo.z) * 0.5f * sz + margin};
    footprint.yaw_degrees = std::atan2(-m.m[0][2], m.m[0][0]) * 180.0f / core::kPi;
    if (f != nullptr && f->mesh_bottom) {
        ground = 1e30f;
        for (int i = 0; i < 8; ++i) {
            const Vec3 corner{(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z};
            ground = std::min(ground, ecs::transformPoint(m, corner).y);
        }
    } else {
        ground = entity.worldPosition().y;
    }
    if (f != nullptr) ground += f->ground_offset;
    return true;
}

bool EditorApp::flattenUnder(ecs::Entity entity, bool commit_collision) {
    if (!entity.valid()) return false;
    if (!entity.has<terrain::TerrainFlatten>()) entity.add<terrain::TerrainFlatten>();
    const terrain::TerrainFlatten f = entity.get<terrain::TerrainFlatten>();
    FlattenState& state = flatten_states_[entity.uuid()];
    state.pose = entity.worldMatrix();
    state.origin = world_.origin();
    state.settings = flattenSignature(f);
    state.applied = false;
    if (!f.enabled) return false;
    terrain::Footprint fp{};
    float ground = 0.0f;
    if (!flattenFootprintOf(entity, fp, ground)) return false;
    ecs::Entity t_entity;
    std::shared_ptr<terrain::TerrainData> data;
    if (!terrainAt(fp.center.x, fp.center.z, t_entity, data)) return false;
    const terrain::Terrain& comp = t_entity.get<terrain::Terrain>();
    const Vec3 origin = t_entity.worldPosition();
    state.terrain = comp.data;
    state.footprint = fp;
    state.blend = f.blend;
    state.patch = terrain::captureFootprint(*data, comp, origin, fp, f.blend);
    state.order = ++flatten_order_;
    // Un pelo por debajo: el zocalo tapa la junta y no hay parpadeo.
    terrain::flattenFootprint(*data, comp, origin, fp, ground - 0.02f, f.blend);
    const int layer = f.paint_layer == -2 ? findLayer(comp, {"tierra", "dirt"}) : f.paint_layer;
    if (layer >= 0) terrain::paintFootprint(*data, comp, origin, fp, layer, 1.2f, 1.0f);
    // Los vecinos que pisa este talud: que el terreno no les suba por encima del suelo.
    for (const auto& [uuid, other] : flatten_states_) {
        if (uuid == entity.uuid() || !other.applied || other.terrain != comp.data || !other.patch.valid() || !state.patch.valid()) continue;
        if (other.patch.x1 < state.patch.x0 || other.patch.x0 > state.patch.x1 || other.patch.y1 < state.patch.y0 ||
            other.patch.y0 > state.patch.y1) {
            continue;
        }
        const ecs::Entity o = world_.find(uuid);
        terrain::Footprint ofp{};
        float oground = 0.0f;
        if (o.valid() && o.has<terrain::TerrainFlatten>() && o.get<terrain::TerrainFlatten>().enabled && flattenFootprintOf(o, ofp, oground)) {
            clampUnder(*data, comp, origin, ofp, oground - 0.02f);
        }
    }
    if (commit_collision) data->commitCollision();
    state.applied = true;
    dirty_ = true;
    return true;
}

void EditorApp::updateTerrainFlatteners() {
    if (!has_project_ || playing()) return;
    // Escena nueva (o otro espacio de trabajo): lo que hay ya esta aplanado.
    if (flatten_prime_ || world_.sceneUuid() != flatten_scene_) {
        flatten_states_.clear();
        flatten_prime_ = false;
        flatten_scene_ = world_.sceneUuid();
        for (const entt::entity h : world_.registry().view<terrain::TerrainFlatten>()) {
            const ecs::Entity e = world_.wrap(h);
            FlattenState& state = flatten_states_[e.uuid()];
            state.pose = e.worldMatrix();
            state.origin = world_.origin();
            state.settings = flattenSignature(e.get<terrain::TerrainFlatten>());
            state.applied = true;  // sin parche: no se puede devolver el sitio de antes
        }
        return;
    }
    // Mientras se arrastra no se toca nada: al soltar.
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right)) return;

    std::vector<std::string> touched;
    const auto restore = [&](const FlattenState& s) {
        if (!s.patch.valid()) return;
        if (const std::shared_ptr<terrain::TerrainData> data = terrain_store_.findPath(s.terrain)) {
            terrain::restorePatch(*data, s.patch);
            touched.push_back(s.terrain);
        }
    };
    // Los que ya no estan (borrados, deshechos): su terreno vuelve a como
    // estaba, del ultimo aplanado al primero.
    std::vector<std::pair<std::uint64_t, Uuid>> gone;
    for (const auto& [uuid, s] : flatten_states_) {
        const ecs::Entity e = world_.find(uuid);
        if (!e.valid() || !e.has<terrain::TerrainFlatten>()) gone.push_back({s.order, uuid});
    }
    std::sort(gone.begin(), gone.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [order, uuid] : gone) {
        restore(flatten_states_[uuid]);
        flatten_states_.erase(uuid);
    }
    // Movidos, cambiados o nuevos.
    const double now = ImGui::GetTime();
    std::vector<ecs::Entity> redo;
    for (const entt::entity h : world_.registry().view<terrain::TerrainFlatten>()) {
        const ecs::Entity e = world_.wrap(h);
        if (!e.activeInHierarchy()) continue;
        const auto it = flatten_states_.find(e.uuid());
        if (it == flatten_states_.end()) {
            redo.push_back(e);
            continue;
        }
        FlattenState& s = it->second;
        const bool moved = !samePose(s.pose, s.origin, e.worldMatrix(), world_.origin());
        const bool changed = s.settings != flattenSignature(e.get<terrain::TerrainFlatten>());
        if (moved || changed || (!s.applied && now >= s.retry_at)) redo.push_back(e);
    }
    for (ecs::Entity e : redo) {
        const auto it = flatten_states_.find(e.uuid());
        terrain::HeightPatch old_patch;
        std::string old_terrain;
        if (it != flatten_states_.end()) {
            if (it->second.applied) {
                restore(it->second);
                old_patch = it->second.patch;
                old_terrain = it->second.terrain;
            }
        }
        // Los vecinos que pisaban el trozo devuelto se vuelven a aplanar.
        if (old_patch.valid()) {
            for (auto& [uuid, other] : flatten_states_) {
                if (uuid == e.uuid() || !other.applied || other.terrain != old_terrain) continue;
                if (other.patch.x1 < old_patch.x0 || other.patch.x0 > old_patch.x1 || other.patch.y1 < old_patch.y0 ||
                    other.patch.y0 > old_patch.y1) {
                    continue;
                }
                const ecs::Entity o = world_.find(uuid);
                if (!o.valid()) continue;
                terrain::Footprint fp{};
                float ground = 0.0f;
                ecs::Entity t_entity;
                std::shared_ptr<terrain::TerrainData> data;
                if (!flattenFootprintOf(o, fp, ground) || !terrainAt(fp.center.x, fp.center.z, t_entity, data)) continue;
                terrain::flattenFootprint(*data, t_entity.get<terrain::Terrain>(), t_entity.worldPosition(), fp, ground - 0.02f,
                                          o.get<terrain::TerrainFlatten>().blend);
            }
        }
        if (!flattenUnder(e, false)) {
            FlattenState& s = flatten_states_[e.uuid()];
            s.retry_at = now + 1.0;  // sin malla aun (cargando) o fuera del terreno
        } else {
            touched.push_back(flatten_states_[e.uuid()].terrain);
        }
    }
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    for (const std::string& path : touched) {
        if (const std::shared_ptr<terrain::TerrainData> data = terrain_store_.findPath(path)) data->commitCollision();
    }
    if (!touched.empty()) dirty_ = true;
}

// ---------------------------------------------------------------------------
// Pueblos y ciudades medievales
// ---------------------------------------------------------------------------

ecs::Entity EditorApp::generateSettlement(const asset::SettlementSettings& in, const Vec3& center, float search_radius, ecs::Entity parent,
                                          std::string* error, bool undo, int* houses_out) {
    // Terreno: el que hay bajo el centro, o el primero con datos.
    ecs::Entity terrain_entity;
    std::shared_ptr<terrain::TerrainData> data;
    if (!terrainAt(center.x, center.z, terrain_entity, data)) {
        world_.forEachDepthFirst([&](ecs::Entity e) {
            if (data || !e.has<terrain::Terrain>()) return;
            if (auto d = terrain_store_.get(e.get<terrain::Terrain>())) {
                data = d;
                terrain_entity = e;
            }
        });
    }
    if (!data) {
        if (error) *error = "no hay terreno en la escena";
        return {};
    }
    const terrain::Terrain comp = terrain_entity.get<terrain::Terrain>();
    const Vec3 origin = terrain_entity.worldPosition();
    const auto ground = [&](float x, float z) { return terrain::heightAt(*data, comp, origin, x, z); };

    // Agua: mar (y minima), rios y lagos de la escena.
    float min_y = -1e9f;
    struct Seg {
        Vec3 a, b;
        float width;
    };
    std::vector<Seg> rivers;
    struct Rect {
        Vec3 c;
        core::Vec2 half;
    };
    std::vector<Rect> lakes;
    for (const entt::entity h : world_.registry().view<water::WaterBody>()) {
        const ecs::Entity e = world_.wrap(h);
        const water::WaterBody& body = e.get<water::WaterBody>();
        if (body.type == water::WaterType::Ocean) {
            min_y = std::max(min_y, e.worldPosition().y + 2.5f);
        } else if (body.type == water::WaterType::River) {
            const std::vector<water::RiverSample> line = water::riverCenterline(body, e.worldMatrix(), 6.0f);
            for (std::size_t i = 0; i + 1 < line.size(); ++i) {
                rivers.push_back({line[i].position, line[i + 1].position, std::max(line[i].width, line[i + 1].width)});
            }
        } else if (body.type == water::WaterType::Lake) {
            lakes.push_back({e.worldPosition(), core::Vec2{body.size.x * 0.5f, body.size.y * 0.5f}});
        }
    }
    const auto dry = [&](float x, float z, float margin) {
        if (ground(x, z) < min_y) return false;
        for (const Seg& s : rivers) {
            const float abx = s.b.x - s.a.x;
            const float abz = s.b.z - s.a.z;
            const float len2 = abx * abx + abz * abz;
            const float t = len2 > 0.0f ? std::clamp(((x - s.a.x) * abx + (z - s.a.z) * abz) / len2, 0.0f, 1.0f) : 0.0f;
            const float dx = x - (s.a.x + abx * t);
            const float dz = z - (s.a.z + abz * t);
            const float reach = s.width * 0.5f + margin;
            if (dx * dx + dz * dz < reach * reach) return false;
        }
        for (const Rect& r : lakes) {
            if (std::abs(x - r.c.x) < r.half.x + margin && std::abs(z - r.c.z) < r.half.y + margin) return false;
        }
        return true;
    };

    asset::SettlementSettings s = in;
    const bool town = s.type == asset::SettlementType::Town;
    const bool hamlet = s.type == asset::SettlementType::Hamlet;
    const std::uint32_t seed = s.seed;
    const bool interiors = settlement_interiors_;

    // --- Variantes de casa (urbanas primero) ---
    std::vector<asset::HouseSettings> variants;
    const auto add = [&](asset::HouseStyle style, std::uint32_t sub) {
        asset::HouseSettings h = asset::housePreset(style, seed * 131U + sub);
        h.interior = interiors;
        if (town && style == asset::HouseStyle::HalfTimbered) h.width = std::min(h.width, 8.0f);
        variants.push_back(h);
    };
    int urban = 0;
    if (town) {
        for (std::uint32_t i = 1; i <= 4; ++i) add(asset::HouseStyle::HalfTimbered, i);
        add(asset::HouseStyle::StoneCottage, 5);
        variants.back().floors = 2;
        urban = 5;
        add(asset::HouseStyle::Thatched, 6);
        add(asset::HouseStyle::TimberCabin, 7);
    } else if (!hamlet) {
        for (std::uint32_t i = 1; i <= 3; ++i) add(asset::HouseStyle::HalfTimbered, i);
        add(asset::HouseStyle::StoneCottage, 4);
        urban = 4;
        add(asset::HouseStyle::Thatched, 5);
        add(asset::HouseStyle::Thatched, 6);
        add(asset::HouseStyle::StoneCottage, 7);
        add(asset::HouseStyle::TimberCabin, 8);
        add(asset::HouseStyle::LogCabin, 9);
    } else {
        add(asset::HouseStyle::Thatched, 1);
        urban = 1;
        add(asset::HouseStyle::Thatched, 2);
        add(asset::HouseStyle::Thatched, 3);
        add(asset::HouseStyle::LogCabin, 4);
        add(asset::HouseStyle::TimberCabin, 5);
        add(asset::HouseStyle::StoneCottage, 6);
    }
    s.urban_variants = urban;
    s.rural_variants = static_cast<int>(variants.size()) - urban;

    struct Built {
        Uuid uuid;
        asset::HouseModel model;
    };
    std::vector<Built> houses(variants.size());
    for (std::size_t i = 0; i < variants.size(); ++i) {
        houses[i].uuid = writeHouseModel(variants[i], error, &houses[i].model, false);
        if (!houses[i].uuid.valid()) return {};
    }
    const auto buildHouse = [&](asset::HouseSettings h, Built& out) {
        h.interior = interiors;
        out.uuid = writeHouseModel(h, error, &out.model, false);
        return out.uuid.valid();
    };
    const auto buildMedieval = [&](asset::MedievalBuilding type, std::uint32_t sub, float scale, Built& out) {
        asset::MedievalSettings ms;
        ms.type = type;
        ms.seed = seed * 17U + sub;
        ms.interior = interiors;
        ms.scale = scale;
        ms.wall_height = 7.5f;
        out.model = asset::buildMedieval(ms);
        char key[96];
        std::snprintf(key, sizeof(key), "%u-%.2f-%d-%.1f", ms.seed, ms.scale, ms.interior ? 1 : 0, ms.wall_height);
        out.uuid = writeBuildingModel(asset::medievalBuildingName(type), hashText(key), out.model, error, false);
        return out.uuid.valid();
    };
    Built tavern;
    Built smithy;
    Built church;
    Built barn;
    Built well;
    Built keep;
    Built gate;
    Built mill;
    std::vector<Built> stalls(3);
    std::map<int, Built> props;
    {
        asset::HouseSettings t = asset::housePreset(asset::HouseStyle::HalfTimbered, seed * 7U + 101U);
        t.width = 11.5f;
        t.depth = 8.5f;
        t.floors = 2;
        t.use = asset::HouseUse::Tavern;
        t.chimney = true;
        if (!buildHouse(t, tavern)) return {};
        asset::HouseSettings sm = asset::housePreset(asset::HouseStyle::StoneCottage, seed * 7U + 202U);
        sm.width = 6.8f;
        sm.depth = 5.6f;
        sm.use = asset::HouseUse::Smithy;
        sm.chimney = true;
        sm.porch = false;
        if (!buildHouse(sm, smithy)) return {};
    }
    if (!buildMedieval(asset::MedievalBuilding::Church, 1, town ? 1.35f : 1.0f, church)) return {};
    if (!buildMedieval(asset::MedievalBuilding::Barn, 2, 1.0f, barn)) return {};
    if (!buildMedieval(asset::MedievalBuilding::Well, 3, 1.0f, well)) return {};
    if (!buildMedieval(asset::MedievalBuilding::Keep, 4, 1.1f, keep)) return {};
    if (!buildMedieval(asset::MedievalBuilding::Gatehouse, 5, 1.0f, gate)) return {};
    if (!buildMedieval(asset::MedievalBuilding::Windmill, 6, 1.0f, mill)) return {};
    for (std::size_t i = 0; i < stalls.size(); ++i) {
        if (!buildMedieval(asset::MedievalBuilding::MarketStall, 10U + static_cast<std::uint32_t>(i), 1.0f, stalls[i])) return {};
    }
    for (int p = static_cast<int>(asset::MedievalBuilding::Barrels); p <= static_cast<int>(asset::MedievalBuilding::Bench); ++p) {
        if (!buildMedieval(static_cast<asset::MedievalBuilding>(p), 20U + static_cast<std::uint32_t>(p), 1.0f, props[p])) return {};
    }
    const auto builtFor = [&](asset::LotKind kind, int variant) -> const Built* {
        switch (kind) {
            case asset::LotKind::House: {
                const int v = variant >= 0 ? variant : -1 - variant;
                return v >= 0 && static_cast<std::size_t>(v) < houses.size() ? &houses[static_cast<std::size_t>(v)] : &houses[0];
            }
            case asset::LotKind::Church: return &church;
            case asset::LotKind::Tavern: return &tavern;
            case asset::LotKind::Smithy: return &smithy;
            case asset::LotKind::Barn: return &barn;
            case asset::LotKind::Well: return &well;
            case asset::LotKind::MarketStall: return &stalls[static_cast<std::size_t>(std::abs(variant)) % stalls.size()];
            case asset::LotKind::Keep: return &keep;
            case asset::LotKind::Gatehouse: return &gate;
            case asset::LotKind::Windmill: return &mill;
            case asset::LotKind::Prop: {
                const auto it = props.find(variant);
                return it != props.end() ? &it->second : &props.begin()->second;
            }
        }
        return &houses[0];
    };
    const asset::SettlementFootprint footprint = [&](asset::LotKind kind, int variant, core::Vec2& half_extents, core::Vec2& offset) {
        footprintOf(builtFor(kind, variant)->model, half_extents, offset);
    };

    // --- Trazado ---
    asset::SettlementTerrain T;
    T.height = ground;
    T.dry = dry;
    T.min_x = origin.x;
    T.min_z = origin.z;
    T.max_x = origin.x + comp.size;
    T.max_z = origin.z + comp.size;
    const asset::SettlementLayout L = asset::layoutSettlement(s, T, footprint, center, search_radius);
    if (!L.ok) {
        if (error) *error = L.error;
        refreshDatabase();
        return {};
    }
    // Tiendas: la variante urbana con el interior de tienda.
    std::map<int, Built> shops;
    for (const asset::SettlementLot& lot : L.lots) {
        if (lot.kind != asset::LotKind::House || lot.variant >= 0 || shops.count(lot.variant) > 0) continue;
        asset::HouseSettings h = variants[static_cast<std::size_t>(std::min(-1 - lot.variant, static_cast<int>(variants.size()) - 1))];
        h.use = asset::HouseUse::Shop;
        if (!buildHouse(h, shops[lot.variant])) return {};
    }
    refreshDatabase();  // los modelos nuevos, de una vez

    // --- Terreno: plaza, calles y muralla allanadas ---
    std::shared_ptr<terrain::TerrainData> before = undo ? std::make_shared<terrain::TerrainData>(*data) : nullptr;
    const terrain::Footprint plaza_fp{L.center, core::Vec2{L.plaza_radius * 0.75f, L.plaza_radius * 0.75f}, 0.0f};
    const float plaza_h = terrain::footprintHeight(*data, comp, origin, plaza_fp);
    terrain::flattenPath(*data, comp, origin, {Vec3{L.center.x, plaza_h, L.center.z}, Vec3{L.center.x + 0.01f, plaza_h, L.center.z}},
                         L.plaza_radius * 2.0f + 2.0f, 6.0f);
    std::vector<std::vector<Vec3>> road_lines;
    for (const asset::SettlementRoad& road : L.roads) {
        std::vector<Vec3> pts = road.points;
        for (Vec3& p : pts) p.y = ground(p.x, p.z);
        smoothHeights(pts, 3, 2);
        terrain::flattenPath(*data, comp, origin, pts, road.width + 0.8f, 3.5f);
        road_lines.push_back(std::move(pts));
    }
    // Muralla: anillo subdividido (sigue el terreno) y su franja allanada.
    std::vector<Vec3> wall_pts;
    std::vector<bool> wall_towers;
    std::vector<bool> wall_gaps;
    if (L.wall.size() >= 3) {
        const std::size_t n = L.wall.size();
        for (std::size_t i = 0; i < n; ++i) {
            const Vec3& a = L.wall[i];
            const Vec3& b = L.wall[(i + 1) % n];
            if (L.wall_gaps[i]) {
                wall_pts.push_back(a);
                wall_towers.push_back(false);
                wall_gaps.push_back(true);
                continue;
            }
            const std::vector<Vec3> sub = subdivide(a, b, 8.0f);
            for (std::size_t k = 0; k + 1 < sub.size(); ++k) {
                wall_pts.push_back(sub[k]);
                wall_towers.push_back(k == 0 && L.wall_towers[i]);
                wall_gaps.push_back(false);
            }
        }
        for (Vec3& p : wall_pts) p.y = ground(p.x, p.z);
        std::vector<Vec3> ring = wall_pts;
        ring.push_back(wall_pts.front());
        smoothHeights(ring, 2, 1);
        terrain::flattenPath(*data, comp, origin, ring, 2.4f + 4.0f, 4.0f);
        for (Vec3& p : wall_pts) p.y = ground(p.x, p.z);
    }

    // --- Entidades ---
    const std::string group_name = asset::settlementTypeName(s.type);
    ecs::Entity group = world_.create(group_name, parent);
    group.setWorldPosition(Vec3{L.center.x, plaza_h, L.center.z});
    const auto sub_group = [&](const char* name) {
        ecs::Entity e = world_.create(name, group);
        e.setLocalPosition(Vec3{});
        return e;
    };
    ecs::Entity g_houses = sub_group("Casas");
    ecs::Entity g_buildings = sub_group("Edificios");
    ecs::Entity g_market = sub_group("Mercado");
    ecs::Entity g_props = sub_group("Objetos");
    ecs::Entity g_walls;
    ecs::Entity g_fields;
    std::vector<std::pair<Vec3, float>> placed;  // para los claros de la vegetacion
    int made = 0;
    int house_index = 0;
    for (const asset::SettlementLot& lot : L.lots) {
        const Built* b = builtFor(lot.kind, lot.variant);
        if (lot.kind == asset::LotKind::House && lot.variant < 0) b = &shops[lot.variant];
        const bool flat = lot.kind != asset::LotKind::Prop;
        const core::Vec2 X{std::cos(lot.yaw * core::kPi / 180.0f), -std::sin(lot.yaw * core::kPi / 180.0f)};
        const core::Vec2 Z{std::sin(lot.yaw * core::kPi / 180.0f), std::cos(lot.yaw * core::kPi / 180.0f)};
        const Vec3 aabb_center{lot.position.x + X.x * lot.offset.x + Z.x * lot.offset.y, 0.0f,
                               lot.position.z + X.y * lot.offset.x + Z.y * lot.offset.y};
        Vec3 pos = lot.position;
        float lo = 0.0f;
        float hi = 0.0f;
        if (lot.kind == asset::LotKind::Gatehouse) {
            pos.y = ground(pos.x, pos.z);
        } else if (flat) {
            const terrain::Footprint fp{aabb_center, lot.half, lot.yaw};
            pos.y = terrain::footprintHeight(*data, comp, origin, fp, &lo, &hi);
            if (lot.kind == asset::LotKind::Well || lot.kind == asset::LotKind::MarketStall) pos.y = plaza_h;
        } else {
            pos.y = ground(pos.x, pos.z);
        }
        ecs::Entity target = g_houses;
        switch (lot.kind) {
            case asset::LotKind::House: target = g_houses; break;
            case asset::LotKind::Well:
            case asset::LotKind::MarketStall: target = g_market; break;
            case asset::LotKind::Prop: target = g_props; break;
            case asset::LotKind::Gatehouse:
                if (!g_walls.valid()) g_walls = sub_group("Muralla");
                target = g_walls;
                break;
            default: target = g_buildings; break;
        }
        ecs::Entity e = placeHouse(b->uuid, pos, lot.yaw, target, false, settlement_lights_ ? &b->model.lights : nullptr, false);
        if (!e.valid()) continue;
        if (lot.kind == asset::LotKind::House) {
            e.setName((lot.variant < 0 ? std::string("Tienda") : e.name()) + " " + std::to_string(++house_index));
        }
        if (flat) {
            terrain::TerrainFlatten& f = e.add<terrain::TerrainFlatten>();
            f.margin = lot.kind == asset::LotKind::Well || lot.kind == asset::LotKind::MarketStall ? 0.2f : 0.4f;
            f.blend = lot.kind == asset::LotKind::Gatehouse ? 4.0f : std::clamp(2.0f + 1.3f * (hi - lo), 2.5f, 9.0f);
            if (lot.kind == asset::LotKind::MarketStall || lot.kind == asset::LotKind::Well) f.paint_layer = -1;
            flattenUnder(e, false);
        }
        placed.push_back({pos, std::hypot(lot.half.x, lot.half.y)});
        ++made;
    }
    // Muralla (una malla, en el espacio del grupo).
    if (wall_pts.size() >= 3) {
        if (!g_walls.valid()) g_walls = sub_group("Muralla");
        asset::CityWallSettings ws;
        const Vec3 base = group.worldPosition();
        for (const Vec3& p : wall_pts) ws.points.push_back(p - base);
        ws.towers = wall_towers;
        ws.gaps = wall_gaps;
        ws.height = 7.5f;
        ws.thickness = 2.4f;
        ws.seed = seed;
        const asset::HouseModel wall = asset::buildCityWall(ws);
        char key[96];
        std::snprintf(key, sizeof(key), "%u-%zu-%.1f-%.1f-%.1f", seed, ws.points.size(), base.x, base.y, base.z);
        const Uuid uuid = writeBuildingModel("Muralla", hashText(key), wall, error);
        if (uuid.valid()) {
            ecs::Entity e = placeHouse(uuid, base, 0.0f, g_walls, false, nullptr, false);
            if (e.valid()) e.setName("Muralla");
        }
    }
    // Campos: hierba seca (trigo), surcos y vallas.
    const int dirt = findLayer(comp, {"tierra", "dirt"});
    const int gravel = findLayer(comp, {"grava", "gravel", "camino"});
    const int dry_grass = findLayer(comp, {"seca", "dry"});
    if (!L.fields.empty() || !L.fences.empty()) g_fields = sub_group("Campos");
    for (const asset::SettlementField& f : L.fields) {
        const terrain::Footprint fp{f.center, f.half, f.yaw};
        if (dry_grass >= 0) terrain::paintFootprint(*data, comp, origin, fp, dry_grass, 2.0f, 0.55f);
        if (dirt >= 0) {
            const float a = f.yaw * core::kPi / 180.0f;
            const Vec3 ax{std::cos(a), 0.0f, -std::sin(a)};
            const Vec3 az{std::sin(a), 0.0f, std::cos(a)};
            for (float z = -f.half.y + 1.3f; z < f.half.y - 0.8f; z += 2.6f) {
                const Vec3 p0 = f.center + ax * (-f.half.x + 1.0f) + az * z;
                const Vec3 p1 = f.center + ax * (f.half.x - 1.0f) + az * z;
                terrain::paintPath(*data, comp, origin, {p0, p1}, 0.9f, dirt, 0.4f, 0.65f);
            }
        }
    }
    if (!L.fences.empty()) {
        std::vector<std::pair<Vec3, Vec3>> segments;
        const Vec3 base = group.worldPosition();
        for (const auto& [a, b] : L.fences) {
            const Vec3 pa{a.x, ground(a.x, a.z), a.z};
            const Vec3 pb{b.x, ground(b.x, b.z), b.z};
            segments.push_back({pa - base, pb - base});
        }
        const asset::HouseModel fences = asset::buildFences(segments);
        char key[96];
        std::snprintf(key, sizeof(key), "%u-%zu-%.1f-%.1f", seed, segments.size(), base.x, base.z);
        const Uuid uuid = writeBuildingModel("Vallas", hashText(key), fences, error);
        if (uuid.valid()) {
            ecs::Entity e = placeHouse(uuid, base, 0.0f, g_fields, false, nullptr, false);
            if (e.valid()) e.setName("Vallas");
        }
    }
    // Pintura: plaza y calles de grava/tierra.
    const int paving = gravel >= 0 ? gravel : dirt;
    if (paving >= 0) {
        terrain::paintPath(*data, comp, origin, {L.center, L.center + Vec3{0.01f, 0.0f, 0.0f}}, L.plaza_radius * 2.0f + 1.0f, paving, 3.0f, 1.0f);
    }
    for (std::size_t r = 0; r < L.roads.size(); ++r) {
        const asset::SettlementRoad& road = L.roads[r];
        const int layer = (town && gravel >= 0) ? gravel : dirt;
        if (layer < 0) continue;
        terrain::paintPath(*data, comp, origin, road_lines[r], road.width, layer, 1.5f, road.main ? 0.95f : 0.85f);
    }
    // Claros en la vegetacion: el pueblo, los campos y los caminos de fuera.
    for (const entt::entity h : world_.registry().view<foliage::Foliage>()) {
        foliage::Foliage& f = world_.registry().get<foliage::Foliage>(h);
        f.clearings.push_back(foliage::FoliageClearing{L.center, L.radius * (town ? 1.12f : 1.05f)});
        for (const asset::SettlementField& field : L.fields) {
            f.clearings.push_back(foliage::FoliageClearing{field.center, std::hypot(field.half.x, field.half.y) + 2.0f});
        }
        for (const auto& [p, r] : placed) {
            if (std::hypot(p.x - L.center.x, p.z - L.center.z) > L.radius) f.clearings.push_back(foliage::FoliageClearing{p, r + 3.0f});
        }
        for (const std::vector<Vec3>& line : road_lines) {
            float last = -1e9f;
            float walked = 0.0f;
            for (std::size_t i = 0; i + 1 < line.size(); ++i) {
                walked += std::hypot(line[i + 1].x - line[i].x, line[i + 1].z - line[i].z);
                const float d = std::hypot(line[i].x - L.center.x, line[i].z - L.center.z);
                if (d < L.radius || walked - last < 12.0f) continue;
                f.clearings.push_back(foliage::FoliageClearing{line[i], 5.5f});
                last = walked;
            }
        }
    }
    data->commitCollision();
    if (undo && before) pushTerrainUndo(comp.data, std::move(before));
    if (houses_out != nullptr) *houses_out = L.houses;
    std::cout << "[Pueblos] " << group_name << ": " << L.houses << " casas, " << made << " edificios y objetos, " << L.roads.size()
              << " calles" << (wall_pts.empty() ? "" : ", murallas") << ", " << L.fields.size() << " campos" << std::endl;
    return group;
}

int EditorApp::placeVillage(int count, std::uint32_t seed, ecs::Entity parent, std::string* error, int type) {
    if (count <= 0) return 0;
    asset::SettlementSettings s;
    if (type < 0) {
        s.type = count <= 12 ? asset::SettlementType::Hamlet : (count <= 40 ? asset::SettlementType::Village : asset::SettlementType::Town);
    } else {
        s.type = static_cast<asset::SettlementType>(std::clamp(type, 0, asset::kSettlementTypeCount - 1));
    }
    s.seed = seed;
    s.houses = count;
    // Centro del primer terreno; se busca el mejor sitio en buena parte de el.
    Vec3 center{};
    float search = 0.0f;
    world_.forEachDepthFirst([&](ecs::Entity e) {
        if (search > 0.0f || !e.has<terrain::Terrain>()) return;
        const terrain::Terrain& comp = e.get<terrain::Terrain>();
        center = e.worldPosition() + Vec3{comp.size * 0.5f, 0.0f, comp.size * 0.5f};
        search = comp.size * 0.4f;
    });
    if (search <= 0.0f) {
        if (error) *error = "no hay terreno en la escena";
        return 0;
    }
    int houses = 0;
    ecs::Entity group = generateSettlement(s, center, search, parent, error, false, &houses);
    if (group.valid()) group.setName(kVillageName);
    return group.valid() ? std::max(houses, 1) : 0;
}

// ---------------------------------------------------------------------------
// Ventana
// ---------------------------------------------------------------------------

void EditorApp::drawHouseGeneratorWindow() {
    if (!show_house_generator_) return;
    ImGui::SetNextWindowSize(ImVec2(440.0f, 700.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Generador de casas y pueblos", &show_house_generator_)) {
        ImGui::End();
        return;
    }
    asset::HouseSettings& s = house_gen_;
    if (ImGui::BeginTabBar("##house_tabs")) {
        // ---------------- Casa suelta ----------------
        if (ImGui::BeginTabItem("Casa")) {
            ImGui::TextWrapped("Casas procedurales listas para el juego, con interior: pocos triángulos, 14 materiales PBR "
                               "compartidos (con relieve), colisión, la puerta aparte (gira en Y) y el terreno aplanado "
                               "bajo su caja.");
            ImGui::Spacing();
            int style = static_cast<int>(s.style);
            const char* styles[asset::kHouseStyleCount] = {"Cabaña de troncos", "Cabaña de tablas", "Casita de piedra",
                                                           "Casa de campo",     "Casa de entramado", "Cabaña de paja"};
            ImGui::SetNextItemWidth(-150.0f);
            if (ImGui::Combo("Estilo", &style, styles, asset::kHouseStyleCount)) {
                const bool interior = s.interior;
                const asset::HouseUse use = s.use;
                s = asset::housePreset(static_cast<asset::HouseStyle>(style), s.seed);
                s.interior = interior;
                s.use = use;
            }
            int use = static_cast<int>(s.use);
            const char* uses[asset::kHouseUseCount] = {"Vivienda", "Taberna", "Herrería", "Tienda"};
            ImGui::SetNextItemWidth(-150.0f);
            if (ImGui::Combo("Uso", &use, uses, asset::kHouseUseCount)) s.use = static_cast<asset::HouseUse>(use);
            ImGui::SetItemTooltip("Cambia el interior (barra y cuartos, fragua y cobertizo, mostrador) y algún detalle de fuera.");
            int seed = static_cast<int>(s.seed);
            ImGui::SetNextItemWidth(-190.0f);
            if (ImGui::InputInt("Semilla", &seed)) s.seed = static_cast<std::uint32_t>(std::max(seed, 0));
            ImGui::SameLine();
            if (ImGui::Button("Al azar")) {
                const bool interior = s.interior;
                const asset::HouseUse u = s.use;
                s = asset::housePreset(s.style, std::random_device{}() % 100000U);
                s.interior = interior;
                s.use = u;
            }
            ImGui::SetItemTooltip("Otras medidas y otra distribución de las ventanas.");

            const auto slider = [](const char* label, float* v, float lo, float hi, const char* fmt) {
                ImGui::SetNextItemWidth(-150.0f);
                ImGui::SliderFloat(label, v, lo, hi, fmt);
            };
            if (ImGui::CollapsingHeader("Medidas", ImGuiTreeNodeFlags_DefaultOpen)) {
                slider("Ancho (m)", &s.width, 4.0f, 16.0f, "%.1f");
                slider("Fondo (m)", &s.depth, 3.5f, 12.0f, "%.1f");
                ImGui::SetNextItemWidth(-150.0f);
                ImGui::SliderInt("Plantas", &s.floors, 1, 3);
                slider("Altura por planta (m)", &s.wall_height, 2.3f, 3.5f, "%.2f");
                slider("Pendiente del tejado", &s.roof_pitch, 15.0f, 60.0f, "%.0f°");
                slider("Alero (m)", &s.roof_overhang, 0.2f, 1.2f, "%.2f");
                if (s.style == asset::HouseStyle::HalfTimbered) slider("Vuelo de la planta alta (m)", &s.jetty, 0.0f, 0.9f, "%.2f");
                ImGui::SetNextItemWidth(-150.0f);
                ImGui::SliderInt("Ventanas delante", &s.windows, -1, 8);
                ImGui::SetItemTooltip("-1 = según el ancho.");
            }
            if (ImGui::CollapsingHeader("Detalles", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox("Interior amueblado", &s.interior);
                ImGui::SetItemTooltip("Hogar con su fuego, mesa y bancos, camas, arcones, alacenas, escalera y forjados, "
                                      "vigas... (con colisión).");
                ImGui::Checkbox("Porche con barandilla", &s.porch);
                ImGui::Checkbox("Chimenea", &s.chimney);
                ImGui::Checkbox("Contraventanas", &s.shutters);
            }
            ImGui::Separator();
            ImGui::BeginDisabled(!has_project_ || playing());
            if (ImGui::Button("Crear en la escena", ImVec2(-1.0f, 34.0f))) {
                std::string error;
                asset::HouseModel model;
                const Uuid uuid = writeHouseModel(s, &error, &model);
                if (!uuid.valid()) {
                    std::cerr << "[Casas] " << error << std::endl;
                } else {
                    // Delante de la camara, sobre el suelo si hay terreno.
                    Vec3 pos = scene_.camera().position() + scene_.camera().forward() * 18.0f;
                    float y = 0.0f;
                    pos.y = groundAt(pos.x, pos.z, y) ? y : 0.0f;
                    ecs::Entity house = placeHouse(uuid, pos, 0.0f, {}, true, &model.lights);
                    if (house.valid()) {
                        selectOnly(house.uuid());
                        revealInHierarchy(house.uuid());
                        commit();
                    }
                }
            }
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
        // ---------------- Edificio suelto ----------------
        if (ImGui::BeginTabItem("Edificio")) {
            ImGui::TextWrapped("Edificios y objetos medievales sueltos (con interior donde lo tienen).");
            const char* names[asset::kMedievalBuildingCount];
            for (int i = 0; i < asset::kMedievalBuildingCount; ++i) names[i] = asset::medievalBuildingName(static_cast<asset::MedievalBuilding>(i));
            ImGui::SetNextItemWidth(-150.0f);
            ImGui::Combo("Edificio", &building_gen_, names, asset::kMedievalBuildingCount);
            ImGui::BeginDisabled(!has_project_ || playing());
            if (ImGui::Button("Crear en la escena##building", ImVec2(-1.0f, 32.0f))) {
                asset::MedievalSettings ms;
                ms.type = static_cast<asset::MedievalBuilding>(building_gen_);
                ms.seed = s.seed;
                ms.interior = s.interior;
                const asset::HouseModel model = asset::buildMedieval(ms);
                std::string error;
                char key[64];
                std::snprintf(key, sizeof(key), "%u-%.2f-%d-%.1f", ms.seed, ms.scale, ms.interior ? 1 : 0, ms.wall_height);
                const Uuid uuid = writeBuildingModel(names[building_gen_], hashText(key), model, &error);
                if (!uuid.valid()) {
                    std::cerr << "[Casas] " << error << std::endl;
                } else {
                    Vec3 pos = scene_.camera().position() + scene_.camera().forward() * 22.0f;
                    float y = 0.0f;
                    pos.y = groundAt(pos.x, pos.z, y) ? y : 0.0f;
                    const bool is_prop = building_gen_ >= static_cast<int>(asset::MedievalBuilding::Barrels);
                    ecs::Entity e = placeHouse(uuid, pos, 0.0f, {}, !is_prop, &model.lights);
                    if (e.valid()) {
                        selectOnly(e.uuid());
                        revealInHierarchy(e.uuid());
                        commit();
                    }
                }
            }
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
        // ---------------- Pueblo / ciudad ----------------
        if (ImGui::BeginTabItem("Pueblo")) {
            asset::SettlementSettings& g = settlement_gen_;
            ImGui::TextWrapped("Pueblos y ciudades medievales completos sobre el terreno: calles y plaza allanadas, casas "
                               "con interior mirando a la calle, iglesia, taberna, herrerías, tiendas, mercado y pozo, "
                               "graneros y campos con vallas, molino y, en las ciudades, murallas con torres, puertas "
                               "y la torre del homenaje.");
            ImGui::Spacing();
            int type = static_cast<int>(g.type);
            const char* types[asset::kSettlementTypeCount] = {"Aldea", "Pueblo", "Ciudad amurallada"};
            ImGui::SetNextItemWidth(-150.0f);
            ImGui::Combo("Tipo", &type, types, asset::kSettlementTypeCount);
            g.type = static_cast<asset::SettlementType>(type);
            int seed = static_cast<int>(g.seed);
            ImGui::SetNextItemWidth(-190.0f);
            if (ImGui::InputInt("Semilla##pueblo", &seed)) g.seed = static_cast<std::uint32_t>(std::max(seed, 0));
            ImGui::SameLine();
            if (ImGui::Button("Al azar##pueblo")) g.seed = std::random_device{}() % 100000U;
            ImGui::SetNextItemWidth(-150.0f);
            ImGui::SliderInt("Casas", &g.houses, 0, 200);
            ImGui::SetItemTooltip("0 = según el tipo (aldea 10, pueblo 28, ciudad 75).");
            ImGui::SetNextItemWidth(-150.0f);
            ImGui::SliderFloat("Radio (m)", &g.radius, 0.0f, 400.0f, g.radius <= 0.0f ? "según el tipo" : "%.0f m");
            ImGui::SetNextItemWidth(-150.0f);
            ImGui::SliderInt("Calles principales", &g.main_roads, 0, 8);
            ImGui::SetItemTooltip("0 = según el tipo.");
            ImGui::SetNextItemWidth(-150.0f);
            ImGui::SliderFloat("Pendiente máxima (m)", &g.max_slope, 1.0f, 10.0f, "%.1f");
            ImGui::SetItemTooltip("Diferencia de altura máxima bajo un edificio (el terreno se aplana igual).");
            ImGui::Checkbox("Murallas (ciudad)", &g.walls);
            ImGui::SameLine();
            ImGui::Checkbox("Campos", &g.fields);
            ImGui::Checkbox("Mercado", &g.market);
            ImGui::SameLine();
            ImGui::Checkbox("Iglesia", &g.church);
            ImGui::SameLine();
            ImGui::Checkbox("Objetos", &g.props);
            ImGui::Checkbox("Interiores amueblados", &settlement_interiors_);
            ImGui::SameLine();
            ImGui::Checkbox("Luces de los hogares", &settlement_lights_);
            ImGui::Checkbox("Delante de la cámara", &settlement_here_);
            ImGui::SetItemTooltip("Sin marcar: busca el sitio más llano y seco del terreno.");
            ImGui::Separator();
            ImGui::BeginDisabled(!has_project_ || playing());
            if (ImGui::Button("Crear pueblo en el terreno", ImVec2(-1.0f, 34.0f))) {
                std::string error;
                // Reemplaza el anterior del mismo tipo (y le devuelve el terreno).
                if (ecs::Entity old = world_.findByName(asset::settlementTypeName(g.type)); old.valid()) {
                    world_.destroy(old);
                    updateTerrainFlatteners();
                }
                Vec3 center{};
                float search = 0.0f;
                if (settlement_here_) {
                    center = scene_.camera().position() + scene_.camera().forward() * 60.0f;
                } else {
                    world_.forEachDepthFirst([&](ecs::Entity e) {
                        if (search > 0.0f || !e.has<terrain::Terrain>()) return;
                        const terrain::Terrain& comp = e.get<terrain::Terrain>();
                        center = e.worldPosition() + Vec3{comp.size * 0.5f, 0.0f, comp.size * 0.5f};
                        search = comp.size * 0.42f;
                    });
                }
                const ecs::Entity group = generateSettlement(g, center, search, world_.findByName("Mundo generado"), &error);
                if (!group.valid()) {
                    std::cerr << "[Pueblos] " << error << std::endl;
                } else {
                    selectOnly(group.uuid());
                    revealInHierarchy(group.uuid());
                    commit();
                }
            }
            ImGui::SetItemTooltip("Reemplaza el anterior del mismo tipo. Ctrl+Z lo deshace (y el terreno).");
            ImGui::Spacing();
            ImGui::SetNextItemWidth(-150.0f);
            ImGui::SliderInt("Casas de la aldea", &house_gen_village_, 1, 60);
            if (ImGui::Button("Aldea rápida en el terreno", ImVec2(-1.0f, 28.0f))) {
                std::string error;
                if (ecs::Entity old = world_.findByName(kVillageName); old.valid()) {
                    world_.destroy(old);
                    updateTerrainFlatteners();
                }
                ecs::Entity parent = world_.findByName("Mundo generado");
                const int made = placeVillage(house_gen_village_, s.seed, parent, &error);
                if (made == 0) std::cerr << "[Casas] " << error << std::endl;
                else commit();
            }
            ImGui::SetItemTooltip("Como en el generador de terreno: aldea, pueblo o ciudad según el número de casas.");
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

}  // namespace cramion::editor
