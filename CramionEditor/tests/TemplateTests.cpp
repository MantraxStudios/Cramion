// Pruebas de las plantillas de proyecto (consola, sin GPU): cada plantilla
// integrada crea un proyecto que se abre, su escena inicial se carga y se
// juega un rato (fisica, navegacion y los scripts de Lua sin errores). Y las
// plantillas del usuario: guardar un proyecto como plantilla y crear otro
// desde ella. Devuelve 0 si todo va.

#include "ProjectTemplates.h"

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
    check(list.size() == 4 && find(list, "blank") && find(list, "third_person") && find(list, "navigation") &&
              find(list, "voxel"),
          "hay 4 plantillas integradas");

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

    // --- Del usuario ---
    {
        std::printf("Plantillas del usuario\n");
        const auto source = project::openProject(root / "Plataformas");
        check(source.has_value() && editor::saveProjectAsTemplate(*source, "Mi plataformas", "Prueba"),
              "guardar un proyecto como plantilla");
        const std::vector<editor::ProjectTemplate> again = editor::availableTemplates();
        const editor::ProjectTemplate* mine = find(again, "user:Mi plataformas");
        check(again.size() == 5 && mine != nullptr && mine->category == "Mis plantillas" && mine->description == "Prueba",
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
