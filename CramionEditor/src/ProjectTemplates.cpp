// Plantillas de proyecto (ver ProjectTemplates.h). Las integradas construyen
// su escena con el ECS: primitivas con colliders y materiales propios,
// scripts de Lua de verdad (jugador, camara, monedas, guardias) y una
// interfaz sencilla, para que el proyecto se pueda jugar con Play nada mas
// crearlo.

#include "ProjectTemplates.h"

#include <CramionCore/CramionCore.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace cramion::editor {

using core::Vec2;
using core::Vec3;

namespace {

constexpr std::uint32_t rgba(int r, int g, int b) {
    return static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8) |
           (static_cast<std::uint32_t>(b) << 16) | 0xFF000000u;
}

// --- Scripts de las plantillas ---------------------------------------------------

constexpr const char* kPlayerScript = R"(-- Jugador en tercera persona: WASD relativo a la camara, Shift para correr
-- y Espacio para saltar. Va sobre un Rigidbody (la fisica lo empuja y lo
-- hace caer); su hijo "Modelo" mira hacia donde camina.
local Jugador = {
    properties = {
        velocidad = 6.0,
        correr = 1.7,       -- multiplicador con Shift
        salto = 6.5,        -- m/s hacia arriba
        aceleracion = 12.0,
    }
}

function Jugador:Start()
    self.inicio = self.entity.position
    self.camara = Scene.find("Main Camera")
    self.modelo = self.entity:find("Modelo")
end

function Jugador:enSuelo()
    -- Desde justo debajo de la capsula (un rayo desde dentro la tocaria a ella).
    return Physics.raycast(self.entity.position + Vec3(0, -1.02, 0), Vec3.down, 0.25) ~= nil
end

function Jugador:Respawn()
    self.entity.position = self.inicio
    self.entity.velocity = Vec3.zero
end

function Jugador:Update(dt)
    local adelante = Vec3(0, 0, -1)
    local derecha = Vec3(1, 0, 0)
    if self.camara then
        adelante = self.camara.forward
        adelante.y = 0
        adelante = adelante:normalized()
        derecha = self.camara.right
        derecha.y = 0
        derecha = derecha:normalized()
    end
    local direccion = derecha * Input.getAxis("Horizontal") + adelante * Input.getAxis("Vertical")
    if direccion:length() > 1 then direccion = direccion:normalized() end

    local rapidez = self.velocidad
    if Input.getKey("shift") then rapidez = rapidez * self.correr end
    local objetivo = direccion * rapidez
    local v = self.entity.velocity
    local t = Mathf.clamp01(self.aceleracion * dt)
    v.x = Mathf.lerp(v.x, objetivo.x, t)
    v.z = Mathf.lerp(v.z, objetivo.z, t)
    if Input.getKeyDown("space") and self:enSuelo() then
        v.y = self.salto
    end
    self.entity.velocity = v

    if self.modelo and direccion:length() > 0.1 then
        self.modelo:lookAt(self.modelo.position + direccion)
    end
    if self.entity.position.y < -20 then self:Respawn() end
end

return Jugador
)";

constexpr const char* kCameraScript = R"(-- Camara en tercera persona: sigue al objetivo desde detras. Clic derecho y
-- raton para girarla, rueda para acercarla.
local CamaraTercera = {
    properties = {
        objetivo = "Jugador",
        distancia = 7.0,
        altura = 1.2,        -- punto al que mira, sobre el objetivo
        inclinacion = 18.0,  -- grados
        sensibilidad = 0.25,
        suavizado = 12.0,
    }
}

function CamaraTercera:Start()
    self.target = Scene.find(self.objetivo)
    self.yaw = 0
    self.pitch = self.inclinacion
end

