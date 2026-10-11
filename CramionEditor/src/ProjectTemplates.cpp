// Plantillas de proyecto (ver ProjectTemplates.h). Las integradas construyen
// su escena con el ECS: primitivas con colliders y materiales propios,
// scripts de C++ de verdad (jugador, camara, monedas, guardias) y una
// interfaz sencilla, para que el proyecto se pueda jugar con Play nada mas
// crearlo.

#include "ProjectTemplates.h"
#include "TemplateScripts.h"
#include "TemplateMmoScripts.h"
#include "TemplateCreatureScripts.h"
#include "TemplateOnlineScripts.h"
#include "TemplateOpenWorldScripts.h"
#include "CreatureModels.h"
#include "LocomotionPack.h"
#include "TemplateLocomotionScripts.h"
#include "Template21Scripts.h"
#include "TemplateVrScripts.h"

#include <CramionCore/ecs/AnimatorController.h>
#include <CramionCore/twod/System2D.h>
#include <CramionCore/ai/StateMachine.h>
#include <CramionCore/ecs/Rigging.h>
#include <CramionCore/net/NetworkObject.h>

#include <CramionCore/CramionCore.h>
#include <CramionCore/asset/XrControllerModels.h>
#include <CramionCore/scripting/CppScripts.h>
#include <CramionCore/xr/XrRig.h>

#include <nlohmann/json.hpp>

#include <CramionFX/asset/ImageFile.h>

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>

namespace cramion::editor {

using core::Vec2;
using core::Vec3;

namespace {

constexpr std::uint32_t rgba(int r, int g, int b) {
    return static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8) |
           (static_cast<std::uint32_t>(b) << 16) | 0xFF000000u;
}

// --- Construccion de escenas -------------------------------------------------------

struct Builder {
    project::ProjectInfo& project;
    ecs::World world;

    explicit Builder(project::ProjectInfo& p) : project(p) {}

    assets::AssetRef material(const std::string& name, const Vec3& color, float roughness, float metallic = 0.0f,
                              const Vec3& emissive = Vec3{}, float emissive_intensity = 1.0f) {
        assets::MaterialAsset m;
        // El color del Inspector es sRGB; el del material, lineal.
        m.base_color = core::Vec4{std::pow(color.x, 2.2f), std::pow(color.y, 2.2f), std::pow(color.z, 2.2f), 1.0f};
        m.roughness = roughness;
        m.metallic = metallic;
        m.emissive = emissive;
        m.emissive_intensity = emissive_intensity;
        const std::filesystem::path path = project.assetsFolder() / "Materials" / (name + ".crmat");
        std::filesystem::create_directories(path.parent_path());
        std::string error;
        if (!assets::saveMaterial(m, path, &error)) throw std::runtime_error("material " + name + ": " + error);
        return assets::AssetRef{m.uuid, assets::AssetType::Material};
    }

    void script(const std::string& file, const char* code) {
        const std::filesystem::path path = project.assetsFolder() / "Scripts" / file;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out << code;
        if (!out) throw std::runtime_error("no se pudo escribir " + file);
    }

    // Todos los archivos de un juego de scripts (TemplateScripts.h).
    template <std::size_t N>
    void scripts(const TemplateFile (&files)[N]) {
        for (const TemplateFile& f : files) script(f.name, f.code);
    }

    // Le pone (o le cambia) el script de C++ de Scripts/<file>; la clase es el
    // nombre del archivo. Los valores van en JSON (ver text() y number()).
    static scripting::CppScript& attach(ecs::Entity e, const std::string& file,
                                        std::vector<scripting::CppScriptValue> values = {}) {
        scripting::CppScript& s = e.has<scripting::CppScript>() ? e.get<scripting::CppScript>() : e.add<scripting::CppScript>();
        s.script = "Scripts/" + file;
        s.class_name = std::filesystem::path(file).stem().string();
        s.values.clear();
        for (const scripting::CppScriptValue& v : values) s.setValue(v.name, v.json);
        return s;
    }
    static std::string text(const std::string& value) { return nlohmann::json(value).dump(); }
    static std::string number(double value) { return nlohmann::json(value).dump(); }

    ecs::Entity box(const std::string& name, const Vec3& position, const Vec3& size, const assets::AssetRef& mat,
                    const Vec3& euler = Vec3{}, ecs::Entity parent = {}) {
        ecs::Entity e = ecs::createPrimitive(world, assets::builtin::kCube, name, parent);
        e.setLocalPosition(position);
        e.setLocalEulerDegrees(euler);
        e.setLocalScale(size);
        e.get<ecs::MeshRenderer>().materials = {mat};
        e.add<physics::BoxCollider>();  // la caja unidad escalada por el Transform
        return e;
    }

    ecs::Entity player(const Vec3& spawn, const assets::AssetRef& body, const assets::AssetRef& visor) {
        ecs::Entity p = world.create("Jugador");
        p.setWorldPosition(spawn);
        physics::CapsuleCollider& capsule = p.add<physics::CapsuleCollider>();
        capsule.radius = 0.45f;
        capsule.height = 1.9f;
        capsule.material.friction = 0.0f;  // no se engancha en las paredes
        physics::Rigidbody& rb = p.add<physics::Rigidbody>();
        rb.mass = 70.0f;
        rb.lock_rotation_x = rb.lock_rotation_y = rb.lock_rotation_z = true;
        rb.continuous = true;
        attach(p, "Jugador.cpp");
        ecs::Entity model = world.create("Modelo", p);
        ecs::Entity mesh = ecs::createPrimitive(world, assets::builtin::kCapsule, "Cuerpo", model);
        mesh.setLocalScale(Vec3{0.9f, 0.95f, 0.9f});
        mesh.get<ecs::MeshRenderer>().materials = {body};
        ecs::Entity eyes = ecs::createPrimitive(world, assets::builtin::kCube, "Visor", model);
        eyes.setLocalPosition(Vec3{0.0f, 0.45f, -0.36f});
        eyes.setLocalScale(Vec3{0.62f, 0.22f, 0.22f});
        eyes.get<ecs::MeshRenderer>().materials = {visor};
        return p;
    }

    void camera(float distance, float pitch) {
        ecs::Entity cam = world.findByName("Main Camera");
        cam.setWorldPosition(Vec3{0.0f, 5.0f, 12.0f});
        attach(cam, "CamaraTercera.cpp", {{"distancia", number(distance)}, {"inclinacion", number(pitch)}});
    }

    // El texto del marcador arriba a la izquierda; script_file vacio = sin script.
    ecs::Entity hud(const std::string& script_file) {
        ecs::Entity canvas = world.create("HUD");
        canvas.add<ui::Canvas>();
        ecs::Entity text = world.create("Marcador", canvas);
        ui::RectTransform& rt = text.add<ui::RectTransform>();
        rt.anchor_min = Vec2{0.0f, 0.0f};
        rt.anchor_max = Vec2{0.0f, 0.0f};
        rt.pivot = Vec2{0.0f, 0.0f};
        rt.position = Vec2{40.0f, 32.0f};
        rt.size = Vec2{1100.0f, 60.0f};
        ui::Text& t = text.add<ui::Text>();
        t.text = "";
        t.font_size = 38.0f;
        t.h_align = ui::HAlign::Left;
        t.shadow = true;
        if (!script_file.empty()) attach(text, script_file);
        return text;
    }

    void ring(float half, float height, float thickness, const assets::AssetRef& mat) {
        box("Muro Norte", Vec3{0.0f, height * 0.5f, -half}, Vec3{half * 2.0f + thickness, height, thickness}, mat);
        box("Muro Sur", Vec3{0.0f, height * 0.5f, half}, Vec3{half * 2.0f + thickness, height, thickness}, mat);
        box("Muro Este", Vec3{half, height * 0.5f, 0.0f}, Vec3{thickness, height, half * 2.0f}, mat);
        box("Muro Oeste", Vec3{-half, height * 0.5f, 0.0f}, Vec3{thickness, height, half * 2.0f}, mat);
    }

    void save(const std::string& scene_name) {
        world.setSceneName(scene_name);
        const std::filesystem::path path = project.assetsFolder() / "Scenes" / (scene_name + ".crscene");
        std::filesystem::create_directories(path.parent_path());
        std::string error;
        if (!ecs::saveScene(world, path, &error)) throw std::runtime_error("escena: " + error);
        project.startup_scene = world.sceneUuid();
    }
};

void buildBlank(project::ProjectInfo& project) {
    Builder b(project);
    b.world.setSceneUuid(Uuid::generate());
    ecs::populateDefaultScene(b.world);
    const assets::AssetRef floor = b.material("Suelo", Vec3{0.62f, 0.63f, 0.65f}, 0.85f);
    b.box("Suelo", Vec3{0.0f, -0.5f, 0.0f}, Vec3{40.0f, 1.0f, 40.0f}, floor);
    b.save("Main");
}

void buildThirdPerson(project::ProjectInfo& project) {
    Builder b(project);
    b.world.setSceneUuid(Uuid::generate());
    ecs::populateDefaultScene(b.world);
    b.scripts(template_scripts::kPlayer);
    b.scripts(template_scripts::kThirdPersonCamera);
    b.scripts(template_scripts::kThirdPerson);

    const assets::AssetRef floor = b.material("Suelo", Vec3{0.58f, 0.60f, 0.63f}, 0.9f);
    const assets::AssetRef wall = b.material("Muro", Vec3{0.34f, 0.37f, 0.42f}, 0.8f);
    const assets::AssetRef orange = b.material("Plataforma naranja", Vec3{0.95f, 0.52f, 0.18f}, 0.55f);
    const assets::AssetRef blue = b.material("Plataforma azul", Vec3{0.16f, 0.50f, 0.92f}, 0.45f);
    const assets::AssetRef body = b.material("Jugador", Vec3{0.10f, 0.72f, 0.95f}, 0.35f);
    const assets::AssetRef visor = b.material("Visor", Vec3{0.05f, 0.06f, 0.08f}, 0.15f, 0.6f);
    const assets::AssetRef gold = b.material("Moneda", Vec3{1.0f, 0.78f, 0.22f}, 0.25f, 1.0f,
                                             Vec3{1.0f, 0.65f, 0.1f}, 0.6f);

    b.box("Suelo", Vec3{0.0f, -0.5f, 0.0f}, Vec3{60.0f, 1.0f, 60.0f}, floor);
    b.ring(30.0f, 2.5f, 1.0f, wall);
    // Escalera de bloques y una rampa hasta una plataforma alta.
    for (int i = 0; i < 5; ++i) {
        const float h = 0.5f * static_cast<float>(i + 1);
        b.box("Escalon " + std::to_string(i + 1), Vec3{8.0f + 2.5f * static_cast<float>(i), h * 0.5f, -6.0f},
              Vec3{2.5f, h, 3.0f}, i % 2 == 0 ? orange : blue);
    }
    b.box("Plataforma alta", Vec3{-10.0f, 2.5f, -12.0f}, Vec3{8.0f, 0.6f, 8.0f}, blue);
    b.box("Rampa", Vec3{-10.0f, 1.2f, -3.2f}, Vec3{3.0f, 0.4f, 10.0f}, orange, Vec3{-14.5f, 0.0f, 0.0f});
    b.box("Columna 1", Vec3{-14.0f, 2.0f, 10.0f}, Vec3{1.5f, 4.0f, 1.5f}, wall);
    b.box("Columna 2", Vec3{-6.0f, 2.0f, 14.0f}, Vec3{1.5f, 4.0f, 1.5f}, wall);
    b.box("Bloque", Vec3{12.0f, 1.0f, 12.0f}, Vec3{4.0f, 2.0f, 4.0f}, blue, Vec3{0.0f, 30.0f, 0.0f});

    const Vec3 coins[] = {{8.0f, 1.4f, -6.0f},   {13.0f, 2.4f, -6.0f}, {18.0f, 3.4f, -6.0f}, {-10.0f, 3.6f, -12.0f},
                          {-12.0f, 3.6f, -14.0f}, {12.0f, 3.0f, 12.0f}, {-10.0f, 1.0f, 10.0f}, {4.0f, 1.0f, 18.0f}};
    int n = 0;
    for (const Vec3& p : coins) {
        ecs::Entity c = ecs::createPrimitive(b.world, assets::builtin::kCylinder, "Moneda " + std::to_string(++n));
        c.setWorldPosition(p);
        c.setLocalEulerDegrees(Vec3{90.0f, 0.0f, 0.0f});
        c.setLocalScale(Vec3{0.6f, 0.08f, 0.6f});
        c.get<ecs::MeshRenderer>().materials = {gold};
        c.setTag("Moneda");
        physics::SphereCollider& trigger = c.add<physics::SphereCollider>();
        trigger.radius = 0.9f;
        trigger.material.is_trigger = true;
        Builder::attach(c, "Moneda.cpp");
    }

    b.player(Vec3{0.0f, 1.1f, 8.0f}, body, visor);
    b.camera(7.0f, 18.0f);
    b.hud("Marcador.cpp");
    b.save("Main");

    std::vector<std::string> tags = ecs::defaultTags();
    tags.push_back("Moneda");
    ecs::saveTags(project.settingsFolder() / "Tags.json", tags);
}

void buildNavigation(project::ProjectInfo& project) {
    Builder b(project);
    b.world.setSceneUuid(Uuid::generate());
    ecs::populateDefaultScene(b.world);
    b.scripts(template_scripts::kPlayer);
    b.scripts(template_scripts::kThirdPersonCamera);
    b.scripts(template_scripts::kNavigation);

    const assets::AssetRef floor = b.material("Suelo", Vec3{0.46f, 0.50f, 0.44f}, 0.9f);
    const assets::AssetRef wall = b.material("Muro", Vec3{0.72f, 0.70f, 0.66f}, 0.8f);
    const assets::AssetRef crate = b.material("Caja", Vec3{0.62f, 0.42f, 0.24f}, 0.75f);
    const assets::AssetRef body = b.material("Jugador", Vec3{0.10f, 0.72f, 0.95f}, 0.35f);
    const assets::AssetRef visor = b.material("Visor", Vec3{0.05f, 0.06f, 0.08f}, 0.15f, 0.6f);
    const assets::AssetRef guard = b.material("Guardia", Vec3{0.88f, 0.18f, 0.16f}, 0.4f);
    const assets::AssetRef eye = b.material("Ojo", Vec3{1.0f, 0.9f, 0.3f}, 0.3f, 0.0f, Vec3{1.0f, 0.8f, 0.2f}, 3.0f);
    const assets::AssetRef goal = b.material("Meta", Vec3{0.3f, 1.0f, 0.45f}, 0.2f, 0.0f, Vec3{0.3f, 1.0f, 0.45f}, 4.0f);

    b.box("Suelo", Vec3{0.0f, -0.5f, 0.0f}, Vec3{52.0f, 1.0f, 52.0f}, floor);
    b.ring(25.0f, 3.0f, 1.0f, wall);
    // Un laberinto sencillo: muros con huecos y cajas para esconderse.
    b.box("Muro A", Vec3{-8.0f, 1.5f, 12.0f}, Vec3{26.0f, 3.0f, 1.0f}, wall);
    b.box("Muro B", Vec3{10.0f, 1.5f, 2.0f}, Vec3{22.0f, 3.0f, 1.0f}, wall);
    b.box("Muro C", Vec3{-12.0f, 1.5f, -6.0f}, Vec3{18.0f, 3.0f, 1.0f}, wall);
    b.box("Muro D", Vec3{4.0f, 1.5f, -14.0f}, Vec3{1.0f, 3.0f, 18.0f}, wall);
    b.box("Muro E", Vec3{-3.0f, 1.5f, 4.0f}, Vec3{1.0f, 3.0f, 10.0f}, wall);
    const Vec3 crates[] = {{16.0f, 0.75f, 16.0f}, {-18.0f, 0.75f, 3.0f}, {14.0f, 0.75f, -8.0f}, {-6.0f, 0.75f, -16.0f}};
    int n = 0;
    for (const Vec3& p : crates) {
        ++n;
        b.box("Caja " + std::to_string(n), p, Vec3{1.5f, 1.5f, 1.5f}, crate, Vec3{0.0f, 20.0f * static_cast<float>(n), 0.0f});
    }

    // La malla de navegacion: todo el nivel.
    ecs::Entity volume = b.world.create("NavMeshBoundsVolume");
    volume.setWorldPosition(Vec3{0.0f, 2.0f, 0.0f});
    volume.add<navigation::NavMeshBounds>().size = Vec3{52.0f, 8.0f, 52.0f};

    const Vec3 guards[] = {{8.0f, 1.0f, 8.0f}, {-14.0f, 1.0f, 0.0f}, {14.0f, 1.0f, -16.0f}};
    n = 0;
    for (const Vec3& p : guards) {
        ecs::Entity g = b.world.create("Guardia " + std::to_string(++n));
        g.setWorldPosition(p);
        navigation::NavAgent& agent = g.add<navigation::NavAgent>();
        agent.speed = 3.6f;
        agent.base_offset = 1.0f;  // pivote en el centro de la capsula
        Builder::attach(g, "Guardia.cpp");
        ecs::Entity mesh = ecs::createPrimitive(b.world, assets::builtin::kCapsule, "Cuerpo", g);
        mesh.get<ecs::MeshRenderer>().materials = {guard};
        ecs::Entity e = ecs::createPrimitive(b.world, assets::builtin::kCube, "Ojo", g);
        e.setLocalPosition(Vec3{0.0f, 0.5f, -0.4f});
        e.setLocalScale(Vec3{0.55f, 0.16f, 0.2f});
        e.get<ecs::MeshRenderer>().materials = {eye};
        // Luz roja para verlos venir.
        ecs::Entity light = ecs::createLight(b.world, ecs::LightType::Point, g);
        light.setLocalPosition(Vec3{0.0f, 1.4f, 0.0f});
        if (auto* l = light.tryGet<ecs::Light>()) {
            l->color = Vec3{1.0f, 0.25f, 0.2f};
            l->intensity = 6.0f;
            l->range = 6.0f;
            l->cast_shadows = false;  // se mueven: sin sombras (mas barato)
        }
    }

    ecs::Entity meta = ecs::createPrimitive(b.world, assets::builtin::kCube, "Meta");
    meta.setWorldPosition(Vec3{19.0f, 1.0f, -20.0f});
    meta.setLocalScale(Vec3{1.4f, 1.4f, 1.4f});
    meta.setLocalEulerDegrees(Vec3{45.0f, 0.0f, 45.0f});
    meta.get<ecs::MeshRenderer>().materials = {goal};
    physics::SphereCollider& trigger = meta.add<physics::SphereCollider>();
    trigger.radius = 1.4f;
    trigger.material.is_trigger = true;
    Builder::attach(meta, "Meta.cpp");

    b.player(Vec3{-20.0f, 1.1f, 20.0f}, body, visor);
    b.camera(13.0f, 50.0f);
    b.hud("Marcador.cpp");
    b.save("Main");

    navigation::NavigationSettings nav;
    navigation::saveNavigationSettings(project.settingsFolder() / "Navigation.json", nav);
}

// --- IA con maquinas de estados ----------------------------------------------------

void buildStateMachines(project::ProjectInfo& project) {
    Builder b(project);
    b.world.setSceneUuid(Uuid::generate());
    ecs::populateDefaultScene(b.world);
    b.scripts(template_scripts::kPlayer);
    b.scripts(template_scripts::kThirdPersonCamera);
    b.scripts(template_scripts::kStateMachines);

    // La maquina de estados del enemigo (la de ejemplo del motor).
    ai::StateMachineAsset machine = ai::exampleEnemyStateMachine();
    machine.uuid = Uuid::generate();
    const std::filesystem::path machine_path = project.assetsFolder() / "IA" / "Enemigo.crfsm";
    std::filesystem::create_directories(machine_path.parent_path());
    std::string error;
    if (!ai::saveStateMachine(machine, machine_path, &error)) throw std::runtime_error("maquina de estados: " + error);

    const assets::AssetRef floor = b.material("Suelo", Vec3{0.44f, 0.48f, 0.46f}, 0.9f);
    const assets::AssetRef wall = b.material("Muro", Vec3{0.70f, 0.68f, 0.64f}, 0.8f);
    const assets::AssetRef body = b.material("Jugador", Vec3{0.10f, 0.72f, 0.95f}, 0.35f);
    const assets::AssetRef visor = b.material("Visor", Vec3{0.05f, 0.06f, 0.08f}, 0.15f, 0.6f);
    const assets::AssetRef enemy = b.material("Enemigo", Vec3{0.85f, 0.30f, 0.12f}, 0.4f);
    const assets::AssetRef eye = b.material("Ojo", Vec3{1.0f, 0.9f, 0.3f}, 0.3f, 0.0f, Vec3{1.0f, 0.8f, 0.2f}, 3.0f);
    const assets::AssetRef marker = b.material("Waypoint", Vec3{0.3f, 0.6f, 1.0f}, 0.3f, 0.0f, Vec3{0.3f, 0.6f, 1.0f}, 2.0f);

    b.box("Suelo", Vec3{0.0f, -0.5f, 0.0f}, Vec3{44.0f, 1.0f, 44.0f}, floor);
    b.ring(21.0f, 2.5f, 1.0f, wall);
    b.box("Muro A", Vec3{-6.0f, 1.25f, 6.0f}, Vec3{14.0f, 2.5f, 1.0f}, wall);
    b.box("Muro B", Vec3{8.0f, 1.25f, -6.0f}, Vec3{1.0f, 2.5f, 12.0f}, wall);

    ecs::Entity volume = b.world.create("NavMeshBoundsVolume");
    volume.setWorldPosition(Vec3{0.0f, 2.0f, 0.0f});
    volume.add<navigation::NavMeshBounds>().size = Vec3{44.0f, 8.0f, 44.0f};

    // Ruta de patrulla: objetos con el tag "Waypoint" (en orden de nombre).
    const Vec3 points[] = {{-14.0f, 0.1f, -14.0f}, {14.0f, 0.1f, -14.0f}, {14.0f, 0.1f, 14.0f}, {-14.0f, 0.1f, 14.0f}};
    int n = 0;
    for (const Vec3& p : points) {
        ecs::Entity w = ecs::createPrimitive(b.world, assets::builtin::kCylinder, "Punto " + std::to_string(++n));
        w.setWorldPosition(p);
        w.setLocalScale(Vec3{0.6f, 0.05f, 0.6f});
        w.get<ecs::MeshRenderer>().materials = {marker};
        w.setTag("Waypoint");
    }

    // Dos enemigos con NavAgent y la misma maquina (el segundo ve mas lejos).
    const Vec3 spawns[] = {{-10.0f, 1.0f, -10.0f}, {10.0f, 1.0f, 10.0f}};
    n = 0;
    for (const Vec3& p : spawns) {
        ecs::Entity g = b.world.create("Enemigo " + std::to_string(++n));
        g.setWorldPosition(p);
        g.setTag("Enemigo");
        navigation::NavAgent& agent = g.add<navigation::NavAgent>();
        agent.speed = 3.4f;
        agent.base_offset = 1.0f;  // pivote en el centro de la capsula
        Builder::attach(g, "Enemigo.cpp");  // hace cada estado (OnStateEnter / OnStateUpdate)
        ai::StateMachine& sm = g.add<ai::StateMachine>();
        sm.machine = assets::AssetRef{machine.uuid, assets::AssetType::StateMachine};
        sm.debug = n == 1;  // sus cambios de estado salen en la Consola
        if (n == 2) sm.variables.push_back(ai::VariableOverride{"rangoVision", static_cast<int>(ai::VarType::Float), "14"});
        ecs::Entity mesh = ecs::createPrimitive(b.world, assets::builtin::kCapsule, "Cuerpo", g);
        mesh.get<ecs::MeshRenderer>().materials = {enemy};
        ecs::Entity e = ecs::createPrimitive(b.world, assets::builtin::kCube, "Ojo", g);
        e.setLocalPosition(Vec3{0.0f, 0.5f, -0.4f});
        e.setLocalScale(Vec3{0.55f, 0.16f, 0.2f});
        e.get<ecs::MeshRenderer>().materials = {eye};
    }

    ecs::Entity player = b.player(Vec3{0.0f, 1.1f, 16.0f}, body, visor);
    ecs::Entity strike = b.world.create("Golpe", player);
    Builder::attach(strike, "Golpe.cpp");
    b.camera(12.0f, 45.0f);
    ecs::Entity hud = b.hud("Estados.cpp");
    if (auto* rt = hud.tryGet<ui::RectTransform>()) rt->size = Vec2{1000.0f, 200.0f};
    if (auto* t = hud.tryGet<ui::Text>()) t->font_size = 28.0f;
    b.save("Main");

    navigation::NavigationSettings nav;
    navigation::saveNavigationSettings(project.settingsFolder() / "Navigation.json", nav);
}

// --- Recursos del mundo de bloques (iconos pixel art, grietas) ---------------------

using Rgba = std::array<std::uint8_t, 4>;

// Un icono de 8 x 8 "pixeles" ampliado a `size` (sin suavizar, como Minecraft).
void pixelIcon(const std::filesystem::path& file, const std::vector<std::string>& rows,
               const std::map<char, Rgba>& palette, std::uint32_t size = 64) {
    asset::ImageRgba8 image;
    image.width = image.height = size;
    image.pixels.assign(static_cast<std::size_t>(size) * size * 4, 0);
    const std::size_t n = rows.size();
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const std::size_t r = y * n / size, c = x * n / size;
            const char key = c < rows[r].size() ? rows[r][c] : '.';
            const auto it = palette.find(key);
            if (it == palette.end()) continue;
            std::memcpy(&image.pixels[(static_cast<std::size_t>(y) * size + x) * 4], it->second.data(), 4);
        }
    }
    std::filesystem::create_directories(file.parent_path());
    if (!asset::saveImagePng(file, image)) throw std::runtime_error("no se pudo escribir " + file.string());
}

