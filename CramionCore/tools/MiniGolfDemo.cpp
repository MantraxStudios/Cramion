// cramion_minigolf: crea el proyecto de demostracion "MiniGolf" con el kit de
// Kenney (Minigolf Kit): menu, 5 hoyos, resultados, musica y efectos.
//
//   cramion_minigolf <kit/Models/FBX format> <tools/minigolf> <carpeta destino> [--probar]
//
// <tools/minigolf> tiene Scripts/*.lua y Audio/*.wav (golf_audio.py). Con
// --probar, ademas juega cada hoyo sin ventana (fisica + scripts) con un
// piloto automatico: la pelota tiene que acabar en el hoyo y el nivel pedir la
// escena siguiente.

#include "CramionCore/asset/AssetDatabase.h"
#include "CramionCore/asset/AssetManager.h"
#include "CramionCore/asset/Importer.h"
#include "CramionCore/asset/MaterialAsset.h"
#include "CramionCore/audio/Audio.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/ModelInstantiation.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/project/Project.h"
#include "CramionCore/scripting/Scripting.h"
#include "CramionCore/ui/UI.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <optional>
#include <stdexcept>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace cramion;
using core::Vec2;
using core::Vec3;
namespace fs = std::filesystem;

constexpr float kImportScale = 0.03f;  // el kit esta en cm: casillas de 3 m
constexpr float kTile = 3.0f;
constexpr float kFloor = 6.333f * kImportScale;  // alto del cesped de las casillas
constexpr float kBallRadius = 3.497f * kImportScale;

const std::vector<std::string> kModels = {
    "straight", "corner", "round-corner-a", "end", "hole-round", "obstacle-block", "bump-walls",
    "bump-down-walls", "tunnel-wide", "tunnel-narrow", "castle", "windmill", "hill-round", "narrow-block",
    "ball-red", "flag-red", "flag-blue", "club-red",
};

// Aberturas de cada pieza en su espacio (x, z), de los mapas del kit.
std::set<std::pair<int, int>> openingsOf(const std::string& model) {
    if (model == "corner" || model == "round-corner-a") return {{-1, 0}, {0, -1}};
    if (model == "end" || model == "hole-round") return {{0, -1}};
    return {{0, -1}, {0, 1}};  // rectas y obstaculos
}

// Giro en Y (0/90/180/270) que lleva las aberturas del modelo a las pedidas.
std::optional<float> yawFor(const std::string& model, const std::set<std::pair<int, int>>& wanted) {
    for (int k = 0; k < 4; ++k) {
        const float a = static_cast<float>(k) * 90.0f;
        const float c = std::cos(a * 3.14159265f / 180.0f), s = std::sin(a * 3.14159265f / 180.0f);
        std::set<std::pair<int, int>> rotated;
        for (auto [x, z] : openingsOf(model)) {
            // R = Ry: x' = c x + s z, z' = -s x + c z
            rotated.insert({static_cast<int>(std::lround(c * x + s * z)), static_cast<int>(std::lround(-s * x + c * z))});
        }
        if (rotated == wanted) return a;
    }
    return std::nullopt;
}

struct Cell {
    int x = 0;
    int z = 0;
    std::string tile;  // vacio: recta o curva segun el camino
};

struct Level {
    std::string scene;
    std::string name;
    int par = 3;
    std::string next;
    std::vector<Cell> path;  // del tee al hoyo
};

const std::vector<Level> kLevels = {
    {"Nivel1", "Primer golpe", 2, "Nivel2", {{0, 0}, {0, -1}, {0, -2}, {0, -3}}},
    {"Nivel2", "La curva", 3, "Nivel3",
     {{0, 0}, {0, -1}, {0, -2}, {1, -2}, {2, -2, "obstacle-block"}, {3, -2}, {4, -2}, {4, -3}, {4, -4}}},
    {"Nivel3", "El tunel", 3, "Nivel4",
     {{0, 0}, {0, -1, "bump-walls"}, {0, -2, "tunnel-wide"}, {0, -3}, {0, -4, "round-corner-a"}, {-1, -4}, {-2, -4}}},
    {"Nivel4", "El castillo", 3, "Nivel5",
     {{0, 0}, {0, -1, "hill-round"}, {0, -2}, {1, -2, "castle"}, {2, -2}, {2, -3, "narrow-block"}, {2, -4}}},
    {"Nivel5", "El molino", 4, "Resultados",
     {{0, 0}, {0, -1}, {0, -2, "windmill"}, {0, -3}, {-1, -3, "bump-down-walls"}, {-2, -3, "tunnel-narrow"}, {-3, -3},
      {-3, -4, "obstacle-block"}, {-3, -5}}},
};

