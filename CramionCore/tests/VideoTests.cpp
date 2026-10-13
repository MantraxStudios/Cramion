// Pruebas del reproductor de video (consola, sin GPU ni codecs): el
// decodificador sintetico (cramion://test?...), AudioSystem sin altavoces
// (renderOffline) y un VideoOutput falso que apunta que frame le llega.
// Ejecutar de una en una. Devuelve 0 si todo va.
//
//   1 probe            info y miniatura
//   2 prepare          Prepared; el reloj no avanza antes del primer frame
//   3 tiempo de juego  30 fps, 1 s a 1/60: frame 30 +- 1; Material Override
//   4 reloj de audio   0,5 s mezclados -> time ~ 0,5; 440 Hz en la salida
//   5 varias fuentes   pista 0 en 3 AudioSource; quitar una -> 2 voces
//   6 varias pistas    pista 0 (440) y pista 1 (880); silenciar la 1
//   7 bucle            un LoopPointReached por vuelta; seccion start/end
//   8 busqueda         seek(1.5) -> SeekCompleted y frame 45, nada del epoch viejo
//   9 saltos           dt 0,5: FrameDropped > 0 y el reloj no se atrasa
//  10 velocidad 2      el tiempo avanza el doble
//  11 stop del Play    todos los hilos en < 300 ms, ninguna voz
//  12 cambio de escena se paran sin eventos de la escena vieja
//  13 destruir en open interrupt + forget en < 250 ms
//  14 politica de URL  http://example.com -> Error
//  15 serializacion    ida y vuelta por SceneSerializer (enums por nombre, pistas y destinos)

#include "CramionCore/audio/Audio.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/video/VideoFiles.h"
#include "CramionCore/video/VideoOutput.h"
#include "CramionCore/video/VideoPlayer.h"
#include "CramionCore/video/VideoSystem.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace cramion;
using core::Vec3;
using Clock = std::chrono::steady_clock;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

// --- VideoOutput falso -----------------------------------------------------------

class FakeOutput final : public video::VideoOutput {
public:
    struct Tex {
        std::uint32_t w = 0, h = 0;
    };
    std::map<std::int32_t, Tex> textures;
    std::map<std::int32_t, std::int64_t> last_index;   // por textura: el frame (marcador del sintetico)
    std::vector<std::int64_t> submitted;               // todos, en orden
    std::map<entt::entity, video::MaterialOverride> overrides;
    std::int32_t plane[2] = {-1, -1};
    std::int32_t next = 100;
    int destroyed = 0;

    std::int32_t createTexture(std::uint32_t w, std::uint32_t h) override {
        textures[next] = Tex{w, h};
        return next++;
    }
    void destroyTexture(std::int32_t id) override {
        textures.erase(id);
        ++destroyed;
    }
    std::int32_t textureForRenderTexture(const Uuid&) override { return -1; }
    std::uint32_t textureWidth(std::int32_t id) const override {
        const auto it = textures.find(id);
        return it == textures.end() ? 0 : it->second.w;
    }
    std::uint32_t textureHeight(std::int32_t id) const override {
        const auto it = textures.find(id);
        return it == textures.end() ? 0 : it->second.h;
    }
    bool submitFrame(std::int32_t id, const video::VideoFrame& frame, const video::ConvertParams&) override {
        std::int64_t index = video::syntheticFrameIndex(frame);
        if (index < 0) index = std::llround(frame.pts * 30.0);
        last_index[id] = index;
        submitted.push_back(index);
        return true;
    }
    void setMaterialOverride(entt::entity renderer, const video::MaterialOverride& o) override { overrides[renderer] = o; }
    void clearMaterialOverride(entt::entity renderer) override { overrides.erase(renderer); }
    void setCameraPlane(bool near_plane, std::int32_t id, float, video::AspectMode) override { plane[near_plane ? 1 : 0] = id; }

    std::int64_t lastSubmitted() const { return submitted.empty() ? -1 : submitted.back(); }
};

// --- Ayudas -------------------------------------------------------------------------

std::string synthetic(const std::string& params) { return "cramion://test?" + params; }

