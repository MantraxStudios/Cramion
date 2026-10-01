#ifndef CRAMION_CORE_AUDIO_WEATHER_AUDIO_H
#define CRAMION_CORE_AUDIO_WEATHER_AUDIO_H

// Sonido del ambiente (environment/Environment.h) sintetizado en tiempo real,
// sin muestras grabadas: un nodo del grafo de miniaudio que genera
//
//   lluvia   siseo de ruido filtrado + el golpeteo de gotas sueltas
//            (impulsos cortos con su tono, repartidos a izquierda y derecha)
//   viento   ruido rosa/marron con un filtro que se abre con las rachas y un
//            silbido resonante cuando sopla fuerte
//   arena    viento con un siseo mas grave
//   truenos  chasquido (si cae cerca) y retumbar largo de ruido marron con
//            varios golpes, mas apagado cuanto mas lejos
//
// El hilo principal solo pone niveles (atomicos) y encola truenos.

#include <atomic>
#include <cstdint>

struct ma_node_graph;

namespace cramion::audio {

class WeatherAudio {
public:
    WeatherAudio();
    ~WeatherAudio();
    WeatherAudio(const WeatherAudio&) = delete;
    WeatherAudio& operator=(const WeatherAudio&) = delete;

    // `output`: el nodo al que se conecta (el master). false si falla.
    bool create(ma_node_graph* graph, void* output, std::uint32_t channels, float sample_rate);
    void destroy();
    bool valid() const;

    // Niveles 0..1 (se suavizan en el hilo de audio).
    void setLevels(float rain, float wind, float gusts, float dust, float volume);
    // Un trueno ahora (volumen 0..1, distancia en m).
    void thunder(float volume, float distance);

    struct Node;  // el nodo de miniaudio (WeatherAudio.cpp)

private:
    Node* node_ = nullptr;
};

}  // namespace cramion::audio

#endif  // CRAMION_CORE_AUDIO_WEATHER_AUDIO_H
