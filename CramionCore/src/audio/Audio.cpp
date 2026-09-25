#include "CramionCore/audio/Audio.h"

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <iostream>
#include <unordered_map>
#include <vector>

namespace cramion::audio {

using core::Vec3;
using ecs::FloatRange;

void AudioSource::reflect(ecs::PropertyVisitor& v) {
    v.field({"clip", "Clip", "Archivo de audio en Assets (WAV, MP3, FLAC, OGG)"}, clip);
    v.field({"volume", "Volumen"}, volume, FloatRange{0.0f, 2.0f, 0.01f, "%.2f", true});
    v.field({"pitch", "Tono"}, pitch, FloatRange{0.1f, 3.0f, 0.01f, "%.2f", true});
    v.field({"loop", "Bucle"}, loop);
    v.field({"play_on_awake", "Sonar al empezar"}, play_on_awake);
    v.field({"mute", "Silencio"}, mute);
    v.field({"spatial", "3D", "Con posicion (se atenua y se situa); sin, 2D (musica, interfaz)"}, spatial);
    const bool all = v.wantsAllFields();
    if (all || !spatial) {
        v.field({"pan", "Panorama"}, pan, FloatRange{-1.0f, 1.0f, 0.01f, "%.2f", true});
    }
    if (all || spatial) {
        v.field({"min_distance", "Distancia minima"}, min_distance, FloatRange{0.01f, 1000.0f, 0.05f, "%.2f m"});
        v.field({"max_distance", "Distancia maxima"}, max_distance, FloatRange{0.1f, 10000.0f, 0.5f, "%.1f m"});
        static constexpr std::array<const char*, 2> kRolloff = {"Logaritmica", "Lineal"};
        ecs::enumField(v, {"rolloff", "Atenuacion"}, rolloff, kRolloff);
        v.field({"doppler", "Doppler"}, doppler, FloatRange{0.0f, 5.0f, 0.01f, "%.2f", true});
    }
}

void AudioListener::reflect(ecs::PropertyVisitor& v) {
    v.field({"volume", "Volumen general"}, volume, FloatRange{0.0f, 2.0f, 0.01f, "%.2f", true});
}

bool isAudioFile(const std::filesystem::path& file) {
    std::string ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".ogg";
}

struct AudioSystem::Impl {
    ma_engine engine{};
    bool ok = false;
    bool running = false;
    std::filesystem::path root;

    struct Voice {
        ma_sound sound{};
        bool loaded = false;
        std::string clip;
        Vec3 last_position{};
        bool has_position = false;
        ~Voice() {
            if (loaded) ma_sound_uninit(&sound);
        }
    };
    std::unordered_map<entt::entity, std::unique_ptr<Voice>> voices;
    std::vector<std::unique_ptr<Voice>> one_shots;
    std::unique_ptr<Voice> preview;
    Vec3 listener_last{};
    bool listener_has_last = false;

    bool load(Voice& voice, const std::string& clip, bool spatial) {
        if (!ok || clip.empty()) return false;
        const std::filesystem::path file = root / std::filesystem::path(std::u8string(clip.begin(), clip.end()));
        return loadFile(voice, file, spatial) && ((voice.clip = clip), true);
    }

    bool loadFile(Voice& voice, const std::filesystem::path& file, bool spatial) {
        if (!ok) return false;
        std::error_code error;
        const auto size = std::filesystem::file_size(file, error);
        // Largos (musica): se leen mientras suenan; cortos, enteros en memoria.
        ma_uint32 flags = (!error && size > 2u * 1024u * 1024u) ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
        if (!spatial) flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;
        const ma_result result = ma_sound_init_from_file_w(&engine, file.wstring().c_str(), flags, nullptr, nullptr,
                                                          &voice.sound);
        if (result != MA_SUCCESS) {
            std::cerr << "[Audio] No se pudo abrir " << file.string() << " (" << ma_result_description(result) << ")\n";
            return false;
        }
        voice.loaded = true;
        return true;
    }

