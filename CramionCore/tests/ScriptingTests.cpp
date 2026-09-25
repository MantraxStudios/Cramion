// Pruebas del scripting en Lua y del audio (consola). Devuelve 0 si todo va.

#include "CramionCore/audio/Audio.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/scripting/Scripting.h"

#include <CramionDM/Input.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

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

void writeFile(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

const std::filesystem::path kRoot = std::filesystem::temp_directory_path() / "cramion_script_assets";

void testScripts() {
    std::printf("Scripts (Lua)\n");
    writeFile(kRoot / "Scripts" / "Mover.lua", R"(
local Mover = { properties = { velocidad = 2.0, activo = true, nombre = "x", destino = Vec3(1, 2, 3) } }
function Mover:Awake() self.contador = 0 end
function Mover:Start() self.empezo = true end
function Mover:Update(dt)
    self.contador = self.contador + 1
    self.entity:translate(Vec3(1, 0, 0) * self.velocidad * dt)
    if Input.getKey("W") then self.entity.name = "PulsoW" end
    if Input.getAxis("Horizontal") > 0 then Debug.log("derecha") end
end
return Mover
)");
    writeFile(kRoot / "Scripts" / "Roto.lua", "local R = {}\nfunction R:Update(dt)\n  local x = nil + 1\nend\nreturn R\n");
    writeFile(kRoot / "Scripts" / "Choque.lua", R"(
local C = {}
function C:OnCollisionEnter(other, contact) Scene.create("choco_con_" .. other.name) end
return C
)");
    writeFile(kRoot / "Scripts" / "Borrar.lua", R"(
local B = {}
function B:Update(dt)
    self.n = (self.n or 0) + 1
    if self.n == 3 then
        local copia = Scene.instantiate(self.entity, Vec3(0, 5, 0))
        copia.name = "Copia"
        self.entity:destroy()
    end
end
return B
)");

    scripting::registerScriptComponents();
    ecs::World world;
    ecs::Entity mover = world.create("Mover");
    scripting::Script& s = mover.add<scripting::Script>();
    s.file = "Scripts/Mover.lua";
    s.properties.push_back(scripting::ScriptProperty{"velocidad", scripting::PropertyType::Number, "10"});

    ecs::Entity broken = world.create("Roto");
    broken.add<scripting::Script>().file = "Scripts/Roto.lua";

    scripting::ScriptSystem system;
    system.setAssetsRoot(kRoot);
    std::vector<std::string> log;
    system.setLog([&](int, const std::string& message) { log.push_back(message); });

    const std::vector<scripting::ScriptProperty> described = system.describe("Scripts/Mover.lua");
    check(described.size() == 4 && described[0].name == "activo" && described[1].name == "destino" &&
              described[1].type == scripting::PropertyType::Vector && described[3].value == "2",
          "describe lee las propiedades del script (con su tipo)");

    dm::Input input;
    system.setInput(&input);
    system.start(world);
    for (int i = 0; i < 10; ++i) system.update(world, 0.1f);
    check(std::abs(mover.worldPosition().x - 10.0f) < 1e-3f,
          "Update mueve el objeto con la propiedad del Inspector (10 m/s x 1 s)");

    dm::Event press;
    press.type = dm::EventType::KeyPressed;
    press.category = dm::EventCategory::Keyboard;
    press.key = dm::Key::W;
    input.onEvent(press);
    press.key = dm::Key::D;
    input.onEvent(press);
    system.update(world, 0.1f);
    check(mover.name() == "PulsoW", "Input.getKey(\"W\") y cambiar el nombre desde Lua");
    check(!log.empty() && log.back() == "derecha", "Input.getAxis y Debug.log");

    check(!system.errors().empty() && system.errors().front().file == "Scripts/Roto.lua" &&
              system.errors().front().line == 3,
          "un error en tiempo de ejecucion da archivo y linea");
    const std::size_t error_count = system.errors().size();
    system.update(world, 0.1f);
    check(system.errors().size() == error_count, "el script que fallo no repite el error cada frame");

    // Recarga en caliente: mismas instancias (el contador sigue), funciones nuevas.
    writeFile(kRoot / "Scripts" / "Mover.lua", R"(
local Mover = { properties = { velocidad = 2.0 } }
function Mover:Update(dt) self.contador = self.contador + 100 end
return Mover
)");
    system.reloadFile("Scripts/Mover.lua");
    const float x_before = mover.worldPosition().x;
    system.update(world, 0.1f);
    std::string out;
    check(system.run("return Scene.find('PulsoW'):getScript().contador", &out) && std::stoi(out) > 100,
          "recarga en caliente conserva el estado");
    check(std::abs(mover.worldPosition().x - x_before) < 1e-5f, "y usa las funciones nuevas");

    // Crear, duplicar y destruir desde Lua.
    ecs::Entity destroyer = world.create("Borrador");
    destroyer.add<scripting::Script>().file = "Scripts/Borrar.lua";
    for (int i = 0; i < 4; ++i) system.update(world, 0.1f);
    check(!world.findByName("Borrador").valid() && world.findByName("Copia").valid() &&
              std::abs(world.findByName("Copia").worldPosition().y - 5.0f) < 1e-4f,
          "Scene.instantiate y entity:destroy");
    system.stop();

    // Choques: OnCollisionEnter con el otro objeto.
    ecs::World physics_world;
    ecs::Entity ground = physics_world.create("Suelo");
    ground.add<physics::BoxCollider>().size = Vec3{20.0f, 1.0f, 20.0f};
    ecs::Entity box = physics_world.create("Caja");
    box.setWorldPosition(Vec3{0.0f, 3.0f, 0.0f});
    box.add<physics::BoxCollider>();
    box.add<physics::Rigidbody>();
    box.add<scripting::Script>().file = "Scripts/Choque.lua";
    physics::PhysicsSystem physics;
    physics.start(physics_world);
    scripting::ScriptSystem scripts;
    scripts.setAssetsRoot(kRoot);
    scripts.setPhysics(&physics);
    scripts.start(physics_world);
    for (int i = 0; i < 120; ++i) {
        physics.update(physics_world, 1.0f / 60.0f);
        scripts.update(physics_world, 1.0f / 60.0f);
    }
    check(physics_world.findByName("choco_con_Suelo").valid(), "OnCollisionEnter(other) desde la fisica");
    scripts.stop();

    // Componente en la escena.
    ecs::World copy;
    ecs::deserializeWorld(copy, ecs::serializeWorld(world));
    const ecs::Entity c = copy.findByName("PulsoW");
    check(c.valid() && c.has<scripting::Script>() && c.get<scripting::Script>().properties.size() == 1 &&
              c.get<scripting::Script>().properties[0].value == "10",
          "el Script (y sus valores) en la escena");
    check(scripting::scriptTemplate("Mi Jugador").find("local MiJugador = {") != std::string::npos,
          "plantilla de script nuevo");
}

// WAV de 0.5 s (seno 440 Hz, 16 bits mono).
void writeWav(const std::filesystem::path& file) {
    const std::uint32_t rate = 22050;
    const std::uint32_t samples = rate / 2;
    std::vector<std::int16_t> data(samples);
    for (std::uint32_t i = 0; i < samples; ++i) {
        data[i] = static_cast<std::int16_t>(std::sin(2.0 * 3.14159265 * 440.0 * i / rate) * 12000.0);
    }
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4);
    u32(36 + samples * 2);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(1);
    u32(rate);
    u32(rate * 2);
    u16(2);
    u16(16);
    out.write("data", 4);
    u32(samples * 2);
    out.write(reinterpret_cast<const char*>(data.data()), samples * 2);
}

