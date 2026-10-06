#ifndef CRAMION_CORE_SPLINE_SPLINE_H
#define CRAMION_CORE_SPLINE_SPLINE_H

// Splines como el Spline Container de Unity / el Spline Component de Unreal:
//
//   Spline          la curva: puntos de control locales a la entidad (cada uno
//                   con su ancho y su giro), abierta o cerrada, Catmull-Rom
//                   (pasa por los puntos), Bezier con tangentes automaticas o
//                   lineal.
//   SplineExtrude   genera una malla a lo largo de la curva: carretera,
//                   camino, rio (el WaterBody de la entidad sigue la curva),
//                   muro, valla con postes, tuberia o railes. Se puede pegar
//                   al terreno y, desde el editor/Lua/MCP, allanar y pintar el
//                   terreno debajo.
//   SplineFollower  mueve una entidad por una spline (camaras, vagonetas,
//                   patrullas, plataformas).
//
// La malla se rehace sola al cambiar la curva o sus ajustes (RenderSync).

#include "CramionCore/Uuid.h"
#include "CramionCore/ecs/Reflection.h"

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cramion::ecs {
class World;
class Entity;
class Mesh;
}  // namespace cramion::ecs

namespace cramion::terrain {
class TerrainStore;
}

namespace cramion::spline {

using core::Mat4;
using core::Vec3;

enum class SplineType : int { CatmullRom = 0, Bezier = 1, Linear = 2 };

struct SplinePoint {
    Vec3 position{};      // local a la entidad
    float width = 1.0f;   // multiplica el ancho de lo extruido
    float roll = 0.0f;    // grados de giro alrededor de la curva (peralte)
};

struct Spline {
    std::vector<SplinePoint> points{SplinePoint{{0.0f, 0.0f, 0.0f}}, SplinePoint{{0.0f, 0.0f, 10.0f}}};
    SplineType type = SplineType::CatmullRom;
    bool closed = false;
    float tension = 0.5f;     // Catmull-Rom / Bezier: 0 = recta entre puntos, 1 = muy curva
    float resolution = 1.0f;  // metros entre muestras de la curva
    bool show_gizmo = true;

    std::uint64_t revision = 1;
    void markModified() { ++revision; }
    void reflect(ecs::PropertyVisitor& v);
};

// Una muestra de la curva en el mundo.
struct SplineSample {
    Vec3 position{};
    Vec3 tangent{0.0f, 0.0f, 1.0f};  // unitaria, hacia delante
    Vec3 right{1.0f, 0.0f, 0.0f};    // unitaria (con el giro aplicado)
    Vec3 up{0.0f, 1.0f, 0.0f};
    float width = 1.0f;
    float distance = 0.0f;  // metros desde el principio
};

// La curva muestreada (en el mundo con la matriz dada, o local con identity).
class SplinePath {
public:
    void build(const Spline& spline, const Mat4& world = Mat4::identity());
    bool empty() const { return samples_.size() < 2; }
    float length() const { return samples_.empty() ? 0.0f : samples_.back().distance; }
    bool closed() const { return closed_; }
    const std::vector<SplineSample>& samples() const { return samples_; }
    std::vector<SplineSample>& mutableSamples() { return samples_; }

