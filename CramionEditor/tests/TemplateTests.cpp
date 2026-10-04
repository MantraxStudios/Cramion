// Pruebas de las plantillas de proyecto (consola, sin GPU): cada plantilla
// integrada crea un proyecto que se abre, su escena inicial se carga y se
// juega un rato (fisica, navegacion y los scripts de Lua sin errores). Y las
// plantillas del usuario: guardar un proyecto como plantilla y crear otro
// desde ella. Devuelve 0 si todo va.

#include "LocomotionPack.h"
#include "ProjectTemplates.h"
#include "CreatureModels.h"

#include <CramionCore/anim/Creature.h>
#include <CramionCore/anim/Humanoid.h>
#include <CramionCore/ecs/AnimatorController.h>
#include <CramionCore/ecs/Rigging.h>
#include <CramionCore/net/NetworkObject.h>
#include <CramionCore/twod/System2D.h>

#include <CramionCore/CramionCore.h>
#include <CramionCore/scripting/CppScripts.h>
#include <CramionCore/xr/XrRig.h>
#include <CramionDM/Input.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>

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

const editor::ProjectTemplate* find(const std::vector<editor::ProjectTemplate>& list, const std::string& id) {
    for (const auto& t : list) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

struct Played {
    bool loaded = false;
    int script_errors = 0;
    ecs::World world;
};

// La fisica 3D de la partida que se esta jugando (para conducir el coche
// desde `each_frame`).
physics::PhysicsSystem* playing_physics = nullptr;

// Abre el proyecto, carga su escena inicial y la juega `seconds`.
void play(const project::ProjectInfo& created, float seconds, Played& out,
          const std::function<void(ecs::World&, navigation::NavigationSystem&, float)>& each_frame = {}) {
    const auto info = project::openProject(created.file);
    if (!info) return;
    const std::filesystem::path scene = info->assetsFolder() / "Scenes" / "Main.crscene";
    std::string error;
    out.loaded = ecs::loadScene(out.world, scene, &error) && out.world.sceneUuid() == info->startup_scene;
    if (!out.loaded) {
        std::printf("  (%s)\n", error.c_str());
        return;
    }
    physics::PhysicsSystem physics;
    // Terrenos de la escena (el de la isla del mundo abierto): colision.
    terrain::TerrainStore terrains;
    terrains.setRoot(info->assetsFolder());
    physics.setTerrainProvider([&](ecs::Entity entity) -> std::shared_ptr<const terrain::TerrainData> {
        const terrain::Terrain* comp = entity.tryGet<terrain::Terrain>();
        return comp != nullptr ? terrains.get(*comp) : nullptr;
    });
    navigation::NavigationSystem nav;
    nav.setPhysics(&physics);
    scripting::ScriptSystem scripts;
    scripts.setAssetsRoot(info->assetsFolder());
    scripts.setPhysics(&physics);
    scripts.setNavigation(&nav);
    scripts.setLog([&](int level, const std::string& message) {
        if (level == 2) std::printf("  [Lua] %s\n", message.c_str());
    });
    twod::System2D twod_system;  // sprites, tilemaps y fisica 2D
    twod_system.setAssetsRoot(info->assetsFolder());
    physics.start(out.world);
    twod_system.start(out.world);
    nav.waitForBuild(out.world);
    scripts.start(out.world);
    playing_physics = &physics;
    const float dt = 1.0f / 60.0f;
    for (float t = 0.0f; t < seconds; t += dt) {
        const int steps = physics.update(out.world, dt, true);
        twod_system.update(out.world, dt, twod::System2D::Mode::Play);
        nav.update(out.world, dt, true);
        scripts.fixedUpdate(out.world, dt, steps);
        scripts.update(out.world, dt);
        if (each_frame) each_frame(out.world, nav, t);
    }
    out.script_errors = static_cast<int>(scripts.errors().size());
    playing_physics = nullptr;
    scripts.stop();
    twod_system.stop();
    physics.stop();
}

std::string hudText(ecs::World& world) {
    const ecs::Entity e = world.findByName("Marcador");
    return e.valid() && e.has<ui::Text>() ? e.get<ui::Text>().text : std::string();
}

}  // namespace

