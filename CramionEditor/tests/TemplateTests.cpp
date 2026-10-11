// Pruebas de las plantillas de proyecto (consola, sin GPU): cada plantilla
// integrada crea un proyecto que se abre, su escena inicial se carga, sus
// scripts de C++ se compilan (como el Play del editor) y se juega un rato
// (fisica, navegacion y los scripts sin errores). Lo que hacen los scripts se
// mira desde fuera: la escena, la interfaz y la consola de la API. Y las
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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

using namespace cramion;
using core::Vec3;

namespace {

int failures = 0;

// Las partes que se prueban: todas, o las que se pasen como argumentos
// (CramionTemplateTests estados mmo).
std::vector<std::string> only;
bool wanted(const char* part) { return only.empty() || std::find(only.begin(), only.end(), part) != only.end(); }
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

const editor::ProjectTemplate* find(const std::vector<editor::ProjectTemplate>& list, const std::string& id) {
    for (const auto& t : list) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

std::filesystem::path scriptHost() {
#if defined(_WIN32)
    return std::filesystem::path(CRAMION_BIN_DIR) / "CramionScriptHost.exe";
#else
    return std::filesystem::path(CRAMION_BIN_DIR) / "CramionScriptHost";
#endif
}

// Un proyecto en Play, como en el editor: fisica, 2D, navegacion, bloques,
// la API (ScriptSystem) y sus scripts de C++ compilados (CppScriptSystem).
struct Game {
    std::optional<project::ProjectInfo> info;
    ecs::World world;
    physics::PhysicsSystem physics;
    navigation::NavigationSystem nav;
    twod::System2D twod;
    voxel::VoxelSystem voxels;
    terrain::TerrainStore terrains;
    dm::Input input;
    scripting::ScriptSystem scripts;
    scripting::CppScriptSystem cpp;
    bool loaded = false;
    bool compiled = false;
    bool running = false;
    bool use_voxels = false;
    bool cursor_locked = false;
    std::filesystem::path save_root;  // mundos de bloques (con use_voxels)
    std::filesystem::path prefs;      // Prefs.* en un archivo (vacio: en memoria)
    std::vector<std::string> logged_errors;
    std::function<void(Game&, float)> each_frame;
    float time = 0.0f;

    // Abre el proyecto y carga su escena inicial.
    bool open(const project::ProjectInfo& created) {
        info = project::openProject(created.file);
        if (!info) return false;
        std::string error;
        loaded = ecs::loadScene(world, info->assetsFolder() / "Scenes" / "Main.crscene", &error) &&
                 world.sceneUuid() == info->startup_scene;
        if (!loaded) std::printf("  (%s)\n", error.c_str());
        return loaded;
    }

    // Compila los scripts de C++ (si hay) y empieza el Play.
    void start() {
        const std::filesystem::path assets = info->assetsFolder();
        terrains.setRoot(assets);
        physics.setTerrainProvider([this](ecs::Entity entity) -> std::shared_ptr<const terrain::TerrainData> {
            const terrain::Terrain* comp = entity.tryGet<terrain::Terrain>();
            return comp != nullptr ? terrains.get(*comp) : nullptr;
        });
        nav.setPhysics(&physics);
        scripts.setAssetsRoot(assets);
        scripts.setPhysics(&physics);
        scripts.setNavigation(&nav);
        scripts.setInput(&input);
        scripts.setCursorLock([this](bool on) { cursor_locked = on; });
        if (!prefs.empty()) scripts.setPrefsFile(prefs);
        scripts.setLog([this](int level, const std::string& message) {
            if (level != 2) return;
            logged_errors.push_back(message);
            std::printf("  [error] %s\n", message.c_str());
        });
        if (use_voxels) {
            voxels.setPhysics(&physics);
            voxels.setAssetsRoot(assets);
            voxels.setSaveRoot(save_root);
            scripts.setVoxels(&voxels);
        }
        twod.setAssetsRoot(assets);

        static bool toolchain = false;
        if (!toolchain) {
            scripting::CppScriptSystem::setToolchainRoot(std::filesystem::path(CRAMION_BIN_DIR) / "toolchain");
            toolchain = true;
        }
        cpp.setAssetsRoot(assets);
        cpp.setBuildFolder(info->folder / "Library" / "CppScripts");
        cpp.setSdkFolder(CRAMION_SDK_DIR);
        cpp.setHostExecutable(scriptHost());
        cpp.setPhysics(&physics);
        cpp.setInput(&input);
        cpp.setScriptSystem(&scripts);
        compiled = !cpp.hasSources();
        if (cpp.hasSources()) {
            const scripting::CppCompileResult built = cpp.compile();
            compiled = built.ok;
            if (built.ok) cpp.usePrebuilt(built.dll);
            std::printf("  (scripts de C++ compilados en %.1f s: %s)\n", built.seconds, built.ok ? "bien" : "con errores");
            for (const scripting::ScriptError& e : built.errors) {
                std::printf("  [compilar] %s:%d %s\n", e.file.c_str(), e.line, e.message.c_str());
            }
            if (!built.ok && built.errors.empty()) std::printf("%s\n", built.log.substr(0, 3000).c_str());
        }

        physics.start(world);
        twod.start(world);
        if (use_voxels) voxels.start(world);
        nav.waitForBuild(world);
        scripts.start(world);
        if (compiled && cpp.hasSources()) cpp.start(world);
        running = true;
    }

    void frame(float dt = 1.0f / 60.0f) {
        if (use_voxels) {
            const ecs::Entity camera = world.findByName("Main Camera");
            voxels.update(dt, camera.valid() ? camera.worldPosition() : Vec3{});
        }
        const int steps = physics.update(world, dt, true);
        twod.update(world, dt, twod::System2D::Mode::Play);
        nav.update(world, dt, true);
        scripts.fixedUpdate(world, dt, steps);
        cpp.fixedUpdate(world, dt, steps);
        scripts.update(world, dt);
        cpp.update(world, dt);
        if (each_frame) each_frame(*this, time);
        time += dt;
        input.newFrame();
    }
    void frames(int n) {
        for (int i = 0; i < n; ++i) frame();
    }

    void key(dm::Key k, bool down) {
        dm::Event e;
        e.type = down ? dm::EventType::KeyPressed : dm::EventType::KeyReleased;
        e.key = k;
        input.onEvent(e);
    }
    // Pulsar y soltar (un frame cada cosa).
    void press(dm::Key k) {
        key(k, true);
        frame();
        key(k, false);
        frame();
    }
    void mouse(dm::MouseButton button, bool down) {
        dm::Event e;
        e.type = down ? dm::EventType::MouseButtonPressed : dm::EventType::MouseButtonReleased;
        e.button = button;
        input.onEvent(e);
    }
    void click(dm::MouseButton button) {
        mouse(button, true);
        frame();
        mouse(button, false);
        frame();
    }

    // Lo que hace un boton de la interfaz: el mensaje `method` al script de
    // C++ de `target` con el control que lo pulso.
    void button(const std::string& target, const std::string& method, ecs::Entity source) {
        cpp.sendMessage(world.findByName(target), method, cpp.entityJson(source));
    }
    // Lo mismo con un valor (texto, numero...) en JSON.
    void message(const std::string& target, const std::string& method, const std::string& value_json) {
        cpp.sendMessage(world.findByName(target), method, value_json);
    }

    // La consola de la API (Scene.find('X').position = Vec3(...)).
    std::string run(const std::string& code) {
        std::string out;
        if (!scripts.run(code, &out)) std::printf("  (consola: %s -> %s)\n", code.c_str(), out.c_str());
        return out;
    }
    void moveTo(const std::string& name, const Vec3& p) {
        char code[256];
        std::snprintf(code, sizeof(code), "local e = Scene.find('%s'); e.position = Vec3(%f, %f, %f); e.velocity = Vec3()",
                      name.c_str(), static_cast<double>(p.x), static_cast<double>(p.y), static_cast<double>(p.z));
        run(code);
    }
    Vec3 position(const std::string& name) {
        const ecs::Entity e = world.findByName(name);
        return e.valid() ? e.worldPosition() : Vec3{};
    }

    std::string text(const std::string& name) {
        const ecs::Entity e = world.findByName(name);
        return e.valid() && e.has<ui::Text>() ? e.get<ui::Text>().text : std::string();
    }
    // El texto de un hijo (los paneles tienen varios "Nombre", "Texto"...).
    std::string childText(const std::string& parent, const std::string& child) {
        const ecs::Entity p = world.findByName(parent);
        if (!p.valid()) return {};
        std::string found;
        world.forEachDepthFirst([&](ecs::Entity e) {
            if (found.empty() && e.name() == child && e.has<ui::Text>() && isUnder(e, p)) found = e.get<ui::Text>().text;
        });
        return found;
    }
    ecs::Entity child(const std::string& parent, const std::string& name) {
        const ecs::Entity p = world.findByName(parent);
        ecs::Entity found;
        if (!p.valid()) return found;
        world.forEachDepthFirst([&](ecs::Entity e) {
            if (!found.valid() && e.name() == name && isUnder(e, p)) found = e;
        });
        return found;
    }
    static bool isUnder(ecs::Entity e, ecs::Entity parent) {
        for (ecs::Entity p = e.parent(); p.valid(); p = p.parent()) {
            if (p == parent) return true;
        }
        return false;
    }
    bool active(const std::string& name) {
        const ecs::Entity e = world.findByName(name);
        return e.valid() && e.activeSelf();
    }

    int errors() {
        for (const scripting::ScriptError& e : cpp.errors()) std::printf("  [C++] %s:%d %s\n", e.file.c_str(), e.line, e.message.c_str());
        for (const scripting::ScriptError& e : scripts.errors()) std::printf("  [API] %s:%d %s\n", e.file.c_str(), e.line, e.message.c_str());
        return static_cast<int>(cpp.errors().size() + scripts.errors().size()) + (compiled ? 0 : 1);
    }

    void stop() {
        if (!running) return;
        running = false;
        cpp.stop();
        scripts.stop();
        if (use_voxels) voxels.stop();
        twod.stop();
        physics.stop();
    }
    ~Game() { stop(); }
};

struct Played {
    bool loaded = false;
    int script_errors = 0;
    std::unique_ptr<Game> game;  // la partida ya parada (para mirar su escena)
    ecs::World& world() { return game->world; }
};

// La fisica 3D de la partida que se esta jugando (para conducir el coche
// desde `each_frame`).
physics::PhysicsSystem* playing_physics = nullptr;

// Abre el proyecto, carga su escena inicial y la juega `seconds`.
void play(const project::ProjectInfo& created, float seconds, Played& out,
          const std::function<void(ecs::World&, navigation::NavigationSystem&, float)>& each_frame = {}) {
    auto g = std::make_unique<Game>();
    if (!g->open(created)) {
        out.game = std::move(g);
        return;
    }
    out.loaded = true;
    g->start();
    playing_physics = &g->physics;
    if (each_frame) g->each_frame = [&](Game& game, float t) { each_frame(game.world, game.nav, t); };
    for (float t = 0.0f; t < seconds; t += 1.0f / 60.0f) g->frame();
    out.script_errors = g->errors();
    playing_physics = nullptr;
    g->stop();
    // La escena tal como quedo (para mirarla despues).
    out.game = std::move(g);
}

std::string hudText(ecs::World& world) {
    const ecs::Entity e = world.findByName("Marcador");
    return e.valid() && e.has<ui::Text>() ? e.get<ui::Text>().text : std::string();
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) only.emplace_back(argv[i]);
    // Sin bufer: si algo aborta, lo ultimo que se escribio queda a la vista.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
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

    std::string kind;
    scripting::CppScriptSystem::setToolchainRoot(std::filesystem::path(CRAMION_BIN_DIR) / "toolchain");
    if (scripting::CppScriptSystem::findCompiler(&kind).empty()) {
        std::printf("Sin compilador de C++: los scripts de las plantillas no se pueden probar\n");
        return 1;
    }
    std::printf("Compilador de los scripts: %s\n", kind.c_str());

    const std::vector<editor::ProjectTemplate> list = editor::availableTemplates();
    std::printf("Plantillas integradas\n");
    check(list.size() == 13 && find(list, "blank") && find(list, "third_person") && find(list, "navigation") &&
              find(list, "third_person_pro") &&
              find(list, "voxel") && find(list, "mmo") && find(list, "creatures") && find(list, "online") &&
              find(list, "open_world") && find(list, "state_machines") && find(list, "platformer_2d") &&
              find(list, "cars") && find(list, "vr"),
          "hay 13 plantillas integradas");

    // Ninguna plantilla escribe Lua: todo son scripts de C++ con su clase.
    {
        bool no_lua = true;
        bool classes_ok = true;
        for (const editor::ProjectTemplate& t : list) {
            if (t.id == "third_person_pro" || t.id == "open_world") continue;  // necesitan el pack / tardan: se miran abajo
            const project::ProjectInfo p = editor::createProjectFromTemplate(t, root, "Lua_" + t.id);
            for (const auto& f : std::filesystem::recursive_directory_iterator(p.assetsFolder(), ec)) {
                if (f.path().extension() == ".lua") {
                    no_lua = false;
                    std::printf("  (%s: %s)\n", t.id.c_str(), f.path().filename().string().c_str());
                }
            }
            ecs::World w;
            std::string error;
            if (!ecs::loadScene(w, p.assetsFolder() / "Scenes" / "Main.crscene", &error)) continue;
            w.forEachDepthFirst([&](ecs::Entity e) {
                if (e.has<scripting::Script>()) no_lua = false;
                const scripting::CppScript* s = e.tryGet<scripting::CppScript>();
                if (s == nullptr) return;
                const bool ok = std::filesystem::exists(p.assetsFolder() / s->script) &&
                                std::filesystem::path(s->script).stem().string() == s->class_name;
                if (!ok) std::printf("  (%s: %s -> %s)\n", t.id.c_str(), e.name().c_str(), s->script.c_str());
                classes_ok = classes_ok && ok;
            });
        }
        check(no_lua, "ninguna plantilla escribe scripts de Lua ni usa el componente Script");
        check(classes_ok, "cada CppScript apunta a su .cpp y a su clase");
    }

    // --- Criaturas: modelos con esqueleto generados, IK, phys bones, ragdoll ---
    if (wanted("criaturas")) {
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
        const ecs::Entity dog_entity = r.world().findByName("Perro");
        const ecs::MeshRenderer* mr = dog_entity.valid() ? dog_entity.tryGet<ecs::MeshRenderer>() : nullptr;
        std::shared_ptr<const assets::ModelAsset> loaded = mr != nullptr ? manager.loadModel(mr->model.uuid) : nullptr;
        check(loaded && loaded->animated && !loaded->parts.empty() && !loaded->parts[0]->bones.empty(),
              "el perro carga como modelo animado con esqueleto");
        check(r.loaded && r.script_errors == 0, "la escena se abre y los scripts corren sin errores");
        check(dog_entity.valid() && dog_entity.has<ecs::InverseKinematics>() && dog_entity.has<ecs::PhysBones>() &&
                  dog_entity.has<ecs::Ragdoll>() && dog_entity.has<ecs::Skeleton>(),
              "el perro lleva IK, Phys Bones, Ragdoll y Esqueleto");
        check(r.world().findByName("Sombrero").valid() && r.world().findByName("Sombrero").has<ecs::BoneSocket>(),
              "el sombrero va enganchado a la cabeza (Bone Socket)");
        std::printf("    el perro anduvo %.2f m\n", moved);
        check(moved > 2.5f, "el perro recorre el circuito");
    }

    // --- Vacia ---
    if (wanted("vacia")) {
        const project::ProjectInfo p = editor::createProjectFromTemplate(*find(list, "blank"), root, "Vacio");
        Played r;
        play(p, 0.5f, r);
        check(r.loaded && r.world().findByName("Suelo").valid() && r.world().findByName("Main Camera").valid(),
              "Vacia: escena inicial con suelo, camara y luz");
    }

    // --- Tercera persona avanzada (el Locomotion Pack de Mixamo, si esta en Descargas) ---
    if (const editor::ProjectTemplate* pro = find(list, "third_person_pro"); wanted("avanzada") && pro != nullptr && !pro->folder.empty()) {
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

        Game g;
        const bool loaded = g.open(p);
        check(loaded, "la escena se abre");
        if (loaded) {
            const ecs::Entity model = g.world.findByName("Modelo");
            check(model.valid() && model.has<ecs::Animator>() && model.has<ecs::InverseKinematics>() &&
                      model.get<ecs::InverseKinematics>().foot_grounding,
                  "el modelo lleva Animator (el controlador) e IK con los pies al suelo");
            g.start();
            check(g.compiled, "los scripts de C++ compilan");
            const ecs::Entity player = g.world.findByName("Jugador");
            g.frames(60);
            const float rest_y = player.worldPosition().y;
            check(std::abs(rest_y - 0.9f) < 0.08f, "el personaje se apoya en el suelo");

            // Andar y correr hacia delante (la camara mira a -Z).
            Vec3 a = player.worldPosition();
            g.key(dm::Key::W, true);
            g.frames(120);
            Vec3 b = player.worldPosition();
            const float walk = (a.z - b.z) / 2.0f;
            g.key(dm::Key::LeftShift, true);
            g.frames(90);
            a = player.worldPosition();
            g.frames(60);
            b = player.worldPosition();
            const float run = a.z - b.z;
            g.key(dm::Key::LeftShift, false);
            g.key(dm::Key::W, false);
            const float y_param = model.get<ecs::Animator>().runtime.values.count("Y") ? model.get<ecs::Animator>().runtime.values.at("Y") : 0.0f;
            std::printf("    anda a %.2f m/s, corre a %.2f m/s (Animator Y = %.2f)\n", walk, run, y_param);
            check(walk > 1.0f && run > 3.5f && run > walk * 2.0f, "anda y corre (Shift) a la velocidad de sus animaciones");
            check(y_param > 3.0f, "el Animator recibe la velocidad hacia delante (Y)");
            const ecs::Entity energy = g.world.findByName("Energia");
            check(energy.valid() && energy.get<ui::Slider>().value < 1.0f, "correr gasta energia (la barra baja)");
            g.frames(60);

            // Escalera: sube andando (sin saltar) hasta el rellano.
            g.moveTo("Jugador", Vec3{-8.0f, 0.95f, 6.5f});
            g.frames(20);
            g.key(dm::Key::W, true);
            float highest = 0.0f;
            for (int i = 0; i < 480; ++i) {
                g.frame();
                highest = std::max(highest, player.worldPosition().y);
                if (player.worldPosition().z < -1.0f) break;
            }
            g.key(dm::Key::W, false);
            std::printf("    en la escalera llega a y = %.2f (rellano a %.2f)\n", highest, 0.18 * 8 + 0.9);
            check(highest > 0.18f * 8.0f + 0.9f - 0.1f, "sube la escalera andando (escalones de 18 cm)");
            g.frames(30);

            // Salto: sube lo que dice alturaSalto, y aterriza.
            g.moveTo("Jugador", Vec3{0.0f, 0.95f, 12.0f});
            g.frames(30);
            const float ground = player.worldPosition().y;
            g.press(dm::Key::Space);
            float peak = ground;
            for (int i = 0; i < 150; ++i) {
                g.frame();
                peak = std::max(peak, player.worldPosition().y);
            }
            std::printf("    salto: %.2f m\n", peak - ground);
            check(peak - ground > 0.8f && peak - ground < 1.4f, "salta (despega en el fotograma de la animacion)");
            check(std::abs(player.worldPosition().y - ground) < 0.08f, "y aterriza");

            // Palanca: E cerca, la mano la agarra y la compuerta sube.
            const ecs::Entity lever = g.world.findByName("Palanca 1");
            const Vec3 lever_front = lever.worldPosition() + core::normalize(lever.forward()) * 1.0f + Vec3{0.0f, 0.95f, 0.0f};
            g.moveTo("Jugador", lever_front);
            g.frames(10);
            const float gate_y = g.position("Compuerta 1").y;
            g.press(dm::Key::E);
            g.frames(40);
            const bool reaching = model.get<ecs::InverseKinematics>().right_hand.weight > 0.9f;
            g.frames(160);
            const float gate_up = g.position("Compuerta 1").y - gate_y;
            check(reaching, "la mano derecha va a la palanca (IK)");
            check(gate_up > 2.5f, "la palanca abre la compuerta");

            // Cristales: recoger uno (el del rellano ya se recogio al subir la escalera).
            const std::string before = hudText(g.world);
            g.moveTo("Jugador", g.position("Cristal 4"));
            g.frames(5);
            const std::string after = hudText(g.world);
            std::printf("    marcador \"%s\" -> \"%s\"\n", before.c_str(), after.c_str());
            check(before != after && contains(after, " / 7"), "recoge un cristal y el marcador lo cuenta");
            check(g.errors() == 0, "los scripts corren sin errores");
        }
    } else {
        std::printf("Tercera persona avanzada: sin el Locomotion Pack en Descargas (no se prueba)\n");
    }

    // --- Tercera persona ---
    if (wanted("tercera")) {
        std::printf("Tercera persona\n");
        const project::ProjectInfo p = editor::createProjectFromTemplate(*find(list, "third_person"), root, "Plataformas");
        int materials = 0;
        for (const auto& f : std::filesystem::directory_iterator(p.assetsFolder() / "Materials")) {
            assets::MaterialAsset m;
            if (assets::loadMaterial(f.path(), m)) ++materials;
        }
        check(materials >= 6, "crea sus materiales (.crmat)");
        Game g;
        check(g.open(p), "la escena inicial se abre");
        g.start();
        g.frames(180);
        const ecs::Entity player = g.world.findByName("Jugador");
        std::printf("  (jugador en y = %.2f, HUD \"%s\")\n", static_cast<double>(player.worldPosition().y), hudText(g.world).c_str());
        check(player.valid() && std::abs(player.worldPosition().y - 0.95f) < 0.3f, "el jugador se apoya en el suelo");
        check(hudText(g.world) == "Monedas  0 / 8", "el marcador cuenta las 8 monedas");
        check(g.world.findAllWithTag("Moneda").size() == 8, "las monedas tienen su tag");
        // Andar con W: la camara mira a -Z.
        const Vec3 a = player.worldPosition();
        g.key(dm::Key::W, true);
        g.frames(60);
        g.key(dm::Key::W, false);
        check(core::length(player.worldPosition() - a) > 2.0f, "WASD mueve al jugador");
        // Una moneda: el jugador pasa por encima.
        g.moveTo("Jugador", g.position("Moneda 1"));
        g.frames(10);
        check(hudText(g.world) == "Monedas  1 / 8", "recoger una moneda la cuenta");
        check(g.errors() == 0, "los scripts se ejecutan sin errores");
    }

    // --- IA y navegacion ---
    if (wanted("navegacion")) {
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
        std::printf("  (un guardia se movio %.1f m; HUD \"%s\")\n", static_cast<double>(moved), hudText(r.world()).c_str());
        check(r.loaded && nav_ready, "la escena genera su malla de navegacion");
        check(r.script_errors == 0, "los scripts se ejecutan sin errores");
        check(moved > 2.0f, "los guardias patrullan por la malla");
        check(hudText(r.world()).find("Atrapado: 0") != std::string::npos, "el HUD empieza a cero");
        navigation::NavigationSettings s;
        check(navigation::loadNavigationSettings(p.settingsFolder() / "Navigation.json", s), "guarda Navigation.json");
    }

    // --- IA con maquinas de estados ---
    if (wanted("estados")) {
        std::printf("IA con maquinas de estados\n");
        const project::ProjectInfo p = editor::createProjectFromTemplate(*find(list, "state_machines"), root, "Estados");
        Game g;
        check(g.open(p), "la escena se abre");
        g.start();
        const ecs::Entity enemy = g.world.findByName("Enemigo 1");
        const Vec3 start = enemy.worldPosition();
        g.frames(240);
        std::printf("  (HUD \"%s\")\n", hudText(g.world).c_str());
        check(core::length(enemy.worldPosition() - start) > 2.0f, "Patrullar: el script mueve al enemigo por la ruta");
        check(contains(hudText(g.world), "Patrullar"), "el HUD muestra el estado de cada enemigo");
        // El jugador se acerca: lo ve y lo persigue.
        g.moveTo("Jugador", enemy.worldPosition() + Vec3{4.0f, 0.2f, 0.0f});
        g.frames(60);
        const std::string chasing = hudText(g.world);
        std::printf("  (cerca: \"%s\")\n", chasing.c_str());
        check(contains(chasing, "Perseguir") || contains(chasing, "Atacar"), "al ver al jugador lo persigue y lo ataca");
        // F golpea: con poca vida huye.
        for (int i = 0; i < 3; ++i) {
            g.moveTo("Jugador", enemy.worldPosition() + Vec3{1.5f, 0.2f, 0.0f});
            g.frame();
            g.press(dm::Key::F);
        }
        g.frames(10);
        std::printf("  (tras golpear: \"%s\")\n", hudText(g.world).c_str());
        check(contains(hudText(g.world), "Huir"), "F le quita vida y huye");
        check(g.errors() == 0, "los scripts se ejecutan sin errores");
    }

    // --- Mundo de bloques ---
    if (wanted("bloques")) {
        std::printf("Mundo de bloques\n");
        const project::ProjectInfo p = editor::createProjectFromTemplate(*find(list, "voxel"), root, "Bloques");
        Game g;
        g.use_voxels = true;
        g.save_root = root / "Mundos";
        const bool loaded = g.open(p);
        ecs::World& world = g.world;
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
            const ecs::Entity camera = world.findByName("Main Camera");
            // Siempre el mismo mundo: con la semilla al azar a veces se aparecia
            // ante una colina y las pruebas de andar y romper fallaban.
            camera.get<scripting::CppScript>().setValue("semilla", "12345");
            g.start();
            check(g.compiled, "el script de C++ compila");
            // Aparece y cae hasta el suelo (espera a que se genere).
            g.frames(600);
            g.voxels.waitUntilReady(camera.worldPosition(), 2, 30.0f);
            g.frames(240);
            const Vec3 eye = camera.worldPosition();
            const int below = g.voxels.getBlock(static_cast<int>(std::floor(eye.x)), static_cast<int>(std::floor(eye.y - 1.62f - 0.2f)),
                                                static_cast<int>(std::floor(eye.z)));
            std::printf("  (ojos en %.2f %.2f %.2f, mundo \"%s\")\n", static_cast<double>(eye.x), static_cast<double>(eye.y),
                        static_cast<double>(eye.z), g.voxels.worldName().c_str());
            check(g.voxels.worldName() == "Mi mundo", "crea el mundo con nombre");
            check(voxel::blockDef(static_cast<voxel::BlockId>(below)).solid || below == voxel::block::Water,
                  "el jugador aparece y se apoya en el suelo");
            check(world.findByName("Corazon 1").get<ui::Image>().texture == "Voxel/Iconos/corazon.png" &&
                      world.findByName("Corazon 10").get<ui::Image>().texture == "Voxel/Iconos/corazon.png",
                  "supervivencia: vida y hambre llenas en pantalla");
            // Andar hacia atras (S) un segundo: con esta semilla, delante hay un escalon.
            g.key(dm::Key::S, true);
            g.frames(60);
            g.key(dm::Key::S, false);
            g.frames(20);
            const float walked = core::length(Vec3{camera.worldPosition().x - eye.x, 0.0f, camera.worldPosition().z - eye.z});
            std::printf("  (anduvo %.2f m)\n", static_cast<double>(walked));
            check(walked > 1.5f, "WASD mueve al jugador por los bloques");

            // Clic: captura el raton; mirar al suelo; mantener el clic rompe el bloque.
            g.click(dm::MouseButton::Left);
            check(g.cursor_locked && g.scripts.cursorLocked(), "un clic captura el raton (primera persona)");
            dm::Event look;
            look.type = dm::EventType::MouseRawMoved;
            look.deltaY = 2000.0f;  // todo abajo
            g.input.onEvent(look);
            g.frame();
            // Rompe un bloque de `block` bajo los pies (manteniendo el clic) y lo recoge.
            const auto breakBelow = [&](voxel::BlockId block, int hold_frames) {
                const voxel::VoxelHit ground = g.voxels.raycast(camera.worldPosition(), camera.forward(), 6.0f);
                if (!ground.hit) return std::make_pair(false, false);
                g.voxels.setBlock(ground.block.x, ground.block.y, ground.block.z, block);
                g.frames(2);
                g.mouse(dm::MouseButton::Left, true);
                g.frames(15);
                const bool cracking = world.findByName("Grietas").activeSelf() && world.findByName("Grietas").get<ecs::MeshRenderer>().mesh;
                g.frames(hold_frames);
                g.mouse(dm::MouseButton::Left, false);
                g.frames(120);  // el objeto cae y se recoge
                return std::make_pair(g.voxels.getBlock(ground.block.x, ground.block.y, ground.block.z) != block, cracking);
            };
            {
                const voxel::VoxelHit ground = g.voxels.raycast(camera.worldPosition(), camera.forward(), 6.0f);
                g.frames(2);
                check(ground.hit && world.findByName("Contorno").activeSelf(), "contorno sobre el bloque apuntado");
            }
            const auto [dirt_broken, cracking] = breakBelow(voxel::block::Dirt, 60);
            check(cracking, "mientras se rompe se ven las grietas");
            check(dirt_broken, "mantener el clic rompe el bloque (segun su dureza)");
            const ecs::Entity slot = world.findByName("Hueco 1");
            check(slot.valid() && slot.children().size() == 2 &&
                      world.wrap(slot.children()[0]).get<ui::Image>().texture == "Voxel/Iconos/dirt.png",
                  "el bloque suelta un objeto, se recoge y la barra muestra su icono");

            // Crafteo: un tronco da 4 tablones; 2 tablones, 4 palos; el pico pide mesa.
            const auto [log_broken, unused] = breakBelow(voxel::block::OakLog, 360);
            (void)unused;
            check(log_broken, "romper un tronco (a mano tarda mas)");
            const auto slots_text = [&] {
                std::string all;
                for (int i = 1; i <= 9; ++i) {
                    const ecs::Entity h = world.findByName("Hueco " + std::to_string(i));
                    for (const auto c : h.valid() ? h.children() : std::vector<entt::entity>{}) {
                        const ecs::Entity e = world.wrap(c);
                        if (e.has<ui::Image>()) all += e.get<ui::Image>().texture + " ";
                        if (e.has<ui::Text>()) all += e.get<ui::Text>().text + " ";
                    }
                }
                return all;
            };
            g.button("Main Camera", "OnReceta", world.findByName("Receta 1"));
            g.frames(2);
            std::printf("  (barra: %s)\n", slots_text().c_str());
            check(contains(slots_text(), "oak_planks.png 4"), "craftear: 1 tronco da 4 tablones");
            g.button("Main Camera", "OnReceta", world.findByName("Receta 4"));
            g.frames(2);
            check(contains(slots_text(), "palo.png 4") && contains(slots_text(), "oak_planks.png 2"), "2 tablones dan 4 palos");
            g.button("Main Camera", "OnReceta", world.findByName("Receta 7"));
            g.frames(2);
            check(!contains(slots_text(), "pico_madera") && contains(g.text("Aviso"), "mesa"), "el pico sin mesa de trabajo no se puede");
            // El inventario (E) marca que recetas se pueden hacer.
            g.press(dm::Key::E);
            const ecs::Entity bag = world.findByName("Inventario");
            check(bag.activeSelf() && !g.cursor_locked, "E abre el inventario y suelta el raton");
            check(!world.findByName("Receta 8").get<ui::Button>().interactable, "sin roca, la receta del pico de piedra esta apagada");
            g.press(dm::Key::E);
            check(!bag.activeSelf() && g.cursor_locked, "E lo cierra y vuelve a capturar el raton");
            check(g.errors() == 0, "el script se ejecuta sin errores");

            // Parar guarda el mundo, el inventario y la vida.
            g.stop();
            check(!g.cursor_locked || !g.scripts.cursorLocked(), "al parar se suelta el raton");
            voxel::VoxelSystem again;
            again.setSaveRoot(root / "Mundos");
            again.start(world);
            const auto worlds = again.listWorlds();
            check(worlds.size() == 1 && worlds[0].name == "Mi mundo" && worlds[0].mode == "supervivencia" &&
                      again.loadWorld("Mi mundo") && !again.meta("player").empty() &&
                      again.meta("inventario").find("palo") != std::string::npos,
                  "se guardan el mundo, la posicion y el inventario");
        }
    }