void testAudio() {
    std::printf("Audio\n");
    writeWav(kRoot / "Audio" / "tono.wav");
    check(audio::isAudioFile("x/musica.OGG") && audio::isAudioFile("a.mp3") && !audio::isAudioFile("a.png"),
          "extensiones de audio");
    audio::registerAudioComponents();
    ecs::World world;
    ecs::Entity e = world.create("Altavoz");
    e.setWorldPosition(Vec3{3.0f, 0.0f, 0.0f});
    audio::AudioSource& source = e.add<audio::AudioSource>();
    source.clip = "Audio/tono.wav";
    source.loop = true;
    ecs::World copy;
    ecs::deserializeWorld(copy, ecs::serializeWorld(world));
    check(copy.findByName("Altavoz").valid() && copy.findByName("Altavoz").get<audio::AudioSource>().loop,
          "AudioSource en la escena");

    audio::AudioSystem system;
    system.setAssetsRoot(kRoot);
    if (!system.available()) {
        std::printf("  (sin dispositivo de audio: se saltan las pruebas de reproduccion)\n");
        return;
    }
    system.start(world);
    system.update(world, 0.016f, Vec3{}, Vec3{0, 0, -1});
    check(system.isPlaying(e), "suena al empezar (play_on_awake), en 3D");
    system.stop(e);
    check(!system.isPlaying(e), "stop");
    system.play(e);
    check(system.isPlaying(e), "play");
    system.stop();
    check(!system.isPlaying(e), "fin del Play: silencio");
}

}  // namespace

int main() {
    testScripts();
    testAudio();
    std::filesystem::remove_all(kRoot);
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