struct Builder {
    assets::AssetDatabase& database;
    assets::AssetManager& manager;
    std::map<std::string, Uuid> models;
    std::map<std::string, Uuid> materials;

    ecs::Entity model(ecs::World& world, const std::string& name, ecs::Entity parent = {}) {
        const auto asset = manager.loadModel(models.at(name));
        if (!asset) throw std::runtime_error("no se pudo cargar " + name);
        ecs::Entity e = ecs::instantiateModel(world, *asset, parent);
        e.setName(name);
        return e;
    }

    static void forEachDescendant(ecs::Entity e, const std::function<void(ecs::Entity)>& fn) {
        fn(e);
        for (std::size_t i = 0; i < e.childCount(); ++i) forEachDescendant(e.child(i), fn);
    }

    void setMaterial(ecs::Entity e, const std::string& material) {
        if (ecs::MeshRenderer* mr = e.tryGet<ecs::MeshRenderer>()) {
            mr->materials = {assets::AssetRef{materials.at(material), assets::AssetType::Material}};
        }
    }

    // --- Campo ---
    struct Course {
        Vec3 tee;
        Vec3 hole;
        float tee_yaw = 0.0f;
        Vec3 center;
    };

    Course course(ecs::World& world, const Level& level, bool decorative) {
        ecs::Entity root = world.create("Campo");
        Course c;
        Vec3 low{1e9f, 0, 1e9f}, high{-1e9f, 0, -1e9f};
        const auto& p = level.path;
        for (std::size_t i = 0; i < p.size(); ++i) {
            std::set<std::pair<int, int>> open;
            if (i > 0) open.insert({p[i - 1].x - p[i].x, p[i - 1].z - p[i].z});
            if (i + 1 < p.size()) open.insert({p[i + 1].x - p[i].x, p[i + 1].z - p[i].z});
            std::string tile = p[i].tile;
            if (i == 0) tile = "end";
            else if (i + 1 == p.size()) tile = "hole-round";
            else if (tile.empty()) {
                const auto a = *open.begin();
                const auto b = *open.rbegin();
                tile = (a.first + b.first == 0 && a.second + b.second == 0) ? "straight" : "corner";
            }
            const std::optional<float> yaw = yawFor(tile, open);
            if (!yaw) throw std::runtime_error(level.scene + ": la pieza " + tile + " no encaja en la casilla " + std::to_string(i));
            ecs::Entity t = model(world, tile, root);
            const Vec3 pos{p[i].x * kTile, 0.0f, p[i].z * kTile};
            t.setLocalPosition(pos);
            t.setLocalEulerDegrees(Vec3{0.0f, *yaw, 0.0f});
            low = Vec3{std::min(low.x, pos.x), 0, std::min(low.z, pos.z)};
            high = Vec3{std::max(high.x, pos.x), 0, std::max(high.z, pos.z)};
            // Colision exacta con la malla (muros, baches, tuneles, el hoyo).
            // Las aspas del molino solo se ven (y giran).
            forEachDescendant(t, [&](ecs::Entity e) {
                if (!e.has<ecs::MeshRenderer>()) return;
                std::string lower = e.name();
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                if (tile == "windmill" && e.parent().valid() && e.parent() != t && lower.find("windmill") == std::string::npos) {
                    // (nodo hijo del molino: las aspas)
                }
                const bool blades = tile == "windmill" && (lower.find("blade") != std::string::npos || lower.find("rotor") != std::string::npos ||
                                                           lower.find("wing") != std::string::npos || lower.find("sail") != std::string::npos ||
                                                           lower.find("top") != std::string::npos);
                if (blades) {
                    if (!decorative) {
                        scripting::Script& s = e.add<scripting::Script>();
                        s.file = "Scripts/Girar.lua";
                        s.properties = {{"eje", scripting::PropertyType::Vector, "0 0 1"}, {"velocidad", scripting::PropertyType::Number, "70"}};
                    }
                    return;
                }
                physics::MeshCollider& mc = e.add<physics::MeshCollider>();
                mc.material.friction = 0.5f;
                mc.material.bounciness = 0.3f;
            });
            if (i == 0) {
                const Vec3 dir{static_cast<float>(p[1].x - p[0].x), 0, static_cast<float>(p[1].z - p[0].z)};
                c.tee = pos - dir * 0.9f + Vec3{0.0f, kFloor + kBallRadius + 0.01f, 0.0f};
                c.tee_yaw = std::atan2(-dir.x, -dir.z) * 180.0f / 3.14159265f;
            }
            if (i + 1 == p.size()) c.hole = pos + Vec3{0.0f, kFloor, 0.0f};
        }
        c.center = (low + high) * 0.5f;

        // Cesped alrededor (fuera de limites) con colision.
        ecs::Entity ground = ecs::createPrimitive(world, assets::builtin::kCube, "Suelo");
        ground.setLocalPosition(Vec3{c.center.x, -0.1f, c.center.z});
        ground.setLocalScale(Vec3{140.0f, 0.2f, 140.0f});
        setMaterial(ground, "Cesped");
        physics::BoxCollider& box = ground.add<physics::BoxCollider>();
        box.material.friction = 0.9f;

        // Bandera en el hoyo (sube cuando llega la pelota).
        ecs::Entity flag = model(world, "flag-red");
        flag.setName("Bandera");
        flag.setLocalPosition(c.hole);
        if (!decorative) {
            scripting::Script& s = flag.add<scripting::Script>();
            s.file = "Scripts/Bandera.lua";
        }
        return c;
    }