    // --- MMO RPG (todo el juego en C++) ---
    if (wanted("mmo")) {
        std::printf("MMO RPG\n");
        const project::ProjectInfo p = editor::createProjectFromTemplate(*find(list, "mmo"), root, "Reinos");
        Game g;
        g.prefs = root / "mmo.prefs";
        const bool loaded = g.open(p);
        ecs::World& world = g.world;
        check(loaded && world.findAllWithTag("Enemigo").size() == 19 && world.findAllWithTag("NPC").size() == 3 &&
                  world.findAllWithTag("Bot").size() == 4,
              "la escena: 19 enemigos, 3 personajes y 4 jugadores simulados");
        check(std::filesystem::exists(p.assetsFolder() / "Scripts" / "Heroe.cpp") &&
                  std::filesystem::exists(p.assetsFolder() / "Scripts" / "Enemigo.cpp") &&
                  std::filesystem::exists(p.assetsFolder() / "Scripts" / "Bot.cpp") &&
                  std::filesystem::exists(p.assetsFolder() / "Scripts" / "NPC.cpp"),
              "los sistemas estan en scripts de C++");
        if (loaded) {
            g.start();
            check(g.compiled, "los scripts de C++ compilan");
            g.frames(120);
            check(g.nav.ready(), "genera la malla de navegacion de todo el mapa");
            check(contains(g.text("UI_Chat"), "Bienvenido"), "el chat da la bienvenida");
            check(contains(g.childText("UI_Jugador", "Nombre"), "Nivel 1") && g.childText("UI_Jugador", "VidaTexto") == "100 / 100" &&
                      g.childText("UI_Hab 5", "Nombre") == "vida x3",
                  "empieza a nivel 1, con 100 de vida y 3 pociones");
            check(g.child("Capitana Elena", "Marca").activeSelf(), "la capitana muestra la marca de mision nueva");

            // Teclado: Tab elige el enemigo mas cercano; I abre el inventario.
            g.moveTo("Jugador", g.position("Lobo 1") + Vec3{2.0f, 0.6f, 0.0f});
            g.frames(2);
            g.press(dm::Key::Tab);
            g.frame();
            check(g.active("UI_Objetivo") && contains(g.childText("UI_Objetivo", "Nombre"), "Lobo"), "Tab elige el lobo de al lado");
            g.press(dm::Key::I);
            check(g.active("UI_Inventario"), "I abre el inventario");
            g.press(dm::Key::Escape);
            check(!g.active("UI_Inventario"), "Escape lo cierra");

            // Hablar (E) con la capitana y aceptar la mision de los lobos.
            const auto talk = [&](const char* npc) {
                g.moveTo("Jugador", g.position(npc) + Vec3{0.0f, 0.0f, 2.0f});
                g.frames(2);
                g.press(dm::Key::E);
            };
            const auto option = [&](int i) {
                g.button("Jugador", "OnOpcion", g.child("UI_Dialogo", "Opcion " + std::to_string(i)));
                g.frames(2);
            };
            const auto optionText = [&](int i) {
                const ecs::Entity o = g.child("UI_Dialogo", "Opcion " + std::to_string(i));
                std::string t;
                if (!o.valid() || !o.activeSelf()) return t;
                world.forEachDepthFirst([&](ecs::Entity e) {
                    if (t.empty() && e.parent() == o && e.has<ui::Text>()) t = e.get<ui::Text>().text;
                });
                return t;
            };
            talk("Capitana Elena");
            check(g.active("UI_Dialogo") && optionText(1) == "Mision: Lobos hambrientos", "E abre el dialogo con la mision");
            option(1);  // la mision
            option(1);  // Aceptar
            check(contains(g.text("UI_Seguimiento"), "Lobos hambrientos: 0/5"), "se acepta la mision");

            // Cazar 5 lobos con Golpe (1) y la bola de fuego (2).
            int deaths = 0;
            for (int n = 1; n <= 19 && !contains(g.text("UI_Seguimiento"), "lista"); ++n) {
                const std::string wolf = "Lobo " + std::to_string(n);
                if (!world.findByName(wolf).valid()) continue;
                for (int tries = 0; tries < 40 && !contains(g.text("UI_Seguimiento"), "lista"); ++tries) {
                    const ecs::Entity model = g.child(wolf, "Modelo");
                    if (model.valid() && !model.activeSelf()) break;  // muerto
                    if (g.active("UI_Muerte")) {
                        ++deaths;
                        g.button("Jugador", "OnReaparecer", world.findByName("UI_Muerte"));
                        g.frames(2);
                    }
                    g.moveTo("Jugador", g.position(wolf) + Vec3{1.6f, 0.6f, 0.0f});
                    g.frames(2);
                    g.press(tries % 3 == 2 ? dm::Key::Num2 : dm::Key::Num1);
                    g.frames(tries % 3 == 2 ? 100 : 60);
                }
            }
            std::printf("  (caza: \"%s\", %d muertes)\n", g.text("UI_Seguimiento").c_str(), deaths);
            check(contains(g.text("UI_Seguimiento"), "Lobos hambrientos: ¡lista!"), "5 lobos muertos: mision lista");
            check(!contains(g.childText("UI_Jugador", "Nombre"), "Nivel 1"), "sube de nivel con la experiencia");

            // Botin: cazando ya se recogen las bolsas que caen al lado; las que
            // queden, pasando por encima (copias de "Plantilla Botin").
            const bool picked = contains(g.text("UI_Chat"), "[Botin] Recoges");
            const auto gold = [&] {
                g.press(dm::Key::I);
                const std::string t = g.childText("UI_Inventario", "Oro");
                g.press(dm::Key::Escape);
                return std::atoi(t.c_str() + (t.size() > 5 ? 5 : t.size()));
            };
            const int gold_before = gold();
            std::vector<Vec3> bags;
            world.forEachDepthFirst([&](ecs::Entity e) {
                if (e.name() == "Plantilla Botin" && e.activeSelf()) bags.push_back(e.worldPosition());
            });
            for (const Vec3& b : bags) {
                g.moveTo("Jugador", b + Vec3{0.0f, 1.0f, 0.0f});
                g.frames(3);
            }
            const int gold_after = gold();
            std::printf("  (%zu bolsas de botin, oro %d -> %d)\n", bags.size(), gold_before, gold_after);
            check(picked || (!bags.empty() && gold_after > gold_before), "los enemigos sueltan botin y se recoge al pasar");

            // Entregar la mision y equipar el premio (la espada de hierro).
            talk("Capitana Elena");
            check(optionText(1) == "Entregar: Lobos hambrientos", "la capitana espera la entrega");
            option(1);
            option(1);
            check(!contains(g.text("UI_Seguimiento"), "Lobos"), "se entrega la mision");
            g.press(dm::Key::Escape);
            g.press(dm::Key::I);
            for (int i = 1; i <= 20; ++i) {
                const ecs::Entity s = g.child("UI_Inventario", "Ranura " + std::to_string(i));
                std::string name;
                world.forEachDepthFirst([&](ecs::Entity e) {
                    if (name.empty() && e.name() == "Nombre" && e.parent() == s && e.has<ui::Text>()) name = e.get<ui::Text>().text;
                });
                if (name == "Espada de hierro") {
                    g.button("Jugador", "OnRanura", s);
                    g.frames(2);
                    break;
                }
            }
            g.press(dm::Key::C);
            check(contains(g.childText("Arma", "Texto"), "Espada de hierro"), "se equipa la espada de la recompensa (sube el ataque)");
            g.press(dm::Key::Escape);

            // Comprar en la tienda: una pocion por 8 de oro.
            const std::string potions_before = g.childText("UI_Hab 5", "Nombre");
            talk("Mercader Tomas");
            for (int i = 1; i <= 6; ++i) {
                if (optionText(i) == "Comerciar") {
                    option(i);
                    break;
                }
            }
            const int shop_gold = std::atoi(g.childText("UI_Inventario", "Oro").c_str() + 5);
            option(1);
            const int shop_after = std::atoi(g.childText("UI_Inventario", "Oro").c_str() + 5);
            std::printf("  (tienda: oro %d -> %d, pociones \"%s\" -> \"%s\")\n", shop_gold, shop_after, potions_before.c_str(),
                        g.childText("UI_Hab 5", "Nombre").c_str());
            check(shop_gold - shop_after == 8 && g.childText("UI_Hab 5", "Nombre") != potions_before, "la tienda vende pociones por oro");
            g.press(dm::Key::Escape);
            g.press(dm::Key::Num5);
            g.frame();
            check(!g.childText("UI_Hab 5", "Tiempo").empty(), "5 usa una pocion (con enfriamiento)");

            // Un goblin ataca al heroe si se acerca.
            g.frames(600);  // se cura
            std::string life;
            for (int i = 0; i < 6; ++i) {
                g.moveTo("Jugador", g.position("Goblin 9") + Vec3{2.0f, 0.3f, 0.0f});
                g.frames(120);
                life = g.childText("UI_Jugador", "VidaTexto");
                if (g.active("UI_Muerte") || life.substr(0, life.find(" / ")) != life.substr(life.find(" / ") + 3)) break;
            }
            check(g.active("UI_Muerte") || life.substr(0, life.find(" / ")) != life.substr(life.find(" / ") + 3),
                  "los goblins atacan al heroe cercano");

            // Morir (el Rey Goblin) y reaparecer.
            for (int i = 0; i < 120 && !g.active("UI_Muerte"); ++i) {
                g.moveTo("Jugador", g.position("Rey Goblin 17") + Vec3{2.0f, 0.0f, 0.0f});
                g.frames(30);
            }
            check(g.active("UI_Muerte"), "al morir sale la pantalla de muerte");
            g.button("Jugador", "OnReaparecer", world.findByName("UI_Muerte"));
            g.frames(2);
            check(!g.active("UI_Muerte"), "reaparece en el pueblo");

            // Los otros jugadores se mueven por el mundo.
            std::vector<Vec3> start;
            for (const ecs::Entity& b : world.findAllWithTag("Bot")) start.push_back(b.worldPosition());
            g.frames(900);
            float moved = 0.0f;
            std::size_t i = 0;
            for (const ecs::Entity& b : world.findAllWithTag("Bot")) {
                if (i < start.size()) moved = std::max(moved, core::length(b.worldPosition() - start[i]));
                ++i;
            }
            std::printf("  (un jugador simulado se movio %.1f m)\n", static_cast<double>(moved));
            check(moved > 3.0f, "los jugadores simulados se mueven por el mundo");

            // Guardar: la partida va a Prefs (al ganar experiencia y al parar).
            const std::string saved = g.run("return Prefs.getString('mmo_partida', '')");
            check(saved.rfind("1|", 0) == 0 && contains(saved, "espada_hierro") && contains(saved, "1:hecha"),
                  "la partida se guarda (nivel, inventario, equipo y misiones)");
            check(g.errors() == 0, "los scripts se ejecutan sin errores");
        }
    }

