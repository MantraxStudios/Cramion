// Pruebas de las mallas creadas por codigo (consola, sin GPU): primitivas con
// las caras hacia fuera, normales y tangentes, validacion, conversion al
// formato del renderizador, la API de los scripts (Mesh, entity.mesh,
// addComponent) y el MeshCollider con Jolt (tambien al cambiar la malla).
// Devuelve 0 si todo va.

#include <iostream>
#include <CramionFX/asset/Dds.h>
#include <CramionFX/asset/TextureCompression.h>
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/RuntimeMesh.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/StaticBatching.h"
#include "CramionCore/ecs/FloatingOrigin.h"
#include "CramionCore/asset/AssetDatabase.h"
#include "CramionCore/asset/AssetManager.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/scripting/NativeApi.h"
#include "CramionCore/scripting/Scripting.h"
#include "CramionCore/asset/Importer.h"
#include "CramionCore/asset/MaterialAsset.h"
#include "CramionCore/ui/UI.h"

#include <CramionFX/asset/ImageFile.h>
#include <CramionFX/asset/Model.h>
#include <CramionFX/vk/FrameBudget.h>
#include <CramionFX/vk/GraphicsSettings.h>

#include <cmath>
#include <cstdio>
#include <array>
#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <fstream>

using namespace cramion;
using core::Vec3;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

// Cada triangulo es antihorario visto desde fuera: su normal geometrica va
// hacia donde apuntan las normales de sus vertices.
bool outward(const ecs::Mesh& m) {
    for (int s = 0; s < m.subMeshCount(); ++s) {
        const auto& t = m.triangles(s);
        for (std::size_t i = 0; i + 2 < t.size(); i += 3) {
            const Vec3 a = m.vertices[t[i]], b = m.vertices[t[i + 1]], c = m.vertices[t[i + 2]];
            const Vec3 face = core::cross(b - a, c - a);
            if (core::length(face) < 1e-9f) continue;  // polos de la esfera
            const Vec3 n = m.normals[t[i]] + m.normals[t[i + 1]] + m.normals[t[i + 2]];
            if (core::dot(face, n) <= 0.0f) return false;
        }
    }
    return true;
}

void testPrimitives() {
    std::printf("Primitivas\n");
    const std::shared_ptr<ecs::Mesh> meshes[] = {ecs::Mesh::cube(Vec3{2.0f, 1.0f, 1.0f}), ecs::Mesh::quad(),
                                                 ecs::Mesh::plane(4.0f, 4.0f, 3, 3),       ecs::Mesh::sphere(1.0f, 16, 8),
                                                 ecs::Mesh::cylinder(0.5f, 2.0f, 12),      ecs::Mesh::capsule(0.5f, 2.0f, 12, 4)};
    bool valid = true, faces = true, tangents = true;
    for (const auto& m : meshes) {
        valid = valid && m->validate().empty();
        faces = faces && outward(*m);
        for (std::size_t i = 0; i < m->tangents.size(); ++i) {
            const Vec3 t{m->tangents[i].x, m->tangents[i].y, m->tangents[i].z};
            tangents = tangents && std::abs(core::length(t) - 1.0f) < 1e-3f && std::abs(core::dot(t, m->normals[i])) < 1e-3f;
        }
        if (!outward(*m)) std::printf("  (%s con caras hacia dentro)\n", m->name.c_str());
    }
    check(valid, "todas se pueden dibujar (validate)");
    check(faces, "caras antihorarias vistas desde fuera");
    check(tangents, "tangentes unitarias y perpendiculares a la normal");
    const auto& cube = meshes[0];
    check(cube->vertices.size() == 24 && cube->triangleCount() == 12, "el cubo: 24 vertices (aristas duras) y 12 triangulos");
    check(std::abs(cube->boundsMin().x + 1.0f) < 1e-5f && std::abs(cube->boundsMax().y - 0.5f) < 1e-5f, "limites del cubo");
    const auto& sphere = meshes[3];
    bool on_sphere = true;
    for (const Vec3& p : sphere->vertices) on_sphere = on_sphere && std::abs(core::length(p) - 1.0f) < 1e-4f;
    check(on_sphere, "la esfera tiene sus vertices a su radio");
}

void testEditing() {
    std::printf("Crear y editar\n");
    ecs::Mesh m;
    check(!m.validate().empty(), "una malla vacia no se dibuja");
    m.vertices = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    m.uv = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    m.setTriangles({0, 1, 2, 0, 2, 7});
    check(m.validate().find("vertice 7") != std::string::npos, "detecta un indice fuera de rango");
    m.setTriangles({0, 1, 2, 0, 2, 3});
    m.recalculateNormals();
    check(core::length(m.normals[0] - Vec3{0, 0, 1}) < 1e-5f, "recalculateNormals: la normal de un quad en XY es +Z");
    m.recalculateTangents();
    check(core::length(Vec3{m.tangents[0].x, m.tangents[0].y, m.tangents[0].z} - Vec3{1, 0, 0}) < 1e-4f,
          "recalculateTangents sigue a +U");
    const std::uint64_t before = m.version();
    m.markModified();
    check(m.version() > before, "markModified sube la version");
    // Dos submallas con dos materiales.
    m.setTriangles({0, 2, 3}, 1);
    m.setTriangles({0, 1, 2}, 0);
    m.materials.resize(2);
    m.materials[1].color = core::Vec4{1.0f, 0.0f, 0.0f, 1.0f};
    const asset::ModelData data = m.toModelData();
    check(data.submeshes.size() == 2 && data.materials.size() == 2 && data.submeshes[1].material == 1 &&
              data.materials[1].base_color.x > 0.9f && data.materials[1].base_color.y < 0.01f,
          "cada submalla es un hueco de material");
    check(data.bones.size() == 1 && data.vertices.size() == 4 && data.indices.size() == 6, "modelo rigido para el renderizador");
    // Sin normales ni tangentes: se calculan al convertir.
    ecs::Mesh bare;
    bare.vertices = m.vertices;
    bare.setTriangles({0, 1, 2, 0, 2, 3});
    const asset::ModelData filled = bare.toModelData();
    check(core::length(filled.vertices[0].normal - Vec3{0, 0, 1}) < 1e-5f && bare.normals.empty(),
          "las normales que faltan se calculan (sin tocar la malla)");
}

void testApiAndPhysics() {
    std::printf("API de los scripts y MeshCollider\n");
    using scripting::api::Value;
    physics::registerPhysicsComponents();
    ecs::World world;
    physics::PhysicsSystem physics;
    physics.start(world);
    scripting::ScriptSystem scripts;
    scripts.setPhysics(&physics);
    scripts.start(world);
    scripting::api::NativeApi& api = scripts.nativeApi();
    const auto vec3s = [](std::initializer_list<Vec3> list) {
        Value::Array out;
        for (const Vec3& v : list) out.emplace_back(v);
        return Value(std::move(out));
    };
    const auto ints = [](std::initializer_list<int> list) {
        Value::Array out;
        for (const int i : list) out.emplace_back(i);
        return Value(std::move(out));
    };
    const auto material = [](Vec3 color, double roughness) {
        Value m = Value::object();
        m.set("color", Value(color));
        m.set("roughness", Value(roughness));
        return m;
    };

    const Value suelo = api.call("Scene.create", {Value("Suelo"), Value(Vec3{0, 0, 0})});
    const Value m = api.call("Mesh.new", {Value("Rampa")});
    api.set("vertices", vec3s({{-5, 0, -5}, {5, 0, -5}, {5, 0, 5}, {-5, 0, 5}}), m);
    api.set("uv", vec3s({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}), m);
    api.set("triangles", ints({0, 2, 1, 0, 3, 2}), m);
    api.call("recalculateNormals", {}, m);
    api.call("setMaterial", {Value(0), material({0.2f, 0.8f, 0.3f}, 0.4)}, m);
    api.set("mesh", m, suelo);
    api.call("Entity:addComponent", {Value("MeshCollider")}, suelo);
    const Value e = api.call("Scene.create", {Value("Bola"), Value(Vec3{0, 3, 0})});
    api.set("mesh", api.call("Mesh.sphere", {Value(0.5)}), e);
    api.call("Entity:addComponent", {Value("SphereCollider")}, e);
    api.call("Entity:addComponent", {Value("Rigidbody")}, e);
    const Value normals = api.get("normals", m);
    const double vertices = api.get("vertexCount", m).asNumber();
    const double triangles = api.get("triangleCount", m).asNumber();
    std::printf("  (%g vertices, %g triangulos)\n", vertices, triangles);
    check(vertices == 4 && triangles == 2 && normals.isArray() && std::abs(normals[0].asVec3().y - 1.0f) < 1e-4f,
          "Mesh.new, vertices, uv, triangles y recalculateNormals desde la API");
    const ecs::Entity floor = world.findByName("Suelo");
    const ecs::Entity ball = world.findByName("Bola");
    check(floor.valid() && floor.has<ecs::MeshRenderer>() && floor.get<ecs::MeshRenderer>().mesh &&
              floor.get<ecs::MeshRenderer>().mesh->materials.size() == 1 && floor.has<physics::MeshCollider>(),
          "entity.mesh anade el MeshRenderer y addComponent el MeshCollider");
    check(ball.valid() && ball.get<ecs::MeshRenderer>().mesh && ball.get<ecs::MeshRenderer>().mesh->vertices.size() > 100,
          "Mesh.sphere desde la API");
    // La bola cae sobre la malla y se para en ella.
    ball.get<physics::Rigidbody>().interpolate = false;
    for (int i = 0; i < 180; ++i) physics.update(world, 1.0f / 60.0f);
    const float rest = ball.worldPosition().y;
    std::printf("  (la bola reposa en y = %.2f)\n", static_cast<double>(rest));
    check(std::abs(rest - 0.5f) < 0.08f, "un Rigidbody choca con la malla creada por codigo");
    // Bajar la malla 2 m: el MeshCollider se rehace y la bola cae hasta ella.
    {
        const Value floor_mesh = api.get("mesh", api.call("Scene.find", {Value("Suelo")}));
        const Value v = api.get("vertices", floor_mesh);
        Value::Array moved;
        for (std::size_t i = 0; i < v.size(); ++i) moved.emplace_back(v[i].asVec3() - Vec3{0, 2, 0});
        api.set("vertices", Value(std::move(moved)), floor_mesh);
    }
    for (int i = 0; i < 120; ++i) physics.update(world, 1.0f / 60.0f);
    std::printf("  (tras bajar la malla, y = %.2f)\n", static_cast<double>(ball.worldPosition().y));
    check(std::abs(ball.worldPosition().y + 1.5f) < 0.15f, "al cambiar la malla se rehace su MeshCollider");
    // Errores claros.
    {
        const Value bad = api.call("Mesh.new");
        api.set("vertices", vec3s({{0, 0, 0}}), bad);
        api.set("triangles", ints({0, 1, 2}), bad);
        check(api.call("validate", {}, bad).asString().find("vertice 1") != std::string::npos,
              "mesh:validate() explica que falla");
    }
    // El ejemplo "Terreno procedural" del manual (docs/manual/ejemplo-terreno.html).
    {
        const Value terreno = api.call("Scene.create", {Value("Terreno")});
        const float tamano = 60.0f, altura = 4.0f;
        const Value malla = api.call("Mesh.plane", {Value(tamano), Value(tamano), Value(80), Value(80)});
        const Value v = api.get("vertices", malla);
        Value::Array out;
        for (std::size_t i = 0; i < v.size(); ++i) {
            Vec3 p = v[i].asVec3();
            p.y = (std::sin(p.x * 0.15f) + std::cos(p.z * 0.12f)) * altura * 0.5f;
            out.emplace_back(p);
        }
        api.set("vertices", Value(std::move(out)), malla);
        api.call("recalculateNormals", {}, malla);
        api.call("recalculateTangents", {}, malla);
        api.call("setMaterial", {Value(0), material({0.35f, 0.6f, 0.25f}, 0.9)}, malla);
        api.set("mesh", malla, terreno);
        api.call("Entity:addComponent", {Value("MeshCollider")}, terreno);
        const std::string summary = std::to_string(static_cast<int>(api.get("vertexCount", malla).asNumber())) + " " +
                                    std::to_string(static_cast<int>(api.get("triangleCount", malla).asNumber())) + " " +
                                    api.call("validate", {}, malla).asString();
        std::printf("  (terreno de la doc: %s)\n", summary.c_str());
        check(summary == "6561 12800 " && world.findByName("Terreno").has<physics::MeshCollider>(),
              "el ejemplo de terreno procedural de la documentacion funciona");
    }
    check(scripts.errors().empty(), "sin errores de los scripts");
    scripts.stop();
    physics.stop();
}

