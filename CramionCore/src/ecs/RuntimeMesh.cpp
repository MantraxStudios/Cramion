#include "CramionCore/ecs/RuntimeMesh.h"

#include <CramionFX/asset/Model.h>

#include <algorithm>
#include <cmath>

namespace cramion::ecs {

using core::Vec2;
using core::Vec3;
using core::Vec4;

namespace {

constexpr float kPi = 3.14159265358979f;

float toLinear(float c) { return std::pow(std::clamp(c, 0.0f, 1.0f), 2.2f); }

// Un vector perpendicular cualquiera (tangente de respaldo).
Vec3 perpendicular(const Vec3& n) {
    const Vec3 axis = std::abs(n.y) < 0.99f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    const Vec3 t = core::cross(axis, n);
    const float len = core::length(t);
    return len > 1e-6f ? t * (1.0f / len) : Vec3{1.0f, 0.0f, 0.0f};
}

}  // namespace

// --- Triangulos -------------------------------------------------------------------

void Mesh::setSubMeshCount(int count) { submeshes_.resize(static_cast<std::size_t>(std::max(count, 1))); }

void Mesh::setTriangles(std::vector<std::uint32_t> triangles, int submesh) {
    if (submesh < 0) return;
    if (static_cast<std::size_t>(submesh) >= submeshes_.size()) submeshes_.resize(static_cast<std::size_t>(submesh) + 1);
    submeshes_[static_cast<std::size_t>(submesh)] = std::move(triangles);
}

const std::vector<std::uint32_t>& Mesh::triangles(int submesh) const {
    static const std::vector<std::uint32_t> kEmpty;
    return submesh >= 0 && static_cast<std::size_t>(submesh) < submeshes_.size() ? submeshes_[static_cast<std::size_t>(submesh)]
                                                                               : kEmpty;
}

std::vector<std::uint32_t> Mesh::allTriangles() const {
    std::vector<std::uint32_t> out;
    for (const auto& s : submeshes_) out.insert(out.end(), s.begin(), s.end());
    return out;
}

std::size_t Mesh::triangleCount() const {
    std::size_t n = 0;
    for (const auto& s : submeshes_) n += s.size() / 3;
    return n;
}

void Mesh::clear() {
    vertices.clear();
    normals.clear();
    tangents.clear();
    uv.clear();
    submeshes_.assign(1, {});
    bounds_min_ = bounds_max_ = Vec3{};
    ++version_;
}

// --- Calculos -------------------------------------------------------------------

void Mesh::recalculateNormals() {
    normals.assign(vertices.size(), Vec3{});
    for (const auto& s : submeshes_) {
        for (std::size_t i = 0; i + 2 < s.size(); i += 3) {
            const std::uint32_t a = s[i], b = s[i + 1], c = s[i + 2];
            if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size()) continue;
            // Sin normalizar: pesa por el area del triangulo.
            const Vec3 face = core::cross(vertices[b] - vertices[a], vertices[c] - vertices[a]);
            normals[a] = normals[a] + face;
            normals[b] = normals[b] + face;
            normals[c] = normals[c] + face;
        }
    }
    for (Vec3& n : normals) {
        const float len = core::length(n);
        n = len > 1e-12f ? n * (1.0f / len) : Vec3{0.0f, 1.0f, 0.0f};
    }
}