    // --- Interfaz ---
    ecs::Entity ui(ecs::World& world, const std::string& name, ecs::Entity parent, Vec2 anchor, Vec2 pivot, Vec2 position, Vec2 size) {
        ecs::Entity e = world.create(name, parent);
        ui::RectTransform& rt = e.add<ui::RectTransform>();
        rt.anchor_min = anchor;
        rt.anchor_max = anchor;
        rt.pivot = pivot;
        rt.position = position;
        rt.size = size;
        return e;
    }

    ecs::Entity text(ecs::World& world, const std::string& name, ecs::Entity parent, Vec2 anchor, Vec2 pivot, Vec2 position,
                     Vec2 size, const std::string& value, float font, Vec3 color = Vec3{1, 1, 1},
                     ui::HAlign align = ui::HAlign::Center) {
        ecs::Entity e = ui(world, name, parent, anchor, pivot, position, size);
        ui::Text& t = e.add<ui::Text>();
        t.text = value;
        t.font_size = font;
        t.color = color;
        t.h_align = align;
        t.shadow = true;
        return e;
    }

    ecs::Entity button(ecs::World& world, const std::string& label, ecs::Entity parent, Vec2 position, Vec2 size,
                       const Uuid& target, const std::string& method, Vec3 color) {
        ecs::Entity e = ui(world, "Boton " + label, parent, Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f}, position, size);
        ui::Button& b = e.add<ui::Button>();
        b.normal = color;
        b.hover = color * 1.2f;
        b.pressed = color * 0.75f;
        b.corner_radius = 16.0f;
        b.target = target;
        b.on_click = method;
        ecs::Entity t = world.create("Texto", e);
        ui::RectTransform& rt = t.add<ui::RectTransform>();
        rt.anchor_min = Vec2{0.0f, 0.0f};
        rt.anchor_max = Vec2{1.0f, 1.0f};
        rt.size = Vec2{0.0f, 0.0f};
        ui::Text& tx = t.add<ui::Text>();
        tx.text = label;
        tx.font_size = size.y * 0.5f;
        tx.shadow = true;
        return e;
    }

    ecs::Entity music(ecs::World& world, ecs::Entity e, const std::string& clip, float volume) {
        audio::AudioSource& a = e.add<audio::AudioSource>();
        a.clip = clip;
        a.loop = true;
        a.spatial = false;
        a.volume = volume;
        a.play_on_awake = true;
        return e;
    }

    // --- Escenas ---
    void environment(ecs::World& world) {
        ecs::populateDefaultScene(world);
        if (ecs::Entity light = world.findByName("Luz direccional"); light.valid()) {
            light.setLocalEulerDegrees(Vec3{-55.0f, 35.0f, 0.0f});
        }
    }

    void levelScene(ecs::World& world, const Level& level, int number) {
        environment(world);
        const Course c = course(world, level, false);

        // Pelota: fisica en la raiz, el modelo del kit como hijo.
        ecs::Entity ball = world.create("Pelota");
        ball.setLocalPosition(c.tee);
        physics::SphereCollider& sphere = ball.add<physics::SphereCollider>();
        sphere.radius = kBallRadius;
        sphere.material.friction = 0.4f;
        sphere.material.bounciness = 0.55f;
        physics::Rigidbody& rb = ball.add<physics::Rigidbody>();
        rb.mass = 0.05f;
        rb.linear_damping = 0.55f;
        rb.angular_damping = 1.2f;
        rb.continuous = true;
        rb.allow_sleep = false;
        ecs::Entity visual = model(world, "ball-red", ball);
        visual.setName("Modelo");
        scripting::Script& bs = ball.add<scripting::Script>();
        bs.file = "Scripts/Pelota.lua";
        bs.properties = {{"yawInicial", scripting::PropertyType::Number, std::to_string(c.tee_yaw)}};

        // Mira: puntos blancos delante de la pelota.
        for (int i = 1; i <= 8; ++i) {
            ecs::Entity dot = ecs::createPrimitive(world, assets::builtin::kSphere, "Mira" + std::to_string(i));
            dot.setLocalScale(Vec3{0.06f, 0.06f, 0.06f});
            setMaterial(dot, "Mira");
            if (ecs::MeshRenderer* mr = dot.tryGet<ecs::MeshRenderer>()) mr->cast_shadows = ecs::ShadowCasting::Off;
        }

        // Hoyo: el centro del agujero (lo usa la pelota).
        ecs::Entity hole = world.create("Hoyo");
        hole.setLocalPosition(c.hole);

        // Camara que sigue a la pelota.
        if (ecs::Entity cam = world.findByName("Main Camera"); cam.valid()) {
            const float r = c.tee_yaw * 3.14159265f / 180.0f;
            cam.setLocalPosition(c.tee + Vec3{std::sin(r) * 3.2f, 1.7f, std::cos(r) * 3.2f});
            scripting::Script& s = cam.add<scripting::Script>();
            s.file = "Scripts/CamaraGolf.lua";
        }

        // Nivel: marcador, musica y paso al siguiente.
        ecs::Entity manager = world.create("Nivel");
        scripting::Script& ns = manager.add<scripting::Script>();
        ns.file = "Scripts/Nivel.lua";
        ns.properties = {{"numero", scripting::PropertyType::Number, std::to_string(number)},
                         {"nombre", scripting::PropertyType::Text, level.name},
                         {"par", scripting::PropertyType::Number, std::to_string(level.par)},
                         {"siguiente", scripting::PropertyType::Text, level.next}};
        music(world, manager, "Audio/nivel.wav", 0.35f);

        ecs::Entity canvas = world.create("HUD");
        canvas.add<ui::Canvas>();
        ecs::Entity panel = ui(world, "Panel", canvas, Vec2{0, 0}, Vec2{0, 0}, Vec2{30, 30}, Vec2{620, 122});
        ui::Image& img = panel.add<ui::Image>();
        img.color = Vec3{0.03f, 0.06f, 0.08f};
        img.alpha = 0.55f;
        img.corner_radius = 16.0f;
        text(world, "HUD_Titulo", panel, Vec2{0, 0}, Vec2{0, 0}, Vec2{26, 12}, Vec2{580, 52}, "HOYO", 36, Vec3{1, 1, 1}, ui::HAlign::Left);
        text(world, "HUD_Golpes", panel, Vec2{0, 0}, Vec2{0, 0}, Vec2{26, 64}, Vec2{580, 46}, "Par  Golpes 0", 30,
             Vec3{0.62f, 0.95f, 0.62f}, ui::HAlign::Left);
        text(world, "HUD_Total", canvas, Vec2{1, 0}, Vec2{1, 0}, Vec2{-40, 40}, Vec2{360, 64}, "Total 0", 42, Vec3{1, 1, 1},
             ui::HAlign::Right);
        ecs::Entity message = text(world, "HUD_Mensaje", canvas, Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f}, Vec2{0, -140},
                                   Vec2{1400, 240}, "", 88, Vec3{1.0f, 0.86f, 0.3f});
        message.setActive(false);
        ecs::Entity power = ui(world, "HUD_Potencia", canvas, Vec2{0.5f, 1}, Vec2{0.5f, 1}, Vec2{0, -80}, Vec2{720, 28});
        ui::Slider& slider = power.add<ui::Slider>();
        slider.interactable = false;
        slider.value = 0.0f;
        slider.fill = Vec3{1.0f, 0.55f, 0.15f};
        slider.handle = Vec3{1.0f, 0.9f, 0.6f};
        power.setActive(false);
        ecs::Entity power_label = text(world, "HUD_PotenciaTexto", canvas, Vec2{0.5f, 1}, Vec2{0.5f, 1}, Vec2{0, -112},
                                       Vec2{400, 40}, "POTENCIA", 26);
        power_label.setActive(false);
        ecs::Entity help = text(world, "HUD_Ayuda", canvas, Vec2{0, 1}, Vec2{0, 1}, Vec2{30, -20}, Vec2{1500, 40},
                                "A/D o clic derecho: apuntar   ·   Mantén ESPACIO o clic: potencia   ·   R: repetir   ·   Rueda: zoom   ·   Esc: menú",
                                22, Vec3{0.9f, 0.92f, 0.95f}, ui::HAlign::Left);
        help.get<ui::Text>().alpha = 0.8f;
    }

    void menuScene(ecs::World& world) {
        environment(world);
        const Course c = course(world, kLevels[4], true);
        if (ecs::Entity cam = world.findByName("Main Camera"); cam.valid()) {
            scripting::Script& s = cam.add<scripting::Script>();
            s.file = "Scripts/Orbita.lua";
            char centro[96];
            std::snprintf(centro, sizeof(centro), "%.2f 0.5 %.2f", c.center.x, c.center.z);
            s.properties = {{"centro", scripting::PropertyType::Vector, centro}, {"radio", scripting::PropertyType::Number, "11"},
                            {"altura", scripting::PropertyType::Number, "6"}};
        }
        ecs::Entity manager = world.create("Menu");
        manager.add<scripting::Script>().file = "Scripts/Menu.lua";
        music(world, manager, "Audio/menu.wav", 0.5f);

        ecs::Entity canvas = world.create("Interfaz");
        canvas.add<ui::Canvas>();
        text(world, "Titulo", canvas, Vec2{0.5f, 0}, Vec2{0.5f, 0}, Vec2{0, 110}, Vec2{1500, 190}, "MINI GOLF", 170,
             Vec3{1.0f, 0.93f, 0.55f});
        text(world, "Subtitulo", canvas, Vec2{0.5f, 0}, Vec2{0.5f, 0}, Vec2{0, 300}, Vec2{1200, 50}, "5 hoyos  ·  hecho con Cramion Engine", 36,
             Vec3{0.92f, 0.96f, 1.0f});
        button(world, "JUGAR", canvas, Vec2{0, 60}, Vec2{440, 96}, manager.uuid(), "OnJugar", Vec3{0.16f, 0.62f, 0.32f});
        button(world, "SALIR", canvas, Vec2{0, 180}, Vec2{440, 80}, manager.uuid(), "OnSalir", Vec3{0.72f, 0.24f, 0.22f});
        text(world, "Menu_Record", canvas, Vec2{0.5f, 1}, Vec2{0.5f, 1}, Vec2{0, -90}, Vec2{1000, 50}, "", 32, Vec3{1.0f, 0.86f, 0.3f});
        text(world, "Controles", canvas, Vec2{0.5f, 1}, Vec2{0.5f, 1}, Vec2{0, -40}, Vec2{1400, 36},
             "Apunta con A/D o el clic derecho, mantén ESPACIO para cargar el golpe y suelta", 24, Vec3{0.9f, 0.92f, 0.95f});
    }

    void resultsScene(ecs::World& world) {
        environment(world);
        const Course c = course(world, kLevels[0], true);
        if (ecs::Entity cam = world.findByName("Main Camera"); cam.valid()) {
            scripting::Script& s = cam.add<scripting::Script>();
            s.file = "Scripts/Orbita.lua";
            char centro[96];
            std::snprintf(centro, sizeof(centro), "%.2f 0.5 %.2f", c.center.x, c.center.z);
            s.properties = {{"centro", scripting::PropertyType::Vector, centro}, {"radio", scripting::PropertyType::Number, "8"}};
        }
        ecs::Entity manager = world.create("Resultados");
        manager.add<scripting::Script>().file = "Scripts/Resultados.lua";
        music(world, manager, "Audio/menu.wav", 0.4f);

        ecs::Entity canvas = world.create("Interfaz");
        canvas.add<ui::Canvas>();
        ecs::Entity panel = ui(world, "Panel", canvas, Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f}, Vec2{0, -20}, Vec2{1180, 760});
        ui::Image& img = panel.add<ui::Image>();
        img.color = Vec3{0.03f, 0.06f, 0.08f};
        img.alpha = 0.7f;
        img.corner_radius = 24.0f;
        text(world, "Titulo", panel, Vec2{0.5f, 0}, Vec2{0.5f, 0}, Vec2{0, 30}, Vec2{1000, 90}, "RESULTADOS", 80, Vec3{1.0f, 0.93f, 0.55f});
        text(world, "Res_Tarjeta", panel, Vec2{0.5f, 0}, Vec2{0.5f, 0}, Vec2{0, 150}, Vec2{980, 290}, "", 34, Vec3{1, 1, 1},
             ui::HAlign::Left);
        text(world, "Res_Total", panel, Vec2{0.5f, 0}, Vec2{0.5f, 0}, Vec2{0, 460}, Vec2{1000, 64}, "", 52, Vec3{0.62f, 0.95f, 0.62f});
        text(world, "Res_Record", panel, Vec2{0.5f, 0}, Vec2{0.5f, 0}, Vec2{0, 530}, Vec2{1000, 50}, "", 38, Vec3{1.0f, 0.86f, 0.3f});
        button(world, "JUGAR OTRA VEZ", canvas, Vec2{-230, 300}, Vec2{420, 84}, manager.uuid(), "OnOtraVez", Vec3{0.16f, 0.62f, 0.32f});
        button(world, "MENÚ", canvas, Vec2{230, 300}, Vec2{420, 84}, manager.uuid(), "OnMenu", Vec3{0.2f, 0.4f, 0.75f});
    }
};

