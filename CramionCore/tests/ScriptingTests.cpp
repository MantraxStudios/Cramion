// Pruebas del scripting en Lua y del audio (consola). Devuelve 0 si todo va.

#include "CramionCore/audio/Audio.h"
#include "CramionCore/ecs/Components.h"
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
void writeWav(const std::filesystem::path& file, double frequency = 440.0) {
    const std::uint32_t rate = 22050;
    const std::uint32_t samples = rate / 2;
    std::vector<std::int16_t> data(samples);
    for (std::uint32_t i = 0; i < samples; ++i) {
        data[i] = static_cast<std::int16_t>(std::sin(2.0 * 3.14159265 * frequency * i / rate) * 12000.0);
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

// Oclusion: una puerta (BoxCollider) entre el oyente y un sonido lo tapa;
// abrirla (moverla) o apagar la oclusion en el AudioListener lo destapa.
void testAudioOcclusion() {
    std::printf("Audio: oclusion, efectos y reverberacion\n");
    ecs::World world;
    ecs::Entity ears = world.create("Camara");
    audio::AudioListener& listener = ears.add<audio::AudioListener>();
    ears.add<physics::CapsuleCollider>();  // el cuerpo del jugador no cuenta como pared
    ecs::Entity radio = world.create("Radio");
    radio.setWorldPosition(Vec3{6.0f, 0.0f, 0.0f});
    radio.add<physics::BoxCollider>().size = Vec3{0.5f, 0.5f, 0.5f};  // su carcasa tampoco
    audio::AudioSource& source = radio.add<audio::AudioSource>();
    source.clip = "Audio/tono.wav";
    source.loop = true;
    source.echo = true;
    source.low_pass = true;
    ecs::Entity door = world.create("Puerta");
    door.setWorldPosition(Vec3{3.0f, 0.0f, 0.0f});
    door.add<physics::BoxCollider>().size = Vec3{0.2f, 3.0f, 3.0f};
    ecs::Entity wall = world.create("Pared");
    wall.setWorldPosition(Vec3{4.5f, 0.0f, 0.0f});
    wall.add<physics::BoxCollider>().size = Vec3{0.2f, 3.0f, 3.0f};
    wall.setActive(false);

    physics::PhysicsSystem physics;
    physics.start(world);
    physics.update(world, 0.0f, false);
    const audio::AudioSystem::OcclusionQuery query = audio::physicsOcclusionQuery(physics);
    check(query(Vec3{}, radio.worldPosition(), radio, ears) == 1.0f, "la puerta cerrada es una pared");
    wall.setActive(true);
    physics.update(world, 0.0f, false);
    check(query(Vec3{}, radio.worldPosition(), radio, ears) == 2.0f, "puerta y pared: dos");
    wall.setActive(false);
    physics.update(world, 0.0f, false);

    audio::AudioSystem system;
    system.setAssetsRoot(kRoot);
    if (!system.available()) {
        std::printf("  (sin dispositivo de audio: se saltan las pruebas de reproduccion)\n");
        return;
    }
    system.setOcclusionQuery(query);
    system.start(world);
    const auto run = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            physics.update(world, 1.0f / 60.0f, false);
            system.update(world, 1.0f / 60.0f, Vec3{}, Vec3{0, 0, -1});
        }
    };
    run(5);
    check(system.isPlaying(radio) && system.occlusionOf(radio) > 0.95f, "con la puerta cerrada suena tapado");
    door.setWorldPosition(Vec3{3.0f, 0.0f, 10.0f});  // se abre
    run(60);
    check(system.occlusionOf(radio) < 0.05f, "al abrir la puerta se destapa (con fundido)");
    door.setWorldPosition(Vec3{3.0f, 0.0f, 0.0f});
    run(60);
    check(system.occlusionOf(radio) > 0.95f, "al cerrarla vuelve a sonar tapado");
    world.findByName("Camara").get<audio::AudioListener>().occlusion = false;
    run(60);
    check(system.occlusionOf(radio) < 0.05f, "apagar la oclusion en el Audio Listener");
    (void)listener;

    // Reverberacion: dentro de la zona, fuera nada.
    ecs::Entity cave = world.create("Cueva");
    audio::AudioReverbZone& zone = cave.add<audio::AudioReverbZone>();
    zone.preset = audio::ReverbPreset::Cave;
    zone.level = 0.8f;
    run(1);
    check(system.reverbLevel() > 0.79f, "dentro de una Audio Reverb Zone reverbera");
    cave.setWorldPosition(Vec3{100.0f, 0.0f, 0.0f});
    run(1);
    check(system.reverbLevel() == 0.0f, "fuera de la zona, sin reverberacion");
    ecs::World copy;
    ecs::deserializeWorld(copy, ecs::serializeWorld(world));
    const ecs::Entity saved = copy.findByName("Radio");
    check(saved.valid() && saved.get<audio::AudioSource>().echo && copy.findByName("Cueva").has<audio::AudioReverbZone>(),
          "efectos y zona se guardan con la escena");
    system.stop();
}

