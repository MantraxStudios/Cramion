// Pruebas de los sistemas de la 2.1 (consola, sin GPU): VFX Graph (Spawn y
// JSON), fisica 2D y tilemaps, localizacion, dialogos, partidas guardadas,
// Behavior Trees, Visual Scripting (compilar), Shader Graph (generar),
// fractura, horneado de sondas y repeticiones. Devuelve 0 si todo va.

#include "CramionCore/ai/BehaviorTree.h"
#include "CramionCore/asset/AssetDatabase.h"
#include "CramionCore/asset/AssetManager.h"
#include "CramionCore/asset/ShaderGraph.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/gameplay/Dialogue.h"
#include "CramionCore/gameplay/Localization.h"
#include "CramionCore/gameplay/SaveGame.h"
#include "CramionCore/lighting/ProbeBaker.h"
#include "CramionCore/physics/Fracture.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/replay/Replay.h"
#include "CramionCore/scripting/VisualScript.h"
#include "CramionCore/terrain/Terrain.h"
#include "CramionCore/twod/System2D.h"
#include "CramionCore/vfx/VisualEffect.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>

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

void testVfx() {
    std::printf("VFX Graph\n");
    vfx::VfxGraph g;
    g.loop = true;
    g.duration = 2.0f;
    g.spawn.push_back(vfx::makeBlock(vfx::BlockKind::ConstantRate));
    g.spawn.back().values[0] = 10.0f;
    vfx::Block burst = vfx::makeBlock(vfx::BlockKind::Burst);
    burst.values[0] = 50.0f;
    burst.values[1] = 0.5f;
    g.spawn.push_back(burst);
    vfx::SpawnState state;
    std::mt19937 rng(1);
    std::uint32_t total = 0;
    for (int i = 0; i < 60; ++i) total += vfx::advanceSpawn(g, {}, state, 1.0f / 60.0f, 0.0f, true, rng);
    check(total >= 59 && total <= 61, "1 s: 10 por caudal + 50 de la rafaga");
    // Otro ciclo: la rafaga vuelve.
    for (int i = 0; i < 120; ++i) total += vfx::advanceSpawn(g, {}, state, 1.0f / 60.0f, 0.0f, true, rng);
    std::printf("  (3 s: %u particulas)\n", total);
    check(total >= 128 && total <= 132, "3 s: 30 por caudal + 2 rafagas (el bucle la repite)");
    // Parametro atado.
    g.params.push_back(vfx::ExposedParam{"Rate", vfx::ParamType::Float, core::Vec4{100.0f, 0, 0, 0}, {}});
    g.spawn[0].bindings[0] = "Rate";
    vfx::ParamValues params;
    params["Rate"] = core::Vec4{100.0f, 0, 0, 0};
    check(std::fabs(vfx::blockValue(g.spawn[0], 0, params) - 100.0f) < 1e-4f, "campo atado a un parametro");
    // JSON de todas las plantillas.
    bool all = true;
    for (int i = 0; i < vfx::vfxPresetCount(); ++i) {
        const vfx::VfxGraph preset = vfx::vfxPreset(i);
        vfx::VfxGraph back;
        if (!vfx::vfxGraphFromText(vfx::vfxGraphToText(preset), back) || back.initialize.size() != preset.initialize.size() ||
            back.spawn.size() != preset.spawn.size() || back.update.size() != preset.update.size() ||
            back.output_blocks.size() != preset.output_blocks.size()) {
            std::printf("  (plantilla %d: spawn %zu/%zu init %zu/%zu update %zu/%zu output %zu/%zu)\n", i, back.spawn.size(),
                        preset.spawn.size(), back.initialize.size(), preset.initialize.size(), back.update.size(),
                        preset.update.size(), back.output_blocks.size(), preset.output_blocks.size());
            all = false;
        }
    }
    check(all, "las plantillas se guardan y se leen igual");
    gfx::VfxInstanceDesc desc;
    vfx::compileGraph(vfx::vfxPreset(1), {}, desc);
    check(!desc.initialize.empty() && !desc.update.empty() && desc.color_over_life, "el fuego compila a bloques de la GPU");
}