void writeVoxelIcons(const std::filesystem::path& folder) {
    const Rgba red{214, 32, 40, 255}, light{255, 170, 170, 255}, dark{60, 20, 24, 255}, empty{70, 70, 76, 200};
    const std::vector<std::string> heart = {".RR..RR.", "RWRRRRRR", "RRRRRRRR", "RRRRRRRR",
                                            ".RRRRRR.", "..RRRR..", "...RR...", "........"};
    pixelIcon(folder / "corazon.png", heart, {{'R', red}, {'W', light}});
    pixelIcon(folder / "corazon_medio.png", {".RR..EE.", "RWRREEEE", "RRRREEEE", "RRRREEEE", ".RRREEE.", "..RREE..", "...RE...", "........"},
              {{'R', red}, {'W', light}, {'E', empty}});
    pixelIcon(folder / "corazon_vacio.png", heart, {{'R', empty}, {'W', empty}});
    (void)dark;

    const Rgba meat{190, 90, 40, 255}, bone{235, 225, 205, 255}, meat_dark{120, 50, 20, 255};
    const std::vector<std::string> food = {"......BB", ".....BWB", "....BBB.", "..MMMB..",
                                           ".MMMMM..", ".MMMMD..", ".MMMD...", "..MM...."};
    pixelIcon(folder / "comida.png", food, {{'M', meat}, {'B', bone}, {'W', bone}, {'D', meat_dark}});
    pixelIcon(folder / "comida_media.png", {"......EE", ".....EEE", "....EEE.", "..MMEE..", ".MMMEE..", ".MMMEE..", ".MMEE...", "..ME...."},
              {{'M', meat}, {'E', empty}});
    pixelIcon(folder / "comida_vacia.png", food, {{'M', empty}, {'B', empty}, {'W', empty}, {'D', empty}});

    const Rgba bubble{150, 210, 255, 230}, shine{255, 255, 255, 255};
    pixelIcon(folder / "aire.png", {"..BBBB..", ".B....B.", "B.W....B", "B......B", "B......B", "B......B", ".B....B.", "..BBBB.."},
              {{'B', bubble}, {'W', shine}});

    const Rgba wood{176, 128, 72, 255}, wood_dark{110, 76, 40, 255};
    pixelIcon(folder / "palo.png", {".......W", "......WB", ".....WB.", "....WB..", "...WB...", "..WB....", ".WB.....", "WB......"},
              {{'W', wood}, {'B', wood_dark}});
    const Rgba coal{30, 30, 34, 255}, coal_shine{90, 90, 100, 255};
    pixelIcon(folder / "carbon.png", {"........", "..KKK...", ".KKGKK..", ".KKKKKK.", ".KGKKKK.", "..KKKGK.", "...KKK..", "........"},
              {{'K', coal}, {'G', coal_shine}});
    const Rgba apple{210, 30, 30, 255}, apple_shine{255, 140, 140, 255}, leaf{70, 160, 50, 255}, stem{100, 70, 40, 255};
    pixelIcon(folder / "manzana.png", {"....L...", "...LS...", ".RRRSRR.", "RRWRRRRR", "RWRRRRRR", "RRRRRRRR", ".RRRRRR.", "..RR.RR."},
              {{'R', apple}, {'W', apple_shine}, {'L', leaf}, {'S', stem}});
    const std::vector<std::string> pick = {".HHHHHH.", "H..WB..H", "...WB...", "...WB...", "...WB...", "...WB...", "...WB...", "...WB..."};
    pixelIcon(folder / "pico_madera.png", pick, {{'H', {150, 108, 60, 255}}, {'W', wood}, {'B', wood_dark}});
    pixelIcon(folder / "pico_piedra.png", pick, {{'H', {128, 128, 132, 255}}, {'W', wood}, {'B', wood_dark}});
}

// Diez etapas de grietas (64 x 64 con alfa): mas lineas y mas largas cada vez.
void writeCrackTextures(const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    constexpr std::uint32_t kSize = 64;
    constexpr int kCell = 2;  // lineas de 2 pixeles
    constexpr int kGrid = static_cast<int>(kSize) / kCell;
    std::vector<std::uint8_t> cracked(kGrid * kGrid, 0);
    std::uint32_t seed = 1234567u;
    const auto rnd = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
    };
    for (int stage = 0; stage < 10; ++stage) {
        // Cada etapa anade grietas a las de la anterior (se van extendiendo).
        // Una o dos grietas nuevas desde cerca del centro hacia fuera.
        const int lines = 1 + (stage % 2);
        for (int l = 0; l < lines; ++l) {
            float x = kGrid * 0.5f + (rnd() - 0.5f) * 6.0f, y = kGrid * 0.5f + (rnd() - 0.5f) * 6.0f;
            float angle = rnd() * 6.2831853f;
            const int steps = 6 + stage * 2;
            for (int s = 0; s < steps; ++s) {
                const int cx = static_cast<int>(x), cy = static_cast<int>(y);
                if (cx >= 0 && cy >= 0 && cx < kGrid && cy < kGrid) cracked[static_cast<std::size_t>(cy * kGrid + cx)] = 1;
                angle += (rnd() - 0.5f) * 1.1f;
                x += std::cos(angle);
                y += std::sin(angle);
            }
        }
        asset::ImageRgba8 image;
        image.width = image.height = kSize;
        image.pixels.assign(static_cast<std::size_t>(kSize) * kSize * 4, 0);
        for (std::uint32_t py = 0; py < kSize; ++py) {
            for (std::uint32_t px = 0; px < kSize; ++px) {
                if (!cracked[static_cast<std::size_t>((py / kCell) * kGrid + px / kCell)]) continue;
                std::uint8_t* p = &image.pixels[(static_cast<std::size_t>(py) * kSize + px) * 4];
                p[0] = 24;
                p[1] = 22;
                p[2] = 20;
                p[3] = 235;
            }
        }
        if (!asset::saveImagePng(folder / ("grieta_" + std::to_string(stage) + ".png"), image)) {
            throw std::runtime_error("no se pudo escribir las grietas");
        }
    }
}

void buildVoxel(project::ProjectInfo& project) {
    voxel::registerVoxelComponents();
    Builder b(project);
    b.world.setSceneUuid(Uuid::generate());
    ecs::populateDefaultScene(b.world);
    b.scripts(template_scripts::kVoxel);

    // Iconos de todos los bloques, de los objetos y de la interfaz; grietas.
    const std::filesystem::path icons = project.assetsFolder() / "Voxel" / "Iconos";
    if (!voxel::writeBlockIcons(icons, 64)) throw std::runtime_error("no se pudieron escribir los iconos de los bloques");
    writeVoxelIcons(icons);
    writeCrackTextures(project.assetsFolder() / "Voxel" / "Grietas");

    // El mundo infinito y su mar (el oceano del motor en la superficie del agua).
    ecs::Entity world = b.world.create("Mundo de bloques");
    const voxel::VoxelWorld& settings = world.add<voxel::VoxelWorld>();
    ecs::Entity sea = b.world.create("Mar", world);
    sea.add<water::WaterBody>() = water::oceanPreset();
    sea.setWorldPosition(Vec3{0.0f, std::round(settings.sea_level) - 0.1f, 0.0f});

    // La camara es el jugador (primera persona); sus botones llaman a su script.
    ecs::Entity cam = b.world.findByName("Main Camera");
    cam.setWorldPosition(Vec3{0.5f, 100.0f, 0.5f});
    Builder::attach(cam, "JugadorBloques.cpp");
    const Uuid player = cam.uuid();

    // --- Interfaz ---
    ecs::Entity canvas = b.world.create("HUD");
    canvas.add<ui::Canvas>();
    const auto rect = [&](ecs::Entity e, Vec2 anchor, Vec2 pivot, Vec2 position, Vec2 size) {
        ui::RectTransform& rt = e.add<ui::RectTransform>();
        rt.anchor_min = anchor;
        rt.anchor_max = anchor;
        rt.pivot = pivot;
        rt.position = position;
        rt.size = size;
        return e;
    };
    const auto stretch = [&](ecs::Entity e) {
        ui::RectTransform& rt = e.add<ui::RectTransform>();
        rt.anchor_min = Vec2{0.0f, 0.0f};
        rt.anchor_max = Vec2{1.0f, 1.0f};
        rt.size = Vec2{0.0f, 0.0f};
        return e;
    };
    const auto image = [&](ecs::Entity e, Vec3 color, float alpha, float radius = 0.0f, const std::string& texture = {}) {
        ui::Image& im = e.add<ui::Image>();
        im.color = color;
        im.alpha = alpha;
        im.corner_radius = radius;
        im.texture = texture;
        return e;
    };
    const auto text = [&](ecs::Entity e, const std::string& value, float font, ui::HAlign align, Vec3 color = Vec3{1, 1, 1}) {
        ui::Text& t = e.add<ui::Text>();
        t.text = value;
        t.font_size = font;
        t.h_align = align;
        t.color = color;
        t.shadow = true;
        return e;
    };
    const auto button = [&](ecs::Entity e, const std::string& method, Vec3 color) {
        ui::Button& bt = e.add<ui::Button>();
        bt.normal = color;
        bt.hover = color * 1.35f;
        bt.pressed = color * 0.7f;
        bt.disabled = Vec3{0.16f, 0.16f, 0.17f};
        bt.corner_radius = 6.0f;
        bt.target = player;
        bt.on_click = method;
        return e;
    };
    // Un hueco de objeto: fondo, icono y cantidad.
    const auto slot = [&](ecs::Entity e, float icon_size) {
        ecs::Entity icon = b.world.create("Icono", e);
        rect(icon, Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f}, Vec2{0.0f, 0.0f}, Vec2{icon_size, icon_size});
        image(icon, Vec3{1, 1, 1}, 0.0f);
        ecs::Entity count = b.world.create("Cantidad", e);
        rect(count, Vec2{1.0f, 1.0f}, Vec2{1.0f, 1.0f}, Vec2{-4.0f, -1.0f}, Vec2{60.0f, 28.0f});
        text(count, "", 22.0f, ui::HAlign::Right);
    };

    // Mira, avisos y datos.
    text(rect(b.world.create("Mira", canvas), Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f}, Vec2{0, 0}, Vec2{60, 60}), "+", 40.0f, ui::HAlign::Center);
    text(rect(b.world.create("Info", canvas), Vec2{0, 0}, Vec2{0, 0}, Vec2{32, 24}, Vec2{1300, 140}), "", 24.0f, ui::HAlign::Left);
    ecs::Entity aviso = text(rect(b.world.create("Aviso", canvas), Vec2{0.5f, 0.0f}, Vec2{0.5f, 0.0f}, Vec2{0, 150}, Vec2{1200, 50}), "",
                             30.0f, ui::HAlign::Center, Vec3{1.0f, 0.92f, 0.55f});
    aviso.get<ui::Text>().alpha = 0.0f;
    // Destello rojo al recibir dano (toda la pantalla).
    image(stretch(b.world.create("Dano", canvas)), Vec3{0.8f, 0.05f, 0.05f}, 0.0f);

    // Barra de 9 huecos, el marco del elegido, vida, hambre y aire.
    image(rect(b.world.create("BarraFondo", canvas), Vec2{0.5f, 1.0f}, Vec2{0.5f, 1.0f}, Vec2{0, -16}, Vec2{9 * 84 + 16, 96}),
          Vec3{0.05f, 0.05f, 0.06f}, 0.55f, 10.0f);
    image(rect(b.world.create("Seleccion", canvas), Vec2{0.5f, 1.0f}, Vec2{0.5f, 1.0f}, Vec2{-4 * 84, -20}, Vec2{84, 84}),
          Vec3{0.95f, 0.95f, 0.9f}, 0.95f, 8.0f);
    for (int i = 1; i <= 9; ++i) {
        ecs::Entity s = b.world.create("Hueco " + std::to_string(i), canvas);
        rect(s, Vec2{0.5f, 1.0f}, Vec2{0.5f, 1.0f}, Vec2{static_cast<float>((i - 5) * 84), -24}, Vec2{76, 76});
        image(s, Vec3{0.14f, 0.14f, 0.16f}, 0.9f, 6.0f);
        slot(s, 56.0f);
    }
    for (int i = 1; i <= 10; ++i) {
        const float step = static_cast<float>(i - 1) * 32.0f;
        image(rect(b.world.create("Corazon " + std::to_string(i), canvas), Vec2{0.5f, 1.0f}, Vec2{0.5f, 1.0f},
                   Vec2{-374.0f + 15.0f + step, -118}, Vec2{30, 30}),
              Vec3{1, 1, 1}, 1.0f, 0.0f, "Voxel/Iconos/corazon.png");
        image(rect(b.world.create("Comida " + std::to_string(i), canvas), Vec2{0.5f, 1.0f}, Vec2{0.5f, 1.0f},
                   Vec2{374.0f - 15.0f - step, -118}, Vec2{30, 30}),
              Vec3{1, 1, 1}, 1.0f, 0.0f, "Voxel/Iconos/comida.png");
        ecs::Entity bubble = image(rect(b.world.create("Aire " + std::to_string(i), canvas), Vec2{0.5f, 1.0f}, Vec2{0.5f, 1.0f},
                                        Vec2{374.0f - 15.0f - step, -152}, Vec2{28, 28}),
                                   Vec3{1, 1, 1}, 1.0f, 0.0f, "Voxel/Iconos/aire.png");
        bubble.setActive(false);
    }
    ecs::Entity item_name = text(rect(b.world.create("NombreObjeto", canvas), Vec2{0.5f, 1.0f}, Vec2{0.5f, 1.0f}, Vec2{0, -190},
                                      Vec2{800, 40}),
                                 "", 28.0f, ui::HAlign::Center);
    item_name.get<ui::Text>().alpha = 0.0f;

    // Inventario y crafteo (E).
    ecs::Entity inventory = b.world.create("Inventario", canvas);
    rect(inventory, Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f}, Vec2{0, -20}, Vec2{1400, 780});
    image(inventory, Vec3{0.07f, 0.08f, 0.1f}, 0.94f, 14.0f);
    text(rect(b.world.create("Titulo", inventory), Vec2{0, 0}, Vec2{0, 0}, Vec2{40, 22}, Vec2{600, 48}), "Inventario", 34.0f,
         ui::HAlign::Left);
    text(rect(b.world.create("TituloBarra", inventory), Vec2{0, 0}, Vec2{0, 0}, Vec2{40, 342}, Vec2{600, 30}), "Barra (1-9)", 20.0f,
         ui::HAlign::Left, Vec3{0.7f, 0.72f, 0.78f});
    for (int i = 1; i <= 36; ++i) {
        const int index = i <= 9 ? i - 1 : i - 10;
        const float x = 40.0f + static_cast<float>(index % 9) * 80.0f;
        const float y = i <= 9 ? 376.0f : 86.0f + static_cast<float>(index / 9) * 80.0f;
        ecs::Entity s = b.world.create("Inv " + std::to_string(i), inventory);
        rect(s, Vec2{0, 0}, Vec2{0, 0}, Vec2{x, y}, Vec2{72, 72});
        button(s, "OnHueco", Vec3{0.18f, 0.19f, 0.22f});
        slot(s, 52.0f);
    }
    text(rect(b.world.create("Ayuda", inventory), Vec2{0, 1}, Vec2{0, 1}, Vec2{40, -24}, Vec2{720, 90}),
         "Clic en un hueco: lo cambia por el elegido de la barra.\nE o Escape: cerrar.", 20.0f, ui::HAlign::Left,
         Vec3{0.7f, 0.72f, 0.78f});
    text(rect(b.world.create("TituloCrafteo", inventory), Vec2{0, 0}, Vec2{0, 0}, Vec2{790, 22}, Vec2{570, 48}), "Crafteo", 34.0f,
         ui::HAlign::Left);
    for (int i = 1; i <= 13; ++i) {
        ecs::Entity r = b.world.create("Receta " + std::to_string(i), inventory);
        rect(r, Vec2{0, 0}, Vec2{0, 0}, Vec2{790, 86.0f + static_cast<float>(i - 1) * 50.0f}, Vec2{580, 44});
        button(r, "OnReceta", Vec3{0.16f, 0.36f, 0.24f});
        image(rect(b.world.create("Icono", r), Vec2{0.0f, 0.5f}, Vec2{0.0f, 0.5f}, Vec2{8, 0}, Vec2{34, 34}), Vec3{1, 1, 1}, 1.0f);
        text(rect(b.world.create("Texto", r), Vec2{0.0f, 0.5f}, Vec2{0.0f, 0.5f}, Vec2{52, 0}, Vec2{520, 40}), "", 19.0f,
             ui::HAlign::Left);
    }
    inventory.setActive(false);

    // Muerte y pausa.
    ecs::Entity death = b.world.create("Muerte", canvas);
    image(stretch(death), Vec3{0.35f, 0.02f, 0.02f}, 0.6f);
    text(rect(b.world.create("Titulo", death), Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f}, Vec2{0, -110}, Vec2{900, 90}), "¡Has muerto!",
         72.0f, ui::HAlign::Center);
    text(rect(b.world.create("Causa", death), Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f}, Vec2{0, -30}, Vec2{900, 50}), "", 30.0f,
         ui::HAlign::Center, Vec3{1.0f, 0.8f, 0.8f});
    ecs::Entity respawn = b.world.create("Reaparecer", death);
    rect(respawn, Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f}, Vec2{0, 70}, Vec2{340, 66});
    button(respawn, "OnReaparecer", Vec3{0.5f, 0.12f, 0.12f});
    text(stretch(b.world.create("Texto", respawn)), "Reaparecer", 30.0f, ui::HAlign::Center);
    death.setActive(false);

    ecs::Entity pause = b.world.create("Pausa", canvas);
    image(stretch(pause), Vec3{0.02f, 0.03f, 0.05f}, 0.6f);
    text(rect(b.world.create("Titulo", pause), Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f}, Vec2{0, -170}, Vec2{600, 90}), "Pausa", 64.0f,
         ui::HAlign::Center);
    const char* pause_buttons[][2] = {{"Seguir", "OnSeguir"}, {"Guardar", "OnGuardar"}, {"Salir", "OnSalir"}};
    for (int i = 0; i < 3; ++i) {
        ecs::Entity bt = b.world.create(pause_buttons[i][0], pause);
        rect(bt, Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f}, Vec2{0, -40.0f + static_cast<float>(i) * 84.0f}, Vec2{360, 66});
        button(bt, pause_buttons[i][1], Vec3{0.16f, 0.36f, 0.72f});
        const std::string label = i == 1 ? "Guardar el mundo" : (i == 2 ? "Guardar y salir" : "Seguir jugando");
        text(stretch(b.world.create("Texto", bt)), label, 28.0f, ui::HAlign::Center);
    }
    pause.setActive(false);
    b.save("Main");
}

// --- Plantilla MMO RPG ----------------------------------------------------------
// El motor solo coloca el mundo (pueblo, bosque, campamento, guarida), los
// personajes y la interfaz; todo lo que pasa lo hacen los scripts de C++
// (TemplateMmoScripts.h).

// Numeros al azar repetibles (el mismo mundo en cada proyecto nuevo).
struct Lcg {
    std::uint32_t state;
    float next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>((state >> 8) & 0xFFFFFF) / 16777216.0f;
    }
    float range(float lo, float hi) { return lo + (hi - lo) * next(); }
};

