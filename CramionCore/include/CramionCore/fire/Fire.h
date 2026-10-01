#ifndef CRAMION_CORE_FIRE_H
#define CRAMION_CORE_FIRE_H

// Fuego que se propaga (incendios de hierba y bosque) con humo volumetrico.
//
// El componente Fire define una zona cuadrada alrededor de su entidad. Dentro
// se simula en la CPU un automata celular (hasta 256 x 256 celdas, 10 pasos
// por segundo, con semilla: el mismo incendio cada vez):
//
//   - Combustible por celda: del terreno de debajo (la hierba y la hierba
//     seca arden bien, la tierra poco, la roca, la arena, la nieve y el barro
//     nada) y nada en rios, lagos ni bajo el mar.
//   - Una celda que arde calienta a sus vecinas: prenden antes cuesta arriba,
//     a favor del viento y con mas combustible. Con viento fuerte saltan
//     pavesas que encienden focos por delante.
//   - Arde mientras le queda combustible, luego quedan brasas y humo, y el
//     suelo carbonizado.
//   - "Solo en la zona": no sale del cuadrado. Si no, la zona crece sola
//     (hasta el tamano maximo) cuando el fuego llega al borde.
//
// El renderizador (gfx::FirePass) dibuja las llamas y el humo con raymarching
// y la geometria quemada (suelo, hierba, arboles) se ve carbonizada. Unas
// luces puntuales que parpadean iluminan alrededor.

#include "CramionCore/ecs/Reflection.h"

#include <CramionFX/core/Math.h>
#include <CramionFX/scene/Light.h>
#include <CramionFX/vk/FirePass.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace cramion::ecs {
class World;
}
namespace cramion::terrain {
class TerrainStore;
}

namespace cramion::fire {

// Punto donde empieza el fuego (respecto al objeto).
struct FireIgnition {
    core::Vec3 position{};
    float radius = 2.0f;
};

enum class FireMode { Edit, Play, Paused };

struct FireState;  // la simulacion (en marcha, no se guarda)

struct Fire {
    bool enabled = true;
    // Zona
    float size = 120.0f;          // lado del cuadrado (m), centrado en el objeto
    float cell_size = 0.5f;       // m por celda (si no cabe en 256 celdas, mas grandes)
    bool spread = true;           // se propaga
    bool limit_to_zone = true;    // solo dentro de la zona (si no, crece)
    float max_size = 800.0f;      // hasta donde crece la zona sin limite (m)
    // Fuego
    float spread_speed = 0.5f;    // m/s del frente sin viento ni pendiente
    float burn_time = 20.0f;      // s que arde una celda con todo su combustible
    float smolder_time = 25.0f;   // s de brasas (y humo) al apagarse
    float flame_height = 3.0f;    // m
    float flame_intensity = 1.0f;
    // Humo
    float smoke_amount = 1.0f;
    float smoke_height = 55.0f;   // m que sube
    float smoke_rise = 3.5f;      // m/s
    core::Vec3 smoke_color{0.42f, 0.4f, 0.38f};
    // Viento
    bool sky_wind = true;         // el del Cielo (direccion y la mitad de su velocidad)
    float wind_speed = 3.0f;      // m/s (si no usa el del cielo)
    float wind_direction = 30.0f; // grados (0 = hacia +X)
    float wind_influence = 1.0f;
    float slope_influence = 1.0f;
    // Combustible
    bool terrain_fuel = true;     // segun las capas del terreno
    float default_fuel = 0.8f;    // donde no hay terreno (0..1)
    float fuel_multiplier = 1.0f;
    // Encendido
    bool ignite_on_start = true;
    std::vector<FireIgnition> ignitions{FireIgnition{}};
    bool simulate_in_editor = false;
    int seed = 1;
    // Luces
    bool lights = true;
    int max_lights = 6;
    float light_intensity = 1.0f;

    // En marcha (no se guarda; las copias no la comparten: ver FireState::owner).
    std::shared_ptr<FireState> state;

    void reflect(ecs::PropertyVisitor& v);
};

// Lo que expone la simulacion de una zona (para Lua, MCP y el editor).
struct FireStats {
    bool active = false;           // hay rejilla en marcha
    int resolution = 0;
    float size = 0.0f;             // lado actual (m)
    core::Vec2 offset{};           // esquina minima (x, z) respecto al objeto
    int burning_cells = 0;         // con llamas
    int smoldering_cells = 0;      // brasas
    int burned_cells = 0;          // ya quemadas
    int burnable_cells = 0;        // con combustible al empezar
    float burning_area = 0.0f;     // m2 con llamas
    float burned_fraction = 0.0f;  // quemado / combustible
    core::Vec2 wind{};
    float simulated_seconds = 0.0f;
};

void registerFireComponents();

// Avanza las simulaciones (una vez por frame). En Edit solo las que tienen
// "Simular en el editor" (las demas se apagan). Al entrar en Play el fuego
// empieza de cero.
void updateFires(ecs::World& world, float delta_seconds, FireMode mode, terrain::TerrainStore* terrains);

// Zonas y luces para el renderizador (las primeras gfx::kFireZoneSlots
// zonas activas). `seconds` = reloj para el parpadeo de las luces.
void collectFireRender(ecs::World& world, float seconds, std::vector<gfx::FireZone>& zones,
                       std::vector<scene::PointLight>& lights);

// --- Acciones (Lua, MCP, el editor). Posiciones en el mundo. ---
// Enciende las celdas con combustible en el circulo de las zonas que lo
// tocan. Devuelve cuantas zonas lo recibieron.
int ignite(ecs::World& world, const core::Vec3& position, float radius);
int extinguish(ecs::World& world, const core::Vec3& position, float radius);
void extinguishAll(ecs::World& world);
// Vuelve a empezar (sin fuego, todo sin quemar).
void resetAll(ecs::World& world);
void reset(Fire& fire);
// Calor (0..1) y carbonizado (0..1) en un punto (el mayor de las zonas).
float heatAt(ecs::World& world, const core::Vec3& position);
float charAt(ecs::World& world, const core::Vec3& position);
// Todas las zonas juntas.
FireStats totalStats(ecs::World& world);
FireStats stats(const Fire& fire);

}  // namespace cramion::fire

#endif  // CRAMION_CORE_FIRE_H
