// VulkanRenderer: la escena del trazado de rayos (RayTracing.h). Aparte para
// no engordar VulkanRenderer.cpp.
//
// Ademas de los modelos, los rayos ven el terreno y los arboles. Sin ellos,
// en exteriores los rayos de la GI y de los reflejos atravesaban el suelo y
// veian cielo por debajo: la luz rebotada salia plana y demasiado clara, el
// sotobosque no se oscurecia bajo las copas y el agua no reflejaba las
// montanas.
//
//   - Terreno: una malla de hasta 512 x 512 celdas sacada de las alturas,
//     siempre POR DEBAJO de la que dibuja la camara (si quedara por encima en
//     un valle, los rayos que salen del suelo chocarian con ella: manchas
//     negras). Color: la mezcla de las capas horneada en una textura.
//     Esculpir o pintar la actualiza en su sitio, sin parar la GPU.
//   - Arboles: la malla media de cada especie (hojas recortadas por alfa con
//     su textura) y una instancia por arbol a menos de kTreeRadius metros.
//
// Nada de esto se construye con el trazado apagado.

#include "CramionFX/vk/VulkanRenderer.h"

#include "CramionFX/scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <unordered_map>

namespace cramion::gfx {
namespace {

constexpr std::uint32_t kTerrainMaxCells = 512;
constexpr std::uint32_t kTerrainAlbedoMax = 1024;
constexpr float kTreeRadius = 350.0f;          // m: arboles en la escena de rayos
constexpr std::size_t kTreeCap = 40000;        // como mucho (los mas cercanos)
constexpr float kTreeReselect = 25.0f;         // m que se mueve la camara antes de volver a elegir
constexpr float kTreeCell = 64.0f;             // m: rejilla para elegirlos rapido
constexpr auto kTerrainUpdateInterval = std::chrono::milliseconds(250);

float sampleHeight(const TerrainPass::RayTracingSource& s, std::int64_t x, std::int64_t y) {
    const auto res = static_cast<std::int64_t>(s.resolution);
    x = std::clamp<std::int64_t>(x, 0, res - 1);
    y = std::clamp<std::int64_t>(y, 0, res - 1);
    return s.heights[static_cast<std::size_t>(y * res + x)] * s.max_height;
}

// Vertices de la malla del terreno para los rayos: `cells` x `cells` celdas
// sobre todo el terreno (posicion relativa a su esquina).
//
// Cada vertice empieza a la altura de su texel; despues, cada triangulo se
// compara con los texeles que cubre y, si en alguno queda por encima de la
// superficie fina (en los valles y cauces: la cuerda pasa por arriba), sus
// tres vertices bajan lo que sobresale. En una ladera plana no baja nada (la
// interpolacion es exacta) y en una cima tampoco (la cuerda ya va por debajo).
std::vector<RayTracing::Vertex> terrainVertices(const TerrainPass::RayTracingSource& s, std::uint32_t cells) {
    const std::uint32_t n = cells + 1;
    const float texels_per_cell = static_cast<float>(s.resolution - 1) / static_cast<float>(cells);
    std::vector<float> heights(static_cast<std::size_t>(n) * n);
    const auto bilinear = [&](float tx, float ty) {
        const auto x0 = static_cast<std::int64_t>(std::floor(tx));
        const auto y0 = static_cast<std::int64_t>(std::floor(ty));
        const float fx = tx - static_cast<float>(x0);
        const float fy = ty - static_cast<float>(y0);
        const float h00 = sampleHeight(s, x0, y0);
        const float h10 = sampleHeight(s, x0 + 1, y0);
        const float h01 = sampleHeight(s, x0, y0 + 1);
        const float h11 = sampleHeight(s, x0 + 1, y0 + 1);
        return (h00 * (1.0f - fx) + h10 * fx) * (1.0f - fy) + (h01 * (1.0f - fx) + h11 * fx) * fy;
    };
    for (std::uint32_t j = 0; j < n; ++j) {
        for (std::uint32_t i = 0; i < n; ++i) {
            heights[static_cast<std::size_t>(j) * n + i] =
                bilinear(static_cast<float>(i) * texels_per_cell, static_cast<float>(j) * texels_per_cell);
        }
    }
    // Cuanto baja cada vertice: lo que mas sobresalga cualquiera de sus triangulos.
    std::vector<float> lower(heights.size(), 0.0f);
    const auto res = s.resolution;
    for (std::uint32_t ty = 0; ty < res; ++ty) {
        const float cy = static_cast<float>(ty) / texels_per_cell;
        const std::uint32_t j = std::min(static_cast<std::uint32_t>(cy), cells - 1);
        const float fy = cy - static_cast<float>(j);
        for (std::uint32_t tx = 0; tx < res; ++tx) {
            const float cx = static_cast<float>(tx) / texels_per_cell;
            const std::uint32_t i = std::min(static_cast<std::uint32_t>(cx), cells - 1);
            const float fx = cx - static_cast<float>(i);
            const std::size_t v00 = static_cast<std::size_t>(j) * n + i;
            const std::size_t v10 = v00 + 1;
            const std::size_t v01 = v00 + n;
            const std::size_t v11 = v01 + 1;
            // Diagonal de v10 a v01 (como los indices).
            float interpolated;
            std::size_t a;
            std::size_t b;
            std::size_t c;
            if (fx + fy <= 1.0f) {
                interpolated = heights[v00] + (heights[v10] - heights[v00]) * fx + (heights[v01] - heights[v00]) * fy;
                a = v00;
                b = v10;
                c = v01;
            } else {
                interpolated = heights[v11] + (heights[v01] - heights[v11]) * (1.0f - fx) +
                               (heights[v10] - heights[v11]) * (1.0f - fy);
                a = v11;
                b = v01;
                c = v10;
            }
            const float excess = interpolated - s.heights[static_cast<std::size_t>(ty) * res + tx] * s.max_height;
            if (excess > 0.0f) {
                lower[a] = std::max(lower[a], excess);
                lower[b] = std::max(lower[b], excess);
                lower[c] = std::max(lower[c], excess);
            }
        }
    }
    // 2 cm mas: nunca empatados con la superficie que se dibuja.
    for (std::size_t v = 0; v < heights.size(); ++v) heights[v] -= lower[v] + 0.02f;

    const float spacing = s.size / static_cast<float>(cells);
    std::vector<RayTracing::Vertex> vertices(heights.size());
    for (std::uint32_t j = 0; j < n; ++j) {
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto at = [&](std::int64_t x, std::int64_t y) {
                x = std::clamp<std::int64_t>(x, 0, cells);
                y = std::clamp<std::int64_t>(y, 0, cells);
                return heights[static_cast<std::size_t>(y) * n + static_cast<std::size_t>(x)];
            };
            const float hl = at(static_cast<std::int64_t>(i) - 1, j);
            const float hr = at(static_cast<std::int64_t>(i) + 1, j);
            const float hd = at(i, static_cast<std::int64_t>(j) - 1);
            const float hu = at(i, static_cast<std::int64_t>(j) + 1);
            const core::Vec3 normal = core::normalize(core::Vec3{hl - hr, 2.0f * spacing, hd - hu});
            const float u = static_cast<float>(i) / static_cast<float>(cells);
            const float v = static_cast<float>(j) / static_cast<float>(cells);
            RayTracing::Vertex& out = vertices[static_cast<std::size_t>(j) * n + i];
            out = RayTracing::Vertex{u * s.size, heights[static_cast<std::size_t>(j) * n + i], v * s.size,
                                     normal.x, normal.y, normal.z, u, v};
        }
    }
    return vertices;
}

std::vector<std::uint32_t> terrainIndices(std::uint32_t cells) {
    const std::uint32_t n = cells + 1;
    std::vector<std::uint32_t> indices;
    indices.reserve(static_cast<std::size_t>(cells) * cells * 6);
    for (std::uint32_t j = 0; j < cells; ++j) {
        for (std::uint32_t i = 0; i < cells; ++i) {
            const std::uint32_t v00 = j * n + i;
            const std::uint32_t v10 = v00 + 1;
            const std::uint32_t v01 = v00 + n;
            const std::uint32_t v11 = v01 + 1;
            indices.insert(indices.end(), {v00, v01, v10, v10, v01, v11});
        }
    }
    return indices;
}

// Color del terreno (sRGB, como terrain.frag) horneado: la mezcla de las capas
// por sus pesos, con todos sus mips.
std::vector<std::vector<std::uint8_t>> terrainAlbedo(const TerrainPass::RayTracingSource& s, std::uint32_t size) {
    std::vector<std::vector<std::uint8_t>> mips;
    std::vector<std::uint8_t> base(static_cast<std::size_t>(size) * size * 4, 255);
    const std::uint32_t splat = s.splat_resolution;
    const std::uint32_t layers = std::max(s.layer_count, 1u);
    for (std::uint32_t y = 0; y < size; ++y) {
        const auto sy = std::min(static_cast<std::uint32_t>((static_cast<float>(y) + 0.5f) / size * splat), splat - 1);
        for (std::uint32_t x = 0; x < size; ++x) {
            const auto sx = std::min(static_cast<std::uint32_t>((static_cast<float>(x) + 0.5f) / size * splat), splat - 1);
            const std::size_t at = (static_cast<std::size_t>(sy) * splat + sx) * 4;
            float weights[kMaxTerrainLayers];
            for (std::uint32_t c = 0; c < 4; ++c) {
                weights[c] = s.splat0[at + c] / 255.0f;
                weights[4 + c] = s.splat1[at + c] / 255.0f;
            }
            float total = 0.0f;
            for (std::uint32_t l = 0; l < kMaxTerrainLayers; ++l) {
                if (l >= layers) weights[l] = 0.0f;
                total += weights[l];
            }
            if (total < 1e-4f) {
                weights[0] = 1.0f;
                total = 1.0f;
            }
            core::Vec3 color{};
            for (std::uint32_t l = 0; l < layers; ++l) color = color + s.layer_color[l] * (weights[l] / total);
            // Donde crece la hierba de la GPU se ven sus briznas, mas oscuras que
            // la textura del suelo: con la textura, la luz rebotada del prado
            // salia verde lima en los troncos y paredes.
            if (s.grass && s.grass_layer < layers) {
                const float share = weights[s.grass_layer] / total;
                const float t = std::clamp((share - s.grass_threshold) / 0.25f, 0.0f, 1.0f);
                const float coverage = t * t * (3.0f - 2.0f * t);
                float dry = 0.0f;
                if (s.grass_dry_layer >= 0 && static_cast<std::uint32_t>(s.grass_dry_layer) < layers) {
                    dry = std::clamp(weights[s.grass_dry_layer] / total * 2.0f, 0.0f, 1.0f);
                }
                const core::Vec3 blades = s.grass_color * (1.0f - dry) + s.grass_dry_color * dry;
                color = color * (1.0f - coverage * 0.9f) + blades * (coverage * 0.9f);
            }
            std::uint8_t* out = base.data() + (static_cast<std::size_t>(y) * size + x) * 4;
            out[0] = static_cast<std::uint8_t>(std::clamp(color.x, 0.0f, 1.0f) * 255.0f + 0.5f);
            out[1] = static_cast<std::uint8_t>(std::clamp(color.y, 0.0f, 1.0f) * 255.0f + 0.5f);
            out[2] = static_cast<std::uint8_t>(std::clamp(color.z, 0.0f, 1.0f) * 255.0f + 0.5f);
            out[3] = 255;
        }
    }
    mips.push_back(std::move(base));
    std::uint32_t level = size;
    while (level > 1) {
        const std::uint32_t next = level / 2;
        const std::vector<std::uint8_t>& previous = mips.back();
        std::vector<std::uint8_t> reduced(static_cast<std::size_t>(next) * next * 4);
        for (std::uint32_t y = 0; y < next; ++y) {
            for (std::uint32_t x = 0; x < next; ++x) {
                for (std::uint32_t c = 0; c < 4; ++c) {
                    const auto texel = [&](std::uint32_t px, std::uint32_t py) {
                        return static_cast<std::uint32_t>(previous[(static_cast<std::size_t>(py) * level + px) * 4 + c]);
                    };
                    const std::uint32_t sum = texel(2 * x, 2 * y) + texel(2 * x + 1, 2 * y) +
                                              texel(2 * x, 2 * y + 1) + texel(2 * x + 1, 2 * y + 1);
                    reduced[(static_cast<std::size_t>(y) * next + x) * 4 + c] = static_cast<std::uint8_t>((sum + 2) / 4);
                }
            }
        }
        mips.push_back(std::move(reduced));
        level = next;
    }
    return mips;
}

std::uint32_t terrainAlbedoSize(const TerrainPass::RayTracingSource& s) {
    std::uint32_t size = 1;
    while (size * 2 <= std::min(s.splat_resolution, kTerrainAlbedoMax)) size *= 2;
    return size;
}

bool leafLayer(std::uint32_t layer) {
    return layer == asset::kTreeLayerLeaves || layer == asset::kTreeLayerNeedles || layer == asset::kTreeLayerFrond ||
           layer == asset::kTreeLayerBirchLeaves || layer == asset::kTreeLayerWillowLeaves ||
           layer == asset::kTreeLayerFirNeedles || layer == asset::kTreeLayerPineShoot;
}

// La malla media de una especie para los rayos: un material por capa de
// textura (corteza opaca, hojas recortadas por alfa). El tinte por vertice de
// foliage.frag (sRGB, 0.5 = la textura tal cual) se queda en su media.
RayTracing::ExtraMesh treeMesh(const asset::TreeMeshData& tree, std::uint32_t layer_texture_base,
                               std::uint32_t layer_count) {
    RayTracing::ExtraMesh mesh;
    mesh.vertices.reserve(tree.vertices.size());
    for (const asset::TreeVertex& v : tree.vertices) {
        mesh.vertices.push_back(RayTracing::Vertex{v.px, v.py, v.pz, v.nx, v.ny, v.nz, v.u, v.v});
    }
    mesh.indices = tree.indices;
    std::vector<std::int32_t> material_of_layer(layer_count, -1);
    std::vector<core::Vec3> tint_sum;
    std::vector<float> tint_count;
    const std::size_t triangles = tree.indices.size() / 3;
    mesh.triangle_materials.resize(triangles, 0);
    for (std::size_t t = 0; t < triangles; ++t) {
        const asset::TreeVertex& first = tree.vertices[tree.indices[t * 3]];
        const auto layer = std::min(static_cast<std::uint32_t>(first.layer + 0.5f), layer_count - 1);
        if (material_of_layer[layer] < 0) {
            material_of_layer[layer] = static_cast<std::int32_t>(mesh.materials.size());
            RayTracing::ExtraMaterial material;
            material.alpha_masked = leafLayer(layer);
            material.roughness = material.alpha_masked ? 0.6f : 0.85f;
            material.texture = static_cast<std::int32_t>(layer_texture_base + layer);
            mesh.materials.push_back(material);
            tint_sum.push_back(core::Vec3{});
            tint_count.push_back(0.0f);
        }
        const auto m = static_cast<std::uint32_t>(material_of_layer[layer]);
        mesh.triangle_materials[t] = m;
        for (std::size_t k = 0; k < 3; ++k) {
            const std::uint32_t color = tree.vertices[tree.indices[t * 3 + k]].color;
            tint_sum[m] = tint_sum[m] + core::Vec3{static_cast<float>(color & 0xFFu), static_cast<float>((color >> 8) & 0xFFu),
                                                   static_cast<float>((color >> 16) & 0xFFu)} *
                                            (2.0f / 255.0f);
            tint_count[m] += 1.0f;
        }
    }
    for (std::size_t m = 0; m < mesh.materials.size(); ++m) {
        const core::Vec3 tint = tint_count[m] > 0.0f ? tint_sum[m] * (1.0f / tint_count[m]) : core::Vec3{1.0f, 1.0f, 1.0f};
        // foliage.frag multiplica el color sRGB por el tinte; aqui la textura
        // ya va a lineal: el tinte, tambien.
        mesh.materials[m].base_color = core::Vec4{std::pow(std::max(tint.x, 0.0f), 2.2f), std::pow(std::max(tint.y, 0.0f), 2.2f),
                                                  std::pow(std::max(tint.z, 0.0f), 2.2f), 1.0f};
    }
    return mesh;
}

std::uint64_t treeCellKey(std::int64_t x, std::int64_t z) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(z);
}

}  // namespace