void buildMmo(project::ProjectInfo& project) {
    Builder b(project);
    b.world.setSceneUuid(Uuid::generate());
    ecs::populateDefaultScene(b.world);
    b.scripts(mmo::kFiles);
    b.scripts(template_scripts::kThirdPersonCamera);

    const assets::AssetRef grass = b.material("Hierba", Vec3{0.36f, 0.52f, 0.27f}, 0.95f);
    const assets::AssetRef road = b.material("Camino", Vec3{0.55f, 0.46f, 0.34f}, 0.95f);
    const assets::AssetRef stone = b.material("Piedra", Vec3{0.58f, 0.57f, 0.55f}, 0.85f);
    const assets::AssetRef plaster = b.material("Pared", Vec3{0.86f, 0.80f, 0.68f}, 0.9f);
    const assets::AssetRef roof = b.material("Tejado", Vec3{0.62f, 0.24f, 0.17f}, 0.8f);
    const assets::AssetRef wood = b.material("Madera", Vec3{0.45f, 0.31f, 0.19f}, 0.85f);
    const assets::AssetRef dark_wood = b.material("Madera oscura", Vec3{0.25f, 0.17f, 0.11f}, 0.85f);
    const assets::AssetRef leaves = b.material("Hojas", Vec3{0.20f, 0.42f, 0.18f}, 0.9f);
    const assets::AssetRef rock = b.material("Roca", Vec3{0.34f, 0.33f, 0.32f}, 0.9f);
    const assets::AssetRef tent = b.material("Lona", Vec3{0.52f, 0.44f, 0.30f}, 0.95f);
    const assets::AssetRef fire = b.material("Fuego", Vec3{1.0f, 0.5f, 0.1f}, 0.5f, 0.0f, Vec3{1.0f, 0.45f, 0.1f}, 6.0f);
    const assets::AssetRef body = b.material("Jugador", Vec3{0.10f, 0.55f, 0.95f}, 0.35f);
    const assets::AssetRef visor = b.material("Visor", Vec3{0.05f, 0.06f, 0.08f}, 0.15f, 0.6f);
    const assets::AssetRef wolf = b.material("Lobo", Vec3{0.42f, 0.42f, 0.45f}, 0.8f);
    const assets::AssetRef goblin = b.material("Goblin", Vec3{0.35f, 0.62f, 0.25f}, 0.7f);
    const assets::AssetRef shaman = b.material("Chaman", Vec3{0.50f, 0.28f, 0.65f}, 0.6f);
    const assets::AssetRef king = b.material("Rey Goblin", Vec3{0.22f, 0.40f, 0.16f}, 0.6f);
    const assets::AssetRef gold = b.material("Oro", Vec3{1.0f, 0.78f, 0.25f}, 0.25f, 1.0f);
    const assets::AssetRef eye = b.material("Ojos", Vec3{1.0f, 0.2f, 0.1f}, 0.3f, 0.0f, Vec3{1.0f, 0.2f, 0.1f}, 4.0f);
    const assets::AssetRef bar_back = b.material("Barra fondo", Vec3{0.08f, 0.05f, 0.05f}, 0.9f);
    const assets::AssetRef bar_fill = b.material("Barra vida", Vec3{0.9f, 0.12f, 0.1f}, 0.5f, 0.0f, Vec3{0.9f, 0.1f, 0.08f}, 1.5f);
    b.material("Marca mision", Vec3{1.0f, 0.82f, 0.1f}, 0.3f, 0.0f, Vec3{1.0f, 0.78f, 0.1f}, 5.0f);
    b.material("Marca entregar", Vec3{0.3f, 0.65f, 1.0f}, 0.3f, 0.0f, Vec3{0.3f, 0.6f, 1.0f}, 5.0f);
    const assets::AssetRef quest_mark = assets::AssetRef{};  // lo pone NPC.cpp

    // Una caja sin collider (decoracion plana: caminos, plaza).
    const auto flat = [&](const std::string& name, const Vec3& p, const Vec3& s, const assets::AssetRef& m, float yaw = 0.0f) {
        ecs::Entity e = b.box(name, p, s, m, Vec3{0.0f, yaw, 0.0f});
        e.remove<physics::BoxCollider>();
        return e;
    };
    const auto primitive = [&](const Uuid& mesh, const std::string& name, ecs::Entity parent, const Vec3& p, const Vec3& s,
                               const assets::AssetRef& m, const Vec3& euler = Vec3{}) {
        ecs::Entity e = ecs::createPrimitive(b.world, mesh, name, parent);
        e.setLocalPosition(p);
        e.setLocalScale(s);
        e.setLocalEulerDegrees(euler);
        e.get<ecs::MeshRenderer>().materials = {m};
        return e;
    };
    const auto point_light = [&](const std::string& name, const Vec3& p, const Vec3& color, float intensity, float range) {
        ecs::Entity l = ecs::createLight(b.world, ecs::LightType::Point);
        l.setName(name);
        l.setWorldPosition(p);
        if (auto* light = l.tryGet<ecs::Light>()) {
            light->color = color;
            light->intensity = intensity;
            light->range = range;
            light->cast_shadows = false;
        }
        return l;
    };
    // Casa: paredes, tejado a dos aguas (una caja girada 45 grados) y puerta.
    const auto house = [&](const std::string& name, const Vec3& p, const Vec3& size, float yaw) {
        ecs::Entity h = b.box(name, p + Vec3{0.0f, size.y * 0.5f, 0.0f}, size, plaster, Vec3{0.0f, yaw, 0.0f});
        const float r = size.z * 0.72f;
        ecs::Entity t = primitive(assets::builtin::kCube, "Tejado", h, Vec3{0.0f, 0.5f, 0.0f},
                                  Vec3{1.08f, r / size.y, r / size.z}, roof, Vec3{45.0f, 0.0f, 0.0f});
        (void)t;
        primitive(assets::builtin::kCube, "Puerta", h, Vec3{0.0f, -0.5f + 1.1f / size.y, 0.505f},
                  Vec3{1.4f / size.x, 2.2f / size.y, 0.02f}, dark_wood);
        return h;
    };

    // --- Terreno y caminos ---
    b.box("Suelo", Vec3{0.0f, -0.5f, 0.0f}, Vec3{280.0f, 1.0f, 280.0f}, grass);
    b.ring(138.0f, 5.0f, 3.0f, rock);
    flat("Plaza", Vec3{0.0f, 0.01f, 0.0f}, Vec3{34.0f, 0.04f, 34.0f}, stone);
    flat("Camino oeste", Vec3{-38.0f, 0.02f, -20.0f}, Vec3{5.0f, 0.04f, 48.0f}, road, 62.0f);
    flat("Camino este", Vec3{38.0f, 0.02f, -32.0f}, Vec3{5.0f, 0.04f, 62.0f}, road, -48.0f);
    flat("Camino norte", Vec3{0.0f, 0.02f, -60.0f}, Vec3{5.0f, 0.04f, 88.0f}, road);

    // --- Villa Alba (el pueblo) ---
    ecs::Entity zone_town = b.world.create("Zona Pueblo");
    zone_town.setWorldPosition(Vec3{0.0f, 0.0f, 0.0f});
    house("Casa 1", Vec3{-22.0f, 0.0f, -14.0f}, Vec3{8.0f, 5.0f, 7.0f}, 90.0f);
    house("Casa 2", Vec3{-22.0f, 0.0f, 6.0f}, Vec3{8.0f, 5.0f, 7.0f}, 90.0f);
    house("Casa 3", Vec3{22.0f, 0.0f, -14.0f}, Vec3{8.0f, 5.0f, 7.0f}, -90.0f);
    house("Casa 4", Vec3{22.0f, 0.0f, 10.0f}, Vec3{9.0f, 5.5f, 7.0f}, -90.0f);
    house("Casa 5", Vec3{-8.0f, 0.0f, 24.0f}, Vec3{9.0f, 5.0f, 7.0f}, 180.0f);
    house("Posada", Vec3{10.0f, 0.0f, 25.0f}, Vec3{12.0f, 6.5f, 8.0f}, 180.0f);
    ecs::Entity chapel = house("Capilla", Vec3{-30.0f, 0.0f, 28.0f}, Vec3{10.0f, 7.0f, 14.0f}, 135.0f);
    primitive(assets::builtin::kCube, "Torre", chapel, Vec3{0.0f, 0.9f, -0.3f}, Vec3{0.3f, 0.9f, 0.22f}, plaster);
    b.box("Pozo", Vec3{0.0f, 0.5f, 0.0f}, Vec3{2.2f, 1.0f, 2.2f}, stone);
    primitive(assets::builtin::kCylinder, "Tejadillo del pozo", b.world.findByName("Pozo"), Vec3{0.0f, 2.2f, 0.0f},
              Vec3{0.8f, 0.08f, 0.8f}, wood);
    // Puestos del mercado.
    for (int i = 0; i < 3; ++i) {
        const float x = 7.0f + static_cast<float>(i) * 4.0f;
        b.box("Puesto " + std::to_string(i + 1), Vec3{x, 0.5f, 8.0f}, Vec3{3.0f, 1.0f, 1.4f}, wood);
        flat("Toldo " + std::to_string(i + 1), Vec3{x, 2.4f, 8.0f}, Vec3{3.4f, 0.1f, 2.0f}, i % 2 == 0 ? roof : tent);
    }
    // Farolas.
    const Vec3 lamps[] = {{-12, 0, -12}, {12, 0, -12}, {-12, 0, 12}, {12, 0, 12}};
    int lamp = 0;
    for (const Vec3& p : lamps) {
        ++lamp;
        b.box("Farola " + std::to_string(lamp), p + Vec3{0.0f, 1.6f, 0.0f}, Vec3{0.25f, 3.2f, 0.25f}, dark_wood);
        point_light("Luz farola " + std::to_string(lamp), p + Vec3{0.0f, 3.5f, 0.0f}, Vec3{1.0f, 0.78f, 0.45f}, 6.0f, 12.0f);
    }
    ecs::Entity spawn = b.world.create("Punto de reaparicion");
    spawn.setWorldPosition(Vec3{0.0f, 1.2f, 8.0f});

    // --- Bosque Gris (oeste): arboles y lobos ---
    const Vec3 forest{-68.0f, 0.0f, -40.0f};
    ecs::Entity zone_forest = b.world.create("Zona Bosque");
    zone_forest.setWorldPosition(forest);
    Lcg rng{12345u};
    for (int i = 0; i < 46; ++i) {
        const float angle = rng.range(0.0f, 6.2832f);
        const float dist = rng.range(4.0f, 36.0f);
        const Vec3 p = forest + Vec3{std::cos(angle) * dist, 0.0f, std::sin(angle) * dist};
        const float height = rng.range(4.0f, 7.0f);
        ecs::Entity tree = b.world.create("Arbol " + std::to_string(i + 1));
        tree.setWorldPosition(p);
        ecs::Entity trunk = primitive(assets::builtin::kCylinder, "Tronco", tree, Vec3{0.0f, height * 0.5f, 0.0f},
                                      Vec3{0.5f, height * 0.5f, 0.5f}, wood);
        trunk.add<physics::BoxCollider>();
        const float crown = rng.range(2.6f, 4.0f);
        primitive(assets::builtin::kSphere, "Copa", tree, Vec3{0.0f, height + crown * 0.3f, 0.0f},
                  Vec3{crown, crown * 1.1f, crown}, leaves);
    }

    // --- Campamento goblin (este): empalizada, tiendas y hoguera ---
    const Vec3 camp{66.0f, 0.0f, -66.0f};
    ecs::Entity zone_camp = b.world.create("Zona Campamento");
    zone_camp.setWorldPosition(camp);
    b.box("Empalizada N", camp + Vec3{0.0f, 1.5f, -22.0f}, Vec3{44.0f, 3.0f, 0.6f}, dark_wood);
    b.box("Empalizada S1", camp + Vec3{-14.0f, 1.5f, 22.0f}, Vec3{16.0f, 3.0f, 0.6f}, dark_wood);
    b.box("Empalizada S2", camp + Vec3{14.0f, 1.5f, 22.0f}, Vec3{16.0f, 3.0f, 0.6f}, dark_wood);
    b.box("Empalizada E", camp + Vec3{22.0f, 1.5f, 0.0f}, Vec3{0.6f, 3.0f, 44.0f}, dark_wood);
    b.box("Empalizada O1", camp + Vec3{-22.0f, 1.5f, -12.0f}, Vec3{0.6f, 3.0f, 20.0f}, dark_wood);
    b.box("Empalizada O2", camp + Vec3{-22.0f, 1.5f, 15.0f}, Vec3{0.6f, 3.0f, 14.0f}, dark_wood);
    const Vec3 tents[] = {{-12, 0, -12}, {10, 0, -14}, {12, 0, 8}, {-10, 0, 10}};
    int tent_n = 0;
    for (const Vec3& t : tents) {
        ++tent_n;
        ecs::Entity e = b.box("Tienda " + std::to_string(tent_n), camp + t + Vec3{0.0f, 1.2f, 0.0f}, Vec3{4.0f, 2.8f, 2.8f}, tent,
                              Vec3{45.0f, static_cast<float>(tent_n) * 40.0f, 0.0f});
        (void)e;
    }
    b.box("Hoguera", camp + Vec3{0.0f, 0.2f, 0.0f}, Vec3{1.6f, 0.4f, 1.6f}, rock);
    primitive(assets::builtin::kSphere, "Llamas", b.world.findByName("Hoguera"), Vec3{0.0f, 1.2f, 0.0f}, Vec3{0.5f, 1.4f, 0.5f}, fire);
    point_light("Luz hoguera", camp + Vec3{0.0f, 1.6f, 0.0f}, Vec3{1.0f, 0.55f, 0.2f}, 10.0f, 16.0f);

    // --- Guarida del Rey (norte): circulo de rocas y trono ---
    const Vec3 lair{0.0f, 0.0f, -118.0f};
    ecs::Entity zone_lair = b.world.create("Zona Guarida");
    zone_lair.setWorldPosition(lair);
    for (int i = 0; i < 12; ++i) {
        if (i == 3) continue;  // la entrada (hacia el camino, al sur)
        const float angle = static_cast<float>(i) * 0.5236f + 0.26f;
        const Vec3 p = lair + Vec3{std::cos(angle) * 17.0f, 2.2f, std::sin(angle) * 17.0f};
        b.box("Roca " + std::to_string(i + 1), p, Vec3{5.0f, 4.4f + static_cast<float>(i % 3), 3.5f}, rock,
              Vec3{static_cast<float>(i * 7 % 15), static_cast<float>(i * 37), 0.0f});
    }
    b.box("Trono", lair + Vec3{0.0f, 1.2f, -9.0f}, Vec3{3.0f, 2.4f, 2.0f}, gold);
    point_light("Luz guarida", lair + Vec3{0.0f, 4.0f, -6.0f}, Vec3{0.6f, 1.0f, 0.4f}, 8.0f, 20.0f);

    // Navegacion: todo el mapa.
    ecs::Entity volume = b.world.create("NavMeshBoundsVolume");
    volume.setWorldPosition(Vec3{0.0f, 4.0f, 0.0f});
    volume.add<navigation::NavMeshBounds>().size = Vec3{276.0f, 14.0f, 276.0f};

    // --- Jugador y camara ---
    ecs::Entity player = b.player(Vec3{0.0f, 1.1f, 8.0f}, body, visor);
    Builder::attach(player, "Heroe.cpp", {{"nombre", Builder::text("Heroe")}});
    b.camera(9.0f, 24.0f);
    const Uuid hero = player.uuid();

    // --- Personajes del pueblo ---
    const auto npc = [&](const std::string& name, const Vec3& p, const Vec3& color, const std::string& greeting, bool merchant) {
        ecs::Entity n = b.world.create(name);
        n.setWorldPosition(p + Vec3{0.0f, 1.0f, 0.0f});
        n.setTag("NPC");
        physics::CapsuleCollider& c = n.add<physics::CapsuleCollider>();
        c.radius = 0.45f;
        c.height = 1.9f;
        Builder::attach(n, "NPC.cpp",
                        {{"nombre", Builder::text(name)},
                         {"saludo", Builder::text(greeting)},
                         {"comerciante", merchant ? "true" : "false"}});
        const assets::AssetRef cloth = b.material("NPC " + name, color, 0.6f);
        ecs::Entity model = b.world.create("Modelo", n);
        primitive(assets::builtin::kCapsule, "Cuerpo", model, Vec3{}, Vec3{0.9f, 0.95f, 0.9f}, cloth);
        primitive(assets::builtin::kCube, "Cara", model, Vec3{0.0f, 0.45f, -0.36f}, Vec3{0.5f, 0.2f, 0.2f}, plaster);
        primitive(assets::builtin::kCube, "Marca", n, Vec3{0.0f, 1.75f, 0.0f}, Vec3{0.32f, 0.32f, 0.32f}, gold,
                  Vec3{45.0f, 0.0f, 45.0f});
        return n;
    };
    npc("Capitana Elena", Vec3{-6.0f, 0.0f, -8.0f}, Vec3{0.75f, 0.2f, 0.2f},
        "Soy la capitana de la guardia de Villa Alba. Estos dias no damos abasto: lobos al oeste, goblins al este...", false);
    npc("Mercader Tomas", Vec3{11.0f, 0.0f, 6.2f}, Vec3{0.3f, 0.55f, 0.3f},
        "¡Bienvenido! Pociones, armas y armaduras al mejor precio de toda la comarca.", true);
    npc("Hermano Anselmo", Vec3{-24.0f, 0.0f, 20.0f}, Vec3{0.85f, 0.85f, 0.8f},
        "Que la luz te guie. Algo oscuro se mueve en la guarida del norte...", false);
    (void)quest_mark;

    // --- Enemigos ---
    int enemy_n = 0;
    const auto health_bar = [&](ecs::Entity e, float height) {
        ecs::Entity bar = b.world.create("Barra", e);
        bar.setLocalPosition(Vec3{0.0f, height, 0.0f});
        bar.setLocalScale(Vec3{1.2f, 0.14f, 0.14f});
        primitive(assets::builtin::kCube, "Fondo", bar, Vec3{}, Vec3{1.0f, 0.8f, 0.8f}, bar_back).get<ecs::MeshRenderer>().cast_shadows =
            ecs::ShadowCasting::Off;
        primitive(assets::builtin::kCube, "Relleno", bar, Vec3{}, Vec3{1.0f, 1.0f, 1.0f}, bar_fill).get<ecs::MeshRenderer>().cast_shadows =
            ecs::ShadowCasting::Off;
        bar.setActive(false);
    };
    const auto enemy = [&](const std::string& type, int level, const Vec3& p) {
        ++enemy_n;
        const bool is_wolf = type == "lobo";
        const bool is_king = type == "rey";
        const std::string label = is_wolf ? "Lobo" : is_king ? "Rey Goblin" : type == "chaman" ? "Chaman" : "Goblin";
        const float half = is_wolf ? 0.45f : is_king ? 1.6f : 0.7f;  // del suelo al pivote
        ecs::Entity e = b.world.create(label + " " + std::to_string(enemy_n));
        e.setWorldPosition(p + Vec3{0.0f, half, 0.0f});
        e.setTag("Enemigo");
        navigation::NavAgent& agent = e.add<navigation::NavAgent>();
        agent.speed = is_wolf ? 4.8f : is_king ? 4.0f : 3.8f;
        agent.base_offset = half;
        agent.radius = is_king ? 0.9f : 0.45f;
        agent.height = half * 2.0f;
        Builder::attach(e, "Enemigo.cpp",
                        {{"tipo", Builder::text(type)},
                         {"nivel", std::to_string(level)}});
        ecs::Entity model = b.world.create("Modelo", e);
        if (is_wolf) {
            primitive(assets::builtin::kCube, "Cuerpo", model, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.55f, 0.55f, 1.3f}, wolf);
            primitive(assets::builtin::kCube, "Cabeza", model, Vec3{0.0f, 0.2f, -0.8f}, Vec3{0.4f, 0.4f, 0.5f}, wolf);
            primitive(assets::builtin::kCube, "Ojos", model, Vec3{0.0f, 0.28f, -1.06f}, Vec3{0.3f, 0.08f, 0.02f}, eye);
            primitive(assets::builtin::kCube, "Cola", model, Vec3{0.0f, 0.15f, 0.8f}, Vec3{0.14f, 0.14f, 0.5f}, wolf,
                      Vec3{-30.0f, 0.0f, 0.0f});
        } else {
            const assets::AssetRef skin = is_king ? king : type == "chaman" ? shaman : goblin;
            const float s = half / 1.0f;
            primitive(assets::builtin::kCapsule, "Cuerpo", model, Vec3{}, Vec3{s, s, s}, skin);
            primitive(assets::builtin::kCube, "Ojos", model, Vec3{0.0f, 0.45f * s, -0.42f * s}, Vec3{0.45f * s, 0.1f * s, 0.05f}, eye);
            if (is_king) {
                primitive(assets::builtin::kCylinder, "Corona", model, Vec3{0.0f, 1.05f * s, 0.0f}, Vec3{0.6f, 0.25f, 0.6f}, gold);
                primitive(assets::builtin::kCube, "Maza", model, Vec3{0.9f, 0.1f, -0.3f}, Vec3{0.3f, 1.6f, 0.3f}, dark_wood);
            } else if (type == "chaman") {
                primitive(assets::builtin::kCylinder, "Baston", model, Vec3{0.55f, 0.1f, 0.0f}, Vec3{0.08f, 0.9f, 0.08f}, dark_wood);
                primitive(assets::builtin::kSphere, "Orbe", model, Vec3{0.55f, 1.05f, 0.0f}, Vec3{0.22f, 0.22f, 0.22f}, fire);
            } else {
                primitive(assets::builtin::kCube, "Garrote", model, Vec3{0.55f, 0.0f, -0.2f}, Vec3{0.14f, 0.9f, 0.14f}, wood,
                          Vec3{20.0f, 0.0f, 0.0f});
            }
        }
        health_bar(e, is_wolf ? 0.9f : half + 0.55f);
        return e;
    };
    Lcg spots{777u};
    for (int i = 0; i < 8; ++i) {
        const float a = spots.range(0.0f, 6.2832f);
        const float d = spots.range(6.0f, 26.0f);
        enemy("lobo", i < 5 ? 1 : 2, forest + Vec3{std::cos(a) * d, 0.0f, std::sin(a) * d});
    }
    for (int i = 0; i < 8; ++i) {
        const float a = spots.range(0.0f, 6.2832f);
        const float d = spots.range(4.0f, 16.0f);
        enemy(i >= 6 ? "chaman" : "goblin", i < 4 ? 3 : 4, camp + Vec3{std::cos(a) * d, 0.0f, std::sin(a) * d});
    }
    enemy("rey", 6, lair + Vec3{0.0f, 0.0f, -4.0f});
    enemy("goblin", 5, lair + Vec3{-5.0f, 0.0f, 0.0f});
    enemy("goblin", 5, lair + Vec3{5.0f, 0.0f, 0.0f});

    // --- Otros jugadores (simulados) ---
    const struct {
        const char* name;
        const char* role;
        int level;
        Vec3 color;
        Vec3 pos;
    } bots[] = {{"Aria_Luz", "mago", 4, {0.8f, 0.3f, 0.9f}, {4.0f, 0.0f, -4.0f}},
                {"Kraven", "guerrero", 5, {0.85f, 0.45f, 0.1f}, {-4.0f, 0.0f, 4.0f}},
                {"Nube42", "mago", 3, {0.9f, 0.9f, 0.95f}, {6.0f, 0.0f, 3.0f}},
                {"Torvald", "guerrero", 4, {0.35f, 0.35f, 0.4f}, {-6.0f, 0.0f, -3.0f}}};
    for (const auto& info : bots) {
        ecs::Entity bot = b.world.create(info.name);
        bot.setWorldPosition(info.pos + Vec3{0.0f, 1.0f, 0.0f});
        bot.setTag("Bot");
        navigation::NavAgent& agent = bot.add<navigation::NavAgent>();
        agent.speed = 4.6f;
        agent.base_offset = 1.0f;
        Builder::attach(bot, "Bot.cpp",
                        {{"nombre", Builder::text(info.name)},
                         {"clase", Builder::text(info.role)},
                         {"nivel", std::to_string(info.level)}});
        ecs::Entity model = b.world.create("Modelo", bot);
        const assets::AssetRef cloth = b.material(std::string("Bot ") + info.name, info.color, 0.45f);
        primitive(assets::builtin::kCapsule, "Cuerpo", model, Vec3{}, Vec3{0.9f, 0.95f, 0.9f}, cloth);
        primitive(assets::builtin::kCube, "Visor", model, Vec3{0.0f, 0.45f, -0.36f}, Vec3{0.62f, 0.22f, 0.22f}, visor);
        const bool mage = std::string(info.role) == "mago";
        primitive(assets::builtin::kCube, mage ? "Baston" : "Espada", model, Vec3{0.55f, 0.1f, -0.2f},
                  mage ? Vec3{0.08f, 1.6f, 0.08f} : Vec3{0.1f, 1.1f, 0.2f}, mage ? dark_wood : stone);
    }

    // Bolsa de botin: Heroe.cpp hace copias donde muere un enemigo.
    ecs::Entity loot = primitive(assets::builtin::kCube, "Plantilla Botin", {}, Vec3{0.0f, -20.0f, 0.0f}, Vec3{0.6f, 0.45f, 0.45f}, gold);
    primitive(assets::builtin::kCube, "Tapa", loot, Vec3{0.0f, 0.6f, 0.0f}, Vec3{1.05f, 0.25f, 1.05f}, wood);
    loot.setActive(false);

    // --- Interfaz ---
    ecs::Entity canvas = b.world.create("HUD");
    canvas.add<ui::Canvas>();
    const auto rect = [&](ecs::Entity e, Vec2 anchor, Vec2 pivot, Vec2 position, Vec2 size) {
        ui::RectTransform& rt = e.add<ui::RectTransform>();
        rt.anchor_min = anchor;
        rt.anchor_max = anchor;
        rt.pivot = pivot;
        rt.position = position;
        rt.size = size;
        return e;
    };
    const auto stretch = [&](ecs::Entity e, float margin = 0.0f) {
        ui::RectTransform& rt = e.add<ui::RectTransform>();
        rt.anchor_min = Vec2{0.0f, 0.0f};
        rt.anchor_max = Vec2{1.0f, 1.0f};
        rt.size = Vec2{-2.0f * margin, -2.0f * margin};
        return e;
    };
    const auto image = [&](ecs::Entity e, Vec3 color, float alpha, float radius = 0.0f) {
        ui::Image& im = e.add<ui::Image>();
        im.color = color;
        im.alpha = alpha;
        im.corner_radius = radius;
        return e;
    };
    const auto text = [&](ecs::Entity e, const std::string& value, float font, ui::HAlign align, Vec3 color = Vec3{1, 1, 1},
                          ui::VAlign valign = ui::VAlign::Middle, bool wrap = false) {
        ui::Text& t = e.add<ui::Text>();
        t.text = value;
        t.font_size = font;
        t.h_align = align;
        t.v_align = valign;
        t.color = color;
        t.shadow = true;
        t.wrap = wrap;
        return e;
    };
    const auto button = [&](ecs::Entity e, const std::string& method, Vec3 color) {
        ui::Button& bt = e.add<ui::Button>();
        bt.normal = color;
        bt.hover = color * 1.35f;
        bt.pressed = color * 0.7f;
        bt.disabled = Vec3{0.16f, 0.16f, 0.17f};
        bt.corner_radius = 6.0f;
        bt.target = hero;
        bt.on_click = method;
        return e;
    };
    const auto child = [&](const std::string& name, ecs::Entity parent) { return b.world.create(name, parent); };
    const Vec2 top_left{0.0f, 0.0f};
    const Vec2 top_right{1.0f, 0.0f};
    const Vec2 top{0.5f, 0.0f};
    const Vec2 bottom{0.5f, 1.0f};
    const Vec2 bottom_left{0.0f, 1.0f};
    const Vec2 center{0.5f, 0.5f};
    // Barra con fondo, relleno (se acorta desde Heroe.cpp) y texto.
    const auto bar = [&](ecs::Entity parent, const std::string& prefix, Vec2 position, Vec2 size, Vec3 back, Vec3 fill, float font) {
        image(rect(child(prefix + "Fondo", parent), top_left, top_left, position, size), back, 0.9f, 4.0f);
        image(rect(child(prefix + "Barra", parent), top_left, top_left, position, size), fill, 1.0f, 4.0f);
        text(rect(child(prefix + "Texto", parent), top_left, top_left, position, size), "", font, ui::HAlign::Center);
    };
    const Vec3 panel_color{0.06f, 0.07f, 0.09f};
    const auto close_button = [&](ecs::Entity panel) {
        ecs::Entity c = rect(child("Cerrar", panel), top_right, top_right, Vec2{-12.0f, 12.0f}, Vec2{40.0f, 36.0f});
        button(c, "OnCerrar", Vec3{0.45f, 0.14f, 0.14f});
        text(stretch(child("Texto", c)), "X", 22.0f, ui::HAlign::Center);
    };

    // Marco del jugador (arriba a la izquierda) y del objetivo (arriba al centro).
    ecs::Entity frame = image(rect(child("UI_Jugador", canvas), top_left, top_left, Vec2{24, 24}, Vec2{360, 112}), panel_color, 0.65f, 10.0f);
    text(rect(child("Nombre", frame), top_left, top_left, Vec2{20, 8}, Vec2{330, 32}), "", 24.0f, ui::HAlign::Left);
    bar(frame, "Vida", Vec2{20, 46}, Vec2{320, 26}, Vec3{0.25f, 0.04f, 0.04f}, Vec3{0.85f, 0.15f, 0.12f}, 18.0f);
    bar(frame, "Mana", Vec2{20, 78}, Vec2{320, 22}, Vec3{0.04f, 0.07f, 0.25f}, Vec3{0.2f, 0.4f, 0.95f}, 16.0f);
    ecs::Entity target = image(rect(child("UI_Objetivo", canvas), top, top, Vec2{0, 24}, Vec2{360, 84}), panel_color, 0.65f, 10.0f);
    text(rect(child("Nombre", target), top_left, top_left, Vec2{20, 8}, Vec2{330, 32}), "", 24.0f, ui::HAlign::Left);
    bar(target, "Vida", Vec2{20, 46}, Vec2{320, 26}, Vec3{0.25f, 0.04f, 0.04f}, Vec3{0.85f, 0.15f, 0.12f}, 18.0f);
    target.setActive(false);

    // Experiencia (abajo del todo).
    image(rect(child("UI_XPFondo", canvas), bottom, bottom, Vec2{0, -8}, Vec2{900, 14}), Vec3{0.08f, 0.05f, 0.12f}, 0.85f, 4.0f);
    image(rect(child("UI_XPBarra", canvas), bottom, Vec2{0.0f, 1.0f}, Vec2{-450, -8}, Vec2{900, 14}), Vec3{0.62f, 0.3f, 0.95f}, 1.0f, 4.0f);
    text(rect(child("UI_XPTexto", canvas), bottom, bottom, Vec2{0, -24}, Vec2{900, 24}), "", 17.0f, ui::HAlign::Center);

    // Barra de habilidades (1-6).
    static const char* kAbilities[] = {"Golpe", "Bola de fuego", "Curar", "Torbellino", "Vida", "Mana"};
    static const Vec3 kAbilityColors[] = {{0.45f, 0.3f, 0.2f}, {0.7f, 0.28f, 0.08f}, {0.2f, 0.55f, 0.25f},
                                          {0.5f, 0.2f, 0.45f}, {0.55f, 0.12f, 0.12f}, {0.14f, 0.25f, 0.6f}};
    for (int i = 0; i < 6; ++i) {
        ecs::Entity slot = rect(child("UI_Hab " + std::to_string(i + 1), canvas), bottom, bottom,
                                Vec2{(static_cast<float>(i) - 2.5f) * 86.0f, -44.0f}, Vec2{78, 72});
        button(slot, "OnHabilidad", kAbilityColors[i]);
        text(rect(child("Nombre", slot), center, center, Vec2{0, 6}, Vec2{74, 44}), kAbilities[i], 14.0f, ui::HAlign::Center,
             Vec3{1, 1, 1}, ui::VAlign::Middle, true);
        image(rect(child("Enfriamiento", slot), bottom, bottom, Vec2{0, 0}, Vec2{78, 0}), Vec3{0, 0, 0}, 0.62f, 6.0f);
        text(rect(child("Tecla", slot), top_left, top_left, Vec2{6, 2}, Vec2{20, 20}), std::to_string(i + 1), 16.0f, ui::HAlign::Left,
             Vec3{1.0f, 0.9f, 0.5f});
        text(rect(child("Tiempo", slot), center, center, Vec2{0, 0}, Vec2{74, 34}), "", 26.0f, ui::HAlign::Center);
    }
    // Barra de lanzamiento.
    ecs::Entity cast = image(rect(child("UI_Lanzamiento", canvas), bottom, bottom, Vec2{0, -150}, Vec2{404, 30}), panel_color, 0.8f, 6.0f);
    image(rect(child("Barra", cast), top_left, top_left, Vec2{2, 2}, Vec2{400, 26}), Vec3{0.95f, 0.6f, 0.15f}, 1.0f, 5.0f);
    text(stretch(child("Texto", cast)), "", 18.0f, ui::HAlign::Center);
    cast.setActive(false);

    // Chat (abajo a la izquierda), misiones (derecha) y minimapa (arriba a la derecha).
    ecs::Entity chat = image(rect(child("UI_ChatFondo", canvas), bottom_left, bottom_left, Vec2{24, -24}, Vec2{660, 250}), panel_color,
                             0.4f, 8.0f);
    text(stretch(child("UI_Chat", chat), 12.0f), "", 17.0f, ui::HAlign::Left, Vec3{0.92f, 0.92f, 0.95f}, ui::VAlign::Bottom, true);
    text(rect(child("UI_Seguimiento", canvas), top_right, top_right, Vec2{-24, 300}, Vec2{430, 260}), "", 19.0f, ui::HAlign::Left,
         Vec3{1.0f, 0.93f, 0.7f}, ui::VAlign::Top, true);
    ecs::Entity map = image(rect(child("UI_Mapa", canvas), top_right, top_right, Vec2{-24, 24}, Vec2{230, 230}), Vec3{0.1f, 0.14f, 0.1f},
                            0.7f, 115.0f);
    for (int i = 1; i <= 40; ++i) {
        ecs::Entity dot = image(rect(child("Punto " + std::to_string(i), map), center, center, Vec2{0, 0}, Vec2{7, 7}), Vec3{1, 0, 0},
                                1.0f, 4.0f);
        dot.setActive(false);
    }
    image(rect(child("Yo", map), center, center, Vec2{0, 0}, Vec2{11, 11}), Vec3{1, 1, 1}, 1.0f, 6.0f);
    text(rect(child("Norte", map), top, top, Vec2{0, 4}, Vec2{30, 24}), "N", 18.0f, ui::HAlign::Center);
    text(rect(child("UI_Zona", canvas), top_right, top_right, Vec2{-24, 258}, Vec2{230, 30}), "", 19.0f, ui::HAlign::Center,
         Vec3{0.85f, 1.0f, 0.8f});

    // Avisos grandes y numeros flotantes.
    ecs::Entity notice = text(rect(child("UI_Aviso", canvas), top, top, Vec2{0, 130}, Vec2{1300, 50}), "", 34.0f, ui::HAlign::Center);
    notice.get<ui::Text>().alpha = 0.0f;
    for (int i = 1; i <= 8; ++i) {
        ecs::Entity f = text(rect(child("UI_Flotante " + std::to_string(i), canvas), center, center, Vec2{0, -120}, Vec2{300, 50}), "",
                             36.0f, ui::HAlign::Center);
        f.get<ui::Text>().alpha = 0.0f;
    }

    // Inventario (I): 20 ranuras.
    ecs::Entity inventory = image(rect(child("UI_Inventario", canvas), center, center, Vec2{-430, -10}, Vec2{530, 600}), panel_color,
                                  0.94f, 12.0f);
    text(rect(child("Titulo", inventory), top_left, top_left, Vec2{22, 14}, Vec2{400, 40}), "Inventario (I)", 30.0f, ui::HAlign::Left);
    text(rect(child("Oro", inventory), top_left, top_left, Vec2{22, 56}, Vec2{400, 30}), "", 21.0f, ui::HAlign::Left,
         Vec3{1.0f, 0.85f, 0.35f});
    for (int i = 0; i < 20; ++i) {
        ecs::Entity s = rect(child("Ranura " + std::to_string(i + 1), inventory), top_left, top_left,
                             Vec2{22.0f + static_cast<float>(i % 4) * 124.0f, 96.0f + static_cast<float>(i / 4) * 96.0f}, Vec2{116, 88});
        button(s, "OnRanura", Vec3{0.15f, 0.16f, 0.2f});
        image(rect(child("Color", s), top_left, top_left, Vec2{6, 6}, Vec2{16, 16}), Vec3{1, 1, 1}, 0.0f, 3.0f);
        text(rect(child("Nombre", s), center, center, Vec2{0, 4}, Vec2{108, 58}), "", 15.0f, ui::HAlign::Center, Vec3{1, 1, 1},
             ui::VAlign::Middle, true);
        text(rect(child("Cantidad", s), Vec2{1.0f, 1.0f}, Vec2{1.0f, 1.0f}, Vec2{-6, -2}, Vec2{60, 24}), "", 16.0f, ui::HAlign::Right);
    }
    text(rect(child("Pista", inventory), bottom_left, bottom_left, Vec2{22, -14}, Vec2{480, 28}), "", 17.0f, ui::HAlign::Left,
         Vec3{0.7f, 0.72f, 0.78f});
    close_button(inventory);
    inventory.setActive(false);

    // Personaje (C): estadisticas y equipo.
    ecs::Entity sheet = image(rect(child("UI_Personaje", canvas), center, center, Vec2{430, -10}, Vec2{470, 560}), panel_color, 0.94f, 12.0f);
    text(rect(child("Titulo", sheet), top_left, top_left, Vec2{22, 14}, Vec2{400, 40}), "Personaje (C)", 30.0f, ui::HAlign::Left);
    text(rect(child("Stats", sheet), top_left, top_left, Vec2{22, 64}, Vec2{420, 330}), "", 20.0f, ui::HAlign::Left, Vec3{1, 1, 1},
         ui::VAlign::Top, true);
    for (int i = 0; i < 2; ++i) {
        ecs::Entity slot = rect(child(i == 0 ? "Arma" : "Armadura", sheet), top_left, top_left, Vec2{22.0f, 410.0f + 62.0f * static_cast<float>(i)},
                                Vec2{426, 52});
        button(slot, "OnDesequipar", Vec3{0.2f, 0.22f, 0.3f});
        text(stretch(child("Texto", slot), 10.0f), "", 18.0f, ui::HAlign::Left);
    }
    close_button(sheet);
    sheet.setActive(false);

    // Diario de misiones (L).
    ecs::Entity journal = image(rect(child("UI_Diario", canvas), center, center, Vec2{0, -10}, Vec2{980, 640}), panel_color, 0.95f, 12.0f);
    text(rect(child("Titulo", journal), top_left, top_left, Vec2{24, 14}, Vec2{600, 40}), "Diario de misiones (L)", 30.0f, ui::HAlign::Left);
    text(rect(child("Texto", journal), top_left, top_left, Vec2{24, 70}, Vec2{930, 550}), "", 19.0f, ui::HAlign::Left, Vec3{1, 1, 1},
         ui::VAlign::Top, true);
    close_button(journal);
    journal.setActive(false);

    // Dialogo con un NPC (misiones y tienda).
    ecs::Entity dialog = image(rect(child("UI_Dialogo", canvas), center, center, Vec2{320, -10}, Vec2{640, 640}), panel_color, 0.95f, 12.0f);
    text(rect(child("Titulo", dialog), top_left, top_left, Vec2{24, 16}, Vec2{540, 44}), "", 30.0f, ui::HAlign::Left,
         Vec3{1.0f, 0.85f, 0.4f});
    text(rect(child("Cuerpo", dialog), top_left, top_left, Vec2{24, 70}, Vec2{592, 250}), "", 20.0f, ui::HAlign::Left, Vec3{1, 1, 1},
         ui::VAlign::Top, true);
    for (int i = 0; i < 6; ++i) {
        ecs::Entity o = rect(child("Opcion " + std::to_string(i + 1), dialog), top_left, top_left,
                             Vec2{24.0f, 336.0f + 48.0f * static_cast<float>(i)}, Vec2{592, 42});
        button(o, "OnOpcion", Vec3{0.16f, 0.3f, 0.5f});
        text(stretch(child("Texto", o), 8.0f), "", 19.0f, ui::HAlign::Left);
    }
    close_button(dialog);
    dialog.setActive(false);

    // Muerte y ayuda.
    ecs::Entity death = image(stretch(child("UI_Muerte", canvas)), Vec3{0.35f, 0.02f, 0.02f}, 0.55f);
    text(rect(child("Titulo", death), center, center, Vec2{0, -100}, Vec2{900, 90}), "Has muerto", 72.0f, ui::HAlign::Center);
    text(rect(child("Causa", death), center, center, Vec2{0, -20}, Vec2{900, 50}), "", 30.0f, ui::HAlign::Center, Vec3{1.0f, 0.8f, 0.8f});
    ecs::Entity respawn = rect(child("Reaparecer", death), center, center, Vec2{0, 80}, Vec2{380, 66});
    button(respawn, "OnReaparecer", Vec3{0.5f, 0.12f, 0.12f});
    text(stretch(child("Texto", respawn)), "Reaparecer en el pueblo", 26.0f, ui::HAlign::Center);
    death.setActive(false);

    ecs::Entity help = image(rect(child("UI_Ayuda", canvas), center, center, Vec2{0, -20}, Vec2{820, 560}), panel_color, 0.95f, 12.0f);
    text(rect(child("Titulo", help), top_left, top_left, Vec2{24, 14}, Vec2{600, 40}), "Controles (H)", 30.0f, ui::HAlign::Left);
    text(rect(child("Texto", help), top_left, top_left, Vec2{24, 70}, Vec2{770, 470}),
         "WASD  moverse          Shift  correr          Espacio  saltar\n"
         "Clic derecho + raton  girar la camara          Rueda  acercar\n\n"
         "Tab  siguiente enemigo          Clic izquierdo  el enemigo al que miras\n"
         "1  Golpe (cuerpo a cuerpo)          2  Bola de fuego (a distancia)\n"
         "3  Curar (nivel 2)          4  Torbellino en area (nivel 3)\n"
         "5 / 6  pociones de vida y de mana\n\n"
         "E  hablar con un personaje (! = mision nueva, azul = para entregar)\n"
         "I  inventario (clic: usar, equipar o vender)          C  personaje\n"
         "L  diario de misiones          Escape  cerrar / quitar objetivo\n"
         "F9  borrar la partida guardada\n\n"
         "La partida se guarda sola. Los otros jugadores del mundo tambien cazan.",
         20.0f, ui::HAlign::Left, Vec3{1, 1, 1}, ui::VAlign::Top, true);
    close_button(help);
    help.setActive(false);

    b.save("Main");

    std::vector<std::string> tags = ecs::defaultTags();
    for (const char* t : {"Enemigo", "NPC", "Bot"}) tags.push_back(t);
    ecs::saveTags(project.settingsFolder() / "Tags.json", tags);
    navigation::NavigationSettings nav;
    navigation::saveNavigationSettings(project.settingsFolder() / "Navigation.json", nav);
}

