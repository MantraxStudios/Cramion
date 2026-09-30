// Generador de casas (Ventana > Generador de casas): cabanas de troncos, de
// tablas, casitas de piedra y casas de campo procedurales. Cada casa es un
// modelo (.crdata en Assets/Casas/Modelos) con dos piezas, "Casa" (con
// MeshCollider) y "Puerta" (bisagra en su origen, BoxCollider), y 8
// materiales .crmat compartidos por todas (Assets/Casas/Materiales) con
// texturas PBR procedurales y relieve (Assets/Casas/Texturas, se crean una
// vez). "Aldea" reparte casas por el terreno generado: en llano, fuera del
// agua, mirando a la plaza, y abre claros en la vegetacion.

#include "EditorApp.h"

#include "Dialogs.h"

#include <CramionCore/asset/Importer.h>
#include <CramionCore/asset/MaterialAsset.h>
#include <CramionCore/asset/ModelMaterials.h>
#include <CramionCore/ecs/ModelInstantiation.h>
#include <CramionCore/foliage/Foliage.h>
#include <CramionCore/physics/PhysicsComponents.h>
#include <CramionCore/terrain/TerrainTools.h>
#include <CramionCore/water/Water.h>
#include <CramionFX/asset/HouseGenerator.h>
#include <CramionFX/asset/ImageFile.h>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <random>
#include <thread>

namespace cramion::editor {

using core::Vec3;

namespace {

constexpr const char* kHouseTextures = "Casas/Texturas";
constexpr const char* kHouseMaterials = "Casas/Materiales";
constexpr const char* kHouseModels = "Casas/Modelos";
constexpr const char* kHouseTextureVersion = "cramion-house-textures-3";
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

std::string settingsKey(const asset::HouseSettings& s) {
    char text[256];
    std::snprintf(text, sizeof(text), "%d-%u-%.2f-%.2f-%d-%.2f-%.1f-%.2f-%d%d%d-%d", static_cast<int>(s.style), s.seed, s.width,
                  s.depth, s.floors, s.wall_height, s.roof_pitch, s.roof_overhang, s.porch ? 1 : 0, s.chimney ? 1 : 0,
                  s.shutters ? 1 : 0, s.windows);
    std::uint32_t h = 2166136261U;
    for (const char* p = text; *p != 0; ++p) h = (h ^ static_cast<std::uint8_t>(*p)) * 16777619U;
    char out[16];
    std::snprintf(out, sizeof(out), "%08x", h);
    return out;
}

}  // namespace

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
        if (!std::filesystem::exists(file) || !assets::loadMaterial(file, mat)) {
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
                case asset::kHouseGlass:
                    mat.base_color = core::Vec4{0.03f, 0.035f, 0.04f, 1.0f};
                    mat.roughness = 0.04f;
                    mat.reflectance = 0.08f;
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
            if (!assets::saveMaterial(mat, file, error)) return false;
        }
        map[assets::modelMaterialKey(name, static_cast<std::size_t>(m))] = mat.uuid;
    }
    return true;
}

Uuid EditorApp::writeHouseModel(const asset::HouseSettings& settings, std::string* error) {
    assets::ModelMaterialMap map;
    if (!ensureHouseMaterials(map, error)) return {};
    const std::filesystem::path folder = project_.assetsFolder() / dialogs::fromUtf8(kHouseModels);
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::string style = asset::houseStyleName(settings.style);
    const std::string file_name = style + " " + settingsKey(settings) + ".crdata";
    const std::filesystem::path file = folder / dialogs::fromUtf8(file_name);

    // La misma casa ya hecha: se reutiliza (instancias del mismo modelo).
    refreshDatabase();
    if (database_) {
        for (const assets::AssetInfo& info : database_->all()) {
            if (info.type == assets::AssetType::Model && std::filesystem::equivalent(database_->root() / info.path, file, ec)) {
                assets::saveModelMaterialMap(project_.settingsFolder(), info.uuid, map);
                return info.uuid;
            }
        }
    }

    const asset::HouseModel house = asset::buildHouse(settings);
    std::vector<assets::ModelNode> nodes;
    nodes.push_back(assets::ModelNode{style, -1, core::Mat4::identity(), -1});
    nodes.push_back(assets::ModelNode{"Casa", 0, core::Mat4::identity(), 0});
    nodes.push_back(assets::ModelNode{"Puerta", 0, core::composeTrs(house.door_hinge, core::Quat{}, Vec3{1.0f, 1.0f, 1.0f}), 1});
    const Uuid uuid = Uuid::generate();
    if (!assets::writeGeneratedModel(file, uuid, style, nodes, {house.house, house.door}, error)) return {};
    assets::saveModelMaterialMap(project_.settingsFolder(), uuid, map);
    refreshDatabase();
    std::cout << "[Casas] " << style << ": " << house.triangles << " triangulos, " << dialogs::utf8(file.filename())
              << std::endl;
    return uuid;
}

