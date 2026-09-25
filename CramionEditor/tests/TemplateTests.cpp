// Pruebas de las plantillas de proyecto (consola, sin GPU): cada plantilla
// integrada crea un proyecto que se abre, su escena inicial se carga y se
// juega un rato (fisica, navegacion y los scripts de Lua sin errores). Y las
// plantillas del usuario: guardar un proyecto como plantilla y crear otro
// desde ella. Devuelve 0 si todo va.

#include "ProjectTemplates.h"

#include <CramionCore/CramionCore.h>

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

    // Plantillas del usuario en una carpeta temporal (no en la de verdad).
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_template_tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "AppData");
    _putenv_s("LOCALAPPDATA", (root / "AppData").string().c_str());

    const std::vector<editor::ProjectTemplate> list = editor::availableTemplates();
    std::printf("Plantillas integradas\n");
    check(list.size() == 3 && find(list, "blank") && find(list, "third_person") && find(list, "navigation"),
          "hay 3 plantillas integradas");

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

    // --- Del usuario ---
    {
        std::printf("Plantillas del usuario\n");
        const auto source = project::openProject(root / "Plataformas");
        check(source.has_value() && editor::saveProjectAsTemplate(*source, "Mi plataformas", "Prueba"),
              "guardar un proyecto como plantilla");
        const std::vector<editor::ProjectTemplate> again = editor::availableTemplates();
        const editor::ProjectTemplate* mine = find(again, "user:Mi plataformas");
        check(again.size() == 4 && mine != nullptr && mine->category == "Mis plantillas" && mine->description == "Prueba",
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
