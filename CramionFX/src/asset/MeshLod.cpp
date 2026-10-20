// LODs automaticos de los modelos estaticos (como el "auto LOD" de Unreal o un
// LOD Group de Unity generado solo): cada nivel con ~la mitad de triangulos
// que el anterior, con meshoptimizer. El renderizador elige el nivel de cada
// objeto por el error en pixeles que tendria en pantalla (ver
// VulkanRenderer::chooseLod), asi que no hay distancias que ajustar.

#include "CramionFX/asset/Model.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <string>
#include <unordered_map>

namespace cramion::asset {
namespace {

// Por debajo de esto no merece la pena: el modelo ya es barato.
constexpr std::size_t kMinModelTriangles = 4096;
// Un material con menos triangulos no se sigue simplificando.
constexpr std::size_t kMinGroupTriangles = 64;
constexpr std::size_t kMaxLods = 5;
// Modelos con esqueleto (personajes): el error que puede tener cada nivel,
// en fraccion del tamano del modelo (meshopt_simplifyScale). Para uno de 2 m:
// 5 mm, 1,2 cm, 2,8 cm, 6 cm y 12 cm. Sin tope (como los estaticos), el
// nivel 1 ya llegaba a la mitad de triangulos a cualquier precio y con Prune
// desaparecian los cuernos, el arco, la cara o las botas a 10-15 m; el
// "sloppy" (agrupa vertices por celdas) juntaba piezas distintas que el
// esqueleto mueve por separado. CRAMION_SKINNED_LOD=old: como antes (A/B).
constexpr float kSkinnedLevelError[kMaxLods] = {0.0025f, 0.006f, 0.014f, 0.03f, 0.06f};

bool oldSkinnedLods() {
    static const bool old = [] {
        const char* v = std::getenv("CRAMION_SKINNED_LOD");
        return v != nullptr && std::strcmp(v, "old") == 0;
    }();
    return old;
}

// Atributos de los rigidos en meshopt_simplifyWithAttributes: normal (3) y UV
// (2). CRAMION_RIGID_LOD=old: solo la forma (meshopt_simplify), como antes.
constexpr std::size_t kRigidAttributes = 5;
bool oldRigidLods() {
    static const bool old = [] {
        const char* v = std::getenv("CRAMION_RIGID_LOD");
        return v != nullptr && std::strcmp(v, "old") == 0;
    }();
    return old;
}

// LODs a mano (setManualLods): error de un nivel que no viene de
// meshoptimizer, en fraccion del tamano del modelo, por cada vez que reduce
// los triangulos de LOD0 (con la mitad: 0,25 %, como el primer nivel de un
// personaje; con la octava parte: 1 %). Sin medirlo contra LOD0: el artista
// ya decidio que se parece; solo hace falta un orden y una distancia
// razonables para chooseLod.
constexpr float kManualLodErrorPerRatio = 0.00125f;

// --- Materiales con recorte por alfa (follaje) ---

bool lowerContains(const std::string& text, std::initializer_list<const char*> words) {
    std::string l(text.size(), '\0');
    std::transform(text.begin(), text.end(), l.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* w : words) {
        if (l.find(w) != std::string::npos) return true;
    }
    return false;
}

// La cabecera de un PNG/TGA/DDS dice si la imagen lleva alfa (sin
// decodificarla: al generar los LODs las texturas aun estan sin leer). -1 = no
// se sabe por la cabecera.
int headerAlpha(const std::uint8_t* b, std::size_t size) {
    // PNG: tipo de color 4 (gris + alfa) o 6 (RGBA) en el IHDR, o un tRNS
    // (paleta o color transparente) antes de los datos.
    if (size >= 33 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') {
        if (b[25] == 4 || b[25] == 6) return 1;
        for (std::size_t i = 8; i + 8 <= size;) {
            const std::uint32_t length = (std::uint32_t{b[i]} << 24) | (std::uint32_t{b[i + 1]} << 16) |
                                         (std::uint32_t{b[i + 2]} << 8) | std::uint32_t{b[i + 3]};
            if (std::memcmp(b + i + 4, "tRNS", 4) == 0) return 1;
            if (std::memcmp(b + i + 4, "IDAT", 4) == 0) return 0;
            i += 12 + static_cast<std::size_t>(length);
        }
        return 0;
    }
    // JPEG: nunca.
    if (size >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return 0;
    // DDS: DXT2-5 o BC7 (DX10) se eligen casi siempre por el alfa; DXT1, no.
    if (size >= 88 && std::memcmp(b, "DDS ", 4) == 0) {
        const std::uint8_t* four_cc = b + 84;
        if (std::memcmp(four_cc, "DXT1", 4) == 0) return 0;
        if (std::memcmp(four_cc, "DXT2", 4) == 0 || std::memcmp(four_cc, "DXT3", 4) == 0 ||
            std::memcmp(four_cc, "DXT4", 4) == 0 || std::memcmp(four_cc, "DXT5", 4) == 0) {
            return 1;
        }
        if (std::memcmp(four_cc, "DX10", 4) == 0 && size >= 132) {
            const std::uint32_t format = std::uint32_t{b[128]} | (std::uint32_t{b[129]} << 8) |
                                         (std::uint32_t{b[130]} << 16) | (std::uint32_t{b[131]} << 24);
            return format == 98 || format == 99 ? 1 : 0;  // BC7 (UNORM / SRGB)
        }
        return -1;
    }
    // TGA (sin firma al principio): 32 bits o bits de alfa en el descriptor.
    if (size >= 18 && (b[2] == 2 || b[2] == 10) && (b[16] == 32 || (b[17] & 0x0F) != 0)) return 1;
    return -1;
}

// El material recorta por alfa (hojas, cesped, rejas: cartas con la forma en
// la textura): su albedo tiene alfa o el factor de alfa es bajo (como decide
// SkinnedModel el `alpha_masked` del render). Si no se puede saber, el nombre.
bool alphaCutout(const ModelData& model, const MaterialData& material) {
    if (material.transparent) return false;  // vidrio: otra pasada
    if (material.base_color.w < 0.5f) return true;
    if (material.albedo_texture >= 0 && static_cast<std::size_t>(material.albedo_texture) < model.textures.size()) {
        const TextureData& t = model.textures[static_cast<std::size_t>(material.albedo_texture)];
        if (t.alpha >= 0) return t.alpha == 1;  // lo midio la compresion
        switch (t.format) {
            case TextureFormat::Bc2:
            case TextureFormat::Bc3:
            case TextureFormat::Bc7: return true;
            case TextureFormat::Rgba8:
                if (!t.pixels.empty()) {
                    for (std::size_t i = 3; i < t.pixels.size(); i += 4) {
                        if (t.pixels[i] < 128) return true;
                    }
                    return false;
                }
                break;
            default: break;
        }
        int alpha = -1;
        if (!t.encoded.empty()) {
            alpha = headerAlpha(t.encoded.data(), std::min<std::size_t>(t.encoded.size(), 4096));
        } else if (!t.source_path.empty()) {
            // Perezosa: solo la cabecera del archivo.
            std::ifstream in(std::filesystem::path(reinterpret_cast<const char8_t*>(t.source_path.c_str())), std::ios::binary);
            std::uint8_t head[4096];
            in.read(reinterpret_cast<char*>(head), sizeof(head));
            const std::size_t got = in ? sizeof(head) : static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0));
            if (got > 0) alpha = headerAlpha(head, got);
        }
        if (alpha >= 0) return alpha == 1;
    }
    return lowerContains(material.name, {"leaf", "leaves", "foliage", "grass", "fern", "bush", "hoja", "cesped",
                                         "hierba", "follaje", "cutout", "alphatest", "masked"});
}

}  // namespace

