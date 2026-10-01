#ifndef CRAMION_CORE_ENVIRONMENT_H
#define CRAMION_CORE_ENVIRONMENT_H

// Ambiente (como Enviro Sky 3 de Unity): un solo componente que lleva el
// clima, la hora, la fecha, las estaciones y el viento de toda la escena.
//
//   Clima        presets (Despejado, Nubes, Cubierto, Niebla, Llovizna,
//                Lluvia, Tormenta, Nevada ligera, Nieve, Ventisca, Tormenta
//                de arena) con transiciones suaves de N segundos entre ellos.
//                Cambios al azar opcionales, con probabilidades segun la
//                estacion.
//   Hora/fecha   hora del dia, dia y mes y latitud: el sol y la luna siguen
//                su recorrido real (mas altos en verano, dias mas largos).
//                El dia puede avanzar solo (duracion del dia en minutos).
//   Estaciones   de la fecha (o fijas): tinte de la hierba y los arboles,
//                temperatura y probabilidades del clima.
//   Superficies  la lluvia moja y encharca poco a poco y se seca despues; la
//                nieve se acumula en lo que mira hacia arriba y se derrite
//                (mojando) cuando hace calor.
//   Tormenta     rayos con su destello (cielo, nubes y escena) y truenos que
//                llegan con el retraso de la distancia.
//   Viento       uno global (direccion, fuerza y rachas) para las nubes, la
//                hierba, los arboles, la lluvia y la nieve.
//
// NO duplica nada: conduce el cielo (Sky: nubes), la niebla del
// post-procesado, la humedad/charcos (Weather), la hierba y la vegetacion,
// que siguen funcionando igual sin el. Lo aplica RenderSync cada frame
// (applyEnvironment) y el audio lee su estado (lluvia, viento, truenos).

#include "CramionCore/ecs/Reflection.h"

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cramion::scene {
class Scene;
}
namespace cramion::gfx {
class VulkanRenderer;
struct PostProcessSettings;
struct GrassDesc;
struct FoliageSettings;
}  // namespace cramion::gfx
namespace cramion::ecs {
class World;
class Entity;
struct Light;
}  // namespace cramion::ecs

namespace cramion::environment {

enum class WeatherPreset : int {
    Clear = 0,
    Cloudy,
    Overcast,
    Foggy,
    LightRain,
    Rain,
    Storm,
    LightSnow,
    Snow,
    Blizzard,
    Sandstorm,
};
inline constexpr int kWeatherPresetCount = 11;

enum class Season : int { Spring = 0, Summer, Autumn, Winter };

// Lo que define un clima. En las transiciones se interpola todo.
struct WeatherState {
    float cloud_coverage = 0.15f;  // 0..1
    float cloud_density = 0.9f;
    float cloud_type = 0.4f;       // 0 estratos .. 1 cumulonimbos
    float fog = 0.0f;              // densidad de la niebla (1/m)
    float sky_fog = 0.0f;          // cuanto vela la niebla el horizonte del cielo (0..1)
    float wind = 2.5f;             // m/s
    float gusts = 0.3f;            // rachas 0..1
    float rain = 0.0f;             // intensidad 0..1
    float snow = 0.0f;             // intensidad 0..1
    float dust = 0.0f;             // arena en el aire 0..1
    float wetness = 0.0f;          // humedad a la que tienden las superficies
    float puddles = 0.0f;          // charcos a los que tienden
    float lightning = 0.0f;        // rayos por minuto
    float sun = 1.0f;              // multiplica la luz del sol
    float ambient = 1.0f;          // multiplica la luz ambiente
    float temperature = 0.0f;      // grados que se suman a los de la estacion
    core::Vec3 tint{1.0f, 1.0f, 1.0f};  // tinte de la luz del sol
};

WeatherState presetState(WeatherPreset preset);
WeatherState lerpState(const WeatherState& a, const WeatherState& b, float t);
// Nombre estable en ingles ("Storm") y nombre visible ("Tormenta").
const char* presetName(WeatherPreset preset);
const char* presetLabel(WeatherPreset preset);
// Acepta el nombre en ingles o en espanol, sin importar mayusculas, espacios
// ni guiones ("light rain", "LightRain", "llovizna", "nieve").
bool presetFromName(std::string_view name, WeatherPreset& out);
const char* seasonName(Season season);   // "Spring"...
const char* seasonLabel(Season season);  // "Primavera"...
bool seasonFromName(std::string_view name, Season& out);

// Un trueno que el audio tiene que sonar: en `time` (segundos del reloj del
// ambiente), con su volumen (0..1) y la distancia del rayo (m).
struct ThunderEvent {
    double time = 0.0;
    float volume = 1.0f;
    float distance = 1000.0f;
};

// Estado en marcha (no se guarda en la escena).
struct EnvironmentRuntime {
    bool initialized = false;
    WeatherPreset target = WeatherPreset::Clear;  // al que se va
    WeatherPreset from_preset = WeatherPreset::Clear;
    WeatherState from;              // de donde sale la transicion
    WeatherState current;           // interpolado ahora
    float transition = 1.0f;        // 0..1
    float transition_seconds = 0.0f;
    float next_change = 0.0f;       // clima al azar: segundos hasta el siguiente
    double elapsed = 0.0;           // reloj propio (s)

    // Superficies
    float wetness = 0.0f;
    float puddles = 0.0f;
    float snow_cover = 0.0f;        // 0..1
    float melt = 0.0f;              // humedad de la nieve derritiendose
    float temperature = 15.0f;      // grados C

    // Viento
    float wind_speed = 0.0f;        // con las rachas (m/s)
    float wind_direction = 30.0f;   // grados (0 = +X), con el vaiven
    core::Vec3 wind{};              // vector (m/s)