void VulkanRenderer::buildRayTracingScene(const scene::Scene& scene) {
    const auto started = std::chrono::steady_clock::now();
    device_.waitIdle();
    std::vector<const asset::ModelData*> models;
    // Los que salieron de la GPU (streaming) van vacios: no tienen texturas
    // ni materiales que enlazar.
    static const asset::ModelData kEvicted{};
    for (std::uint32_t i = 0; i < scene.models().size(); ++i) {
        models.push_back(modelResident(i) ? scene.models()[i].get() : &kEvicted);
    }
    // uploadModels/uploadModel los mantienen a la par; por si acaso.
    models.resize(std::min(models.size(), skinned_models_.size()));

    RayTracing::ExtraScene extra;
    rt_terrains_.clear();
    std::vector<RtTerrainCache> cache;
    cache.swap(rt_terrain_cache_);
    std::vector<TerrainPass::RayTracingSource> sources = terrain_pass_.rayTracingSources();
    std::sort(sources.begin(), sources.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    for (const TerrainPass::RayTracingSource& source : sources) {
        if (source.resolution < 3 || source.splat_resolution < 2 || source.size <= 0.0f) continue;
        RtTerrain terrain;
        terrain.id = source.id;
        terrain.resolution = source.resolution;
        terrain.splat_resolution = source.splat_resolution;
        terrain.cells = std::min(kTerrainMaxCells, source.resolution - 1);
        terrain.texture_size = terrainAlbedoSize(source);
        terrain.height_revision = source.height_revision;
        terrain.look_revision = source.look_revision;
        terrain.extra = static_cast<std::uint32_t>(extra.meshes.size());
        terrain.texture = static_cast<std::uint32_t>(extra.textures.size());

        // Lo calculado la vez anterior, si el terreno no cambio.
        const auto cached = std::find_if(cache.begin(), cache.end(), [&](const RtTerrainCache& c) {
            return c.id == source.id && c.cells == terrain.cells && c.texture_size == terrain.texture_size;
        });
        RtTerrainCache entry;
        entry.id = source.id;
        entry.cells = terrain.cells;
        entry.texture_size = terrain.texture_size;
        entry.height_revision = source.height_revision;
        entry.look_revision = source.look_revision;
        if (cached != cache.end() && cached->height_revision == source.height_revision) {
            entry.vertices = std::move(cached->vertices);
        } else {
            entry.vertices = terrainVertices(source, terrain.cells);
        }
        if (cached != cache.end() && cached->look_revision == source.look_revision) {
            entry.albedo = std::move(cached->albedo);
        } else {
            entry.albedo = terrainAlbedo(source, terrain.texture_size);
        }

        RayTracing::ExtraMesh mesh;
        mesh.vertices = entry.vertices;
        mesh.indices = terrainIndices(terrain.cells);
        mesh.triangle_materials.assign(mesh.indices.size() / 3, 0);
        RayTracing::ExtraMaterial material;
        material.roughness = 0.9f;
        material.texture = static_cast<std::int32_t>(terrain.texture);
        mesh.materials.push_back(material);
        mesh.updatable = true;
        extra.meshes.push_back(std::move(mesh));

        RayTracing::ExtraTexture texture;
        texture.size = terrain.texture_size;
        texture.mips = entry.albedo;
        extra.textures.push_back(std::move(texture));
        rt_terrains_.push_back(terrain);
        rt_terrain_cache_.push_back(std::move(entry));
    }

    rt_species_extra_.fill(-1);
    if (foliage_pass_.albedoLayers() > 0) {
        const auto layer_base = static_cast<std::uint32_t>(extra.textures.size());
        for (std::uint32_t layer = 0; layer < foliage_pass_.albedoLayers(); ++layer) {
            RayTracing::ExtraTexture texture;
            texture.image = foliage_pass_.albedoImage();
            texture.format = vk::Format::eR8G8B8A8Srgb;
            texture.layer = layer;
            texture.mip_count = foliage_pass_.albedoMips();
            extra.textures.push_back(texture);
        }
        for (std::uint32_t s = 0; s < FoliagePass::kSpecies; ++s) {
            const asset::TreeMeshData& tree = foliage_pass_.rayTracingMesh(s);
            if (tree.indices.empty()) continue;
            rt_species_extra_[s] = static_cast<std::int32_t>(extra.meshes.size());
            extra.meshes.push_back(treeMesh(tree, layer_base, foliage_pass_.albedoLayers()));
        }
    }
    rt_species_revision_ = foliage_pass_.speciesRevision();

    ray_tracing_.build(device_, models, skinned_models_, ibl_probe_.irradianceBuffer(), extra);
    rt_scene_evicted_ = model_evicted_;
    rt_scene_evicted_.resize(models.size(), false);
    rt_extra_base_ = static_cast<std::uint32_t>(models.size());
    ray_traced_models_ = static_cast<std::uint32_t>(models.size());
    ray_pinned_.assign(skinned_models_.size(), false);
    ray_pinned_models_.clear();
    rt_scene_dirty_ = false;
    rt_model_edit_pending_ = false;
    rt_trees_valid_ = false;
    path_tracing_reset_ = true;
    std::cout << "[Vulkan] Escena de rayos: " << models.size() << " modelos, " << rt_terrains_.size() << " terreno(s), "
              << (foliage_pass_.albedoLayers() > 0 ? FoliagePass::kSpecies : 0u) << " especies de arboles en "
              << std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count()
              << " ms\n";
}

void VulkanRenderer::prepareRayTracingScene(const scene::Scene& scene, std::vector<RayTracing::Instance>& instances) {
    const auto now = std::chrono::steady_clock::now();
    // Modelos cambiados con uploadModel: los rayos los ven al poco (rehacer la
    // escena entera es un tiron: no mientras se editan).
    if (rt_model_edit_pending_ && now - rt_model_edit_time_ > std::chrono::seconds(1)) rt_scene_dirty_ = true;
    // Terrenos nuevos, quitados o con otra resolucion; arboles regenerados.
    std::vector<TerrainPass::RayTracingSource> sources = terrain_pass_.rayTracingSources();
    std::sort(sources.begin(), sources.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    std::size_t usable = 0;
    for (const auto& source : sources) usable += source.resolution >= 3 && source.splat_resolution >= 2 && source.size > 0.0f;
    bool terrains_changed = usable != rt_terrains_.size();
    for (std::size_t i = 0, k = 0; !terrains_changed && i < sources.size(); ++i) {
        const auto& source = sources[i];
        if (source.resolution < 3 || source.splat_resolution < 2 || source.size <= 0.0f) continue;
        const RtTerrain& known = rt_terrains_[k++];
        terrains_changed = known.id != source.id || known.resolution != source.resolution ||
                           known.splat_resolution != source.splat_resolution;
    }
    if (terrains_changed || foliage_pass_.speciesRevision() != rt_species_revision_) rt_scene_dirty_ = true;
    if (rt_scene_dirty_) buildRayTracingScene(scene);
    if (!ray_tracing_.sceneBuilt()) return;

    // --- Terrenos: esculpido o pintado en su sitio (como mucho 4 veces por
    // segundo), y su instancia ---
    for (RtTerrain& terrain : rt_terrains_) {
        const auto it = std::find_if(sources.begin(), sources.end(), [&](const auto& s) { return s.id == terrain.id; });
        if (it == sources.end()) continue;
        const TerrainPass::RayTracingSource& source = *it;
        if (source.height_revision != terrain.height_revision && now - terrain.last_update > kTerrainUpdateInterval) {
            ray_tracing_.updateExtraVertices(terrain.extra, terrainVertices(source, terrain.cells));
            terrain.height_revision = source.height_revision;
            terrain.last_update = now;
        }
        if (source.look_revision != terrain.look_revision && now - terrain.last_texture > kTerrainUpdateInterval) {
            ray_tracing_.updateExtraTexture(terrain.texture, terrainAlbedo(source, terrain.texture_size));
            terrain.look_revision = source.look_revision;
            terrain.last_texture = now;
        }
        if (source.visible) {
            instances.push_back(RayTracing::Instance{rt_extra_base_ + terrain.extra, core::translate(source.origin),
                                                     RayTracing::kMaskTerrain});
        }
    }

    // --- Arboles cercanos ---
    const std::vector<FoliageInstance>& trees = foliage_pass_.cpuInstances();
    const core::Vec3 camera = scene.camera().position();
    const core::Vec3 origin = foliage_pass_.origin();
    if (foliage_pass_.instancesRevision() != rt_tree_grid_revision_) {
        // Rejilla de las instancias (para no recorrer millones al moverse).
        rt_tree_grid_.clear();
        for (std::uint32_t i = 0; i < trees.size(); ++i) {
            const auto cx = static_cast<std::int64_t>(std::floor(trees[i].x / kTreeCell));
            const auto cz = static_cast<std::int64_t>(std::floor(trees[i].z / kTreeCell));
            rt_tree_grid_[treeCellKey(cx, cz)].push_back(i);
        }
        rt_tree_grid_revision_ = foliage_pass_.instancesRevision();
        rt_trees_valid_ = false;
    }
    const bool moved = core::length(camera - rt_tree_center_) > kTreeReselect ||
                       core::length(origin - rt_tree_origin_) > 1e-3f;
    if (!rt_trees_valid_ || moved) {
        rt_tree_instances_.clear();
        rt_tree_center_ = camera;
        rt_tree_origin_ = origin;
        rt_trees_valid_ = true;
        // En coordenadas absolutas (las de las instancias).
        const core::Vec3 center = camera + origin;
        std::vector<std::pair<float, std::uint32_t>> candidates;
        const auto c0x = static_cast<std::int64_t>(std::floor((center.x - kTreeRadius) / kTreeCell));
        const auto c1x = static_cast<std::int64_t>(std::floor((center.x + kTreeRadius) / kTreeCell));
        const auto c0z = static_cast<std::int64_t>(std::floor((center.z - kTreeRadius) / kTreeCell));
        const auto c1z = static_cast<std::int64_t>(std::floor((center.z + kTreeRadius) / kTreeCell));
        for (std::int64_t cx = c0x; cx <= c1x; ++cx) {
            for (std::int64_t cz = c0z; cz <= c1z; ++cz) {
                const auto cell = rt_tree_grid_.find(treeCellKey(cx, cz));
                if (cell == rt_tree_grid_.end()) continue;
                for (const std::uint32_t i : cell->second) {
                    const float dx = trees[i].x - center.x;
                    const float dz = trees[i].z - center.z;
                    const float d2 = dx * dx + dz * dz;
                    if (d2 < kTreeRadius * kTreeRadius) candidates.emplace_back(d2, i);
                }
            }
        }
        if (candidates.size() > kTreeCap) {
            std::nth_element(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(kTreeCap), candidates.end());
            candidates.resize(kTreeCap);
        }
        for (const auto& [distance, i] : candidates) {
            const FoliageInstance& tree = trees[i];
            const std::uint32_t species = std::min((tree.packed >> 18) & 3u, FoliagePass::kSpecies - 1);
            if (rt_species_extra_[species] < 0) continue;
            const float yaw = static_cast<float>(tree.packed & 1023u) * (6.2831853f / 1024.0f);
            const float scale = 0.25f + static_cast<float>((tree.packed >> 10) & 255u) * (3.75f / 255.0f);
            const float c = std::cos(yaw) * scale;
            const float s = std::sin(yaw) * scale;
            // Como foliage.vert: x' = c x + s z, z' = -s x + c z (por columnas).
            core::Mat4 transform = core::Mat4::identity();
            transform.m[0][0] = c;
            transform.m[0][2] = -s;
            transform.m[1][1] = scale;
            transform.m[2][0] = s;
            transform.m[2][2] = c;
            transform.m[3][0] = tree.x - origin.x;
            transform.m[3][1] = tree.y - origin.y;
            transform.m[3][2] = tree.z - origin.z;
            rt_tree_instances_.push_back(RayTracing::Instance{
                rt_extra_base_ + static_cast<std::uint32_t>(rt_species_extra_[species]), transform,
                RayTracing::kMaskFoliage});
        }
    }
    instances.insert(instances.end(), rt_tree_instances_.begin(), rt_tree_instances_.end());
}

}  // namespace cramion::gfx