void testPhysics2D() {
    std::printf("Fisica 2D\n");
    twod::register2DComponents();
    ecs::World world;
    ecs::Entity ground = world.create("Suelo");
    ground.add<twod::BoxCollider2D>().size = core::Vec2{20.0f, 1.0f};
    ecs::Entity box = world.create("Caja");
    box.setWorldPosition(Vec3{0.0f, 5.0f, 0.0f});
    box.add<twod::BoxCollider2D>();
    box.add<twod::Rigidbody2D>();
    twod::System2D system;
    system.start(world);
    for (int i = 0; i < 240; ++i) system.update(world, 1.0f / 60.0f, twod::System2D::Mode::Play);
    const float y = box.worldPosition().y;
    check(y > 0.8f && y < 1.2f, "la caja cae y reposa sobre el suelo");
    twod::RaycastHit2D hit;
    const bool hit_ground = system.physics().raycast(core::Vec2{3.0f, 5.0f}, core::Vec2{0.0f, -1.0f}, 20.0f, hit);
    check(hit_ground && hit.entity == ground && std::fabs(hit.point.y - 0.5f) < 0.05f, "raycast 2D contra el suelo");
    bool entered = false;
    for (const twod::Physics2DEvent& e : system.takeEvents()) entered = entered || e.type == twod::Physics2DEventType::CollisionEnter;
    check(entered, "evento OnCollisionEnter2D");
    system.stop();

    // Tilemap: celdas, rectangulos solidos y Rule Tiles.
    twod::Tilemap map;
    for (int x = 0; x < 5; ++x) map.setTile(x, 0, 1);
    for (int x = 0; x < 5; ++x) map.setTile(x, 1, 1);
    check(map.getTile(3, 1) == 1 && map.getTile(9, 9) == 0, "poner y leer celdas");
    const std::vector<twod::TileRect> rects = twod::solidRects(map, nullptr);
    check(rects.size() == 1 && rects[0].w == 5 && rects[0].h == 2, "dos filas iguales: un rectangulo");
    twod::Tileset tileset;
    tileset.rule_tiles.push_back(twod::makeTerrainRuleTile("Terreno", 0, 3));
    twod::Tilemap terrain;
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) terrain.setTile(x, y, -1);
    }
    const int center = twod::resolveRuleTile(tileset, terrain.layers[0].data, 1, 1, 0);
    const int corner = twod::resolveRuleTile(tileset, terrain.layers[0].data, 0, 2, 0);
    check(center != corner, "la Rule Tile elige otro dibujo en la esquina");
}

void testLocalization() {
    std::printf("Localizacion\n");
    gameplay::Localization loc;
    loc.addLanguage("es", "Espanol");
    loc.addLanguage("en", "English");
    std::string error;
    const bool ok = loc.fromCsv("key,es,en\nhola,Hola {0},Hello {0}\nmonedas#one,{n} moneda,{n} coin\nmonedas#other,{n} monedas,{n} coins\n",
                                true, &error);
    check(ok, "leer un CSV");
    loc.setLanguage("es");
    check(loc.get("hola", {"Ana"}) == "Hola Ana", "argumentos");
    check(loc.plural("monedas", 1) == "1 moneda" && loc.plural("monedas", 3) == "3 monedas", "plurales");
    loc.setLanguage("en");
    check(loc.get("hola", {"Ana"}) == "Hello Ana", "cambiar de idioma");
    check(loc.get("no.existe") == "no.existe", "clave que falta: la propia clave");
}

