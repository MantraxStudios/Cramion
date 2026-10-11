#ifndef CRAMION_CORE_WATER_H
#define CRAMION_CORE_WATER_H

// Agua procedural (sin texturas), para oceano/playa, lagos y rios:
//
//   WaterBody     el componente. Oceano: plano infinito a la altura de la
//                 entidad con oleaje FFT (espectro JONSWAP en 4 cascadas,
//                 como Crest). Lago: rectangulo (size) con olas suaves
//                 (Gerstner); la orilla la pone el terreno. Rio: cinta que
//                 sigue sus puntos (locales a la entidad), con corriente.
//   sampleWater   altura, normal y velocidad del agua en un punto: la misma
//                 superficie que dibuja la GPU (las mismas amplitudes del
//                 espectro, transformada inversa en la CPU), para la
//                 flotacion de Jolt y para consultas del juego.
//   oceanSpectrum el espectro del oceano (lo sube RenderSync a la GPU).
//   waterTime     reloj compartido por el render y la fisica.

#include "CramionCore/ecs/Reflection.h"

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace cramion::water {

enum class WaterType : int { Ocean = 0, Lake = 1, River = 2 };

struct RiverPoint {
    core::Vec3 position{};  // local a la entidad
    float width = 8.0f;     // metros
};

struct WaterBody {
    WaterType type = WaterType::Lake;
    core::Vec2 size{80.0f, 80.0f};  // lago: ancho x largo (m)

    // --- Oleaje (Gerstner, 8 ondas alrededor del viento) ---
    float wave_height = 0.25f;     // altura de la ola principal (m)
    float wavelength = 10.0f;      // de la ola principal (m)
    float wave_speed = 1.0f;       // 1 = velocidad fisica en aguas profundas
    float steepness = 0.45f;       // 0 = senoidal, 1 = crestas afiladas
    float wind_direction = 30.0f;  // grados (0 = +X)
    float wind_spread = 35.0f;     // grados de dispersion de las ondas
    float flow_speed = 1.5f;       // rio: corriente (m/s)
    // Oceano (FFT): viento y mar de fondo. wind_speed > 0 calcula la altura
    // y la longitud de onda del espectro JONSWAP con el viento y el fetch
    // (distancia sobre la que sopla); 0 = wave_height / wavelength a mano.
    float wind_speed = 0.0f;       // m/s
    float fetch = 300.0f;          // km
    float swell_height = 0.0f;     // mar de fondo (m, altura significativa)
    float swell_wavelength = 120.0f;
    float swell_direction = 0.0f;  // grados (0 = +X)

    // --- Aspecto ---
    core::Vec3 shallow_color{0.10f, 0.42f, 0.38f};  // dispersion (lo que tine el agua)
    core::Vec3 deep_color{0.012f, 0.055f, 0.075f};   // color del agua profunda
    float clarity = 4.0f;          // metros hasta que el fondo casi no se ve
    float roughness = 0.05f;
    float refraction = 0.5f;
    float detail = 1.0f;           // ondulacion fina (viento)
    float foam = 1.0f;             // espuma de crestas y orilla
    float shore_foam = 1.5f;       // ancho de la espuma de orilla (m)
    float shore_waves = 1.0f;      // olas que rompen en la playa
    float caustics = 1.0f;
    float scattering = 1.0f;       // luz a traves de las crestas
    float foam_persistence = 4.0f; // segundos que dura la espuma de las crestas (oceano)

    // --- Bajo el agua ---
    float god_rays = 1.0f;         // rayos de sol bajo la superficie
    float particles = 1.0f;        // particulas en suspension

    // --- Fisica (Jolt) ---
    bool buoyancy = true;
    float density = 1.0f;         // flotacion relativa (1 = agua)
    float drag = 0.5f;            // frenado de lo que flota

    std::vector<RiverPoint> points;  // rio

    void reflect(ecs::PropertyVisitor& v);
};

// Valores de fabrica de cada tipo (los menus del editor).
WaterBody oceanPreset();
WaterBody lakePreset();
WaterBody riverPreset();

// Punto del agua bajo/sobre `position` (mundo).
struct WaterSample {
    bool inside = false;  // dentro de la extension del cuerpo
    float height = 0.0f;  // y de la superficie
    core::Vec3 normal{0.0f, 1.0f, 0.0f};
    core::Vec3 velocity{};  // corriente (rio) + movimiento orbital de la ola
};
WaterSample sampleWater(const WaterBody& body, const core::Mat4& world, const core::Vec3& position, float time);

// Desplazamiento Gerstner de un punto de la cuadricula (x, z del mundo) en el
// instante `time`: xyz = desplazamiento, `normal` y `jacobian` (espuma).
core::Vec3 gerstner(const WaterBody& body, float x, float z, float time, core::Vec3* normal = nullptr,
                    float* jacobian = nullptr);

// Linea central del rio en el mundo, subdividida (Catmull-Rom) cada `step` m:
// posicion y ancho por punto.
struct RiverSample {
    core::Vec3 position{};
    float width = 0.0f;
    core::Vec3 tangent{1.0f, 0.0f, 0.0f};
    float distance = 0.0f;  // a lo largo del rio (m)
};
std::vector<RiverSample> riverCenterline(const WaterBody& body, const core::Mat4& world, float step = 1.0f);
// sampleWater con la linea central del rio ya calculada (riverCenterline con
// paso 1 m): para muchas consultas al mismo rio en el mismo instante (la
// flotacion de cada cuerpo en cada paso de la fisica).
WaterSample sampleWater(const WaterBody& body, const core::Mat4& world, const core::Vec3& position, float time,
                        const std::vector<RiverSample>& river_line);

// --- Oceano FFT ---
// Cuatro cascadas de 128 x 128 modos (cada una 3.7 veces mas pequena que la
// anterior; cada onda del espectro esta en una sola cascada). La GPU hace la
// evolucion en el tiempo y la transformada inversa cada frame
// (water_fft.comp); la CPU, la misma suma de las tres cascadas grandes (todo
// su contenido cabe en 64 x 64) para la flotacion.
inline constexpr int kOceanCascades = 4;
inline constexpr int kOceanResolution = 128;
inline constexpr float kOceanPeriod = 256.0f;  // s: las frecuencias se redondean a multiplos de 2pi/periodo

struct OceanSpectrum {
    std::array<float, kOceanCascades> sizes{};           // lado de cada cascada (m)
    std::array<float, kOceanCascades> slope_variance{};  // pendiente^2 media de cada cascada (rugosidad lejana)
    float significant_height = 0.0f;  // altura significativa resultante (m)
    float peak_wavelength = 0.0f;     // m
    float choppiness = 0.0f;          // desplazamiento horizontal (crestas afiladas)
    float wave_speed = 1.0f;
    // Por cascada y modo (orden natural de la FFT: indice m -> k = m o m - N),
    // 8 floats: h0(k) (re, im), conj(h0(-k)) (re, im), omega, kx, kz, 0.
    std::vector<float> modes;
    std::uint64_t key = 0;  // firma de los parametros (cambia = otro espectro)
};
// Espectro de un oceano (en cache; se puede llamar desde cualquier hilo).
std::shared_ptr<const OceanSpectrum> oceanSpectrum(const WaterBody& body);
// Desplazamiento del oceano FFT en el punto de la cuadricula (x, z) relativo
// al origen del agua: xyz = desplazamiento; `normal` y `velocity` opcionales.
core::Vec3 oceanDisplacement(const WaterBody& body, float x, float z, float time, core::Vec3* normal = nullptr,
                             core::Vec3* velocity = nullptr);

float waterTime();
void advanceWaterTime(float delta_seconds);

// Quien sabe mejor si la camara esta bajo el agua (un mundo de bloques: una
// cueva bajo el nivel del mar no esta sumergida aunque el oceano sea un
// plano infinito). Devuelve 1 = bajo el agua, 0 = fuera, -1 = no sabe (se
// decide con el oleaje, como siempre). Vacio = sin nadie.
using UnderwaterOverride = std::function<int(const core::Vec3& camera)>;
void setUnderwaterOverride(UnderwaterOverride test);
int underwaterOverride(const core::Vec3& camera);

void registerWaterComponents();

}  // namespace cramion::water

#endif  // CRAMION_CORE_WATER_H