// --- Criaturas: IK de animales, phys bones, ragdoll y huesos ------------------------

void buildCreatures(project::ProjectInfo& project) {
    Builder b(project);
    b.world.setSceneUuid(Uuid::generate());
    ecs::populateDefaultScene(b.world);
    b.scripts(creature_scripts::kFiles);
    b.scripts(template_scripts::kThirdPersonCamera);

    // Los dos modelos con esqueleto, generados (como si se hubieran importado).
    const asset::ModelData dog_model = makeDogModel();
    const asset::ModelData dummy_model = makeDummyModel();
    const Uuid dog_uuid = Uuid::generate();
    const Uuid dummy_uuid = Uuid::generate();
    std::string error;
    if (!writeCreatureModel(project.assetsFolder() / "Models" / "Perro.crdata", dog_uuid, "Perro", dog_model, &error) ||
        !writeCreatureModel(project.assetsFolder() / "Models" / "Maniqui.crdata", dummy_uuid, "Maniqui", dummy_model, &error)) {
        throw std::runtime_error(error);
    }

    const assets::AssetRef floor = b.material("Cesped", Vec3{0.42f, 0.58f, 0.3f}, 0.95f);
    const assets::AssetRef stone = b.material("Piedra", Vec3{0.55f, 0.55f, 0.58f}, 0.85f);
    const assets::AssetRef wood = b.material("Madera", Vec3{0.62f, 0.44f, 0.26f}, 0.8f);
    const assets::AssetRef red = b.material("Sombrero", Vec3{0.85f, 0.15f, 0.12f}, 0.5f);
    const assets::AssetRef ball = b.material("Pelota", Vec3{1.0f, 0.82f, 0.2f}, 0.35f, 0.0f, Vec3{1.0f, 0.7f, 0.1f}, 0.4f);

    b.box("Suelo", Vec3{0.0f, -0.5f, 0.0f}, Vec3{30.0f, 1.0f, 30.0f}, floor);
    // Escalera: sube tres escalones, un rellano y baja (en el lado de delante).
    const float step = 0.12f;
    for (int i = 0; i < 3; ++i) {
        const float h = step * static_cast<float>(i + 1);
        b.box("Escalon subida " + std::to_string(i + 1), Vec3{0.0f + 0.6f * static_cast<float>(i), h * 0.5f, 7.0f},
              Vec3{0.6f, h, 2.0f}, wood);
        b.box("Escalon bajada " + std::to_string(i + 1), Vec3{4.8f - 0.6f * static_cast<float>(i), h * 0.5f, 7.0f},
              Vec3{0.6f, h, 2.0f}, wood);
    }
    b.box("Rellano", Vec3{2.4f, step * 1.5f, 7.0f}, Vec3{1.8f, step * 3.0f, 2.0f}, wood);
    // Rampa y bajada (lado derecho): el perro se inclina con la pendiente.
    b.box("Rampa", Vec3{7.0f, 0.2f, -0.5f}, Vec3{2.4f, 0.2f, 4.2f}, stone, Vec3{10.0f, 0.0f, 0.0f});
    b.box("Meseta", Vec3{7.0f, 0.46f, -3.4f}, Vec3{2.4f, 0.2f, 1.8f}, stone);
    b.box("Bajada", Vec3{7.0f, 0.2f, -6.0f}, Vec3{2.4f, 0.2f, 3.6f}, stone, Vec3{-14.0f, 0.0f, 0.0f});
    // Piedras sueltas (lado de atras): cada pata pisa a una altura.
    const Vec3 rocks[] = {{-1.5f, 0.06f, -7.0f}, {-2.6f, 0.1f, -6.6f}, {-3.8f, 0.05f, -7.2f},
                          {-4.9f, 0.12f, -6.8f}, {-6.0f, 0.07f, -6.3f}};
    int r = 0;
    for (const Vec3& p : rocks) {
        const float h = p.y * 2.0f;
        b.box("Piedra " + std::to_string(++r), p, Vec3{0.7f, h, 0.6f}, stone, Vec3{0.0f, 25.0f * static_cast<float>(r), 4.0f});
    }

    // Pelota que siguen con la cabeza.
    ecs::Entity pelota = ecs::createPrimitive(b.world, assets::builtin::kSphere, "Pelota");
    pelota.setLocalScale(Vec3{0.3f, 0.3f, 0.3f});
    pelota.get<ecs::MeshRenderer>().materials = {ball};
    Builder::attach(pelota, "Pelota.cpp");

    // --- El perro ---
    ecs::Entity dog = b.world.create("Perro");
    dog.setWorldPosition(Vec3{-7.0f, 0.0f, 7.0f});
    dog.add<ecs::MeshRenderer>().model = assets::AssetRef{dog_uuid, assets::AssetType::Model};
    dog.add<ecs::Animator>().clip_name = "Caminar";
    ecs::InverseKinematics& dog_ik = dog.add<ecs::InverseKinematics>();
    ecs::suggestCreatureIK(dog_model, dog_ik);  // 4 patas al suelo, cabeza y cuello
    dog_ik.look_weight = 0.7f;
    dog_ik.look_max_angle = 80.0f;
    dog_ik.max_step = 0.4f;
    dog.add<ecs::PhysBones>().chains = ecs::suggestPhysBones(dog_model);  // cola y orejas
    ecs::Ragdoll& dog_rag = dog.add<ecs::Ragdoll>();
    dog_rag.mass = 25.0f;
    dog_rag.bones = ecs::suggestRagdollBones(dog_model, 1.0f);
    ecs::Skeleton& dog_skeleton = dog.add<ecs::Skeleton>();
    dog_skeleton.show_bones = false;
    Builder::attach(dog, "Perro.cpp");
    // Sombrero enganchado a la cabeza (Bone Socket) y un collider en el
    // cuerpo para que la cola no lo atraviese.
    ecs::Entity hat_socket = b.world.create("Sombrero", dog);
    ecs::BoneSocket& hat = hat_socket.add<ecs::BoneSocket>();
    hat.bone = "Head";
    hat.position = Vec3{0.0f, 0.1f, -0.02f};
    ecs::Entity hat_mesh = ecs::createPrimitive(b.world, assets::builtin::kCylinder, "Copa", hat_socket);
    hat_mesh.setLocalScale(Vec3{0.1f, 0.05f, 0.1f});
    hat_mesh.get<ecs::MeshRenderer>().materials = {red};
    ecs::Entity brim = ecs::createPrimitive(b.world, assets::builtin::kCylinder, "Ala", hat_socket);
    brim.setLocalPosition(Vec3{0.0f, -0.045f, 0.0f});
    brim.setLocalScale(Vec3{0.2f, 0.01f, 0.2f});
    brim.get<ecs::MeshRenderer>().materials = {red};
    ecs::Entity body_collider = b.world.create("Collider cuerpo", dog);
    body_collider.add<ecs::BoneSocket>().bone = "Body";
    ecs::PhysBoneCollider& body_sphere = body_collider.add<ecs::PhysBoneCollider>();
    body_sphere.radius = 0.14f;

    // --- El maniqui (un pie en un escalon) ---
    b.box("Escalon del maniqui", Vec3{-3.1f, 0.08f, -1.5f}, Vec3{0.3f, 0.16f, 0.5f}, wood);
    ecs::Entity dummy = b.world.create("Maniqui");
    dummy.setWorldPosition(Vec3{-3.0f, 0.0f, -1.5f});
    dummy.add<ecs::MeshRenderer>().model = assets::AssetRef{dummy_uuid, assets::AssetType::Model};
    dummy.add<ecs::Animator>().clip_name = "Reposo";
    ecs::InverseKinematics& dummy_ik = dummy.add<ecs::InverseKinematics>();
    ecs::suggestCreatureIK(dummy_model, dummy_ik);  // pies en el suelo
    dummy_ik.look_weight = 0.9f;
    dummy.add<ecs::PhysBones>().chains = ecs::suggestPhysBones(dummy_model);  // la coleta
    ecs::Ragdoll& dummy_rag = dummy.add<ecs::Ragdoll>();
    dummy_rag.mass = 70.0f;
    dummy_rag.blend_out = 0.6f;
    dummy.add<ecs::Skeleton>().show_bones = false;
    Builder::attach(dummy, "Maniqui.cpp");
    ecs::Entity head_collider = b.world.create("Collider cabeza", dummy);
    head_collider.add<ecs::BoneSocket>().bone = "Head";
    ecs::PhysBoneCollider& head_sphere = head_collider.add<ecs::PhysBoneCollider>();
    head_sphere.radius = 0.11f;
    head_sphere.offset = Vec3{0.0f, 0.09f, 0.0f};

    // Camara que sigue al perro y el texto de ayuda.
    ecs::Entity cam = b.world.findByName("Main Camera");
    cam.setWorldPosition(Vec3{0.0f, 4.0f, 14.0f});
    Builder::attach(cam, "CamaraTercera.cpp",
                    {{"objetivo", Builder::text("Perro")},
                     {"distancia", "5.5"},
                     {"altura", "0.4"},
                     {"inclinacion", "22"}});
    ecs::Entity help = b.hud("Controles.cpp");
    help.get<ui::Text>().font_size = 24.0f;
    help.get<ui::RectTransform>().size = Vec2{1800.0f, 40.0f};
    b.save("Main");
}