    // --- Mundo abierto (rendimiento) ---
    if (wanted("abierto")) {
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
        const ecs::Entity pl = r.world().findByName("Jugador");
        const float ground_y = terrain::heightAt(*data, tc, island.worldPosition(), pl.worldPosition().x, pl.worldPosition().z);
        std::printf("    jugador a %.2f m, suelo a %.2f m\n", pl.worldPosition().y, ground_y);
        check(r.loaded && r.script_errors == 0, "la escena se juega y los scripts corren sin errores");
        check(ground_y > 5.0f, "el jugador aparece en tierra, no en el mar");
        check(std::abs(pl.worldPosition().y - ground_y - 1.4f) < 0.8f, "y se queda de pie sobre el terreno (colision)");
        const ecs::Entity hud = r.world().findByName("UI_Rendimiento");
        check(hud.valid() && hud.get<ui::Text>().text.find("FPS") != std::string::npos, "el HUD de rendimiento mide");
    }

    // --- Online: un servidor y un cliente en el mismo proceso ---
    if (wanted("online")) {
        std::printf("Online\n");
        const project::ProjectInfo p = editor::createProjectFromTemplate(*find(list, "online"), root, "Arena");
        check(std::filesystem::exists(p.assetsFolder() / "Prefabs" / "Jugador.crprefab") &&
                  std::filesystem::exists(p.assetsFolder() / "Prefabs" / "Balon.crprefab") &&
                  std::filesystem::exists(p.assetsFolder() / "Scripts" / "Red.cpp"),
              "prefabs de red y scripts");
        auto host_game = std::make_unique<Game>();
        auto guest_game = std::make_unique<Game>();
        Game& host = *host_game;
        Game& guest = *guest_game;
        host.prefs = root / "prefs_servidor.txt";
        guest.prefs = root / "prefs_cliente.txt";
        bool opened = true;
        for (Game* g : {&host, &guest}) {
            opened = g->open(p) && opened;
            if (!opened) continue;
            // Puerto de prueba y un evento cada 30 s (se espera al final).
            scripting::CppScript& red = g->world.findByName("Red").get<scripting::CppScript>();
            red.setValue("puerto", "27790");
            red.setValue("segundosEvento", "30");
        }
        check(opened, "la escena se abre dos veces (servidor y cliente)");
        if (opened) {
            host.start();
            guest.start();
            check(host.compiled && guest.compiled, "los scripts de C++ compilan");
            const auto frames = [&](int n) {
                for (int i = 0; i < n; ++i) {
                    host.frame();
                    guest.frame();
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            };
            const auto count = [](Game& g, const std::string& name) {
                int n = 0;
                for (const auto h : g.world.registry().view<net::NetworkObject>()) {
                    if (g.world.wrap(h).name() == name) ++n;
                }
                return n;
            };
            // El objeto de red `name` de `owner` (0 = el primero) y su id.
            const auto net = [](Game& g, const std::string& name, std::uint32_t owner) {
                for (const auto h : g.world.registry().view<net::NetworkObject>()) {
                    const ecs::Entity e = g.world.wrap(h);
                    const net::NetworkObject& n = e.get<net::NetworkObject>();
                    if (e.name() == name && (owner == 0 || n.owner == owner)) return e;
                }
                return ecs::Entity{};
            };
            const auto netId = [](ecs::Entity e) { return e.valid() ? std::to_string(e.get<net::NetworkObject>().net_id) : std::string("0"); };
            const auto vec = [](const Vec3& v) {
                char t[96];
                std::snprintf(t, sizeof(t), "Vec3(%f, %f, %f)", static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z));
                return std::string(t);
            };
            frames(5);
            check(host.active("UI_Menu") && !host.active("UI_Juego"), "empieza en el menu");
            host.run("Scene.find('UI_Nombre').text = 'Ana'");
            host.button("Red", "OnCrear", host.world.findByName("UI_Menu"));
            frames(5);
            check(host.run("return Network.isServer()") == "true" && !host.active("UI_Menu"), "Crear partida: servidor y a jugar");
            check(count(host, "Jugador") == 1 && count(host, "Balon") == 1 && count(host, "Caja") == 4 && count(host, "Moneda") == 8,
                  "el servidor crea su jugador, el balon, 4 cajas y 8 monedas");
            guest.run("Scene.find('UI_Nombre').text = 'Luis'; Scene.find('UI_IP').text = '127.0.0.1'");
            guest.button("Red", "OnUnirse", guest.world.findByName("UI_Menu"));
            for (int i = 0; i < 200 && count(guest, "Jugador") < 2; ++i) frames(1);
            frames(30);
            check(guest.run("return Network.myId()") == "2", "Unirse: el cliente es el jugador 2");
            check(count(guest, "Jugador") == 2 && count(guest, "Balon") == 1 && count(guest, "Moneda") >= 8,
                  "el cliente recibe los dos jugadores, el balon y las monedas");
            check(count(host, "Jugador") == 2, "el servidor ve al nuevo jugador");
            check(contains(host.text("UI_Chat"), "Luis se ha unido") && contains(guest.text("UI_Chat"), "Luis se ha unido"),
                  "el chat anuncia la llegada en los dos");
            check(contains(guest.text("UI_Marcador"), "Ana") && contains(guest.text("UI_Marcador"), "Luis"),
                  "el marcador del cliente tiene a los dos");

            // Chat del cliente: pasa por el servidor, que pone el nombre.
            guest.message("Red", "OnChat", "\"hola desde el cliente\"");
            frames(20);
            check(contains(host.text("UI_Chat"), "Luis: hola desde el cliente") && contains(guest.text("UI_Chat"), "Luis: hola desde el cliente"),
                  "chat: el mensaje del cliente llega a todos con su nombre");

            // El cliente mueve su jugador: el servidor lo ve moverse.
            const std::string mine = netId(net(guest, "Jugador", 2));
            guest.run("Network.find(" + mine + ").position = Vec3(10, 1.2, 10)");
            frames(40);
            const ecs::Entity guest_on_host = net(host, "Jugador", 2);
            check(guest_on_host.valid() && std::abs(guest_on_host.worldPosition().x - 10.0f) < 0.6f,
                  "el jugador del cliente se mueve tambien en el servidor");

            // Monedas: el servidor da el punto al que pasa cerca (el jugador del
            // cliente va hasta una).
            const ecs::Entity coin = net(host, "Moneda", 0);
            guest.run("Network.find(" + mine + ").position = " + vec(coin.worldPosition() + Vec3{0.0f, 0.4f, 0.0f}));
            frames(40);
            // (por el camino puede coger alguna mas)
            check(contains(guest.text("UI_Marcador"), "Luis  ") && !contains(guest.text("UI_Marcador"), "Luis  0 pts") && !coin.valid(),
                  "recoger una moneda: 1 punto (lo decide el servidor)");
            guest.run("Network.find(" + mine + ").position = Vec3(10, 1.2, 10)");
            frames(40);

            // Patear el balon (F en el cliente -> el servidor aplica el golpe).
            const ecs::Entity ball = net(host, "Balon", 0);
            const std::string ball_id = netId(ball);
            host.run("local b = Network.find(" + ball_id + "); b.position = " + vec(guest_on_host.worldPosition() + Vec3{1.2f, 0.0f, 0.0f}) +
                     "; b.velocity = Vec3()");
            frames(10);
            const float before = ball.worldPosition().x;
            guest.run("Network.send('patear', {}, 'server')");
            frames(40);
            check(ball.worldPosition().x > before + 0.5f, "patear: el balon sale disparado en el servidor");
            check(guest.position("Balon").x > before, "y el cliente lo ve moverse");

            // Empujar el balon caminando (sin F): el jugador del cliente lo
            // mueve tambien en el servidor, no solo el del servidor.
            host.run("local b = Network.find(" + ball_id + "); b.position = " +
                     vec(guest_on_host.worldPosition() + Vec3{2.2f, -0.6f, 0.0f}) + "; b.velocity = Vec3(); b.angularVelocity = Vec3()");
            frames(40);
            const float rest = ball.worldPosition().x;
            guest.key(dm::Key::D, true);
            frames(70);
            guest.key(dm::Key::D, false);
            frames(20);
            std::printf("  balon empujado: %.2f -> %.2f\n", static_cast<double>(rest), static_cast<double>(ball.worldPosition().x));
            check(ball.worldPosition().x > rest + 1.5f, "empujar: el jugador del cliente mueve el balon en el servidor");
            check(guest.position("Balon").x > rest + 1.0f, "y en el cliente el balon tambien avanza");
            // Esperar a que el balon se pare de verdad en el servidor.
            for (int k = 0; k < 40; ++k) {
                frames(30);
                if (host.run("return Network.find(" + ball_id + ").velocity.x") == "0") break;
                if (playing_physics == nullptr && core::length(host.physics.linearVelocity(ball)) < 0.05f) break;
            }
            frames(60);
            const float gap = core::length(guest.position("Balon") - ball.worldPosition());
            std::printf("  diferencia cliente/servidor en reposo: %.3f m\n", static_cast<double>(gap));
            check(gap < 0.25f, "al pararse, el balon del cliente coincide con el del servidor");

            // Gol: +5 al ultimo que lo toco (el cliente lo pateo).
            const std::string board = guest.text("UI_Marcador");
            const std::size_t at = board.find("Luis  ");
            const int points = at == std::string::npos ? 0 : std::atoi(board.c_str() + at + 6);
            host.run("Network.find(" + ball_id + ").position = Vec3(27.5, 1, 0)");
            frames(20);
            check(contains(guest.text("UI_Aviso"), "GOL de Luis"), "gol: aviso a todos");
            check(contains(guest.text("UI_Marcador"), "Luis  " + std::to_string(points + 5) + " pts"), "y +5 puntos");

            // Evento (cada 30 s): lluvia de monedas o monedas dobles.
            bool event = false;
            for (int k = 0; k < 40 && !event; ++k) {
                frames(60);
                event = contains(guest.text("UI_Aviso"), "onedas");
            }
            check(event, "un evento se anuncia en todos");

            // El cliente sale: su jugador desaparece en el servidor.
            guest.button("Red", "OnSalir", guest.world.findByName("UI_Juego"));
            frames(40);
            check(guest.active("UI_Menu") && count(guest, "Jugador") == 0, "salir: el cliente vuelve al menu sin objetos de red");
            check(count(host, "Jugador") == 1 && contains(host.text("UI_Chat"), "Luis ha salido"), "el servidor quita su jugador y lo anuncia");
            check(host.errors() == 0 && guest.errors() == 0, "los scripts se ejecutan sin errores");
            host.scripts.shutdownNetwork();
            guest.scripts.shutdownNetwork();
        }
    }

