#ifndef CRAMION_CORE_WATER_H
#define CRAMION_CORE_WATER_H

// Agua procedural (sin texturas), para oceano/playa, lagos y rios:
//
//   WaterBody     el componente. Oceano: plano infinito a la altura de la
//                 entidad con oleaje Gerstner. Lago: rectangulo (size) con
//                 olas suaves; la orilla la pone el terreno. Rio: cinta que
//                 sigue sus puntos (locales a la entidad), con corriente.
//   sampleWater   altura, normal y velocidad del agua en un punto: la misma
//                 formula que el shader (water.vert), para la flotacion de
//                 Jolt y para consultas del juego.
//   waterTime     reloj compartido por el render y la fisica.

#include "CramionCore/ecs/Reflection.h"

#include <CramionFX/core/Math.h>

#include <cstdint>
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

float waterTime();
void advanceWaterTime(float delta_seconds);

void registerWaterComponents();

}  // namespace cramion::water

#endif  // CRAMION_CORE_WATER_H