ecs::Entity makePlayer(ecs::World& world, const std::string& url, const std::function<void(video::VideoPlayer&)>& setup = {}) {
    ecs::Entity e = world.create("Pantalla");
    video::VideoPlayer& p = e.add<video::VideoPlayer>();
    p.source = video::VideoSource::Url;
    p.url = url;
    p.render_mode = video::RenderMode::ApiOnly;
    p.audio_output = video::AudioOutput::None;
    p.time_source = video::TimeSource::GameTime;
    if (setup) setup(p);
    return e;
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// update(dt) con un respiro para el hilo de decodificacion.
void steps(video::VideoSystem& vs, ecs::World& world, float dt, int count, int sleep_ms = 2) {
    for (int i = 0; i < count; ++i) {
        sleepMs(sleep_ms);
        vs.update(world, dt, dt);
    }
}

// update(0) hasta que se cumpla (o 3 s).
bool waitFor(video::VideoSystem& vs, ecs::World& world, const std::function<bool()>& done, int timeout_ms = 3000) {
    const auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < end) {
        vs.update(world, 0.0f, 0.0f);
        if (done()) return true;
        sleepMs(2);
    }
    return false;
}

bool prepared(video::VideoSystem& vs, ecs::Entity e) {
    const video::PlayerState s = vs.status(e).state;
    return s != video::PlayerState::Idle && s != video::PlayerState::Preparing && s != video::PlayerState::Error;
}

int countEvents(const std::vector<video::VideoEvent>& events, video::VideoEventType type) {
    int n = 0;
    for (const video::VideoEvent& ev : events) n += ev.type == type ? 1 : 0;
    return n;
}

// Potencia de una frecuencia (Goertzel) en el canal izquierdo.
double tone(const std::vector<float>& stereo, double freq, double rate = 48000.0) {
    const std::size_t n = stereo.size() / 2;
    if (n == 0) return 0.0;
    const double k = 2.0 * std::cos(2.0 * 3.14159265358979323846 * freq / rate);
    double s1 = 0.0, s2 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double s = stereo[i * 2] + k * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - k * s1 * s2)) / static_cast<double>(n);
}

struct AudioRig {
    audio::AudioSystem audio{audio::AudioSystem::Offline{}};
    std::vector<float> block = std::vector<float>(800 * 2);
    std::vector<float> recorded;

    // Un frame: video, audio y 1/60 s mezclado.
    void frame(video::VideoSystem& vs, ecs::World& world, bool record = false) {
        vs.update(world, 1.0f / 60.0f, 1.0f / 60.0f);
        audio.update(world, 1.0f / 60.0f, Vec3{}, Vec3{0, 0, -1});
        audio.renderOffline(block.data(), 800);
        if (record) recorded.insert(recorded.end(), block.begin(), block.end());
        sleepMs(1);
    }
};

// --- Pruebas ------------------------------------------------------------------------

void testFiles() {
    std::printf("1. Archivos, URLs y probe\n");
    check(video::isVideoFile("a/b/Intro.MP4") && video::isVideoFile("x.webm") && !video::isVideoFile("x.wav"),
          "isVideoFile por extension");
    std::string why;
    check(!video::checkVideoUrl("http://example.com/v.mp4", &why) && !why.empty(), "http a otro dominio: no (con motivo)");
    check(video::checkVideoUrl("https://example.com/v.mp4", nullptr), "https: si");
    check(video::checkVideoUrl("http://localhost:8080/v.mp4", nullptr), "http a localhost: si");
    check(!video::checkVideoUrl("rtsp://camara/1", nullptr), "rtsp: aun no (Fase 2)");
    std::string error;
    check(video::resolveClip("../fuera.mp4", std::filesystem::temp_directory_path(), &error).empty() && !error.empty(),
          "un clip no sale de Assets");

    video::VideoInfo info;
    asset::ImageRgba8 thumb;
    const bool ok = video::probeVideoUri(synthetic("w=64&h=36&fps=30&dur=2&tracks=2"), info, &thumb, 32);
    std::printf("  (%ux%u, %.1f fps, %.2f s, %zu pistas, miniatura %ux%u)\n", info.stream.width, info.stream.height, info.stream.fps,
                info.stream.duration, info.stream.audio.size(), thumb.width, thumb.height);
    check(ok && info.stream.width == 64 && info.stream.height == 36, "probe: tamano");
    check(std::fabs(info.stream.fps - 30.0) < 1e-6 && std::fabs(info.stream.duration - 2.0) < 1e-6, "probe: fps y duracion");
    check(info.stream.audio.size() == 2, "probe: 2 pistas de audio");
    check(info.has_thumbnail && thumb.width == 32 && !thumb.pixels.empty(), "probe: miniatura reducida");
    // Cache en disco (archivo local sin decodificador: se guarda el error y no se reintenta).
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "cramion_video_tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path fake = dir / "falso.mp4";
    std::ofstream(fake, std::ios::binary) << "no es un video";
    video::VideoInfo bad;
    video::probeVideo(fake, dir / "cache", bad);
    video::VideoInfo cached;
    check(video::readCachedVideoInfo(dir / "cache", fake, cached) && !cached.ok, "probe: el resultado queda en la cache (.json)");
}