void Mesh::recalculateTangents() {
    if (normals.size() != vertices.size()) recalculateNormals();
    std::vector<Vec3> tan(vertices.size(), Vec3{});
    std::vector<Vec3> bitan(vertices.size(), Vec3{});
    const bool has_uv = uv.size() == vertices.size();
    if (has_uv) {
        for (const auto& s : submeshes_) {
            for (std::size_t i = 0; i + 2 < s.size(); i += 3) {
                const std::uint32_t a = s[i], b = s[i + 1], c = s[i + 2];
                if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size()) continue;
                const Vec3 e1 = vertices[b] - vertices[a], e2 = vertices[c] - vertices[a];
                const Vec2 d1{uv[b].x - uv[a].x, uv[b].y - uv[a].y}, d2{uv[c].x - uv[a].x, uv[c].y - uv[a].y};
                const float det = d1.x * d2.y - d2.x * d1.y;
                if (std::abs(det) < 1e-12f) continue;
                const float r = 1.0f / det;
                const Vec3 t = (e1 * d2.y - e2 * d1.y) * r;
                const Vec3 bt = (e2 * d1.x - e1 * d2.x) * r;
                for (const std::uint32_t v : {a, b, c}) {
                    tan[v] = tan[v] + t;
                    bitan[v] = bitan[v] + bt;
                }
            }
        }
    }
    tangents.resize(vertices.size());
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const Vec3& n = normals[i];
        // Gram-Schmidt: la tangente en el plano de la normal.
        Vec3 t = tan[i] - n * core::dot(n, tan[i]);
        const float len = core::length(t);
        t = len > 1e-8f ? t * (1.0f / len) : perpendicular(n);
        const float w = core::dot(core::cross(n, t), bitan[i]) < 0.0f ? -1.0f : 1.0f;
        tangents[i] = Vec4{t.x, t.y, t.z, w};
    }
}

void Mesh::recalculateBounds() {
    if (vertices.empty()) {
        bounds_min_ = bounds_max_ = Vec3{};
        return;
    }
    bounds_min_ = bounds_max_ = vertices[0];
    for (const Vec3& p : vertices) {
        bounds_min_ = Vec3{std::min(bounds_min_.x, p.x), std::min(bounds_min_.y, p.y), std::min(bounds_min_.z, p.z)};
        bounds_max_ = Vec3{std::max(bounds_max_.x, p.x), std::max(bounds_max_.y, p.y), std::max(bounds_max_.z, p.z)};
    }
}

void Mesh::markModified() {
    recalculateBounds();
    ++version_;
}

std::string Mesh::validate() const {
    if (vertices.empty()) return "sin vertices";
    if (triangleCount() == 0) return "sin triangulos";
    if (!normals.empty() && normals.size() != vertices.size()) return "normals no tiene tantos elementos como vertices";
    if (!tangents.empty() && tangents.size() != vertices.size()) return "tangents no tiene tantos elementos como vertices";
    if (!uv.empty() && uv.size() != vertices.size()) return "uv no tiene tantos elementos como vertices";
    for (std::size_t s = 0; s < submeshes_.size(); ++s) {
        if (submeshes_[s].size() % 3 != 0) return "la submalla " + std::to_string(s) + " no tiene un numero de indices multiplo de 3";
        for (const std::uint32_t i : submeshes_[s]) {
            if (i >= vertices.size()) {
                return "la submalla " + std::to_string(s) + " usa el vertice " + std::to_string(i) + " (solo hay " +
                       std::to_string(vertices.size()) + ")";
            }
        }
    }
    return {};
}

std::string Mesh::materialLayout() const {
    std::string s = std::to_string(submeshes_.size());
    for (const MeshMaterial& m : materials) {
        s += '|' + m.texture + '|' + m.normal_map + '|' + m.emission_map + '|' + std::to_string(m.tiling.x) + ',' +
             std::to_string(m.tiling.y) + ',' + std::to_string(m.offset.x) + ',' + std::to_string(m.offset.y);
    }
    return s;
}

void Mesh::applyFactors(const MeshMaterial& m, void* material_data) {
    asset::MaterialData& mat = *static_cast<asset::MaterialData*>(material_data);
    mat.base_color = Vec4{toLinear(m.color.x), toLinear(m.color.y), toLinear(m.color.z), std::clamp(m.color.w, 0.0f, 1.0f)};
    mat.metallic = std::clamp(m.metallic, 0.0f, 1.0f);
    mat.roughness = std::clamp(m.roughness, 0.02f, 1.0f);
    mat.emissive = Vec3{toLinear(m.emission.x), toLinear(m.emission.y), toLinear(m.emission.z)} * std::max(m.emission_intensity, 0.0f);
    mat.normal_scale = m.normal_strength;
}