void testMaterials() {
    std::printf("Materiales y efectos\n");
    const auto wire = ecs::Mesh::wireCube(Vec3{1.0f, 1.0f, 1.0f}, 0.02f);
    check(wire->validate().empty() && wire->triangleCount() == 12 * 12 && outward(*wire) &&
              std::abs(wire->boundsMax().x - 0.51f) < 1e-4f,
          "wireCube: 12 barras con las caras hacia fuera");

    // Una textura en Assets, repetida 2 x 3 en la submalla 1.
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_mesh_tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "Textures");
    asset::ImageRgba8 img;
    img.width = img.height = 4;
    img.pixels.assign(4 * 4 * 4, 200);
    img.pixels[3] = 0;  // un pixel transparente: recorte por alfa
    asset::saveImagePng(root / "Textures" / "rejilla.png", img);
    ecs::Mesh m = *ecs::Mesh::quad();
    m.setTriangles({0, 1, 2}, 0);
    m.setTriangles({0, 2, 3}, 1);
    m.materials.resize(2);
    m.materials[1].texture = "Textures/rejilla.png";
    m.materials[1].normal_map = "Textures/no_existe.png";
    m.materials[1].tiling = core::Vec2{2.0f, 3.0f};
    asset::ModelData data = m.toModelData(root);
    check(data.textures.size() == 1 && data.materials[1].albedo_texture == 0 && data.materials[1].normal_texture == -1 &&
              data.materials[0].albedo_texture == -1,
          "texturas por submalla desde Assets (las que no existen, sin textura)");
    check(std::abs(data.vertices[3].uv.x - 0.0f) < 1e-6f && std::abs(data.vertices[3].uv.y - 0.0f) < 1e-6f &&
              std::abs(data.vertices[2].uv.x - 2.0f) < 1e-6f,
          "repeticion de la textura en las UV de su submalla");
    asset::finalizeModel(data, "prueba");
    check(data.textures[0].width == 4 && !data.textures[0].pixels.empty(), "se decodifican al subirla");

    // Desde los scripts: solo factores = actualizacion en vivo; texturas = resubir.
    using scripting::api::Value;
    const auto obj = [](std::initializer_list<std::pair<const char*, Value>> fields) {
        Value o = Value::object();
        for (const auto& [key, value] : fields) o.set(key, value);
        return o;
    };
    physics::registerPhysicsComponents();
    ui::registerUiComponents();
    assets::MaterialAsset glow;
    glow.emissive = Vec3{1.0f, 0.5f, 0.1f};
    glow.emissive_intensity = 4.0f;
    std::filesystem::create_directories(root / "Materials");
    assets::saveMaterial(glow, root / "Materials" / "Brillo.crmat");
    ecs::World world;
    scripting::ScriptSystem scripts;
    scripts.setAssetsRoot(root);
    scripts.start(world);
    scripting::api::NativeApi& api = scripts.nativeApi();
    const Value efectos = api.call("Scene.create", {Value("Efectos")});
    api.set("mesh", api.call("Mesh.cube", {Value(1)}), efectos);
    const Value malla = api.get("mesh", efectos);
    const ecs::Entity e = world.findByName("Efectos");
    ecs::Mesh& mesh = *e.get<ecs::MeshRenderer>().mesh;
    api.call("setMaterial",
             {Value(0), obj({{"color", Value(Vec3{1, 0, 0})}, {"emission", Value(Vec3{1, 1, 0})}, {"emissionIntensity", Value(3)}})},
             malla);
    const std::uint64_t v1 = mesh.version(), mv1 = mesh.materialVersion();
    api.call("setMaterial", {Value(0), obj({{"emissionIntensity", Value(5)}})}, malla);  // mismo hueco ya creado
    const std::uint64_t v2 = mesh.version(), mv2 = mesh.materialVersion();
    check(v2 == v1 && mv2 > mv1 && mesh.materials[0].emission_intensity == 5.0f,
          "cambiar el brillo o el color no vuelve a subir la malla (efectos por frame)");
    api.call("setMaterial", {Value(0), obj({{"texture", Value("Textures/rejilla.png")}, {"tiling", Value(Vec3{4, 4, 0})}})}, malla);
    check(mesh.version() > v2 && mesh.materials[0].texture == "Textures/rejilla.png" && mesh.materials[0].tiling.x == 4.0f,
          "cambiar la textura o la repeticion si la vuelve a subir");
    const Value got = api.call("getMaterial", {Value(0)}, malla);
    check(got["texture"].asString() == "Textures/rejilla.png" && got["emissionIntensity"].asNumber() == 5.0,
          "getMaterial devuelve el material");
    // Un .crmat en el hueco 1 del MeshRenderer, y sin sombras.
    const Value found = api.call("Scene.find", {Value("Efectos")});
    api.set("castShadows", Value(false), found);
    const Value set = api.call("Entity:setMaterial", {Value(1), Value("Materials/Brillo")}, found);
    const ecs::MeshRenderer& r = e.get<ecs::MeshRenderer>();
    check(set.truthy() && r.materials.size() == 2 && r.materials[1].uuid == glow.uuid && !r.materials[0].valid() &&
              r.cast_shadows == ecs::ShadowCasting::Off,
          "entity().setMaterial pone un .crmat en un hueco; castShadows");
    // Interfaz: imagen, transparencia, posicion y tamano.
    ecs::Entity icon = world.create("Icono");
    icon.add<ui::RectTransform>();
    icon.add<ui::Image>();
    const Value i = api.call("Scene.find", {Value("Icono")});
    api.set("texture", Value("Voxel/Iconos/stone.png"), i);
    api.set("alpha", Value(0.5), i);
    api.set("uiPosition", Value(Vec3{10, -20, 0}), i);
    api.set("uiSize", Value(Vec3{64, 32, 0}), i);
    check(icon.get<ui::Image>().texture == "Voxel/Iconos/stone.png" && icon.get<ui::Image>().alpha == 0.5f &&
              icon.get<ui::RectTransform>().position.y == -20.0f && icon.get<ui::RectTransform>().size.x == 64.0f,
          "texture, alpha, uiPosition y uiSize de la interfaz");
    // El ejemplo "Varios materiales y efectos" del manual (docs/manual/ejemplo-efectos.html).
    const Value cristal = api.call("Scene.create", {Value("Cristal")});
    const Value cubo = api.call("Mesh.cube", {Value(1)});
    const Value tris = api.get("triangles", cubo);
    Value::Array lados, tapas;
    for (std::size_t t = 0; t < tris.size(); t += 3) {
        Value::Array& destino = (t >= 12 && t < 24) ? tapas : lados;
        for (std::size_t k = 0; k < 3; ++k) destino.push_back(tris[t + k]);
    }
    api.call("setTriangles", {Value(std::move(lados)), Value(0)}, cubo);
    api.call("setTriangles", {Value(std::move(tapas)), Value(1)}, cubo);
    api.call("setMaterial",
             {Value(0), obj({{"texture", Value("Textures/piedra.png")}, {"normalMap", Value("Textures/piedra_n.png")},
                             {"tiling", Value(Vec3{2, 2, 0})}})},
             cubo);
    api.call("setMaterial",
             {Value(1), obj({{"color", Value(Vec3{0.2f, 0.6f, 1})}, {"emission", Value(Vec3{0.2f, 0.6f, 1})},
                             {"emissionIntensity", Value(4)}})},
             cubo);
    api.set("mesh", cubo, cristal);
    const bool doc = api.call("getTriangles", {Value(0)}, cubo).size() == 24 && api.call("getTriangles", {Value(1)}, cubo).size() == 12;
    ecs::Mesh& crystal = *world.findByName("Cristal").get<ecs::MeshRenderer>().mesh;
    const std::uint64_t before_pulse = crystal.version();
    api.call("setMaterial", {Value(1), obj({{"emissionIntensity", Value(3.0 + std::sin(1.3 * 4) * 2)}})}, cubo);
    // Todo lo de la submalla 1 esta en +Y o -Y.
    bool caps = true;
    for (const std::uint32_t k : crystal.triangles(1)) caps = caps && std::abs(std::abs(crystal.vertices[k].y) - 0.5f) < 1e-5f;
    check(doc && crystal.subMeshCount() == 2 && crystal.triangles(0).size() == 24 && crystal.triangles(1).size() == 12 && caps &&
              crystal.version() == before_pulse,
          "el ejemplo de varios materiales y efectos de la documentacion funciona");
    check(scripts.errors().empty(), "sin errores de los scripts");
    scripts.stop();
    std::filesystem::remove_all(root, ec);
}

// Clusteres del importador: el mismo suelo en metros y en centimetros da el
// mismo numero de submallas (antes, en cm, eran celdas de 5 cm: miles).
asset::ModelData gridModel(float unit) {
    constexpr int kSide = 200;  // 200x200 quads = 80000 triangulos
    asset::ModelData m{};
    m.bones.push_back(asset::Bone{"root", 0, core::Mat4::identity()});
    m.materials.push_back(asset::MaterialData{});
    for (int z = 0; z <= kSide; ++z) {
        for (int x = 0; x <= kSide; ++x) {
            asset::SkinnedVertex v{};
            v.position = Vec3{x * 0.5f * unit, 0.0f, z * 0.5f * unit};
            m.vertices.push_back(v);
        }
    }
    asset::SubMesh sub{};
    sub.node = 0;
    for (int z = 0; z < kSide; ++z) {
        for (int x = 0; x < kSide; ++x) {
            const auto i = static_cast<std::uint32_t>(z * (kSide + 1) + x);
            for (const std::uint32_t k : {i, i + kSide + 1, i + 1, i + 1, i + kSide + 1, i + kSide + 2}) {
                m.indices.push_back(k);
            }
        }
    }
    sub.index_count = static_cast<std::uint32_t>(m.indices.size());
    m.submeshes.push_back(sub);
    return m;
}

void testClusters() {
    std::printf("\n[Clusteres]\n");
    asset::ModelData meters = gridModel(1.0f);
    asset::ModelData centimeters = gridModel(100.0f);
    const std::size_t triangles = meters.indices.size() / 3;
    asset::clusterSubmeshes(meters);
    asset::clusterSubmeshes(centimeters);
    std::printf("  metros %zu, centimetros %zu submallas\n", meters.submeshes.size(),
                centimeters.submeshes.size());
    check(meters.submeshes.size() == centimeters.submeshes.size(), "mismos clusteres en m y cm");
    check(centimeters.submeshes.size() > 1 && centimeters.submeshes.size() <= triangles / 1024 + 1,
          "clusteres acotados por triangulos");
    std::size_t indices = 0;
    for (const asset::SubMesh& s : centimeters.submeshes) indices += s.index_count;
    check(indices == centimeters.indices.size(), "no se pierden triangulos");
    check(!asset::isOverClustered(centimeters), "recien agrupado no se reagrupa");

    // Como un .crdata viejo: una submalla por quad.
    asset::ModelData old = gridModel(100.0f);
    old.submeshes.clear();
    for (std::uint32_t first = 0; first < old.indices.size(); first += 6) {
        asset::SubMesh s{};
        s.first_index = first;
        s.index_count = 6;
        s.node = 0;
        old.submeshes.push_back(s);
    }
    check(asset::isOverClustered(old), "detecta un modelo sobre-agrupado");
    asset::clusterSubmeshes(old);
    check(old.submeshes.size() == centimeters.submeshes.size(), "reagrupado como uno nuevo");
}