void testPrepare() {
    std::printf("2. Prepare y esperar al primer frame\n");
    ecs::World world;
    FakeOutput out;
    video::VideoSystem vs;
    vs.setHost({nullptr, &out, {}});
    ecs::Entity e = makePlayer(world, synthetic("dur=2"), [](video::VideoPlayer& p) { p.play_on_awake = false; });
    vs.start(world);
    vs.update(world, 0.0f, 0.0f);
    check(vs.status(e).state == video::PlayerState::Idle, "sin Play On Awake: Idle");
    vs.prepare(e);
    const bool ok = waitFor(vs, world, [&] { return prepared(vs, e); });
    std::vector<video::VideoEvent> events = vs.takeEvents();
    check(ok && vs.status(e).state == video::PlayerState::Prepared, "prepare -> Prepared");
    check(countEvents(events, video::VideoEventType::Prepared) == 1, "un evento Prepared");
    check(out.lastSubmitted() == 0, "el primer frame ya esta en la textura");
    steps(vs, world, 1.0f / 60.0f, 10);
    check(vs.status(e).time == 0.0 && out.lastSubmitted() == 0, "preparado sin Play: el reloj no avanza");

    // Apertura lenta (delay): con wait_for_first_frame el reloj espera.
    ecs::Entity slow = makePlayer(world, synthetic("dur=2&delay=300"));
    steps(vs, world, 1.0f / 60.0f, 20);
    const video::PlayerStatus s = vs.status(slow);
    check(s.state == video::PlayerState::Preparing && s.time == 0.0, "mientras abre: Preparing y time = 0");
    const bool slow_ok = waitFor(vs, world, [&] { return vs.status(slow).state == video::PlayerState::Playing; });
    check(slow_ok && vs.status(slow).time < 0.05, "empieza en 0 al tener el primer frame");
    events = vs.takeEvents();
    check(countEvents(events, video::VideoEventType::Started) >= 1, "evento Started");
    vs.stop();
}

void testGameTime() {
    std::printf("3. Tiempo de juego y Material Override\n");
    ecs::World world;
    FakeOutput out;
    video::VideoSystem vs;
    vs.setHost({nullptr, &out, {}});
    ecs::Entity e = makePlayer(world, synthetic("fps=30&dur=10"),
                               [](video::VideoPlayer& p) { p.render_mode = video::RenderMode::MaterialOverride; });
    vs.start(world);
    check(waitFor(vs, world, [&] { return prepared(vs, e); }), "preparado");
    steps(vs, world, 1.0f / 60.0f, 60);
    const std::int64_t frame = out.lastSubmitted();
    std::printf("  (1 s de juego: frame %lld, time %.3f)\n", static_cast<long long>(frame), vs.status(e).time);
    check(frame >= 29 && frame <= 31, "1 s a 30 fps: frame 30 +- 1");
    check(std::fabs(vs.status(e).time - 1.0) < 0.02, "status().time ~ 1 s");
    const auto it = out.overrides.find(e.handle());
    check(it != out.overrides.end() && it->second.texture >= 0 && out.textures.count(it->second.texture) == 1,
          "Material Override sobre el propio objeto con una textura propia");
    check(vs.textureOf(e) == (it != out.overrides.end() ? it->second.texture : -2), "textureOf da esa textura");
    vs.stop();
    check(out.overrides.empty() && out.textures.empty(), "al parar: override quitado y textura destruida");
}

