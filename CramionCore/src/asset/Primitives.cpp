#include "Primitives.h"

#include <cmath>

namespace cramion::assets::primitives {

namespace {

using asset::ModelData;
using asset::SkinnedVertex;
using core::Vec2;
using core::Vec3;
using core::Vec4;

constexpr float kPi = 3.14159265358979323846f;

// Constructor de mallas: vertices e indices en sentido antihorario visto
// desde fuera (el de assimp y el que espera el renderizador).
struct MeshBuilder {
    ModelData model;

    std::uint32_t vertex(const Vec3& position, const Vec3& normal, const Vec2& uv) {
        SkinnedVertex v{};
        v.position = position;
        v.normal = core::normalize(normal);
        v.uv = uv;
        v.joints[0] = 0;
        v.weights[0] = 1.0f;
        model.vertices.push_back(v);
        return static_cast<std::uint32_t>(model.vertices.size() - 1);
    }

    void triangle(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        model.indices.push_back(a);
        model.indices.push_back(b);
        model.indices.push_back(c);
    }

    void quad(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
        triangle(a, b, c);
        triangle(a, c, d);
    }

    // Tangentes a partir de las UV (mismo convenio que el lector de OBJ y
    // aiProcess_CalcTangentSpace), una submalla y el material por defecto.
    ModelData finish(const std::string& name) {
        std::vector<Vec3> tangents(model.vertices.size(), Vec3{});
        std::vector<Vec3> bitangents(model.vertices.size(), Vec3{});
        for (std::size_t i = 0; i + 2 < model.indices.size(); i += 3) {
            const std::uint32_t ia = model.indices[i];
            const std::uint32_t ib = model.indices[i + 1];
            const std::uint32_t ic = model.indices[i + 2];
            const SkinnedVertex& a = model.vertices[ia];
            const SkinnedVertex& b = model.vertices[ib];
            const SkinnedVertex& c = model.vertices[ic];
            const Vec3 e1 = b.position - a.position;
            const Vec3 e2 = c.position - a.position;
            const float du1 = b.uv.x - a.uv.x;
            const float dv1 = b.uv.y - a.uv.y;
            const float du2 = c.uv.x - a.uv.x;
            const float dv2 = c.uv.y - a.uv.y;
            const float det = du1 * dv2 - du2 * dv1;
            if (std::abs(det) < 1e-12f) {
                continue;
            }
            const float r = 1.0f / det;
            const Vec3 t = (e1 * dv2 - e2 * dv1) * r;
            const Vec3 bt = (e2 * du1 - e1 * du2) * r;
            for (const std::uint32_t v : {ia, ib, ic}) {
                tangents[v] += t;
                bitangents[v] += bt;
            }
        }
        for (std::size_t v = 0; v < model.vertices.size(); ++v) {
            SkinnedVertex& vertex = model.vertices[v];
            Vec3 t = tangents[v] - vertex.normal * core::dot(vertex.normal, tangents[v]);
            const float length = core::length(t);
            if (length < 1e-8f) {
                // Polos de la esfera: cualquier perpendicular a la normal.
                const Vec3 helper = std::abs(vertex.normal.y) < 0.99f ? Vec3{0.0f, 1.0f, 0.0f}
                                                                      : Vec3{1.0f, 0.0f, 0.0f};
                t = core::normalize(core::cross(helper, vertex.normal));
            } else {
                t = t * (1.0f / length);
            }
            const float handedness =
                core::dot(core::cross(vertex.normal, t), bitangents[v]) < 0.0f ? -1.0f : 1.0f;
            vertex.tangent = Vec4{t.x, t.y, t.z, handedness};
        }

        model.name = name;
        model.nodes.push_back(asset::Node{name, -1, core::Mat4::identity()});
        model.bones.push_back(asset::Bone{name, 0, core::Mat4::identity()});

        asset::MaterialData material{};
        material.name = "Default";
        // Gris medio en lineal (~0.8 en sRGB), como el material por defecto
        // de Unity: ni brilla ni se come la luz.
        material.base_color = Vec4{0.6f, 0.6f, 0.6f, 1.0f};
        material.roughness = 0.5f;
        material.metallic = 0.0f;
        model.materials.push_back(material);

        asset::SubMesh submesh{};
        submesh.first_index = 0;
        submesh.index_count = static_cast<std::uint32_t>(model.indices.size());
        submesh.material = 0;
        submesh.node = 0;
        model.submeshes.push_back(submesh);
        asset::computeSubmeshBounds(model);
        return std::move(model);
    }
};

ModelData cube() {
    MeshBuilder b;
    // Cada cara con sus 4 vertices (normales planas y UV 0..1 por cara).
    const Vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const Vec3& n : normals) {
        // Base de la cara: u y v perpendiculares a la normal, con u x v = n.
        const Vec3 up = std::abs(n.y) > 0.5f ? Vec3{0.0f, 0.0f, n.y > 0.0f ? -1.0f : 1.0f}
                                             : Vec3{0.0f, 1.0f, 0.0f};
        const Vec3 u = core::cross(up, n);
        const Vec3 v = core::cross(n, u);
        const Vec3 c = n * 0.5f;
        const std::uint32_t a0 = b.vertex(c - u * 0.5f - v * 0.5f, n, Vec2{0.0f, 1.0f});
        const std::uint32_t a1 = b.vertex(c + u * 0.5f - v * 0.5f, n, Vec2{1.0f, 1.0f});
        const std::uint32_t a2 = b.vertex(c + u * 0.5f + v * 0.5f, n, Vec2{1.0f, 0.0f});
        const std::uint32_t a3 = b.vertex(c - u * 0.5f + v * 0.5f, n, Vec2{0.0f, 0.0f});
        b.quad(a0, a1, a2, a3);
    }
    return b.finish("Cube");
}

// Esfera UV: `rings` paralelos y `segments` meridianos.
ModelData sphere(float radius = 0.5f, int segments = 48, int rings = 24) {
    MeshBuilder b;
    for (int r = 0; r <= rings; ++r) {
        const float v = static_cast<float>(r) / static_cast<float>(rings);
        const float theta = v * kPi;
        for (int s = 0; s <= segments; ++s) {
            const float u = static_cast<float>(s) / static_cast<float>(segments);
            const float phi = u * 2.0f * kPi;
            const Vec3 n{std::sin(theta) * std::cos(phi), std::cos(theta),
                         -std::sin(theta) * std::sin(phi)};
            b.vertex(n * radius, n, Vec2{u, v});
        }
    }
    const int stride = segments + 1;
    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < segments; ++s) {
            const auto i0 = static_cast<std::uint32_t>(r * stride + s);
            const auto i1 = static_cast<std::uint32_t>((r + 1) * stride + s);
            b.quad(i0, i1, i1 + 1, i0 + 1);
        }
    }
    return b.finish("Sphere");
}