// El importador informa de su avance (lo lee la barra del editor).
void testImportProgress() {
    std::printf("\n[Progreso de importacion]\n");
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "CramionImportProgressTest";
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    const std::filesystem::path obj = root / "quad.obj";
    std::ofstream(obj, std::ios::binary) << "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3\nf 1 3 4\n";
    assets::ImportProgress progress;
    check(progress.fraction() == 0.0f && progress.stage() == "En cola", "empieza en cola");
    const assets::ImportResult result = assets::importAny(obj, root / "Assets", &progress);
    check(result.ok, "importa el .obj");
    check(progress.fraction() == 1.0f, "termina al 100 %");
    check(progress.stage() == "Terminado", "ultima etapa: Terminado");
    std::filesystem::remove_all(root, ec);
}

// Static batching (exportar): cubos Static en un lote, un espejo sigue con
// las caras hacia fuera, lo que se mueve o no es Static no entra.
void testStaticBatching() {
    std::printf("\n[Static batching]\n");
    physics::registerPhysicsComponents();
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "CramionStaticBatchTest";
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "Assets", ec);
    assets::AssetDatabase database;
    database.open(root / "Assets");

    ecs::World world;
    const auto cube = [&](const char* name, const Vec3& position, const Vec3& scale, bool is_static) {
        ecs::Entity e = world.create(name);
        e.setLocalPosition(position);
        e.setLocalScale(scale);
        e.get<ecs::EntityInfo>().is_static = is_static;
        e.add<ecs::MeshRenderer>().model = assets::AssetRef{assets::builtin::kCube, assets::AssetType::Model};
        return e;
    };
    ecs::Entity a = cube("A", Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, true);
    ecs::Entity b = cube("B", Vec3{5.0f, 0.0f, 0.0f}, Vec3{2.0f, 1.0f, 1.0f}, true);
    ecs::Entity mirror = cube("Espejo", Vec3{0.0f, 0.0f, 5.0f}, Vec3{-1.0f, 1.0f, 1.0f}, true);
    ecs::Entity loose = cube("NoStatic", Vec3{9.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, false);
    ecs::Entity falling = cube("ConRigidbody", Vec3{0.0f, 5.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, true);
    falling.add<physics::Rigidbody>();

    const std::filesystem::path file = root / "Assets" / "batch.crdata";
    ecs::StaticBatchReport report;
    const bool ok = ecs::buildStaticBatch(world, database, file, ecs::StaticBatchOptions{}, report);
    std::printf("  %s\n", report.message.c_str());
    check(ok && std::filesystem::exists(file), "escribe el lote");
    check(report.combined == 3 && report.kept_movable == 1, "combina los 3 Static quietos, no el del Rigidbody");
    check(report.draws_before == 1 && report.draws_after == 1, "mismo material: una llamada");
    check(a.get<ecs::EntityInfo>().static_batched && b.get<ecs::EntityInfo>().static_batched &&
              mirror.get<ecs::EntityInfo>().static_batched,
          "marca los originales");
    check(!loose.get<ecs::EntityInfo>().static_batched && !falling.get<ecs::EntityInfo>().static_batched,
          "no toca los que no entran");
    const ecs::Entity batch = world.findByName("Static Batch");
    check(batch.valid() && batch.has<ecs::MeshRenderer>(), "anade la entidad del lote");

    database.refresh();
    const auto info = database.find(batch.get<ecs::MeshRenderer>().model.uuid);
    const auto model = info ? assets::AssetManager::readModel(info->uuid, info->path, info->name) : nullptr;
    check(model && model->parts.size() == 1, "el lote se lee como un modelo");
    if (model && !model->parts.empty()) {
        const asset::ModelData& part = *model->parts[0];
        check(part.indices.size() / 3 == 3 * 12, "36 triangulos (3 cubos)");
        bool faces = true;
        float max_x = -1e9f;
        for (std::size_t i = 0; i + 2 < part.indices.size(); i += 3) {
            const auto& va = part.vertices[part.indices[i]];
            const auto& vb = part.vertices[part.indices[i + 1]];
            const auto& vc = part.vertices[part.indices[i + 2]];
            const Vec3 face = core::cross(vb.position - va.position, vc.position - va.position);
            if (core::dot(face, va.normal + vb.normal + vc.normal) <= 0.0f) faces = false;
            max_x = std::max({max_x, va.position.x, vb.position.x, vc.position.x});
        }
        check(faces, "caras hacia fuera (tambien el espejo)");
        check(std::abs(max_x - 6.0f) < 1e-4f, "vertices en el mundo (B llega a x = 6)");
    }

    // El marcador sobrevive a guardar y abrir la escena.
    ecs::World copy;
    ecs::deserializeWorld(copy, ecs::serializeWorld(world));
    const ecs::Entity a2 = copy.findByName("A");
    check(a2.valid() && a2.get<ecs::EntityInfo>().is_static && a2.get<ecs::EntityInfo>().static_batched,
          "Static y static_batched se guardan en la escena");

    // Repetida y "grande" (copias x vertices por encima del tope): se queda
    // instanciada, no se copia.
    {
        ecs::World repeated;
        for (int i = 0; i < 2; ++i) {
            ecs::Entity e = repeated.create("Copia");
            e.setLocalPosition(Vec3{static_cast<float>(i) * 3.0f, 0.0f, 0.0f});
            e.get<ecs::EntityInfo>().is_static = true;
            e.add<ecs::MeshRenderer>().model = assets::AssetRef{assets::builtin::kCube, assets::AssetType::Model};
        }
        ecs::StaticBatchOptions tight;
        tight.max_copied_vertices = 40;  // 2 cubos x 24 vertices = 48
        ecs::StaticBatchReport r;
        check(!ecs::buildStaticBatch(repeated, database, root / "Assets" / "rep.crdata", tight, r) &&
                  r.kept_instanced == 2,
              "malla repetida y grande: se queda instanciada");
    }

    ecs::StaticBatchReport again;
    check(!ecs::buildStaticBatch(copy, database, root / "Assets" / "otra.crdata", ecs::StaticBatchOptions{}, again) &&
              !std::filesystem::exists(root / "Assets" / "otra.crdata"),
          "no se combina dos veces");
    std::filesystem::remove_all(root, ec);
}

// Origen flotante: el mundo se desplaza sin que nada cambie de sitio de
// verdad (entidades, escena guardada, cuerpos de la fisica).
void testFloatingOrigin() {
    std::printf("\n[Origen flotante]\n");
    ecs::World world;
    ecs::Entity far_entity = world.create("Lejos");
    far_entity.setLocalPosition(Vec3{5000.0f, 10.0f, -3000.0f});
    ecs::Entity child = world.create("Hijo", far_entity);
    child.setLocalPosition(Vec3{1.0f, 0.0f, 0.0f});

    check(!ecs::updateFloatingOrigin(world, Vec3{100.0f, 0.0f, 0.0f}), "cerca del origen no se desplaza");
    const std::optional<Vec3> offset = ecs::updateFloatingOrigin(world, Vec3{5000.0f, 10.0f, -3000.0f});
    check(offset && offset->x == 5120.0f && offset->y == 0.0f && offset->z == -3072.0f,
          "se desplaza en pasos de 1024 m");
    const Vec3 local = far_entity.localPosition();
    check(local.x == -120.0f && local.y == 10.0f && local.z == 72.0f, "la raiz queda cerca del origen");
    const ecs::DVec3 real = world.absolute(far_entity.worldPosition());
    check(real.x == 5000.0 && real.y == 10.0 && real.z == -3000.0, "misma posicion real (doble precision)");
    check(child.localPosition().x == 1.0f, "los hijos no cambian (son relativos al padre)");

    ecs::World copy;
    ecs::deserializeWorld(copy, ecs::serializeWorld(world));
    const ecs::Entity far_copy = copy.findByName("Lejos");
    check(copy.origin() == world.origin() && far_copy.valid() && far_copy.localPosition().x == -120.0f,
          "la escena guarda el origen y las posiciones relativas");

    // Un cuerpo dinamico que cae: sigue su camino al desplazar el mundo.
    physics::registerPhysicsComponents();
    ecs::World pw;
    ecs::Entity box = pw.create("Caja");
    box.setLocalPosition(Vec3{3000.0f, 50.0f, 0.0f});
    box.add<physics::Rigidbody>();
    box.add<physics::BoxCollider>();
    physics::PhysicsSystem physics;
    physics.start(pw);
    for (int i = 0; i < 30; ++i) physics.update(pw, 1.0f / 60.0f);
    const ecs::DVec3 before = pw.absolute(box.worldPosition());
    const float speed_before = physics.linearVelocity(box).y;
    const std::optional<Vec3> shift = ecs::updateFloatingOrigin(pw, box.worldPosition());
    if (shift) physics.shiftOrigin(*shift);
    physics.update(pw, 1.0f / 60.0f);
    const ecs::DVec3 after = pw.absolute(box.worldPosition());
    check(shift.has_value() && std::abs(box.worldPosition().x) < 1024.0f, "la caja queda cerca del origen");
    check(std::abs(after.x - before.x) < 0.01 && after.y < before.y && before.y - after.y < 0.2,
          "la fisica sigue igual (misma posicion real, sigue cayendo)");
    check(std::abs(physics.linearVelocity(box).y - speed_before) < 0.5f, "conserva la velocidad");
    physics.stop();
}

}  // namespace