void testAudioClock() {
    std::printf("4. Reloj de audio\n");
    ecs::World world;
    FakeOutput out;
    AudioRig rig;
    video::VideoSystem vs;
    vs.setHost({&rig.audio, &out, {}});
    ecs::Entity e = makePlayer(world, synthetic("fps=30&dur=10&tracks=1"), [](video::VideoPlayer& p) {
        p.audio_output = video::AudioOutput::Direct;
        p.direct_spatial = false;
        p.time_source = video::TimeSource::AudioClock;
    });
    rig.audio.start(world);
    vs.start(world);
    const auto end = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < end && vs.status(e).state != video::PlayerState::Playing) {
        vs.update(world, 0.0f, 0.0f);
        sleepMs(2);
    }
    check(vs.status(e).state == video::PlayerState::Playing, "reproduciendo");
    for (int i = 0; i < 30; ++i) rig.frame(vs, world, true);  // 0,5 s mezclados
    vs.update(world, 0.0f, 0.0f);
    const video::PlayerStatus s = vs.status(e);
    std::printf("  (0,5 s de audio: time %.3f, voces %d, 440 Hz %.4f, 1000 Hz %.4f)\n", s.time, s.audio_voices,
                tone(rig.recorded, 440.0), tone(rig.recorded, 1000.0));
    check(s.audio_voices == 1, "una voz de stream (Direct)");
    check(std::fabs(s.time - 0.5) < 0.06, "time ~ 0,5 s (+- 1 frame y un bloque)");
    check(tone(rig.recorded, 440.0) > 0.02 && tone(rig.recorded, 440.0) > 5.0 * tone(rig.recorded, 1000.0), "suenan 440 Hz");
    vs.stop();
    rig.audio.stop();
}

void testMultiSource() {
    std::printf("5. Una pista en varias fuentes\n");
    ecs::World world;
    FakeOutput out;
    AudioRig rig;
    video::VideoSystem vs;
    vs.setHost({&rig.audio, &out, {}});
    std::vector<Uuid> speakers;
    for (int i = 0; i < 3; ++i) {
        ecs::Entity s = world.create("Altavoz");
        audio::AudioSource& src = s.add<audio::AudioSource>();
        src.spatial = false;
        src.play_on_awake = false;
        speakers.push_back(s.uuid());
    }
    ecs::Entity e = makePlayer(world, synthetic("fps=30&dur=10&tracks=1"), [&](video::VideoPlayer& p) {
        p.audio_output = video::AudioOutput::AudioSource;
        p.tracks.resize(1);
        p.tracks[0].targets = speakers;
    });
    rig.audio.start(world);
    vs.start(world);
    waitFor(vs, world, [&] { return vs.status(e).state == video::PlayerState::Playing; });
    for (int i = 0; i < 20; ++i) rig.frame(vs, world, true);
    check(vs.status(e).audio_voices == 3, "3 voces de stream (una por fuente)");
    check(tone(rig.recorded, 440.0) > 0.02, "440 Hz en la salida");
    e.get<video::VideoPlayer>().tracks[0].targets.pop_back();
    rig.frame(vs, world);
    check(vs.status(e).audio_voices == 2, "al quitar un destino quedan 2");
    vs.stop();
    rig.audio.stop();
}