VertexCacheStats optimizeVertexCache(std::vector<std::uint32_t>& indices, const std::vector<SubMesh>& submeshes,
                                     const std::vector<MaterialData>& materials, std::size_t vertex_count) {
    VertexCacheStats stats;
    if (indices.empty() || vertex_count == 0) return stats;
    // Cada submalla con sus vertices renumerados (0..n): el coste va con el
    // tamano de la submalla, no con el de todo el modelo (un lote estatico
    // tiene millones de vertices y miles de clusteres).
    std::vector<std::uint32_t> local_of(vertex_count, 0);
    std::vector<std::uint32_t> stamp(vertex_count, 0);
    std::vector<std::uint32_t> global_of;
    std::vector<std::uint32_t> local;
    std::vector<std::uint32_t> optimized;
    double before = 0.0;
    double after = 0.0;
    std::size_t triangles = 0;
    std::uint32_t generation = 0;
    for (const SubMesh& submesh : submeshes) {
        if (submesh.index_count < 3 || submesh.first_index + submesh.index_count > indices.size()) continue;
        if (submesh.material < materials.size() && materials[submesh.material].transparent) continue;
        ++generation;
        global_of.clear();
        local.resize(submesh.index_count);
        bool valid = true;
        for (std::uint32_t i = 0; i < submesh.index_count; ++i) {
            const std::uint32_t v = indices[submesh.first_index + i];
            if (v >= vertex_count) {
                valid = false;
                break;
            }
            if (stamp[v] != generation) {
                stamp[v] = generation;
                local_of[v] = static_cast<std::uint32_t>(global_of.size());
                global_of.push_back(v);
            }
            local[i] = local_of[v];
        }
        if (!valid) continue;
        const meshopt_VertexCacheStatistics old_stats =
            meshopt_analyzeVertexCache(local.data(), local.size(), global_of.size(), 16, 0, 0);
        optimized.resize(local.size());
        meshopt_optimizeVertexCache(optimized.data(), local.data(), local.size(), global_of.size());
        const meshopt_VertexCacheStatistics new_stats =
            meshopt_analyzeVertexCache(optimized.data(), optimized.size(), global_of.size(), 16, 0, 0);
        const std::size_t count = submesh.index_count / 3;
        before += static_cast<double>(old_stats.acmr) * static_cast<double>(count);
        after += static_cast<double>(new_stats.acmr) * static_cast<double>(count);
        triangles += count;
        for (std::uint32_t i = 0; i < submesh.index_count; ++i) {
            indices[submesh.first_index + i] = global_of[optimized[i]];
        }
    }
    if (triangles > 0) {
        stats.acmr_before = static_cast<float>(before / static_cast<double>(triangles));
        stats.acmr_after = static_cast<float>(after / static_cast<double>(triangles));
    }
    return stats;
}