    // --- Plataformas 2D: tilemap, sprites y fisica 2D ---
    if (wanted("plataformas")) {
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
        const ecs::Entity cam = r.world().findByName("Main Camera");
        check(r.loaded, "la escena inicial se abre");
        check(r.script_errors == 0, "los scripts se ejecutan sin errores");
        check(y_25 < 1.5f && y_25 > -3.0f && std::abs(y_25 - y_15) < 0.05f,
              "el jugador cae y se queda sobre el tilemap (Tilemap Collider 2D)");
        check(r.world().findAllWithTag("Moneda").size() == 7, "las 7 monedas tienen su tag");
        check(cam.valid() && cam.has<ecs::Camera>() && cam.get<ecs::Camera>().orthographic, "camara ortografica");
        const ecs::Entity score = r.world().findByName("Marcador");
        check(score.valid() && !score.has<scripting::CppScript>(), "el marcador no lleva script (lo escribe el jugador)");
    }

    // --- Coches: Vehicle + Wheel Collider ---
    if (wanted("coches")) {
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
        const ecs::Entity car = r.world().findByName("Coche");
        const float moved = car.valid() ? std::abs(car.worldPosition().z - start_z) : 0.0f;
        std::printf("  (%d ruedas en el suelo; tras 4 s a fondo: %.1f km/h, marcha %d, %.1f m; HUD \"%s\")\n", on_ground,
                    static_cast<double>(last.speed_kmh), last.gear, static_cast<double>(moved), hudText(r.world()).c_str());
        check(r.loaded, "la escena inicial se abre");
        check(r.script_errors == 0, "los scripts se ejecutan sin errores");
        check(last.valid && last.wheel_count == 4 && on_ground == 4, "el coche se apoya en sus 4 Wheel Collider");
        check(last.speed_kmh > 20.0f && last.gear >= 1 && moved > 10.0f, "acelera y avanza con el motor y las marchas");
        check(contains(hudText(r.world()), "km/h"), "el velocimetro muestra la velocidad");
    }