// Plano de 10 x 10 m mirando hacia +Y, en 10 x 10 cuadros.
ModelData plane() {
    MeshBuilder b;
    constexpr int kCells = 10;
    for (int z = 0; z <= kCells; ++z) {
        for (int x = 0; x <= kCells; ++x) {
            const float fx = static_cast<float>(x) / kCells;
            const float fz = static_cast<float>(z) / kCells;
            b.vertex(Vec3{-5.0f + 10.0f * fx, 0.0f, -5.0f + 10.0f * fz}, Vec3{0.0f, 1.0f, 0.0f},
                     Vec2{fx, fz});
        }
    }
    constexpr int kStride = kCells + 1;
    for (int z = 0; z < kCells; ++z) {
        for (int x = 0; x < kCells; ++x) {
            const auto i0 = static_cast<std::uint32_t>(z * kStride + x);
            const auto i1 = static_cast<std::uint32_t>((z + 1) * kStride + x);
            b.quad(i0, i1, i1 + 1, i0 + 1);
        }
    }
    return b.finish("Plane");
}

// Cilindro (y capsula): cuerpo de altura `body` y radio `radius`; con
// `hemispheres` las tapas son medias esferas en lugar de discos.
ModelData cylinder(bool hemispheres) {
    MeshBuilder b;
    constexpr int kSegments = 48;
    const float radius = 0.5f;
    const float half = hemispheres ? 0.5f : 1.0f;  // la capsula mide 2 m con las tapas

    // Cuerpo.
    const auto body_start = static_cast<std::uint32_t>(b.model.vertices.size());
    for (int s = 0; s <= kSegments; ++s) {
        const float u = static_cast<float>(s) / kSegments;
        const float phi = u * 2.0f * kPi;
        const Vec3 n{std::cos(phi), 0.0f, -std::sin(phi)};
        b.vertex(Vec3{n.x * radius, half, n.z * radius}, n, Vec2{u, 0.0f});
        b.vertex(Vec3{n.x * radius, -half, n.z * radius}, n, Vec2{u, 1.0f});
    }
    for (int s = 0; s < kSegments; ++s) {
        const std::uint32_t top = body_start + static_cast<std::uint32_t>(s * 2);
        b.quad(top, top + 1, top + 3, top + 2);
    }

    if (!hemispheres) {
        // Tapas planas.
        for (const float y : {half, -half}) {
            const Vec3 n{0.0f, y > 0.0f ? 1.0f : -1.0f, 0.0f};
            const std::uint32_t center = b.vertex(Vec3{0.0f, y, 0.0f}, n, Vec2{0.5f, 0.5f});
            const auto ring = static_cast<std::uint32_t>(b.model.vertices.size());
            for (int s = 0; s <= kSegments; ++s) {
                const float phi = static_cast<float>(s) / kSegments * 2.0f * kPi;
                b.vertex(Vec3{std::cos(phi) * radius, y, -std::sin(phi) * radius}, n,
                         Vec2{0.5f + 0.5f * std::cos(phi), 0.5f + 0.5f * std::sin(phi)});
            }
            for (int s = 0; s < kSegments; ++s) {
                const std::uint32_t a = ring + static_cast<std::uint32_t>(s);
                if (y > 0.0f) {
                    b.triangle(center, a, a + 1);
                } else {
                    b.triangle(center, a + 1, a);
                }
            }
        }
        return b.finish("Cylinder");
    }

    // Tapas semiesfericas.
    constexpr int kRings = 12;
    for (const float sign : {1.0f, -1.0f}) {
        const auto start = static_cast<std::uint32_t>(b.model.vertices.size());
        for (int r = 0; r <= kRings; ++r) {
            const float theta = static_cast<float>(r) / kRings * (kPi * 0.5f);  // 0 = polo
            for (int s = 0; s <= kSegments; ++s) {
                const float u = static_cast<float>(s) / kSegments;
                const float phi = u * 2.0f * kPi;
                const Vec3 n{std::sin(theta) * std::cos(phi), sign * std::cos(theta),
                             -std::sin(theta) * std::sin(phi)};
                b.vertex(Vec3{n.x * radius, sign * half + n.y * radius, n.z * radius}, n,
                         Vec2{u, static_cast<float>(r) / kRings});
            }
        }
        constexpr int kStride = kSegments + 1;
        for (int r = 0; r < kRings; ++r) {
            for (int s = 0; s < kSegments; ++s) {
                const std::uint32_t i0 = start + static_cast<std::uint32_t>(r * kStride + s);
                const std::uint32_t i1 = start + static_cast<std::uint32_t>((r + 1) * kStride + s);
                if (sign > 0.0f) {
                    b.quad(i0, i1, i1 + 1, i0 + 1);
                } else {
                    b.quad(i0, i0 + 1, i1 + 1, i1);
                }
            }
        }
    }
    return b.finish("Capsule");
}