asset::ModelData Mesh::toModelData(const std::filesystem::path& assets_root) const {
    asset::ModelData data;
    data.name = name;
    // Lo que falte se completa en una copia (la malla del usuario no cambia).
    Mesh filled;
    const bool need_normals = normals.size() != vertices.size();
    const bool need_tangents = tangents.size() != vertices.size();
    if (need_normals || need_tangents) {
        filled.vertices = vertices;
        filled.normals = need_normals ? std::vector<Vec3>{} : normals;
        filled.uv = uv;
        filled.submeshes_ = submeshes_;
        if (need_normals) filled.recalculateNormals();
        filled.recalculateTangents();
    }
    const std::vector<Vec3>& n = need_normals ? filled.normals : normals;
    const std::vector<Vec4>& t = need_tangents ? filled.tangents : tangents;
    data.vertices.resize(vertices.size());
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        asset::SkinnedVertex& v = data.vertices[i];
        v.position = vertices[i];
        v.normal = n[i];
        v.uv = uv.size() == vertices.size() ? uv[i] : Vec2{0.0f, 0.0f};
        v.tangent = t[i];
        v.weights[0] = 1.0f;  // un hueso: el del modelo (rigido)
    }
    for (std::size_t s = 0; s < submeshes_.size(); ++s) {
        const std::vector<std::uint32_t>& tris = submeshes_[s];
        if (tris.empty()) continue;
        asset::SubMesh sub;
        sub.first_index = static_cast<std::uint32_t>(data.indices.size());
        sub.index_count = static_cast<std::uint32_t>(tris.size());
        sub.material = static_cast<std::uint32_t>(s);
        Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        for (const std::uint32_t i : tris) {
            const Vec3& p = vertices[i];
            lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
        sub.bounds_min = lo;
        sub.bounds_max = hi;
        data.indices.insert(data.indices.end(), tris.begin(), tris.end());
        data.submeshes.push_back(sub);
    }
    // Un material por submalla (tambien las vacias: los huecos coinciden).
    // Las texturas se comparten entre submallas si son el mismo archivo.
    std::vector<std::string> texture_paths;
    const auto texture = [&](const std::string& relative) -> std::int32_t {
        if (relative.empty() || assets_root.empty()) return -1;
        const std::filesystem::path file = assets_root / std::filesystem::path(std::u8string(relative.begin(), relative.end()));
        std::error_code error;
        if (!std::filesystem::is_regular_file(file, error)) return -1;
        const std::u8string u8 = file.u8string();
        const std::string path(u8.begin(), u8.end());
        for (std::size_t i = 0; i < texture_paths.size(); ++i) {
            if (texture_paths[i] == path) return static_cast<std::int32_t>(i);
        }
        asset::TextureData t;
        t.name = relative;
        t.source_path = path;
        data.textures.push_back(std::move(t));
        texture_paths.push_back(path);
        return static_cast<std::int32_t>(data.textures.size() - 1);
    };
    std::vector<std::uint8_t> transformed(data.vertices.size(), 0);
    for (std::size_t s = 0; s < std::max<std::size_t>(submeshes_.size(), 1); ++s) {
        const MeshMaterial m = s < materials.size() ? materials[s] : MeshMaterial{};
        asset::MaterialData mat;
        mat.name = name + " " + std::to_string(s);
        applyFactors(m, &mat);
        mat.albedo_texture = texture(m.texture);
        mat.normal_texture = texture(m.normal_map);
        mat.emissive_texture = texture(m.emission_map);
        data.materials.push_back(mat);
        // Repeticion y desplazamiento de la textura en las UV de su submalla.
        const bool identity = m.tiling.x == 1.0f && m.tiling.y == 1.0f && m.offset.x == 0.0f && m.offset.y == 0.0f;
        if (identity || s >= submeshes_.size()) continue;
        for (const std::uint32_t i : submeshes_[s]) {
            if (i >= data.vertices.size() || transformed[i]) continue;
            transformed[i] = 1;
            Vec2& t = data.vertices[i].uv;
            t = Vec2{t.x * m.tiling.x + m.offset.x, t.y * m.tiling.y + m.offset.y};
        }
    }
    // Un nodo y un hueso en el origen, como los OBJ importados: modelo rigido.
    data.nodes.push_back(asset::Node{"root", -1, core::Mat4::identity()});
    data.bones.push_back(asset::Bone{"root", 0, core::Mat4::identity()});
    return data;
}