// LODs automaticos: un terreno ondulado (malla cerrada) mas cientos de
// briznas sueltas, como un modelo de hierba.
void testLods() {
    std::printf("\nLODs automaticos\n");
    asset::ModelData model{};
    model.name = "lod_test";
    model.bones.resize(1);
    model.materials.resize(2);
    constexpr int kGrid = 160;
    for (int z = 0; z <= kGrid; ++z) {
        for (int x = 0; x <= kGrid; ++x) {
            asset::SkinnedVertex v{};
            v.position = Vec3{static_cast<float>(x) * 0.1f, std::sin(x * 0.07f) * std::cos(z * 0.05f),
                              static_cast<float>(z) * 0.1f};
            v.normal = Vec3{0, 1, 0};
            v.weights[0] = 1.0f;
            model.vertices.push_back(v);
        }
    }
    for (int z = 0; z < kGrid; ++z) {
        for (int x = 0; x < kGrid; ++x) {
            const auto a = static_cast<std::uint32_t>(z * (kGrid + 1) + x);
            const std::uint32_t b = a + 1;
            const auto c = static_cast<std::uint32_t>(a + kGrid + 1);
            const std::uint32_t d = c + 1;
            for (const std::uint32_t i : {a, c, b, b, c, d}) model.indices.push_back(i);
        }
    }
    const auto terrain_indices = static_cast<std::uint32_t>(model.indices.size());
    // Briznas: 2 triangulos cada una, sin compartir vertices.
    for (int blade = 0; blade < 3000; ++blade) {
        const float bx = static_cast<float>(blade % 60) * 0.26f;
        const float bz = static_cast<float>(blade / 60) * 0.31f;
        const auto base = static_cast<std::uint32_t>(model.vertices.size());
        for (int k = 0; k < 4; ++k) {
            asset::SkinnedVertex v{};
            v.position = Vec3{bx + (k & 1 ? 0.02f : 0.0f), 2.0f + (k & 2 ? 0.3f : 0.0f), bz};
            v.normal = Vec3{0, 0, 1};
            v.weights[0] = 1.0f;
            model.vertices.push_back(v);
        }
        for (const std::uint32_t i : {0u, 2u, 1u, 1u, 2u, 3u}) model.indices.push_back(base + i);
    }
    asset::SubMesh terrain{};
    terrain.index_count = terrain_indices;
    asset::SubMesh grass{};
    grass.first_index = terrain_indices;
    grass.index_count = static_cast<std::uint32_t>(model.indices.size()) - terrain_indices;
    grass.material = 1;
    model.submeshes = {terrain, grass};
    asset::clusterSubmeshes(model);

    const std::size_t levels = asset::generateLods(model);
    check(levels >= 3, "genera al menos 3 niveles");
    std::size_t previous = model.indices.size() / 3;
    bool decreasing = true;
    bool in_range = true;
    bool errors_grow = true;
    bool bounds_ok = true;
    bool both_materials = true;
    float previous_error = 0.0f;
    for (const asset::MeshLod& lod : model.lods) {
        std::size_t triangles = 0;
        bool has[2] = {false, false};
        for (const asset::SubMesh& submesh : lod.submeshes) {
            triangles += submesh.index_count / 3;
            if (submesh.material < 2) has[submesh.material] = true;
            if (submesh.first_index + submesh.index_count > model.lod_indices.size()) in_range = false;
            if (submesh.bounds_min.x > submesh.bounds_max.x) bounds_ok = false;
            for (std::uint32_t i = 0; in_range && i < submesh.index_count; ++i) {
                if (model.lod_indices[submesh.first_index + i] >= model.vertices.size()) in_range = false;
            }
        }
        decreasing = decreasing && triangles < previous;
        errors_grow = errors_grow && lod.error >= previous_error;
        both_materials = both_materials && has[0];
        std::printf("    LOD: %zu triangulos, error %.4f\n", triangles, lod.error);
        previous = triangles;
        previous_error = lod.error;
    }
    check(decreasing, "cada nivel tiene menos triangulos que el anterior");
    check(in_range, "los indices de los LODs estan dentro del buffer y de los vertices");
    check(errors_grow, "el error crece con el nivel");
    check(bounds_ok && both_materials, "cada nivel conserva el terreno con sus cajas");
    check(model.lods.empty() || model.lods[0].submeshes.size() >= 2, "el LOD1 sigue partido en clusteres");

    // Las mismas briznas con recorte por alfa (follaje): sin Prune ni sloppy,
    // todas siguen en cada nivel (antes desaparecian enteras).
    {
        asset::ModelData masked = model;
        masked.materials[1].base_color.w = 0.3f;
        asset::generateLods(masked);
        bool kept = !masked.lods.empty();
        for (const asset::MeshLod& lod : masked.lods) {
            std::size_t blades = 0;
            for (const asset::SubMesh& submesh : lod.submeshes) {
                if (submesh.material == 1) blades += submesh.index_count / 3;
            }
            kept = kept && blades == 3000u * 2u;
        }
        check(kept, "con recorte por alfa las briznas siguen enteras en todos los LODs (y el terreno baja)");
    }

    // LODs a mano (_LOD1 del archivo): sus vertices van detras de los de
    // LOD0, su material se empareja por nombre y sustituyen a los automaticos.
    {
        asset::ModelData manual = model;
        asset::ModelData level{};
        level.materials.push_back(manual.materials[0]);
        for (int k = 0; k < 4; ++k) {
            asset::SkinnedVertex v{};
            v.position = Vec3{k & 1 ? 15.0f : 0.0f, 0.0f, k & 2 ? 15.0f : 0.0f};
            v.normal = Vec3{0, 1, 0};
            v.weights[0] = 1.0f;
            level.vertices.push_back(v);
        }
        level.indices = {0, 2, 1, 1, 2, 3};
        asset::SubMesh quad{};
        quad.index_count = 6;
        level.submeshes = {quad};
        const std::size_t base_vertices = manual.vertices.size();
        const std::size_t levels_set = asset::setManualLods(manual, {&level});
        bool ok = levels_set == 1 && manual.lods.size() == 1 && manual.vertices.size() == base_vertices + 4 &&
                  manual.lods[0].error > 0.0f && !manual.lods[0].submeshes.empty();
        for (const asset::SubMesh& submesh : ok ? manual.lods[0].submeshes : std::vector<asset::SubMesh>{}) {
            ok = ok && submesh.material == 0 && submesh.first_index + submesh.index_count <= manual.lod_indices.size();
            for (std::uint32_t i = 0; ok && i < submesh.index_count; ++i) {
                const std::uint32_t v = manual.lod_indices[submesh.first_index + i];
                ok = v >= base_vertices && v < manual.vertices.size();
            }
        }
        check(ok, "los LODs a mano se anaden detras de LOD0 con su material");
    }

    // Calidad de texturas: una imagen de 256 con el limite en 64 se reduce al
    // decodificar (y sin limite queda igual).
    {
        asset::ImageRgba8 image{};
        image.width = image.height = 256;
        image.pixels.assign(256u * 256u * 4u, 0);
        for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
            image.pixels[i] = static_cast<std::uint8_t>((i / 4) % 256);
            image.pixels[i + 3] = 255;
        }
        const std::filesystem::path png = std::filesystem::temp_directory_path() / "cramion_texture_limit.png";
        check(asset::saveImagePng(png, image), "escribe la imagen de prueba");
        const auto decode = [&](std::uint32_t limit) {
            asset::ModelData textured{};
            textured.indices = {0, 1, 2};
            textured.vertices.resize(3);
            asset::TextureData texture{};
            texture.name = "prueba";
            texture.source_path = png.string();
            textured.textures.push_back(texture);
            asset::setMaxTextureSize(limit);
            asset::finalizeModel(textured, "prueba");
            asset::setMaxTextureSize(0);
            return textured.textures[0];
        };
        const asset::TextureData limited = decode(64);
        const asset::TextureData full = decode(0);
        check(limited.width == 64 && limited.height == 64 && limited.pixels.size() == 64u * 64u * 4u,
              "con limite 64 la textura de 256 queda en 64x64");
        check(full.width == 256, "sin limite queda en 256");
        std::filesystem::remove(png);
    }

    // Compresion BC7 con cache (como Unity): la imagen sale en BC7 con todos
    // sus mips, casi igual a la original, opaca; la segunda vez se lee de la
    // cache; y una con recortes se marca con alfa.
    {
        const std::filesystem::path folder = std::filesystem::temp_directory_path() / "cramion_bc7_cache";
        std::filesystem::remove_all(folder);
        asset::ImageRgba8 image{};
        image.width = image.height = 256;
        image.pixels.assign(256u * 256u * 4u, 255);
        for (std::uint32_t y = 0; y < 256; ++y) {
            for (std::uint32_t x = 0; x < 256; ++x) {
                std::uint8_t* p = &image.pixels[(static_cast<std::size_t>(y) * 256 + x) * 4];
                p[0] = static_cast<std::uint8_t>(x);
                p[1] = static_cast<std::uint8_t>(y);
                p[2] = static_cast<std::uint8_t>((x * 7 + y * 13) % 256);
            }
        }
        const std::filesystem::path png = std::filesystem::temp_directory_path() / "cramion_bc7.png";
        check(asset::saveImagePng(png, image), "escribe la imagen para BC7");
        const auto decode = [&] {
            asset::ModelData textured{};
            textured.indices = {0, 1, 2};
            textured.vertices.resize(3);
            asset::TextureData texture{};
            texture.name = "bc7";
            texture.source_path = png.string();
            textured.textures.push_back(texture);
            asset::finalizeModel(textured, "bc7");
            return textured.textures[0];
        };
        asset::setTextureCacheFolder(folder);
        const asset::TextureData first = decode();
        check(first.format == asset::TextureFormat::Bc7 && first.mip_levels == 9, "BC7 con sus 9 mips");
        check(first.alpha == 0, "la imagen opaca no se marca con alfa");
        const std::vector<std::uint8_t> back = asset::decodeBlockCompressed(first);
        double error = 0.0;
        for (std::size_t i = 0; i < back.size() && i < image.pixels.size(); ++i) {
            const double d = static_cast<double>(back[i]) - image.pixels[i];
            error += d * d;
        }
        const double mse = error / static_cast<double>(image.pixels.size());
        const double psnr = mse > 0.0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0;
        std::cout << "  BC7: PSNR " << psnr << " dB, " << first.pixels.size() << " bytes (RGBA8: "
                  << image.pixels.size() << ")\n";
        check(back.size() == image.pixels.size() && psnr > 40.0, "BC7 casi igual a la original (PSNR > 40 dB)");
        std::size_t cached_files = 0;
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            cached_files += entry.path().extension() == ".dds" ? 1 : 0;
        }
        check(cached_files == 1, "queda un .dds en la cache");
        const asset::TextureData second = decode();
        check(second.format == asset::TextureFormat::Bc7 && second.pixels == first.pixels && second.alpha == 0,
              "la segunda vez sale igual de la cache");

        for (std::size_t i = 3; i < image.pixels.size(); i += 4 * 64) image.pixels[i] = 0;
        check(asset::saveImagePng(png, image), "escribe la imagen con recortes");
        check(decode().alpha == 1, "la imagen con recortes se marca con alfa");

        asset::setTextureCacheFolder({});
        check(decode().format == asset::TextureFormat::Rgba8, "sin carpeta de cache no se comprime");
        std::filesystem::remove(png);
        std::filesystem::remove_all(folder);
    }

    // Arrays de capas en BC7 con su cache (arboles y capas del terreno): un
    // degradado pasa (>= 40 dB) y vuelve igual de la cache; el ruido puro no
    // pasa y la cache lo apunta como RGBA8.
    {
        constexpr std::uint32_t kSide = 64;
        const auto chain = [&](bool noise) {
            asset::MipLayers layers(2);
            std::uint32_t seed = 12345;
            for (auto& layer : layers) {
                for (std::uint32_t side = kSide; side >= 1; side /= 2) {
                    std::vector<std::uint8_t> level(static_cast<std::size_t>(side) * side * 4);
                    for (std::uint32_t i = 0; i < side * side; ++i) {
                        for (std::uint32_t c = 0; c < 4; ++c) {
                            seed = seed * 1664525u + 1013904223u;
                            level[i * 4 + c] = noise ? static_cast<std::uint8_t>(seed >> 24)
                                                     : static_cast<std::uint8_t>(c == 3 ? 255 : (i % side) * 255 / side);
                        }
                    }
                    layer.push_back(std::move(level));
                    if (side == 1) break;
                }
            }
            return layers;
        };
        const asset::MipLayers smooth = chain(false);
        const asset::MipLayers noise = chain(true);
        std::atomic<bool> stop{false};
        asset::Bc7ArrayCache cache;
        cache.bc7.assign(2, 0);
        cache.blocks.assign(2, {});
        double psnr[2] = {0.0, 0.0};
        cache.bc7[0] = asset::compressBc7Layers(smooth, kSide, cache.blocks[0], stop, 2, false, psnr[0]) ? 1 : 0;
        cache.bc7[1] = asset::compressBc7Layers(noise, kSide, cache.blocks[1], stop, 2, false, psnr[1]) ? 1 : 0;
        if (!cache.bc7[1]) cache.blocks[1].clear();
        std::printf("    capas en BC7: degradado %.1f dB, ruido %.1f dB\n", psnr[0], psnr[1]);
        check(cache.bc7[0] == 1 && psnr[0] >= asset::kMinUiPsnr, "capas: un degradado pasa a BC7 (>= 40 dB)");
        check(cache.bc7[1] == 0, "capas: el ruido se queda en RGBA8 (pierde nitidez)");
        const std::uint64_t hash = asset::hashMipLayers(noise, asset::hashMipLayers(smooth, kSide));
        check(hash != asset::hashMipLayers(smooth, asset::hashMipLayers(noise, kSide)), "capas: el hash depende del orden");
        const std::filesystem::path file = std::filesystem::temp_directory_path() / "cramion_capas_bc7.bc7";
        check(asset::writeBc7ArrayCache(file, hash, kSide, {&smooth, &noise}, cache), "capas: se escribe la cache");
        asset::Bc7ArrayCache back;
        check(asset::readBc7ArrayCache(file, hash, kSide, {&smooth, &noise}, back) && back.bc7 == cache.bc7 &&
                  back.blocks[0] == cache.blocks[0] && back.blocks[1].empty(),
              "capas: la cache vuelve igual (BC7 y el apunte de RGBA8)");
        check(!asset::readBc7ArrayCache(file, hash + 1, kSide, {&smooth, &noise}, back), "capas: otro hash no vale");
        std::filesystem::remove(file);
    }

    // Un modelo animado (o pequeno) no tiene LODs.
    asset::ModelData small{};
    small.vertices.resize(3);
    small.indices = {0, 1, 2};
    small.submeshes = {asset::SubMesh{0, 3, 0}};
    check(asset::generateLods(small) == 0 && small.lods.empty(), "un modelo pequeno no genera LODs");

    // Orden para la cache de vertices: mismos triangulos en cada submalla,
    // menos vertices transformados por triangulo, y lo transparente igual.
    {
        asset::ModelData grid;
        constexpr std::uint32_t kSide = 64;
        for (std::uint32_t y = 0; y <= kSide; ++y) {
            for (std::uint32_t x = 0; x <= kSide; ++x) {
                asset::SkinnedVertex v{};
                v.position = Vec3{static_cast<float>(x), static_cast<float>(y), 0.0f};
                grid.vertices.push_back(v);
            }
        }
        std::vector<std::array<std::uint32_t, 3>> tris;
        for (std::uint32_t y = 0; y < kSide; ++y) {
            for (std::uint32_t x = 0; x < kSide; ++x) {
                const std::uint32_t a = y * (kSide + 1) + x;
                tris.push_back({a, a + 1, a + kSide + 1});
                tris.push_back({a + 1, a + kSide + 2, a + kSide + 1});
            }
        }
        // Desordenados (como tras los clusteres): un salto fijo por la lista.
        std::vector<std::array<std::uint32_t, 3>> shuffled;
        for (std::size_t i = 0, k = 0; i < tris.size(); ++i, k = (k + 2731) % tris.size()) shuffled.push_back(tris[k]);
        for (const auto& t : shuffled) grid.indices.insert(grid.indices.end(), t.begin(), t.end());
        const std::uint32_t half = static_cast<std::uint32_t>(grid.indices.size() / 2 / 3 * 3);
        grid.materials.resize(2);
        grid.materials[1].transparent = true;
        grid.submeshes = {asset::SubMesh{0, half, 0}, asset::SubMesh{half, static_cast<std::uint32_t>(grid.indices.size()) - half, 1}};
        const std::vector<std::uint32_t> original = grid.indices;
        const asset::VertexCacheStats stats = asset::optimizeVertexCache(grid);
        const auto sorted_tris = [](const std::vector<std::uint32_t>& idx, std::uint32_t first, std::uint32_t count) {
            std::vector<std::array<std::uint32_t, 3>> out;
            for (std::uint32_t i = first; i + 2 < first + count; i += 3) {
                std::array<std::uint32_t, 3> t{idx[i], idx[i + 1], idx[i + 2]};
                std::rotate(t.begin(), std::min_element(t.begin(), t.end()), t.end());  // mismo giro
                out.push_back(t);
            }
            std::sort(out.begin(), out.end());
            return out;
        };
        check(sorted_tris(grid.indices, 0, half) == sorted_tris(original, 0, half),
              "cache de vertices: los mismos triangulos (y con el mismo giro) en la submalla");
        check(std::equal(grid.indices.begin() + half, grid.indices.end(), original.begin() + half),
              "cache de vertices: la submalla transparente no cambia de orden");
        check(stats.acmr_after < stats.acmr_before * 0.7f && stats.acmr_after < 1.0f,
              "cache de vertices: menos vertices transformados por triangulo");
        std::printf("    ACMR de la rejilla desordenada: %.2f -> %.2f\n", stats.acmr_before, stats.acmr_after);
    }

    // Modelos reales (opcional): CRAMION_LOD_FILES="a.crdata;b.crdata".
    if (const char* files = std::getenv("CRAMION_LOD_FILES")) {
        std::string list(files);
        std::size_t start = 0;
        while (start < list.size()) {
            std::size_t end = list.find(';', start);
            if (end == std::string::npos) end = list.size();
            const std::filesystem::path file = list.substr(start, end - start);
            start = end + 1;
            const auto loaded = assets::AssetManager::readModel(Uuid{}, file, file.stem().string(), false);
            check(loaded != nullptr, "carga el modelo real");
        }
    }
}