void testDialogue() {
    std::printf("Dialogos\n");
    gameplay::DialogueAsset d;
    d.name = "Prueba";
    const int start = d.add(gameplay::DialogueNodeType::Start).id;
    const int line = d.add(gameplay::DialogueNodeType::Line).id;
    const int choice = d.add(gameplay::DialogueNodeType::Choice).id;
    const int set = d.add(gameplay::DialogueNodeType::SetVariable).id;
    const int end = d.add(gameplay::DialogueNodeType::End).id;
    d.find(start)->next = line;
    d.find(line)->speaker = "Mercader";
    d.find(line)->text = "Tienes {$oro} monedas";
    d.find(line)->next = choice;
    d.find(choice)->options = {gameplay::DialogueOption{"Comprar", {}, {}, true, set},
                               gameplay::DialogueOption{"Adios", {}, {}, true, end}};
    d.find(set)->variable = "oro";
    d.find(set)->op = "-=";
    d.find(set)->value = "3";
    d.find(set)->next = end;
    gameplay::DialogueSystem system;
    system.setVariable("oro", 10);
    check(system.start(d), "empieza");
    check(system.hasLine() && system.line().text == "Tienes 10 monedas", "linea con variable");
    // Una linea seguida de Opciones las ensena a la vez (con la linea encima).
    check(system.waitingChoice() && system.choices().size() == 2, "opciones junto a la linea");
    check(system.choose(0), "elegir la primera");
    std::printf("  (oro = %s, activo = %d)\n", system.variable("oro").dump().c_str(), system.active() ? 1 : 0);
    check(!system.active() && system.variable("oro").get<double>() == 7.0, "la opcion cambia la variable y termina");
}

void testSave() {
    std::printf("Partidas\n");
    gameplay::registerSaveComponents();
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "cramion_save_test";
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    ecs::World world;
    ecs::Entity e = world.create("Cofre");
    e.add<gameplay::Saveable>();
    e.setWorldPosition(Vec3{1.0f, 2.0f, 3.0f});
    gameplay::SaveSystem saves;
    saves.setFolder(folder);
    saves.beginScene(world, "Test", "Test.crscene");
    saves.values()["oro"] = 42;
    std::string error;
    check(saves.save(world, "slot1", "Prueba", &error), "guardar");
    saves.flush();
    e.setWorldPosition(Vec3{9.0f, 9.0f, 9.0f});
    saves.values()["oro"] = 0;
    nlohmann::json snapshot;
    check(saves.read("slot1", snapshot, &error), "leer la ranura");
    check(saves.apply(world, snapshot, &error), "aplicar");
    check(std::fabs(e.worldPosition().x - 1.0f) < 1e-3f && saves.values().value("oro", 0) == 42, "posicion y valores de vuelta");
    check(saves.list().size() == 1, "listar ranuras");
    std::filesystem::remove_all(folder, ec);
}

void testBehaviorTree() {
    std::printf("Behavior Trees\n");
    ai::BehaviorTreeAsset tree;
    tree.ensureRoot();
    ai::Variable v;
    v.name = "listo";
    v.value.type = ai::VarType::Bool;
    tree.blackboard.push_back(v);
    const int sequence = tree.addNode(ai::BtNodeKind::Sequence, 0);
    const int set = tree.addNode(ai::BtNodeKind::SetBlackboard, sequence);
    tree.nodes[static_cast<std::size_t>(set)].params.key = "listo";
    tree.nodes[static_cast<std::size_t>(set)].params.value = "true";
    ai::BtRuntime rt;
    ai::resetBehaviorTree(tree, rt);
    ai::BtContext ctx;
    ctx.dt = 0.016f;
    const ai::BtStatus status = ai::tickBehaviorTree(tree, rt, ctx);
    const ai::Value* listo = rt.find("listo");
    check(listo != nullptr && listo->b, "Set Blackboard escribe la clave");
    check(status == ai::BtStatus::Success || status == ai::BtStatus::Running, "la secuencia termina bien");
    check(ai::behaviorTreeFromJson(ai::behaviorTreeToJson(ai::exampleGuardBehaviorTree()), tree), "el guardia de ejemplo se lee");
}

void testGraphs() {
    std::printf("Visual Scripting y Shader Graph\n");
    const vscript::CompileResult compiled = vscript::compileGraph(vscript::exampleGraph(), "Ejemplo.crgraph");
    check(compiled.ok && compiled.errors.empty(), "el grafo de ejemplo es valido");
    namespace sg = assets::shadergraph;
    sg::GenerateResult result;
    const sg::Graph graph = sg::makeDefault();
    check(sg::generate(graph, "Prueba", result) && result.errors.empty(), "el Shader Graph genera el .crshader");
    check(result.code.find("property") != std::string::npos, "con la propiedad del tinte");
    sg::Graph back;
    check(sg::parse(sg::serialize(graph), back) && back.nodes.size() == graph.nodes.size(), "el grafo se guarda y se lee");
}