// Lo que se oye de verdad (mezcla sin altavoces): la puerta y el paso bajo
// quitan la mayor parte de un tono agudo.
void testAudioRendered() {
    std::printf("Audio: senal mezclada (sin dispositivo)\n");
    writeWav(kRoot / "Audio" / "agudo.wav", 4000.0);
    ecs::World world;
    ecs::Entity ears = world.create("Camara");
    ears.add<audio::AudioListener>();
    ecs::Entity radio = world.create("Radio");
    radio.setWorldPosition(Vec3{0.0f, 0.0f, -4.0f});
    audio::AudioSource& source = radio.add<audio::AudioSource>();
    source.clip = "Audio/agudo.wav";
    source.loop = true;
    source.min_distance = 10.0f;  // sin atenuacion por distancia
    float walls = 0.0f;
    audio::AudioSystem system{audio::AudioSystem::Offline{}};
    system.setAssetsRoot(kRoot);
    system.setOcclusionQuery([&](const Vec3&, const Vec3&, ecs::Entity, ecs::Entity) { return walls; });
    system.start(world);
    std::vector<float> block(800 * 2);
    const auto rms = [&](int frames_of_warmup) {
        for (int i = 0; i < frames_of_warmup; ++i) {
            system.update(world, 1.0f / 60.0f, Vec3{}, Vec3{0, 0, -1});
            system.renderOffline(block.data(), 800);
        }
        double sum = 0.0;
        for (int i = 0; i < 6; ++i) {
            system.update(world, 1.0f / 60.0f, Vec3{}, Vec3{0, 0, -1});
            system.renderOffline(block.data(), 800);
            for (const float v : block) sum += static_cast<double>(v) * v;
        }
        return std::sqrt(sum / (6.0 * block.size()));
    };
    const double open = rms(30);
    walls = 1.0f;
    const double closed = rms(60);
    walls = 0.0f;
    const double reopened = rms(60);
    source.low_pass = true;
    source.low_pass_cutoff = 500.0f;
    const double filtered = rms(30);
    std::printf("  RMS: libre %.4f, tras la puerta %.4f, abierta %.4f, paso bajo 500 Hz %.4f\n", open, closed, reopened, filtered);
    check(open > 0.01, "el tono suena");
    check(closed < open * 0.3, "tras una pared se oye mucho mas bajo y apagado");
    check(std::abs(reopened - open) < open * 0.1, "al abrir vuelve a oirse igual");
    check(filtered < open * 0.2, "el paso bajo del Audio Source quita los agudos");
    system.stop();
}

}  // namespace