ecs::Entity EditorApp::placeHouse(const Uuid& model_uuid, const Vec3& position, float yaw_degrees, ecs::Entity parent) {
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
        } else if (child.name() == "Puerta" && model->parts.size() > 1) {
            // La hoja: caja con el centro en la mitad (el origen es la bisagra).
            const asset::ModelData& door = *model->parts[1];
            Vec3 lo{1e9f, 1e9f, 1e9f};
            Vec3 hi{-1e9f, -1e9f, -1e9f};
            for (const asset::SkinnedVertex& v : door.vertices) {
                lo = Vec3{std::min(lo.x, v.position.x), std::min(lo.y, v.position.y), std::min(lo.z, v.position.z)};
                hi = Vec3{std::max(hi.x, v.position.x), std::max(hi.y, v.position.y), std::max(hi.z, v.position.z)};
            }
            physics::BoxCollider& box = child.add<physics::BoxCollider>();
            box.size = Vec3{hi.x - lo.x, hi.y - lo.y, std::max(hi.z - lo.z, 0.04f)};
            box.center = (lo + hi) * 0.5f;
        }
    }
    return root;
}

bool EditorApp::groundAt(float x, float z, float& y) {
    bool found = false;
    world_.forEachDepthFirst([&](ecs::Entity e) {
        if (found || !e.has<terrain::Terrain>()) return;
        const terrain::Terrain& comp = e.get<terrain::Terrain>();
        const Vec3 origin = e.worldPosition();
        if (x < origin.x || z < origin.z || x > origin.x + comp.size || z > origin.z + comp.size) return;
        if (auto data = terrain_store_.get(comp)) {
            y = terrain::heightAt(*data, comp, origin, x, z);
            found = true;
        }
    });
    return found;
}