void copyFolder(const fs::path& from, const fs::path& to) {
    fs::create_directories(to);
    for (const auto& entry : fs::directory_iterator(from)) {
        if (entry.is_regular_file()) fs::copy_file(entry.path(), to / entry.path().filename(), fs::copy_options::overwrite_existing);
    }
}

std::optional<assets::AssetInfo> findByPath(const assets::AssetDatabase& db, const fs::path& path) {
    std::error_code e;
    const fs::path wanted = fs::weakly_canonical(path, e);
    for (const assets::AssetInfo& info : db.all()) {
        if (!info.path.empty() && fs::weakly_canonical(info.path, e) == wanted) return info;
    }
    return std::nullopt;
}

// --- Prueba sin ventana: un piloto automatico juega cada hoyo ---
int playTest(const project::ProjectInfo& project, assets::AssetDatabase& database, assets::AssetManager& manager) {
    int failures = 0;
    for (std::size_t n = 0; n < kLevels.size(); ++n) {
        const Level& level = kLevels[n];
        ecs::World world;
        std::string error;
        if (!ecs::loadScene(world, project.assetsFolder() / "Escenas" / (level.scene + ".crscene"), &error)) {
            std::cout << "  FALLO " << level.scene << ": " << error << "\n";
            ++failures;
            continue;
        }
        physics::PhysicsSystem physics;
        physics.setAssetManager(&manager);
        scripting::ScriptSystem scripts;
        scripts.setAssetsRoot(project.assetsFolder());
        scripts.setPhysics(&physics);
        scripts.setPrefsFile(fs::temp_directory_path() / "cramion_minigolf_test.prefs");
        std::vector<std::string> lua_errors;
        scripts.setLog([&](int lvl, const std::string& m) {
            if (lvl >= 2) lua_errors.push_back(m);
        });
        physics.start(world);
        scripts.start(world);
        ecs::Entity ball = world.findByName("Pelota");
        ecs::Entity hole = world.findByName("Hoyo");

        // Puntos del camino (centros de las casillas) hasta el hoyo.
        std::vector<Vec3> waypoints;
        for (const Cell& c : level.path) waypoints.push_back(Vec3{c.x * kTile, 0, c.z * kTile});
        waypoints.back() = hole.worldPosition();

        const float dt = 1.0f / 60.0f;
        int shots = 0, resets = 0;
        float still = 0.0f, since_shot = 10.0f, boost = 1.0f;
        std::size_t previous_target = 0;
        std::string next_scene;
        Vec3 last_shot = ball.worldPosition();
        for (int frame = 0; frame < 60 * 240 && next_scene.empty(); ++frame) {
            const int steps = physics.update(world, dt, true);
            scripts.fixedUpdate(world, 1.0f / 60.0f, steps);
            scripts.update(world, dt);
            if (const fs::path req = scripts.takeSceneRequest(); !req.empty()) next_scene = req.stem().string();
            if (!ball.valid()) break;
            const Vec3 p = ball.worldPosition();
            const float speed = core::length(physics.linearVelocity(ball));
            since_shot += dt;
            if (p.y < 0.1f) ++resets;
            still = (speed < 0.08f) ? still + dt : 0.0f;
            // Parada (y no en el hoyo): siguiente tiro hacia el punto del
            // camino mas lejano que quede delante.
            if (still > 0.5f && since_shot > 0.8f && shots < 30) {
                const Vec3 flat{p.x, 0, p.z};
                std::size_t nearest = 0;
                float best = 1e9f;
                for (std::size_t i = 0; i < waypoints.size(); ++i) {
                    const Vec3 w{waypoints[i].x, 0, waypoints[i].z};
                    const float d = core::length(w - flat);
                    if (d < best) { best = d; nearest = i; }
                }
                std::size_t target = std::min(nearest + 1, waypoints.size() - 1);
                const Vec3 goal{waypoints[target].x, 0, waypoints[target].z};
                Vec3 to = goal - flat;
                const float d = core::length(to);
                if (d < 1e-3f) continue;
                to = to * (1.0f / d);
                const bool last = target + 1 == waypoints.size();
                // Sin avanzar (misma casilla objetivo): mas fuerte, como un jugador.
                boost = target == previous_target ? std::min(boost * 1.45f, 4.0f) : 1.0f;
                previous_target = target;
                const float v = std::clamp((d * (last ? 0.62f : 0.75f) + 0.5f) * boost, 0.8f, 10.0f);
                physics.setLinearVelocity(ball, to * v);
                if (std::getenv("MINIGOLF_TRAZA") != nullptr) {
                    std::printf("        tiro %d desde (%.2f %.2f %.2f) hacia casilla %zu, %.1f m/s\n", shots + 1, p.x, p.y, p.z,
                                target, v);
                }
                last_shot = p;
                ++shots;
                since_shot = 0.0f;
                still = 0.0f;
            }
        }
        const bool ok = next_scene == level.next && lua_errors.empty();
        std::printf("  %s %-8s %-13s par %d: %2d tiros del piloto, escena siguiente \"%s\"%s\n", ok ? "OK   " : "FALLO",
                    level.scene.c_str(), level.name.c_str(), level.par, shots, next_scene.c_str(),
                    resets > 0 ? " (se salio alguna vez)" : "");
        for (const std::string& m : lua_errors) std::printf("        Lua: %s\n", m.c_str());
        if (!ok) ++failures;
        scripts.stop();
        physics.stop();
    }
    // Menu y resultados: los scripts arrancan sin errores.
    for (const char* name : {"Menu", "Resultados"}) {
        ecs::World world;
        std::string error;
        ecs::loadScene(world, project.assetsFolder() / "Escenas" / (std::string(name) + ".crscene"), &error);
        scripting::ScriptSystem scripts;
        scripts.setAssetsRoot(project.assetsFolder());
        scripts.setPrefsFile(fs::temp_directory_path() / "cramion_minigolf_test.prefs");
        std::vector<std::string> lua_errors;
        scripts.setLog([&](int lvl, const std::string& m) {
            if (lvl >= 2) lua_errors.push_back(m);
        });
        scripts.start(world);
        for (int i = 0; i < 30; ++i) scripts.update(world, 1.0f / 60.0f);
        std::string extra;
        if (std::string(name) == "Resultados") {
            const ecs::Entity total = world.findByName("Res_Total");
            extra = total.valid() ? total.get<ui::Text>().text : "";
        } else {
            const ecs::Entity record = world.findByName("Menu_Record");
            extra = record.valid() ? record.get<ui::Text>().text : "";
        }
        std::printf("  %s %-10s \"%s\"\n", lua_errors.empty() ? "OK   " : "FALLO", name, extra.c_str());
        for (const std::string& m : lua_errors) std::printf("        Lua: %s\n", m.c_str());
        if (!lua_errors.empty()) ++failures;
        scripts.stop();
    }
    (void)database;
    return failures;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "uso: cramion_minigolf <kit/Models/FBX format> <tools/minigolf> <carpeta destino> [--probar]\n";
        return 1;
    }
    const fs::path kit = argv[1];
    const fs::path demo = argv[2];
    const fs::path parent = argv[3];
    const bool test = argc >= 5 && std::string(argv[4]) == "--probar";
    try {
        physics::registerPhysicsComponents();
        scripting::registerScriptComponents();
        ui::registerUiComponents();
        audio::registerAudioComponents();

        const fs::path folder = parent / "MiniGolf";
        if (fs::exists(folder)) {
            std::cout << "Se reemplaza " << folder.string() << "\n";
            fs::remove_all(folder);
        }
        project::ProjectInfo project = project::createProject(parent, "MiniGolf");
        const fs::path assets = project.assetsFolder();

        // Modelos del kit (cm -> m, x3).
        std::map<std::string, fs::path> imported;
        assets::ModelImportSettings settings;
        settings.scale = kImportScale;
        for (const std::string& name : kModels) {
            const assets::ImportResult r = assets::importModel(kit / (name + ".fbx"), assets / "Modelos", settings);
            if (!r.ok) throw std::runtime_error("importar " + name + ": " + r.message);
            imported[name] = r.info.path;
        }
        copyFolder(demo / "Scripts", assets / "Scripts");
        copyFolder(demo / "Audio", assets / "Audio");

        // Materiales.
        fs::create_directories(assets / "Materiales");
        assets::MaterialAsset grass;
        grass.base_color = core::Vec4{0.16f, 0.42f, 0.2f, 1.0f};
        grass.roughness = 0.95f;
        assets::saveMaterial(grass, assets / "Materiales" / "Cesped.crmat");
        assets::MaterialAsset dot;
        dot.base_color = core::Vec4{1.0f, 1.0f, 1.0f, 1.0f};
        dot.emissive = Vec3{1.0f, 1.0f, 1.0f};
        dot.emissive_intensity = 1.5f;
        assets::saveMaterial(dot, assets / "Materiales" / "Mira.crmat");

        assets::AssetDatabase database;
        database.open(assets);
        assets::AssetManager manager(database);
        manager.setCacheFolder(project.libraryFolder() / "Cache");
        Builder b{database, manager, {}, {}};
        for (const auto& [name, path] : imported) {
            const auto info = findByPath(database, path);
            if (!info) throw std::runtime_error("no esta en la base de datos: " + path.string());
            b.models[name] = info->uuid;
        }
        for (const char* m : {"Cesped", "Mira"}) {
            const auto info = findByPath(database, assets / "Materiales" / (std::string(m) + ".crmat"));
            if (!info) throw std::runtime_error(std::string("material ") + m);
            b.materials[m] = info->uuid;
        }
        // Nombres de los nodos del molino (las aspas giran).
        if (const auto mill = manager.loadModel(b.models["windmill"])) {
            std::cout << "  molino:";
            for (const auto& node : mill->nodes) {
                std::cout << " [" << node.name;
                if (node.part >= 0) {
                    Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
                    for (const auto& v : mill->parts[static_cast<std::size_t>(node.part)]->vertices) {
                        lo = Vec3{std::min(lo.x, v.position.x), std::min(lo.y, v.position.y), std::min(lo.z, v.position.z)};
                        hi = Vec3{std::max(hi.x, v.position.x), std::max(hi.y, v.position.y), std::max(hi.z, v.position.z)};
                    }
                    std::printf(" tam %.2f %.2f %.2f pos %.2f %.2f %.2f", hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, node.local.m[3][0],
                                node.local.m[3][1], node.local.m[3][2]);
                }
                std::cout << "]";
            }
            std::cout << "\n";
        }

        // Escenas.
        fs::create_directories(assets / "Escenas");
        const auto save = [&](const std::string& name, const std::function<void(ecs::World&)>& build) {
            ecs::World world;
            build(world);
            std::string error;
            if (!ecs::saveScene(world, assets / "Escenas" / (name + ".crscene"), &error)) throw std::runtime_error(name + ": " + error);
            std::cout << "  escena " << name << "\n";
        };
        save("Menu", [&](ecs::World& w) { b.menuScene(w); });
        for (std::size_t i = 0; i < kLevels.size(); ++i) {
            save(kLevels[i].scene, [&](ecs::World& w) { b.levelScene(w, kLevels[i], static_cast<int>(i + 1)); });
        }
        save("Resultados", [&](ecs::World& w) { b.resultsScene(w); });

        // Escena inicial: el menu.
        database.refresh();
        if (const auto menu = findByPath(database, assets / "Escenas" / "Menu.crscene")) {
            project.startup_scene = menu->uuid;
            project::saveProject(project);
        }
        std::cout << "Proyecto listo: " << project.file.string() << "\n";

        if (test) {
            std::cout << "Prueba (piloto automatico, sin ventana):\n";
            const int failures = playTest(project, database, manager);
            std::cout << "\n" << failures << " fallos\n";
            return failures == 0 ? 0 : 2;
        }
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