VertexCacheStats optimizeVertexCache(ModelData& model) {
    return optimizeVertexCache(model.indices, model.submeshes, model.materials, model.vertices.size());
}

std::size_t generateLods(ModelData& model) {
    model.lods.clear();
    model.lod_indices.clear();
    // Tambien los de esqueleto: un LOD solo cambia los triangulos y comparte
    // los vertices, asi que los pesos de los huesos siguen valiendo (la
    // simplificacion se mide en la pose de reposo).
    if (model.vertices.empty() || model.indices.size() / 3 < kMinModelTriangles) {
        return 0;
    }
    const auto start = std::chrono::steady_clock::now();

    // Se simplifica cada material entero (no cluster a cluster): asi no se
    // abren grietas entre clusteres vecinos. Luego se vuelve a partir.
    struct Group {
        std::uint32_t material = 0;
        std::vector<std::uint32_t> indices;
        float error = 0.0f;  // acumulado desde LOD0, en unidades del modelo
        bool done = false;
        // Recorte por alfa (follaje): sin Prune, sin "sloppy" y con los bordes
        // fijos. Cada hoja es una carta suelta: Prune la quitaba entera,
        // sloppy soldaba cartas vecinas por celdas y colapsar sus bordes las
        // encogia (el arbol quedaba en tronco y ramas peladas a media
        // distancia). Asi solo se simplifica lo de dentro de las cartas.
        bool cutout = false;
    };
    std::vector<Group> groups;
    std::size_t cutout_groups = 0;
    {
        std::unordered_map<std::uint32_t, std::size_t> group_of;
        for (const SubMesh& submesh : model.submeshes) {
            const auto [it, inserted] = group_of.try_emplace(submesh.material, groups.size());
            if (inserted) {
                const bool cutout =
                    submesh.material < model.materials.size() && alphaCutout(model, model.materials[submesh.material]);
                cutout_groups += cutout ? 1 : 0;
                groups.push_back(Group{submesh.material, {}, 0.0f, false, cutout});
            }
            std::vector<std::uint32_t>& out = groups[it->second].indices;
            out.insert(out.end(), model.indices.begin() + submesh.first_index,
                       model.indices.begin() + submesh.first_index + submesh.index_count);
        }
    }

    const float* positions = &model.vertices[0].position.x;
    const std::size_t vertex_count = model.vertices.size();
    constexpr std::size_t kStride = sizeof(SkinnedVertex);
    const float extent = meshopt_simplifyScale(positions, vertex_count, kStride);
    // Error absoluto (unidades del modelo); Prune quita las piezas sueltas
    // (hojas, briznas, tornillos) cuando ya no aportan; Sparse porque cada
    // material usa solo parte de los vertices.
    constexpr unsigned int kOptions =
        meshopt_SimplifyErrorAbsolute | meshopt_SimplifyPrune | meshopt_SimplifySparse;
    // Recorte por alfa: sin Prune y con los bordes de las cartas fijos.
    constexpr unsigned int kCutoutOptions =
        meshopt_SimplifyErrorAbsolute | meshopt_SimplifySparse | meshopt_SimplifyLockBorder;

    // Con esqueleto: cada nivel con su tope de error y sin "sloppy" (ver
    // kSkinnedLevelError). Los estaticos, como siempre.
    const bool skinned = model.bones.size() > 1 && !oldSkinnedLods() && extent > 0.0f;
    // Y con atributos (meshopt_simplifyWithAttributes): el error de un
    // personaje no es solo la forma en reposo. Cuentan tambien
    //   - la normal (sombreado): 30 grados ~ 0,5 % del tamano;
    //   - la UV (la textura resbala: la cara o la barba se desdibujan):
    //     0,005 de UV (10 texeles de 2048) ~ 0,25 % del tamano;
    //   - a que huesos va (la media de las posiciones de sus huesos en la
    //     pose de union, por los pesos): juntar un vertice del brazo con uno
    //     del pecho los estira al animar; se mide en unidades del modelo.
    // meshoptimizer lleva las posiciones a un cubo de lado 1 (divide por
    // `extent`) pero los atributos solo los multiplica por su peso: los pesos
    // van en fraccion del tamano, y los de los huesos (unidades del modelo)
    // ademas divididos por `extent`.
    constexpr std::size_t kAttributes = 8;
    std::vector<float> attributes;
    float attribute_weights[kAttributes] = {};
    if (skinned) {
        std::vector<core::Vec3> joint(model.bones.size());
        for (std::size_t b = 0; b < model.bones.size(); ++b) {
            const core::Mat4 bind = core::inverse(model.bones[b].offset);
            joint[b] = core::Vec3{bind.m[3][0], bind.m[3][1], bind.m[3][2]};
        }
        attributes.resize(vertex_count * kAttributes);
        for (std::size_t v = 0; v < vertex_count; ++v) {
            const SkinnedVertex& x = model.vertices[v];
            float* a = &attributes[v * kAttributes];
            a[0] = x.normal.x;
            a[1] = x.normal.y;
            a[2] = x.normal.z;
            a[3] = x.uv.x;
            a[4] = x.uv.y;
            core::Vec3 j{};
            float total = 0.0f;
            for (std::size_t k = 0; k < kMaxBoneInfluences; ++k) {
                if (x.weights[k] <= 0.0f || x.joints[k] >= joint.size()) continue;
                j = j + joint[x.joints[k]] * x.weights[k];
                total += x.weights[k];
            }
            if (total > 0.0f) j = j * (1.0f / total);
            a[5] = j.x;
            a[6] = j.y;
            a[7] = j.z;
        }
        const float wn = 0.01f, wu = 0.5f, wb = 0.5f / extent;
        const float w[kAttributes] = {wn, wn, wn, wu, wu, wb, wb, wb};
        std::copy(std::begin(w), std::end(w), attribute_weights);
    }
    // Rigidos (rocas, edificios, arboles): tambien con la normal y la UV, con
    // los mismos pesos que los personajes. Solo con la forma, meshoptimizer
    // colapsaba aristas donde cambia el sombreado (las caras de una roca
    // salian facetadas) o donde la textura resbala (letreros, cortezas). El
    // error que devuelve ya cuenta la normal y la UV, asi que el nivel se
    // elige algo mas tarde (mas conservador). CRAMION_RIGID_LOD=old: solo la
    // forma, como antes (A/B).
    const bool rigid_attributes = !skinned && extent > 0.0f && !oldRigidLods();
    if (rigid_attributes) {
        attributes.assign(vertex_count * kAttributes, 0.0f);
        const std::vector<SkinnedVertex>& source = model.vertices.get();  // (sin copiar al escribir)
        for (std::size_t v = 0; v < vertex_count; ++v) {
            const SkinnedVertex& x = source[v];
            float* a = &attributes[v * kAttributes];
            a[0] = x.normal.x;
            a[1] = x.normal.y;
            a[2] = x.normal.z;
            a[3] = x.uv.x;
            a[4] = x.uv.y;
        }
        const float wn = 0.01f, wu = 0.5f;
        const float w[kAttributes] = {wn, wn, wn, wu, wu, 0.0f, 0.0f, 0.0f};
        std::copy(std::begin(w), std::end(w), attribute_weights);
    }
    // Los rigidos solo usan normal y UV (los 5 primeros); el resto, huesos.
    const std::size_t attribute_count = skinned ? kAttributes : kRigidAttributes;

    std::vector<std::uint32_t> scratch;
    for (std::size_t level = 1; level <= kMaxLods; ++level) {
        bool reduced = false;
        for (Group& group : groups) {
            const std::size_t count = group.indices.size();
            if (group.done || count / 3 < kMinGroupTriangles) {
                group.done = true;
                continue;
            }
            const std::size_t target = (count / 2) / 3 * 3;
            scratch.resize(count);
            float error = 0.0f;
            // Con esqueleto el tope es el del nivel menos lo que ya se perdio
            // en los anteriores (se simplifica desde el nivel anterior).
            const float budget = skinned ? extent * kSkinnedLevelError[level - 1] - group.error : extent;
            if (budget <= 0.0f) {
                group.done = true;
                continue;
            }
            const unsigned int options = group.cutout ? kCutoutOptions : kOptions;
            std::size_t result =
                skinned || rigid_attributes
                    ? meshopt_simplifyWithAttributes(scratch.data(), group.indices.data(), count, positions,
                                                     vertex_count, kStride, attributes.data(),
                                                     kAttributes * sizeof(float), attribute_weights, attribute_count,
                                                     nullptr, target, budget, options, &error)
                    : meshopt_simplify(scratch.data(), group.indices.data(), count, positions, vertex_count,
                                       kStride, target, budget, options, &error);
            // Con esqueleto: lo que no baje dentro del tope se queda para el
            // nivel siguiente (con mas tope), sin "sloppy".
            if (skinned && result > count * 90 / 100) continue;
            // La topologia no deja bajar (p. ej. mallas de muchas piezas
            // abiertas): simplificacion "sloppy" (agrupa vertices por celdas).
            // Con UV de lightmap no: juntaria vertices de cartas distintas y
            // el lightmap se veria cruzado (las costuras de las cartas se
            // respetan como costuras con la normal, PLAN-LIGHTMAPS 1.5).
            // Con esqueleto tampoco (ni con CRAMION_SKINNED_LOD=old): soldaria
            // vertices de piezas que los huesos mueven por separado (brazo y
            // torso). Ni con recorte por alfa (ver Group::cutout).
            if (!skinned && !group.cutout && model.bones.size() <= 1 && result > count * 85 / 100 &&
                model.lightmap_uvs.empty()) {
                std::vector<std::uint32_t> sloppy(count);
                float sloppy_error = 0.0f;
                const std::size_t sloppy_result =
                    meshopt_simplifySloppy(sloppy.data(), group.indices.data(), count, positions, vertex_count,
                                           kStride, nullptr, target, 1.0f, &sloppy_error);
                if (sloppy_result < result * 85 / 100) {
                    scratch.swap(sloppy);
                    result = sloppy_result;
                    error = sloppy_error * extent;  // sloppy da el error relativo
                }
            }
            if (result > count * 90 / 100) {
                group.done = true;  // ya no baja: se queda como esta
                continue;
            }
            scratch.resize(result);
            group.indices.swap(scratch);
            // Se simplifica desde el nivel anterior: los errores se suman.
            group.error += error;
            reduced = true;
        }
        // Con esqueleto, un nivel que no baja con su tope puede bajar con el
        // del siguiente.
        if (!reduced && skinned) continue;
        if (!reduced) break;

        MeshLod lod{};
        std::vector<std::uint32_t> indices;
        for (const Group& group : groups) {
            lod.error = std::max(lod.error, group.error);
            if (group.indices.empty()) continue;  // podado entero
            SubMesh submesh{};
            submesh.first_index = static_cast<std::uint32_t>(indices.size());
            submesh.index_count = static_cast<std::uint32_t>(group.indices.size());
            submesh.material = group.material;
            lod.submeshes.push_back(submesh);
            indices.insert(indices.end(), group.indices.begin(), group.indices.end());
        }
        const std::size_t triangles = indices.size() / 3;
        clusterIndexRanges(model.vertices, indices, lod.submeshes);
        // La simplificacion y los clusteres desordenan los triangulos.
        optimizeVertexCache(indices, lod.submeshes, model.materials, model.vertices.size());
        const auto offset = static_cast<std::uint32_t>(model.lod_indices.size());
        for (SubMesh& submesh : lod.submeshes) submesh.first_index += offset;
        model.lod_indices.insert(model.lod_indices.end(), indices.begin(), indices.end());
        model.lods.push_back(std::move(lod));
        if (triangles < 256) break;
    }

    if (!model.lods.empty()) {
        const float seconds =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
        std::cout << "[Modelo] " << model.name << ": " << model.lods.size() << " LODs ("
                  << model.indices.size() / 3;
        for (const MeshLod& lod : model.lods) {
            std::size_t triangles = 0;
            for (const SubMesh& submesh : lod.submeshes) triangles += submesh.index_count / 3;
            std::cout << " -> " << triangles;
        }
        std::cout << " triangulos";
        if (cutout_groups > 0) std::cout << ", " << cutout_groups << " materiales con recorte por alfa sin podar";
        if (skinned) {
            // Error de cada nivel en % del tamano (forma, normal, UV y huesos).
            std::cout << ", error";
            for (const MeshLod& lod : model.lods) {
                std::cout << " " << 100.0f * lod.error / std::max(extent, 1e-6f) << " %";
            }
        }
        std::cout << ") en " << seconds << " s\n";
    }
    return model.lods.size();
}