void testMultiTrack() {
    std::printf("6. Varias pistas\n");
    ecs::World world;
    FakeOutput out;
    AudioRig rig;
    video::VideoSystem vs;
    vs.setHost({&rig.audio, &out, {}});
    ecs::Entity a = world.create("A");
    a.add<audio::AudioSource>().spatial = false;
    ecs::Entity b = world.create("B");
    b.add<audio::AudioSource>().spatial = false;
    ecs::Entity e = makePlayer(world, synthetic("fps=30&dur=10&tracks=2"), [&](video::VideoPlayer& p) {
        p.audio_output = video::AudioOutput::AudioSource;
        p.tracks.resize(2);
        p.tracks[0].targets = {a.uuid()};
        p.tracks[1].targets = {b.uuid()};
    });
    rig.audio.start(world);
    vs.start(world);
    waitFor(vs, world, [&] { return vs.status(e).state == video::PlayerState::Playing; });
    for (int i = 0; i < 20; ++i) rig.frame(vs, world, true);
    const double t880 = tone(rig.recorded, 880.0), t440 = tone(rig.recorded, 440.0);
    check(t440 > 0.02 && t880 > 0.02, "pista 0 (440 Hz) en A y pista 1 (880 Hz) en B");
    e.get<video::VideoPlayer>().tracks[1].mute = true;
    for (int i = 0; i < 5; ++i) rig.frame(vs, world);
    rig.recorded.clear();
    for (int i = 0; i < 20; ++i) rig.frame(vs, world, true);
    const double m880 = tone(rig.recorded, 880.0), m440 = tone(rig.recorded, 440.0);
    std::printf("  (antes 440 %.4f 880 %.4f; con la 1 silenciada 440 %.4f 880 %.4f)\n", t440, t880, m440, m880);
    check(m880 < t880 * 0.1 && m440 > 0.02, "silenciar la pista 1 quita los 880 Hz");
    vs.stop();
    rig.audio.stop();
}

void testLoop() {
    std::printf("7. Bucle\n");
    ecs::World world;
    FakeOutput out;
    video::VideoSystem vs;
    vs.setHost({nullptr, &out, {}});
    ecs::Entity e = makePlayer(world, synthetic("fps=30&dur=1"), [](video::VideoPlayer& p) { p.loop = true; });
    vs.start(world);
    waitFor(vs, world, [&] { return prepared(vs, e); });
    vs.takeEvents();
    steps(vs, world, 1.0f / 30.0f, 75);  // 2,5 s
    std::vector<video::VideoEvent> events = vs.takeEvents();
    std::printf("  (2,5 s: %d vueltas, time %.3f)\n", countEvents(events, video::VideoEventType::LoopPointReached), vs.status(e).time);
    check(countEvents(events, video::VideoEventType::LoopPointReached) == 2, "un LoopPointReached por vuelta (2 en 2,5 s)");
    check(vs.status(e).state == video::PlayerState::Playing && std::fabs(vs.status(e).time - 0.5) < 0.05, "sigue en la segunda vuelta");
    vs.stop();

    // Seccion 0,5..1,0 en bucle.
    ecs::World w2;
    FakeOutput out2;
    video::VideoSystem vs2;
    vs2.setHost({nullptr, &out2, {}});
    ecs::Entity s = makePlayer(w2, synthetic("fps=30&dur=4"), [](video::VideoPlayer& p) {
        p.loop = true;
        p.start_time = 0.5f;
        p.end_time = 1.0f;
    });
    vs2.start(w2);
    waitFor(vs2, w2, [&] { return prepared(vs2, s); });
    vs2.takeEvents();
    out2.submitted.clear();
    steps(vs2, w2, 1.0f / 30.0f, 48);  // 1,6 s
    events = vs2.takeEvents();
    bool inside = !out2.submitted.empty();
    for (const std::int64_t f : out2.submitted) inside = inside && f >= 15 && f < 30;
    check(countEvents(events, video::VideoEventType::LoopPointReached) == 3, "seccion de 0,5 s: 3 vueltas en 1,6 s");
    check(inside, "solo frames de la seccion (15..29)");
    vs2.stop();

    // Sin bucle: termina (Ended) con un LoopPointReached.
    ecs::World w3;
    FakeOutput out3;
    video::VideoSystem vs3;
    vs3.setHost({nullptr, &out3, {}});
    ecs::Entity once = makePlayer(w3, synthetic("fps=30&dur=0.5"));
    vs3.start(w3);
    waitFor(vs3, w3, [&] { return prepared(vs3, once); });
    steps(vs3, w3, 1.0f / 30.0f, 30);
    events = vs3.takeEvents();
    check(vs3.status(once).state == video::PlayerState::Ended && countEvents(events, video::VideoEventType::LoopPointReached) == 1,
          "sin bucle: Ended y un LoopPointReached");
    check(out3.lastSubmitted() == 14, "se queda en el ultimo frame");
    vs3.stop();
}