// Graphics: las opciones van al host; Graphics.post al volumen global.
class FakeGraphics final : public scripting::GraphicsHost {
public:
    std::vector<scripting::GraphicsOption> opts = {
        {"vsync", false, true, "", {}},
        {"texture_quality", std::string("auto"), true, "", {"auto", "low", "high"}},
        {"target_fps", 60.0, true, "", {}},
        {"gpu", std::string("Test GPU"), false, "", {}},
    };
    std::string level = "Personalizada";
    int saves = 0;
    std::vector<scripting::GraphicsOption> options() const override { return opts; }
    bool set(const std::string& key, const scripting::GraphicsValue& v, std::string& error) override {
        for (auto& o : opts) {
            if (o.key != key) continue;
            if (!o.writable) {
                error = "solo lectura";
                return false;
            }
            if (v.index() != o.value.index()) {
                error = "tipo";
                return false;
            }
            o.value = v;
            return true;
        }
        error = "no existe";
        return false;
    }
    std::vector<std::string> qualityLevels() const override { return {"Baja", "Media", "Alta", "Ultra"}; }
    bool setQuality(const std::string& l, std::string&) override {
        level = l;
        return true;
    }
    std::string quality() const override { return level; }
    std::vector<std::pair<int, int>> resolutions() const override { return {{1920, 1080}, {1280, 720}}; }
    bool save(std::string&) override {
        ++saves;
        return true;
    }
};

void testGraphics() {
    std::printf("Graphics\n");
    ecs::World world;
    ecs::Entity volume = world.create("Volumen");
    volume.add<ecs::PostProcessing>();
    FakeGraphics host;
    scripting::ScriptSystem scripts;
    scripts.setAssetsRoot(kRoot);
    scripts.setGraphics(&host);
    scripts.start(world);
    std::string out;
    bool ok = scripts.run(R"(
        Graphics.vsync = true
        Graphics.set{ texture_quality = "high", target_fps = 144 }
        local bad = Graphics.set("gpu", "otra")
        Graphics.setQuality(2)
        local n = 0
        for _, o in ipairs(Graphics.options()) do n = n + 1 end
        Graphics.post.bloom = false
        Graphics.post.bloom_intensity = 0.5
        Graphics.post.tonemapper = "ACES"
        Graphics.save()
        return tostring(Graphics.vsync) .. "|" .. Graphics.get("texture_quality") .. "|" .. Graphics.target_fps ..
               "|" .. tostring(bad) .. "|" .. Graphics.getQuality() .. "|" .. n .. "|" .. #Graphics.resolutions() ..
               "|" .. tostring(Graphics.post.bloom) .. "|" .. Graphics.post.bloom_intensity .. "|" ..
               Graphics.post.tonemapper .. "|" .. tostring(Graphics.post.no_existe) .. "|" .. #Graphics.postKeys()
    )", &out);
    std::printf("    %s\n", out.c_str());
    check(ok, "la tabla Graphics desde Lua sin errores");
    check(out.rfind("true|high|144", 0) == 0, "Graphics.vsync = true, Graphics.set{...} y Graphics.get");
    check(out.find("|false|Alta|4|2|") != std::string::npos,
          "solo lectura rechazada, setQuality(2) = Alta, options() y resolutions()");
    const gfx::PostProcessSettings& s = volume.get<ecs::PostProcessing>().settings;
    check(!s.bloom && std::abs(s.bloom_intensity - 0.5f) < 1e-4f, "Graphics.post cambia el volumen global de la escena");
    check(out.find("|false|0.5|ACES|nil|") != std::string::npos, "Graphics.post lee campos y enums; clave desconocida = nil");
    check(host.saves == 1, "Graphics.save() llega al programa");
    scripts.stop();

    // Sin volumen global, Graphics.post crea uno.
    ecs::World empty;
    scripting::ScriptSystem scripts2;
    scripts2.setAssetsRoot(kRoot);
    scripts2.start(empty);
    ok = scripts2.run("Graphics.post.vignette = false; return Graphics.get('vsync') == nil", &out);
    bool created = false;
    for (const entt::entity h : empty.registry().view<ecs::PostProcessing>()) {
        created = !empty.registry().get<ecs::PostProcessing>(h).settings.vignette;
    }
    check(ok && created && out == "true", "sin volumen global lo crea; sin host las opciones son nil");
    scripts2.stop();
}