void testFracture() {
    std::printf("Fractura\n");
    physics::FractureSettings settings;
    settings.pieces = 12;
    physics::FractureData data;
    std::string error;
    const bool ok = physics::fractureMesh(physics::boxSource(Vec3{2.0f, 1.0f, 1.0f}), settings, data, &error);
    check(ok && data.roots().size() >= 6, "la caja se parte en trozos");
    float volume = 0.0f;
    for (const int r : data.roots()) volume += data.pieces[static_cast<std::size_t>(r)].volume;
    check(std::fabs(volume - 2.0f) < 0.2f, "los trozos suman el volumen de la caja");
    physics::FractureData back;
    check(physics::fractureFromText(physics::fractureToText(data), back) && back.pieces.size() == data.pieces.size(), "el .crfracture se lee");
}

void testBake() {
    std::printf("Iluminacion horneada\n");
    lighting::BakeScene scene;
    // Suelo blanco de 20 x 20 m y un sol desde arriba.
    // Antihorarios vistos desde arriba (la cara de fuera mira a +Y).
    const lighting::BakeTriangle a{Vec3{-10, 0, -10}, Vec3{10, 0, 10}, Vec3{10, 0, -10}, Vec3{0.8f, 0.8f, 0.8f}, {}};
    const lighting::BakeTriangle b{Vec3{-10, 0, -10}, Vec3{-10, 0, 10}, Vec3{10, 0, 10}, Vec3{0.8f, 0.8f, 0.8f}, {}};
    scene.triangles = {a, b};
    scene.lights.sun.direction = Vec3{0.0f, -1.0f, 0.0f};
    scene.lights.sun.color = Vec3{1.0f, 1.0f, 1.0f};
    scene.lights.sun.intensity = 3.0f;
    lighting::BakeVolume volume;
    volume.min = Vec3{-2.0f, 0.5f, -2.0f};
    volume.size = Vec3{4.0f, 2.0f, 4.0f};
    volume.nx = volume.ny = volume.nz = 2;
    scene.volumes = {volume};
    scene.settings.rays = 128;
    scene.settings.bounces = 1;
    scene.settings.threads = 2;
    gfx::BakedLighting out;
    lighting::BakeStats stats;
    std::string error;
    check(lighting::bakeProbes(scene, out, nullptr, &stats, &error) && stats.probes == 8, "8 sondas horneadas");
    const core::Vec4* probe = out.probes.data();
    const Vec3 down = lighting::probeIrradiance(probe, Vec3{0.0f, -1.0f, 0.0f});
    const Vec3 up = lighting::probeIrradiance(probe, Vec3{0.0f, 1.0f, 0.0f});
    std::printf("  (abajo %.3f, arriba %.3f, cielo %.2f, invalidas %zu)\n", down.x, up.x,
                lighting::probeSkyVisibility(probe, Vec3{0.0f, 1.0f, 0.0f}), stats.invalid_probes);
    check(down.x > 0.1f && down.x > up.x, "el suelo al sol rebota luz hacia arriba");
    check(lighting::probeSkyVisibility(probe, Vec3{0.0f, 1.0f, 0.0f}) > 0.8f, "arriba se ve el cielo");
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "cramion_bake_test.crbake";
    gfx::BakedLighting back;
    gfx::LightingMode mode = gfx::LightingMode::Realtime;
    check(lighting::saveBakedLighting(file, out, gfx::LightingMode::Baked) && lighting::loadBakedLighting(file, back, mode) &&
              back.probes.size() == out.probes.size() && mode == gfx::LightingMode::Baked,
          "el .crbake se guarda y se lee");
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

// Que entra en el horneado: Static propio y heredado (hijo de una instancia
// de prefab o de la raiz de un modelo), Rigidbody, desactivados y terreno.
void testBakeGather() {
    std::printf("Horneado: que entra\n");
    assets::AssetDatabase database;
    assets::AssetManager manager(database);
    const auto cube = [](ecs::World& world, const char* name, ecs::Entity parent = {}) {
        ecs::Entity e = world.create(name, parent);
        e.add<ecs::MeshRenderer>().mesh = ecs::Mesh::cube();
        return e;
    };
    {
        ecs::World world;
        ecs::Entity a = cube(world, "Estatico");
        a.get<ecs::EntityInfo>().is_static = true;
        cube(world, "Suelto");
        lighting::BakeScene scene;
        lighting::BakeGatherReport report;
        lighting::gatherBakeScene(world, manager, scene, {}, {}, &report);
        check(report.meshes_baked == 1 && scene.triangles.size() == 12, "un cubo Static: solo el (12 triangulos)");
        check(report.skipped[static_cast<int>(lighting::BakeSkip::NotStatic)] == 1, "el otro se omite por no ser Static");
    }
    {
        // La raiz de una instancia de prefab marcada Static (sin malla) y su
        // hijo con malla (sin marcar): el hijo hereda el Static.
        ecs::World world;
        ecs::Entity root = world.create("Prefab");
        root.get<ecs::EntityInfo>().is_static = true;
        ecs::Entity child = cube(world, "Hijo", root);
        child.setLocalPosition(Vec3{0.0f, 2.0f, 0.0f});
        cube(world, "Otro");
        lighting::BakeScene scene;
        lighting::BakeGatherReport report;
        lighting::gatherBakeScene(world, manager, scene, {}, {}, &report);
        check(lighting::staticInHierarchy(child), "el hijo de un Static es Static");
        check(report.meshes_baked == 1 && report.static_marked == 1 && scene.triangles.size() == 12,
              "el hijo de la instancia Static se hornea");
        check(!scene.triangles.empty() && scene.triangles.front().a.y > 1.0f, "en su sitio del mundo");
    }
    {
        // Nada Static: todo lo que no se mueve. Un Rigidbody Dynamic se omite;
        // Static con Rigidbody Dynamic entra (con aviso).
        ecs::World world;
        cube(world, "Quieto");
        cube(world, "Cae").add<physics::Rigidbody>();
        lighting::BakeScene scene;
        lighting::BakeGatherReport report;
        lighting::gatherBakeScene(world, manager, scene, {}, {}, &report);
        check(report.used_all_non_moving && report.meshes_baked == 1 &&
                  report.skipped[static_cast<int>(lighting::BakeSkip::DynamicBody)] == 1,
              "sin Static: entra lo que no se mueve, no el Rigidbody Dynamic");
        ecs::World world2;
        ecs::Entity s = cube(world2, "Static con fisica");
        s.get<ecs::EntityInfo>().is_static = true;
        s.add<physics::Rigidbody>();
        lighting::BakeScene scene2;
        lighting::BakeGatherReport report2;
        lighting::gatherBakeScene(world2, manager, scene2, {}, {}, &report2);
        check(report2.meshes_baked == 1 && report2.static_with_dynamic_rb == 1, "Static manda sobre el Rigidbody Dynamic");
    }
    {
        // Un Static desactivado no deja la escena vacia sin decir por que.
        ecs::World world;
        ecs::Entity a = cube(world, "Apagado");
        a.get<ecs::EntityInfo>().is_static = true;
        a.setActive(false);
        lighting::BakeScene scene;
        lighting::BakeGatherReport report;
        lighting::gatherBakeScene(world, manager, scene, {}, {}, &report);
        const std::string summary = report.summary();
        std::printf("  (%s)\n", summary.c_str());
        check(scene.triangles.empty() && summary.find("Nada que hornear") != std::string::npos &&
                  summary.find("desactivadas") != std::string::npos,
              "sin nada que hornear, el informe dice el motivo");
    }
    {
        // Terreno Static desde su mapa de alturas (sin collider ni fisica).
        ecs::World world;
        ecs::Entity t = world.create("Terreno");
        t.get<ecs::EntityInfo>().is_static = true;
        terrain::Terrain& comp = t.add<terrain::Terrain>();
        comp.size = 16.0f;
        comp.height = 4.0f;
        comp.collision = false;
        auto data = std::make_shared<terrain::TerrainData>();
        data->create(17, 16, 0.5f);
        lighting::BakeScene scene;
        lighting::BakeGatherReport report;
        lighting::gatherBakeScene(world, manager, scene, {},
                                  [&](ecs::Entity) -> std::shared_ptr<const terrain::TerrainData> { return data; }, &report);
        check(report.terrains == 1 && report.terrain_triangles == 16u * 16u * 2u && scene.triangles.size() == 512,
              "el terreno entra (16 x 16 celdas)");
        check(!scene.triangles.empty() && std::fabs(scene.triangles.front().a.y - 2.0f) < 1e-3f, "a su altura (0.5 x 4 m)");
    }
    {
        // Luces: la Horneada lleva su directa; la de solo tiempo real no entra.
        ecs::World world;
        ecs::Entity baked = world.create("Lampara");
        ecs::Light& l = baked.add<ecs::Light>();
        l.bake_mode = ecs::LightBakeMode::Baked;
        ecs::Entity realtime = world.create("Linterna");
        realtime.add<ecs::Light>().bake_mode = ecs::LightBakeMode::Realtime;
        world.create("Mixta").add<ecs::Light>();
        scene::LightSet sun;
        lighting::BakeScene scene;
        lighting::BakeGatherReport report;
        lighting::gatherBakeLights(world, sun, scene, ecs::LightBakeMode::Mixed, &report);
        int baked_count = 0;
        for (const std::uint8_t b : scene.point_baked) baked_count += b;
        check(scene.lights.points.size() == 2 && baked_count == 1 && scene.baked_light_ids.size() == 1 &&
                  scene.baked_light_ids[0][0] == baked.uuid().high && scene.baked_light_ids[0][1] == baked.uuid().low,
              "luces: 1 horneada y 1 mixta en el horneado, la de tiempo real fuera");
        check(!scene.sun_baked && report.lights_realtime == 1, "el sol mixto no se hornea entero");
    }
}

// Lee un texel del lightmap de superficie (la misma tabla que baked_gi.frag)
// y su irradiancia hacia `n`. -1 si no esta.
float surfaceIrradiance(const gfx::SurfaceLightmap& s, const Vec3& p, const Vec3& n) {
    if (s.capacity == 0) return -1.0f;
    const float inv = 1.0f / s.cell_size;
    const std::uint64_t key = gfx::surfaceCellKey(static_cast<int>(std::floor(p.x * inv)), static_cast<int>(std::floor(p.y * inv)),
                                                  static_cast<int>(std::floor(p.z * inv)));
    const std::uint32_t lo = static_cast<std::uint32_t>(key & 0xFFFFFFFFull);
    const std::uint32_t hi = static_cast<std::uint32_t>(key >> 32);
    std::uint32_t slot = gfx::surfaceCellHash(lo, hi) & (s.capacity - 1);
    for (std::uint32_t i = 0; i < s.capacity; ++i) {
        const std::uint32_t klo = s.keys[static_cast<std::size_t>(slot) * 2];
        const std::uint32_t khi = s.keys[static_cast<std::size_t>(slot) * 2 + 1];
        if (klo == gfx::kSurfaceEmptyKey && khi == gfx::kSurfaceEmptyKey) return -1.0f;
        if (klo == lo && khi == hi) {
            const core::Vec4* t = &s.texels[static_cast<std::size_t>(slot) * gfx::kSurfaceTexelVec4];
            // floats 0..11 = c0.rgb c1.rgb c2.rgb c3.rgb (rojo: 0, 3, 6, 9)
            const float c0 = t[0].x, c1 = t[0].w, c2 = t[1].z, c3 = t[2].y;
            return c0 * 0.282095f + c1 * 0.488603f * n.y + c2 * 0.488603f * n.z + c3 * 0.488603f * n.x;
        }
        slot = (slot + 1) & (s.capacity - 1);
    }
    return -1.0f;
}

// Un horneado diminuto con el sol Horneado: el lightmap lleva su luz directa
// y la sombra de un tejado.
void testBakedDirect() {
    std::printf("Luz directa y sombras horneadas\n");
    lighting::BakeScene scene;
    const Vec3 albedo{0.5f, 0.5f, 0.5f};
    // Suelo de 8 x 8 m (mira a +Y) y un tejado de 2 x 8 m a 1 m sobre x = -2.
    scene.triangles.push_back({Vec3{-4, 0, -4}, Vec3{4, 0, 4}, Vec3{4, 0, -4}, albedo, {}});
    scene.triangles.push_back({Vec3{-4, 0, -4}, Vec3{-4, 0, 4}, Vec3{4, 0, 4}, albedo, {}});
    scene.triangles.push_back({Vec3{-3, 1, -4}, Vec3{-1, 1, 4}, Vec3{-1, 1, -4}, albedo, {}});
    scene.triangles.push_back({Vec3{-3, 1, -4}, Vec3{-3, 1, 4}, Vec3{-1, 1, 4}, albedo, {}});
    scene.lights.sun.direction = Vec3{0.0f, -1.0f, 0.0f};
    scene.lights.sun.color = Vec3{1.0f, 1.0f, 1.0f};
    scene.lights.sun.intensity = 3.0f;
    scene.sun_baked = true;
    scene.baked_light_ids.push_back({0x1234ull, 0x5678ull});
    scene.settings.sky_intensity = 0.0f;
    scene.settings.bounces = 0;
    scene.settings.rays = 16;
    scene.settings.surface_texel = 0.25f;
    scene.settings.surface_rays = 16;
    scene.settings.direct_samples = 4;
    scene.settings.threads = 2;
    gfx::BakedLighting out;
    lighting::BakeStats stats;
    std::string error;
    check(lighting::bakeProbes(scene, out, nullptr, &stats, &error) && !out.surface.empty(), "horneado con lightmap de superficie");
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const float lit = surfaceIrradiance(out.surface, Vec3{2.1f, 0.0f, 0.1f}, up);
    const float shadow = surfaceIrradiance(out.surface, Vec3{-1.9f, 0.0f, 0.1f}, up);
    std::printf("  (al sol %.3f, a la sombra %.3f, texeles %zu)\n", lit, shadow, stats.surface_texels);
    check(lit > 2.4f && lit < 3.6f, "el suelo al sol tiene la luz directa del sol (3)");
    check(shadow > -0.5f && shadow < lit * 0.2f, "bajo el tejado queda la sombra horneada");
    check(out.sun_baked && out.baked_lights.size() == 1, "lo horneado entero queda apuntado");
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "cramion_bake_direct_test.crbake";
    gfx::BakedLighting back;
    gfx::LightingMode mode = gfx::LightingMode::Realtime;
    check(lighting::saveBakedLighting(file, out, gfx::LightingMode::Baked) && lighting::loadBakedLighting(file, back, mode) &&
              back.sun_baked && back.baked_lights.size() == 1 && back.baked_lights[0][0] == 0x1234ull &&
              back.surface.count == out.surface.count && gfx::bakedSunMatches(back, Vec3{0.0f, -1.0f, 0.0f}) &&
              !gfx::bakedSunMatches(back, Vec3{0.3f, -1.0f, 0.0f}),
          "el .crbake (version 3) guarda las luces horneadas");
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

void testReplay() {
    std::printf("Repeticiones\n");
    ecs::World world;
    ecs::Entity e = world.create("Pelota");
    replay::ReplayOptions options;
    options.rate = 30.0f;
    replay::ReplaySystem r;
    check(r.startRecording(world, options), "empieza a grabar");
    for (int i = 0; i <= 60; ++i) {
        e.setWorldPosition(Vec3{static_cast<float>(i) * 0.1f, 0.0f, 0.0f});
        r.update(world, 1.0f / 60.0f);
    }
    r.stopRecording();
    check(r.duration() > 0.9f, "un segundo grabado");
    e.setWorldPosition(Vec3{100.0f, 0.0f, 0.0f});
    check(r.play(world, 0.0f, 1.0f), "reproduce");
    r.setPaused(true);
    r.seek(0.5f);
    r.update(world, 0.0f);
    check(std::fabs(e.worldPosition().x - 3.0f) < 0.3f, "a los 0.5 s la pelota esta donde estaba");
    r.stop(world);
    check(std::fabs(e.worldPosition().x - 100.0f) < 1e-3f, "al parar todo vuelve como estaba");
}

}  // namespace

int main() {
    testVfx();
    testPhysics2D();
    testLocalization();
    testDialogue();
    testSave();
    testBehaviorTree();
    testGraphs();
    testFracture();
    testBake();
    testBakeGather();
    testBakedDirect();
    testReplay();
    std::printf("\n%d de %d comprobaciones bien\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