// Presupuesto adaptativo con una GPU simulada: cada pasada cuesta segun las
// palancas, como en el motor.
void testFrameBudget() {
    std::printf("\nPresupuesto adaptativo\n");
    using gfx::Lever;
    gfx::FrameBudget budget;
    budget.setStartLevels(gfx::HardwareTier::High);
    budget.setTargetFps(60.0f);  // 16.7 ms
    float load = 1.0f;           // lo pesada que es la escena
    const auto simulate = [&](std::vector<gfx::GpuTiming>& passes) {
        const float res = budget.renderScale() * budget.renderScale();
        const float lod = 1.0f / (1.0f + 0.5f * budget.level(Lever::Lod));
        const float shadow = 1.0f / (1.0f + 0.6f * budget.level(Lever::ShadowDetail));
        passes = {
            {"Sombras (cascadas)", 8.0f * load * shadow * (0.6f + 0.4f * lod)},
            {"Geometria + culling", 5.0f * load * lod * (0.5f + 0.5f * res)},
            {"GI", budget.level(Lever::Gi) > 0 ? 0.0f : 3.4f * res},
            {"Reflejos", budget.level(Lever::Reflections) > 0 ? 0.0f : 0.9f * res},
            {"SSAO", budget.level(Lever::Ssao) > 0 ? 0.0f : 0.3f * res},
            {"Iluminacion", 0.6f * res},
            {"Volumetrica", (budget.level(Lever::Volumetric) > 0 ? 0.2f : 0.4f) * res},
        };
        float total = 0.4f;
        for (const gfx::GpuTiming& t : passes) total += t.milliseconds;
        return total;
    };
    std::vector<gfx::GpuTiming> passes;
    float gpu = 0.0f;
    const float dt = 1.0f / 60.0f;
    float reached = -1.0f;
    int changes_late = 0;
    std::string last;
    for (int frame = 0; frame < 60 * 30; ++frame) {
        gpu = simulate(passes);
        budget.update(dt, gpu, passes);
        const float t = frame * dt;
        if (reached < 0.0f && budget.smoothedGpuMs() <= budget.targetMilliseconds()) reached = t;
        if (t > 20.0f && budget.lastAction() != last) ++changes_late;
        last = budget.lastAction();
    }
    std::printf("    GPU %.2f ms (objetivo %.2f), en objetivo a los %.1f s; LOD %u, sombras %u, GI %u, escala %.2f\n", gpu,
                budget.targetMilliseconds(), reached, budget.level(Lever::Lod), budget.level(Lever::ShadowDetail),
                budget.level(Lever::Gi), budget.renderScale());
    check(reached >= 0.0f && reached < 6.0f, "llega al objetivo en menos de 6 s");
    check(gpu <= budget.targetMilliseconds() * 1.02f, "se queda dentro del presupuesto");
    check(changes_late <= 1, "no oscila (casi sin cambios en los ultimos 10 s)");
    check(budget.level(Lever::Lod) + budget.level(Lever::ShadowDetail) > 0 && budget.level(Lever::Gi) == 0,
          "baja primero lo que mas ahorra y menos se nota (sombras/LOD antes que la GI)");
    check(budget.level(Lever::CameraFx) == 0 && budget.level(Lever::Clouds) == 0,
          "una escena sin desenfoques ni nubes nunca baja esas palancas");
    std::printf("    ahorro medido del primer paso de LOD: %.2f ms\n", budget.learnedSaving(Lever::Lod, 0));
    check(budget.learnedSaving(Lever::Lod, 0) > 0.5f, "mide y recuerda lo que ahorro el paso que dio");

    // La escena se aligera: vuelve a subir la calidad sin pasarse.
    load = 0.3f;
    for (int frame = 0; frame < 60 * 40; ++frame) {
        gpu = simulate(passes);
        budget.update(dt, gpu, passes);
    }
    std::printf("    escena ligera: GPU %.2f ms; LOD %u, sombras %u, escala %.2f\n", gpu, budget.level(Lever::Lod),
                budget.level(Lever::ShadowDetail), budget.renderScale());
    check(budget.level(Lever::ShadowDetail) == 0 && budget.level(Lever::Lod) == 0, "con margen recupera la calidad");
    check(gpu <= budget.targetMilliseconds(), "y sigue dentro del presupuesto");

    // El usuario cambia un ajuste: lo bajado vuelve al punto de partida en el
    // acto (antes seguia bajado hasta reiniciar el motor).
    {
        gfx::FrameBudget b;
        b.setTargetFps(60.0f);
        std::vector<gfx::GpuTiming> heavy = {{"GI", 20.0f}, {"SSAO", 4.0f}, {"Reflejos", 4.0f}};
        for (int frame = 0; frame < 60 * 10; ++frame) b.update(dt, 40.0f, heavy);
        int lowered = 0;
        for (std::size_t i = 0; i < gfx::kLeverCount; ++i) lowered += b.level(static_cast<Lever>(i));
        check(lowered > 0, "con la GPU desbordada baja palancas");
        b.reset();
        int after = 0;
        for (std::size_t i = 0; i < gfx::kLeverCount; ++i) after += b.level(static_cast<Lever>(i));
        check(after == 0 && b.renderScale() == 1.0f, "reset devuelve toda la calidad al instante");
        gfx::PostProcessSettings before{};
        before.global_illumination = false;
        gfx::PostProcessSettings now = before;
        check(!gfx::postQualityRaised(before, now), "sin cambios no se reinicia");
        now.global_illumination = true;
        check(gfx::postQualityRaised(before, now), "prender la GI reinicia el presupuesto");
    }

    // GPU de gama baja (3 veces mas lenta): tiene que bajar hondo, sin oscilar.
    {
        gfx::FrameBudget weak;
        weak.setStartLevels(gfx::HardwareTier::Low);
        weak.setTargetFps(60.0f);
        std::vector<gfx::GpuTiming> p;
        float ms = 0.0f;
        float in_target = -1.0f;
        int late_changes = 0;
        std::string previous;
        for (int frame = 0; frame < 60 * 60; ++frame) {
            const float res = weak.renderScale() * weak.renderScale();
            const float lod = 1.0f / (1.0f + 0.5f * weak.level(Lever::Lod));
            const float shadow = 1.0f / (1.0f + 0.6f * weak.level(Lever::ShadowDetail));
            p = {{"Sombras (cascadas)", 24.0f * shadow * (0.6f + 0.4f * lod)},
                 {"Geometria + culling", 15.0f * lod * (0.5f + 0.5f * res)},
                 {"GI", weak.level(Lever::Gi) > 0 ? 0.0f : 10.0f * res},
                 {"Reflejos", weak.level(Lever::Reflections) > 0 ? 0.0f : 2.7f * res},
                 {"SSAO", weak.level(Lever::Ssao) > 0 ? 0.0f : 1.0f * res},
                 {"Iluminacion", 1.8f * res},
                 {"Volumetrica", (weak.level(Lever::Volumetric) > 0 ? 0.6f : 1.2f) * res}};
            ms = 1.2f * res;
            for (const gfx::GpuTiming& t : p) ms += t.milliseconds;
            weak.update(dt, ms, p);
            if (in_target < 0.0f && weak.smoothedGpuMs() <= weak.targetMilliseconds()) in_target = frame * dt;
            if (frame * dt > 45.0f && weak.lastAction() != previous) ++late_changes;
            previous = weak.lastAction();
        }
        std::printf("    gama baja: GPU %.2f ms, en objetivo a los %.1f s; LOD %u, sombras %u, GI %u, escala %.2f\n", ms,
                    in_target, weak.level(Lever::Lod), weak.level(Lever::ShadowDetail), weak.level(Lever::Gi),
                    weak.renderScale());
        check(in_target >= 0.0f && in_target < 15.0f && ms <= weak.targetMilliseconds() * 1.02f,
              "gama baja: llega a 60 FPS en menos de 15 s");
        check(late_changes <= 1, "gama baja: estable al final");
        const gfx::PostProcessSettings low = weak.apply(gfx::PostProcessSettings{});
        check(weak.level(Lever::Volumetric) == 0 ||
                  (low.volumetric_light && low.volumetric_steps <= 16 && low.volumetric_steps >= 8 && !low.light_shafts),
              "gama baja: el polvo se abarata pero no se apaga (no depende de mirar al sol)");
    }

    // GPU sin timestamps (algunas de movil): solo el tiempo del frame, sin
    // tiempos por pasada. Tiene que bajar igual, y no solo la resolucion (la
    // unica palanca cuyo ahorro se podia estimar sin pasadas).
    {
        gfx::FrameBudget blind;
        blind.setTargetFps(30.0f);
        const std::vector<gfx::GpuTiming> none;
        for (int frame = 0; frame < 60 * 10; ++frame) blind.update(dt, 60.0f, none);
        int effects = 0;
        for (std::size_t i = 0; i < gfx::kLeverCount; ++i) {
            if (static_cast<Lever>(i) != Lever::RenderScale) effects += blind.level(static_cast<Lever>(i));
        }
        std::printf("    sin timestamps: %d pasos de efectos, escala %.2f\n", effects, blind.renderScale());
        check(effects > 0 && blind.renderScale() < 1.0f,
              "sin tiempos por pasada (GPU sin timestamps) baja efectos y resolucion");
    }

    // Apagado: nada cambia.
    budget.setEnabled(false);
    gfx::PostProcessSettings user{};
    const gfx::PostProcessSettings applied = budget.apply(user);
    check(applied.global_illumination == user.global_illumination && applied.lod_pixel_error == user.lod_pixel_error &&
              budget.renderScale() == 1.0f,
          "apagado no toca nada");
    check(gfx::tierFor(2048, false) == gfx::HardwareTier::Low && gfx::tierFor(8192, false) == gfx::HardwareTier::High &&
              gfx::tierFor(16384, true) == gfx::HardwareTier::Low && gfx::shadowResolutionFor(gfx::HardwareTier::Low) == 1536,
          "perfiles de hardware y resolucion de sombras");
}