int main() {
    // Solo importar el Locomotion Pack y ver las medidas (desarrollo).
    if (const char* pack = std::getenv("LOCOMOTION_PACK")) {
        const std::filesystem::path out = std::filesystem::temp_directory_path() / "cramion_locomotion_probe";
        std::error_code ec;
        std::filesystem::remove_all(out, ec);
        std::filesystem::create_directories(out, ec);
        const editor::locomotion::PackResult r = editor::locomotion::importPack(pack, out);
        for (const std::string& line : r.log) std::printf("%s\n", line.c_str());
        std::printf("%s %s\n", r.ok ? "OK" : "FALLO", r.error.c_str());
        return r.ok ? 0 : 1;
    }
    physics::registerPhysicsComponents();
    terrain::registerTerrainComponents();
    water::registerWaterComponents();
    foliage::registerFoliageComponents();
    navigation::registerNavigationComponents();
    audio::registerAudioComponents();
    scripting::registerScriptComponents();
    ui::registerUiComponents();
    voxel::registerVoxelComponents();

    // Plantillas del usuario en una carpeta temporal (no en la de verdad).
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_template_tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "AppData");
    _putenv_s("LOCALAPPDATA", (root / "AppData").string().c_str());

    const std::vector<editor::ProjectTemplate> list = editor::availableTemplates();
    std::printf("Plantillas integradas\n");
    check(list.size() == 13 && find(list, "blank") && find(list, "third_person") && find(list, "navigation") &&
              find(list, "third_person_pro") &&
              find(list, "voxel") && find(list, "mmo") && find(list, "creatures") && find(list, "online") &&
              find(list, "open_world") && find(list, "state_machines") && find(list, "platformer_2d") &&
              find(list, "cars") && find(list, "vr"),
          "hay 13 plantillas integradas");

    // --- Criaturas: modelos con esqueleto generados, IK, phys bones, ragdoll ---
    {
        std::printf("Criaturas\n");
        const asset::ModelData dog = editor::makeDogModel();
        const asset::ModelData dummy = editor::makeDummyModel();
        const creature::Rig rig = creature::detect(dog.nodes, humanoid::restGlobals(dog.nodes));
        int three = 0;
        for (const creature::Leg& l : rig.legs) three += l.bones == 3 ? 1 : 0;
        check(rig.valid && rig.legs.size() == 4 && three == 4 && rig.head >= 0 && rig.tail.size() >= 4 && rig.forward.z < -0.9f,
              "el perro: 4 patas de 3 huesos, cabeza, cola y mira a -Z");
        check(humanoid::detect(dummy.nodes).valid, "el maniqui es humanoide (nombres de Mixamo)");
        check(!dog.animations.empty() && dog.animations[0].name == "Caminar" && !dog.vertices.empty() &&
                  dog.bones.size() + 1 == dog.nodes.size(),
              "mallas skinneadas, huesos y clips");
        ecs::InverseKinematics ik;
        check(ecs::suggestCreatureIK(dog, ik) && ik.chains.size() == 4 && ik.look_bone == "Head" && ik.align_body,
              "IK automatico del perro: 4 cadenas al suelo, cabeza y cuerpo inclinable");
        const std::vector<ecs::PhysBoneChain> pb = ecs::suggestPhysBones(dog);
        check(pb.size() == 3, "phys bones automaticos: cola y dos orejas");
        check(ecs::suggestPhysBones(dummy).size() == 1, "phys bones del maniqui: la coleta");
        check(ecs::suggestRagdollBones(dummy, 1.0f).size() == 12, "ragdoll del maniqui: 12 huesos");

        const project::ProjectInfo p = editor::createProjectFromTemplate(*find(list, "creatures"), root, "Criaturas");
        check(std::filesystem::exists(p.assetsFolder() / "Models" / "Perro.crdata") &&
                  std::filesystem::exists(p.assetsFolder() / "Models" / "Maniqui.crdata"),
              "escribe los modelos (.crdata)");
        assets::AssetDatabase database;
        database.open(p.assetsFolder());
        assets::AssetManager manager(database);
        Played r;
        float moved = 0.0f;
        Vec3 start{};
        play(p, 3.0f, r, [&](ecs::World& w, navigation::NavigationSystem&, float t) {
            const ecs::Entity dog_entity = w.findByName("Perro");
            if (t < 0.02f) start = dog_entity.worldPosition();
            moved = std::max(moved, core::length(dog_entity.worldPosition() - start));
        });
        const ecs::Entity dog_entity = r.world.findByName("Perro");
        const ecs::MeshRenderer* mr = dog_entity.valid() ? dog_entity.tryGet<ecs::MeshRenderer>() : nullptr;
        std::shared_ptr<const assets::ModelAsset> loaded = mr != nullptr ? manager.loadModel(mr->model.uuid) : nullptr;
        check(loaded && loaded->animated && !loaded->parts.empty() && !loaded->parts[0]->bones.empty(),
              "el perro carga como modelo animado con esqueleto");
        check(r.loaded && r.script_errors == 0, "la escena se abre y los scripts corren sin errores");
        check(dog_entity.valid() && dog_entity.has<ecs::InverseKinematics>() && dog_entity.has<ecs::PhysBones>() &&
                  dog_entity.has<ecs::Ragdoll>() && dog_entity.has<ecs::Skeleton>(),
              "el perro lleva IK, Phys Bones, Ragdoll y Esqueleto");
        check(r.world.findByName("Sombrero").valid() && r.world.findByName("Sombrero").has<ecs::BoneSocket>(),
              "el sombrero va enganchado a la cabeza (Bone Socket)");
        std::printf("    el perro anduvo %.2f m\n", moved);
        check(moved > 2.5f, "el perro recorre el circuito");
    }

    // --- Vacia ---
    {
        const project::ProjectInfo p = editor::createProjectFromTemplate(*find(list, "blank"), root, "Vacio");
        Played r;
        play(p, 0.5f, r);
        check(r.loaded && r.world.findByName("Suelo").valid() && r.world.findByName("Main Camera").valid(),
              "Vacia: escena inicial con suelo, camara y luz");
    }

    // --- Tercera persona avanzada (el Locomotion Pack de Mixamo, si esta en Descargas) ---
    if (const editor::ProjectTemplate* pro = find(list, "third_person_pro"); pro != nullptr && !pro->folder.empty()) {
        std::printf("Tercera persona avanzada (%s)\n", pro->folder.string().c_str());
        const project::ProjectInfo p = editor::createProjectFromTemplate(*pro, root, "Locomocion");
        const std::filesystem::path clips = p.assetsFolder() / "Animations" / "Locomotion";
        int cranims = 0;
        for (const auto& f : std::filesystem::directory_iterator(clips, ec)) cranims += f.path().extension() == ".cranim" ? 1 : 0;
        check(cranims >= 10, "importa las animaciones del pack como clips (.cranim)");
        ecs::AnimatorController controller;
        check(ecs::loadAnimatorController(p.assetsFolder() / "Animations" / "Personaje.cranimator", controller) &&
                  !controller.states.empty() && controller.states[0].motion == ecs::AnimatorMotion::BlendTree2D &&
                  controller.states[0].children.size() == 8 && controller.states.size() == 6,
              "Animator Controller: Blend Tree 2D de 8 clips, salto y 4 giros");
        bool backwards = false;
        for (const ecs::BlendTreeChild& c : controller.states[0].children) backwards = backwards || (c.speed < 0.0f && c.position.y < 0.0f);
        check(backwards, "andar de espaldas: el clip de andar al reves");

        ecs::World world;
        std::string error;
        const bool loaded = ecs::loadScene(world, p.assetsFolder() / "Scenes" / "Main.crscene", &error);
        check(loaded, "la escena se abre");
        if (loaded) {
            const ecs::Entity model = world.findByName("Modelo");
            check(model.valid() && model.has<ecs::Animator>() && model.has<ecs::InverseKinematics>() &&
                      model.get<ecs::InverseKinematics>().foot_grounding,
                  "el modelo lleva Animator (el controlador) e IK con los pies al suelo");
            physics::PhysicsSystem physics;
            physics.start(world);
            dm::Input input;
            scripting::ScriptSystem scripts;
            scripts.setAssetsRoot(p.assetsFolder());
            scripts.setPhysics(&physics);
            scripts.setInput(&input);
            scripts.setLog([&](int level, const std::string& message) {
                if (level == 2) std::printf("  [Lua] %s\n", message.c_str());
            });
            scripts.start(world);
            const auto frames = [&](int n) {
                for (int i = 0; i < n; ++i) {
                    const float dt = 1.0f / 60.0f;
                    const int steps = physics.update(world, dt, true);
                    scripts.fixedUpdate(world, dt, steps);
                    scripts.update(world, dt);
                    input.newFrame();
                }
            };
            const auto key = [&](dm::Key k, bool down) {
                dm::Event e;
                e.type = down ? dm::EventType::KeyPressed : dm::EventType::KeyReleased;
                e.key = k;
                input.onEvent(e);
            };
            const auto lua = [&](const std::string& code) {
                std::string out;
                if (!scripts.run("local J = Scene.find('Jugador'):getScript()\n" + code, &out)) std::printf("  (lua: %s)\n", out.c_str());
                return out;
            };
            const ecs::Entity player = world.findByName("Jugador");
            frames(60);
            const float rest_y = player.worldPosition().y;
            check(std::abs(rest_y - 0.9f) < 0.08f, "el personaje se apoya en el suelo");

            // Andar y correr hacia delante (la camara mira a -Z).
            Vec3 a = player.worldPosition();
            key(dm::Key::W, true);
            frames(120);
            Vec3 b = player.worldPosition();
            const float walk = (a.z - b.z) / 2.0f;
            key(dm::Key::LeftShift, true);
            frames(90);
            a = player.worldPosition();
            frames(60);
            b = player.worldPosition();
            const float run = a.z - b.z;
            key(dm::Key::LeftShift, false);
            key(dm::Key::W, false);
            const float y_param = model.get<ecs::Animator>().runtime.values.count("Y") ? model.get<ecs::Animator>().runtime.values.at("Y") : 0.0f;
            std::printf("    anda a %.2f m/s, corre a %.2f m/s (Animator Y = %.2f)\n", walk, run, y_param);
            check(walk > 1.0f && run > 3.5f && run > walk * 2.0f, "anda y corre (Shift) a la velocidad de sus animaciones");
            check(y_param > 3.0f, "el Animator recibe la velocidad hacia delante (Y)");
            check(lua("return tostring(J.energia < J.energiaMax)") == "true", "correr gasta energia");
            frames(60);

            // Escalera: sube andando (sin saltar) hasta el rellano.
            lua("J.entity.position = Vec3(-8, 0.95, 6.5); J.entity.velocity = Vec3.zero; J.yaw = 0");
            frames(20);
            key(dm::Key::W, true);
            float highest = 0.0f;
            for (int i = 0; i < 480; ++i) {
                frames(1);
                highest = std::max(highest, player.worldPosition().y);
                if (i % 30 == 0 && std::getenv("CRAMION_TRACE")) {
                    std::printf("      t %.1f  z %.2f  y %.2f\n", i / 60.0, player.worldPosition().z, player.worldPosition().y);
                }
                if (player.worldPosition().z < -1.0f) break;
            }
            key(dm::Key::W, false);
            std::printf("    en la escalera llega a y = %.2f (rellano a %.2f)\n", highest, 0.18 * 8 + 0.9);
            check(highest > 0.18f * 8.0f + 0.9f - 0.1f, "sube la escalera andando (escalones de 18 cm)");
            frames(30);

            // Salto: sube lo que dice alturaSalto, y aterriza.
            lua("J.entity.position = Vec3(0, 0.95, 12); J.entity.velocity = Vec3.zero");
            frames(30);
            const float ground = player.worldPosition().y;
            key(dm::Key::Space, true);
            frames(1);
            key(dm::Key::Space, false);
            float peak = ground;
            for (int i = 0; i < 150; ++i) {
                frames(1);
                peak = std::max(peak, player.worldPosition().y);
            }
            std::printf("    salto: %.2f m\n", peak - ground);
            check(peak - ground > 0.8f && peak - ground < 1.4f, "salta (despega en el fotograma de la animacion)");
            check(lua("return tostring(J.salto == nil) .. tostring(J.enSuelo)") == "truetrue", "aterriza y termina el salto");

            // Palanca: E cerca, la mano la agarra y la compuerta sube.
            lua("local p = Scene.find('Palanca 1'); J.entity.position = p.position + p.forward * 1.0 + Vec3(0, 0.95, 0)");
            frames(10);
            const float gate_y = world.findByName("Compuerta 1").worldPosition().y;
            key(dm::Key::E, true);
            frames(1);
            key(dm::Key::E, false);
            frames(40);
            const bool reaching = model.get<ecs::InverseKinematics>().right_hand.weight > 0.9f;
            frames(160);
            const float gate_up = world.findByName("Compuerta 1").worldPosition().y - gate_y;
            check(reaching, "la mano derecha va a la palanca (IK)");
            check(gate_up > 2.5f, "la palanca abre la compuerta");

            // Cristales: recoger uno.
            // (El del rellano ya se recogio al subir la escalera.)
            const std::string before = lua("return tostring(J.cristales)");
            lua("local c = Scene.find('Cristal 4'); J.entity.position = c.position");
            frames(5);
            const std::string after = lua("return tostring(J.cristales)");
            std::printf("    cristales %s -> %s, marcador \"%s\"\n", before.c_str(), after.c_str(), hudText(world).c_str());
            check(!before.empty() && after == std::to_string(std::stoi(before) + 1) &&
                      hudText(world).find(after + " / 7") != std::string::npos,
                  "recoge un cristal y el marcador lo cuenta");
            check(scripts.errors().empty(), "los scripts corren sin errores");
            scripts.stop();
            physics.stop();
        }
    } else {
        std::printf("Tercera persona avanzada: sin el Locomotion Pack en Descargas (no se prueba)\n");
    }

    // --- Tercera persona ---
    {
        std::printf("Tercera persona\n");
        const project::ProjectInfo p = editor::createProjectFromTemplate(*find(list, "third_person"), root, "Plataformas");
        int materials = 0;
        for (const auto& f : std::filesystem::directory_iterator(p.assetsFolder() / "Materials")) {
            assets::MaterialAsset m;
            if (assets::loadMaterial(f.path(), m)) ++materials;
        }
        check(materials >= 6, "crea sus materiales (.crmat)");
        Played r;
        float start_y = 0.0f;
        play(p, 3.0f, r, [&](ecs::World& w, navigation::NavigationSystem&, float t) {
            if (t < 0.02f) start_y = w.findByName("Jugador").worldPosition().y;
        });
        const ecs::Entity player = r.world.findByName("Jugador");
        std::printf("  (jugador en y = %.2f, HUD \"%s\")\n", static_cast<double>(player.worldPosition().y),
                    hudText(r.world).c_str());
        check(r.loaded, "la escena inicial se abre");
        check(r.script_errors == 0, "los scripts se ejecutan sin errores");
        check(player.valid() && std::abs(player.worldPosition().y - 0.95f) < 0.3f, "el jugador se apoya en el suelo");
        check(hudText(r.world) == "Monedas  0 / 8", "el marcador cuenta las 8 monedas");
        check(r.world.findAllWithTag("Moneda").size() == 8, "las monedas tienen su tag");
    }

    // --- IA y navegacion ---
    {
        std::printf("IA y navegacion\n");
        const project::ProjectInfo p = editor::createProjectFromTemplate(*find(list, "navigation"), root, "Escapa");
        Played r;
        std::vector<Vec3> guard_start;
        float moved = 0.0f;
        bool nav_ready = false;
        play(p, 6.0f, r, [&](ecs::World& w, navigation::NavigationSystem& nav, float t) {
            nav_ready = nav_ready || nav.ready();
            for (int i = 0; i < 3; ++i) {
                const ecs::Entity g = w.findByName("Guardia " + std::to_string(i + 1));
                if (t < 0.02f) guard_start.push_back(g.worldPosition());
                else if (static_cast<std::size_t>(i) < guard_start.size()) {
                    moved = std::max(moved, core::length(g.worldPosition() - guard_start[static_cast<std::size_t>(i)]));
                }
            }
        });
        std::printf("  (un guardia se movio %.1f m; HUD \"%s\")\n", static_cast<double>(moved), hudText(r.world).c_str());
        check(r.loaded && nav_ready, "la escena genera su malla de navegacion");
        check(r.script_errors == 0, "los scripts se ejecutan sin errores");
        check(moved > 2.0f, "los guardias patrullan por la malla");
        check(hudText(r.world).find("Atrapado: 0") != std::string::npos, "el HUD empieza a cero");
        navigation::NavigationSettings s;
        check(navigation::loadNavigationSettings(p.settingsFolder() / "Navigation.json", s), "guarda Navigation.json");
    }

    // --- Mundo de bloques ---
    {
        std::printf("Mundo de bloques\n");
        const project::ProjectInfo p = editor::createProjectFromTemplate(*find(list, "voxel"), root, "Bloques");
        const auto info = project::openProject(p.file);
        ecs::World world;
        std::string error;
        const bool loaded = info && ecs::loadScene(world, info->assetsFolder() / "Scenes" / "Main.crscene", &error);
        check(loaded && world.findByName("Mundo de bloques").valid() &&
                  world.findByName("Mundo de bloques").has<voxel::VoxelWorld>() && world.findByName("Mar").valid(),
              "la escena tiene el mundo de bloques y su mar");
        const std::filesystem::path icons = p.assetsFolder() / "Voxel" / "Iconos";
        check(std::filesystem::exists(icons / "stone.png") && std::filesystem::exists(icons / "palo.png") &&
                  std::filesystem::exists(icons / "corazon_medio.png") && std::filesystem::exists(icons / "pico_piedra.png") &&
                  std::filesystem::exists(p.assetsFolder() / "Voxel" / "Grietas" / "grieta_9.png"),
              "iconos de bloques, objetos y estado; texturas de grietas");
        check(world.findByName("Inventario").valid() && !world.findByName("Inventario").activeSelf() &&
                  world.findByName("Receta 13").valid() && world.findByName("Inv 36").valid() && world.findByName("Corazon 10").valid(),
              "la interfaz: barra, vida, hambre, inventario de 36 huecos y 13 recetas");
        if (loaded) {
            // Como el juego: fisica, bloques y scripts, con teclado y raton simulados.
            physics::PhysicsSystem physics;
            physics.start(world);
            voxel::VoxelSystem voxels;
            voxels.setPhysics(&physics);
            voxels.setAssetsRoot(info->assetsFolder());
            voxels.setSaveRoot(root / "Mundos");
            voxels.start(world);
            dm::Input input;
            scripting::ScriptSystem scripts;
            scripts.setAssetsRoot(info->assetsFolder());
            scripts.setPhysics(&physics);
            scripts.setVoxels(&voxels);
            scripts.setInput(&input);
            bool locked = false;
            scripts.setCursorLock([&](bool on) { locked = on; });
            scripts.setLog([&](int level, const std::string& message) {
                if (level == 2) std::printf("  [Lua] %s\n", message.c_str());
            });
            scripts.start(world);
            const ecs::Entity camera = world.findByName("Main Camera");
            const auto frame = [&](float dt) {
                voxels.update(dt, camera.worldPosition());
                const int steps = physics.update(world, dt, true);
                scripts.fixedUpdate(world, dt, steps);
                scripts.update(world, dt);
                input.newFrame();
            };
            const auto frames = [&](int n) {
                for (int i = 0; i < n; ++i) frame(1.0f / 60.0f);
            };
            const auto send = [&](dm::EventType type, dm::Key key, dm::MouseButton button = dm::MouseButton::Left) {
                dm::Event e;
                e.type = type;
                e.key = key;
                e.button = button;
                input.onEvent(e);
            };
            const auto click = [&](dm::MouseButton button) {
                send(dm::EventType::MouseButtonPressed, dm::Key::Unknown, button);
                frame(1.0f / 60.0f);
                send(dm::EventType::MouseButtonReleased, dm::Key::Unknown, button);
                frame(1.0f / 60.0f);
            };
            const auto press = [&](dm::Key key) {
                send(dm::EventType::KeyPressed, key);
                frame(1.0f / 60.0f);
                send(dm::EventType::KeyReleased, key);
                frame(1.0f / 60.0f);
            };
            // Codigo contra el jugador (su instancia del script: J).
            const auto lua = [&](const std::string& code) {
                std::string out;
                const bool ok = scripts.run("local J = Scene.find('Main Camera'):getScript()\n" + code, &out);
                if (!ok) std::printf("  (lua: %s)\n", out.c_str());
                return out;
            };
            const auto text_of = [&](const char* name) {
                const ecs::Entity e = world.findByName(name);
                return e.valid() && e.has<ui::Text>() ? e.get<ui::Text>().text : std::string();
            };

            // Siempre el mismo mundo (antes de su Start): con la semilla al azar
            // (math.random de Lua) a veces se aparecia ante una colina y las
            // pruebas de andar y romper fallaban de vez en cuando.
            lua("J.semilla = 12345");
            // Aparece y cae hasta el suelo (espera a que se genere).
            frames(600);
            voxels.waitUntilReady(camera.worldPosition(), 2, 30.0f);
            frames(240);
            const Vec3 eye = camera.worldPosition();
            const int below = voxels.getBlock(static_cast<int>(std::floor(eye.x)), static_cast<int>(std::floor(eye.y - 1.62f - 0.2f)),
                                              static_cast<int>(std::floor(eye.z)));
            std::printf("  (ojos en %.2f %.2f %.2f, mundo \"%s\")\n", static_cast<double>(eye.x), static_cast<double>(eye.y),
                        static_cast<double>(eye.z), voxels.worldName().c_str());
            check(voxels.worldName() == "Mi mundo", "crea el mundo con nombre");
            check(voxel::blockDef(static_cast<voxel::BlockId>(below)).solid || below == voxel::block::Water,
                  "el jugador aparece y se apoya en el suelo");
            check(lua("return tostring(J.vida) .. ' ' .. tostring(J.hambre)") == "20 20" &&
                      world.findByName("Corazon 1").get<ui::Image>().texture == "Voxel/Iconos/corazon.png",
                  "supervivencia: vida y hambre llenas en pantalla");
            // Andar con W un segundo.
            send(dm::EventType::KeyPressed, dm::Key::W);
            frames(60);
            send(dm::EventType::KeyReleased, dm::Key::W);
            frames(20);
            const float walked = core::length(Vec3{camera.worldPosition().x - eye.x, 0.0f, camera.worldPosition().z - eye.z});
            std::printf("  (anduvo %.2f m)\n", static_cast<double>(walked));
            check(walked > 1.5f, "WASD mueve al jugador por los bloques");

            // Clic: captura el raton; mirar al suelo; mantener el clic rompe el bloque.
            click(dm::MouseButton::Left);
            check(locked && scripts.cursorLocked(), "un clic captura el raton (primera persona)");
            dm::Event look;
            look.type = dm::EventType::MouseRawMoved;
            look.deltaY = 2000.0f;  // todo abajo
            input.onEvent(look);
            frame(1.0f / 60.0f);
            // Un bloque de tierra bajo los pies (se rompe con la mano).
            const voxel::VoxelHit ground = voxels.raycast(camera.worldPosition(), camera.forward(), 6.0f);
            if (ground.hit) voxels.setBlock(ground.block.x, ground.block.y, ground.block.z, voxel::block::Dirt);
            frames(2);
            check(ground.hit && world.findByName("Contorno").activeSelf(), "contorno sobre el bloque apuntado");
            send(dm::EventType::MouseButtonPressed, dm::Key::Unknown, dm::MouseButton::Left);
            frames(15);
            const bool cracking = world.findByName("Grietas").activeSelf() && world.findByName("Grietas").get<ecs::MeshRenderer>().mesh;
            frames(60);
            send(dm::EventType::MouseButtonReleased, dm::Key::Unknown, dm::MouseButton::Left);
            frame(1.0f / 60.0f);
            check(cracking, "mientras se rompe se ven las grietas");
            check(ground.hit && voxels.getBlock(ground.block.x, ground.block.y, ground.block.z) != voxel::block::Dirt,
                  "mantener el clic rompe el bloque (segun su dureza)");
            frames(120);  // el objeto cae y se recoge
            check(lua("return tostring(J:cuenta('dirt'))") == "1", "el bloque suelta un objeto y se recoge");
            check(world.findByName("Hueco 1").valid() &&
                      world.findByName("Hueco 1").children().size() == 2 &&
                      world.wrap(world.findByName("Hueco 1").children()[0]).get<ui::Image>().texture == "Voxel/Iconos/dirt.png",
                  "la barra muestra su icono");

            // Crafteo: troncos -> tablones -> palos; el pico pide mesa de trabajo.
            lua("J:dar('oak_log', 3)");
            lua("J:OnReceta(Scene.find('Receta 1'))");
            check(lua("return tostring(J:cuenta('oak_planks')) .. ' ' .. tostring(J:cuenta('oak_log'))") == "4 2",
                  "craftear: 1 tronco da 4 tablones");
            lua("J:OnReceta(Scene.find('Receta 4'))");
            check(lua("return tostring(J:cuenta('palo')) .. ' ' .. tostring(J:cuenta('oak_planks'))") == "4 2",
                  "2 tablones dan 4 palos");
            lua("J:OnReceta(Scene.find('Receta 7'))");
            check(lua("return tostring(J:cuenta('pico_madera'))") == "0" && text_of("Aviso").find("mesa") != std::string::npos,
                  "el pico sin mesa de trabajo no se puede");
            lua("J:OnReceta(Scene.find('Receta 1')); J:OnReceta(Scene.find('Receta 1')); J:OnReceta(Scene.find('Receta 5'))");
            check(lua("return tostring(J:cuenta('crafting_table'))") == "1", "4 tablones dan la mesa de trabajo");
            lua("local c = J.centro; Voxel.setBlock(c.x + 2, c.y, c.z, 'crafting_table'); J:OnReceta(Scene.find('Receta 7'))");
            check(lua("return tostring(J:cuenta('pico_madera'))") == "1", "junto a la mesa, el pico de madera");
            // El inventario (E) marca que recetas se pueden hacer.
            press(dm::Key::E);
            const ecs::Entity bag = world.findByName("Inventario");
            check(bag.activeSelf() && !locked, "E abre el inventario y suelta el raton");
            check(!world.findByName("Receta 8").get<ui::Button>().interactable &&
                      text_of("Texto").size() > 0,
                  "sin roca, la receta del pico de piedra esta apagada");
            press(dm::Key::E);
            check(!bag.activeSelf() && locked, "E lo cierra y vuelve a capturar el raton");

            // Dano por caida, comer, morir y reaparecer.
            lua("J.centro = J.centro + Vec3(0, 10, 0); J.caidaDesde = J.centro.y; J.vel = Vec3.zero");
            frames(150);
            const std::string after_fall = lua("return tostring(J.vida)");
            std::printf("  (vida tras caer 10 m: %s)\n", after_fall.c_str());
            check(std::atoi(after_fall.c_str()) < 20 && std::atoi(after_fall.c_str()) > 8, "caer desde alto quita vida");
            lua("J.hambre = 10; J:dar('manzana', 1); for i = 1, 36 do local h = J.inv[i]; if h and h.item == 'manzana' then "
                "J.inv[i], J.inv[1] = J.inv[1], J.inv[i] end end; J.elegido = 1; J:pintarTodo()");
            click(dm::MouseButton::Right);
            check(lua("return tostring(J.hambre) .. ' ' .. tostring(J:cuenta('manzana'))") == "14 0",
                  "clic derecho con una manzana: se come (+4 de hambre)");
            lua("J:herir(40, 'prueba')");
            frame(1.0f / 60.0f);
            check(world.findByName("Muerte").activeSelf() && text_of("Causa") == "prueba" && !locked, "sin vida, la pantalla de muerte");
            lua("J:OnReaparecer()");
            frames(30);
            check(!world.findByName("Muerte").activeSelf() && lua("return tostring(J.vida)") == "20", "reaparecer con la vida llena");
            check(scripts.errors().empty(), "el script se ejecuta sin errores");

            // Parar guarda el mundo, el inventario y la vida.
            scripts.stop();
            check(!locked, "al parar se suelta el raton");
            voxels.stop();
            physics.stop();
            voxel::VoxelSystem again;
            again.setSaveRoot(root / "Mundos");
            again.start(world);
            const auto worlds = again.listWorlds();
            check(worlds.size() == 1 && worlds[0].name == "Mi mundo" && worlds[0].mode == "supervivencia" &&
                      again.loadWorld("Mi mundo") && !again.meta("player").empty() &&
                      again.meta("inventario").find("pico_madera") != std::string::npos,
                  "se guardan el mundo, la posicion y el inventario");
        }
    }

    // --- MMO RPG (todo el juego en Lua) ---
    {
        std::printf("MMO RPG\n");
        const project::ProjectInfo p = editor::createProjectFromTemplate(*find(list, "mmo"), root, "Reinos");
        const auto info = project::openProject(p.file);
        ecs::World world;
        std::string error;
        const bool loaded = info && ecs::loadScene(world, info->assetsFolder() / "Scenes" / "Main.crscene", &error);
        check(loaded && world.findAllWithTag("Enemigo").size() == 19 && world.findAllWithTag("NPC").size() == 3 &&
                  world.findAllWithTag("Bot").size() == 4,
              "la escena: 19 enemigos, 3 personajes y 4 jugadores simulados");
        check(std::filesystem::exists(p.assetsFolder() / "Scripts" / "Heroe.lua") &&
                  std::filesystem::exists(p.assetsFolder() / "Scripts" / "Enemigo.lua") &&
                  std::filesystem::exists(p.assetsFolder() / "Scripts" / "Bot.lua") &&
                  std::filesystem::exists(p.assetsFolder() / "Scripts" / "NPC.lua"),
              "los sistemas estan en scripts de Lua");
        if (loaded) {
            physics::PhysicsSystem physics;
            navigation::NavigationSystem nav;
            nav.setPhysics(&physics);
            physics.start(world);
            nav.waitForBuild(world);
            dm::Input input;
            scripting::ScriptSystem scripts;
            scripts.setAssetsRoot(info->assetsFolder());
            scripts.setPhysics(&physics);
            scripts.setNavigation(&nav);
            scripts.setInput(&input);
            scripts.setPrefsFile(root / "mmo.prefs");
            scripts.setLog([&](int level, const std::string& message) {
                if (level == 2) std::printf("  [Lua] %s\n", message.c_str());
            });
            scripts.start(world);
            const auto frame = [&](float dt) {
                const int steps = physics.update(world, dt, true);
                nav.update(world, dt, true);
                scripts.fixedUpdate(world, dt, steps);
                scripts.update(world, dt);
                input.newFrame();
            };
            const auto frames = [&](int n) {
                for (int i = 0; i < n; ++i) frame(1.0f / 60.0f);
            };
            const auto press = [&](dm::Key key) {
                dm::Event e;
                e.type = dm::EventType::KeyPressed;
                e.key = key;
                input.onEvent(e);
                frame(1.0f / 60.0f);
                e.type = dm::EventType::KeyReleased;
                input.onEvent(e);
                frame(1.0f / 60.0f);
            };
            const auto lua = [&](const std::string& code) {
                std::string out;
                const bool ok = scripts.run("local H = Scene.find('Jugador'):getScript()\n" + code, &out);
                if (!ok) std::printf("  (lua: %s)\n", out.c_str());
                return out;
            };
            const auto text_of = [&](const char* name) {
                const ecs::Entity e = world.findByName(name);
                return e.valid() && e.has<ui::Text>() ? e.get<ui::Text>().text : std::string();
            };

            frames(120);
            check(nav.ready(), "genera la malla de navegacion de todo el mapa");
            check(text_of("UI_Chat").find("Bienvenido") != std::string::npos, "el chat da la bienvenida");
            check(lua("return tostring(H.nivel) .. '|' .. H.vida .. '|' .. H:cuenta('pocion_vida')") == "1|100|3",
                  "empieza a nivel 1, con 100 de vida y 3 pociones");
            check(lua("return tostring(Scene.find('Capitana Elena'):find('Marca').active)") == "true",
                  "la capitana muestra la marca de mision nueva");

            // Teclado: Tab elige el enemigo mas cercano; I abre el inventario.
            lua("local w = Scene.find('Lobo 1'); H.entity.position = w.position + Vec3(2, 0.6, 0)");
            frames(2);
            press(dm::Key::Tab);
            check(lua("return H.objetivo and H.objetivo.name or 'nada'").find("Lobo") != std::string::npos, "Tab elige el lobo de al lado");
            press(dm::Key::I);
            check(world.findByName("UI_Inventario").activeSelf(), "I abre el inventario");
            press(dm::Key::Escape);
            check(!world.findByName("UI_Inventario").activeSelf(), "Escape lo cierra");

            // Hablar (E) con la capitana y aceptar la mision de los lobos.
            lua("local c = Scene.find('Capitana Elena'); H.entity.position = c.position + Vec3(0, 0, 2)");
            frames(2);
            press(dm::Key::E);
            check(world.findByName("UI_Dialogo").activeSelf() && lua("return H.opciones[1].texto") == "Mision: Lobos hambrientos",
                  "E abre el dialogo con la mision");
            lua("local d = Scene.find('UI_Dialogo'); H:OnOpcion(d:find('Opcion 1')); H:OnOpcion(d:find('Opcion 1'))");
            check(lua("return H.misiones[1] and H.misiones[1].estado or 'no'") == "activa", "se acepta la mision");

            // Cazar 5 lobos con Golpe (1) y la bola de fuego.
            const std::string hunt = lua(R"(
                local muertos = 0
                for _, e in ipairs(Scene.findAllWithTag('Enemigo')) do
                    local s = e:getScript()
                    if muertos < 5 and s.tipo == 'lobo' and s:EstaVivo() then
                        H.entity.position = e.position + Vec3(1.6, 0.6, 0)
                        H.objetivo = e
                        for i = 1, 60 do
                            if not s:EstaVivo() then break end
                            H.gcd = 0; H.mana = H.manaMax
                            H:usarHabilidad(i % 3 == 0 and 2 or 1)
                            if H.lanzando then H.lanzando.t = H.lanzando.dur; H:lanzar(0) end
                        end
                        if not s:EstaVivo() then muertos = muertos + 1 end
                    end
                end
                return muertos .. '|' .. H.misiones[1].progreso .. '|' .. H.misiones[1].estado .. '|' .. H.nivel
            )");
            std::printf("  (caza: %s)\n", hunt.c_str());
            check(hunt.rfind("5|5|lista|", 0) == 0, "5 lobos muertos: mision lista");
            check(lua("return tostring(H.nivel >= 2)") == "true", "sube de nivel con la experiencia");
            // Botin: pasar por encima de las bolsas.
            const int gold_before = std::atoi(lua("return tostring(H.oro)").c_str());
            const int bags = std::atoi(lua("return tostring(#H.botines)").c_str());
            for (int i = 1; i <= bags; ++i) {
                lua("local b = H.botines[1]; if b then H.entity.position = b.e.position + Vec3(0, 1, 0) end");
                frames(3);
            }
            const int gold_after = std::atoi(lua("return tostring(H.oro)").c_str());
            std::printf("  (%d bolsas de botin, oro %d -> %d)\n", bags, gold_before, gold_after);
            check(bags > 0 && gold_after > gold_before, "los enemigos sueltan botin y se recoge al pasar");

            // Entregar, equipar el premio y comprar en la tienda.
            check(lua("return tostring(H:entregarMision(1))") == "true" && lua("return H.misiones[1].estado") == "hecha",
                  "se entrega la mision");
            check(lua("return tostring(H:equipar('espada_hierro')) .. '|' .. H.equipo.arma") == "true|espada_hierro",
                  "se equipa la espada de la recompensa (sube el ataque)");
            const std::string shop = lua(R"(
                local antes, pociones = H.oro, H:cuenta('pocion_vida')
                H:abrirTienda(Scene.find('Mercader Tomas'))
                H:comprar('pocion_vida')
                return (antes - H.oro) .. '|' .. (H:cuenta('pocion_vida') - pociones)
            )");
            check(shop == "8|1", "la tienda vende pociones por oro");
            check(lua("H:usarHabilidad(5); return tostring(H.enfriamientos[5] > 0)") == "true", "5 usa una pocion (con enfriamiento)");

            // Un goblin ataca al heroe si se acerca.
            lua("H.vida = H.vidaMax; local g = Scene.find('Goblin 9'); H.entity.position = g.position + Vec3(2, 0.3, 0)");
            frames(240);
            check(lua("return tostring(H.vida < H.vidaMax or H.muerto)") == "true", "los goblins atacan al heroe cercano");

            // Morir y reaparecer.
            lua("H:RecibirDano(99999, nil, 'Prueba')");
            check(world.findByName("UI_Muerte").activeSelf(), "al morir sale la pantalla de muerte");
            lua("H:OnReaparecer()");
            check(!world.findByName("UI_Muerte").activeSelf() && lua("return tostring(H.muerto)") == "false", "reaparece en el pueblo");

            // Los otros jugadores se mueven por el mundo.
            std::vector<Vec3> start;
            for (const ecs::Entity& b : world.findAllWithTag("Bot")) start.push_back(b.worldPosition());
            frames(900);
            float moved = 0.0f;
            std::size_t i = 0;
            for (const ecs::Entity& b : world.findAllWithTag("Bot")) {
                if (i < start.size()) moved = std::max(moved, core::length(b.worldPosition() - start[i]));
                ++i;
            }
            std::printf("  (un jugador simulado se movio %.1f m)\n", static_cast<double>(moved));
            check(moved > 3.0f, "los jugadores simulados se mueven por el mundo");

            // Guardar: la partida va a Prefs.
            lua("H:guardar()");
            const std::string saved = lua("return Prefs.getString('mmo_partida', '')");
            check(saved.rfind("1|", 0) == 0 && saved.find("espada_hierro") != std::string::npos && saved.find("1:hecha") != std::string::npos,
                  "la partida se guarda (nivel, inventario, equipo y misiones)");
            check(scripts.errors().empty(), "los scripts se ejecutan sin errores");
            for (const scripting::ScriptError& e : scripts.errors()) {
                std::printf("  [error] %s:%d %s\n", e.file.c_str(), e.line, e.message.c_str());
            }
            scripts.stop();
            physics.stop();
        }
    }

    // --- Mundo abierto (rendimiento) ---
    {
        std::printf("Mundo abierto\n");
        const auto t0 = std::chrono::steady_clock::now();
        const project::ProjectInfo p = editor::createProjectFromTemplate(*find(list, "open_world"), root, "Isla");
        std::printf("    creada en %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        check(std::filesystem::exists(p.assetsFolder() / "Terrains" / "Isla.crterrain"), "guarda el terreno de la isla (.crterrain)");
        ecs::World w;
        std::string error;
        check(ecs::loadScene(w, p.assetsFolder() / "Scenes" / "Main.crscene", &error), "la escena se abre");
        const ecs::Entity island = w.findByName("Isla");
        const ecs::Entity forest = w.findByName("Vegetacion");
        const ecs::Entity ocean = w.findByName("Oceano");
        check(island.valid() && island.has<terrain::Terrain>() && island.get<terrain::Terrain>().size == 8192.0f,
              "terreno de 8 km");
        check(ocean.valid() && ocean.has<water::WaterBody>() && ocean.get<water::WaterBody>().type == water::WaterType::Ocean,
              "oceano");
        check(forest.valid() && forest.has<foliage::Foliage>(), "vegetacion (componente Foliage)");

        // Sembrar el bosque tal como lo hara el motor.
        terrain::TerrainStore store;
        store.setRoot(p.assetsFolder());
        const terrain::Terrain& tc = island.get<terrain::Terrain>();
        std::shared_ptr<terrain::TerrainData> data = store.get(tc);
        std::vector<foliage::FoliageGround> ground{{data, tc, island.worldPosition()}};
        const auto t1 = std::chrono::steady_clock::now();
        const foliage::FoliageResult trees = foliage::generateFoliage(forest.get<foliage::Foliage>(), forest.worldPosition(), ground);
        std::printf("    %zu arboles sembrados en %.2f s (de %llu celdas)\n", trees.instances.size(),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count(),
                    static_cast<unsigned long long>(trees.candidates));
        check(trees.instances.size() > 1500000, "mas de 1,5 millones de arboles en la isla");
        bool above_sea = true;
        for (std::size_t i = 0; i < trees.instances.size(); i += 1009) above_sea = above_sea && trees.instances[i].y > 3.0f;
        check(above_sea, "ninguno en el mar ni en la playa");

        // Jugar: el jugador cae sobre el terreno y el HUD mide sin errores.
        Played r;
        float lowest = 1e9f;
        play(p, 3.0f, r, [&](ecs::World& world, navigation::NavigationSystem&, float) {
            const ecs::Entity pl = world.findByName("Jugador");
            if (pl.valid()) lowest = std::min(lowest, pl.worldPosition().y);
        });
        const ecs::Entity pl = r.world.findByName("Jugador");
        const float ground_y = terrain::heightAt(*data, tc, island.worldPosition(), pl.worldPosition().x, pl.worldPosition().z);
        std::printf("    jugador a %.2f m, suelo a %.2f m\n", pl.worldPosition().y, ground_y);
        check(r.loaded && r.script_errors == 0, "la escena se juega y los scripts corren sin errores");
        check(ground_y > 5.0f, "el jugador aparece en tierra, no en el mar");
        check(std::abs(pl.worldPosition().y - ground_y - 1.4f) < 0.8f, "y se queda de pie sobre el terreno (colision)");
        const ecs::Entity hud = r.world.findByName("UI_Rendimiento");
        check(hud.valid() && hud.get<ui::Text>().text.find("FPS") != std::string::npos, "el HUD de rendimiento mide");
    }

    // --- Online: un servidor y un cliente en el mismo proceso ---
    {
        std::printf("Online\n");
        const project::ProjectInfo p = editor::createProjectFromTemplate(*find(list, "online"), root, "Arena");
        check(std::filesystem::exists(p.assetsFolder() / "Prefabs" / "Jugador.crprefab") &&
                  std::filesystem::exists(p.assetsFolder() / "Prefabs" / "Balon.crprefab") &&
                  std::filesystem::exists(p.assetsFolder() / "Scripts" / "Red.lua"),
              "prefabs de red y scripts");
        struct Game {
            ecs::World world;
            physics::PhysicsSystem physics;
            scripting::ScriptSystem scripts;
            std::vector<std::string> errors;
        };
        Game host, guest;
        const auto open = [&](Game& g) {
            std::string error;
            const bool ok = ecs::loadScene(g.world, p.assetsFolder() / "Scenes" / "Main.crscene", &error);
            g.scripts.setAssetsRoot(p.assetsFolder());
            g.scripts.setPhysics(&g.physics);
            g.scripts.setPrefsFile(root / ("prefs_" + std::to_string(reinterpret_cast<std::uintptr_t>(&g)) + ".txt"));
            g.scripts.setLog([&g](int level, const std::string& m) {
                if (level == 2) {
                    g.errors.push_back(m);
                    std::printf("  [Lua] %s\n", m.c_str());
                }
            });
            g.physics.start(g.world);
            g.scripts.start(g.world);
            return ok;
        };
        check(open(host) && open(guest), "la escena se abre dos veces (servidor y cliente)");
        const auto frames = [&](int n) {
            for (int i = 0; i < n; ++i) {
                for (Game* g : {&host, &guest}) {
                    const int steps = g->physics.update(g->world, 1.0f / 60.0f, true);
                    g->scripts.fixedUpdate(g->world, 1.0f / 60.0f, steps);
                    g->scripts.update(g->world, 1.0f / 60.0f);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        const auto lua = [&](Game& g, const std::string& code) {
            std::string out;
            if (!g.scripts.run("local R = Scene.find('Red'):getScript()\n" + code, &out)) std::printf("  (lua: %s)\n", out.c_str());
            return out;
        };
        const auto text_of = [](Game& g, const char* name) {
            const ecs::Entity e = g.world.findByName(name);
            return e.valid() && e.has<ui::Text>() ? e.get<ui::Text>().text : std::string();
        };
        const auto count = [](Game& g, const std::string& name) {
            int n = 0;
            for (const auto h : g.world.registry().view<net::NetworkObject>()) {
                if (g.world.wrap(h).name() == name) ++n;
            }
            return n;
        };
        frames(5);
        check(host.world.findByName("UI_Menu").activeSelf() && !host.world.findByName("UI_Juego").activeSelf(), "empieza en el menu");
        lua(host, "R.puerto = 27790; R.campoNombre.text = 'Ana'; R:OnCrear()");
        frames(5);
        check(lua(host, "return tostring(Network.isServer())") == "true" && !host.world.findByName("UI_Menu").activeSelf(),
              "Crear partida: servidor y a jugar");
        check(count(host, "Jugador") == 1 && count(host, "Balon") == 1 && count(host, "Caja") == 4 && count(host, "Moneda") == 8,
              "el servidor crea su jugador, el balon, 4 cajas y 8 monedas");
        lua(guest, "R.puerto = 27790; R.campoNombre.text = 'Luis'; R.campoIP.text = '127.0.0.1'; R:OnUnirse()");
        for (int i = 0; i < 200 && count(guest, "Jugador") < 2; ++i) frames(1);
        frames(30);
        check(lua(guest, "return tostring(Network.myId())") == "2", "Unirse: el cliente es el jugador 2");
        check(count(guest, "Jugador") == 2 && count(guest, "Balon") == 1 && count(guest, "Moneda") >= 8,
              "el cliente recibe los dos jugadores, el balon y las monedas");
        check(count(host, "Jugador") == 2, "el servidor ve al nuevo jugador");
        check(text_of(host, "UI_Chat").find("Luis se ha unido") != std::string::npos &&
                  text_of(guest, "UI_Chat").find("Luis se ha unido") != std::string::npos,
              "el chat anuncia la llegada en los dos");
        check(text_of(guest, "UI_Marcador").find("Ana") != std::string::npos && text_of(guest, "UI_Marcador").find("Luis") != std::string::npos,
              "el marcador del cliente tiene a los dos");

        // Chat del cliente: pasa por el servidor, que pone el nombre.
        lua(guest, "R:OnChat('hola desde el cliente')");
        frames(20);
        check(text_of(host, "UI_Chat").find("Luis: hola desde el cliente") != std::string::npos &&
                  text_of(guest, "UI_Chat").find("Luis: hola desde el cliente") != std::string::npos,
              "chat: el mensaje del cliente llega a todos con su nombre");

        // El cliente mueve su jugador: el servidor lo ve moverse.
        const std::string mine = "local mio; for _, j in ipairs(Network.objects()) do if j.name == 'Jugador' and j:isMine() then mio = j end end\n";
        lua(guest, mine + "mio.position = Vec3(10, 1.2, 10)");
        frames(40);
        check(lua(host, "local e = Network.find(" + lua(guest, mine + "return tostring(mio.netId)") + "); return tostring(math.floor(e.position.x + 0.5))") == "10",
              "el jugador del cliente se mueve tambien en el servidor");

        // Monedas: el servidor da el punto al que pasa cerca.
        lua(host, "local m = R.monedasVivas[1]; local j = R.jugadores[2].avatar; m.position = j.position");
        frames(20);
        check(text_of(guest, "UI_Marcador").find("Luis  1 pts") != std::string::npos, "recoger una moneda: 1 punto (lo decide el servidor)");

        // Patear el balon (F en el cliente -> el servidor aplica el golpe).
        lua(host, "local b = R.objetos[1].entidad; local j = R.jugadores[2].avatar; b.position = j.position + Vec3(1.2, 0, 0); b.velocity = Vec3.zero");
        frames(10);
        const std::string before = lua(host, "return tostring(R.objetos[1].entidad.position.x)");
        lua(guest, "Network.send('patear', {}, 'server')");
        frames(40);
        const std::string after = lua(host, "return tostring(R.objetos[1].entidad.position.x)");
        check(std::atof(after.c_str()) > std::atof(before.c_str()) + 0.5f, "patear: el balon sale disparado en el servidor");
        check(lua(guest, "local b = Scene.find('Balon'); return tostring(b.position.x > " + before + ")") == "true",
              "y el cliente lo ve moverse");

        // Empujar el balon caminando (sin F): el jugador del cliente lo mueve
        // tambien en el servidor, no solo el del servidor.
        lua(host, "local b = R.objetos[1].entidad; local j = R.jugadores[2].avatar; b.position = j.position + Vec3(2.2, -0.6, 0); "
                  "b.velocity = Vec3.zero; b.angularVelocity = Vec3.zero");
        frames(40);
        const std::string rest = lua(host, "return tostring(R.objetos[1].entidad.position.x)");
        lua(guest, mine + "mio:getScript().Update = function() end");
        for (int i = 0; i < 70; ++i) {
            lua(guest, mine + "local v = mio.velocity; mio.velocity = Vec3(5, v.y, 0)");
            frames(1);
        }
        frames(20);
        const std::string pushed = lua(host, "return tostring(R.objetos[1].entidad.position.x)");
        std::printf("  balon empujado: %s -> %s\n", rest.c_str(), pushed.c_str());
        check(std::atof(pushed.c_str()) > std::atof(rest.c_str()) + 1.5f, "empujar: el jugador del cliente mueve el balon en el servidor");
        check(lua(guest, "local b = Scene.find('Balon'); return tostring(b.position.x > " + rest + " + 1.0)") == "true",
              "y en el cliente el balon tambien avanza");
        // Esperar a que el balon se pare de verdad en el servidor (rueda y
        // rebota unos segundos; en marcha el cliente va un poco por detras).
        for (int k = 0; k < 40; ++k) {
            frames(30);
            if (lua(host, "return tostring(R.objetos[1].entidad.velocity:length() < 0.05)") == "true") break;
        }
        frames(60);
        const std::string server_ball = lua(host, "local p = R.objetos[1].entidad.position; return string.format('Vec3(%f,%f,%f)', p.x, p.y, p.z)");
        const std::string gap = lua(guest, "return tostring((Scene.find('Balon').position - " + server_ball + "):length())");
        std::printf("  diferencia cliente/servidor en reposo: %s m\n", gap.c_str());
        check(std::atof(gap.c_str()) < 0.25f, "al pararse, el balon del cliente coincide con el del servidor");

        // Gol: +5 al ultimo que lo toco (al empujar pudo coger alguna moneda:
        // se cuenta desde lo que tenia).
        const int points = std::atoi(lua(host, "return tostring(R.jugadores[2].puntos)").c_str());
        lua(host, "local b = R.objetos[1]; b.ultimo = 2; b.entidad.position = Vec3(27.5, 1, 0)");
        frames(20);
        check(text_of(guest, "UI_Aviso").find("GOL de Luis") != std::string::npos, "gol: aviso a todos");
        check(text_of(guest, "UI_Marcador").find("Luis  " + std::to_string(points + 5) + " pts") != std::string::npos,
              "y +5 puntos");

        // Evento: lluvia de monedas.
        lua(host, "R.eventoTiempo = 0; math.randomseed(1)");
        frames(5);
        check(!text_of(guest, "UI_Aviso").empty(), "un evento se anuncia en todos");

        // El cliente sale: su jugador desaparece en el servidor.
        lua(guest, "R:OnSalir()");
        frames(40);
        check(guest.world.findByName("UI_Menu").activeSelf() && count(guest, "Jugador") == 0, "salir: el cliente vuelve al menu sin objetos de red");
        check(count(host, "Jugador") == 1 && text_of(host, "UI_Chat").find("Luis ha salido") != std::string::npos,
              "el servidor quita su jugador y lo anuncia");
        check(host.errors.empty() && guest.errors.empty(), "los scripts se ejecutan sin errores");
        host.scripts.shutdownNetwork();
        guest.scripts.shutdownNetwork();
        host.scripts.stop();
        guest.scripts.stop();
        host.physics.stop();
        guest.physics.stop();
    }

    // --- Plataformas 2D: tilemap, sprites y fisica 2D ---
    {
        std::printf("Plataformas 2D\n");
        const project::ProjectInfo p = editor::createProjectFromTemplate(*find(list, "platformer_2d"), root, "Plataformas2D");
        check(std::filesystem::exists(p.assetsFolder() / "2D" / "Terreno.crtileset") &&
                  std::filesystem::exists(p.assetsFolder() / "2D" / "jugador.png"),
              "escribe el tileset y los sprites");
        Played r;
        float y_15 = 0.0f;
        float y_25 = 0.0f;
        play(p, 2.6f, r, [&](ecs::World& w, navigation::NavigationSystem&, float t) {
            const float y = w.findByName("Jugador").worldPosition().y;
            if (t < 1.5f) y_15 = y;
            if (t < 2.5f) y_25 = y;
        });
        std::printf("  (jugador en y = %.2f a 1.5 s y %.2f a 2.5 s)\n", static_cast<double>(y_15), static_cast<double>(y_25));
        const ecs::Entity cam = r.world.findByName("Main Camera");
        check(r.loaded, "la escena inicial se abre");
        check(r.script_errors == 0, "los scripts se ejecutan sin errores");
        check(y_25 < 1.5f && y_25 > -3.0f && std::abs(y_25 - y_15) < 0.05f,
              "el jugador cae y se queda sobre el tilemap (Tilemap Collider 2D)");
        check(r.world.findAllWithTag("Moneda").size() == 7, "las 7 monedas tienen su tag");
        check(cam.valid() && cam.has<ecs::Camera>() && cam.get<ecs::Camera>().orthographic, "camara ortografica");
    }

    // --- Coches: Vehicle + Wheel Collider ---
    {
        std::printf("Coches\n");
        const project::ProjectInfo p = editor::createProjectFromTemplate(*find(list, "cars"), root, "Coches");
        Played r;
        int on_ground = 0;
        physics::PhysicsSystem::VehicleState last;
        float start_z = 0.0f;
        play(p, 5.0f, r, [&](ecs::World& w, navigation::NavigationSystem&, float t) {
            const ecs::Entity car = w.findByName("Coche");
            if (playing_physics == nullptr || !car.valid()) return;
            if (t < 1.0f) {
                on_ground = playing_physics->vehicleState(car).wheels_on_ground;
                start_z = car.worldPosition().z;
            } else {
                playing_physics->setVehicleInput(car, 1.0f, 0.0f, 0.0f, 0.0f);  // a fondo, recto
            }
            last = playing_physics->vehicleState(car);
        });
        const ecs::Entity car = r.world.findByName("Coche");
        const float moved = car.valid() ? std::abs(car.worldPosition().z - start_z) : 0.0f;
        std::printf("  (%d ruedas en el suelo; tras 4 s a fondo: %.1f km/h, marcha %d, %.1f m)\n", on_ground,
                    static_cast<double>(last.speed_kmh), last.gear, static_cast<double>(moved));
        check(r.loaded, "la escena inicial se abre");
        check(r.script_errors == 0, "los scripts se ejecutan sin errores");
        check(last.valid && last.wheel_count == 4 && on_ground == 4, "el coche se apoya en sus 4 Wheel Collider");
        check(last.speed_kmh > 20.0f && last.gear >= 1 && moved > 10.0f, "acelera y avanza con el motor y las marchas");
    }

    // --- Realidad virtual: Jugador VR, objetos que se cogen y panel en el mundo ---
    {
        std::printf("Realidad virtual\n");
        const project::ProjectInfo p = editor::createProjectFromTemplate(*find(list, "vr"), root, "VR");
        Played r;
        // Como en el Play del editor: la capsula del jugador no choca con lo
        // que se coge (XrRig::updateCollisions antes de cada paso).
        xr::XrRig rig_system;
        bool ignored = false;
        play(p, 2.0f, r, [&](ecs::World& w, navigation::NavigationSystem&, float) {
            if (playing_physics == nullptr) return;
            rig_system.setPhysics(playing_physics);
            rig_system.updateCollisions(w);
            const ecs::Entity player = w.findByName("Jugador VR");
            const ecs::Entity crate = w.findByName("Caja 0-0");
            ignored = player.valid() && crate.valid() && playing_physics->collisionIgnored(player, crate);
        });
        check(r.loaded, "la escena inicial se abre");
        check(ignored, "el Jugador VR no choca con lo que se coge (XR Grabbable)");
        const ecs::Entity rig = r.world.findByName("Jugador VR");
        const ecs::Entity left = r.world.findByName("Mano izquierda");
        const ecs::Entity right = r.world.findByName("Mano derecha");
        check(rig.valid() && rig.has<xr::XrOrigin>() && rig.has<physics::CharacterController>() && rig.has<xr::XrPlayer>() &&
                  left.valid() && left.has<xr::XrInteractor>() && left.get<xr::XrController>().hand == xr::Hand::Left &&
                  right.valid() && right.has<xr::XrInteractor>() && right.get<xr::XrController>().hand == xr::Hand::Right &&
                  right.get<xr::XrInteractor>().ray_material.valid(),
              "Jugador VR con capsula y dos manos con XR Interactor (y su rayo)");
        int grabbables = 0;
        bool all_bodies = true;
        r.world.forEachDepthFirst([&](ecs::Entity e) {
            if (!e.has<xr::XrGrabbable>()) return;
            ++grabbables;
            all_bodies = all_bodies && e.has<physics::Rigidbody>();
        });
        check(grabbables >= 10 && all_bodies, "objetos con XR Grabbable y Rigidbody");
        // Tras 2 s de fisica siguen en la mesa y la estanteria (no atraviesan nada).
        const ecs::Entity cube = r.world.findByName("Cubo rojo");
        const ecs::Entity ball = r.world.findByName("Pelota 1");
        std::printf("  (cubo a %.3f m, pelota a %.3f m)\n", cube.valid() ? static_cast<double>(cube.worldPosition().y) : -1.0,
                    ball.valid() ? static_cast<double>(ball.worldPosition().y) : -1.0);
        check(cube.valid() && std::abs(cube.worldPosition().y - 0.81f) < 0.03f && ball.valid() &&
                  std::abs(ball.worldPosition().y - 0.955f) < 0.05f,
              "los objetos se quedan en la mesa y la estanteria");
        const ecs::Entity panel = r.world.findByName("Panel VR");
        const scripting::CppScript* menu = panel.valid() ? panel.tryGet<scripting::CppScript>() : nullptr;
        check(menu != nullptr && menu->class_name == "MenuVR" && menu->value("objetos") != nullptr &&
                  std::filesystem::exists(p.assetsFolder() / "Scripts" / "MenuVR.h") &&
                  std::filesystem::exists(p.assetsFolder() / "Scripts" / "MenuVR.cpp"),
              "el panel lleva su script de C++ (MenuVR.h + MenuVR.cpp)");
        // El rayo de una mano apunta al boton y el gatillo lo pulsa.
        ui::UiSystem ui;
        std::array<ui::UiPointer, ui::UiSystem::kMaxPointers> pointers{};
        ui.updateWorld(r.world, pointers, true, 0.0f);
        const bool one_canvas = ui.worldCanvases().size() == 1 && !ui.worldCanvases().front().commands.empty();
        const ecs::Entity button = r.world.findByName("Reiniciar objetos");
        bool clicked = false;
        bool hovered = false;
        if (one_canvas && button.valid() && panel.valid()) {
            // Del centro del boton (pixeles del canvas) a un punto del mundo.
            const ui::WorldCanvasDraw& c = ui.worldCanvases().front();
            const float px = 40.0f + 170.0f;
            const float py = 236.0f + 40.0f;
            Vec3 center{c.transform.m[3][0], c.transform.m[3][1], c.transform.m[3][2]};
            const Vec3 x_axis{c.transform.m[0][0], c.transform.m[0][1], c.transform.m[0][2]};
            const Vec3 y_axis{c.transform.m[1][0], c.transform.m[1][1], c.transform.m[1][2]};
            const Vec3 z_axis{c.transform.m[2][0], c.transform.m[2][1], c.transform.m[2][2]};
            const Vec3 target = center + x_axis * ((px / c.width - 0.5f) * c.size.x) + y_axis * ((0.5f - py / c.height) * c.size.y);
            pointers[1].valid = true;
            pointers[1].origin = target + z_axis * 1.0f;  // un metro delante del panel
            pointers[1].direction = z_axis * -1.0f;
            ui.updateWorld(r.world, pointers, true, 0.1f);
            hovered = ui.pointerHits()[1].hit && ui.pointerHits()[1].over_control &&
                      std::abs(ui.pointerHits()[1].distance - 1.0f) < 0.01f;
            pointers[1].down = true;
            ui.updateWorld(r.world, pointers, true, 0.2f);
            pointers[1].down = false;
            ui.updateWorld(r.world, pointers, true, 0.3f);
            for (const ui::UiEvent& e : ui.takeEvents()) {
                clicked = clicked || (e.method == "OnReiniciar" && e.target == panel);
            }
        }
        check(one_canvas, "un Canvas en el mundo con su lista de dibujo");
        check(hovered && clicked, "el rayo apunta al boton y el gatillo lo pulsa (OnReiniciar al script)");
    }

    // --- Del usuario ---
    {
        std::printf("Plantillas del usuario\n");
        const auto source = project::openProject(root / "Plataformas");
        check(source.has_value() && editor::saveProjectAsTemplate(*source, "Mi plataformas", "Prueba"),
              "guardar un proyecto como plantilla");
        const std::vector<editor::ProjectTemplate> again = editor::availableTemplates();
        const editor::ProjectTemplate* mine = find(again, "user:Mi plataformas");
        check(again.size() == 14 && mine != nullptr && mine->category == "Mis plantillas" && mine->description == "Prueba",
              "aparece en la lista con su descripcion");
        if (mine != nullptr) {
            const project::ProjectInfo p = editor::createProjectFromTemplate(*mine, root, "Copia");
            Played r;
            play(p, 1.0f, r);
            check(r.loaded && r.script_errors == 0 && r.world.findByName("Jugador").valid() &&
                      std::filesystem::exists(p.assetsFolder() / "Scripts" / "Jugador.lua"),
                  "un proyecto nuevo desde ella tiene todo (escena inicial, scripts)");
        }
        bool threw = false;
        try {
            editor::createProjectFromTemplate(*find(again, "blank"), root, "Copia");
        } catch (const std::exception&) {
            threw = true;
        }
        check(threw && std::filesystem::exists(root / "Copia" / "Assets" / "Scripts" / "Jugador.lua"),
              "no pisa un proyecto que ya existe");
    }

    std::filesystem::remove_all(root, ec);
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