    // Rayos
    float lightning_timer = 5.0f;
    float flash = 0.0f;             // brillo del destello ahora
    float flash_age = 10.0f;        // segundos desde el ultimo rayo
    float flash_peak = 0.0f;
    core::Vec3 flash_position{};    // donde cayo (mundo)
    std::vector<core::Vec4> bolt;   // segmentos: pares (a.xyz, ancho), (b.xyz, brillo)
    std::vector<ThunderEvent> thunder;
    std::uint32_t rng = 0x9E3779B9u;
    bool strike_requested = false;
    float strike_distance = -1.0f;  // pedido por script (<0 = al azar)

    // Astros y estacion
    core::Vec3 to_sun{0.0f, 1.0f, 0.0f};
    core::Vec3 to_moon{0.0f, -1.0f, 0.0f};
    Season season_now = Season::Summer;
    float season_tint = 0.0f;       // 0 verde .. 1 otono (tambien el invierno seco)
    float cloud_bottom = 1500.0f;   // base de las nubes (la del Sky)
};

struct Environment {
    // --- Clima ---
    WeatherPreset weather = WeatherPreset::Clear;
    float transition_time = 8.0f;     // s al cambiar de clima
    bool random_weather = false;
    float min_duration = 120.0f;      // s que dura cada clima al azar
    float max_duration = 360.0f;
    float random_transition = 40.0f;  // s de las transiciones al azar
    // --- Hora y fecha ---
    bool control_time = true;         // mueve el sol y la luna (cielo fisico)
    float time_of_day = 10.0f;        // horas
    bool time_progress = false;       // el dia avanza solo
    float day_length = 24.0f;         // minutos reales por dia de juego
    int day = 21;
    int month = 6;
    float latitude = 40.0f;           // grados (negativo = hemisferio sur)
    // --- Estaciones ---
    bool season_from_date = true;
    Season season = Season::Summer;
    bool vegetation_tint = true;      // la hierba y las hojas cambian de color
    // --- Viento ---
    float wind_direction = 30.0f;     // grados (0 = +X)
    float wind_strength = 1.0f;       // multiplica el viento del clima
    bool wind_wander = true;          // la direccion cambia despacio
    // --- Lluvia y nieve ---
    float precipitation_density = 1.0f;
    bool splashes = true;
    bool snow_accumulation = true;
    float accumulation_speed = 1.0f;
    float melt_speed = 1.0f;
    // --- Tormenta ---
    bool lightning = true;
    float lightning_frequency = 1.0f;
    float lightning_brightness = 1.0f;
    bool lightning_bolts = true;      // el rayo se ve en el cielo
    // --- Niebla ---
    float fog_strength = 1.0f;
    // --- Audio (en Play) ---
    bool ambient_audio = true;
    float audio_volume = 1.0f;
    float thunder_volume = 1.0f;

    EnvironmentRuntime runtime;

    void reflect(ecs::PropertyVisitor& v);
};

// Lo que el ambiente aplica este frame (RenderSync lo guarda para la hierba
// y la vegetacion).
struct EnvironmentFrame {
    bool valid = false;
    WeatherState state;
    float wind_speed = 0.0f;
    float wind_direction = 30.0f;
    float snow_cover = 0.0f;
    float season_tint = 0.0f;
    bool vegetation_tint = true;
};

// --- Uso desde scripts, MCP y el editor ---
// El primer Environment activo (nullptr si no hay).
Environment* findEnvironment(ecs::World& world);
// Lo crea si no hay: entidad "Ambiente" con Sky (cielo fisico) y Environment.
Environment& ensureEnvironment(ecs::World& world);
// Cambia de clima en `seconds` (0 = al instante: tambien las superficies).
void setWeather(Environment& env, WeatherPreset preset, float seconds);
// Lanza un rayo ya (distancia en m; <0 = al azar).
void strikeLightning(Environment& env, float distance = -1.0f);
// Nieve acumulada (0..1) y humedad, al instante.
void setSnowCover(Environment& env, float cover);
void setWetness(Environment& env, float wetness, float puddles);
// Nombre del clima actual: el de destino si la transicion paso de la mitad.
WeatherPreset currentPreset(const Environment& env);
// Dia del ano (1..365) de la fecha.
int dayOfYear(int day, int month);
// Estacion de la fecha y latitud.
Season seasonForDate(int day, int month, float latitude);
// Direccion hacia el sol (mundo: X este, Y arriba, -Z norte).
core::Vec3 sunDirection(float hours, int day_of_year, float latitude);

// --- Aplicacion (RenderSync) ---
// Avanza el ambiente (`delta_seconds` 0 = no avanza: segunda vista del
// editor) y lo aplica: nubes, sol/luna, humedad, niebla, precipitacion,
// rayos. `post` es el post-procesado ya mezclado (se le suma la niebla).
// `directional`: la luz direccional de la escena (su color e intensidad
// siguen valiendo; su giro no, si el ambiente lleva la hora).
// Sin Environment en el mundo devuelve un frame invalido y deja el
// renderizador como estaba antes de que existiera.
EnvironmentFrame applyEnvironment(ecs::World& world, scene::Scene& scene, gfx::VulkanRenderer& renderer,
                                  float delta_seconds, gfx::PostProcessSettings post, const ecs::Light* directional,
                                  bool hdr_sky, float weather_wetness, float weather_puddles, bool weather_rain);
// Viento, estacion y nieve en la hierba y los arboles.
void applyToGrass(const EnvironmentFrame& frame, gfx::GrassDesc& grass);
void applyToFoliage(const EnvironmentFrame& frame, gfx::FoliageSettings& foliage);

}  // namespace cramion::environment

#endif  // CRAMION_CORE_ENVIRONMENT_H
