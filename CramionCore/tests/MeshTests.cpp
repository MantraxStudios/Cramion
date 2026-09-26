// Pruebas de las mallas creadas por codigo (consola, sin GPU): primitivas con
// las caras hacia fuera, normales y tangentes, validacion, conversion al
// formato del renderizador, la API de Lua (Mesh, entity.mesh,
// addComponent) y el MeshCollider con Jolt (tambien al cambiar la malla).
// Devuelve 0 si todo va.

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
#include "CramionCore/scripting/Scripting.h"
#include "CramionCore/asset/Importer.h"
#include "CramionCore/asset/MaterialAsset.h"
#include "CramionCore/ui/UI.h"

#include <CramionFX/asset/ImageFile.h>
#include <CramionFX/asset/Model.h>

#include <cmath>
#include <cstdio>
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

void testLuaAndPhysics() {
    std::printf("Lua y MeshCollider\n");
    physics::registerPhysicsComponents();
    ecs::World world;
    physics::PhysicsSystem physics;
    physics.start(world);
    scripting::ScriptSystem scripts;
    scripts.setPhysics(&physics);
    scripts.start(world);
    std::string out;
    const bool ok = scripts.run(R"(
        local suelo = Scene.create("Suelo", Vec3(0, 0, 0))
        local m = Mesh.new("Rampa")
        m.vertices = { Vec3(-5, 0, -5), Vec3(5, 0, -5), Vec3(5, 0, 5), Vec3(-5, 0, 5) }
        m.uv = { Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(1, 1, 0), Vec3(0, 1, 0) }
        m.triangles = { 0, 2, 1, 0, 3, 2 }
        m:recalculateNormals()
        m:setMaterial(0, { color = Vec3(0.2, 0.8, 0.3), roughness = 0.4 })
        suelo.mesh = m
        suelo:addComponent("MeshCollider")
        local e = Scene.create("Bola", Vec3(0, 3, 0))
        e.mesh = Mesh.sphere(0.5)
        e:addComponent("SphereCollider")
        e:addComponent("Rigidbody")
        return tostring(m) .. " n=" .. tostring(m.normals[1]) .. " tri=" .. #m.triangles
    )", &out);
    std::printf("  (%s)\n", out.c_str());
    check(ok && out.find("4 vertices, 2 triangulos") != std::string::npos && out.find("1.000") != std::string::npos,
          "Mesh.new, vertices, uv, triangles y recalculateNormals desde Lua");
    const ecs::Entity floor = world.findByName("Suelo");
    const ecs::Entity ball = world.findByName("Bola");
    check(floor.valid() && floor.has<ecs::MeshRenderer>() && floor.get<ecs::MeshRenderer>().mesh &&
              floor.get<ecs::MeshRenderer>().mesh->materials.size() == 1 && floor.has<physics::MeshCollider>(),
          "entity.mesh anade el MeshRenderer y addComponent el MeshCollider");
    check(ball.valid() && ball.get<ecs::MeshRenderer>().mesh && ball.get<ecs::MeshRenderer>().mesh->vertices.size() > 100,
          "Mesh.sphere desde Lua");
    // La bola cae sobre la malla y se para en ella.
    ball.get<physics::Rigidbody>().interpolate = false;
    for (int i = 0; i < 180; ++i) physics.update(world, 1.0f / 60.0f);
    const float rest = ball.worldPosition().y;
    std::printf("  (la bola reposa en y = %.2f)\n", static_cast<double>(rest));
    check(std::abs(rest - 0.5f) < 0.08f, "un Rigidbody choca con la malla creada por codigo");
    // Bajar la malla 2 m desde Lua: el MeshCollider se rehace y la bola cae hasta ella.
    const bool moved = scripts.run(R"(
        local m = Scene.find("Suelo").mesh
        local v = m.vertices
        for i = 1, #v do v[i] = v[i] - Vec3(0, 2, 0) end
        m.vertices = v
    )", &out);
    for (int i = 0; i < 120; ++i) physics.update(world, 1.0f / 60.0f);
    std::printf("  (tras bajar la malla, y = %.2f)\n", static_cast<double>(ball.worldPosition().y));
    check(moved && std::abs(ball.worldPosition().y + 1.5f) < 0.15f, "al cambiar la malla se rehace su MeshCollider");
    // Errores claros.
    const bool bad = scripts.run(R"(
        local m = Mesh.new()
        m.vertices = { Vec3(0, 0, 0) }
        m.triangles = { 0, 1, 2 }
        return m:validate()
    )", &out);
    check(bad && out.find("vertice 1") != std::string::npos, "mesh:validate() explica que falla");
    // El ejemplo "Terreno procedural" del manual (docs/manual/ejemplo-terreno.html), tal cual.
    const bool example = scripts.run(R"(
        local self = { entity = Scene.create("Terreno"), tamano = 60.0, altura = 4.0 }
        self.malla = Mesh.plane(self.tamano, self.tamano, 80, 80)
        local v = self.malla.vertices
        for i = 1, #v do
            local p = v[i]
            p.y = (math.sin(p.x * 0.15) + math.cos(p.z * 0.12)) * self.altura * 0.5
            v[i] = p
        end
        self.malla.vertices = v
        self.malla:recalculateNormals()
        self.malla:recalculateTangents()
        self.malla:setMaterial(0, { color = Vec3(0.35, 0.6, 0.25), roughness = 0.9 })
        self.entity.mesh = self.malla
        self.entity:addComponent("MeshCollider")
        return self.malla.vertexCount .. " " .. self.malla.triangleCount .. " " .. self.malla:validate()
    )", &out);
    std::printf("  (terreno de la doc: %s)\n", out.c_str());
    check(example && out == "6561 12800 " && world.findByName("Terreno").has<physics::MeshCollider>(),
          "el ejemplo de terreno procedural de la documentacion funciona");
    check(scripts.errors().empty(), "sin errores de Lua");
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

    // Lua: solo factores = actualizacion en vivo; texturas = resubir.
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
    std::string out;
    scripts.run(R"(
        local e = Scene.create("Efectos")
        e.mesh = Mesh.cube(1)
        MALLA = e.mesh
        V0, M0 = 0, 0
    )", &out);
    const ecs::Entity e = world.findByName("Efectos");
    ecs::Mesh& mesh = *e.get<ecs::MeshRenderer>().mesh;
    const std::uint64_t v0 = mesh.version(), mv0 = mesh.materialVersion();
    scripts.run("MALLA:setMaterial(0, { color = Vec3(1, 0, 0), emission = Vec3(1, 1, 0), emissionIntensity = 3 })", &out);
    const std::uint64_t v1 = mesh.version(), mv1 = mesh.materialVersion();
    scripts.run("MALLA:setMaterial(0, { emissionIntensity = 5 })", &out);  // mismo hueco ya creado
    const std::uint64_t v2 = mesh.version(), mv2 = mesh.materialVersion();
    check(v2 == v1 && mv2 > mv1 && mesh.materials[0].emission_intensity == 5.0f,
          "cambiar el brillo o el color no vuelve a subir la malla (efectos por frame)");
    scripts.run("MALLA:setMaterial(0, { texture = 'Textures/rejilla.png', tiling = Vec3(4, 4, 0) })", &out);
    check(mesh.version() > v2 && mesh.materials[0].texture == "Textures/rejilla.png" && mesh.materials[0].tiling.x == 4.0f,
          "cambiar la textura o la repeticion si la vuelve a subir");
    (void)v0;
    (void)mv0;
    const bool got = scripts.run("local t = MALLA:getMaterial(0); return t.texture .. ' ' .. tostring(t.emissionIntensity)", &out);
    check(got && out == "Textures/rejilla.png 5.0", "getMaterial devuelve el material");
    // Un .crmat en el hueco 1 del MeshRenderer, y sin sombras.
    const bool set = scripts.run("local e = Scene.find('Efectos'); e.castShadows = false; return e:setMaterial(1, 'Materials/Brillo')", &out);
    const ecs::MeshRenderer& r = e.get<ecs::MeshRenderer>();
    check(set && out == "true" && r.materials.size() == 2 && r.materials[1].uuid == glow.uuid && !r.materials[0].valid() &&
              r.cast_shadows == ecs::ShadowCasting::Off,
          "entity:setMaterial pone un .crmat en un hueco; castShadows");
    // Interfaz desde Lua: imagen, transparencia, posicion y tamano.
    ecs::Entity icon = world.create("Icono");
    icon.add<ui::RectTransform>();
    icon.add<ui::Image>();
    scripts.run(R"(
        local i = Scene.find("Icono")
        i.texture = "Voxel/Iconos/stone.png"
        i.alpha = 0.5
        i.uiPosition = Vec3(10, -20, 0)
        i.uiSize = Vec3(64, 32, 0)
    )", &out);
    check(icon.get<ui::Image>().texture == "Voxel/Iconos/stone.png" && icon.get<ui::Image>().alpha == 0.5f &&
              icon.get<ui::RectTransform>().position.y == -20.0f && icon.get<ui::RectTransform>().size.x == 64.0f,
          "texture, alpha, uiPosition y uiSize de la interfaz");
    // El ejemplo "Varios materiales y efectos" del manual (docs/manual/ejemplo-efectos.html).
    const bool doc = scripts.run(R"(
        local self = { entity = Scene.create("Cristal") }
        self.malla = Mesh.cube(1)
        local tris = self.malla.triangles
        local lados, tapas = {}, {}
        for i = 1, #tris, 3 do
            local destino = (i > 12 and i <= 24) and tapas or lados
            table.insert(destino, tris[i]); table.insert(destino, tris[i + 1]); table.insert(destino, tris[i + 2])
        end
        self.malla:setTriangles(lados, 0)
        self.malla:setTriangles(tapas, 1)
        self.malla:setMaterial(0, { texture = "Textures/piedra.png", normalMap = "Textures/piedra_n.png", tiling = Vec3(2, 2, 0) })
        self.malla:setMaterial(1, { color = Vec3(0.2, 0.6, 1), emission = Vec3(0.2, 0.6, 1), emissionIntensity = 4 })
        self.entity.mesh = self.malla
        CRISTAL = self.malla
        return #self.malla:getTriangles(0) .. " " .. #self.malla:getTriangles(1)
    )", &out);
    ecs::Mesh& crystal = *world.findByName("Cristal").get<ecs::MeshRenderer>().mesh;
    const std::uint64_t before_pulse = crystal.version();
    scripts.run("CRISTAL:setMaterial(1, { emissionIntensity = 3 + math.sin(1.3 * 4) * 2 })", &out);
    // Todo lo de la submalla 1 esta en +Y o -Y.
    bool caps = true;
    for (const std::uint32_t i : crystal.triangles(1)) caps = caps && std::abs(std::abs(crystal.vertices[i].y) - 0.5f) < 1e-5f;
    check(doc && crystal.subMeshCount() == 2 && crystal.triangles(0).size() == 24 && crystal.triangles(1).size() == 12 && caps &&
              crystal.version() == before_pulse,
          "el ejemplo de varios materiales y efectos de la documentacion funciona");
    check(scripts.errors().empty(), "sin errores de Lua");
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

    // Un modelo animado (o pequeno) no tiene LODs.
    asset::ModelData small{};
    small.vertices.resize(3);
    small.indices = {0, 1, 2};
    small.submeshes = {asset::SubMesh{0, 3, 0}};
    check(asset::generateLods(small) == 0 && small.lods.empty(), "un modelo pequeno no genera LODs");

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

int main() {
    testLods();
    testPrimitives();
    testEditing();
    testLuaAndPhysics();
    testMaterials();
    testClusters();
    testImportProgress();
    testStaticBatching();
    testFloatingOrigin();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