    // Muestra a `distance` metros (interpola entre las muestras; cerrada: da
    // la vuelta).
    SplineSample atDistance(float distance) const;
    // 0..1 de la longitud.
    SplineSample atNormalized(float t) const { return atDistance(t * length()); }
    // Punto de la curva mas cercano; devuelve su distancia en la curva.
    float closestDistance(const Vec3& point, Vec3* closest = nullptr) const;

private:
    std::vector<SplineSample> samples_;
    bool closed_ = false;
};

// Evalua la curva entre los puntos de control `segment` y `segment + 1`
// (t = 0..1), en local.
Vec3 evaluateSegment(const Spline& spline, int segment, float t);
int segmentCount(const Spline& spline);

enum class ExtrudeShape : int {
    Road = 0,    // calzada plana con arcen
    Path = 1,    // camino de tierra (mas fino, bordes irregulares)
    River = 2,   // cauce: el WaterBody de la entidad (tipo Rio) sigue la curva
    Wall = 3,    // muro macizo con albardilla
    Fence = 4,   // valla: postes y dos travesanos
    Pipe = 5,    // tubo
    Rails = 6,   // dos carriles con traviesas
    Ribbon = 7,  // cinta plana (UI en el mundo, trazos, alfombras)
};

struct SplineExtrude {
    ExtrudeShape shape = ExtrudeShape::Road;
    float width = 6.0f;          // metros (por el ancho de cada punto)
    float height = 0.15f;        // grosor de la calzada / alto del muro o valla
    float uv_meters = 6.0f;      // metros por repeticion de la textura a lo largo
    bool conform_to_terrain = true;  // pega la curva al terreno (Y del terreno)
    float ground_offset = 0.05f;     // metros sobre el terreno
    float post_spacing = 2.5f;       // valla y railes: metros entre postes/traviesas
    float segments_per_meter = 1.0f;
    bool collider = true;            // anade un MeshCollider
    core::Vec3 color{0.32f, 0.32f, 0.33f};  // sin .crmat
    float roughness = 0.85f;

    // Terreno (se aplica con applyToTerrain: editor, Lua o MCP)
    float terrain_blend = 4.0f;  // metros de talud a cada lado
    int paint_layer = -2;        // -2 = "Tierra"/"Camino" si existe, -1 = no pintar

    std::uint64_t revision = 1;
    void markModified() { ++revision; }
    void reflect(ecs::PropertyVisitor& v);
};

enum class FollowMode : int { Loop = 0, PingPong = 1, Once = 2 };

struct SplineFollower {
    Uuid spline;                 // entidad con Spline (vacio = la del padre)
    float speed = 5.0f;          // m/s
    float distance = 0.0f;       // posicion actual en la curva (m)
    FollowMode mode = FollowMode::Loop;
    bool orient = true;          // mira hacia delante
    bool play_on_start = true;
    bool playing = true;         // estado en Play (no se guarda)
    bool started = false;        // ya se aplico play_on_start
    float offset_side = 0.0f;    // metros a la derecha de la curva
    float offset_up = 0.0f;
    int direction = 1;           // +1 / -1 (PingPong)

    void reflect(ecs::PropertyVisitor& v);
};

void registerSplineComponents();

// Rehace las mallas de SplineExtrude que cambiaron. `terrains` (opcional)
// para pegarlas al terreno.
void updateSplineMeshes(ecs::World& world, terrain::TerrainStore* terrains);
// Mueve los SplineFollower (en Play).
void updateSplineFollowers(ecs::World& world, float delta_seconds);

// La malla que generaria un SplineExtrude (en local a la entidad).
std::shared_ptr<ecs::Mesh> buildExtrudeMesh(const Spline& spline, const SplineExtrude& extrude,
                                            const std::vector<SplineSample>& local_samples);

// La curva de una entidad en el mundo (pegada al terreno si su SplineExtrude
// lo pide y hay terreno).
SplinePath worldPath(const ecs::Entity& entity, terrain::TerrainStore* terrains);

// Allana (y pinta) el terreno bajo la curva con el ancho de su
// SplineExtrude. Devuelve cuantos terrenos cambiaron.
int applyToTerrain(ecs::World& world, const ecs::Entity& entity, terrain::TerrainStore& terrains);

// Crea una entidad con Spline (+ SplineExtrude con la forma dada si
// shape >= 0) que pasa por los puntos (mundo).
ecs::Entity createSplineEntity(ecs::World& world, const std::vector<Vec3>& world_points, int shape,
                               const std::string& name, bool closed = false);

}  // namespace cramion::spline

#endif  // CRAMION_CORE_SPLINE_SPLINE_H