int budgetLevelSum(const gfx::FrameBudget& b) {
    int sum = 0;
    for (std::size_t i = 0; i < gfx::kLeverCount; ++i) sum += b.level(static_cast<gfx::Lever>(i));
    return sum;
}

// Arranque en una GPU lenta (el "media 0" de los logs): el perfilador aun no
// tiene el primer frame (0 ms) y despues todos pasan de 8 ms. Y los tirones.
void testFrameBudgetStartup() {
    std::printf("\nPresupuesto: arranque, tirones y saltos\n");
    using gfx::Lever;
    {
        gfx::FrameBudget b;
        b.setStartLevels(gfx::HardwareTier::Low);
        b.setTargetFps(60.0f);
        const int start_sum = budgetLevelSum(b);
        b.update(1.0f / 60.0f, 0.0f, {});
        const std::vector<gfx::GpuTiming> passes = {{"GI", 10.0f}, {"Iluminacion", 8.0f}, {"Geometria + culling", 12.0f}};
        for (int frame = 0; frame < 15 * 20; ++frame) b.update(1.0f / 15.0f, 70.0f, passes);
        std::printf("    GPU lenta: media %.2f ms, escala %.2f, suma de palancas %d (salida %d)\n", b.smoothedGpuMs(),
                    b.renderScale(), budgetLevelSum(b), start_sum);
        check(b.smoothedGpuMs() > 50.0f, "GPU lenta: la media no se queda en 0 (el 0 del primer frame no cuenta)");
        check(b.level(Lever::RenderScale) >= 3 && budgetLevelSum(b) >= start_sum,
              "GPU lenta: no sube la calidad por encima del arranque Bajo");
    }
    {
        // Un salto que dura es la nueva normalidad, no un tiron.
        gfx::FrameBudget b;
        b.setTargetFps(60.0f);
        for (int frame = 0; frame < 120; ++frame) b.update(1.0f / 60.0f, 3.0f, {});
        for (int frame = 0; frame < 120; ++frame) b.update(1.0f / 60.0f, 30.0f, {});
        std::printf("    salto de 3 a 30 ms: media %.2f ms\n", b.smoothedGpuMs());
        check(b.smoothedGpuMs() > 25.0f, "un salto sostenido se acepta (antes se tiraba para siempre)");
    }
    {
        gfx::FrameBudget b;
        b.setTargetFps(60.0f);
        for (int frame = 0; frame < 120; ++frame) b.update(1.0f / 60.0f, frame == 60 ? 250.0f : 10.0f, {});
        std::printf("    10 ms con un tiron de 250 ms: media %.2f ms\n", b.smoothedGpuMs());
        check(b.smoothedGpuMs() < 12.0f, "un tiron suelto no cuenta");
    }
    {
        gfx::FrameBudget b;
        b.update(1.0f / 60.0f, 0.0f, {});
        b.update(1.0f / 60.0f, std::nanf(""), {});
        b.update(1.0f / 60.0f, 20.0f, {});
        check(std::fabs(b.smoothedGpuMs() - 20.0f) < 1e-3f, "sin medida (0, NaN) no cuenta: siembra el primer dato real");
    }
    {
        // Calentamiento: tras sembrar, medio segundo (y 4 muestras) sin tocar nada.
        gfx::FrameBudget b;
        b.setTargetFps(60.0f);
        const std::vector<gfx::GpuTiming> heavy = {{"GI", 20.0f}, {"SSAO", 4.0f}, {"Reflejos", 4.0f}};
        float first_change = -1.0f;
        for (int frame = 0; frame < 120; ++frame) {
            b.update(1.0f / 60.0f, 40.0f, heavy);
            if (first_change < 0.0f && !b.lastAction().empty()) first_change = static_cast<float>(frame + 1) / 60.0f;
        }
        std::printf("    primer cambio a los %.2f s\n", first_change);
        check(first_change >= 0.5f && first_change < 1.5f, "calentamiento: el primer cambio espera medio segundo");
    }
    {
        // A 4 FPS cada cambio se mide con al menos 6 muestras (antes con 1-2,
        // y la mitad aun eran de antes del cambio).
        gfx::FrameBudget b;
        b.setTargetFps(60.0f);
        const std::vector<gfx::GpuTiming> heavy = {
            {"GI", 100.0f}, {"SSAO", 40.0f}, {"Reflejos", 40.0f}, {"Iluminacion", 60.0f}};
        std::vector<int> changes;
        std::string last;
        for (int frame = 0; frame < 60; ++frame) {
            b.update(0.25f, 250.0f, heavy);
            if (b.lastAction() != last) {
                changes.push_back(frame);
                last = b.lastAction();
            }
        }
        bool spaced = changes.size() >= 2;
        for (std::size_t i = 1; i < changes.size(); ++i) spaced = spaced && changes[i] - changes[i - 1] >= 6;
        std::printf("    a 4 FPS: %zu cambios\n", changes.size());
        check(spaced, "a pocos FPS cada cambio se mide con al menos 6 muestras");
    }
}

// Reinicios que pide la escena (setPostProcess en cada frame con la mezcla de
// volumenes): uno entero cada 10 s como mucho; entre medias, solo la palanca
// de lo que se prendio, para que se vea ya.
void testBudgetResets() {
    std::printf("\nPresupuesto: reinicios por la escena\n");
    using gfx::Lever;
    const gfx::PostProcessSettings before{};
    gfx::PostProcessSettings after = before;
    after.lod_pixel_error = before.lod_pixel_error * 0.8f;
    check(gfx::postLeversRaised(before, after) == 0, "bajar el error de LOD un 20 % no reinicia (antes bastaba un 1 %)");
    after.lod_pixel_error = before.lod_pixel_error * 0.5f;
    check(gfx::postLeversRaised(before, after) == (1u << static_cast<unsigned>(Lever::Lod)),
          "bajarlo a la mitad devuelve la palanca de LOD");
    after = before;
    after.depth_of_field = true;
    check(gfx::postLeversRaised(before, after) == (1u << static_cast<unsigned>(Lever::CameraFx)),
          "prender la profundidad de campo devuelve los desenfoques");

    gfx::FrameBudget b;
    b.setTargetFps(60.0f);
    const std::vector<gfx::GpuTiming> heavy = {{"GI", 20.0f}, {"SSAO", 4.0f}, {"Reflejos", 4.0f}};
    for (int frame = 0; frame < 60 * 12; ++frame) b.update(1.0f / 60.0f, 40.0f, heavy);
    check(b.level(Lever::Gi) == 1 && b.level(Lever::Ssao) == 1, "con la GPU desbordada baja la GI y el SSAO");
    b.qualityRaised(1u << static_cast<unsigned>(Lever::Gi));
    check(budgetLevelSum(b) == 0 && b.lastAction() == "Ajustes cambiados: calidad restablecida",
          "el primer aviso de la escena reinicia entero");
    for (int frame = 0; frame < 60 * 3; ++frame) b.update(1.0f / 60.0f, 40.0f, heavy);
    const bool lowered = b.level(Lever::Gi) == 1 && b.level(Lever::Ssao) == 1;
    b.qualityRaised(1u << static_cast<unsigned>(Lever::Gi));
    check(lowered && b.level(Lever::Gi) == 0 && b.level(Lever::Ssao) == 1,
          "otro aviso antes de 10 s: solo vuelve lo que se prendio");
}