    // --- Realidad virtual: Jugador VR, objetos que se cogen y panel en el mundo ---
    if (wanted("vr")) {
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
        check(r.script_errors == 0, "el script del panel compila y corre sin errores");
        check(ignored, "el Jugador VR no choca con lo que se coge (XR Grabbable)");
        const ecs::Entity rig = r.world().findByName("Jugador VR");
        const ecs::Entity left = r.world().findByName("Mano izquierda");
        const ecs::Entity right = r.world().findByName("Mano derecha");
        check(rig.valid() && rig.has<xr::XrOrigin>() && rig.has<physics::CharacterController>() && rig.has<xr::XrPlayer>() &&
                  left.valid() && left.has<xr::XrInteractor>() && left.get<xr::XrController>().hand == xr::Hand::Left &&
                  right.valid() && right.has<xr::XrInteractor>() && right.get<xr::XrController>().hand == xr::Hand::Right &&
                  right.get<xr::XrInteractor>().ray_material.valid(),
              "Jugador VR con capsula y dos manos con XR Interactor (y su rayo)");
        int grabbables = 0;
        bool all_bodies = true;
        r.world().forEachDepthFirst([&](ecs::Entity e) {
            if (!e.has<xr::XrGrabbable>()) return;
            ++grabbables;
            all_bodies = all_bodies && e.has<physics::Rigidbody>();
        });
        check(grabbables >= 10 && all_bodies, "objetos con XR Grabbable y Rigidbody");
        // Tras 2 s de fisica siguen en la mesa y la estanteria (no atraviesan nada).
        const ecs::Entity cube = r.world().findByName("Cubo rojo");
        const ecs::Entity ball = r.world().findByName("Pelota 1");
        std::printf("  (cubo a %.3f m, pelota a %.3f m)\n", cube.valid() ? static_cast<double>(cube.worldPosition().y) : -1.0,
                    ball.valid() ? static_cast<double>(ball.worldPosition().y) : -1.0);
        check(cube.valid() && std::abs(cube.worldPosition().y - 0.81f) < 0.03f && ball.valid() &&
                  std::abs(ball.worldPosition().y - 0.955f) < 0.05f,
              "los objetos se quedan en la mesa y la estanteria");
        const ecs::Entity panel = r.world().findByName("Panel VR");
        const scripting::CppScript* menu = panel.valid() ? panel.tryGet<scripting::CppScript>() : nullptr;
        check(menu != nullptr && menu->class_name == "MenuVR" && menu->value("objetos") != nullptr &&
                  std::filesystem::exists(p.assetsFolder() / "Scripts" / "MenuVR.h") &&
                  std::filesystem::exists(p.assetsFolder() / "Scripts" / "MenuVR.cpp"),
              "el panel lleva su script de C++ (MenuVR.h + MenuVR.cpp)");
        // El rayo de una mano apunta al boton y el gatillo lo pulsa.
        ui::UiSystem ui;
        std::array<ui::UiPointer, ui::UiSystem::kMaxPointers> pointers{};
        ui.updateWorld(r.world(), pointers, true, 0.0f);
        const bool one_canvas = ui.worldCanvases().size() == 1 && !ui.worldCanvases().front().commands.empty();
        const ecs::Entity button = r.world().findByName("Reiniciar objetos");
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
            ui.updateWorld(r.world(), pointers, true, 0.1f);
            hovered = ui.pointerHits()[1].hit && ui.pointerHits()[1].over_control &&
                      std::abs(ui.pointerHits()[1].distance - 1.0f) < 0.01f;
            pointers[1].down = true;
            ui.updateWorld(r.world(), pointers, true, 0.2f);
            pointers[1].down = false;
            ui.updateWorld(r.world(), pointers, true, 0.3f);
            for (const ui::UiEvent& e : ui.takeEvents()) {
                clicked = clicked || (e.method == "OnReiniciar" && e.target == panel);
            }
        }
        check(one_canvas, "un Canvas en el mundo con su lista de dibujo");
        check(hovered && clicked, "el rayo apunta al boton y el gatillo lo pulsa (OnReiniciar al script)");
    }

    // --- Del usuario ---
    if (wanted("usuario")) {
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
            check(r.loaded && r.script_errors == 0 && r.world().findByName("Jugador").valid() &&
                      std::filesystem::exists(p.assetsFolder() / "Scripts" / "Jugador.cpp"),
                  "un proyecto nuevo desde ella tiene todo (escena inicial, scripts)");
        }
        bool threw = false;
        try {
            editor::createProjectFromTemplate(*find(again, "blank"), root, "Copia");
        } catch (const std::exception&) {
            threw = true;
        }
        check(threw && std::filesystem::exists(root / "Copia" / "Assets" / "Scripts" / "Jugador.cpp"),
              "no pisa un proyecto que ya existe");
    }

    std::filesystem::remove_all(root, ec);
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