function CamaraTercera:LateUpdate(dt)
    if not self.target then return end
    if Input.getMouseButton(1) then
        local d = Input.mouseDelta()
        self.yaw = self.yaw - d.x * self.sensibilidad
        self.pitch = Mathf.clamp(self.pitch + d.y * self.sensibilidad, -5, 75)
    end
    self.distancia = Mathf.clamp(self.distancia - Input.getAxis("Mouse ScrollWheel"), 3, 25)

    local yaw = math.rad(self.yaw)
    local pitch = math.rad(self.pitch)
    local foco = self.target.position + Vec3(0, self.altura, 0)
    local detras = Vec3(math.sin(yaw) * math.cos(pitch), math.sin(pitch), math.cos(yaw) * math.cos(pitch))
    local deseada = foco + detras * self.distancia
    self.entity.position = self.entity.position:lerp(deseada, Mathf.clamp01(self.suavizado * dt))
    self.entity:lookAt(foco)
end

return CamaraTercera
)";

constexpr const char* kCoinScript = R"(-- Moneda: flota, gira y se recoge al tocarla (su collider es un trigger).
local Moneda = { properties = { valor = 1 } }

function Moneda:Start()
    self.base = self.entity.position
    self.fase = self.base.x + self.base.z
end

function Moneda:Update(dt)
    self.entity:rotate(Vec3(0, 120 * dt, 0))
    self.entity.position = self.base + Vec3(0, math.sin(Time.time * 2.5 + self.fase) * 0.15, 0)
end

function Moneda:OnTriggerEnter(other)
    if other.name ~= "Jugador" then return end
    local marcador = Scene.find("Marcador")
    if marcador then marcador:getScript():Sumar(self.valor) end
    self.entity:destroy()
end

return Moneda
)";

constexpr const char* kScoreScript = R"(-- Marcador del HUD: cuenta las monedas recogidas y guarda el record.
local Marcador = { properties = {} }

function Marcador:Start()
    self.puntos = 0
    self.total = #Scene.findAllWithTag("Moneda")
    self:pintar()
end

function Marcador:pintar()
    self.entity.text = string.format("Monedas  %d / %d", self.puntos, self.total)
end

function Marcador:Sumar(n)
    self.puntos = self.puntos + n
    self:pintar()
    if self.puntos >= self.total then
        self.entity.text = "¡Todas las monedas!"
        self.entity.color = Vec3(1.0, 0.85, 0.25)
        Prefs.setInt("record", math.max(Prefs.getInt("record", 0), self.puntos))
    end
end

return Marcador
)";

constexpr const char* kGuardScript = R"(-- Guardia con NavAgent: patrulla por puntos al azar de la malla de
-- navegacion y persigue al jugador si lo ve (sin paredes en medio). Si lo
-- alcanza, el jugador vuelve al inicio.
local Guardia = {
    properties = {
        radioPatrulla = 10.0,
        vision = 11.0,
        alcance = 1.3,
    }
}

function Guardia:Start()
    self.casa = self.entity.position
    self.jugador = Scene.find("Jugador")
    self.hud = Scene.find("Marcador")
    self.estado = "patrulla"
    self.espera = 0
end

function Guardia:veAlJugador()
    if not self.jugador then return false end
    if self.entity:distanceTo(self.jugador) > self.vision then return false end
    local libre = Navigation.raycast(self.entity.position, self.jugador.position)
    return libre
end

function Guardia:Update(dt)
    if self.jugador and self.entity:distanceTo(self.jugador) < self.alcance then
        self.jugador:getScript():Respawn()
        if self.hud then self.hud:getScript():Atrapado() end
        self.entity:stopMoving()
        self.estado = "patrulla"
        return
    end

    if self:veAlJugador() then
        self.estado = "persigue"
        self.entity:moveTo(self.jugador.position)
        return
    end

    if self.estado == "persigue" then
        -- Lo perdio de vista: va a donde lo vio por ultima vez.
        if not self.entity.isMoving then self.estado = "patrulla" end
        return
    end

    if not self.entity.isMoving then
        self.espera = self.espera - dt
        if self.espera <= 0 then
            local punto = Navigation.randomPoint(self.casa, self.radioPatrulla)
            if punto then self.entity:moveTo(punto) end
            self.espera = Mathf.random(0.5, 2.5)
        end
    end
end

