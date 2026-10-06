#include "CramionCore/audio/VoiceChat.h"

#include <miniaudio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <iostream>
#include <mutex>

namespace cramion::audio {

namespace {
VoiceChat* g_active = nullptr;
using Clock = std::chrono::steady_clock;
}  // namespace

VoiceChat* activeVoiceChat() { return g_active; }
void setActiveVoiceChat(VoiceChat* voice) { g_active = voice; }

struct VoiceChat::Impl {
    ma_device capture{};
    ma_device playback{};
    bool capture_ok = false;
    bool playback_ok = false;

    std::mutex mutex;
    std::vector<std::int16_t> mic;          // muestras capturadas sin trocear
    std::vector<std::string> outgoing;      // trozos listos (mu-law)
    std::atomic<float> mic_level{0.0f};
    Clock::time_point last_local_voice{};
    // Cada jugador: su cola de muestras y su volumen.
    struct Stream {
        std::deque<std::int16_t> samples;
        bool primed = false;  // ya tiene colchon (empieza a sonar)
        float gain = 1.0f;
        bool muted = false;
        core::Vec3 position{};
        bool has_position = false;
        Clock::time_point last_frame{};
    };
    std::map<std::uint32_t, Stream> streams;
    VoiceChat* owner = nullptr;

    static void onCapture(ma_device* device, void* /*out*/, const void* input, ma_uint32 frames) {
        auto* self = static_cast<Impl*>(device->pUserData);
        const auto* in = static_cast<const std::int16_t*>(input);
        if (in == nullptr) return;
        std::lock_guard lock(self->mutex);
        self->mic.insert(self->mic.end(), in, in + frames);
        if (self->mic.size() > static_cast<std::size_t>(kSampleRate)) {
            self->mic.erase(self->mic.begin(), self->mic.end() - kSampleRate);
        }
    }

