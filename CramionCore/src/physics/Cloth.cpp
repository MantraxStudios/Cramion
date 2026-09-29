#include "CramionCore/physics/Cloth.h"

#include "CramionCore/ecs/RuntimeMesh.h"

#include <CramionFX/asset/Model.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>

namespace cramion::physics {

using core::Mat4;
using core::Vec3;

void Cloth::reflect(ecs::PropertyVisitor& v) {
    const bool all = v.wantsAllFields();
    v.field({"width", "Ancho", "Metros (X local; la escala de la entidad lo agranda)"}, width,
            ecs::FloatRange{0.05f, 100.0f, 0.01f, "%.2f m"});
    v.field({"height", "Alto", "Metros (Y local)"}, height, ecs::FloatRange{0.05f, 100.0f, 0.01f, "%.2f m"});
    v.field({"segments_x", "Divisiones X", "Cuadros a lo ancho: mas = pliegues mas finos (y mas caro)"}, segments_x, 1, 96);
    v.field({"segments_y", "Divisiones Y"}, segments_y, 1, 96);
    static constexpr std::array<const char*, 6> kPins = {"Ninguna (cae entera)", "Borde de arriba (cortina)",
                                                         "Esquinas de arriba", "Borde izquierdo (bandera)",
                                                         "Las cuatro esquinas", "El centro"};
    ecs::enumField(v, {"pin", "Fijada por", "Particulas pegadas a la entidad (la siguen al moverla)"}, pin, kPins);
    if (v.beginGroup("Simulacion")) {
        v.field({"mass", "Masa", "kg de toda la tela"}, mass, ecs::FloatRange{0.01f, 1000.0f, 0.01f, "%.2f kg"});
        v.field({"stiffness", "Rigidez", "0 = elastica, 1 = no se estira"}, stiffness, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.field({"bending", "Resistencia a doblarse", "0 = se arruga libre (seda), 1 = rigida (lona)"}, bending,
                ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.field({"damping", "Frenado", "Resistencia del aire"}, damping, ecs::FloatRange{0.0f, 5.0f, 0.01f, "%.2f"});
        v.field({"friction", "Friccion"}, friction, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.field({"thickness", "Grosor", "Separacion con lo que toca (evita que se meta dentro)"}, thickness,
                ecs::FloatRange{0.0f, 0.2f, 0.001f, "%.3f m"});
        v.field({"gravity_scale", "Escala de gravedad"}, gravity_scale, ecs::FloatRange{-5.0f, 5.0f, 0.01f, "%.2f"});
        v.field({"iterations", "Iteraciones", "Mas = mas precisa y rigida (y mas cara)"}, iterations, 1, 32);
        v.field({"collide", "Chocar", "Con los colliders y rigidbodies de su capa"}, collide);
        v.endGroup();
    }
    if (v.beginGroup("Viento")) {
        v.field({"wind", "Viento", "m/s en el mundo (una bandera: 5..12)"}, wind, ecs::Vec3Kind::Position);
        v.field({"turbulence", "Rachas", "0 = constante, 1 = rachas fuertes"}, turbulence, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.field({"air_drag", "Empuje del aire", "Cuanto la mueve el viento"}, air_drag, ecs::FloatRange{0.0f, 10.0f, 0.01f, "%.2f"});
        v.endGroup();
    }
    if (v.beginGroup("Aspecto")) {
        v.field({"color", "Color", "Sin material .crmat en el Mesh Renderer"}, color, ecs::Vec3Kind::Color);
        v.field({"roughness", "Rugosidad"}, roughness, ecs::FloatRange{0.04f, 1.0f, 0.01f, "%.2f", true});
        v.field({"double_sided", "Doble cara", "Se ve por detras"}, double_sided);
        v.endGroup();
    }
    (void)all;
    segments_x = std::clamp(segments_x, 1, 96);
    segments_y = std::clamp(segments_y, 1, 96);
    iterations = std::clamp(iterations, 1, 32);
}

int Cloth::particlesX() const { return std::clamp(segments_x, 1, 96) + 1; }
int Cloth::particlesY() const { return std::clamp(segments_y, 1, 96) + 1; }
int Cloth::particleCount() const { return particlesX() * particlesY(); }

std::string Cloth::layoutKey() const {
    std::ostringstream key;
    key << width << "x" << height << "/" << particlesX() << "x" << particlesY() << "/" << static_cast<int>(pin) << "/"
        << double_sided << "/" << color.x << "," << color.y << "," << color.z << "/" << roughness;
    return key.str();
}

std::vector<Vec3> clothRestPositions(const Cloth& cloth) {
    const int nx = cloth.particlesX();
    const int ny = cloth.particlesY();
    std::vector<Vec3> out(static_cast<std::size_t>(nx * ny));
    for (int y = 0; y < ny; ++y) {
        for (int x = 0; x < nx; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(nx - 1);
            const float v = static_cast<float>(y) / static_cast<float>(ny - 1);
            // Fila 0 arriba (+Y), columna 0 a la izquierda (-X).
            out[static_cast<std::size_t>(y * nx + x)] = Vec3{(u - 0.5f) * cloth.width, (0.5f - v) * cloth.height, 0.0f};
        }
    }
    return out;
}

bool clothPinned(const Cloth& cloth, int x, int y) {
    const int last_x = cloth.particlesX() - 1;
    const int last_y = cloth.particlesY() - 1;
    switch (cloth.pin) {
        case ClothPin::None: return false;
        case ClothPin::TopEdge: return y == 0;
        case ClothPin::TopCorners: return y == 0 && (x == 0 || x == last_x);
        case ClothPin::LeftEdge: return x == 0;
        case ClothPin::AllCorners: return (x == 0 || x == last_x) && (y == 0 || y == last_y);
        case ClothPin::Center: return x == last_x / 2 && y == last_y / 2;
    }
    return false;
}

std::vector<std::uint32_t> clothTriangles(const Cloth& cloth) {
    const int nx = cloth.particlesX();
    const int ny = cloth.particlesY();
    std::vector<std::uint32_t> out;
    out.reserve(static_cast<std::size_t>((nx - 1) * (ny - 1) * 6));
    for (int y = 0; y + 1 < ny; ++y) {
        for (int x = 0; x + 1 < nx; ++x) {
            const auto a = static_cast<std::uint32_t>(y * nx + x);        // arriba izquierda
            const auto b = a + 1;                                          // arriba derecha
            const auto c = static_cast<std::uint32_t>((y + 1) * nx + x);  // abajo izquierda
            const auto d = c + 1;
            // Diagonales alternas: se pliega igual hacia los dos lados.
            if ((x + y) % 2 == 0) {
                out.insert(out.end(), {c, d, b, c, b, a});
            } else {
                out.insert(out.end(), {c, d, a, d, b, a});
            }
        }
    }
    return out;
}

namespace {

// Ejes de la tela en cada particula: +U (a la derecha), +V (hacia arriba) y la normal.
void clothFrame(const Cloth& cloth, const std::vector<Vec3>& p, int x, int y, Vec3& tangent, Vec3& bitangent, Vec3& normal) {
    const int nx = cloth.particlesX();
    const int ny = cloth.particlesY();
    const auto at = [&](int i, int j) { return p[static_cast<std::size_t>(j * nx + i)]; };
    const Vec3 du = at(std::min(x + 1, nx - 1), y) - at(std::max(x - 1, 0), y);
    const Vec3 dv = at(x, std::max(y - 1, 0)) - at(x, std::min(y + 1, ny - 1));  // fila 0 arriba
    Vec3 n = core::cross(du, dv);
    const float len = core::length(n);
    normal = len > 1e-9f ? n * (1.0f / len) : Vec3{0.0f, 0.0f, 1.0f};
    const float lu = core::length(du);
    tangent = lu > 1e-9f ? du * (1.0f / lu) : Vec3{1.0f, 0.0f, 0.0f};
    // Ortonormal (la tela se cizalla).
    tangent = tangent - normal * core::dot(tangent, normal);
    const float lt = core::length(tangent);
    tangent = lt > 1e-9f ? tangent * (1.0f / lt) : Vec3{1.0f, 0.0f, 0.0f};
    bitangent = core::cross(normal, tangent);
}

}  // namespace

void clothNormals(const Cloth& cloth, const std::vector<Vec3>& positions, std::vector<Vec3>& normals) {
    const int nx = cloth.particlesX();
    const int ny = cloth.particlesY();
    normals.resize(positions.size());
    if (positions.size() != static_cast<std::size_t>(nx * ny)) return;
    for (int y = 0; y < ny; ++y) {
        for (int x = 0; x < nx; ++x) {
            Vec3 t, b;
            clothFrame(cloth, positions, x, y, t, b, normals[static_cast<std::size_t>(y * nx + x)]);
        }
    }
}

asset::ModelData clothModel(const Cloth& cloth) {
    asset::ModelData data;
    data.name = "Tela";
    const int nx = cloth.particlesX();
    const int ny = cloth.particlesY();
    const std::vector<Vec3> rest = clothRestPositions(cloth);
    const std::size_t count = rest.size();
    const int sides = cloth.double_sided ? 2 : 1;
    data.vertices.resize(count * static_cast<std::size_t>(sides));
    for (int side = 0; side < sides; ++side) {
        for (int y = 0; y < ny; ++y) {
            for (int x = 0; x < nx; ++x) {
                const std::size_t i = static_cast<std::size_t>(y * nx + x);
                asset::SkinnedVertex& v = data.vertices[i + count * static_cast<std::size_t>(side)];
                v.position = rest[i];
                v.normal = Vec3{0.0f, 0.0f, side == 0 ? 1.0f : -1.0f};
                v.uv = core::Vec2{static_cast<float>(x) / static_cast<float>(nx - 1), static_cast<float>(y) / static_cast<float>(ny - 1)};
                // La cara de atras tiene las U al reves (la textura se lee bien por detras).
                v.tangent = core::Vec4{side == 0 ? 1.0f : -1.0f, 0.0f, 0.0f, 1.0f};
                v.joints[0] = static_cast<std::uint32_t>(i);
                v.weights[0] = 1.0f;
            }
        }
    }
    const std::vector<std::uint32_t> front = clothTriangles(cloth);
    data.indices = front;
    if (sides == 2) {
        for (std::size_t t = 0; t < front.size(); t += 3) {
            const auto base = static_cast<std::uint32_t>(count);
            data.indices.insert(data.indices.end(), {front[t] + base, front[t + 2] + base, front[t + 1] + base});
        }
    }
    asset::SubMesh sub;
    sub.first_index = 0;
    sub.index_count = static_cast<std::uint32_t>(data.indices.size());
    sub.material = 0;
    sub.bounds_min = Vec3{-cloth.width * 0.5f, -cloth.height * 0.5f, -0.01f};
    sub.bounds_max = Vec3{cloth.width * 0.5f, cloth.height * 0.5f, 0.01f};
    data.submeshes.push_back(sub);

    ecs::MeshMaterial look;
    look.color = core::Vec4{cloth.color.x, cloth.color.y, cloth.color.z, 1.0f};
    look.roughness = cloth.roughness;
    asset::MaterialData material;
    material.name = "Tela";
    ecs::Mesh::applyFactors(look, &material);
    data.materials.push_back(material);

    // Un nodo y un hueso por particula (sin padres), en su sitio de reposo.
    data.nodes.reserve(count);
    data.bones.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        Mat4 local = Mat4::identity();
        local.m[3][0] = rest[i].x;
        local.m[3][1] = rest[i].y;
        local.m[3][2] = rest[i].z;
        Mat4 offset = Mat4::identity();
        offset.m[3][0] = -rest[i].x;
        offset.m[3][1] = -rest[i].y;
        offset.m[3][2] = -rest[i].z;
        const std::string name = "p" + std::to_string(i);
        data.nodes.push_back(asset::Node{name, -1, local});
        data.bones.push_back(asset::Bone{name, static_cast<std::int32_t>(i), offset});
    }
    return data;
}

void clothBoneGlobals(const Cloth& cloth, const Mat4& entity_world, const std::vector<Vec3>& world_positions,
                      std::vector<Mat4>& globals) {
    const int nx = cloth.particlesX();
    const int ny = cloth.particlesY();
    if (world_positions.size() != static_cast<std::size_t>(nx * ny)) return;
    globals.resize(world_positions.size());
    const Mat4 to_local = core::inverse(entity_world);
    for (int y = 0; y < ny; ++y) {
        for (int x = 0; x < nx; ++x) {
            const std::size_t i = static_cast<std::size_t>(y * nx + x);
            Vec3 t, b, n;
            clothFrame(cloth, world_positions, x, y, t, b, n);
            Mat4 world = Mat4::identity();
            world.m[0][0] = t.x, world.m[0][1] = t.y, world.m[0][2] = t.z;
            world.m[1][0] = b.x, world.m[1][1] = b.y, world.m[1][2] = b.z;
            world.m[2][0] = n.x, world.m[2][1] = n.y, world.m[2][2] = n.z;
            world.m[3][0] = world_positions[i].x;
            world.m[3][1] = world_positions[i].y;
            world.m[3][2] = world_positions[i].z;
            globals[i] = to_local * world;
        }
    }
}

}  // namespace cramion::physics