return Guardia
)";

constexpr const char* kGoalScript = R"(-- La meta: al llegar se gana y el nivel empieza de nuevo a los 3 segundos.
local Meta = { properties = {} }

function Meta:Update(dt)
    self.entity:rotate(Vec3(0, 45 * dt, 0))
    if self.reinicio then
        self.reinicio = self.reinicio - dt
        if self.reinicio <= 0 then Scene.load(Scene.name()) end
    end
end

function Meta:OnTriggerEnter(other)
    if other.name ~= "Jugador" or self.reinicio then return end
    local hud = Scene.find("Marcador")
    if hud then hud:getScript():Ganar() end
    self.reinicio = 3
end

return Meta
)";

constexpr const char* kEscapeHudScript = R"(-- HUD del modo escapa: veces atrapado y el mensaje final.
local Marcador = { properties = {} }

function Marcador:Start()
    self.atrapado = 0
    self:pintar()
end

function Marcador:pintar()
    self.entity.text = string.format("Llega a la meta sin que te atrapen  ·  Atrapado: %d", self.atrapado)
end

function Marcador:Atrapado()
    self.atrapado = self.atrapado + 1
    self:pintar()
end

function Marcador:Ganar()
    self.entity.text = "¡Lo lograste!"
    self.entity.color = Vec3(0.4, 1.0, 0.5)
end

return Marcador
)";

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

    static void attach(ecs::Entity e, const std::string& file,
                       std::vector<scripting::ScriptProperty> properties = {}) {
        scripting::Script& s = e.add<scripting::Script>();
        s.file = "Scripts/" + file;
        s.properties = std::move(properties);
    }

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
        attach(p, "Jugador.lua");
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
        attach(cam, "CamaraTercera.lua",
               {{"distancia", scripting::PropertyType::Number, std::to_string(distance)},
                {"inclinacion", scripting::PropertyType::Number, std::to_string(pitch)}});
    }

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
        attach(text, script_file);
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
    b.script("Jugador.lua", kPlayerScript);
    b.script("CamaraTercera.lua", kCameraScript);
    b.script("Moneda.lua", kCoinScript);
    b.script("Marcador.lua", kScoreScript);

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
        Builder::attach(c, "Moneda.lua");
    }

    b.player(Vec3{0.0f, 1.1f, 8.0f}, body, visor);
    b.camera(7.0f, 18.0f);
    b.hud("Marcador.lua");
    b.save("Main");

    std::vector<std::string> tags = ecs::defaultTags();
    tags.push_back("Moneda");
    ecs::saveTags(project.settingsFolder() / "Tags.json", tags);
}

void buildNavigation(project::ProjectInfo& project) {
    Builder b(project);
    b.world.setSceneUuid(Uuid::generate());
    ecs::populateDefaultScene(b.world);
    b.script("Jugador.lua", kPlayerScript);
    b.script("CamaraTercera.lua", kCameraScript);
    b.script("Guardia.lua", kGuardScript);
    b.script("Meta.lua", kGoalScript);
    b.script("Marcador.lua", kEscapeHudScript);

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
        Builder::attach(g, "Guardia.lua");
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
    Builder::attach(meta, "Meta.lua");

    b.player(Vec3{-20.0f, 1.1f, 20.0f}, body, visor);
    b.camera(13.0f, 50.0f);
    b.hud("Marcador.lua");
    b.save("Main");

    navigation::NavigationSettings nav;
    navigation::saveNavigationSettings(project.settingsFolder() / "Navigation.json", nav);
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
        "navigation", "IA y navegación", "Integradas",
        "Escapa de los guardias: patrullan un laberinto con NavMesh y te persiguen si te ven. Llega a la meta.",
        {"NavMesh generado en tiempo real", "Guardias con NavAgent (patrullar / perseguir)",
         "Visión con Navigation.raycast", "Meta, HUD y reinicio del nivel"},
        rgba(60, 200, 110), TemplateArt::Navigation, {}});

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
        } else if (t.id == "navigation") {
            buildNavigation(info);
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