// Palancas nuevas (desenfoques de camara despues del escalado, nubes y la
// volumetrica en tres niveles) y los niveles de salida por gama.
void testFrameBudgetLevers() {
    std::printf("\nPresupuesto: desenfoques, nubes, volumetrica y arranque por gama\n");
    using gfx::Lever;
    const float dt = 1.0f / 60.0f;
    {
        // Caso tipo WhatIs: escena pesada con profundidad de campo y motion
        // blur (6 ms a la resolucion de salida: la escala no los baja).
        gfx::FrameBudget b;
        b.setStartLevels(gfx::HardwareTier::High);
        b.setTargetFps(60.0f);
        static constexpr float kCameraFx[] = {1.0f, 0.5f, 0.0f};
        float first_camera_fx = -1.0f;
        float first_gi = -1.0f;
        float reached = -1.0f;
        float gpu = 0.0f;
        float camera_fx_ms = 0.0f;
        for (int frame = 0; frame < 60 * 30; ++frame) {
            const float res = b.renderScale() * b.renderScale();
            const float lod = 1.0f / (1.0f + 0.5f * b.level(Lever::Lod));
            const float shadow = 1.0f / (1.0f + 0.6f * b.level(Lever::ShadowDetail));
            camera_fx_ms = 6.0f * kCameraFx[std::min<int>(b.level(Lever::CameraFx), 2)];
            const std::vector<gfx::GpuTiming> passes = {
                {"Sombras (cascadas)", 12.0f * shadow * (0.6f + 0.4f * lod)},
                {"Geometria + culling", 7.5f * lod * (0.5f + 0.5f * res)},
                {"GI", b.level(Lever::Gi) > 0 ? 0.0f : 3.4f * res},
                {"Reflejos", b.level(Lever::Reflections) > 0 ? 0.0f : 0.9f * res},
                {"SSAO", b.level(Lever::Ssao) > 0 ? 0.0f : 0.3f * res},
                {"Iluminacion", 0.6f * res},
                {"Motion blur + profundidad de campo", camera_fx_ms},
                {"Bloom", 0.5f},
                {"Composicion", 0.3f}};
            gpu = 0.4f;
            for (const gfx::GpuTiming& t : passes) gpu += t.milliseconds;
            b.update(dt, gpu, passes);
            const float t = static_cast<float>(frame + 1) * dt;
            if (first_camera_fx < 0.0f && b.level(Lever::CameraFx) > 0) first_camera_fx = t;
            if (first_gi < 0.0f && b.level(Lever::Gi) > 0) first_gi = t;
            if (reached < 0.0f && b.smoothedGpuMs() <= b.targetMilliseconds()) reached = t;
        }
        std::printf("    tipo WhatIs: GPU %.2f ms, en objetivo a los %.1f s; desenfoques %u (a los %.1f s), GI %u, escala %.2f\n",
                    gpu, reached, b.level(Lever::CameraFx), first_camera_fx, b.level(Lever::Gi), b.renderScale());
        check(first_camera_fx >= 0.0f && (first_gi < 0.0f || first_camera_fx < first_gi),
              "los desenfoques de camara bajan antes que la GI");
        check(reached >= 0.0f && reached < 10.0f && gpu <= b.targetMilliseconds() * 1.02f,
              "con desenfoques llega al objetivo en menos de 10 s");
    }
    {
        // Nubes y volumetrica pesadas (exterior con muchas luces en una GPU floja).
        gfx::FrameBudget b;
        b.setStartLevels(gfx::HardwareTier::High);
        b.setTargetFps(60.0f);
        static constexpr float kClouds[] = {1.0f, 0.5f, 0.2f};
        static constexpr float kVolumetric[] = {1.0f, 0.55f, 0.3f, 0.1f};
        float clouds_ms = 0.0f;
        float volumetric_ms = 0.0f;
        float gpu = 0.0f;
        std::uint8_t max_clouds = 0;
        std::uint8_t max_volumetric = 0;
        for (int frame = 0; frame < 60 * 30; ++frame) {
            const float res = b.renderScale() * b.renderScale();
            clouds_ms = 10.0f * kClouds[std::min<int>(b.level(Lever::Clouds), 2)] * res;
            volumetric_ms = 8.0f * kVolumetric[std::min<int>(b.level(Lever::Volumetric), 3)] * res;
            const std::vector<gfx::GpuTiming> passes = {
                {"Sombras (cascadas)", 2.0f},
                {"Geometria + culling", 2.0f * (0.5f + 0.5f * res)},
                {"Nubes", clouds_ms},
                {"Volumetrica", volumetric_ms},
                {"Rayos de luz", b.level(Lever::Volumetric) > 0 ? 0.0f : 0.5f},
                {"Iluminacion", 1.0f * res},
                {"GI", b.level(Lever::Gi) > 0 ? 0.0f : 2.0f * res}};
            gpu = 0.4f;
            for (const gfx::GpuTiming& t : passes) gpu += t.milliseconds;
            b.update(dt, gpu, passes);
            max_clouds = std::max(max_clouds, b.level(Lever::Clouds));
            max_volumetric = std::max(max_volumetric, b.level(Lever::Volumetric));
        }
        std::printf("    nubes y volumetrica: GPU %.2f ms; nubes %u (%.2f ms), volumetrica %u (%.2f ms), escala %.2f\n", gpu,
                    b.level(Lever::Clouds), clouds_ms, b.level(Lever::Volumetric), volumetric_ms, b.renderScale());
        check(max_clouds > 0 && max_volumetric > 0, "baja las nubes y la volumetrica");
        check(clouds_ms < 6.0f && volumetric_ms < 4.8f && gpu <= b.targetMilliseconds() * 1.02f,
              "y sus pasadas bajan hasta entrar en el presupuesto");
    }
    {
        // Lo que aplica cada nivel.
        gfx::FrameBudget b;
        gfx::PostProcessSettings user{};
        user.depth_of_field = true;
        user.motion_blur = true;
        const gfx::PostProcessSettings full = b.apply(user);
        check(full.dof_samples == 64 && full.motion_blur_samples == 24 && full.cloud_steps == 128 &&
                  full.cloud_shadow_interval == 1 && full.volumetric_steps == 32 && full.volumetric_local_lights,
              "a tope, la calidad de siempre");
        b.setStartLevel(Lever::CameraFx, 1);
        gfx::PostProcessSettings s = b.apply(user);
        check(s.depth_of_field && s.motion_blur && s.dof_samples == 32 && s.motion_blur_samples == 12,
              "desenfoque de camara 1: 32 y 12 muestras");
        b.setStartLevel(Lever::CameraFx, 2);
        s = b.apply(user);
        check(!s.depth_of_field && !s.motion_blur, "desenfoque de camara 2: sin profundidad de campo ni motion blur");
        b.setStartLevel(Lever::Volumetric, 2);
        s = b.apply(user);
        check(s.volumetric_light && s.volumetric_steps == 8 && s.volumetric_local_lights && !s.light_shafts,
              "volumetrica 2: 8 tramos");
        b.setStartLevel(Lever::Volumetric, 3);
        s = b.apply(user);
        check(s.volumetric_light && s.volumetric_steps == 8 && !s.volumetric_local_lights,
              "volumetrica 3: sin luces locales (solo el sol)");
        b.setStartLevel(Lever::Clouds, 1);
        s = b.apply(user);
        check(s.cloud_steps == 64 && s.cloud_shadow_interval == 1, "nubes 1: 64 pasos");
        b.setStartLevel(Lever::Clouds, 2);
        s = b.apply(user);
        check(s.cloud_steps == 32 && s.cloud_shadow_interval == 4, "nubes 2: 32 pasos y su sombra cada 4 frames");
        check(b.maxLevel(Lever::CameraFx) == 2 && b.maxLevel(Lever::Clouds) == 2 && b.maxLevel(Lever::Volumetric) == 3,
              "niveles de las palancas nuevas");
    }
    {
        // Arranque por gama: Bajo empieza sin lo mas caro (SSAO, reflejos,
        // desenfoques, nubes a 64 pasos); Medio, sin desenfoques completos.
        gfx::FrameBudget low;
        low.setStartLevels(gfx::HardwareTier::Low);
        check(low.level(Lever::Lod) == 0 && low.level(Lever::ShadowDetail) == 2 && low.level(Lever::Volumetric) == 1 &&
                  low.level(Lever::ContactShadows) == 1 && low.level(Lever::Lighting) == 1 && low.level(Lever::Ssao) == 1 &&
                  low.level(Lever::Reflections) == 1 && low.level(Lever::Gi) == 0 &&
                  low.level(Lever::RenderScale) == 3 && low.level(Lever::CameraFx) == 2 && low.level(Lever::Clouds) == 1,
              "arranque Bajo: SSAO, reflejos, desenfoques y nubes incluidos");
        gfx::FrameBudget medium;
        medium.setStartLevels(gfx::HardwareTier::Medium);
        check(medium.level(Lever::Lod) == 0 && medium.level(Lever::ShadowDetail) == 1 &&
                  medium.level(Lever::Volumetric) == 1 && medium.level(Lever::CameraFx) == 1 &&
                  budgetLevelSum(medium) == 3,
              "arranque Medio: sombras, volumetrica y desenfoques");
        // F2-8: el LOD y el alcance de las sombras van por gama (no por la
        // palanca: no se cuentan dos veces).
        check(gfx::lodScaleFor(gfx::HardwareTier::Low) == 2.0f && gfx::lodScaleFor(gfx::HardwareTier::Medium) == 1.5f &&
                  gfx::lodScaleFor(gfx::HardwareTier::High) == 1.0f &&
                  gfx::shadowDistanceFor(gfx::HardwareTier::Low) == 60.0f &&
                  gfx::shadowDistanceFor(gfx::HardwareTier::Medium) == 80.0f &&
                  gfx::shadowDistanceFor(gfx::HardwareTier::Ultra) == 100.0f,
              "sesgo de LOD y alcance de sombras por gama");
        gfx::FrameBudget high;
        high.setStartLevels(gfx::HardwareTier::High);
        check(budgetLevelSum(high) == 0, "arranque Alto: a tope");
    }
}

// Resolucion interna: sin doble reduccion (preset que ya escala), con suelo,
// y un paso cada 3 s como mucho.
void testRenderScaleFloor() {
    std::printf("\nResolucion interna: suelo y sin doble reduccion\n");
    using gfx::Lever;
    check(gfx::renderScaleStartLevel(gfx::HardwareTier::Low, 1.0f / 1.5f) == 0 &&
              gfx::renderScaleStartLevel(gfx::HardwareTier::Low, 1.0f) == 3 &&
              gfx::renderScaleStartLevel(gfx::HardwareTier::High, 1.0f) == 0,
          "con un preset que ya escala, la resolucion no empieza bajada");
    check(std::fabs(gfx::renderScaleFloor(1009, 1.0f) - 540.0f / 1009.0f) < 1e-4f &&
              std::fabs(gfx::renderScaleFloor(2160, 1.0f) - 0.5f) < 1e-4f && gfx::renderScaleFloor(400, 1.0f) == 1.0f &&
              gfx::renderScaleFloor(1080, 0.5f) == 1.0f,
          "suelo: max(540 lineas, la mitad de la salida), nunca por debajo de lo que ya pide el preset");
    {
        // Bajo con FSR 1 Calidad (0.67), como Keryan: antes 0.67 x 0.67 = 0.45 (862x453).
        const float preset = 1.0f / 1.5f;
        gfx::FrameBudget b;
        b.setStartLevels(gfx::HardwareTier::Low);
        b.setStartLevel(Lever::RenderScale, gfx::renderScaleStartLevel(gfx::HardwareTier::Low, preset));
        b.setRenderScaleFloor(gfx::renderScaleFloor(1009, preset));
        const float scale = preset * b.renderScale();
        std::printf("    Bajo + preset 0.67: escala %.3f (%ldx%ld de 1920x1009)\n", scale, std::lround(1920.0f * scale),
                    std::lround(1009.0f * scale));
        check(scale >= 0.58f && std::lround(1009.0f * scale) == 673, "Bajo con un preset de 0.67 arranca a 0.58 o mas");
    }
    {
        const float floor = gfx::renderScaleFloor(1009, 1.0f);
        gfx::FrameBudget b;
        b.setStartLevels(gfx::HardwareTier::High);
        b.setTargetFps(60.0f);
        b.setRenderScaleFloor(floor);
        const std::vector<gfx::GpuTiming> passes = {{"Geometria + culling", 19.0f}};
        float last_change = -100.0f;
        float min_gap = 100.0f;
        std::uint8_t level = b.level(Lever::RenderScale);
        // Por encima del objetivo (sin llegar a 1.5 veces) todo el rato.
        for (int frame = 0; frame < 60 * 30; ++frame) {
            b.update(1.0f / 60.0f, 20.0f, passes);
            if (b.level(Lever::RenderScale) != level) {
                const float t = static_cast<float>(frame + 1) / 60.0f;
                min_gap = std::min(min_gap, t - last_change);
                last_change = t;
                level = b.level(Lever::RenderScale);
            }
        }
        std::printf("    por encima: escala %.3f (suelo %.3f), pasos separados %.1f s como poco\n", b.renderScale(),
                    floor, min_gap);
        check(b.renderScale() >= floor - 1e-4f && !b.renderScaleFloorLifted(),
              "por encima del objetivo (sin llegar a 1.5 veces) no baja del suelo");
        check(min_gap >= 2.99f, "un paso de resolucion cada 3 s como mucho");
        // Una GPU que no llega ni de lejos durante mas de 3 s: baja del suelo.
        for (int frame = 0; frame < 60 * 15; ++frame) b.update(1.0f / 60.0f, 55.0f, passes);
        std::printf("    muy por encima: escala %.3f\n", b.renderScale());
        check(b.renderScaleFloorLifted() && b.renderScale() < floor, "muy por encima durante 3 s: baja del suelo");
        // Con margen vuelve a subir y el suelo vuelve a valer.
        for (int frame = 0; frame < 60 * 40; ++frame) b.update(1.0f / 60.0f, 4.0f, passes);
        std::printf("    con margen: escala %.3f\n", b.renderScale());
        check(!b.renderScaleFloorLifted() && b.renderScale() >= floor - 1e-4f,
              "con margen la resolucion sube y el suelo vuelve");
    }
}

// Objetos diminutos: umbral por gama y con histeresis (sin parpadeo).
void testTinyObjects() {
    std::printf("\nObjetos diminutos por gama\n");
    check(gfx::tinyObjectPixelsFor(gfx::HardwareTier::Low) == 1.5f &&
              gfx::tinyObjectPixelsFor(gfx::HardwareTier::Medium) == 1.0f &&
              gfx::tinyObjectPixelsFor(gfx::HardwareTier::High) == 0.75f &&
              gfx::tinyObjectPixelsFor(gfx::HardwareTier::Ultra) == 0.5f,
          "umbral por gama: 1.5 / 1 / 0.75 / 0.5 px");
    const float t = gfx::tinyObjectPixelsFor(gfx::HardwareTier::Low);
    bool tiny = false;
    tiny = gfx::tinyObject(1.6f, t, tiny);
    const bool a = !tiny;
    tiny = gfx::tinyObject(1.4f, t, tiny);
    const bool b = tiny;
    tiny = gfx::tinyObject(1.7f, t, tiny);
    const bool c = tiny;
    tiny = gfx::tinyObject(1.9f, t, tiny);
    const bool d = !tiny;
    check(a && b && c && d, "histeresis: se oculta por debajo de t y vuelve por encima de 1.25 t");
}

