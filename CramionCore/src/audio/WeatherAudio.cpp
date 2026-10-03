#include "WeatherAudio.h"

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace cramion::audio {

namespace {

constexpr float kTwoPi = 6.28318530718f;
constexpr std::uint32_t kMaxDrops = 32;
constexpr std::uint32_t kMaxThunder = 4;
constexpr std::uint32_t kQueue = 8;

// Coeficiente de un filtro de un polo para la frecuencia de corte `hz`.
float onePole(float hz, float rate) { return 1.0f - std::exp(-kTwoPi * std::clamp(hz, 5.0f, rate * 0.45f) / rate); }

}  // namespace

struct WeatherAudio::Node {
    ma_node_base base;  // primero: el grafo lo trata como un ma_node
    ma_uint32 channels = 2;
    float rate = 48000.0f;

    // Hilo principal -> hilo de audio.
    std::atomic<float> rain{0.0f};
    std::atomic<float> wind{0.0f};
    std::atomic<float> gusts{0.0f};
    std::atomic<float> dust{0.0f};
    std::atomic<float> volume{1.0f};
    struct Request {
        float volume = 1.0f;
        float distance = 1000.0f;
    };
    std::array<Request, kQueue> queue{};
    std::atomic<std::uint32_t> head{0};  // escribe el hilo principal
    std::atomic<std::uint32_t> tail{0};  // lee el hilo de audio
    std::atomic<std::uint32_t> cut{0};   // peticiones de silencio (silence())
    std::atomic<bool> paused{false};
    std::uint32_t cut_done = 0;          // las ya atendidas (hilo de audio)
    float fade = 1.0f;                   // ganancia del fundido al silenciar
    bool fading = false;

    // --- Estado del hilo de audio ---
    std::uint32_t seed = 0x1234567u;
    float rain_level = 0.0f;
    float wind_level = 0.0f;
    float gust_level = 0.0f;
    float dust_level = 0.0f;
    float volume_level = 1.0f;
    // Lluvia: siseo (paso alto + paso bajo por canal).
    float hiss_lp[2] = {0.0f, 0.0f};
    float hiss_hp[2] = {0.0f, 0.0f};
    struct Drop {
        float y1 = 0.0f, y2 = 0.0f, k = 0.0f;  // oscilador recursivo
        float amp = 0.0f, decay = 0.0f, pan = 0.5f;
        bool active = false;
    };
    std::array<Drop, kMaxDrops> drops{};
    // Viento: ruido marron filtrado y un silbido (paso banda).
    float brown[2] = {0.0f, 0.0f};
    float wind_lp[2] = {0.0f, 0.0f};
    float whistle_lp = 0.0f, whistle_bp = 0.0f;
    float gust_env = 0.0f, gust_target = 0.0f;
    std::uint32_t gust_timer = 0;
    // Truenos.
    struct Thunder {
        bool active = false;
        float t = 0.0f;
        float volume = 1.0f;
        float distance = 1000.0f;
        float attack = 0.1f, decay = 5.0f, pan = 0.5f;
        float cutoff = 200.0f;
        float brown = 0.0f, lp1 = 0.0f, lp2 = 0.0f;
        float roll = 1.0f, roll_target = 1.0f;
        std::uint32_t roll_timer = 0;
        float crack_lp = 0.0f;
    };
    std::array<Thunder, kMaxThunder> thunder{};

    float white() {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return static_cast<float>(seed) * (2.0f / 4294967295.0f) - 1.0f;
    }
    float uniform() { return white() * 0.5f + 0.5f; }

    void startThunder(float vol, float distance) {
        Thunder* slot = &thunder[0];
        for (Thunder& t : thunder) {
            if (!t.active) {
                slot = &t;
                break;
            }
            if (t.t > slot->t) slot = &t;  // el mas viejo
        }
        Thunder& t = *slot;
        t = Thunder{};
        t.active = true;
        t.volume = vol;
        t.distance = distance;
        // Lejos: llega mas apagado (el aire se come los agudos), mas lento y largo.
        t.attack = 0.03f + std::min(distance / 6000.0f, 0.5f);
        t.decay = 3.5f + std::min(distance / 900.0f, 5.0f) + uniform() * 1.5f;
        t.cutoff = 70.0f + 520.0f * std::exp(-distance / 1800.0f);
        t.pan = 0.25f + 0.5f * uniform();
    }

    // Todo callado y el estado a cero (tras el fundido de silence()).
    void reset() {
        for (Drop& d : drops) d = Drop{};
        for (Thunder& t : thunder) t = Thunder{};
        rain_level = wind_level = gust_level = dust_level = 0.0f;
        hiss_lp[0] = hiss_lp[1] = hiss_hp[0] = hiss_hp[1] = 0.0f;
        brown[0] = brown[1] = wind_lp[0] = wind_lp[1] = 0.0f;
        whistle_lp = whistle_bp = 0.0f;
        gust_env = gust_target = 0.0f;
    }

    void process(float* out, ma_uint32 frames) {
        // En pausa: silencio y todo se queda como estaba.
        if (paused.load(std::memory_order_relaxed) && cut.load(std::memory_order_acquire) == cut_done) {
            std::fill(out, out + static_cast<std::size_t>(frames) * channels, 0.0f);
            return;
        }
        // Silencio pedido (fin del Play): los truenos en cola se tiran y lo que
        // suena se apaga en ~15 ms.
        const std::uint32_t cuts = cut.load(std::memory_order_acquire);
        if (cuts != cut_done) {
            cut_done = cuts;
            fading = true;
            tail.store(head.load(std::memory_order_acquire), std::memory_order_release);
        }
        // Truenos pedidos.
        std::uint32_t h = head.load(std::memory_order_acquire);
        std::uint32_t tl = tail.load(std::memory_order_relaxed);
        while (tl != h) {
            const Request r = queue[tl % kQueue];
            if (!fading) startThunder(r.volume, r.distance);
            ++tl;
        }
        tail.store(tl, std::memory_order_release);

        const float smooth = 1.0f - std::exp(-1.0f / (0.35f * rate));
        const float rain_t = rain.load(std::memory_order_relaxed);
        const float wind_t = wind.load(std::memory_order_relaxed);
        const float gust_t = gusts.load(std::memory_order_relaxed);
        const float dust_t = dust.load(std::memory_order_relaxed);
        const float vol_t = volume.load(std::memory_order_relaxed);
        const float hiss_hp_a = onePole(700.0f, rate);
        const float hiss_lp_a = onePole(7500.0f, rate);
        const float whistle_center = 700.0f;

        for (ma_uint32 i = 0; i < frames; ++i) {
            rain_level += (rain_t - rain_level) * smooth;
            wind_level += (wind_t - wind_level) * smooth;
            gust_level += (gust_t - gust_level) * smooth;
            dust_level += (dust_t - dust_level) * smooth;
            volume_level += (vol_t - volume_level) * smooth;

            // Rachas: un objetivo al azar cada ~1-2 s, suavizado.
            if (gust_timer == 0) {
                gust_target = uniform();
                gust_timer = static_cast<std::uint32_t>(rate * (0.8f + 1.4f * uniform()));
            }
            --gust_timer;
            gust_env += (gust_target - gust_env) * (1.0f / (0.6f * rate));
            const float gust = 1.0f + gust_level * (gust_env * 1.4f - 0.5f);

            float l = 0.0f;
            float r = 0.0f;

            // --- Lluvia ---
            if (rain_level > 0.001f || dust_level > 0.001f) {
                // Mezcla mas baja: la lluvia fina sonaba como un aguacero y tapaba el juego.
                const float hiss_gain = 0.06f * std::pow(rain_level, 1.2f) + 0.05f * dust_level * gust;
                for (int c = 0; c < 2; ++c) {
                    const float x = white();
                    hiss_hp[c] += hiss_hp_a * (x - hiss_hp[c]);
                    const float high = x - hiss_hp[c];
                    hiss_lp[c] += hiss_lp_a * (high - hiss_lp[c]);
                    (c == 0 ? l : r) += hiss_lp[c] * hiss_gain;
                }
            }
            if (rain_level > 0.001f) {
                // Gotas: impulsos de Poisson (mas lluvia, mas golpes).
                const float rate_drops = 30.0f + 1100.0f * rain_level;
                if (uniform() < rate_drops / rate) {
                    for (Drop& d : drops) {
                        if (d.active) continue;
                        const float hz = 1400.0f + 4200.0f * uniform();
                        const float w = kTwoPi * hz / rate;
                        d.k = 2.0f * std::cos(w);
                        d.amp = (0.015f + 0.08f * uniform() * uniform()) * rain_level;
                        d.y1 = std::sin(w) * d.amp;
                        d.y2 = 0.0f;
                        d.decay = std::exp(-1.0f / ((0.0015f + 0.006f * uniform()) * rate));
                        d.pan = uniform();
                        d.active = true;
                        break;
                    }
                }
            }
            for (Drop& d : drops) {
                if (!d.active) continue;
                const float y = d.k * d.y1 - d.y2;
                d.y2 = d.y1 * d.decay;
                d.y1 = y * d.decay;
                l += y * (1.0f - d.pan);
                r += y * d.pan;
                if (std::abs(d.y1) + std::abs(d.y2) < 1e-5f) d.active = false;
            }

            // --- Viento ---
            const float wind_amount = std::max(wind_level, dust_level * 0.8f);
            if (wind_amount > 0.001f) {
                const float cutoff = 180.0f + 900.0f * std::min(wind_amount * gust, 1.5f);
                const float a = onePole(cutoff, rate);
                const float gain = 0.22f * std::pow(wind_amount, 1.5f) * std::max(gust, 0.1f);
                for (int c = 0; c < 2; ++c) {
                    brown[c] = std::clamp(brown[c] * 0.996f + white() * 0.06f, -1.0f, 1.0f);
                    wind_lp[c] += a * (brown[c] * 3.0f - wind_lp[c]);
                    (c == 0 ? l : r) += wind_lp[c] * gain;
                }
                // Silbido: paso banda de ruido blanco (filtro de estado variable).
                const float f = 2.0f * std::sin(3.14159265f * (whistle_center + 500.0f * gust_env) / rate);
                const float x = white();
                whistle_lp += f * whistle_bp;
                const float hp = x - whistle_lp - 0.08f * whistle_bp;
                whistle_bp += f * hp;
                const float whistle = whistle_bp * 0.05f * std::pow(std::max(wind_amount * gust - 0.35f, 0.0f), 1.5f);
                l += whistle;
                r += whistle * 0.8f;
            }

            // --- Truenos ---
            for (Thunder& t : thunder) {
                if (!t.active) continue;
                const float time = t.t;
                t.t += 1.0f / rate;
                if (time > t.decay * 2.5f + t.attack) {
                    t.active = false;
                    continue;
                }
                // Retumbar: ruido marron en dos pasos bajos, con golpes al azar.
                if (t.roll_timer == 0) {
                    t.roll_target = 0.35f + 0.9f * uniform() * uniform() + (uniform() < 0.15f ? 0.8f : 0.0f);
                    t.roll_timer = static_cast<std::uint32_t>(rate * (0.12f + 0.35f * uniform()));
                }
                --t.roll_timer;
                t.roll += (t.roll_target - t.roll) * (1.0f / (0.08f * rate));
                t.brown = std::clamp(t.brown * 0.998f + white() * 0.08f, -1.0f, 1.0f);
                const float a = onePole(t.cutoff, rate);
                t.lp1 += a * (t.brown - t.lp1);
                t.lp2 += a * (t.lp1 - t.lp2);
                const float env = (1.0f - std::exp(-time / t.attack)) * std::exp(-time / t.decay);
                float s = t.lp2 * 5.0f * env * t.roll;
                // Chasquido si cayo cerca.
                if (t.distance < 1500.0f && time < 0.4f) {
                    const float near = 1.0f - t.distance / 1500.0f;
                    t.crack_lp += 0.5f * (white() - t.crack_lp);
                    s += t.crack_lp * near * std::exp(-time / 0.06f) * 1.2f;
                }
                s *= t.volume * 0.9f;
                l += s * (1.2f - t.pan);
                r += s * (0.2f + t.pan);
            }

            // Suave (sin recortes duros) y al volumen general.
            l = std::tanh(l * volume_level);
            r = std::tanh(r * volume_level);
            if (fading) {
                fade -= 1.0f / (0.015f * rate);
                if (fade <= 0.0f) {
                    reset();
                    fade = 1.0f;
                    fading = false;
                    l = r = 0.0f;
                } else {
                    l *= fade;
                    r *= fade;
                }
            }
            float* frame = out + static_cast<std::size_t>(i) * channels;
            frame[0] = l;
            if (channels > 1) frame[1] = r;
            for (ma_uint32 c = 2; c < channels; ++c) frame[c] = 0.5f * (l + r);
        }
    }
};

namespace {

void weatherProcess(ma_node* node, const float** /*frames_in*/, ma_uint32* /*count_in*/, float** frames_out,
                    ma_uint32* count_out) {
    auto* n = reinterpret_cast<WeatherAudio::Node*>(node);
    n->process(frames_out[0], *count_out);
}

ma_node_vtable kWeatherVtable = {weatherProcess, nullptr, 0, 1, MA_NODE_FLAG_CONTINUOUS_PROCESSING};

}  // namespace

WeatherAudio::WeatherAudio() = default;

WeatherAudio::~WeatherAudio() { destroy(); }

bool WeatherAudio::create(ma_node_graph* graph, void* output, std::uint32_t channels, float sample_rate) {
    destroy();
    if (graph == nullptr || output == nullptr) return false;
    node_ = new Node();
    node_->channels = channels;
    node_->rate = sample_rate;
    const ma_uint32 out_channels[1] = {channels};
    ma_node_config config = ma_node_config_init();
    config.vtable = &kWeatherVtable;
    config.inputBusCount = 0;
    config.outputBusCount = 1;
    config.pInputChannels = nullptr;
    config.pOutputChannels = out_channels;
    if (ma_node_init(graph, &config, nullptr, &node_->base) != MA_SUCCESS) {
        delete node_;
        node_ = nullptr;
        return false;
    }
    ma_node_attach_output_bus(&node_->base, 0, static_cast<ma_node*>(output), 0);
    return true;
}

void WeatherAudio::destroy() {
    if (node_ == nullptr) return;
    ma_node_uninit(&node_->base, nullptr);
    delete node_;
    node_ = nullptr;
}

bool WeatherAudio::valid() const { return node_ != nullptr; }

void WeatherAudio::setLevels(float rain, float wind, float gusts, float dust, float volume) {
    if (node_ == nullptr) return;
    node_->rain.store(std::clamp(rain, 0.0f, 1.0f), std::memory_order_relaxed);
    node_->wind.store(std::clamp(wind, 0.0f, 1.5f), std::memory_order_relaxed);
    node_->gusts.store(std::clamp(gusts, 0.0f, 1.0f), std::memory_order_relaxed);
    node_->dust.store(std::clamp(dust, 0.0f, 1.0f), std::memory_order_relaxed);
    node_->volume.store(std::clamp(volume, 0.0f, 4.0f), std::memory_order_relaxed);
}

void WeatherAudio::silence() {
    if (node_ == nullptr) return;
    setLevels(0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    node_->cut.fetch_add(1, std::memory_order_release);
}

void WeatherAudio::setPaused(bool paused) {
    if (node_ != nullptr) node_->paused.store(paused, std::memory_order_relaxed);
}

void WeatherAudio::thunder(float volume, float distance) {
    if (node_ == nullptr) return;
    const std::uint32_t h = node_->head.load(std::memory_order_relaxed);
    const std::uint32_t t = node_->tail.load(std::memory_order_acquire);
    if (h - t >= kQueue) return;  // lleno (no deberia: el hilo de audio vacia la cola)
    node_->queue[h % kQueue] = Node::Request{volume, distance};
    node_->head.store(h + 1, std::memory_order_release);
}

}  // namespace cramion::audio
