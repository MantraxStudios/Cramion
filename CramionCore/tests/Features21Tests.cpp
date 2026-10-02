// Pruebas de los sistemas de la 2.1 (consola, sin GPU): VFX Graph (Spawn y
// JSON), fisica 2D y tilemaps, localizacion, dialogos, partidas guardadas,
// Behavior Trees, Visual Scripting (compilar), Shader Graph (generar),
// fractura, horneado de sondas y repeticiones. Devuelve 0 si todo va.

#include "CramionCore/ai/BehaviorTree.h"
#include "CramionCore/asset/ShaderGraph.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/gameplay/Dialogue.h"
#include "CramionCore/gameplay/Localization.h"
#include "CramionCore/gameplay/SaveGame.h"
#include "CramionCore/lighting/ProbeBaker.h"
#include "CramionCore/physics/Fracture.h"
#include "CramionCore/replay/Replay.h"
#include "CramionCore/scripting/VisualScript.h"
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
    check(compiled.ok && compiled.lua.find("return") != std::string::npos, "el grafo de ejemplo compila a Lua");
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
    testReplay();
    std::printf("\n%d de %d comprobaciones bien\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
