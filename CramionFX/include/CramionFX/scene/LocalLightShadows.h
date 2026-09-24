#ifndef CRAMION_SCENE_LOCAL_LIGHT_SHADOWS_H
#define CRAMION_SCENE_LOCAL_LIGHT_SHADOWS_H

#include "CramionFX/core/Math.h"
#include "CramionFX/scene/Camera.h"
#include "CramionFX/scene/Light.h"

#include <array>
#include <cstdint>
#include <vector>

namespace cramion::scene {

// Topes de luces locales con sombra. Deben coincidir con las constantes de
// shaders/lighting.frag.
//
// Todos los focos proyectan sombra (son pocos y cada uno es un solo mapa). De
// las luces puntuales solo las mas cercanas a la camara: cada una cuesta seis
// mapas, uno por cara del cubo.
inline constexpr std::uint32_t kMaxShadowedSpotLights = kMaxSpotLights;
inline constexpr std::uint32_t kMaxShadowedPointLights = 8;
inline constexpr std::uint32_t kPointShadowFaceCount = 6;

// Sombra de un foco: una sola proyeccion en perspectiva que cubre su cono.
struct SpotShadow {
    core::Mat4 light_view_projection = core::Mat4::identity();
    core::Vec3 position{};
    float range = 0.0f;

    // Tamano en el mundo de un texel por cada unidad de distancia a la luz. El
    // shader lo multiplica por la distancia para el desplazamiento por normal.
    float texel_scale = 0.0f;

    bool active = false;
    // Hay que volver a dibujar el mapa este frame (la luz ha cambiado).
    bool dirty = false;
};

// Sombra de una luz puntual: seis proyecciones en perspectiva, una por cara
// del cubo (+X, -X, +Y, -Y, +Z, -Z), guardadas como capas consecutivas.
struct PointShadow {
    std::array<core::Mat4, kPointShadowFaceCount> face_view_projection{};
    core::Vec3 position{};
    float range = 0.0f;
    float texel_scale = 0.0f;

    // Cuanto se aplica la sombra (0..1). Baja a 0 a medida que la luz se
    // acerca a perder su hueco frente a otra mas cercana: asi la sombra se
    // funde en vez de desaparecer de golpe al moverse la camara.
    float fade = 1.0f;

    // Indice en LightSet::points de la luz que ocupa este hueco; -1 = libre.
    std::int32_t light_index = -1;
    bool dirty = false;

    bool active() const { return light_index >= 0; }
};

// Sombras de las luces locales (focos y puntuales).
//
// Las cascadas no tienen sentido aqui: sirven para repartir un frustum de
// camara casi infinito entre varios mapas ortograficos, y una luz local tiene
// un alcance acotado que cabe entero en un frustum en perspectiva. Lo que si se
// comparte con el CSM es el filtrado (PCF de tienda) y el desplazamiento por
// normal del shader de iluminacion.
//
// Dos ideas para que salga barato:
//
//   - Cache: el mundo es estatico, asi que el mapa de una luz que no se ha
//     movido sigue siendo valido. Cada hueco recuerda que luz tiene dibujada y
//     solo se marca `dirty` cuando cambia. Las luces fijas se dibujan una vez.
//
//   - Huecos estables: las luces puntuales con sombra se eligen por cercania a
//     la camara, pero una luz que ya tenia hueco lo conserva. Asi, al moverse
//     la camara no se reordena todo y no hay que redibujar la cache.
//
// Las caras del cubo se proyectan con un FOV algo mayor de 90 grados (banda de
// guarda): el PCF de un pixel junto a la arista de una cara lee unos texeles
// mas alla, y sin ese margen se saldria del mapa y dejaria costuras de luz.
class LocalLightShadows {
public:
    // Recalcula matrices y huecos para las luces de este frame.
    void update(const Camera& camera, const LightSet& lights, std::uint32_t spot_map_size,
                std::uint32_t point_map_size);

    // Olvida la cache: todas las luces se redibujan en el siguiente update().
    // Hay que llamarlo si cambia la geometria del mundo.
    void invalidate();

    const std::array<SpotShadow, kMaxShadowedSpotLights>& spots() const { return spots_; }
    const std::array<PointShadow, kMaxShadowedPointLights>& points() const { return points_; }

    // Hueco de sombra de la luz con ese indice en LightSet; -1 si no tiene.
    std::int32_t spotSlot(std::size_t spot_index) const;
    std::int32_t pointSlot(std::size_t point_index) const;

    // Eje hacia el que mira cada cara del cubo.
    static core::Vec3 faceDirection(std::uint32_t face);

private:
    void updateSpots(const LightSet& lights, std::uint32_t map_size);
    void updatePoints(const Camera& camera, const LightSet& lights, std::uint32_t map_size);

    // Lo que hay dibujado en un hueco. Si la luz de este frame no coincide, el
    // hueco se redibuja.
    struct Signature {
        core::Vec3 position{};
        core::Vec3 direction{};
        float range = 0.0f;
        float angle = 0.0f;
        bool valid = false;

        bool operator==(const Signature& other) const;
    };

    std::array<SpotShadow, kMaxShadowedSpotLights> spots_{};
    std::array<PointShadow, kMaxShadowedPointLights> points_{};

    std::array<Signature, kMaxShadowedSpotLights> spot_rendered_{};
    std::array<Signature, kMaxShadowedPointLights> point_rendered_{};

    std::vector<std::int32_t> spot_slots_;
    std::vector<std::int32_t> point_slots_;

    // Plano cercano de las proyecciones. Pequeno para no recortar lo que esta
    // pegado a la luz.
    static constexpr float kNearPlane = 0.05f;

    // Texeles de margen en cada borde de las caras del cubo, para el PCF.
    static constexpr float kPointGuardTexels = 2.0f;
    // Distancia (en unidades de la puntuacion, ~bloques) a lo largo de la cual
    // la sombra de una luz puntual se funde antes de perder su hueco.
    static constexpr float kPointShadowFadeBand = 8.0f;
};

}  // namespace cramion::scene

#endif  // CRAMION_SCENE_LOCAL_LIGHT_SHADOWS_H