// Online: todo el juego en C++ (Red.cpp). Arena con porterias, balon, cajas
// y monedas (del servidor), jugadores sincronizados, chat, marcador y eventos.
void buildOnline(project::ProjectInfo& project) {
    Builder b(project);
    b.world.setSceneUuid(Uuid::generate());
    ecs::populateDefaultScene(b.world);
    b.scripts(online::kFiles);
    b.scripts(template_scripts::kThirdPersonCamera);

    const assets::AssetRef grass = b.material("Cesped", Vec3{0.30f, 0.55f, 0.28f}, 0.95f);
    const assets::AssetRef line = b.material("Linea", Vec3{0.95f, 0.95f, 0.95f}, 0.8f);
    const assets::AssetRef wall = b.material("Muro", Vec3{0.62f, 0.64f, 0.68f}, 0.85f);
    const assets::AssetRef post = b.material("Porteria", Vec3{0.95f, 0.95f, 0.97f}, 0.4f, 0.2f);
    const assets::AssetRef net_blue = b.material("Red azul", Vec3{0.15f, 0.35f, 0.85f}, 0.9f);
    const assets::AssetRef net_red = b.material("Red roja", Vec3{0.85f, 0.18f, 0.15f}, 0.9f);
    const assets::AssetRef wood = b.material("Madera", Vec3{0.55f, 0.38f, 0.22f}, 0.85f);
    const assets::AssetRef ball = b.material("Balon", Vec3{0.96f, 0.96f, 0.96f}, 0.45f);
    const assets::AssetRef gold = b.material("Oro", Vec3{1.0f, 0.78f, 0.25f}, 0.25f, 1.0f, Vec3{1.0f, 0.7f, 0.2f}, 0.6f);
    const assets::AssetRef visor = b.material("Visor", Vec3{0.05f, 0.06f, 0.08f}, 0.15f, 0.6f);
    // Un color por jugador (JugadorRed.cpp elige el suyo por su id).
    const std::pair<const char*, Vec3> kColors[] = {
        {"Azul", {0.10f, 0.45f, 0.95f}},  {"Rojo", {0.90f, 0.15f, 0.12f}},    {"Verde", {0.20f, 0.75f, 0.25f}},
        {"Amarillo", {0.98f, 0.82f, 0.10f}}, {"Morado", {0.55f, 0.25f, 0.85f}}, {"Naranja", {0.98f, 0.50f, 0.10f}},
        {"Cian", {0.10f, 0.80f, 0.85f}},  {"Rosa", {0.95f, 0.40f, 0.70f}},
    };
    assets::AssetRef first_color{};
    for (const auto& [name, color] : kColors) {
        const assets::AssetRef m = b.material(std::string("Jugador ") + name, color, 0.35f);
        if (!first_color.valid()) first_color = m;
    }

    // --- Arena: 52 x 52 m con muros y una porteria a cada lado (x = +-26) ---
    constexpr float kHalf = 26.0f;
    constexpr float kGoal = 4.0f;  // media anchura de la porteria
    b.box("Suelo", Vec3{0.0f, -0.5f, 0.0f}, Vec3{kHalf * 2.0f + 8.0f, 1.0f, kHalf * 2.0f}, grass);
    const auto flat = [&](const std::string& name, const Vec3& p, const Vec3& s) {
        ecs::Entity e = b.box(name, p, s, line);
        e.remove<physics::BoxCollider>();
        return e;
    };
    flat("Linea central", Vec3{0.0f, 0.005f, 0.0f}, Vec3{0.25f, 0.01f, kHalf * 2.0f});
    flat("Area azul", Vec3{-kHalf + 3.0f, 0.005f, 0.0f}, Vec3{0.25f, 0.01f, kGoal * 2.0f + 4.0f});
    flat("Area roja", Vec3{kHalf - 3.0f, 0.005f, 0.0f}, Vec3{0.25f, 0.01f, kGoal * 2.0f + 4.0f});
    b.box("Muro Norte", Vec3{0.0f, 1.0f, -kHalf}, Vec3{kHalf * 2.0f + 0.6f, 2.0f, 0.6f}, wall);
    b.box("Muro Sur", Vec3{0.0f, 1.0f, kHalf}, Vec3{kHalf * 2.0f + 0.6f, 2.0f, 0.6f}, wall);
    const float side = (kHalf - kGoal);
    for (const float sx : {-1.0f, 1.0f}) {
        const std::string tag = sx < 0.0f ? " Oeste" : " Este";
        const assets::AssetRef net = sx < 0.0f ? net_blue : net_red;
        // Muro con el hueco de la porteria.
        b.box("Muro" + tag + " 1", Vec3{sx * kHalf, 1.0f, -(kGoal + side * 0.5f)}, Vec3{0.6f, 2.0f, side}, wall);
        b.box("Muro" + tag + " 2", Vec3{sx * kHalf, 1.0f, kGoal + side * 0.5f}, Vec3{0.6f, 2.0f, side}, wall);
        // Porteria: fondo, lados, larguero y postes.
        b.box("Porteria" + tag + " fondo", Vec3{sx * (kHalf + 3.0f), 1.25f, 0.0f}, Vec3{0.3f, 2.5f, kGoal * 2.0f}, net);
        b.box("Porteria" + tag + " lado 1", Vec3{sx * (kHalf + 1.5f), 1.25f, -kGoal}, Vec3{3.0f, 2.5f, 0.2f}, net);
        b.box("Porteria" + tag + " lado 2", Vec3{sx * (kHalf + 1.5f), 1.25f, kGoal}, Vec3{3.0f, 2.5f, 0.2f}, net);
        b.box("Porteria" + tag + " techo", Vec3{sx * (kHalf + 1.5f), 2.55f, 0.0f}, Vec3{3.0f, 0.1f, kGoal * 2.0f}, net);
        b.box("Larguero" + tag, Vec3{sx * kHalf, 2.5f, 0.0f}, Vec3{0.3f, 0.3f, kGoal * 2.0f + 0.3f}, post);
        b.box("Poste" + tag + " 1", Vec3{sx * kHalf, 1.25f, -kGoal}, Vec3{0.3f, 2.5f, 0.3f}, post);
        b.box("Poste" + tag + " 2", Vec3{sx * kHalf, 1.25f, kGoal}, Vec3{0.3f, 2.5f, 0.3f}, post);
    }
    // Rampas y plataformas en las esquinas (para saltar y esconder monedas).
    b.box("Rampa 1", Vec3{-15.0f, 0.9f, -17.0f}, Vec3{4.0f, 0.4f, 8.0f}, wall, Vec3{-13.0f, 0.0f, 0.0f});
    b.box("Plataforma 1", Vec3{-15.0f, 1.8f, -22.5f}, Vec3{6.0f, 0.4f, 4.0f}, wall);
    b.box("Rampa 2", Vec3{15.0f, 0.9f, 17.0f}, Vec3{4.0f, 0.4f, 8.0f}, wall, Vec3{13.0f, 0.0f, 0.0f});
    b.box("Plataforma 2", Vec3{15.0f, 1.8f, 22.5f}, Vec3{6.0f, 0.4f, 4.0f}, wall);

    // Camara: vista general en el menu; en la partida sigue a tu jugador.
    ecs::Entity cam = b.world.findByName("Main Camera");
    cam.setWorldPosition(Vec3{0.0f, 30.0f, 40.0f});
    cam.setLocalEulerDegrees(Vec3{-36.0f, 0.0f, 0.0f});
    Builder::attach(cam, "CamaraTercera.cpp",
                    {{"objetivo", Builder::text("")},
                     {"distancia", "9"},
                     {"inclinacion", "22"}});

    // El juego.
    ecs::Entity game = b.world.create("Red");
    Builder::attach(game, "Red.cpp");
    const Uuid game_id = game.uuid();

    // --- Prefabs de red (se crean en otro mundo: no van en la escena) ---
    {
        ecs::World pw;
        const std::filesystem::path folder = project.assetsFolder() / "Prefabs";
        std::filesystem::create_directories(folder);
        const auto make = [&](ecs::Entity root, const char* file) {
            std::string error;
            if (!ecs::createPrefab(pw, root, folder / file, &error)) throw std::runtime_error(std::string(file) + ": " + error);
        };
        const auto mesh = [&](const Uuid& shape, const std::string& name, ecs::Entity parent, const Vec3& p, const Vec3& s,
                              const assets::AssetRef& m, const Vec3& euler = Vec3{}) {
            ecs::Entity e = ecs::createPrimitive(pw, shape, name, parent);
            e.setLocalPosition(p);
            e.setLocalScale(s);
            e.setLocalEulerDegrees(euler);
            e.get<ecs::MeshRenderer>().materials = {m};
            return e;
        };

        // Jugador: capsula con Rigidbody (como la plantilla Tercera persona) y su script.
        ecs::Entity player = pw.create("Jugador");
        physics::CapsuleCollider& capsule = player.add<physics::CapsuleCollider>();
        capsule.radius = 0.45f;
        capsule.height = 1.9f;
        capsule.material.friction = 0.0f;
        physics::Rigidbody& rb = player.add<physics::Rigidbody>();
        rb.mass = 70.0f;
        rb.lock_rotation_x = rb.lock_rotation_y = rb.lock_rotation_z = true;
        rb.continuous = true;
        net::NetworkObject& pn = player.add<net::NetworkObject>();
        pn.send_rate = 20.0f;
        pn.smoothing = 14.0f;
        Builder::attach(player, "JugadorRed.cpp");
        ecs::Entity model = pw.create("Modelo", player);
        mesh(assets::builtin::kCapsule, "Cuerpo", model, Vec3{}, Vec3{0.9f, 0.95f, 0.9f}, first_color);
        mesh(assets::builtin::kCube, "Visor", model, Vec3{0.0f, 0.45f, -0.36f}, Vec3{0.62f, 0.22f, 0.22f}, visor);
        make(player, "Jugador.crprefab");

        // Moneda: no manda su posicion (esta quieta; cada uno la gira en su pantalla).
        ecs::Entity coin = pw.create("Moneda");
        mesh(assets::builtin::kCylinder, "Disco", coin, Vec3{}, Vec3{0.8f, 0.08f, 0.8f}, gold, Vec3{90.0f, 0.0f, 0.0f});
        coin.add<net::NetworkObject>().sync_transform = false;
        Builder::attach(coin, "MonedaRed.cpp");
        make(coin, "Moneda.crprefab");

        // Balon y caja: los simula el servidor y, con fisica local, tambien
        // cada cliente (asi cualquiera los empuja al pasar; el servidor corrige).
        ecs::Entity ball_e = mesh(assets::builtin::kSphere, "Balon", {}, Vec3{}, Vec3{0.7f, 0.7f, 0.7f}, ball);
        physics::SphereCollider& sphere = ball_e.add<physics::SphereCollider>();
        sphere.material.bounciness = 0.55f;
        sphere.material.friction = 0.4f;
        physics::Rigidbody& brb = ball_e.add<physics::Rigidbody>();
        brb.mass = 0.8f;
        brb.linear_damping = 0.25f;
        brb.angular_damping = 0.4f;
        brb.continuous = true;
        net::NetworkObject& bn = ball_e.add<net::NetworkObject>();
        bn.send_rate = 30.0f;
        bn.smoothing = 18.0f;
        bn.local_physics = true;
        make(ball_e, "Balon.crprefab");

        ecs::Entity crate = mesh(assets::builtin::kCube, "Caja", {}, Vec3{}, Vec3{1.3f, 1.3f, 1.3f}, wood);
        crate.add<physics::BoxCollider>();
        crate.add<physics::Rigidbody>().mass = 8.0f;
        net::NetworkObject& cn = crate.add<net::NetworkObject>();
        cn.send_rate = 20.0f;
        cn.local_physics = true;
        make(crate, "Caja.crprefab");
    }

    // --- Interfaz ---
    ecs::Entity canvas = b.world.create("HUD");
    canvas.add<ui::Canvas>();
    const auto rect = [&](ecs::Entity e, Vec2 anchor, Vec2 pivot, Vec2 position, Vec2 size) {
        ui::RectTransform& rt = e.add<ui::RectTransform>();
        rt.anchor_min = anchor;
        rt.anchor_max = anchor;
        rt.pivot = pivot;
        rt.position = position;
        rt.size = size;
        return e;
    };
    const auto stretch = [&](ecs::Entity e, float margin = 0.0f) {
        ui::RectTransform& rt = e.add<ui::RectTransform>();
        rt.anchor_min = Vec2{0.0f, 0.0f};
        rt.anchor_max = Vec2{1.0f, 1.0f};
        rt.size = Vec2{-2.0f * margin, -2.0f * margin};
        return e;
    };
    const auto image = [&](ecs::Entity e, Vec3 color, float alpha, float radius = 0.0f) {
        ui::Image& im = e.add<ui::Image>();
        im.color = color;
        im.alpha = alpha;
        im.corner_radius = radius;
        return e;
    };
    const auto text = [&](ecs::Entity e, const std::string& value, float font, ui::HAlign align, Vec3 color = Vec3{1, 1, 1},
                          ui::VAlign valign = ui::VAlign::Middle, bool wrap = false) {
        ui::Text& t = e.add<ui::Text>();
        t.text = value;
        t.font_size = font;
        t.h_align = align;
        t.v_align = valign;
        t.color = color;
        t.shadow = true;
        t.wrap = wrap;
        return e;
    };
    const auto button = [&](ecs::Entity e, const std::string& label, const std::string& method, Vec3 color) {
        ui::Button& bt = e.add<ui::Button>();
        bt.normal = color;
        bt.hover = color * 1.3f;
        bt.pressed = color * 0.7f;
        bt.corner_radius = 8.0f;
        bt.target = game_id;
        bt.on_click = method;
        ecs::Entity t = b.world.create("Texto", e);
        ui::RectTransform& rt = t.add<ui::RectTransform>();
        rt.anchor_min = Vec2{0.0f, 0.0f};
        rt.anchor_max = Vec2{1.0f, 1.0f};
        ui::Text& tx = t.add<ui::Text>();
        tx.text = label;
        tx.font_size = 26.0f;
        tx.h_align = ui::HAlign::Center;
        tx.shadow = true;
        return e;
    };
    const auto field = [&](ecs::Entity e, const std::string& value, const std::string& placeholder, int max_length,
                           const std::string& on_submit = {}) {
        ui::InputField& f = e.add<ui::InputField>();
        f.text = value;
        f.placeholder = placeholder;
        f.font_size = 24.0f;
        f.max_length = max_length;
        f.target = game_id;
        f.on_submit = on_submit;
        return e;
    };
    const auto child = [&](const std::string& name, ecs::Entity parent) { return b.world.create(name, parent); };
    const Vec2 top_left{0.0f, 0.0f}, top_right{1.0f, 0.0f}, top{0.5f, 0.0f}, bottom_left{0.0f, 1.0f}, center{0.5f, 0.5f};
    const Vec3 panel{0.06f, 0.07f, 0.09f};

    // Menu: nombre, IP y crear / unirse.
    ecs::Entity menu = image(rect(child("UI_Menu", canvas), center, center, Vec2{0, 0}, Vec2{620, 470}), panel, 0.88f, 14.0f);
    text(rect(child("Titulo", menu), top_left, top_left, Vec2{30, 22}, Vec2{560, 50}), "Cramion Online", 44.0f, ui::HAlign::Left,
         Vec3{0.45f, 0.75f, 1.0f});
    text(rect(child("Sub", menu), top_left, top_left, Vec2{30, 74}, Vec2{560, 30}), "Arena con balon, monedas, chat y eventos", 20.0f,
         ui::HAlign::Left, Vec3{0.7f, 0.72f, 0.78f});
    text(rect(child("Etiqueta nombre", menu), top_left, top_left, Vec2{30, 118}, Vec2{250, 30}), "Tu nombre", 22.0f, ui::HAlign::Left);
    field(rect(child("UI_Nombre", menu), top_left, top_left, Vec2{30, 150}, Vec2{560, 46}), "Jugador", "Tu nombre", 16);
    text(rect(child("Etiqueta IP", menu), top_left, top_left, Vec2{30, 206}, Vec2{560, 30}), "IP del servidor (para unirse)", 22.0f,
         ui::HAlign::Left);
    field(rect(child("UI_IP", menu), top_left, top_left, Vec2{30, 238}, Vec2{560, 46}), "127.0.0.1", "127.0.0.1", 64);
    button(rect(child("Crear", menu), top_left, top_left, Vec2{30, 304}, Vec2{270, 58}), "Crear partida", "OnCrear",
           Vec3{0.10f, 0.45f, 0.85f});
    button(rect(child("Unirse", menu), top_left, top_left, Vec2{320, 304}, Vec2{270, 58}), "Unirse", "OnUnirse",
           Vec3{0.15f, 0.55f, 0.30f});
    text(rect(child("UI_Estado", menu), top_left, top_left, Vec2{30, 376}, Vec2{560, 80}), "", 19.0f, ui::HAlign::Left,
         Vec3{1.0f, 0.88f, 0.55f}, ui::VAlign::Top, true);

    // En partida: chat, marcador, avisos, informacion y salir.
    ecs::Entity hud = stretch(child("UI_Juego", canvas));
    ecs::Entity chat = image(rect(child("Chat", hud), bottom_left, bottom_left, Vec2{24, -84}, Vec2{620, 250}), panel, 0.55f, 10.0f);
    text(stretch(child("UI_Chat", chat), 12.0f), "", 19.0f, ui::HAlign::Left, Vec3{0.93f, 0.94f, 0.97f}, ui::VAlign::Bottom, true);
    field(rect(child("UI_EscribirChat", hud), bottom_left, bottom_left, Vec2{24, -24}, Vec2{620, 50}), "",
          "Clic aqui, escribe y pulsa Enter...", 120, "OnChat");
    image(rect(child("Marcador fondo", hud), top_right, top_right, Vec2{-24, 24}, Vec2{520, 290}), panel, 0.55f, 10.0f);
    text(rect(child("UI_Marcador", hud), top_right, top_right, Vec2{-36, 34}, Vec2{496, 270}), "", 20.0f, ui::HAlign::Left,
         Vec3{1, 1, 1}, ui::VAlign::Top, true);
    ecs::Entity notice = text(rect(child("UI_Aviso", hud), top, top, Vec2{0, 150}, Vec2{1300, 70}), "", 46.0f, ui::HAlign::Center,
                              Vec3{1.0f, 0.85f, 0.3f});
    notice.get<ui::Text>().alpha = 0.0f;
    text(rect(child("UI_Info", hud), top_left, top_left, Vec2{150, 24}, Vec2{1100, 64}), "", 18.0f, ui::HAlign::Left,
         Vec3{0.85f, 0.88f, 0.92f}, ui::VAlign::Top, true);
    button(rect(child("Salir", hud), top_left, top_left, Vec2{24, 24}, Vec2{110, 46}), "Salir", "OnSalir", Vec3{0.55f, 0.16f, 0.16f});

    b.save("Main");
}