void testSeek() {
    std::printf("8. Busqueda\n");
    ecs::World world;
    FakeOutput out;
    video::VideoSystem vs;
    vs.setHost({nullptr, &out, {}});
    ecs::Entity e = makePlayer(world, synthetic("fps=30&dur=4&gop=10"));
    vs.start(world);
    waitFor(vs, world, [&] { return prepared(vs, e); });
    steps(vs, world, 1.0f / 60.0f, 10);
    vs.pause(e);
    vs.takeEvents();
    out.submitted.clear();
    vs.seek(e, 1.5);
    const bool done = waitFor(vs, world, [&] { return !out.submitted.empty(); });
    const std::vector<video::VideoEvent> events = vs.takeEvents();
    bool clean = !out.submitted.empty();
    for (const std::int64_t f : out.submitted) clean = clean && f >= 45;
    std::printf("  (tras seek(1.5): frame %lld, status %lld)\n", static_cast<long long>(out.lastSubmitted()),
                static_cast<long long>(vs.status(e).frame));
    check(done && countEvents(events, video::VideoEventType::SeekCompleted) == 1, "SeekCompleted");
    check(out.lastSubmitted() == 45 && vs.status(e).frame == 45, "el frame pasa a ser el 45 (gop 10: descarta del 40 al 44)");
    check(clean, "ningun frame anterior al objetivo (ni del epoch viejo)");
    vs.play(e);
    steps(vs, world, 1.0f / 30.0f, 15);
    check(out.lastSubmitted() >= 59 && out.lastSubmitted() <= 61, "sigue desde ahi (0,5 s despues: frame 60)");
    vs.stop();
}

void testDrops() {
    std::printf("9. Saltos de frames\n");
    ecs::World world;
    FakeOutput out;
    video::VideoSystem vs;
    vs.setHost({nullptr, &out, {}});
    ecs::Entity e = makePlayer(world, synthetic("fps=30&dur=20"), [](video::VideoPlayer& p) { p.skip_on_drop = true; });
    vs.start(world);
    waitFor(vs, world, [&] { return prepared(vs, e); });
    vs.takeEvents();
    steps(vs, world, 0.5f, 4, 10);
    const std::vector<video::VideoEvent> events = vs.takeEvents();
    const video::PlayerStatus s = vs.status(e);
    std::printf("  (2 s en 4 pasos: time %.3f, frame %lld, perdidos %llu)\n", s.time, static_cast<long long>(s.frame),
                static_cast<unsigned long long>(s.dropped));
    check(s.dropped > 0 && countEvents(events, video::VideoEventType::FrameDropped) > 0, "FrameDropped > 0");
    check(std::fabs(s.time - 2.0) < 0.01 && s.state == video::PlayerState::Playing, "el reloj no se atrasa (2 s)");
    // Con pasos de 0,5 s la imagen va como mucho un paso por detras (frames 45..60).
    check(out.lastSubmitted() >= 44, "la imagen sigue al reloj (saltando)");
    vs.stop();
}

void testSpeed() {
    std::printf("10. Velocidad 2\n");
    ecs::World world;
    FakeOutput out;
    video::VideoSystem vs;
    vs.setHost({nullptr, &out, {}});
    ecs::Entity e = makePlayer(world, synthetic("fps=30&dur=10"), [](video::VideoPlayer& p) { p.playback_speed = 2.0f; });
    vs.start(world);
    waitFor(vs, world, [&] { return prepared(vs, e); });
    steps(vs, world, 1.0f / 60.0f, 30);  // 0,5 s
    std::printf("  (0,5 s a 2x: time %.3f, frame %lld)\n", vs.status(e).time, static_cast<long long>(out.lastSubmitted()));
    check(std::fabs(vs.status(e).time - 1.0) < 0.02, "0,5 s a velocidad 2 = 1 s de video");
    check(out.lastSubmitted() >= 29 && out.lastSubmitted() <= 31, "frame 30");
    vs.stop();
}