// --- Primitivas -------------------------------------------------------------------

std::shared_ptr<Mesh> Mesh::quad(float width, float height) {
    auto m = std::make_shared<Mesh>();
    m->name = "Quad";
    const float w = width * 0.5f, h = height * 0.5f;
    m->vertices = {{-w, -h, 0.0f}, {w, -h, 0.0f}, {w, h, 0.0f}, {-w, h, 0.0f}};
    m->uv = {{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}};
    m->normals.assign(4, Vec3{0.0f, 0.0f, 1.0f});
    m->setTriangles({0, 1, 2, 0, 2, 3});
    m->recalculateTangents();
    m->markModified();
    return m;
}

std::shared_ptr<Mesh> Mesh::cube(const Vec3& size) {
    auto m = std::make_shared<Mesh>();
    m->name = "Cubo";
    const Vec3 h = size * 0.5f;
    // Cada cara con sus 4 vertices (aristas duras): normal, eje u y eje v.
    const Vec3 faces[6][3] = {{{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},  {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
                              {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},  {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
                              {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},   {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}};
    for (const auto& f : faces) {
        const Vec3 n = f[0], u = f[1], v = f[2];
        const auto base = static_cast<std::uint32_t>(m->vertices.size());
        const Vec2 corners[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (const Vec2& c : corners) {
            const Vec3 p = n + u * c.x + v * c.y;
            m->vertices.push_back(Vec3{p.x * h.x, p.y * h.y, p.z * h.z});
            m->normals.push_back(n);
            m->uv.push_back(Vec2{(c.x + 1.0f) * 0.5f, 1.0f - (c.y + 1.0f) * 0.5f});
        }
        auto& tris = m->submeshes_[0];
        tris.insert(tris.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    m->recalculateTangents();
    m->markModified();
    return m;
}

std::shared_ptr<Mesh> Mesh::plane(float width, float depth, int segments_x, int segments_z) {
    auto m = std::make_shared<Mesh>();
    m->name = "Plano";
    const int sx = std::clamp(segments_x, 1, 1024), sz = std::clamp(segments_z, 1, 1024);
    for (int z = 0; z <= sz; ++z) {
        for (int x = 0; x <= sx; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(sx), v = static_cast<float>(z) / static_cast<float>(sz);
            m->vertices.push_back(Vec3{(u - 0.5f) * width, 0.0f, (v - 0.5f) * depth});
            m->normals.push_back(Vec3{0.0f, 1.0f, 0.0f});
            m->uv.push_back(Vec2{u, v});
        }
    }
    auto& tris = m->submeshes_[0];
    for (int z = 0; z < sz; ++z) {
        for (int x = 0; x < sx; ++x) {
            const auto i = static_cast<std::uint32_t>(z * (sx + 1) + x);
            const auto row = static_cast<std::uint32_t>(sx + 1);
            // Antihorario visto desde arriba (+Y).
            tris.insert(tris.end(), {i, i + row, i + row + 1, i, i + row + 1, i + 1});
        }
    }
    m->recalculateTangents();
    m->markModified();
    return m;
}

std::shared_ptr<Mesh> Mesh::sphere(float radius, int segments, int rings) {
    auto m = std::make_shared<Mesh>();
    m->name = "Esfera";
    const int seg = std::clamp(segments, 3, 512), rg = std::clamp(rings, 2, 512);
    for (int r = 0; r <= rg; ++r) {
        const float v = static_cast<float>(r) / static_cast<float>(rg);
        const float phi = v * kPi;  // 0 arriba, pi abajo
        for (int s = 0; s <= seg; ++s) {
            const float u = static_cast<float>(s) / static_cast<float>(seg);
            const float theta = u * 2.0f * kPi;
            // Polos exactos (sin triangulos diminutos de orientacion al azar).
            const float sp = (r == 0 || r == rg) ? 0.0f : std::sin(phi);
            const float cp = r == 0 ? 1.0f : (r == rg ? -1.0f : std::cos(phi));
            const Vec3 n{sp * std::cos(theta), cp, -sp * std::sin(theta)};
            m->vertices.push_back(n * radius);
            m->normals.push_back(n);
            m->uv.push_back(Vec2{u, v});
        }
    }
    auto& tris = m->submeshes_[0];
    const auto row = static_cast<std::uint32_t>(seg + 1);
    for (int r = 0; r < rg; ++r) {
        for (int s = 0; s < seg; ++s) {
            const auto i = static_cast<std::uint32_t>(r) * row + static_cast<std::uint32_t>(s);
            tris.insert(tris.end(), {i, i + row, i + row + 1, i, i + row + 1, i + 1});
        }
    }
    m->recalculateTangents();
    m->markModified();
    return m;
}

std::shared_ptr<Mesh> Mesh::cylinder(float radius, float height, int segments) {
    auto m = std::make_shared<Mesh>();
    m->name = "Cilindro";
    const int seg = std::clamp(segments, 3, 512);
    const float h = height * 0.5f;
    auto& tris = m->submeshes_[0];
    // Lateral (aristas suaves).
    for (int s = 0; s <= seg; ++s) {
        const float u = static_cast<float>(s) / static_cast<float>(seg);
        const float a = u * 2.0f * kPi;
        const Vec3 n{std::cos(a), 0.0f, -std::sin(a)};
        for (const float y : {-h, h}) {
            m->vertices.push_back(Vec3{n.x * radius, y, n.z * radius});
            m->normals.push_back(n);
            m->uv.push_back(Vec2{u, y > 0.0f ? 0.0f : 1.0f});
        }
    }
    for (int s = 0; s < seg; ++s) {
        const auto i = static_cast<std::uint32_t>(s * 2);
        tris.insert(tris.end(), {i, i + 2, i + 3, i, i + 3, i + 1});
    }
    // Tapas.
    for (const float side : {1.0f, -1.0f}) {
        const auto center = static_cast<std::uint32_t>(m->vertices.size());
        m->vertices.push_back(Vec3{0.0f, h * side, 0.0f});
        m->normals.push_back(Vec3{0.0f, side, 0.0f});
        m->uv.push_back(Vec2{0.5f, 0.5f});
        for (int s = 0; s <= seg; ++s) {
            const float a = static_cast<float>(s) / static_cast<float>(seg) * 2.0f * kPi;
            m->vertices.push_back(Vec3{std::cos(a) * radius, h * side, -std::sin(a) * radius});
            m->normals.push_back(Vec3{0.0f, side, 0.0f});
            m->uv.push_back(Vec2{0.5f + std::cos(a) * 0.5f, 0.5f + std::sin(a) * 0.5f});
        }
        for (int s = 0; s < seg; ++s) {
            const std::uint32_t a = center + 1 + static_cast<std::uint32_t>(s), b = a + 1;
            if (side > 0.0f) tris.insert(tris.end(), {center, a, b});
            else tris.insert(tris.end(), {center, b, a});
        }
    }
    m->recalculateTangents();
    m->markModified();
    return m;
}

std::shared_ptr<Mesh> Mesh::capsule(float radius, float height, int segments, int rings) {
    auto m = std::make_shared<Mesh>();
    m->name = "Capsula";
    const int seg = std::clamp(segments, 3, 512), rg = std::clamp(rings, 1, 256);
    const float half = std::max(height * 0.5f - radius, 0.0f);  // media altura del tramo recto
    // Filas de la semiesfera de arriba, el tramo recto y la de abajo.
    std::vector<std::pair<float, float>> rows;  // (phi, desplazamiento en y)
    for (int r = 0; r <= rg; ++r) rows.emplace_back(static_cast<float>(r) / static_cast<float>(rg) * kPi * 0.5f, half);
    for (int r = 0; r <= rg; ++r) rows.emplace_back(kPi * 0.5f + static_cast<float>(r) / static_cast<float>(rg) * kPi * 0.5f, -half);
    const float total = static_cast<float>(rows.size() - 1);
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const auto [phi, offset] = rows[r];
        for (int s = 0; s <= seg; ++s) {
            const float u = static_cast<float>(s) / static_cast<float>(seg);
            const float theta = u * 2.0f * kPi;
            const bool pole = r == 0 || r + 1 == rows.size();
            const float sp = pole ? 0.0f : std::sin(phi);
            const float cp = r == 0 ? 1.0f : (r + 1 == rows.size() ? -1.0f : std::cos(phi));
            const Vec3 n{sp * std::cos(theta), cp, -sp * std::sin(theta)};
            m->vertices.push_back(n * radius + Vec3{0.0f, offset, 0.0f});
            m->normals.push_back(n);
            m->uv.push_back(Vec2{u, static_cast<float>(r) / total});
        }
    }
    auto& tris = m->submeshes_[0];
    const auto row = static_cast<std::uint32_t>(seg + 1);
    for (std::size_t r = 0; r + 1 < rows.size(); ++r) {
        for (int s = 0; s < seg; ++s) {
            const auto i = static_cast<std::uint32_t>(r) * row + static_cast<std::uint32_t>(s);
            tris.insert(tris.end(), {i, i + row, i + row + 1, i, i + row + 1, i + 1});
        }
    }
    m->recalculateTangents();
    m->markModified();
    return m;
}

std::shared_ptr<Mesh> Mesh::wireCube(const Vec3& size, float thickness) {
    auto m = std::make_shared<Mesh>();
    m->name = "Contorno";
    const Vec3 h = size * 0.5f;
    const float t = std::max(thickness, 1e-4f) * 0.5f;
    // Una barra (caja) de `lo` a `hi`, con sus 6 caras.
    const auto bar = [&](const Vec3& lo, const Vec3& hi) {
        const auto box = Mesh::cube(hi - lo);
        const Vec3 c = (lo + hi) * 0.5f;
        const auto base = static_cast<std::uint32_t>(m->vertices.size());
        for (const Vec3& p : box->vertices) m->vertices.push_back(p + c);
        m->normals.insert(m->normals.end(), box->normals.begin(), box->normals.end());
        m->uv.insert(m->uv.end(), box->uv.begin(), box->uv.end());
        m->tangents.insert(m->tangents.end(), box->tangents.begin(), box->tangents.end());
        for (const std::uint32_t i : box->triangles(0)) m->submeshes_[0].push_back(base + i);
    };
    for (const float a : {-1.0f, 1.0f}) {
        for (const float b : {-1.0f, 1.0f}) {
            bar(Vec3{-h.x - t, a * h.y - t, b * h.z - t}, Vec3{h.x + t, a * h.y + t, b * h.z + t});  // a lo largo de X
            bar(Vec3{a * h.x - t, -h.y - t, b * h.z - t}, Vec3{a * h.x + t, h.y + t, b * h.z + t});  // a lo largo de Y
            bar(Vec3{a * h.x - t, b * h.y - t, -h.z - t}, Vec3{a * h.x + t, b * h.y + t, h.z + t});  // a lo largo de Z
        }
    }
    m->markModified();
    return m;
}

}  // namespace cramion::ecs
