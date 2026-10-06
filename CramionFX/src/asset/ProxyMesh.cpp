#include "CramionFX/asset/ProxyMesh.h"

#include "CramionFX/asset/Dds.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <unordered_map>

namespace cramion::asset {

namespace {

core::Vec3 transformPoint(const core::Mat4& m, const core::Vec3& p) {
    const core::Vec4 r = m * core::Vec4{p.x, p.y, p.z, 1.0f};
    return core::Vec3{r.x, r.y, r.z};
}

core::Vec3 transformNormal(const core::Mat4& m, const core::Vec3& n) {
    // Sin escalas raras basta la parte 3x3 (los proxies son de lejos).
    const core::Vec4 r = m * core::Vec4{n.x, n.y, n.z, 0.0f};
    return core::normalize(core::Vec3{r.x, r.y, r.z});
}

float toLinear(float c) { return std::pow(std::clamp(c, 0.0f, 1.0f), 2.2f); }
float toSrgb(float c) { return std::pow(std::clamp(c, 0.0f, 1.0f), 1.0f / 2.2f); }

// Color medio (lineal) de la textura de albedo (muestreo disperso).
bool averageTexture(const TextureData& t, core::Vec3& out) {
    std::vector<std::uint8_t> decoded;
    const std::uint8_t* px = nullptr;
    if (t.format == TextureFormat::Rgba8) {
        if (t.pixels.size() < static_cast<std::size_t>(t.width) * t.height * 4) return false;
        px = t.pixels.data();
    } else {
        decoded = decodeBlockCompressed(t);
        if (decoded.size() < static_cast<std::size_t>(t.width) * t.height * 4) return false;
        px = decoded.data();
    }
    const std::size_t count = static_cast<std::size_t>(t.width) * t.height;
    if (count == 0) return false;
    const std::size_t step = std::max<std::size_t>(count / 2048, 1);
    double r = 0.0, g = 0.0, b = 0.0, n = 0.0;
    for (std::size_t i = 0; i < count; i += step) {
        const std::uint8_t* p = px + i * 4;
        if (p[3] < 64) continue;  // recortado (hojas): no cuenta
        r += toLinear(p[0] / 255.0f);
        g += toLinear(p[1] / 255.0f);
        b += toLinear(p[2] / 255.0f);
        n += 1.0;
    }
    if (n <= 0.0) return false;
    out = core::Vec3{static_cast<float>(r / n), static_cast<float>(g / n), static_cast<float>(b / n)};
    return true;
}

}  // namespace

std::size_t ProxyMesh::triangles() const {
    std::size_t t = 0;
    for (const Group& g : groups) t += g.indices.size() / 3;
    return t;
}

bool buildProxyMesh(const std::vector<ProxyPart>& parts, float ratio, std::size_t max_triangles, int max_colors,
                    ProxyMesh& out) {
    out = ProxyMesh{};
    // 1) Todo junto, con el color medio de cada material de cada pieza.
    struct Tri {
        std::uint32_t a, b, c;
        core::Vec3 color;
    };
    std::vector<Tri> tris;
    std::map<const ModelData*, std::vector<core::Vec3>> material_colors;
    for (const ProxyPart& part : parts) {
        const ModelData* m = part.model;
        if (m == nullptr || m->vertices.empty()) continue;
        std::vector<core::Vec3>& colors = material_colors[m];
        if (colors.empty()) {
            for (const MaterialData& mat : m->materials) {
                core::Vec3 c{toLinear(mat.base_color.x), toLinear(mat.base_color.y), toLinear(mat.base_color.z)};
                core::Vec3 tex{1.0f, 1.0f, 1.0f};
                if (mat.albedo_texture >= 0 && mat.albedo_texture < static_cast<std::int32_t>(m->textures.size())) {
                    averageTexture(m->textures[static_cast<std::size_t>(mat.albedo_texture)], tex);
                }
                colors.push_back(c * tex);
            }
            if (colors.empty()) colors.push_back(core::Vec3{0.5f, 0.5f, 0.5f});
        }
        const std::uint32_t base = static_cast<std::uint32_t>(out.positions.size());
        for (const SkinnedVertex& v : m->vertices) {
            out.positions.push_back(transformPoint(part.transform, v.position));
            out.normals.push_back(transformNormal(part.transform, v.normal));
        }
        for (const SubMesh& sm : m->submeshes) {
            const core::Vec3 color = colors[std::min<std::size_t>(sm.material, colors.size() - 1)];
            for (std::uint32_t i = 0; i + 2 < sm.index_count; i += 3) {
                const std::uint32_t* idx = m->indices.data() + sm.first_index + i;
                tris.push_back(Tri{base + idx[0], base + idx[1], base + idx[2], color});
            }
        }
    }
    out.sourceTriangles = tris.size();
    if (tris.empty()) return false;

    // 2) Grupos de color: cuantizado a 4 bits por canal y los mas usados.
    const auto keyOf = [](const core::Vec3& c) {
        const auto q = [](float v) { return static_cast<int>(std::clamp(toSrgb(v), 0.0f, 1.0f) * 15.0f + 0.5f); };
        return (q(c.x) << 8) | (q(c.y) << 4) | q(c.z);
    };
    std::unordered_map<int, std::pair<std::size_t, core::Vec3>> bins;  // triangulos y suma de color
    for (const Tri& t : tris) {
        auto& b = bins[keyOf(t.color)];
        ++b.first;
        b.second += t.color;
    }
    std::vector<std::pair<std::size_t, int>> order;
    for (const auto& [k, v] : bins) order.emplace_back(v.first, k);
    std::sort(order.begin(), order.end(), std::greater<>());
    const std::size_t palette_size = std::min<std::size_t>(order.size(), static_cast<std::size_t>(std::max(max_colors, 1)));
    std::vector<core::Vec3> palette;
    for (std::size_t i = 0; i < palette_size; ++i) {
        const auto& b = bins[order[i].second];
        palette.push_back(b.second * (1.0f / static_cast<float>(b.first)));
    }
    std::vector<std::vector<std::uint32_t>> group_indices(palette.size());
    for (const Tri& t : tris) {
        std::size_t best = 0;
        float best_d = 1e30f;
        for (std::size_t p = 0; p < palette.size(); ++p) {
            const core::Vec3 d = t.color - palette[p];
            const float dd = core::dot(d, d);
            if (dd < best_d) {
                best_d = dd;
                best = p;
            }
        }
        group_indices[best].insert(group_indices[best].end(), {t.a, t.b, t.c});
    }

    // 3) Simplificacion por grupo (sloppy: junta piezas sueltas, que es lo
    // que hace falta de lejos).
    const std::size_t target_total =
        std::min(max_triangles, std::max<std::size_t>(static_cast<std::size_t>(static_cast<float>(tris.size()) * ratio), 64));
    const float* positions = &out.positions[0].x;
    for (std::size_t g = 0; g < palette.size(); ++g) {
        std::vector<std::uint32_t>& idx = group_indices[g];
        if (idx.empty()) continue;
        const std::size_t share = std::max<std::size_t>(target_total * idx.size() / (tris.size() * 3), 8) * 3;
        if (share < idx.size()) {
            std::vector<std::uint32_t> simplified(idx.size());
            float error = 0.0f;
            const std::size_t n = meshopt_simplifySloppy(simplified.data(), idx.data(), idx.size(), positions,
                                                         out.positions.size(), sizeof(core::Vec3), nullptr, share,
                                                         1.0f, &error);
            simplified.resize(n);
            if (!simplified.empty()) idx.swap(simplified);
        }
        ProxyMesh::Group group;
        group.color = core::Vec3{toSrgb(palette[g].x), toSrgb(palette[g].y), toSrgb(palette[g].z)};
        group.indices = std::move(idx);
        out.groups.push_back(std::move(group));
    }
    // 4) Solo los vertices que quedan.
    std::vector<std::uint32_t> remap(out.positions.size(), UINT32_MAX);
    std::vector<core::Vec3> positions_out;
    std::vector<core::Vec3> normals_out;
    for (ProxyMesh::Group& g : out.groups) {
        for (std::uint32_t& i : g.indices) {
            if (remap[i] == UINT32_MAX) {
                remap[i] = static_cast<std::uint32_t>(positions_out.size());
                positions_out.push_back(out.positions[i]);
                normals_out.push_back(out.normals[i]);
            }
            i = remap[i];
        }
    }
    out.positions.swap(positions_out);
    out.normals.swap(normals_out);
    return !out.groups.empty();
}

bool writeProxyObj(const ProxyMesh& mesh, const std::filesystem::path& obj_file, const std::string& name,
                   std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(obj_file.parent_path(), ec);
    std::filesystem::path mtl_file = obj_file;
    mtl_file.replace_extension(".mtl");
    {
        std::ofstream mtl(mtl_file, std::ios::binary);
        if (!mtl) {
            if (error != nullptr) *error = "no se pudo escribir " + mtl_file.string();
            return false;
        }
        for (std::size_t g = 0; g < mesh.groups.size(); ++g) {
            const core::Vec3 c = mesh.groups[g].color;
            mtl << "newmtl hlod_" << g << "\nKd " << c.x << " " << c.y << " " << c.z << "\nNs 10\n\n";
        }
    }
    std::ofstream obj(obj_file, std::ios::binary);
    if (!obj) {
        if (error != nullptr) *error = "no se pudo escribir " + obj_file.string();
        return false;
    }
    obj << "# HLOD de Cramion: " << name << " (" << mesh.triangles() << " triangulos de " << mesh.sourceTriangles << ")\n";
    obj << "mtllib " << mtl_file.filename().string() << "\no " << name << "\n";
    for (const core::Vec3& p : mesh.positions) obj << "v " << p.x << " " << p.y << " " << p.z << "\n";
    for (const core::Vec3& n : mesh.normals) obj << "vn " << n.x << " " << n.y << " " << n.z << "\n";
    for (std::size_t g = 0; g < mesh.groups.size(); ++g) {
        obj << "usemtl hlod_" << g << "\n";
        const std::vector<std::uint32_t>& idx = mesh.groups[g].indices;
        for (std::size_t i = 0; i + 2 < idx.size(); i += 3) {
            const std::uint32_t a = idx[i] + 1, b = idx[i + 1] + 1, c = idx[i + 2] + 1;
            obj << "f " << a << "//" << a << " " << b << "//" << b << " " << c << "//" << c << "\n";
        }
    }
    return static_cast<bool>(obj);
}

}  // namespace cramion::asset