void testStop() {
    std::printf("11. Stop del Play\n");
    ecs::World world;
    FakeOutput out;
    AudioRig rig;
    video::VideoSystem vs;
    vs.setHost({&rig.audio, &out, {}});
    std::vector<ecs::Entity> list;
    for (int i = 0; i < 4; ++i) {
        list.push_back(makePlayer(world, synthetic(i == 3 ? "dur=10&tracks=1&delay=60000" : "dur=10&tracks=1"),
                                  [](video::VideoPlayer& p) {
                                      p.audio_output = video::AudioOutput::Direct;
                                      p.direct_spatial = false;
                                  }));
    }
    rig.audio.start(world);
    vs.start(world);
    waitFor(vs, world, [&] { return vs.status(list[0]).state == video::PlayerState::Playing; });
    for (int i = 0; i < 5; ++i) rig.frame(vs, world);
    const int voices = vs.status(list[0]).audio_voices;
    const auto t0 = Clock::now();
    vs.stop();
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    std::printf("  (stop: %.1f ms; voces antes %d)\n", ms, voices);
    check(voices == 1, "habia voz de stream");
    check(ms < 300.0, "todos los hilos terminan en < 300 ms (tambien el que esta abriendo)");
    bool idle = true;
    for (const ecs::Entity& e : list) idle = idle && vs.status(e).state == video::PlayerState::Idle && vs.status(e).audio_voices == 0;
    check(idle, "ningun reproductor ni voz de stream");
    check(out.textures.empty(), "texturas soltadas");
    rig.audio.stop();
}

void testSceneChange() {
    std::printf("12. Cambio de escena\n");
    ecs::World world;
    FakeOutput out;
    video::VideoSystem vs;
    vs.setHost({nullptr, &out, {}});
    ecs::Entity e = makePlayer(world, synthetic("fps=30&dur=10"));
    vs.start(world);
    waitFor(vs, world, [&] { return prepared(vs, e); });
    steps(vs, world, 1.0f / 60.0f, 5);  // eventos sin recoger
    // Como resetSceneState: generacion nueva y mundo vacio.
    world.beginNewSceneGeneration();
    world.clear();
    vs.update(world, 1.0f / 60.0f, 1.0f / 60.0f);
    check(vs.takeEvents().empty(), "ningun evento de la escena vieja");
    check(out.textures.empty(), "salidas soltadas");
    // La escena nueva (el mismo mundo) vuelve a reproducir lo suyo.
    ecs::Entity fresh = makePlayer(world, synthetic("fps=30&dur=10"));
    check(waitFor(vs, world, [&] { return prepared(vs, fresh); }), "la escena nueva reproduce");
    vs.stop();
}

void testDestroyWhileOpening() {
    std::printf("13. Destruir mientras abre\n");
    ecs::World world;
    FakeOutput out;
    video::VideoSystem vs;
    vs.setHost({nullptr, &out, {}});
    ecs::Entity e = makePlayer(world, synthetic("dur=10&delay=60000"));
    vs.start(world);
    steps(vs, world, 1.0f / 60.0f, 3);
    check(vs.status(e).state == video::PlayerState::Preparing, "abriendo (decodificador lento)");
    const auto t0 = Clock::now();
    vs.forget(e);
    world.destroy(e);
    vs.update(world, 1.0f / 60.0f, 1.0f / 60.0f);
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    std::printf("  (forget: %.1f ms)\n", ms);
    check(ms < 250.0, "interrupt + forget en < 250 ms");
    // Y sin forget: la entidad desaparece y update la suelta.
    ecs::Entity other = makePlayer(world, synthetic("dur=10&delay=60000"));
    steps(vs, world, 1.0f / 60.0f, 3);
    const auto t1 = Clock::now();
    world.destroy(other);
    vs.update(world, 1.0f / 60.0f, 1.0f / 60.0f);
    const double ms2 = std::chrono::duration<double, std::milli>(Clock::now() - t1).count();
    check(ms2 < 250.0, "entidad destruida: update la suelta en < 250 ms");
    vs.stop();
}