int EditorApp::placeVillage(int count, std::uint32_t seed, ecs::Entity parent, std::string* error) {
    if (count <= 0) return 0;
    // El terreno (el primero con datos).
    ecs::Entity terrain_entity;
    std::shared_ptr<terrain::TerrainData> data;
    world_.forEachDepthFirst([&](ecs::Entity e) {
        if (data || !e.has<terrain::Terrain>()) return;
        if (auto d = terrain_store_.get(e.get<terrain::Terrain>())) {
            data = d;
            terrain_entity = e;
        }
    });
    if (!data) {
        if (error) *error = "no hay terreno en la escena";
        return 0;
    }
    const terrain::Terrain& comp = terrain_entity.get<terrain::Terrain>();
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
    // Llano: poca diferencia de altura en la huella de la casa.
    const auto flatness = [&](float x, float z, float radius, float& lo, float& hi) {
        lo = 1e9f;
        hi = -1e9f;
        for (int i = 0; i < 9; ++i) {
            for (int j = 0; j < 9; ++j) {
                const float y = ground(x + (static_cast<float>(i) / 8.0f - 0.5f) * 2.0f * radius,
                                       z + (static_cast<float>(j) / 8.0f - 0.5f) * 2.0f * radius);
                lo = std::min(lo, y);
                hi = std::max(hi, y);
            }
        }
        return hi - lo;
    };

    std::mt19937 rng(seed * 7919U + 17U);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const float size = comp.size;
    // --- La plaza: el sitio mas llano entre muchos candidatos ---
    Vec3 center{};
    float best = 1e9f;
    for (int i = 0; i < 1500; ++i) {
        const float x = origin.x + size * (0.12f + 0.76f * unit(rng));
        const float z = origin.z + size * (0.12f + 0.76f * unit(rng));
        const float y = ground(x, z);
        if (y < min_y || !dry(x, z, 30.0f)) continue;
        float lo = 0.0f;
        float hi = 0.0f;
        const float range = flatness(x, z, 35.0f, lo, hi);
        // Llano y no muy alto (los pueblos estan en los valles).
        const float score = range + (y - origin.y) * 0.02f;
        if (score < best) {
            best = score;
            center = Vec3{x, y, z};
        }
    }
    if (best >= 1e8f) {
        if (error) *error = "no hay sitio llano y seco para la aldea";
        return 0;
    }

    // --- Modelos: unas cuantas variantes (instancias del mismo modelo) ---
    const int variants = std::clamp(count / 2 + 1, 1, 6);
    std::vector<float> radii;
    std::vector<Uuid> uuids;
    for (int v = 0; v < variants; ++v) {
        asset::HouseStyle style = asset::HouseStyle::LogCabin;
        const float r = unit(rng);
        if (r < 0.4f) style = asset::HouseStyle::LogCabin;
        else if (r < 0.65f) style = asset::HouseStyle::TimberCabin;
        else if (r < 0.85f) style = asset::HouseStyle::StoneCottage;
        else style = asset::HouseStyle::Farmhouse;
        const asset::HouseSettings s = asset::housePreset(style, seed * 13U + static_cast<std::uint32_t>(v) + 1U);
        const Uuid uuid = writeHouseModel(s, error);
        if (!uuid.valid()) return 0;
        uuids.push_back(uuid);
        radii.push_back(std::sqrt(s.width * s.width + (s.depth + 3.0f) * (s.depth + 3.0f)) * 0.5f + 1.5f);
    }

    // --- Reparto alrededor de la plaza ---
    ecs::Entity village = world_.create(kVillageName, parent);
    village.setWorldPosition(center);
    std::vector<std::pair<Vec3, float>> placed;
    int made = 0;
    for (int attempt = 0; attempt < count * 300 && made < count; ++attempt) {
        const std::size_t v = static_cast<std::size_t>(made) % uuids.size();
        const float radius = radii[v];
        const float ring = 14.0f + 22.0f * std::sqrt(static_cast<float>(made + 1)) * (0.7f + 0.6f * unit(rng));
        const float angle = unit(rng) * 6.2831853f;
        const float x = center.x + std::cos(angle) * ring;
        const float z = center.z + std::sin(angle) * ring;
        bool free = true;
        for (const auto& [p, r] : placed) {
            const float dx = p.x - x;
            const float dz = p.z - z;
            if (dx * dx + dz * dz < (r + radius + 3.0f) * (r + radius + 3.0f)) free = false;
        }
        if (!free || !dry(x, z, radius + 6.0f)) continue;
        float lo = 0.0f;
        float hi = 0.0f;
        if (flatness(x, z, radius, lo, hi) > 1.1f || lo < min_y) continue;
        // Encima del punto mas alto menos un poco: el zocalo de piedra baja
        // hasta 0.6 m bajo el suelo de la casa.
        const float y = std::max(hi - 0.2f, lo);
        // La puerta (+Z) mira a la plaza.
        const float yaw = std::atan2(center.x - x, center.z - z) * 57.29578f + (unit(rng) - 0.5f) * 20.0f;
        ecs::Entity house = placeHouse(uuids[v], Vec3{x, y, z}, yaw, village);
        if (!house.valid()) continue;
        placed.push_back({Vec3{x, y, z}, radius});
        ++made;
    }

    // Claros en la vegetacion (los arboles no crecen dentro de las casas).
    for (const entt::entity h : world_.registry().view<foliage::Foliage>()) {
        foliage::Foliage& f = world_.registry().get<foliage::Foliage>(h);
        for (const auto& [p, r] : placed) f.clearings.push_back(foliage::FoliageClearing{p, r + 3.0f});
        f.clearings.push_back(foliage::FoliageClearing{center, 12.0f});
    }
    std::cout << "[Casas] Aldea con " << made << " casas (" << uuids.size() << " modelos)" << std::endl;
    return made;
}

