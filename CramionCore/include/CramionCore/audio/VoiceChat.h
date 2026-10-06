#ifndef CRAMION_CORE_AUDIO_VOICE_CHAT_H
#define CRAMION_CORE_AUDIO_VOICE_CHAT_H

// Chat de voz para el multijugador (como Vivox / el voice chat de Steam):
// el microfono a 16 kHz en mono, en trozos de 20 ms comprimidos con mu-law
// (G.711: 16 KB/s por quien habla) que van por la red sin fiabilidad; cada
// jugador tiene su bufer anti-saltos y se mezclan en la salida.
//
//   Voice.start()                       abre el microfono y la salida
//   Voice.setMode("push")               "push" (pulsar para hablar) | "open" (por voz) | "off"
//   Voice.setTalking(true/false)        en modo push (Input.getAction("Hablar")...)
//   Voice.setProximity(30)              volumen segun la distancia (0 = todos igual)
//   Voice.isSpeaking(id)                quien habla ahora (indicadores en la UI)
//   Voice.setMuted(id, true)            silenciar a un jugador
//
// La sesion de red la pone el sistema de scripts (NetworkSession::sendVoice).

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cramion::audio {

enum class VoiceMode : int { Off = 0, PushToTalk = 1, Open = 2 };

class VoiceChat {
public:
    static constexpr int kSampleRate = 16000;
    static constexpr int kFrameSamples = 320;  // 20 ms

    VoiceChat();
    ~VoiceChat();
    VoiceChat(const VoiceChat&) = delete;
    VoiceChat& operator=(const VoiceChat&) = delete;

    bool start(std::string* error = nullptr);
    void stop();
    bool running() const;

    void setMode(VoiceMode mode) { mode_ = mode; }
    VoiceMode mode() const { return mode_; }
    void setTalking(bool talking) { talking_ = talking; }
    void setVoiceThreshold(float rms) { threshold_ = rms; }  // modo por voz (0..1)
    void setVolume(float v) { volume_ = v; }
    void setMicGain(float g) { mic_gain_ = g; }
    void setMuted(std::uint32_t player, bool muted);
    bool muted(std::uint32_t player) const;
    void setProximity(float meters) { proximity_ = meters; }

    // Cada frame: los trozos del microfono listos para mandar (ya en mu-law).
    std::vector<std::string> takeOutgoing();
    // Llega un trozo de `player` (o de nadie: player = 0 se ignora).
    void receive(std::uint32_t player, const std::string& frame);
    // Posiciones para el volumen por distancia (quien escucha y cada jugador).
    void setListener(const core::Vec3& position) { listener_ = position; }
    void setSpeakerPosition(std::uint32_t player, const core::Vec3& position);

    bool localSpeaking() const;
    bool isSpeaking(std::uint32_t player) const;
    float micLevel() const;  // 0..1 (medidor)

    // mu-law (G.711) de 16 bits a 8 y al reves (publicas para las pruebas).
    static std::uint8_t encodeMuLaw(std::int16_t sample);
    static std::int16_t decodeMuLaw(std::uint8_t value);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    VoiceMode mode_ = VoiceMode::PushToTalk;
    bool talking_ = false;
    float threshold_ = 0.02f;
    float volume_ = 1.0f;
    float mic_gain_ = 1.0f;
    float proximity_ = 0.0f;
    core::Vec3 listener_{};
};

// La del juego (Lua Voice.*). nullptr si no se ha creado.
VoiceChat* activeVoiceChat();
void setActiveVoiceChat(VoiceChat* voice);

}  // namespace cramion::audio

#endif  // CRAMION_CORE_AUDIO_VOICE_CHAT_H