// Juego exportado: los ajustes del visor del editor (Graphics.ini del
// proyecto) adaptados al PC del jugador, salvo con la casilla de bloqueo.
void testShippedGraphics() {
    std::printf("\nJuego exportado: ajustes del proyecto adaptados a cada PC\n");
    gfx::GraphicsSettings project{};
    project.adaptive = false;
    project.target_fps = 240.0f;
    project.shadow_resolution = 6144;
    project.texture_max_size = 16384;
    project.texture_filtering = 16;
    project.texture_lod_bias = -1.0f;
    const gfx::GraphicsSettings low = gfx::shippedGraphics(project, gfx::HardwareTier::Low, 60.0f, false);
    std::printf("    Bajo a 60 Hz: adaptativo %d, objetivo %.0f, sombras %d, texturas %d, filtrado %d, sesgo %.1f\n",
                low.adaptive ? 1 : 0, low.target_fps, low.shadow_resolution, low.texture_max_size, low.texture_filtering,
                low.texture_lod_bias);
    check(low.adaptive && low.target_fps == 60.0f && low.shadow_resolution == 1536 && low.texture_max_size == 2048 &&
              low.texture_filtering == 4 && low.texture_lod_bias == 0.0f,
          "{0, 240, 6144, 16384, 16, -1} en Bajo a 60 Hz da {1, 60, 1536, 2048, 4, 0}");
    const gfx::GraphicsSettings locked = gfx::shippedGraphics(project, gfx::HardwareTier::Low, 60.0f, true);
    check(locked == project && !locked.adaptive, "con el bloqueo, tal cual (adaptive sigue en 0)");
    const gfx::GraphicsSettings fast = gfx::shippedGraphics(project, gfx::HardwareTier::Ultra, 240.0f, false);
    check(fast.target_fps == 144.0f && fast.shadow_resolution == 4096 && fast.texture_max_size == 8192 &&
              fast.texture_lod_bias == -1.0f,
          "Ultra a 240 Hz: objetivo 144 como mucho; el sesgo negativo se queda");
    gfx::GraphicsSettings automatic{};  // 0 = segun el PC
    automatic.target_fps = 30.0f;
    const gfx::GraphicsSettings keep = gfx::shippedGraphics(automatic, gfx::HardwareTier::Medium, 0.0f, false);
    check(keep.target_fps == 30.0f && keep.shadow_resolution == 0 && keep.texture_max_size == 0 &&
              keep.texture_filtering == 0,
          "lo automatico (0) y un objetivo de 30 se quedan; Hz desconocidos no cambian nada");
    gfx::GraphicsSettings small = project;
    small.shadow_resolution = 1024;
    small.texture_filtering = 1;
    small.target_fps = 200.0f;
    const gfx::GraphicsSettings capped = gfx::shippedGraphics(small, gfx::HardwareTier::Medium, 0.0f, false);
    check(capped.shadow_resolution == 1024 && capped.texture_filtering == 1 && capped.target_fps == 60.0f,
          "lo que ya es menor que la gama se respeta; sin Hz conocidos, 60 como mucho");
    check(gfx::shippedGraphics(automatic, gfx::HardwareTier::High, 24.0f, false).target_fps == 30.0f,
          "nunca por debajo de 30 FPS");
}

// Marcas de la GPU: el compute del principio y los mapas fijos no bajan con
// la resolucion; el mapa del cielo cuenta en la iluminacion ligera.
void testPassMarks() {
    std::printf("\nPresupuesto: marcas de la GPU\n");
    using gfx::Lever;
    const auto first_lowered = [](const std::vector<gfx::GpuTiming>& passes, float gpu) {
        gfx::FrameBudget b;
        b.setStartLevels(gfx::HardwareTier::High);
        b.setTargetFps(60.0f);
        for (int frame = 0; frame < 120 && b.lastAction().empty(); ++frame) b.update(1.0f / 60.0f, gpu, passes);
        for (std::size_t i = 0; i < gfx::kLeverCount; ++i) {
            if (b.level(static_cast<Lever>(i)) > 0) return static_cast<int>(i);
        }
        return -1;
    };
    // 22 ms de compute (terreno, oceano, liquidos) y 8 de iluminacion: bajar
    // la resolucion apenas ahorra (el compute no depende de ella).
    const int compute = first_lowered({{"Preparacion (compute)", 22.0f}, {"Iluminacion", 8.0f}}, 30.0f);
    std::printf("    con compute: primero %s\n", compute >= 0 ? gfx::FrameBudget::leverName(static_cast<Lever>(compute)) : "nada");
    check(compute == static_cast<int>(Lever::Lighting),
          "el compute del principio no cuenta como coste de la resolucion (baja antes la iluminacion ligera)");
    // Sin la marca (todo era de la resolucion) habria bajado la resolucion.
    const int unnamed = first_lowered({{"Algo", 22.0f}, {"Iluminacion", 8.0f}}, 30.0f);
    check(unnamed == static_cast<int>(Lever::RenderScale), "(sin marca, el mismo coste se cobraba a la resolucion)");
    // La oclusion del cielo la quita la iluminacion ligera.
    const int sky = first_lowered({{"Oclusion del cielo", 6.0f}, {"Iluminacion", 2.0f}, {"Geometria + culling", 6.0f}},
                                  22.0f);
    check(sky == static_cast<int>(Lever::Lighting), "el mapa del cielo desde arriba cuenta en la iluminacion ligera");
}

// Render Textures a un ritmo segun la gama, escalonadas por camara.
void testRenderTextureInterval() {
    std::printf("\nRender Textures por gama\n");
    check(gfx::renderTextureIntervalFor(gfx::HardwareTier::Low) == 3 &&
              gfx::renderTextureIntervalFor(gfx::HardwareTier::Medium) == 2 &&
              gfx::renderTextureIntervalFor(gfx::HardwareTier::High) == 1 &&
              gfx::renderTextureIntervalFor(gfx::HardwareTier::Ultra) == 1,
          "cada 3 frames en Bajo, 2 en Medio, todos en Alto y Ultra");
    // Dos portales (como PortalLab) en Bajo: cada uno un frame de cada 3 y
    // nunca los dos en el mismo frame.
    int drawn[2] = {0, 0};
    int both = 0;
    int views = 0;
    for (std::uint64_t frame = 0; frame < 300; ++frame) {
        const bool a = gfx::renderTextureDue(frame, 0, 3);
        const bool b = gfx::renderTextureDue(frame, 1, 3);
        drawn[0] += a ? 1 : 0;
        drawn[1] += b ? 1 : 0;
        both += a && b ? 1 : 0;
        views += 1 + (a ? 1 : 0) + (b ? 1 : 0);
    }
    std::printf("    dos camaras en Bajo: %d y %d de 300 frames, %.2f vistas por frame\n", drawn[0], drawn[1],
                views / 300.0);
    check(drawn[0] == 100 && drawn[1] == 100 && both == 0, "escalonadas: cada una 1 de cada 3 frames, nunca a la vez");
    bool every = true;
    for (std::uint64_t frame = 0; frame < 10; ++frame) every = every && gfx::renderTextureDue(frame, 1, 1);
    check(every, "con intervalo 1 se dibujan todos los frames (como antes)");
}

// VR: la resolucion de cada ojo no se hunde (presupuesto con tope, tope de
// VRAM segun la memoria, sesgo de mips al escalar).
void testVrResolution() {
    std::printf("\nResolucion en VR\n");
    using gfx::Lever;
    const float dt = 1.0f / 90.0f;
    // GPU de gama baja muy por encima de los 90 Hz del casco: sin tope bajaba
    // la resolucion al 50 %; con el tope de VR se queda en 0.87 y baja lo demas.
    gfx::FrameBudget vr;
    vr.setStartLevels(gfx::HardwareTier::Low);  // empezaba en 67 %
    vr.setLevelCap(Lever::RenderScale, 1);
    vr.setTargetFps(90.0f);
    check(vr.renderScale() >= 0.869f, "VR: la gama baja ya no empieza al 67 % por ojo");
    const std::vector<gfx::GpuTiming> heavy = {{"GI", 10.0f}, {"SSAO", 3.0f}, {"Reflejos", 3.0f},
                                               {"Sombras (cascadas)", 8.0f}, {"Geometria + culling", 6.0f}};
    for (int frame = 0; frame < 90 * 20; ++frame) vr.update(dt, 40.0f, heavy);
    std::printf("    escala %.2f, GI %u, SSAO %u, reflejos %u\n", vr.renderScale(), vr.level(Lever::Gi),
                vr.level(Lever::Ssao), vr.level(Lever::Reflections));
    check(vr.renderScale() >= 0.869f && vr.maxLevel(Lever::RenderScale) == 1,
          "VR: el presupuesto no baja la resolucion por debajo de 0.87");
    check(vr.level(Lever::Gi) > 0 && vr.level(Lever::Ssao) > 0, "VR: baja antes los efectos");
    vr.reset();
    check(vr.renderScale() >= 0.869f, "VR: reset respeta el tope");
    vr.setLevelCap(Lever::RenderScale, 255);
    check(vr.maxLevel(Lever::RenderScale) == 5 && vr.level(Lever::RenderScale) == 3,
          "al salir de VR vuelve al tope normal y al punto de partida");

    // Tope por VRAM (ojo de Quest 3, 2528x2704).
    const float cap8 = gfx::vrVramScaleCap(8192, 2528, 2704);
    const float cap4 = gfx::vrVramScaleCap(4096, 2528, 2704);
    std::printf("    tope VRAM: 4 GB %.3f, 8 GB %.3f\n", cap4, cap8);
    check(cap8 > 0.85f && cap8 < 0.9f, "VR: con 8 GB el ojo de Quest 3 va a ~0.87 (antes 0.67)");
    check(cap4 >= 0.67f && cap4 < 0.7f, "VR: con poca VRAM nunca menos que antes (1.5x 1080p)");
    check(gfx::vrVramScaleCap(12288, 2528, 2704) == 1.0f && gfx::vrVramScaleCap(0, 2528, 2704) == 1.0f &&
              gfx::vrVramScaleCap(8192, 1832, 1920) == 1.0f,
          "VR: sin tope con 12 GB, VRAM desconocida o un ojo que ya cabe");
    // Escritorio: ventana maximizada en un monitor de 5760x3240.
    const float desk8 = gfx::desktopVramScaleCap(8192, 5559, 3144);
    check(desk8 > 0.67f && desk8 < 0.71f, "escritorio: con 8 GB, 5559x3144 se dibuja a ~4K (0.69)");
    check(gfx::desktopVramScaleCap(8192, 3840, 2160) == 1.0f && gfx::desktopVramScaleCap(4096, 2560, 1440) == 1.0f &&
              gfx::desktopVramScaleCap(24576, 7680, 4320) == 1.0f && gfx::desktopVramScaleCap(0, 7680, 4320) == 1.0f,
          "escritorio: sin tope a 4K con 8 GB, 1440p con 4 GB, desde 24 GB o con VRAM desconocida");

    // Sesgo de mips al escalar.
    check(gfx::upscaleMipBias(1.0f, gfx::Upscaler::Taa) == 0.0f && gfx::upscaleMipBias(0.5f, gfx::Upscaler::Off) == 0.0f,
          "sin escalar no hay sesgo");
    check(std::fabs(gfx::upscaleMipBias(0.5f, gfx::Upscaler::Taa) + 1.0f) < 1e-4f &&
              std::fabs(gfx::upscaleMipBias(0.5f, gfx::Upscaler::Fsr1) + 0.5f) < 1e-4f &&
              gfx::upscaleMipBias(0.25f, gfx::Upscaler::Dlss) == -1.0f,
          "sesgo log2(interna/salida), la mitad con FSR 1, como mucho -1");
}

int main() {
    testFrameBudget();
    testFrameBudgetStartup();
    testBudgetResets();
    testFrameBudgetLevers();
    testRenderScaleFloor();
    testTinyObjects();
    testShippedGraphics();
    testPassMarks();
    testRenderTextureInterval();
    testVrResolution();
    testLods();
    testPrimitives();
    testEditing();
    testApiAndPhysics();
    testMaterials();
    testClusters();
    testImportProgress();
    testStaticBatching();
    testFloatingOrigin();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