void EditorApp::drawHouseGeneratorWindow() {
    if (!show_house_generator_) return;
    ImGui::SetNextWindowSize(ImVec2(420.0f, 560.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Generador de casas", &show_house_generator_)) {
        ImGui::End();
        return;
    }
    asset::HouseSettings& s = house_gen_;
    ImGui::TextWrapped("Casas y cabañas procedurales listas para el juego: pocos triángulos, 8 materiales PBR "
                       "compartidos (con relieve), colisión y la puerta aparte (gira en Y).");
    ImGui::Spacing();
    int style = static_cast<int>(s.style);
    const char* styles[asset::kHouseStyleCount] = {"Cabaña de troncos", "Cabaña de tablas", "Casita de piedra",
                                                   "Casa de campo"};
    ImGui::SetNextItemWidth(-150.0f);
    if (ImGui::Combo("Estilo", &style, styles, asset::kHouseStyleCount)) {
        s = asset::housePreset(static_cast<asset::HouseStyle>(style), s.seed);
    }
    int seed = static_cast<int>(s.seed);
    ImGui::SetNextItemWidth(-190.0f);
    if (ImGui::InputInt("Semilla", &seed)) s.seed = static_cast<std::uint32_t>(std::max(seed, 0));
    ImGui::SameLine();
    if (ImGui::Button("Al azar")) s = asset::housePreset(s.style, std::random_device{}() % 100000U);
    ImGui::SetItemTooltip("Otras medidas y otra distribución de las ventanas.");

    const auto slider = [](const char* label, float* v, float lo, float hi, const char* fmt) {
        ImGui::SetNextItemWidth(-150.0f);
        ImGui::SliderFloat(label, v, lo, hi, fmt);
    };
    if (ImGui::CollapsingHeader("Medidas", ImGuiTreeNodeFlags_DefaultOpen)) {
        slider("Ancho (m)", &s.width, 4.0f, 16.0f, "%.1f");
        slider("Fondo (m)", &s.depth, 3.5f, 12.0f, "%.1f");
        ImGui::SetNextItemWidth(-150.0f);
        ImGui::SliderInt("Plantas", &s.floors, 1, 2);
        slider("Altura por planta (m)", &s.wall_height, 2.3f, 3.5f, "%.2f");
        slider("Pendiente del tejado", &s.roof_pitch, 15.0f, 55.0f, "%.0f°");
        slider("Alero (m)", &s.roof_overhang, 0.2f, 1.2f, "%.2f");
        ImGui::SetNextItemWidth(-150.0f);
        ImGui::SliderInt("Ventanas delante", &s.windows, -1, 8);
        ImGui::SetItemTooltip("-1 = según el ancho.");
    }
    if (ImGui::CollapsingHeader("Detalles", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Porche con barandilla", &s.porch);
        ImGui::Checkbox("Chimenea", &s.chimney);
        ImGui::Checkbox("Contraventanas", &s.shutters);
    }
    ImGui::Separator();
    ImGui::BeginDisabled(!has_project_ || playing());
    if (ImGui::Button("Crear en la escena", ImVec2(-1.0f, 34.0f))) {
        std::string error;
        const Uuid uuid = writeHouseModel(s, &error);
        if (!uuid.valid()) {
            std::cerr << "[Casas] " << error << std::endl;
        } else {
            // Delante de la camara, sobre el suelo si hay terreno.
            Vec3 pos = scene_.camera().position() + scene_.camera().forward() * 18.0f;
            float y = 0.0f;
            pos.y = groundAt(pos.x, pos.z, y) ? y : 0.0f;
            ecs::Entity house = placeHouse(uuid, pos, 0.0f, {});
            if (house.valid()) {
                selectOnly(house.uuid());
                revealInHierarchy(house.uuid());
                commit();
            }
        }
    }
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-150.0f);
    ImGui::SliderInt("Casas de la aldea", &house_gen_village_, 1, 40);
    if (ImGui::Button("Crear aldea en el terreno", ImVec2(-1.0f, 30.0f))) {
        std::string error;
        if (ecs::Entity old = world_.findByName(kVillageName); old.valid()) world_.destroy(old);
        ecs::Entity parent = world_.findByName("Mundo generado");
        const int made = placeVillage(house_gen_village_, s.seed, parent, &error);
        if (made == 0) std::cerr << "[Casas] " << error << std::endl;
        else commit();
    }
    ImGui::SetItemTooltip("En un sitio llano y seco del terreno, mirando a la plaza. Reemplaza la aldea anterior.");
    ImGui::EndDisabled();
    ImGui::End();
}

}  // namespace cramion::editor