// Mundo abierto: una isla de 8 x 8 km con relieve, playas, rocas y nieve,
// rodeada de oceano, con unos 2 millones de arboles instanciados (componente
// Vegetacion) y un jugador en tercera persona. Prueba de rendimiento: el HUD
// dice los FPS, los ms de CPU y GPU y cuantos arboles se dibujan.
void buildOpenWorld(project::ProjectInfo& project) {
    Builder b(project);
    b.world.setSceneUuid(Uuid::generate());
    ecs::populateDefaultScene(b.world);
    b.scripts(template_scripts::kPlayer);
    b.scripts(template_scripts::kThirdPersonCamera);
    b.scripts(openworld::kFiles);

    // --- Terreno: relieve fBm con una mascara de isla ---
    constexpr float kSize = 8192.0f;
    constexpr float kHeight = 450.0f;
    constexpr float kSea = 0.15f;  // el mar (y = 0) a este 0..1 del terreno
    terrain::Terrain comp;
    comp.data = "Terrains/Isla.crterrain";
    comp.size = kSize;
    comp.height = kHeight;
    comp.resolution = 1025;
    comp.splat_resolution = 1024;
    comp.lod_distance = 2.0f;
    terrain::TerrainData data;
    data.create(1025, 1024, 0.0f);
    terrain::generateRelief(data, 2027, 3.2f, 0.52f, 0.45f, 0.0f);
    {
        const auto res = static_cast<int>(data.resolution());
        std::vector<float>& h = data.heights();
        for (int y = 0; y < res; ++y) {
            for (int x = 0; x < res; ++x) {
                const float u = static_cast<float>(x) / static_cast<float>(res - 1) * 2.0f - 1.0f;
                const float v = static_cast<float>(y) / static_cast<float>(res - 1) * 2.0f - 1.0f;
                // Costa algo irregular: la distancia al centro deformada.
                const float angle = std::atan2(v, u);
                const float d = std::sqrt(u * u + v * v) * (1.0f + 0.08f * std::sin(angle * 5.0f) + 0.05f * std::cos(angle * 3.0f));
                const float t = std::clamp((0.92f - d) / 0.42f, 0.0f, 1.0f);
                const float mask = t * t * (3.0f - 2.0f * t);
                float& value = h[static_cast<std::size_t>(y) * static_cast<std::size_t>(res) + static_cast<std::size_t>(x)];
                const float land = kSea + 0.02f + value * 0.78f;
                value = 0.02f + (land - 0.02f) * mask;
            }
        }
    }
    // Capas: hierba (0) de base, arena (3) en la costa, roca (2) en los taludes y
    // tierra (1) en lo mas alto.
    terrain::paintByRules(data, comp, 3, 0.0f, 90.0f, 0.0f, kSea + 5.0f / kHeight);
    terrain::paintByRules(data, comp, 2, 34.0f, 90.0f, 0.0f, 1.0f);
    terrain::paintByRules(data, comp, 1, 0.0f, 90.0f, 0.72f, 1.0f);
    {
        const std::filesystem::path path = project.assetsFolder() / "Terrains" / "Isla.crterrain";
        std::filesystem::create_directories(path.parent_path());
        if (!data.save(path)) throw std::runtime_error("no se pudo guardar el terreno");
    }
    const Vec3 origin{-kSize * 0.5f, -kSea * kHeight, -kSize * 0.5f};
    ecs::Entity island = b.world.create("Isla");
    island.setWorldPosition(origin);
    island.add<terrain::Terrain>() = comp;

    // Niebla de mundo abierto: fina y alta (se ve la isla entera desde lo alto).
    if (ecs::Entity env = b.world.findByName("Entorno"); env.valid() && env.has<ecs::PostProcessing>()) {
        env.get<ecs::PostProcessing>().settings.fog_density = 0.00022f;
        env.get<ecs::PostProcessing>().settings.fog_height_falloff = 0.0035f;
    }

    // --- Oceano ---
    ecs::Entity ocean = b.world.create("Oceano");
    ocean.setWorldPosition(Vec3{0.0f, 0.0f, 0.0f});
    ocean.add<water::WaterBody>() = water::oceanPreset();

    // --- Bosque: ~2 millones de arboles en la isla ---
    ecs::Entity forest = b.world.create("Vegetacion");
    forest.setWorldPosition(Vec3{0.0f, 0.0f, 0.0f});
    foliage::Foliage& f = forest.add<foliage::Foliage>();
    f.area = kSize;
    f.density = 700.0f;
    f.seed = 7;
    f.pine = 0.55f;
    f.oak = 0.3f;
    f.birch = 0.15f;
    f.min_height = 4.0f;                          // sobre la playa
    f.max_height = 0.7f * kHeight - kSea * kHeight;  // sin arboles en las cumbres
    f.max_slope = 30.0f;
    f.max_instances = 6000000;
    f.lod1_distance = 110.0f;
    f.lod2_distance = 520.0f;
    f.max_distance = 3000.0f;
    f.shadow_distance = 130.0f;

    // --- Jugador en un claro cerca de la costa sur ---
    const auto ground_at = [&](float x, float z) { return terrain::heightAt(data, comp, origin, x, z); };
    Vec3 spawn{0.0f, 40.0f, 0.0f};
    for (float z = kSize * 0.35f; z > -kSize * 0.35f; z -= 16.0f) {
        const float y = ground_at(0.0f, z);
        const Vec3 n = terrain::normalAt(data, comp, origin, 0.0f, z);
        if (y > 12.0f && y < 60.0f && n.y > 0.95f) {
            spawn = Vec3{0.0f, y + 2.0f, z};
            break;
        }
    }
    // Un claro donde empieza el jugador (sin arboles encima).
    forest.get<foliage::Foliage>().clearings.push_back({spawn, 30.0f});
    const assets::AssetRef body = b.material("Jugador", Vec3{0.95f, 0.45f, 0.10f}, 0.4f);
    const assets::AssetRef visor = b.material("Visor", Vec3{0.05f, 0.06f, 0.08f}, 0.15f, 0.6f);
    ecs::Entity player = b.player(spawn, body, visor);
    // Mas rapido: la isla es grande.
    Builder::attach(player, "Jugador.cpp", {{"velocidad", "9"}, {"correr", "2.6"}});
    ecs::Entity cam = b.world.findByName("Main Camera");
    cam.setWorldPosition(spawn + Vec3{0.0f, 4.0f, 9.0f});
    Builder::attach(cam, "CamaraTercera.cpp",
                    {{"distancia", "8"}, {"inclinacion", "14"}});

    // --- HUD de rendimiento ---
    ecs::Entity canvas = b.world.create("HUD");
    canvas.add<ui::Canvas>();
    ecs::Entity panel = b.world.create("UI_Panel", canvas);
    {
        ui::RectTransform& rt = panel.add<ui::RectTransform>();
        rt.anchor_min = rt.anchor_max = Vec2{0.0f, 0.0f};
        rt.pivot = Vec2{0.0f, 0.0f};
        rt.position = Vec2{20.0f, 20.0f};
        rt.size = Vec2{980.0f, 196.0f};
        ui::Image& im = panel.add<ui::Image>();
        im.color = Vec3{0.05f, 0.06f, 0.08f};
        im.alpha = 0.62f;
        im.corner_radius = 10.0f;
    }
    ecs::Entity text = b.world.create("UI_Rendimiento", panel);
    {
        ui::RectTransform& rt = text.add<ui::RectTransform>();
        rt.anchor_min = Vec2{0.0f, 0.0f};
        rt.anchor_max = Vec2{1.0f, 1.0f};
        rt.size = Vec2{-28.0f, -24.0f};
        ui::Text& t = text.add<ui::Text>();
        t.text = "Midiendo...";
        t.font_size = 22.0f;
        t.h_align = ui::HAlign::Left;
        t.v_align = ui::VAlign::Top;
        t.shadow = true;
        t.wrap = true;
    }
    ecs::Entity perf = b.world.create("Rendimiento");
    Builder::attach(perf, "Rendimiento.cpp");

    b.save("Main");
}

// Tercera persona avanzada: el personaje y las animaciones del Locomotion
// Pack de Mixamo (el del usuario: el motor no puede repartirlo) con un
// Animator Controller (Blend Tree 2D, salto, giros en el sitio), IK de pies,
// mirada y manos, y un circuito para lucirlo. Todo el juego en C++.
void buildThirdPersonPro(project::ProjectInfo& project, std::filesystem::path pack) {
    if (pack.empty()) pack = locomotion::findDownloadedPack();
    if (pack.empty()) {
        throw std::runtime_error(
            "Falta el Locomotion Pack de Mixamo: en mixamo.com elige el pack \"Locomotion Pack\" (FBX, con skin) y "
            "dejalo en Descargas (el .zip o la carpeta descomprimida), o indica donde esta.");
    }
    const locomotion::PackResult pr = locomotion::importPack(pack, project.assetsFolder());
    for (const std::string& line : pr.log) std::cout << "[Plantilla] " << line << "\n";
    if (!pr.ok) throw std::runtime_error(pr.error);

    // --- El Animator Controller ---
    ecs::AnimatorController c;
    c.uuid = Uuid::generate();
    const auto param = [&](const char* name, ecs::AnimatorParameterType type, float value = 0.0f) {
        c.parameters.push_back({name, type, value});
    };
    param("X", ecs::AnimatorParameterType::Float);
    param("Y", ecs::AnimatorParameterType::Float);
    param("Moviendo", ecs::AnimatorParameterType::Bool);
    param("EnSuelo", ecs::AnimatorParameterType::Bool, 1.0f);
    param("Saltar", ecs::AnimatorParameterType::Trigger);
    for (const char* t : {"GirarIzq90", "GirarDer90", "GirarIzq180", "GirarDer180"}) {
        param(t, ecs::AnimatorParameterType::Trigger);
    }
    const auto clip_ref = [&](const std::string& key) {
        const locomotion::ClipInfo* info = pr.clip(key);
        return assets::AssetRef{info != nullptr ? info->uuid : Uuid{}, assets::AssetType::AnimationClip};
    };
    const auto speed_of = [&](const std::string& key, float fallback) {
        const locomotion::ClipInfo* info = pr.clip(key);
        return info != nullptr && info->speed > 0.1f ? info->speed : fallback;
    };
    const float walk = speed_of("walk", 1.6f);
    const float run = speed_of("run", 4.3f);
    const float side_walk = speed_of("strafe_walk_left", 1.7f);
    const float side_run = speed_of("strafe_run_left", 4.4f);

    ecs::AnimatorState moving;
    moving.name = "Locomocion";
    moving.motion = ecs::AnimatorMotion::BlendTree2D;
    moving.blend_parameter = "X";
    moving.blend_parameter_y = "Y";
    moving.position = core::Vec2{0.0f, 0.0f};
    const auto child = [&](const std::string& key, float x, float y, float speed = 1.0f) {
        if (pr.clip(key) == nullptr) return;
        ecs::BlendTreeChild ch;
        ch.clip = clip_ref(key);
        ch.position = core::Vec2{x, y};
        ch.speed = speed;
        moving.children.push_back(ch);
    };
    child("idle", 0.0f, 0.0f);
    child("walk", 0.0f, walk);
    child("run", 0.0f, run);
    child("walk", 0.0f, -walk, -1.0f);  // de espaldas: andar al reves
    child("strafe_walk_left", -side_walk, 0.0f);
    child("strafe_walk_right", side_walk, 0.0f);
    child("strafe_run_left", -side_run, 0.0f);
    child("strafe_run_right", side_run, 0.0f);
    c.states.push_back(moving);

    const auto clip_state = [&](const char* name, const std::string& key, core::Vec2 at) {
        ecs::AnimatorState s;
        s.name = name;
        s.clip = clip_ref(key);
        s.loop = false;
        s.position = at;
        c.states.push_back(s);
        return static_cast<int>(c.states.size()) - 1;
    };
    const int jump = clip_state("Saltar", "jump", core::Vec2{280.0f, -160.0f});
    const locomotion::ClipInfo* jump_info = pr.clip("jump");
    const float jump_duration = jump_info != nullptr ? std::max(jump_info->duration, 0.1f) : 2.0f;
    const float takeoff = jump_info != nullptr && jump_info->landing > jump_info->takeoff ? jump_info->takeoff : 0.8f;
    const float landing = jump_info != nullptr && jump_info->landing > jump_info->takeoff ? jump_info->landing : 1.25f;
    const auto transition = [&](int from, int to, float duration, std::vector<ecs::AnimatorCondition> conditions,
                                float exit_time = -1.0f) {
        ecs::AnimatorTransition t;
        t.from = from;
        t.to = to;
        t.duration = duration;
        t.conditions = std::move(conditions);
        if (exit_time >= 0.0f) {
            t.has_exit_time = true;
            t.exit_time = exit_time;
        }
        c.transitions.push_back(t);
    };
    using Mode = ecs::AnimatorConditionMode;
    transition(ecs::kAnyState, jump, 0.12f, {{"Saltar", Mode::If, 0.0f}});
    transition(jump, 0, 0.25f, {}, 0.97f);
    // Aterrizando y moviendose: vuelve antes a andar o correr.
    transition(jump, 0, 0.2f, {{"Moviendo", Mode::If, 0.0f}, {"EnSuelo", Mode::If, 0.0f}},
               std::min(0.95f, (landing + 0.15f) / jump_duration));
    struct Turn {
        const char* state;
        const char* key;
        const char* trigger;
        core::Vec2 at;
    };
    const Turn turns[] = {{"Girar 90 izq", "turn90_left", "GirarIzq90", {280.0f, 60.0f}},
                          {"Girar 90 der", "turn90_right", "GirarDer90", {280.0f, 140.0f}},
                          {"Girar 180 izq", "turn_left", "GirarIzq180", {280.0f, 220.0f}},
                          {"Girar 180 der", "turn_right", "GirarDer180", {280.0f, 300.0f}}};
    for (const Turn& t : turns) {
        if (pr.clip(t.key) == nullptr) continue;
        const int s = clip_state(t.state, t.key, t.at);
        transition(0, s, 0.15f, {{t.trigger, Mode::If, 0.0f}});
        transition(s, 0, 0.2f, {}, 0.9f);
        transition(s, 0, 0.2f, {{"Moviendo", Mode::If, 0.0f}});  // echa a andar: se corta
    }
    const std::filesystem::path controller_path = project.assetsFolder() / "Animations" / "Personaje.cranimator";
    std::string error;
    if (!ecs::saveAnimatorController(c, controller_path, &error)) throw std::runtime_error("controlador: " + error);

    // --- Escena ---
    Builder b(project);
    b.world.setSceneUuid(Uuid::generate());
    ecs::populateDefaultScene(b.world);
    b.scripts(locomotion_scripts::kFiles);

    const assets::AssetRef floor = b.material("Suelo", Vec3{0.52f, 0.54f, 0.56f}, 0.9f);
    const assets::AssetRef stone = b.material("Piedra", Vec3{0.46f, 0.45f, 0.43f}, 0.85f);
    const assets::AssetRef wood = b.material("Madera", Vec3{0.60f, 0.42f, 0.25f}, 0.75f);
    const assets::AssetRef wall = b.material("Muro", Vec3{0.36f, 0.38f, 0.42f}, 0.8f);
    const assets::AssetRef orange = b.material("Naranja", Vec3{0.95f, 0.52f, 0.18f}, 0.55f);
    const assets::AssetRef blue = b.material("Azul", Vec3{0.16f, 0.50f, 0.92f}, 0.45f);
    const assets::AssetRef metal = b.material("Metal", Vec3{0.62f, 0.63f, 0.66f}, 0.35f, 1.0f);
    const assets::AssetRef red = b.material("Pomo", Vec3{0.85f, 0.12f, 0.10f}, 0.4f);
    const assets::AssetRef crystal = b.material("Cristal", Vec3{0.3f, 0.9f, 1.0f}, 0.1f, 0.0f, Vec3{0.2f, 0.8f, 1.0f}, 2.5f);

    b.box("Suelo", Vec3{0.0f, -0.5f, 0.0f}, Vec3{70.0f, 1.0f, 70.0f}, floor);
    b.ring(35.0f, 2.5f, 1.0f, wall);

    // Escalera de 8 escalones (18 cm), rellano y bajada: los pies se apoyan en
    // cada escalon (IK) y el personaje sube sin saltar.
    const float rise = 0.18f;
    const float tread = 0.4f;
    const float top = rise * 8.0f;
    for (int i = 0; i < 8; ++i) {
        const float h = rise * static_cast<float>(i + 1);
        b.box("Escalon " + std::to_string(i + 1), Vec3{-8.0f, h * 0.5f, 4.0f - tread * static_cast<float>(i) - tread * 0.5f},
              Vec3{3.0f, h, tread}, i % 2 == 0 ? wood : orange);
    }
    b.box("Rellano", Vec3{-8.0f, top * 0.5f, -1.2f}, Vec3{3.0f, top, 4.0f}, wood);
    for (int i = 0; i < 7; ++i) {
        const float h = top - rise * static_cast<float>(i + 1);
        b.box("Bajada " + std::to_string(i + 1), Vec3{-8.0f, h * 0.5f, -3.2f - tread * static_cast<float>(i) - tread * 0.5f},
              Vec3{3.0f, h, tread}, i % 2 == 0 ? orange : wood);
    }
    // Rampa (15 grados) hasta una plataforma.
    const float slope = 15.0f;
    const float ramp_len = 6.0f;
    const float ramp_top = ramp_len * std::sin(slope * 3.14159265f / 180.0f);
    b.box("Rampa", Vec3{8.0f, ramp_top * 0.5f - 0.1f, 4.0f - ramp_len * 0.5f * std::cos(slope * 3.14159265f / 180.0f)},
          Vec3{3.0f, 0.3f, ramp_len}, stone, Vec3{slope, 0.0f, 0.0f});
    b.box("Plataforma", Vec3{8.0f, ramp_top * 0.5f, -3.8f}, Vec3{3.0f, ramp_top, 4.0f}, stone);
    // Piedras de alturas distintas: cada pie pisa a una altura.
    std::mt19937 rng(7);
    const auto range = [&](float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); };
    for (int i = 0; i < 14; ++i) {
        const float h = range(0.08f, 0.32f);
        const Vec3 p{range(-3.0f, 3.0f), h * 0.5f, range(-12.0f, -6.0f)};
        b.box("Roca " + std::to_string(i + 1), p, Vec3{range(0.6f, 1.2f), h, range(0.6f, 1.2f)}, stone,
              Vec3{range(-4.0f, 4.0f), range(0.0f, 90.0f), range(-4.0f, 4.0f)});
    }
    // Bloques para saltar (hay que saltar: mas altos que un escalon).
    const float blocks[] = {0.7f, 1.3f, 1.9f};
    for (int i = 0; i < 3; ++i) {
        const float h = blocks[i];
        b.box("Bloque " + std::to_string(i + 1), Vec3{17.0f, h * 0.5f, 4.0f - 3.2f * static_cast<float>(i)},
              Vec3{2.2f, h, 2.2f}, i % 2 == 0 ? blue : orange);
    }
    // Cajas que se empujan.
    for (int i = 0; i < 4; ++i) {
        ecs::Entity crate = b.box("Caja " + std::to_string(i + 1),
                                  Vec3{3.0f + 1.0f * static_cast<float>(i % 2), 0.4f + 0.8f * static_cast<float>(i / 2), 7.0f},
                                  Vec3{0.8f, 0.8f, 0.8f}, wood);
        physics::Rigidbody& rb = crate.add<physics::Rigidbody>();
        rb.mass = 15.0f;
    }

    // Patio con compuerta (la abre la palanca 1) y camara con otra (palanca 2).
    b.box("Patio muro norte", Vec3{0.0f, 1.5f, -27.0f}, Vec3{12.0f, 3.0f, 0.6f}, wall);
    b.box("Patio muro oeste", Vec3{-6.0f, 1.5f, -22.5f}, Vec3{0.6f, 3.0f, 9.0f}, wall);
    b.box("Patio muro este", Vec3{6.0f, 1.5f, -22.5f}, Vec3{0.6f, 3.0f, 9.0f}, wall);
    b.box("Patio muro sur izq", Vec3{-4.0f, 1.5f, -18.0f}, Vec3{4.0f, 3.0f, 0.6f}, wall);
    b.box("Patio muro sur der", Vec3{4.0f, 1.5f, -18.0f}, Vec3{4.0f, 3.0f, 0.6f}, wall);
    b.box("Camara muro oeste", Vec3{-22.0f, 1.5f, -8.0f}, Vec3{0.6f, 3.0f, 6.0f}, wall);
    b.box("Camara muro norte", Vec3{-19.0f, 1.5f, -11.0f}, Vec3{6.6f, 3.0f, 0.6f}, wall);
    b.box("Camara muro sur", Vec3{-19.0f, 1.5f, -5.0f}, Vec3{6.6f, 3.0f, 0.6f}, wall);
    b.box("Camara techo", Vec3{-19.0f, 3.15f, -8.0f}, Vec3{6.6f, 0.3f, 6.6f}, wall);
    const auto gate = [&](const std::string& name, const Vec3& p, const Vec3& size) {
        ecs::Entity g = b.box(name, p, size, metal);
        g.add<physics::Rigidbody>().type = physics::BodyType::Kinematic;
    };
    gate("Compuerta 1", Vec3{0.0f, 1.5f, -18.0f}, Vec3{4.0f, 3.0f, 0.4f});
    gate("Compuerta 2", Vec3{-16.0f, 1.5f, -8.0f}, Vec3{0.4f, 3.0f, 5.4f});

    const auto lever = [&](const std::string& name, const Vec3& p, float yaw, const std::string& target) {
        ecs::Entity root = b.world.create(name);
        root.setWorldPosition(p);
        root.setLocalEulerDegrees(Vec3{0.0f, yaw, 0.0f});
        root.setTag("Palanca");
        ecs::Entity base = ecs::createPrimitive(b.world, assets::builtin::kCube, "Base", root);
        base.setLocalPosition(Vec3{0.0f, 0.45f, 0.0f});
        base.setLocalScale(Vec3{0.4f, 0.9f, 0.3f});
        base.get<ecs::MeshRenderer>().materials = {metal};
        base.add<physics::BoxCollider>();
        ecs::Entity arm = b.world.create("Brazo", root);
        arm.setLocalPosition(Vec3{0.0f, 0.85f, -0.15f});
        arm.setLocalEulerDegrees(Vec3{10.0f, 0.0f, 0.0f});
        ecs::Entity rod = ecs::createPrimitive(b.world, assets::builtin::kCylinder, "Vara", arm);
        rod.setLocalPosition(Vec3{0.0f, 0.22f, 0.0f});
        rod.setLocalScale(Vec3{0.05f, 0.22f, 0.05f});
        rod.get<ecs::MeshRenderer>().materials = {metal};
        ecs::Entity knob = ecs::createPrimitive(b.world, assets::builtin::kSphere, "Pomo", arm);
        knob.setLocalPosition(Vec3{0.0f, 0.46f, 0.0f});
        knob.setLocalScale(Vec3{0.1f, 0.1f, 0.1f});
        knob.get<ecs::MeshRenderer>().materials = {red};
        Builder::attach(root, "Palanca.cpp", {{"compuerta", Builder::text(target)}});
    };
    // (La palanca mira a -Z: yaw 180 = hacia +Z, por donde llega el jugador.)
    lever("Palanca 1", Vec3{3.8f, 0.0f, -16.8f}, 180.0f, "Compuerta 1");
    lever("Palanca 2", Vec3{-14.5f, 0.0f, -4.2f}, 270.0f, "Compuerta 2");

    // Estatua (el personaje la mira al pasar).
    ecs::Entity statue = ecs::createPrimitive(b.world, assets::builtin::kCylinder, "Pedestal");
    statue.setWorldPosition(Vec3{-3.0f, 0.5f, 2.0f});
    statue.setLocalScale(Vec3{0.7f, 0.5f, 0.7f});
    statue.get<ecs::MeshRenderer>().materials = {stone};
    statue.add<physics::CapsuleCollider>();
    ecs::Entity bust = ecs::createPrimitive(b.world, assets::builtin::kSphere, "Estatua");
    bust.setWorldPosition(Vec3{-3.0f, 1.45f, 2.0f});
    bust.setLocalScale(Vec3{0.45f, 0.45f, 0.45f});
    bust.get<ecs::MeshRenderer>().materials = {metal};
    bust.setTag("Interes");

    const Vec3 gems[] = {{-8.0f, top + 1.0f, -1.2f},   {8.0f, ramp_top + 1.0f, -3.8f}, {17.0f, 1.9f + 1.0f, -2.4f},
                         {0.0f, 1.0f, -9.0f},          {-2.0f, 1.0f, -23.0f},          {3.0f, 1.0f, -25.0f},
                         {-19.5f, 1.0f, -8.0f}};
    int g = 0;
    for (const Vec3& p : gems) {
        ecs::Entity e = ecs::createPrimitive(b.world, assets::builtin::kSphere, "Cristal " + std::to_string(++g));
        e.setWorldPosition(p);
        e.setLocalScale(Vec3{0.28f, 0.45f, 0.28f});
        e.get<ecs::MeshRenderer>().materials = {crystal};
        e.setTag("Cristal");
        Builder::attach(e, "Cristal.cpp");
    }

    // --- El personaje: capsula con Rigidbody, un pivote que gira y el modelo ---
    ecs::Entity player = b.world.create("Jugador");
    player.setWorldPosition(Vec3{0.0f, 0.95f, 12.0f});
    physics::CapsuleCollider& capsule = player.add<physics::CapsuleCollider>();
    capsule.radius = 0.3f;
    capsule.height = 1.8f;
    capsule.material.friction = 0.0f;
    physics::Rigidbody& rb = player.add<physics::Rigidbody>();
    rb.mass = 70.0f;
    rb.lock_rotation_x = rb.lock_rotation_y = rb.lock_rotation_z = true;
    rb.continuous = true;
    const auto num = [](float v) { return Builder::number(v); };
    const auto duration_of = [&](const char* key, float fallback) {
        const locomotion::ClipInfo* info = pr.clip(key);
        return info != nullptr ? info->duration : fallback;
    };
    Builder::attach(player, "Personaje.cpp",
                    {{"velAndar", num(walk)},
                     {"velCorrer", num(run)},
                     {"velLateral", num(side_walk)},
                     {"velLateralCorrer", num(side_run)},
                     {"despegue", num(takeoff)},
                     {"aterrizaje", num(landing)},
                     {"duracionSalto", num(jump_duration)},
                     {"durGiro90", num(duration_of("turn90_left", 0.93f))},
                     {"durGiro180", num(duration_of("turn_left", 1.63f))}});
    ecs::Entity pivot = b.world.create("Pivote", player);
    ecs::Entity model = b.world.create("Modelo", pivot);
    model.setLocalPosition(Vec3{0.0f, -0.9f, 0.0f});
    model.setLocalEulerDegrees(Vec3{0.0f, 180.0f, 0.0f});  // Mixamo mira a +Z; el motor, a -Z
    model.add<ecs::MeshRenderer>().model = assets::AssetRef{pr.character, assets::AssetType::Model};
    ecs::Animator& animator = model.add<ecs::Animator>();
    animator.controller = assets::AssetRef{c.uuid, assets::AssetType::AnimatorController};
    ecs::InverseKinematics& ik = model.add<ecs::InverseKinematics>();
    ik.foot_grounding = true;
    ik.foot_locking = true;  // los pies apoyados no patinan
    ik.max_step = 0.4f;
    ik.look_weight = 0.0f;
    ik.look_max_angle = 75.0f;
    ik.right_hand.weight = 0.0f;
    // Se inclina al arrancar, frenar y girar (la inercia del cuerpo).
    ecs::ProceduralAnimation& body = model.add<ecs::ProceduralAnimation>();
    body.lean = true;
    body.lean_amount = 7.0f;
    model.add<ecs::Skeleton>().show_bones = false;

    // Camara orbital y la interfaz.
    ecs::Entity cam = b.world.findByName("Main Camera");
    cam.setWorldPosition(Vec3{0.0f, 2.5f, 16.0f});
    Builder::attach(cam, "CamaraOrbital.cpp");

    ecs::Entity canvas = b.world.create("HUD");
    canvas.add<ui::Canvas>();
    const auto rect = [&](const std::string& name, Vec2 anchor, Vec2 pivot_at, Vec2 position, Vec2 size) {
        ecs::Entity e = b.world.create(name, canvas);
        ui::RectTransform& rt = e.add<ui::RectTransform>();
        rt.anchor_min = anchor;
        rt.anchor_max = anchor;
        rt.pivot = pivot_at;
        rt.position = position;
        rt.size = size;
        return e;
    };
    const auto label = [&](ecs::Entity e, const std::string& value, float size, ui::HAlign align,
                           ui::VAlign valign = ui::VAlign::Middle) {
        ui::Text& t = e.add<ui::Text>();
        t.text = value;
        t.font_size = size;
        t.h_align = align;
        t.v_align = valign;
        t.shadow = true;
        return e;
    };
    label(rect("Marcador", Vec2{0.0f, 0.0f}, Vec2{0.0f, 0.0f}, Vec2{40.0f, 30.0f}, Vec2{900.0f, 50.0f}), "", 36.0f,
          ui::HAlign::Left);
    label(rect("Ayuda", Vec2{0.0f, 1.0f}, Vec2{0.0f, 1.0f}, Vec2{40.0f, -24.0f}, Vec2{1500.0f, 40.0f}),
          "WASD mover   Shift correr   Espacio saltar   Clic derecho apuntar   E palanca   Rueda distancia   "
          "F1 depuracion   Esc soltar el raton",
          22.0f, ui::HAlign::Left);
    ecs::Entity energy = rect("Energia", Vec2{0.5f, 1.0f}, Vec2{0.5f, 1.0f}, Vec2{0.0f, -80.0f}, Vec2{420.0f, 16.0f});
    ui::Slider& bar = energy.add<ui::Slider>();
    bar.interactable = false;
    bar.value = 1.0f;
    bar.fill = Vec3{0.35f, 0.85f, 0.35f};
    bar.handle = bar.fill;
    label(rect("Aviso", Vec2{0.5f, 1.0f}, Vec2{0.5f, 1.0f}, Vec2{0.0f, -140.0f}, Vec2{900.0f, 50.0f}), "", 30.0f,
          ui::HAlign::Center);
    label(rect("Depuracion", Vec2{0.0f, 0.0f}, Vec2{0.0f, 0.0f}, Vec2{40.0f, 100.0f}, Vec2{900.0f, 180.0f}), "", 24.0f,
          ui::HAlign::Left, ui::VAlign::Top);
    Builder::attach(canvas, "Interfaz.cpp");
    b.save("Main");

    std::vector<std::string> tags = ecs::defaultTags();
    for (const char* t : {"Palanca", "Cristal", "Interes"}) tags.push_back(t);
    ecs::saveTags(project.settingsFolder() / "Tags.json", tags);
}