    void apply(Voice& voice, const AudioSource& source, const Vec3& position, float dt) {
        ma_sound& s = voice.sound;
        ma_sound_set_volume(&s, source.mute ? 0.0f : std::max(source.volume, 0.0f));
        ma_sound_set_pitch(&s, std::max(source.pitch, 0.01f));
        ma_sound_set_looping(&s, source.loop ? MA_TRUE : MA_FALSE);
        ma_sound_set_spatialization_enabled(&s, source.spatial ? MA_TRUE : MA_FALSE);
        if (source.spatial) {
            ma_sound_set_position(&s, position.x, position.y, position.z);
            ma_sound_set_min_distance(&s, std::max(source.min_distance, 0.01f));
            ma_sound_set_max_distance(&s, std::max(source.max_distance, source.min_distance + 0.01f));
            ma_sound_set_attenuation_model(&s, source.rolloff == Rolloff::Linear ? ma_attenuation_model_linear
                                                                                 : ma_attenuation_model_inverse);
            ma_sound_set_doppler_factor(&s, std::max(source.doppler, 0.0f));
            if (voice.has_position && dt > 1e-5f) {
                const Vec3 velocity = (position - voice.last_position) * (1.0f / dt);
                ma_sound_set_velocity(&s, velocity.x, velocity.y, velocity.z);
            }
            voice.last_position = position;
            voice.has_position = true;
        } else {
            ma_sound_set_pan(&s, std::clamp(source.pan, -1.0f, 1.0f));
        }
    }
};

AudioSystem::AudioSystem() : impl_(std::make_unique<Impl>()) {
    ma_engine_config config = ma_engine_config_init();
    config.listenerCount = 1;
    if (ma_engine_init(&config, &impl_->engine) == MA_SUCCESS) {
        impl_->ok = true;
    } else {
        std::cerr << "[Audio] Sin dispositivo de audio: el juego sonara en silencio\n";
    }
}

AudioSystem::~AudioSystem() {
    stop();
    stopPreview();
    if (impl_->ok) ma_engine_uninit(&impl_->engine);
}

bool AudioSystem::available() const { return impl_->ok; }
void AudioSystem::setAssetsRoot(const std::filesystem::path& root) { impl_->root = root; }
bool AudioSystem::running() const { return impl_->running; }

void AudioSystem::start(ecs::World& world) {
    stop();
    impl_->running = true;
    impl_->listener_has_last = false;
    for (const entt::entity handle : world.registry().view<AudioSource>()) {
        const ecs::Entity e = world.wrap(handle);
        const AudioSource& source = e.get<AudioSource>();
        if (!source.play_on_awake || !e.activeInHierarchy()) continue;
        play(e);
    }
}

void AudioSystem::update(ecs::World& world, float delta_seconds, const Vec3& camera_position, const Vec3& camera_forward) {
    Impl& d = *impl_;
    if (!d.ok) return;

    // --- Oyente: el AudioListener activo o la camara ---
    Vec3 position = camera_position;
    Vec3 forward = camera_forward;
    float master = 1.0f;
    for (const entt::entity handle : world.registry().view<AudioListener>()) {
        const ecs::Entity e = world.wrap(handle);
        if (!e.activeInHierarchy()) continue;
        position = e.worldPosition();
        forward = e.forward();
        master = e.get<AudioListener>().volume;
        break;
    }
    ma_engine_listener_set_position(&d.engine, 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(&d.engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&d.engine, 0, 0.0f, 1.0f, 0.0f);
    if (d.listener_has_last && delta_seconds > 1e-5f) {
        const Vec3 velocity = (position - d.listener_last) * (1.0f / delta_seconds);
        ma_engine_listener_set_velocity(&d.engine, 0, velocity.x, velocity.y, velocity.z);
    }
    d.listener_last = position;
    d.listener_has_last = true;
    ma_engine_set_volume(&d.engine, std::max(master, 0.0f));

    if (!d.running) return;

    // --- Fuentes: las que siguen, las nuevas (sonar al empezar) y las quitadas ---
    for (auto it = d.voices.begin(); it != d.voices.end();) {
        const ecs::Entity e = world.wrap(it->first);
        const AudioSource* source = world.registry().valid(it->first) ? e.tryGet<AudioSource>() : nullptr;
        if (source == nullptr || !e.activeInHierarchy()) {
            it = d.voices.erase(it);
            continue;
        }
        // Clip cambiado (Inspector o script): se vuelve a cargar.
        if (source->clip != it->second->clip) {
            const bool was_playing = ma_sound_is_playing(&it->second->sound);
            it->second = std::make_unique<Impl::Voice>();
            if (d.load(*it->second, source->clip, source->spatial) && was_playing) ma_sound_start(&it->second->sound);
        }
        if (it->second->loaded) d.apply(*it->second, *source, e.worldPosition(), delta_seconds);
        ++it;
    }
    for (auto it = d.one_shots.begin(); it != d.one_shots.end();) {
        if (!(*it)->loaded || ma_sound_at_end(&(*it)->sound)) {
            it = d.one_shots.erase(it);
        } else {
            ++it;
        }
    }
}

void AudioSystem::stop() {
    impl_->voices.clear();
    impl_->one_shots.clear();
    impl_->running = false;
}

void AudioSystem::play(ecs::Entity entity) {
    Impl& d = *impl_;
    const AudioSource* source = entity.valid() ? entity.tryGet<AudioSource>() : nullptr;
    if (source == nullptr || !d.ok) return;
    auto& voice = d.voices[entity.handle()];
    if (!voice || voice->clip != source->clip) {
        voice = std::make_unique<Impl::Voice>();
        if (!d.load(*voice, source->clip, source->spatial)) return;
    }
    d.apply(*voice, *source, entity.worldPosition(), 0.0f);
    ma_sound_seek_to_pcm_frame(&voice->sound, 0);
    ma_sound_start(&voice->sound);
}

void AudioSystem::stop(ecs::Entity entity) {
    const auto it = impl_->voices.find(entity.handle());
    if (it != impl_->voices.end() && it->second->loaded) {
        ma_sound_stop(&it->second->sound);
        ma_sound_seek_to_pcm_frame(&it->second->sound, 0);
    }
}

void AudioSystem::pause(ecs::Entity entity) {
    const auto it = impl_->voices.find(entity.handle());
    if (it != impl_->voices.end() && it->second->loaded) ma_sound_stop(&it->second->sound);
}

bool AudioSystem::isPlaying(ecs::Entity entity) const {
    const auto it = impl_->voices.find(entity.handle());
    return it != impl_->voices.end() && it->second->loaded && ma_sound_is_playing(&it->second->sound);
}

void AudioSystem::playOneShot(const std::string& clip, const Vec3& position, float volume, bool spatial) {
    Impl& d = *impl_;
    auto voice = std::make_unique<Impl::Voice>();
    if (!d.load(*voice, clip, spatial)) return;
    AudioSource source;
    source.volume = volume;
    source.spatial = spatial;
    source.max_distance = 100.0f;
    d.apply(*voice, source, position, 0.0f);
    ma_sound_start(&voice->sound);
    d.one_shots.push_back(std::move(voice));
}

void AudioSystem::preview(const std::filesystem::path& file) {
    Impl& d = *impl_;
    stopPreview();
    auto voice = std::make_unique<Impl::Voice>();
    if (!d.loadFile(*voice, file, false)) return;
    ma_sound_start(&voice->sound);
    d.preview = std::move(voice);
}

void AudioSystem::stopPreview() { impl_->preview.reset(); }

bool AudioSystem::previewing() const {
    return impl_->preview && impl_->preview->loaded && ma_sound_is_playing(&impl_->preview->sound);
}

void registerAudioComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("AudioSource") == nullptr) {
        registry.registerComponent<AudioSource>("AudioSource", "Audio Source", "Audio");
        registry.registerComponent<AudioListener>("AudioListener", "Audio Listener", "Audio");
    }
}

}  // namespace cramion::audio