void testUrlPolicy() {
    std::printf("14. Politica de URL\n");
    ecs::World world;
    FakeOutput out;
    video::VideoSystem vs;
    vs.setHost({nullptr, &out, {}});
    ecs::Entity e = makePlayer(world, "http://example.com/video.mp4");
    vs.start(world);
    vs.update(world, 1.0f / 60.0f, 1.0f / 60.0f);
    const std::vector<video::VideoEvent> events = vs.takeEvents();
    const video::PlayerStatus s = vs.status(e);
    std::string message;
    for (const video::VideoEvent& ev : events) {
        if (ev.type == video::VideoEventType::Error) message = ev.message;
    }
    std::printf("  (%s)\n", message.c_str());
    check(s.state == video::PlayerState::Error && !message.empty(), "Error con el motivo de la politica");
    vs.stop();
}

void testSerialization() {
    std::printf("15. Serializacion\n");
    ecs::World world;
    ecs::Entity speaker = world.create("Altavoz");
    ecs::Entity e = world.create("Pantalla");
    video::VideoPlayer& p = e.add<video::VideoPlayer>();
    p.source = video::VideoSource::Url;
    p.url = "https://example.com/a.m3u8";
    p.clip = "Videos/intro.mp4";
    p.render_mode = video::RenderMode::CameraNearPlane;
    p.material_property = video::MaterialProperty::Both;
    p.aspect = video::AspectMode::FitVertically;
    p.audio_output = video::AudioOutput::AudioSource;
    p.time_source = video::TimeSource::UnscaledTime;
    p.max_resolution = video::MaxResolution::P720;
    p.decode = video::DecodeMode::Software;
    p.color = video::ColorOutput::Linear;
    p.loop = true;
    p.playback_speed = 1.5f;
    p.end_time = 3.25f;
    p.target_renderer = speaker.uuid();
    p.tracks.resize(2);
    p.tracks[0].targets = {speaker.uuid(), e.uuid()};
    p.tracks[1].mute = true;
    p.tracks[1].volume = 0.25f;
    const std::string json = ecs::serializeWorld(world);
    check(json.find("CameraNearPlane") != std::string::npos && json.find("FitVertically") != std::string::npos,
          "enums por nombre en el .crscene");
    ecs::World copy;
    std::string error;
    const bool ok = ecs::deserializeWorld(copy, json, &error);
    const ecs::Entity back = copy.findByName("Pantalla");
    const video::VideoPlayer* q = back.valid() ? back.tryGet<video::VideoPlayer>() : nullptr;
    check(ok && q != nullptr, "la escena se lee con solo el registro del componente");
    if (q == nullptr) return;
    check(q->source == p.source && q->url == p.url && q->clip == p.clip && q->render_mode == p.render_mode &&
              q->material_property == p.material_property && q->aspect == p.aspect && q->audio_output == p.audio_output &&
              q->time_source == p.time_source && q->max_resolution == p.max_resolution && q->decode == p.decode &&
              q->color == p.color && q->loop && std::fabs(q->playback_speed - 1.5f) < 1e-6f && std::fabs(q->end_time - 3.25f) < 1e-6f,
          "campos y enums");
    check(q->target_renderer == speaker.uuid(), "referencia a otro objeto");
    check(q->tracks.size() == 2 && q->tracks[0].targets.size() == 2 && q->tracks[0].targets[0] == speaker.uuid() &&
              q->tracks[0].targets[1] == e.uuid() && q->tracks[1].mute && std::fabs(q->tracks[1].volume - 0.25f) < 1e-6f,
          "pistas y sus destinos (lista dentro de lista)");
}

}  // namespace

int main(int argc, char** argv) {
    video::registerVideoComponents();
    audio::registerAudioComponents();
    const std::string only = argc > 1 ? argv[1] : "";
    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"files", testFiles},     {"prepare", testPrepare},       {"gametime", testGameTime}, {"audioclock", testAudioClock},
        {"sources", testMultiSource}, {"tracks", testMultiTrack}, {"loop", testLoop},         {"seek", testSeek},
        {"drops", testDrops},     {"speed", testSpeed},           {"stop", testStop},         {"scene", testSceneChange},
        {"destroy", testDestroyWhileOpening}, {"url", testUrlPolicy}, {"serialize", testSerialization},
    };
    for (const auto& [name, fn] : tests) {
        if (only.empty() || only == name) fn();
    }
    std::printf("\n%d/%d comprobaciones bien\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