void copyFolder(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code error;
    if (!std::filesystem::is_directory(from, error)) return;
    std::filesystem::create_directories(to, error);
    std::filesystem::copy(from, to,
                          std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                          error);
    if (error) throw std::runtime_error("no se pudo copiar " + from.string() + ": " + error.message());
}

std::string safeFolderName(const std::string& name) {
    std::string out;
    for (const char c : name) {
        out += (std::string("<>:\"/\\|?*").find(c) != std::string::npos || static_cast<unsigned char>(c) < 32) ? '_' : c;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out.empty() ? "Plantilla" : out;
}

std::filesystem::path fromUtf8(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

}  // namespace

// ================================================================================

std::filesystem::path userTemplatesFolder() {
    std::filesystem::path base;
    char* local = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&local, &length, "LOCALAPPDATA") == 0 && local != nullptr && *local != '\0') {
        base = local;
        std::free(local);
    } else {
        std::free(local);
        std::error_code error;
        base = std::filesystem::temp_directory_path(error);
    }
    return base / "Cramion" / "Templates";
}

// --- Plataformas 2D (2.1) ------------------------------------------------------------

namespace pixel2d {

struct Canvas {
    int w = 0, h = 0;
    std::vector<std::uint8_t> rgba;
    Canvas(int width, int height) : w(width), h(height), rgba(static_cast<std::size_t>(width * height * 4), 0) {}
    void set(int x, int y, std::uint32_t c) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        std::uint8_t* p = &rgba[static_cast<std::size_t>((y * w + x) * 4)];
        p[0] = static_cast<std::uint8_t>(c >> 24);
        p[1] = static_cast<std::uint8_t>(c >> 16);
        p[2] = static_cast<std::uint8_t>(c >> 8);
        p[3] = static_cast<std::uint8_t>(c);
    }
    void rect(int x0, int y0, int x1, int y1, std::uint32_t c) {
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) set(x, y, c);
        }
    }
    void save(const std::filesystem::path& file) const {
        asset::ImageRgba8 image;
        image.width = static_cast<std::uint32_t>(w);
        image.height = static_cast<std::uint32_t>(h);
        image.pixels = rgba;
        std::filesystem::create_directories(file.parent_path());
        if (!asset::saveImagePng(file, image)) throw std::runtime_error("no se pudo escribir " + file.string());
    }
};

// Celda (col, fila) de 16 px de un terreno: hierba arriba, tierra con
// piedrecitas y bordes mas oscuros segun el lado.
void terrainTile(Canvas& c, int col, int row, bool top, bool bottom, bool left, bool right, std::mt19937& rng) {
    const int ox = col * 16, oy = row * 16;
    std::uniform_int_distribution<int> noise(0, 9);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            std::uint32_t color = noise(rng) < 2 ? 0x7A4A28FFu : 0x8B5A33FFu;  // tierra
            if (noise(rng) == 0) color = 0x9E9A92FFu;                           // piedrecita
            if (top && y < 4) color = y == 3 ? 0x3E8C2AFFu : (noise(rng) < 3 ? 0x5DB83AFFu : 0x4FA832FFu);
            if (left && x == 0) color = 0x5A3519FFu;
            if (right && x == 15) color = 0x5A3519FFu;
            if (bottom && y == 15) color = 0x5A3519FFu;
            c.set(ox + x, oy + y, color);
        }
    }
}

}  // namespace pixel2d

void buildPlatformer2D(project::ProjectInfo& project) {
    Builder b(project);
    b.world.setSceneUuid(Uuid::generate());
    ecs::populateDefaultScene(b.world);
    b.scripts(template21::kPlatformer2D);

    // --- Imagenes (pixel art de 16 px, 16 pixeles por unidad) ---
    const std::filesystem::path art = project.assetsFolder() / "2D";
    std::mt19937 rng(7);
    pixel2d::Canvas tiles(64, 64);
    // Terreno 3x3 (celdas 0-2, 4-6, 8-10): esquinas, bordes y centro.
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            pixel2d::terrainTile(tiles, col, row, row == 0, row == 2, col == 0, col == 2, rng);
        }
    }
    // 3: bloque de piedra (plataformas).
    tiles.rect(48, 0, 63, 15, 0x8C8C94FFu);
    tiles.rect(48, 0, 63, 1, 0xB4B4BCFFu);
    tiles.rect(48, 14, 63, 15, 0x5E5E66FFu);
    tiles.rect(55, 2, 56, 13, 0x6E6E76FFu);
    // 7: flores (decoracion, sin colision).
    tiles.rect(52, 26, 52, 31, 0x2F8A2AFFu);
    tiles.rect(51, 24, 53, 25, 0xF2C230FFu);
    tiles.rect(58, 27, 58, 31, 0x2F8A2AFFu);
    tiles.rect(57, 25, 59, 26, 0xE8505AFFu);
    tiles.save(art / "terreno.png");

    // Jugador: 4 cortes de 16 x 16 (quieto, correr x2, saltar).
    pixel2d::Canvas hero(64, 16);
    for (int f = 0; f < 4; ++f) {
        const int ox = f * 16;
        hero.rect(ox + 5, 1, ox + 10, 6, 0xF2C29AFFu);   // cabeza
        hero.rect(ox + 4, 0, ox + 11, 1, 0x6B3B1EFFu);   // pelo
        hero.set(ox + 9, 3, 0x1A1A1AFFu);                 // ojo
        hero.rect(ox + 4, 7, ox + 11, 11, 0x2E7BE6FFu);  // cuerpo
        const int step = f == 1 ? 1 : (f == 2 ? -1 : 0);
        if (f == 3) {
            hero.rect(ox + 4, 12, ox + 6, 13, 0x23314AFFu);
            hero.rect(ox + 9, 12, ox + 11, 13, 0x23314AFFu);
        } else {
            hero.rect(ox + 5 + step, 12, ox + 6 + step, 15, 0x23314AFFu);
            hero.rect(ox + 9 - step, 12, ox + 10 - step, 15, 0x23314AFFu);
        }
    }
    hero.save(art / "jugador.png");
    twod::SpriteSheet hero_sheet;
    hero_sheet.mode = twod::SpriteMode::Multiple;
    hero_sheet.pixels_per_unit = 16.0f;
    hero_sheet.filter = twod::SpriteFilter::Point;
    hero_sheet.width = 64;
    hero_sheet.height = 16;
    hero_sheet.frames = twod::sliceGrid(64, 16, 16, 16, 0, 0, 0, 0, "jugador");
    twod::saveSpriteSheet(art / "jugador.png", hero_sheet);

    // Moneda.
    pixel2d::Canvas coin(16, 16);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const float dx = static_cast<float>(x) - 7.5f;
            const float dy = static_cast<float>(y) - 7.5f;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d < 6.5f) coin.set(x, y, d > 5.2f ? 0xC9901AFFu : (dx + dy < -3.0f ? 0xFFF0A0FFu : 0xF7C531FFu));
        }
    }
    coin.save(art / "moneda.png");
    twod::SpriteSheet coin_sheet;
    coin_sheet.pixels_per_unit = 16.0f;
    coin_sheet.filter = twod::SpriteFilter::Point;
    twod::saveSpriteSheet(art / "moneda.png", coin_sheet);

    // Tileset con la Rule Tile del terreno y las flores sin colision.
    twod::Tileset tileset;
    tileset.image = "2D/terreno.png";
    tileset.tile_width = 16;
    tileset.tile_height = 16;
    tileset.filter = twod::SpriteFilter::Point;
    tileset.no_collider = {7};
    tileset.rule_tiles.push_back(twod::makeTerrainRuleTile("Terreno", 0, 4));
    std::string error;
    if (!twod::saveTileset(art / "Terreno.crtileset", tileset, &error)) throw std::runtime_error(error);

    // --- Escena ---
    ecs::Entity cam = b.world.findByName("Main Camera");
    cam.setWorldPosition(Vec3{0.0f, 1.5f, 10.0f});
    cam.setLocalEulerDegrees(Vec3{0.0f, 0.0f, 0.0f});
    if (ecs::Camera* c = cam.tryGet<ecs::Camera>()) {
        c->orthographic = true;
        c->ortho_size = 6.0f;
    }
    Builder::attach(cam, "Camara2D.cpp");

    ecs::Entity level = b.world.create("Nivel");
    twod::Tilemap& map = level.add<twod::Tilemap>();
    map.tileset = "2D/Terreno.crtileset";
    map.layers = {twod::TilemapLayer{}, twod::TilemapLayer{}};
    map.layers[0].name = "Suelo";
    map.layers[1].name = "Detalles";
    map.layers[1].collision = false;
    map.layers[1].order = 1;
    const auto ground = [&](int x0, int x1, int top) {
        for (int x = x0; x <= x1; ++x) {
            for (int y = top - 3; y <= top; ++y) map.setTile(x, y, -1, 0);
        }
    };
    ground(-12, 8, -1);
    ground(12, 22, -1);
    ground(26, 34, 0);
    ground(36, 46, 1);
    for (int x = 4; x <= 6; ++x) map.setTile(x, 3, 4, 0);    // plataformas de piedra
    for (int x = 15; x <= 18; ++x) map.setTile(x, 4, 4, 0);
    for (int x = 28; x <= 30; ++x) map.setTile(x, 5, 4, 0);
    for (const int x : {-6, -2, 14, 19, 31, 40}) map.setTile(x, x > 34 ? 2 : (x > 24 ? 1 : 0), 8, 1);  // flores
    level.add<twod::TilemapCollider2D>();

    ecs::Entity player = b.world.create("Jugador");
    player.setWorldPosition(Vec3{0.0f, 1.5f, 0.0f});
    twod::SpriteRenderer& sprite = player.add<twod::SpriteRenderer>();
    sprite.sprite = "2D/jugador.png";
    sprite.order_in_layer = 10;
    sprite.alpha_cutoff = 0.5f;
    twod::SpriteAnimator& anim = player.add<twod::SpriteAnimator>();
    anim.clips = {twod::SpriteClip{"Quieto", "0", 4.0f, true}, twod::SpriteClip{"Correr", "1-2", 10.0f, true},
                  twod::SpriteClip{"Saltar", "3", 4.0f, true}};
    anim.default_clip = "Quieto";
    twod::Rigidbody2D& body = player.add<twod::Rigidbody2D>();
    body.freeze_rotation = true;
    body.mass = 1.0f;
    twod::BoxCollider2D& box = player.add<twod::BoxCollider2D>();
    box.size = core::Vec2{0.6f, 0.95f};
    box.material.friction = 0.0f;
    Builder::attach(player, "Jugador2D.cpp");

    const Vec3 coins[] = {{5, 4.6f, 0}, {16.5f, 5.6f, 0}, {29, 6.6f, 0}, {10, 1.0f, 0}, {24, 2.0f, 0}, {41, 3.0f, 0}, {-8, 0.6f, 0}};
    int n = 0;
    for (const Vec3& p : coins) {
        ecs::Entity c = b.world.create("Moneda " + std::to_string(++n));
        c.setWorldPosition(p);
        c.setTag("Moneda");
        twod::SpriteRenderer& s = c.add<twod::SpriteRenderer>();
        s.sprite = "2D/moneda.png";
        s.order_in_layer = 5;
        s.alpha_cutoff = 0.5f;
        twod::CircleCollider2D& col = c.add<twod::CircleCollider2D>();
        col.radius = 0.4f;
        col.material.is_trigger = true;
    }

    ecs::Entity marcador = b.hud("");
    marcador.get<ui::Text>().text = "Monedas: 0   (A/D moverse, Espacio saltar)";
    b.save("Main");

    std::vector<std::string> tags = ecs::defaultTags();
    tags.push_back("Moneda");
    ecs::saveTags(project.settingsFolder() / "Tags.json", tags);
}

// --- Coches (2.1) -------------------------------------------------------------------

void buildCars(project::ProjectInfo& project) {
    Builder b(project);
    b.world.setSceneUuid(Uuid::generate());
    ecs::populateDefaultScene(b.world);
    b.scripts(template21::kCars);

    const assets::AssetRef asphalt = b.material("Asfalto", Vec3{0.22f, 0.23f, 0.25f}, 0.85f);
    const assets::AssetRef line = b.material("Linea", Vec3{0.95f, 0.95f, 0.9f}, 0.6f);
    const assets::AssetRef ramp = b.material("Rampa", Vec3{0.95f, 0.55f, 0.15f}, 0.6f);
    const assets::AssetRef barrier = b.material("Barrera", Vec3{0.85f, 0.15f, 0.12f}, 0.5f);
    const assets::AssetRef paint = b.material("Pintura", Vec3{0.08f, 0.35f, 0.85f}, 0.3f, 0.6f);
    const assets::AssetRef glass = b.material("Cristal", Vec3{0.05f, 0.06f, 0.08f}, 0.1f, 0.8f);
    const assets::AssetRef tyre = b.material("Neumatico", Vec3{0.05f, 0.05f, 0.05f}, 0.95f);

    b.box("Suelo", Vec3{0.0f, -0.5f, 0.0f}, Vec3{300.0f, 1.0f, 300.0f}, asphalt);
    for (int i = -6; i <= 6; ++i) {
        ecs::Entity l = ecs::createPrimitive(b.world, assets::builtin::kCube, "Linea " + std::to_string(i + 7));
        l.setLocalPosition(Vec3{0.0f, 0.01f, static_cast<float>(i) * 10.0f});
        l.setLocalScale(Vec3{0.25f, 0.02f, 4.0f});
        l.get<ecs::MeshRenderer>().materials = {line};
    }
    b.box("Rampa 1", Vec3{0.0f, 1.0f, -60.0f}, Vec3{6.0f, 0.4f, 12.0f}, ramp, Vec3{14.0f, 0.0f, 0.0f});
    b.box("Rampa 2", Vec3{40.0f, 0.8f, 0.0f}, Vec3{12.0f, 0.4f, 6.0f}, ramp, Vec3{0.0f, 0.0f, -12.0f});
    b.ring(120.0f, 1.2f, 1.0f, barrier);
    for (int i = 0; i < 8; ++i) {
        b.box("Cono " + std::to_string(i + 1), Vec3{-20.0f + static_cast<float>(i) * 4.0f, 0.4f, -25.0f}, Vec3{0.5f, 0.8f, 0.5f},
              ramp);
    }

    // El coche: Rigidbody + Box Collider + Vehicle; 4 Wheel Collider con su
    // rueda visible (un pivote que gira y el cilindro dentro). El frente es -Z.
    ecs::Entity car = b.world.create("Coche");
    car.setWorldPosition(Vec3{0.0f, 1.2f, 0.0f});
    physics::BoxCollider& col = car.add<physics::BoxCollider>();
    col.size = Vec3{1.8f, 0.6f, 4.2f};
    physics::Rigidbody& rb = car.add<physics::Rigidbody>();
    rb.mass = 1200.0f;
    rb.continuous = true;
    physics::Vehicle& vehicle = car.add<physics::Vehicle>();
    vehicle.keyboard = true;
    vehicle.center_of_mass_offset = Vec3{0.0f, -0.35f, 0.0f};
    ecs::Entity shell = ecs::createPrimitive(b.world, assets::builtin::kCube, "Carroceria", car);
    shell.setLocalScale(Vec3{1.8f, 0.6f, 4.2f});
    shell.get<ecs::MeshRenderer>().materials = {paint};
    ecs::Entity cabin = ecs::createPrimitive(b.world, assets::builtin::kCube, "Cabina", car);
    cabin.setLocalPosition(Vec3{0.0f, 0.5f, 0.3f});
    cabin.setLocalScale(Vec3{1.5f, 0.5f, 2.0f});
    cabin.get<ecs::MeshRenderer>().materials = {glass};
    const Vec3 offsets[4] = {{-0.95f, -0.3f, -1.35f}, {0.95f, -0.3f, -1.35f}, {-0.95f, -0.3f, 1.35f}, {0.95f, -0.3f, 1.35f}};
    const char* names[4] = {"Rueda delantera izquierda", "Rueda delantera derecha", "Rueda trasera izquierda", "Rueda trasera derecha"};
    for (int i = 0; i < 4; ++i) {
        ecs::Entity pivot = b.world.create(names[i], car);
        pivot.setLocalPosition(offsets[i]);
        ecs::Entity rim = ecs::createPrimitive(b.world, assets::builtin::kCylinder, "Neumatico", pivot);
        rim.setLocalEulerDegrees(Vec3{0.0f, 0.0f, 90.0f});
        rim.setLocalScale(Vec3{0.76f, 0.25f, 0.76f});
        rim.get<ecs::MeshRenderer>().materials = {tyre};
        ecs::Entity wheel = b.world.create(std::string(names[i]) + " (Wheel Collider)", car);
        wheel.setLocalPosition(offsets[i]);
        physics::WheelCollider& wc = wheel.add<physics::WheelCollider>();
        wc.max_steer_angle = i < 2 ? 32.0f : 0.0f;
        wc.drive = i >= 2;  // traccion trasera
        wc.max_handbrake_torque = i < 2 ? 0.0f : 4000.0f;
        wc.visual = pivot.uuid();
    }

    ecs::Entity cam = b.world.findByName("Main Camera");
    cam.setWorldPosition(Vec3{0.0f, 4.0f, 9.0f});
    Builder::attach(cam, "CamaraCoche.cpp");
    ecs::Entity hud = b.hud("Velocimetro.cpp");
    hud.get<ui::Text>().text = "W/S acelerar y frenar, A/D girar, Espacio freno de mano, R volver a la salida";
    b.save("Main");
}