// Esqueletos desde Lua: campos de cualquier componente, IK, ragdoll y huesos.
void testRigLua() {
    std::printf("Lua: huesos, IK, ragdoll y campos de componentes\n");
    ecs::World world;
    ecs::Entity dog = world.create("Perro");
    ecs::Entity ball = world.create("Pelota");
    ball.setLocalPosition(Vec3{1.0f, 0.0f, 2.0f});
    scripting::ScriptSystem scripts;
    scripts.setAssetsRoot(kRoot);
    // Un "esqueleto" de mentira: la pose de un hueso y sus nombres.
    scripting::ScriptSystem::SkeletonHost host;
    host.bone_world = [](ecs::Entity, const std::string& bone, core::Mat4& m) {
        if (bone != "Head") return false;
        m = core::translate(Vec3{0.0f, 1.7f, 0.9f});
        return true;
    };
    host.bone_names = [](ecs::Entity) { return std::vector<std::string>{"Body", "Head", "Tail1"}; };
    scripts.setSkeletonHost(host);
    scripts.start(world);
    std::string out;
    const bool ok = scripts.run(R"(
        local dog = Scene.find('Perro')
        local ball = Scene.find('Pelota')
        dog:setField('PhysBones', 'chains[1].bone', 'Tail1')
        dog:setField('PhysBones', 'chains[1].gravity', 0.5)
        dog:setField('PhysBones', 'chains[2].bone', 'Ear_L')
        dog:setIKTarget('FrontFoot_L', ball, 3)
        dog:setIKTarget('left_hand', Vec3(1, 2, 3))
        dog:setLookAt(ball, 0.8)
        dog:setBoneRotation('Head', Vec3(0, 30, 0))
        dog.ragdoll = true
        local head = dog:getBonePosition('Head')
        return dog:getField('PhysBones', 'chains#') .. '|' .. dog:getField('PhysBones', 'chains[1].gravity') .. '|' ..
               tostring(dog.ragdoll) .. '|' .. #dog:getBones() .. '|' .. string.format('%.1f', head.y) .. '|' ..
               tostring(dog:getBonePosition('Nada')) .. '|' .. dog:getField('InverseKinematics', 'chains[1].length') .. '|' ..
               tostring(dog:setField('Light', 'no_existe', 1))
    )", &out);
    std::printf("    %s\n", out.c_str());
    check(ok, "sin errores de Lua");
    check(out == "2|0.5|true|3|1.7|nil|3|false",
          "setField/getField con listas, ragdoll, getBones, getBonePosition y claves que no existen");
    const ecs::PhysBones* pb = dog.tryGet<ecs::PhysBones>();
    check(pb != nullptr && pb->chains.size() == 2 && pb->chains[0].bone == "Tail1" && std::abs(pb->chains[0].gravity - 0.5f) < 1e-5f,
          "setField anade el componente y los elementos de la lista");
    const ecs::InverseKinematics* ik = dog.tryGet<ecs::InverseKinematics>();
    check(ik != nullptr && ik->chains.size() == 1 && ik->chains[0].target == ball.uuid() && ik->chains[0].length == 3 &&
              ik->left_hand.use_position && ik->look_at == ball.uuid() && std::abs(ik->look_weight - 0.8f) < 1e-5f,
          "setIKTarget (entidad o punto), longitud de la cadena y setLookAt");
    const ecs::Skeleton* sk = dog.tryGet<ecs::Skeleton>();
    check(sk != nullptr && sk->bones.size() == 1 && std::abs(sk->bones[0].rotation.y - 30.0f) < 1e-4f, "setBoneRotation");
    check(dog.has<ecs::Ragdoll>() && dog.get<ecs::Ragdoll>().active, "entity.ragdoll = true");
    scripts.stop();
}

int main() {
    testScripts();
    testAudio();
    testAudioOcclusion();
    testAudioRendered();
    testGraphics();
    testRigLua();
    std::filesystem::remove_all(kRoot);
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
