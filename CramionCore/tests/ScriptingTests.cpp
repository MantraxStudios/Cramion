// Pruebas del sistema de scripts y del audio (consola). Devuelve 0 si todo va.
//
// La API (Graphics, huesos, IK, ragdoll, getField/setField...) tiene sus
// pruebas en tests/native (una por modulo). Aqui: las escenas de antes de la
// 2.9 con scripts de Lua (se cargan, avisan y no se ejecutan), la plantilla de
// script nuevo y el audio.

#include "CramionCore/audio/Audio.h"
#include "CramionCore/environment/Environment.h"
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

const std::filesystem::path kRoot = std::filesystem::temp_directory_path() / "cramion_script_assets";

void testScripts() {
    std::printf("Scripts: escenas con Lua (obsoleto) y plantilla\n");
    scripting::registerScriptComponents();
    ecs::World world;
    ecs::Entity mover = world.create("Mover");
    scripting::Script& script = mover.add<scripting::Script>();
    script.file = "Scripts/Mover.lua";
    script.properties.push_back({"velocidad", scripting::PropertyType::Number, "10"});
    world.create("Otro").add<scripting::Script>().file = "Scripts/Otro.lua";

    scripting::ScriptSystem system;
    system.setAssetsRoot(kRoot);
    std::vector<std::string> log;
    system.setLog([&](int, const std::string& m) { log.push_back(m); });
    const auto warnings = [&] {
        int n = 0;
        for (const std::string& m : log) n += m.find("ya no ejecuta Lua") != std::string::npos ? 1 : 0;
        return n;
    };
    system.start(world);
    const Vec3 before = mover.worldPosition();
    for (int i = 0; i < 10; ++i) system.update(world, 0.1f);
    check(warnings() == 1 && log.front().find("2 objeto(s)") != std::string::npos,
          "una escena con scripts de Lua avisa una vez (cuantos objetos)");
    check(core::length(mover.worldPosition() - before) < 1e-6f && system.errors().empty(),
          "el script de Lua no se ejecuta (ni da errores)");
    system.stop();
    system.start(world);
    system.update(world, 0.1f);
    check(warnings() == 1, "al volver a darle a Play no se repite el aviso");
    system.stop();

    // El componente (y sus valores) se conserva en la escena para convertirlo.
    ecs::World copy;
    ecs::deserializeWorld(copy, ecs::serializeWorld(world));
    const ecs::Entity c = copy.findByName("Mover");
    check(c.valid() && c.has<scripting::Script>() && c.get<scripting::Script>().file == "Scripts/Mover.lua" &&
              c.get<scripting::Script>().properties.size() == 1 && c.get<scripting::Script>().properties[0].value == "10",
          "el Script de Lua (y sus valores) se conserva en la escena");
    check(scripting::scriptTemplate("MiJugador").find("void MiJugador::update(float dt)") != std::string::npos,
          "plantilla de script nuevo (C++)");
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

// Al parar el Play no queda nada sonando: ni las fuentes, ni la lluvia y el
// viento sintetizados del ambiente (seguian sonando para siempre: fuera del
// Play nadie bajaba sus niveles), ni la cola de la reverberacion. Y la pausa
// del Play calla todo y lo deja seguir.
void testAudioStop() {
    std::printf("Audio: parar y pausar el Play\n");
    writeWav(kRoot / "Audio" / "agudo.wav", 4000.0);
    ecs::World world;
    world.create("Camara").add<audio::AudioListener>();
    ecs::Entity radio = world.create("Radio");
    audio::AudioSource& source = radio.add<audio::AudioSource>();
    source.clip = "Audio/agudo.wav";
    source.loop = true;
    source.min_distance = 10.0f;
    environment::Environment& env = world.create("Ambiente").add<environment::Environment>();
    env.runtime.initialized = true;
    env.runtime.current.rain = 1.0f;
    env.runtime.wind_speed = 14.0f;
    audio::AudioReverbZone& cave = world.create("Cueva").add<audio::AudioReverbZone>();
    cave.preset = audio::ReverbPreset::Cave;
    cave.level = 0.9f;
    audio::AudioSystem system{audio::AudioSystem::Offline{}};
    system.setAssetsRoot(kRoot);
    std::vector<float> block(800 * 2);  // 1/60 s a 48 kHz
    // `update`: como en Play (el editor solo llama a update() en Play).
    const auto rms = [&](bool update, int blocks) {
        double sum = 0.0;
        for (int i = 0; i < blocks; ++i) {
            if (update) system.update(world, 1.0f / 60.0f, Vec3{}, Vec3{0, 0, -1});
            system.renderOffline(block.data(), 800);
            for (const float v : block) sum += static_cast<double>(v) * v;
        }
        return std::sqrt(sum / (static_cast<double>(blocks) * block.size()));
    };
    system.start(world);
    rms(true, 60);  // la lluvia y el viento suben en ~0.35 s
    const double playing = rms(true, 6);
    source.mute = true;
    rms(true, 30);
    const double ambience = rms(true, 6);
    system.stop();
    rms(false, 2);  // el fundido (15 ms)
    const double stopped = rms(false, 60);
    std::printf("  RMS: en Play %.4f, solo el ambiente %.4f, tras parar %.6f\n", playing, ambience, stopped);
    check(playing > 0.01 && ambience > 0.005, "en Play suenan la radio, la lluvia y el viento");
    check(stopped < 1e-4, "al parar no queda nada sonando (fuentes, ambiente ni reverberacion)");
    check(!system.isPlaying(radio), "la fuente ya no suena (se libero)");

    // Pausa: todo callado; al quitarla sigue sonando.
    source.mute = false;
    system.start(world);
    rms(true, 60);
    system.setPaused(true);
    rms(false, 2);
    const double paused = rms(false, 30);
    system.setPaused(false);
    rms(true, 10);
    const double resumed = rms(true, 6);
    std::printf("  RMS: en pausa %.6f, al seguir %.4f\n", paused, resumed);
    check(paused < 1e-4, "en pausa no suena nada");
    check(resumed > 0.01 && system.isPlaying(radio), "al quitar la pausa sigue sonando");
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

int main() {
    testScripts();
    testAudio();
    testAudioOcclusion();
    testAudioRendered();
    testAudioStop();
    std::filesystem::remove_all(kRoot);
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