struct Builtin {
    Uuid uuid;
    const char* name;
};

constexpr Builtin kBuiltins[] = {
    {builtin::kCube, "Cube"},         {builtin::kSphere, "Sphere"},
    {builtin::kPlane, "Plane"},       {builtin::kCylinder, "Cylinder"},
    {builtin::kCapsule, "Capsule"},
};

}  // namespace

const std::vector<AssetInfo>& builtinInfos() {
    static const std::vector<AssetInfo> infos = [] {
        std::vector<AssetInfo> result;
        for (const Builtin& b : kBuiltins) {
            AssetInfo info{};
            info.uuid = b.uuid;
            info.type = AssetType::Model;
            info.name = b.name;
            result.push_back(info);
        }
        return result;
    }();
    return infos;
}

bool isBuiltin(const Uuid& uuid) {
    for (const Builtin& b : kBuiltins) {
        if (b.uuid == uuid) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<ModelAsset> make(const Uuid& uuid) {
    ModelData data;
    if (uuid == builtin::kCube) {
        data = cube();
    } else if (uuid == builtin::kSphere) {
        data = sphere();
    } else if (uuid == builtin::kPlane) {
        data = plane();
    } else if (uuid == builtin::kCylinder) {
        data = cylinder(false);
    } else if (uuid == builtin::kCapsule) {
        data = cylinder(true);
    } else {
        return nullptr;
    }
    auto asset = std::make_shared<ModelAsset>();
    asset->uuid = uuid;
    asset->name = data.name;
    asset->nodes.push_back(ModelNode{data.name, -1, core::Mat4::identity(), 0});
    asset->parts.push_back(std::make_shared<ModelData>(std::move(data)));
    return asset;
}

}  // namespace cramion::assets::primitives
