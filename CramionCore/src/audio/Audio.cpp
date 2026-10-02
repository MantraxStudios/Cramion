#include "CramionCore/cvar/CVar.h"
#include "CramionCore/profiling/Profiler.h"
#include "CramionCore/audio/Audio.h"

#include "CramionCore/environment/Environment.h"
#include "WeatherAudio.h"

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <unordered_map>
#include <vector>

namespace cramion::audio {
namespace {
cvar::CVar<int> g_max_voices("audio.MaxVoices", 32,
                             "Fuentes que se mezclan de verdad a la vez: las demas (las que menos se oyen y las que "
                             "estan fuera de su alcance) siguen 'sonando' para el juego pero no gastan CPU",
                             cvar::Saved, 1, 512);
}  // namespace

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
        v.field({"occlusion", "Oclusion", "Con paredes (colliders) entre el sonido y el oyente se oye tapado.\n"
                                          "La oclusion se activa en el Audio Listener."},
                occlusion);
    }
    // Efectos (como los Audio Filters de Unity).
    v.field({"low_pass", "Paso bajo", "Quita los agudos: sonido lejano, apagado, por radio"}, low_pass);
    if (all || low_pass) {
        v.field({"low_pass_cutoff", "  Corte paso bajo"}, low_pass_cutoff, FloatRange{20.0f, 22000.0f, 10.0f, "%.0f Hz", true});
    }
    v.field({"high_pass", "Paso alto", "Quita los graves: altavoz pequeno, telefono"}, high_pass);
    if (all || high_pass) {
        v.field({"high_pass_cutoff", "  Corte paso alto"}, high_pass_cutoff, FloatRange{10.0f, 10000.0f, 5.0f, "%.0f Hz", true});
    }
    v.field({"echo", "Eco"}, echo);
    if (all || echo) {
        v.field({"echo_delay", "  Retardo"}, echo_delay, FloatRange{0.01f, 2.0f, 0.005f, "%.2f s", true});
        v.field({"echo_decay", "  Repeticion"}, echo_decay, FloatRange{0.0f, 0.95f, 0.01f, "%.2f", true});
        v.field({"echo_wet", "  Mezcla del eco"}, echo_wet, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
    }
    v.field({"reverb_send", "Envio a reverberacion", "Cuanto reverbera en las Audio Reverb Zone (0 = nada)"}, reverb_send,
            FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
}

void AudioListener::reflect(ecs::PropertyVisitor& v) {
    const bool all = v.wantsAllFields();
    v.field({"volume", "Volumen general"}, volume, FloatRange{0.0f, 2.0f, 0.01f, "%.2f", true});
    v.field({"occlusion", "Oclusion", "Los sonidos con paredes (colliders) en medio se oyen tapados:\n"
                                      "detras de una puerta cerrada, dentro de una casa. Desde un script:\n"
                                      "entidad.audioOcclusion = false / Audio.setOcclusion(true)"},
            occlusion);
    if (all || occlusion) {
        v.field({"occlusion_cutoff", "  Agudos tras una pared"}, occlusion_cutoff, FloatRange{100.0f, 8000.0f, 10.0f, "%.0f Hz", true});
        v.field({"occlusion_volume", "  Volumen por pared"}, occlusion_volume, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.field({"occlusion_max_walls", "  Paredes como mucho"}, occlusion_max_walls, 1, 8);
        v.field({"occlusion_speed", "  Rapidez del cambio"}, occlusion_speed, FloatRange{0.5f, 30.0f, 0.1f, "%.1f", true});
    }
    v.field({"low_pass", "Paso bajo general", "Todo se oye apagado (bajo el agua, menu de pausa)"}, low_pass);
    if (all || low_pass) {
        v.field({"low_pass_cutoff", "  Corte"}, low_pass_cutoff, FloatRange{50.0f, 22000.0f, 10.0f, "%.0f Hz", true});
    }
}

void AudioReverbZone::reflect(ecs::PropertyVisitor& v) {
    v.field({"min_distance", "Distancia minima", "Hasta aqui, reverberacion completa"}, min_distance,
            FloatRange{0.0f, 1000.0f, 0.1f, "%.1f m"});
    v.field({"max_distance", "Distancia maxima", "Desde aqui, nada"}, max_distance, FloatRange{0.1f, 2000.0f, 0.1f, "%.1f m"});
    static constexpr std::array<const char*, 8> kPresets = {"Personalizado", "Habitacion", "Bano", "Sala grande",
                                                           "Cueva", "Estadio", "Bosque", "Bajo el agua"};
    ecs::enumField(v, {"preset", "Tipo"}, preset, kPresets);
    if (v.wantsAllFields() || preset == ReverbPreset::Custom) {
        v.field({"room_size", "  Tamano"}, room_size, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.field({"damping", "  Amortiguacion"}, damping, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
    }
    v.field({"level", "Nivel"}, level, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
}

void reverbPresetValues(const AudioReverbZone& zone, float& room_size, float& damping, float& level) {
    level = zone.level;
    switch (zone.preset) {
        case ReverbPreset::Room: room_size = 0.45f; damping = 0.55f; break;
        case ReverbPreset::Bathroom: room_size = 0.55f; damping = 0.15f; break;
        case ReverbPreset::Hall: room_size = 0.82f; damping = 0.4f; break;
        case ReverbPreset::Cave: room_size = 0.93f; damping = 0.2f; break;
        case ReverbPreset::Arena: room_size = 0.97f; damping = 0.5f; break;
        case ReverbPreset::Forest: room_size = 0.3f; damping = 0.85f; break;
        case ReverbPreset::Underwater: room_size = 0.75f; damping = 0.95f; break;
        case ReverbPreset::Custom:
        default: room_size = zone.room_size; damping = zone.damping; break;
    }
}

bool isAudioFile(const std::filesystem::path& file) {
    std::string ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".ogg";
}

// -----------------------------------------------------------------------------
// Nodos propios del grafo de miniaudio (se procesan en el hilo de audio; los
// ajustes llegan por atomicos).
//
//   sonido -> VoiceFx --(salida 0)--> Master -> altavoces
//                     \--(salida 1)--> Reverb -> Master
// -----------------------------------------------------------------------------

namespace {

constexpr float kPi = 3.14159265358979f;

// Biquad (RBJ) en forma directa transpuesta II.
struct Biquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    std::array<float, 8> z1{};
    std::array<float, 8> z2{};

    void set(bool high, float cutoff, float rate) {
        const float w0 = 2.0f * kPi * std::clamp(cutoff, 10.0f, rate * 0.45f) / rate;
        const float cw = std::cos(w0);
        const float alpha = std::sin(w0) / (2.0f * 0.7071f);
        const float a0 = 1.0f + alpha;
        if (high) {
            b0 = (1.0f + cw) * 0.5f / a0;
            b1 = -(1.0f + cw) / a0;
        } else {
            b0 = (1.0f - cw) * 0.5f / a0;
            b1 = (1.0f - cw) / a0;
        }
        b2 = b0;
        a1 = -2.0f * cw / a0;
        a2 = (1.0f - alpha) / a0;
    }
    float process(float x, std::size_t c) {
        const float y = b0 * x + z1[c];
        z1[c] = b1 * x - a1 * y + z2[c];
        z2[c] = b2 * x - a2 * y;
        return y;
    }
    void reset() {
        z1.fill(0.0f);
        z2.fill(0.0f);
    }
};

// Efectos de un sonido.
struct VoiceFx {
    ma_node_base base;  // primero: el grafo lo trata como un ma_node
    ma_uint32 channels = 2;
    float rate = 48000.0f;
    // Ajustes (hilo principal -> hilo de audio). 0 = apagado.
    std::atomic<float> low_pass{0.0f};
    std::atomic<float> high_pass{0.0f};
    std::atomic<float> gain{1.0f};
    std::atomic<float> echo_delay{0.35f};
    std::atomic<float> echo_decay{0.45f};
    std::atomic<float> echo_wet{0.0f};
    std::atomic<float> send{0.0f};
    // Estado del hilo de audio.
    Biquad lp;
    Biquad hp;
    float lp_current = 0.0f;
    float hp_current = 0.0f;
    float gain_current = 1.0f;
    float send_current = 0.0f;
    std::vector<float> echo_buffer;  // 2 s
    ma_uint32 echo_pos = 0;
};

void voiceProcess(ma_node* node, const float** frames_in, ma_uint32* count_in, float** frames_out, ma_uint32* count_out) {
    VoiceFx& fx = *reinterpret_cast<VoiceFx*>(node);
    const ma_uint32 frames = *count_out;
    const ma_uint32 available = (frames_in != nullptr && frames_in[0] != nullptr && count_in != nullptr) ? *count_in : 0;
    const float* in = available > 0 ? frames_in[0] : nullptr;
    float* dry = frames_out[0];
    float* wet = frames_out[1];
    const std::size_t ch = std::min<std::size_t>(fx.channels, 8);

    const float lp_target = fx.low_pass.load(std::memory_order_relaxed);
    const float hp_target = fx.high_pass.load(std::memory_order_relaxed);
    const bool lp_on = lp_target > 0.0f && lp_target < fx.rate * 0.45f;
    const bool hp_on = hp_target > 0.0f;
    if (lp_on && std::abs(lp_target - fx.lp_current) > fx.lp_current * 0.005f) {
        if (fx.lp_current <= 0.0f) fx.lp.reset();
        fx.lp.set(false, lp_target, fx.rate);
        fx.lp_current = lp_target;
    }
    if (!lp_on) fx.lp_current = 0.0f;
    if (hp_on && std::abs(hp_target - fx.hp_current) > fx.hp_current * 0.005f) {
        if (fx.hp_current <= 0.0f) fx.hp.reset();
        fx.hp.set(true, hp_target, fx.rate);
        fx.hp_current = hp_target;
    }
    if (!hp_on) fx.hp_current = 0.0f;

    const float echo_wet = fx.echo_wet.load(std::memory_order_relaxed);
    const float echo_decay = std::clamp(fx.echo_decay.load(std::memory_order_relaxed), 0.0f, 0.95f);
    const ma_uint32 capacity = static_cast<ma_uint32>(fx.echo_buffer.size() / std::max<std::size_t>(ch, 1));
    const ma_uint32 delay_frames =
        std::clamp(static_cast<ma_uint32>(fx.echo_delay.load(std::memory_order_relaxed) * fx.rate), 1u, std::max(capacity, 1u));
    const bool echo_on = echo_wet > 0.0f && capacity > 0;
    if (fx.echo_pos >= delay_frames) fx.echo_pos = 0;

    const float gain_target = fx.gain.load(std::memory_order_relaxed);
    const float send_target = fx.send.load(std::memory_order_relaxed);
    const float gain_step = frames > 0 ? (gain_target - fx.gain_current) / static_cast<float>(frames) : 0.0f;
    const float send_step = frames > 0 ? (send_target - fx.send_current) / static_cast<float>(frames) : 0.0f;

    for (ma_uint32 i = 0; i < frames; ++i) {
        fx.gain_current += gain_step;
        fx.send_current += send_step;
        for (std::size_t c = 0; c < fx.channels; ++c) {
            float x = (in != nullptr && i < available) ? in[i * fx.channels + c] : 0.0f;
            if (c < ch) {
                if (lp_on) x = fx.lp.process(x, c);
                if (hp_on) x = fx.hp.process(x, c);
                if (echo_on) {
                    float& slot = fx.echo_buffer[static_cast<std::size_t>(fx.echo_pos) * ch + c];
                    const float delayed = slot;
                    slot = x + delayed * echo_decay;
                    x += delayed * echo_wet;
                }
            }
            x *= fx.gain_current;
            dry[i * fx.channels + c] = x;
            wet[i * fx.channels + c] = x * fx.send_current;
        }
        if (echo_on) fx.echo_pos = (fx.echo_pos + 1) % delay_frames;
    }
    fx.gain_current = gain_target;
    fx.send_current = send_target;
}

ma_node_vtable kVoiceVtable = {voiceProcess, nullptr, 1, 2,
                               MA_NODE_FLAG_CONTINUOUS_PROCESSING | MA_NODE_FLAG_ALLOW_NULL_INPUT};

// Reverberacion (Freeverb de Jezar: 8 filtros peine y 4 pasa-todo por canal).
struct ReverbFx {
    ma_node_base base;
    ma_uint32 channels = 2;
    std::atomic<float> room{0.5f};
    std::atomic<float> damping{0.5f};
    std::atomic<float> level{0.0f};
    float level_current = 0.0f;
    struct Comb {
        std::vector<float> buffer;
        std::size_t index = 0;
        float store = 0.0f;
    };
    struct AllPass {
        std::vector<float> buffer;
        std::size_t index = 0;
    };
    std::array<std::array<Comb, 8>, 2> combs;
    std::array<std::array<AllPass, 4>, 2> allpasses;

    void setup(float rate) {
        static constexpr std::array<int, 8> kComb = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static constexpr std::array<int, 4> kAllPass = {556, 441, 341, 225};
        const float scale = rate / 44100.0f;
        for (int side = 0; side < 2; ++side) {
            const int spread = side == 0 ? 0 : 23;
            for (std::size_t i = 0; i < 8; ++i) {
                combs[side][i].buffer.assign(static_cast<std::size_t>((kComb[i] + spread) * scale) + 1, 0.0f);
            }
            for (std::size_t i = 0; i < 4; ++i) {
                allpasses[side][i].buffer.assign(static_cast<std::size_t>((kAllPass[i] + spread) * scale) + 1, 0.0f);
            }
        }
    }
};

void reverbProcess(ma_node* node, const float** frames_in, ma_uint32* count_in, float** frames_out, ma_uint32* count_out) {
    ReverbFx& fx = *reinterpret_cast<ReverbFx*>(node);
    const ma_uint32 frames = *count_out;
    const ma_uint32 available = (frames_in != nullptr && frames_in[0] != nullptr && count_in != nullptr) ? *count_in : 0;
    const float* in = available > 0 ? frames_in[0] : nullptr;
    float* out = frames_out[0];
    const float feedback = std::clamp(fx.room.load(std::memory_order_relaxed), 0.0f, 1.0f) * 0.28f + 0.7f;
    const float damp = std::clamp(fx.damping.load(std::memory_order_relaxed), 0.0f, 1.0f) * 0.4f;
    const float level_target = fx.level.load(std::memory_order_relaxed) * 3.0f;
    const float level_step = frames > 0 ? (level_target - fx.level_current) / static_cast<float>(frames) : 0.0f;
    const ma_uint32 ch = fx.channels;
    for (ma_uint32 i = 0; i < frames; ++i) {
        fx.level_current += level_step;
        float input = 0.0f;
        if (in != nullptr && i < available) {
            for (ma_uint32 c = 0; c < ch; ++c) input += in[i * ch + c];
            input /= static_cast<float>(std::max(ch, 1u));
        }
        input *= 0.015f;
        std::array<float, 2> side_out{};
        for (int side = 0; side < 2; ++side) {
            float sum = 0.0f;
            for (ReverbFx::Comb& comb : fx.combs[side]) {
                float& slot = comb.buffer[comb.index];
                const float y = slot;
                comb.store = y * (1.0f - damp) + comb.store * damp;
                slot = input + comb.store * feedback;
                if (++comb.index >= comb.buffer.size()) comb.index = 0;
                sum += y;
            }
            for (ReverbFx::AllPass& ap : fx.allpasses[side]) {
                float& slot = ap.buffer[ap.index];
                const float buffered = slot;
                slot = sum + buffered * 0.5f;
                sum = buffered - sum;
                if (++ap.index >= ap.buffer.size()) ap.index = 0;
            }
            side_out[side] = sum * fx.level_current;
        }
        for (ma_uint32 c = 0; c < ch; ++c) {
            out[i * ch + c] = ch == 1 ? (side_out[0] + side_out[1]) * 0.5f : (c < 2 ? side_out[c] : 0.0f);
        }
    }
    fx.level_current = level_target;
}

ma_node_vtable kReverbVtable = {reverbProcess, nullptr, 1, 1,
                                MA_NODE_FLAG_CONTINUOUS_PROCESSING | MA_NODE_FLAG_ALLOW_NULL_INPUT};

// Todo lo que se oye: paso bajo general del AudioListener.
struct MasterFx {
    ma_node_base base;
    ma_uint32 channels = 2;
    float rate = 48000.0f;
    std::atomic<float> low_pass{0.0f};
    Biquad lp;
    float lp_current = 0.0f;
};

void masterProcess(ma_node* node, const float** frames_in, ma_uint32* count_in, float** frames_out, ma_uint32* count_out) {
    MasterFx& fx = *reinterpret_cast<MasterFx*>(node);
    const ma_uint32 frames = *count_out;
    const ma_uint32 available = (frames_in != nullptr && frames_in[0] != nullptr && count_in != nullptr) ? *count_in : 0;
    const float* in = available > 0 ? frames_in[0] : nullptr;
    float* out = frames_out[0];
    const float target = fx.low_pass.load(std::memory_order_relaxed);
    const bool on = target > 0.0f && target < fx.rate * 0.45f;
    if (on && std::abs(target - fx.lp_current) > fx.lp_current * 0.005f) {
        if (fx.lp_current <= 0.0f) fx.lp.reset();
        fx.lp.set(false, target, fx.rate);
        fx.lp_current = target;
    }
    if (!on) fx.lp_current = 0.0f;
    const std::size_t ch = std::min<std::size_t>(fx.channels, 8);
    for (ma_uint32 i = 0; i < frames; ++i) {
        for (std::size_t c = 0; c < fx.channels; ++c) {
            float x = (in != nullptr && i < available) ? in[i * fx.channels + c] : 0.0f;
            if (on && c < ch) x = fx.lp.process(x, c);
            out[i * fx.channels + c] = x;
        }
    }
}

ma_node_vtable kMasterVtable = {masterProcess, nullptr, 1, 1,
                                MA_NODE_FLAG_CONTINUOUS_PROCESSING | MA_NODE_FLAG_ALLOW_NULL_INPUT};

}  // namespace

struct AudioSystem::Impl {
    ma_engine engine{};
    bool ok = false;
    bool running = false;
    std::filesystem::path root;
    ma_uint32 channels = 2;
    float rate = 48000.0f;
    MasterFx master;
    ReverbFx reverb;
    bool master_ok = false;
    bool reverb_ok = false;
    OcclusionQuery occlusion_query;

    // Lo que dice el AudioListener este frame.
    AudioListener listener;
    bool listener_found = false;
    ecs::Entity listener_entity;
    Vec3 listener_position{};

    struct Voice {
        ma_sound sound{};
        bool loaded = false;
        VoiceFx fx;
        bool fx_ok = false;
        std::string clip;
        Vec3 last_position{};
        bool has_position = false;
        // Oclusion (paredes suavizadas y el objetivo del ultimo rayo).
        float walls = 0.0f;
        float walls_target = 0.0f;
        float query_timer = 0.0f;
        bool queried = false;
        // Sonidos sueltos: sus ajustes y su sitio.
        AudioSource settings;
        Vec3 position{};
        // Virtualizacion (audio.MaxVoices): sonando para el juego pero sin
        // mezclarse; su tiempo sigue avanzando y vuelve donde tocaria.
        bool virtual_voice = false;
        double virtual_since = 0.0;
        ma_uint64 virtual_cursor = 0;
        float audible = 0.0f;  // volumen que llega al oyente (este frame)
        ~Voice() {
            if (loaded) ma_sound_uninit(&sound);
            if (fx_ok) ma_node_uninit(&fx.base, nullptr);
        }
    };
    std::unordered_map<entt::entity, std::unique_ptr<Voice>> voices;
    double clock = 0.0;  // segundos de juego (para el tiempo de las voces virtuales)
    std::size_t real_voices = 0;
    std::size_t virtual_voices = 0;

    // Lo que se oye de una fuente desde el oyente: volumen por la caida con
    // la distancia (0 fuera de su alcance).
    static float audibility(const AudioSource& s, const Vec3& source, const Vec3& listener) {
        if (s.mute) return 0.0f;
        const float volume = std::max(s.volume, 0.0f);
        if (!s.spatial) return volume;
        const float distance = core::length(source - listener);
        const float min_d = std::max(s.min_distance, 0.01f);
        const float max_d = std::max(s.max_distance, min_d + 0.01f);
        if (distance >= max_d) return 0.0f;
        return volume * std::min(1.0f, min_d / std::max(distance, min_d));
    }

    // Deja sonar de verdad solo las mas audibles; las demas pasan a virtuales.
    void virtualizeVoices() {
        const int max_voices = std::max(g_max_voices.get(), 1);
        std::vector<Voice*> wanted;
        for (auto& [handle, v] : voices) {
            if (!v->loaded) continue;
            if (v->virtual_voice || ma_sound_is_playing(&v->sound)) wanted.push_back(v.get());
        }
        std::sort(wanted.begin(), wanted.end(), [](const Voice* a, const Voice* b) { return a->audible > b->audible; });
        real_voices = virtual_voices = 0;
        for (std::size_t i = 0; i < wanted.size(); ++i) {
            Voice& v = *wanted[i];
            const bool keep_real = static_cast<int>(i) < max_voices && v.audible > 1e-4f;
            if (keep_real) {
                if (v.virtual_voice) devirtualize(v);
                if (!v.virtual_voice) ++real_voices;
            } else {
                if (!v.virtual_voice) {
                    ma_sound_get_cursor_in_pcm_frames(&v.sound, &v.virtual_cursor);
                    ma_sound_stop(&v.sound);
                    v.virtual_voice = true;
                    v.virtual_since = clock;
                }
                ++virtual_voices;
            }
        }
    }

    void devirtualize(Voice& v) {
        ma_uint32 rate = 0;
        ma_sound_get_data_format(&v.sound, nullptr, nullptr, &rate, nullptr, 0);
        ma_uint64 length = 0;
        ma_sound_get_length_in_pcm_frames(&v.sound, &length);
        const double elapsed = std::max(clock - v.virtual_since, 0.0) * static_cast<double>(std::max(rate, 1u));
        ma_uint64 cursor = v.virtual_cursor + static_cast<ma_uint64>(elapsed);
        v.virtual_voice = false;
        if (length > 0 && cursor >= length) {
            if (!ma_sound_is_looping(&v.sound)) return;  // ya habria terminado
            cursor %= length;
        }
        ma_sound_seek_to_pcm_frame(&v.sound, cursor);
        ma_sound_start(&v.sound);
    }
    std::vector<std::unique_ptr<Voice>> one_shots;
    std::unique_ptr<Voice> preview;
    Vec3 listener_last{};
    bool listener_has_last = false;
    float reverb_level = 0.0f;
    // Lluvia, viento y truenos del ambiente (sintetizados, al master).
    WeatherAudio weather_audio;

    ma_node_graph* graph() { return ma_engine_get_node_graph(&engine); }

    void initBuses() {
        channels = ma_engine_get_channels(&engine);
        rate = static_cast<float>(ma_engine_get_sample_rate(&engine));
        const ma_uint32 io[1] = {channels};
        master.channels = channels;
        master.rate = rate;
        ma_node_config config = ma_node_config_init();
        config.vtable = &kMasterVtable;
        config.pInputChannels = io;
        config.pOutputChannels = io;
        if (ma_node_init(graph(), &config, nullptr, &master.base) == MA_SUCCESS) {
            master_ok = true;
            ma_node_attach_output_bus(&master.base, 0, ma_engine_get_endpoint(&engine), 0);
        }
        reverb.channels = channels;
        reverb.setup(rate);
        config.vtable = &kReverbVtable;
        if (ma_node_init(graph(), &config, nullptr, &reverb.base) == MA_SUCCESS) {
            reverb_ok = true;
            ma_node_attach_output_bus(&reverb.base, 0, master_ok ? static_cast<ma_node*>(&master.base) : ma_engine_get_endpoint(&engine), 0);
        }
        weather_audio.create(graph(), master_ok ? static_cast<void*>(&master.base) : static_cast<void*>(ma_engine_get_endpoint(&engine)),
                             channels, rate);
    }

    void uninitBuses() {
        weather_audio.destroy();
        if (reverb_ok) ma_node_uninit(&reverb.base, nullptr);
        if (master_ok) ma_node_uninit(&master.base, nullptr);
        reverb_ok = master_ok = false;
    }

    // El sonido pasa por sus efectos y de ahi al master y a la reverberacion.
    void attachEffects(Voice& voice) {
        if (!master_ok) return;
        voice.fx.channels = channels;
        voice.fx.rate = rate;
        voice.fx.echo_buffer.assign(static_cast<std::size_t>(rate * 2.0f) * std::min<ma_uint32>(channels, 8), 0.0f);
        const ma_uint32 in[1] = {channels};
        const ma_uint32 out[2] = {channels, channels};
        ma_node_config config = ma_node_config_init();
        config.vtable = &kVoiceVtable;
        config.pInputChannels = in;
        config.pOutputChannels = out;
        if (ma_node_init(graph(), &config, nullptr, &voice.fx.base) != MA_SUCCESS) return;
        voice.fx_ok = true;
        ma_node_attach_output_bus(&voice.fx.base, 0, &master.base, 0);
        if (reverb_ok) ma_node_attach_output_bus(&voice.fx.base, 1, &reverb.base, 0);
        ma_node_attach_output_bus(&voice.sound, 0, &voice.fx.base, 0);
    }

    bool load(Voice& voice, const std::string& clip, bool spatial) {
        if (!ok || clip.empty()) return false;
        const std::filesystem::path file = root / std::filesystem::path(std::u8string(clip.begin(), clip.end()));
        if (!loadFile(voice, file, spatial)) return false;
        voice.clip = clip;
        attachEffects(voice);
        return true;
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

    void apply(Voice& voice, const AudioSource& source, const Vec3& position, float dt, ecs::Entity entity) {
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
        applyEffects(voice, source, position, dt, entity);
    }

    // Oclusion y efectos del sonido (el nodo los lee en el hilo de audio).
    void applyEffects(Voice& voice, const AudioSource& source, const Vec3& position, float dt, ecs::Entity entity) {
        if (!voice.fx_ok) return;
        const bool occlude = listener.occlusion && source.occlusion && source.spatial && occlusion_query;
        if (occlude) {
            voice.query_timer -= dt;
            if (voice.query_timer <= 0.0f || !voice.queried) {
                // Un rayo cada ~0.1 s por sonido (repartidos: no todos en el mismo frame).
                voice.query_timer = 0.08f + static_cast<float>(reinterpret_cast<std::uintptr_t>(&voice) % 7) * 0.006f;
                const float reach = std::max(source.max_distance, source.min_distance) * 1.25f;
                if (core::length(position - listener_position) <= reach) {
                    const float walls = occlusion_query(listener_position, position, entity, listener_entity);
                    voice.walls_target = std::clamp(walls, 0.0f, static_cast<float>(std::max(listener.occlusion_max_walls, 1)));
                }
                if (!voice.queried) voice.walls = voice.walls_target;  // sin fundido al empezar a sonar
                voice.queried = true;
            }
        } else {
            voice.walls_target = 0.0f;
            voice.queried = false;
        }
        const float blend = std::min(1.0f, std::max(listener.occlusion_speed, 0.1f) * dt);
        voice.walls += (voice.walls_target - voice.walls) * (dt > 0.0f ? blend : 1.0f);

        float cutoff = source.low_pass ? source.low_pass_cutoff : 0.0f;
        float gain = 1.0f;
        if (voice.walls > 0.01f) {
            // Una pared: del sonido limpio (20 kHz) al corte del oyente; mas
            // paredes, mas bajo y mas apagado.
            const float t = std::min(voice.walls, 1.0f);
            float occluded = std::exp(std::log(20000.0f) + (std::log(std::max(listener.occlusion_cutoff, 50.0f)) - std::log(20000.0f)) * t);
            occluded /= 1.0f + 0.6f * std::max(voice.walls - 1.0f, 0.0f);
            cutoff = cutoff > 0.0f ? std::min(cutoff, occluded) : occluded;
            gain = std::pow(std::clamp(listener.occlusion_volume, 0.0f, 1.0f), voice.walls);
        }
        voice.fx.low_pass.store(cutoff, std::memory_order_relaxed);
        voice.fx.high_pass.store(source.high_pass ? source.high_pass_cutoff : 0.0f, std::memory_order_relaxed);
        voice.fx.gain.store(gain, std::memory_order_relaxed);
        voice.fx.echo_wet.store(source.echo ? std::clamp(source.echo_wet, 0.0f, 1.0f) : 0.0f, std::memory_order_relaxed);
        voice.fx.echo_delay.store(std::clamp(source.echo_delay, 0.01f, 2.0f), std::memory_order_relaxed);
        voice.fx.echo_decay.store(source.echo_decay, std::memory_order_relaxed);
        voice.fx.send.store(std::clamp(source.reverb_send, 0.0f, 1.0f), std::memory_order_relaxed);
    }
};

AudioSystem::AudioSystem() : impl_(std::make_unique<Impl>()) {
    ma_engine_config config = ma_engine_config_init();
    config.listenerCount = 1;
    if (ma_engine_init(&config, &impl_->engine) == MA_SUCCESS) {
        impl_->ok = true;
        impl_->initBuses();
    } else {
        std::cerr << "[Audio] Sin dispositivo de audio: el juego sonara en silencio\n";
    }
}

AudioSystem::AudioSystem(Offline) : impl_(std::make_unique<Impl>()) {
    ma_engine_config config = ma_engine_config_init();
    config.listenerCount = 1;
    config.noDevice = MA_TRUE;
    config.channels = 2;
    config.sampleRate = 48000;
    if (ma_engine_init(&config, &impl_->engine) == MA_SUCCESS) {
        impl_->ok = true;
        impl_->initBuses();
    }
}

void AudioSystem::renderOffline(float* out, std::uint32_t frames) {
    if (!impl_->ok) return;
    ma_engine_read_pcm_frames(&impl_->engine, out, frames, nullptr);
}

AudioSystem::~AudioSystem() {
    stop();
    stopPreview();
    if (impl_->ok) {
        impl_->uninitBuses();
        ma_engine_uninit(&impl_->engine);
    }
}

bool AudioSystem::available() const { return impl_->ok; }
void AudioSystem::setAssetsRoot(const std::filesystem::path& root) { impl_->root = root; }
bool AudioSystem::running() const { return impl_->running; }
void AudioSystem::setOcclusionQuery(OcclusionQuery query) { impl_->occlusion_query = std::move(query); }
float AudioSystem::reverbLevel() const { return impl_->reverb_level; }

float AudioSystem::occlusionOf(ecs::Entity entity) const {
    const auto it = impl_->voices.find(entity.handle());
    if (it == impl_->voices.end() || !it->second->loaded || !ma_sound_is_playing(&it->second->sound)) return -1.0f;
    return it->second->walls;
}

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
    CR_PROFILE_SCOPE("Audio");
    Impl& d = *impl_;
    if (!d.ok) return;

    // --- Oyente: el AudioListener activo o la camara ---
    Vec3 position = camera_position;
    Vec3 forward = camera_forward;
    d.listener = AudioListener{};
    d.listener.occlusion = true;  // sin AudioListener: oclusion con los valores por defecto
    d.listener_found = false;
    d.listener_entity = {};
    for (const entt::entity handle : world.registry().view<AudioListener>()) {
        const ecs::Entity e = world.wrap(handle);
        if (!e.activeInHierarchy()) continue;
        position = e.worldPosition();
        forward = e.forward();
        d.listener = e.get<AudioListener>();
        d.listener_found = true;
        d.listener_entity = e;
        break;
    }
    d.listener_position = position;
    ma_engine_listener_set_position(&d.engine, 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(&d.engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&d.engine, 0, 0.0f, 1.0f, 0.0f);
    if (d.listener_has_last && delta_seconds > 1e-5f) {
        const Vec3 velocity = (position - d.listener_last) * (1.0f / delta_seconds);
        ma_engine_listener_set_velocity(&d.engine, 0, velocity.x, velocity.y, velocity.z);
    }
    d.listener_last = position;
    d.listener_has_last = true;
    ma_engine_set_volume(&d.engine, std::max(d.listener.volume, 0.0f));
    if (d.master_ok) {
        d.master.low_pass.store(d.listener.low_pass ? d.listener.low_pass_cutoff : 0.0f, std::memory_order_relaxed);
    }

    // --- Reverberacion: la zona que mas envuelve al oyente ---
    float best = 0.0f;
    float room = 0.5f;
    float damping = 0.5f;
    float level = 0.0f;
    for (const entt::entity handle : world.registry().view<AudioReverbZone>()) {
        const ecs::Entity e = world.wrap(handle);
        if (!e.activeInHierarchy()) continue;
        const AudioReverbZone& zone = e.get<AudioReverbZone>();
        const float distance = core::length(e.worldPosition() - position);
        const float inner = std::max(zone.min_distance, 0.0f);
        const float outer = std::max(zone.max_distance, inner + 0.01f);
        const float weight = distance <= inner ? 1.0f : distance >= outer ? 0.0f : 1.0f - (distance - inner) / (outer - inner);
        if (weight <= best) continue;
        best = weight;
        reverbPresetValues(zone, room, damping, level);
        level *= weight;
    }
    d.reverb_level = level;
    if (d.reverb_ok) {
        d.reverb.room.store(room, std::memory_order_relaxed);
        d.reverb.damping.store(damping, std::memory_order_relaxed);
        d.reverb.level.store(level, std::memory_order_relaxed);
    }

    // --- Ambiente (environment::Environment): lluvia, viento y truenos ---
    // Solo en Play; fuera, en silencio (los truenos pendientes se descartan).
    if (d.weather_audio.valid()) {
        environment::Environment* env = environment::findEnvironment(world);
        if (env != nullptr && env->runtime.initialized) {
            environment::EnvironmentRuntime& rt = env->runtime;
            const bool on = d.running && env->ambient_audio;
            if (on) {
                const environment::WeatherState& s = rt.current;
                d.weather_audio.setLevels(s.rain, std::clamp(rt.wind_speed / 16.0f, 0.0f, 1.5f), s.gusts, s.dust,
                                          std::max(env->audio_volume, 0.0f));
            } else {
                d.weather_audio.setLevels(0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
            }
            for (auto it = rt.thunder.begin(); it != rt.thunder.end();) {
                if (it->time > rt.elapsed) {
                    ++it;
                    continue;
                }
                // Si se paso hace mucho (pausa, editor), no suena tarde.
                if (on && rt.elapsed - it->time < 2.0) {
                    d.weather_audio.thunder(std::clamp(it->volume * std::max(env->thunder_volume, 0.0f), 0.0f, 2.0f),
                                            it->distance);
                }
                it = rt.thunder.erase(it);
            }
        } else {
            d.weather_audio.setLevels(0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        }
    }

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
        if (it->second->loaded) {
            d.apply(*it->second, *source, e.worldPosition(), delta_seconds, e);
            it->second->audible = Impl::audibility(*source, e.worldPosition(), d.listener_position);
        }
        ++it;
    }
    d.clock += delta_seconds;
    d.virtualizeVoices();
    prof::counter("Voces de audio", static_cast<double>(d.real_voices));
    prof::counter("Voces virtuales", static_cast<double>(d.virtual_voices));
    for (auto it = d.one_shots.begin(); it != d.one_shots.end();) {
        Impl::Voice& shot = **it;
        if (!shot.loaded || ma_sound_at_end(&shot.sound)) {
            it = d.one_shots.erase(it);
        } else {
            d.applyEffects(shot, shot.settings, shot.position, delta_seconds, {});
            ++it;
        }
    }
}

void AudioSystem::shiftOrigin(const core::Vec3& offset) {
    Impl& d = *impl_;
    d.listener_last = d.listener_last - offset;
    d.listener_position = d.listener_position - offset;
    for (auto& [handle, voice] : d.voices) {
        if (voice) voice->last_position = voice->last_position - offset;
    }
    // Los sonidos sueltos se colocaron una vez: se mueven con el mundo.
    for (auto& shot : d.one_shots) {
        if (!shot || !shot->loaded) continue;
        const ma_vec3f p = ma_sound_get_position(&shot->sound);
        ma_sound_set_position(&shot->sound, p.x - offset.x, p.y - offset.y, p.z - offset.z);
        shot->position = shot->position - offset;
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
    voice->queried = false;  // la oclusion se mira ya, sin fundido
    voice->virtual_voice = false;
    d.apply(*voice, *source, entity.worldPosition(), 0.0f, entity);
    ma_sound_seek_to_pcm_frame(&voice->sound, 0);
    ma_sound_start(&voice->sound);
}

void AudioSystem::stop(ecs::Entity entity) {
    const auto it = impl_->voices.find(entity.handle());
    if (it != impl_->voices.end() && it->second->loaded) {
        it->second->virtual_voice = false;
        ma_sound_stop(&it->second->sound);
        ma_sound_seek_to_pcm_frame(&it->second->sound, 0);
    }
}

void AudioSystem::pause(ecs::Entity entity) {
    const auto it = impl_->voices.find(entity.handle());
    if (it != impl_->voices.end() && it->second->loaded) {
        if (it->second->virtual_voice) {  // pausada: se queda donde iba
            Impl& d = *impl_;
            ma_uint32 rate = 0;
            ma_sound_get_data_format(&it->second->sound, nullptr, nullptr, &rate, nullptr, 0);
            const double elapsed = std::max(d.clock - it->second->virtual_since, 0.0) * static_cast<double>(std::max(rate, 1u));
            ma_sound_seek_to_pcm_frame(&it->second->sound, it->second->virtual_cursor + static_cast<ma_uint64>(elapsed));
            it->second->virtual_voice = false;
        }
        ma_sound_stop(&it->second->sound);
    }
}

bool AudioSystem::isPlaying(ecs::Entity entity) const {
    const auto it = impl_->voices.find(entity.handle());
    return it != impl_->voices.end() && it->second->loaded &&
           (it->second->virtual_voice || ma_sound_is_playing(&it->second->sound));
}

namespace {
OneShotListener& oneShotListener() {
    static OneShotListener listener;
    return listener;
}
}  // namespace

void setOneShotListener(OneShotListener listener) { oneShotListener() = std::move(listener); }

void AudioSystem::playOneShot(const std::string& clip, const Vec3& position, float volume, bool spatial) {
    if (const OneShotListener& listener = oneShotListener()) listener(clip, position, volume, spatial);
    Impl& d = *impl_;
    auto voice = std::make_unique<Impl::Voice>();
    if (!d.load(*voice, clip, spatial)) return;
    AudioSource source;
    source.volume = volume;
    source.spatial = spatial;
    source.max_distance = 100.0f;
    voice->settings = source;
    voice->position = position;
    d.apply(*voice, source, position, 0.0f, {});
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
    if (registry.find("AudioReverbZone") == nullptr) {
        registry.registerComponent<AudioReverbZone>("AudioReverbZone", "Audio Reverb Zone", "Audio");
    }
}

}  // namespace cramion::audio