// Realidad virtual (como la VR Template de Unity): el Jugador VR con fisica
// (andar con el stick, girar, saltar, andar por la habitacion), manos que
// cogen y lanzan objetos con fisica (XR Interactor + XR Grabbable) y un panel
// de UI en el mundo que se usa con el rayo y el gatillo, con su script de C++.
void buildVr(project::ProjectInfo& project) {
    xr::registerXrComponents();  // (se guardan en la escena por su nombre)
    Builder b(project);
    b.world.setSceneUuid(Uuid::generate());
    ecs::populateDefaultScene(b.world);
    // La camara la pone el Jugador VR (la de la escena por defecto sobra).
    if (ecs::Entity old = b.world.findByName("Main Camera"); old.valid()) b.world.destroy(old);
    b.script("MenuVR.h", template_vr::kMenuHeader);
    b.script("MenuVR.cpp", template_vr::kMenuSource);

    const assets::AssetRef floor = b.material("Suelo", Vec3{0.55f, 0.57f, 0.6f}, 0.8f);
    const assets::AssetRef wood = b.material("Madera", Vec3{0.55f, 0.36f, 0.22f}, 0.6f);
    const assets::AssetRef dark = b.material("Plastico oscuro", Vec3{0.08f, 0.085f, 0.09f}, 0.45f);
    const assets::AssetRef laser = b.material("Rayo", Vec3{0.2f, 0.6f, 1.0f}, 0.5f, 0.0f, Vec3{0.2f, 0.6f, 1.0f}, 6.0f);
    const assets::AssetRef red = b.material("Rojo", Vec3{0.85f, 0.18f, 0.15f}, 0.45f);
    const assets::AssetRef blue = b.material("Azul", Vec3{0.15f, 0.4f, 0.9f}, 0.45f);
    const assets::AssetRef yellow = b.material("Amarillo", Vec3{0.95f, 0.78f, 0.15f}, 0.45f);
    const assets::AssetRef green = b.material("Verde", Vec3{0.2f, 0.7f, 0.3f}, 0.45f);
    const assets::AssetRef metal = b.material("Metal", Vec3{0.75f, 0.76f, 0.78f}, 0.25f, 1.0f);
    const assets::AssetRef crate = b.material("Caja", Vec3{0.78f, 0.6f, 0.38f}, 0.7f);

    b.box("Suelo", Vec3{0.0f, -0.5f, 0.0f}, Vec3{30.0f, 1.0f, 30.0f}, floor);
    // Una mesa delante del jugador y una estanteria para las pelotas.
    b.box("Mesa", Vec3{0.0f, 0.375f, -0.35f}, Vec3{1.4f, 0.75f, 0.7f}, wood);
    b.box("Estanteria", Vec3{1.25f, 0.45f, -0.6f}, Vec3{0.5f, 0.9f, 0.5f}, wood);

    // Lo que se coge: Rigidbody + collider + XR Grabbable.
    std::vector<ecs::Entity> grabbables;
    const auto grabbable = [&](const std::string& name, const Uuid& shape, const Vec3& position, const Vec3& size,
                               const assets::AssetRef& mat, float mass) {
        ecs::Entity e = ecs::createPrimitive(b.world, shape, name);
        e.setLocalPosition(position);
        e.setLocalScale(size);
        e.get<ecs::MeshRenderer>().materials = {mat};
        if (shape == assets::builtin::kSphere) {
            e.add<physics::SphereCollider>();
        } else if (shape == assets::builtin::kCylinder) {
            e.add<physics::BoxCollider>().size = Vec3{1.0f, 2.0f, 1.0f};  // el cilindro mide 2 de alto
        } else {
            e.add<physics::BoxCollider>();
        }
        physics::Rigidbody& rb = e.add<physics::Rigidbody>();
        rb.mass = mass;
        rb.continuous = true;  // al lanzarlo rapido no atraviesa nada
        e.add<xr::XrGrabbable>();
        grabbables.push_back(e);
        return e;
    };
    const float top = 0.75f;
    grabbable("Cubo rojo", assets::builtin::kCube, Vec3{-0.45f, top + 0.06f, -0.3f}, Vec3{0.12f, 0.12f, 0.12f}, red, 0.4f);
    grabbable("Cubo azul", assets::builtin::kCube, Vec3{-0.25f, top + 0.06f, -0.45f}, Vec3{0.12f, 0.12f, 0.12f}, blue, 0.4f);
    grabbable("Cubo amarillo", assets::builtin::kCube, Vec3{-0.3f, top + 0.18f, -0.4f}, Vec3{0.12f, 0.12f, 0.12f}, yellow,
              0.4f);
    grabbable("Lata", assets::builtin::kCylinder, Vec3{0.05f, top + 0.06f, -0.3f}, Vec3{0.07f, 0.06f, 0.07f}, metal, 0.3f);
    for (int i = 0; i < 3; ++i) {
        grabbable("Pelota " + std::to_string(i + 1), assets::builtin::kSphere,
                  Vec3{1.15f + static_cast<float>(i) * 0.1f, 0.96f, -0.6f}, Vec3{0.11f, 0.11f, 0.11f},
                  i == 0 ? green : i == 1 ? red : yellow, 0.25f);
    }
    // Una linterna: se coge siempre igual (punto de agarre) y alumbra.
    ecs::Entity torch = grabbable("Linterna", assets::builtin::kCylinder, Vec3{0.35f, top + 0.04f, -0.35f},
                                  Vec3{0.05f, 0.11f, 0.05f}, dark, 0.35f);
    torch.setLocalEulerDegrees(Vec3{90.0f, 0.0f, 0.0f});
    xr::XrGrabbable& torch_grab = torch.get<xr::XrGrabbable>();
    torch_grab.snap_to_hand = true;
    torch_grab.attach_rotation = Vec3{-90.0f, 0.0f, 0.0f};  // la punta (+Y) hacia delante, como el mando
    ecs::Entity torch_light = ecs::createLight(b.world, ecs::LightType::Spot, torch);
    torch_light.setName("Haz");
    torch_light.setLocalPosition(Vec3{0.0f, 1.1f, 0.0f});
    torch_light.setLocalEulerDegrees(Vec3{90.0f, 0.0f, 0.0f});  // hacia la punta (+Y del cilindro)
    torch_light.get<ecs::Light>().intensity = 6.0f;
    torch_light.get<ecs::Light>().range = 12.0f;
    // Un muro de cajas para tirarle las pelotas.
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3 - row; ++col) {
            ecs::Entity box = b.box("Caja " + std::to_string(row) + "-" + std::to_string(col),
                                    Vec3{-1.6f + 0.26f * static_cast<float>(col) + 0.13f * static_cast<float>(row),
                                         0.125f + 0.255f * static_cast<float>(row), -2.4f},
                                    Vec3{0.25f, 0.25f, 0.25f}, crate);
            box.add<physics::Rigidbody>().mass = 2.0f;
            box.add<xr::XrGrabbable>();
            grabbables.push_back(box);
        }
    }

    // El Jugador VR: XR Origin (de pie) con su capsula y XR Player.
    ecs::Entity rig = b.world.create("Jugador VR");
    rig.setWorldPosition(Vec3{0.0f, 0.0f, 0.6f});
    rig.add<xr::XrOrigin>();
    physics::CharacterController& cc = rig.add<physics::CharacterController>();
    cc.height = 1.8f;
    cc.radius = 0.25f;
    cc.center = Vec3{0.0f, 0.9f, 0.0f};
    cc.crouch_height = 1.1f;
    cc.rotate_to_movement = false;  // (sin casco, WASD lo mueve para probar)
    cc.walk_speed = 2.5f;
    cc.run_speed = 4.5f;
    cc.crouch_speed = 1.5f;
    cc.jump_height = 0.8f;
    rig.add<xr::XrPlayer>();
    ecs::Entity camera = ecs::createCamera(b.world, rig);
    camera.setName("Main Camera");
    camera.setLocalPosition(Vec3{0.0f, 1.6f, 0.0f});
    camera.get<ecs::Camera>().is_main = true;
    camera.get<ecs::Camera>().near_plane = 0.05f;
    static constexpr const char* kHands[] = {"Mano izquierda", "Mano derecha"};
    for (int h = 0; h < 2; ++h) {
        ecs::Entity hand = b.world.create(kHands[h], rig);
        hand.setLocalPosition(Vec3{h == 0 ? -0.22f : 0.22f, 1.1f, -0.3f});
        xr::XrController& controller = hand.add<xr::XrController>();
        controller.hand = static_cast<xr::Hand>(h);
        xr::XrInteractor& interactor = hand.add<xr::XrInteractor>();
        interactor.ray_material = laser;
        // El mando de Quest 3, justo donde esta el de verdad (cambialo por tus manos).
        ecs::Entity model = ecs::createPrimitive(b.world, assets::builtin::questController(h), "Mando Quest 3", hand);
        model.get<ecs::MeshRenderer>().cast_shadows = ecs::ShadowCasting::Off;
    }

    // El panel: un Canvas en modo Mundo (80 x 56 cm) que mira al jugador.
    ecs::Entity panel = b.world.create("Panel VR");
    panel.setLocalPosition(Vec3{-1.1f, 1.35f, -0.35f});
    panel.setLocalEulerDegrees(Vec3{0.0f, 40.0f, 0.0f});
    ui::Canvas& canvas = panel.add<ui::Canvas>();
    canvas.render_mode = ui::RenderMode::WorldSpace;
    canvas.reference = Vec2{800.0f, 560.0f};
    canvas.pixels_per_meter = 1000.0f;
    const auto element = [&](const std::string& name, ecs::Entity parent, const Vec2& position, const Vec2& size) {
        ecs::Entity e = b.world.create(name, parent);
        ui::RectTransform& rt = e.add<ui::RectTransform>();
        rt.anchor_min = rt.anchor_max = Vec2{0.0f, 0.0f};
        rt.pivot = Vec2{0.0f, 0.0f};
        rt.position = position;
        rt.size = size;
        return e;
    };
    const auto label = [&](ecs::Entity parent, const std::string& text, float size, ui::HAlign align) {
        ecs::Entity e = b.world.create("Texto", parent);
        ui::RectTransform& rt = e.add<ui::RectTransform>();
        rt.anchor_min = Vec2{0.0f, 0.0f};
        rt.anchor_max = Vec2{1.0f, 1.0f};
        rt.size = Vec2{0.0f, 0.0f};
        ui::Text& t = e.add<ui::Text>();
        t.text = text;
        t.font_size = size;
        t.h_align = align;
        t.color = Vec3{1.0f, 1.0f, 1.0f};
        return e;
    };
    ecs::Entity background = element("Fondo", panel, Vec2{0.0f, 0.0f}, Vec2{800.0f, 560.0f});
    ui::Image& bg = background.add<ui::Image>();
    bg.color = Vec3{0.07f, 0.08f, 0.1f};
    bg.alpha = 0.92f;
    bg.corner_radius = 24.0f;
    ecs::Entity title = element("Titulo", panel, Vec2{40.0f, 28.0f}, Vec2{720.0f, 70.0f});
    label(title, "Cramion VR", 54.0f, ui::HAlign::Left);
    ecs::Entity help = element("Ayuda", panel, Vec2{40.0f, 104.0f}, Vec2{720.0f, 110.0f});
    ecs::Entity help_text = label(help,
                                  "Agarre: coger y lanzar (de cerca o con el rayo)\n"
                                  "Gatillo: pulsar esta pantalla  -  Stick izq.: andar  -  Stick der.: girar  -  A: saltar",
                                  25.0f, ui::HAlign::Left);
    help_text.get<ui::Text>().wrap = true;
    help_text.get<ui::Text>().color = Vec3{0.78f, 0.82f, 0.88f};
    ecs::Entity reset = element("Reiniciar objetos", panel, Vec2{40.0f, 236.0f}, Vec2{340.0f, 80.0f});
    ui::Button& reset_button = reset.add<ui::Button>();
    reset_button.normal = Vec3{0.16f, 0.45f, 0.95f};
    reset_button.hover = Vec3{0.3f, 0.58f, 1.0f};
    reset_button.pressed = Vec3{0.1f, 0.32f, 0.75f};
    reset_button.corner_radius = 16.0f;
    reset_button.target = panel.uuid();
    reset_button.on_click = "OnReiniciar";
    label(reset, "Reiniciar objetos", 30.0f, ui::HAlign::Center);
    ecs::Entity smooth = element("Giro suave", panel, Vec2{420.0f, 252.0f}, Vec2{48.0f, 48.0f});
    ui::Toggle& smooth_toggle = smooth.add<ui::Toggle>();
    smooth_toggle.on = false;
    smooth_toggle.target = panel.uuid();
    smooth_toggle.on_change = "OnGiroSuave";
    ecs::Entity smooth_label = element("Texto giro", panel, Vec2{484.0f, 252.0f}, Vec2{280.0f, 48.0f});
    label(smooth_label, "Giro suave", 30.0f, ui::HAlign::Left);
    ecs::Entity speed_label = element("Texto velocidad", panel, Vec2{40.0f, 350.0f}, Vec2{720.0f, 44.0f});
    label(speed_label, "Velocidad al andar", 28.0f, ui::HAlign::Left);
    ecs::Entity speed = element("Velocidad", panel, Vec2{40.0f, 400.0f}, Vec2{720.0f, 50.0f});
    ui::Slider& speed_slider = speed.add<ui::Slider>();
    speed_slider.min = 1.0f;
    speed_slider.max = 5.0f;
    speed_slider.value = cc.walk_speed;
    speed_slider.target = panel.uuid();
    speed_slider.on_change = "OnVelocidad";
    ecs::Entity status = element("Estado", panel, Vec2{40.0f, 480.0f}, Vec2{720.0f, 50.0f});
    ecs::Entity status_text = label(status, "Coge algo con el boton de agarre", 26.0f, ui::HAlign::Left);
    status_text.get<ui::Text>().color = Vec3{0.55f, 0.85f, 0.6f};

    // Su script de C++ (MenuVR.h + MenuVR.cpp): los objetos que reinicia, el
    // jugador y el texto de los avisos.
    scripting::CppScript& script = panel.add<scripting::CppScript>();
    script.script = "Scripts/MenuVR.cpp";
    script.class_name = "MenuVR";
    nlohmann::json objects = nlohmann::json::array();
    for (const ecs::Entity& e : grabbables) objects.push_back({{"$uuid", e.uuid().toString()}});
    script.values = {{"jugador", nlohmann::json{{"$uuid", rig.uuid().toString()}}.dump()},
                     {"objetos", objects.dump()},
                     {"estado", nlohmann::json{{"$uuid", status_text.uuid().toString()}}.dump()}};
    b.save("Main");
}

std::vector<ProjectTemplate> availableTemplates() {
    std::vector<ProjectTemplate> list;
    list.push_back(ProjectTemplate{
        "blank", "Vacío", "Integradas",
        "Un proyecto limpio: cielo, sol, cámara y un suelo. Para empezar desde cero.",
        {"Cielo físico y sol", "Suelo con collider", "Post-proceso global"}, rgba(0, 143, 242), TemplateArt::Blank, {}});
    list.push_back(ProjectTemplate{
        "third_person", "Tercera persona", "Integradas",
        "Un personaje que corre y salta con cámara orbital, plataformas, rampas y monedas que recoger.",
        {"Jugador con Rigidbody (WASD, Shift, Espacio)", "Cámara orbital (clic derecho y rueda)",
         "Monedas con trigger y marcador en el HUD", "Escalera, rampa y plataformas"},
        rgba(242, 140, 40), TemplateArt::ThirdPerson, {}});
    list.push_back(ProjectTemplate{
        "third_person_pro", "Tercera persona avanzada", "Integradas",
        "Un personaje de Mixamo con todas las animaciones del Locomotion Pack: anda, corre, se mueve de lado al "
        "apuntar, gira en el sitio y salta sincronizado con la fisica. Pies en el suelo, mirada y manos con IK. "
        "Todo el juego en C++. Usa tu Locomotion Pack (mixamo.com) desde Descargas.",
        {"Blend Tree 2D con las velocidades medidas de cada animacion (los pies no patinan)",
         "Escalones, rampas y rocas: sube sin saltar y cada pie se apoya con IK",
         "Apuntar (clic derecho): desplazamiento lateral, de espaldas y giros de 90 y 180 grados en el sitio",
         "Salto con el despegue y el aterrizaje de la animacion; palancas con la mano (IK) y camara con muelle"},
        rgba(250, 110, 60), TemplateArt::ThirdPersonPro, locomotion::findDownloadedPack()});
    list.push_back(ProjectTemplate{
        "navigation", "IA y navegación", "Integradas",
        "Escapa de los guardias: patrullan un laberinto con NavMesh y te persiguen si te ven. Llega a la meta.",
        {"NavMesh generado en tiempo real", "Guardias con NavAgent (patrullar / perseguir)",
         "Visión con Navigation.raycast", "Meta, HUD y reinicio del nivel"},
        rgba(60, 200, 110), TemplateArt::Navigation, {}});
    list.push_back(ProjectTemplate{
        "state_machines", "IA con máquinas de estados", "Integradas",
        "Enemigos con una máquina de estados como las de Bolt: patrullan una ruta, te persiguen si te ven, te atacan, "
        "huyen con poca vida y vuelven a casa. Cada estado lo hace su script de C++ (OnStateEnter / OnStateUpdate); ábrela en la ventana Máquina de estados "
        "y mira en Play cómo cambia de estado.",
        {"Máquina Enemigo.crfsm: Patrullar, Perseguir, Atacar, Huir y Volver", "Variables (pizarra) con valores por enemigo",
         "Transiciones con variables, temporizador y expresiones", "F para golpear y HUD con el estado de cada enemigo"},
        rgba(240, 120, 60), TemplateArt::Navigation, {}});
    list.push_back(ProjectTemplate{
        "voxel", "Mundo de bloques", "Integradas",
        "Supervivencia en un mundo infinito de bloques como Minecraft: rompe, recoge, craftea herramientas y construye. "
        "Vida, hambre y aire. Se guarda solo.",
        {"Mundo infinito con biomas, cuevas, arboles y mar", "Inventario de 36 huecos con iconos y 13 recetas de crafteo",
         "Vida, hambre, aire, dano por caida, muerte y reaparicion", "Contorno del bloque, grietas al romper y objetos que se recogen"},
        rgba(110, 190, 70), TemplateArt::Voxel, {}});
    list.push_back(ProjectTemplate{
        "mmo", "MMO RPG", "Integradas",
        "Un mundo de rol online en miniatura: pueblo con misiones y tienda, bosque de lobos, campamento goblin y un jefe. "
        "Otros jugadores cazan a tu lado. Todo el juego esta en C++ (Heroe, Enemigo, Bot, NPC).",
        {"Combate con objetivo (Tab), 4 habilidades con mana, enfriamientos y pociones",
         "5 misiones, dialogos, tienda, inventario de 20 huecos y equipo",
         "Enemigos con IA (aggro, leash, reaparicion), botin y un jefe con ataque en area",
         "Interfaz completa: marcos, barra de habilidades, chat, minimapa y niveles"},
        rgba(210, 160, 60), TemplateArt::Mmo, {}});
    list.push_back(ProjectTemplate{
        "creatures", "Criaturas: IK, ragdoll y phys bones", "Integradas",
        "Un perro que recorre escaleras, una rampa y piedras apoyando cada pata con IK, con la cola y las orejas "
        "fisicas (Phys Bones) y un sombrero enganchado a la cabeza; y un maniqui que cae como un ragdoll. Todo desde C++.",
        {"IK de animales: 4 patas de 3 huesos al suelo, el cuerpo se inclina y la cabeza sigue a la pelota",
         "Phys Bones en la cola, las orejas y la coleta, con colliders en el cuerpo y la cabeza",
         "Ragdoll con Jolt (R y F) que hereda la velocidad y vuelve a la animacion",
         "Esqueleto visible (B), Bone Sockets y los dos modelos con esqueleto generados"},
        rgba(230, 120, 90), TemplateArt::Creatures, {}});
    list.push_back(ProjectTemplate{
        "online", "Online (multijugador)", "Integradas",
        "Una arena multijugador por red: crea una partida o unete con la IP de un amigo. Jugadores sincronizados, chat, "
        "marcador, monedas, un balon con porterias, cajas que se patean y eventos. Todo el juego en C++ (Red.cpp).",
        {"Crear partida o unirse por IP (servidor que tambien juega)", "Jugadores sincronizados con Network.spawn y isMine",
         "Chat con nombres, avisos y marcador con puntos y ping", "Balon, cajas y monedas del servidor; goles y eventos"},
        rgba(80, 170, 255), TemplateArt::Online, {}});
    list.push_back(ProjectTemplate{
        "open_world", "Mundo abierto (rendimiento)", "Integradas",
        "Una isla de 8 x 8 km con relieve, playas y montanas, rodeada de oceano y con unos 2 millones de arboles "
        "instanciados. Recorrela en tercera persona y mide el rendimiento: FPS, ms de GPU y arboles dibujados.",
        {"Terreno de 8 km (1025 x 1025) con capas por altura y pendiente", "Oceano con oleaje y playas",
         "~2 millones de arboles (Vegetacion): recorte y niveles de detalle en la GPU",
         "HUD de rendimiento y teclas para forzar el motor (calidad, densidad, distancia, sombras)"},
        rgba(70, 180, 120), TemplateArt::OpenWorld, {}});
    list.push_back(ProjectTemplate{
        "platformer_2d", "Plataformas 2D", "Integradas",
        "Un juego de plataformas en 2D con pixel art: corre, salta, recoge monedas y no te caigas. Tilemap con Rule "
        "Tiles, sprites animados, física 2D y cámara que sigue al jugador. Todo el juego en C++.",
        {"Tilemap con Rule Tiles (bordes y esquinas solos) y Tilemap Collider 2D",
         "Jugador con Rigidbody 2D, Sprite Animator (quieto, correr, saltar) y volteo",
         "Monedas con trigger 2D y marcador en el HUD", "Cámara ortográfica que sigue al jugador"},
        rgba(120, 200, 80), TemplateArt::Blank, {}});
    list.push_back(ProjectTemplate{
        "vr", "Realidad virtual (VR)", "Integradas",
        "Como la VR Template de Unity: juega con el casco en primera persona, coge y lanza objetos con fisica con las "
        "manos (de cerca o con el rayo) y usa un panel de UI en el mundo con el gatillo. El panel tiene su script de "
        "C++ (MenuVR).",
        {"Jugador VR con fisica: andar, girar por pasos o suave, saltar, de pie o sentado",
         "Manos con XR Interactor: agarre para coger, rayo para coger a distancia y laser",
         "Objetos con XR Grabbable: siguen a la mano con fisica y se lanzan; una linterna con punto de agarre",
         "Panel de UI en el mundo (boton, casilla y slider) que se pulsa con el rayo y el gatillo"},
        rgba(90, 160, 255), TemplateArt::Blank, {}});
    list.push_back(ProjectTemplate{
        "cars", "Coches", "Integradas",
        "Un coche que se conduce en una pista con rampas y conos: motor con marchas, suspensión, freno de mano y "
        "cámara de persecución. Hecho con Vehicle y Wheel Collider.",
        {"Vehicle con motor, cambio automático y tracción trasera", "4 Wheel Collider con ruedas visibles que giran",
         "Cámara de persecución con muelle", "Velocímetro (km/h, marcha, rpm) y R para volver a la salida"},
        rgba(80, 140, 255), TemplateArt::ThirdPerson, {}});

    // Del usuario.
    std::error_code error;
    const std::filesystem::path root = userTemplatesFolder();
    for (std::filesystem::directory_iterator it(root, error); !error && it != std::filesystem::directory_iterator();
         it.increment(error)) {
        const std::filesystem::path json_file = it->path() / "template.json";
        std::ifstream in(json_file, std::ios::binary);
        if (!in) continue;
        const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
        if (json.is_discarded() || !json.is_object()) continue;
        ProjectTemplate t;
        const std::u8string folder_name = it->path().filename().u8string();
        t.id = "user:" + std::string(folder_name.begin(), folder_name.end());
        t.name = json.value("name", std::string(folder_name.begin(), folder_name.end()));
        t.description = json.value("description", std::string("Plantilla guardada desde un proyecto."));
        t.category = "Mis plantillas";
        if (json.contains("features") && json["features"].is_array()) {
            for (const auto& f : json["features"]) {
                if (f.is_string()) t.features.push_back(f.get<std::string>());
            }
        }
        t.accent = json.value("accent", rgba(170, 110, 240));
        t.art = TemplateArt::User;
        t.folder = it->path();
        list.push_back(std::move(t));
    }
    return list;
}

project::ProjectInfo createProjectFromTemplate(const ProjectTemplate& t, const std::filesystem::path& parent,
                                               const std::string& name) {
    std::filesystem::create_directories(parent);
    project::ProjectInfo info = project::createProject(parent, name);
    try {
        if (t.art == TemplateArt::User) {
            copyFolder(t.folder / "Assets", info.assetsFolder());
            copyFolder(t.folder / "ProjectSettings", info.settingsFolder());
            std::ifstream in(t.folder / "template.json", std::ios::binary);
            const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
            if (!json.is_discarded() && json.contains("startup_scene") && json["startup_scene"].is_string()) {
                info.startup_scene = Uuid::parse(json["startup_scene"].get<std::string>());
            }
        } else if (t.id == "third_person") {
            buildThirdPerson(info);
        } else if (t.id == "third_person_pro") {
            buildThirdPersonPro(info, t.folder);
        } else if (t.id == "navigation") {
            buildNavigation(info);
        } else if (t.id == "state_machines") {
            buildStateMachines(info);
        } else if (t.id == "voxel") {
            buildVoxel(info);
        } else if (t.id == "mmo") {
            buildMmo(info);
        } else if (t.id == "open_world") {
            buildOpenWorld(info);
        } else if (t.id == "online") {
            buildOnline(info);
        } else if (t.id == "creatures") {
            buildCreatures(info);
        } else if (t.id == "platformer_2d") {
            buildPlatformer2D(info);
        } else if (t.id == "cars") {
            buildCars(info);
        } else if (t.id == "vr") {
            buildVr(info);
        } else {
            buildBlank(info);
        }
        project::saveProject(info);
    } catch (...) {
        // Nada a medias: la carpeta la acaba de crear createProject.
        std::error_code error;
        std::filesystem::remove_all(info.folder, error);
        throw;
    }
    return info;
}

bool saveProjectAsTemplate(const project::ProjectInfo& project, const std::string& name,
                           const std::string& description, std::string* error) {
    try {
        const std::filesystem::path folder = userTemplatesFolder() / fromUtf8(safeFolderName(name));
        std::error_code ec;
        std::filesystem::remove_all(folder, ec);
        std::filesystem::create_directories(folder);
        copyFolder(project.assetsFolder(), folder / "Assets");
        copyFolder(project.settingsFolder(), folder / "ProjectSettings");
        nlohmann::json json;
        json["name"] = name;
        json["description"] = description.empty() ? "Plantilla creada desde \"" + project.name + "\"." : description;
        json["startup_scene"] = project.startup_scene.valid() ? project.startup_scene.toString() : std::string();
        json["accent"] = rgba(170, 110, 240);
        std::ofstream out(folder / "template.json", std::ios::binary | std::ios::trunc);
        out << json.dump(2);
        if (!out) throw std::runtime_error("no se pudo escribir template.json");
        return true;
    } catch (const std::exception& e) {
        if (error != nullptr) *error = e.what();
        return false;
    }
}

}  // namespace cramion::editor
