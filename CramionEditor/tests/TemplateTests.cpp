// Pruebas de las plantillas de proyecto (consola, sin GPU): cada plantilla
// integrada crea un proyecto que se abre, su escena inicial se carga y se
// juega un rato (fisica, navegacion y los scripts de Lua sin errores). Y las
// plantillas del usuario: guardar un proyecto como plantilla y crear otro
// desde ella. Devuelve 0 si todo va.

#include "ProjectTemplates.h"
#include "CreatureModels.h"

#include <CramionCore/anim/Creature.h>
#include <CramionCore/anim/Humanoid.h>
#include <CramionCore/ecs/Rigging.h>

#include <CramionCore/CramionCore.h>
#include <CramionDM/Input.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    navigation::NavigationSystem nav;
    nav.setPhysics(&physics);
    scripting::ScriptSystem scripts;
    scripts.setAssetsRoot(info->assetsFolder());
    scripts.setPhysics(&physics);
    scripts.setNavigation(&nav);
    scripts.setLog([&](int level, const std::string& message) {
        if (level == 2) std::printf("  [Lua] %s\n", message.c_str());
    });
    physics.start(out.world);
    nav.waitForBuild(out.world);
    scripts.start(out.world);
    const float dt = 1.0f / 60.0f;
    for (float t = 0.0f; t < seconds; t += dt) {
        const int steps = physics.update(out.world, dt, true);
        nav.update(out.world, dt, true);
        scripts.fixedUpdate(out.world, dt, steps);
        scripts.update(out.world, dt);
        if (each_frame) each_frame(out.world, nav, t);
    }
    out.script_errors = static_cast<int>(scripts.errors().size());
    scripts.stop();
    physics.stop();
}

std::string hudText(ecs::World& world) {
    const ecs::Entity e = world.findByName("Marcador");
    return e.valid() && e.has<ui::Text>() ? e.get<ui::Text>().text : std::string();
}

}  // namespace

int main() {
    physics::registerPhysicsComponents();
    terrain::registerTerrainComponents();
    water::registerWaterComponents();
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
    check(list.size() == 6 && find(list, "blank") && find(list, "third_person") && find(list, "navigation") &&
              find(list, "voxel") && find(list, "mmo") && find(list, "creatures"),
          "hay 6 plantillas integradas");

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

    // --- Del usuario ---
    {
        std::printf("Plantillas del usuario\n");
        const auto source = project::openProject(root / "Plataformas");
        check(source.has_value() && editor::saveProjectAsTemplate(*source, "Mi plataformas", "Prueba"),
              "guardar un proyecto como plantilla");
        const std::vector<editor::ProjectTemplate> again = editor::availableTemplates();
        const editor::ProjectTemplate* mine = find(again, "user:Mi plataformas");
        check(again.size() == 7 && mine != nullptr && mine->category == "Mis plantillas" && mine->description == "Prueba",
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