std::size_t setManualLods(ModelData& model, const std::vector<const ModelData*>& levels) {
    model.lods.clear();
    model.lod_indices.clear();
    // Solo piezas rigidas (un hueso): los personajes se importan en una pieza
    // y sus LODs salen de generateLods.
    if (model.vertices.empty() || model.indices.empty() || model.bones.size() > 1 || model.materials.empty()) {
        return 0;
    }
    const std::size_t base_vertices = model.vertices.size();
    const std::size_t base_triangles = model.indices.size() / 3;
    const float extent = meshopt_simplifyScale(&model.vertices.get()[0].position.x, base_vertices, sizeof(SkinnedVertex));
    const bool with_colors = model.vertex_colors.size() == base_vertices;
    const bool with_lightmap = !model.lightmap_uvs.empty();
    float previous_error = 0.0f;
    std::size_t previous_triangles = base_triangles;

    for (const ModelData* level : levels) {
        if (level == nullptr || level->vertices.empty() || level->indices.empty()) continue;
        const std::vector<SkinnedVertex>& level_vertices = level->vertices.get();
        const std::vector<std::uint32_t>& level_indices = level->indices.get();
        const std::size_t triangles = level_indices.size() / 3;
        // Un nivel con mas triangulos que el anterior no sirve de LOD.
        if (triangles == 0 || triangles >= previous_triangles) continue;

        // Sus vertices van detras de los de LOD0 (y de los niveles
        // anteriores): un LOD solo cambia los indices, como los automaticos.
        const auto offset = static_cast<std::uint32_t>(model.vertices.size());
        {
            std::vector<SkinnedVertex>& vertices = model.vertices.mut();
            vertices.insert(vertices.end(), level_vertices.begin(), level_vertices.end());
        }
        if (with_colors) {
            std::vector<std::uint32_t>& colors = model.vertex_colors.mut();
            if (level->vertex_colors.size() == level_vertices.size()) {
                colors.insert(colors.end(), level->vertex_colors.begin(), level->vertex_colors.end());
            } else {
                colors.insert(colors.end(), level_vertices.size(), 0xFFFFFFFFu);
            }
        }
        // Sin UV de lightmap propia: (-1, -1) = sondas (ver lightmap_uvs).
        if (with_lightmap) model.lightmap_uvs.resize(model.vertices.size(), core::Vec2{-1.0f, -1.0f});

        // Sus materiales, los de LOD0 con el mismo nombre (un LOD no crea
        // materiales: SkinnedModel los sube con LOD0); si no hay, el del mismo
        // indice o el primero.
        const auto material_of = [&](std::uint32_t m) -> std::uint32_t {
            if (m < level->materials.size()) {
                const std::string& name = level->materials[m].name;
                for (std::size_t k = 0; k < model.materials.size(); ++k) {
                    if (!name.empty() && model.materials[k].name == name) return static_cast<std::uint32_t>(k);
                }
            }
            return m < model.materials.size() ? m : 0u;
        };
        std::vector<std::uint32_t> materials;
        std::vector<std::vector<std::uint32_t>> by_material;
        for (const SubMesh& submesh : level->submeshes) {
            if (submesh.first_index + submesh.index_count > level_indices.size()) continue;
            const std::uint32_t material = material_of(submesh.material);
            std::size_t slot = 0;
            while (slot < materials.size() && materials[slot] != material) ++slot;
            if (slot == materials.size()) {
                materials.push_back(material);
                by_material.emplace_back();
            }
            for (std::uint32_t i = 0; i < submesh.index_count; ++i) {
                const std::uint32_t v = level_indices[submesh.first_index + i];
                by_material[slot].push_back(v < level_vertices.size() ? v + offset : offset);
            }
        }

        MeshLod lod{};
        std::vector<std::uint32_t> indices;
        for (std::size_t slot = 0; slot < materials.size(); ++slot) {
            if (by_material[slot].empty()) continue;
            SubMesh submesh{};
            submesh.first_index = static_cast<std::uint32_t>(indices.size());
            submesh.index_count = static_cast<std::uint32_t>(by_material[slot].size());
            submesh.material = materials[slot];
            lod.submeshes.push_back(submesh);
            indices.insert(indices.end(), by_material[slot].begin(), by_material[slot].end());
        }
        if (indices.empty()) continue;
        clusterIndexRanges(model.vertices, indices, lod.submeshes);
        optimizeVertexCache(indices, lod.submeshes, model.materials, model.vertices.size());
        const auto first = static_cast<std::uint32_t>(model.lod_indices.size());
        for (SubMesh& submesh : lod.submeshes) submesh.first_index += first;
        model.lod_indices.insert(model.lod_indices.end(), indices.begin(), indices.end());
        // Cada nivel con mas error que el anterior (chooseLod los recorre en
        // orden).
        const float ratio = static_cast<float>(base_triangles) / static_cast<float>(std::max<std::size_t>(triangles, 1));
        lod.error = std::max(extent * kManualLodErrorPerRatio * ratio, previous_error * 1.25f + 1e-6f);
        previous_error = lod.error;
        previous_triangles = triangles;
        model.lods.push_back(std::move(lod));
    }

    if (!model.lods.empty()) {
        std::cout << "[Modelo] " << model.name << ": " << model.lods.size() << " LODs a mano (" << base_triangles;
        for (const MeshLod& lod : model.lods) {
            std::size_t triangles = 0;
            for (const SubMesh& submesh : lod.submeshes) triangles += submesh.index_count / 3;
            std::cout << " -> " << triangles;
        }
        std::cout << " triangulos)\n";
    } else if (model.vertices.size() != base_vertices) {
        // Ningun nivel valido: LOD0 tal cual.
        model.vertices.mut().resize(base_vertices);
        if (with_colors) model.vertex_colors.mut().resize(base_vertices);
        if (with_lightmap) model.lightmap_uvs.resize(base_vertices);
    }
    return model.lods.size();
}

}  // namespace cramion::asset