    static void onPlayback(ma_device* device, void* output, const void* /*in*/, ma_uint32 frames) {
        auto* self = static_cast<Impl*>(device->pUserData);
        auto* out = static_cast<std::int16_t*>(output);
        std::fill(out, out + frames, static_cast<std::int16_t>(0));
        std::lock_guard lock(self->mutex);
        const float master = self->owner != nullptr ? self->owner->volume_ : 1.0f;
        for (auto& [id, s] : self->streams) {
            (void)id;
            if (!s.primed) {
                if (s.samples.size() < static_cast<std::size_t>(kFrameSamples * 3)) continue;  // 60 ms de colchon
                s.primed = true;
            }
            if (s.samples.empty()) {
                s.primed = false;
                continue;
            }
            const float g = s.muted ? 0.0f : s.gain * master;
            const ma_uint32 n = std::min<ma_uint32>(frames, static_cast<ma_uint32>(s.samples.size()));
            for (ma_uint32 i = 0; i < n; ++i) {
                const int mixed = out[i] + static_cast<int>(static_cast<float>(s.samples.front()) * g);
                out[i] = static_cast<std::int16_t>(std::clamp(mixed, -32768, 32767));
                s.samples.pop_front();
            }
        }
    }
};

VoiceChat::VoiceChat() : impl_(std::make_unique<Impl>()) { impl_->owner = this; }

VoiceChat::~VoiceChat() {
    stop();
    if (g_active == this) g_active = nullptr;
}

bool VoiceChat::start(std::string* error) {
    if (running()) return true;
    ma_device_config cc = ma_device_config_init(ma_device_type_capture);
    cc.capture.format = ma_format_s16;
    cc.capture.channels = 1;
    cc.sampleRate = kSampleRate;
    cc.dataCallback = &Impl::onCapture;
    cc.pUserData = impl_.get();
    impl_->capture_ok = ma_device_init(nullptr, &cc, &impl_->capture) == MA_SUCCESS &&
                        ma_device_start(&impl_->capture) == MA_SUCCESS;
    ma_device_config pc = ma_device_config_init(ma_device_type_playback);
    pc.playback.format = ma_format_s16;
    pc.playback.channels = 1;
    pc.sampleRate = kSampleRate;
    pc.dataCallback = &Impl::onPlayback;
    pc.pUserData = impl_.get();
    impl_->playback_ok = ma_device_init(nullptr, &pc, &impl_->playback) == MA_SUCCESS &&
                         ma_device_start(&impl_->playback) == MA_SUCCESS;
    if (!impl_->capture_ok) std::cerr << "[Voz] No hay microfono (solo se escucha)\n";
    if (!impl_->playback_ok && error != nullptr) *error = "no se pudo abrir la salida de audio";
    return impl_->playback_ok;
}

void VoiceChat::stop() {
    if (impl_->capture_ok) ma_device_uninit(&impl_->capture);
    if (impl_->playback_ok) ma_device_uninit(&impl_->playback);
    impl_->capture_ok = impl_->playback_ok = false;
    std::lock_guard lock(impl_->mutex);
    impl_->mic.clear();
    impl_->outgoing.clear();
    impl_->streams.clear();
}

bool VoiceChat::running() const { return impl_->playback_ok || impl_->capture_ok; }

void VoiceChat::setMuted(std::uint32_t player, bool muted) {
    std::lock_guard lock(impl_->mutex);
    impl_->streams[player].muted = muted;
}

bool VoiceChat::muted(std::uint32_t player) const {
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->streams.find(player);
    return it != impl_->streams.end() && it->second.muted;
}

std::vector<std::string> VoiceChat::takeOutgoing() {
    std::vector<std::string> frames;
    std::vector<std::int16_t> mic;
    {
        std::lock_guard lock(impl_->mutex);
        const std::size_t whole = impl_->mic.size() / kFrameSamples * kFrameSamples;
        mic.assign(impl_->mic.begin(), impl_->mic.begin() + static_cast<std::ptrdiff_t>(whole));
        impl_->mic.erase(impl_->mic.begin(), impl_->mic.begin() + static_cast<std::ptrdiff_t>(whole));
    }
    for (std::size_t f = 0; f + kFrameSamples <= mic.size(); f += kFrameSamples) {
        double sum = 0.0;
        for (int i = 0; i < kFrameSamples; ++i) {
            const double v = static_cast<double>(mic[f + static_cast<std::size_t>(i)]) * mic_gain_ / 32768.0;
            sum += v * v;
        }
        const float rms = static_cast<float>(std::sqrt(sum / kFrameSamples));
        impl_->mic_level = std::min(rms * 4.0f, 1.0f);
        const bool send = mode_ == VoiceMode::Open ? rms >= threshold_ : (mode_ == VoiceMode::PushToTalk && talking_);
        if (!send) continue;
        impl_->last_local_voice = Clock::now();
        std::string frame(static_cast<std::size_t>(kFrameSamples), '\0');
        for (int i = 0; i < kFrameSamples; ++i) {
            const float s = static_cast<float>(mic[f + static_cast<std::size_t>(i)]) * mic_gain_;
            frame[static_cast<std::size_t>(i)] =
                static_cast<char>(encodeMuLaw(static_cast<std::int16_t>(std::clamp(s, -32768.0f, 32767.0f))));
        }
        frames.push_back(std::move(frame));
    }
    return frames;
}

void VoiceChat::setSpeakerPosition(std::uint32_t player, const core::Vec3& position) {
    std::lock_guard lock(impl_->mutex);
    Impl::Stream& s = impl_->streams[player];
    s.position = position;
    s.has_position = true;
}

void VoiceChat::receive(std::uint32_t player, const std::string& frame) {
    if (player == 0 || frame.empty()) return;
    std::lock_guard lock(impl_->mutex);
    Impl::Stream& s = impl_->streams[player];
    s.last_frame = Clock::now();
    s.gain = 1.0f;
    if (proximity_ > 0.0f && s.has_position) {
        const float d = core::length(s.position - listener_);
        s.gain = std::clamp(1.0f - d / proximity_, 0.0f, 1.0f);
        s.gain *= s.gain;  // cae mas natural
    }
    for (const char c : frame) s.samples.push_back(decodeMuLaw(static_cast<std::uint8_t>(c)));
    // Mas de medio segundo de retraso: se tira lo viejo (no se acumula).
    const std::size_t cap = static_cast<std::size_t>(kSampleRate / 2);
    while (s.samples.size() > cap) s.samples.pop_front();
}

bool VoiceChat::localSpeaking() const {
    return Clock::now() - impl_->last_local_voice < std::chrono::milliseconds(250);
}

bool VoiceChat::isSpeaking(std::uint32_t player) const {
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->streams.find(player);
    return it != impl_->streams.end() && Clock::now() - it->second.last_frame < std::chrono::milliseconds(250);
}

float VoiceChat::micLevel() const { return impl_->mic_level; }

// G.711 mu-law.
std::uint8_t VoiceChat::encodeMuLaw(std::int16_t sample) {
    constexpr int kBias = 0x84;
    constexpr int kClip = 32635;
    int s = sample;
    const int sign = (s >> 8) & 0x80;
    if (sign != 0) s = -s;
    s = std::min(s, kClip) + kBias;
    int exponent = 7;
    for (int mask = 0x4000; (s & mask) == 0 && exponent > 0; mask >>= 1) --exponent;
    const int mantissa = (s >> (exponent + 3)) & 0x0F;
    return static_cast<std::uint8_t>(~(sign | (exponent << 4) | mantissa));
}

std::int16_t VoiceChat::decodeMuLaw(std::uint8_t value) {
    const int u = ~value & 0xFF;
    const int sign = u & 0x80;
    const int exponent = (u >> 4) & 0x07;
    const int mantissa = u & 0x0F;
    int s = ((mantissa << 3) + 0x84) << exponent;
    s -= 0x84;
    return static_cast<std::int16_t>(sign != 0 ? -s : s);
}

}  // namespace cramion::audio
