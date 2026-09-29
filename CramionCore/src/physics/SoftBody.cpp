#include "CramionCore/physics/SoftBody.h"

#include "CramionCore/ecs/RuntimeMesh.h"

#include <CramionFX/asset/Model.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <sstream>
#include <tuple>

namespace cramion::physics {

using core::Mat4;
using core::Quat;
using core::Vec3;

void SoftBody::reflect(ecs::PropertyVisitor& v) {
    static constexpr std::array<const char*, 2> kShapes = {"Esfera", "Cubo"};
    ecs::enumField(v, {"shape", "Forma"}, shape, kShapes);
    v.field({"size", "Tamano", "Metros (esfera: el diametro es X)"}, size, ecs::Vec3Kind::Scale);
    v.field({"resolution", "Resolucion", "Divisiones por lado de cada cara: mas = mas suave (y mas caro)"}, resolution, 2, 16);
    if (v.beginGroup("Simulacion")) {
        v.field({"mass", "Masa", "kg"}, mass, ecs::FloatRange{0.01f, 1000.0f, 0.01f, "%.2f kg"});
        v.field({"stiffness", "Firmeza", "0 = gelatina muy blanda, 1 = goma dura"}, stiffness,
                ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.field({"pressure", "Presion", "Aire de dentro: 0 = se desinfla, 1 = aguanta su peso, 3+ = balon"}, pressure,
                ecs::FloatRange{0.0f, 10.0f, 0.01f, "%.2f"});
        v.field({"damping", "Frenado", "Mas = tiembla menos rato"}, damping, ecs::FloatRange{0.0f, 5.0f, 0.01f, "%.2f"});
        v.field({"friction", "Friccion"}, friction, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.field({"restitution", "Rebote"}, restitution, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.field({"gravity_scale", "Escala de gravedad"}, gravity_scale, ecs::FloatRange{-5.0f, 5.0f, 0.01f, "%.2f"});
        v.field({"thickness", "Grosor", "Separacion con lo que toca"}, thickness, ecs::FloatRange{0.0f, 0.2f, 0.001f, "%.3f m"});
        v.field({"iterations", "Iteraciones", "Mas = mas precisa (y mas cara)"}, iterations, 1, 32);
        v.field({"collide", "Chocar", "Con los colliders y rigidbodies de su capa"}, collide);
        v.field({"follow_entity", "La entidad lo sigue", "La entidad va al centro del cuerpo (scripts, camaras)"}, follow_entity);
        v.endGroup();
    }
    if (v.beginGroup("Aspecto")) {
        v.field({"color", "Color", "Sin material .crmat en el Mesh Renderer"}, color, ecs::Vec3Kind::Color);
        v.field({"roughness", "Rugosidad", "Bajo = brillante, como gelatina"}, roughness, ecs::FloatRange{0.04f, 1.0f, 0.01f, "%.2f", true});
        v.field({"metallic", "Metalicidad"}, metallic, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.endGroup();
    }
    resolution = std::clamp(resolution, 2, 16);
    iterations = std::clamp(iterations, 1, 32);
}

std::string SoftBody::layoutKey() const {
    std::ostringstream key;
    key << static_cast<int>(shape) << "/" << size.x << "," << size.y << "," << size.z << "/" << std::clamp(resolution, 2, 16)
        << "/" << color.x << "," << color.y << "," << color.z << "/" << roughness << "/" << metallic;
    return key.str();
}

namespace {

// Caras del cubo: normal, eje u y eje v (u x v = normal), como Mesh::cube.
constexpr float kFaces[6][3][3] = {{{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},  {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
                                   {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},  {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
                                   {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},   {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}};

Vec3 faceAxis(int face, int axis) { return Vec3{kFaces[face][axis][0], kFaces[face][axis][1], kFaces[face][axis][2]}; }

// Punto de la cara en el cubo [-1, 1] y su posicion final (esfera o caja).
Vec3 shapePoint(const SoftBody& body, const Vec3& cube) {
    if (body.shape == SoftBodyShape::Sphere) {
        const float r = body.size.x * 0.5f;
        // Cubo -> esfera con el reparto de Nowell (triangulos mas parecidos que normalizar).
        const float x2 = cube.x * cube.x, y2 = cube.y * cube.y, z2 = cube.z * cube.z;
        const Vec3 s{cube.x * std::sqrt(std::max(0.0f, 1.0f - y2 * 0.5f - z2 * 0.5f + y2 * z2 / 3.0f)),
                     cube.y * std::sqrt(std::max(0.0f, 1.0f - z2 * 0.5f - x2 * 0.5f + z2 * x2 / 3.0f)),
                     cube.z * std::sqrt(std::max(0.0f, 1.0f - x2 * 0.5f - y2 * 0.5f + x2 * y2 / 3.0f))};
        return s * r;
    }
    return Vec3{cube.x * body.size.x * 0.5f, cube.y * body.size.y * 0.5f, cube.z * body.size.z * 0.5f};
}

// Rejilla de cada cara con los vertices soldados (las aristas se comparten).
struct Built {
    SoftBodyMesh mesh;
    struct RenderVertex {
        Vec3 position;
        Vec3 normal;
        core::Vec2 uv;
        Vec3 tangent;
        std::uint32_t particle = 0;
    };
    std::vector<RenderVertex> render;
    std::vector<std::uint32_t> render_indices;
};

Built build(const SoftBody& body) {
    Built out;
    const int n = std::clamp(body.resolution, 2, 16);
    std::map<std::tuple<int, int, int>, std::uint32_t> welded;
    for (int face = 0; face < 6; ++face) {
        const Vec3 normal = faceAxis(face, 0), u = faceAxis(face, 1), v = faceAxis(face, 2);
        std::vector<std::uint32_t> grid_particles(static_cast<std::size_t>((n + 1) * (n + 1)));
        const auto render_base = static_cast<std::uint32_t>(out.render.size());
        for (int j = 0; j <= n; ++j) {
            for (int i = 0; i <= n; ++i) {
                const float a = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(n);
                const float b = -1.0f + 2.0f * static_cast<float>(j) / static_cast<float>(n);
                const Vec3 cube = normal + u * a + v * b;
                // Clave entera en la rejilla del cubo (2n pasos por eje): las aristas coinciden.
                const auto key = std::make_tuple(static_cast<int>(std::lround((cube.x + 1.0f) * n)),
                                                 static_cast<int>(std::lround((cube.y + 1.0f) * n)),
                                                 static_cast<int>(std::lround((cube.z + 1.0f) * n)));
                auto it = welded.find(key);
                if (it == welded.end()) {
                    it = welded.emplace(key, static_cast<std::uint32_t>(out.mesh.particles.size())).first;
                    out.mesh.particles.push_back(shapePoint(body, cube));
                }
                grid_particles[static_cast<std::size_t>(j * (n + 1) + i)] = it->second;
                Built::RenderVertex rv;
                rv.position = shapePoint(body, cube);
                rv.normal = body.shape == SoftBodyShape::Sphere ? core::normalize(rv.position) : normal;
                rv.uv = core::Vec2{static_cast<float>(i) / static_cast<float>(n), 1.0f - static_cast<float>(j) / static_cast<float>(n)};
                rv.tangent = u;
                rv.particle = it->second;
                out.render.push_back(rv);
            }
        }
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                const auto at = [&](int x, int y) { return static_cast<std::size_t>(y * (n + 1) + x); };
                const std::size_t q[4] = {at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)};
                // Diagonal alterna: se deforma igual en todas direcciones.
                const bool flip = (i + j) % 2 == 1;
                const std::size_t tris[2][3] = {{q[0], q[1], flip ? q[3] : q[2]}, {flip ? q[1] : q[0], q[2], q[3]}};
                for (const auto& t : tris) {
                    for (const std::size_t k : t) {
                        out.mesh.triangles.push_back(grid_particles[k]);
                        out.render_indices.push_back(render_base + static_cast<std::uint32_t>(k));
                    }
                }
            }
        }
    }
    softBodyNormals(out.mesh.triangles, out.mesh.particles, out.mesh.rest_normals);
    return out;
}

// Giro mas corto de a a b (unitarios).
Quat fromTo(const Vec3& a, const Vec3& b) {
    const float d = core::dot(a, b);
    if (d < -0.9999f) {
        Vec3 axis = core::cross(Vec3{1.0f, 0.0f, 0.0f}, a);
        if (core::length(axis) < 1e-3f) axis = core::cross(Vec3{0.0f, 1.0f, 0.0f}, a);
        axis = core::normalize(axis);
        return Quat{axis.x, axis.y, axis.z, 0.0f};
    }
    const Vec3 c = core::cross(a, b);
    return core::normalize(Quat{c.x, c.y, c.z, 1.0f + d});
}

}  // namespace

void softBodyNormals(const std::vector<std::uint32_t>& triangles, const std::vector<Vec3>& positions, std::vector<Vec3>& normals) {
    normals.assign(positions.size(), Vec3{});
    for (std::size_t t = 0; t + 2 < triangles.size(); t += 3) {
        const std::uint32_t a = triangles[t], b = triangles[t + 1], c = triangles[t + 2];
        if (a >= positions.size() || b >= positions.size() || c >= positions.size()) continue;
        const Vec3 n = core::cross(positions[b] - positions[a], positions[c] - positions[a]);  // pesada por el area
        normals[a] = normals[a] + n;
        normals[b] = normals[b] + n;
        normals[c] = normals[c] + n;
    }
    for (Vec3& n : normals) {
        const float len = core::length(n);
        n = len > 1e-12f ? n * (1.0f / len) : Vec3{0.0f, 1.0f, 0.0f};
    }
}

SoftBodyMesh softBodyMesh(const SoftBody& body) { return build(body).mesh; }

asset::ModelData softBodyModel(const SoftBody& body) {
    const Built b = build(body);
    asset::ModelData data;
    data.name = "Cuerpo blando";
    data.vertices.resize(b.render.size());
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (std::size_t i = 0; i < b.render.size(); ++i) {
        const Built::RenderVertex& r = b.render[i];
        asset::SkinnedVertex& v = data.vertices[i];
        v.position = r.position;
        v.normal = r.normal;
        v.uv = r.uv;
        v.tangent = core::Vec4{r.tangent.x, r.tangent.y, r.tangent.z, 1.0f};
        v.joints[0] = r.particle;
        v.weights[0] = 1.0f;
        lo = Vec3{std::min(lo.x, r.position.x), std::min(lo.y, r.position.y), std::min(lo.z, r.position.z)};
        hi = Vec3{std::max(hi.x, r.position.x), std::max(hi.y, r.position.y), std::max(hi.z, r.position.z)};
    }
    data.indices = b.render_indices;
    asset::SubMesh sub;
    sub.index_count = static_cast<std::uint32_t>(data.indices.size());
    sub.bounds_min = lo;
    sub.bounds_max = hi;
    data.submeshes.push_back(sub);
    ecs::MeshMaterial look;
    look.color = core::Vec4{body.color.x, body.color.y, body.color.z, 1.0f};
    look.roughness = body.roughness;
    look.metallic = body.metallic;
    asset::MaterialData material;
    material.name = "Cuerpo blando";
    ecs::Mesh::applyFactors(look, &material);
    data.materials.push_back(material);
    for (std::size_t i = 0; i < b.mesh.particles.size(); ++i) {
        const Vec3& p = b.mesh.particles[i];
        Mat4 local = Mat4::identity();
        local.m[3][0] = p.x, local.m[3][1] = p.y, local.m[3][2] = p.z;
        Mat4 offset = Mat4::identity();
        offset.m[3][0] = -p.x, offset.m[3][1] = -p.y, offset.m[3][2] = -p.z;
        const std::string name = "p" + std::to_string(i);
        data.nodes.push_back(asset::Node{name, -1, local});
        data.bones.push_back(asset::Bone{name, static_cast<std::int32_t>(i), offset});
    }
    return data;
}

void softBodyBoneGlobals(const SoftBodyMesh& mesh, const Mat4& entity_world, const std::vector<Vec3>& world_positions,
                         std::vector<Mat4>& globals) {
    if (world_positions.size() != mesh.particles.size() || mesh.rest_normals.size() != mesh.particles.size()) return;
    std::vector<Vec3> normals;
    softBodyNormals(mesh.triangles, world_positions, normals);
    globals.resize(world_positions.size());
    const Mat4 to_local = core::inverse(entity_world);
    // Normal de reposo en el mundo (la entidad puede estar girada).
    for (std::size_t i = 0; i < world_positions.size(); ++i) {
        const Vec3 rest_world = core::normalize(Vec3{
            entity_world.m[0][0] * mesh.rest_normals[i].x + entity_world.m[1][0] * mesh.rest_normals[i].y + entity_world.m[2][0] * mesh.rest_normals[i].z,
            entity_world.m[0][1] * mesh.rest_normals[i].x + entity_world.m[1][1] * mesh.rest_normals[i].y + entity_world.m[2][1] * mesh.rest_normals[i].z,
            entity_world.m[0][2] * mesh.rest_normals[i].x + entity_world.m[1][2] * mesh.rest_normals[i].y + entity_world.m[2][2] * mesh.rest_normals[i].z});
        // Giro del mundo: lo que giro la normal desde el reposo, sobre el giro de la entidad.
        const Quat turn = fromTo(rest_world, normals[i]);
        Mat4 world = core::composeTrs(world_positions[i], turn, Vec3{1.0f, 1.0f, 1.0f}) * entity_world;
        // Solo el giro de la entidad (sin su traslacion): la particula va en su sitio.
        world.m[3][0] = world_positions[i].x;
        world.m[3][1] = world_positions[i].y;
        world.m[3][2] = world_positions[i].z;
        world.m[3][3] = 1.0f;
        globals[i] = to_local * world;
    }
}

}  // namespace cramion::physics
